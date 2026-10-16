#include "state.hpp"
#include <QDateTime>
#include <QUuid>
#include <algorithm>
namespace owcs {
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
  return {{"status", seen < 0 ? "unknown" : (visible && now - seen <= ttl ? "fresh" : "stale")},
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
  sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
  sequence_ = 0;
  lastVisible_ = -1;
  panelCount_ = 0;
}
void State::name(int row, Match result, qint64 now) {
  if (row < 0 || row >= 10)
    return;
  auto &p = players_[row];
  auto prior = p.fields["display_name"].value;
  if (!prior.isNull() && prior.toString().toCaseFolded() == result.id.toCaseFolded())
    result.id = prior.toString();
  if (!p.pendingName.observe(nullable(result.id), result.confidence, now,
                             result.confidence >= .88 ? 3 : 5, Confirmation::Exact, 10000))
    return;
  if (!prior.isNull() && prior != p.pendingName.value) {
    // Only confirmed text identity can reset the row. Visual name jitter may
    // request OCR, but cannot freeze counters or erase another field's history.
    p.fields.clear();
    p.heroPending = false;
    ++p.generation;
  }
  p.fields["display_name"] = p.pendingName;
  p.signature = p.currentSignature.clone();
  p.nameUncertain = false;
}
bool State::needsName(int row) const {
  return row >= 0 && row < 10 &&
         (players_[row].nameUncertain || players_[row].fields.value("display_name").value.isNull());
}
QJsonObject State::update(const Observation &obs, const QString &source, qint64 now,
                          double latency) {
  bool visible = obs.visible && obs.players.size() == 10;
  if (visible) {
    // Hiding Tab is not evidence of a different player. Existing values remain
    // stale until observed again; an actual name change is confirmed below.
    lastVisible_ = now;
    panelCount_ = obs.confirmed ? panelCount_ + 1 : 0;
    for (int i = 0; i < 10; i++) {
      auto raw = obs.players[i].toObject();
      auto &p = players_[i];
      auto sig = Recognizer::signature(obs.names[i]);
      p.currentSignature = sig;
      if (!sig.empty()) {
        if (p.signature.empty())
          p.signature = sig;
        bool changed = p.signature.dot(sig) < .60;
        if (changed != p.nameUncertain)
          p.pendingName = Field{};
        p.nameUncertain = changed;
      }
      auto &f = p.fields;
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
          p.heroPending = selection != f["hero_selection"].value ||
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
        f[tier].observe(accepted ? QJsonValue(perk) : QJsonValue(QJsonValue::Null), confidence, now,
                        3);
      }
    }
  } else {
    panelCount_ = 0;
    for (auto &p : players_)
      for (auto &f : p.fields) {
        f.count = 0;
        f.candidate = QJsonValue::Null;
      }
  }
  QJsonArray rows;
  for (int i = 0; i < 10; i++) {
    auto &p = players_[i];
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
      fields[it.key()] = it->metadata(now, visible, it.key() == "display_name" ? 15000 : 1000);
    // Row-local readings remain usable while its name is being rechecked, but
    // must not be attributed to the old name in either the dock or public API.
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
    rows.append(QJsonObject{
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
        {"display_name_confidence", f["display_name"].confidence},
        {"display_name_status",
         p.nameUncertain || f["display_name"].value.isNull() ? "unknown" : "confirmed"},
        {"updated_at", utcNow()},
        {"scoreboard", board}});
  }
  bool hud = obs.hudOffset != -10000;
  return {{"type", "state.snapshot"},
          {"schema_version", "2.0"},
          {"stream_id", streamId_},
          {"session_id", sessionId_},
          {"sequence", double(++sequence_)},
          {"observed_at", utcNow()},
          {"source_age_ms", 0},
          {"source_status", "online"},
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
  auto rows = snapshot["players"].toArray();
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
    rows[i] = p;
  }
  snapshot["players"] = rows;
  return snapshot;
}
} // namespace owcs
