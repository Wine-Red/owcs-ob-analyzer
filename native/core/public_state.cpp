#include "public_state.hpp"
#include "player_rows.hpp"
#include <QJsonArray>
namespace owcs {
QString localizedDisplay(const QJsonValue &id, const QJsonValue &name) {
  if (!id.isString() || id.toString().isEmpty())
    return QStringLiteral("—");
  return name.isString() && !name.toString().isEmpty() ? name.toString() : id.toString();
}
QJsonObject localizedState(QJsonObject snapshot, const QJsonObject &names) {
  for (const auto *collection : {"players", "participants"}) {
    auto rows = snapshot[collection].toArray();
    auto label = [](const QJsonObject &map, const QJsonValue &id) -> QJsonValue {
      const auto name = id.isString() ? map[id.toString()].toString() : QString();
      return name.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(name);
    };
    for (int i = 0; i < rows.size(); ++i) {
      auto p = rows[i].toObject();
      const auto hero = p["hero"];
      p["hero_name_zhCN"] = label(names["heroes"].toObject(), hero);
      p["role"] = label(names["roles"].toObject(), hero);
      const auto heroPerks = names["perks"].toObject()[hero.toString()].toObject();
      auto board = p["scoreboard"].toObject(), perks = board["perks"].toObject();
      for (auto tier : {"minor", "major"}) {
        auto perk = perks[tier].toObject();
        perk["name_zhCN"] = perk["selection"] == "selected" && hero.isString()
                                ? label(heroPerks[tier].toObject(), perk["id"])
                                : QJsonValue(QJsonValue::Null);
        perks[tier] = perk;
      }
      board["perks"] = perks;
      p["scoreboard"] = board;
      rows[i] = p;
    }
    snapshot[collection] = rows;
  }
  return snapshot;
}
QJsonObject publicState(const QJsonObject &s) {
  QString status = s["source_status"].toString("waiting");
  if (!s["error"].isNull() && !s["error"].isUndefined())
    status = "error";
  else if (status == "online" && s["analysis_status"] == "delayed")
    status = "delayed";
  const bool panel = s["scoreboard"].toObject()["confirmed"].toBool() && status == "online";
  QJsonObject result{{"schema_version", "4.0"}, {"status", status}, {"panel_visible", panel}};
  result["game_paused"] = panel ? s.value("game_paused") : QJsonValue(QJsonValue::Null);
  for (auto key : {"stream_id", "session_id", "sequence", "observed_at"})
    if (s.contains(key))
      result[key] = s[key];
  if (status == "error")
    result["error"] = s["error"].isString() ? s["error"] : QJsonValue("recognition_unavailable");
  QJsonArray rows;
  for (const auto &value : orderedPlayerRows(s["participants"].toArray())) {
    const auto p = value.toObject(), b = p["scoreboard"].toObject();
    const auto id = p["player_id"].toString();
    if (id.isEmpty())
      continue;
    const auto fields = b["fields"].toObject();
    QJsonObject row{{"player_id", id},
                    {"presence_status", panel ? p["presence_status"] : QJsonValue("unconfirmed")},
                    {"team", p["side"] == "left"    ? QJsonValue("A")
                             : p["side"] == "right" ? QJsonValue("B")
                                                    : QJsonValue(QJsonValue::Null)}};
    for (auto key :
         {"display_name", "hero", "hero_name_zhCN", "role", "hero_selection", "life_state"})
      row[key] = p.contains(key) ? p[key] : QJsonValue(QJsonValue::Null);
    for (auto key : {"ultimate_charge", "ultimate_ready", "respawn_seconds", "stats", "perks"})
      row[key] = b.value(key);
    QJsonArray stale;
    // Keep observation history explicit; unknown values use null rather than a
    // second field-path list. No physical-slot or internal identity state leaks.
    const QPair<const char *, const char *> paths[] = {{"display_name", "display_name"},
                                                       {"hero", "hero"},
                                                       {"hero_selection", "hero_selection"},
                                                       {"life", "life_state"},
                                                       {"respawn", "respawn_seconds"},
                                                       {"ultimate_charge", "ultimate_charge"},
                                                       {"ultimate_ready", "ultimate_ready"},
                                                       {"minor", "perks.minor"},
                                                       {"major", "perks.major"},
                                                       {"eliminations", "stats.eliminations"},
                                                       {"assists", "stats.assists"},
                                                       {"deaths", "stats.deaths"},
                                                       {"damage", "stats.damage"},
                                                       {"healing", "stats.healing"},
                                                       {"mitigated", "stats.mitigated"}};
    for (const auto &[key, path] : paths) {
      const auto state = fields[key].toObject()["status"].toString("unknown");
      if (state == "fresh" || state == "stale") {
        if (state == "stale" || !panel)
          stale.append(path);
        continue;
      }
      const auto parts = QString::fromLatin1(path).split('.');
      if (parts.size() == 1)
        row[parts[0]] = QJsonValue(QJsonValue::Null);
      else {
        auto group = row[parts[0]].toObject();
        group[parts[1]] = QJsonValue(QJsonValue::Null);
        row[parts[0]] = group;
      }
    }
    if (row["hero"].isNull()) {
      row["hero_name_zhCN"] = QJsonValue(QJsonValue::Null);
      row["role"] = QJsonValue(QJsonValue::Null);
    }
    row["stale_fields"] = stale;
    rows.append(row);
  }
  result["players"] = rows;
  return result;
}
} // namespace owcs
