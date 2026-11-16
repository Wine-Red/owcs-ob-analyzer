#pragma once
#include <QJsonObject>
#include <QWidget>
class QTableWidget;
class QLabel;
namespace owcs {
class PlayerTable : public QWidget {
public:
  explicit PlayerTable(QString orderFile, QWidget *parent = nullptr);
  void refresh(const QJsonObject &snapshot);

private:
  bool savePriorities(const QJsonObject &priorities);
  void render();
  QString orderFile_;
  QJsonObject snapshot_;
  QTableWidget *table_;
  QLabel *message_;
};
} // namespace owcs
