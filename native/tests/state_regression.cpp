#include "state_regression.hpp"
#include "core/state.hpp"

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
  // deliberately unavailable; valid numeric observations must still flow.
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
      record("name mismatch cannot starve damage", stats["damage"], 10000 + 1000 * (i - 1),
             b["stats"].toObject()["damage"], b["fields"].toObject()["damage"].toObject(),
             "name_crop_fault");
    record("old name not attached to current row", "visual mismatch", QJsonValue::Null,
           p["display_name"], {}, "name_crop_fault");
    record("visual mismatch cannot reset identity", "visual mismatch", 0, b["identity_generation"],
           {}, "name_crop_fault");
    identity.name(0, {}, i * 200 + 10);
  }
  return {{"checks", evidence.size()}, {"failures", failures}, {"evidence", evidence}};
}
