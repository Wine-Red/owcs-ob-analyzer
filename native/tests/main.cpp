#include "core/names.hpp"
#include "core/public_state.hpp"
#include "core/recognizer.hpp"
#include "core/state.hpp"
#include "life_regression.hpp"
#include "runtime/registry.hpp"
#include "state_regression.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <chrono>
#include <cmath>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
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
    if (args.contains("--state-audit")) {
      auto audit = stateRegression(r.detect(owcs::readImage(path + "/perks.png")));
      std::cout << QJsonDocument(audit).toJson().toStdString();
      return audit["failures"].toInt() ? 1 : 0;
    }
    if (args.contains("--inspect")) {
      int index = args.indexOf("--inspect");
      if (index + 1 >= args.size())
        throw std::runtime_error("--inspect requires an output directory");
      QString output = args[index + 1];
      if (!QDir().mkpath(output))
        throw std::runtime_error("Cannot create inspection directory");
      auto source = owcs::readImage(path);
      cv::Mat normalized;
      cv::resize(source, normalized,
                 {1600, int(std::nearbyint(source.rows * 1600. / source.cols))});
      QJsonArray trace;
      auto obs = r.detect(source, &trace);
      auto overlay = source.clone();
      auto save = [&](const QString &name, const cv::Mat &image) {
        std::vector<uchar> bytes;
        if (!cv::imencode(".png", image, bytes))
          throw std::runtime_error("Cannot encode inspection image");
        QFile f(output + "/" + name);
        if (!f.open(QIODevice::WriteOnly) || f.write(reinterpret_cast<const char *>(bytes.data()),
                                                     bytes.size()) != qint64(bytes.size()))
          throw std::runtime_error("Cannot write inspection image");
      };
      for (int i = 0; i < trace.size(); ++i) {
        auto entry = trace[i].toObject();
        auto a = entry["rect"].toArray();
        cv::Rect area(a[0].toInt(), a[1].toInt(), a[2].toInt(), a[3].toInt());
        const auto &image = entry["sampling"] == "source" ? source : normalized;
        if (area.empty() || (area & cv::Rect(0, 0, image.cols, image.rows)) != area)
          throw std::runtime_error("Inspection crop outside input");
        QString name = QString("%1-row%2-%3-%4.png")
                           .arg(i, 3, 10, QChar('0'))
                           .arg(entry["row"].toInt() + 1)
                           .arg(entry["field"].toString())
                           .arg(entry["sampling"].toString());
        save(name, image(area));
        entry["file"] = name;
        trace[i] = entry;
        // Overlay shows numeric input areas, not overlapping search windows.
        auto field = entry["field"].toString();
        if (field == "portrait" || field == "ultimate_charge" || field == "damage" ||
            field == "healing" || field == "mitigated" || field == "eliminations" ||
            field == "assists" || field == "deaths") {
          double scale = source.cols / double(image.cols);
          cv::Rect native(int(std::nearbyint(area.x * scale)), int(std::nearbyint(area.y * scale)),
                          int(std::nearbyint(area.width * scale)),
                          int(std::nearbyint(area.height * scale)));
          cv::rectangle(overlay, native,
                        field == "portrait" ? cv::Scalar(255, 180, 0) : cv::Scalar(0, 255, 255), 1);
        }
      }
      save("overlay.png", overlay);
      QJsonObject report{
          {"input", path}, {"bounds", obs.bounds}, {"crops", trace}, {"raw", obs.players}};
      QFile json(output + "/crops.json");
      if (!json.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot write crop report");
      json.write(QJsonDocument(report).toJson());
      std::cout << "Exported " << trace.size() << " production crops\n";
      return 0;
    }
    if (args.contains("--patches")) {
      QFile input(path + "/patches.json");
      if (!input.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing patch annotations");
      for (auto item : QJsonDocument::fromJson(input.readAll()).array()) {
        auto c = item.toObject();
        auto patch = owcs::readImage(path + "/" + c["file"].toString());
        QJsonObject result;
        if (c["kind"] == "face") {
          auto face = r.face(patch);
          result = {{"hero", owcs::nullable(face.candidate.id)},
                    {"hero_confidence", face.candidate.confidence},
                    {"life", face.life},
                    {"ring", face.ring},
                    {"cross", face.cross},
                    {"normal_confidence", face.normalConfidence},
                    {"respawn", owcs::nullable(face.respawn.value)}};
        } else if (c["kind"] == "perk")
          result = r.perk(patch, c["hero"].toString(), c["tier"].toString());
        else {
          auto number = r.number(patch, c["maximum"].toInt(999999));
          result = {{"value", owcs::nullable(number.value)}, {"confidence", number.confidence}};
        }
        std::cout << QJsonDocument(QJsonObject{{"file", c["file"]}, {"result", result}})
                         .toJson(QJsonDocument::Compact)
                         .toStdString()
                  << "\n";
      }
      return 0;
    }
    if (args.contains("--sequence")) {
      owcs::State state("sequence");
      qint64 time = 0;
      for (const auto &file : QDir(path).entryList({"frame-*.png"}, QDir::Files, QDir::Name)) {
        auto observed = r.detect(owcs::readImage(path + "/" + file));
        if (args.contains("--settled")) {
          state.reset();
          for (int i = 0; i < 3; ++i) {
            state.update(observed, file, time, 0);
            time += 200;
          }
        }
        auto snapshot = state.update(observed, file, time, 0);
        std::cout << QJsonDocument(
                         QJsonObject{{"file", file},
                                     {"settled_observations", args.contains("--settled") ? 4 : 0},
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
      snapshot = owcs::localizedState(snapshot, r.localizedNames());
      if (args.contains("--public"))
        snapshot = owcs::publicState(snapshot);
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
    // Known geometry, with a deliberately offset/quantized header. The row
    // lattice, not the header thickness, defines the true crop coordinates.
    {
      cv::Mat grid(1000, 1600, CV_8UC3, cv::Scalar(8, 8, 8));
      const double origin = 101.5, scale = 1.055;
      cv::rectangle(grid, {400, 100, 800, 27}, cv::Scalar(210, 210, 210), -1);
      for (int team = 0; team < 2; ++team)
        for (int row = 0; row < 5; ++row) {
          int a = int(std::nearbyint(origin + (27 + row * 55 + team * 339) * scale));
          int b = int(std::nearbyint(origin + (82 + row * 55 + team * 339) * scale));
          int c = row % 2 ? 95 : 40;
          cv::rectangle(grid, {400, a, 800, b - a}, cv::Scalar(c, c, c), -1);
        }
      QJsonArray crops;
      auto fitted = r.detect(grid, &crops);
      check(fitted.visible, "offset header still locates known row lattice");
      int portraits = 0;
      for (auto item : crops) {
        auto entry = item.toObject();
        if (entry["field"] != "portrait")
          continue;
        auto area = entry["rect"].toArray();
        int row = entry["row"].toInt();
        double center = area[1].toDouble() + area[3].toDouble() / 2;
        double wanted = origin + (54 + 55 * (row % 5) + (row >= 5 ? 339 : 0)) * scale;
        check(std::abs(center - wanted) <= 1.,
              "calibrated origin reaches actual portrait crop " + std::to_string(row));
        ++portraits;
      }
      check(portraits == 10, "geometry audit covers every row");
    }
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
        check(e["life"].toArray()[i] == "dead" ? p["hero"].isNull()
                                               : p["hero"] == e["heroes"].toArray()[i],
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
    // Scale the panel relative to an unchanged canvas. Resizing an entire
    // screenshot alone cannot exercise header-thickness/row-pitch drift.
    for (auto key : {"ready", "stability-single-perk"}) {
      auto original = owcs::readImage(path + "/" + key + ".png");
      auto e = expected[key].toObject();
      for (double scale : {.96, .98, 1.02, 1.04}) {
        cv::Mat scaled;
        auto transform = cv::getRotationMatrix2D(
            cv::Point2f(original.cols / 2.f, original.rows / 2.f), 0, scale);
        cv::warpAffine(original, scaled, transform, original.size());
        auto sample = r.detect(scaled);
        std::string label = std::string(key) + " panel-scale " + std::to_string(scale);
        check(sample.confirmed && sample.players.size() == 10, label + " located");
        if (sample.players.size() != 10)
          continue;
        for (int row = 0; row < 10; ++row) {
          auto p = sample.players[row].toObject();
          check(p["ultimate_charge"] == e["charge"].toArray()[row], label + " ultimate");
          int column = 0;
          for (auto stat : {"eliminations", "assists", "deaths", "damage", "healing", "mitigated"})
            check(p["stats"].toObject()[stat] == e["stats"].toArray()[row].toArray()[column++],
                  label + " " + stat + "/" + std::to_string(row));
        }
      }
    }
    auto frame = owcs::readImage(path + "/perks.png");
    // Blank/light backgrounds must not become digits through normalization.
    for (int brightness : {0, 90, 150, 220, 255})
      check(!r.number(cv::Mat(23, 64, CV_8UC3, cv::Scalar::all(brightness))).value,
            "uniform numeric patch rejected " + std::to_string(brightness));
    // These transforms reproduce the old 100 -> 800 error and ready-check
    // misses without manufacturing labels from recognition output.
    auto deathSource = owcs::readImage(path + "/deaths.png");
    cv::Mat shiftedDigits;
    cv::warpAffine(deathSource, shiftedDigits, cv::Mat(cv::Matx23d(1, 0, .45, 0, 1, .45)),
                   deathSource.size());
    auto shiftedObservation = r.detect(shiftedDigits);
    check(shiftedObservation.players.size() == 10, "shifted numeric panel located");
    if (shiftedObservation.players.size() == 10) {
      auto counters = shiftedObservation.players[8].toObject()["stats"].toObject();
      check(counters["mitigated"] == 100, "subpixel shifted 100 is not 800");
      check(counters["damage"] == 1343, "threshold consensus preserves 1343");
    }
    for (int width : {1280, 1920}) {
      auto readySource = owcs::readImage(path + "/ready.png");
      cv::resize(readySource, readySource,
                 {width, int(std::nearbyint(readySource.rows * double(width) / readySource.cols))},
                 0, 0, cv::INTER_AREA);
      auto readyObservation = r.detect(readySource);
      check(readyObservation.players.size() == 10, "resampled ready panel located");
      for (int row = 0; row < readyObservation.players.size(); ++row)
        check(readyObservation.players[row].toObject()["ultimate_charge"] ==
                  expected["ready"].toObject()["charge"].toArray()[row],
              "ready glyph local alignment " + std::to_string(width) + "/" + std::to_string(row));
    }
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
    for (auto color : {cv::Vec3b(130, 45, 85), cv::Vec3b(35, 160, 185), cv::Vec3b(80, 80, 80)}) {
      cv::Mat themed;
      cv::resize(frame, themed, {1600, 1000});
      for (int y = obs.bounds[1].toInt(); y < obs.bounds[1].toInt() + 26; ++y)
        for (int x = obs.bounds[0].toInt(); x < obs.bounds[2].toInt(); ++x) {
          auto &p = themed.at<cv::Vec3b>(y, x);
          if (std::min({p[0], p[1], p[2]}) > 170)
            p = color;
        }
      auto alternateHeader = r.detect(themed);
      check(alternateHeader.confirmed && alternateHeader.players.size() == 10,
            "colored header uses geometric panel fallback");
    }
    // Qt 6.5's non-const operator[] returns a proxy into the temporary object.
    // Materialize a value before that object is destroyed.
    auto perk = [&](int row, const char *tier, const char *key) -> QJsonValue {
      return obs.players[row].toObject()["perks"].toObject()[tier].toObject()[key];
    };
    check(perk(1, "major", "id") == "Focused Rush", "major perk");
    check(perk(1, "minor", "id") == "Partial Scan", "minor perk");
    check(perk(6, "major", "selection") == "unselected", "empty major perk");
    check(perk(6, "minor", "id").isNull(), "cold-start death does not invent perk identity");
    check(perk(9, "major", "id") == "Cybersecurity", "Sombra major");
    check(perk(9, "minor", "id") == "Life Hack", "Sombra minor");
    auto deaths = r.detect(owcs::readImage(path + "/deaths.png"));
    std::array<int, 5> seconds{2, 2, 8, 2, 2};
    for (int i = 0; i < 5; i++)
      check(deaths.players[i].toObject()["respawn_seconds"].toInt() == seconds[i], "respawn timer");
    owcs::State deathHistory("death-history");
    auto active = obs;
    auto activeRow = active.players[0].toObject();
    activeRow["hero"] = "tracer";
    activeRow["hero_selection"] = "selected";
    activeRow["hero_confidence"] = .95;
    activeRow["selection_confidence"] = .95;
    activeRow["life_state"] = "alive";
    activeRow["life_confidence"] = .95;
    active.players[0] = activeRow;
    QJsonObject deathSnapshot;
    for (int i = 0; i < 3; ++i)
      deathSnapshot = deathHistory.update(active, "test", i * 200, 0);
    auto occluded = active;
    auto deadRow = activeRow;
    deadRow["hero"] = "hanzo"; // An adversarial wrong match must never replace history.
    deadRow["life_state"] = "dead";
    deadRow["portrait_occluded"] = true;
    deadRow["respawn_seconds"] = 8;
    occluded.players[0] = deadRow;
    for (int i = 0; i < 5; ++i)
      deathSnapshot = deathHistory.update(occluded, "test", 1000 + i * 500, 0);
    auto retained = deathSnapshot["players"].toArray()[0].toObject();
    check(retained["hero"] == "tracer" && retained["life_state"] == "dead",
          "death holds confirmed hero despite contradictory portrait");
    check(retained["scoreboard"]
                  .toObject()["fields"]
                  .toObject()["hero"]
                  .toObject()["age_ms"]
                  .toInt() == 2600,
          "held hero is history, not a fabricated fresh portrait observation");
    owcs::State coldDeath("cold-death");
    for (int i = 0; i < 4; ++i)
      deathSnapshot = coldDeath.update(occluded, "test", i * 200, 0);
    check(deathSnapshot["players"].toArray()[0].toObject()["hero"].isNull(),
          "cold-start death leaves unknown hero empty");
    activeRow["hero"] = "hanzo";
    active.players[0] = activeRow;
    deathSnapshot = deathHistory.update(active, "test", 4000, 0);
    check(deathSnapshot["players"].toArray()[0].toObject()["hero"].isNull(),
          "revival confirms a changed hero before combining its data");
    for (int i = 1; i < 3; ++i)
      deathSnapshot = deathHistory.update(active, "test", 4000 + i * 200, 0);
    check(deathSnapshot["players"].toArray()[0].toObject()["hero"] == "hanzo",
          "revival can confirm a new hero");
    deathHistory.reset();
    for (int i = 0; i < 3; ++i)
      deathSnapshot = deathHistory.update(occluded, "test", 5000 + i * 200, 0);
    check(deathSnapshot["players"].toArray()[0].toObject()["hero"].isNull(),
          "reset prevents death history leaking across sessions");
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
        QJsonValue value = result.players[i].toObject()["ultimate_charge"];
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
    for (int i = 0; i < 3; ++i)
      state.name(0, {"PLAYER_A", .99}, 10000 + i * 10);
    changed.names[0] = obs.names[6];
    changed.players[0] = obs.players[6];
    auto uncertain = state.update(changed, "test", 10200, 0);
    check(board(uncertain)["identity_generation"] == 0 &&
              board(uncertain)["stats"].toObject()["damage"] == 4350 &&
              board(uncertain)["fields"].toObject()["damage"].toObject()["status"] == "fresh" &&
              uncertain["players"].toArray()[0].toObject()["display_name"].isNull(),
          "name recheck hides only attribution, not independent row readings");
    auto recovered = state.update(obs, "test", 10400, 0);
    check(board(recovered)["identity_generation"] == 0 &&
              board(recovered)["fields"].toObject()["damage"].toObject()["status"] == "fresh",
          "one-frame name glitch does not erase the row");
    for (int i = 0; i < 3; ++i)
      reset = state.update(changed, "test", 10600 + i * 200, 0);
    check(board(reset)["identity_generation"] == 0 &&
              board(reset)["stats"] == changed.players[0].toObject()["stats"] && state.needsName(0),
          "persistent visual name difference cannot reset or block row data");
    state.name(0, {"PLAYER_A", .99}, 11010);
    state.name(0, {"PLAYER_A", .99}, 11020);
    state.name(0, {"PLAYER_A", .99}, 11030);
    reset = state.update(changed, "test", 11200, 0);
    check(board(reset)["identity_generation"] == 0 && !state.needsName(0) &&
              reset["players"].toArray()[0].toObject()["display_name"] == "PLAYER_A",
          "same OCR name reanchors changed appearance without clearing counters");
    state.name(0, {"PLAYER_B", .99}, 11210);
    state.name(0, {"PLAYER_B", .99}, 11220);
    check(board(state.update(changed, "test", 11400, 0))["identity_generation"] == 0,
          "unconfirmed different OCR name cannot reset identity");
    state.name(0, {"PLAYER_B", .99}, 11410);
    reset = state.update(changed, "test", 11600, 0);
    check(board(reset)["identity_generation"] == 1 &&
              board(reset)["stats"].toObject()["damage"].isNull(),
          "confirmed different OCR name resets identity and counters");
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
              board(pendingSwap)["ultimate_charge"] == 80,
          "hero confirmation does not erase independently confirmed charge");
    check(!board(pendingSwap)["stats"].toObject()["damage"].isNull(),
          "hero change preserves independent counters");
    replacement["hero"] = QJsonValue::Null;
    swapped.players[0] = replacement;
    auto missedSwap = heroSwap.update(swapped, "test", 1400, 0);
    check(missedSwap["players"].toArray()[0].toObject()["hero"].isNull(),
          "unreadable replacement frame does not restore historical hero");
    check(board(missedSwap)["ultimate_charge"] == 3 &&
              missedSwap["players"].toArray()[0].toObject()["life_state"] == "alive",
          "charge and life update while replacement hero is unreadable");
    replacement["hero"] = "tracer";
    swapped.players[0] = replacement;
    for (int i = 0; i < 3; ++i)
      pendingSwap = heroSwap.update(swapped, "test", 1600 + i * 200, 0);
    check(pendingSwap["players"].toArray()[0].toObject()["hero"] == "tracer" &&
              board(pendingSwap)["ultimate_charge"] == 3,
          "replacement hero and its charge confirm after intermittent miss");
    // Public projection must preserve business values without leaking diagnostics.
    auto compact = owcs::publicState(snapshot);
    {
      const auto names = r.localizedNames();
      check(names["heroes"].toObject().size() == 54, "all hero Chinese names shipped");
      check(names["heroes"].toObject()["tracer"] == QStringLiteral("猎空"),
            "client hero translation");
      auto translated = owcs::publicState(owcs::localizedState(snapshot, names));
      auto translatedRows = translated["players"].toArray();
      for (int i = 0; i < translatedRows.size(); ++i) {
        auto row = translatedRows[i].toObject();
        row.remove("hero_name_zhCN");
        auto perks = row["perks"].toObject();
        for (auto tier : {"minor", "major"}) {
          auto perk = perks[tier].toObject();
          perk.remove("name_zhCN");
          perks[tier] = perk;
        }
        row["perks"] = perks;
        auto legacy = compact["players"].toArray()[i].toObject();
        legacy.remove("hero_name_zhCN");
        check(row == legacy, "localization preserves every legacy field");
      }
      check(owcs::localizedDisplay("tracer", QStringLiteral("猎空")) == QStringLiteral("猎空") &&
                owcs::localizedDisplay("custom-hero", QJsonValue::Null) == "custom-hero" &&
                owcs::localizedDisplay(QJsonValue::Null, QStringLiteral("旧名称")) ==
                    QStringLiteral("—"),
            "dock Chinese display, English fallback and null identity");
      auto labelRow = [&](const QJsonValue &hero, const QString &tier, const QJsonValue &id,
                          const QString &selection, const QJsonObject &catalog) {
        QJsonObject row{
            {"hero", hero},
            {"scoreboard",
             QJsonObject{{"perks", QJsonObject{{tier, QJsonObject{{"id", id},
                                                                  {"selection", selection}}}}}}}};
        return owcs::publicState(
                   owcs::localizedState({{"players", QJsonArray{row}}}, catalog))["players"]
            .toArray()[0]
            .toObject();
      };
      auto perkCatalog = names["perks"].toObject();
      int translatedPerks = 0;
      for (auto hero = perkCatalog.begin(); hero != perkCatalog.end(); ++hero) {
        for (auto tier : {"minor", "major"}) {
          auto labels = hero.value().toObject()[tier].toObject();
          for (auto perk = labels.begin(); perk != labels.end(); ++perk) {
            auto row = labelRow(hero.key(), tier, perk.key(), "selected", names);
            auto value = row["perks"].toObject()[tier].toObject();
            check(value["id"] == perk.key() && value["name_zhCN"] == perk.value() &&
                      row["hero_name_zhCN"].isString(),
                  "hero/tier scoped perk Chinese name");
            ++translatedPerks;
          }
        }
      }
      check(translatedPerks == 216, "all active perk labels shipped");
      auto noLabels = labelRow("tracer", "major", "Blink Packs", "selected", {});
      check(noLabels["hero"] == "tracer" && noLabels["hero_name_zhCN"].isNull() &&
                noLabels["perks"].toObject()["major"].toObject()["name_zhCN"].isNull(),
            "older resource packs retain IDs without fabricated names");
      for (auto selection : {"unselected", "not_shown", "unknown"})
        check(labelRow("tracer", "major", "Blink Packs", selection, names)["perks"]
                  .toObject()["major"]
                  .toObject()["name_zhCN"]
                  .isNull(),
              "nonselected perk has no Chinese label");
      check(labelRow(QJsonValue::Null, "major", "Blink Packs", "selected", names)["hero_name_zhCN"]
                .isNull(),
            "pending hero has no Chinese label");
      check(labelRow("ana", "major", "Blink Packs", "selected", names)["perks"]
                .toObject()["major"]
                .toObject()["name_zhCN"]
                .isNull(),
            "hero swap cannot reuse another hero's translation");
      auto oldLabels =
          owcs::publicState(owcs::State::age(owcs::localizedState(snapshot, names), 3000));
      check(oldLabels["players"].toArray()[0].toObject()["hero_name_zhCN"] ==
                    translatedRows[0].toObject()["hero_name_zhCN"] &&
                oldLabels["players"].toArray()[0].toObject()["stale_fields"].toArray().contains(
                    "hero"),
            "retained Chinese name shares hero staleness");
    }
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
    intermittent.observe(1000, .98, 0, 2, owcs::Confirmation::Nondecreasing);
    intermittent.observe(QJsonValue::Null, 0, 200, 2, owcs::Confirmation::Nondecreasing);
    intermittent.observe(1050, .98, 400, 2, owcs::Confirmation::Nondecreasing);
    check(intermittent.value == 1000, "one unreadable frame does not starve changing counters");
    intermittent.observe(QJsonValue::Null, 0, 2000, 2, owcs::Confirmation::Nondecreasing);
    intermittent.observe(1500, .98, 2200, 2, owcs::Confirmation::Nondecreasing);
    check(intermittent.value == 1000 && intermittent.seen == 0,
          "expired candidates and missing observations never refresh values");
    field.observe(80, .99, 0, 2, owcs::Confirmation::Nondecreasing);
    field.observe(81, .99, 200, 2, owcs::Confirmation::Nondecreasing);
    check(field.value == 80, "charging confirms the previous actual observation");
    field.observe(8, .99, 400, 2, owcs::Confirmation::Nondecreasing);
    check(field.value == 80, "reject one erroneous drop");
    field.observe(0, .99, 600, 2, owcs::Confirmation::Nondecreasing);
    field.observe(1, .99, 800, 2, owcs::Confirmation::Nondecreasing);
    check(field.value == 0, "accept confirmed ult reset");
    owcs::NameReader names(args[1]);
    check(names.read(obs.names[6]).id == "WINERED", "Latin OCR 1");
    check(names.read(obs.names[8]).id == "HELLCAT", "Latin OCR 2");
    check(names.read(cv::Mat::zeros(28, 148, CV_8UC3)).id.isEmpty(), "blank OCR rejected");
    auto lifeChecks = lifeRegression(r, path + "/../life");
    checks += lifeChecks.first;
    failures += lifeChecks.second;
    auto audit = stateRegression(obs);
    checks += audit["checks"].toInt();
    failures += audit["failures"].toInt();
    for (auto item : audit["evidence"].toArray())
      if (!item.toObject()["passed"].toBool())
        std::cerr << QJsonDocument(item.toObject()).toJson(QJsonDocument::Compact).toStdString()
                  << "\n";
    std::cout << checks << " checks; " << failures << " failures\n";
    return failures ? 1 : 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
