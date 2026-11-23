#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
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
// State emits participants in session-local first-confirmation order. Stable
// priority sorting preserves that order for ties, regardless of presence or
// scoreboard position. Move complete records without changing their metadata.
inline QJsonArray orderedPlayerRows(const QJsonArray &participants,
                                    const QJsonObject &priorities = {}) {
  std::vector<QJsonObject> rows;
  rows.reserve(participants.size());
  for (const auto &value : participants)
    rows.push_back(value.toObject());
  std::stable_sort(rows.begin(), rows.end(), [&](const auto &a, const auto &b) {
    return playerPriority(priorities, a["player_id"].toString()) <
           playerPriority(priorities, b["player_id"].toString());
  });
  QJsonArray result;
  for (const auto &row : rows)
    result.append(row);
  return result;
}
} // namespace owcs
