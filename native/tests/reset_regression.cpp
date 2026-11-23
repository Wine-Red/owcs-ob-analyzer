#include "core/public_state.hpp"
#include "runtime/engine.hpp"
#include "state_view.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <iostream>
#include <opencv2/imgproc.hpp>
#include <thread>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  cv::setNumThreads(2);
  auto args = app.arguments();
  auto &registry = owcs::Registry::instance();
  int checks = 0;
  QJsonArray errors;
  auto check = [&](bool ok, const char *label) {
    ++checks;
    if (!ok)
      errors.append(label);
  };
  owcs::Recognizer reader(args[1]);
  auto image = owcs::readImage(args[2] + "/perks.png");
  auto observation = reader.detect(image);
  owcs::State state("clear-a");
  qint64 now = 0;
  auto observe = [&](const owcs::Observation &frame, int count) {
    for (int i = 0; i < count; ++i)
      state.update(frame, "fixture", ++now, 0);
  };
  auto identify = [&](const QString &name) {
    for (int i = 0; i < 3; ++i)
      state.name(1, {name, .99}, ++now);
  };
  observe(observation, 5);
  identify("Alpha");
  identify("Beta"); // Alpha's confirmed perks now exist only in the identity history.
  observe(observation, 5);
  auto seeded =
      owcs::localizedState(state.snapshot(observation, "fixture", now, 0), reader.localizedNames());
  registry.setRoster({"Alpha", "Beta"});
  registry.received("clear-a", "fixture", owcs::monotonicMs());
  registry.received("clear-b", "other", owcs::monotonicMs());
  check(registry.clear("clear-a", owcs::ClearScope::Perks) &&
            registry.state("clear-a")["analysis_status"] == "initializing",
        "clearing perks before recognition preserves waiting state");
  auto seededGeneration = registry.resetToken("clear-a").generation;
  registry.publish("clear-a", seeded, seededGeneration);
  registry.publish("clear-b", seeded, 0);
  auto original = owcs::publicState(registry.state("clear-a"));
  check(original["players"].toArray().size() == 2, "fixture has archived and active identities");
  auto expectedPlayers = original["players"].toArray();
  const QJsonObject noPerks{{"minor", QJsonValue::Null}, {"major", QJsonValue::Null}};
  for (auto row = expectedPlayers.begin(); row != expectedPlayers.end(); ++row) {
    auto player = row->toObject();
    auto perks = player["perks"].toObject();
    check(perks["minor"].toObject()["id"] == "Partial Scan" &&
              perks["major"].toObject()["id"] == "Focused Rush",
          "both tiers were confirmed for each identity before clear");
    player["perks"] = noPerks;
    QJsonArray stale;
    for (const auto &key : player["stale_fields"].toArray())
      if (!key.toString().startsWith("perks."))
        stale.append(key);
    player["stale_fields"] = stale;
    *row = player;
  }
  registry.clear("clear-a", owcs::ClearScope::Perks);
  registry.publish("clear-a", seeded, seededGeneration);
  auto cleared = owcs::publicState(registry.state("clear-a"));
  check(cleared["session_id"] == original["session_id"] && cleared["players"] == expectedPlayers,
        "perk clear takes effect immediately, preserving all other player fields and order");
  check(owcs::publicState(registry.state("clear-b"))["players"] == original["players"],
        "perk clear leaves the other source's selections intact");

  // Same hero after halftime: reconfirm empty slots and restore an archived player.
  auto emptySlots = observation;
  auto row = emptySlots.players[1].toObject();
  const QJsonObject unselected{
      {"selection", "unselected"}, {"id", QJsonValue::Null}, {"confidence", .99}};
  row["perks"] = QJsonObject{{"minor", unselected}, {"major", unselected}};
  emptySlots.players[1] = row;
  state.clearPerks();
  auto activePerks = [&] {
    return test::physicalPlayer(state.snapshot(emptySlots, "fixture", now, 0), 1)["perks"]
        .toObject();
  };
  observe(emptySlots, 3);
  check(activePerks()["minor"].toObject()["selection"] == "unselected" &&
            activePerks()["major"].toObject()["selection"] == "unselected",
        "same hero can confirm both empty slots after halftime");
  identify("Alpha");
  check(activePerks()["minor"].toObject()["selection"] == "unknown" &&
            activePerks()["major"].toObject()["selection"] == "unknown",
        "reconnecting an archived player cannot restore pre-click perk memory");
  observe(observation, 2);
  state.clearPerks();
  observe(observation, 1);
  check(activePerks()["minor"].toObject()["selection"] == "unknown",
        "pre-click partial confirmations cannot carry into the next round");
  observe(observation, 2);
  check(activePerks()["minor"].toObject()["id"] == "Partial Scan" &&
            activePerks()["major"].toObject()["id"] == "Focused Rush",
        "perks can be learned again after three post-click observations");
  const auto previous = registry.resetToken("clear-a");
  check(registry.clear("clear-a"), "clear existing source succeeds");
  auto empty = owcs::publicState(registry.state("clear-a"));
  check(empty["players"].toArray().isEmpty() && !empty.contains("players_by_id") &&
            !empty.contains("retained_players") && !empty.contains("player_order"),
        "clear immediately removes all current and historical data without another frame");
  check(!owcs::publicState(registry.state("clear-b"))["players"].toArray().isEmpty(),
        "clear cannot affect another source");
  check(registry.roster() == QStringList{"Alpha", "Beta"}, "clear preserves roster");
  check(!registry.clear("unknown-source"), "unknown source cannot clear selected source");
  registry.publish("clear-a", seeded, previous.generation);
  check(owcs::publicState(registry.state("clear-a"))["players"].toArray().isEmpty(),
        "pre-clear worker completion cannot republish discarded history");
  auto intermediate = registry.resetToken("clear-a");
  registry.clear("clear-a");
  registry.publish("clear-a", seeded, intermediate.generation);
  check(owcs::publicState(registry.state("clear-a"))["players"].toArray().isEmpty(),
        "repeated clicks reject every superseded generation");
  registry.remove("clear-a");
  registry.remove("clear-b");

  // Exercise the actual worker, including a queued frame from before Clear.
  {
    owcs::Engine engine("clear-worker", args[1]);
    owcs::Settings settings;
    settings.names = false;
    settings.autoTab = false;
    engine.configure(settings);
    cv::Mat bgra;
    cv::cvtColor(image, bgra, cv::COLOR_BGR2BGRA);
    auto wait = [&](auto predicate) {
      auto deadline = owcs::monotonicMs() + 10000;
      do {
        if (predicate())
          return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      } while (owcs::monotonicMs() < deadline);
      return false;
    };
    auto tick = [&] {
      auto sequence = registry.state("clear-worker")["sequence"].toDouble();
      engine.submit(bgra.clone(), "fixture", owcs::monotonicMs());
      return wait([&] { return registry.state("clear-worker")["sequence"].toDouble() > sequence; });
    };
    check(tick() && tick() && tick() && tick(), "worker recognizes frames before clear");
    auto old = registry.state("clear-worker");
    check(test::physicalPlayer(old, 0)["stats"].toObject()["damage"] == 4350,
          "worker has confirmed data to clear");
    registry.clear("clear-worker", owcs::ClearScope::Perks);
    engine.submit(bgra.clone(), "fixture", registry.resetToken("clear-worker").after);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(tick() && tick(), "worker resumes after perk clear");
    auto partial = registry.state("clear-worker");
    check(
        partial["session_id"] == old["session_id"] &&
            test::physicalPlayer(partial, 0)["stats"] == test::physicalPlayer(old, 0)["stats"] &&
            test::physicalPlayer(partial, 1)["perks"].toObject()["minor"].toObject()["selection"] ==
                "unknown",
        "queued pre-click capture cannot count toward post-clear perk confirmation");
    check(tick() && test::physicalPlayer(registry.state("clear-worker"), 1)["perks"]
                            .toObject()["minor"]
                            .toObject()["id"] == "Partial Scan",
          "actual worker confirms a newly observed perk");
    registry.clear("clear-worker");
    registry.clear("clear-worker", owcs::ClearScope::Perks);
    auto reset = registry.resetToken("clear-worker");
    engine.submit(bgra.clone(), "fixture", reset.after);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(owcs::publicState(registry.state("clear-worker"))["players"].toArray().isEmpty(),
          "queued pre-clear capture cannot seed the new session");
    check(tick(), "worker resumes without toggling recognition");
    auto first = registry.state("clear-worker");
    check(first["session_id"] != old["session_id"] &&
              test::physicalPlayer(first, 0)["stats"].toObject()["damage"].isNull(),
          "first new frame uses a new session without old confirmed counters");
    check(tick(), "worker continues confirming new observations");
    check(test::physicalPlayer(registry.state("clear-worker"), 0)["stats"].toObject()["damage"] ==
              4350,
          "newly confirmed data repopulates normally");
  }
  std::cout << QJsonDocument(
                   QJsonObject{{"checks", checks}, {"failures", errors.size()}, {"errors", errors}})
                   .toJson()
                   .toStdString();
  return errors.isEmpty() ? 0 : 1;
}
