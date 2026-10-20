#pragma once
#include <QJsonObject>
#include <QObject>
#include <QUrlQuery>
#include <functional>
namespace owcs {
class LiquipediaClient : public QObject {
public:
  using Callback = std::function<void(QJsonObject, QString)>;
  explicit LiquipediaClient(QString cacheDir, QObject *parent = nullptr);
  ~LiquipediaClient();
  bool search(const QString &, Callback);
  bool tournament(const QString &, Callback);

private:
  class Job;
  Job *job_ = nullptr;
  QString cacheDir_;
  bool busy_ = false;
  qint64 lastRequest_ = -30000, lastParse_ = -30000;
  bool fetch(QUrlQuery, Callback);
};
} // namespace owcs
