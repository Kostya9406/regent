#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSize>
#include <QTimer>

class QWidget;

namespace td::ui {

// Captures only the chosen page. ffmpeg receives RGB frames over stdin; neither
// the desktop nor neighbouring windows are included in a recording.
class PageRecorder : public QObject {
  Q_OBJECT
public:
  explicit PageRecorder(QObject *parent = nullptr);
  ~PageRecorder() override;

  // Starts asynchronously. Existing files are never overwritten. Error() is
  // emitted for invalid requests as well as encoder failures.
  bool Start(QWidget *target, const QString &outputPath);
  void Stop();
  // True during startup, recording and trailer finalization.
  bool IsRecording() const { return active_; }

signals:
  void Started(const QString &outputPath);
  void Finished(const QString &outputPath);
  void Error(const QString &message);
  void RecordingChanged(bool active);

private:
  void CaptureFrame();
  void ReadDiagnostics();
  void Complete(bool success, const QString &message = QString());

  QPointer<QWidget> target_;
  QProcess encoder_;
  QTimer frameTimer_;
  QTimer stopTimer_;
  QString outputPath_;
  QByteArray diagnostics_;
  qint64 framesWritten_ = 0;
  bool active_ = false;
  bool stopping_ = false;
  const QSize frameSize_{1280, 720};
};

}  // namespace td::ui
