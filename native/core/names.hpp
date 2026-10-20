#pragma once
#include "recognizer.hpp"
#include <memory>
namespace owcs {
struct RosterMatch {
  QString name;
  double score = 0;
};
// Length-aware weighted edit distance; reject close runner-ups. Empty pool uses OCR.
RosterMatch resolveRoster(const QString &, const QStringList &);
QString matchRoster(const QString &, const QStringList &);
RosterMatch resolveName(const QString &, const QStringList &);
bool isEmptySlotName(const QString &);
QStringList parseRoster(const QString &);
class NameReader {
public:
  explicit NameReader(const QString &models);
  ~NameReader();
  Match read(const cv::Mat &, const QStringList &roster = {});

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace owcs
