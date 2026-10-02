// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   main.cpp                                                                     ║
// ║   RF Power Meter V5 - Assistant (Qt, Windows + Linux)                          ║
// ╚════════════════════════════════════════════════════════════════════════════════╝

#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QSettings>

int main(int argc, char* argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("rf-powermeter"));
    QApplication::setApplicationName(QStringLiteral("assistant"));
    QApplication::setApplicationDisplayName(QStringLiteral("RF Power Meter V5 Assistant"));
    QApplication::setApplicationVersion(QStringLiteral(RFPM_VERSION));
    QSettings::setDefaultFormat(QSettings::IniFormat);   // a readable .ini on every platform

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("Companion app for the USB RF Power Meter V5"));
    cli.addHelpOption();
    cli.addVersionOption();
    const QCommandLineOption portOpt({QStringLiteral("p"), QStringLiteral("port")}, QStringLiteral("Serial port to select (COM7, ttyACM0)."),
                                     QStringLiteral("name"));
    const QCommandLineOption connectOpt({QStringLiteral("c"), QStringLiteral("connect")}, QStringLiteral("Connect on startup."));
    const QCommandLineOption simOpt(QStringLiteral("simulate"), QStringLiteral("Start in demo mode with the built-in simulated meter."));
    const QCommandLineOption sizeOpt(QStringLiteral("size"), QStringLiteral("Initial window size."), QStringLiteral("WxH"));
    const QCommandLineOption shotOpt(QStringLiteral("screenshot"), QStringLiteral("Save a window screenshot after 5 s, then quit."),
                                     QStringLiteral("file.png"));
    const QCommandLineOption tabOpt(QStringLiteral("tab"), QStringLiteral("Tab to show: 0 Monitor, 1 Sweep, 2 Log."), QStringLiteral("index"));
    cli.addOptions({portOpt, connectOpt, simOpt, sizeOpt, shotOpt, tabOpt});
    cli.process(app);

    StartupOptions options;
    options.port = cli.value(portOpt);
    options.connect = cli.isSet(connectOpt);
    options.simulate = cli.isSet(simOpt);
    options.screenshotPath = cli.value(shotOpt);
    if (cli.isSet(tabOpt)) options.startTab = cli.value(tabOpt).toInt();
    const QStringList wh = cli.value(sizeOpt).split(QLatin1Char('x'));
    if (wh.size() == 2) options.size = QSize(wh[0].toInt(), wh[1].toInt());

    theme::apply(app);

    MainWindow window(options);
    window.show();
    return app.exec();
}
