#include "roster_regression.hpp"
#include "core/names.hpp"
#include "core/roster.hpp"
#include "runtime/registry.hpp"
#include <QFile>
#include <QJsonDocument>
QJsonObject rosterRegression(const QString &dir) {
  int checks = 0;
  QJsonArray failures;
  auto check = [&](bool ok, QString label) {
    ++checks;
    if (!ok)
      failures.append(label);
  };
  using owcs::matchRoster;
  check(matchRoster("LIP", {"Lip", "Leave"}) == "Lip", "exact short ID wins");
  check(matchRoster("L1P", {"LIP"}) == "LIP", "short ID confusable glyph");
  check(matchRoster("LXP", {"LIP"}).isEmpty(), "short ID arbitrary substitution rejected");
  check(matchRoster("Sh", {"Shu", "Shy"}).isEmpty(), "short truncation ambiguous");
  check(matchRoster("Leavc", {"Leave", "Landon"}) == "Leave", "five character near match");
  check(matchRoster("FunnyAstr", {"FunnyAstro", "UltraViolet"}) == "FunnyAstro", "long omission");
  check(matchRoster("UltrxViolex", {"UltraViolet", "Viol2t"}) == "UltraViolet",
        "long two substitutions");
  check(matchRoster("Sguarfree", {"Sugarfree"}) == "Sugarfree", "transposition");
  check(matchRoster("CHORONG", {"CH0R0NG"}) == "CH0R0NG", "multiple confusable glyphs");
  check(matchRoster("PlayerX", {"PlayerA", "PlayerB"}).isEmpty(), "near tie abstains");
  check(matchRoster("Fun", {"FunnyAstro"}).isEmpty(), "short prefix cannot match long name");
  check(matchRoster("FreeAgent", {"Leave", "Shy"}).isEmpty(), "unrelated names abstain");
  check(matchRoster(QStringLiteral("选手甲"), {QStringLiteral("选手甲"), "Leave"}) ==
            QStringLiteral("选手甲"),
        "Unicode exact matching");
  check(owcs::parseRoster("Leave\nLEAVE\nShy") == QStringList{"Leave", "Shy"},
        "pool casefold deduplication");
  check(matchRoster(QString(33, 'a'), {QString(32, 'a')}).isEmpty(), "overlong OCR rejected");
  const QStringList longNames{"FunnyAstro", "UltraViolet", "Sugarfree", "FAMIGLISTIMO"};
  for (const auto &name : longNames)
    for (int i = 0; i < name.size(); ++i) {
      auto missing = name;
      missing.remove(i, 1);
      check(matchRoster(missing, longNames) == name,
            "single omission: " + name + " at " + QString::number(i));
    }
  for (auto input :
       {"https://evil.test/overwatch/Event", "https://liquipedia.net.evil.test/overwatch/Event",
        "https://liquipedia.net/dota2/Event", "https://x@liquipedia.net/overwatch/Event",
        "https://liquipedia.net:444/overwatch/Event", "Template:Example", "../api.php", ""})
    check(owcs::liquipediaPage(input).isEmpty(), "reject invalid page: " + QString(input));
  check(owcs::liquipediaPage(
            "https://liquipedia.net/overwatch/Overwatch_Champions_Series/2026#Participants") ==
            "Overwatch Champions Series/2026",
        "canonical page URL");
  check(owcs::liquipediaSearch("OWCS 2026") ==
            "intitle:Overwatch intitle:Champions intitle:Series intitle:2026",
        "title search expands OWCS");
  QFile html(dir + "/champions-clash-2026.html"), expected(dir + "/expected.json");
  check(html.open(QIODevice::ReadOnly) && expected.open(QIODevice::ReadOnly),
        "real roster fixture opens");
  auto actual = owcs::parseTournamentRoster(QString::fromUtf8(html.readAll()));
  auto labels = QJsonDocument::fromJson(expected.readAll()).object();
  QStringList names;
  for (auto name : labels["names"].toArray())
    names << name.toString();
  names.sort(Qt::CaseInsensitive);
  auto sorted = actual.names;
  sorted.sort(Qt::CaseInsensitive);
  check(!names.isEmpty() && sorted == names, "real event players match independent parser");
  check(actual.participants == labels["participants"].toInt(), "real event participant cards");
  auto member = [](QString name, QString role) {
    return "<div class='team-participant-card__member'><div "
           "class='team-participant-card__member-role-left'><img alt='" +
           role + "'></div><div class='team-participant-card__member-name'><span class='name'><a>" +
           name + "</a></span></div></div>";
  };
  QString synthetic =
      "<h2>Participants<span>[edit]</span></h2><div class='team-participant-card'><div "
      "class='toggle-area'><button data-toggle-area-btn='1'>Main</button><button "
      "data-toggle-area-btn='2'>Staff</button><button data-toggle-area-btn='3'>Bench</button>"
      "<div data-toggle-area-content='1'>" +
      member("Player&#48;", "DPS") + member("TBD", "Tank") +
      "</div>"
      "<div data-toggle-area-content='2'>" +
      member("Coach", "Support") +
      "</div>"
      "<div data-toggle-area-content='3'>" +
      member("SubPlayer", "") +
      "</div></div></div><h2>Results</h2><div class='team-participant-card'>" +
      member("Unrelated", "DPS") + "</div>";
  check(owcs::parseTournamentRoster(synthetic).names == QStringList{"Player0", "SubPlayer"},
        "include bench and decode entities; exclude staff and unrelated sections");
  check(owcs::parseTournamentRoster("<h2>Participants</h2>").names.isEmpty(),
        "no roster never invents names");
  auto &registry = owcs::Registry::instance();
  registry.setRoster({"Leave", "Shy"});
  check(registry.roster() == QStringList{"Leave", "Shy"}, "one shared pool independent of team");
  registry.setRoster({});
  return {{"checks", checks},
          {"failures", failures.size()},
          {"errors", failures},
          {"fixture_names", QJsonArray::fromStringList(sorted)}};
}
