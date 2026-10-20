#include "liquipedia.hpp"
#include "../core/roster.hpp"
#include "../core/state.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <windows.h>
#include <winhttp.h>
namespace owcs {
namespace {
struct Handle {
  HINTERNET value;
  ~Handle() {
    if (value)
      WinHttpCloseHandle(value);
  }
  operator HINTERNET() const { return value; }
};
QByteArray get(const QString &path, QString &error) {
  // Use Windows TLS and proxy handling; no dependency on the OBS host's Qt TLS plugins.
  Handle session{WinHttpOpen(L"OWCSOBAnalyzer/0.5.7 (https://github.com/Wine-Red/owcs-ob-analyzer)",
                             WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session) {
    error = QStringLiteral("无法初始化网络连接");
    return {};
  }
  WinHttpSetTimeouts(session, 10000, 10000, 10000, 15000);
  DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
  WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
  Handle connection{WinHttpConnect(session, L"liquipedia.net", INTERNET_DEFAULT_HTTPS_PORT, 0)};
  Handle request{connection ? WinHttpOpenRequest(connection, L"GET", path.toStdWString().c_str(),
                                                 nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                            : nullptr};
  if (!request) {
    error = QStringLiteral("无法创建 Liquipedia 请求");
    return {};
  }
  DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
  if (!WinHttpSendRequest(request, L"Accept: application/json\r\n", -1, WINHTTP_NO_REQUEST_DATA, 0,
                          0, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    error = QStringLiteral("Liquipedia 连接失败（Windows 错误 %1），请检查网络后重试")
                .arg(GetLastError());
    return {};
  }
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    error = QStringLiteral("Liquipedia 返回 HTTP %1，请稍后重试；现有名单已保留").arg(status);
    return {};
  }
  QByteArray body;
  char buffer[16384];
  DWORD read = 0;
  while (true) {
    if (!WinHttpReadData(request, buffer, sizeof(buffer), &read)) {
      error = QStringLiteral("读取赛事数据失败");
      return {};
    }
    if (!read)
      break;
    body.append(buffer, int(read));
    if (body.size() > 8 * 1024 * 1024) {
      error = QStringLiteral("赛事响应过大，已停止读取");
      return {};
    }
  }
  return body;
}
} // namespace
class LiquipediaClient::Job : public QThread {
public:
  QString path, error;
  QJsonObject data;
  void run() override {
    auto bytes = get(path, error);
    if (!error.isEmpty())
      return;
    QJsonParseError e;
    auto doc = QJsonDocument::fromJson(bytes, &e);
    if (e.error != QJsonParseError::NoError || !doc.isObject()) {
      error = QStringLiteral("Liquipedia 未返回有效的赛事数据");
      return;
    }
    data = doc.object();
    if (data.contains("error"))
      error = QStringLiteral("Liquipedia：%1")
                  .arg(data["error"].toObject()["info"].toString("API error"));
  }
};
LiquipediaClient::LiquipediaClient(QString cacheDir, QObject *parent)
    : QObject(parent), cacheDir_(std::move(cacheDir)) {}
LiquipediaClient::~LiquipediaClient() {
  if (job_) {
    job_->wait();
    delete job_;
  }
}
bool LiquipediaClient::search(const QString &input, Callback callback) {
  auto query = liquipediaSearch(input);
  if (query.isEmpty()) {
    callback({}, QStringLiteral("请输入赛事名称或年份"));
    return true;
  }
  return fetch(QUrlQuery{{"action", "query"},
                         {"list", "search"},
                         {"srsearch", query},
                         {"srnamespace", "0"},
                         {"srlimit", "50"}},
               std::move(callback));
}
bool LiquipediaClient::tournament(const QString &input, Callback callback) {
  auto page = liquipediaPage(input);
  if (page.isEmpty()) {
    callback({}, QStringLiteral("请选择有效的 Liquipedia Overwatch 赛事"));
    return true;
  }
  return fetch(QUrlQuery{{"action", "parse"}, {"page", page}, {"prop", "text|revid"}},
               std::move(callback));
}
bool LiquipediaClient::fetch(QUrlQuery query, Callback callback) {
  if (busy_)
    return false;
  busy_ = true;
  query.addQueryItem("format", "json");
  const bool parse = query.queryItemValue("action") == "parse";
  auto path = "/overwatch/api.php?" + query.toString(QUrl::FullyEncoded);
  auto key = QString::fromLatin1(
      QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex());
  auto file = cacheDir_ + "/" + key + ".json";
  QFile cached(file);
  if (cached.open(QIODevice::ReadOnly)) {
    auto value = QJsonDocument::fromJson(cached.readAll()).object();
    auto age = QDateTime::currentSecsSinceEpoch() - qint64(value["time"].toDouble());
    if (age >= 0 && age < (parse ? 300 : 3600) && value["data"].isObject()) {
      QTimer::singleShot(0, this, [this, callback, value] {
        busy_ = false;
        callback(value["data"].toObject(), {});
      });
      return true;
    }
  }
  auto now = monotonicMs();
  auto wait =
      std::max<qint64>({0, 2000 - (now - lastRequest_), parse ? 30000 - (now - lastParse_) : 0});
  QTimer::singleShot(int(wait), this, [this, path, parse, file, callback] {
    lastRequest_ = monotonicMs();
    if (parse)
      lastParse_ = lastRequest_;
    job_ = new Job;
    job_->path = path;
    connect(job_, &QThread::finished, this, [this, file, callback] {
      auto data = job_->data;
      auto error = job_->error;
      job_->wait();
      delete job_;
      job_ = nullptr;
      busy_ = false;
      if (error.isEmpty()) {
        QDir().mkpath(cacheDir_);
        QSaveFile cache(file);
        if (cache.open(QIODevice::WriteOnly)) {
          cache.write(
              QJsonDocument(
                  QJsonObject{{"time", double(QDateTime::currentSecsSinceEpoch())}, {"data", data}})
                  .toJson(QJsonDocument::Compact));
          cache.commit();
        }
      }
      callback(data, error);
    });
    job_->start();
  });
  return true;
}
} // namespace owcs
