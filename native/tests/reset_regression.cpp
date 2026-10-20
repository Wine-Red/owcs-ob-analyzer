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
  state.update(observation, "fixture", 0, 0);
  state.update(observation, "fixture", 200, 0);
  for (int n = 0; n < 3; ++n)
    state.name(0, {"Alpha", .99}, 210 + n);
  auto seeded = state.snapshot(observation, "fixture", 220, 0);
  registry.setRoster({"Alpha", "Beta"});
  registry.received("clear-a", "fixture", owcs::monotonicMs());
  registry.received("clear-b", "other", owcs::monotonicMs());
  registry.publish("clear-a", seeded, 0);
  registry.publish("clear-b", seeded, 0);
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
    check(tick() && tick(), "worker recognizes frames before clear");
    auto old = registry.state("clear-worker");
    check(test::physicalPlayer(old, 0)["stats"].toObject()["damage"] == 4350,
          "worker has confirmed data to clear");
    registry.clear("clear-worker");
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
