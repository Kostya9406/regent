#include "core/FlightRecorder.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
// Always evaluated, unlike REQUIRE(): the checks call the code under test,
// and an NDEBUG build must not turn the test into a no-op that "passes".
#define REQUIRE(cond)                                                                 \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::cerr << __FILE__ << ":" << __LINE__ << ": requirement failed: " #cond "\n"; \
      std::exit(1);                                                                   \
    }                                                                                 \
  } while (0)
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  td::FlightRecorder recorder;
  td::Snapshot snapshot;
  snapshot.time = QDateTime::currentDateTimeUtc();
  snapshot.memTotal = 1ULL << 40;
  snapshot.memAvailable = 100;
  td::ProcessInfo process;
  process.key = {1234, 18446744073709551614ULL};
  process.rss = 18446744073709551613ULL;
  process.name = "test"; process.cmdline = "test --flag"; process.user = "me";
  process.cpu = 12.3; process.state = 'S'; process.threads = 5;
  snapshot.processes.append(process);
  REQUIRE(recorder.Capture(snapshot));
  REQUIRE(!recorder.Capture(snapshot));
  for (int i = 1; i <= 650; ++i) {
    snapshot.generation = i;
    snapshot.time = snapshot.time.addSecs(1);
    REQUIRE(recorder.Capture(snapshot));
  }
  REQUIRE(recorder.Frames().size() == 600);
  QTemporaryDir dir;
  const QString path = dir.filePath("recording.json");
  QString error;
  REQUIRE(recorder.Save(path, &error));
  td::FlightRecorder loaded;
  REQUIRE(loaded.Load(path, &error));
  REQUIRE(loaded.Frames().size() == 600);
  REQUIRE(loaded.Frames().back().processes[0].key.start == process.key.start);
  REQUIRE(loaded.Frames().back().processes[0].rss == process.rss);
  REQUIRE(loaded.Frames().back().processes[0].cmdline == process.cmdline);
  QFile file(path);
  REQUIRE(file.open(QIODevice::ReadOnly));
  QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
  file.close();
  auto frames = root["frames"].toArray();
  auto frame = frames[0].toObject();
  frame["cpu"] = -20;
  frames[0] = frame;
  root["frames"] = frames;
  REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(QJsonDocument(root).toJson()); file.close();
  REQUIRE(!loaded.Load(path, &error));
  REQUIRE(loaded.Frames().size() == 600); // invalid import leaves existing data intact
  root["version"] = 2;
  REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(QJsonDocument(root).toJson()); file.close();
  REQUIRE(!loaded.Load(path, &error));
  REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  REQUIRE(file.resize(td::FlightRecorder::MaxBytes + 1));
  file.close();
  REQUIRE(!loaded.Load(path, &error));
  REQUIRE(loaded.Frames().size() == 600);
  loaded.Clear();
  REQUIRE(loaded.Frames().empty());
  // Past the per-sample cap the busiest processes are kept and the rest counted.
  snapshot.processes.resize(td::FlightRecorder::MaxProcesses + 1);
  for (int i = 0; i < snapshot.processes.size(); ++i) {
    snapshot.processes[i].key = {i + 2, 1};
    snapshot.processes[i].name = "idle";
  }
  snapshot.processes.last() = process;  // the only busy one
  ++snapshot.generation;
  REQUIRE(loaded.Capture(snapshot));
  REQUIRE(loaded.Frames().back().processes.size() == td::FlightRecorder::MaxProcesses);
  REQUIRE(loaded.Frames().back().omitted == 1);
  REQUIRE([&] {
    for (const auto &p : loaded.Frames().back().processes) if (p.key == process.key) return true;
    return false;
  }());
  REQUIRE(loaded.Save(path, &error));
  {
    // Owner-only: recordings contain every process's command line.
    const auto perms = QFileInfo(path).permissions();
    REQUIRE(!(perms & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup |
                       QFileDevice::WriteOther)));
    td::FlightRecorder reread;
    REQUIRE(reread.Load(path, &error));
    REQUIRE(reread.Frames().back().omitted == 1);
  }
  REQUIRE(!loaded.Load(dir.path(), &error));  // a directory (or FIFO) is not read
  loaded.Clear();
  snapshot.processes.fill(process, 1600);
  for (auto &p : snapshot.processes) p.cmdline = QString(4096, 'x');
  for (int i = 0; i < 12; ++i) {
    ++snapshot.generation;
    REQUIRE(loaded.Capture(snapshot));
  }
  REQUIRE(!loaded.Frames().empty() && loaded.Frames().size() < 10);
  std::cout << "Flight recorder: frame/memory limits, deduplication, exact 64-bit IDs, export/import, invalid values/version, transactional imports, oversized file, clear, process limit passed\n";
}
