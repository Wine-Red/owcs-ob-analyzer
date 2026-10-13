#pragma once
#include <QMap>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
namespace owcs {
class Server : public QObject {
public:
  explicit Server(QObject *parent = nullptr);
  ~Server() override { stop(); }
  bool listen(quint16 port, bool lan);
  void stop();
  QString status() const { return status_; }

private:
  struct Client {
    QByteArray buffer;
    bool websocket = false;
    QString stream;
  };
  void read(QTcpSocket *);
  void reply(QTcpSocket *, int, const QByteArray &,
             const QByteArray &type = "application/json; charset=utf-8");
  void broadcast();
  static QByteArray frame(QByteArray, int opcode = 1);
  QTcpServer server_;
  QTimer timer_;
  QMap<QTcpSocket *, Client> clients_;
  QString status_;
};
} // namespace owcs
