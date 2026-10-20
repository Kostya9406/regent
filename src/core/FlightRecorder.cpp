#include "core/FlightRecorder.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace td {
namespace {
qint64 Size(const FlightRecorder::Frame &f) {
  qint64 size = sizeof(f);
  for (const auto &p : f.processes)
    size += sizeof(p) + 2 * (p.name.size() + p.cmdline.size() + p.user.size());
  return size;
}
bool Number(const QJsonValue &v, double *out, double min, double max) {
  if (!v.isDouble()) return false;
  *out = v.toDouble();
  return std::isfinite(*out) && *out >= min && *out <= max;
}
bool Unsigned(const QJsonValue &v, quint64 *out) {
  bool ok = false;
  const QString text = v.toString();
  *out = text.toULongLong(&ok);
  return ok && text == QString::number(*out);
}
bool Text(const QJsonValue &v, QString *out, int limit) {
  if (!v.isString() || v.toString().size() > limit) return false;
  *out = v.toString();
  return true;
}
QJsonObject Encode(const FlightRecorder::Frame &f) {
  QJsonArray processes;
  for (const auto &p : f.processes)
    processes.append(QJsonObject{{"pid", p.key.pid}, {"start", QString::number(p.key.start)},
      {"name", p.name}, {"command", p.cmdline}, {"user", p.user}, {"state", QString(p.state)},
      {"cpu", p.cpu}, {"rss", QString::number(p.rss)}, {"threads", p.threads}});
  QJsonObject o{{"time", f.time.toUTC().toString(Qt::ISODateWithMs)}, {"cpu", f.cpu},
    {"memoryUsed", QString::number(f.memoryUsed)}, {"memoryTotal", QString::number(f.memoryTotal)},
    {"processes", processes}};
  if (f.omitted > 0) o["omitted"] = f.omitted;  // optional: older version-1 files lack it
  if (f.tempC >= 0) o["tempC"] = f.tempC;
  if (f.memPressure >= 0) o["memPressure"] = f.memPressure;
  if (f.diskBps >= 0) o["diskBps"] = f.diskBps;
  if (f.netBps >= 0) o["netBps"] = f.netBps;
  return o;
}
}  // namespace

void FlightRecorder::Clear() {
  frames_.clear();
  bytes_ = 0;
  hasGeneration_ = false;
}

void FlightRecorder::Append(Frame frame) {
  bytes_ += Size(frame);
  frames_.push_back(std::move(frame));
  while (int(frames_.size()) > maxFrames_ || bytes_ > MaxBytes) {
    bytes_ -= Size(frames_.front());
    frames_.pop_front();
  }
}

void FlightRecorder::SetMaxFrames(int frames) {
  maxFrames_ = std::clamp(frames, 1, int(MaxFrames));
  while (int(frames_.size()) > maxFrames_) {
    bytes_ -= Size(frames_.front());
    frames_.pop_front();
  }
}

bool FlightRecorder::Capture(const Snapshot &s) {
  if (hasGeneration_ && generation_ == s.generation) return false;
  if (!s.time.isValid()) return false;
  Frame f;
  f.time = s.time;
  f.cpu = std::isfinite(s.cpuUtil) ? std::clamp(s.cpuUtil, 0.0, 100.0) : 0;
  f.memoryUsed = s.memUsed();
  f.memoryTotal = s.memTotal;
  f.tempC = s.cpuTemp >= 0 && s.cpuTemp < 200 ? s.cpuTemp : -1;
  f.memPressure = std::clamp(s.memPressure, 0.0, 100.0);
  double disk = 0, net = 0;
  for (const DiskInfo &d : s.disks) disk += d.readBps + d.writeBps;
  for (const NetInfo &n : s.nets)
    if (n.physical) net += n.rxBps + n.txBps;
  f.diskBps = std::isfinite(disk) ? disk : 0;
  f.netBps = std::isfinite(net) ? net : 0;
  // Past the cap keep the busiest processes rather than dropping the whole
  // sample: recording must not silently stop on a machine with many processes.
  QVector<const ProcessInfo *> kept;
  kept.reserve(s.processes.size());
  for (const auto &p : s.processes) kept << &p;
  if (kept.size() > MaxProcesses) {
    std::nth_element(kept.begin(), kept.begin() + MaxProcesses, kept.end(), [](auto *a, auto *b) {
      return a->cpu != b->cpu ? a->cpu > b->cpu : a->rss > b->rss;
    });
    f.omitted = int(kept.size() - MaxProcesses);
    kept.resize(MaxProcesses);
  }
  f.processes.reserve(kept.size());
  for (const ProcessInfo *pp : kept) {
    const ProcessInfo &p = *pp;
    ProcessInfo copy;
    copy.key = p.key;
    copy.name = p.name.left(1024);
    copy.cmdline = p.cmdline.left(4096);
    copy.user = p.user.left(256);
    copy.state = p.state;
    copy.cpu = std::isfinite(p.cpu) ? std::clamp(p.cpu, 0.0, 100.0) : 0;
    copy.rss = p.rss;
    copy.threads = p.threads;
    f.processes.append(std::move(copy));
  }
  if (Size(f) > MaxBytes) return false;
  Append(std::move(f));
  generation_ = s.generation;
  hasGeneration_ = true;
  return true;
}

bool FlightRecorder::Save(const QString &path, QString *error) const {
  QJsonArray frames;
  for (const auto &f : frames_) frames.append(Encode(f));
  const QByteArray data = QJsonDocument(QJsonObject{{"format", "regent-flight-recorder"},
      {"version", 1}, {"frames", frames}}).toJson(QJsonDocument::Compact);
  if (data.size() > MaxBytes) {
    if (error) *error = "Recording exceeds the 64 MiB file limit; clear it and record a shorter session.";
    return false;
  }
  QSaveFile file(path);
  // Owner-only: samples hold every process's command line, which can carry secrets.
  if (!file.open(QIODevice::WriteOnly) ||
      !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
      file.write(data) != data.size() || !file.commit()) {
    if (error) *error = file.errorString();
    return false;
  }
  return true;
}

bool FlightRecorder::Load(const QString &path, QString *error) {
  auto fail = [error](const QString &message) { if (error) *error = message; return false; };
  QFile file(path);
  // Regular files only: opening a FIFO or device would block the GUI thread.
  if (!QFileInfo(path).isFile()) return fail("Not a regular file.");
  if (!file.open(QIODevice::ReadOnly)) return fail(file.errorString());
  if (file.size() > MaxBytes) return fail("Recording exceeds the 64 MiB limit.");
  const QByteArray data = file.read(MaxBytes + 1);
  if (data.size() > MaxBytes) return fail("Recording exceeds the 64 MiB limit.");
  QJsonParseError parse;
  const auto doc = QJsonDocument::fromJson(data, &parse);
  if (parse.error != QJsonParseError::NoError || !doc.isObject()) return fail("Invalid recording JSON.");
  const auto root = doc.object();
  // "taskdeck-flight-recorder": recordings saved before the rename to Regent.
  const QString format = root["format"].toString();
  if ((format != "regent-flight-recorder" && format != "taskdeck-flight-recorder") || root["version"].toDouble() != 1 ||
      !root["frames"].isArray()) return fail("Unsupported recording format or version.");
  const auto frames = root["frames"].toArray();
  if (frames.size() > MaxFrames) return fail("Recording contains too many samples.");
  FlightRecorder candidate;
  for (const auto &value : frames) {
    if (!value.isObject()) return fail("Invalid sample.");
    const auto o = value.toObject();
    Frame f;
    f.time = QDateTime::fromString(o["time"].toString(), Qt::ISODateWithMs);
    if (!f.time.isValid() ||
        !Number(o["cpu"], &f.cpu, 0, 100) || !Unsigned(o["memoryUsed"], &f.memoryUsed) ||
        !Unsigned(o["memoryTotal"], &f.memoryTotal) || f.memoryUsed > f.memoryTotal ||
        !o["processes"].isArray()) return fail("Invalid sample values or timestamps.");
    const auto processes = o["processes"].toArray();
    if (processes.size() > MaxProcesses) return fail("Sample contains too many processes.");
    for (const auto &pv : processes) {
      if (!pv.isObject()) return fail("Invalid process.");
      const auto p = pv.toObject();
      ProcessInfo info;
      double pid = 0, threads = 0;
      QString state;
      if (!Number(p["pid"], &pid, 1, std::numeric_limits<int>::max()) || std::floor(pid) != pid ||
          !Number(p["threads"], &threads, 0, std::numeric_limits<int>::max()) || std::floor(threads) != threads ||
          !Unsigned(p["start"], &info.key.start) || !Unsigned(p["rss"], &info.rss) ||
          !Number(p["cpu"], &info.cpu, 0, 100) || !Text(p["name"], &info.name, 1024) ||
          !Text(p["command"], &info.cmdline, 4096) || !Text(p["user"], &info.user, 256) ||
          !Text(p["state"], &state, 1)) return fail("Invalid process values.");
      info.key.pid = int(pid);
      info.threads = int(threads);
      info.state = state.isEmpty() ? QChar() : state.at(0);
      f.processes.append(std::move(info));
    }
    for (auto [key, out, max] : {std::tuple{"tempC", &f.tempC, 200.0}, std::tuple{"memPressure", &f.memPressure, 100.0},
                                 std::tuple{"diskBps", &f.diskBps, 1e15}, std::tuple{"netBps", &f.netBps, 1e15}})
      if (o.contains(key) && !Number(o[key], out, 0, max)) return fail("Invalid sample values or timestamps.");
    if (o.contains("omitted")) {
      double omitted = 0;
      if (!Number(o["omitted"], &omitted, 0, std::numeric_limits<int>::max()) || std::floor(omitted) != omitted)
        return fail("Invalid sample values or timestamps.");
      f.omitted = int(omitted);
    }
    if (candidate.bytes_ + Size(f) > MaxBytes) return fail("Decoded recording exceeds the memory limit.");
    candidate.Append(std::move(f));
  }
  *this = std::move(candidate);  // Failed imports never replace the current recording.
  return true;
}

namespace {
double MemPct(const FlightRecorder::Frame &f) { return f.memoryTotal ? 100.0 * f.memoryUsed / f.memoryTotal : 0; }

QString TopBy(const FlightRecorder::Frame &f, bool memory) {
  const ProcessInfo *best = nullptr;
  for (const ProcessInfo &p : f.processes)
    if (!best || (memory ? p.rss > best->rss : p.cpu > best->cpu)) best = &p;
  if (!best) return {};
  // One pass: a process may call itself "%1".
  if (memory) return QString("%1 %2 MiB").arg(best->name, QString::number(best->rss / 1048576.0, 'f', 0));
  // Process CPU is a share of the whole machine; that is what the recorder keeps.
  return QString("%1 %2% CPU").arg(best->name, QString::number(best->cpu, 'f', 1));
}
}  // namespace

QVector<FlightRecorder::Anomaly> FlightRecorder::Anomalies(const std::deque<Frame> &frames) {
  QVector<Anomaly> out;
  // A run starts when a value crosses its threshold; one marker per run.
  bool inCpu = false, inMem = false, inTemp = false, inPressure = false;
  for (int i = 0; i < int(frames.size()); ++i) {
    const Frame &f = frames[i];
    const bool cpu = f.cpu >= 85, mem = MemPct(f) >= 90, temp = f.tempC >= 90, pressure = f.memPressure >= 10;
    if (cpu && !inCpu) out.push_back({i, "cpu", QString("CPU %1%").arg(f.cpu, 0, 'f', 0), TopBy(f, false)});
    else if (!cpu && i > 0 && f.cpu - frames[i - 1].cpu >= 40)
      out.push_back({i, "spike", QString("CPU jump %1% → %2%").arg(frames[i - 1].cpu, 0, 'f', 0).arg(f.cpu, 0, 'f', 0),
                     TopBy(f, false)});
    if (mem && !inMem) out.push_back({i, "memory", QString("Memory %1%").arg(MemPct(f), 0, 'f', 0), TopBy(f, true)});
    if (temp && !inTemp) out.push_back({i, "temp", QString("CPU %1 °C").arg(f.tempC, 0, 'f', 0), TopBy(f, false)});
    if (pressure && !inPressure)
      out.push_back({i, "pressure", QString("Memory pressure %1%").arg(f.memPressure, 0, 'f', 0), TopBy(f, true)});
    inCpu = cpu;
    inMem = mem;
    inTemp = temp;
    inPressure = pressure;
  }
  return out;
}

FlightRecorder::Stats FlightRecorder::Summarize(const std::deque<Frame> &frames) {
  Stats s;
  s.samples = int(frames.size());
  if (frames.empty()) return s;
  s.durationMs = qMax<qint64>(0, frames.front().time.msecsTo(frames.back().time));
  for (const Frame &f : frames) {
    s.avgCpu += f.cpu;
    s.maxCpu = qMax(s.maxCpu, f.cpu);
    s.avgMemPct += MemPct(f);
    s.maxMemPct = qMax(s.maxMemPct, MemPct(f));
    s.maxTempC = qMax(s.maxTempC, f.tempC);
  }
  s.avgCpu /= frames.size();
  s.avgMemPct /= frames.size();
  return s;
}

}  // namespace td
