#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <tuple>
#include <vector>

namespace owcs {
// Presentation only: keep whole named records intact while ordering the table
// by scoreboard position. A temporarily unconfirmed name keeps its last known
// placement; only confirmed departures move to the historical section.
inline QJsonArray orderedPlayerRows(const QJsonArray &participants) {
  std::vector<QJsonObject> rows;
  rows.reserve(participants.size());
  for (const auto &value : participants)
    rows.push_back(value.toObject());
  auto key = [](const QJsonObject &row) {
    const auto side = row["side"].toString();
    const auto slot = row["slot"].toInt();
    const bool valid = (side == "left" || side == "right") && slot >= 1 && slot <= 5;
    return std::make_tuple(row["presence_status"] == "missing" || !valid ? 1 : 0,
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
