#pragma once
#include "../runtime/liquipedia.hpp"
#include <QDialog>
namespace owcs {
void loadRoster(const QString &file);
QDialog *createRosterDialog(const QString &file, LiquipediaClient *, QWidget *parent);
} // namespace owcs
