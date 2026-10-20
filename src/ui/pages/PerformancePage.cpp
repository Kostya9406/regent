#include "ui/pages/Pages.h"

#include "core/Fs.h"
#include "core/Settings.h"
#include "ui/OmarchyPalette.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QRegularExpression>
#include <QGridLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace td::ui {

namespace {

enum class Kind { Cpu, Memory, Energy, Thermals, Disk, Net, Gpu };

struct Device {
  QString key;
  Kind kind;
  int index = 0;
  theme::Ink color;
  QWidget *row = nullptr;
  QLabel *rowTitle = nullptr, *rowLine1 = nullptr, *rowLine2 = nullptr;
  GraphWidget *mini = nullptr;
  QWidget *detail = nullptr;
  QLabel *title = nullptr, *subtitle = nullptr, *headline = nullptr;
  GraphWidget *big = nullptr, *big2 = nullptr;
  QLabel *stats = nullptr;
  QWidget *coreGrid = nullptr;
  QVector<GraphWidget *> cores;
  QStackedWidget *cpuViews = nullptr;
  QStringList statRows;  // last stats, re-laid out at once when the pane crosses the column width
  int statColumns = 2;
};

QString Row(const QString &k, const QString &v) {
  return QString("<tr><td style='color:%3; padding-right:18px'>%1</td><td style='color:%4'>%2</td></tr>")
      .arg(k, v.toHtmlEscaped(), theme::Dim().name(), theme::Bright().name());  // values come from sysfs and drivers
}

QString Table(const QStringList &rows, int columns = 2) {
  // Lay key/value rows out in `columns` side-by-side tables.
  QString html = "<table cellspacing='0' cellpadding='3' width='100%'><tr>";
  int per = int(std::ceil(rows.size() / double(columns)));
  for (int c = 0; c < columns; ++c) {
    html += "<td valign='top'><table cellpadding='3'>";
    for (int i = c * per; i < std::min<int>(int(rows.size()), (c + 1) * per); ++i) html += rows[i];
    html += "</table></td>";
  }
  return html + "</tr></table>";
}

class PerformancePage : public Page {
public:
  explicit PerformancePage(QWidget *parent) : Page(parent) {
    auto *split = new QSplitter;
    split->setObjectName("PerformanceSplit");
    split->setChildrenCollapsible(false);
    list_ = new QListWidget;
    list_->setObjectName("PerformanceDevices");
    list_->setMinimumWidth(200);
    list_->setMaximumWidth(420);
    list_->setSpacing(2);
    // Rows follow the list's width; a narrow list drops the mini graphs, not the text.
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->viewport()->installEventFilter(this);
    auto listStyle = [this] {
      list_->setStyleSheet("QListWidget::item { border-left: 3px solid transparent; }"
                           "QListWidget::item:selected { border-left: 3px solid " + theme::Accent().name() + "; }");
    };
    listStyle();
    connect(&OmarchyPalette::Get(), &OmarchyPalette::Changed, this, listStyle);
    details_ = new QStackedWidget;
    details_->setObjectName("PerformanceDetails");
    details_->setProperty("primaryGraphic",true);
    details_->setMinimumWidth(0);
    details_->installEventFilter(this);
    split->addWidget(list_);
    split->addWidget(details_);
    split->setStretchFactor(1, 1);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(8, 8, 8, 0);
    l->addWidget(split);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int r) {
      if (r >= 0 && r < devices_.size()) details_->setCurrentWidget(devices_[r].detail);
    });

    menu_ = new QMenu(tr("&Performance"), this);
    QAction *copy = menu_->addAction(tr("&Copy details"));
    connect(copy, &QAction::triggered, this, [this] {
      int r = list_->currentRow();
      if (r >= 0) QApplication::clipboard()->setText(devices_[r].stats->text().remove(QRegularExpression("<[^>]*>")));
    });
  }

  QString Id() const override { return "performance"; }
  QString Title() const override { return tr("Performance"); }
  QMenu *PageMenu() override { return menu_; }
  bool WantsBackgroundUpdates() const override { return true; }

  void Update(const SnapshotPtr &s) override {
    EnsureDevices(*s);
    bool fresh = s->generation != lastGeneration_;
    lastGeneration_ = s->generation;
    // From the nominal interval: a manual refresh or timer jitter must not resize the history.
    int cap = std::max(10, int(Settings::Get().graphSeconds() / Settings::Get().historyIntervalSec()));
    for (Device &d : devices_) Fill(d, *s, fresh, cap);
  }

private:
  QString KeyList(const Snapshot &s) const {
    QStringList k{"cpu", "mem", "energy", "thermals"};
    for (const DiskInfo &d : s.disks) k << "disk:" + d.name;
    for (const NetInfo &n : s.nets)
      if (n.physical) k << "net:" + n.name;
    for (const GpuInfo &g : s.gpus) k << "gpu:" + g.card;
    return k.join(',');
  }

  void EnsureDevices(const Snapshot &s) {
    QString keys = KeyList(s);
    if (keys == keys_) return;
    keys_ = keys;
    QString selected = list_->currentRow() >= 0 ? devices_[list_->currentRow()].key : QString();
    list_->clear();
    for (Device &d : devices_) delete d.detail;
    devices_.clear();
    auto add = [&](QString key, Kind kind, int index, theme::Ink color) {
      Device d;
      d.key = std::move(key);
      d.kind = kind;
      d.index = index;
      d.color = color;
      Build(d, s);
      devices_ << d;
    };
    add("cpu", Kind::Cpu, 0, theme::Cpu);
    add("mem", Kind::Memory, 0, theme::Memory);
    add("energy", Kind::Energy, 0, theme::Energy);
    add("thermals", Kind::Thermals, 0, theme::Temp);
    for (int i = 0; i < s.disks.size(); ++i) add("disk:" + s.disks[i].name, Kind::Disk, i, theme::Disk);
    for (int i = 0; i < s.nets.size(); ++i)
      if (s.nets[i].physical) add("net:" + s.nets[i].name, Kind::Net, i, theme::Net);
    for (int i = 0; i < s.gpus.size(); ++i) add("gpu:" + s.gpus[i].card, Kind::Gpu, i, theme::Gpu);
    int sel = 0;
    for (int i = 0; i < devices_.size(); ++i)
      if (devices_[i].key == selected) sel = i;
    list_->setCurrentRow(sel);
  }

  static constexpr int kMiniGraphListWidth = 300;
  bool narrowStats_ = false;

  bool eventFilter(QObject *watched, QEvent *e) override {
    if (watched == details_ && e->type() == QEvent::Resize && NarrowStats() != narrowStats_) {
      // Re-lay the stats out now, not at the next sample: a pane that was just
      // narrowed would otherwise show the clipped two-column table for a second.
      narrowStats_ = NarrowStats();
      for (Device &d : devices_)
        if (d.stats && !d.statRows.isEmpty()) d.stats->setText(Table(d.statRows, narrowStats_ ? 1 : d.statColumns));
    }
    if (watched == list_->viewport() && e->type() == QEvent::Resize) {
      const bool graphs = list_->viewport()->width() >= kMiniGraphListWidth;
      for (Device &d : devices_)
        if (d.mini) d.mini->setVisible(graphs);
    }
    return Page::eventFilter(watched, e);
  }

  // Two key/value columns side by side only where both fit; a narrow pane stacks them.
  bool NarrowStats() const { return details_->width() < 560; }
  QString StatsTable(Device &d, const QStringList &rows, int columns = 2) const {
    d.statRows = rows;
    d.statColumns = columns;
    return Table(rows, NarrowStats() ? 1 : columns);
  }

  void Build(Device &d, const Snapshot &s) {
    // List row: title, two lines, mini graph.
    d.row = new QWidget;
    auto *rl = new QGridLayout(d.row);
    rl->setContentsMargins(10, 8, 10, 8);
    d.rowTitle = new QLabel;
    d.rowTitle->setFont(theme::Mono(13, true));
    d.rowLine1 = new QLabel;
    d.rowLine1->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    d.rowLine1->setObjectName("Dim");
    d.rowLine2 = new QLabel;
    d.rowLine2->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    d.rowLine2->setObjectName("Dim");
    d.mini = new GraphWidget;
    d.mini->SetCompact(true);
    d.mini->AddSeries(d.color);
    if (d.kind == Kind::Disk || d.kind == Kind::Net) {
      d.mini->AddSeries(d.color.Lighter(150), false);
      d.mini->SetAutoMax(d.kind == Kind::Net);
    }
    if (d.kind == Kind::Thermals) d.mini->SetMax(110);
    d.mini->setFixedSize(110, 70);
    rl->addWidget(d.rowTitle, 0, 0);
    rl->addWidget(d.rowLine1, 1, 0);
    rl->addWidget(d.rowLine2, 2, 0);
    rl->addWidget(d.mini, 0, 1, 3, 1);
    rl->setColumnStretch(0, 1);
    auto *item = new QListWidgetItem(list_);
    item->setSizeHint(QSize(0, 86));
    d.mini->setVisible(list_->viewport()->width() >= kMiniGraphListWidth);
    list_->setItemWidget(item, d.row);

    // Detail pane.
    d.detail = new QWidget;
    auto *dv = new QVBoxLayout(d.detail);
    dv->setContentsMargins(20, 10, 20, 10);
    auto *head = new QHBoxLayout;
    d.title = new QLabel;
    d.title->setFont(theme::Mono(26, true));
    d.subtitle = new QLabel;
    d.subtitle->setObjectName("Dim");
    d.subtitle->setFont(theme::Mono(13));
    d.subtitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    head->addWidget(d.title);
    head->addStretch(1);
    head->addWidget(d.subtitle);
    dv->addLayout(head);
    d.headline = new QLabel;
    d.headline->setObjectName("Dim");
    d.headline->setWordWrap(true);
    dv->addWidget(d.headline);

    d.big = new GraphWidget;
    d.big->AddSeries(d.color);
    d.big->setMinimumHeight(220);
    if (d.kind == Kind::Cpu) {
      d.big->AddSeries(theme::Kernel, false);
      auto *viewBox = new QHBoxLayout;
      viewBox->addStretch(1);
      auto *mode = new QComboBox;
      mode->addItems({tr("Overall utilization"), tr("Logical processors")});
      viewBox->addWidget(mode);
      dv->addLayout(viewBox);
      d.cpuViews = new QStackedWidget;
      d.cpuViews->addWidget(d.big);
      d.coreGrid = new QWidget;
      auto *g = new QGridLayout(d.coreGrid);
      g->setSpacing(4);
      g->setContentsMargins(0, 0, 0, 0);
      int n = int(s.cores.size());
      int cols = n <= 4 ? n : n <= 16 ? 4 : 8;
      for (int i = 0; i < n; ++i) {
        auto *cg = new GraphWidget;
        cg->AddSeries(theme::Cpu);
        cg->SetLabels(QString("CPU %1").arg(i), QString(), QString(), QString());
        g->addWidget(cg, i / std::max(1, cols), i % std::max(1, cols));
        d.cores << cg;
      }
      d.cpuViews->addWidget(d.coreGrid);
      dv->addWidget(d.cpuViews, 1);
      connect(mode, &QComboBox::currentIndexChanged, d.cpuViews, &QStackedWidget::setCurrentIndex);
      mode->setCurrentIndex(1);
    } else {
      if (d.kind == Kind::Thermals) d.big->SetMax(110);
      dv->addWidget(d.big, 1);
      if (d.kind == Kind::Disk || d.kind == Kind::Net) {
        d.big->SetAutoMax(d.kind == Kind::Net);
        d.big2 = new GraphWidget;
        d.big2->AddSeries(d.color.Lighter(140));
        d.big2->SetAutoMax(true);
        d.big2->setMinimumHeight(120);
        dv->addWidget(d.big2, 1);
      }
    }
    d.stats = new QLabel;
    d.stats->setTextFormat(Qt::RichText);
    d.stats->setTextInteractionFlags(Qt::TextSelectableByMouse);
    d.stats->setFont(theme::Mono(13));
    d.stats->setWordWrap(true);
    d.stats->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    dv->addWidget(d.stats);
    details_->addWidget(d.detail);
  }

  void Fill(Device &d, const Snapshot &s, bool fresh, int cap) {
    Settings &st = Settings::Get();
    d.mini->SetCapacity(cap);
    d.big->SetCapacity(cap);
    if (d.big2) d.big2->SetCapacity(cap);
    for (GraphWidget *g : d.cores) g->SetCapacity(cap);
    bool show = isVisible() && details_->currentWidget() == d.detail;
    switch (d.kind) {
      case Kind::Cpu: {
        if (fresh) {
          d.mini->Push(0, s.cpuUtil);
          d.big->Push(0, s.cpuUtil);
          d.big->Push(1, s.cpuKernel);
          for (int i = 0; i < d.cores.size() && i < s.cores.size(); ++i) d.cores[i]->Push(0, s.cores[i].util);
        }
        d.rowTitle->setText(tr("CPU"));
        d.rowLine1->setText(QString("%1%  %2 GHz").arg(s.cpuUtil, 0, 'f', 1).arg(s.cpuMhz / 1000, 0, 'f', 2));
        d.rowLine2->setText(tr("%1 logical processors").arg(s.logical));
        if (!show) break;
        d.title->setText(tr("CPU"));
        d.subtitle->setText(s.cpuModel);
        d.headline->setText(tr("%1% utilization · kernel %2% · %3 logical processors")
                                .arg(s.cpuUtil, 0, 'f', 1).arg(s.cpuKernel, 0, 'f', 1).arg(s.logical));
        d.big->SetLabels("100%", QString(), tr("% Utilization over %1 seconds").arg(st.graphSeconds()), "0");
        d.stats->setText(StatsTable(d, {Row(tr("Utilization"), QString::number(s.cpuUtil, 'f', 1) + "%"),
                                Row(tr("Speed"), QString::number(s.cpuMhz / 1000, 'f', 2) + " GHz"),
                                Row(tr("Processes"), QString::number(s.processes.size())),
                                Row(tr("Threads"), QString::number(s.threadCount)),
                                Row(tr("Up time"), fs::humanDuration(s.uptimeSec)),
                                Row(tr("Temperature"), st.FormatTemp(s.cpuTemp, 1)),
                                Row(tr("Base speed"), QString::number(s.baseMhz / 1000, 'f', 2) + " GHz"),
                                Row(tr("Max speed"), QString::number(s.maxMhz / 1000, 'f', 2) + " GHz"),
                                Row(tr("Sockets"), QString::number(s.sockets)),
                                Row(tr("Physical cores"), QString::number(s.physicalCores)),
                                Row(tr("Logical processors"), QString::number(s.logical)),
                                Row(tr("Virtualization"), s.virtualization ? tr("Supported") : tr("Not reported")),
                                Row(tr("Virtual machine"), s.virtualMachine ? tr("Yes") : tr("No")),
                                Row(tr("Frequency driver"), s.power.driver),
                                Row(tr("Governor"), s.power.governor),
                                Row(tr("Load average"), QString("%1 %2 %3").arg(s.load[0], 0, 'f', 2).arg(s.load[1], 0, 'f', 2).arg(s.load[2], 0, 'f', 2))},
                               3));
        break;
      }
      case Kind::Memory: {
        double pct = s.memTotal ? 100.0 * s.memUsed() / s.memTotal : 0;
        if (fresh) {
          d.mini->Push(0, pct);
          d.big->Push(0, pct);
        }
        d.rowTitle->setText(tr("Memory"));
        d.rowLine1->setText(QString("%1 / %2").arg(fs::humanBytes(s.memUsed()), fs::humanBytes(s.memTotal)));
        d.rowLine2->setText(QString::number(pct, 'f', 1) + "%");
        if (!show) break;
        d.title->setText(tr("Memory"));
        d.subtitle->setText(fs::humanBytes(s.memTotal));
        d.headline->setText(tr("%1% in use").arg(pct, 0, 'f', 1));
        d.big->SetLabels(fs::humanBytes(s.memTotal), QString(), tr("Memory usage"), "0");
        d.stats->setText(StatsTable(d, {Row(tr("In use"), fs::humanBytes(s.memUsed())),
                                Row(tr("Available"), fs::humanBytes(s.memAvailable)),
                                Row(tr("Cached"), fs::humanBytes(s.memCached)),
                                Row(tr("Buffers"), fs::humanBytes(s.memBuffers)),
                                Row(tr("Swap used"), fs::humanBytes(s.swapTotal - s.swapFree)),
                                Row(tr("Swap total"), fs::humanBytes(s.swapTotal)),
                                Row(tr("Pressure (avg10)"), QString::number(s.memPressure, 'f', 2) + "%")}));
        break;
      }
      case Kind::Energy: {
        const PowerInfo &p = s.power;
        double w = p.cpuPackageW >= 0 ? p.cpuPackageW : p.batteryW;
        if (fresh) {
          d.mini->SetAutoMax(true, 1);  // watts: a 5–50 W range must fill the graph
          d.big->SetAutoMax(true, 1);
          d.mini->Push(0, std::max(0.0, w));
          d.big->Push(0, std::max(0.0, w));
        }
        d.rowTitle->setText(tr("Energy"));
        d.rowLine1->setText(p.onAc ? tr("AC Power") : tr("Battery %1%").arg(p.batteryPct));
        d.rowLine2->setText(w >= 0 ? QString::number(w, 'f', 1) + " W"
                                   : p.raplRestricted ? tr("Power access restricted") : tr("Power unavailable"));
        if (!show) break;
        d.title->setText(tr("Energy"));
        d.subtitle->setText(p.onAc ? tr("AC Power") : tr("On battery"));
        d.headline->setText(p.raplRestricted ? tr("RAPL energy counters are root-only on this kernel; "
                                                  "battery discharge is shown instead when available.")
                                             : QString());
        d.big->SetLabels(tr("Watts"), QString(), QString(), "0");
        QStringList rows{Row(tr("Source"), p.onAc ? tr("AC adapter") : tr("Battery"))};
        if (p.hasBattery) {
          rows << Row(tr("Battery"), QString("%1% (%2)").arg(p.batteryPct).arg(p.batteryStatus));
          if (p.batteryW >= 0) rows << Row(tr("Battery power"), QString::number(p.batteryW, 'f', 2) + " W");
        }
        if (p.cpuPackageW >= 0) rows << Row(tr("CPU package"), QString::number(p.cpuPackageW, 'f', 2) + " W");
        rows << Row(tr("Governor"), p.governor) << Row(tr("Energy preference"), p.epp);
        d.stats->setText(StatsTable(d, rows));
        break;
      }
      case Kind::Thermals: {
        if (fresh) {
          d.mini->Push(0, std::max(0.0, s.cpuTemp));
          d.big->Push(0, std::max(0.0, s.cpuTemp));
        }
        d.rowTitle->setText(tr("Thermals"));
        d.rowLine1->setText(tr("Whole machine"));
        d.rowLine2->setText(st.FormatTemp(s.cpuTemp));
        if (!show) break;
        d.title->setText(tr("Thermals"));
        d.subtitle->setText(tr("hwmon sensors"));
        d.headline->setText(tr("Hottest CPU sensor: %1").arg(st.FormatTemp(s.cpuTemp, 1)));
        d.big->SetLabels(st.FormatTemp(110), QString(), tr("CPU temperature"), st.FormatTemp(0));
        QStringList rows;
        for (const Sensor &sn : s.sensors)
          if (sn.kind == "temp") rows << Row(sn.chip + " · " + sn.label, st.FormatTemp(sn.value, 1));
          else if (sn.kind == "fan") rows << Row(sn.chip + " · " + sn.label, QString::number(sn.value, 'f', 0) + " RPM");
        d.stats->setText(StatsTable(d, rows, 2));
        break;
      }
      case Kind::Disk: {
        if (d.index >= s.disks.size()) break;
        const DiskInfo &k = s.disks[d.index];
        if (fresh) {
          d.mini->Push(0, k.busy);
          d.big->Push(0, k.busy);
          d.big2->Push(0, k.readBps + k.writeBps);
        }
        d.rowTitle->setText(tr("Disk %1 (%2)").arg(d.index).arg(k.name));
        d.rowLine1->setText(k.model.isEmpty() ? tr("Physical disk") : k.model);
        d.rowLine2->setText(QString::number(k.busy, 'f', 1) + "%");
        if (!show) break;
        d.title->setText(tr("Disk %1").arg(d.index));
        d.subtitle->setText(k.model);
        d.headline->setText(tr("Active time %1%").arg(k.busy, 0, 'f', 1));
        d.big->SetLabels("100%", QString(), tr("Active time"), "0");
        d.big2->SetLabels(fs::humanRate(d.big2->Max()), QString(), tr("Transfer rate"), "0");
        d.stats->setText(StatsTable(d, {Row(tr("Read speed"), fs::humanRate(k.readBps)),
                                Row(tr("Write speed"), fs::humanRate(k.writeBps)),
                                Row(tr("Capacity"), fs::humanBytes(k.size)),
                                Row(tr("Type"), k.rotational ? tr("HDD") : k.name.startsWith("nvme") ? tr("NVMe SSD") : tr("SSD")),
                                Row(tr("Device"), "/dev/" + k.name)}));
        break;
      }
      case Kind::Net: {
        if (d.index >= s.nets.size()) break;
        const NetInfo &n = s.nets[d.index];
        if (fresh) {
          d.mini->Push(0, n.rxBps);
          d.mini->Push(1, n.txBps);
          d.big->Push(0, n.rxBps);
          d.big2->Push(0, n.txBps);
        }
        d.rowTitle->setText(n.wireless ? tr("Wi-Fi") : tr("Ethernet"));
        d.rowLine1->setText(n.name);
        d.rowLine2->setText(tr("S: %1  R: %2").arg(fs::humanRate(n.txBps), fs::humanRate(n.rxBps)));
        if (!show) break;
        d.title->setText(n.wireless ? tr("Wi-Fi") : tr("Ethernet"));
        d.subtitle->setText(n.name);
        d.headline->setText(n.up ? tr("Connected") : tr("Not connected"));
        d.big->SetLabels(fs::humanRate(d.big->Max()), QString(), tr("Receive"), "0");
        d.big2->SetLabels(fs::humanRate(d.big2->Max()), QString(), tr("Send"), "0");
        d.stats->setText(StatsTable(d, {Row(tr("Send"), fs::humanRate(n.txBps)), Row(tr("Receive"), fs::humanRate(n.rxBps)),
                                Row(tr("IPv4 address"), n.ipv4.isEmpty() ? "—" : n.ipv4),
                                Row(tr("Link speed"), n.speedMbps > 0 ? QString("%1 Mbps").arg(n.speedMbps) : "—"),
                                Row(tr("Adapter"), n.name)}));
        break;
      }
      case Kind::Gpu: {
        if (d.index >= s.gpus.size()) break;
        const GpuInfo &g = s.gpus[d.index];
        if (fresh) {
          d.mini->Push(0, std::max(0.0, g.util));
          d.big->Push(0, std::max(0.0, g.util));
        }
        d.rowTitle->setText(g.name);
        d.rowLine1->setText(tr("GPU %1").arg(d.index));
        d.rowLine2->setText(g.asleep ? tr("Asleep") : g.util < 0 ? tr("No load counter") : QString::number(g.util, 'f', 0) + "%");
        if (!show) break;
        d.title->setText(tr("GPU %1").arg(d.index));
        d.subtitle->setText(g.name);
        if (g.asleep)
          d.headline->setText(tr("Asleep (runtime suspended): idle, and left asleep"));
        else if (g.util < 0)
          d.headline->setText(g.driver == "nvidia" ? tr("Utilization needs nvidia-smi (nvidia-utils)")
                                                   : tr("The %1 driver reports no load: no busy counter and no engine time per client")
                                                         .arg(g.driver));
        else if (g.utilSource == "clients")
          d.headline->setText(tr("%1% utilization · busiest engine, from your processes").arg(g.util, 0, 'f', 0));
        else
          d.headline->setText(tr("%1% utilization").arg(g.util, 0, 'f', 0));
        d.big->SetLabels("100%", QString(), tr("Utilization"), "0");
        QStringList rows{Row(tr("Driver"), g.driver), Row(tr("Card"), g.card)};
        if (g.mhz > 0) rows << Row(tr("Clock"), QString("%1 MHz").arg(g.mhz));
        if (g.vramTotal > 0) rows << Row(tr("Dedicated memory"), fs::humanBytes(g.vramUsed) + " / " + fs::humanBytes(g.vramTotal));
        if (g.tempC >= 0) rows << Row(tr("Temperature"), st.FormatTemp(g.tempC, 1));
        d.stats->setText(StatsTable(d, rows));
        break;
      }
    }
  }

  QListWidget *list_;
  QStackedWidget *details_;
  QMenu *menu_;
  QVector<Device> devices_;
  QString keys_;
  quint64 lastGeneration_ = 0;
};

}  // namespace

Page *CreatePerformancePage(QWidget *parent) { return new PerformancePage(parent); }

}  // namespace td::ui
