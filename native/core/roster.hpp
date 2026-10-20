#pragma once
#include <QJsonObject>
#include <QStringList>
namespace owcs {
struct RosterPage {
  QStringList names, warnings;
  int participants = 0;
};
QString liquipediaPage(const QString &urlOrTitle);
QString liquipediaSearch(const QString &text);
RosterPage parseTournamentRoster(const QString &html);
} // namespace owcs
