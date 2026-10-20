#pragma once

// Drawn widgets shared by the pages: history graphs, segmented "VFD"
// meters, the VFD readout and the line icons for the sidebar.

#include "ui/Theme.h"

#include <QColor>
#include <QIcon>
#include <QVector>
#include <QWidget>
#include <deque>

namespace td::ui {

// A rolling history graph on a grid, like Task Manager's. Values are 0..max.
class GraphWidget : public QWidget {
  Q_OBJECT
public:
  struct Series {
    theme::Ink color;
    std::deque<double> values;
    bool fill = true;
    bool rightAxis = false;  // plotted against rightMax (e.g. temperature)
  };

  explicit GraphWidget(QWidget *parent = nullptr);
  int AddSeries(theme::Ink color, bool fill = true, bool rightAxis = false);
  void Push(int series, double value);
  void SetCapacity(int samples);
  void SetMax(double max) { max_ = max; update(); }
  void SetRightMax(double max) { rightMax_ = max; update(); }
  // Scale to the data, never below `floor`: the default suits byte rates, watts need ~1.
  void SetAutoMax(bool on, double floor = 1024) {
    autoMax_ = on;
    autoFloor_ = floor;
  }
  void SetGrid(theme::Ink grid) { grid_ = grid; }
  void SetLabels(QString topLeft, QString topRight, QString bottomLeft, QString bottomRight);
  void SetCompact(bool c) { compact_ = c; update(); }
  double Max() const { return max_; }

protected:
  void paintEvent(QPaintEvent *) override;

private:
  QVector<Series> series_;
  int capacity_ = 60;
  double max_ = 100, rightMax_ = 100;
  bool autoMax_ = false;
  double autoFloor_ = 1024;
  bool compact_ = false;
  theme::Ink grid_;
  QString tl_, tr_, bl_, br_;
};

// A column (or row) of lit segments; lit count follows value/max.
class SegmentMeter : public QWidget {
  Q_OBJECT
public:
  SegmentMeter(theme::Ink color, Qt::Orientation o, QWidget *parent = nullptr);
  void SetValue(double v, double max = 100);
  void SetGradient(bool g) { gradient_ = g; }
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent *) override;

private:
  theme::Ink color_;
  Qt::Orientation orientation_;
  double value_ = 0, max_ = 100;
  bool gradient_ = false;
};

// Seven-segment-ish readout on a dark glass, e.g. "SYS  1.3GHZ".
class VfdLabel : public QWidget {
  Q_OBJECT
public:
  VfdLabel(theme::Ink color, QWidget *parent = nullptr);
  void SetText(const QString &left, const QString &right);
  QSize sizeHint() const override { return {240, 34}; }
  QSize minimumSizeHint() const override { return {120, 30}; }

protected:
  void paintEvent(QPaintEvent *) override;

private:
  theme::Ink color_;
  QString left_, right_;
};

// Line icons drawn in code, so nothing depends on the icon theme.
QIcon NavIcon(const QString &id, QColor color = QColor(0xd8, 0xd8, 0xdc));
QIcon PixelIcon(const QString &id, QColor color);
// Regent's own mark: a deck of three cards, the front one carrying a pulse,
// in pixels and the current theme's colours.
QIcon AppIcon();

}  // namespace td::ui
