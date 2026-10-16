#include "recognizer.hpp"
#include "life.hpp"
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
// Candidate-specific masks exclude both background intensity and its contribution
// to the mean/variance. Merely painting the reference background black is not enough.
static ForegroundBank foregroundBank(const cv::Mat &values, const cv::Mat &weights) {
  ForegroundBank bank{cv::Mat::zeros(values.size(), CV_32F), weights.clone(), {}};
  cv::reduce(weights, bank.counts, 1, cv::REDUCE_SUM);
  for (int i = 0; i < values.rows; ++i) {
    float count = bank.counts.at<float>(i);
    if (count < 24)
      continue;
    double mean = values.row(i).dot(weights.row(i)) / count;
    cv::Mat centered = values.row(i) - mean;
    cv::Mat square = centered.mul(centered);
    double norm = std::sqrt(square.dot(weights.row(i)));
    if (norm > 1e-6) {
      centered = centered.mul(weights.row(i)) / norm;
      centered.copyTo(bank.values.row(i));
    }
  }
  return bank;
}
static cv::Mat foregroundScores(const ForegroundBank &bank, const cv::Mat &features) {
  cv::Mat scores(bank.values.rows, features.rows, CV_32F);
  cv::Mat square = features.mul(features);
  // Small batches use SIMD dot products; generic GEMM on the bundled OpenCV
  // is disproportionately expensive during HUD offset refinement.
  for (int j = 0; j < features.rows; ++j) {
    auto feature = features.row(j), squared = square.row(j);
    for (int i = 0; i < scores.rows; ++i) {
      auto weights = bank.weights.row(i);
      double n = bank.counts.at<float>(i), sum = weights.dot(feature);
      double variance = n >= 24 ? weights.dot(squared) - sum * sum / n : 0;
      scores.at<float>(i, j) =
          variance > 1e-9 ? bank.values.row(i).dot(feature) / std::sqrt(variance) : -1;
    }
  }
  return scores;
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
cv::Rect Panel::rect(int row, double x1, double y1, double x2, double y2, bool r,
                     double sourceScale) const {
  int x = r ? right : left, a = rnd((x + x1 * scale) * sourceScale),
      b = rnd((x + x2 * scale) * sourceScale), c = rnd((cy(row) + y1 * scale) * sourceScale),
      d = rnd((cy(row) + y2 * scale) * sourceScale);
  return {a, c, std::max(0, b - a), std::max(0, d - c)};
}
cv::Mat Panel::crop(const cv::Mat &im, int row, double x1, double y1, double x2, double y2, bool r,
                    double sourceScale) const {
  auto area = rect(row, x1, y1, x2, y2, r, sourceScale);
  if (area.empty() || (area & cv::Rect(0, 0, im.cols, im.rows)) != area)
    return {};
  return im(area);
}
Recognizer::Recognizer(const QString &dir) {
  QFile file(dir + "/manifest.json");
  if (!file.open(QIODevice::ReadOnly))
    throw std::runtime_error("Missing recognition manifest");
  auto m = QJsonDocument::fromJson(file.readAll()).object();
  if (m["asset_format"].toInt() != 3)
    throw std::runtime_error(
        "Recognition pack format 3 required; install a matching complete resource pack");
  resourceVersion_ = m["resource_version"].toString();
  if (resourceVersion_.isEmpty())
    throw std::runtime_error("Recognition pack has no resource version");
  for (auto n : m["portrait_names"].toArray())
    portraitNames_ << n.toString();
  for (auto n : m["hud_names"].toArray())
    hudNames_ << n.toString();
  perkNames_ = m["perks"].toArray();
  localizedNames_ = m["names_zhCN"].toObject();
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
      digitLabels_.cols != 1 || portraits_.type() != CV_32F || portraits_.cols != 1764 ||
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
  QFile maskFile(dir + "/foreground.qz");
  if (!maskFile.open(QIODevice::ReadOnly) || maskFile.size() > 64000000)
    throw std::runtime_error("Missing or oversized foreground masks");
  auto maskPacked = maskFile.readAll();
  if (maskPacked.size() < 4 || uchar(maskPacked[0]) > 3)
    throw std::runtime_error("Oversized foreground masks");
  auto maskBytes = qUncompress(maskPacked);
  if (!maskBytes.startsWith("OWCSFG01"))
    throw std::runtime_error("Invalid foreground masks");
  qsizetype maskAt = 8;
  cv::Mat portraitMask, hudMask;
  for (auto pair : {std::make_pair(&portraitMask, &portraits_), std::make_pair(&hudMask, &hud_)}) {
    if (maskAt + 8 > maskBytes.size())
      throw std::runtime_error("Truncated foreground mask header");
    quint32 shape[2];
    memcpy(shape, maskBytes.constData() + maskAt, 8);
    maskAt += 8;
    quint64 size = quint64(shape[0]) * shape[1] * 4;
    if (shape[0] != quint32(pair.second->rows) || shape[1] != quint32(pair.second->cols) ||
        size > quint64(maskBytes.size() - maskAt))
      throw std::runtime_error("Foreground mask dimensions mismatch");
    *pair.first = cv::Mat(int(shape[0]), int(shape[1]), CV_32F, maskBytes.data() + maskAt).clone();
    maskAt += qsizetype(size);
    if (!cv::checkRange(*pair.first, true, nullptr, 0, 1.00001))
      throw std::runtime_error("Invalid foreground mask weights");
  }
  if (maskAt != maskBytes.size())
    throw std::runtime_error("Trailing foreground mask data");
  fullBank_ = foregroundBank(portraits_, portraitMask);
  cv::Mat centers, centerMasks;
  for (int i = 0; i < portraits_.rows; ++i) {
    centers.push_back(
        portraits_.row(i).reshape(1, 42)(cv::Rect(9, 9, 24, 24)).clone().reshape(1, 1));
    centerMasks.push_back(
        portraitMask.row(i).reshape(1, 42)(cv::Rect(9, 9, 24, 24)).clone().reshape(1, 1));
  }
  centerBank_ = foregroundBank(centers, centerMasks);
  hudBank_ = foregroundBank(hud_, hudMask);
  if (digits_.empty() || hud_.empty() || portraits_.rows != portraitNames_.size() ||
      perks_.rows != perkNames_.size())
    throw std::runtime_error("Invalid recognition assets");
  hudCoarse_.create(hud_.rows, 16 * 12, CV_32F);
  cv::Mat coarseMasks(hud_.rows, 16 * 12, CV_32F);
  for (int i = 0; i < hud_.rows; ++i) {
    resize(hud_.row(i).reshape(1, 48), 16, 12, cv::INTER_AREA)
        .reshape(1, 1)
        .copyTo(hudCoarse_.row(i));
    auto small = resize(hudMask.row(i).reshape(1, 48), 16, 12, cv::INTER_AREA);
    small.setTo(0, small < .999f);
    small.reshape(1, 1).copyTo(coarseMasks.row(i));
  }
  hudCoarseBank_ = foregroundBank(hudCoarse_, coarseMasks);
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
  double minimum = 0, maximumLuminance = 0;
  cv::minMaxLoc(gray(image), &minimum, &maximumLuminance);
  if (maximumLuminance - minimum < 20)
    return {}; // a uniform light rectangle is not a string of solid digits
  auto mask = textMask(image);
  auto result = numberMask(mask, maximum);
  if (result.value && result.confidence >= .94)
    return result;
  // A plausible first segmentation can still be wrong (e.g. a bridged 100
  // becomes 800). Check low-confidence readings too, not only rejected ones.
  struct Vote {
    int count = 0;
    double confidence = 0;
  };
  std::map<int, Vote> votes;
  if (result.value)
    votes[*result.value] = {1, result.confidence};
  for (int threshold : {85, 125, 145, 165, 185}) {
    cv::Mat strict = cv::Mat::zeros(image.size(), CV_8U);
    for (int y = 0; y < image.rows; ++y)
      for (int x = 0; x < image.cols; ++x) {
        auto pixel = image.at<cv::Vec3b>(y, x);
        int lo = std::min({pixel[0], pixel[1], pixel[2]});
        int hi = std::max({pixel[0], pixel[1], pixel[2]});
        strict.at<uchar>(y, x) = lo > threshold && hi - lo < 48 ? 255 : 0;
      }
    auto candidate = numberMask(strict, maximum);
    if (!candidate.value)
      continue;
    auto &vote = votes[*candidate.value];
    ++vote.count;
    vote.confidence = std::max(vote.confidence, candidate.confidence);
  }
  if (votes.empty()) {
    // Dim zeroes and antialiased text inherit the team's fill. A fixed neutral-
    // white test loses their strokes; local luminance contrast preserves them.
    auto luminance = gray(image);
    std::vector<uchar> pixels(luminance.datastart, luminance.dataend);
    auto middle = pixels.begin() + pixels.size() / 2;
    std::nth_element(pixels.begin(), middle, pixels.end());
    double background = *middle, peak = 0;
    cv::minMaxLoc(luminance, nullptr, &peak);
    if (peak - background >= 30)
      for (double fraction : {.35, .5, .65}) {
        cv::Mat local;
        cv::threshold(luminance, local, background + fraction * (peak - background), 255,
                      cv::THRESH_BINARY);
        auto candidate = numberMask(local, maximum);
        if (candidate.value) {
          auto &vote = votes[*candidate.value];
          ++vote.count;
          vote.confidence = std::max(vote.confidence, candidate.confidence);
        }
      }
  }
  Number agreed;
  for (auto &[value, vote] : votes) {
    if (vote.count < 2)
      continue;
    bool dominates = true;
    for (auto &[otherValue, other] : votes)
      if (otherValue != value && other.count >= 2 &&
          (vote.count < 3 || vote.count <= other.count || vote.confidence <= other.confidence))
        dominates = false;
    if (dominates)
      agreed = {value, vote.confidence};
  }
  if (!agreed.value && votes.size() > 1)
    return {}; // no candidate dominates the competing stable segmentations
  return agreed.value ? agreed : (votes.size() <= 1 ? result : Number{});
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
    // Tight normalization destroys aspect ratio: a one-pixel vertical stroke
    // expands into a solid block and can falsely score as 8. Such a narrow
    // component is not evidence for a wide digit; let another mask resolve it.
    if (best != 1 && piece.cols < piece.rows * .35)
      return {};
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
  cv::Mat vertical;
  cv::Sobel(im, vertical, CV_32F, 0, 1, 3);
  std::vector<cv::Mat> gradients;
  cv::split(vertical, gradients);
  cv::Mat contrast = cv::abs(gradients[0]);
  for (int channel = 1; channel < 3; ++channel)
    cv::max(contrast, cv::abs(gradients[channel]), contrast);
  // Equal-luminance colors still form a boundary; no particular hue is required.
  cv::Mat boundaries = contrast > 24;
  cv::morphologyEx(boundaries, boundaries, cv::MORPH_CLOSE, cv::Mat::ones(1, 9, CV_8U));
  cv::morphologyEx(boundaries, boundaries, cv::MORPH_OPEN, cv::Mat::ones(1, 401, CV_8U));
  std::vector<std::vector<cv::Point>> edges;
  cv::findContours(boundaries, edges, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
  const size_t appearanceCandidates = contours.size();
  for (auto &a : edges) {
    auto top = cv::boundingRect(a);
    for (auto &b : edges) {
      auto bottom = cv::boundingRect(b);
      int height = bottom.y - top.y;
      if (height < 19 || height > 36 || std::abs(top.x - bottom.x) > 30 ||
          std::abs(top.br().x - bottom.br().x) > 30)
        continue;
      // Row glow/role effects can extend the lower edge. The upper header edge
      // provides horizontal bounds; the lower one only establishes its height.
      int x = top.x + 1, right = top.br().x - 1;
      int y = top.y + top.height / 2 + 1;
      contours.push_back(
          {{x, y}, {right - 1, y}, {right - 1, y + height - 1}, {x, y + height - 1}});
    }
  }
  size_t candidateIndex = 0;
  std::optional<Panel> result;
  double distance = 1e9;
  for (auto &c : contours) {
    bool geometry = candidateIndex++ >= appearanceCandidates;
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
    if (geometry)
      band = b.height;
    double scale = band >= 24 && band <= 28 ? 1 : band / 26.;
    if (scale <= .7 || scale >= 1.4 || b.y + 639 * scale > im.rows)
      continue;
    Panel p{b.x, double(b.y), b.x + b.width, scale};
    // Validate repeated row separators, independent of either team's fill.
    auto g = gray(im);
    std::array<int, 2> separators{};
    for (int team = 0; team < 2; ++team)
      for (int row = 0; row < 4; ++row) {
        int cy = rnd(p.top + (82 + 55 * row + 339 * team) * scale);
        int left = p.left + rnd(100 * scale), right = p.right - rnd(35 * scale);
        double strongest = 0;
        for (int dy = -2; dy <= 2; ++dy) {
          int y = cy + dy;
          if (y < 1 || y + 1 >= g.rows)
            continue;
          cv::Mat difference;
          cv::absdiff(g(cv::Rect(left, y - 1, right - left, 1)),
                      g(cv::Rect(left, y + 1, right - left, 1)), difference);
          strongest =
              std::max(strongest, double(cv::countNonZero(difference > 3)) / (right - left));
        }
        separators[team] += strongest > .4;
      }
    // Header thickness is quantized to only ~26 pixels. A one-pixel error
    // cannot determine the scale of a 639-pixel table. If its inferred scale
    // does not explain every row boundary, fit the repeated separators across
    // both teams instead of letting the vertical crop drift down the table.
    if (separators[0] != 4 || separators[1] != 4) {
      int left = p.left + rnd(100 * scale), right = p.right - rnd(35 * scale);
      std::vector<double> profile(g.rows, 0);
      for (int y = std::max(1, int(std::floor(p.top)));
           y < std::min(g.rows - 1, rnd(p.top + 660 * scale)); ++y) {
        cv::Mat difference;
        cv::absdiff(g(cv::Rect(left, y - 1, right - left, 1)),
                    g(cv::Rect(left, y + 1, right - left, 1)), difference);
        profile[y] = double(cv::countNonZero(difference > 3)) / (right - left);
      }
      auto at = [&](double y) {
        int i = int(std::floor(y));
        return i >= 0 && i + 1 < g.rows ? profile[i] * (1 - (y - i)) + profile[i + 1] * (y - i)
                                        : 0.;
      };
      double best = 0, calibrated = scale, calibratedTop = p.top, proximity = 1e9;
      for (int step = -80; step <= 80; ++step) {
        double candidate = scale + step * .001;
        if (candidate <= .7 || candidate >= 1.4 || p.top + 639 * candidate > im.rows)
          continue;
        for (double origin = p.top - 2.; origin <= p.top + 2.; origin += .25) {
          if (origin < 0 || origin + 639 * candidate > im.rows)
            continue;
          double total = 0;
          int supported = 0;
          for (int team = 0; team < 2; ++team)
            for (int row = 0; row < 4; ++row) {
              double evidence = at(origin + (82 + 55 * row + 339 * team) * candidate);
              total += evidence;
              supported += evidence > .4;
            }
          double near = std::abs(candidate - scale) * 10 + std::abs(origin - p.top);
          if (supported == 8 &&
              (total > best + .0001 || (std::abs(total - best) <= .0001 && near < proximity))) {
            best = total;
            calibrated = candidate;
            calibratedTop = origin;
            proximity = near;
          }
        }
      }
      if (best >= 6.4) {
        // A sharp separator gives a two-pixel plateau in the difference
        // profile. Fit the centers of all eight observed edges, rather than
        // choosing whichever plateau pixel is closest to the coarse header.
        // The +.5 converts pixel-center gradients to rectangle edge coordinates.
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int points = 0;
        for (int team = 0; team < 2; ++team)
          for (int row = 0; row < 4; ++row) {
            double x = 82 + 55 * row + 339 * team;
            int center = rnd(calibratedTop + x * calibrated);
            double weight = 0, position = 0;
            for (int y = center - 2; y <= center + 2; ++y)
              if (y >= 0 && y < int(profile.size()) && profile[y] > .4) {
                weight += profile[y];
                position += profile[y] * (y + .5);
              }
            if (weight == 0)
              continue;
            double y = position / weight;
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
            ++points;
          }
        if (points == 8) {
          double fittedScale = (points * sxy - sx * sy) / (points * sxx - sx * sx);
          double fittedTop = (sy - fittedScale * sx) / points;
          if (fittedScale > .7 && fittedScale < 1.4 && fittedTop >= 0 &&
              fittedTop + 639 * fittedScale <= im.rows) {
            calibrated = fittedScale;
            calibratedTop = fittedTop;
          }
        }
        p.scale = calibrated;
        p.top = calibratedTop;
        separators = {4, 4};
      }
    }
    bool valid = separators[0] == 4 && separators[1] == 4;
    double d = std::abs((p.left + p.right) / 2. - 800) + (geometry ? 1000 : 0);
    if (valid && d < distance) {
      distance = d;
      result = p;
    }
  }
  return result;
}
static Match choose(const cv::Mat &scores, const QStringList &names, double minimum,
                    double margin) {
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
Match Recognizer::portrait(const cv::Mat &im) const {
  if (im.empty())
    return {};
  auto canonical = gray(resize(im, 48, 48));
  auto full = canonical(cv::Rect(3, 3, 42, 42)).clone();
  full.convertTo(full, CV_32F);
  auto features = full.reshape(1, 1);
  auto match = choose(foregroundScores(fullBank_, features), portraitNames_, .72, .12);
  if (!match.id.isEmpty())
    return match;
  // Header/resize rounding can move a small face by one pixel. Search only a
  // bounded translation; retain the foreground mask and distinct-hero margin.
  cv::Mat aligned;
  for (int dy : {-1, 0, 1})
    for (int dx : {-1, 0, 1}) {
      auto patch = canonical(cv::Rect(3 + dx, 3 + dy, 42, 42)).clone();
      patch.convertTo(patch, CV_32F);
      aligned.push_back(patch.reshape(1, 1));
    }
  cv::Mat scores;
  cv::reduce(foregroundScores(fullBank_, aligned), scores, 1, cv::REDUCE_MAX);
  match = choose(scores, portraitNames_, .72, .12);
  return match;
}
double Recognizer::normalPortrait(const cv::Mat &source) const {
  const auto canonical = gray(resize(source, 48, 48));
  cv::Mat full, centers;
  for (int dy : {-1, 0, 1})
    for (int dx : {-1, 0, 1}) {
      cv::Mat patch, center;
      canonical(cv::Rect(3 + dx, 3 + dy, 42, 42)).convertTo(patch, CV_32F);
      canonical(cv::Rect(12 + dx, 12 + dy, 24, 24)).convertTo(center, CV_32F);
      full.push_back(patch.reshape(1, 1));
      centers.push_back(center.reshape(1, 1));
    }
  const auto context = foregroundScores(fullBank_, full);
  const auto badge = foregroundScores(centerBank_, centers);
  double best = 0;
  // Require agreement at the same template AND translation. Independent maxima
  // could explain the face with one hero and the overlay area with another.
  for (int i = 0; i < context.rows; ++i)
    for (int j = 0; j < context.cols; ++j)
      best = std::max(best, double(std::min(context.at<float>(i, j), badge.at<float>(i, j))));
  return best;
}
FaceReading Recognizer::face(const cv::Mat &source) const {
  FaceReading out;
  if (source.empty())
    return out;
  cv::Mat canonical = resize(source, 54, 54), placeholderMatches;
  cv::matchTemplate(gray(canonical), unselectedPanel_, placeholderMatches, cv::TM_CCOEFF_NORMED);
  cv::minMaxLoc(placeholderMatches, nullptr, &out.selectionConfidence);
  out.unselected = out.selectionConfidence >= .90;
  if (out.unselected)
    return out;
  // Evaluate identity and overlay independently. The same complete-portrait
  // matcher is used for every state; no death-specific partial-face fallback.
  out.candidate = portrait(source);
  out.normalConfidence = normalPortrait(source);
  const auto normal = classifyLife({}, out.normalConfidence);
  if (normal.kind == LifeKind::Alive) {
    out.life = "alive";
    out.lifeConfidence = normal.confidence;
    return out;
  }
  auto evidence = lifeEvidence(source);
  out.ring = evidence.ring;
  out.cross = evidence.cross;
  const auto decision = classifyLife(evidence, out.normalConfidence);
  out.lifeConfidence = decision.confidence;
  if (decision.kind == LifeKind::Dead) {
    out.portraitOccluded = true;
    out.life = "dead";
    // The initial cross is not a countdown digit. OCR only a settled ring;
    // reading the timer is optional and never a prerequisite for death.
    if (out.cross < .76 && out.ring >= .80) {
      int x1 = rnd(source.cols * 19. / 54), x2 = rnd(source.cols * 36. / 54);
      int y1 = rnd(source.rows * 18. / 54), y2 = rnd(source.rows * 37. / 54);
      out.respawn = number(source(cv::Rect(x1, y1, x2 - x1, y2 - y1)), 30);
      if (!out.respawn.value && source.cols != 54)
        out.respawn = number(canonical(cv::Rect(19, 18, 17, 19)), 30);
    }
  }
  return out;
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
  cv::Mat scores = foregroundScores(coarse ? hudCoarseBank_ : hudBank_, features);
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
  auto grayImage = gray(im);
  for (int slot = 0; slot < 10; ++slot) {
    int left = rnd((xs[slot] - .023) * 1600) + 2, right = rnd(xe[slot] * 1600) - 2;
    double boundary = 0;
    for (int y = y2 + offset + 2; y < std::min(im.rows - 1, y2 + offset + 22); ++y) {
      cv::Mat difference;
      cv::absdiff(grayImage(cv::Rect(left, y - 1, right - left, 1)),
                  grayImage(cv::Rect(left, y + 1, right - left, 1)), difference);
      boundary = std::max(boundary, double(cv::countNonZero(difference > 8)) / (right - left));
    }
    plates[slot / 5] += boundary >= .55;
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
Observation Recognizer::detect(const cv::Mat &frame, QJsonArray *cropTrace) {
  Observation result;
  if (cropTrace)
    *cropTrace = {};
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
    auto trace = [&](const QString &field, double a, double b, double c, double d, bool right,
                     bool native) {
      if (!cropTrace)
        return;
      bool source = native && frame.cols > im.cols;
      const auto &sample = source ? frame : im;
      auto box = p->rect(row, a, b, c, d, right, source ? frame.cols / double(im.cols) : 1);
      cropTrace->append(QJsonObject{{"row", row},
                                    {"field", field},
                                    {"sampling", source ? "source" : "normalized"},
                                    {"image_width", sample.cols},
                                    {"image_height", sample.rows},
                                    {"rect", QJsonArray{box.x, box.y, box.width, box.height}}});
    };
    auto crop = [&](double a, double b, double c, double d) {
      return p->crop(im, row, a, b, c, d);
    };
    // Locate at bounded cost, but do not discard source detail before reading
    // small glyphs. Scale coordinates once, before rounding the final crop.
    auto detailCrop = [&](double a, double b, double c, double d, bool right = false) {
      return frame.cols > im.cols
                 ? p->crop(frame, row, a, b, c, d, right, frame.cols / double(im.cols))
                 : p->crop(im, row, a, b, c, d, right);
    };
    auto readNumber = [&](const QString &field, double a, double b, double c, double d, bool right,
                          int maximum) {
      trace(field, a, b, c, d, right, true);
      auto reading = number(detailCrop(a, b, c, d, right), maximum);
      // Source and normalized sampling have different antialiasing phases.
      // Only retry an abstention, using the same segmentation/acceptance rules.
      if (!reading.value && frame.cols > im.cols) {
        trace(field, a, b, c, d, right, false);
        reading = number(p->crop(im, row, a, b, c, d, right), maximum);
      }
      return reading;
    };
    trace("portrait", 23, -27, 77, 27, false, true);
    auto identity = face(detailCrop(23, -27, 77, 27));
    bool unselected = identity.unselected;
    double placeholderScore = identity.selectionConfidence;
    trace("ultimate_ready", 84, -13, 110, 13, false, false);
    double readyScore = correlation(gray(resize(crop(84, -13, 110, 13), 26, 26)), ready_);
    if (readyScore <= .8) {
      // Search the immediate neighborhood instead of lowering the threshold:
      // a resize can shift the small checkmark relative to the inferred row.
      cv::Mat scores;
      trace("ultimate_ready_search", 82, -15, 112, 15, false, true);
      cv::matchTemplate(gray(resize(detailCrop(82, -15, 112, 15), 30, 30)), ready_, scores,
                        cv::TM_CCOEFF_NORMED);
      double aligned = 0;
      cv::minMaxLoc(scores, nullptr, &aligned);
      readyScore = std::max(readyScore, aligned);
    }
    auto charge = readNumber("ultimate_charge", 87, -9, 107, 10, false, 100);
    bool ready = readyScore > .8;
    if (ready)
      charge = {100, readyScore};
    const bool dead = identity.life == "dead";
    const auto respawn = identity.respawn;
    // Only direct portrait occlusion invalidates its match. An unresolved life
    // reading must not suppress an otherwise accepted hero observation.
    auto hero = identity.portraitOccluded ? Match{} : identity.candidate;
    QJsonObject stats, confidence;
    int numericColumns = 0;
    for (int i = 0; i < 6; i++) {
      int w = i < 3 ? 17 : 32;
      auto n = readNumber(statNames[i], offsets[i] - w, -11, offsets[i] + w, 12, true, 999999);
      stats[statNames[i]] = nullable(n.value);
      confidence[statNames[i]] = n.confidence;
      numericColumns += n.value.has_value();
    }
    QJsonObject perks;
    for (auto tier : {QString("major"), QString("minor")}) {
      bool shown = perkLayout == 2 || (perkLayout == 1 && tier == "minor");
      int x = tier == "major" ? -470 : -422;
      if (shown)
        trace("perk_" + tier, x - 20, -20, x + 20, 20, true, true);
      perks[tier] = shown ? perk(detailCrop(x - 20, -20, x + 20, 20, true), hero.id, tier)
                          : QJsonObject{{"id", QJsonValue::Null},
                                        {"selection", "not_shown"},
                                        {"confidence", .95}};
    }
    QString life = identity.life;
    result.players.append(QJsonObject{
        {"hero", nullable(hero.id)},
        {"hero_selection",
         unselected ? "unselected" : (hero.id.isEmpty() ? "unknown" : "selected")},
        {"selection_confidence", unselected ? placeholderScore : hero.confidence},
        {"hero_confidence", hero.confidence},
        {"portrait_occluded", identity.portraitOccluded},
        {"life_state", life},
        {"life_confidence", identity.lifeConfidence},
        {"ultimate_charge", nullable(charge.value)},
        {"ultimate_ready", charge.value ? QJsonValue(ready) : QJsonValue(QJsonValue::Null)},
        {"ultimate_confidence", charge.confidence},
        {"respawn_seconds", dead ? nullable(respawn.value) : QJsonValue(QJsonValue::Null)},
        {"perks", perks},
        {"stats", stats},
        {"stat_confidence", confidence}});
    trace("display_name", 119, -17, 267, 11, false, false);
    result.names[row] = crop(119, -17, 267, 11).clone();
    bool evidence = numericColumns >= 4 && charge.value.has_value();
    evidenceRows += evidence;
    evidenceTeams[row / 5] += evidence;
  }
  result.confirmed = evidenceRows >= 8 && evidenceTeams[0] >= 3 && evidenceTeams[1] >= 3;
  return result;
}
} // namespace owcs
