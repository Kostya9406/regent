#include "core/Json.h"

#include <QJsonArray>
#include <algorithm>

namespace td {

namespace {
double R1(double v) { return std::round(v * 10) / 10; }
}  // namespace

QJsonObject ToJson(const Snapshot &s, int topProcesses) {
  QJsonObject cpu{
      {"percent", R1(s.cpuUtil)},
      {"kernelPercent", R1(s.cpuKernel)},
      {"mhz", std::round(s.cpuMhz)},
      {"tempC", s.cpuTemp < 0 ? QJsonValue() : QJsonValue(R1(s.cpuTemp))},
      {"model", s.cpuModel},
      {"logical", s.logical},
  };
  QJsonArray cores;
  for (const CpuCore &c : s.cores) cores << R1(c.util);
  cpu["cores"] = cores;

  QJsonObject mem{
      {"totalBytes", double(s.memTotal)},
      {"usedBytes", double(s.memUsed())},
      {"percent", s.memTotal ? R1(100.0 * s.memUsed() / s.memTotal) : 0.0},
      {"swapUsedBytes", double(s.swapTotal - s.swapFree)},
      {"swapTotalBytes", double(s.swapTotal)},
      {"pressure", R1(s.memPressure)},
  };

  double rx = 0, tx = 0;
  for (const NetInfo &n : s.nets)
    if (n.physical) {
      rx += n.rxBps;
      tx += n.txBps;
    }
  double rd = 0, wr = 0, busy = 0;
  for (const DiskInfo &d : s.disks) {
    rd += d.readBps;
    wr += d.writeBps;
    busy = std::max(busy, d.busy);
  }
  QJsonArray gpus;
  for (const GpuInfo &g : s.gpus)
    gpus << QJsonObject{{"name", g.name},
                        {"percent", g.util < 0 ? QJsonValue() : QJsonValue(R1(g.util))},
                        {"source", g.utilSource.isEmpty() ? QJsonValue() : QJsonValue(g.utilSource)},
                        {"tempC", g.tempC < 0 ? QJsonValue() : QJsonValue(R1(g.tempC))}};

  QVector<const ProcessInfo *> top;
  for (const ProcessInfo &p : s.processes) top << &p;
  std::partial_sort(top.begin(), top.begin() + std::min<qsizetype>(topProcesses, top.size()), top.end(),
                    [](auto *a, auto *b) { return a->cpu > b->cpu; });
  QJsonArray procs;
  for (int i = 0; i < std::min<qsizetype>(topProcesses, top.size()); ++i)
    procs << QJsonObject{{"pid", top[i]->key.pid},
                         {"name", top[i]->name},
                         {"cpu", R1(top[i]->cpu)},
                         {"rssBytes", double(top[i]->rss)}};

  QJsonObject power{{"onAc", s.power.onAc}};
  if (s.power.hasBattery) {
    power["batteryPercent"] = s.power.batteryPct;
    power["batteryStatus"] = s.power.batteryStatus;
    if (s.power.batteryW >= 0) power["batteryWatts"] = R1(s.power.batteryW);
  }

  return QJsonObject{
      {"schema", kJsonSchema},
      {"generation", double(s.generation)},
      {"time", s.time.toString(Qt::ISODate)},
      {"cpu", cpu},
      {"memory", mem},
      {"disk", QJsonObject{{"readBps", std::round(rd)}, {"writeBps", std::round(wr)}, {"busyPercent", R1(busy)}}},
      {"network", QJsonObject{{"rxBps", std::round(rx)}, {"txBps", std::round(tx)}, {"ip", s.primaryIp}}},
      {"gpus", gpus},
      {"power", power},
      {"processCount", s.processes.size()},
      {"threadCount", s.threadCount},
      {"uptimeSec", std::round(s.uptimeSec)},
      {"topProcesses", procs},
  };
}

}  // namespace td
