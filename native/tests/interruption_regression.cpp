#include "core/names.hpp"
#include "core/player_rows.hpp"
#include "core/public_state.hpp"
#include "core/state.hpp"
#include "state_view.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <iostream>
#include <opencv2/imgproc.hpp>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  cv::setNumThreads(2);
  auto args = app.arguments();
  int checks = 0;
  QJsonArray errors;
  auto check = [&](bool ok, QString label) {
    ++checks;
    if (!ok)
      errors.append(label);
  };
  owcs::Recognizer reader(args[1]);
  owcs::NameReader names(args[1]);
  auto original = owcs::readImage(args[2] + "/disconnected.png");
  auto pausedImage = owcs::readImage(args[2] + "/paused.png");
  auto base = reader.detect(original);
  auto paused = reader.detect(pausedImage);
  check(base.confirmed && !base.paused, "disconnected panel stays confirmed, no false pause");
  check(paused.confirmed && paused.paused, "pause banner detected without losing panel");
  check(base.players[9].toObject()["slot_status"] == "empty" && base.names[9].empty(),
        "waiting player is an empty slot, never a name");
  check(paused.players[9].toObject()["slot_status"] == "empty", "paused empty slot detected");
  check(paused.names[4].empty(), "pause-covered name withheld");
  check(paused.players[4].toObject()["stats"].toObject()["damage"].isNull(),
        "pause-covered stat withheld");
  check(paused.players[0].toObject()["hero"] == "moira" &&
            paused.players[6].toObject()["hero"] == "junkrat",
        "other rows still recognize heroes");
  for (int i = 0; i < 9; ++i)
    check(base.players[i].toObject()["slot_status"] == "occupied",
          "real occupied row " + QString::number(i));
  for (double scale : {.75, 1.2}) {
    cv::Mat scaled;
    cv::resize(pausedImage, scaled, {}, scale, scale);
    auto result = reader.detect(scaled);
    check(result.confirmed && result.paused &&
              result.players[9].toObject()["slot_status"] == "empty",
          "scaled pause and vacancy " + QString::number(scale));
  }
  auto raw = names.read(base.names[0], {});
  auto fallback = names.read(base.names[0], {"Leave", "Shy", "Insane"});
  check(!raw.id.isEmpty() && fallback.id == raw.id,
        "real screenshot OCR falls back with unrelated roster");
  check(owcs::resolveName("XINSANE", {"INSANE"}).name == "INSANE", "fuzzy match remains preferred");
  check(owcs::resolveName("PlayerX", {"PlayerA", "PlayerB"}).name == "PlayerX",
        "ambiguous roster falls back");
  check(owcs::resolveName("Unknown", {"Leave"}).name == "Unknown", "unlisted name preserved");
  check(owcs::resolveName(QStringLiteral("等待玩家"), {}).name.isEmpty(),
        "reserved vacancy never becomes player");
  owcs::State similar("similar-name-isolation");
  similar.update(base, "test", 0, 0);
  similar.update(base, "test", 200, 0);
  for (int n = 0; n < 3; ++n)
    similar.name(0, {"Support1", .99}, 210 + n);
  auto lookalike = base;
  lookalike.names[0] = base.names[1];
  auto changed = lookalike.players[0].toObject();
  auto changedStats = changed["stats"].toObject();
  changedStats["damage"] = 9999;
  changed["stats"] = changedStats;
  lookalike.players[0] = changed;
  similar.update(lookalike, "test", 400, 0);
  auto blocked = owcs::publicState(similar.update(lookalike, "test", 600, 0))["players"]
                     .toArray()[0]
                     .toObject();
  check(blocked["player_id"] == "name:support1" && blocked["presence_status"] == "unconfirmed" &&
            blocked["stats"].toObject()["damage"] == 0,
        "one-character lookalike replacement cannot update previous owner's stats");
  check(owcs::Recognizer::signatureSimilarity(owcs::Recognizer::signature(base.names[0]),
                                              owcs::Recognizer::signature(base.names[1])) < .96,
        "similar real names must reverify identity");
  cv::Mat shiftedName;
  cv::Mat affine = (cv::Mat_<double>(2, 3) << 1, 0, 1, 0, 1, 0);
  cv::warpAffine(base.names[0], shiftedName, affine, base.names[0].size());
  check(owcs::Recognizer::signatureSimilarity(owcs::Recognizer::signature(base.names[0]),
                                              owcs::Recognizer::signature(shiftedName)) >= .96,
        "tiny alignment shifts are compared separately from shared prefixes");

  const QStringList botNames{
      QStringLiteral("支援1"), QStringLiteral("支援2"), QStringLiteral("输出1"),
      QStringLiteral("输出2"), QStringLiteral("重装1"), QStringLiteral("支援3"),
      QStringLiteral("输出3"), QStringLiteral("输出4"), QStringLiteral("重装2")};
  for (int row = 0; row < 9; ++row)
    check(names.read(base.names[row], {"Leave", "800", "Insane"}).id == botNames[row],
          "exact bot OCR without hallucinated suffix " + QString::number(row));
  const QStringList realNames{"ALPHARI", "EZHAN", "LENGSA", "MAG",    "RECALL",
                              "800",     "DAMO",  "IR1S",   "INSANE", "VICTORIA"};
  for (auto name : realNames) {
    auto crop = owcs::readImage(args[2] + "/../names/" + name + ".png");
    check(names.read(crop, realNames).id == name, "exact OBS preview name " + name);
    // The extra colored area is background, not permission to append a glyph.
    cv::Mat textured;
    cv::copyMakeBorder(crop, textured, 0, 0, 0, 45, cv::BORDER_CONSTANT, cv::Scalar(30, 60, 100));
    cv::line(textured, {crop.cols + 3, 4}, {textured.cols - 2, 19}, {50, 90, 130}, 2);
    check(names.read(textured, {}).id == name, "raw fallback ignores trailing background " + name);
  }

  owcs::State stable("subpixel-sequence");
  stable.update(base, "test", 0, 0);
  stable.update(base, "test", 200, 0);
  for (int n = 0; n < 3; ++n)
    stable.name(0, {botNames[0], .99}, 210 + n);
  for (int frame = 0; frame < 30; ++frame) {
    auto jitter = base;
    double shift = (frame % 4) * .25;
    cv::Mat transform = (cv::Mat_<double>(2, 3) << 1, 0, shift, 0, 1, shift);
    jitter.names[0].release(); // Observation copies share cv::Mat storage.
    cv::warpAffine(base.names[0], jitter.names[0], transform, base.names[0].size(),
                   cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    auto s = stable.update(jitter, "test", 400 + frame * 200, 0);
    auto p = owcs::publicState(s)["players"].toArray()[0].toObject();
    check(p["display_name"] == botNames[0] && p["presence_status"] == "present" &&
              !p["stale_fields"].toArray().contains("stats.damage"),
          "subpixel sequence keeps identity and counters fresh " + QString::number(frame));
  }
  // A noisy OCR read is quarantined, but one subsequent reading of the
  // confirmed owner recovers in the SAME published cycle, without a new vote.
  stable.name(0, {"SpuriousSuffix", .99}, 6600);
  stable.update(base, "test", 6800, 0);
  stable.name(0, {botNames[0], .99}, 6800);
  auto recovered = owcs::publicState(stable.snapshot(base, "test", 6800, 0));
  check(recovered["players"].toArray()[0].toObject()["display_name"] == botNames[0],
        "same-cycle OCR reverification restores confirmed owner");
  auto again = stable.snapshot(base, "test", 6800, 0);
  check(again["sequence"] == recovered["sequence"], "serialization cannot confirm another frame");
  for (auto pair : {std::pair{"LONGPLAYER1", "LONGPLAYER2"}, std::pair{"800", "8000"}}) {
    cv::Mat first(28, 148, CV_8UC3, cv::Scalar(0)), second = first.clone();
    cv::putText(first, pair.first, {2, 21}, cv::FONT_HERSHEY_SIMPLEX, .55, {255, 255, 255}, 1,
                cv::LINE_AA);
    cv::putText(second, pair.second, {2, 21}, cv::FONT_HERSHEY_SIMPLEX, .55, {255, 255, 255}, 1,
                cv::LINE_AA);
    check(owcs::Recognizer::signatureSimilarity(owcs::Recognizer::signature(first),
                                                owcs::Recognizer::signature(second)) < .96,
          QString("changed final glyph requires verification: ") + pair.first);
  }

  owcs::State state("disconnect-sequence");
  auto input = base;
  qint64 clock = 0;
  auto player = test::physicalPlayer;
  auto tick = [&] {
    clock += 200;
    return state.update(input, "test", clock, 0);
  };
  auto confirm = [&](int row, QString name) {
    for (int i = 0; i < 5; ++i)
      state.name(row, {name, .99}, ++clock);
  };
  auto damage = [&](int row, int value) {
    auto p = input.players[row].toObject(), stats = p["stats"].toObject();
    stats["damage"] = value;
    p["stats"] = stats;
    input.players[row] = p;
  };
  damage(0, 100);
  damage(1, 200);
  tick();
  tick();
  confirm(0, "Alpha");
  confirm(1, "Beta");
  tick();
  auto firstName = input.names[0];
  auto firstRow = input.players[0];
  input.players[0] = base.players[9];
  input.names[0] = {};
  damage(1, 250);
  tick();
  auto absent = tick();
  check(player(absent, 0)["slot_status"] == "empty" && player(absent, 0)["player_id"].isNull(),
        "vacancy removes current owner");
  check(player(absent, 0)["historical_player_id"] == "name:alpha" &&
            player(absent, 0)["stats"].toObject()["damage"] == 100,
        "vacancy retains explicitly attributed stale history");
  check(player(absent, 1)["stats"].toObject()["damage"] == 250 &&
            !player(absent, 1)["stale_fields"].toArray().contains("stats.damage"),
        "unaffected row continues updating during disconnect");
  check(!state.needsName(0, clock), "empty slot does not consume OCR budget");
  state.name(0, {QStringLiteral("等待玩家"), 1}, ++clock);
  check(player(tick(), 0)["player_id"].isNull(), "vacancy cannot be claimed by OCR");
  // Beta shifts into the departed slot; its former position becomes vacant.
  input.players[0] = input.players[1];
  input.names[0] = input.names[1];
  input.players[1] = base.players[9];
  input.names[1] = {};
  tick();
  confirm(0, "Beta");
  auto shifted = tick();
  check(player(shifted, 0)["player_id"] == "name:beta" &&
            player(shifted, 0)["stats"].toObject()["damage"] == 250,
        "shifted player restores its own history, not departed player's");
  bool archived = false;
  for (auto v : shifted["retained_players"].toArray())
    archived |= v.toObject()["player_id"] == "name:alpha" &&
                v.toObject()["stats"].toObject()["damage"] == 100;
  check(archived, "departed player remains accessible after its row is replaced");
  // New, unlisted participant fills the vacancy, starting at zero.
  input.players[1] = firstRow;
  input.names[1] = firstName;
  damage(1, 0);
  tick();
  confirm(1, owcs::resolveName("NewPlayer", {"Alpha", "Beta"}).name);
  auto fresh = tick();
  check(player(fresh, 1)["display_name"] == "NewPlayer" &&
            player(fresh, 1)["stats"].toObject()["damage"].isNull(),
        "replacement cannot inherit former occupant's counters");
  fresh = tick();
  check(player(fresh, 1)["stats"].toObject()["damage"] == 0,
        "replacement confirms its own counters");
  // Original player returns: preserve history until the new counters confirm.
  state.name(1, {"Alpha", .99}, ++clock);
  tick();
  confirm(1, "Alpha");
  auto returned = tick();
  check(player(returned, 1)["player_id"] == "name:alpha" &&
            player(returned, 1)["stats"].toObject()["damage"] == 100 &&
            player(returned, 1)["stale_fields"].toArray().contains("stats.damage"),
        "returning player holds own history until new counters confirm");
  returned = tick();
  check(player(returned, 1)["stats"].toObject()["damage"] == 0 &&
            !player(returned, 1)["stale_fields"].toArray().contains("stats.damage"),
        "returning player accepts zero without adding or retaining the old floor");
  damage(1, 150);
  tick();
  returned = tick();
  check(player(returned, 1)["stats"].toObject()["damage"] == 150,
        "returning counters resume at actual observed total");
  input.paused = true;
  input.names[1] = {};
  damage(1, 99999);
  damage(0, 300);
  tick();
  auto duringPause = tick();
  check(duringPause["game_paused"] == true &&
            player(duringPause, 1)["stats"].toObject()["damage"] == 150,
        "covered row retains last value through pause");
  check(player(duringPause, 0)["stats"].toObject()["damage"] == 300,
        "readable row continues during pause");
  input.paused = false;
  input.names[1] = firstName;
  damage(1, 160);
  tick();
  auto resumed = tick();
  check(resumed["game_paused"] == false && player(resumed, 1)["stats"].toObject()["damage"] == 160,
        "uncovered row resumes after pause");
  state.reset();
  auto reset = tick();
  check(reset["retained_players"].toArray().isEmpty(), "new match clears archived identities");

  // Realistic compaction: the SECOND player disappears, rows 3/4/5 move up,
  // and only the BOTTOM slot is empty. API and UI identities must not move.
  owcs::State compacting("named-compaction");
  auto full = base;
  const QStringList keys{"eliminations", "assists", "deaths", "damage", "healing", "mitigated"};
  for (int row = 0; row < 9; ++row) {
    auto p = full.players[row].toObject();
    QJsonObject stats;
    for (int k = 0; k < keys.size(); ++k)
      stats[keys[k]] = (row + 1) * 100 + k;
    p["stats"] = stats;
    full.players[row] = p;
  }
  auto current = full;
  qint64 t = 0;
  auto advance = [&] {
    t += 200;
    return owcs::publicState(compacting.update(current, "test", t, 0));
  };
  auto identify = [&](int row, int person) {
    for (int n = 0; n < 3; ++n)
      compacting.name(row, {"Person" + QString::number(person), .99}, ++t);
  };
  auto named = [](const QJsonObject &s, int person) {
    return test::player(s, "name:person" + QString::number(person));
  };
  advance();
  advance();
  // Reproduce late name confirmations: A1 and B3 arrive after later slots.
  for (int i : {1, 2, 3, 4, 5, 6, 8, 0, 7})
    identify(i, i);
  auto initialNamed = advance();
  auto stableOrder = test::ids(initialNamed);
  QJsonObject priorities;
  for (int i = 0; i < 9; ++i)
    priorities["name:person" + QString::number(i)] = 9 - i;
  auto prioritized = [&](QJsonObject snapshot) {
    snapshot["player_priorities"] = priorities;
    return owcs::publicState(snapshot);
  };
  const auto fixedPriorityOrder =
      test::ids(prioritized(compacting.snapshot(current, "test", t, 0)));
  auto dockRows = [&] {
    return owcs::orderedPlayerRows(
        compacting.snapshot(current, "test", t, 0)["participants"].toArray());
  };
  auto dockIds = [&] {
    QJsonArray ids;
    for (const auto &row : dockRows())
      ids.append(row.toObject()["player_id"]);
    return ids;
  };
  QJsonArray expectedDockIds;
  for (int i : {1, 2, 3, 4, 5, 6, 8, 0, 7})
    expectedDockIds.append("name:person" + QString::number(i));
  check(dockIds() == expectedDockIds && stableOrder == expectedDockIds,
        "late confirmations append to a stable first-confirmation order");
  check(initialNamed["players"].toArray().size() == 9,
        "named output contains confirmed players, never the waiting slot");
  for (int i = 0; i < 9; ++i)
    check(named(initialNamed, i)["team"] == (i < 5 ? "A" : "B"),
          "confirmed named and physical records agree on team: " + QString::number(i));
  check(!initialNamed.contains("players_by_id") && !initialNamed.contains("retained_players") &&
            !initialNamed.contains("player_order"),
        "only one player collection is public");
  for (int i = 1; i < 4; ++i) {
    current.players[i] = full.players[i + 1];
    current.names[i] = full.names[i + 1];
  }
  current.players[4] = base.players[9];
  current.names[4] = {};
  auto pendingCompaction = advance();
  check(named(pendingCompaction, 1)["presence_status"] == "unconfirmed" &&
            !named(pendingCompaction, 1).contains("position"),
        "unresolved compaction cannot guess which occupant disconnected");
  check(named(pendingCompaction, 1)["team"] == "A" &&
            named(pendingCompaction, 1)["player_id"] == "name:person1",
        "unresolved identity retains its own team and ID");
  for (int i = 3; i >= 1; --i)
    identify(i, i + 1); // Deliberately resolve destinations in reverse order.
  advance();
  auto compressed = advance();
  check(compressed["players"].toArray().size() == 9,
        "compaction preserves nine identities without creating a waiting player");
  check(dockIds() == expectedDockIds && test::ids(compressed) == stableOrder,
        "compaction and departure retain every display position");
  const auto rawParticipants = compacting.snapshot(current, "test", t, 0)["participants"].toArray();
  const auto sortedParticipants = owcs::orderedPlayerRows(rawParticipants);
  bool recordsIntact = true;
  for (const auto &row : sortedParticipants)
    recordsIntact &= rawParticipants.contains(row);
  check(recordsIntact && sortedParticipants.size() == rawParticipants.size(),
        "display sorting preserves complete identity, stats and stale metadata");
  check(named(compressed, 1)["presence_status"] == "missing" &&
            !named(compressed, 1).contains("position") &&
            named(compressed, 1)["hero"] == named(initialNamed, 1)["hero"] &&
            named(compressed, 1)["perks"] == named(initialNamed, 1)["perks"],
        "departed player keeps its complete own snapshot after row replacement");
  check(named(compressed, 1)["team"] == "A",
        "departed identity retains last confirmed team through compaction");
  check(test::ids(compressed).toVariantList().count(QString("name:person1")) == 1,
        "departed history appears exactly once in the same players list");
  for (auto key : keys)
    check(named(compressed, 1)["stats"].toObject()[key] ==
                  named(initialNamed, 1)["stats"].toObject()[key] &&
              named(compressed, 1)["stale_fields"].toArray().contains("stats." + key),
          "departed counter remains attributed and stale: " + key);
  for (int i = 2; i <= 4; ++i)
    check(named(compressed, i)["presence_status"] == "present" &&
              named(compressed, i)["stats"] == named(initialNamed, i)["stats"],
          "shifted occupant owns its values: " + QString::number(i));

  auto checkFixedOrder = [&](const char *label) {
    const auto snapshot = compacting.snapshot(current, "test", t, 0);
    QJsonArray ids;
    for (const auto &row : owcs::orderedPlayerRows(snapshot["participants"].toArray(), priorities))
      ids.append(row.toObject()["player_id"]);
    check(ids == fixedPriorityOrder && test::ids(prioritized(snapshot)) == fixedPriorityOrder &&
              dockIds() == stableOrder && test::ids(owcs::publicState(snapshot)) == stableOrder,
          label);
  };
  checkFixedOrder("departure keeps explicit priorities and default ties in dock and API");
  const auto compactedObservation = current;
  current.visible = current.confirmed = false;
  const auto hidden = advance();
  checkFixedOrder("Tab release after departure cannot move any player");
  check(named(hidden, 1)["presence_status"] == "unconfirmed" &&
            named(hidden, 1)["stats"] == named(compressed, 1)["stats"] &&
            named(hidden, 1)["stale_fields"].toArray().contains("stats.damage"),
        "hidden departed record retains stats and explicitly marks history");
  current = compactedObservation;
  advance();
  checkFixedOrder("first returning Tab frame keeps all positions during confirmation");
  const auto reopened = advance();
  checkFixedOrder("confirmed Tab panel cannot move departed player to the bottom");
  check(named(reopened, 1)["presence_status"] == "missing" &&
            named(reopened, 1)["stats"] == named(compressed, 1)["stats"],
        "restored panel marks departure without losing history");
  const auto aged = owcs::State::age(compacting.snapshot(current, "test", t, 0), 4000);
  check(test::ids(prioritized(aged)) == fixedPriorityOrder &&
            test::ids(owcs::publicState(aged)) == stableOrder,
        "stale source cannot reorder explicit priorities or default ties");

  // The same name returns to its original position with ALL counters reset.
  current = full;
  auto returning = current.players[1].toObject();
  QJsonObject zero;
  for (auto key : keys)
    zero[key] = 0;
  returning["stats"] = zero;
  current.players[1] = returning;
  advance();
  for (int i = 1; i < 5; ++i)
    identify(i, i);
  advance();
  auto reconnected = advance();
  checkFixedOrder("reconnection preserves both explicit-priority and equal-priority order");
  check(dockIds() == expectedDockIds, "reconnected identity keeps its fixed display position");
  check(test::ids(reconnected) == stableOrder &&
            named(reconnected, 1)["presence_status"] == "present" &&
            named(reconnected, 1)["player_id"] == "name:person1",
        "reconnected player updates existing named record at new verified position");
  check(named(reconnected, 1)["team"] == "A", "reconnection confirms current team");
  for (auto key : keys)
    check(named(reconnected, 1)["stats"].toObject()[key] == 0 &&
              !named(reconnected, 1)["stale_fields"].toArray().contains("stats." + key),
          "reconnected counter starts at observed zero: " + key);
  for (auto key : keys)
    zero[key] = 3;
  returning["stats"] = zero;
  current.players[1] = returning;
  advance();
  auto low = advance();
  check(named(low, 1)["stats"] == zero,
        "lower new readings update without a historical floor or sum");
  for (int i : {0, 2, 3, 4, 5, 6, 7, 8})
    check(named(low, i)["stats"] == named(initialNamed, i)["stats"],
          "other player counters untouched by reconnect: " + QString::number(i));

  // Position metadata also survives a side change without changing the key.
  QJsonValue formerRow = current.players[0];
  current.players[0] = current.players[5];
  current.players[5] = formerRow;
  std::swap(current.names[0], current.names[5]);
  advance();
  identify(0, 5);
  identify(5, 0);
  advance();
  auto crossSide = advance();
  checkFixedOrder("team and physical slot changes preserve fixed order");
  auto beforePauseDockIds = dockIds();
  check(named(crossSide, 0)["team"] == "B" &&
            named(crossSide, 0)["stats"] == named(initialNamed, 0)["stats"] &&
            crossSide["players"].toArray().size() == 9,
        "name identity does not contain team or physical row");
  check(named(crossSide, 0)["team"] == "B" && named(crossSide, 5)["team"] == "A",
        "confirmed side change updates team without locking the lineup");
  current.paused = true;
  current.names[5] = {};
  auto obscured = advance();
  check(dockIds() == beforePauseDockIds,
        "temporary OCR loss keeps last known display placement without claiming presence");
  check(named(obscured, 0)["presence_status"] == "unconfirmed" &&
            named(obscured, 0)["stats"] == named(crossSide, 0)["stats"] &&
            named(obscured, 5)["presence_status"] == "present",
        "pause OCR loss holds named history without declaring a disconnect");
  check(named(obscured, 0)["team"] == "B" && named(obscured, 5)["team"] == "A",
        "pause retains last confirmed team independently for each player");
  auto staleNamed =
      owcs::publicState(owcs::State::age(compacting.snapshot(current, "test", t, 0), 4000));
  check(named(staleNamed, 5)["presence_status"] == "unconfirmed" &&
            !named(staleNamed, 5).contains("position") &&
            named(staleNamed, 5)["stale_fields"].toArray().contains("stats.damage"),
        "aged named records cannot advertise live positions or fresh statistics");
  check(named(staleNamed, 0)["team"] == "B" && named(staleNamed, 5)["team"] == "A",
        "hidden or stale panel retains teams as historical metadata");
  compacting.reset();
  auto clearedNamed = advance();
  check(clearedNamed["players"].toArray().isEmpty() &&
            clearedNamed["session_id"] != initialNamed["session_id"],
        "reset clears the named registry and order together");
  std::cout << QJsonDocument(
                   QJsonObject{{"checks", checks}, {"failures", errors.size()}, {"errors", errors}})
                   .toJson()
                   .toStdString();
  return errors.isEmpty() ? 0 : 1;
}
