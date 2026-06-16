#include "httpserver.h"

#include <QDebug>
#include <QFile>
#include <QGeoCoordinate>
#include <QTcpSocket>

HttpServer::HttpServer(const QString &storagePath, quint16 port,
                       QObject *parent)
    : QObject(parent), m_storagePath(storagePath), m_port(port) {
  connect(&m_server, &QTcpServer::newConnection, this,
          &HttpServer::onNewConnection);
}

bool HttpServer::start() {
  if (!m_server.listen(QHostAddress::Any, m_port)) {
    qCritical() << "Failed to start server:" << m_server.errorString();
    return false;
  }
  qInfo() << "HTTP server listening on port" << m_port;
  qInfo() << "GPS data stored at:" << m_storagePath;
  return true;
}

void HttpServer::onNewConnection() {
  while (m_server.hasPendingConnections()) {
    QTcpSocket *socket = m_server.nextPendingConnection();
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
      m_buffers[socket] += socket->readAll();
      QByteArray &buf = m_buffers[socket];

      int headerEnd = buf.indexOf("\r\n\r\n");
      if (headerEnd < 0)
        return; // headers not yet complete

      // Extract Content-Length
      int contentLength = 0;
      QByteArray headers = buf.left(headerEnd);
      for (const QByteArray &line : headers.split('\n')) {
        QByteArray lower = line.trimmed().toLower();
        if (lower.startsWith("content-length:")) {
          contentLength = lower.mid(15).trimmed().toInt();
          break;
        }
      }

      int bodyReceived = buf.size() - (headerEnd + 4);
      if (bodyReceived < contentLength)
        return; // body not yet complete

      handleClient(socket, buf);
      m_buffers.remove(socket);
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
      m_buffers.remove(socket);
      socket->deleteLater();
    });
  }
}

void HttpServer::handleClient(QTcpSocket *socket, const QByteArray &data) {

  // Parse request line
  int lineEnd = data.indexOf("\r\n");
  if (lineEnd < 0)
    return;
  QByteArray requestLine = data.left(lineEnd);
  QList<QByteArray> parts = requestLine.split(' ');
  if (parts.size() < 2) {
    socket->write(buildResponse(400, "Bad Request"));
    socket->disconnectFromHost();
    return;
  }

  QByteArray method = parts[0];
  QByteArray rawPath = parts[1];

  // Split path and query string
  QByteArray path = rawPath;
  QByteArray queryString;
  int qmark = rawPath.indexOf('?');
  if (qmark >= 0) {
    path = rawPath.left(qmark);
    queryString = rawPath.mid(qmark + 1);
  }

  // Parse query params into a simple map
  QMap<QByteArray, QByteArray> params;
  for (const QByteArray &kv : queryString.split('&')) {
    int eq = kv.indexOf('=');
    if (eq > 0)
      params[kv.left(eq)] = kv.mid(eq + 1);
  }

  // Separate headers from body
  int headerEnd = data.indexOf("\r\n\r\n");
  QByteArray body;
  if (headerEnd >= 0)
    body = data.mid(headerEnd + 4);

  QByteArray response;

  if (method == "GET" && path == "/ping") {
    response = buildResponse(200, "{\"status\":\"pong\"}");

  } else if (method == "GET" && path == "/download") {
    if (!params.contains("page")) {
      response = buildResponse(400, "{\"error\":\"missing page parameter\"}\n");
    } else {
      bool pageOk = false, sizeOk = false;
      int page = params["page"].toInt(&pageOk);
      int size = params.contains("size") ? params["size"].toInt(&sizeOk) : 1000;
      if (!params.contains("size"))
        sizeOk = true;

      if (!pageOk || !sizeOk || page < 1 || size < 1) {
        response = buildResponse(400, "{\"error\":\"invalid page or size\"}\n");
      } else {
        const QList<QGeoCoordinate> &coords = m_path.path();
        int total = coords.size();
        int offset = (page - 1) * size;

        QByteArray json = "{\"page\":" + QByteArray::number(page) +
                          ",\"size\":" + QByteArray::number(size) +
                          ",\"total\":" + QByteArray::number(total);

        if (offset >= total && total > 0) {
          json += ",\"count\":0,\"coordinates\":[]}\n";
        } else {
          int end = qMin(offset + size, total);
          int count = qMax(0, end - offset);
          json += ",\"count\":" + QByteArray::number(count) +
                  ",\"coordinates\":[";
          for (int i = offset; i < end; ++i) {
            const QGeoCoordinate &c = coords[i];
            if (i > offset)
              json += ",";
            json += "{\"lat\":" +
                    QByteArray::number(c.latitude(), 'f', 7) +
                    ",\"lon\":" +
                    QByteArray::number(c.longitude(), 'f', 7);
            if (c.type() == QGeoCoordinate::Coordinate3D)
              json += ",\"alt\":" + QByteArray::number(c.altitude(), 'f', 2);
            json += "}";
          }
          json += "]}\n";
        }
        response = buildResponse(200, json);
      }
    }

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
        QByteArray msg = ("{\"status\":\"ok\",\"rows_received\":" +
                          QByteArray::number(rows) + "}\n");
        response = buildResponse(200, msg);
      }
    }

  } else {
    response = buildResponse(404, "{\"error\":\"not found\"}\n");
  }

  socket->write(response);
  socket->disconnectFromHost();
}

QByteArray HttpServer::buildResponse(int status, const QByteArray &body) {
  QByteArray statusText = (status == 200)   ? "OK"
                          : (status == 400) ? "Bad Request"
                          : (status == 404) ? "Not Found"
                                            : "Internal Server Error";

  return "HTTP/1.1 " + QByteArray::number(status) + " " + statusText +
         "\r\n"
         "Content-Type: application/json\r\n"
         "Content-Length: " +
         QByteArray::number(body.size()) +
         "\r\n"
         "Connection: close\r\n"
         "\r\n" +
         body;
}

// Expected CSV: latitude,longitude[,timestamp] — header rows (non-numeric) are
// skipped
bool HttpServer::appendGpsPoints(const QByteArray &csvData, QString &error) {
  QFile file(m_storagePath);
  if (!file.open(QIODevice::Append)) {
    error = "Cannot open storage file: " + file.errorString();
    return false;
  }

  int written = 0;

  for (const QByteArray &rawLine : csvData.split('\n')) {
    QByteArray line = rawLine.trimmed();
    if (line.isEmpty())
      continue;

    QList<QByteArray> fields = line.split(',');
    if (fields.size() < 2)
      continue;

    bool latOk = false, lonOk = false;
    double lat = fields[0].toDouble(&latOk);
    double lon = fields[1].toDouble(&lonOk);

    if (!latOk || !lonOk)
      continue;

    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
      error = QString("Coordinates out of range: %1,%2").arg(lat).arg(lon);
      return false;
    }

    QGeoCoordinate coord(lat, lon);
    if (fields.size() >= 3) {
      bool altOk = false;
      double alt = fields[2].trimmed().toDouble(&altOk);
      if (altOk)
        coord.setAltitude(alt);
    }
    m_path.addCoordinate(coord);

    QByteArray row = fields[0].trimmed() + "," + fields[1].trimmed();
    if (fields.size() >= 3)
      row += "," + fields[2].trimmed();
    row += "\n";
    file.write(row);

    ++written;
  }

  qDebug() << "Appended" << written << "GPS points";
  return true;
}
