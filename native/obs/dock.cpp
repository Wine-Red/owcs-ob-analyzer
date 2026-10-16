#include "dock.hpp"
#include "../core/public_state.hpp"
#include "../runtime/registry.hpp"
#include "../runtime/server.hpp"
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
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
#include <QWidget>
#include <obs-frontend-api.h>
#include <obs-module.h>
namespace {
class Dock : public QWidget {
public:
  owcs::Server server{this};
  QTimer timer;
  QComboBox *streams;
  QLabel *status;
  QLabel *address;
  QTableWidget *table;
  QSpinBox *port;
  QCheckBox *lan;
  Dock() {
    auto layout = new QVBoxLayout(this);
    auto instructions =
        new QLabel(QStringLiteral("在任意视频源的「滤镜」中添加「OWCS 观战数据识别」。"));
    instructions->setWordWrap(true);
    layout->addWidget(instructions);
    streams = new QComboBox;
    layout->addWidget(streams);
    status = new QLabel(QStringLiteral("等待视频源"));
    status->setWordWrap(true);
    layout->addWidget(status);
    table = new QTableWidget(10, 11);
    table->setHorizontalHeaderLabels({QStringLiteral("队/行"), QStringLiteral("选手"),
                                      QStringLiteral("英雄"), QStringLiteral("生存"),
                                      QStringLiteral("大招"), QStringLiteral("威能"), "E", "A", "D",
                                      QStringLiteral("伤害"), QStringLiteral("治疗/抵挡")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->verticalHeader()->hide();
    layout->addWidget(table);
    auto settings = new QHBoxLayout;
    port = new QSpinBox;
    port->setRange(1024, 65535);
    port->setValue(8765);
    lan = new QCheckBox(QStringLiteral("允许局域网"));
    auto apply = new QPushButton(QStringLiteral("应用接口设置"));
    settings->addWidget(port);
    settings->addWidget(lan);
    settings->addWidget(apply);
    layout->addLayout(settings);
    address = new QLabel;
    address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(address);
    auto actions = new QHBoxLayout;
    auto copy = new QPushButton(QStringLiteral("复制 JSON"));
    auto save = new QPushButton(QStringLiteral("导出 JSON"));
    actions->addWidget(copy);
    actions->addWidget(save);
    layout->addLayout(actions);
    char *config = obs_module_config_path("settings.json");
    obs_data_t *data = config ? obs_data_create_from_json_file(config) : nullptr;
    if (data) {
      int p = int(obs_data_get_int(data, "port"));
      if (p >= 1024 && p <= 65535)
        port->setValue(p);
      lan->setChecked(obs_data_get_bool(data, "lan"));
      obs_data_release(data);
    }
    bfree(config);
    connect(streams, &QComboBox::currentIndexChanged, this, [this] {
      owcs::Registry::instance().select(streams->currentData().toString());
      refresh();
    });
    connect(apply, &QPushButton::clicked, this, [this] {
      startServer();
      char *path = obs_module_config_path("settings.json");
      if (path) {
        auto settings = obs_data_create();
        obs_data_set_int(settings, "port", port->value());
        obs_data_set_bool(settings, "lan", lan->isChecked());
        QFileInfo info(QString::fromUtf8(path));
        QDir().mkpath(info.absolutePath());
        obs_data_save_json_safe(settings, path, "tmp", "bak");
        obs_data_release(settings);
        bfree(path);
      }
    });
    connect(copy, &QPushButton::clicked, this, [] {
      QApplication::clipboard()->setText(QString::fromUtf8(
          QJsonDocument(owcs::publicState(owcs::Registry::instance().state())).toJson()));
    });
    connect(save, &QPushButton::clicked, this, [this] {
      auto path = QFileDialog::getSaveFileName(this, QStringLiteral("导出当前观战数据"),
                                               "owcs-state.json", "JSON (*.json)");
      if (path.isEmpty())
        return;
      QSaveFile file(path);
      if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(owcs::publicState(owcs::Registry::instance().state())).toJson());
        file.commit();
      }
    });
    timer.setInterval(500);
    connect(&timer, &QTimer::timeout, this, [this] { refresh(); });
    timer.start();
    startServer();
    resize(900, 440);
  }
  void startServer() {
    server.listen(quint16(port->value()), lan->isChecked());
    address->setText(server.status());
  }
  static QString show(QJsonValue v) {
    return v.isNull() || v.isUndefined()
               ? QStringLiteral("—")
               : (v.isString() ? v.toString() : QString::number(v.toInt()));
  }
  void refresh() {
    auto list = owcs::Registry::instance().streams();
    {
      QSignalBlocker block(streams);
      auto selected = streams->currentData().toString();
      streams->clear();
      for (auto v : list) {
        auto o = v.toObject();
        streams->addItem(o["name"].toString() + " · " + o["stream_id"].toString().left(8),
                         o["stream_id"].toString());
      }
      int index = streams->findData(selected);
      if (index >= 0)
        streams->setCurrentIndex(index);
    }
    auto s = owcs::Registry::instance().state();
    auto b = s["scoreboard"].toObject();
    const auto rows = s["players"].toArray();
    bool retained = false;
    for (auto row : rows) {
      const auto player = row.toObject();
      retained |= !player["hero"].isNull() && !player["hero"].isUndefined();
    }
    QString text =
        b["confirmed"].toBool() ? QStringLiteral("面板已确认") : QStringLiteral("等待完整观战面板");
    if (!b["confirmed"].toBool() && s["hud"].toObject()["status"] == "locked")
      text = QStringLiteral("已识别观战 HUD，等待 Tab 面板；自动 Tab 开启时请将游戏保持在前台");
    if (s["analysis_status"] == "delayed")
      text = QStringLiteral("视频输入正常，识别正在处理；暂时显示已获得的结果");
    if (s["analysis_status"] == "initializing")
      text = QStringLiteral("已收到视频，正在初始化识别");
    if (s["source_status"] == "stale")
      text = QStringLiteral("超过 2.5 秒未收到视频帧：检查来源是否仍在渲染");
    if (s["source_status"] == "stopped")
      text = QStringLiteral("识别已暂停");
    if (!s["error"].isNull() && !s["error"].isUndefined())
      text = s["error"].toString();
    if (retained && (!b["confirmed"].toBool() || s["source_status"] != "online"))
      text += QStringLiteral("。灰色为上次识别结果（非实时），切回游戏显示面板后继续更新");
    else if (retained)
      text += QStringLiteral("；灰色值为历史观测，— 表示尚未识别");
    status->setText(text);
    for (int i = 0; i < 10; i++) {
      auto p = (i < rows.size() ? rows[i] : QJsonValue{}).toObject(),
           board = p["scoreboard"].toObject(), stats = board["stats"].toObject(),
           fields = board["fields"].toObject();
      auto fresh = [&](QString key, QJsonValue value) {
        return fields[key].toObject()["status"] == "unknown" ? QStringLiteral("—") : show(value);
      };
      auto perks = board["perks"].toObject();
      QString perkText;
      for (auto tier : {"minor", "major"}) {
        auto v = perks[tier].toObject();
        if (v["id"].isString())
          perkText += owcs::localizedDisplay(v["id"], v["name_zhCN"]) + " ";
      }
      QStringList values{
          QString(i < 5 ? "A%1" : "B%1").arg(i % 5 + 1),
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
          fresh("healing", stats["healing"]) + " / " + fresh("mitigated", stats["mitigated"])};
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
                                         {"healing", "mitigated"}};
      for (int j = 0; j < values.size(); j++) {
        if (!table->item(i, j))
          table->setItem(i, j, new QTableWidgetItem);
        table->item(i, j)->setText(values[j]);
        bool stale = false;
        for (const auto &key : fieldKeys[j])
          stale |= fields[key].toObject()["status"] == "stale";
        table->item(i, j)->setForeground(stale ? QBrush(QColor(145, 150, 160)) : QBrush());
        table->item(i, j)->setToolTip(stale ? QStringLiteral("上次识别结果，已过期，不代表当前状态")
                                            : QString());
      }
    }
  }
};
Dock *dock = nullptr;
} // namespace
void createAnalyzerDock() {
  if (dock)
    return;
  dock = new Dock;
  if (!obs_frontend_add_dock_by_id("owcs-ob-analyzer", "OWCS 观战数据", dock)) {
    delete dock;
    dock = nullptr;
  }
}
void destroyAnalyzerDock() {
  if (!dock)
    return;
  dock->timer.stop();
  dock->server.stop();
  obs_frontend_remove_dock("owcs-ob-analyzer");
  dock = nullptr;
}
