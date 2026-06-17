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
    static const QByteArray help = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>KrtkServer API</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#0d1117;color:#e6edf3;padding:2rem;max-width:780px;margin:0 auto}
h1{font-size:1.35rem;font-weight:600;color:#f0f6fc;margin-bottom:.3rem}
.sub{color:#8b949e;font-size:.82rem;margin-bottom:2rem}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;margin-bottom:.75rem;overflow:hidden}
.row{display:flex;align-items:center;gap:.75rem;padding:.8rem 1rem}
.badge{font-size:.68rem;font-weight:700;padding:.18rem .45rem;border-radius:4px;letter-spacing:.07em;min-width:54px;text-align:center}
.GET{background:#0d2b1a;color:#3fb950}
.POST{background:#0c1f3f;color:#58a6ff}
.DELETE{background:#2d0f0f;color:#f85149}
.path{font-family:'SF Mono',Consolas,monospace;font-size:.92rem;font-weight:500;color:#f0f6fc}
.desc{color:#8b949e;font-size:.8rem;margin-left:auto;text-align:right}
.params{border-top:1px solid #21262d;padding:.65rem 1rem}
table{width:100%;border-collapse:collapse;font-size:.78rem}
th{color:#6e7681;font-weight:500;text-align:left;padding:.15rem .5rem .35rem}
td{padding:.28rem .5rem;color:#c9d1d9;vertical-align:top}
td:first-child{font-family:'SF Mono',Consolas,monospace;color:#79c0ff;white-space:nowrap}
.req{color:#f85149;font-size:.67rem;font-weight:600;background:#2d0f0f;padding:.1rem .3rem;border-radius:3px}
.opt{color:#6e7681;font-size:.67rem}
.note{color:#6e7681;padding-top:.4rem;font-style:italic}
</style>
</head>
<body>
<h1>KrtkServer API</h1>
<p class="sub">GPS track server &mdash; responses are JSON unless noted &bull; <code>file</code> param defaults to the current active file</p>

<div class="card">
  <div class="row"><span class="badge GET">GET</span><span class="path">/ping</span><span class="desc">Health check</span></div>
</div>

<div class="card">
  <div class="row"><span class="badge GET">GET</span><span class="path">/files</span><span class="desc">List all GPS files in storage</span></div>
</div>

<div class="card">
  <div class="row"><span class="badge GET">GET</span><span class="path">/download</span><span class="desc">Full path as JSON array</span></div>
  <div class="params"><table>
    <tr><th>param</th><th></th><th>description</th></tr>
    <tr><td>file</td><td><span class="opt">optional</span></td><td>File to read from</td></tr>
  </table></div>
</div>

<div class="card">
  <div class="row"><span class="badge GET">GET</span><span class="path">/path</span><span class="desc">Paginated coordinates</span></div>
  <div class="params"><table>
    <tr><th>param</th><th></th><th>description</th></tr>
    <tr><td>page</td><td><span class="req">required</span></td><td>Page number (1-based)</td></tr>
    <tr><td>size</td><td><span class="opt">optional</span></td><td>Page size (default 1000)</td></tr>
    <tr><td>file</td><td><span class="opt">optional</span></td><td>File to read from</td></tr>
  </table></div>
</div>

<div class="card">
  <div class="row"><span class="badge POST">POST</span><span class="path">/upload</span><span class="desc">Append CSV body; sets active file</span></div>
  <div class="params"><table>
    <tr><th>param</th><th></th><th>description</th></tr>
    <tr><td>file</td><td><span class="opt">optional</span></td><td>File to append to</td></tr>
    <tr><td colspan="3" class="note">Body: one <code>lat,lon[,alt]</code> per line &mdash; non-numeric header rows are skipped</td></tr>
  </table></div>
</div>

<div class="card">
  <div class="row"><span class="badge POST">POST</span><span class="path">/save</span><span class="desc">Overwrite file with in-memory path</span></div>
  <div class="params"><table>
    <tr><th>param</th><th></th><th>description</th></tr>
    <tr><td>file</td><td><span class="opt">optional</span></td><td>File to write to</td></tr>
  </table></div>
</div>

<div class="card">
  <div class="row"><span class="badge DELETE">DELETE</span><span class="path">/delete</span><span class="desc">Delete a GPS file</span></div>
  <div class="params"><table>
    <tr><th>param</th><th></th><th>description</th></tr>
    <tr><td>file</td><td><span class="opt">optional</span></td><td>File to delete</td></tr>
  </table></div>
</div>

<div class="card">
  <div class="row"><span class="badge GET">GET</span><span class="path">/view</span><span class="desc">Interactive Leaflet map — draw a path and download as CSV</span></div>
</div>

<div class="card">
  <div class="row"><span class="badge GET">GET</span><span class="path">/help</span><span class="desc">This page</span></div>
</div>

</body>
</html>)html";
    response = buildResponse(200, help, "text/html");

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

  } else if (method == "GET" && path == "/view") {
    static const QByteArray viewHtml = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Draw Path</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<link rel="stylesheet" href="https://unpkg.com/leaflet-control-geocoder@2.4.0/dist/Control.Geocoder.css"/>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#0d1117;color:#e6edf3;display:flex;flex-direction:column;height:100vh}
#toolbar{display:flex;align-items:center;gap:.6rem;padding:.5rem .75rem;background:#161b22;border-bottom:1px solid #30363d;flex-shrink:0;flex-wrap:wrap}
#toolbar h1{font-size:.9rem;font-weight:600;color:#f0f6fc;margin-right:.4rem}
button{font-size:.78rem;font-weight:600;padding:.32rem .75rem;border-radius:5px;border:1px solid #30363d;cursor:pointer;transition:background .15s}
#btnDraw{background:#0c1f3f;color:#58a6ff;border-color:#1f6feb}
#btnDraw.active{background:#1f6feb;color:#fff}
#btnClear{background:#2d0f0f;color:#f85149;border-color:#6e1a1a}
#btnDownload{background:#0d2b1a;color:#3fb950;border-color:#238636}
#btnDownload:disabled{opacity:.4;cursor:not-allowed}
#btnUpload{background:#1f2a1f;color:#56d364;border-color:#2ea043}
#btnUpload:disabled{opacity:.4;cursor:not-allowed}
#btnLoad{background:#1a1a2e;color:#d2a8ff;border-color:#6e40c9}
#count{font-size:.75rem;color:#8b949e;margin-left:.25rem}
#map{flex:1;position:relative}
#filePanel{position:absolute;top:.75rem;right:.75rem;z-index:1000;background:#161b22;border:1px solid #30363d;border-radius:8px;min-width:200px;max-width:280px;box-shadow:0 4px 16px #0008;display:none}
#filePanel.open{display:block}
#filePanelHeader{display:flex;align-items:center;justify-content:space-between;padding:.5rem .75rem;border-bottom:1px solid #21262d}
#filePanelHeader span{font-size:.8rem;font-weight:600;color:#f0f6fc}
#filePanelClose{background:none;border:none;color:#8b949e;font-size:1rem;cursor:pointer;padding:0 .1rem;line-height:1}
#filePanelClose:hover{color:#f85149}
#fileList{max-height:260px;overflow-y:auto;padding:.35rem 0}
.file-item{display:flex;align-items:center;justify-content:space-between;padding:.4rem .75rem;cursor:pointer;gap:.5rem;transition:background .12s}
.file-item:hover{background:#21262d}
.file-name{font-size:.78rem;font-family:'SF Mono',Consolas,monospace;color:#79c0ff;flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.file-show{font-size:.68rem;font-weight:600;color:#3fb950;background:#0d2b1a;border:1px solid #238636;border-radius:4px;padding:.15rem .4rem;cursor:pointer;white-space:nowrap;flex-shrink:0}
.file-show:hover{background:#238636;color:#fff}
#fileEmpty{font-size:.78rem;color:#6e7681;padding:.6rem .75rem;text-align:center}
#loadedLabel{font-size:.75rem;color:#d2a8ff;margin-left:.1rem;display:none}
</style>
</head>
<body>
<div id="toolbar">
  <h1>Draw Path</h1>
  <button id="btnDraw">Draw</button>
  <button id="btnClear">Clear</button>
  <button id="btnDownload" disabled>Download CSV</button>
  <button id="btnUpload" disabled>Upload</button>
  <button id="btnLoad">Load</button>
  <span id="count">0 points</span>
  <span id="loadedLabel"></span>
</div>
<div id="map">
  <div id="filePanel">
    <div id="filePanelHeader">
      <span>Saved files</span>
      <button id="filePanelClose">&#x2715;</button>
    </div>
    <div id="fileList"><div id="fileEmpty">Loading…</div></div>
  </div>
</div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script src="https://unpkg.com/leaflet-control-geocoder@2.4.0/dist/Control.Geocoder.js"></script>
<script>
const map = L.map('map').setView([-22.999118, -47.044591], 17);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
  attribution: '&copy; OpenStreetMap contributors', maxZoom: 19
}).addTo(map);

L.Control.geocoder({defaultMarkGeocode: false, placeholder: 'Search place…'})
  .on('markgeocode', e => { map.fitBounds(e.geocode.bbox, {padding:[24,24]}); })
  .addTo(map);

let drawing = false;
let points = [];
let polyline = L.polyline([], {color:'#58a6ff',weight:3}).addTo(map);

let loadedPolyline = L.polyline([], {color:'#d2a8ff',weight:3,dashArray:'6 4'}).addTo(map);
let loadedMarkers = [];

const btnDraw = document.getElementById('btnDraw');
const btnClear = document.getElementById('btnClear');
const btnDownload = document.getElementById('btnDownload');
const btnUpload = document.getElementById('btnUpload');
const btnLoad = document.getElementById('btnLoad');
const countEl = document.getElementById('count');
const loadedLabel = document.getElementById('loadedLabel');
const filePanel = document.getElementById('filePanel');
const fileList = document.getElementById('fileList');
const fileEmpty = document.getElementById('fileEmpty');
const filePanelClose = document.getElementById('filePanelClose');

function updateCount() {
  countEl.textContent = points.length + ' point' + (points.length !== 1 ? 's' : '');
  btnDownload.disabled = points.length === 0;
  btnUpload.disabled = points.length === 0;
}

let pressing = false;

function addPoint(latlng) {
  const {lat, lng} = latlng;
  if (points.length > 0) {
    const last = L.latLng(points[points.length - 1]);
    if (last.distanceTo(latlng) < 0.1) return; // skip if under 10cm apart
  }
  points.push([lat, lng]);
  polyline.setLatLngs(points);
  updateCount();
}

btnDraw.addEventListener('click', () => {
  drawing = !drawing;
  btnDraw.classList.toggle('active', drawing);
  btnDraw.textContent = drawing ? 'Stop' : 'Draw';
  map.getContainer().style.cursor = drawing ? 'crosshair' : '';
  if (drawing) map.dragging.disable();
  else { map.dragging.enable(); pressing = false; }
});

map.on('mousedown', e => {
  if (!drawing || e.originalEvent.button !== 0) return;
  pressing = true;
  addPoint(e.latlng);
});

map.on('mousemove', e => {
  if (!drawing || !pressing) return;
  addPoint(e.latlng);
});

document.addEventListener('mouseup', () => { pressing = false; });

btnClear.addEventListener('click', () => {
  points = [];
  polyline.setLatLngs([]);
  updateCount();
  if (drawing) {
    drawing = false;
    pressing = false;
    btnDraw.classList.remove('active');
    btnDraw.textContent = 'Draw';
    map.getContainer().style.cursor = '';
    map.dragging.enable();
  }
});

btnDownload.addEventListener('click', () => {
  if (points.length === 0) return;
  const csv = 'latitude,longitude\n' + points.map(p => p[0].toFixed(7) + ',' + p[1].toFixed(7)).join('\n');
  const blob = new Blob([csv], {type:'text/csv'});
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = 'path.csv';
  a.click();
  URL.revokeObjectURL(url);
});

btnUpload.addEventListener('click', async () => {
  if (points.length === 0) return;
  const name = prompt('Save as filename (e.g. mypath.csv):');
  if (!name || !name.trim()) return;
  const csv = points.map(p => p[0].toFixed(7) + ',' + p[1].toFixed(7)).join('\n');
  try {
    const res = await fetch('/upload?file=' + encodeURIComponent(name.trim()), {
      method: 'POST',
      headers: {'Content-Type': 'text/plain'},
      body: csv
    });
    const json = await res.json();
    if (res.ok) {
      alert('Uploaded ' + points.length + ' points as "' + name.trim() + '".');
    } else {
      alert('Upload failed: ' + (json.error || res.status));
    }
  } catch(e) {
    alert('Upload error: ' + e.message);
  }
});

function clearLoaded() {
  loadedPolyline.setLatLngs([]);
  loadedMarkers.forEach(m => map.removeLayer(m));
  loadedMarkers = [];
  loadedLabel.style.display = 'none';
  loadedLabel.textContent = '';
}

async function loadFile(name) {
  clearLoaded();
  filePanel.classList.remove('open');
  try {
    const res = await fetch('/download?file=' + encodeURIComponent(name));
    const json = await res.json();
    if (!json.path || json.path.length === 0) { alert('No coordinates in ' + name); return; }
    const latlngs = json.path.map(p => [p.lat, p.lon]);
    loadedPolyline.setLatLngs(latlngs);
    latlngs.forEach(ll => {
      const m = L.circleMarker(ll, {radius:3,color:'#d2a8ff',fillColor:'#d2a8ff',fillOpacity:0.8,weight:1}).addTo(map);
      loadedMarkers.push(m);
    });
    map.fitBounds(loadedPolyline.getBounds(), {padding:[24,24]});
    loadedLabel.textContent = name + ' (' + latlngs.length + ' pts)';
    loadedLabel.style.display = 'inline';
  } catch(e) {
    alert('Failed to load ' + name + ': ' + e.message);
  }
}

btnLoad.addEventListener('click', async () => {
  if (filePanel.classList.contains('open')) {
    filePanel.classList.remove('open');
    return;
  }
  fileEmpty.textContent = 'Loading…';
  fileList.innerHTML = '';
  fileList.appendChild(fileEmpty);
  filePanel.classList.add('open');
  try {
    const res = await fetch('/files');
    const json = await res.json();
    fileList.innerHTML = '';
    if (!json.files || json.files.length === 0) {
      fileEmpty.textContent = 'No saved files found.';
      fileList.appendChild(fileEmpty);
      return;
    }
    json.files.forEach(name => {
      const row = document.createElement('div');
      row.className = 'file-item';
      const nameEl = document.createElement('span');
      nameEl.className = 'file-name';
      nameEl.textContent = name;
      nameEl.title = name;
      const showBtn = document.createElement('button');
      showBtn.className = 'file-show';
      showBtn.textContent = 'Show';
      showBtn.addEventListener('click', e => { e.stopPropagation(); loadFile(name); });
      row.appendChild(nameEl);
      row.appendChild(showBtn);
      row.addEventListener('click', () => loadFile(name));
      fileList.appendChild(row);
    });
  } catch(e) {
    fileEmpty.textContent = 'Error: ' + e.message;
    fileList.innerHTML = '';
    fileList.appendChild(fileEmpty);
  }
});

filePanelClose.addEventListener('click', () => filePanel.classList.remove('open'));
</script>
</body>
</html>)html";
    response = buildResponse(200, viewHtml, "text/html");

  } else {
    response = buildResponse(404, "{\"error\":\"not found\"}\n");
  }

  socket->write(response);
  socket->disconnectFromHost();
}

QByteArray HttpServer::buildResponse(int status, const QByteArray &body,
                                      const QByteArray &contentType) {
  QByteArray statusText = (status == 200)   ? "OK"
                          : (status == 400) ? "Bad Request"
                          : (status == 404) ? "Not Found"
                                            : "Internal Server Error";

  return "HTTP/1.1 " + QByteArray::number(status) + " " + statusText +
         "\r\nContent-Type: " + contentType +
         "\r\nContent-Length: " + QByteArray::number(body.size()) +
         "\r\nConnection: close\r\n\r\n" + body;
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
