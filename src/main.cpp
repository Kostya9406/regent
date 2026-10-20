// Regent — a native Qt 6 task manager for Linux.
//
//   regent                 open the window (or raise the running one)
//   regent --page=ID       same, on a given page (summary, processes, …)
//   regent --json          print one snapshot as JSON and exit
//   regent --stream [MS]   print a JSON line every MS milliseconds (default 2000; also --stream=MS)
//            [--notify]      …and send desktop notifications for alerts (one Regent per session does)
//   regent --select-pid N  open Processes with that process selected
//   regent --active-window open Processes on the process that owns the focused window
//   regent top | kill | menu   headless subcommands, see core/Cli.h
//
// A second launch forwards to the running instance over a local
// socket and exits, so a desktop key bound to `regent` summons the window.

#include "core/Alerts.h"
#include "core/Cli.h"
#include "core/Hyprland.h"
#include "core/Json.h"
#include "core/Notify.h"
#include "core/Sampler.h"
#include "core/Services.h"
#include "core/Settings.h"
#include "ui/MainWindow.h"
#include "ui/OmarchyPalette.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLockFile>
#include <QStandardPaths>
#include <QThreadPool>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <csignal>
#include <cstdio>
#include <unistd.h>

namespace {

// $XDG_RUNTIME_DIR is private to the user (0700): another local account cannot
// pre-create the socket or hold the lock there, as it could in a shared /tmp.
QString RuntimeDir() {
  QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  return dir.isEmpty() ? QDir::tempPath() : dir;
}
QString InstanceName() {
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  return dir.isEmpty() ? QString("regent-instance-v1-%1").arg(getuid()) : dir + "/regent-instance-v1";
}

// "--stream", "--stream MS" or "--stream=MS"; 2000 ms when absent or not a number,
// never faster than 250 ms. Returns -1 when argv[i] is not --stream at all.
int StreamInterval(int argc, char **argv, int i) {
  const QByteArray a = argv[i];
  QByteArray value;
  if (a.startsWith("--stream=")) value = a.mid(9);
  else if (a == "--stream") value = i + 1 < argc ? QByteArray(argv[i + 1]) : QByteArray();
  else return -1;
  bool ok = false;
  const long long ms = value.toLongLong(&ok);
  if (!ok || ms <= 0) return 2000;
  return int(std::clamp<long long>(ms, 250, 3600 * 1000));
}

// "--fields a,b" or "--fields=a,b"; empty when absent.
QStringList Fields(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const QByteArray a = argv[i];
    if (a.startsWith("--fields=")) return QString::fromUtf8(a.mid(9)).split(',', Qt::SkipEmptyParts);
    if (a == "--fields" && i + 1 < argc) return QString::fromUtf8(argv[i + 1]).split(',', Qt::SkipEmptyParts);
  }
  return {};
}

QJsonArray AlertsJson(const QVector<td::alerts::Alert> &alerts) {
  QJsonArray out;
  for (const auto &a : alerts)
    out << QJsonObject{{"id", a.id}, {"level", a.level == td::alerts::Level::Critical ? "critical" : "warn"},
                       {"title", a.title}, {"body", a.body}, {"page", a.page}};
  return out;
}

int PrintJson(int argc, char **argv, bool stream, int intervalMs, bool notify) {
  QCoreApplication app(argc, argv);
  std::signal(SIGPIPE, SIG_DFL);  // a closed reader ends the stream quietly
  const QStringList fields = Fields(argc, argv);
  td::Sampler sampler;
  // The bar needs totals and the top processes, not per-process extras; one
  // --json snapshot can afford them (they also give Intel and Arm GPUs a load).
  sampler.SetDetail(!stream);
  td::alerts::Monitor monitor(td::alerts::FromSettings());
  std::unique_ptr<td::notify::Speaker> speaker;
  if (stream && notify) speaker = std::make_unique<td::notify::Speaker>();
  QElapsedTimer sinceUnits;
  sampler.Collect();
  QThread::msleep(stream ? std::min(intervalMs, 1000) : 500);
  do {
    td::SnapshotPtr s = sampler.Collect();
    QJsonObject obj = td::ToJson(*s);
    if (stream) {
      // Thresholds set in the window reach the bar within half a minute.
      if (!sinceUnits.isValid() || sinceUnits.elapsed() > 30000) {
        sinceUnits.start();
        td::Settings::Get().Reload();
        const td::alerts::Thresholds t = td::alerts::FromSettings();
        monitor.SetThresholds(t);
        if (t.enabled && t.failedUnits) {
          bool systemOk = false, userOk = false;
          const QStringList failed = td::services::Failed(td::services::Bus::System, &systemOk) +
                                     td::services::Failed(td::services::Bus::User, &userOk);
          if (systemOk && userOk) monitor.SetFailedUnits(failed);  // a D-Bus hiccup is not "nothing failed"
        }
      }
      const auto fresh = monitor.Feed(*s);
      if (speaker && td::Settings::Get().value("alertNotifications", true).toBool() && speaker->Holds())
        for (const auto &a : fresh) td::notify::Send(a);
      obj["alerts"] = AlertsJson(monitor.Active());
      obj["alertLevel"] = monitor.Severity();
    }
    if (!fields.isEmpty()) obj = td::cli::SelectFields(obj, fields);
    QByteArray line = QJsonDocument(obj).toJson(stream ? QJsonDocument::Compact : QJsonDocument::Indented);
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fputc('\n', stdout);
    if (std::fflush(stdout) != 0) return 0;
    if (stream) QThread::msleep(intervalMs);
  } while (stream);
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  // Headless modes first: no GUI connection needed.
  if (argc > 1 && argv[1][0] != '-') {
    QCoreApplication app(argc, argv);
    std::signal(SIGPIPE, SIG_DFL);
    const int rc = td::cli::Run(app.arguments().mid(1));
    if (rc >= 0) return rc;
    std::fprintf(stderr, "regent: unknown command %s (try top, kill, menu or --help)\n", argv[1]);
    return 2;
  }
  bool notify = false;
  for (int i = 1; i < argc; ++i) notify |= QByteArray(argv[i]) == "--notify";
  for (int i = 1; i < argc; ++i) {
    QByteArray a = argv[i];
    if (a == "--json") return PrintJson(argc, argv, false, 0, false);
    if (int ms = StreamInterval(argc, argv, i); ms > 0) return PrintJson(argc, argv, true, ms, notify);
  }

  QApplication app(argc, argv);
  app.setApplicationName("regent");
  app.setApplicationDisplayName("Regent");
  app.setDesktopFileName("regent");
  app.setApplicationVersion("0.6.0");

  QCommandLineParser cli;
  cli.setApplicationDescription("Native system monitor and task manager.");
  cli.addHelpOption();
  cli.addVersionOption();
  cli.addOption({"page", "Open on page <id>: summary, performance, processes, system-info, startup-apps, users, "
                         "services, power-and-freq, connections, installed-apps, drivers, disk-space, flight-recorder, benchmarks.", "id"});
  cli.addOption({"json", "Print one snapshot as JSON and exit."});
  // Handled before the parser (no GUI needed); declared here for --help only.
  cli.addOption({"stream", "Print a JSON snapshot line every <ms> (default 2000, at least 250).", "ms", "2000"});
  cli.addOption({"notify", "With --stream: send desktop notifications for alerts."});
  cli.addOption({"fields", "With --json or --stream: only these keys (cpu,mem,temp,net,gpu,top,procs or dotted paths).", "list"});
  cli.addOption({"select-pid", "Open Processes with process <pid> selected.", "pid"});
  cli.addOption({"active-window", "Open Processes on the process that owns the focused window (bind it to a key)."});
  cli.addOption({"capture-pages", "Render every page to <dir>/<page>.png and exit (use with QT_QPA_PLATFORM=offscreen).", "dir"});
  cli.process(app);
  QString page = cli.value("page");
  QString captureDir = cli.value("capture-pages");
  // Ask Hyprland before our own window takes the focus.
  int selectPid = cli.value("select-pid").toInt();
  if (cli.isSet("active-window")) selectPid = td::hypr::ActiveWindow().pid;
  if (selectPid > 0) page = "processes";

  // Single instance only inside the private $XDG_RUNTIME_DIR: in a shared /tmp
  // another user could take the socket name first and receive our commands.
  const bool single = captureDir.isEmpty() &&
                      !QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation).isEmpty();
  if (captureDir.isEmpty() && !single)
    std::fprintf(stderr, "regent: no XDG_RUNTIME_DIR; running standalone\n");
  // Two launches at once must not both decide they are first: hold a lock
  // around "is anyone listening?" and "then I will".
  QLockFile lock(RuntimeDir() + QString("/regent-%1.lock").arg(getuid()));
  // Bounded: a stuck lock must not keep Regent from opening; worst case two windows.
  if (single && !lock.tryLock(5000))
    std::fprintf(stderr, "regent: instance lock busy; continuing\n");

  // Already running? Hand over and leave. (A capture run is always its own instance.)
  if (single) {
    QLocalSocket probe;
    probe.connectToServer(InstanceName());
    if (probe.waitForConnected(300)) {
      probe.write(QString("summon %1 %2\n").arg(page.isEmpty() ? "-" : page).arg(selectPid).toUtf8());
      probe.waitForBytesWritten(300);
      probe.waitForReadyRead(500);  // ack
      return 0;
    }
  }
  QLocalServer server;
  if (single) {
    QLocalServer::removeServer(InstanceName());
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(InstanceName()))
      std::fprintf(stderr, "regent: single-instance socket unavailable (%s); running standalone\n",
                   qPrintable(server.errorString()));
    lock.unlock();
  }

  app.setStyle("Fusion");
  // The whole UI in the Omarchy theme: its colours and its monospace font,
  // re-applied live when `omarchy theme set` switches themes.
  auto applyTheme = [&app] {
    // Palette first, so the style sheet repolishes every widget against the new colours;
    // the other way round, widgets keep palettes computed from the old ones.
    QPalette pal = app.palette();
    pal.setColor(QPalette::Window, td::theme::Bg());
    pal.setColor(QPalette::Base, td::theme::Base());
    pal.setColor(QPalette::AlternateBase, td::theme::Mix(td::theme::Base(), td::theme::Bg(), 0.5));
    pal.setColor(QPalette::Text, td::theme::Text());
    pal.setColor(QPalette::WindowText, td::theme::Text());
    pal.setColor(QPalette::ButtonText, td::theme::Text());
    pal.setColor(QPalette::Button, td::theme::Raised());
    pal.setColor(QPalette::Highlight, td::theme::Selection());
    pal.setColor(QPalette::HighlightedText, td::theme::Bright());
    pal.setColor(QPalette::ToolTipBase, td::theme::Deep());
    pal.setColor(QPalette::ToolTipText, td::theme::Bright());
    pal.setColor(QPalette::PlaceholderText, td::theme::Dim());
    pal.setColor(QPalette::Link, td::theme::Accent());
    app.setPalette(pal);
    // Style sheet before fonts: setting it afterwards would reset widgets to the platform font.
    app.setStyleSheet(td::theme::StyleSheet());
    QFont f = td::theme::Mono(13);
    app.setFont(f);
    // The platform theme gives some widget classes fonts of their own; override those too.
    for (const char *cls : {"QAbstractItemView", "QListView", "QTreeView", "QHeaderView", "QMenu", "QMenuBar",
                            "QLineEdit", "QAbstractButton", "QPushButton", "QToolButton", "QComboBox", "QLabel",
                            "QTabBar", "QMessageBox", "QTipLabel", "QPlainTextEdit", "QCheckBox"})
      app.setFont(f, cls);
  };
  applyTheme();
  QObject::connect(&td::ui::OmarchyPalette::Get(), &td::ui::OmarchyPalette::Changed, &app, applyTheme);

  td::ui::MainWindow w;
  QObject::connect(&server, &QLocalServer::newConnection, &w, [&] {
    while (QLocalSocket *s = server.nextPendingConnection()) {
      QObject::connect(s, &QLocalSocket::readyRead, &w, [s, &w] {
        QString line = QString::fromUtf8(s->readLine()).trimmed();
        if (line.startsWith("summon")) {
          // "summon <page|-> [pid]"; older Regents sent "summon <page>".
          const QStringList parts = line.mid(6).split(' ', Qt::SkipEmptyParts);
          const QString p = parts.value(0) == "-" ? QString() : parts.value(0);
          if (!p.isEmpty()) w.ShowPage(p);
          if (int pid = parts.value(1).toInt(); pid > 0) w.SelectProcess(pid);
          w.Summon();
        }
        s->write("ack\n");
        s->flush();
        s->disconnectFromServer();
      });
      QObject::connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
    }
  });
  if (!captureDir.isEmpty()) {
    // Each page gets a few samples so graphs and async lists have content.
    if (!QDir().mkpath(captureDir)) {
      std::fprintf(stderr, "regent: cannot create %s\n", qPrintable(captureDir));
      return 1;
    }
    QStringList size = qEnvironmentVariable("REGENT_CAPTURE_SIZE", "1400x900").split('x');  // e.g. 656x850
    w.resize(size.value(0).toInt() > 0 ? size[0].toInt() : 1400, size.value(1).toInt() > 0 ? size[1].toInt() : 900);
    QStringList ids = w.AllPageIds();  // a page made of parts is shot part by part
    const QStringList known = ids + w.PageIds();
    // REGENT_CAPTURE_PAGES=summary,performance picks pages (in that order);
    // REGENT_CAPTURE_DELAY=60000 lets graphs fill before each shot.
    QStringList only;
    for (const QString &id : qEnvironmentVariable("REGENT_CAPTURE_PAGES").split(','))
      if (!id.trimmed().isEmpty()) only << id.trimmed();
    if (!only.isEmpty()) {
      QStringList picked;
      for (const QString &id : only) {
        if (!known.contains(id)) {
          std::fprintf(stderr, "regent: unknown page '%s' (pages: %s)\n", qPrintable(id), qPrintable(ids.join(", ")));
          return 1;
        }
        picked << id;
      }
      ids = picked;
    }
    if (ids.isEmpty()) {
      std::fprintf(stderr, "regent: no pages to capture\n");
      return 1;
    }
    int delay = 3500;
    if (qEnvironmentVariableIsSet("REGENT_CAPTURE_DELAY")) {
      bool ok = false;
      delay = qEnvironmentVariable("REGENT_CAPTURE_DELAY").toInt(&ok);
      if (!ok || delay < 500) {
        std::fprintf(stderr, "regent: REGENT_CAPTURE_DELAY must be milliseconds, 500 or more\n");
        return 1;
      }
    }
    w.show();
    auto next = std::make_shared<int>(0);
    w.ShowPage(ids.first());
    auto *timer = new QTimer(&w);
    QObject::connect(timer, &QTimer::timeout, &w, [&w, ids, next, captureDir, timer] {
      const QString file = captureDir + '/' + ids[*next] + ".png";
      if (!w.grab().save(file)) {  // scripts rely on the exit status, not on looking for files
        std::fprintf(stderr, "regent: cannot write %s\n", qPrintable(file));
        timer->stop();
        QCoreApplication::exit(1);
        return;
      }
      if (++*next >= ids.size()) {
        timer->stop();
        QCoreApplication::quit();
        return;
      }
      w.ShowPage(ids[*next]);
    });
    timer->start(delay);
    int rc = app.exec();
    QThreadPool::globalInstance()->waitForDone();
    return rc;
  }
  if (!page.isEmpty()) w.ShowPage(page);
  if (selectPid > 0) w.SelectProcess(selectPid);
  w.show();
  int rc = app.exec();
  // Background loads (services, inventories, sockets) finish before QApplication goes away.
  QThreadPool::globalInstance()->waitForDone();
  return rc;
}
