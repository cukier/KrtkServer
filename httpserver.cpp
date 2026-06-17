#include "httpserver.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QGeoCoordinate>
#include <QTcpSocket>

HttpServer::HttpServer(const QString &storageDir, quint16 port, QObject *parent)
    : QObject(parent), m_storagePath(storageDir), m_port(port) {
  connect(&m_server, &QTcpServer::newConnection, this,
          &HttpServer::onNewConnection);
}

bool HttpServer::start() {
  QDir().mkpath(m_storagePath);
  if (!m_server.listen(QHostAddress::Any, m_port)) {
    qCritical() << "Failed to start server:" << m_server.errorString();
    return false;
  }
  qInfo() << "HTTP server listening on port" << m_port;
  qInfo() << "GPS storage directory:" << m_storagePath;
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
        return;

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
        return;

      handleClient(socket, buf);
      m_buffers.remove(socket);
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
      m_buffers.remove(socket);
      socket->deleteLater();
    });
  }
}

bool HttpServer::isValidFileName(const QString &name) {
  if (name.isEmpty() || name == "." || name == ".." ||
      name.contains('/') || name.contains('\\') || name.contains('\0'))
    return false;
  for (const QChar &c : name)
    if (!c.isLetterOrNumber() && c != '_' && c != '-' && c != '.')
      return false;
  return true;
}

QString HttpServer::resolveFilePath(const QMap<QByteArray, QByteArray> &params) const {
  QString name = params.contains("file") ? QString::fromUtf8(params["file"]) : m_file;
  if (!isValidFileName(name))
    return QString();
  return m_storagePath + "/" + name;
}

QList<QGeoCoordinate> HttpServer::readCoordinates(const QString &filePath,
                                                   QString &error) const {
  QFile file(filePath);
  if (!file.exists()) {
    error = "file not found";
    return {};
  }
  if (!file.open(QIODevice::ReadOnly)) {
    error = file.errorString();
    return {};
  }
  QList<QGeoCoordinate> coords;
  while (!file.atEnd()) {
    QByteArray line = file.readLine().trimmed();
    if (line.isEmpty())
      continue;
    QList<QByteArray> fields = line.split(',');
    if (fields.size() < 2)
      continue;
    bool latOk, lonOk;
    double lat = fields[0].toDouble(&latOk);
    double lon = fields[1].toDouble(&lonOk);
    if (!latOk || !lonOk)
      continue;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
      continue;
    QGeoCoordinate coord(lat, lon);
    if (fields.size() >= 3) {
      bool altOk;
      double alt = fields[2].trimmed().toDouble(&altOk);
      if (altOk)
        coord.setAltitude(alt);
    }
    coords.append(coord);
  }
  return coords;
}

bool HttpServer::saveToFile(const QString &filePath, QString &error) const {
  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    error = file.errorString();
    return false;
  }
  for (const QGeoCoordinate &coord : m_path.path()) {
    QByteArray row = QByteArray::number(coord.latitude(), 'f', 7) + "," +
                     QByteArray::number(coord.longitude(), 'f', 7);
    if (coord.type() == QGeoCoordinate::Coordinate3D)
      row += "," + QByteArray::number(coord.altitude(), 'f', 2);
    row += "\n";
    file.write(row);
  }
  qInfo() << "Saved" << m_path.path().size() << "GPS points to" << filePath;
  return true;
}

void HttpServer::handleClient(QTcpSocket *socket, const QByteArray &data) {
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

  QByteArray path = rawPath;
  QByteArray queryString;
  int qmark = rawPath.indexOf('?');
  if (qmark >= 0) {
    path = rawPath.left(qmark);
    queryString = rawPath.mid(qmark + 1);
  }

  QMap<QByteArray, QByteArray> params;
  for (const QByteArray &kv : queryString.split('&')) {
    int eq = kv.indexOf('=');
    if (eq > 0)
      params[kv.left(eq)] = kv.mid(eq + 1);
  }

  int headerEnd = data.indexOf("\r\n\r\n");
  QByteArray body;
  if (headerEnd >= 0)
    body = data.mid(headerEnd + 4);

  // Returns coords from file if file param/m_file resolves, else from m_path
  auto getCoords = [&](QString &outErr) -> QList<QGeoCoordinate> {
    QString fp = resolveFilePath(params);
    if (!fp.isEmpty())
      return readCoordinates(fp, outErr);
    return m_path.path();
  };

  QByteArray response;

  if (method == "GET" && path == "/help") {
    static const QByteArray help =
        "{"
        "\"endpoints\":["
          "{"
            "\"method\":\"GET\","
            "\"path\":\"/ping\","
            "\"description\":\"Health check\","
            "\"params\":[]"
          "},"
          "{"
            "\"method\":\"GET\","
            "\"path\":\"/files\","
            "\"description\":\"List all GPS files in the storage directory\","
            "\"params\":[]"
          "},"
          "{"
            "\"method\":\"GET\","
            "\"path\":\"/download\","
            "\"description\":\"Return the full path as a JSON array\","
            "\"params\":["
              "{\"name\":\"file\",\"required\":false,\"description\":\"File to read from; defaults to current file\"}"
            "]"
          "},"
          "{"
            "\"method\":\"GET\","
            "\"path\":\"/path\","
            "\"description\":\"Return paginated coordinates\","
            "\"params\":["
              "{\"name\":\"page\",\"required\":true,\"description\":\"Page number (1-based)\"},"
              "{\"name\":\"size\",\"required\":false,\"description\":\"Page size (default 1000)\"},"
              "{\"name\":\"file\",\"required\":false,\"description\":\"File to read from; defaults to current file\"}"
            "]"
          "},"
          "{"
            "\"method\":\"POST\","
            "\"path\":\"/upload\","
            "\"description\":\"Append GPS points from a CSV body (lat,lon[,alt] per line); sets the current file\","
            "\"params\":["
              "{\"name\":\"file\",\"required\":false,\"description\":\"File to append to; defaults to current file\"}"
            "]"
          "},"
          "{"
            "\"method\":\"POST\","
            "\"path\":\"/save\","
            "\"description\":\"Overwrite a file with the current in-memory path\","
            "\"params\":["
              "{\"name\":\"file\",\"required\":false,\"description\":\"File to write to; defaults to current file\"}"
            "]"
          "},"
          "{"
            "\"method\":\"DELETE\","
            "\"path\":\"/delete\","
            "\"description\":\"Delete a GPS file from the storage directory\","
            "\"params\":["
              "{\"name\":\"file\",\"required\":false,\"description\":\"File to delete; defaults to current file\"}"
            "]"
          "},"
          "{"
            "\"method\":\"GET\","
            "\"path\":\"/help\","
            "\"description\":\"This API map\","
            "\"params\":[]"
          "}"
        "]"
        "}";
    response = buildResponse(200, help);

  } else if (method == "GET" && path == "/ping") {
    response = buildResponse(200, "{\"status\":\"pong\"}");

  } else if (method == "GET" && path == "/files") {
    QDir dir(m_storagePath);
    QStringList files = dir.entryList(QDir::Files, QDir::Name);
    QByteArray json = "{\"files\":[";
    for (int i = 0; i < files.size(); ++i) {
      if (i > 0)
        json += ",";
      json += "\"" + files[i].toUtf8() + "\"";
    }
    json += "]}";
    response = buildResponse(200, json);

  } else if (method == "GET" && path == "/download") {
    // Full path JSON (swapped: was /path)
    QString err;
    QList<QGeoCoordinate> coords = getCoords(err);
    if (!err.isEmpty()) {
      response = buildResponse(404, ("{\"error\":\"" + err + "\"}\n").toUtf8());
    } else if (coords.isEmpty()) {
      response = buildResponse(404, "{\"error\":\"no path found\"}\n");
    } else {
      QByteArray json = "{\"path\":[";
      for (int i = 0; i < coords.size(); ++i) {
        if (i > 0)
          json += ",";
        const QGeoCoordinate &c = coords[i];
        json += "{\"lat\":" + QByteArray::number(c.latitude(), 'f', 7) +
                ",\"lon\":" + QByteArray::number(c.longitude(), 'f', 7);
        if (c.type() == QGeoCoordinate::Coordinate3D)
          json += ",\"alt\":" + QByteArray::number(c.altitude(), 'f', 2);
        json += "}";
      }
      json += "]}";
      response = buildResponse(200, json);
    }

  } else if (method == "GET" && path == "/path") {
    // Paginated (swapped: was /download)
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
        QString err;
        QList<QGeoCoordinate> coords = getCoords(err);
        if (!err.isEmpty()) {
          response = buildResponse(404, ("{\"error\":\"" + err + "\"}\n").toUtf8());
        } else {
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
            json += ",\"count\":" + QByteArray::number(count) + ",\"coordinates\":[";
            for (int i = offset; i < end; ++i) {
              const QGeoCoordinate &c = coords[i];
              if (i > offset)
                json += ",";
              json += "{\"lat\":" + QByteArray::number(c.latitude(), 'f', 7) +
                      ",\"lon\":" + QByteArray::number(c.longitude(), 'f', 7);
              if (c.type() == QGeoCoordinate::Coordinate3D)
                json += ",\"alt\":" + QByteArray::number(c.altitude(), 'f', 2);
              json += "}";
            }
            json += "]}\n";
          }
          response = buildResponse(200, json);
        }
      }
    }

  } else if (method == "POST" && path == "/upload") {
    if (body.isEmpty()) {
      response = buildResponse(400, "{\"error\":\"empty body\"}\n");
    } else {
      QString fp = resolveFilePath(params);
      if (fp.isEmpty()) {
        response = buildResponse(400, "{\"error\":\"missing or invalid file parameter\"}\n");
      } else {
        QString error;
        if (!appendGpsPoints(body, fp, error)) {
          response = buildResponse(400, ("{\"error\":\"" + error + "\"}\n").toUtf8());
        } else {
          if (params.contains("file"))
            m_file = QString::fromUtf8(params["file"]);
          int rows = body.count('\n');
          response = buildResponse(200, "{\"status\":\"ok\",\"rows_received\":" +
                                            QByteArray::number(rows) + "}\n");
        }
      }
    }

  } else if (method == "POST" && path == "/save") {
    QString fp = resolveFilePath(params);
    if (fp.isEmpty()) {
      response = buildResponse(400, "{\"error\":\"missing or invalid file parameter\"}\n");
    } else {
      QString error;
      if (!saveToFile(fp, error)) {
        response = buildResponse(500, ("{\"error\":\"" + error + "\"}\n").toUtf8());
      } else {
        response = buildResponse(200, "{\"status\":\"ok\",\"saved\":" +
                                          QByteArray::number(m_path.path().size()) + "}\n");
      }
    }

  } else if (method == "DELETE" && path == "/delete") {
    QString fp = resolveFilePath(params);
    if (fp.isEmpty()) {
      response = buildResponse(400, "{\"error\":\"missing or invalid file parameter\"}\n");
    } else if (!QFile::exists(fp)) {
      response = buildResponse(404, "{\"error\":\"file not found\"}\n");
    } else if (!QFile::remove(fp)) {
      response = buildResponse(500, "{\"error\":\"failed to delete file\"}\n");
    } else {
      QString name = params.contains("file") ? QString::fromUtf8(params["file"]) : m_file;
      if (name == m_file)
        m_file.clear();
      response = buildResponse(200, "{\"status\":\"ok\"}\n");
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

// Expected CSV: latitude,longitude[,altitude] — header rows (non-numeric) are skipped
bool HttpServer::appendGpsPoints(const QByteArray &csvData,
                                  const QString &filePath, QString &error) {
  QFile file(filePath);
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

  qDebug() << "Appended" << written << "GPS points to" << filePath;
  return true;
}
