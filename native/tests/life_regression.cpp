#include "life_regression.hpp"
#include "core/state.hpp"
#include <QFile>
#include <QJsonDocument>
#include <iostream>

std::pair<int, int> lifeRegression(owcs::Recognizer &r, const QString &directory) {
  int checks = 0, failures = 0;
  auto check = [&](bool ok, const QString &label) {
    ++checks;
    if (!ok) {
      ++failures;
      std::cerr << "life: " << label.toStdString() << "\n";
    }
  };
  QFile file(directory + "/expected.json");
  if (!file.open(QIODevice::ReadOnly))
    throw std::runtime_error("Missing real-frame life annotations");
  auto labels = QJsonDocument::fromJson(file.readAll()).object();
  QMap<QString, owcs::Observation> observations;
  for (auto it = labels.begin(); it != labels.end(); ++it) {
    auto obs = r.detect(owcs::readImage(directory + "/" + it.key() + ".png"));
    observations[it.key()] = obs;
    check(obs.confirmed && obs.players.size() == 10, it.key() + " panel");
    if (obs.players.size() != 10)
      continue;
    auto expected = it.value().toObject();
    for (int i = 0; i < 10; ++i) {
      auto raw = obs.players[i].toObject();
      QString label = it.key() + "/" + QString::number(i);
      check(raw["life_state"] == expected["life"].toArray()[i], label + " life");
      check(raw["hero"] == expected["heroes"].toArray()[i], label + " hero");
      auto timers = expected["respawn"].toObject();
      if (timers.contains(QString::number(i)))
        check(raw["respawn_seconds"] == timers[QString::number(i)], label + " timer");
    }
  }
  owcs::State state("real-frame-transitions");
  qint64 time = 0;
  auto update = [&](const QString &name, int repeats) {
    QJsonObject snapshot;
    for (int i = 0; i < repeats; ++i) {
      snapshot = state.update(observations[name], name, time, 0);
      time += 200;
    }
    return snapshot;
  };
  auto player = [](const QJsonObject &snapshot, int row) {
    return snapshot["players"].toArray()[row].toObject();
  };
  // User's OBS screenshot: Mauga's clean portrait has circular costume/face
  // edges. Check raw classification separately from temporal publication.
  auto mauga = r.detect(owcs::readImage(directory + "/mauga-obs.png"));
  check(mauga.confirmed && mauga.players.size() == 10, "Mauga OBS panel located");
  if (mauga.players.size() == 10) {
    check(mauga.players[4].toObject()["hero"] == "mauga" &&
              mauga.players[4].toObject()["life_state"] == "alive",
          "Mauga clean portrait raw life is alive");
    check(mauga.players[7].toObject()["life_state"] == "dead",
          "real countdown beside Mauga remains dead");
    check(mauga.players[5].toObject()["life_state"] == "dead" &&
              mauga.players[5].toObject()["hero"].isNull(),
          "diagonally shifted skull cross in OBS screenshot is dead without inventing hero");
    owcs::State local("mauga-life");
    local.update(mauga, "test", 0, 0);
    auto alive = local.update(mauga, "test", 200, 0);
    check(player(alive, 4)["life_state"] == "alive" && player(alive, 4)["hero"] == "mauga",
          "Mauga raw alive is published within two observations");
    auto overlay = mauga;
    overlay.players[4] = mauga.players[7]; // Fault injection, not a captured Mauga death.
    local.update(overlay, "test", 400, 0);
    auto deadMauga = local.update(overlay, "test", 600, 0);
    check(player(deadMauga, 4)["life_state"] == "dead" && player(deadMauga, 4)["hero"] == "mauga",
          "Mauga history does not suppress a real countdown observation");
    local.update(mauga, "test", 800, 0);
    auto revived = local.update(mauga, "test", 1000, 0);
    check(player(revived, 4)["life_state"] == "alive" &&
              player(revived, 4)["scoreboard"].toObject()["respawn_seconds"].isNull(),
          "Mauga clear portrait restores alive and clears timer");
  }
  auto initial = update("before-swap", 3);
  check(player(initial, 9)["hero"] == "vendetta", "initial hero confirmed");
  auto pending = update("white-portrait", 1);
  check(player(pending, 9)["hero"].isNull(), "replacement never presents old hero as current");
  auto white = update("white-portrait", 1);
  check(player(white, 9)["hero"] == "shion" && player(white, 9)["life_state"] == "alive",
        "white portrait confirms within two observations and is not dead");
  auto crossed = update("death-cross", 2);
  auto dead = player(crossed, 5);
  check(dead["hero"] == "dmon" && dead["life_state"] == "dead",
        "cross establishes death while retaining confirmed hero");
  check(dead["scoreboard"].toObject()["respawn_seconds"].isNull(),
        "cross is never published as a countdown digit");
  auto expanded = update("expanded-cross", 2);
  check(player(expanded, 6)["life_state"] == "alive",
        "adjacent animation occlusion does not kill its neighbor");
  auto neighbor = player(expanded, 6)["scoreboard"].toObject()["fields"].toObject();
  check(neighbor["life"].toObject()["age_ms"].toInt() >= 400,
        "occluded neighbor retains history without refreshing its observation");
  auto countdown = update("countdown", 2);
  check(player(countdown, 5)["hero"] == "dmon" && player(countdown, 5)["life_state"] == "dead",
        "countdown cannot replace the retained hero");
  auto recovered = update("cassidy-swap", 2);
  check(player(recovered, 9)["hero"] == "cassidy" && player(recovered, 9)["life_state"] == "alive",
        "Cassidy switch confirms within two observations");
  check(player(recovered, 5)["life_state"] == "alive" &&
            player(recovered, 5)["scoreboard"].toObject()["respawn_seconds"].isNull(),
        "respawn returns to alive and clears timer");
  owcs::State cold("cold-start-death");
  QJsonObject coldSnapshot;
  for (int i = 0; i < 3; ++i)
    coldSnapshot = cold.update(observations["death-cross"], "test", i * 200, 0);
  check(player(coldSnapshot, 5)["hero"].isNull() && player(coldSnapshot, 5)["life_state"] == "dead",
        "starting during death never invents an identity");

  // Fault injection on a real detected row isolates field-update behavior from
  // image classification. A bad field must not starve unrelated observations.
  owcs::State independent("independent-fields");
  auto sequence = observations["white-portrait"];
  auto row = sequence.players[9].toObject();
  auto submit = [&](qint64 now) {
    sequence.players[9] = row;
    return player(independent.update(sequence, "test", now, 0), 9);
  };
  auto board = [](const QJsonObject &p) { return p["scoreboard"].toObject(); };
  auto status = [&](const QJsonObject &p, const QString &field) -> QJsonValue {
    return board(p)["fields"].toObject()[field].toObject()["status"];
  };
  submit(0);
  auto settled = submit(200);
  check(settled["hero"] == "shion", "independent sequence starts with confirmed hero");

  row["hero"] = "cassidy";
  row["life_state"] = "unknown";
  row["ultimate_charge"] = 51;
  row["ultimate_ready"] = false;
  auto first = submit(400);
  auto changed = submit(600);
  check(first["hero"].isNull() && changed["hero"] == "cassidy",
        "unknown life cannot block a two-observation hero swap");
  check(board(changed)["ultimate_charge"] == 51 && status(changed, "ultimate_charge") == "fresh",
        "hero confirmation cannot reset independently observed charge");

  row["hero"] = "tracer";
  row["life_state"] = "dead"; // Faulty life result, no actual portrait overlay.
  row["portrait_occluded"] = false;
  submit(800);
  auto conflicting = submit(1000);
  check(conflicting["hero"] == "tracer" && conflicting["life_state"] == "dead",
        "life classification does not veto a valid unobscured hero observation");

  row["hero"] = QJsonValue::Null;
  row["hero_selection"] = "unknown";
  row["life_state"] = "alive";
  row["respawn_seconds"] = QJsonValue::Null;
  submit(1200);
  auto revived = submit(1400);
  check(revived["life_state"] == "alive" && board(revived)["respawn_seconds"].isNull(),
        "revival does not require a readable hero");

  // Keep the hero unresolved for seconds, including an ultimate ready -> spent
  // transition, while life and counters continue changing on their own clocks.
  for (int i = 0; i < 12; ++i) {
    row["hero"] = i % 2 ? "hanzo" : "cassidy";
    row["hero_selection"] = "selected";
    row["life_state"] = i < 6 ? "dead" : "alive";
    row["respawn_seconds"] = i < 6 ? QJsonValue(8 - i / 2) : QJsonValue(QJsonValue::Null);
    row["ultimate_charge"] = i < 6 ? 100 : i - 6;
    row["ultimate_ready"] = i < 6;
    auto stats = row["stats"].toObject();
    stats["damage"] = QJsonValue::Null;
    stats["healing"] = 100 + i;
    row["stats"] = stats;
    auto perks = row["perks"].toObject();
    perks["minor"] =
        QJsonObject{{"id", QJsonValue::Null}, {"selection", "unselected"}, {"confidence", .95}};
    row["perks"] = perks;
    auto result = submit(1600 + i * 200);
    check(result["hero"].isNull(), "alternating hero remains unconfirmed");
    if (i == 0 || i == 6)
      continue; // Each independent transition gets its own second observation.
    check(result["life_state"] == row["life_state"] && status(result, "life") == "fresh",
          "life transitions despite unresolved hero");
    check(board(result)["ultimate_charge"] == (i < 6 ? 100 : i - 7) &&
              board(result)["ultimate_ready"] == row["ultimate_ready"],
          "ready and spent charge update despite unresolved hero");
    check(board(result)["stats"].toObject()["healing"] == 99 + i &&
              status(result, "healing") == "fresh",
          "missing damage cannot freeze another statistic");
    check(board(result)["respawn_seconds"] == row["respawn_seconds"],
          "countdown and recovery independent of hero confirmation");
    if (i >= 2)
      check(board(result)["perks"].toObject()["minor"].toObject()["selection"] == "unselected",
            "empty perk slot does not require hero identity");
    if (i >= 6)
      check(status(result, "damage") == "stale", "unreadable damage keeps its own age");
  }
  row["hero"] = "cassidy";
  submit(4200);
  auto resolved = submit(4400);
  check(resolved["hero"] == "cassidy" && resolved["life_state"] == "alive",
        "hero recovers without any ultimate change");
  return {checks, failures};
}
