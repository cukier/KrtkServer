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
  parser.addOption({{"d", "dir"},
                    "Directory to store GPS files (default: gps_data)",
                    "dir",
                    "gps_data"});
  parser.process(app);

  quint16 port = static_cast<quint16>(parser.value("port").toUShort());
  QString storagePath = parser.value("dir");

  HttpServer server(storagePath, port);
  if (!server.start())
    return 1;

  return app.exec();
}
