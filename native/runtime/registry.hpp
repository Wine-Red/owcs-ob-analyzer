#pragma once
#include "../core/state.hpp"
#include <QMutex>
namespace owcs {
struct ResetToken {
  quint64 generation = 0;
  quint64 lastSessionReset = 0;
  qint64 after = -1;
};
enum class ClearScope { AllData, Perks };
class Registry {
public:
  static Registry &instance();
  void publish(const QString &, QJsonObject, quint64 resetGeneration);
  bool clear(const QString &id, ClearScope scope = ClearScope::AllData);
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
  QJsonObject playerPriorities() const;
  void setPlayerPriorities(QJsonObject);

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
  QJsonObject playerPriorities_;
};
} // namespace owcs
