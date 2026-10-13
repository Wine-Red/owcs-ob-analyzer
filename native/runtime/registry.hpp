#pragma once
#include "../core/state.hpp"
#include <QMutex>
namespace owcs {
class Registry {
public:
  static Registry &instance();
  void publish(const QString &, QJsonObject);
  void received(const QString &, const QString &source, qint64 captured);
  void remove(const QString &);
  QJsonObject state(const QString &id = {}) const;
  QJsonArray streams() const;
  void select(const QString &id);
  bool claimInput(const QString &id);
  void releaseInput(const QString &id);

private:
  struct Entry {
    QJsonObject snapshot;
    qint64 seen = 0;
    qint64 captured = -1;
    QString source;
  };
  mutable QMutex mutex_;
  QMap<QString, Entry> entries_;
  QString selected_, inputOwner_;
};
} // namespace owcs
