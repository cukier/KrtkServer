#include "httpserver.h"

#include <QTcpSocket>
#include <QFile>
#include <QTextStream>
#include <QDebug>

HttpServer::HttpServer(const QString &storagePath, quint16 port, QObject *parent)
    : QObject(parent), m_storagePath(storagePath), m_port(port)
{
    connect(&m_server, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);
}

bool HttpServer::start()
{
    if (!m_server.listen(QHostAddress::Any, m_port)) {
        qCritical() << "Failed to start server:" << m_server.errorString();
        return false;
    }
    qInfo() << "HTTP server listening on port" << m_port;
    qInfo() << "GPS data stored at:" << m_storagePath;
    return true;
}

void HttpServer::onNewConnection()
{
    while (m_server.hasPendingConnections()) {
        QTcpSocket *socket = m_server.nextPendingConnection();
        // Read entire request before handling (buffer until headers + body complete)
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            handleClient(socket);
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
}

void HttpServer::handleClient(QTcpSocket *socket)
{
    QByteArray data = socket->readAll();

    // Parse request line
    int lineEnd = data.indexOf("\r\n");
    if (lineEnd < 0) return;
    QByteArray requestLine = data.left(lineEnd);
    QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() < 2) {
        socket->write(buildResponse(400, "Bad Request"));
        socket->disconnectFromHost();
        return;
    }

    QByteArray method = parts[0];
    QByteArray path   = parts[1];

    // Separate headers from body
    int headerEnd = data.indexOf("\r\n\r\n");
    QByteArray body;
    if (headerEnd >= 0)
        body = data.mid(headerEnd + 4);

    QByteArray response;

    if (method == "GET" && path == "/ping") {
        response = buildResponse(200, "pong\n");

    } else if (method == "POST" && path == "/upload") {
        if (body.isEmpty()) {
            response = buildResponse(400, "{\"error\":\"empty body\"}\n");
        } else {
            QString error;
            if (!appendGpsPoints(body, error)) {
                QByteArray msg = ("{\"error\":\"" + error + "\"}\n").toUtf8();
                response = buildResponse(400, msg);
            } else {
                int rows = body.count('\n');
                QByteArray msg = ("{\"status\":\"ok\",\"rows_received\":" + QByteArray::number(rows) + "}\n");
                response = buildResponse(200, msg);
            }
        }

    } else {
        response = buildResponse(404, "{\"error\":\"not found\"}\n");
    }

    socket->write(response);
    socket->disconnectFromHost();
}

QByteArray HttpServer::buildResponse(int status, const QByteArray &body)
{
    QByteArray statusText = (status == 200) ? "OK"
                          : (status == 400) ? "Bad Request"
                          : (status == 404) ? "Not Found"
                          : "Internal Server Error";

    return "HTTP/1.1 " + QByteArray::number(status) + " " + statusText + "\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" + body;
}

// Expected CSV: latitude,longitude[,timestamp] — header rows (non-numeric) are skipped
bool HttpServer::appendGpsPoints(const QByteArray &csvData, QString &error)
{
    QFile file(m_storagePath);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        error = "Cannot open storage file: " + file.errorString();
        return false;
    }

    QTextStream out(&file);
    int written = 0;

    for (const QByteArray &rawLine : csvData.split('\n')) {
        QByteArray line = rawLine.trimmed();
        if (line.isEmpty()) continue;

        QList<QByteArray> fields = line.split(',');
        if (fields.size() < 2) continue;

        bool latOk = false, lonOk = false;
        double lat = fields[0].trimmed().toDouble(&latOk);
        double lon = fields[1].trimmed().toDouble(&lonOk);

        if (!latOk || !lonOk) continue; // header or bad row

        if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
            error = QString("Coordinates out of range: %1,%2").arg(lat).arg(lon);
            return false;
        }

        if (fields.size() >= 3)
            out << fields[0].trimmed() << "," << fields[1].trimmed() << "," << fields[2].trimmed() << "\n";
        else
            out << fields[0].trimmed() << "," << fields[1].trimmed() << "\n";

        ++written;
    }

    qDebug() << "Appended" << written << "GPS points";
    return true;
}
