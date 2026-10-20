#pragma once
#include "../core/state.hpp"
#include <QMutex>
namespace owcs {
struct ResetToken {
  quint64 generation = 0;
  qint64 after = -1;
};
class Registry {
public:
  static Registry &instance();
  void publish(const QString &, QJsonObject, quint64 resetGeneration);
  bool clear(const QString &id);
  ResetToken resetToken(const QString &id) const;
  void received(const QString &, const QString &source, qint64 captured);
  void remove(const QString &);
  QJsonObject state(const QString &id = {}) const;
  QJsonArray streams() const;
  void select(const QString &id);
  bool claimInput(const QString &id);
  void releaseInput(const QString &id);
  QStringList roster() const;
  void setRoster(QStringList);

private:
  struct Entry {
    QJsonObject snapshot;
    qint64 seen = 0;
    qint64 captured = -1;
    QString source;
    ResetToken reset;
  };
  mutable QMutex mutex_;
  QMap<QString, Entry> entries_;
  QString selected_, inputOwner_;
  QStringList roster_;
};
} // namespace owcs
