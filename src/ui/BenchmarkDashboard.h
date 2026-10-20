#pragma once
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include "core/Benchmarks.h"
namespace td::ui {
class BenchmarkDashboard : public QWidget {
  Q_OBJECT
 public:
  explicit BenchmarkDashboard(QWidget* parent = nullptr);
  void Begin(benchmarks::Kind kind, int durationMs);
  void Complete(const benchmarks::Result& result);
  bool Running() const { return running_; }
  double Score(benchmarks::Kind kind) const { return scores_[int(kind)]; }

 protected:
  void paintEvent(QPaintEvent*) override;
  void showEvent(QShowEvent*) override;
  void hideEvent(QHideEvent*) override;

 private:
  QTimer animation_;
  QElapsedTimer clock_;
  benchmarks::Kind selected_ = benchmarks::Kind::Cpu;
  double scores_[5] = {0, 0, 0, 0, 0};
  QVector<double> history_[5];
  QString status_[5];
  bool measured_[5] = {false, false, false, false, false}, running_ = false;
  int durationMs_ = 1000;
  QString allCores_, sustained_, direct_;  // companion results shown under CPU and DISK
};
}  // namespace td::ui
