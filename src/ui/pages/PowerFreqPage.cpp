#include "ui/Safe.h"
#include "ui/pages/Pages.h"

#include "core/Fs.h"
#include "core/Settings.h"

#include <QComboBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QHBoxLayout>
#include <QAbstractItemView>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace td::ui {

namespace {

// power-profiles-daemon, under its current name and the original one.
struct ProfilesService {
  const char *service, *path, *iface;
};
constexpr ProfilesService kProfiles[] = {
    {"org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles", "org.freedesktop.UPower.PowerProfiles"},
    {"net.hadess.PowerProfiles", "/net/hadess/PowerProfiles", "net.hadess.PowerProfiles"}};

// Sensor tree with running Min/Max, as in HWiNFO.
class PowerFreqPage : public Page {
public:
  explicit PowerFreqPage(QWidget *parent) : Page(parent) {
    tree_ = new QTreeWidget;
    tree_->setHeaderLabels({tr("Sensor"), tr("Value"), tr("Min"), tr("Max")});
    tree_->header()->resizeSection(0, 380);
    for (int c = 1; c < 4; ++c) tree_->header()->resizeSection(c, 130);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(tree_);
  }

  QString Id() const override { return "power-and-freq"; }
  QString Title() const override { return tr("Power & Freq"); }
  bool WantsBackgroundUpdates() const override { return true; }  // keeps Min/Max honest

  void BuildHeader(QHBoxLayout *h) override {
    auto *label = new QLabel(tr("Profile"));
    label->setObjectName("Dim");
    h->addWidget(label);
    profile_ = new QComboBox;
    profile_->addItem(tr("Power saver"), "power-saver");
    profile_->addItem(tr("Balanced"), "balanced");
    profile_->addItem(tr("Performance"), "performance");
    profile_->setToolTip(tr("power-profiles-daemon profile, as in the Omarchy menu"));
    profile_->setEnabled(false);
    label->setBuddy(profile_);
    connect(profile_, &QComboBox::activated, this, [this] { SetProfile(profile_->currentData().toString()); });
    h->addWidget(profile_);
    auto *reset = new QPushButton(tr("Reset Min/&Max"));
    connect(reset, &QPushButton::clicked, this, [this] { stats_.clear(); });
    h->addWidget(reset);
  }

  void Update(const SnapshotPtr &s) override {
    Settings &st = Settings::Get();
    auto mhz = [](double v) { return QString::number(v / 1000, 'f', 2) + " GHz"; };
    auto temp = [&st](double v) { return st.FormatTemp(v, 1); };
    auto pct = [](double v) { return QString::number(v, 'f', 1) + "%"; };
    auto watts = [](double v) { return QString::number(v, 'f', 2) + " W"; };
    auto rpm = [](double v) { return QString::number(v, 'f', 0) + " RPM"; };
    auto volts = [](double v) { return QString::number(v, 'f', 3) + " V"; };

    seen_.clear();
    QString cpu = tr("CPU · %1").arg(s->cpuModel);
    Put({cpu, tr("Clocks")}, tr("Average effective"), s->cpuMhz, mhz);
    for (int i = 0; i < s->cores.size(); ++i)
      Put({cpu, tr("Clocks")}, tr("CPU %1").arg(i), s->cores[i].mhz, mhz);
    Put({cpu, tr("Utilization")}, tr("Total"), s->cpuUtil, pct);
    Put({cpu, tr("Utilization")}, tr("Kernel"), s->cpuKernel, pct);
    for (int i = 0; i < s->cores.size(); ++i)
      Put({cpu, tr("Utilization")}, tr("CPU %1").arg(i), s->cores[i].util, pct);
    if (s->power.cpuPackageW >= 0) Put({cpu, tr("Powers")}, tr("Package (RAPL)"), s->power.cpuPackageW, watts);

    for (const Sensor &sn : s->sensors) {
      QString group = sn.kind == "temp" ? tr("Temperatures") : sn.kind == "fan" ? tr("Fans")
                      : sn.kind == "power" ? tr("Powers") : sn.kind == "in" ? tr("Voltages")
                      : sn.kind == "curr" ? tr("Currents") : sn.kind;
      auto fmt = sn.kind == "temp" ? std::function<QString(double)>(temp) : sn.kind == "fan" ? std::function<QString(double)>(rpm)
                 : sn.kind == "power" ? std::function<QString(double)>(watts)
                 : sn.kind == "curr" ? std::function<QString(double)>([](double v) { return QString::number(v, 'f', 3) + " A"; })
                                     : std::function<QString(double)>(volts);
      Put({tr("Sensors · %1").arg(sn.chip), group}, sn.label, sn.value, fmt);
    }
    for (const GpuInfo &g : s->gpus) {
      if (g.util >= 0) Put({tr("GPU · %1").arg(g.name)}, tr("Utilization"), g.util, pct);
      if (g.mhz > 0) Put({tr("GPU · %1").arg(g.name)}, tr("Clock"), g.mhz * 1000, mhz);
      if (g.tempC >= 0) Put({tr("GPU · %1").arg(g.name)}, tr("Temperature"), g.tempC, temp);
    }
    if (s->power.hasBattery) {
      Put({tr("Battery")}, tr("Charge"), s->power.batteryPct, [](double v) { return QString::number(v, 'f', 0) + "%"; });
      if (s->power.batteryW >= 0) Put({tr("Battery")}, tr("Power"), s->power.batteryW, watts);
    }
    // Policy: strings, no min/max.
    QString cf = "/sys/devices/system/cpu/";
    Text({tr("Policy")}, tr("Frequency driver"), s->power.driver);
    Text({tr("Policy")}, tr("Governor"), s->power.governor);
    Text({tr("Policy")}, tr("Energy performance preference"), s->power.epp);
    Text({tr("Policy")}, tr("Energy performance bias (0 performance … 15 saving)"),
         fs::readText(cf + "cpu0/power/energy_perf_bias"));
    Text({tr("Policy")}, tr("Turbo"),
         fs::exists(cf + "intel_pstate/no_turbo") ? (fs::readInt(cf + "intel_pstate/no_turbo") == 1 ? tr("disabled") : tr("enabled"))
                                                  : fs::readText(cf + "cpufreq/boost"));
    Text({tr("Policy")}, tr("Platform profile"), fs::readText("/sys/firmware/acpi/platform_profile"));
    Text({tr("Policy")}, tr("Power source"), s->power.onAc ? tr("AC adapter") : tr("Battery"));
    if (s->power.raplRestricted)
      Text({tr("Policy")}, tr("RAPL energy counters"), tr("root-only (power access restricted)"));

    // What keeps the CPU from idling: the processes woken most often.
    QVector<const ProcessInfo *> wakers;
    for (const ProcessInfo &p : s->processes)
      if (p.wakeups > 0) wakers << &p;
    std::sort(wakers.begin(), wakers.end(), [](auto *a, auto *b) { return a->wakeups > b->wakeups; });
    auto perSec = [](double v) { return QString::number(v, 'f', 0) + QStringLiteral(" /s"); };
    for (int i = 0; i < std::min<qsizetype>(8, wakers.size()); ++i)
      Put({tr("Wakeups · busiest processes")}, QString("%1 (%2)").arg(wakers[i]->name).arg(wakers[i]->key.pid),
          wakers[i]->wakeups, perSec);

    if (profile_ && s->generation - profileAt_ >= 5) {
      profileAt_ = s->generation;
      ReadProfile();
    }

    // Rows that disappeared (a sensor went away) are removed.
    // Deepest first, so a parent is never deleted before the children it owns.
    QStringList gone;
    for (auto it = rows_.cbegin(); it != rows_.cend(); ++it)
      if (!seen_.contains(it.key())) gone << it.key();
    std::sort(gone.begin(), gone.end(), [](const QString &a, const QString &b) { return a.count('\x1f') > b.count('\x1f'); });
    for (const QString &k : gone) delete rows_.take(k);
    if (!expanded_) {
      tree_->expandToDepth(0);
      expanded_ = true;
    }
  }

private:
  struct Stat { double min, max; };

  void ReadProfile() {
    for (const ProfilesService &svc : kProfiles) {
      QDBusMessage m = QDBusMessage::createMethodCall(svc.service, svc.path, "org.freedesktop.DBus.Properties", "Get");
      m << QString(svc.iface) << QString("ActiveProfile");
      auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m, 1000), this);
      connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, svc] {
        w->deleteLater();
        QDBusPendingReply<QDBusVariant> r = *w;
        if (r.isError()) return;
        service_ = &svc;
        const int i = profile_->findData(r.value().variant().toString());
        profile_->setEnabled(true);
        if (i >= 0 && !profile_->view()->isVisible()) profile_->setCurrentIndex(i);
      });
    }
  }

  void SetProfile(const QString &name) {
    if (!service_) return;
    QDBusMessage m = QDBusMessage::createMethodCall(service_->service, service_->path, "org.freedesktop.DBus.Properties", "Set");
    m << QString(service_->iface) << QString("ActiveProfile") << QVariant::fromValue(QDBusVariant(name));
    m.setInteractiveAuthorizationAllowed(true);
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m, 30000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, name] {
      w->deleteLater();
      if (w->isError()) safe::warning(this, tr("Power profile"), tr("Could not switch to %1: %2").arg(name, w->error().message()));
      else emit Status(tr("Power profile: %1").arg(name));
      ReadProfile();
    });
  }

  QComboBox *profile_ = nullptr;
  const ProfilesService *service_ = nullptr;
  quint64 profileAt_ = 0;

  QTreeWidgetItem *Node(const QStringList &path) {
    QString key = path.join('\x1f');
    // Every ancestor counts as seen too, or the sweep below would delete it with its children.
    for (int n = 1; n <= path.size(); ++n) seen_.insert(path.mid(0, n).join('\x1f'));
    if (QTreeWidgetItem *it = rows_.value(key)) return it;
    QTreeWidgetItem *parent = path.size() > 1 ? Node(path.mid(0, path.size() - 1)) : nullptr;
    auto *it = parent ? new QTreeWidgetItem(parent, {path.last()}) : new QTreeWidgetItem(tree_, {path.last()});
    if (path.size() < 3) {
      QFont f = it->font(0);
      f.setBold(path.size() <= 1);
      it->setFont(0, f);
    }
    rows_.insert(key, it);
    return it;
  }

  void Put(QStringList path, const QString &label, double v, const std::function<QString(double)> &fmt) {
    path << label;
    QString key = path.join('\x1f');
    QTreeWidgetItem *it = Node(path);
    auto s = stats_.find(key);
    if (s == stats_.end()) s = stats_.insert(key, {v, v});
    s->min = std::min(s->min, v);
    s->max = std::max(s->max, v);
    it->setText(1, fmt(v));
    it->setText(2, fmt(s->min));
    it->setText(3, fmt(s->max));
  }

  void Text(QStringList path, const QString &label, const QString &value) {
    path << label;
    Node(path)->setText(1, value.isEmpty() ? QStringLiteral("—") : value);
  }

  QTreeWidget *tree_;
  QHash<QString, QTreeWidgetItem *> rows_;
  QHash<QString, Stat> stats_;
  QSet<QString> seen_;
  bool expanded_ = false;
};

}  // namespace

Page *CreatePowerFreqPage(QWidget *parent) { return new PowerFreqPage(parent); }

}  // namespace td::ui
