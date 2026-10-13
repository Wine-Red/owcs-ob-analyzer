#pragma once
#include <QJsonObject>
namespace owcs {
// The public contract deliberately excludes recognizer diagnostics.
QJsonObject publicState(const QJsonObject &snapshot);
} // namespace owcs
