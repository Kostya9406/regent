#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QScrollArea>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

#include "ui/Safe.h"
#include "core/Fs.h"
#include "core/Settings.h"
#include "ui/PixelSystemBoard.h"
#include "ui/Roles.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "ui/pages/Pages.h"
namespace td::ui {
namespace {
class SummaryPage : public Page {
 public:
  explicit SummaryPage(QWidget* parent) : Page(parent) {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget;
    grid_ = new QGridLayout(body);
    grid_->setContentsMargins(12, 12, 12, 12);
    grid_->setSpacing(12);
    board_ = new PixelSystemBoard;
    grid_->addWidget(board_, 0, 0, 1, 2);
    auto graph = [&](const QString& title, theme::Ink ink, GraphWidget** out, QLabel** foot) {
      auto* card = new QFrame;
      card->setObjectName("Card");
      auto* l = new QVBoxLayout(card);
      auto* label = new QLabel(title);
      label->setFont(theme::Mono(13, true));
      l->addWidget(label);
      *out = new GraphWidget;
      (*out)->AddSeries(ink);
      (*out)->setMinimumHeight(145);
      l->addWidget(*out, 1);
      *foot = new QLabel;
      (*foot)->setObjectName("Dim");
      (*foot)->setTextFormat(Qt::PlainText);
      (*foot)->setWordWrap(true);
      (*foot)->setMinimumHeight(QFontMetrics((*foot)->font()).lineSpacing() * 2 + 4);
      l->addWidget(*foot);
      return card;
    };
    cpuCard_ = graph(tr("CPU / LOAD HISTORY"), theme::Cpu, &cpu_, &cpuFoot_);
    cpu_->AddSeries(theme::Kernel, false);
    cpu_->AddSeries(theme::Temp, false, true);
    cpu_->SetRightMax(110);
    memoryCard_ = graph(tr("MEMORY / WORKING SET"), theme::Memory, &memory_, &memoryFoot_);
    grid_->addWidget(cpuCard_, 1, 0);
    grid_->addWidget(memoryCard_, 1, 1);
    top_ = new QTreeWidget;
    top_->setObjectName("SummaryProcesses");
    top_->setRootIsDecorated(false);
    top_->setHeaderLabels({tr("Process"), tr("PID"), tr("CPU"), tr("Memory")});
    top_->setMinimumHeight(120);
    top_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    top_->setSelectionMode(QAbstractItemView::NoSelection);
    top_->setUniformRowHeights(true);
    grid_->addWidget(top_, 2, 0, 1, 2);
    grid_->setColumnStretch(0, 1);
    grid_->setColumnStretch(1, 1);
    scroll->setWidget(body);
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(scroll);
  }
  QString Id() const override { return "summary"; }
  QString Title() const override { return tr("Summary"); }
  bool WantsBackgroundUpdates() const override { return true; }
  void Update(const SnapshotPtr& s) override {
    board_->SetSnapshot(s);
    const bool fresh = s->generation != generation_;
    generation_ = s->generation;
    const int capacity =
        qBound(10, int(Settings::Get().graphSeconds() / Settings::Get().historyIntervalSec()), 6000);
    cpu_->SetCapacity(capacity);
    memory_->SetCapacity(capacity);
    const double mem = s->memTotal ? 100.0 * s->memUsed() / s->memTotal : 0;
    if (fresh) {
      cpu_->Push(0, s->cpuUtil);
      cpu_->Push(1, s->cpuKernel);
      cpu_->Push(2, std::max(0.0, s->cpuTemp));
      memory_->Push(0, mem);
    }
    cpu_->SetLabels(QString::number(s->cpuUtil, 'f', 1) + "%", Settings::Get().FormatTemp(s->cpuTemp),
                    "CPU / KERNEL", "");
    memory_->SetLabels(QString::number(mem, 'f', 1) + "%", fs::humanBytes(s->memTotal), "USED / TOTAL", "");
    // Two fixed lines each, so the cards stay the same height and nothing is cut.
    cpuFoot_->setText(tr("%1 cores · %2 GHz · PSI %6%\nload %3 / %4 / %5")
                          .arg(s->logical)
                          .arg(s->cpuMhz / 1000, 0, 'f', 2)
                          .arg(s->load[0], 0, 'f', 2)
                          .arg(s->load[1], 0, 'f', 2)
                          .arg(s->load[2], 0, 'f', 2)
                          .arg(s->cpuPressure, 0, 'f', 1));
    memoryFoot_->setText(tr("used %1 / %2 · available %3\ncache %4 · swap %5 / %6 · PSI %7%")
                             .arg(fs::humanBytes(s->memUsed()), fs::humanBytes(s->memTotal),
                                  fs::humanBytes(s->memAvailable), fs::humanBytes(s->memCached),
                                  fs::humanBytes(s->swapTotal > s->swapFree ? s->swapTotal - s->swapFree : 0),
                                  fs::humanBytes(s->swapTotal))
                             .arg(s->memPressure, 0, 'f', 1));
    if (!isVisible()) return;
    QVector<const ProcessInfo*> processes;
    for (const auto& p : s->processes) processes << &p;
    const int n = qMin(16, int(processes.size()));
    std::partial_sort(processes.begin(), processes.begin() + n, processes.end(),
                      [](auto* a, auto* b) { return a->cpu > b->cpu; });
    while (top_->topLevelItemCount() > n) delete top_->takeTopLevelItem(top_->topLevelItemCount() - 1);
    for (int i = 0; i < n; ++i) {
      auto* item = top_->topLevelItem(i);
      if (!item) item = new QTreeWidgetItem(top_);
      const auto& p = *processes[i];
      item->setData(0, Qt::UserRole, QString("%1:%2").arg(p.key.pid).arg(p.key.start));
      item->setText(0, p.name);
      item->setText(1, QString::number(p.key.pid));
      item->setText(2, QString::number(p.cpu, 'f', 1) + "%");
      item->setData(2, kValueRole, p.cpu);
      item->setText(3, fs::humanBytes(p.rss));
      item->setData(3, kValueRole, double(p.rss));
      item->setForeground(2, theme::Cpu());
      item->setToolTip(0, safe::Tip(p.cmdline));
    }
  }

 protected:
  void resizeEvent(QResizeEvent* e) override {
    Page::resizeEvent(e);
    bool narrow = width() < (narrow_ ? 700 : 640);
    if (narrow == narrow_) return;
    QLayoutItem* tableItem = grid_->itemAtPosition(narrow_ ? 3 : 2, 0);
    QWidget* table = tableItem ? tableItem->widget() : nullptr;
    grid_->removeWidget(cpuCard_);
    grid_->removeWidget(memoryCard_);
    if (table) grid_->removeWidget(table);
    narrow_ = narrow;
    grid_->addWidget(cpuCard_, 1, 0, 1, narrow ? 2 : 1);
    grid_->addWidget(memoryCard_, narrow ? 2 : 1, narrow ? 0 : 1, 1, narrow ? 2 : 1);
    if (table) grid_->addWidget(table, narrow ? 3 : 2, 0, 1, 2);
  }

 private:
  quint64 generation_ = 0;
  bool narrow_ = false;
  QGridLayout* grid_;
  PixelSystemBoard* board_;
  GraphWidget *cpu_, *memory_;
  QLabel *cpuFoot_, *memoryFoot_;
  QWidget *cpuCard_, *memoryCard_;
  QTreeWidget* top_;
};
}  // namespace
Page* CreateSummaryPage(QWidget* parent) { return new SummaryPage(parent); }
}  // namespace td::ui
