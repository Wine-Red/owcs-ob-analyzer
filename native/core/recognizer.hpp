#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <array>
#include <opencv2/core.hpp>
#include <optional>
#include <vector>

namespace owcs {
struct Match {
  QString id;
  double confidence = 0;
};
struct Number {
  std::optional<int> value;
  double confidence = 0;
};
struct FaceReading {
  Match candidate;
  QString life = "unknown";
  Number respawn;
  bool unselected = false;
  bool portraitOccluded = false;
  double selectionConfidence = 0, lifeConfidence = 0, ring = 0, cross = 0;
};
struct Panel {
  int left = 0;
  double top = 0;
  int right = 0;
  double scale = 1;
  double cy(int row) const;
  cv::Rect rect(int row, double x1, double y1, double x2, double y2, bool rightAnchor = false,
                double sourceScale = 1) const;
  cv::Mat crop(const cv::Mat &, int row, double x1, double y1, double x2, double y2,
               bool rightAnchor = false, double sourceScale = 1) const;
};
struct Observation {
  bool visible = false;
  bool confirmed = false;
  int hudOffset = -10000;
  QJsonArray players;
  QJsonArray bounds;
  std::array<cv::Mat, 10> names;
};
struct ForegroundBank {
  cv::Mat values, weights, counts;
};
class Recognizer {
public:
  explicit Recognizer(const QString &assetDir);
  Observation detect(const cv::Mat &bgr, QJsonArray *cropTrace = nullptr);
  Number number(const cv::Mat &, int maximum = 999999) const;
  FaceReading face(const cv::Mat &) const;
  QJsonObject perk(const cv::Mat &, const QString &hero, const QString &tier) const;
  static cv::Mat textMask(const cv::Mat &);
  static cv::Mat signature(const cv::Mat &);
  QString resourceVersion() const { return resourceVersion_; }

private:
  Number numberMask(const cv::Mat &, int maximum) const;
  std::optional<Panel> locate(const cv::Mat &) const;
  Match portrait(const cv::Mat &) const;
  double hudQuality(const cv::Mat &, int offset, bool coarse) const;
  int locateHud(const cv::Mat &);
  cv::Mat digits_, digitLabels_, portraits_, hud_, hudCoarse_, hudIndices_, perks_, ready_;
  ForegroundBank fullBank_, hudBank_, hudCoarseBank_;
  cv::Mat unselectedHud_, unselectedPanel_;
  QStringList portraitNames_, hudNames_;
  QString resourceVersion_;
  QJsonArray perkNames_;
  int hudCandidate_ = -10000, hudCount_ = 0;
  std::vector<int> searchOffsets_;
  size_t searchIndex_ = 0;
  int searchHeight_ = 0;
};
cv::Mat readImage(const QString &, int flags = 1);
inline QJsonValue nullable(const QString &s) {
  return s.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s);
}
inline QJsonValue nullable(std::optional<int> n) {
  return n ? QJsonValue(*n) : QJsonValue(QJsonValue::Null);
}
} // namespace owcs
