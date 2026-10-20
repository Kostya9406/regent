#pragma once

#include "core/Snapshot.h"
#include <deque>

namespace td {

// A bounded, rolling history. Process identities retain their start time so
// reused PIDs remain distinguishable when inspecting an old recording.
class FlightRecorder {
public:
  struct Frame {
    QDateTime time;
    double cpu = 0;
    quint64 memoryUsed = 0, memoryTotal = 0;
    QVector<ProcessInfo> processes;
    int omitted = 0;  // processes beyond MaxProcesses, least busy first
    // Optional in files (absent before Regent 0.5): -1 = not recorded.
    double tempC = -1;
    double memPressure = -1;
    double diskBps = -1, netBps = -1;
  };
  // Moments worth a look, each with the process most likely behind it.
  struct Anomaly {
    int index = 0;      // frame
    QString kind;       // cpu, spike, memory, pressure, temp
    QString text;       // "CPU 97%"
    QString culprit;    // "rustc 380% CPU" ("" when the frame has no processes)
  };
  struct Stats {
    int samples = 0;
    qint64 durationMs = 0;
    double avgCpu = 0, maxCpu = 0;
    double avgMemPct = 0, maxMemPct = 0;
    double maxTempC = -1;
  };
  static constexpr int MaxFrames = 600;
  static constexpr int MaxProcesses = 8192;
  static constexpr qint64 MaxBytes = 64 * 1024 * 1024;

  bool Capture(const Snapshot &snapshot);
  void Clear();
  void SetMaxFrames(int frames);  // a pre-roll ring keeps fewer; trims the oldest at once
  static QVector<Anomaly> Anomalies(const std::deque<Frame> &frames);
  static Stats Summarize(const std::deque<Frame> &frames);
  const std::deque<Frame> &Frames() const { return frames_; }
  bool Save(const QString &path, QString *error) const;
  bool Load(const QString &path, QString *error);

private:
  void Append(Frame frame);
  std::deque<Frame> frames_;
  int maxFrames_ = MaxFrames;
  qint64 bytes_ = 0;
  quint64 generation_ = 0;
  bool hasGeneration_ = false;
};

}  // namespace td
