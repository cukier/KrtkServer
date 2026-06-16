#pragma once

#include <QObject>
#include <QTcpServer>
#include <QString>

class HttpServer : public QObject
{
    Q_OBJECT
public:
    explicit HttpServer(const QString &storagePath, quint16 port = 8080, QObject *parent = nullptr);
    bool start();

private slots:
    void onNewConnection();

private:
    QTcpServer m_server;
    QString m_storagePath;
    quint16 m_port;

    void handleClient(QTcpSocket *socket);
    QByteArray buildResponse(int status, const QByteArray &body);
    bool appendGpsPoints(const QByteArray &csvData, QString &error);
};
