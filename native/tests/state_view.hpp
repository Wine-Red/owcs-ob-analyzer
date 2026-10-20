#pragma once
#include <QJsonArray>
#include <QJsonObject>

namespace test {
// Inspect the internal state machine by physical row, independently of the API.
inline QJsonObject physicalPlayer(const QJsonObject &snapshot, int index) {
  auto row = snapshot["players"].toArray()[index].toObject();
  const auto board = row["scoreboard"].toObject();
  for (auto it = board.begin(); it != board.end(); ++it)
    row[it.key()] = it.value();
  QJsonArray stale;
  const auto fields = board["fields"].toObject();
  for (auto it = fields.begin(); it != fields.end(); ++it) {
    if (it.value().toObject()["status"] != "stale")
      continue;
    auto key = it.key();
    if (board["stats"].toObject().contains(key))
      key = "stats." + key;
    stale.append(key);
  }
  row["stale_fields"] = stale;
  return row;
}
inline QJsonObject player(const QJsonObject &snapshot, const QString &id) {
  for (const auto &value : snapshot["players"].toArray())
    if (value.toObject()["player_id"] == id)
      return value.toObject();
  return {};
}
inline QJsonArray ids(const QJsonObject &snapshot) {
  QJsonArray ids;
  for (const auto &value : snapshot["players"].toArray())
    ids.append(value.toObject()["player_id"]);
  return ids;
}
} // namespace test
