#pragma once
#include "recognizer.hpp"
#include <QMap>
#include <chrono>

namespace owcs {
qint64 monotonicMs();
QString utcNow();
struct Field {
  QJsonValue value = QJsonValue::Null, candidate = QJsonValue::Null;
  int count = 0;
  qint64 seen = -1;
  qint64 candidateSeen = -1;
  QString observed;
  double confidence = 0;
  bool observe(QJsonValue, double, qint64, int required = 2, int maxStep = -1,
               qint64 candidateWindow = 1000);
  QJsonObject metadata(qint64 now, bool visible, int ttl = 1000) const;
};
struct Player {
  QMap<QString, Field> fields;
  cv::Mat signature;
  cv::Mat pendingSignature;
  int identityMisses = 0;
  bool heroPending = false;
  int generation = 0;
};
class State {
public:
  explicit State(QString streamId);
  QJsonObject update(const Observation &, const QString &source, qint64 now, double latency);
  void name(int row, Match result, qint64 now);
  bool needsName(int row) const;
  void reset();
  static QJsonObject age(QJsonObject snapshot, qint64 now);

private:
  std::array<Player, 10> players_;
  QString streamId_, sessionId_;
  qint64 sequence_ = 0, lastVisible_ = -1;
  int panelCount_ = 0;
};
} // namespace owcs
