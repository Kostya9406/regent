#include "ui/Widgets.h"

#include "core/Settings.h"
#include "ui/OmarchyPalette.h"
#include "ui/Theme.h"

#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <algorithm>

namespace td::ui {

// ---------------------------------------------------------------- GraphWidget

GraphWidget::GraphWidget(QWidget *parent) : QWidget(parent) {
  setMinimumSize(60, 30);
  setAttribute(Qt::WA_OpaquePaintEvent);
  connect(&OmarchyPalette::Get(), &OmarchyPalette::Changed, this, qOverload<>(&QWidget::update));
}

int GraphWidget::AddSeries(theme::Ink color, bool fill, bool rightAxis) {
  Series s;
  s.color = color;
  s.fill = fill;
  s.rightAxis = rightAxis;
  series_ << s;
  if (!grid_.isValid()) grid_ = color;
  return int(series_.size() - 1);
}

void GraphWidget::Push(int i, double v) {
  if (i < 0 || i >= series_.size()) return;
  auto &d = series_[i].values;
  d.push_back(v);
  while (int(d.size()) > capacity_) d.pop_front();
  if (autoMax_) {  // any series: a peak in the second one must widen the scale at once
    double m = 0;
    for (const Series &s : series_)
      if (!s.rightAxis)
        for (double x : s.values) m = std::max(m, x);
    // Round the scale up to 1/2/5 × 10^n so the axis label stays readable.
    double scale = 1;
    while (scale * 10 <= m) scale *= 10;
    for (double step : {1.0, 2.0, 5.0, 10.0})
      if (step * scale >= m) {
        max_ = std::max(autoFloor_, step * scale);
        break;
      }
  }
  update();
}

void GraphWidget::SetCapacity(int n) {
  capacity_ = std::max(2, n);
  for (Series &s : series_)
    while (int(s.values.size()) > capacity_) s.values.pop_front();
}

void GraphWidget::SetLabels(QString tl, QString tr, QString bl, QString br) {
  tl_ = std::move(tl);
  tr_ = std::move(tr);
  bl_ = std::move(bl);
  br_ = std::move(br);
  update();
}

void GraphWidget::paintEvent(QPaintEvent *) {
  QPainter p(this);
  QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  // Pixel style, as on the Connections dashboard: deep ground, a dot grid, a square frame.
  p.fillRect(rect(), theme::Deep());
  QColor dot = theme::Mix(theme::Deep(), grid_.isValid() ? grid_() : theme::Border(), compact_ ? 0.35 : 0.3);
  const int pitch = compact_ ? 6 : 9;
  for (int x = 3; x < width(); x += pitch)
    for (int y = 3; y < height(); y += pitch) p.fillRect(QRectF(x, y, 1.5, 1.5), dot);
  QColor rule = theme::Mix(theme::Deep(), theme::Border(), 0.55);
  p.setPen(QPen(rule, 1, Qt::DotLine));
  for (int i = 1; i < 4; ++i) p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4), QPointF(r.right(), r.top() + r.height() * i / 4));

  p.setRenderHint(QPainter::Antialiasing);
  bool smooth = Settings::Get().smoothGraphCurves();
  for (const Series &s : series_) {
    if (s.values.size() < 2) continue;
    double max = s.rightAxis ? rightMax_ : max_;
    double dx = r.width() / double(capacity_ - 1);
    double x0 = r.right() - dx * double(s.values.size() - 1);
    QVector<QPointF> pts;
    pts.reserve(int(s.values.size()));
    for (size_t i = 0; i < s.values.size(); ++i) {
      double v = std::clamp(s.values[i] / max, 0.0, 1.0);
      pts << QPointF(x0 + dx * double(i), r.bottom() - v * (r.height() - 2) - 1);
    }
    QPainterPath line(pts.first());
    for (int i = 1; i < pts.size(); ++i) {
      if (smooth) {
        QPointF a = pts[i - 1], b = pts[i];
        double mx = (a.x() + b.x()) / 2;
        line.cubicTo(QPointF(mx, a.y()), QPointF(mx, b.y()), b);
      } else {
        // Steps, like a pixel plot.
        line.lineTo(QPointF(pts[i].x(), pts[i - 1].y()));
        line.lineTo(pts[i]);
      }
    }
    const QColor color = s.color();
    if (s.fill) {
      QPainterPath area = line;
      area.lineTo(pts.last().x(), r.bottom());
      area.lineTo(pts.first().x(), r.bottom());
      area.closeSubpath();
      QColor f = color;
      f.setAlpha(compact_ ? 70 : 50);
      p.fillPath(area, f);
    }
    QColor glow = color;
    glow.setAlpha(55);
    p.strokePath(line, QPen(glow, compact_ ? 3 : 5, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.strokePath(line, QPen(color, compact_ ? 1.2 : 2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    // The newest sample as a lit pixel.
    if (!compact_) p.fillRect(QRectF(pts.last().x() - 3, pts.last().y() - 3, 6, 6), color);
  }
  p.setRenderHint(QPainter::Antialiasing, false);
  p.setPen(QPen(theme::Border(), 1));
  p.setBrush(Qt::NoBrush);
  p.drawRect(r);

  if (!compact_) {
    p.setPen(theme::Dim());
    p.setFont(theme::Mono(11));
    QRectF tr = r.adjusted(6, 3, -6, -3);
    p.drawText(tr, Qt::AlignLeft | Qt::AlignTop, tl_);
    p.drawText(tr, Qt::AlignRight | Qt::AlignTop, tr_);
    p.drawText(tr, Qt::AlignLeft | Qt::AlignBottom, bl_);
    p.drawText(tr, Qt::AlignRight | Qt::AlignBottom, br_);
  }
}

// --------------------------------------------------------------- SegmentMeter

SegmentMeter::SegmentMeter(theme::Ink color, Qt::Orientation o, QWidget *parent)
    : QWidget(parent), color_(color), orientation_(o) {
  connect(&OmarchyPalette::Get(), &OmarchyPalette::Changed, this, qOverload<>(&QWidget::update));
}

void SegmentMeter::SetValue(double v, double max) {
  value_ = v;
  max_ = max > 0 ? max : 100;
  update();
}

QSize SegmentMeter::sizeHint() const {
  return orientation_ == Qt::Vertical ? QSize(40, 240) : QSize(400, 34);
}

void SegmentMeter::paintEvent(QPaintEvent *) {
  QPainter p(this);
  bool vert = orientation_ == Qt::Vertical;
  int len = vert ? height() : width();
  int thick = vert ? std::min(width(), 46) : height();  // a meter column keeps its width when the card grows
  double offset = vert ? (width() - thick) / 2.0 : 0;
  int seg = std::max(4, thick / (vert ? 5 : 3));
  int gap = std::max(2, seg / 3);
  int n = std::max(1, (len + gap) / (seg + gap));
  double frac = std::clamp(value_ / max_, 0.0, 1.0);
  int lit = int(std::ceil(frac * n - 0.001));
  if (value_ > 0 && lit == 0) lit = 1;
  QColor off = theme::Mix(theme::Deep(), color_(), 0.16);
  for (int i = 0; i < n; ++i) {
    QColor c = color_();
    if (gradient_) {
      double t = double(i) / std::max(1, n - 1);
      c = t < 0.6 ? theme::Good() : t < 0.85 ? theme::Warn() : theme::Bad();
    }
    bool on = i < lit;
    QRectF r = vert ? QRectF(offset + thick * 0.14, len - (i + 1) * (seg + gap) + gap, thick * 0.72, seg)
                    : QRectF(i * (seg + gap), thick * 0.12, seg, thick * 0.76);
    if (on) {
      QColor glow = c;
      glow.setAlpha(60);
      p.fillRect(r.adjusted(-1.5, -1.5, 1.5, 1.5), glow);
    }
    p.fillRect(r, on ? c : off);
  }
}

// -------------------------------------------------------------------- VfdLabel

VfdLabel::VfdLabel(theme::Ink color, QWidget *parent) : QWidget(parent), color_(color) {
  setMinimumHeight(30);
  connect(&OmarchyPalette::Get(), &OmarchyPalette::Changed, this, qOverload<>(&QWidget::update));
}

void VfdLabel::SetText(const QString &left, const QString &right) {
  left_ = left.toUpper();
  right_ = right.toUpper();
  update();
}

void VfdLabel::paintEvent(QPaintEvent *) {
  QPainter p(this);
  QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  p.fillRect(rect(), theme::Deep());
  p.setPen(QPen(theme::Border(), 1));
  p.drawRect(r);
  p.setFont(theme::Mono(int(r.height() * 0.5), true));
  QRectF tr = r.adjusted(10, 0, -10, 0);
  QColor glow = color_();
  glow.setAlpha(80);
  p.setPen(glow);
  p.drawText(tr.translated(0.7, 0.7), Qt::AlignVCenter | Qt::AlignLeft, left_);
  p.drawText(tr.translated(0.7, 0.7), Qt::AlignVCenter | Qt::AlignRight, right_);
  p.setPen(color_());
  p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, left_);
  p.drawText(tr, Qt::AlignVCenter | Qt::AlignRight, right_);
}

// --------------------------------------------------------------------- Icons

QIcon NavIcon(const QString &id, QColor color) {
  QPixmap pm(64, 64);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(color, 4, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
  p.setBrush(Qt::NoBrush);
  QPainterPath path;
  if (id == "summary") {
    path.moveTo(10, 30); path.lineTo(32, 10); path.lineTo(54, 30);
    path.moveTo(16, 26); path.lineTo(16, 54); path.lineTo(48, 54); path.lineTo(48, 26);
    path.addRect(27, 38, 10, 16);
  } else if (id == "performance") {
    path.addRoundedRect(8, 10, 48, 44, 4, 4);
    path.moveTo(14, 36); path.lineTo(24, 36); path.lineTo(30, 22); path.lineTo(36, 46); path.lineTo(42, 32); path.lineTo(50, 32);
  } else if (id == "processes") {
    path.moveTo(10, 10); path.lineTo(10, 54); path.lineTo(54, 54);
    path.moveTo(18, 44); path.lineTo(28, 28); path.lineTo(38, 38); path.lineTo(52, 18);
  } else if (id == "systeminfo") {
    path.addEllipse(QPointF(32, 32), 22, 22);
    path.moveTo(32, 28); path.lineTo(32, 44);
    path.addEllipse(QPointF(32, 20), 1.5, 1.5);
  } else if (id == "startup") {
    path.moveTo(18, 10); path.lineTo(50, 32); path.lineTo(18, 54); path.closeSubpath();
  } else if (id == "users") {
    path.addEllipse(QPointF(24, 22), 9, 9);
    path.moveTo(8, 52); path.cubicTo(8, 36, 40, 36, 40, 52);
    path.addEllipse(QPointF(44, 26), 7, 7);
    path.moveTo(42, 40); path.cubicTo(52, 40, 58, 44, 58, 52);
  } else if (id == "services") {
    path.moveTo(14, 50); path.lineTo(36, 28);
    path.addEllipse(QPointF(42, 22), 10, 10);
    path.moveTo(14, 50); path.lineTo(10, 54);
  } else if (id == "power") {
    path.moveTo(36, 8); path.lineTo(18, 36); path.lineTo(32, 36); path.lineTo(28, 56); path.lineTo(46, 28); path.lineTo(32, 28); path.closeSubpath();
  } else if (id == "connections") {
    path.addEllipse(QPointF(32, 32), 22, 22);
    path.addEllipse(QPointF(32, 32), 10, 22);
    path.moveTo(10, 32); path.lineTo(54, 32);
    path.moveTo(14, 21); path.lineTo(50, 21);
    path.moveTo(14, 43); path.lineTo(50, 43);
  } else if (id == "apps") {
    path.addRoundedRect(10, 10, 44, 44, 5, 5);
    for (int y : {22, 32, 42}) { path.moveTo(18, y); path.lineTo(22, y); path.moveTo(28, y); path.lineTo(46, y); }
  } else if (id == "drivers") {
    path.addRoundedRect(18, 18, 28, 28, 3, 3);
    for (int x : {24, 32, 40}) { path.moveTo(x, 10); path.lineTo(x, 18); path.moveTo(x, 46); path.lineTo(x, 54); }
    for (int y : {24, 32, 40}) { path.moveTo(10, y); path.lineTo(18, y); path.moveTo(46, y); path.lineTo(54, y); }
  } else if (id == "diskspace") {
    path.addRoundedRect(8, 20, 48, 24, 5, 5);
    path.addEllipse(QPointF(46, 32), 2, 2);
  } else if (id == "units") {
    // Slices holding units: a bar with two blocks hanging from it.
    path.addRect(10, 10, 44, 10);
    path.moveTo(16, 20); path.lineTo(16, 49); path.lineTo(24, 49);
    path.moveTo(16, 33); path.lineTo(24, 33);
    path.addRect(24, 28, 30, 10);
    path.addRect(24, 44, 30, 10);
  } else if (id == "benchmarks") {
    // A gauge: its dial and the needle near the top of the scale.
    path.arcMoveTo(8, 12, 48, 48, 200); path.arcTo(8, 12, 48, 48, 200, -220);
    path.moveTo(32, 36); path.lineTo(46, 22);
    path.addEllipse(QPointF(32, 36), 3, 3);
  } else if (id == "flight") {
    // A recorder: a dot recording over a timeline with a marked moment.
    path.addEllipse(QPointF(20, 22), 9, 9);
    path.moveTo(8, 48); path.lineTo(56, 48);
    path.moveTo(40, 40); path.lineTo(40, 56);
  } else if (id == "settings") {
    path.addEllipse(QPointF(32, 32), 8, 8);
    for (int a = 0; a < 360; a += 45) {
      QTransform t; t.translate(32, 32); t.rotate(a);
      path.moveTo(t.map(QPointF(0, -14))); path.lineTo(t.map(QPointF(0, -22)));
    }
  } else if (id == "refresh") {
    path.arcMoveTo(12, 12, 40, 40, 60); path.arcTo(12, 12, 40, 40, 60, 290);
    path.moveTo(46, 8); path.lineTo(46, 20); path.lineTo(34, 20);
  } else if (id == "menu") {
    for (int y : {18, 32, 46}) { path.moveTo(10, y); path.lineTo(54, y); }
  }
  p.drawPath(path);
  return QIcon(pm);
}

}  // namespace td::ui

namespace td::ui {
QIcon PixelIcon(const QString& id, QColor color) {
  QPixmap pm(64, 64);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setPen(Qt::NoPen);
  p.setBrush(color);
  QStringList rows;
  if (id == "record")
    rows = {"00000000", "00111100", "01111110", "01111110", "01111110", "01111110", "00111100", "00000000"};
  else if (id == "stop")
    rows = {"00000000", "01111110", "01111110", "01111110", "01111110", "01111110", "01111110", "00000000"};
  else if (id == "live")
    rows = {"10000001", "11000001", "11100001", "11110001", "11110001", "11100001", "11000001", "10000001"};
  else if (id == "save")
    rows = {"11111110", "11000111", "11000111", "11111111", "10000001", "10111101", "10111101", "11111111"};
  else if (id == "history")
    rows = {"00111100", "01000010", "10001001", "10001001", "10001101", "10000001", "01000010", "00111100"};
  else if (id == "import")
    rows = {"00010000", "00010000", "01010100", "00111000", "00010000", "10000001", "10000001", "11111111"};
  else if (id == "folder")
    rows = {"00000000", "11100000", "11111110", "10000001", "10000001", "10000001", "11111111", "00000000"};
  else if (id == "clear")
    rows = {"00111100", "11111111", "01000010", "01011010", "01011010", "01011010", "01000010", "00111100"};
  else
    rows = {"00000000", "01000010", "00100100", "00011000", "00011000", "00100100", "01000010", "00000000"};
  for (int y = 0; y < rows.size(); ++y)
    for (int x = 0; x < rows[y].size(); ++x)
      if (rows[y][x] == '1') p.drawRect(x * 7 + 4, y * 7 + 4, 6, 6);
  return QIcon(pm);
}
}  // namespace td::ui

namespace td::ui {
QIcon AppIcon() {
  // 16 × 16 cells of 4 px. Cards back to front; each hides what is behind it.
  QPixmap pm(64, 64);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  auto card = [&p](int x0, int y0, int x1, int y1, QColor edge) {
    p.fillRect(x0 * 4, y0 * 4, (x1 - x0 + 1) * 4, (y1 - y0 + 1) * 4, theme::Base());
    for (int x = x0; x <= x1; ++x) {
      p.fillRect(x * 4, y0 * 4, 4, 4, edge);
      p.fillRect(x * 4, y1 * 4, 4, 4, edge);
    }
    for (int y = y0; y <= y1; ++y) {
      p.fillRect(x0 * 4, y * 4, 4, 4, edge);
      p.fillRect(x1 * 4, y * 4, 4, 4, edge);
    }
  };
  card(5, 0, 15, 9, theme::Dim());
  card(3, 2, 13, 11, theme::Text());
  card(0, 4, 10, 15, theme::Accent());
  static const int pulse[][2] = {{1, 10}, {2, 10}, {3, 10}, {4, 9}, {4, 8}, {4, 7}, {5, 6}, {6, 7}, {6, 8},
                                 {6, 9},  {6, 10}, {6, 11}, {6, 12}, {7, 11}, {7, 10}, {8, 10}, {9, 10}};
  for (const auto &c : pulse) p.fillRect(c[0] * 4, c[1] * 4, 4, 4, theme::Good());
  return QIcon(pm);
}
}  // namespace td::ui
