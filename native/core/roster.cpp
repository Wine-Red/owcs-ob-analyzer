#include "roster.hpp"
#include "names.hpp"
#include <QMap>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <vector>
namespace owcs {
QString liquipediaPage(const QString &input) {
  QString page = input.trimmed();
  if (page.startsWith("http://") || page.startsWith("https://")) {
    QUrl url(page, QUrl::StrictMode);
    if (!url.isValid() || (url.host() != "liquipedia.net" && url.host() != "www.liquipedia.net") ||
        !url.userInfo().isEmpty() || url.port() != -1 || !url.path().startsWith("/overwatch/"))
      return {};
    page = url.path() == "/overwatch/index.php"
               ? QUrlQuery(url).queryItemValue("title", QUrl::FullyDecoded)
               : url.path().mid(11);
  }
  page.replace('_', ' ');
  page = page.trimmed();
  if (page.isEmpty() || page.size() > 250 || page.endsWith(".php", Qt::CaseInsensitive) ||
      page.contains(QRegularExpression("[:?#\\\\\\p{Cc}]")))
    return {};
  return page;
}
QString liquipediaSearch(const QString &text) {
  QString s = text;
  s.replace(QRegularExpression("\\bOWCS\\b", QRegularExpression::CaseInsensitiveOption),
            "Overwatch Champions Series");
  QStringList query;
  for (auto word : s.split(QRegularExpression("[^\\p{L}\\p{N}]+"), Qt::SkipEmptyParts))
    query.append("intitle:" + word);
  return query.join(' ');
}
namespace {
QString decoded(QString text) {
  static const QRegularExpression numeric("&#(x[0-9a-fA-F]+|[0-9]+);");
  auto matches = numeric.globalMatch(text);
  struct Replacement {
    int pos, size;
    QString value;
  };
  std::vector<Replacement> replacements;
  while (matches.hasNext()) {
    auto m = matches.next();
    auto n = m.captured(1);
    bool ok;
    char32_t c = n.startsWith('x') ? n.mid(1).toUInt(&ok, 16) : n.toUInt(&ok);
    if (ok && c && c <= 0x10ffff && !(c >= 0xd800 && c <= 0xdfff))
      replacements.push_back(
          {int(m.capturedStart()), int(m.capturedLength()), QString::fromUcs4(&c, 1)});
  }
  for (auto i = replacements.rbegin(); i != replacements.rend(); ++i)
    text.replace(i->pos, i->size, i->value);
  text.replace("&nbsp;", " ")
      .replace("&quot;", "\"")
      .replace("&apos;", "'")
      .replace("&lt;", "<")
      .replace("&gt;", ">")
      .replace("&amp;", "&");
  return text;
}
struct Node {
  QString tag, text;
  QMap<QString, QString> attrs;
  int parent = -1, end = 0;
  bool has(const QString &c) const {
    return attrs.value("class").split(' ', Qt::SkipEmptyParts).contains(c);
  }
};
// Small inert HTML tree: never executes HTML, scripts, URLs or styles. Void tags
// and nested participant cards are handled structurally, not by a greedy regex.
std::vector<Node> tree(const QString &html) {
  std::vector<Node> nodes(1);
  std::vector<int> stack{0};
  static const QRegularExpression token(
      "<!--[\\s\\S]*?-->|<(?:[^>\"']|\"[^\"]*\"|'[^']*')*>|[^<]+");
  static const QRegularExpression tag("^<\\s*(/?)\\s*([a-zA-Z0-9]+)");
  static const QRegularExpression attr(
      "([a-zA-Z_:][a-zA-Z0-9_:.-]*)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))");
  auto tokens = token.globalMatch(html);
  while (tokens.hasNext()) {
    const auto t = tokens.next().captured();
    if (!t.startsWith('<')) {
      if (nodes[stack.back()].tag != "script" && nodes[stack.back()].tag != "style")
        for (int i : stack)
          nodes[i].text += t;
      continue;
    }
    auto match = tag.match(t);
    if (!match.hasMatch())
      continue;
    const auto name = match.captured(2).toLower();
    if (!match.captured(1).isEmpty()) {
      for (int k = int(stack.size()) - 1; k > 0; --k)
        if (nodes[stack[k]].tag == name) {
          while (int(stack.size()) > k) {
            nodes[stack.back()].end = int(nodes.size());
            stack.pop_back();
          }
          break;
        }
      continue;
    }
    Node n;
    n.tag = name;
    n.parent = stack.back();
    auto attrs = attr.globalMatch(t.mid(match.capturedLength()));
    while (attrs.hasNext()) {
      auto a = attrs.next();
      n.attrs[a.captured(1).toLower()] =
          decoded(a.captured(2).isNull() ? (a.captured(3).isNull() ? a.captured(4) : a.captured(3))
                                         : a.captured(2));
    }
    int index = int(nodes.size());
    nodes.push_back(n);
    if (t.endsWith("/>") || QStringList{"img", "br", "hr", "input", "meta", "link", "wbr", "source",
                                        "area", "col", "embed", "param"}
                                .contains(name))
      nodes[index].end = index + 1;
    else
      stack.push_back(index);
  }
  for (int i : stack)
    nodes[i].end = int(nodes.size());
  for (auto &n : nodes)
    n.text = decoded(n.text).simplified();
  return nodes;
}
} // namespace
RosterPage parseTournamentRoster(const QString &html) {
  RosterPage result;
  if (html.size() > 8 * 1024 * 1024) {
    result.warnings << QStringLiteral("赛事页面过大");
    return result;
  }
  auto nodes = tree(html);
  auto findClass = [&](int parent, QString cls) {
    for (int j = parent + 1; j < nodes[parent].end; ++j)
      if (nodes[j].has(cls))
        return j;
    return -1;
  };
  bool participants = false;
  for (int i = 1; i < nodes.size(); ++i) {
    const auto &n = nodes[i];
    if (n.tag == "h2") {
      QString title = n.text;
      title.remove("[edit]", Qt::CaseInsensitive);
      participants = QRegularExpression("^(participants|participating teams|teams)$",
                                        QRegularExpression::CaseInsensitiveOption)
                         .match(title.trimmed())
                         .hasMatch();
    }
    if (!participants || !n.has("team-participant-card"))
      continue;
    ++result.participants;
    int before = result.names.size();
    for (int j = i + 1; j < n.end; ++j) {
      if (!nodes[j].has("team-participant-card__member"))
        continue;
      QString tab, role, rightRole;
      for (int p = nodes[j].parent; p > i; p = nodes[p].parent) {
        if (!nodes[p].attrs.contains("data-toggle-area-content"))
          continue;
        int area = nodes[p].parent;
        while (area > i && !nodes[area].has("toggle-area"))
          area = nodes[area].parent;
        for (int k = area + 1; k < nodes[area].end; ++k)
          if (nodes[k].attrs.value("data-toggle-area-btn") ==
              nodes[p].attrs["data-toggle-area-content"]) {
            tab = nodes[k].text;
            break;
          }
        break;
      }
      int right = findClass(j, "team-participant-card__member-role-right");
      if (right >= 0)
        rightRole = nodes[right].text;
      if (QRegularExpression("staff|coach|manager|analyst|owner|content|social",
                             QRegularExpression::CaseInsensitiveOption)
              .match(tab + " " + rightRole)
              .hasMatch())
        continue;
      int left = findClass(j, "team-participant-card__member-role-left");
      if (left >= 0)
        for (int k = left + 1; k < nodes[left].end; ++k)
          if (nodes[k].tag == "img") {
            role = nodes[k].attrs.value("alt");
            break;
          }
      if (!QRegularExpression("^(dps|damage|tank|support|flex)$",
                              QRegularExpression::CaseInsensitiveOption)
               .match(role)
               .hasMatch() &&
          !QRegularExpression("^(main|players|substitutes?|subs|reserves?|bench)$",
                              QRegularExpression::CaseInsensitiveOption)
               .match(tab)
               .hasMatch())
        continue;
      int name = findClass(j, "team-participant-card__member-name");
      if (name < 0)
        continue;
      int label = findClass(name, "name");
      if (label < 0)
        continue;
      for (int k = label + 1; k < nodes[label].end; ++k) {
        if (nodes[k].tag != "a")
          continue;
        auto names = parseRoster(nodes[k].text);
        if (names.size() == 1 &&
            !QRegularExpression("^(TBD|TBA|unknown|[? -]+)$",
                                QRegularExpression::CaseInsensitiveOption)
                 .match(names.front())
                 .hasMatch() &&
            !result.names.contains(names.front(), Qt::CaseInsensitive))
          result.names << names.front();
        break;
      }
    }
    if (result.names.size() == before)
      result.warnings << QStringLiteral("有参赛卡片未提供可识别的选手名单");
  }
  result.warnings.removeDuplicates();
  if (result.names.isEmpty())
    result.warnings << QStringLiteral("未找到赛事参赛选手，请选具体赛事页面，或手动添加");
  return result;
}
} // namespace owcs
