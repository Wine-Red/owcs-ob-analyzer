#include "recognizer.hpp"
#include <QFile>
#include <QJsonDocument>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>

namespace owcs {
static int rnd(double x) { return int(std::nearbyint(x)); }
static cv::Mat resize(const cv::Mat &im, int w, int h, int mode = cv::INTER_LINEAR) {
  cv::Mat out;
  cv::resize(im, out, {w, h}, 0, 0, mode);
  return out;
}
static cv::Mat gray(const cv::Mat &im) {
  if (im.channels() == 1)
    return im;
  cv::Mat out;
  cv::cvtColor(im, out, cv::COLOR_BGR2GRAY);
  return out;
}
static cv::Mat normalized(cv::Mat m) {
  m.convertTo(m, CV_32F);
  m = m.reshape(1, 1).clone();
  m -= cv::mean(m)[0];
  double n = cv::norm(m);
  if (n > 1e-6)
    m /= n;
  else
    m.setTo(0);
  return m;
}
static cv::Mat hudFeature(const cv::Mat &patch, bool coarse) {
  auto g = gray(resize(patch, 64, 48, patch.cols > 64 ? cv::INTER_AREA : cv::INTER_CUBIC));
  cv::GaussianBlur(g, g, {3, 3}, 0);
  cv::Mat dx, dy, mag;
  cv::Sobel(g, dx, CV_32F, 1, 0);
  cv::Sobel(g, dy, CV_32F, 0, 1);
  cv::magnitude(dx, dy, mag);
  if (coarse)
    mag = resize(mag, 16, 12, cv::INTER_AREA);
  return normalized(mag);
}
static double correlation(const cv::Mat &a, const cv::Mat &b) {
  cv::Mat out;
  cv::matchTemplate(a, b, out, cv::TM_CCOEFF_NORMED);
  return out.at<float>(0);
}
cv::Mat readImage(const QString &path, int flags) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    throw std::runtime_error(("Missing image: " + path).toStdString());
  auto data = f.readAll();
  return cv::imdecode(std::vector<unsigned char>(data.begin(), data.end()), flags);
}
double Panel::cy(int row) const {
  return top + (54 + 55 * (row % 5) + (row >= 5 ? 339 : 0)) * scale;
}
cv::Mat Panel::crop(const cv::Mat &im, int row, double x1, double y1, double x2, double y2,
                    bool r) const {
  int x = r ? right : left, a = rnd(x + x1 * scale), b = rnd(x + x2 * scale),
      c = rnd(cy(row) + y1 * scale), d = rnd(cy(row) + y2 * scale);
  if (a < 0 || c < 0 || b > im.cols || d > im.rows || b <= a || d <= c)
    return {};
  return im(cv::Rect(a, c, b - a, d - c));
}
Recognizer::Recognizer(const QString &dir) {
  QFile file(dir + "/manifest.json");
  if (!file.open(QIODevice::ReadOnly))
    throw std::runtime_error("Missing recognition manifest");
  auto m = QJsonDocument::fromJson(file.readAll()).object();
  if (m["asset_format"].toInt() != 2)
    throw std::runtime_error(
        "Recognition pack format 2 required; install a matching complete resource pack");
  resourceVersion_ = m["resource_version"].toString();
  if (resourceVersion_.isEmpty())
    throw std::runtime_error("Recognition pack has no resource version");
  for (auto n : m["portrait_names"].toArray())
    portraitNames_ << n.toString();
  for (auto n : m["hud_names"].toArray())
    hudNames_ << n.toString();
  perkNames_ = m["perks"].toArray();
  QFile templates(dir + "/templates.qz");
  if (!templates.open(QIODevice::ReadOnly) || templates.size() > 64000000)
    throw std::runtime_error("Missing or oversized template bundle");
  auto packed = templates.readAll();
  if (packed.size() < 4)
    throw std::runtime_error("Invalid template bundle");
  const auto *prefix = reinterpret_cast<const uchar *>(packed.constData());
  quint32 length = (quint32(prefix[0]) << 24) | (quint32(prefix[1]) << 16) |
                   (quint32(prefix[2]) << 8) | prefix[3];
  if (length > 128000000)
    throw std::runtime_error("Oversized template data");
  auto bytes = qUncompress(packed);
  qsizetype at = 8;
  if (!bytes.startsWith("OWCSTM01"))
    throw std::runtime_error("Invalid template format");
  for (auto matrix : {&digits_, &digitLabels_, &portraits_, &hud_, &hudIndices_, &perks_}) {
    if (at + 12 > bytes.size())
      throw std::runtime_error("Truncated template header");
    quint32 header[3];
    memcpy(header, bytes.constData() + at, 12);
    at += 12;
    quint64 size = quint64(header[0]) * header[1] * 4;
    if (header[0] > 10000 || header[1] > 10000 || header[2] > 1 ||
        size > quint64(bytes.size() - at))
      throw std::runtime_error("Invalid template dimensions");
    *matrix =
        cv::Mat(int(header[0]), int(header[1]), header[2] ? CV_32S : CV_32F, bytes.data() + at)
            .clone();
    at += qsizetype(size);
  }
  if (at != bytes.size() || digits_.cols != 384 || digits_.type() != CV_32F ||
      digitLabels_.type() != CV_32S || digitLabels_.rows != digits_.rows ||
      digitLabels_.cols != 1 || portraits_.type() != CV_32F || portraits_.cols != 1055 ||
      portraits_.rows != portraitNames_.size() || hud_.type() != CV_32F || hud_.cols != 3072 ||
      hudIndices_.type() != CV_32S || hudIndices_.rows != hud_.rows || hudIndices_.cols != 1 ||
      perks_.type() != CV_32F || perks_.cols != 400)
    throw std::runtime_error("Recognition pack matrices do not match their metadata");
  for (int i = 0; i < digitLabels_.rows; ++i)
    if (digitLabels_.at<int>(i) < 0 || digitLabels_.at<int>(i) > 9)
      throw std::runtime_error("Invalid digit label in recognition pack");
  for (int i = 0; i < hudIndices_.rows; ++i)
    if (hudIndices_.at<int>(i) < 0 || hudIndices_.at<int>(i) >= hudNames_.size())
      throw std::runtime_error("Invalid HUD hero index in recognition pack");
  ready_ = readImage(dir + "/ready.png", cv::IMREAD_GRAYSCALE);
  auto placeholder = readImage(dir + "/unselected-hud.png");
  unselectedHud_ = gray(placeholder);
  unselectedPanel_ =
      gray(resize(readImage(dir + "/unselected-panel.png"), 54, 54))(cv::Rect(4, 4, 46, 46))
          .clone();
  QFile fullFile(dir + "/portraits-full.qz");
  if (!fullFile.open(QIODevice::ReadOnly) || fullFile.size() > 4000000)
    throw std::runtime_error("Missing full portrait templates");
  auto fullPacked = fullFile.readAll();
  if (fullPacked.size() < 4 || uchar(fullPacked[0]) != 0 || uchar(fullPacked[1]) > 64)
    throw std::runtime_error("Oversized full portrait templates");
  auto full = qUncompress(fullPacked);
  quint32 dimensions[2]{};
  if (!full.startsWith("OWCSFP01") || full.size() < 16)
    throw std::runtime_error("Invalid full portrait templates");
  memcpy(dimensions, full.constData() + 8, 8);
  if (dimensions[0] != quint32(portraits_.rows) || dimensions[1] != 42 * 42 ||
      full.size() != 16 + quint64(dimensions[0]) * dimensions[1] * 4)
    throw std::runtime_error("Invalid full portrait dimensions");
  fullPortraits_ =
      cv::Mat(int(dimensions[0]), int(dimensions[1]), CV_32F, full.data() + 16).clone();
  int column = 0;
  for (int y = 3; y < 45; ++y)
    for (int x = 3; x < 45; ++x) {
      int distance = (x - 24) * (x - 24) + (y - 24) * (y - 24);
      if (distance <= 15 * 15)
        continue;
      if (distance > 18 * 18)
        deathColumns_.push_back(column);
      ++column;
    }
  deathPortraits_.create(portraits_.rows, int(deathColumns_.size()), CV_32F);
  for (int i = 0; i < portraits_.rows; ++i) {
    for (int j = 0; j < int(deathColumns_.size()); ++j)
      deathPortraits_.at<float>(i, j) = portraits_.at<float>(i, deathColumns_[j]);
    normalized(deathPortraits_.row(i)).copyTo(deathPortraits_.row(i));
  }
  if (digits_.empty() || hud_.empty() || portraits_.rows != portraitNames_.size() ||
      perks_.rows != perkNames_.size())
    throw std::runtime_error("Invalid recognition assets");
  hudCoarse_.create(hud_.rows, 16 * 12, CV_32F);
  for (int i = 0; i < hud_.rows; ++i)
    normalized(resize(hud_.row(i).reshape(1, 48), 16, 12, cv::INTER_AREA))
        .copyTo(hudCoarse_.row(i));
}
cv::Mat Recognizer::textMask(const cv::Mat &im) {
  cv::Mat out(im.rows, im.cols, CV_8U, cv::Scalar(0));
  for (int y = 0; y < im.rows; y++)
    for (int x = 0; x < im.cols; x++) {
      auto p = im.at<cv::Vec3b>(y, x);
      int lo = std::min({p[0], p[1], p[2]}), hi = std::max({p[0], p[1], p[2]});
      out.at<uchar>(y, x) = (hi - lo < 48 && lo > 105) ? 255 : 0;
    }
  return out;
}
Number Recognizer::number(const cv::Mat &image, int maximum) const {
  if (image.empty())
    return {};
  auto mask = textMask(image);
  auto result = numberMask(mask, maximum);
  if (result.value)
    return result;
  // Anti-aliased edges sometimes connect neighboring glyphs. Accept a retry
  // only when two stricter masks independently agree, retaining unknown otherwise.
  Number retry;
  int agreements = 0;
  for (int threshold : {135, 155, 175}) {
    auto strict = mask.clone();
    for (int y = 0; y < image.rows; ++y)
      for (int x = 0; x < image.cols; ++x) {
        auto pixel = image.at<cv::Vec3b>(y, x);
        if (std::min({pixel[0], pixel[1], pixel[2]}) <= threshold)
          strict.at<uchar>(y, x) = 0;
      }
    auto candidate = numberMask(strict, maximum);
    if (!candidate.value)
      continue;
    if (retry.value && candidate.value != retry.value)
      return result;
    ++agreements;
    retry = {candidate.value,
             retry.value ? std::min(retry.confidence, candidate.confidence) : candidate.confidence};
  }
  return agreements >= 2 ? retry : result;
}
Number Recognizer::numberMask(const cv::Mat &mask, int maximum) const {
  cv::Mat labels, stats, centroids;
  int count = cv::connectedComponentsWithStats(mask, labels, stats, centroids);
  std::vector<cv::Rect> boxes;
  std::vector<int> heights;
  int typical = 0;
  int height = 0;
  for (int i = 1; i < count; i++)
    if (stats.at<int>(i, 4) >= 3 && stats.at<int>(i, 3) >= 5) {
      auto b = cv::Rect(stats.at<int>(i, 0), stats.at<int>(i, 1), stats.at<int>(i, 2),
                        stats.at<int>(i, 3));
      boxes.push_back(b);
      heights.push_back(b.height);
      height = std::max(height, b.height);
    }
  std::sort(boxes.begin(), boxes.end(), [](auto a, auto b) { return a.x < b.x; });
  // A thousands separator can touch a digit and extend below the baseline.
  // Use the typical digit height, not the tallest connected component.
  if (!heights.empty()) {
    std::sort(heights.begin(), heights.end());
    typical = heights[(heights.size() - 1) / 2];
    cv::Mat rowInk;
    cv::reduce(mask, rowInk, 1, cv::REDUCE_SUM, CV_32S);
    double peak = 0;
    cv::minMaxLoc(rowInk, nullptr, &peak);
    int first = -1, last = -1;
    for (int y = 0; y < mask.rows; ++y)
      if (rowInk.at<int>(y) >= peak * .25 && rowInk.at<int>(y) > 0) {
        if (first < 0)
          first = y;
        last = y;
      }
    if (last - first + 1 >= 5)
      typical = std::min(typical, last - first + 1);
    for (auto &b : boxes) {
      if (b.height <= typical || b.width <= typical * .85)
        continue;
      // A connected comma lives below the digit's main body. Trim only
      // columns absent from that body, then measure the remaining digit again.
      auto part = mask(b);
      std::vector<cv::Point> body;
      cv::findNonZero(part.rowRange(0, std::max(1, rnd(typical * .65))), body);
      if (body.empty())
        continue;
      auto core = cv::boundingRect(body);
      std::vector<cv::Point> ink;
      cv::findNonZero(part.colRange(core.x, core.x + core.width), ink);
      auto trimmed = cv::boundingRect(ink);
      b = cv::Rect(b.x + core.x + trimmed.x, b.y + trimmed.y, trimmed.width, trimmed.height);
    }
    // All digits may be joined by GPU antialiasing. Their comma descender
    // must not inflate the inferred character width and reduce the digit count.
    height = typical;
  }
  std::vector<cv::Mat> pieces;
  for (auto b : boxes) {
    if (b.height < height * .65)
      continue;
    auto part = mask(b);
    if (b.width <= b.height * 1.05) {
      pieces.push_back(part);
      continue;
    }
    int n = std::max(2, rnd(b.width / (height * .8)));
    std::vector<int> cuts{0};
    cv::Mat sums;
    cv::reduce(part, sums, 0, cv::REDUCE_SUM, CV_32S);
    for (int k = 1; k < n; k++) {
      int t = rnd(double(b.width) * k / n), a = std::max(cuts.back() + 2, t - 2),
          z = std::min(b.width - 2, t + 2);
      if (a > z)
        continue;
      int best = a;
      for (int j = a; j <= z; j++)
        if (sums.at<int>(j) < sums.at<int>(best) ||
            (sums.at<int>(j) == sums.at<int>(best) && std::abs(j - t) < std::abs(best - t)))
          best = j;
      cuts.push_back(best);
    }
    cuts.push_back(b.width);
    for (size_t i = 1; i < cuts.size(); i++) {
      auto sub = part.colRange(cuts[i - 1], cuts[i]);
      std::vector<cv::Point> points;
      cv::findNonZero(sub, points);
      if (points.empty())
        continue;
      auto box = cv::boundingRect(points);
      if (box.height >= height * .65)
        pieces.push_back(sub(box));
    }
  }
  if (pieces.empty() || pieces.size() > size_t(QString::number(maximum).size()))
    return {};
  int value = 0;
  double confidence = 1;
  for (auto piece : pieces) {
    // A comma may connect two digits, so remove its remaining tail after
    // splitting as well as before. OBS GPU sampling can create this bridge.
    if (piece.rows > typical && piece.cols > typical * .85) {
      std::vector<cv::Point> body;
      cv::findNonZero(piece.rowRange(0, std::max(1, rnd(typical * .65))), body);
      if (!body.empty()) {
        auto core = cv::boundingRect(body);
        piece = piece.colRange(core.x, core.x + core.width);
        std::vector<cv::Point> ink;
        cv::findNonZero(piece, ink);
        piece = piece(cv::boundingRect(ink));
      }
    }
    auto p = resize(piece, 16, 24, cv::INTER_AREA);
    p.convertTo(p, CV_32F, 1. / 255);
    p = p.reshape(1, 1);
    std::array<double, 10> scores;
    scores.fill(-1);
    for (int i = 0; i < digits_.rows; i++) {
      double score = 1 - cv::norm(digits_.row(i), p, cv::NORM_L2SQR) / 384.;
      int label = digitLabels_.at<int>(i);
      scores[label] = std::max(scores[label], score);
    }
    int best = int(std::max_element(scores.begin(), scores.end()) - scores.begin());
    double score = scores[best];
    scores[best] = -1;
    double runner = *std::max_element(scores.begin(), scores.end());
    if (score < .84 || score - runner < .018)
      return {std::nullopt, score};
    value = value * 10 + best;
    confidence = std::min(confidence, score);
  }
  return value <= maximum ? Number{value, confidence} : Number{};
}
std::optional<Panel> Recognizer::locate(const cv::Mat &im) const {
  cv::Mat hsv, mask, closed;
  cv::cvtColor(im, hsv, cv::COLOR_BGR2HSV);
  cv::inRange(hsv, cv::Scalar(0, 0, 170), cv::Scalar(180, 45, 240), mask);
  cv::morphologyEx(mask, closed, cv::MORPH_CLOSE, cv::Mat::ones(3, 25, CV_8U));
  // Light portraits can touch the header. Keep long horizontal bands before
  // finding contours so the portrait does not enlarge the header's bounds.
  cv::morphologyEx(closed, closed, cv::MORPH_OPEN, cv::Mat::ones(1, 301, CV_8U));
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(closed, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
  std::optional<Panel> result;
  double distance = 1e9;
  for (auto &c : contours) {
    auto b = cv::boundingRect(c);
    if (b.width <= 480 || b.width >= 1100 || b.height < 15 || b.height > 50 ||
        std::abs(b.x + b.width / 2. - 800) >= 120)
      continue;
    int band = 0;
    for (int y = b.y; y < b.y + b.height; y++) {
      bool present =
          cv::countNonZero(mask(cv::Rect(b.x + 5, y, b.width - 10, 1))) > .65 * (b.width - 10);
      if (!present && band)
        break;
      if (present)
        band++;
    }
    double scale = band >= 24 && band <= 28 ? 1 : band / 26.;
    if (scale <= .7 || scale >= 1.4 || b.y + 639 * scale > im.rows)
      continue;
    Panel p{b.x, b.y, b.x + b.width, scale};
    bool valid = true;
    for (int base : {0, 5}) {
      std::array<cv::Vec3d, 5> colors;
      for (int i = 0; i < 5; i++) {
        auto patch = p.crop(im, base + i, -280, -5, -265, 5, true);
        if (patch.empty()) {
          valid = false;
          break;
        }
        std::vector<cv::Mat> channels;
        cv::split(patch, channels);
        for (int ch = 0; ch < 3; ch++) {
          std::vector<uchar> v(channels[ch].begin<uchar>(), channels[ch].end<uchar>());
          std::sort(v.begin(), v.end());
          colors[i][ch] = (v[(v.size() - 1) / 2] + v[v.size() / 2]) / 2.;
        }
      }
      if (!valid)
        break;
      for (int ch = 0; ch < 3; ch++) {
        double mean = 0, var = 0;
        for (auto v : colors)
          mean += v[ch] / 5.;
        for (auto v : colors)
          var += (v[ch] - mean) * (v[ch] - mean) / 5.;
        if (std::sqrt(var) > 38)
          valid = false;
      }
    }
    double d = std::abs((p.left + p.right) / 2. - 800);
    if (valid && d < distance) {
      distance = d;
      result = p;
    }
  }
  return result;
}
static cv::Mat portraitFeatures(const cv::Mat &im) {
  auto g = gray(resize(im, 48, 48));
  cv::Mat mask(48, 48, CV_8U, cv::Scalar(1));
  cv::circle(mask, {24, 24}, 15, cv::Scalar(0), -1);
  mask.rowRange(0, 3).setTo(0);
  mask.rowRange(45, 48).setTo(0);
  mask.colRange(0, 3).setTo(0);
  mask.colRange(45, 48).setTo(0);
  cv::Mat out(1, cv::countNonZero(mask), CV_32F);
  int i = 0;
  for (int y = 0; y < 48; y++)
    for (int x = 0; x < 48; x++)
      if (mask.at<uchar>(y, x))
        out.at<float>(i++) = g.at<uchar>(y, x);
  return normalized(out);
}
static Match choose(const cv::Mat &templates, const cv::Mat &f, const QStringList &names,
                    double minimum, double margin) {
  cv::Mat scores = templates * f.t();
  std::map<QString, double> by;
  for (int i = 0; i < scores.rows; i++) {
    double score = scores.at<float>(i);
    auto it = by.find(names[i]);
    if (it == by.end() || score > it->second)
      by[names[i]] = score;
  }
  QString name;
  double best = -1, second = -1;
  for (auto &[n, s] : by) {
    if (s > best) {
      second = best;
      best = s;
      name = n;
    } else
      second = std::max(second, s);
  }
  return {best >= minimum && best - second >= margin ? name : QString(), std::clamp(best, 0., 1.)};
}
Match Recognizer::portrait(const cv::Mat &im, bool deathOverlay) const {
  if (im.empty())
    return {};
  auto features = portraitFeatures(im);
  if (deathOverlay) {
    cv::Mat edge(1, int(deathColumns_.size()), CV_32F);
    for (int j = 0; j < edge.cols; ++j)
      edge.at<float>(j) = features.at<float>(deathColumns_[j]);
    auto match = choose(deathPortraits_, normalized(edge), portraitNames_, .60, .15);
    if (!match.id.isEmpty())
      return match;
  } else {
    auto full = gray(resize(im, 48, 48))(cv::Rect(3, 3, 42, 42)).clone();
    auto match = choose(fullPortraits_, normalized(full), portraitNames_, .72, .12);
    if (!match.id.isEmpty())
      return match;
  }
  return choose(portraits_, features, portraitNames_, .60, .12);
}
static cv::Mat perkFeatures(cv::Mat im) {
  im = gray(im);
  cv::Mat mask;
  cv::threshold(im, mask, 200, 255, cv::THRESH_BINARY);
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
  if (!contours.empty()) {
    auto it = std::max_element(contours.begin(), contours.end(), [](auto &a, auto &b) {
      return cv::contourArea(a) < cv::contourArea(b);
    });
    auto b = cv::boundingRect(*it);
    if (b.width > im.cols * .4 && b.height > im.rows * .4)
      im = im(b);
  }
  return normalized(resize(im, 32, 32)(cv::Rect(6, 6, 20, 20)));
}
QJsonObject Recognizer::perk(const cv::Mat &im, const QString &hero, const QString &tier) const {
  if (im.empty())
    return {{"id", QJsonValue::Null}, {"selection", "unknown"}, {"confidence", 0}};
  auto g = gray(resize(im, 40, 40));
  int total = 0, white = 0;
  for (int y = 0; y < 40; y++)
    for (int x = 0; x < 40; x++)
      if ((x - 20) * (x - 20) + (y - 20) * (y - 20) < 256) {
        total++;
        if (g.at<uchar>(y, x) > 195)
          white++;
      }
  if (white < double(total) * .4)
    return {{"id", QJsonValue::Null}, {"selection", "unselected"}, {"confidence", .9}};
  auto f = perkFeatures(im);
  QString id;
  double best = -1, second = -1;
  std::map<QString, double> byId;
  for (int i = 0; i < perks_.rows; i++) {
    auto n = perkNames_[i].toObject();
    if (n["hero"].toString().replace('_', '-') != QString(hero).replace('_', '-') ||
        n["tier"].toString() != tier)
      continue;
    double score = perks_.row(i).dot(f);
    auto key = n["id"].toString();
    auto entry = byId.find(key);
    if (entry == byId.end() || score > entry->second)
      byId[key] = score;
  }
  for (auto &[key, score] : byId) {
    if (score > best) {
      second = best;
      best = score;
      id = key;
    } else
      second = std::max(second, score);
  }
  bool accepted = best > .65 && best - std::max(second, 0.) > .12;
  return {{"id", accepted ? QJsonValue(id) : QJsonValue(QJsonValue::Null)},
          {"selection", accepted ? "selected" : "unknown"},
          {"confidence", std::clamp(best, 0., 1.)}};
}
double Recognizer::hudQuality(const cv::Mat &im, int offset, bool coarse) const {
  const std::array<double, 10> xs{.047, .102, .157, .212, .268, .730, .785, .841, .896, .951};
  const std::array<double, 10> xe{.074, .128, .183, .238, .295, .757, .812, .868, .923, .978};
  const int y1 = rnd(.0674 * std::min(im.rows, 900)), y2 = rnd(.1029 * std::min(im.rows, 900));
  if (y1 + offset < 0 || y2 + offset > im.rows)
    return -1;
  const auto &templates = coarse ? hudCoarse_ : hud_;
  cv::Mat features(10, templates.cols, CV_32F);
  for (int slot = 0; slot < 10; ++slot) {
    auto patch = im(cv::Rect(rnd(xs[slot] * 1600), y1 + offset,
                             rnd(xe[slot] * 1600) - rnd(xs[slot] * 1600), y2 - y1));
    hudFeature(patch, coarse).copyTo(features.row(slot));
  }
  // Match all ten slots together so the template matrix is read once per candidate.
  cv::Mat scores = templates * features.t();
  int accepted = 0;
  std::array<int, 2> acceptedTeams{};
  double sum = 0;
  for (int slot = 0; slot < 10; ++slot) {
    std::vector<float> heroes(hudNames_.size(), -1.f);
    for (int i = 0; i < scores.rows; ++i) {
      auto &value = heroes[hudIndices_.at<int>(i)];
      value = std::max(value, scores.at<float>(i, slot));
    }
    auto best = std::max_element(heroes.begin(), heroes.end());
    float score = *best;
    *best = -1.f;
    float runner = *std::max_element(heroes.begin(), heroes.end());
    int left = std::max(0, rnd(xs[slot] * 1600) - 12);
    int right = std::min(im.cols, rnd(xe[slot] * 1600) + 12);
    int top = std::max(0, y1 + offset - 3), bottom = std::min(im.rows, y2 + offset + 3);
    double emptyScore = 0;
    double emptyQuality = 0;
    if (right - left >= unselectedHud_.cols && bottom - top >= unselectedHud_.rows) {
      cv::Mat matches;
      cv::matchTemplate(gray(im(cv::Rect(left, top, right - left, bottom - top))), unselectedHud_,
                        matches, cv::TM_CCOEFF_NORMED);
      cv::Point location;
      cv::minMaxLoc(matches, nullptr, &emptyScore, nullptr, &location);
      emptyQuality = emptyScore - .003 * std::abs(location.y - (y1 + offset - top));
    }
    bool empty = emptyScore >= .90 && emptyScore - score >= .08;
    bool matched =
        empty || (score >= (coarse ? .45 : .50) && score - runner >= (coarse ? .10 : .20));
    accepted += matched;
    acceptedTeams[slot / 5] += matched;
    sum += empty ? emptyQuality : score;
  }
  if (accepted >= 6 && acceptedTeams[0] >= 2 && acceptedTeams[1] >= 2 && sum / 10 >= .50)
    return sum / 10;
  // Death effects and newer portraits may fail hero matching. Repeated team
  // nameplates plus confident portraits on BOTH teams can still identify the HUD.
  // This authorizes opening the panel; it does not invent the unknown heroes.
  if (acceptedTeams[0] < 2 || acceptedTeams[1] < 2 || y2 + offset + 18 > im.rows)
    return -1;
  std::array<int, 2> plates{};
  std::array<std::vector<cv::Vec3d>, 2> plateColors;
  for (int slot = 0; slot < 10; ++slot) {
    int left = rnd((xs[slot] - .023) * 1600) + 2, right = rnd(xe[slot] * 1600) - 2;
    auto patch = im(cv::Rect(left, y2 + offset + 5, right - left, 13));
    cv::Vec3d color;
    for (int ch = 0; ch < 3; ++ch) {
      std::vector<uchar> values;
      for (int y = 0; y < patch.rows; ++y)
        for (int x = 0; x < patch.cols; ++x)
          values.push_back(patch.at<cv::Vec3b>(y, x)[ch]);
      std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
      color[ch] = values[values.size() / 2];
    }
    int uniform = 0;
    for (int y = 0; y < patch.rows; ++y)
      for (int x = 0; x < patch.cols; ++x) {
        auto pixel = patch.at<cv::Vec3b>(y, x);
        uniform += cv::norm(cv::Vec3d(pixel) - color) <= 45;
      }
    if (uniform >= patch.total() * .55) {
      ++plates[slot / 5];
      plateColors[slot / 5].push_back(color);
    }
  }
  for (int team = 0; team < 2; ++team) {
    plates[team] = 0;
    for (auto center : plateColors[team]) {
      int consistent = 0;
      for (auto c : plateColors[team])
        consistent += cv::norm(c - center) <= 45;
      plates[team] = std::max(plates[team], consistent);
    }
  }
  return plates[0] >= 3 && plates[1] >= 3 && plates[0] + plates[1] >= 8 ? sum / 10 : -1.;
}
int Recognizer::locateHud(const cv::Mat &im) {
  using Clock = std::chrono::steady_clock;
  auto start = Clock::now();
  int candidate = -10000;
  if (hudCandidate_ != -10000 && hudQuality(im, hudCandidate_, false) >= 0)
    candidate = hudCandidate_;
  else if (hudQuality(im, 0, false) >= 0)
    candidate = 0;
  else {
    if (searchOffsets_.empty() || searchIndex_ >= searchOffsets_.size() ||
        searchHeight_ != im.rows) {
      searchOffsets_.clear();
      searchIndex_ = 0;
      searchHeight_ = im.rows;
      const std::array<double, 10> xs{.047, .102, .157, .212, .268, .730, .785, .841, .896, .951};
      const std::array<double, 10> xe{.074, .128, .183, .238, .295, .757, .812, .868, .923, .978};
      double lh = std::min(im.rows, 900);
      int y1 = rnd(.0674 * lh), y2 = rnd(.1029 * lh);
      auto g = gray(im);
      cv::Mat edge;
      cv::Sobel(g, edge, CV_32F, 0, 1);
      edge = cv::abs(edge);
      std::vector<std::pair<double, int>> scores;
      for (int dy = -y1; dy <= im.rows - y2; dy += 3) {
        double score = 0;
        for (int i = 0; i < 10; ++i)
          for (int y : {y1 + dy, y2 + dy, rnd(.1237 * lh) + dy})
            if (y > 2 && y + 2 < im.rows)
              score += cv::mean(
                  edge(cv::Rect(rnd(xs[i] * 1600), y - 1, rnd((xe[i] - xs[i]) * 1600), 3)))[0];
        scores.push_back({score, dy});
      }
      std::sort(scores.rbegin(), scores.rend());
      std::vector<int> peaks;
      for (auto [score, dy] : scores) {
        if (score < 300)
          break;
        if (std::any_of(peaks.begin(), peaks.end(), [&](int v) { return std::abs(v - dy) < 6; }))
          continue;
        peaks.push_back(dy);
        for (int d = -8; d <= 8; ++d)
          if (dy + d >= -y1 && dy + d <= im.rows - y2 &&
              std::find(searchOffsets_.begin(), searchOffsets_.end(), dy + d) ==
                  searchOffsets_.end())
            searchOffsets_.push_back(dy + d);
        if (peaks.size() >= 10)
          break;
      }
    }
    std::vector<std::pair<double, int>> candidates;
    // Spread failed searches across frames instead of blocking the worker for seconds.
    while (searchIndex_ < searchOffsets_.size()) {
      int offset = searchOffsets_[searchIndex_++];
      double quality = hudQuality(im, offset, true);
      if (quality >= 0)
        candidates.push_back({quality, offset});
      if (Clock::now() - start > std::chrono::milliseconds(100))
        break;
    }
    std::sort(candidates.rbegin(), candidates.rend());
    double best = -1;
    for (size_t i = 0; i < std::min<size_t>(candidates.size(), 3); ++i) {
      // Coarse features tolerate a few pixels of error. Refine the whole local
      // neighborhood before locking, including when the search slice ends early.
      for (int dy = candidates[i].second - 5; dy <= candidates[i].second + 5; ++dy) {
        double quality = hudQuality(im, dy, false);
        if (quality > best) {
          best = quality;
          candidate = dy;
        }
      }
      if (candidate != -10000 || Clock::now() - start > std::chrono::milliseconds(180))
        break;
    }
  }
  if (candidate == -10000) {
    hudCandidate_ = -10000;
    hudCount_ = 0;
    return candidate;
  }
  searchOffsets_.clear();
  searchIndex_ = 0;
  hudCount_ = candidate == hudCandidate_ ? hudCount_ + 1 : 1;
  hudCandidate_ = candidate;
  return hudCount_ >= 3 ? candidate : -10000;
}
cv::Mat Recognizer::signature(const cv::Mat &im) {
  if (im.empty())
    return {};
  auto mask = textMask(im);
  if (cv::countNonZero(mask) < 10)
    return {};
  return normalized(resize(mask, 96, 24));
}
Observation Recognizer::detect(const cv::Mat &frame) {
  Observation result;
  if (frame.empty() || frame.cols < 320 || frame.rows < 200)
    return result;
  auto im = resize(frame, 1600, rnd(frame.rows * 1600. / frame.cols));
  auto p = locate(im);
  if (!p) {
    result.hudOffset = locateHud(im);
    return result;
  }
  result.visible = true;
  result.bounds = {p->left, p->top, p->right, rnd(p->top + 639 * p->scale)};
  double width = (p->right - p->left) / p->scale;
  int perkLayout = width > 740 ? 2 : (width > 700 ? 1 : 0), evidenceRows = 0;
  std::array<int, 2> evidenceTeams{};
  const std::array<const char *, 6> statNames{"eliminations", "assists", "deaths",
                                              "damage",       "healing", "mitigated"};
  const std::array<int, 6> offsets{-376, -334, -291, -225, -149, -72};
  for (int row = 0; row < 10; row++) {
    auto crop = [&](double a, double b, double c, double d) {
      return p->crop(im, row, a, b, c, d);
    };
    auto face = crop(23, -27, 77, 27);
    cv::Mat placeholderMatches;
    cv::matchTemplate(gray(resize(face, 54, 54)), unselectedPanel_, placeholderMatches,
                      cv::TM_CCOEFF_NORMED);
    double placeholderScore = 0;
    cv::minMaxLoc(placeholderMatches, nullptr, &placeholderScore);
    bool unselected = placeholderScore >= .90;
    double readyScore = correlation(gray(resize(crop(84, -13, 110, 13), 26, 26)), ready_);
    auto charge = number(crop(87, -9, 107, 10), 100);
    bool ready = readyScore > .8;
    if (ready)
      charge = {100, readyScore};
    auto respawn = number(crop(42, -9, 59, 10), 30);
    cv::Mat hsv;
    cv::cvtColor(resize(face, 54, 54), hsv, cv::COLOR_BGR2HSV);
    std::array<int, 4> counts{}, totals{};
    for (int y = 0; y < 54; y++)
      for (int x = 0; x < 54; x++) {
        int d = (x - 27) * (x - 27) + (y - 27) * (y - 27);
        if (d <= 144 || d >= 289)
          continue;
        auto v = hsv.at<cv::Vec3b>(y, x);
        bool marked =
            (((v[0] < 10 || v[0] > 170) && v[1] > 110 && v[2] > 95) || (v[1] < 65 && v[2] > 155));
        for (int q : {x < 27 ? 0 : 1, y < 27 ? 2 : 3}) {
          totals[q]++;
          if (marked)
            counts[q]++;
        }
      }
    double ring = 1;
    for (int q = 0; q < 4; q++)
      ring = std::min(ring, double(counts[q]) / std::max(totals[q], 1));
    if (ring > .38 && (!respawn.value || respawn.confidence <= .88)) {
      // Light portraits can join the countdown digit at the normal OCR threshold.
      auto counter = crop(42, -9, 59, 10).clone();
      for (int y = 0; y < counter.rows; ++y)
        for (int x = 0; x < counter.cols; ++x) {
          auto &pixel = counter.at<cv::Vec3b>(y, x);
          if (std::min({pixel[0], pixel[1], pixel[2]}) <= 150)
            pixel = {};
        }
      respawn = number(counter, 30);
    }
    bool dead = !unselected && respawn.value && respawn.confidence > .88 && ring > .38;
    auto hero = unselected ? Match{} : portrait(face, dead);
    QJsonObject stats, confidence;
    int numericColumns = 0;
    for (int i = 0; i < 6; i++) {
      int w = i < 3 ? 17 : 32;
      auto n = number(p->crop(im, row, offsets[i] - w, -11, offsets[i] + w, 12, true));
      stats[statNames[i]] = nullable(n.value);
      confidence[statNames[i]] = n.confidence;
      numericColumns += n.value.has_value();
    }
    QJsonObject perks;
    for (auto tier : {QString("major"), QString("minor")}) {
      bool shown = perkLayout == 2 || (perkLayout == 1 && tier == "minor");
      int x = tier == "major" ? -470 : -422;
      perks[tier] = shown ? perk(p->crop(im, row, x - 20, -20, x + 20, 20, true), hero.id, tier)
                          : QJsonObject{{"id", QJsonValue::Null},
                                        {"selection", "not_shown"},
                                        {"confidence", .95}};
    }
    QString life = dead ? "dead" : (hero.id.isEmpty() ? "unknown" : "alive");
    result.players.append(QJsonObject{
        {"hero", nullable(hero.id)},
        {"hero_selection",
         unselected ? "unselected" : (hero.id.isEmpty() ? "unknown" : "selected")},
        {"selection_confidence", unselected ? placeholderScore : hero.confidence},
        {"hero_confidence", hero.confidence},
        {"life_state", life},
        {"life_confidence", dead ? respawn.confidence : hero.confidence},
        {"ultimate_charge", nullable(charge.value)},
        {"ultimate_ready", charge.value ? QJsonValue(ready) : QJsonValue(QJsonValue::Null)},
        {"ultimate_confidence", charge.confidence},
        {"respawn_seconds", dead ? nullable(respawn.value) : QJsonValue(QJsonValue::Null)},
        {"perks", perks},
        {"stats", stats},
        {"stat_confidence", confidence}});
    result.names[row] = crop(119, -17, 267, 11).clone();
    bool evidence = numericColumns >= 4 && charge.value.has_value();
    evidenceRows += evidence;
    evidenceTeams[row / 5] += evidence;
  }
  result.confirmed = evidenceRows >= 8 && evidenceTeams[0] >= 3 && evidenceTeams[1] >= 3;
  return result;
}
} // namespace owcs
