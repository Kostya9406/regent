#pragma once
#include <QString>
namespace td::ui {
class FlightRecorderControls {
 public:
  virtual ~FlightRecorderControls() = default;
  virtual void StartCapture() = 0;
  virtual void StopCapture() = 0;
  virtual bool SaveCapture(const QString& path = {}) = 0;
  virtual void ImportCapture() = 0;
  virtual void ClearCapture() = 0;
  virtual void FollowLatest() = 0;
  virtual bool IsCapturing() const = 0;
  virtual qint64 CaptureElapsedMs() const = 0;
  virtual int SampleCount() const = 0;
};
}  // namespace td::ui
