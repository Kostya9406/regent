#include "ui/PixelSystemBoard.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

#include "core/Fs.h"
#include "core/Settings.h"
#include "ui/OmarchyPalette.h"
#include "ui/Theme.h"
namespace td::ui {
namespace {
void Dot(QPainter& p, int x, int y, int size, QColor color, bool lit = true) {
  if (lit) {
    QColor glow = color;
    glow.setAlpha(30);
    p.fillRect(x - 1, y - 1, size + 2, size + 2, glow);
  }
  p.fillRect(x, y, size, size, color);
}
// A lit LED pixel: a two-step halo, the body, then a bright top-left bevel and a
// darker bottom edge, so a row of them reads as glowing hardware, not flat squares.
void Led(QPainter& p, int x, int y, int size, QColor color, double strength = 1.0) {
  QColor far = color, outer = color, inner = color;
  far.setAlphaF(std::min(1.0, 0.12 * strength));
  outer.setAlphaF(std::min(1.0, 0.26 * strength));
  inner.setAlphaF(std::min(1.0, 0.55 * strength));
  const int f = qMax(3, size * 3 / 5), o = qMax(2, size / 3), i = qMax(1, size / 6);
  p.fillRect(x - f, y - f, size + 2 * f, size + 2 * f, far);
  p.fillRect(x - o, y - o, size + 2 * o, size + 2 * o, outer);
  p.fillRect(x - i, y - i, size + 2 * i, size + 2 * i, inner);
  p.fillRect(x, y, size, size, color);
  const int edge = qMax(1, size / 5);
  p.fillRect(x, y, size, edge, color.lighter(145));
  p.fillRect(x, y, edge, size, color.lighter(130));
  p.fillRect(x, y + size - edge, size, edge, color.darker(135));
}
// An unlit LED: the meter's own colour, faintly, so the empty track belongs to it.
void Off(QPainter& p, int x, int y, int size, QColor color) {
  p.fillRect(x, y, size, size, theme::Mix(theme::Raised(), color, 0.16));
  p.fillRect(x, y, size, qMax(1, size / 5), theme::Mix(theme::Raised(), color, 0.26));
}
}  // namespace
PixelSystemBoard::PixelSystemBoard(QWidget* parent) : QWidget(parent) {
  setObjectName("PixelSystemBoard");
  setMinimumHeight(260);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setAccessibleName(tr("Omarchy system dashboard: CPU, memory, GPU, temperature and CPU core activity"));
  timer_.setInterval(33);
  connect(&timer_, &QTimer::timeout, this, &PixelSystemBoard::Animate);
  connect(&OmarchyPalette::Get(), &OmarchyPalette::Changed, this, [this] {
    grid_ = QPixmap();
    update();
  });
}
void PixelSystemBoard::Animate() {
  bool settled = true;
  for (int i = 0; i < 4; ++i) {
    shown_[i] += (targets_[i] - shown_[i]) * 0.14;
    if (std::abs(targets_[i] - shown_[i]) < 0.1) shown_[i] = targets_[i];
    else settled = false;
  }
  if (settled) timer_.stop();  // nothing moves between samples: no idle 30 fps repaint
  update();
}
void PixelSystemBoard::showEvent(QShowEvent*) {
  if (!timer_.isActive()) timer_.start();
}
void PixelSystemBoard::hideEvent(QHideEvent*) { timer_.stop(); }
void PixelSystemBoard::resizeEvent(QResizeEvent*) {
  grid_ = QPixmap();
  setMinimumHeight(width() < 640 ? 320 : 260);
}
void PixelSystemBoard::SetSnapshot(const SnapshotPtr& s) {
  snapshot_ = s;
  targets_[0] = s->cpuUtil;
  targets_[1] = s->memTotal ? 100.0 * s->memUsed() / s->memTotal : 0;
  // The busiest GPU: on a hybrid laptop the sleeping discrete one reads 0.
  gpu_ = -1;
  for (const GpuInfo& g : s->gpus) gpu_ = std::max(gpu_, g.util);
  targets_[2] = std::max(0.0, gpu_);
  targets_[3] = std::max(0.0, s->cpuTemp) / 110 * 100;
  ++phase_;
  if (!isVisible())
    for (int i = 0; i < 4; ++i) shown_[i] = targets_[i];
  else if (!timer_.isActive())
    timer_.start();
  update();
}
void PixelSystemBoard::paintEvent(QPaintEvent*) {
  QPainter p(this);
  if (grid_.size() != size() * devicePixelRatioF()) {
    grid_ = QPixmap(size() * devicePixelRatioF());
    grid_.setDevicePixelRatio(devicePixelRatioF());
    grid_.fill(theme::Base());
    QPainter g(&grid_);
    const QColor dot = theme::Mix(theme::Base(), theme::Border(), 0.35);
    for (int x = 8; x < width(); x += 9)
      for (int y = 8; y < height(); y += 9) Dot(g, x, y, 1, dot, false);
  }
  p.drawPixmap(0, 0, grid_);
  const int logo[9] = {0b001111100, 0b011000110, 0b110000011, 0b110110011, 0b110110011,
                       0b110000011, 0b110000011, 0b011000110, 0b001111100};
  for (int y = 0; y < 9; ++y)
    for (int x = 0; x < 9; ++x)
      if (logo[y] & (1 << (8 - x))) Led(p, 16 + x * 4, 14 + y * 4, 3, theme::Accent(), 0.8);
  p.setFont(theme::Mono(21, true));
  p.setPen(theme::Text());
  p.drawText(66, 32, "OMARCHY");
  p.setFont(theme::Mono(10));
  p.setPen(theme::Accent());
  p.drawText(67, 49, tr("SYSTEM / LIVE TELEMETRY"));
  if (!snapshot_) return;
  const auto& s = *snapshot_;
  p.setFont(theme::Mono(11));
  p.setPen(theme::Dim());
  if (width() > 640)
    p.drawText(QRect(260, 16, width() - 276, 38), Qt::AlignRight | Qt::AlignVCenter,
               s.hostname + "  /  " + fs::humanDuration(s.uptimeSec));
  const int cols = width() < 640 ? 2 : 4;
  const int cell = (width() - 32) / cols;
  const QString labels[4] = {"CPU", "MEMORY", "GPU", "THERMAL"};
  const QColor colors[4] = {theme::Cpu(), theme::Memory(), theme::Gpu(), theme::Temp()};
  const QString values[4] = {
      QString::number(s.cpuUtil, 'f', 1) + "%", QString::number(targets_[1], 'f', 1) + "%",
      gpu_ < 0 ? QStringLiteral("—") : QString::number(gpu_, 'f', 0) + "%",
      Settings::Get().FormatTemp(s.cpuTemp)};
  for (int i = 0; i < 4; ++i) {
    const int x = 16 + (i % cols) * cell, y = 76 + (i / cols) * 70;
    p.setFont(theme::Mono(10));
    p.setPen(theme::Dim());
    p.drawText(x, y, labels[i]);
    p.setFont(theme::Mono(20, true));
    p.setPen(colors[i]);
    p.drawText(x, y + 25, values[i]);
    // Chunky 10 px LEDs on a 14 px pitch; the leading pixel burns brightest.
    constexpr int led = 10, pitch = 14;
    int pixels = qMax(1, (cell - 18) / pitch), lit = qRound(std::clamp(shown_[i], 0.0, 100.0) * pixels / 100);
    for (int j = 0; j < pixels; ++j) {
      if (j >= lit) Off(p, x + j * pitch, y + 33, led, colors[i]);
      else Led(p, x + j * pitch, y + 33, led, j == lit - 1 ? colors[i].lighter(125) : colors[i], j == lit - 1 ? 1.4 : 1.0);
    }
  }
  const int y = cols == 2 ? 220 : 151;
  p.setFont(theme::Mono(10));
  p.setPen(theme::Dim());
  p.drawText(16, y, tr("CORE ACTIVITY"));
  int coreCount = s.cores.size();
  constexpr int coreLed = 13, corePitch = 18;
  int coreCols = qMax(1, (width() - 32) / corePitch);
  // Aggregate when a machine exposes more cores than the visible pixel grid.
  int cells = qMin(coreCount, coreCols * 2);
  for (int i = 0; i < cells; ++i) {
    double util = 0;
    int begin = i * coreCount / cells, end = (i + 1) * coreCount / cells;
    for (int c = begin; c < end; ++c) util += s.cores[c].util;
    util /= qMax(1, end - begin);
    const QColor hue = util > 75 ? theme::Temp() : theme::Cpu();
    const int cx = 16 + (i % coreCols) * corePitch, cy = y + 10 + (i / coreCols) * corePitch;
    // Idle cores stay dim; busy ones light up and glow with their load.
    if (util <= 5) Off(p, cx, cy, coreLed, hue);
    else Led(p, cx, cy, coreLed, theme::Mix(theme::Raised(), hue, std::clamp(0.35 + util / 100.0, 0.0, 1.0)),
             std::clamp(util / 60.0, 0.4, 1.6));
  }
  double rx = 0, tx = 0, read = 0, write = 0;
  for (const auto& n : s.nets)
    if (n.physical) {
      rx += n.rxBps;
      tx += n.txBps;
    }
  for (const auto& d : s.disks) {
    read += d.readBps;
    write += d.writeBps;
  }
  p.setPen(theme::Border());
  p.drawLine(16, height() - 47, width() - 16, height() - 47);
  p.setFont(theme::Mono(10));
  const QFontMetrics fm(p.font());
  const int textWidth = width() - 32 - 70;  // leave room for the activity markers
  // NET and DISK each get a five-LED activity marker on their own line, stepping
  // once per snapshot while that subsystem moves data.
  const auto marker = [&](int top, bool active, QColor color) {
    for (int i = 0; i < 5; ++i)
      active && phase_ % 5 == quint64(i) ? Led(p, width() - 75 + i * 11, top, 7, color, 1.2)
                                         : Off(p, width() - 75 + i * 11, top, 7, color);
  };
  p.setPen(theme::Net());
  p.drawText(16, height() - 29,
             fm.elidedText(tr("NET ↓ %1  ↑ %2").arg(fs::humanRate(rx), fs::humanRate(tx)), Qt::ElideRight, textWidth));
  marker(height() - 39, rx + tx > 0, theme::Net());
  p.setPen(theme::Disk());
  p.drawText(16, height() - 11,
             fm.elidedText(tr("DISK R %1  W %2").arg(fs::humanRate(read), fs::humanRate(write)), Qt::ElideRight,
                           textWidth));
  marker(height() - 21, read + write > 0, theme::Disk());
}
}  // namespace td::ui
