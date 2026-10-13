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
struct Panel {
  int left = 0, top = 0, right = 0;
  double scale = 1;
  double cy(int row) const;
  cv::Mat crop(const cv::Mat &, int row, double x1, double y1, double x2, double y2,
               bool rightAnchor = false) const;
};
struct Observation {
  bool visible = false;
  bool confirmed = false;
  int hudOffset = -10000;
  QJsonArray players;
  QJsonArray bounds;
  std::array<cv::Mat, 10> names;
};
class Recognizer {
public:
  explicit Recognizer(const QString &assetDir);
  Observation detect(const cv::Mat &bgr);
  Number number(const cv::Mat &, int maximum = 999999) const;
  static cv::Mat textMask(const cv::Mat &);
  static cv::Mat signature(const cv::Mat &);
  QString resourceVersion() const { return resourceVersion_; }

private:
  Number numberMask(const cv::Mat &, int maximum) const;
  std::optional<Panel> locate(const cv::Mat &) const;
  Match portrait(const cv::Mat &, bool deathOverlay) const;
  double hudQuality(const cv::Mat &, int offset, bool coarse) const;
  int locateHud(const cv::Mat &);
  QJsonObject perk(const cv::Mat &, const QString &, const QString &) const;
  cv::Mat digits_, digitLabels_, portraits_, hud_, hudCoarse_, hudIndices_, perks_, ready_;
  cv::Mat fullPortraits_, deathPortraits_;
  cv::Mat unselectedHud_, unselectedPanel_;
  std::vector<int> deathColumns_;
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
