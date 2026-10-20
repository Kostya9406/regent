#pragma once

// Runs the Sampler on its own thread at the chosen cadence and publishes
// each Snapshot to the GUI thread through a queued signal.

#include "core/Snapshot.h"

#include <QObject>
#include <QThread>

namespace td {

class SamplingEngine : public QObject {
  Q_OBJECT
public:
  explicit SamplingEngine(QObject *parent = nullptr);
  ~SamplingEngine() override;

  void Start(int intervalMs);
  void SetInterval(int intervalMs);  // 0 pauses
  void RefreshNow();
  SnapshotPtr Latest() const { return latest_; }

signals:
  void Published(td::SnapshotPtr snapshot);

private:
  class Worker;
  QThread thread_;
  Worker *worker_ = nullptr;
  SnapshotPtr latest_;
};

}  // namespace td

Q_DECLARE_METATYPE(td::SnapshotPtr)
