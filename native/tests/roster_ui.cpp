#include "core/roster.hpp"
#include "obs/roster_dialog.hpp"
#include "runtime/registry.hpp"
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  if (app.arguments().size() < 2)
    return 2;
  const auto fixtures = app.arguments()[1];
  QTemporaryDir temp;
  const auto cache = temp.path() + "/cache";
  const auto file = temp.path() + "/roster.json";
  QDir().mkpath(cache);
  int checks = 0, failures = 0;
  auto check = [&](bool ok, const char *name) {
    ++checks;
    if (!ok) {
      ++failures;
      std::cerr << name << "\n";
    }
  };
  auto cached = [&](QUrlQuery query, QJsonObject data) {
    query.addQueryItem("format", "json");
    auto path = "/overwatch/api.php?" + query.toString(QUrl::FullyEncoded);
    auto key = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
    QFile f(cache + "/" + key + ".json");
    f.open(QIODevice::WriteOnly);
    f.write(QJsonDocument(
                QJsonObject{{"time", double(QDateTime::currentSecsSinceEpoch())}, {"data", data}})
                .toJson());
  };
  const QString title = "Overwatch Champions Series/2026/Champions Clash";
  cached(QUrlQuery{{"action", "query"},
                   {"list", "search"},
                   {"srsearch", owcs::liquipediaSearch("OWCS 2026")},
                   {"srnamespace", "0"},
                   {"srlimit", "50"}},
         {{"query", QJsonObject{{"search", QJsonArray{QJsonObject{{"title", title}}}}}}});
  QFile html(fixtures + "/champions-clash-2026.html");
  html.open(QIODevice::ReadOnly);
  cached(QUrlQuery{{"action", "parse"}, {"page", title}, {"prop", "text|revid"}},
         {{"parse", QJsonObject{{"title", title},
                                {"revid", 1143625},
                                {"text", QJsonObject{{"*", QString::fromUtf8(html.readAll())}}}}}});
  owcs::LiquipediaClient client(cache);
  QPointer<QDialog> dialog = owcs::createRosterDialog(file, &client, nullptr);
  dialog->setAttribute(Qt::WA_DontShowOnScreen);
  dialog->show();
  auto pump = [&] {
    app.processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  };
  auto button = [&](QString text) -> QPushButton * {
    for (auto b : dialog->findChildren<QPushButton *>())
      if (b->text() == text)
        return b;
    return nullptr;
  };
  auto wait = [&](const std::function<bool()> &done) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < 3000) {
      pump();
      QThread::msleep(10);
    }
    check(done(), "UI async request completed");
  };
  auto search = dialog->findChild<QLineEdit *>();
  search->setText("OWCS 2026");
  button(QStringLiteral("搜索赛事"))->click();
  auto combo = dialog->findChild<QComboBox *>();
  wait([&] { return combo->count() == 1; });
  check(combo->currentData().toString() == title, "event selection preserves exact title");
  button(QStringLiteral("导入选手"))->click();
  auto editor = dialog->findChild<QPlainTextEdit *>();
  wait([&] { return editor->toPlainText().split('\n').size() == 50; });
  editor->appendPlainText("ManualPlayer\nLEAVE");
  pump();
  if (app.arguments().size() > 2) {
    QDir().mkpath(app.arguments()[2]);
    dialog->grab().save(app.arguments()[2] + "/roster-dialog.png");
  }
  button(QStringLiteral("保存并应用"))->click();
  pump();
  check(owcs::Registry::instance().roster().size() == 51 &&
            owcs::Registry::instance().roster().contains("ManualPlayer"),
        "import and manual names saved in one pool with deduplication");
  owcs::Registry::instance().setRoster({});
  owcs::loadRoster(file);
  check(owcs::Registry::instance().roster().size() == 51, "saved pool restores after reload");
  QFile saved(file);
  saved.open(QIODevice::ReadOnly);
  auto data = QJsonDocument::fromJson(saved.readAll()).object();
  check(data["sources"].toArray().size() == 1 &&
            data["sources"].toArray()[0].toObject()["revision"] == 1143625,
        "source revision saved");
  dialog = owcs::createRosterDialog(file, &client, nullptr);
  dialog->setAttribute(Qt::WA_DontShowOnScreen);
  dialog->show();
  dialog->findChild<QPlainTextEdit *>()->setPlainText("CancelledEdit");
  button(QStringLiteral("取消"))->click();
  pump();
  check(owcs::Registry::instance().roster().size() == 51, "cancel does not change active pool");
  std::cout << checks << " UI checks; " << failures << " failures\n";
  return failures ? 1 : 0;
}
