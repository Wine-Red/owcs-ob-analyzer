#include "names.hpp"
#define ORT_API_MANUAL_INIT
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <windows.h>

namespace owcs {
static QString normalize(QString s) {
  s = s.normalized(QString::NormalizationForm_KC);
  s.remove(QRegularExpression("[\\s\\p{Cc}\\p{Cf}]"));
  while (!s.isEmpty() && QString("|[]{}()<>《》【】").contains(s.front()))
    s.remove(0, 1);
  while (!s.isEmpty() && QString("|[]{}()<>《》【】").contains(s.back()))
    s.chop(1);
  return s;
}
QStringList parseRoster(const QString &text) {
  QStringList result;
  for (const auto &line : text.split(QRegularExpression("[\\r\\n]+"))) {
    auto name = normalize(line);
    if (!name.isEmpty() && name.toUcs4().size() <= 32 &&
        !result.contains(name, Qt::CaseInsensitive))
      result.append(name);
  }
  return result;
}
namespace {
double substitution(char32_t a, char32_t b) {
  if (a == b)
    return 0;
  for (const auto &group : {QStringLiteral("0o"), QStringLiteral("1il"), QStringLiteral("5s"),
                            QStringLiteral("2z"), QStringLiteral("8b")})
    if (a <= 127 && b <= 127 && group.contains(QChar(ushort(a))) &&
        group.contains(QChar(ushort(b))))
      return .15;
  return 1;
}
double distance(const QString &a, const QString &b) {
  const auto x = a.toUcs4(), y = b.toUcs4();
  std::vector<std::vector<double>> d(x.size() + 1, std::vector<double>(y.size() + 1));
  for (int i = 0; i <= x.size(); ++i)
    d[i][0] = i;
  for (int j = 0; j <= y.size(); ++j)
    d[0][j] = j;
  for (int i = 1; i <= x.size(); ++i)
    for (int j = 1; j <= y.size(); ++j) {
      d[i][j] = std::min(
          {d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + substitution(x[i - 1], y[j - 1])});
      if (i > 1 && j > 1 && x[i - 1] == y[j - 2] && x[i - 2] == y[j - 1])
        d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + .75);
    }
  return d.back().back();
}
} // namespace
RosterMatch resolveRoster(const QString &text, const QStringList &roster) {
  const auto name = normalize(text), key = name.toCaseFolded();
  const int length = key.toUcs4().size();
  if (!length || length > 32)
    return {};
  if (roster.isEmpty())
    return {name, 1};
  QStringList exact;
  for (const auto &entry : roster)
    if (normalize(entry).toCaseFolded() == key)
      exact.append(entry);
  if (!exact.isEmpty())
    return exact.size() == 1 ? RosterMatch{exact.front(), 1} : RosterMatch{};
  RosterMatch best;
  double runner = 0;
  int bestLength = 0;
  for (const auto &entry : roster) {
    auto candidate = normalize(entry).toCaseFolded();
    int n = candidate.toUcs4().size(), longest = std::max(n, length),
        shortest = std::min(n, length);
    if (!n || n > 32 || shortest < longest * .75 ||
        std::abs(n - length) > (shortest < 9    ? 1
                                : shortest < 13 ? 2
                                                : 3))
      continue;
    double score = 1 - distance(key, candidate) / longest;
    if (score > best.score) {
      runner = best.score;
      best = {entry, score};
      bestLength = shortest;
    } else
      runner = std::max(runner, score);
  }
  // Short IDs have very little evidence: only near-identical glyph readings pass.
  // Longer IDs can tolerate omissions, substitutions and adjacent transpositions.
  double threshold = bestLength <= 3 ? .90 : bestLength == 4 ? .80 : bestLength < 9 ? .78 : .76;
  double margin = bestLength <= 3 ? .15 : bestLength <= 5 ? .12 : .08;
  return best.score >= threshold && best.score - runner >= margin ? best : RosterMatch{};
}
QString matchRoster(const QString &text, const QStringList &roster) {
  return resolveRoster(text, roster).name;
}
bool isEmptySlotName(const QString &text) {
  const auto key = normalize(text).toCaseFolded();
  return key == QStringLiteral("等待玩家") || key == "waitingforplayer" ||
         key == "waitingforplayers";
}
RosterMatch resolveName(const QString &text, const QStringList &roster) {
  const auto name = normalize(text);
  if (name.isEmpty() || name.toUcs4().size() > 32 || isEmptySlotName(name))
    return {};
  auto match = resolveRoster(name, roster);
  return match.name.isEmpty() ? RosterMatch{name, 1} : match;
}
struct NameReader::Impl {
  Ort::Env env{nullptr};
  Ort::Session session{nullptr};
  QStringList characters;
  std::string input, output;
  explicit Impl(const QString &dir) {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
      auto api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
      if (!api)
        throw std::runtime_error("ONNX Runtime API version mismatch");
      Ort::InitApi(api);
    });
    env = Ort::Env(ORT_LOGGING_LEVEL_ERROR, "owcs-ob");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(2);
    opts.SetInterOpNumThreads(1);
    opts.DisableCpuMemArena();
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    auto path = (dir + "/names.onnx").toStdWString();
    session = Ort::Session(env, path.c_str(), opts);
    Ort::AllocatorWithDefaultOptions alloc;
    input = session.GetInputNameAllocated(0, alloc).get();
    output = session.GetOutputNameAllocated(0, alloc).get();
    auto metadata = session.GetModelMetadata().LookupCustomMetadataMapAllocated("character", alloc);
    if (!metadata)
      throw std::runtime_error("OCR model has no character dictionary");
    characters = QString::fromUtf8(metadata.get()).split('\n');
    if (!characters.isEmpty() && characters.last().isEmpty())
      characters.removeLast();
    for (auto &c : characters)
      if (c.endsWith('\r'))
        c.chop(1);
    characters.prepend("blank");
    characters.append(" ");
  }
  Match infer(const cv::Mat &image) {
    int width = std::max(320, int(48. * image.cols / image.rows)),
        rw = std::min(width, int(std::ceil(48. * image.cols / image.rows)));
    cv::Mat resized;
    cv::resize(image, resized, {rw, 48});
    std::vector<float> data(3 * 48 * width, 0);
    for (int y = 0; y < 48; y++)
      for (int x = 0; x < rw; x++) {
        auto p = resized.at<cv::Vec3b>(y, x);
        for (int c = 0; c < 3; c++)
          data[(c * 48 + y) * width + x] = p[c] / 127.5f - 1;
      }
    std::array<int64_t, 4> shape{1, 3, 48, width};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto tensor =
        Ort::Value::CreateTensor<float>(memory, data.data(), data.size(), shape.data(), 4);
    const char *inputs[]{input.c_str()};
    const char *outputs[]{output.c_str()};
    auto result = session.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
    auto dims = result[0].GetTensorTypeAndShapeInfo().GetShape();
    if (dims.size() != 3)
      return {};
    const float *values = result[0].GetTensorData<float>();
    int prior = -1, count = 0;
    double sum = 0;
    QString text;
    for (int64_t i = 0; i < dims[1]; i++) {
      auto row = values + i * dims[2];
      int index = int(std::max_element(row, row + dims[2]) - row);
      if (index != 0 && index != prior && index < characters.size()) {
        text += characters[index];
        sum += row[index];
        count++;
      }
      prior = index;
    }
    return {normalize(text), count ? sum / count : 0};
  }
};
NameReader::NameReader(const QString &dir) : impl_(std::make_unique<Impl>(dir)) {}
NameReader::~NameReader() = default;
Match NameReader::read(const cv::Mat &image, const QStringList &roster) {
  if (image.empty())
    return {};
  // The scoreboard is translucent. Feeding the entire colored name cell to
  // OCR lets background texture become extra characters after short names.
  // Restrict both readings to the actual neutral, bright glyph foreground.
  auto mask = Recognizer::textMask(image);
  std::vector<cv::Point> points;
  cv::findNonZero(mask, points);
  if (points.size() < 10)
    return {};
  cv::Mat tight, foreground, inverse;
  cv::copyMakeBorder(mask(cv::boundingRect(points)), tight, 3, 3, 3, 3, cv::BORDER_CONSTANT,
                     cv::Scalar(0));
  cv::cvtColor(tight, foreground, cv::COLOR_GRAY2BGR);
  cv::bitwise_not(foreground, inverse);
  auto a = impl_->infer(foreground);
  if (a.confidence < .72 || a.id.isEmpty() || a.id.size() > 32)
    return {};
  auto b = impl_->infer(inverse);
  auto matchedA = resolveName(a.id, roster), matchedB = resolveName(b.id, roster);
  a.id = matchedA.name;
  b.id = matchedB.name;
  a.confidence = std::min(a.confidence, matchedA.score);
  b.confidence = std::min(b.confidence, matchedB.score);
  if (a.id.toCaseFolded() != b.id.toCaseFolded() || b.confidence < .72)
    return {};
  bool valid = false;
  for (auto c : a.id)
    valid |= c.isLetterOrNumber();
  return valid ? Match{a.id, std::min(a.confidence, b.confidence)} : Match{};
}
} // namespace owcs
