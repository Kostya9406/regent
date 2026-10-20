#include "ui/PageRecorder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QStandardPaths>
#include <QWidget>

namespace td::ui {

PageRecorder::PageRecorder(QObject *parent) : QObject(parent) {
  frameTimer_.setTimerType(Qt::PreciseTimer);
  frameTimer_.setInterval(1000 / 30);
  stopTimer_.setSingleShot(true);
  stopTimer_.setInterval(15000);
  connect(&frameTimer_, &QTimer::timeout, this, &PageRecorder::CaptureFrame);
  connect(&stopTimer_, &QTimer::timeout, this, [this] {
    encoder_.kill();
    Complete(false, tr("Video encoder did not finish in time."));
  });
  connect(&encoder_, &QProcess::started, this, [this] {
    if (stopping_) {
      encoder_.closeWriteChannel();
      return;
    }
    frameTimer_.start();
    CaptureFrame();
    emit Started(outputPath_);
  });
  connect(&encoder_, &QProcess::readyReadStandardError, this, &PageRecorder::ReadDiagnostics);
  connect(&encoder_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    // Crashes are also reported by finished(), after stderr has been drained.
    if (error == QProcess::Crashed) return;
    ReadDiagnostics();
    const QString detail = diagnostics_.isEmpty()
        ? encoder_.errorString() : QString::fromLocal8Bit(diagnostics_).trimmed();
    encoder_.kill();
    Complete(false, tr("Cannot record page: %1").arg(detail));
  });
  connect(&encoder_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this](int code, QProcess::ExitStatus status) {
    ReadDiagnostics();
    const bool ok = status == QProcess::NormalExit && code == 0;
    if (ok && framesWritten_ == 0) {  // stopped before the first frame: ffmpeg exits 0 on empty input
      Complete(false, tr("Recording stopped before any frame was captured."));
      return;
    }
    Complete(ok, ok ? QString() : tr("Video encoder failed: %1")
        .arg(QString::fromLocal8Bit(diagnostics_).trimmed()));
  });
}

PageRecorder::~PageRecorder() {
  frameTimer_.stop();
  stopTimer_.stop();
  // Do not dispatch callbacks into a partially destroyed object.
  encoder_.disconnect(this);
  if (encoder_.state() != QProcess::NotRunning) {
    encoder_.kill();
    encoder_.waitForFinished(1000);
    if (active_) QFile::remove(outputPath_);  // killed mid-file: no trailer, not playable
  }
}

bool PageRecorder::Start(QWidget *target, const QString &outputPath) {
  if (active_ || encoder_.state() != QProcess::NotRunning) {
    emit Error(tr("A page recording is already in progress."));
    return false;
  }
  const QString executable = QStandardPaths::findExecutable("ffmpeg");
  if (executable.isEmpty()) {
    emit Error(tr("Install ffmpeg to record a page as WebM video."));
    return false;
  }
  if (!target || target->size().isEmpty() || outputPath.isEmpty()) {
    emit Error(tr("Choose a page and an output file before recording."));
    return false;
  }
  const QFileInfo destination(outputPath);
  if (destination.exists() || !destination.dir().exists()) {
    emit Error(tr("Choose a new output file in an existing directory."));
    return false;
  }
  target_ = target;
  outputPath_ = destination.absoluteFilePath();
  diagnostics_.clear();
  framesWritten_ = 0;
  stopping_ = false;
  active_ = true;
  emit RecordingChanged(true);
  encoder_.setProcessChannelMode(QProcess::SeparateChannels);
  // Wall-clock input timestamps keep the recording's duration correct when a
  // busy encoder forces us to drop frames. ffmpeg fills gaps at the output FPS.
  encoder_.start(executable, {
    "-hide_banner", "-loglevel", "error", "-n",
    "-use_wallclock_as_timestamps", "1", "-f", "rawvideo",
    "-pixel_format", "rgb24", "-video_size", "1280x720", "-framerate", "30",
    "-i", "pipe:0", "-an", "-c:v", "libvpx-vp9", "-deadline", "realtime",
    "-cpu-used", "6", "-b:v", "3000k", "-pix_fmt", "yuv420p",
    "-r", "30", "-fps_mode", "cfr", "-f", "webm", outputPath_
  }, QIODevice::ReadWrite);
  return true;
}

void PageRecorder::Stop() {
  if (!active_ || stopping_) return;
  stopping_ = true;
  frameTimer_.stop();
  // QProcess flushes buffered writes before closing stdin, so the final frame
  // is complete and ffmpeg can write the WebM trailer without blocking the UI.
  if (encoder_.state() == QProcess::Running) encoder_.closeWriteChannel();
  stopTimer_.start();
}

void PageRecorder::CaptureFrame() {
  if (!active_ || stopping_) return;
  if (!target_) {
    Stop();
    return;
  }
  constexpr qint64 frameBytes = 1280 * 720 * 3;
  // Bound QProcess's userspace queue to two frames, skipping capture when the
  // encoder is behind. No synchronous waits occur on the recording path.
  if (encoder_.bytesToWrite() > frameBytes) return;
  QImage frame(frameSize_, QImage::Format_RGB888);
  frame.fill(Qt::black);
  const QPixmap page = target_->grab();
  if (page.isNull()) return;
  QSize fitted = page.size();
  fitted.scale(frameSize_, Qt::KeepAspectRatio);
  QPainter painter(&frame);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.drawPixmap(QRect(QPoint((frame.width() - fitted.width()) / 2,
                                 (frame.height() - fitted.height()) / 2), fitted), page);
  painter.end();
  if (encoder_.write(reinterpret_cast<const char *>(frame.constBits()), frameBytes) == frameBytes) ++framesWritten_;
}

void PageRecorder::ReadDiagnostics() {
  diagnostics_.append(encoder_.readAllStandardError());
  if (diagnostics_.size() > 8192) diagnostics_ = diagnostics_.right(8192);
}

void PageRecorder::Complete(bool success, const QString &message) {
  if (!active_) return;
  frameTimer_.stop();
  stopTimer_.stop();
  active_ = false;
  stopping_ = false;
  target_.clear();
  // Start() refused existing files and ffmpeg runs with -n, so the file is ours:
  // a failed or killed encode leaves no trailer and is not worth keeping.
  if (!success) QFile::remove(outputPath_);
  emit RecordingChanged(false);
  if (success) emit Finished(outputPath_);
  else emit Error(message);
}

}  // namespace td::ui
