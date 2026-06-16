#include "httpserver.h"
#include <QCommandLineParser>
#include <QCoreApplication>

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  app.setApplicationName("KrtkServer");

  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOption(
      {{"p", "port"}, "Port to listen on (default: 8080)", "port", "8080"});
  parser.addOption({{"f", "file"},
                    "File to store GPS points (default: gps_points.csv)",
                    "file",
                    "gps_points.csv"});
  parser.process(app);

  quint16 port = static_cast<quint16>(parser.value("port").toUShort());
  QString storagePath = parser.value("file");

  HttpServer server(storagePath, port);
  if (!server.start())
    return 1;

  return app.exec();
}
