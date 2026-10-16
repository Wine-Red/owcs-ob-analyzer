#pragma once
#include <QJsonObject>
namespace owcs {
// The public contract deliberately excludes recognizer diagnostics.
QJsonObject publicState(const QJsonObject &snapshot);
// Attach display names only after identity/state confirmation. Missing labels stay null.
QJsonObject localizedState(QJsonObject snapshot, const QJsonObject &names);
QString localizedDisplay(const QJsonValue &id, const QJsonValue &name);
} // namespace owcs
