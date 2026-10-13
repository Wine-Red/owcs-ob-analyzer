#include "public_state.hpp"
#include <QJsonArray>
namespace owcs {
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
