#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <tuple>
#include <vector>

namespace owcs {
inline constexpr int defaultPlayerPriority = 1000;
inline int playerPriority(const QJsonObject &priorities, const QString &id) {
  const auto value = priorities.value(id);
  const int number = value.toInt(defaultPlayerPriority);
  return value.isDouble() && value.toDouble() == number && number >= 1 && number <= 9999
             ? number
             : defaultPlayerPriority;
}
inline QJsonObject normalizedPlayerPriorities(const QJsonObject &priorities) {
  QJsonObject result;
  for (auto it = priorities.begin(); it != priorities.end(); ++it) {
    const int number = playerPriority(priorities, it.key());
    if (it.key().startsWith("name:") && it.key().size() > 5 && number != defaultPlayerPriority)
      result[it.key()] = number;
  }
  return result;
}
// Presentation only: keep whole named records intact while ordering the table
// by priority, then scoreboard position. A temporarily unconfirmed name keeps its last known
// placement; only confirmed departures move to the historical section.
inline QJsonArray orderedPlayerRows(const QJsonArray &participants,
                                    const QJsonObject &priorities = {}) {
  std::vector<QJsonObject> rows;
  rows.reserve(participants.size());
  for (const auto &value : participants)
    rows.push_back(value.toObject());
  auto key = [&](const QJsonObject &row) {
    const auto side = row["side"].toString();
    const auto slot = row["slot"].toInt();
    const bool valid = (side == "left" || side == "right") && slot >= 1 && slot <= 5;
    return std::make_tuple(row["presence_status"] == "missing" || !valid ? 1 : 0,
                           playerPriority(priorities, row["player_id"].toString()),
                           side == "left"    ? 0
                           : side == "right" ? 1
                                             : 2,
                           valid ? slot : 6, row["presence_status"] == "present" ? 0 : 1);
  };
  std::stable_sort(rows.begin(), rows.end(),
                   [&](const auto &a, const auto &b) { return key(a) < key(b); });
  QJsonArray result;
  for (const auto &row : rows)
    result.append(row);
  return result;
}
} // namespace owcs
