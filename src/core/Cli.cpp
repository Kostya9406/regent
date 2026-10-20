#include "core/Cli.h"

#include "core/Actions.h"
#include "core/Fs.h"
#include "core/Hyprland.h"
#include "core/Sampler.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <unistd.h>

namespace td::cli {

namespace {

void Out(const QString &s) { std::fputs(qPrintable(s), stdout); }
void Err(const QString &s) { std::fputs(qPrintable(s), stderr); }

// Two samples half a second apart, so rates (CPU, wakeups) are real.
SnapshotPtr Sample() {
  Sampler sampler;
  sampler.Collect();
  QThread::msleep(500);
  return sampler.Collect();
}

QString Pad(const QString &s, int width, bool right = false) {
  QString t = s.left(width);
  return right ? t.rightJustified(width) : t.leftJustified(width);
}

int Top(const QStringList &args) {
  int n = 15;
  QString sort = "cpu";
  for (int i = 0; i < args.size(); ++i) {
    if ((args[i] == "-n" || args[i] == "--count") && i + 1 < args.size()) n = std::clamp(args[++i].toInt(), 1, 500);
    else if (args[i].startsWith("-n") && args[i].size() > 2) n = std::clamp(args[i].mid(2).toInt(), 1, 500);
    else if (args[i] == "--sort" && i + 1 < args.size()) sort = args[++i];
    else {
      Err(QString("regent top: unknown option %1\nusage: regent top [-n N] [--sort cpu|mem|gpu|wakeups]\n").arg(args[i]));
      return 2;
    }
  }
  if (!QStringList{"cpu", "mem", "gpu", "wakeups"}.contains(sort)) {
    Err("regent top: --sort takes cpu, mem, gpu or wakeups\n");
    return 2;
  }
  SnapshotPtr s = Sample();
  QVector<const ProcessInfo *> v;
  for (const ProcessInfo &p : s->processes) v << &p;
  auto key = [&](const ProcessInfo *p) {
    return sort == "mem" ? double(p->rss) : sort == "gpu" ? p->gpu : sort == "wakeups" ? p->wakeups : p->cpu;
  };
  std::stable_sort(v.begin(), v.end(), [&](auto *a, auto *b) { return key(a) > key(b); });
  if (v.size() > n) v.resize(n);
  Out(QString("CPU %1%  ·  memory %2 / %3  ·  %4 processes\n\n")
          .arg(s->cpuUtil, 0, 'f', 1)
          .arg(fs::humanBytes(s->memUsed()), fs::humanBytes(s->memTotal))
          .arg(s->processes.size()));
  Out(Pad("PID", 8, true) + "  " + Pad("CPU%", 6, true) + "  " + Pad("MEMORY", 9, true) + "  " + Pad("GPU%", 5, true) +
      "  " + Pad("WAKE/S", 7, true) + "  " + "NAME\n");
  for (const ProcessInfo *p : v)
    Out(Pad(QString::number(p->key.pid), 8, true) + "  " + Pad(QString::number(p->cpu, 'f', 1), 6, true) + "  " +
        Pad(fs::humanBytes(p->rss), 9, true) + "  " + Pad(p->gpu < 0 ? "—" : QString::number(p->gpu, 'f', 0), 5, true) +
        "  " + Pad(p->wakeups < 0 ? "—" : QString::number(p->wakeups, 'f', 0), 7, true) + "  " + p->name +
        (p->windowTitle.isEmpty() ? QString() : "  · " + p->windowTitle.left(50)) + "\n");
  return 0;
}

// Which processes a NAME or PID means. A name matches the process name, or the
// file name of the first word of its command line ("python3 script.py" → python3).
QVector<ProcessInfo> Resolve(const Snapshot &s, const QString &what) {
  QVector<ProcessInfo> out;
  bool isPid = false;
  const int pid = what.toInt(&isPid);
  for (const ProcessInfo &p : s.processes) {
    if (isPid ? p.key.pid == pid
              : p.name == what || QFileInfo(p.cmdline.section(' ', 0, 0)).fileName() == what)
      out << p;
  }
  return out;
}

int Kill(const QStringList &args) {
  bool force = false, all = false;
  QStringList targets;
  for (const QString &a : args) {
    if (a == "--force" || a == "-9") force = true;
    else if (a == "--all") all = true;
    else if (a.startsWith('-')) {
      Err(QString("regent kill: unknown option %1\n").arg(a));
      return 2;
    } else targets << a;
  }
  if (targets.isEmpty()) {
    Err("usage: regent kill NAME|PID… [--force] [--all]\n"
        "  SIGTERM by default; --force sends SIGKILL; --all is needed when a name matches several processes.\n");
    return 2;
  }
  Sampler sampler;
  sampler.SetDetail(false);  // names and PIDs are all kill needs
  SnapshotPtr s = sampler.Collect();
  int failures = 0;
  for (const QString &t : targets) {
    QVector<ProcessInfo> hits = Resolve(*s, t);
    hits.erase(std::remove_if(hits.begin(), hits.end(), [](const ProcessInfo &p) {
                 return p.key.pid <= 1 || p.key.pid == getpid();
               }), hits.end());
    if (hits.isEmpty()) {
      Err(QString("regent kill: no process %1\n").arg(t));
      ++failures;
      continue;
    }
    if (hits.size() > 1 && !all) {
      Err(QString("regent kill: %1 matches %2 processes; pass --all or a PID:\n").arg(t).arg(hits.size()));
      for (const ProcessInfo &p : hits)
        Err(QString("  %1  %2  %3\n").arg(p.key.pid, 7).arg(p.name, p.cmdline.left(80)));
      ++failures;
      continue;
    }
    for (const ProcessInfo &p : hits) {
      actions::Result r = actions::Signal(p.key, force ? SIGKILL : SIGTERM);
      if (r.ok) Out(QString("%1 %2 (%3)\n").arg(force ? "killed" : "ended", p.name).arg(p.key.pid));
      else {
        Err(QString("regent kill: %1 (%2): %3\n").arg(p.name).arg(p.key.pid).arg(r.message));
        ++failures;
      }
    }
  }
  return failures ? 1 : 0;
}

// One choice from a list, in Omarchy's menu when it is there (rows as
// "glyph<TAB>label<TAB>subtext"), otherwise in a dmenu-style launcher.
// Returns the row index, or -1 when dismissed.
// Process names and window titles are anyone's to choose: a tab or newline in
// them must not split a menu row or start a new one.
QString Clean(QString s) {
  for (QChar &c : s)
    if (c.category() == QChar::Other_Control || c == QChar::LineSeparator || c == QChar::ParagraphSeparator) c = ' ';
  return s;
}

int Pick(const QString &prompt, QVector<QStringList> rows) {
  for (QStringList &r : rows)
    for (QString &field : r) field = Clean(field);
  auto line = [](const QStringList &r) { return r.value(2).isEmpty() ? r.value(1) : r.value(1) + "  ·  " + r.value(2); };
  QString program;
  QStringList argv;
  const QString omarchy = QStandardPaths::findExecutable("omarchy-menu-select");
  if (!omarchy.isEmpty()) {
    program = omarchy;
    argv << prompt;
    for (const QStringList &r : rows) argv << r.join('\t');
    argv << "--" << "--width" << "760";
  } else {
    for (const auto &[exe, args] : {std::pair{"walker", QStringList{"--dmenu", "-p", prompt}},
                                    std::pair{"fuzzel", QStringList{"--dmenu", "-p", prompt + ": "}},
                                    std::pair{"wofi", QStringList{"--dmenu", "-p", prompt}},
                                    std::pair{"rofi", QStringList{"-dmenu", "-i", "-p", prompt}}}) {
      program = QStandardPaths::findExecutable(exe);
      if (!program.isEmpty()) {
        argv = args;
        break;
      }
    }
    if (program.isEmpty()) {
      Err("regent menu: needs omarchy-menu-select, walker, fuzzel, wofi or rofi\n");
      return -2;
    }
  }
  QProcess p;
  p.start(program, argv);
  if (!p.waitForStarted(5000)) return -2;
  if (omarchy.isEmpty()) {
    for (const QStringList &r : rows) p.write((line(r) + '\n').toUtf8());
  }
  p.closeWriteChannel();
  p.waitForFinished(-1);
  const QString chosen = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
  if (chosen.isEmpty()) return -1;
  for (int i = 0; i < rows.size(); ++i) {
    // Omarchy returns "label<TAB>subtext"; a dmenu returns the line it was given.
    if (chosen == rows[i].value(1) + '\t' + rows[i].value(2) || chosen == line(rows[i])) return i;
  }
  return -1;
}

int Menu() {
  SnapshotPtr s = Sample();
  QVector<const ProcessInfo *> windowed, others;
  for (const ProcessInfo &p : s->processes) {
    if (p.key.pid == getpid()) continue;
    (p.hasWindow ? windowed : others) << &p;
  }
  auto busiest = [](auto *a, auto *b) { return a->cpu != b->cpu ? a->cpu > b->cpu : a->rss > b->rss; };
  std::sort(windowed.begin(), windowed.end(), busiest);
  std::sort(others.begin(), others.end(), busiest);
  if (others.size() > 60) others.resize(60);
  QVector<const ProcessInfo *> list = windowed + others;
  QVector<QStringList> rows;
  for (const ProcessInfo *p : list) {
    QString sub = QString("PID %1 · %2% CPU · %3").arg(p->key.pid).arg(p->cpu, 0, 'f', 1).arg(fs::humanBytes(p->rss));
    if (p->hasWindow) sub += QString(" · workspace %1 · %2").arg(p->workspace, p->windowTitle.left(60));
    rows << QStringList{QString::fromUcs4(p->hasWindow ? U"\U000F08C6" : U"\U000F0493"), p->name, sub};
  }
  const int chosen = Pick("Processes", rows);
  if (chosen < 0) return chosen == -1 ? 0 : 1;
  const ProcessInfo p = *list[chosen];

  struct Act {
    char32_t glyph;
    QString label;
    int id;
  };
  QVector<Act> acts;
  if (p.hasWindow) acts << Act{U'\U000F05AF', "Focus window", 0};
  acts << Act{U'\U000F0208', "Show in Regent", 1};
  // The compositor, shell or polkit agent: only from Regent's window, with a warning there.
  const bool critical = actions::CriticalProcess(p.key.pid);
  if (!critical) {
    acts << Act{U'\U000F0159', "End task", 2};
    acts << Act{U'\U000F068C', "Kill now", 3};
  }
  if (p.state == 'T') acts << Act{U'\U000F040A', "Resume", 5};
  else if (!critical) acts << Act{U'\U000F03E4', "Suspend", 4};
  QVector<QStringList> actionRows;
  for (const Act &a : acts) actionRows << QStringList{QString::fromUcs4(&a.glyph, 1), a.label, QString()};
  // Rows without subtext come back as the bare label.
  for (QStringList &r : actionRows) r.removeLast();
  const int a = Pick(QString("%1 · PID %2").arg(p.name).arg(p.key.pid), actionRows);
  if (a < 0) return a == -1 ? 0 : 1;
  actions::Result r{true, {}};
  switch (acts[a].id) {
    case 0: r.ok = hypr::Focus(p.windowAddress); if (!r.ok) r.message = "the window is gone"; break;
    case 1:
      QProcess::startDetached(QCoreApplication::applicationFilePath(),
                              {"--page", "processes", "--select-pid", QString::number(p.key.pid)});
      break;
    case 2: r = actions::Signal(p.key, SIGTERM); break;
    case 3: r = actions::Signal(p.key, SIGKILL); break;
    case 4: r = actions::Signal(p.key, SIGSTOP); break;
    case 5: r = actions::Signal(p.key, SIGCONT); break;
  }
  if (!r.ok) {
    Err(QString("regent menu: %1: %2\n").arg(p.name, r.message));
    const QString notify = QStandardPaths::findExecutable("omarchy-notification-send");
    // The body may carry markup for the notification daemon: escape the process name.
    if (!notify.isEmpty())
      QProcess::startDetached(notify, {"-g", QString::fromUcs4(U"\U000F0028"), acts[a].label + " failed",
                                       (p.name + ": " + r.message).toHtmlEscaped()});
    return 1;
  }
  return 0;
}

}  // namespace

QJsonObject SelectFields(const QJsonObject &full, const QStringList &fields) {
  static const QHash<QString, QString> alias = {{"mem", "memory"}, {"temp", "cpu.tempC"}, {"net", "network"},
                                                {"gpu", "gpus"},    {"top", "topProcesses"}, {"procs", "processCount"}};
  QJsonObject out;
  for (QString f : fields) {
    f = f.trimmed();
    if (f.isEmpty()) continue;
    // An alias also stands for the first part of a path: mem.percent → memory.percent.
    QString path = alias.value(f, f);
    if (path == f && f.contains('.')) {
      const QString head = f.section('.', 0, 0);
      if (alias.contains(head) && !alias.value(head).contains('.')) path = alias.value(head) + f.mid(head.size());
    }
    QJsonValue v = full;
    for (const QString &part : path.split('.')) v = v.isObject() ? v.toObject().value(part) : QJsonValue();
    out.insert(f, v.isUndefined() ? QJsonValue() : v);
  }
  return out;
}

int Run(const QStringList &args) {
  if (args.isEmpty()) return -1;
  const QString cmd = args.first();
  if (cmd == "top") return Top(args.mid(1));
  if (cmd == "kill") return Kill(args.mid(1));
  if (cmd == "menu") return args.size() == 1 ? Menu() : (Err("usage: regent menu\n"), 2);
  return -1;
}

}  // namespace td::cli
