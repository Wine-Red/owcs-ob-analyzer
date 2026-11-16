#include "state.hpp"
#include "names.hpp"
#include <QDateTime>
#include <QUuid>
#include <algorithm>
namespace owcs {
static QString identityKey(const QString &name) {
  return "name:" + name.normalized(QString::NormalizationForm_KC).toCaseFolded();
}
static void clearCandidates(QMap<QString, Field> &fields) {
  for (auto &f : fields) {
    f.candidate = QJsonValue::Null;
    f.count = 0;
  }
}
qint64 monotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
QString utcNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
bool Field::observe(QJsonValue v, double conf, qint64 now, int required, Confirmation policy,
                    qint64 candidateWindow) {
  if (candidateSeen >= 0 && now - candidateSeen > candidateWindow) {
    candidate = QJsonValue::Null;
    count = 0;
  }
  if (v.isNull() || v.isUndefined()) {
    // One unreadable frame is not contradictory evidence. Never refresh the
    // published value, but allow the next nearby observation to confirm it.
    decision = "unreadable";
    return false;
  }
  bool numeric = policy == Confirmation::Nondecreasing && v.isDouble();
  bool same = v == candidate;
  bool accept = v == value || (same ? count + 1 : 1) >= required;
  bool previous = false;
  if (numeric && candidate.isDouble() && count > 0 && v.toInt() > candidate.toInt()) {
    // Confirm the PREVIOUS actual observation, never an extrapolated value.
    // This costs one sample of latency and removes any magnitude/rate ceiling.
    // A lone upward spike has no following support; a lone drop followed by a
    // return to the established range is not a reset either.
    previous = !accept && !(value.isDouble() && candidate.toInt() < value.toInt() &&
                            v.toInt() >= value.toInt());
    accept |= previous;
  }
  if (accept) {
    retained = false;
    value = previous ? candidate : v;
    seen = previous ? candidateSeen : now;
    observed = previous ? candidateObserved : utcNow();
    confidence = previous ? candidateConfidence : std::clamp(conf, 0., 1.);
  }
  count = same ? count + 1 : 1;
  candidate = v;
  candidateSeen = now;
  candidateObserved = utcNow();
  candidateConfidence = std::clamp(conf, 0., 1.);
  decision = accept ? (previous ? "confirmed_previous" : "accepted") : "confirming";
  return accept;
}

QJsonObject Field::metadata(qint64 now, bool visible, int ttl) const {
  return {{"status",
           seen < 0 ? "unknown" : (visible && !retained && now - seen <= ttl ? "fresh" : "stale")},
          {"observed_at", seen < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(observed)},
          {"age_ms", seen < 0 ? QJsonValue(QJsonValue::Null)
                              : QJsonValue(double(std::max<qint64>(0, now - seen)))},
          {"confidence", confidence},
          {"decision", decision},
          {"candidate_count", count}};
}
State::State(QString id) : streamId_(std::move(id)) { reset(); }
void State::reset() {
  players_ = {};
  history_.clear();
  lastRows_.clear();
  playerOrder_.clear();
  sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
  sequence_ = 0;
  lastVisible_ = -1;
  panelCount_ = 0;
}
void State::name(int row, Match result, qint64 now) {
  if (row < 0 || row >= 10)
    return;
  auto &p = players_[row];
  if (p.empty || isEmptySlotName(result.id) || result.id.isEmpty() || p.currentSignature.empty()) {
    p.pendingName.observe(QJsonValue::Null, 0, now);
    return;
  }
  auto prior = p.fields["display_name"].value;
  if (!prior.isNull() && prior.toString().toCaseFolded() == result.id.toCaseFolded())
    result.id = prior.toString();
  if (!prior.isNull() && prior != result.id) {
    if (!p.nameUncertain)
      history_[identityKey(prior.toString())] = p.fields;
    p.textUncertain = p.nameUncertain = true;
    clearCandidates(p.fields);
  }
  if (!p.pendingName.observe(nullable(result.id), result.confidence, now,
                             result.confidence >= .88 ? 3 : 5, Confirmation::Exact, 10000))
    return;
  // A name cannot own two readable rows on the scoreboard. Wait until the
  // previous row is missing/rechecking; swaps are then resolved independently.
  for (int i = 0; i < 10; ++i)
    if (i != row && !players_[i].nameUncertain &&
        players_[i].fields.value("display_name").value.toString().toCaseFolded() ==
            result.id.toCaseFolded()) {
      p.textUncertain = p.nameUncertain = true;
      p.pendingName = Field{};
      return;
    }
  for (int i = 0; i < 10; ++i)
    if (i != row && players_[i].fields.value("display_name").value.toString().toCaseFolded() ==
                        result.id.toCaseFolded()) {
      // The old location must reverify, even if its historical crop reappears.
      players_[i].textUncertain = players_[i].nameUncertain = true;
      players_[i].pendingName = Field{};
    }
  if (prior != p.pendingName.value &&
      (!prior.isNull() || p.nameUncertain || history_.contains(identityKey(result.id)))) {
    p.fields = history_.value(identityKey(result.id));
    clearCandidates(p.fields);
    for (auto &field : p.fields)
      field.retained = true;
    p.heroPending = false;
    p.chargeReadyCandidate = QJsonValue::Null;
    ++p.generation;
  }
  p.fields["display_name"] = p.pendingName;
  p.signature = p.currentSignature.clone();
  p.nameUncertain = p.textUncertain = false;
  p.nameVerified = now;
  const auto id = identityKey(result.id);
  history_[id] = p.fields;
  lastRows_[id] = row;
  if (!playerOrder_.contains(id))
    playerOrder_.append(id);
  // Session-local, bounded retention; reset() also clears all archived players.
  if (playerOrder_.size() > 100) {
    for (const auto oldId : playerOrder_) {
      bool active = false;
      for (const auto &other : players_)
        active |= !other.nameUncertain &&
                  identityKey(other.fields.value("display_name").value.toString()) == oldId;
      if (!active) {
        history_.remove(oldId);
        lastRows_.remove(oldId);
        playerOrder_.removeAll(oldId);
        break;
      }
    }
  }
}
bool State::needsName(int row, qint64 now) const {
  return row >= 0 && row < 10 && !players_[row].empty && !players_[row].currentSignature.empty() &&
         (players_[row].nameUncertain ||
          players_[row].fields.value("display_name").value.isNull() ||
          (now >= 0 && now - players_[row].nameVerified >= 5000));
}
QJsonObject State::update(const Observation &obs, const QString &source, qint64 now,
                          double latency) {
  bool visible = obs.visible && obs.players.size() == 10;
  if (visible) {
    // Hiding Tab is not evidence of a different player. Existing values remain
    // stale until observed again; an actual name change is confirmed below.
    lastVisible_ = now;
    panelCount_ = obs.confirmed ? panelCount_ + 1 : 0;
    // Archive before resolving any row in this frame, so either order of a
    // simultaneous swap restores the same last confirmed per-player state.
    for (int i = 0; i < 10; ++i)
      if (!players_[i].nameUncertain && players_[i].fields.value("display_name").value.isString())
        history_[identityKey(players_[i].fields.value("display_name").value.toString())] =
            players_[i].fields;
    for (int i = 0; i < 10; i++) {
      auto raw = obs.players[i].toObject();
      auto &p = players_[i];
      p.empty = raw["slot_status"] == "empty";
      if (p.empty)
        p.textUncertain = true; // Even the same returning crop must reverify its owner.
      auto sig = p.empty ? cv::Mat{} : Recognizer::signature(obs.names[i]);
      p.currentSignature = sig;
      if (!sig.empty()) {
        if (p.signature.empty())
          p.signature = sig;
        bool changed = Recognizer::signatureSimilarity(p.signature, sig) < .96 || p.textUncertain;
        if (changed != p.nameUncertain) {
          // Reverification of the established owner needs one matching OCR
          // reading; a new owner still needs its own repeated confirmations.
          p.pendingName = p.fields.value("display_name");
          p.pendingName.candidate = QJsonValue::Null;
          p.pendingName.count = 0;
        }
        p.nameUncertain = changed;
      } else {
        p.nameUncertain = true;
        p.pendingName = Field{};
      }
      auto &f = p.fields;
      if (p.nameUncertain) {
        // No row observation can be attributed while the occupant is uncertain.
        // Preserve the last confirmed values, and break every candidate chain.
        clearCandidates(f);
        for (auto &field : f)
          field.retained = true;
        continue;
      }
      // A confirmed name is still observed while its text signature matches.
      // Avoid re-running name OCR merely to refresh unchanged identity metadata.
      if (!sig.empty() && !p.nameUncertain && !f["display_name"].value.isNull())
        f["display_name"].observe(f["display_name"].value, f["display_name"].confidence, now, 1);
      auto old = f["hero"].value;
      auto oldSelection = f["hero_selection"].value;
      auto selection =
          raw["hero_selection"].toString(raw["hero"].isNull() ? "unknown" : "selected");
      const bool occluded = raw["portrait_occluded"].toBool();
      if (!occluded) {
        f["hero_selection"].observe(
            selection == "unknown" ? QJsonValue(QJsonValue::Null) : QJsonValue(selection),
            raw["selection_confidence"].toDouble(raw["hero_confidence"].toDouble()), now, 2);
        f["hero"].observe(raw["hero"], raw["hero_confidence"].toDouble(), now, 2);
        if (selection == "unselected" || !raw["hero"].isNull())
          p.heroPending = QJsonValue(selection) != f["hero_selection"].value ||
                          (!raw["hero"].isNull() && raw["hero"] != f["hero"].value);
      } else {
        // Direct overlay evidence invalidates this portrait only. Historical
        // life state never gates a new unobscured hero observation.
        for (auto key : {"hero", "hero_selection"}) {
          f[key].candidate = QJsonValue::Null;
          f[key].count = 0;
        }
        // A readable respawn overlay establishes selection even if this source
        // has never seen the unobscured hero. The hero ID itself stays unknown.
        f["hero_selection"].observe("selected", raw["life_confidence"].toDouble(), now, 1);
      }
      if ((!old.isNull() && f["hero"].value != old) || oldSelection != f["hero_selection"].value)
        for (auto key : {"minor", "major"})
          f.remove(key);
      bool unselected = f["hero_selection"].value == "unselected";
      if (unselected && selection == "unselected")
        for (auto key :
             {"hero", "life", "respawn", "ultimate_charge", "ultimate_ready", "minor", "major"})
          f.remove(key);
      if (selection != "unselected") {
        f["life"].observe(raw["life_state"] == "unknown" ? QJsonValue(QJsonValue::Null)
                                                         : raw["life_state"],
                          raw["life_confidence"].toDouble(), now);
        if (raw["life_state"] == "dead")
          f["respawn"].observe(raw["respawn_seconds"], raw["life_confidence"].toDouble(), now, 1);
        if (raw["life_state"] == "alive" && f["life"].value == "alive")
          f.remove("respawn");
      }
      if (selection != "unselected") {
        auto &charge = f["ultimate_charge"];
        if (charge.observe(raw["ultimate_charge"], raw["ultimate_confidence"].toDouble(), now, 2,
                           Confirmation::Nondecreasing)) {
          auto ready = charge.decision == "confirmed_previous" ? p.chargeReadyCandidate
                                                               : raw["ultimate_ready"];
          f["ultimate_ready"].observe(ready, charge.confidence, charge.seen, 1);
          f["ultimate_ready"].observed = charge.observed;
        }
        if (!raw["ultimate_charge"].isNull() && !raw["ultimate_charge"].isUndefined())
          p.chargeReadyCandidate = raw["ultimate_ready"];
      }
      auto stats = raw["stats"].toObject(), conf = raw["stat_confidence"].toObject();
      // Once the owner is confirmed, accept its newly observed counters even
      // below the archived values. Confirmation still rejects isolated OCR drops.
      for (auto it = stats.begin(); it != stats.end(); ++it)
        f[it.key()].observe(it.value(), conf[it.key()].toDouble(), now, 2,
                            Confirmation::Nondecreasing);
      for (auto tier : {"minor", "major"}) {
        auto perk = raw["perks"].toObject()[tier].toObject();
        auto confidence = perk.take("confidence").toDouble();
        const bool emptySlot =
            perk["selection"] == "unselected" || perk["selection"] == "not_shown";
        bool accepted = selection != "unselected" &&
                        (emptySlot || (perk["selection"] == "selected" && !p.heroPending &&
                                       !raw["hero"].isNull() && raw["hero"] == f["hero"].value));
        // A held scoreboard can lose its perk icons. Only positive, hero-bound
        // observations may replace a confirmed choice; disappearance is history,
        // not proof that the player unlearned it. Hero/identity resets above still
        // discard the old choice before we reach this point.
        const bool remembered = f[tier].value.toObject()["selection"] == "selected";
        if (remembered && (!accepted || emptySlot)) {
          f[tier].retained = true;
          f[tier].candidate = QJsonValue::Null;
          f[tier].count = 0;
          accepted = false;
        }
        f[tier].observe(accepted ? QJsonValue(perk) : QJsonValue(QJsonValue::Null), confidence, now,
                        3);
      }
    }
  } else {
    panelCount_ = 0;
    for (auto &p : players_) {
      p.pendingName = Field{};
      for (auto &f : p.fields) {
        f.retained = true;
        f.count = 0;
        f.candidate = QJsonValue::Null;
      }
    }
  }
  ++sequence_;
  return snapshot(obs, source, now, latency);
}
QJsonObject State::snapshot(const Observation &obs, const QString &source, qint64 now,
                            double latency) {
  bool visible = obs.visible && obs.players.size() == 10;
  auto serialize = [&](Player &p, int i) {
    auto &f = p.fields;
    QJsonObject stats, fields, perks;
    for (auto key : {"eliminations", "assists", "deaths", "damage", "healing", "mitigated"})
      stats[key] = f[key].value;
    for (auto tier : {"minor", "major"})
      perks[tier] =
          f[tier].value.isNull()
              ? QJsonValue(QJsonObject{{"id", QJsonValue::Null}, {"selection", "unknown"}})
              : f[tier].value;
    for (auto key : {"hero", "hero_selection", "life", "respawn", "ultimate_charge",
                     "ultimate_ready", "display_name"})
      (void)f[key];
    for (auto it = f.begin(); it != f.end(); ++it)
      fields[it.key()] =
          it->metadata(now, visible && !p.nameUncertain, it.key() == "display_name" ? 15000 : 1000);
    // Retained values are stale until the row identity is confirmed again.
    if (p.nameUncertain) {
      fields["display_name"] = Field{}.metadata(now, false);
    }
    // Only identity and hero-bound perks await a replacement portrait. Life,
    // charge, countdown and counters retain their own observation clocks.
    if (p.heroPending) {
      for (auto key : {"hero", "hero_selection"})
        fields[key] = Field{}.metadata(now, false);
      for (auto tier : {"minor", "major"})
        if (perks[tier].toObject()["selection"] == "selected") {
          perks[tier] = QJsonObject{{"id", QJsonValue::Null}, {"selection", "unknown"}};
          fields[tier] = Field{}.metadata(now, false);
        }
    }
    QString side = i < 5 ? "left" : "right";
    QJsonObject board{{"row", i % 5 + 1},
                      {"slot_basis", "scoreboard_row"},
                      {"identity_generation", p.generation},
                      {"ultimate_charge", f["ultimate_charge"].value},
                      {"ultimate_ready", f["ultimate_ready"].value},
                      {"respawn_seconds", f["respawn"].value},
                      {"perks", perks},
                      {"stats", stats},
                      {"fields", fields}};
    return QJsonObject{
        {"slot_status", p.empty           ? "empty"
                        : p.nameUncertain ? "pending"
                                          : "occupied"},
        {"historical_player_id", p.nameUncertain && f["display_name"].value.isString()
                                     ? QJsonValue(identityKey(f["display_name"].value.toString()))
                                     : QJsonValue(QJsonValue::Null)},
        {"historical_display_name",
         p.nameUncertain ? f["display_name"].value : QJsonValue(QJsonValue::Null)},
        {"slot_id", side + "-" + QString::number(i % 5 + 1)},
        {"side", side},
        {"slot", i % 5 + 1},
        {"hero", p.heroPending ? QJsonValue(QJsonValue::Null) : f["hero"].value},
        {"hero_selection", p.heroPending || f["hero_selection"].value.isNull()
                               ? QJsonValue("unknown")
                               : f["hero_selection"].value},
        {"hero_confidence", p.heroPending ? 0 : f["hero"].confidence},
        {"hero_status", p.heroPending || f["hero"].value.isNull() ? "unknown" : "confirmed"},
        {"life_state", f["life"].value.isNull() ? QJsonValue("unknown") : f["life"].value},
        {"life_confidence", f["life"].confidence},
        {"life_status", f["life"].value.isNull() ? "unknown" : "confirmed"},
        {"display_name", p.nameUncertain ? QJsonValue(QJsonValue::Null) : f["display_name"].value},
        {"player_id", p.nameUncertain || f["display_name"].value.isNull()
                          ? QJsonValue(QJsonValue::Null)
                          : QJsonValue(identityKey(f["display_name"].value.toString()))},
        {"identity_status", p.nameUncertain                    ? "pending"
                            : f["display_name"].value.isNull() ? "unknown"
                                                               : "confirmed"},
        {"display_name_confidence", f["display_name"].confidence},
        {"display_name_status",
         p.nameUncertain || f["display_name"].value.isNull() ? "unknown" : "confirmed"},
        {"updated_at", utcNow()},
        {"scoreboard", board}};
  };
  QJsonArray rows;
  QMap<QString, int> locations;
  bool teamResolved[2]{visible && panelCount_ >= 2, visible && panelCount_ >= 2};
  for (int i = 0; i < 10; ++i) {
    auto &p = players_[i];
    rows.append(serialize(p, i));
    const auto name = p.fields.value("display_name").value;
    if (!p.empty && (p.nameUncertain || !name.isString()))
      teamResolved[i / 5] = false;
    if (!p.empty && !p.nameUncertain && name.isString()) {
      const auto id = identityKey(name.toString());
      history_[id] = p.fields;
      lastRows_[id] = i;
      if (visible && panelCount_ >= 2)
        locations[id] = i;
    }
  }
  // Stable named records are separate from the ten observed scoreboard slots.
  // Empty slots never create players; a departed player survives compaction.
  QJsonArray participants;
  for (const auto &id : playerOrder_) {
    int row = lastRows_.value(id);
    bool present = locations.contains(id);
    QJsonObject record;
    if (present) {
      row = locations[id];
      record = rows[row].toObject();
    } else {
      Player historical;
      historical.fields = history_.value(id);
      for (auto &field : historical.fields)
        field.retained = true;
      record = serialize(historical, row);
    }
    record["presence_status"] = present                 ? "present"
                                : teamResolved[row / 5] ? "missing"
                                                        : "unconfirmed";
    record["position"] = present ? QJsonValue(QJsonObject{{"side", record["side"]},
                                                          {"slot", record["slot"]},
                                                          {"slot_id", record["slot_id"]}})
                                 : QJsonValue(QJsonValue::Null);
    participants.append(record);
  }
  // Keep departed players accessible even after another occupant takes their row.
  QJsonArray retained;
  for (auto it = history_.cbegin(); it != history_.cend(); ++it) {
    bool active = false;
    for (int i = 0; i < 10; ++i)
      active |= !players_[i].nameUncertain &&
                identityKey(players_[i].fields.value("display_name").value.toString()) == it.key();
    if (active)
      continue;
    QJsonObject stats;
    for (auto key : {"eliminations", "assists", "deaths", "damage", "healing", "mitigated"})
      stats[key] = it.value().value(key).value;
    retained.append(QJsonObject{{"player_id", it.key()},
                                {"display_name", it.value().value("display_name").value},
                                {"hero", it.value().value("hero").value},
                                {"stats", stats},
                                {"status", "stale"}});
  }
  bool hud = obs.hudOffset != -10000;
  return {{"type", "state.snapshot"},
          {"schema_version", "2.0"},
          {"stream_id", streamId_},
          {"session_id", sessionId_},
          {"sequence", double(sequence_)},
          {"observed_at", utcNow()},
          {"source_age_ms", 0},
          {"source_status", "online"},
          {"game_paused", obs.paused},
          {"retained_players", retained},
          {"participants", participants},
          {"view", "scoreboard"},
          {"hud", QJsonObject{{"status", hud ? "locked" : "searching"},
                              {"offset_y_px",
                               hud ? QJsonValue(obs.hudOffset) : QJsonValue(QJsonValue::Null)}}},
          {"source", QJsonObject{{"name", source}, {"cycle_latency_ms", latency}}},
          {"scoreboard",
           QJsonObject{{"status", visible ? "visible" : "unavailable"},
                       {"confirmed", panelCount_ >= 2},
                       {"bounds", visible ? QJsonValue(obs.bounds) : QJsonValue(QJsonValue::Null)},
                       {"coordinate_width", 1600}}},
          {"players", rows}};
}
QJsonObject State::age(QJsonObject snapshot, qint64 ageMs) {
  snapshot["source_age_ms"] = double(std::max<qint64>(0, ageMs));
  if (ageMs > 2500)
    snapshot["source_status"] = "stale";
  for (const auto *collection : {"players", "participants"}) {
    auto rows = snapshot[collection].toArray();
    for (int i = 0; i < rows.size(); i++) {
      auto p = rows[i].toObject(), b = p["scoreboard"].toObject(), fields = b["fields"].toObject();
      for (auto it = fields.begin(); it != fields.end(); ++it) {
        auto f = it.value().toObject();
        if (f["age_ms"].isNull())
          continue;
        double age = f["age_ms"].toDouble() + std::max<qint64>(0, ageMs);
        f["age_ms"] = age;
        if (age > (it.key() == "display_name" ? 15000 : 1000) ||
            snapshot["source_status"] != "online")
          f["status"] = "stale";
        it.value() = f;
      }
      b["fields"] = fields;
      p["scoreboard"] = b;
      if (collection == QStringLiteral("participants") && snapshot["source_status"] != "online") {
        p["presence_status"] = "unconfirmed";
        p["position"] = QJsonValue::Null;
      }
      rows[i] = p;
    }
    snapshot[collection] = rows;
  }
  return snapshot;
}
} // namespace owcs
