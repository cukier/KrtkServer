#pragma once

#include <QGeoPath>
#include <QObject>
#include <QString>
#include <QTcpServer>

class HttpServer : public QObject {
  Q_OBJECT
public:
  explicit HttpServer(const QString &storagePath, quint16 port = 8080,
                      QObject *parent = nullptr);
  bool start();

  const QGeoPath &path() const { return m_path; }

private slots:
  void onNewConnection();

private:
  QTcpServer m_server;
  QString m_storagePath;
  quint16 m_port;
  QGeoPath m_path;

  void handleClient(QTcpSocket *socket);
  QByteArray buildResponse(int status, const QByteArray &body);
  bool appendGpsPoints(const QByteArray &csvData, QString &error);
};
