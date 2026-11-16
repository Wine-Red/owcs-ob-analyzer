#include "core/player_rows.hpp"
#include "core/public_state.hpp"
#include "core/state.hpp"
#include "obs/player_table.hpp"
#include "runtime/registry.hpp"
#include "runtime/tab_refresh.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <iostream>

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  cv::setNumThreads(2);
  int checks = 0, failures = 0;
  auto check = [&](bool ok, const char *message) {
    ++checks;
    if (!ok) {
      ++failures;
      std::cerr << message << '\n';
    }
  };
  const auto args = app.arguments();
  if (args.size() < 3)
    return 2;

  // Virtual-clock scheduling only: never call SendInput or require a game.
  owcs::TabRefresh pulse;
  pulse.pressed(0);
  check(!pulse.due(60000), "refresh defaults off");
  pulse.configure(true, 10);
  check(!pulse.due(100), "enabling refresh starts a full interval");
  check(!pulse.due(10099) && pulse.due(10100), "10 second boundary");
  pulse.released(10100);
  check(pulse.waiting(10349) && !pulse.waiting(10350), "at least 250 ms key-up gap");
  pulse.pressed(10400);
  check(!pulse.due(20399) && pulse.due(20400), "interval restarts from successful press");
  pulse.configure(false, 10);
  check(!pulse.due(999999) && !pulse.waiting(999999), "disabling cancels pending refresh");
  pulse.configure(true, 60);
  pulse.pressed(0);
  check(!pulse.due(59999) && pulse.due(60000), "custom interval");
  pulse.configure(true, -10);
  pulse.pressed(0);
  check(!pulse.due(1999) && pulse.due(2000), "minimum interval clamped");
  pulse.configure(true, 9999);
  pulse.pressed(0);
  check(!pulse.due(299999) && pulse.due(300000), "maximum interval clamped");
  pulse.released(300000);
  pulse.reset();
  check(!pulse.waiting(300001) && !pulse.due(300001), "stop or lease loss discards schedule");

  owcs::Recognizer reader(args[1]);
  auto base = reader.detect(owcs::readImage(args[2] + "/perks.png"));
  auto input = base;
  owcs::State state("perk-memory");
  qint64 now = 0;
  auto tick = [&] {
    now += 200;
    return state.update(input, "fixture", now, 0);
  };
  auto rawPerk = [&](QString selection, QJsonValue id = QJsonValue::Null) {
    auto row = input.players[1].toObject();
    auto perks = row["perks"].toObject();
    perks["minor"] = QJsonObject{{"selection", selection}, {"id", id}, {"confidence", .99}};
    row["perks"] = perks;
    input.players[1] = row;
  };
  auto board = [](const QJsonObject &s, int row = 1) {
    return s["players"].toArray()[row].toObject()["scoreboard"].toObject();
  };
  auto perk = [&](const QJsonObject &s, int row = 1) {
    return board(s, row)["perks"].toObject()["minor"].toObject();
  };
  QJsonObject s;
  for (int i = 0; i < 5; ++i)
    s = tick();
  for (int i = 0; i < 3; ++i)
    state.name(1, {"Alpha", .99}, ++now);
  s = tick();
  check(perk(s)["id"] == "Partial Scan", "real screenshot establishes perk");
  const QJsonValue seen = board(s)["fields"].toObject()["minor"].toObject()["observed_at"];
  for (const auto selection : {"unselected", "not_shown", "unknown"}) {
    rawPerk(selection);
    s = tick();
    check(board(s)["fields"].toObject()["minor"].toObject()["status"] == "stale",
          "missing perk is historical immediately, before TTL expires");
    for (int i = 0; i < 8; ++i)
      s = tick();
    check(perk(s)["id"] == "Partial Scan", "missing icon cannot erase chosen perk");
    const auto meta = board(s)["fields"].toObject()["minor"].toObject();
    check(meta["status"] == "stale" && meta["observed_at"] == seen,
          "memory does not renew observation timestamp");
    const auto published = owcs::publicState(s)["players"].toArray();
    check(published.size() == 1 &&
              published[0].toObject()["stale_fields"].toArray().contains("perks.minor"),
          "public API marks remembered perk historical");
  }
  rawPerk("selected", "Partial Scan");
  s = tick();
  check(board(s)["fields"].toObject()["minor"].toObject()["status"] == "fresh",
        "visible same perk refreshes memory");
  auto hiddenMajor = input.players[1].toObject();
  auto tiers = hiddenMajor["perks"].toObject();
  tiers["major"] = QJsonObject{{"id", QJsonValue::Null}, {"selection", "not_shown"}};
  hiddenMajor["perks"] = tiers;
  input.players[1] = hiddenMajor;
  s = tick();
  check(board(s)["perks"].toObject()["major"].toObject()["id"] == "Focused Rush" &&
            board(s)["fields"].toObject()["major"].toObject()["status"] == "stale",
        "major perk memory is independent of a visible minor perk");
  rawPerk("selected", "Synthetic Alternative");
  s = tick();
  check(perk(s)["id"] == "Partial Scan", "one contradictory recognition rejected");
  rawPerk("unselected");
  tick();
  rawPerk("selected", "Synthetic Alternative");
  tick();
  s = tick();
  check(perk(s)["id"] == "Partial Scan", "missing frame breaks alternative confirmation");
  s = tick();
  check(perk(s)["id"] == "Synthetic Alternative", "confirmed different perk can replace memory");

  // Row movement must carry the player's memory, never the old row's data.
  std::swap(input.names[1], input.names[2]);
  const QJsonValue oldRow = input.players[1];
  input.players[1] = input.players[2];
  input.players[2] = oldRow;
  auto movedRow = input.players[2].toObject();
  auto movedPerks = movedRow["perks"].toObject();
  movedPerks["minor"] = QJsonObject{{"selection", "not_shown"}, {"id", QJsonValue::Null}};
  movedRow["perks"] = movedPerks;
  input.players[2] = movedRow;
  tick();
  for (int i = 0; i < 3; ++i)
    state.name(2, {"Alpha", .99}, ++now);
  s = tick();
  check(perk(s, 2)["id"] == "Synthetic Alternative",
        "memory follows confirmed identity across rows");
  for (int i = 0; i < 3; ++i)
    state.name(1, {"Beta", .99}, ++now);
  s = tick();
  check(perk(s, 1)["id"] != "Synthetic Alternative", "replacement player cannot inherit memory");
  auto changedHero = input.players[2].toObject();
  changedHero["hero"] = "tracer";
  changedHero["hero_selection"] = "selected";
  input.players[2] = changedHero;
  tick();
  s = tick();
  check(perk(s, 2)["id"].isNull(), "confirmed hero swap clears memory");
  state.reset();
  input = base;
  rawPerk("unselected");
  for (int i = 0; i < 6; ++i)
    s = tick();
  check(perk(s)["selection"] == "unselected" && perk(s)["id"].isNull(),
        "clear has no memory; never-selected slot remains unselected");
  input = base;
  for (int i = 0; i < 5; ++i)
    s = tick();
  auto unselected = input.players[1].toObject();
  unselected["hero"] = QJsonValue::Null;
  unselected["hero_selection"] = "unselected";
  input.players[1] = unselected;
  tick();
  s = tick();
  check(perk(s)["id"].isNull(), "unselecting hero discards perk memory");

  auto player = [](QString id, QString side, int slot, QString presence = "present") {
    return QJsonObject{{"player_id", "name:" + id},
                       {"display_name", id},
                       {"side", side},
                       {"slot", slot},
                       {"presence_status", presence},
                       {"hero", "tracer"},
                       {"scoreboard", QJsonObject{{"stats", QJsonObject{{"damage", slot * 123}}}}}};
  };
  QJsonArray rows{player("beta", "right", 1), player("alpha", "left", 1),
                  player("gone", "left", 2, "missing")};
  QJsonObject priorities{{"name:beta", 1}, {"name:gone", 1}};
  auto ordered = owcs::orderedPlayerRows(rows, priorities);
  check(ordered[0] == rows[0] && ordered[1] == rows[1] && ordered[2] == rows[2],
        "priority orders whole records, departures still last");
  check(owcs::orderedPlayerRows(rows)[0] == rows[1], "default preserves physical order");
  priorities["name:alpha"] = 1;
  check(owcs::orderedPlayerRows(rows, priorities)[0] == rows[1], "ties use physical order");
  check(owcs::normalizedPlayerPriorities({{"name:bad", -1},
                                          {"name:fraction", 1.5},
                                          {"name:string", "1"},
                                          {"name:default", 1000},
                                          {"", 1}})
            .isEmpty(),
        "invalid settings ignored");

  QTemporaryDir temp;
  auto &registry = owcs::Registry::instance();
  registry.setPlayerPriorities({});
  QJsonObject snapshot{{"type", "state.snapshot"},
                       {"participants", rows},
                       {"source_status", "online"},
                       {"scoreboard", QJsonObject{{"confirmed", true}}}};
  registry.publish("priority-test", snapshot, 0);
  owcs::PlayerTable widget(temp.path() + "/order.json");
  widget.setAttribute(Qt::WA_DontShowOnScreen);
  widget.resize(1400, 360);
  widget.show();
  widget.refresh(snapshot);
  auto table = widget.findChild<QTableWidget *>();
  auto spin = qobject_cast<QSpinBox *>(table->cellWidget(1, 0));
  check(spin && spin->objectName() == "priority:name:beta", "priority editor belongs to ID");
  spin->setValue(1);
  widget.activateWindow();
  spin->setFocus();
  app.processEvents();
  widget.refresh(snapshot);
  check(table->cellWidget(1, 0) == spin && spin->value() == 1,
        "periodic refresh cannot overwrite an active editor");
  auto liveRows = rows;
  auto livePlayer = liveRows[0].toObject();
  auto liveBoard = livePlayer["scoreboard"].toObject();
  liveBoard["stats"] = QJsonObject{{"damage", 4321}};
  livePlayer["scoreboard"] = liveBoard;
  liveRows[0] = livePlayer;
  auto liveSnapshot = snapshot;
  liveSnapshot["participants"] = liveRows;
  widget.refresh(liveSnapshot);
  check(table->cellWidget(1, 0) == spin && spin->value() == 1 &&
            table->item(1, 10)->text() == "4321",
        "live stats continue updating while priority editor is active");
  QMetaObject::invokeMethod(spin, "editingFinished", Qt::DirectConnection);
  app.processEvents();
  check(table->item(0, 2)->text() == "beta", "UI edit reorders immediately");
  auto api = owcs::publicState(registry.state("priority-test"));
  check(api["players"].toArray()[0].toObject()["player_id"] == "name:beta" &&
            !api.contains("player_priorities"),
        "API uses order without exposing internal config");
  QFile file(temp.path() + "/order.json");
  file.open(QIODevice::ReadOnly);
  check(QJsonDocument::fromJson(file.readAll()).object()["name:beta"] == 1,
        "priority is persisted by ID");
  file.close();
  registry.setPlayerPriorities({});
  owcs::PlayerTable reopened(temp.path() + "/order.json");
  reopened.refresh(snapshot);
  check(registry.playerPriorities()["name:beta"] == 1, "saved order loads after restart");
  registry.clear("priority-test");
  check(registry.playerPriorities()["name:beta"] == 1, "data clear preserves ordering preference");
  // Invalid storage is reported, and a failed write cannot silently change API order.
  QFile blocked(temp.path() + "/blocked");
  blocked.open(QIODevice::WriteOnly);
  blocked.close();
  owcs::PlayerTable unwritable(temp.path() + "/blocked/order.json");
  unwritable.refresh(snapshot);
  auto failedTable = unwritable.findChild<QTableWidget *>();
  auto failedSpin = qobject_cast<QSpinBox *>(failedTable->cellWidget(0, 0));
  failedSpin->setValue(2);
  QMetaObject::invokeMethod(failedSpin, "editingFinished", Qt::DirectConnection);
  app.processEvents();
  check(registry.playerPriorities()["name:beta"] == 1 &&
            !unwritable.findChild<QLabel *>("playerOrderMessage")->text().isEmpty(),
        "failed save leaves previous order and shows error");
  if (args.size() > 3) {
    QDir().mkpath(args[3]);
    widget.grab().save(args[3] + "/player-priority.png");
  }
  widget.findChild<QPushButton *>("resetPlayerOrder")->click();
  check(registry.playerPriorities().isEmpty() && table->item(0, 2)->text() == "alpha",
        "restore default resets UI and shared API order");
  registry.remove("priority-test");
  std::cout << checks << " feature checks; " << failures << " failures\n";
  return failures ? 1 : 0;
}
