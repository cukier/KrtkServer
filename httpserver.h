#pragma once

#include <QGeoPath>
#include <QMap>
#include <QObject>
#include <QString>
#include <QTcpServer>

class HttpServer : public QObject {
  Q_OBJECT
public:
  explicit HttpServer(const QString &storageDir, quint16 port = 8080,
                      QObject *parent = nullptr);
  bool start();

  const QGeoPath &path() const { return m_path; }

private slots:
  void onNewConnection();

private:
  QTcpServer m_server;
  QString m_storagePath;
  QString m_file;
  quint16 m_port;
  QGeoPath m_path;
  QMap<QTcpSocket *, QByteArray> m_buffers;

  void handleClient(QTcpSocket *socket, const QByteArray &data);
  QByteArray buildResponse(int status, const QByteArray &body,
                           const QByteArray &contentType = "application/json");
  bool appendGpsPoints(const QByteArray &csvData, const QString &filePath, QString &error);
  bool saveToFile(const QString &filePath, QString &error) const;
  QList<QGeoCoordinate> readCoordinates(const QString &filePath, QString &error) const;
  QString resolveFilePath(const QMap<QByteArray, QByteArray> &params) const;
  static bool isValidFileName(const QString &name);
};
