#include "ui/PixelWidgets.h"

#include "core/Fs.h"
#include "ui/OmarchyPalette.h"
#include "ui/Theme.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QtMath>

#include <sys/statvfs.h>

namespace td::ui {

namespace {

constexpr int kCell = 9;  // pixel pitch: 7 px square + 2 px gap
constexpr int kDot = 7;
constexpr int kRows = 3;  // pixel rows per volume bar
constexpr int kAnimMs = 1100;

QColor C(const char *key, QColor fallback) { return OmarchyPalette::Get().color(key, fallback); }

QColor Mix(const QColor &a, const QColor &b, double t) {
  t = std::clamp(t, 0.0, 1.0);
  return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                          float(a.blueF() + (b.blueF() - a.blueF()) * t));
}

// A lit square with a one-pixel halo, the way the Omarchy pixel art draws.
void Pixel(QPainter &p, QRectF r, const QColor &c, bool lit, double glow = 0) {
  if (lit && glow > 0) {
    QColor h = c;
    h.setAlphaF(float(0.25 * glow));
    p.fillRect(r.adjusted(-1.5, -1.5, 1.5, 1.5), h);
  }
  p.fillRect(r, c);
}

quint32 Hash(quint32 x) {
  x ^= x >> 16;
  x *= 0x7feb352d;
  x ^= x >> 15;
  x *= 0x846ca68b;
  return x ^ (x >> 16);
}

QFont Mono(int px, bool bold = true) {
  QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  f.setPixelSize(px);
  f.setBold(bold);
  return f;
}

}  // namespace

// ---------------------------------------------------------------- PixelVolumes

PixelVolumes::PixelVolumes(QWidget *parent) : QWidget(parent) {
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  anim_.setInterval(33);
  connect(&anim_, &QTimer::timeout, this, [this] {
    // Full frame rate while pixels fall, then a slow twinkle.
    if (clock_.elapsed() - startedMs_ > kAnimMs + 400 && anim_.interval() != 160) anim_.setInterval(160);
    ++twinkleSeed_;
    update();
  });
  poll_.setInterval(5000);
  connect(&poll_, &QTimer::timeout, this, &PixelVolumes::Refresh);
  connect(&OmarchyPalette::Get(), &OmarchyPalette::Changed, this, qOverload<>(&QWidget::update));
  clock_.start();
  Refresh();
}

void PixelVolumes::showEvent(QShowEvent *) {
  startedMs_ = clock_.elapsed();  // replay the fill each time the page opens
  anim_.setInterval(33);
  anim_.start();
  poll_.start();
}

void PixelVolumes::hideEvent(QHideEvent *) {
  anim_.stop();
  poll_.stop();
}

void PixelVolumes::Refresh() {
  QVector<Volume> vols;
  QSet<QString> devices;
  QString mounts = QString::fromStdString(fs::readRaw("/proc/self/mounts"));
  QVector<QStringList> rows;
  for (const QString &l : mounts.split('\n', Qt::SkipEmptyParts)) {
    QStringList f = l.split(' ');
    if (f.size() >= 3 && f[0].startsWith("/dev/") && !f[0].startsWith("/dev/loop")) rows << f;
  }
  // Shortest mount point first, so "/" represents a btrfs device mounted at "/" and "/home".
  std::sort(rows.begin(), rows.end(), [](const QStringList &a, const QStringList &b) { return a[1].size() < b[1].size(); });
  for (const QStringList &f : rows) {
    if (devices.contains(f[0])) continue;
    struct statvfs st;
    QString mount = QString(f[1]).replace("\\040", " ");
    if (::statvfs(mount.toLocal8Bit().constData(), &st) != 0 || st.f_blocks == 0) continue;
    devices.insert(f[0]);
    Volume v;
    v.mount = mount;
    v.device = f[0];
    v.fstype = f[2];
    v.total = quint64(st.f_blocks) * st.f_frsize;
    v.used = quint64(st.f_blocks - st.f_bfree) * st.f_frsize;
    vols << v;
  }
  bool changed = vols.size() != volumes_.size();
  for (int i = 0; !changed && i < vols.size(); ++i)
    changed = vols[i].mount != volumes_[i].mount || qAbs(double(vols[i].used) - double(volumes_[i].used)) > vols[i].total * 0.002;
  volumes_ = vols;
  AssignSlices();
  setFixedHeight(heightForWidth(width()));
  if (changed) {
    startedMs_ = clock_.elapsed();
    if (isVisible()) anim_.start(33);
  }
  update();
}

void PixelVolumes::SetBreakdown(const QString &path, const QVector<Slice> &slices) {
  breakdownPath_ = path;
  breakdown_ = slices;
  AssignSlices();
  startedMs_ = clock_.elapsed();  // a new scan result replays the fill
  if (isVisible()) anim_.start(33);
  update();
}

void PixelVolumes::AssignSlices() {
  const QString &path = breakdownPath_;
  int best = -1;
  for (int i = 0; i < volumes_.size(); ++i) {
    volumes_[i].slices.clear();
    const QString &m = volumes_[i].mount;
    if (!path.isEmpty() && (path == m || path.startsWith(m.endsWith('/') ? m : m + '/')) &&
        (best < 0 || m.size() > volumes_[best].mount.size()))
      best = i;
  }
  if (best >= 0) volumes_[best].slices = breakdown_;
}

int PixelVolumes::Columns(int width) const { return std::max(8, (width - 24) / kCell); }

int PixelVolumes::heightForWidth(int) const { return 16 + int(volumes_.size()) * (24 + kRows * kCell + 12); }

QSize PixelVolumes::sizeHint() const { return {600, heightForWidth(600)}; }

void PixelVolumes::paintEvent(QPaintEvent *) {
  QPainter p(this);
  QColor bg = C("darker_background", QColor(0x16, 0x16, 0x18));
  QColor fg = C("foreground", theme::Text());
  QColor dim = C("dark_foreground", theme::Dim());
  QColor accent = C("accent", theme::Accent());
  QColor accent2 = C("magenta", theme::Memory());
  QColor warn = C("yellow", theme::Energy());
  QColor bad = C("red", QColor(0xf7, 0x76, 0x8e));
  QColor freeCell = Mix(bg, C("muted", QColor(0x41, 0x48, 0x68)), 0.55);
  p.fillRect(rect(), bg);

  const int cols = Columns(width());
  const int cells = cols * kRows;
  const double elapsed = double(clock_.elapsed() - startedMs_);
  int y = 10;
  for (const Volume &v : volumes_) {
    double usedFrac = v.total ? double(v.used) / double(v.total) : 0;
    p.setFont(Mono(12));
    p.setPen(fg);
    p.drawText(QRect(12, y, width() - 24, 18), Qt::AlignLeft | Qt::AlignVCenter,
               QString("%1  %2").arg(v.mount, v.fstype));
    p.setPen(usedFrac > 0.9 ? bad : usedFrac > 0.75 ? warn : dim);
    p.drawText(QRect(12, y, width() - 24, 18), Qt::AlignRight | Qt::AlignVCenter,
               QString("%1 / %2  ·  %3%  ·  %4 free")
                   .arg(fs::humanBytes(double(v.used)), fs::humanBytes(double(v.total)))
                   .arg(qRound(usedFrac * 100))
                   .arg(fs::humanBytes(double(v.total - v.used))));
    y += 24;

    int usedCells = int(std::round(usedFrac * cells));
    // Which cells the scan's file types own, in order, then plain "used".
    QVector<QColor> owner(usedCells, QColor());
    int at = 0;
    for (const Slice &s : v.slices) {
      // At least one pixel per file type, so a small folder still shows on a big disk.
      int n = v.total && s.bytes > 0 ? std::max(1, int(std::round(s.bytes / double(v.total) * cells))) : 0;
      for (int k = 0; k < n && at < usedCells; ++k) owner[at++] = s.color;
    }
    for (int i = 0; i < cells; ++i) {
      int col = i / kRows, row = kRows - 1 - i % kRows;  // fill each column bottom-up, left to right
      QRectF r(12 + col * kCell, y + row * kCell, kDot, kDot);
      bool used = i < usedCells;
      if (!used) {
        Pixel(p, r, freeCell, false);
        continue;
      }
      // Each pixel drops into place in turn.
      double start = double(i) / std::max(1, usedCells) * kAnimMs * 0.8;
      double k = std::clamp((elapsed - start) / 180.0, 0.0, 1.0);
      if (k <= 0) {
        Pixel(p, r, freeCell, false);
        continue;
      }
      QColor c = owner[i].isValid() ? owner[i] : Mix(accent, accent2, double(col) / cols);
      if (usedFrac > 0.9 && !owner[i].isValid()) c = Mix(c, bad, 0.55);
      // Idle twinkle: a few pixels at a time brighten for one beat.
      bool twinkle = k >= 1 && Hash(quint32(i) * 2654435761u ^ (twinkleSeed_ / 3)) % 97 == 0;
      if (twinkle) c = c.lighter(150);
      QRectF dropped = r.translated(0, -(1 - k) * kCell * 2.5);
      QColor shown = c;
      shown.setAlphaF(float(0.35 + 0.65 * k));
      Pixel(p, dropped, shown, true, twinkle ? 1.0 : 0.4);
    }
    y += kRows * kCell + 12;
  }
  if (volumes_.isEmpty()) {
    p.setPen(dim);
    p.drawText(rect(), Qt::AlignCenter, tr("No block-device filesystems are mounted."));
  }
}

// ------------------------------------------------------------------ RallyDash

RallyDash::RallyDash(QWidget *parent) : QWidget(parent) {
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setFixedHeight(260);
  anim_.setInterval(33);
  connect(&anim_, &QTimer::timeout, this, [this] {
    qint64 now = clock_.elapsed();
    double dt = (now - lastMs_) / 1000.0;
    lastMs_ = now;
    double total = reading_.rxBps + reading_.txBps;
    double target = std::clamp(std::log10(1 + total) / 8.0, 0.0, 1.0);  // full scale ≈ 100 MB/s
    shownRpm_ += (target - shownRpm_) * std::min(1.0, dt * 4);        // needle inertia
    phase_ += dt * (0.15 + 1.6 * shownRpm_);
    update();
  });
  connect(&OmarchyPalette::Get(), &OmarchyPalette::Changed, this, qOverload<>(&QWidget::update));
  clock_.start();
}

void RallyDash::SetReading(const Reading &r) { reading_ = r; }

void RallyDash::showEvent(QShowEvent *) {
  lastMs_ = clock_.elapsed();
  anim_.start();
}

void RallyDash::hideEvent(QHideEvent *) { anim_.stop(); }

void RallyDash::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  QColor bg = C("darker_background", QColor(0x10, 0x10, 0x12));
  p.fillRect(rect(), bg);
  // Faint pixel grid behind everything.
  QColor grid = Mix(bg, C("muted", QColor(0x41, 0x48, 0x68)), 0.25);
  for (int x = 4; x < width(); x += kCell)
    for (int y = 4; y < height(); y += kCell) p.fillRect(QRectF(x, y, 1.5, 1.5), grid);

  QRectF all = rect().adjusted(10, 10, -10, -10);
  double split = std::min(all.width() * 0.42, all.height() * 1.45);
  DrawRevCounter(p, QRectF(all.left(), all.top(), split, all.height()));
  DrawChassis(p, QRectF(all.left() + split + 10, all.top(), all.width() - split - 10, all.height()));
}

void RallyDash::DrawRevCounter(QPainter &p, const QRectF &r) {
  QColor amber = C("yellow", QColor(0xe0, 0xaf, 0x68));
  QColor orange = C("orange", QColor(0xeb, 0x92, 0x7b));
  QColor red = C("red", QColor(0xf7, 0x76, 0x8e));
  QColor dim = Mix(C("darker_background", QColor(0x10, 0x10, 0x12)), amber, 0.12);
  QColor fg = C("foreground", theme::Text());
  QPointF c(r.center().x(), r.top() + r.height() * 0.56);
  double radius = std::min(r.width() * 0.46, r.height() * 0.52);
  const int segs = 32;
  int lit = int(std::round(shownRpm_ * segs));
  // 240° sweep from lower-left to lower-right, segment blocks like an LCD bar dash.
  for (int i = 0; i < segs; ++i) {
    double a = qDegreesToRadians(210.0 - 240.0 * (i + 0.5) / segs);
    double t = double(i) / segs;
    QColor col = t < 0.7 ? amber : t < 0.88 ? orange : red;
    bool on = i < lit;
    double len = radius * (0.17 + 0.05 * (i % 4 == 0));
    QPointF outer(c.x() + std::cos(a) * radius, c.y() - std::sin(a) * radius);
    QPointF inner(c.x() + std::cos(a) * (radius - len), c.y() - std::sin(a) * (radius - len));
    if (on) {
      QColor glow = col;
      glow.setAlphaF(0.3f);
      p.setPen(QPen(glow, 10, Qt::SolidLine, Qt::FlatCap));
      p.drawLine(inner, outer);
    }
    p.setPen(QPen(on ? col : dim, 6, Qt::SolidLine, Qt::FlatCap));
    p.drawLine(inner, outer);
  }
  // Scale: log10 of bytes/s — 1 B … 100 MB.
  p.setFont(Mono(10, false));
  p.setPen(Mix(fg, amber, 0.4));
  const char *marks[] = {"0", "100", "10K", "1M", "100M"};
  for (int i = 0; i < 5; ++i) {
    double a = qDegreesToRadians(210.0 - 240.0 * i / 4.0);
    QPointF pt(c.x() + std::cos(a) * (radius * 0.66), c.y() - std::sin(a) * (radius * 0.66));
    p.drawText(QRectF(pt.x() - 22, pt.y() - 8, 44, 16), Qt::AlignCenter, marks[i]);
  }
  // Digital readout in the middle.
  double total = reading_.rxBps + reading_.txBps;
  p.setFont(Mono(int(radius * 0.22)));
  p.setPen(amber);
  p.drawText(QRectF(c.x() - radius, c.y() - radius * 0.2, radius * 2, radius * 0.34), Qt::AlignCenter,
             fs::humanRate(total));
  p.setFont(Mono(11, false));
  p.setPen(Mix(fg, amber, 0.3));
  p.drawText(QRectF(c.x() - radius, c.y() + radius * 0.16, radius * 2, 18), Qt::AlignCenter,
             QString("↓ %1   ↑ %2").arg(fs::humanRate(reading_.rxBps),
                                         fs::humanRate(reading_.txBps)));
  p.setFont(Mono(12));
  p.setPen(Mix(fg, amber, 0.6));
  p.drawText(QRectF(c.x() - radius, c.y() + radius * 0.42, radius * 2, 18), Qt::AlignCenter, tr("NET  THROUGHPUT"));
}

void RallyDash::DrawChassis(QPainter &p, const QRectF &r) {
  QColor fg = C("foreground", theme::Text());
  QColor dim = C("dark_foreground", theme::Dim());
  QColor body = Mix(C("darker_background", QColor(0x10, 0x10, 0x12)), C("muted", QColor(0x41, 0x48, 0x68)), 0.5);
  struct Wheel {
    QString label;
    int count;
    QColor color;
  };
  // Front axle: what stays on this machine or network; rear: what reaches out.
  Wheel wheels[4] = {{tr("LOOP"), reading_.loopback, C("cyan", QColor(0x44, 0x9d, 0xab))},
                     {tr("LAN"), reading_.lan, C("green", QColor(0x9e, 0xce, 0x6a))},
                     {tr("WAN"), reading_.internet, C("accent", theme::Accent())},
                     {tr("ANY"), reading_.anyIface, C("orange", QColor(0xeb, 0x92, 0x7b))}};
  int maxCount = 8;
  for (const Wheel &w : wheels) maxCount = std::max(maxCount, w.count);

  // Chassis, nose pointing up.
  double h = r.height() - 30, w = std::min(r.width() * 0.42, h * 0.55);
  QRectF chassis(r.center().x() - w / 2, r.top() + 8, w, h);
  p.setPen(QPen(dim, 2));
  p.setBrush(body);
  p.drawRoundedRect(chassis, w * 0.28, w * 0.28);
  double frontY = chassis.top() + h * 0.2, rearY = chassis.top() + h * 0.8;
  double cx = chassis.center().x(), cy = chassis.center().y();
  // Drive train: prop shaft, both axles, centre differential.
  QColor shaft = Mix(body, fg, 0.35);
  p.setPen(QPen(shaft, 3));
  p.drawLine(QPointF(cx, frontY), QPointF(cx, rearY));
  double axle = w / 2 + 14;
  p.drawLine(QPointF(cx - axle, frontY), QPointF(cx + axle, frontY));
  p.drawLine(QPointF(cx - axle, rearY), QPointF(cx + axle, rearY));
  QRectF diff(cx - 26, cy - 15, 52, 30);
  p.setPen(QPen(fg, 1.5));
  p.setBrush(Mix(body, fg, 0.12));
  p.drawRect(diff);
  int front = reading_.loopback + reading_.lan, rear = reading_.internet + reading_.anyIface;
  int fpct = front + rear ? qRound(100.0 * front / (front + rear)) : 50;
  p.setFont(Mono(11));
  p.setPen(fg);
  p.drawText(diff, Qt::AlignCenter, QString("%1:%2").arg(fpct).arg(100 - fpct));

  // Wheels: a stack of segments each, lit by that class's share.
  const double tw = 22, th = h * 0.26;
  QPointF hubs[4] = {{cx - axle, frontY}, {cx + axle, frontY}, {cx - axle, rearY}, {cx + axle, rearY}};
  for (int i = 0; i < 4; ++i) {
    QRectF tire(hubs[i].x() - tw / 2, hubs[i].y() - th / 2, tw, th);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 120));
    p.drawRoundedRect(tire.adjusted(-3, -3, 3, 3), 5, 5);
    const int segs = 8;
    int lit = wheels[i].count ? std::max(1, int(std::ceil(double(wheels[i].count) / maxCount * segs))) : 0;
    double sh = (th - (segs + 1) * 2) / segs;
    for (int s = 0; s < segs; ++s) {
      QRectF seg(tire.left() + 3, tire.bottom() - 2 - (s + 1) * (sh + 2), tw - 6, sh);
      bool on = s < lit;
      QColor c = on ? wheels[i].color : Mix(QColor(0, 0, 0), wheels[i].color, 0.18);
      Pixel(p, seg, c, on, on ? 0.6 : 0);
    }
    // Label outside the wheel.
    bool left = i % 2 == 0;
    QRectF lab = left ? QRectF(tire.left() - 120, tire.center().y() - 20, 110, 40)
                      : QRectF(tire.right() + 10, tire.center().y() - 20, 110, 40);
    p.setFont(Mono(12));
    p.setPen(wheels[i].color);
    p.drawText(lab, (left ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignTop, wheels[i].label);
    p.setFont(Mono(18));
    p.setPen(fg);
    p.drawText(lab, (left ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignBottom, QString::number(wheels[i].count));

    // Torque pixels: from the differential, along the prop shaft, out the axle.
    int particles = wheels[i].count ? std::min(6, 1 + wheels[i].count / 4) : 0;
    QPointF a(cx, cy), b(cx, hubs[i].y()), e = hubs[i];
    double l1 = std::abs(b.y() - a.y()), l2 = std::abs(e.x() - b.x());
    for (int k = 0; k < particles; ++k) {
      double t = std::fmod(phase_ + double(k) / particles + i * 0.13, 1.0);
      double d = t * (l1 + l2);
      QPointF pt = d < l1 ? QPointF(cx, a.y() + (b.y() - a.y()) * d / l1)
                          : QPointF(b.x() + (e.x() - b.x()) * (d - l1) / l2, b.y());
      Pixel(p, QRectF(pt.x() - 2.5, pt.y() - 2.5, 5, 5), wheels[i].color, true, 1.0);
    }
  }

  // Bottom strip: the counts that are not wheels.
  p.setFont(Mono(12));
  p.setPen(dim);
  // The legend goes first when the strip is narrow; the counts never get cut.
  QString strip = tr("EST %1   LISTEN %2   UNIX %3   F:R = local : remote")
                      .arg(reading_.established).arg(reading_.listening).arg(reading_.ipc);
  if (QFontMetrics(p.font()).horizontalAdvance(strip) > r.width())
    strip = tr("EST %1  LISTEN %2  UNIX %3").arg(reading_.established).arg(reading_.listening).arg(reading_.ipc);
  p.drawText(QRectF(r.left(), r.bottom() - 18, r.width(), 18), Qt::AlignCenter,
             QFontMetrics(p.font()).elidedText(strip, Qt::ElideRight, int(r.width())));
}

}  // namespace td::ui
