#include "player_table.hpp"
#include "../core/player_rows.hpp"
#include "../core/public_state.hpp"
#include "../runtime/registry.hpp"
#include <QApplication>
#include <QBrush>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace owcs {
static QString displayValue(QJsonValue v) {
  return v.isNull() || v.isUndefined() ? QStringLiteral("—")
                                       : (v.isString() ? v.toString() : QString::number(v.toInt()));
}
PlayerTable::PlayerTable(QString orderFile, QWidget *parent)
    : QWidget(parent), orderFile_(std::move(orderFile)) {
  auto layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  auto hint = new QLabel(QStringLiteral("选手优先级：数字越小越靠前；同值按本次会话首次确认顺序排列"
                                        "。掉线或暂时识别不到时保留位置和历史数据。"));
  hint->setWordWrap(true);
  layout->addWidget(hint);
  table_ = new QTableWidget(0, 13);
  table_->setHorizontalHeaderLabels({QStringLiteral("优先级"), QStringLiteral("状态/位置"),
                                     QStringLiteral("选手"), QStringLiteral("英雄"),
                                     QStringLiteral("生存"), QStringLiteral("大招"),
                                     QStringLiteral("威能"), "E", "A", "D", QStringLiteral("伤害"),
                                     QStringLiteral("治疗/抵挡"), QStringLiteral("职责")});
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table_->verticalHeader()->hide();
  layout->addWidget(table_);

  auto reset = new QPushButton(QStringLiteral("恢复默认顺序"));
  reset->setObjectName("resetPlayerOrder");
  reset->setToolTip(QStringLiteral("清除所有来源的选手优先级设置，恢复各会话首次确认的顺序"));
  layout->addWidget(reset);
  message_ = new QLabel;
  message_->setObjectName("playerOrderMessage");
  message_->setWordWrap(true);
  layout->addWidget(message_);
  QFile file(orderFile_);
  if (file.exists()) {
    QJsonParseError error;
    const bool opened = file.open(QIODevice::ReadOnly);
    auto doc = QJsonDocument::fromJson(opened ? file.readAll() : QByteArray{}, &error);
    if (opened && error.error == QJsonParseError::NoError && doc.isObject())
      Registry::instance().setPlayerPriorities(doc.object());
    else
      message_->setText(QStringLiteral("无法读取选手排序设置，保留当前顺序。"));
  }
  connect(reset, &QPushButton::clicked, this, [this] {
    if (savePriorities({}))
      render();
  });
}
bool PlayerTable::savePriorities(const QJsonObject &priorities) {
  const auto clean = normalizedPlayerPriorities(priorities);
  QSaveFile file(orderFile_);
  const auto data = QJsonDocument(clean).toJson();
  if (!QDir().mkpath(QFileInfo(orderFile_).absolutePath()) || !file.open(QIODevice::WriteOnly) ||
      file.write(data) != data.size() || !file.commit()) {
    message_->setText(QStringLiteral("选手排序保存失败，仍使用原顺序：") + file.errorString());
    return false;
  }
  Registry::instance().setPlayerPriorities(clean);
  message_->clear();
  return true;
}
void PlayerTable::refresh(const QJsonObject &snapshot) {
  snapshot_ = snapshot;
  render();
}
void PlayerTable::render() {
  const auto &s = snapshot_;
  const auto priorities = Registry::instance().playerPriorities();
  auto rows = orderedPlayerRows(s["participants"].toArray(), priorities);
  auto focus = QApplication::focusWidget();
  const bool editing =
      focus && table_->isAncestorOf(focus) &&
      (qobject_cast<QSpinBox *>(focus) || qobject_cast<QSpinBox *>(focus->parentWidget()));
  if (editing) {
    // Keep the editor attached to its player while still refreshing live stats.
    // Apply changed row positions after focus leaves the priority editor.
    QJsonArray pinned;
    for (int i = 0; i < table_->rowCount(); ++i) {
      const auto item = table_->item(i, 1);
      if (!item)
        continue;
      const auto id = item->data(Qt::UserRole).toString();
      for (int j = 0; j < rows.size(); ++j)
        if (rows[j].toObject()["player_id"] == id) {
          pinned.append(rows.takeAt(j));
          break;
        }
    }
    for (const auto &row : rows)
      pinned.append(row);
    rows = pinned;
  }
  table_->setRowCount(rows.size());
  for (int i = 0; i < rows.size(); i++) {
    auto p = (i < rows.size() ? rows[i] : QJsonValue{}).toObject(),
         board = p["scoreboard"].toObject(), stats = board["stats"].toObject(),
         fields = board["fields"].toObject();
    const auto id = p["player_id"].toString();
    auto priority = qobject_cast<QSpinBox *>(table_->cellWidget(i, 0));
    if (!priority || priority->objectName() != "priority:" + id) {
      priority = new QSpinBox(table_);
      priority->setObjectName("priority:" + id);
      priority->setRange(1, 9999);
      priority->setKeyboardTracking(false);
      priority->setValue(playerPriority(priorities, id));
      priority->setEnabled(!id.isEmpty());
      priority->setToolTip(QStringLiteral(
          "数字越小越靠前；默认 1000；同值按本次会话首次确认顺序排列。回车或离开输入框后保存"));
      table_->setCellWidget(i, 0, priority);
      connect(priority, &QSpinBox::editingFinished, this, [this, priority, id] {
        auto updated = Registry::instance().playerPriorities();
        const int number = priority->value();
        if (number != playerPriority(updated, id)) {
          updated[id] = number;
          savePriorities(updated);
        }
        // Finish the editor event before replacing widgets during reordering.
        {
          QSignalBlocker block(priority);
          priority->clearFocus();
        }
        QTimer::singleShot(0, this, [this] { render(); });
      });
    } else if (!priority->hasFocus() && !priority->isAncestorOf(focus)) {
      QSignalBlocker block(priority);
      priority->setValue(playerPriority(priorities, id));
    }
    auto fresh = [&](QString key, QJsonValue value) {
      return fields[key].toObject()["status"] == "unknown" ? QStringLiteral("—")
                                                           : displayValue(value);
    };
    auto perks = board["perks"].toObject();
    QString perkText;
    for (auto tier : {"minor", "major"}) {
      auto v = perks[tier].toObject();
      if (v["id"].isString())
        perkText += owcs::localizedDisplay(v["id"], v["name_zhCN"]) + " ";
    }
    QStringList values{p["presence_status"] == "missing" ? QStringLiteral("离场")
                       : p["presence_status"] != "present" || s["source_status"] != "online" ||
                               s["analysis_status"] == "delayed"
                           ? QStringLiteral("待确认")
                           : QString(p["side"] == "left" ? "A%1" : "B%1").arg(p["slot"].toInt()),
                       fresh("display_name", p["display_name"]),
                       p["hero_selection"] == "unselected"
                           ? fresh("hero_selection", QStringLiteral("未选英雄"))
                           : fresh("hero", owcs::localizedDisplay(p["hero"], p["hero_name_zhCN"])),
                       fresh("life", p["life_state"]),
                       fresh("ultimate_charge", board["ultimate_charge"]),
                       perkText,
                       fresh("eliminations", stats["eliminations"]),
                       fresh("assists", stats["assists"]),
                       fresh("deaths", stats["deaths"]),
                       fresh("damage", stats["damage"]),
                       fresh("healing", stats["healing"]) + " / " +
                           fresh("mitigated", stats["mitigated"]),
                       fresh("hero", p["role"] == "tank"      ? QStringLiteral("重装")
                                     : p["role"] == "damage"  ? QStringLiteral("输出")
                                     : p["role"] == "support" ? QStringLiteral("支援")
                                                              : QStringLiteral("—"))};
    const QList<QStringList> fieldKeys{{},
                                       {"display_name"},
                                       {"hero", "hero_selection"},
                                       {"life"},
                                       {"ultimate_charge"},
                                       {"minor", "major"},
                                       {"eliminations"},
                                       {"assists"},
                                       {"deaths"},
                                       {"damage"},
                                       {"healing", "mitigated"},
                                       {"hero"}};
    for (int j = 0; j < values.size(); j++) {
      if (!table_->item(i, j + 1))
        table_->setItem(i, j + 1, new QTableWidgetItem);
      table_->item(i, j + 1)->setText(values[j]);
      table_->item(i, j + 1)->setData(Qt::UserRole, p["player_id"].toString());
      bool stale = false;
      for (const auto &key : fieldKeys[j])
        stale |= fields[key].toObject()["status"] == "stale";
      table_->item(i, j + 1)->setForeground(stale ? QBrush(QColor(145, 150, 160)) : QBrush());
      table_->item(i, j + 1)->setToolTip(
          stale ? QStringLiteral("上次识别结果，已过期，不代表当前状态") : QString());
    }
  }
}
} // namespace owcs
