// Standalone integration checks; see README-page-recorder.md.
#include "ui/PageRecorder.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

namespace {

void Require(bool condition, const char *message) {
  if (condition) return;
  std::fprintf(stderr, "FAIL: %s\n", message);
  std::exit(1);
}

void VerifyVideo(const QString &path, double minimumDuration) {
  QProcess probe;
  probe.start(QStandardPaths::findExecutable("ffprobe"), {
    "-v", "error", "-show_entries", "stream=codec_name,width,height,r_frame_rate",
    "-show_entries", "format=duration", "-of", "json", path
  });
  Require(probe.waitForFinished(10000) && probe.exitCode() == 0, "ffprobe reads finalized WebM");
  const auto report = QJsonDocument::fromJson(probe.readAllStandardOutput()).object();
  const auto streams = report.value("streams").toArray();
  Require(streams.size() == 1, "video has one stream and no desktop audio");
  const auto stream = streams.first().toObject();
  Require(stream.value("codec_name").toString() == "vp9", "video uses VP9");
  Require(stream.value("width").toInt() == 1280 && stream.value("height").toInt() == 720,
          "video frame size is 1280x720");
  Require(stream.value("r_frame_rate").toString() == "30/1", "video frame rate is 30 fps");
  const double duration = report.value("format").toObject().value("duration").toString().toDouble();
  Require(duration >= minimumDuration && duration < 5.0, "duration follows capture time");
  QProcess decoder;
  decoder.start(QStandardPaths::findExecutable("ffmpeg"), {
    "-v", "error", "-i", path, "-f", "null", "-"
  });
  Require(decoder.waitForFinished(10000) && decoder.exitCode() == 0,
          "all recorded frames decode successfully");
}

void Record(td::ui::PageRecorder &recorder, const QString &path, bool deleteTarget) {
  auto *page = new QLabel("Regent page recording regression");
  page->resize(640, 360);
  page->setStyleSheet("background: #174d35; color: white;");
  page->show();
  QEventLoop loop;
  bool started = false;
  bool finished = false;
  int activeTransitions = 0;
  int heartbeats = 0;
  QTimer heartbeat;
  heartbeat.setInterval(10);
  QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&] { ++heartbeats; });
  const auto stateConnection = QObject::connect(&recorder, &td::ui::PageRecorder::RecordingChanged,
      &loop, [&](bool active) {
    Require(active == (activeTransitions == 0), "state transitions active then inactive");
    ++activeTransitions;
  });
  const auto startConnection = QObject::connect(&recorder, &td::ui::PageRecorder::Started,
      &loop, [&](const QString &output) {
    Require(output == path && recorder.IsRecording(), "Started exposes output and active state");
    started = true;
    QTimer::singleShot(deleteTarget ? 350 : 1200, &loop, [&] {
      if (deleteTarget) {
        delete page;
        page = nullptr;
      } else {
        recorder.Stop();
        recorder.Stop();  // Repeated Stop is harmless during finalization.
      }
    });
  });
  const auto finishConnection = QObject::connect(&recorder, &td::ui::PageRecorder::Finished,
      &loop, [&](const QString &output) {
    Require(output == path && !recorder.IsRecording(), "Finished runs after inactive transition");
    finished = true;
    loop.quit();
  });
  const auto errorConnection = QObject::connect(&recorder, &td::ui::PageRecorder::Error,
      &loop, [&](const QString &error) {
    std::fprintf(stderr, "Unexpected encoder error: %s\n", qPrintable(error));
    Require(false, "recording completes without error");
  });
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
    Require(false, "recording finishes within 20 seconds");
  });
  timeout.start(20000);
  heartbeat.start();
  Require(recorder.Start(page, path), "Start accepts a valid widget and new output");
  Require(recorder.IsRecording(), "Start immediately marks recording active");
  loop.exec();
  Require(started && finished && activeTransitions == 2, "complete asynchronous lifecycle emitted");
  Require(heartbeats >= 5, "GUI event loop remains responsive during encoding");
  QObject::disconnect(stateConnection);
  QObject::disconnect(startConnection);
  QObject::disconnect(finishConnection);
  QObject::disconnect(errorConnection);
  delete page;
  VerifyVideo(path, deleteTarget ? 0.1 : 0.7);
}

}  // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty()) {
    std::printf("Page recorder checks skipped: ffmpeg/ffprobe not installed.\n");
    return 0;  // a missing optional tool is not a regression
  }
  QTemporaryDir output;
  Require(output.isValid(), "temporary output directory is writable");
  td::ui::PageRecorder recorder;
  QLabel page("page");
  page.resize(100, 100);
  int errors = 0;
  const auto errorsConnection = QObject::connect(&recorder, &td::ui::PageRecorder::Error,
      &app, [&](const QString &) { ++errors; });
  const QByteArray originalPath = qgetenv("PATH");
  qputenv("PATH", output.path().toLocal8Bit());
  Require(!recorder.Start(&page, output.filePath("missing-ffmpeg.webm")), "missing ffmpeg rejected");
  qputenv("PATH", originalPath);
  Require(errors == 1 && !recorder.IsRecording(), "missing ffmpeg reports error and stays idle");
  const QString existing = output.filePath("existing.webm");
  QFile protectedFile(existing);
  Require(protectedFile.open(QIODevice::WriteOnly), "existing fixture created");
  protectedFile.write("keep this file");
  protectedFile.close();
  Require(!recorder.Start(&page, existing), "existing output rejected");
  Require(protectedFile.open(QIODevice::ReadOnly) && protectedFile.readAll() == "keep this file",
          "existing output remains unchanged");
  protectedFile.close();
  Require(!recorder.Start(nullptr, output.filePath("null.webm")), "null target rejected");
  Require(errors == 3 && !recorder.IsRecording(), "invalid requests report errors and stay idle");
  recorder.Stop();
  QObject::disconnect(errorsConnection);
  Record(recorder, output.filePath("normal.webm"), false);
  // Reuse the recorder after finished, then destroy the captured widget during
  // capture. QPointer detection must stop the encoder and finalize the trailer.
  Record(recorder, output.filePath("deleted-target.webm"), true);
  std::puts("Page recorder checks passed.");
  return 0;
}
