#pragma once
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include "core/Snapshot.h"
namespace td::ui {
class PixelSystemBoard : public QWidget {
 public:
  explicit PixelSystemBoard(QWidget* parent = nullptr);
  void SetSnapshot(const SnapshotPtr& snapshot);
  QSize sizeHint() const override { return {900, 260}; }
  QSize minimumSizeHint() const override { return {240, 220}; }

 protected:
  void paintEvent(QPaintEvent*) override;
  void showEvent(QShowEvent*) override;
  void hideEvent(QHideEvent*) override;
  void resizeEvent(QResizeEvent*) override;

 private:
  SnapshotPtr snapshot_;
  QTimer timer_;
  double shown_[4] = {0, 0, 0, 0};
  double targets_[4] = {0, 0, 0, 0};
  double gpu_ = -1;  // busiest GPU, percent; -1: none reports a load
  quint64 phase_ = 0;     // activity marker: one step per snapshot
  QPixmap grid_;          // the dotted background, rebuilt on resize or theme change
  void Animate();         // eases toward targets_; the timer stops once they are reached
};
}  // namespace td::ui
