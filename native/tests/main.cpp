#include "core/names.hpp"
#include "core/public_state.hpp"
#include "core/recognizer.hpp"
#include "core/state.hpp"
#include "runtime/registry.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <chrono>
#include <iostream>
#include <opencv2/imgproc.hpp>
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  cv::setNumThreads(2); // Match the production worker's threading budget.
  try {
    auto args = app.arguments();
    if (args.size() < 3)
      return 2;
    owcs::Recognizer r(args[1]);
    auto path = args[2];
    if (args.contains("--sequence")) {
      owcs::State state("sequence");
      qint64 time = 0;
      for (const auto &file : QDir(path).entryList({"frame-*.png"}, QDir::Files, QDir::Name)) {
        auto observed = r.detect(owcs::readImage(path + "/" + file));
        auto snapshot = state.update(observed, file, time, 0);
        std::cout << QJsonDocument(QJsonObject{{"file", file},
                                               {"raw", observed.players},
                                               {"state", snapshot}})
                         .toJson(QJsonDocument::Compact)
                         .toStdString()
                  << "\n";
        time += 500;
      }
      return 0;
    }
    if (path.endsWith(".png")) {
      auto image = owcs::readImage(path);
      owcs::Observation obs;
      owcs::State state("test");
      QJsonObject snapshot;
      for (int i = 0; i < 6; i++) {
        obs = r.detect(image);
        snapshot = state.update(obs, path, i * 200, 0);
      }
      if (args.contains("--names")) {
        owcs::NameReader names(args[1]);
        for (int i = 0; i < 10; i++) {
          auto name = names.read(obs.names[i]);
          for (int n = 0; n < 5; n++)
            state.name(i, name, 1000);
        }
        snapshot = state.update(obs, path, 1000, 0);
      }
      std::cout << QJsonDocument(snapshot).toJson().toStdString();
      return 0;
    }
    QFile f(path + "/expected.json");
    f.open(QIODevice::ReadOnly);
    auto expected = QJsonDocument::fromJson(f.readAll()).object();
    int failures = 0, checks = 0;
    auto check = [&](bool pass, const std::string &label) {
      checks++;
      if (!pass) {
        failures++;
        std::cerr << label << "\n";
      }
    };
    for (auto key : {"deaths", "ready", "perks", "stability-single-perk", "stability-obs-scaled",
                     "stability-connected-digits"}) {
      auto obs = r.detect(owcs::readImage(path + "/" + key + ".png"));
      auto e = expected[key].toObject();
      if (!obs.visible || obs.players.size() != 10) {
        std::cerr << key << " panel missing\n";
        failures++;
        continue;
      }
      auto charge = e["charge"].toArray(), stats = e["stats"].toArray();
      for (int i = 0; i < 10; i++) {
        auto p = obs.players[i].toObject();
        check(p["hero"] == e["heroes"].toArray()[i],
              std::string(key) + " hero " + std::to_string(i));
        check(p["life_state"] == e["life"].toArray()[i],
              std::string(key) + " life " + std::to_string(i));
        check(p["ultimate_ready"].toBool() == (charge[i].toInt() == 100),
              std::string(key) + " ready");
        checks++;
        if (p["ultimate_charge"] != charge[i]) {
          failures++;
          std::cerr << key << " row " << i << " ult " << p["ultimate_charge"].toInt(-1)
                    << " expected " << charge[i].toInt() << "\n";
        }
        int j = 0;
        for (auto stat : {"eliminations", "assists", "deaths", "damage", "healing", "mitigated"}) {
          checks++;
          if (p["stats"].toObject()[stat] != stats[i].toArray()[j]) {
            failures++;
            std::cerr << key << " row " << i << " " << stat << " mismatch\n";
          }
          j++;
        }
      }
    }
    auto frame = owcs::readImage(path + "/perks.png");
    auto unselectedFrame = owcs::readImage(path + "/unselected-panel.png");
    auto unselected = r.detect(unselectedFrame);
    check(unselected.confirmed && unselected.players.size() == 10,
          "unselected panel confirmed by layout and counters");
    owcs::State selectionState("selection");
    QJsonObject selectionSnapshot;
    for (int i = 0; i < 6; ++i)
      selectionSnapshot = selectionState.update(unselected, "test", i * 200, 0);
    for (auto row : selectionSnapshot["players"].toArray()) {
      auto player = row.toObject();
      check(player["hero_selection"] == "unselected" && player["hero"].isNull(),
            "question portrait is explicit unselected");
      check(player["life_state"] == "unknown" &&
                player["scoreboard"].toObject()["ultimate_charge"].isNull(),
            "unselected is not alive, dead or charged");
      check(!player["scoreboard"].toObject()["stats"].toObject()["damage"].isNull(),
            "unselected keeps visible statistics");
    }
    const int emptyStats[10][6] = {{2, 1, 6, 5514, 13, 587},   {6, 5, 2, 3322, 6303, 0},
                                   {5, 4, 2, 3497, 8502, 829}, {7, 0, 6, 7064, 0, 0},
                                   {7, 2, 2, 9534, 834, 5448}, {7, 12, 1, 3375, 8193, 1211},
                                   {11, 11, 1, 2708, 7259, 0}, {7, 0, 6, 4652, 1, 58},
                                   {12, 0, 4, 8267, 469, 182}, {6, 1, 2, 8126, 692, 7300}};
    for (int row = 0; row < unselected.players.size(); ++row) {
      int col = 0;
      for (auto key : {"eliminations", "assists", "deaths", "damage", "healing", "mitigated"})
        check(unselected.players[row].toObject()["stats"].toObject()[key] == emptyStats[row][col++],
              "unselected panel numeric row " + std::to_string(row));
    }
    auto selectedAgain = r.detect(frame);
    selectedAgain.names = unselected.names;
    for (int i = 0; i < 6; ++i)
      selectionSnapshot = selectionState.update(selectedAgain, "test", 1200 + i * 200, 0);
    check(selectionSnapshot["players"].toArray()[0].toObject()["hero_selection"] == "selected" &&
              !selectionSnapshot["players"].toArray()[0].toObject()["hero"].isNull(),
          "unselected to selected transition");
    for (int i = 0; i < 6; ++i)
      selectionSnapshot = selectionState.update(unselected, "test", 2400 + i * 200, 0);
    auto cleared = selectionSnapshot["players"].toArray()[0].toObject();
    check(
        cleared["hero_selection"] == "unselected" && cleared["hero"].isNull() &&
            cleared["scoreboard"].toObject()["perks"].toObject()["minor"].toObject()["id"].isNull(),
        "selected to unselected clears historical hero and perks");
    auto emptyHud = owcs::readImage(path + "/unselected-hud.png");
    cv::resize(emptyHud, emptyHud, {1600, 1000});
    for (int shift : {0, 150, 450}) {
      for (int recolor : {0, 1}) {
        auto image = emptyHud.clone();
        if (recolor) {
          cv::Mat hsv;
          cv::cvtColor(image, hsv, cv::COLOR_BGR2HSV);
          for (int y = 0; y < hsv.rows; ++y)
            for (int x = 0; x < hsv.cols; ++x)
              hsv.at<cv::Vec3b>(y, x)[0] = (hsv.at<cv::Vec3b>(y, x)[0] + 50) % 180;
          cv::cvtColor(hsv, image, cv::COLOR_HSV2BGR);
        }
        cv::Mat translated;
        cv::warpAffine(image, translated, cv::Mat(cv::Matx23d(1, 0, 0, 0, 1, shift)), image.size());
        owcs::Recognizer locator(args[1]);
        owcs::Observation found;
        for (int i = 0; i < 30 && found.hudOffset == -10000; ++i)
          found = locator.detect(translated);
        check(found.hudOffset != -10000 && std::abs(found.hudOffset - shift) <= 1,
              "unselected HUD shift " + std::to_string(shift) + " recolor " +
                  std::to_string(recolor) + " got " + std::to_string(found.hudOffset));
      }
    }
    auto mixedHud = emptyHud.clone();
    auto knownHud = owcs::readImage(path + "/hud.png");
    cv::resize(knownHud, knownHud, mixedHud.size());
    knownHud.colRange(800, 1600).copyTo(mixedHud.colRange(800, 1600));
    owcs::Recognizer mixedLocator(args[1]);
    owcs::Observation mixedResult;
    for (int i = 0; i < 15 && mixedResult.hudOffset == -10000; ++i)
      mixedResult = mixedLocator.detect(mixedHud);
    check(mixedResult.hudOffset != -10000, "mixed selected and unselected HUD");
    auto noCounters = unselectedFrame.clone();
    noCounters.colRange(1280, 2080).setTo(0);
    check(!r.detect(noCounters).confirmed,
          "placeholder portraits alone cannot confirm a scoreboard");
    emptyHud.colRange(800, 1600).setTo(0);
    owcs::Recognizer emptyOneTeam(args[1]);
    for (int i = 0; i < 10; ++i)
      check(emptyOneTeam.detect(emptyHud).hudOffset == -10000,
            "single placeholder team cannot authorize Tab");
    auto antialias = owcs::readImage(path + "/antialias-digits.png");
    std::array<int, 14> antialiasExpected{8985, 6456, 6456, 9282, 8060, 8281, 8483,
                                          8483, 9682, 8680, 8680, 8687, 6632, 6654};
    for (int i = 0; i < 14; ++i) {
      auto number = r.number(antialias.rowRange(i * 23, (i + 1) * 23));
      check(number.value == antialiasExpected[i], "antialias number " + std::to_string(i));
    }
    auto stablePerk = r.detect(owcs::readImage(path + "/stability-single-perk.png"));
    check(stablePerk.players[6].toObject()["perks"].toObject()["minor"].toObject()["id"] ==
              "Ulterior Motive",
          "canonical hero ID and current official perk glyph");
    auto touching = r.detect(owcs::readImage(path + "/header-touching-portrait.png"));
    check(touching.visible && touching.players.size() == 10, "light portrait touching header");
    check(touching.bounds[0].toInt() == 459 && touching.bounds[1].toInt() == 181,
          "touching portrait does not shift header bounds");
    check(touching.players.size() == 10 &&
              touching.players[0].toObject()["ultimate_charge"] == 24 &&
              touching.players[0].toObject()["stats"].toObject()["damage"] == 480,
          "touching header preserves numeric crop alignment");
    auto obs = r.detect(frame);
    auto perk = [&](int row, const char *tier, const char *key) {
      return obs.players[row].toObject()["perks"].toObject()[tier].toObject()[key];
    };
    check(perk(1, "major", "id") == "Focused Rush", "major perk");
    check(perk(1, "minor", "id") == "Partial Scan", "minor perk");
    check(perk(6, "major", "selection") == "unselected", "empty major perk");
    check(perk(6, "minor", "id") == "X Machina", "selected minor perk");
    check(perk(9, "major", "id") == "Cybersecurity", "Sombra major");
    check(perk(9, "minor", "id") == "Life Hack", "Sombra minor");
    auto deaths = r.detect(owcs::readImage(path + "/deaths.png"));
    std::array<int, 5> seconds{2, 2, 8, 2, 2};
    for (int i = 0; i < 5; i++)
      check(deaths.players[i].toObject()["respawn_seconds"].toInt() == seconds[i], "respawn timer");
    cv::Mat moved;
    cv::warpAffine(frame, moved, cv::Mat(cv::Matx23d(1, 0, 24, 0, 1, 33)), frame.size());
    auto shifted = r.detect(moved);
    check(shifted.visible && shifted.bounds[0].toInt() - obs.bounds[0].toInt() == 24 &&
              shifted.bounds[1].toInt() - obs.bounds[1].toInt() == 33,
          "panel follows translation");
    for (int width : {1280, 1920, 2560}) {
      cv::Mat resized;
      cv::resize(frame, resized, {width, width * 1000 / 1600});
      auto result = r.detect(resized);
      check(result.visible, "resized panel visible");
      int known = 0;
      for (int i = 0; i < result.players.size(); i++) {
        auto value = result.players[i].toObject()["ultimate_charge"];
        known += !value.isNull();
        check(value.isNull() || value == expected["perks"].toObject()["charge"].toArray()[i],
              "resized ult never invented");
      }
      check(known >= 9, "resized ult coverage");
    }
    auto hud = owcs::readImage(path + "/hud.png");
    for (int dy : {0, 150, 450}) {
      owcs::Recognizer locator(args[1]);
      cv::warpAffine(hud, moved, cv::Mat(cv::Matx23d(1, 0, 0, 0, 1, dy)), hud.size());
      owcs::Observation found;
      for (int i = 0; i < 15 && found.hudOffset == -10000; i++)
        found = locator.detect(moved);
      check(!found.visible && std::abs(found.hudOffset - dy) <= 1,
            "HUD localization offset " + std::to_string(dy) + " got " +
                std::to_string(found.hudOffset));
    }
    auto liveHud = owcs::readImage(path + "/live-new-heroes-hud.png");
    for (int style : {0, 1}) {
      cv::Mat alternative;
      if (style == 0) {
        cv::cvtColor(liveHud, alternative, cv::COLOR_BGR2HSV);
        for (int y = 0; y < alternative.rows; ++y)
          for (int x = 0; x < alternative.cols; ++x)
            alternative.at<cv::Vec3b>(y, x)[0] = (alternative.at<cv::Vec3b>(y, x)[0] + 50) % 180;
        cv::cvtColor(alternative, alternative, cv::COLOR_HSV2BGR);
      } else {
        cv::cvtColor(liveHud, alternative, cv::COLOR_BGR2GRAY);
        cv::cvtColor(alternative, alternative, cv::COLOR_GRAY2BGR);
      }
      owcs::Recognizer colorLocator(args[1]);
      owcs::Observation found;
      for (int i = 0; i < 30 && found.hudOffset == -10000; ++i)
        found = colorLocator.detect(alternative);
      check(found.hudOffset != -10000,
            "HUD fallback independent of blue/red palette " + std::to_string(style));
    }
    owcs::Recognizer liveLocator(args[1]);
    owcs::Observation liveResult;
    for (int i = 0; i < 15 && liveResult.hudOffset == -10000; ++i)
      liveResult = liveLocator.detect(liveHud);
    check(liveResult.hudOffset != -10000 && std::abs(liveResult.hudOffset) <= 1,
          "live HUD with deaths and newer heroes");
    auto oneTeam = liveHud.clone();
    oneTeam.colRange(oneTeam.cols / 2, oneTeam.cols).setTo(0);
    owcs::Recognizer oneTeamLocator(args[1]);
    for (int i = 0; i < 8; ++i)
      check(oneTeamLocator.detect(oneTeam).hudOffset == -10000,
            "one team alone cannot authorize automatic Tab");
    auto blank = r.detect(cv::Mat::zeros(1000, 1600, CV_8UC3));
    check(!blank.visible && blank.hudOffset == -10000, "black frame rejected");
    auto distractor = hud.clone();
    distractor.rowRange(0, 180).setTo(0);
    owcs::Recognizer searcher(args[1]);
    double slowest = 0;
    for (int i = 0; i < 12; ++i) {
      auto start = std::chrono::steady_clock::now();
      auto result = searcher.detect(distractor);
      auto elapsed =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
              .count();
      slowest = std::max(slowest, elapsed);
      check(!result.visible && result.hudOffset == -10000, "gameplay without HUD rejected");
    }
    check(slowest < 1000, "HUD search must not block the worker for seconds");
    std::cout << "HUD search slowest frame: " << slowest << " ms\n";
    owcs::State state("test");
    QJsonObject snapshot;
    for (int i = 0; i < 6; i++)
      snapshot = state.update(obs, "test", i * 200, 0);
    auto board = [](QJsonObject s, int row = 0) {
      return s["players"].toArray()[row].toObject()["scoreboard"].toObject();
    };
    check(snapshot["scoreboard"].toObject()["confirmed"].toBool(), "temporal panel confirmation");
    check(board(snapshot)["stats"].toObject()["damage"] == 4350, "confirmed statistics");
    auto stale = owcs::State::age(snapshot, 3000);
    check(stale["source_status"] == "stale", "source expiry");
    check(board(stale)["fields"].toObject()["damage"].toObject()["status"] == "stale",
          "field expiry");
    check(board(stale)["stats"].toObject()["damage"] == 4350, "stale preserves last known");
    auto &registry = owcs::Registry::instance();
    auto now = owcs::monotonicMs();
    registry.received("heartbeat-test", "test", now);
    check(registry.state("heartbeat-test")["analysis_status"] == "initializing",
          "first frame reported before analysis finishes");
    stale["source_status"] = "online";
    stale["source"] = QJsonObject{{"captured_at", double(now - 6000)}, {"cycle_latency_ms", 6000}};
    registry.publish("heartbeat-test", stale);
    auto delayed = registry.state("heartbeat-test");
    check(delayed["source_status"] == "online" && delayed["source_age_ms"].toDouble() < 1000,
          "fresh input remains online during slow analysis");
    check(delayed["analysis_status"] == "delayed" && delayed["analysis_age_ms"].toDouble() >= 6000,
          "slow analysis reported separately");
    check(board(delayed)["fields"].toObject()["damage"].toObject()["status"] == "stale" &&
              board(delayed)["stats"].toObject()["damage"] == 4350,
          "input heartbeat never refreshes old observations");
    registry.received("heartbeat-test", "test", now - 6000);
    check(registry.state("heartbeat-test")["source_status"] == "stale",
          "missing input still expires");
    registry.remove("heartbeat-test");
    auto hidden = state.update({}, "test", 1200, 0);
    check(board(hidden)["fields"].toObject()["damage"].toObject()["status"] == "stale",
          "hidden invalidates fields");
    auto reset = state.update(obs, "test", 10000, 0);
    check(board(reset)["identity_generation"] == 0 &&
              board(reset)["stats"].toObject()["damage"] == 4350,
          "reopening the same panel preserves confirmed identity");
    auto changed = obs;
    changed.names[0] = obs.names[6];
    changed.players[0] = obs.players[6];
    auto uncertain = state.update(changed, "test", 10200, 0);
    check(board(uncertain)["identity_generation"] == 0 &&
              board(uncertain)["stats"].toObject()["damage"] == 4350 &&
              board(uncertain)["fields"].toObject()["damage"].toObject()["status"] == "stale",
          "one bad name frame retains but invalidates old row");
    auto recovered = state.update(obs, "test", 10400, 0);
    check(board(recovered)["identity_generation"] == 0 &&
              board(recovered)["fields"].toObject()["damage"].toObject()["status"] == "fresh",
          "one-frame name glitch does not erase the row");
    for (int i = 0; i < 3; ++i)
      reset = state.update(changed, "test", 10600 + i * 200, 0);
    check(board(reset)["identity_generation"] == 1 &&
              board(reset)["stats"].toObject()["damage"].isNull(),
          "consistent different player resets identity and counters");
    owcs::Field field;
    owcs::State heroSwap("hero-swap");
    auto swapped = stablePerk;
    auto replacement = swapped.players[0].toObject();
    replacement["hero"] = "hanzo";
    replacement["ultimate_charge"] = 80;
    swapped.players[0] = replacement;
    for (int i = 0; i < 6; ++i)
      heroSwap.update(swapped, "test", i * 200, 0);
    replacement["hero"] = "tracer";
    replacement["ultimate_charge"] = 3;
    swapped.players[0] = replacement;
    auto pendingSwap = heroSwap.update(swapped, "test", 1200, 0);
    check(pendingSwap["players"].toArray()[0].toObject()["hero"].isNull() &&
              board(pendingSwap)["ultimate_charge"].isNull(),
          "replacement portrait never publishes old hero with new charge");
    check(!board(pendingSwap)["stats"].toObject()["damage"].isNull(),
          "hero change preserves independent counters");
    replacement["hero"] = QJsonValue::Null;
    swapped.players[0] = replacement;
    auto missedSwap = heroSwap.update(swapped, "test", 1400, 0);
    check(missedSwap["players"].toArray()[0].toObject()["hero"].isNull(),
          "unreadable replacement frame does not restore historical hero");
    replacement["hero"] = "tracer";
    swapped.players[0] = replacement;
    for (int i = 0; i < 3; ++i)
      pendingSwap = heroSwap.update(swapped, "test", 1600 + i * 200, 0);
    check(pendingSwap["players"].toArray()[0].toObject()["hero"] == "tracer" &&
              board(pendingSwap)["ultimate_charge"] == 3,
          "replacement hero and its charge confirm after intermittent miss");
    // Public projection must preserve business values without leaking diagnostics.
    auto compact = owcs::publicState(snapshot);
    auto compactRow = compact["players"].toArray()[0].toObject();
    check(compact["schema_version"] == "3.0" && compact["panel_visible"].toBool(),
          "compact confirmed panel");
    check(compactRow["stats"] == board(snapshot)["stats"], "compact preserves counters");
    check(!compact.contains("source") && !compact.contains("hud") &&
              !compact.contains("recognition") && !compactRow.contains("scoreboard") &&
              !compactRow.contains("hero_confidence"),
          "compact excludes diagnostics");
    check(compactRow["unknown_fields"].toArray().contains("display_name"),
          "unread name explicitly unknown");
    check(!compactRow["stale_fields"].toArray().contains("stats.damage"),
          "current counter not stale");
    auto publicStale = owcs::publicState(owcs::State::age(snapshot, 3000));
    check(publicStale["status"] == "stale" && !publicStale["panel_visible"].toBool(),
          "offline snapshot never appears current");
    check(publicStale["players"].toArray()[0].toObject()["stale_fields"].toArray().contains(
              "stats.damage"),
          "stale counter path preserved");
    auto publicHidden = owcs::publicState(hidden);
    check(!publicHidden["panel_visible"].toBool() &&
              publicHidden["players"].toArray()[0].toObject()["stats"] == board(hidden)["stats"],
          "hidden retains historical values");
    auto delayedSnapshot = snapshot;
    delayedSnapshot["analysis_status"] = "delayed";
    auto publicDelayed = owcs::publicState(delayedSnapshot);
    check(publicDelayed["status"] == "delayed" &&
              publicDelayed["players"].toArray()[0].toObject()["stale_fields"].toArray().contains(
                  "hero"),
          "delayed analysis invalidates fresh metadata");
    auto unavailable = owcs::publicState(
        QJsonObject{{"source_status", "waiting"}, {"analysis_status", "initializing"}});
    check(unavailable["players"].toArray().isEmpty() && unavailable["status"] == "waiting",
          "compact initializing response");
    auto publicPending = owcs::publicState(missedSwap)["players"].toArray()[0].toObject();
    check(publicPending["hero"].isNull() &&
              publicPending["unknown_fields"].toArray().contains("hero"),
          "pending swap is unknown not historical hero");
    check(QJsonDocument(compact).toJson(QJsonDocument::Compact).size() <
              QJsonDocument(snapshot).toJson(QJsonDocument::Compact).size() / 2,
          "compact response less than half original size");
    owcs::Field intermittent;
    intermittent.observe(1000, .98, 0, 2, 500);
    intermittent.observe(QJsonValue::Null, 0, 200, 2, 500);
    intermittent.observe(1050, .98, 400, 2, 500);
    check(intermittent.value == 1050, "one unreadable frame does not starve changing counters");
    intermittent.observe(QJsonValue::Null, 0, 2000, 2, 500);
    intermittent.observe(1500, .98, 2200, 2, 500);
    check(intermittent.value == 1050 && intermittent.seen == 400,
          "expired candidates and missing observations never refresh values");
    field.observe(80, .99, 0, 2, 20);
    field.observe(81, .99, 200, 2, 20);
    check(field.value == 81, "continuously charging ult");
    field.observe(8, .99, 400, 2, 20);
    check(field.value == 81, "reject one erroneous drop");
    field.observe(0, .99, 600, 2, 20);
    field.observe(1, .99, 800, 2, 20);
    check(field.value == 1, "accept confirmed ult reset");
    owcs::NameReader names(args[1]);
    check(names.read(obs.names[6]).id == "WINERED", "Latin OCR 1");
    check(names.read(obs.names[8]).id == "HELLCAT", "Latin OCR 2");
    check(names.read(cv::Mat::zeros(28, 148, CV_8UC3)).id.isEmpty(), "blank OCR rejected");
    std::cout << checks << " checks; " << failures << " failures\n";
    return failures ? 1 : 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
