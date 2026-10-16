#include "public_state.hpp"
#include <QJsonArray>
namespace owcs {
QString localizedDisplay(const QJsonValue &id, const QJsonValue &name) {
  if (!id.isString() || id.toString().isEmpty())
    return QStringLiteral("—");
  return name.isString() && !name.toString().isEmpty() ? name.toString() : id.toString();
}
QJsonObject localizedState(QJsonObject snapshot, const QJsonObject &names) {
  auto rows = snapshot["players"].toArray();
  auto label = [](const QJsonObject &map, const QJsonValue &id) -> QJsonValue {
    const auto name = id.isString() ? map[id.toString()].toString() : QString();
    return name.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(name);
  };
  for (int i = 0; i < rows.size(); ++i) {
    auto p = rows[i].toObject();
    const auto hero = p["hero"];
    p["hero_name_zhCN"] = label(names["heroes"].toObject(), hero);
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
  snapshot["players"] = rows;
  return snapshot;
}
QJsonObject publicState(const QJsonObject &s) {
  QString status = s["source_status"].toString("waiting");
  if (!s["error"].isNull() && !s["error"].isUndefined())
    status = "error";
  else if (status == "online" && s["analysis_status"] == "delayed")
    status = "delayed";
  const bool panel = s["scoreboard"].toObject()["confirmed"].toBool() && status == "online";
  QJsonObject result{{"schema_version", "3.0"}, {"status", status}, {"panel_visible", panel}};
  for (auto key : {"stream_id", "session_id", "sequence", "observed_at"})
    if (s.contains(key))
      result[key] = s[key];
  if (status == "error")
    result["error"] = s["error"].isString() ? s["error"] : QJsonValue("recognition_unavailable");
  QJsonArray rows;
  for (const auto &value : s["players"].toArray()) {
    const auto p = value.toObject(), b = p["scoreboard"].toObject();
    const auto fields = b["fields"].toObject();
    QJsonObject row;
    for (auto key :
         {"slot_id", "side", "slot", "display_name", "hero", "hero_selection", "life_state"})
      row[key] = p.value(key);
    row["hero_name_zhCN"] = p.value("hero_name_zhCN").isUndefined() ? QJsonValue(QJsonValue::Null)
                                                                    : p.value("hero_name_zhCN");
    for (auto key : {"identity_generation", "ultimate_charge", "ultimate_ready", "respawn_seconds",
                     "stats", "perks"})
      row[key] = b.value(key);
    QJsonArray stale, unknown;
    // Explicit field paths form a stable contract, independent of internal names.
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
      if (state != "fresh" && state != "stale")
        unknown.append(path);
      else if (state == "stale" || !panel)
        stale.append(path);
    }
    row["stale_fields"] = stale;
    row["unknown_fields"] = unknown;
    rows.append(row);
  }
  result["players"] = rows;
  return result;
}
} // namespace owcs
