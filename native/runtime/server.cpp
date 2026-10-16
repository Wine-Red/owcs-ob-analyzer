#include "server.hpp"
#include "../core/public_state.hpp"
#include "registry.hpp"
#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QUrl>
#include <QUrlQuery>
namespace owcs {
Server::Server(QObject *parent) : QObject(parent) {
  connect(&server_, &QTcpServer::newConnection, this, [this] {
    while (server_.hasPendingConnections()) {
      auto s = server_.nextPendingConnection();
      if (clients_.size() >= 32) {
        s->disconnectFromHost();
        s->deleteLater();
        continue;
      }
      clients_[s] = {};
      connect(s, &QTcpSocket::readyRead, this, [this, s] { read(s); });
      connect(s, &QTcpSocket::disconnected, this, [this, s] {
        clients_.remove(s);
        s->deleteLater();
      });
      QTimer::singleShot(5000, s, [this, s] {
        if (clients_.contains(s) && !clients_[s].websocket)
          s->disconnectFromHost();
      });
    }
  });
  timer_.setInterval(200);
  connect(&timer_, &QTimer::timeout, this, [this] { broadcast(); });
}
bool Server::listen(quint16 port, bool lan) {
  stop();
  if (!server_.listen(lan ? QHostAddress::AnyIPv4 : QHostAddress::LocalHost, port)) {
    status_ = QStringLiteral("接口启动失败：") + server_.errorString();
    return false;
  }
  status_ = QString("http://%1:%2/v1/state")
                .arg(lan ? QStringLiteral("本机IP") : QString("127.0.0.1"))
                .arg(server_.serverPort());
  timer_.start();
  return true;
}
void Server::stop() {
  timer_.stop();
  server_.close();
  auto sockets = clients_.keys();
  for (auto s : sockets) {
    disconnect(s, nullptr, this, nullptr);
    s->abort();
    s->deleteLater();
  }
  clients_.clear();
}
void Server::reply(QTcpSocket *s, int code, const QByteArray &body, const QByteArray &type) {
  QByteArray head =
      "HTTP/1.1 " + QByteArray::number(code) + (code == 200 ? " OK\r\n" : " Error\r\n");
  head += "Content-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) +
          "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: "
          "*\r\nConnection: close\r\n\r\n";
  s->write(head + body);
  s->disconnectFromHost();
}
QByteArray Server::frame(QByteArray body, int opcode) {
  QByteArray out;
  out.append(char(0x80 | opcode));
  quint64 n = body.size();
  if (n < 126)
    out.append(char(n));
  else if (n <= 65535) {
    out.append(char(126));
    out.append(char(n >> 8));
    out.append(char(n));
  } else {
    out.append(char(127));
    for (int i = 7; i >= 0; i--)
      out.append(char(n >> (i * 8)));
  }
  out += body;
  return out;
}
void Server::read(QTcpSocket *s) {
  if (!clients_.contains(s))
    return;
  auto &c = clients_[s];
  c.buffer += s->readAll();
  if (c.buffer.size() > 65536) {
    s->abort();
    return;
  }
  if (c.websocket) {
    while (c.buffer.size() >= 2) {
      const auto *p = reinterpret_cast<const unsigned char *>(c.buffer.constData());
      int op = p[0] & 15;
      bool masked = p[1] & 128;
      quint64 size = p[1] & 127;
      int header = 2;
      if (size == 126) {
        if (c.buffer.size() < 4)
          return;
        size = (p[2] << 8) | p[3];
        header = 4;
      } else if (size == 127) {
        s->abort();
        return;
      }
      if (!masked || (p[0] & 0x70) || !(p[0] & 0x80) || size > 32768) {
        s->abort();
        return;
      }
      if (c.buffer.size() < header + 4 + qint64(size))
        return;
      QByteArray body = c.buffer.mid(header + 4, size);
      for (int i = 0; i < body.size(); i++)
        body[i] = char(uchar(body[i]) ^ p[header + i % 4]);
      c.buffer.remove(0, header + 4 + size);
      if (op == 8) {
        s->write(frame({}, 8));
        s->disconnectFromHost();
        return;
      }
      if (op == 9 && size <= 125)
        s->write(frame(body, 10));
      else if (op != 10) {
        s->abort();
        return;
      }
    }
    return;
  }
  int end = c.buffer.indexOf("\r\n\r\n");
  if (end < 0) {
    if (c.buffer.size() > 8192)
      s->abort();
    return;
  }
  auto lines = c.buffer.left(end).split('\n');
  auto request = lines.value(0).trimmed().split(' ');
  if (request.size() != 3 || request[0] != "GET") {
    reply(s, 405, "{}");
    return;
  }
  QUrl url = QUrl::fromEncoded(request[1]);
  QString stream = QUrlQuery(url).queryItemValue("stream");
  QMap<QByteArray, QByteArray> headers;
  for (auto line : lines) {
    int at = line.indexOf(':');
    if (at > 0)
      headers[line.left(at).trimmed().toLower()] = line.mid(at + 1).trimmed();
  }
  if (url.path() == "/v1/state-stream") {
    auto key = headers.value("sec-websocket-key");
    if (headers.value("upgrade").toLower() != "websocket" ||
        headers.value("sec-websocket-version") != "13" ||
        QByteArray::fromBase64(key).size() != 16) {
      reply(s, 400, "{}");
      return;
    }
    auto accept = QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                                           QCryptographicHash::Sha1)
                      .toBase64();
    s->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: "
             "websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
             accept + "\r\n\r\n");
    c.websocket = true;
    c.stream = stream;
    c.buffer.remove(0, end + 4);
    s->write(frame(QJsonDocument(publicState(Registry::instance().state(stream)))
                       .toJson(QJsonDocument::Compact)));
    return;
  }
  QJsonObject result;
  int code = 200;
  if (url.path() == "/health")
    result = {{"service", "owcs-ob-obs-plugin"}, {"version", "0.5.2"}, {"status", "ok"}};
  else if (url.path() == "/v1/streams") {
    QJsonArray list;
    for (const auto &value : Registry::instance().streams()) {
      auto item = value.toObject();
      item.remove("source_age_ms");
      list.append(item);
    }
    result = {{"streams", list}};
  } else if (url.path() == "/v1/state") {
    result = Registry::instance().state(stream);
    if (result["type"] != "state.snapshot")
      code = 503;
    result = publicState(result);
  } else {
    code = 404;
    result = {{"error", "not_found"}};
  }
  reply(s, code, QJsonDocument(result).toJson(QJsonDocument::Compact));
}
void Server::broadcast() {
  for (auto it = clients_.begin(); it != clients_.end(); ++it) {
    if (!it->websocket)
      continue;
    auto socket = it.key();
    if (socket->bytesToWrite() > 262144) {
      socket->abort();
      return;
    }
    socket->write(frame(QJsonDocument(publicState(Registry::instance().state(it->stream)))
                            .toJson(QJsonDocument::Compact)));
  }
}
} // namespace owcs
