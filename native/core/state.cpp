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
bool Field::observe(QJsonValue v, double conf, qint64 now, int required, int step,
                    qint64 candidateWindow) {
  if (candidateSeen >= 0 && now - candidateSeen > candidateWindow) {
    candidate = QJsonValue::Null;
    count = 0;
  }
  if (v.isNull() || v.isUndefined()) {
    // One unreadable frame is not contradictory evidence. Never refresh the
    // published value, but allow the next nearby observation to confirm it.
    return false;
  }
  bool same = v == candidate;
  if (step >= 0 && v.isDouble() && candidate.isDouble())
    same = v.toInt() >= candidate.toInt() && v.toInt() - candidate.toInt() <= step;
  count = same ? count + 1 : 1;
  candidate = v;
  candidateSeen = now;
  if (v == value || count >= required) {
    value = v;
    seen = now;
    observed = utcNow();
    confidence = std::clamp(conf, 0., 1.);
    return true;
  }
  return false;
}
QJsonObject Field::metadata(qint64 now, bool visible, int ttl) const {
  return {{"status", seen < 0 ? "unknown" : (visible && now - seen <= ttl ? "fresh" : "stale")},
          {"observed_at", seen < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(observed)},
          {"age_ms", seen < 0 ? QJsonValue(QJsonValue::Null)
                              : QJsonValue(double(std::max<qint64>(0, now - seen)))},
          {"confidence", confidence}};
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
  if (row < 0 || row >= 10 || players_[row].identityMisses > 0)
    return;
  players_[row].fields["display_name"].observe(nullable(result.id), result.confidence, now,
                                               result.confidence >= .88 ? 3 : 5, -1, 10000);
}
bool State::needsName(int row) const {
  return row >= 0 && row < 10 && players_[row].identityMisses == 0 &&
         players_[row].fields.value("display_name").value.isNull();
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
      if (!p.signature.empty() && !sig.empty() && p.signature.dot(sig) < .60) {
        p.identityMisses = !p.pendingSignature.empty() && p.pendingSignature.dot(sig) >= .70
                               ? p.identityMisses + 1
                               : 1;
        p.pendingSignature = sig;
        if (p.identityMisses < 3)
          continue;
        int gen = p.generation + 1;
        p = Player{};
        p.generation = gen;
      }
      p.identityMisses = 0;
      p.pendingSignature.release();
      if (!sig.empty())
        p.signature = sig;
      auto &f = p.fields;
      // A confirmed name is still observed while its text signature matches.
      // Avoid re-running name OCR merely to refresh unchanged identity metadata.
      if (!sig.empty() && !f["display_name"].value.isNull())
        f["display_name"].observe(f["display_name"].value, f["display_name"].confidence, now, 1);
      auto old = f["hero"].value;
      auto oldSelection = f["hero_selection"].value;
      auto selection =
          raw["hero_selection"].toString(raw["hero"].isNull() ? "unknown" : "selected");
      f["hero_selection"].observe(
          selection == "unknown" ? QJsonValue(QJsonValue::Null) : QJsonValue(selection),
          raw["selection_confidence"].toDouble(raw["hero_confidence"].toDouble()), now, 3);
      f["hero"].observe(raw["hero"], raw["hero_confidence"].toDouble(), now, 3);
      if (selection == "unselected" || !raw["hero"].isNull())
        p.heroPending = selection != f["hero_selection"].value ||
                        (!raw["hero"].isNull() && raw["hero"] != f["hero"].value);
      if ((!old.isNull() && f["hero"].value != old) || oldSelection != f["hero_selection"].value)
        for (auto key : {"ultimate_charge", "ultimate_ready", "minor", "major"})
          f.remove(key);
      bool unselected = f["hero_selection"].value == "unselected";
      if (unselected && selection != "selected")
        for (auto key : {"hero", "life", "respawn"})
          f.remove(key);
      if (!unselected && selection != "unselected") {
        f["life"].observe(raw["life_state"] == "unknown" ? QJsonValue(QJsonValue::Null)
                                                         : raw["life_state"],
                          raw["life_confidence"].toDouble(), now);
        f["respawn"].observe(raw["respawn_seconds"], raw["life_confidence"].toDouble(), now, 1);
        if (f["life"].value == "alive")
          f.remove("respawn");
      }
      if (!unselected && !p.heroPending &&
          f["ultimate_charge"].observe(raw["ultimate_charge"],
                                       raw["ultimate_confidence"].toDouble(), now, 2, 20))
        f["ultimate_ready"].observe(raw["ultimate_ready"], raw["ultimate_confidence"].toDouble(),
                                    now, 1);
      auto stats = raw["stats"].toObject(), conf = raw["stat_confidence"].toObject();
      for (auto it = stats.begin(); it != stats.end(); ++it)
        f[it.key()].observe(
            it.value(), conf[it.key()].toDouble(), now, 2,
            (it.key() == "damage" || it.key() == "healing" || it.key() == "mitigated") ? 500 : 3);
      for (auto tier : {"minor", "major"}) {
        auto perk = raw["perks"].toObject()[tier].toObject();
        auto confidence = perk.take("confidence").toDouble();
        bool accepted = !p.heroPending && perk["selection"] != "unknown" && !raw["hero"].isNull() &&
                        raw["hero"] == f["hero"].value;
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
      fields[it.key()] = it->metadata(now, visible && p.identityMisses == 0,
                                      it.key() == "display_name" ? 15000 : 1000);
    // Do not combine a newly observed charge/perk with a historical hero while
    // the replacement portrait is being confirmed. Keep counters independent.
    if (p.heroPending) {
      for (auto key : {"hero", "hero_selection", "life", "respawn", "ultimate_charge",
                       "ultimate_ready", "minor", "major"})
        fields[key] = Field{}.metadata(now, false);
      for (auto tier : {"minor", "major"})
        perks[tier] = QJsonObject{{"id", QJsonValue::Null}, {"selection", "unknown"}};
    }
    QString side = i < 5 ? "left" : "right";
    QJsonObject board{
        {"row", i % 5 + 1},
        {"slot_basis", "scoreboard_row"},
        {"identity_generation", p.generation},
        {"ultimate_charge",
         p.heroPending ? QJsonValue(QJsonValue::Null) : f["ultimate_charge"].value},
        {"ultimate_ready",
         p.heroPending ? QJsonValue(QJsonValue::Null) : f["ultimate_ready"].value},
        {"respawn_seconds", p.heroPending ? QJsonValue(QJsonValue::Null) : f["respawn"].value},
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
        {"life_state",
         p.heroPending || f["life"].value.isNull() ? QJsonValue("unknown") : f["life"].value},
        {"life_confidence", p.heroPending ? 0 : f["life"].confidence},
        {"life_status", p.heroPending || f["life"].value.isNull() ? "unknown" : "confirmed"},
        {"display_name", f["display_name"].value},
        {"display_name_confidence", f["display_name"].confidence},
        {"display_name_status", f["display_name"].value.isNull() ? "unknown" : "confirmed"},
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
