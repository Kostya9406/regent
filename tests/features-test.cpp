// Checks for the Regent-only features: alerts, flight anomalies and the
// HTML report, CLI field selection, cgroup units, the command palette's
// fuzzy match and the Hyprland dispatcher quoting. No desktop is touched:
// nothing here sends a notification, a signal or a dispatcher.

#include "core/Actions.h"
#include "core/Alerts.h"
#include "core/Benchmarks.h"
#include "core/Cgroups.h"
#include "core/Cli.h"
#include "core/FlightRecorder.h"
#include "core/Fs.h"
#include "core/FlightReport.h"
#include "core/Hyprland.h"
#include "core/Settings.h"
#include "core/Startup.h"
#include <QStandardPaths>
#include "ui/CommandPalette.h"
#include "ui/Safe.h"

#include <QTextDocument>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>

using namespace td;

namespace {

Snapshot Base(double interval = 1) {
  Snapshot s;
  s.time = QDateTime::currentDateTime();
  s.intervalSec = interval;
  s.memTotal = 16ull << 30;
  s.memAvailable = 8ull << 30;
  s.cpuTemp = 50;
  return s;
}

void Write(const QString &path, const QByteArray &text) {
  QFile f(path);
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(text);
}

}  // namespace

class FeaturesTest : public QObject {
  Q_OBJECT
private slots:
  void temperatureNeedsToHoldAndClearsWithMargin() {
    alerts::Monitor m;
    Snapshot s = Base();
    s.cpuTemp = 95;
    for (int i = 0; i < 9; ++i) QVERIFY(m.Feed(s).isEmpty());  // nine seconds: not yet
    auto fresh = m.Feed(s);
    QCOMPARE(fresh.size(), 1);
    QCOMPARE(fresh.first().id, QString("temp"));
    QCOMPARE(m.Severity(), 2);
    QVERIFY(m.Feed(s).isEmpty());  // already active: no second notification
    s.cpuTemp = 87;                // below 90 but within the 5° margin: stays on
    m.Feed(s);
    QCOMPARE(m.Active().size(), 1);
    s.cpuTemp = 80;
    m.Feed(s);
    QCOMPARE(m.Active().size(), 0);
  }

  void diskAndMemory() {
    alerts::Monitor m;
    Snapshot s = Base();
    s.volumes = {{"/", 100ull << 30, 3ull << 30}, {"/boot", 1ull << 30, 900ull << 20}};
    auto fresh = m.Feed(s);
    QCOMPARE(fresh.size(), 1);
    QCOMPARE(fresh.first().id, QString("disk:/"));
    s.memAvailable = s.memTotal / 100;  // 1 %
    for (int i = 0; i < 4; ++i) m.Feed(s);
    fresh = m.Feed(s);
    QCOMPARE(fresh.size(), 1);
    QCOMPARE(fresh.first().id, QString("memfull"));
  }

  void failedUnitsBaseline() {
    alerts::Monitor m;
    m.SetFailedUnits({"boot-broken.service"});  // already failed when we started: background
    Snapshot s = Base();
    QVERIFY(m.Feed(s).isEmpty());
    m.SetFailedUnits({"boot-broken.service", "new.service"});
    auto fresh = m.Feed(s);
    QCOMPARE(fresh.size(), 1);
    QCOMPARE(fresh.first().id, QString("unit:new.service"));
    m.SetFailedUnits({});                       // everything recovered
    m.Feed(s);
    QCOMPARE(m.Active().size(), 0);
    m.SetFailedUnits({"boot-broken.service"});  // fails again after recovering: news now
    fresh = m.Feed(s);
    QCOMPARE(fresh.size(), 1);
    // new.service in a restart loop: active again, but no second notification within ten minutes.
    m.SetFailedUnits({"new.service"});
    QVERIFY(m.Feed(s).isEmpty());
    QCOMPARE(m.Active().size(), 1);
  }

  void throttleIsAnEvent() {
    alerts::Monitor m;
    Snapshot s = Base();
    s.throttleEvents = 10;
    QVERIFY(m.Feed(s).isEmpty());
    s.throttleEvents = 12;
    QCOMPARE(m.Feed(s).size(), 1);
    for (int i = 0; i < 61; ++i) m.Feed(s);
    QCOMPARE(m.Active().size(), 0);  // a minute without new throttling clears it
  }

  void disabledAlertsStayQuiet() {
    alerts::Thresholds t;
    t.enabled = false;
    alerts::Monitor m(t);
    Snapshot s = Base();
    s.volumes = {{"/", 100, 1}};
    QVERIFY(m.Feed(s).isEmpty());
    QCOMPARE(m.Severity(), 0);
  }

  void anomaliesNameTheCulprit() {
    std::deque<FlightRecorder::Frame> frames;
    for (int i = 0; i < 20; ++i) {
      FlightRecorder::Frame f;
      f.time = QDateTime::currentDateTime().addSecs(i);
      f.cpu = i >= 5 && i < 10 ? 97 : 10;
      f.memoryTotal = 100;
      f.memoryUsed = i == 15 ? 95 : 40;
      f.tempC = i == 7 ? 96 : 60;
      ProcessInfo busy, big;
      busy.name = "rustc";
      busy.cpu = 80;
      big.name = "chrome";
      big.rss = 4ull << 30;
      f.processes = {busy, big};
      frames.push_back(f);
    }
    const auto list = FlightRecorder::Anomalies(frames);
    QCOMPARE(list.size(), 3);  // one CPU run, one temperature, one memory
    QCOMPARE(list[0].kind, QString("cpu"));
    QCOMPARE(list[0].index, 5);
    QVERIFY(list[0].culprit.startsWith("rustc"));
    QCOMPARE(list[1].kind, QString("temp"));
    QCOMPARE(list[2].kind, QString("memory"));
    QVERIFY(list[2].culprit.startsWith("chrome"));
    const auto stats = FlightRecorder::Summarize(frames);
    QCOMPARE(stats.samples, 20);
    QCOMPARE(stats.maxCpu, 97.0);
    QCOMPARE(stats.maxTempC, 96.0);

    // The report escapes process names and carries no command lines.
    frames[3].processes[0].name = "<script>alert(1)</script>";
    frames[3].processes[0].cmdline = "secret-token=abc";
    const QString html = report::Html(frames, "host<1>");
    QVERIFY(html.contains("&lt;script&gt;"));
    QVERIFY(!html.contains("<script>"));
    QVERIFY(!html.contains("secret-token"));
    QVERIFY(html.contains("host&lt;1&gt;"));
    QVERIFY(html.contains("<polyline class=\"temp\""));
    // "%4" is a valid process name; it must come out as itself, not as another field.
    for (auto &f : frames) f.processes[0].name = "x%4%1";
    const QString again = report::Html(frames, "%2");
    QVERIFY(again.contains("x%4%1"));
    QVERIFY(again.contains("Flight report · %2"));
  }

  void flightFramesKeepNewFieldsAndReadOldFiles() {
    QTemporaryDir dir;
    FlightRecorder r;
    Snapshot s = Base();
    s.generation = 1;
    s.cpuTemp = 71;
    s.memPressure = 3.5;
    QVERIFY(r.Capture(s));
    QString error;
    QVERIFY2(r.Save(dir.filePath("a.json"), &error), qPrintable(error));
    FlightRecorder back;
    QVERIFY2(back.Load(dir.filePath("a.json"), &error), qPrintable(error));
    QCOMPARE(back.Frames().front().tempC, 71.0);
    QCOMPARE(back.Frames().front().memPressure, 3.5);
    // A version-1 file without the new keys still loads, with "not recorded";
    // so does one saved while Regent was still called TaskDeck.
    Write(dir.filePath("old.json"),
          R"({"format":"taskdeck-flight-recorder","version":1,"frames":[{"time":"2026-10-19T10:00:00.000Z","cpu":5,)"
          R"("memoryUsed":"1","memoryTotal":"2","processes":[]}]})");
    QVERIFY2(back.Load(dir.filePath("old.json"), &error), qPrintable(error));
    QCOMPARE(back.Frames().front().tempC, -1.0);
    // Out-of-range values are refused.
    Write(dir.filePath("bad.json"),
          R"({"format":"regent-flight-recorder","version":1,"frames":[{"time":"2026-10-19T10:00:00.000Z","cpu":5,)"
          R"("memoryUsed":"1","memoryTotal":"2","processes":[],"tempC":900}]})");
    QVERIFY(!back.Load(dir.filePath("bad.json"), &error));
    // A pre-roll ring keeps only its last frames.
    FlightRecorder ring;
    ring.SetMaxFrames(3);
    for (int g = 1; g <= 5; ++g) {
      s.generation = quint64(g);
      ring.Capture(s);
    }
    QCOMPARE(int(ring.Frames().size()), 3);
  }

  void hyprlandAutostart() {
    using td::startup::HyprlandCommand;
    bool off = false;
    QCOMPARE(HyprlandCommand(R"(  o.launch_on_start("foo --x"))", &off), QString("foo --x"));
    QVERIFY(!off);
    QCOMPARE(HyprlandCommand(R"(hl.exec_cmd(o.launch('bar "q"')))", &off), QString("bar \"q\""));
    QCOMPARE(HyprlandCommand(R"(-- o.launch_on_start("my-service"))", &off), QString());  // Omarchy's example
    QCOMPARE(HyprlandCommand(R"(-- [regent off] o.launch_on_start("foo"))", &off), QString("foo"));
    QVERIFY(off);
    QCOMPARE(HyprlandCommand("hl.bind(\"SUPER\")", &off), QString());
    // Disabling comments the user's line out behind the marker; enabling restores it byte for byte.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/hypr";
    QVERIFY(QDir().mkpath(dir));
    const QByteArray original = "-- Extra autostart processes.\n-- o.launch_on_start(\"my-service\")\n"
                                "  o.launch_on_start(\"foo --x\")\nhl.exec_cmd(o.launch(\"bar\"))\n";
    { QFile f(dir + "/autostart.lua"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(original); }
    auto mine = [] {
      QVector<td::startup::Entry> out;
      for (const auto &e : td::startup::List())
        if (e.source == td::startup::Entry::Source::Hyprland && e.user) out << e;
      return out;
    };
    auto entries = mine();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries[0].exec, QString("foo --x"));
    QString error;
    QVERIFY2(td::startup::SetEnabled(entries[0], false, &error), qPrintable(error));
    entries = mine();
    QVERIFY(!entries[0].enabled);
    { QFile f(dir + "/autostart.lua"); QVERIFY(f.open(QIODevice::ReadOnly)); QVERIFY(f.readAll().contains("  -- [regent off] o.launch_on_start(\"foo --x\")")); }
    QVERIFY2(td::startup::SetEnabled(entries[0], true, &error), qPrintable(error));
    { QFile f(dir + "/autostart.lua"); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(), original); }
    // A line changed since it was read is not touched.
    auto stale = mine()[1];
    stale.lineText += " ";
    QVERIFY(!td::startup::SetEnabled(stale, false, &error));
    // Omarchy's own lines are never edited.
    for (const auto &e : td::startup::List())
      if (e.source == td::startup::Entry::Source::Hyprland && !e.user) { QVERIFY(!td::startup::SetEnabled(e, false, &error)); break; }
  }

  void cliFields() {
    const QJsonObject full{{"cpu", QJsonObject{{"percent", 12.5}, {"tempC", 61}}},
                           {"memory", QJsonObject{{"percent", 40}}},
                           {"processCount", 300}};
    const QJsonObject out = cli::SelectFields(full, {"cpu.percent", "temp", "mem.percent", "procs", "nope"});
    QCOMPARE(out.value("cpu.percent").toDouble(), 12.5);
    QCOMPARE(out.value("temp").toInt(), 61);
    QCOMPARE(out.value("mem.percent").toInt(), 40);
    QCOMPARE(out.value("procs").toInt(), 300);
    QVERIFY(out.contains("nope") && out.value("nope").isNull());
    QCOMPARE(cli::Run({"no-such-command"}), -1);
  }

  void unitNames() {
    QCOMPARE(cgroups::DisplayName("app-Hyprland-google\\x2dchrome\\x2dstable-c5543781.scope"),
             QString("google-chrome-stable"));
    QCOMPARE(cgroups::DisplayName("app-com.google.Chrome-4015.scope"), QString("Chrome"));
    QCOMPARE(cgroups::DisplayName("app-foot@autostart.service"), QString("foot"));
    QCOMPARE(cgroups::DisplayName("NetworkManager.service"), QString("NetworkManager"));
    QCOMPARE(cgroups::DisplayName("session-1.scope"), QString("session-1"));
    QCOMPARE(cgroups::DisplayName("user-1000.slice"), QString("user-1000"));
    QCOMPARE(cgroups::DisplayName("system-gpg\\x2dagent.slice"), QString("system-gpg-agent"));
    QCOMPARE(cgroups::DisplayName("tmux-spawn-0a1baaa8-2827-4b8f-ab20-2c5b7579c273.scope"), QString("tmux-spawn"));
  }

  void cgroupSamplerOnAFakeTree() {
    QTemporaryDir root;
    QDir d(root.path());
    QVERIFY(d.mkpath("user.slice/app-foo.scope"));
    QVERIFY(d.mkpath("user.slice/not-a-unit"));
    Write(root.filePath("user.slice/cpu.stat"), "usage_usec 1000\n");
    Write(root.filePath("user.slice/memory.current"), "4096\n");
    Write(root.filePath("user.slice/app-foo.scope/cpu.stat"), "usage_usec 500\n");
    Write(root.filePath("user.slice/app-foo.scope/memory.current"), "2048\n");
    Write(root.filePath("user.slice/app-foo.scope/cgroup.procs"), "10\n11\n");
    Write(root.filePath("user.slice/app-foo.scope/cgroup.events"), "populated 1\nfrozen 1\n");
    Write(root.filePath("user.slice/app-foo.scope/io.stat"), "259:0 rbytes=100 wbytes=50 rios=1\n8:0 rbytes=10 wbytes=5\n");
    cgroups::Sampler sampler(root.path());
    auto nodes = sampler.Collect();
    QCOMPARE(nodes.size(), 2);  // "not-a-unit" is not a unit
    QCOMPARE(nodes[0].path, QString("user.slice"));
    QCOMPARE(nodes[1].parent, QString("user.slice"));
    QCOMPARE(nodes[1].memory, quint64(2048));
    QCOMPARE(nodes[1].tasks, 2);
    QVERIFY(nodes[1].frozen);
    QCOMPARE(nodes[1].readBps, 0.0);
    QCOMPARE(nodes[0].readBps, -1.0);  // no io.stat: unknown, not zero
    QTest::qWait(100);
    Write(root.filePath("user.slice/app-foo.scope/io.stat"), "259:0 rbytes=1100 wbytes=50\n8:0 rbytes=10 wbytes=5\n");
    nodes = sampler.Collect();
    QVERIFY(nodes[1].readBps > 0);
  }

  void paletteScore() {
    using P = ui::CommandPalette;
    QVERIFY(P::Score("kil fir", "Kill firefox (1234)") > 0);
    QVERIFY(P::Score("rst blu", "Restart bluetooth.service") > 0);
    QVERIFY(P::Score("xyz", "Kill firefox (1234)") < 0);
    // Word starts beat letters scattered in the middle.
    QVERIFY(P::Score("gp", "Go to Performance") > P::Score("gp", "Settings: Graph speed"));
    QVERIFY(P::Score("proc", "Go to Processes") > P::Score("proc", "Export HTML report"));
    QVERIFY(P::Score("", "anything") == 0);
  }

  void throttleVerdict() {
    using benchmarks::ThrottleSample;
    QVector<ThrottleSample> steady, falling;
    for (int t = 1; t <= 60; ++t) {
      steady << ThrottleSample{double(t), 1000 + (t % 3), 3000, 70, 5};
      // Full speed for 20 s, then 30% slower and hot, with the counters moving.
      falling << ThrottleSample{double(t), t <= 20 ? 1000.0 : 700.0, t <= 20 ? 4000.0 : 2800.0, t <= 20 ? 80.0 : 99.0,
                                t <= 20 ? 0 : t - 20};
    }
    const auto a = benchmarks::Judge(steady);
    QVERIFY(!a.throttles);
    QVERIFY(a.text.startsWith("Stable"));
    const auto b = benchmarks::Judge(falling);
    QVERIFY(b.throttles);
    QVERIFY(b.dropPct > 25 && b.dropPct < 35);
    QCOMPARE(b.dropAtSec, 21.0);
    QCOMPARE(b.maxTempC, 99.0);
    QCOMPARE(b.events, 40);
    QVERIFY(b.text.startsWith("Throttles"));
    QVERIFY(!benchmarks::Judge({{1, 1, -1, -1, -1}}).throttles);  // too short to say
  }

  void allCoresAndDirectDisk() {
    std::atomic<bool> cancel{false};
    const auto one = benchmarks::Run(benchmarks::Kind::Cpu, 200, cancel);
    const auto all = benchmarks::RunCpuAll(200, cancel, 2);
    QVERIFY(all.error.isEmpty() && !all.cancelled);
    QCOMPARE(all.detail, QString("2 threads"));
    QVERIFY2(all.score > one.score * 1.3, "two threads do clearly more work than one");
    std::atomic<quint64> progress{0};
    const auto sustained = benchmarks::RunSustained(150, cancel, &progress);
    QVERIFY(progress.load() > 0 && sustained.score > 0);
    // tmpfs has no direct I/O: an error, never a RAM number posing as a drive.
    QTemporaryDir tmp;
    const auto refused = benchmarks::RunDiskDirect(tmp.path(), 200, cancel);
    if (QFile::exists("/proc/self") && QDir(tmp.path()).absolutePath().startsWith("/tmp"))
      QVERIFY(!refused.error.isEmpty() || refused.score > 0);
    // On the home drive it measures, and leaves nothing behind.
    QTemporaryDir onDrive(QDir::homePath() + "/.cache/regent-direct-test-XXXXXX");
    QVERIFY(onDrive.isValid());
    const auto direct = benchmarks::RunDiskDirect(onDrive.path(), 300, cancel);
    QVERIFY2(direct.error.isEmpty(), qPrintable(direct.error));
    QVERIFY(direct.score > 0 && direct.secondaryScore > 0);
    QVERIFY(QDir(onDrive.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
  }

  void processChangesReachEveryThread() {
    // Our own process, with a few extra threads: only raising nice and OOM
    // score, which needs no privileges and harms nothing.
    std::atomic<bool> stop{false};
    std::vector<std::thread> pool;
    for (int i = 0; i < 3; ++i) pool.emplace_back([&] { while (!stop) QThread::msleep(5); });
    QThread::msleep(20);
    const std::string stat = fs::readRaw(QString("/proc/%1/stat").arg(getpid()), 4096);
    const char *p = stat.c_str() + stat.rfind(')') + 2;
    for (int field = 3; field < 22; ++field) p = std::strchr(p, ' ') + 1;
    const ProcessKey self{int(getpid()), std::strtoull(p, nullptr, 10)};
    const int before = getpriority(PRIO_PROCESS, 0);
    auto r = actions::SetNice(self, std::min(19, before + 1));
    QVERIFY2(r.ok && r.message.isEmpty(), qPrintable(r.message));
    for (const QString &t : fs::list(QString("/proc/%1/task").arg(getpid())))
      QCOMPARE(getpriority(PRIO_PROCESS, id_t(t.toInt())), std::min(19, before + 1));
    r = actions::SetIoPriority(self, {2, 7});
    QVERIFY2(r.ok, qPrintable(r.message));
    QCOMPARE(actions::GetIoPriority(self).level, 7);
    r = actions::SetAffinity(self, actions::OnlineCpus());
    QVERIFY2(r.ok, qPrintable(r.message));
    int oom = 0;
    QVERIFY(actions::OomScoreAdj(self, &oom));
    r = actions::SetOomScoreAdj(self, std::min(1000, oom + 1));
    QVERIFY2(r.ok, qPrintable(r.message));
    QVERIFY(actions::OomScoreAdj(self, &oom) && oom >= 1);
    // The same PID with another start time is another process: refused, untouched.
    const ProcessKey reused{self.pid, self.start + 1};
    QVERIFY(!actions::SetNice(reused, 19).ok);
    QVERIFY(!actions::SetOomScoreAdj(reused, 1000).ok);
    QCOMPARE(getpriority(PRIO_PROCESS, 0), std::min(19, before + 1));
    stop = true;
    for (auto &t : pool) t.join();
  }

  void untrustedTextStaysText() {
    // argv is anyone's to set; as a tooltip it must not load /dev/zero or a FIFO.
    const QString evil = "<img src=\"/dev/zero\"><b>x</b>\nline two";
    QTextDocument doc;
    doc.setHtml(ui::safe::Tip(evil));
    QCOMPARE(doc.toPlainText().replace(QChar::LineSeparator, '\n').replace(QChar::ParagraphSeparator, '\n'), evil);
    QVERIFY(!doc.toHtml().contains("<img"));
    QVERIFY(ui::safe::Tip(QString()).isEmpty());
  }

  void luaQuoting() {
    QCOMPARE(hypr::LuaString("3"), QString("\"3\""));
    QCOMPARE(hypr::LuaString("a\"b\\c\nd"), QString("\"a\\\"b\\\\cd\""));
    // A focus dispatch never goes out for an address that is not 0x…hex.
    QVERIFY(!hypr::Focus("0x1\" }) hl.exec_cmd(\"x"));
    QVERIFY(!hypr::Close("active"));
  }
};

int main(int argc, char **argv) {
  QTemporaryDir config;
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  QApplication app(argc, argv);
  FeaturesTest t;
  return QTest::qExec(&t, argc, argv);
}

#include "features-test.moc"
