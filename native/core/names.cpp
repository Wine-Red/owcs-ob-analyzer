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
Match NameReader::read(const cv::Mat &image) {
  if (image.empty())
    return {};
  auto a = impl_->infer(image);
  if (a.confidence < .72 || a.id.isEmpty() || a.id.size() > 32)
    return {};
  cv::Mat gray, bgr;
  cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
  cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
  auto b = impl_->infer(bgr);
  if (a.id.toCaseFolded() != b.id.toCaseFolded() || b.confidence < .72)
    return {};
  bool valid = false;
  for (auto c : a.id)
    valid |= c.isLetterOrNumber();
  return valid ? Match{a.id, std::min(a.confidence, b.confidence)} : Match{};
}
} // namespace owcs
