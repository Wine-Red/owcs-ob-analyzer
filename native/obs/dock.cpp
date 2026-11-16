#include "dock.hpp"
#include "../core/player_rows.hpp"
#include "../core/public_state.hpp"
#include "../runtime/registry.hpp"
#include "../runtime/server.hpp"
#include "player_table.hpp"
#include "roster_dialog.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
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
  owcs::PlayerTable *table;
  QSpinBox *port;
  QCheckBox *lan;
  QPushButton *rosterButton;
  QPushButton *clearButton;
  QPointer<QDialog> rosterDialog;
  Dock() {
    auto layout = new QVBoxLayout(this);
    auto instructions =
        new QLabel(QStringLiteral("在任意视频源的「滤镜」中添加「OWCS 观战数据识别」。"));
    instructions->setWordWrap(true);
    layout->addWidget(instructions);
    streams = new QComboBox;
    layout->addWidget(streams);
    char *rosterPath = obs_module_config_path("roster.json");
    QString rosterFile = QString::fromUtf8(rosterPath ? rosterPath : "");
    bfree(rosterPath);
    owcs::loadRoster(rosterFile);
    auto rosterClient = new owcs::LiquipediaClient(
        QFileInfo(rosterFile).absolutePath() + "/liquipedia-cache", this);
    rosterButton = new QPushButton(QStringLiteral("选手名单"));
    layout->addWidget(rosterButton);
    connect(rosterButton, &QPushButton::clicked, this, [this, rosterFile, rosterClient] {
      if (!rosterDialog)
        rosterDialog = owcs::createRosterDialog(rosterFile, rosterClient, this);
      rosterDialog->show();
      rosterDialog->raise();
      rosterDialog->activateWindow();
    });
    status = new QLabel(QStringLiteral("等待视频源"));
    status->setWordWrap(true);
    layout->addWidget(status);
    char *orderPath = obs_module_config_path("player-order.json");
    table = new owcs::PlayerTable(QString::fromUtf8(orderPath ? orderPath : ""));
    bfree(orderPath);
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
    clearButton = new QPushButton(QStringLiteral("清空数据"));
    clearButton->setObjectName("owcsClearData");
    clearButton->setToolTip(
        QStringLiteral("清空当前来源的选手数据和历史，保留选手名单与设置；识别继续运行"));
    actions->addWidget(copy);
    actions->addWidget(save);
    actions->addWidget(clearButton);
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
    connect(clearButton, &QPushButton::clicked, this, [this] {
      owcs::Registry::instance().clear(streams->currentData().toString());
      refresh();
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
  void refresh() {
    rosterButton->setText(
        QStringLiteral("选手名单（%1 人）").arg(owcs::Registry::instance().roster().size()));
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
    clearButton->setEnabled(streams->count() > 0);
    auto b = s["scoreboard"].toObject();
    const auto rows = owcs::orderedPlayerRows(s["participants"].toArray());
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
    if (s["analysis_status"] == "cleared")
      text = QStringLiteral("数据已清空，等待新画面重新识别");
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
    if (s["game_paused"].toBool())
      status->setText(text + QStringLiteral("；游戏暂停，遮挡处保留历史，其余行继续识别"));
    table->refresh(s);
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
