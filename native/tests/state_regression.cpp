#include "state_regression.hpp"
#include "core/names.hpp"
#include "core/public_state.hpp"
#include "core/state.hpp"
#include "state_view.hpp"

QJsonObject stateRegression(const owcs::Observation &base) {
  QJsonArray evidence;
  int failures = 0;
  auto record = [&](QString test, QJsonValue raw, QJsonValue expected, QJsonValue published,
                    QJsonObject metadata, QString layer) {
    bool ok = expected == published;
    failures += !ok;
    evidence.append(QJsonObject{{"test", test},
                                {"raw", raw},
                                {"expected_published", expected},
                                {"published", published},
                                {"metadata", metadata},
                                {"input_kind", layer},
                                {"passed", ok}});
  };
  owcs::State state("numeric-progression");
  auto obs = base;
  auto row = obs.players[0].toObject();
  const QStringList counters{"eliminations", "assists", "deaths", "damage", "healing", "mitigated"};
  auto step = [&](int sequence, QJsonValue input, QJsonValue expected, QString label,
                  QString kind = "correct_raw") {
    QJsonObject stats = row["stats"].toObject();
    for (const auto &key : counters)
      stats[key] = input;
    row["stats"] = stats;
    obs.players[0] = row;
    auto snapshot = state.update(obs, "test", sequence * 200, 0);
    auto b = snapshot["players"].toArray()[0].toObject()["scoreboard"].toObject();
    for (const auto &key : counters)
      record(label + "/" + key, input, expected, b["stats"].toObject()[key],
             b["fields"].toObject()[key].toObject(), kind);
  };
  step(0, 0, QJsonValue::Null, "cold start confirmation");
  step(1, 0, 0, "initial value");
  step(2, 1000, 0, "large first increase");
  step(3, 5000, 1000, "continued fast increase");
  step(4, 15000, 5000, "no rate cap");
  step(5, QJsonValue::Null, 5000, "missing raw retains history", "raw_miss");
  step(6, 30000, 15000, "progression survives a missing observation");
  step(7, 12, 15000, "isolated wrong drop rejected", "injected_raw_error");
  step(8, 40000, 15000, "return to valid progression");
  // A change from a published value to an arbitrary higher value can be either
  // valid growth or an OCR spike. Require a second observation after a change,
  // rather than treating the previous stable reading as confirmation.
  step(9, 40000, 40000, "unchanged value");
  step(10, 90000, 40000, "single upward spike rejected", "injected_raw_error");
  step(11, 40000, 40000, "upward spike never published");
  step(12, 0, 40000, "reset awaits its own confirmation");
  step(13, 2, 0, "replay reset confirms without a magnitude limit");
  step(20, 50000, 0, "long gap starts fresh confirmation");
  step(21, 60000, 50000, "large progression after gap");

  owcs::State identity("name-isolation");
  obs = base;
  auto first = [&](int i) { return identity.update(obs, "test", i * 200, 0); };
  first(0);
  first(1);
  for (int i = 0; i < 3; ++i)
    identity.name(0, {"ALPHA", .99}, 220 + i * 10);
  // Use a different real name crop for a persistent visual mismatch. OCR is
  // deliberately unavailable; another row's readings must not overwrite history.
  obs.names[0] = base.names[6];
  for (int i = 2; i < 12; ++i) {
    auto changed = obs.players[0].toObject();
    auto stats = changed["stats"].toObject();
    stats["damage"] = 10000 + 1000 * i;
    changed["stats"] = stats;
    obs.players[0] = changed;
    auto p = first(i)["players"].toArray()[0].toObject();
    auto b = p["scoreboard"].toObject();
    if (i > 2)
      record("name mismatch retains damage", stats["damage"],
             base.players[0].toObject()["stats"].toObject()["damage"],
             b["stats"].toObject()["damage"], b["fields"].toObject()["damage"].toObject(),
             "name_crop_fault");
    record("old name not attached to current row", "visual mismatch", QJsonValue::Null,
           p["display_name"], {}, "name_crop_fault");
    record("visual mismatch cannot reset identity", "visual mismatch", 0, b["identity_generation"],
           {}, "name_crop_fault");
    identity.name(0, {}, i * 200 + 10);
  }
  auto check = [&](QString label, bool ok) {
    record(label, QJsonValue::Null, true, ok, {}, "synthetic_sequence");
  };
  check("roster exact takes precedence", owcs::matchRoster("F0O", {"F0o", "Foo"}) == "F0o");
  check("roster unique confusion", owcs::matchRoster("F0O", {"Foo"}) == "Foo");
  check("roster ambiguity abstains", owcs::matchRoster("F00", {"F0o", "Foo"}).isEmpty());
  check("roster wrong team abstains", owcs::matchRoster("Alpha", {"Beta"}).isEmpty());
  check("roster unique vertical glyph", owcs::matchRoster("A1pha", {"Alpha"}) == "Alpha");
  check("no roster no guessed correction", owcs::matchRoster("F0O", {}) == "F0O");
  check("roster duplicate case abstains", owcs::matchRoster("ALPHA", {"Alpha", "alpha"}).isEmpty());
  check("roster line parsing",
        owcs::parseRoster(" Alpha\r\n\nBeta\nAlpha") == QStringList{"Alpha", "Beta"});

  owcs::State moving("moving");
  auto input = base;
  input.names[1] = base.names[6];
  auto damage = [&](int row, int value) {
    auto p = input.players[row].toObject();
    auto stats = p["stats"].toObject();
    stats["damage"] = value;
    p["stats"] = stats;
    input.players[row] = p;
  };
  qint64 clock = 0;
  auto tick = [&] {
    clock += 200;
    return moving.update(input, "test", clock, 0);
  };
  auto player = [](const QJsonObject &s, int row) { return test::physicalPlayer(s, row); };
  auto confirm = [&](int row, QString name) {
    for (int i = 0; i < 3; ++i)
      moving.name(row, {name, .99}, ++clock);
  };
  damage(0, 100);
  damage(1, 900);
  tick();
  tick();
  confirm(0, "Alpha");
  confirm(1, "Beta");
  auto initial = tick();
  check("periodic name verification",
        !moving.needsName(0, clock) && moving.needsName(0, clock + 5000));
  check("confirmed player has session identity", player(initial, 0)["player_id"] == "name:alpha");
  // Both occupants move; confirm the destination before the source is replaced.
  std::swap(input.names[0], input.names[1]);
  damage(0, 900);
  damage(1, 100);
  auto pending = tick();
  check("swap hides old attribution", player(pending, 0)["player_id"].isNull() &&
                                          player(pending, 1)["identity_status"] == "pending");
  confirm(1, "Alpha");
  confirm(0, "Beta");
  auto moved = tick();
  check("swapped identity survives row move",
        player(moved, 1)["player_id"] == player(initial, 0)["player_id"]);
  check("swap restores each players own counters",
        player(moved, 0)["stats"].toObject()["damage"] == 900 &&
            player(moved, 1)["stats"].toObject()["damage"] == 100);
  check("row generation advances", player(moved, 0)["identity_generation"] == 1 &&
                                       player(moved, 1)["identity_generation"] == 1);
  // An empty row cannot transfer even highly confident garbage to its occupant.
  auto visibleName = input.names[1];
  input.names[1] = {};
  damage(1, 99999);
  tick();
  auto absent = tick();
  check("empty row retains counter", player(absent, 1)["stats"].toObject()["damage"] == 100);
  check("empty row history is stale",
        player(absent, 1)["stale_fields"].toArray().contains("stats.damage"));
  check("unobscured row continues", player(absent, 0)["identity_status"] == "confirmed");
  // Returning counters replace history after confirmation, even when reset to zero.
  input.names[1] = visibleName;
  damage(1, 0);
  tick();
  auto returned = tick();
  check("return accepts confirmed zero without accumulation",
        player(returned, 1)["stats"].toObject()["damage"] == 0 &&
            !player(returned, 1)["stale_fields"].toArray().contains("stats.damage"));
  damage(1, 150);
  tick();
  returned = tick();
  check("return resumes actual cumulative total",
        player(returned, 1)["stats"].toObject()["damage"] == 150);
  // A new player starts without the previous occupant's history.
  moving.name(0, {"Gamma", .99}, ++clock);
  auto newPending = tick();
  check("text mismatch alone quarantines row",
        player(newPending, 0)["identity_status"] == "pending");
  confirm(0, "Gamma");
  damage(0, 0);
  auto newPlayer = tick();
  check("replacement cannot inherit counters",
        player(newPlayer, 0)["stats"].toObject()["damage"].isNull());
  newPlayer = tick();
  check("replacement starts at its observed zero",
        player(newPlayer, 0)["stats"].toObject()["damage"] == 0);
  confirm(0, "Alpha");
  auto duplicate = tick();
  check("same team duplicate cannot steal identity",
        player(duplicate, 0)["identity_status"] == "pending");
  confirm(0, "Gamma");
  confirm(0, "Beta");
  auto rejoined = tick();
  check("departed player restores own archive",
        player(rejoined, 0)["player_id"] == "name:beta" &&
            player(rejoined, 0)["stats"].toObject()["damage"] == 900 &&
            player(rejoined, 0)["stale_fields"].toArray().contains("stats.damage"));
  confirm(5, "Beta");
  auto oppositeDuplicate = tick();
  check("global pool duplicate cannot claim an opposite team row",
        player(oppositeDuplicate, 5)["identity_status"] == "pending" &&
            player(oppositeDuplicate, 0)["display_name"] == "Beta");
  // Hero miss holds the last ID; an explicitly unselected portrait is separate.
  auto selected = input.players[1].toObject();
  selected["hero"] = "ana";
  selected["hero_selection"] = "selected";
  selected["portrait_occluded"] = false;
  input.players[1] = selected;
  tick();
  tick();
  QJsonValue heroBefore = player(tick(), 1)["hero"];
  auto blank = input.players[1].toObject();
  blank["hero"] = QJsonValue::Null;
  blank["hero_selection"] = "unknown";
  input.players[1] = blank;
  for (int i = 0; i < 8; ++i)
    returned = tick();
  check("blank hero retains last confirmed hero",
        !heroBefore.isNull() && player(returned, 1)["hero"] == heroBefore &&
            player(returned, 1)["stale_fields"].toArray().contains("hero"));
  auto hidden = moving.update({}, "test", clock + 2000, 0);
  check("full panel loss retains hero", player(hidden, 1)["hero"] == heroBefore &&
                                            !hidden["scoreboard"].toObject()["confirmed"].toBool());
  clock += 2000;
  damage(1, 0);
  tick();
  auto reopened = tick();
  check("panel hidden during reconnect accepts new confirmed counters",
        player(reopened, 1)["stats"].toObject()["damage"] == 0 &&
            !player(reopened, 1)["stale_fields"].toArray().contains("stats.damage"));
  moving.reset();
  auto cleared = moving.update({}, "test", clock + 2200, 0);
  check("reset clears session history", player(cleared, 1)["hero"].isNull() &&
                                            player(cleared, 1)["player_id"].isNull() &&
                                            cleared["session_id"] != initial["session_id"]);
  tick();
  tick();
  confirm(0, "Beta");
  check("same name in new session cannot restore old totals",
        player(tick(), 0)["stats"].toObject()["damage"] == 0);
  return {{"checks", evidence.size()}, {"failures", failures}, {"evidence", evidence}};
}
