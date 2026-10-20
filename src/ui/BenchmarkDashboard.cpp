#include "ui/BenchmarkDashboard.h"

#include <QPainter>
#include <QPainterPath>
#include <cmath>

#include "ui/OmarchyPalette.h"
#include "ui/Theme.h"
namespace td::ui {
namespace {
// The panel has five instruments; the extra tests report on the CPU and DISK ones.
benchmarks::Kind Instrument(benchmarks::Kind k) {
  switch (k) {
    case benchmarks::Kind::CpuAll:
    case benchmarks::Kind::Sustained: return benchmarks::Kind::Cpu;
    case benchmarks::Kind::DiskDirect: return benchmarks::Kind::Disk;
    default: return k;
  }
}
QString Label(benchmarks::Kind k) {
  k = Instrument(k);
  switch (k) {
    case benchmarks::Kind::Cpu:
      return "CPU";
    case benchmarks::Kind::Memory:
      return "MEMORY";
    case benchmarks::Kind::Disk:
      return "DISK";
    case benchmarks::Kind::Gpu:
      return "GPU";
    case benchmarks::Kind::Internet:
      return "NETWORK";
    default: break;
  }
  return {};
}
QString Unit(benchmarks::Kind k) {
  k = Instrument(k);
  switch (k) {
    case benchmarks::Kind::Cpu:
      return "M iter/s";
    case benchmarks::Kind::Memory:
      return "GiB/s";
    case benchmarks::Kind::Disk:
      return "MiB/s";
    case benchmarks::Kind::Gpu:
      return "fills/s";
    case benchmarks::Kind::Internet:
      return "Mbit/s";
    default: break;
  }
  return {};
}
QColor Ink(benchmarks::Kind k) {
  k = Instrument(k);
  switch (k) {
    case benchmarks::Kind::Cpu:
      return theme::Cpu();
    case benchmarks::Kind::Memory:
      return theme::Memory();
    case benchmarks::Kind::Disk:
      return theme::Disk();
    case benchmarks::Kind::Gpu:
      return theme::Gpu();
    case benchmarks::Kind::Internet:
      return theme::Net();
    default: break;
  }
  return theme::Accent();
}
void Cell(QPainter& p, int x, int y, QColor c, int size = 4) { p.fillRect(x, y, size, size, c); }
void Symbol(QPainter& p, QRect r, benchmarks::Kind kind, QColor c) {
  const int x = r.center().x() - 18, y = r.center().y() - 18;
  p.setPen(QPen(c, 2));
  p.setBrush(Qt::NoBrush);
  if (kind == benchmarks::Kind::Cpu || kind == benchmarks::Kind::Gpu) {
    p.drawRect(x + 7, y + 7, 23, 23);
    for (int i = 0; i < 4; ++i) {
      Cell(p, x + 11 + i * 5, y, c, 3);
      Cell(p, x + 11 + i * 5, y + 35, c, 3);
      Cell(p, x, y + 11 + i * 5, c, 3);
      Cell(p, x + 35, y + 11 + i * 5, c, 3);
    }
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col) Cell(p, x + 11 + col * 5, y + 11 + row * 5, c, 3);
  } else if (kind == benchmarks::Kind::Memory) {
    for (int i = 0; i < 3; ++i) {
      p.drawRect(x + 3, y + 4 + i * 11, 32, 7);
      for (int j = 0; j < 4; ++j) Cell(p, x + 8 + j * 7, y + 6 + i * 11, c, 3);
    }
  } else if (kind == benchmarks::Kind::Disk) {
    p.drawRect(x + 2, y + 2, 34, 34);
    p.drawRect(x + 8, y + 8, 22, 20);
    for (int i = 0; i < 5; ++i) Cell(p, x + 7 + i * 5, y + 31, c, 3);
  } else {
    for (int i = 0; i < 3; ++i) {
      p.drawRect(x + i * 13, y + 12, 10, 10);
      if (i < 2) p.drawLine(x + 10 + i * 13, y + 17, x + 13 + i * 13, y + 17);
    }
    p.drawLine(x + 18, y, x + 18, y + 12);
    p.drawLine(x + 18, y + 22, x + 18, y + 36);
  }
}
}  // namespace
BenchmarkDashboard::BenchmarkDashboard(QWidget* parent) : QWidget(parent) {
  setObjectName("BenchmarkDashboard");
  setAccessibleName(tr("Graphical benchmark instrument panel with measured scores and run history"));
  setMinimumHeight(285);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  for (auto& s : status_) s = tr("NOT RUN");
  animation_.setInterval(33);
  connect(&animation_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
  connect(&OmarchyPalette::Get(), &OmarchyPalette::Changed, this, qOverload<>(&QWidget::update));
}
void BenchmarkDashboard::Begin(benchmarks::Kind kind, int durationMs) {
  kind = Instrument(kind);
  selected_ = kind;
  durationMs_ = qMax(1, durationMs);
  clock_.start();
  running_ = true;
  status_[int(kind)] = tr("RUNNING");
  if (isVisible()) animation_.start();
  update();
}
void BenchmarkDashboard::Complete(const benchmarks::Result& r) {
  selected_ = Instrument(r.kind);
  running_ = false;
  animation_.stop();
  const int i = int(selected_);
  if (r.kind != selected_) {
    // A companion test: its own line under the instrument, never mixed into the history chart.
    const bool ok = r.error.isEmpty() && !r.cancelled;
    status_[i] = measured_[i] ? tr("MEASURED") : tr("NOT RUN");
    if (r.kind == benchmarks::Kind::CpuAll)
      allCores_ = ok ? tr("all cores %1").arg(r.score, 0, 'g', 4) +
                           (measured_[i] && scores_[i] > 0 ? tr(" · ×%1").arg(r.score / scores_[i], 0, 'f', 1) : QString())
                     : tr("all cores —");
    else if (r.kind == benchmarks::Kind::Sustained)
      sustained_ = ok ? r.detail.section('.', 0, 0) : tr("sustained —");
    else if (r.kind == benchmarks::Kind::DiskDirect)
      direct_ = ok ? tr("direct %1 / %2").arg(r.score, 0, 'f', 0).arg(r.secondaryScore, 0, 'f', 0) : tr("direct —");
    update();
    return;
  }
  if (!r.error.isEmpty()) {
    status_[i] = tr("UNAVAILABLE");
    measured_[i] = false;
  } else if (r.cancelled) {
    status_[i] = tr("CANCELLED");
    measured_[i] = false;
  } else {
    status_[i] = tr("MEASURED");
    scores_[i] = r.score;
    measured_[i] = true;
    history_[i].append(r.score);
    if (history_[i].size() > 24) history_[i].removeFirst();
  }
  update();
}
void BenchmarkDashboard::showEvent(QShowEvent*) {
  if (running_) animation_.start();
}
void BenchmarkDashboard::hideEvent(QHideEvent*) { animation_.stop(); }
void BenchmarkDashboard::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), theme::Base());
  for (int x = 6; x < width(); x += 9)
    for (int y = 6; y < height(); y += 9) Cell(p, x, y, theme::Mix(theme::Base(), theme::Border(), .35), 1);
  p.setPen(theme::Accent());
  p.setFont(theme::Mono(13, true));
  p.drawText(14, 24, "OMARCHY / BENCH LAB");
  p.setFont(theme::Mono(10));
  p.setPen(theme::Dim());
  p.drawText(14, 43, tr("Real measurements · each history chart uses its own scale"));
  const int columns = 5, w = qMax(30, (width() - 24) / columns);
  const benchmarks::Kind order[] = {benchmarks::Kind::Cpu, benchmarks::Kind::Memory, benchmarks::Kind::Gpu,
                                    benchmarks::Kind::Disk, benchmarks::Kind::Internet};
  for (int slot = 0; slot < 5; ++slot) {
    auto kind = order[slot];
    int i = int(kind);
    QRect r(12 + slot * w, 56, w - 6, height() - 105);
    QColor color = Ink(kind);
    p.fillRect(r, theme::Bg());
    p.setPen(kind == selected_ ? color : theme::Border());
    p.drawRect(r.adjusted(0, 0, -1, -1));
    p.setFont(theme::Mono(width() < 750 ? 9 : 11, true));
    p.setPen(color);
    p.drawText(r.adjusted(4, 4, -4, -4), Qt::AlignHCenter | Qt::AlignTop, Label(kind));
    Symbol(p, QRect(r.left(), r.top() + 22, r.width(), 40), kind, color);
    p.setFont(theme::Mono(width() < 750 ? 13 : 19, true));
    p.drawText(QRect(r.x() + 2, r.y() + 65, r.width() - 4, 25), Qt::AlignCenter,
               measured_[i] ? QString::number(scores_[i], 'g', 4) : QStringLiteral("—"));
    p.setFont(theme::Mono(9));
    p.setPen(theme::Dim());
    p.drawText(QRect(r.x() + 2, r.y() + 90, r.width() - 4, 16), Qt::AlignCenter, Unit(kind));
    // Companion results: all cores and the sustained verdict on CPU, direct I/O on DISK.
    QStringList extra;
    if (kind == benchmarks::Kind::Cpu) extra << allCores_ << sustained_;
    if (kind == benchmarks::Kind::Disk) extra << direct_;
    extra.removeAll(QString());
    p.setFont(theme::Mono(8));
    const QFontMetrics ef(p.font());
    for (int e = 0; e < extra.size(); ++e)
      p.drawText(QRect(r.x() + 2, r.y() + 106 + e * 12, r.width() - 4, 12), Qt::AlignCenter,
                 ef.elidedText(extra[e], Qt::ElideRight, r.width() - 4));
    QRect graph = r.adjusted(6, 110 + int(extra.size()) * 12, -6, -24);
    double max = 0;
    for (double v : history_[i]) max = std::max(max, v);
    p.setPen(theme::Border());
    p.drawLine(graph.bottomLeft(), graph.bottomRight());
    if (!history_[i].isEmpty() && graph.height() > 6) {
      int bar = qMax(2, graph.width() / qMax(4, int(history_[i].size())));
      for (int j = 0; j < history_[i].size(); ++j) {
        int h = max > 0 ? qRound(history_[i][j] / max * graph.height()) : 0;
        for (int y = 0; y < h; y += 6)
          Cell(p, graph.x() + j * bar, graph.bottom() - y, color, qMin(4, bar - 1));
      }
    }
    p.setFont(theme::Mono(8));
    p.setPen(running_ && kind == selected_ ? color : theme::Dim());
    p.drawText(QRect(r.x() + 2, r.bottom() - 20, r.width() - 4, 16), Qt::AlignCenter, status_[i]);
  }
  const int y = height() - 35, cells = qMax(1, (width() - 28) / 8);
  double progress = running_ ? std::clamp(double(clock_.elapsed()) / durationMs_, 0.0, .98) : 0;
  for (int i = 0; i < cells; ++i) {
    bool lit = running_ && i < int(progress * cells);
    Cell(p, 14 + i * 8, y, lit ? Ink(selected_) : theme::Raised(), 5);
  }
  p.setPen(theme::Dim());
  p.setFont(theme::Mono(10));
  p.drawText(14, height() - 10,
             running_
                 ? tr("%1 · elapsed %2 s").arg(Label(selected_)).arg(clock_.elapsed() / 1000.0, 0, 'f', 1)
                 : tr("Choose a test · completed runs build the pixel history charts"));
}
}  // namespace td::ui
