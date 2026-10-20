#include "ui/pages/Pages.h"

#include "core/Fs.h"
#include "core/Settings.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <sys/utsname.h>

namespace td::ui {

namespace {

using Rows = QVector<QPair<QString, QString>>;

// One overview like every other page: each category is a section of the
// table (shown as cards), so the whole system reads top to bottom and the
// filter narrows it all at once.
class SystemInfoPage : public Page {
public:
  explicit SystemInfoPage(QWidget *parent) : Page(parent) {
    table_ = new QTreeWidget;
    table_->setHeaderLabels({tr("Property"), tr("Value")});
    table_->header()->resizeSection(0, 260);
    for (const QString &name : Categories()) {
      auto *g = new QTreeWidgetItem(table_, {name});
      g->setFlags(Qt::ItemIsEnabled);
      QFont f = g->font(0);
      f.setBold(true);
      g->setFont(0, f);
      g->setExpanded(true);
    }
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(table_);
  }

  QString Id() const override { return "system-info"; }
  QString Title() const override { return tr("System Info"); }
  void FocusFilter() override { filter_->setFocus(); }

  void BuildHeader(QHBoxLayout *h) override {
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter info"));
    filter_->setClearButtonEnabled(true);
    connect(filter_, &QLineEdit::textChanged, this, [this] {
      if (snap_) Update(snap_);
    });
    h->addWidget(filter_);
    auto *copy = new QPushButton(tr("&Copy"));
    copy->setToolTip(tr("Copy what is shown as text"));
    connect(copy, &QPushButton::clicked, this, [this] {
      if (!snap_) return;  // nothing sampled yet
      QString out;
      int rows = 0;
      for (int i = 0; i < table_->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *g = table_->topLevelItem(i);
        if (g->isHidden()) continue;
        out += (out.isEmpty() ? "" : "\n") + Categories()[i] + '\n';
        for (int k = 0; k < g->childCount(); ++k, ++rows)
          out += "  " + g->child(k)->text(0) + ": " + g->child(k)->text(1) + '\n';
      }
      QApplication::clipboard()->setText(out);
      emit Status(tr("Copied %1 rows").arg(rows));
    });
    h->addWidget(copy);
  }

  void Update(const SnapshotPtr &s) override {
    snap_ = s;
    const QString f = filter_ ? filter_->text().trimmed() : QString();
    const QStringList names = Categories();
    for (int c = 0; c < names.size(); ++c) {
      Rows rows = RowsFor(Category(c), *s);
      // A category whose name matches keeps all its rows.
      if (!f.isEmpty() && !names[c].contains(f, Qt::CaseInsensitive))
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&f](const QPair<QString, QString> &r) {
                     return !r.first.contains(f, Qt::CaseInsensitive) && !r.second.contains(f, Qt::CaseInsensitive);
                   }), rows.end());
      QTreeWidgetItem *g = table_->topLevelItem(c);
      g->setHidden(rows.isEmpty());
      while (g->childCount() > rows.size()) delete g->takeChild(g->childCount() - 1);
      for (int i = 0; i < rows.size(); ++i) {
        QTreeWidgetItem *it = g->child(i);
        if (!it) it = new QTreeWidgetItem(g);
        it->setText(0, rows[i].first);
        it->setText(1, rows[i].second);
      }
    }
  }

private:
  enum Category { Overview, Processor, Memory, Graphics, Storage, Power, Network, OperatingSystem, Providers, SnapshotInfo };
  static QStringList Categories() {
    return {tr("Hardware Overview"), tr("Processor"), tr("Memory"), tr("Graphics"), tr("Storage"), tr("Power"),
            tr("Network"), tr("Operating System"), tr("Native Providers"), tr("Snapshot")};
  }

  static QString Dmi(const char *f) { return fs::readText(QString("/sys/class/dmi/id/") + f); }
  // "vendor name", or nothing when neither is known (no DMI on ARM and some VMs).
  static QString Dmi(const char *vendor, const char *name) { return (Dmi(vendor) + ' ' + Dmi(name)).trimmed(); }

  // /proc/mounts writes space, tab, newline and backslash as \040, \011, \012, \134.
  static QString Unescape(const QString &field) {
    QString out;
    for (int i = 0; i < field.size(); ++i) {
      bool ok = false;
      const int c = field[i] == '\\' && i + 3 < field.size() ? field.mid(i + 1, 3).toInt(&ok, 8) : 0;
      if (ok) {
        // A newline would split the card and the copied text: show it as a space.
        out += c == '\n' ? QChar(' ') : QChar(c);
        i += 3;
      } else {
        out += field[i];
      }
    }
    return out;
  }

  Rows RowsFor(Category cat, const Snapshot &s) {
    Settings &st = Settings::Get();
    Rows r;
    if (cat == Overview) {
      r << qMakePair(tr("Computer"), Dmi("sys_vendor", "product_name"))
        << qMakePair(tr("Processor"), s.cpuModel)
        << qMakePair(tr("Logical processors"), QString::number(s.logical))
        << qMakePair(tr("Physical cores"), QString::number(s.physicalCores))
        << qMakePair(tr("Sockets"), QString::number(s.sockets))
        << qMakePair(tr("Memory"), fs::humanBytes(s.memTotal));
      for (const GpuInfo &g : s.gpus) r << qMakePair(tr("Graphics"), g.name);
      r << qMakePair(tr("Distribution"), s.distro) << qMakePair(tr("Kernel"), s.kernel)
        << qMakePair(tr("Virtual machine"), s.virtualMachine ? tr("Yes") : tr("No"))
        << qMakePair(tr("Time since boot"), fs::humanDuration(s.uptimeSec));
    } else if (cat == Processor) {
      r << qMakePair(tr("Model"), s.cpuModel) << qMakePair(tr("Base speed"), QString("%1 MHz").arg(s.baseMhz))
        << qMakePair(tr("Max speed"), QString("%1 MHz").arg(s.maxMhz))
        << qMakePair(tr("Current speed"), QString("%1 MHz").arg(qRound(s.cpuMhz)))
        << qMakePair(tr("Frequency driver"), s.power.driver) << qMakePair(tr("Governor"), s.power.governor)
        << qMakePair(tr("Energy preference"), s.power.epp)
        << qMakePair(tr("Virtualization"), s.virtualization ? tr("VT-x / AMD-V") : tr("Not reported"))
        << qMakePair(tr("Package temperature"), st.FormatTemp(s.cpuTemp, 1));
      for (int i = 0; i < s.cores.size(); ++i)
        r << qMakePair(tr("CPU %1").arg(i), QString("%1 MHz · %2%").arg(qRound(s.cores[i].mhz)).arg(s.cores[i].util, 0, 'f', 0));
    } else if (cat == Memory) {
      r << qMakePair(tr("Installed"), fs::humanBytes(s.memTotal)) << qMakePair(tr("In use"), fs::humanBytes(s.memUsed()))
        << qMakePair(tr("Available"), fs::humanBytes(s.memAvailable)) << qMakePair(tr("Cached"), fs::humanBytes(s.memCached))
        << qMakePair(tr("Swap"), fs::humanBytes(s.swapTotal - s.swapFree) + " / " + fs::humanBytes(s.swapTotal));
      for (const QString &sw : QString::fromStdString(fs::readRaw("/proc/swaps")).split('\n').mid(1))
        if (!sw.trimmed().isEmpty()) r << qMakePair(tr("Swap device"), sw.simplified());
    } else if (cat == Graphics) {
      for (const GpuInfo &g : s.gpus)
        r << qMakePair(g.card, g.name) << qMakePair(g.card + ' ' + tr("driver"), g.driver);
      r << qMakePair(tr("Session type"), qEnvironmentVariable("XDG_SESSION_TYPE"))
        << qMakePair(tr("Desktop"), qEnvironmentVariable("XDG_CURRENT_DESKTOP"));
    } else if (cat == Storage) {
      for (const DiskInfo &d : s.disks)
        r << qMakePair("/dev/" + d.name, QString("%1 · %2").arg(d.model, fs::humanBytes(d.size)));
      for (const QString &m : QString::fromStdString(fs::readRaw("/proc/mounts")).split('\n')) {
        QStringList f = m.split(' ');
        if (f.size() > 2 && f[0].startsWith("/dev/")) r << qMakePair(Unescape(f[1]), Unescape(f[0]) + " (" + f[2] + ")");
      }
    } else if (cat == Power) {
      r << qMakePair(tr("Source"), s.power.onAc ? tr("AC adapter") : tr("Battery"));
      if (s.power.hasBattery)
        r << qMakePair(tr("Battery"), QString("%1% · %2").arg(s.power.batteryPct).arg(s.power.batteryStatus));
      r << qMakePair(tr("CPU package power"), s.power.cpuPackageW >= 0 ? QString::number(s.power.cpuPackageW, 'f', 1) + " W"
                                                                         : s.power.raplRestricted ? tr("Power access restricted") : tr("Unavailable"));
    } else if (cat == Network) {
      r << qMakePair(tr("Host name"), s.hostname) << qMakePair(tr("Primary address"), s.primaryIp);
      for (const NetInfo &n : s.nets)
        r << qMakePair(n.name, QString("%1 · %2%3").arg(n.up ? tr("up") : tr("down"), n.ipv4.isEmpty() ? "—" : n.ipv4,
                                                        n.physical ? QString() : tr(" · virtual")));
    } else if (cat == OperatingSystem) {
      utsname u{};
      uname(&u);
      r << qMakePair(tr("Distribution"), s.distro) << qMakePair(tr("Kernel"), QString::fromLatin1(u.release))
        << qMakePair(tr("Kernel build"), QString::fromLatin1(u.version)) << qMakePair(tr("Architecture"), QString::fromLatin1(u.machine))
        << qMakePair(tr("Host name"), s.hostname);
      for (const auto &[label, value] : {qMakePair(tr("Firmware"), Dmi("bios_vendor", "bios_version")),
                                         qMakePair(tr("Board"), Dmi("board_vendor", "board_name"))})
        if (!value.isEmpty()) r << qMakePair(label, value);
      r << qMakePair(tr("Qt"), QString::fromLatin1(qVersion()));
    } else if (cat == Providers) {
      auto ok = [](const QString &p) { return fs::exists(p) ? tr("available") : tr("missing"); };
      r << qMakePair("/proc/stat", ok("/proc/stat")) << qMakePair("/proc/pressure", ok("/proc/pressure/memory"))
        << qMakePair("/sys/class/hwmon", ok("/sys/class/hwmon")) << qMakePair("/sys/class/drm", ok("/sys/class/drm"))
        << qMakePair("/sys/class/powercap (RAPL)", s.power.raplRestricted ? tr("root-only") : ok("/sys/class/powercap/intel-rapl:0"))
        << qMakePair(tr("Hyprland IPC"), qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE") ? tr("available") : tr("not running"))
        << qMakePair(tr("systemd (D-Bus)"), ok("/run/systemd/system"));
    } else if (cat == SnapshotInfo) {
      r << qMakePair(tr("Generation"), QString::number(s.generation))
        << qMakePair(tr("Taken"), s.time.toString(Qt::ISODateWithMs))
        << qMakePair(tr("Interval"), QString("%1 s").arg(s.intervalSec, 0, 'f', 3))
        << qMakePair(tr("Processes"), QString::number(s.processes.size()))
        << qMakePair(tr("Threads"), QString::number(s.threadCount))
        << qMakePair(tr("Sensors"), QString::number(s.sensors.size()));
    }
    return r;
  }

  QTreeWidget *table_;
  QLineEdit *filter_ = nullptr;
  SnapshotPtr snap_;
};

}  // namespace

Page *CreateSystemInfoPage(QWidget *parent) { return new SystemInfoPage(parent); }

}  // namespace td::ui
