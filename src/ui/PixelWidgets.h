#pragma once

// Pixel-grid graphics drawn in the current Omarchy theme's colours.
//
// PixelVolumes: one bar of square "pixels" per mounted filesystem; used space
//   fills in left to right with a falling-pixel animation, and once a folder
//   has been scanned its share is coloured by file type.
// RallyDash: a 1980s rally-car digital dashboard for network connections — a
//   segmented rev counter for throughput and a top-down four-wheel-drive
//   chassis whose wheels are the four exposure classes; pixels run down the
//   drive shafts in proportion to traffic.

#include <QColor>
#include <QElapsedTimer>
#include <QTimer>
#include <QVector>
#include <QWidget>

namespace td::ui {

class PixelVolumes : public QWidget {
  Q_OBJECT
public:
  struct Slice {
    double bytes;  // part of the volume's used space
    QColor color;
  };
  explicit PixelVolumes(QWidget *parent = nullptr);
  void Refresh();  // re-read mounts and statvfs
  // Colours the part of `mountPoint`'s used space that a scan covered.
  void SetBreakdown(const QString &path, const QVector<Slice> &slices);
  QSize sizeHint() const override;
  int heightForWidth(int w) const override;
  bool hasHeightForWidth() const override { return true; }

protected:
  void paintEvent(QPaintEvent *) override;
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

private:
  struct Volume {
    QString mount, device, fstype;
    quint64 total = 0, used = 0;
    QVector<Slice> slices;
  };
  int Columns(int width) const;
  void AssignSlices();  // attach breakdown_ to the volume holding breakdownPath_
  QVector<Volume> volumes_;
  QString breakdownPath_;
  QVector<Slice> breakdown_;
  QTimer anim_, poll_;
  QElapsedTimer clock_;
  qint64 startedMs_ = 0;
  quint32 twinkleSeed_ = 1;
};

class RallyDash : public QWidget {
  Q_OBJECT
public:
  struct Reading {
    int loopback = 0, lan = 0, internet = 0, anyIface = 0, ipc = 0;
    int established = 0, listening = 0;
    double rxBps = 0, txBps = 0;
  };
  explicit RallyDash(QWidget *parent = nullptr);
  void SetReading(const Reading &r);
  QSize sizeHint() const override { return {900, 260}; }
  QSize minimumSizeHint() const override { return {320, 220}; }

protected:
  void paintEvent(QPaintEvent *) override;
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

private:
  void DrawRevCounter(class QPainter &p, const QRectF &r);
  void DrawChassis(class QPainter &p, const QRectF &r);
  Reading reading_;
  double shownRpm_ = 0;  // eased needle value 0..1
  double phase_ = 0;     // particle phase
  QTimer anim_;
  QElapsedTimer clock_;
  qint64 lastMs_ = 0;
};

}  // namespace td::ui
