#include "core/Sampler.h"

#include "core/Fs.h"
#include "core/HwIds.h"
#include "core/Hyprland.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>

#include <algorithm>
#include <climits>
#include <cstring>
#include <tuple>
#include <dirent.h>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <csignal>
#include <pwd.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace td {

namespace {

// Counters can go backwards (a re-created interface, per-CPU iowait); never wrap.
inline quint64 Delta(quint64 now, quint64 before) { return now >= before ? now - before : 0; }

// Splits one line of whitespace-separated unsigned counters.
QVector<quint64> Counters(const char *p, const char *end) {
  QVector<quint64> v;
  while (p < end) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p >= end || *p == '\n') break;
    char *e = nullptr;
    v << std::strtoull(p, &e, 10);
    if (e == p) break;
    p = e;
  }
  return v;
}

std::string Line(const std::string &text, size_t &pos) {
  size_t nl = text.find('\n', pos);
  std::string l = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
  pos = nl == std::string::npos ? text.size() : nl + 1;
  return l;
}

quint64 MemField(const std::string &mi, const char *key) {
  size_t p = mi.find(key);
  if (p == std::string::npos) return 0;
  p += std::strlen(key);
  return std::strtoull(mi.c_str() + p, nullptr, 10) * 1024;
}

double PsiAvg10(const QString &path) {
  std::string t = fs::readRaw(path, 512);
  size_t p = t.find("avg10=");
  return p == std::string::npos ? 0 : std::strtod(t.c_str() + p + 6, nullptr);
}

}  // namespace

Sampler::Sampler() {
  ticksPerSec_ = sysconf(_SC_CLK_TCK);
  pageSize_ = sysconf(_SC_PAGESIZE);
}

SnapshotPtr Sampler::Collect() {
  auto s = std::make_shared<Snapshot>();
  auto now = std::chrono::steady_clock::now();
  s->intervalSec = primed_ ? std::chrono::duration<double>(now - last_).count() : 1.0;
  if (s->intervalSec <= 0.01) s->intervalSec = 0.01;
  s->generation = ++generation_;
  s->time = QDateTime::currentDateTime();

  ReadStatic(*s);
  ReadCpu(*s);
  ReadMemory(*s);
  ReadDisks(*s);
  ReadNets(*s);
  ReadGpus(*s);
  ReadSensors(*s);
  ReadPower(*s);
  ReadThrottle(*s);
  ReadVolumes(*s);
  gpuEngines_.clear();
  gpuClientsSeen_.clear();
  ReadProcesses(*s);
  FinishGpus(*s);
  MarkWindowedProcesses(*s);

  last_ = now;
  primed_ = true;
  return s;
}

void Sampler::ReadStatic(Snapshot &s) {
  if (cpuModel_.isEmpty()) {
    std::string ci = fs::readRaw("/proc/cpuinfo");
    QSet<QString> cores, packages;
    QString phys;
    size_t pos = 0;
    while (pos < ci.size()) {
      QString l = QString::fromStdString(Line(ci, pos));
      int c = l.indexOf(':');
      if (c < 0) continue;
      QString k = l.left(c).trimmed(), v = l.mid(c + 1).trimmed();
      if (k == "processor") ++logical_;
      else if (k == "model name" && cpuModel_.isEmpty()) cpuModel_ = v;
      else if (k == "physical id") { phys = v; packages << v; }
      else if (k == "core id") cores << phys + ':' + v;
      else if (k == "flags" && !virt_) {
        virt_ = v.contains(" vmx") || v.contains(" svm");
        vm_ = v.contains(" hypervisor");
      }
    }
    physical_ = cores.isEmpty() ? logical_ : int(cores.size());
    sockets_ = packages.isEmpty() ? 1 : int(packages.size());
    qint64 base = fs::readInt("/sys/devices/system/cpu/cpu0/cpufreq/base_frequency");
    qint64 max = fs::readInt("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    baseMhz_ = base > 0 ? base / 1000.0 : max / 1000.0;
    maxMhz_ = max > 0 ? max / 1000.0 : baseMhz_;
    kernel_ = QSysInfo::kernelVersion();
    distro_ = QSysInfo::prettyProductName();
    // Screenshots meant for publishing can stand in another name for the machine's.
    hostname_ = qEnvironmentVariable("REGENT_CAPTURE_HOSTNAME", QSysInfo::machineHostName());
  }
  s.cpuModel = cpuModel_;
  s.logical = logical_;
  s.physicalCores = physical_;
  s.sockets = sockets_;
  s.baseMhz = baseMhz_;
  s.maxMhz = maxMhz_;
  s.virtualization = virt_;
  s.virtualMachine = vm_;
  s.kernel = kernel_;
  s.distro = distro_;
  s.hostname = hostname_;

  std::string up = fs::readRaw("/proc/uptime", 128);
  s.uptimeSec = std::strtod(up.c_str(), nullptr);
  std::string la = fs::readRaw("/proc/loadavg", 128);
  std::sscanf(la.c_str(), "%lf %lf %lf", &s.load[0], &s.load[1], &s.load[2]);
  s.memPressure = PsiAvg10("/proc/pressure/memory");
  s.cpuPressure = PsiAvg10("/proc/pressure/cpu");
}

void Sampler::ReadCpu(Snapshot &s) {
  std::string st = fs::readRaw("/proc/stat");
  QVector<CpuTimes> cur;
  size_t pos = 0;
  while (pos < st.size()) {
    std::string l = Line(st, pos);
    if (l.compare(0, 3, "cpu") != 0) break;
    size_t sp = l.find(' ');
    auto v = Counters(l.c_str() + sp, l.c_str() + l.size());
    if (v.size() < 8) continue;
    CpuTimes t;
    // user nice system idle iowait irq softirq steal
    for (int i = 0; i < 8; ++i) t.total += v[i];
    t.idle = v[3] + v[4];
    t.kernel = v[2] + v[5] + v[6];
    cur << t;
  }
  auto util = [](const CpuTimes &a, const CpuTimes &b, double *kernel) {
    quint64 dt = Delta(b.total, a.total);
    if (dt == 0) return 0.0;
    if (kernel) *kernel = std::clamp(100.0 * double(Delta(b.kernel, a.kernel)) / double(dt), 0.0, 100.0);
    return std::clamp(100.0 * (1.0 - double(Delta(b.idle, a.idle)) / double(dt)), 0.0, 100.0);
  };
  if (cpuPrev_.size() == cur.size() && !cur.isEmpty()) {
    s.cpuUtil = util(cpuPrev_[0], cur[0], &s.cpuKernel);
    for (int i = 1; i < cur.size(); ++i) s.cores.push_back({util(cpuPrev_[i], cur[i], nullptr), 0});
  } else {
    for (int i = 1; i < cur.size(); ++i) s.cores.push_back({0, 0});
  }
  cpuPrev_ = cur;

  double sum = 0;
  int n = 0;
  for (int i = 0; i < s.cores.size(); ++i) {
    qint64 khz = fs::readInt(QString("/sys/devices/system/cpu/cpu%1/cpufreq/scaling_cur_freq").arg(i));
    if (khz > 0) {
      s.cores[i].mhz = khz / 1000.0;
      sum += s.cores[i].mhz;
      ++n;
    }
  }
  s.cpuMhz = n ? sum / n : 0;
}

void Sampler::ReadMemory(Snapshot &s) {
  std::string mi = fs::readRaw("/proc/meminfo");
  s.memTotal = MemField(mi, "MemTotal:");
  s.memAvailable = MemField(mi, "MemAvailable:");
  s.memCached = MemField(mi, "\nCached:") + MemField(mi, "SReclaimable:");
  s.memBuffers = MemField(mi, "Buffers:");
  s.swapTotal = MemField(mi, "SwapTotal:");
  s.swapFree = MemField(mi, "SwapFree:");
}

void Sampler::ReadDisks(Snapshot &s) {
  std::string ds = fs::readRaw("/proc/diskstats");
  QHash<QString, DiskPrev> cur;
  size_t pos = 0;
  while (pos < ds.size()) {
    std::string l = Line(ds, pos);
    char name[64];
    unsigned long long f[11];
    int major, minor;
    if (std::sscanf(l.c_str(), "%d %d %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                    &major, &minor, name, &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7],
                    &f[8], &f[9], &f[10]) < 14)
      continue;
    QString n = QString::fromLatin1(name);
    // Whole physical disks only: /sys/block lists them, partitions live below.
    if (n.startsWith("loop") || n.startsWith("ram") || n.startsWith("zram") || n.startsWith("dm-"))
      continue;
    if (!fs::exists("/sys/block/" + n + "/device")) continue;
    DiskPrev now{f[2] * 512, f[6] * 512, f[9]};
    cur.insert(n, now);
    DiskInfo d;
    d.name = n;
    d.model = fs::readText("/sys/block/" + n + "/device/model");
    d.size = quint64(fs::readInt("/sys/block/" + n + "/size", 0)) * 512;
    d.rotational = fs::readInt("/sys/block/" + n + "/queue/rotational", 0) == 1;
    auto it = diskPrev_.constFind(n);
    if (it != diskPrev_.constEnd()) {
      d.readBps = Delta(now.rd, it->rd) / s.intervalSec;
      d.writeBps = Delta(now.wr, it->wr) / s.intervalSec;
      d.busy = qBound(0.0, Delta(now.ticks, it->ticks) / (s.intervalSec * 10.0), 100.0);
    }
    s.disks << d;
  }
  diskPrev_ = cur;
}

void Sampler::ReadNets(Snapshot &s) {
  std::string nd = fs::readRaw("/proc/net/dev");
  QHash<QString, NetPrev> cur;
  QHash<QString, QString> ips;
  for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
    for (const QNetworkAddressEntry &e : iface.addressEntries()) {
      if (e.ip().protocol() == QAbstractSocket::IPv4Protocol) {
        ips.insert(iface.name(), e.ip().toString());
        break;
      }
    }
  }
  size_t pos = 0;
  Line(nd, pos);
  Line(nd, pos);
  while (pos < nd.size()) {
    std::string l = Line(nd, pos);
    size_t c = l.find(':');
    if (c == std::string::npos) continue;
    QString n = QString::fromStdString(l.substr(0, c)).trimmed();
    if (n == "lo") continue;
    auto v = Counters(l.c_str() + c + 1, l.c_str() + l.size());
    if (v.size() < 9) continue;
    NetPrev now{v[0], v[8]};
    cur.insert(n, now);
    NetInfo ni;
    ni.name = n;
    QString base = "/sys/class/net/" + n;
    ni.physical = fs::exists(base + "/device");
    ni.wireless = fs::exists(base + "/wireless");
    ni.up = fs::readText(base + "/operstate") == "up";
    ni.speedMbps = fs::readInt(base + "/speed");
    ni.ipv4 = ips.value(n);
    auto it = netPrev_.constFind(n);
    if (it != netPrev_.constEnd()) {
      ni.rxBps = Delta(now.rx, it->rx) / s.intervalSec;
      ni.txBps = Delta(now.tx, it->tx) / s.intervalSec;
    }
    s.nets << ni;
  }
  netPrev_ = cur;
  // The "primary" address is the one on the interface carrying the default route.
  std::string rt = fs::readRaw("/proc/net/route");
  pos = 0;
  Line(rt, pos);
  while (pos < rt.size()) {
    std::string l = Line(rt, pos);
    char iface[32];
    unsigned dest = 1;
    if (std::sscanf(l.c_str(), "%31s %x", iface, &dest) == 2 && dest == 0) {
      QString ip = ips.value(QString::fromLatin1(iface));
      if (!ip.isEmpty()) {
        s.primaryIp = ip;
        break;
      }
    }
  }
  std::stable_sort(s.nets.begin(), s.nets.end(), [](const NetInfo &a, const NetInfo &b) {
    return a.physical > b.physical;
  });
}

void Sampler::ReadGpus(Snapshot &s) {
  if (gpuStatic_.isEmpty()) {
    // A GPU renders: it has a render node. Display-only cards (simpledrm left
    // over at boot, an SoC's display controller, USB adapters, vkms) have none.
    QSet<QString> renders;
    for (const QString &c : fs::list("/sys/class/drm"))
      if (c.startsWith("renderD")) renders << QFile::symLinkTarget("/sys/class/drm/" + c + "/device");
    for (const QString &c : fs::list("/sys/class/drm")) {
      if (!c.startsWith("card") || c.contains('-')) continue;
      QString dev = "/sys/class/drm/" + c + "/device";
      if (!renders.contains(QFile::symLinkTarget(dev))) continue;
      GpuInfo g;
      g.card = c;
      g.pciAddress = QFileInfo(QFile::symLinkTarget(dev)).fileName();
      quint16 vendor = quint16(fs::readText(dev + "/vendor").toUInt(nullptr, 16));
      quint16 device = quint16(fs::readText(dev + "/device").toUInt(nullptr, 16));
      QString link = QFile::symLinkTarget(dev + "/driver");
      g.driver = link.section('/', -1);
      // Platform GPUs (Arm SoCs) have no PCI ids: their device node names them.
      QString vendorName = vendor ? hwids::PciVendor(vendor).section(' ', 0, 0) : QString();
      // pci.ids names the chip and puts the marketing name in brackets
      // ("TigerLake-LP GT2 [Iris Xe Graphics]"): the bracket is what people know.
      QString model = hwids::PciDevice(vendor, device);
      if (int open = model.indexOf('['), close = model.lastIndexOf(']'); open >= 0 && close > open + 1)
        model = model.mid(open + 1, close - open - 1);
      g.name = vendor ? (vendorName + ' ' + model).trimmed() : g.pciAddress;
      if (g.name.isEmpty()) g.name = g.driver;
      gpuStatic_ << g;
    }
  }
  for (GpuInfo g : gpuStatic_) {
    QString dev = "/sys/class/drm/" + g.card + "/device";
    // A sleeping discrete GPU is woken by most of its attributes (amdgpu's
    // busy and VRAM files, hwmon, nvidia-smi): leave it asleep, it is idle.
    if (fs::readText(dev + "/power/runtime_status") == "suspended") {
      g.asleep = true;
      g.util = 0;
      g.utilSource = "asleep";
      s.gpus << g;
      continue;
    }
    qint64 busy = fs::readInt(dev + "/gpu_busy_percent");
    if (busy >= 0) {
      g.util = busy;
      g.utilSource = "sysfs";
    }
    qint64 used = fs::readInt(dev + "/mem_info_vram_used");
    qint64 total = fs::readInt(dev + "/mem_info_vram_total");
    if (used >= 0 && total > 0) {
      g.vramUsed = used;
      g.vramTotal = total;
    }
    // Clock: i915 per card, xe per tile and GT, amdgpu and others through devfreq.
    qint64 freq = fs::readInt("/sys/class/drm/" + g.card + "/gt_act_freq_mhz");
    if (freq <= 0) freq = fs::readInt(dev + "/tile0/gt0/freq0/act_freq");
    if (freq <= 0)
      for (const QString &f : fs::list(dev + "/devfreq"))
        if (qint64 hz = fs::readInt(dev + "/devfreq/" + f + "/cur_freq"); hz > 0) freq = hz / 1000000;
    if (freq > 0) g.mhz = freq;
    for (const QString &h : fs::list(dev + "/hwmon")) {
      qint64 t = fs::readInt(dev + "/hwmon/" + h + "/temp1_input");
      if (t > 0) g.tempC = t / 1000.0;
    }
    s.gpus << g;
  }
  if (detail_) ReadNvidiaSmi(s);
}

namespace {
// Runs argv, returns its stdout, or nothing if it fails or outlives the
// deadline. A hung nvidia-smi (a wedged driver leaves it in D state) is sent
// SIGKILL and left for ReapChildren: never waited for, so it cannot stall sampling.
std::optional<std::string> RunBounded(const QString &program, const QStringList &args, int timeoutMs,
                                      QVector<pid_t> *stuck) {
  int out[2];
  if (::pipe2(out, O_CLOEXEC) != 0) return std::nullopt;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  QVector<QByteArray> storage{program.toLocal8Bit()};
  for (const QString &a : args) storage << a.toLocal8Bit();
  QVector<char *> argv;
  for (QByteArray &b : storage) argv << b.data();
  argv << nullptr;
  pid_t pid = -1;
  const int rc = ::posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(out[1]);
  if (rc != 0) {
    ::close(out[0]);
    return std::nullopt;
  }
  std::string text;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  bool eof = false;
  while (!eof) {
    const int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    pollfd fd{out[0], POLLIN, 0};
    if (left <= 0 || ::poll(&fd, 1, left) <= 0) break;
    char buf[4096];
    const ssize_t n = ::read(out[0], buf, sizeof buf);
    if (n > 0) text.append(buf, size_t(n));
    else if (n == 0 || errno != EINTR) eof = true;
  }
  ::close(out[0]);
  // Closing its output, it is about to exit: give it the rest of the deadline.
  int status = 0;
  pid_t done = 0;
  while ((done = ::waitpid(pid, &status, WNOHANG)) == 0 && std::chrono::steady_clock::now() < deadline) ::usleep(2000);
  if (done == 0) {
    ::kill(pid, SIGKILL);
    *stuck << pid;
    return std::nullopt;
  }
  if (done < 0) return std::nullopt;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
  return text;
}
}  // namespace

void Sampler::ReadNvidiaSmi(Snapshot &s) {
  // NVIDIA's own driver publishes no counters in sysfs and no engine time in
  // fdinfo; nvidia-smi is how it is read, for the awake cards it drives only.
  for (int i = int(nvidiaStuck_.size()) - 1; i >= 0; --i)
    if (::waitpid(nvidiaStuck_[i], nullptr, WNOHANG) != 0) nvidiaStuck_.remove(i);
  QStringList awake;
  for (const GpuInfo &g : s.gpus)
    if (g.driver == "nvidia" && !g.asleep && g.util < 0) awake << g.pciAddress;
  if (awake.isEmpty()) return;
  if (!nvidiaLooked_) {
    nvidiaLooked_ = true;
    nvidiaSmi_ = QStandardPaths::findExecutable("nvidia-smi");
  }
  if (nvidiaSmi_.isEmpty()) return;
  // Every few seconds, not every sample: each query wakes the driver, and on a
  // hybrid laptop constant queries would keep the card from going back to sleep.
  const auto now = std::chrono::steady_clock::now();
  if (now >= nvidiaNext_ && nvidiaStuck_.isEmpty()) {
    nvidiaNext_ = now + std::chrono::seconds(5);
    const auto out = RunBounded(nvidiaSmi_,
                                {"--query-gpu=pci.bus_id,utilization.gpu,memory.used,memory.total,temperature.gpu,clocks.gr",
                                 "--format=csv,noheader,nounits", "--id=" + awake.join(',')},
                                1000, &nvidiaStuck_);
    nvidia_.clear();
    for (const QString &line : QString::fromStdString(out.value_or(std::string())).split('\n', Qt::SkipEmptyParts)) {
      const QStringList f = line.split(',');
      if (f.size() >= 6) nvidia_.insert(f[0].trimmed().right(7).toLower(), f);  // the domain is padded to 8 digits
    }
    // Broken (driver and library out of step after an update, hung, gone): try again much later.
    if (nvidia_.isEmpty()) nvidiaNext_ = now + std::chrono::minutes(5);
  }
  for (GpuInfo &g : s.gpus) {
    if (g.driver != "nvidia" || g.asleep) continue;
    const auto it = nvidia_.constFind(g.pciAddress.right(7).toLower());
    if (it == nvidia_.constEnd()) continue;
    const QStringList &f = *it;
    bool ok = false;
    if (double v = f[1].trimmed().toDouble(&ok); ok) {
      g.util = std::clamp(v, 0.0, 100.0);
      g.utilSource = "nvidia-smi";
    }
    if (double used = f[2].trimmed().toDouble(&ok); ok) g.vramUsed = used * 1048576.0;
    if (double total = f[3].trimmed().toDouble(&ok); ok && total > 0) g.vramTotal = total * 1048576.0;
    if (double t = f[4].trimmed().toDouble(&ok); ok) g.tempC = t;
    if (double mhz = f[5].trimmed().toDouble(&ok); ok && mhz > 0) g.mhz = mhz;
  }
}

void Sampler::FinishGpus(Snapshot &s) {
  // Drivers without a busy counter (i915, xe, most Arm GPUs) report each
  // client's engine time in fdinfo. The busiest engine, summed over our
  // processes' clients, is the GPU's load as this user can see it.
  for (GpuInfo &g : s.gpus) {
    if (g.util >= 0 || g.asleep) continue;
    auto it = gpuEngines_.constFind(g.pciAddress);
    if (it == gpuEngines_.constEnd()) it = gpuEngines_.constFind("driver:" + g.driver);
    if (it == gpuEngines_.constEnd()) continue;
    double best = 0;
    for (double v : *it) best = qMax(best, v);
    g.util = std::clamp(best, 0.0, 100.0);
    g.utilSource = "clients";
  }
}

void Sampler::ReadSensors(Snapshot &s) {
  double pkg = -1, hottest = -1;
  QStringList hwmons = fs::list("/sys/class/hwmon");
  std::sort(hwmons.begin(), hwmons.end());
  QHash<QString, int> chipCount;
  for (const QString &h : hwmons) ++chipCount[fs::readText("/sys/class/hwmon/" + h + "/name")];
  for (const QString &h : hwmons) {
    QString base = "/sys/class/hwmon/" + h;
    QString chip = fs::readText(base + "/name");
    // Two NVMe drives both call themselves "nvme": tell them apart.
    QString chipLabel = chipCount.value(chip) > 1 ? QString("%1 (%2)").arg(chip, h) : chip;
    for (const QString &f : fs::list(base)) {
      if (!f.endsWith("_input")) continue;
      QString stem = f.left(f.size() - 6);
      int digit = 0;
      while (digit < stem.size() && !stem[digit].isDigit()) ++digit;
      QString kind = stem.left(digit);
      qint64 raw = fs::readInt(base + '/' + f, LLONG_MIN);
      if (raw == LLONG_MIN) continue;
      Sensor sn;
      sn.chip = chipLabel;
      sn.kind = kind;
      sn.label = fs::readText(base + '/' + stem + "_label");
      if (sn.label.isEmpty()) sn.label = stem;
      if (kind == "temp") sn.value = raw / 1000.0;
      else if (kind == "power") sn.value = raw / 1e6;
      else if (kind == "in" || kind == "curr") sn.value = raw / 1000.0;  // mV → V, mA → A
      else sn.value = raw;  // fan rpm
      if (kind == "temp") {
        if ((chip == "coretemp" && sn.label.startsWith("Package")) ||
            (chip == "k10temp" && (sn.label == "Tctl" || sn.label == "Tdie")) ||
            (chip == "zenpower" && sn.label == "Tdie"))
          pkg = sn.value;
        if (sn.value < 150) hottest = qMax(hottest, sn.value);
      }
      s.sensors << sn;
    }
  }
  s.cpuTemp = pkg >= 0 ? pkg : hottest;
  std::sort(s.sensors.begin(), s.sensors.end(), [](const Sensor &a, const Sensor &b) {
    return std::tie(a.chip, a.kind, a.label) < std::tie(b.chip, b.kind, b.label);
  });
}

void Sampler::ReadPower(Snapshot &s) {
  PowerInfo &p = s.power;
  bool sawMains = false, anyOnline = false;
  for (const QString &ps : fs::list("/sys/class/power_supply")) {
    QString base = "/sys/class/power_supply/" + ps;
    QString type = fs::readText(base + "/type");
    if (type == "Mains" || type.startsWith("USB")) {
      sawMains = true;
      anyOnline |= fs::readInt(base + "/online", 0) == 1;
    }
    if (type == "Battery" && fs::readText(base + "/scope") != "Device") {
      p.hasBattery = true;
      p.batteryPct = int(fs::readInt(base + "/capacity", -1));
      p.batteryStatus = fs::readText(base + "/status");
      qint64 pw = fs::readInt(base + "/power_now");
      if (pw < 0) {
        // Some drivers report the discharge current as negative.
        qint64 cur = fs::readInt(base + "/current_now", LLONG_MIN), volt = fs::readInt(base + "/voltage_now");
        if (cur != LLONG_MIN && volt > 0) pw = qint64(std::abs(double(cur)) * double(volt) / 1e6);
      }
      if (pw >= 0) p.batteryW = std::abs(pw) / 1e6;
    }
  }
  p.onAc = sawMains ? anyOnline : !p.hasBattery || p.batteryStatus != "Discharging";
  // RAPL energy counters are root-only since CVE-2020-8694; say so instead of guessing.
  quint64 &lastEnergy = raplEnergy_;
  auto &lastAt = raplAt_;
  QString rapl = "/sys/class/powercap/intel-rapl:0/energy_uj";
  if (fs::exists(rapl)) {
    qint64 e = fs::readInt(rapl);
    auto now = std::chrono::steady_clock::now();
    if (e < 0) {
      p.raplRestricted = true;
    } else {
      if (lastEnergy && quint64(e) >= lastEnergy)
        p.cpuPackageW = (e - lastEnergy) / 1e6 / std::chrono::duration<double>(now - lastAt).count();
      lastEnergy = e;
      lastAt = now;
    }
  }
  QString cf = "/sys/devices/system/cpu/cpu0/cpufreq/";
  p.governor = fs::readText(cf + "scaling_governor");
  p.epp = fs::readText(cf + "energy_performance_preference");
  p.driver = fs::readText(cf + "scaling_driver");
}

QString Sampler::UserName(uid_t uid) {
  auto it = users_.constFind(uid);
  if (it != users_.constEnd()) return *it;
  passwd pw, *res = nullptr;
  char buf[4096];
  QString name = getpwuid_r(uid, &pw, buf, sizeof buf, &res) == 0 && res
                     ? QString::fromLocal8Bit(pw.pw_name)
                     : QString::number(uid);
  users_.insert(uid, name);
  return name;
}

void Sampler::ReadProcesses(Snapshot &s) {
  std::unordered_map<ProcessKey, ProcPrev, ProcessKeyHash> cur;
  cur.reserve(procPrev_.size() + 32);
  const double cpuScale = 100.0 / (double(ticksPerSec_) * s.intervalSec * qMax(1, logical_));
  DIR *d = ::opendir("/proc");
  if (!d) return;
  const bool rescanDrm = detail_ && --drmScanCountdown_ <= 0;
  if (rescanDrm) drmScanCountdown_ = 5;
  ++processPass_;
  const auto sampledAt = std::chrono::steady_clock::now();
  char path[64];
  while (dirent *e = ::readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    int pid = std::atoi(e->d_name);
    std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
    std::string st = fs::readRaw(path, 4096);
    size_t lp = st.find('('), rp = st.rfind(')');
    if (lp == std::string::npos || rp == std::string::npos) continue;
    ProcessInfo p;
    p.key.pid = pid;
    p.name = QString::fromUtf8(st.data() + lp + 1, int(rp - lp - 1));
    const char *q = st.c_str() + rp + 2;
    p.state = QChar(*q);
    // Fields after "state", numbered as in proc(5) starting at field 4 (ppid).
    QVector<quint64> f;
    {
      const char *end = st.c_str() + st.size();
      const char *x = q + 1;
      while (x < end && f.size() < 22) {
        while (x < end && *x == ' ') ++x;
        char *ne = nullptr;
        long long v = std::strtoll(x, &ne, 10);
        if (ne == x) break;
        f << quint64(v);
        x = ne;
      }
    }
    if (f.size() < 21) continue;
    p.ppid = int(f[0]);
    quint64 ticks = f[10] + f[11];  // utime + stime
    p.nice = int(qint64(f[15]));
    p.threads = int(f[16]);
    p.key.start = f[18];
    p.vsize = f[19];
    p.rss = f[20] * quint64(pageSize_);
    s.threadCount += p.threads;

    std::snprintf(path, sizeof path, "/proc/%d/status", pid);
    std::string status = fs::readRaw(path, 8192);
    size_t up = status.find("\nUid:");
    if (up != std::string::npos) p.uid = uid_t(std::strtoul(status.c_str() + up + 5, nullptr, 10));
    size_t anon = status.find("\nRssAnon:");
    if (anon != std::string::npos) p.anon = std::strtoull(status.c_str() + anon + 10, nullptr, 10) * 1024;
    p.user = UserName(p.uid);

    auto it = procPrev_.find(p.key);
    const ProcPrev *prev = it != procPrev_.end() ? &it->second : nullptr;
    ProcPrev now;
    now.ticks = ticks;
    // The costly reads take turns: each process every few samples, staggered by
    // PID so no sample does them all. The values carry over in between.
    const bool turn = !prev || (quint64(pid) + processPass_) % 4 == 0;
    if (turn) {
      std::snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
      std::string cl = fs::readRaw(path, 4096);
      for (char &c : cl)
        if (c == 0) c = ' ';
      now.cmdline = QString::fromUtf8(cl).trimmed();
    } else {
      now.cmdline = prev->cmdline;
    }
    p.cmdline = now.cmdline;
    if (detail_ && !turn) {
      now.switches = prev->switches;
      now.hasSwitches = prev->hasSwitches;
      now.switchesAt = prev->switchesAt;
      now.wakeups = prev->wakeups;
      if (now.wakeups >= 0) p.wakeups = now.wakeups;
      if (!rescanDrm) now.drmFds = prev->drmFds;
    } else if (detail_) {
      now.switchesAt = sampledAt;
      // Each time a thread is scheduled in it was woken: the cost that keeps a laptop awake.
      std::snprintf(path, sizeof path, "/proc/%d/task", pid);
      if (DIR *tasks = ::opendir(path)) {
        char tp[96];
        while (dirent *t = ::readdir(tasks)) {
          if (t->d_name[0] < '0' || t->d_name[0] > '9') continue;
          std::snprintf(tp, sizeof tp, "/proc/%d/task/%.20s/schedstat", pid, t->d_name);
          // "run_ns wait_ns timeslices"; empty without CONFIG_SCHED_INFO
          std::string ss = fs::readRaw(tp, 128);
          const char *x = ss.c_str();
          char *e = nullptr;
          int fields = 0;
          quint64 v = 0;
          for (; fields < 3; ++fields) {
            v = std::strtoull(x, &e, 10);
            if (e == x) break;
            x = e;
          }
          if (fields == 3) now.switches.insert(std::atoi(t->d_name), v);
        }
        ::closedir(tasks);
        now.hasSwitches = !now.switches.isEmpty();
      }
      // Only threads alive in both samples: one that exits must not cancel the others.
      // Over the time since this process's own last read, not one sample's interval.
      const double span = prev ? std::chrono::duration<double>(sampledAt - prev->switchesAt).count() : 0;
      if (prev && prev->hasSwitches && now.hasSwitches && span > 0.05) {
        quint64 sum = 0;
        for (auto t = now.switches.cbegin(); t != now.switches.cend(); ++t) {
          auto before = prev->switches.constFind(t.key());
          if (before != prev->switches.constEnd()) sum += Delta(t.value(), *before);
        }
        p.wakeups = now.wakeups = sum / span;
      }
      if (prev && !rescanDrm) now.drmFds = prev->drmFds;
    }
    std::snprintf(path, sizeof path, "/proc/%d/io", pid);
    std::string io = fs::readRaw(path, 1024);
    if (!io.empty()) {
      now.io = true;
      size_t r = io.find("\nread_bytes:"), w = io.find("\nwrite_bytes:");
      if (r != std::string::npos) now.readBytes = std::strtoull(io.c_str() + r + 12, nullptr, 10);
      if (w != std::string::npos) now.writeBytes = std::strtoull(io.c_str() + w + 13, nullptr, 10);
    }
    if (prev) {
      p.cpu = double(Delta(ticks, prev->ticks)) * cpuScale;
      if (now.io && prev->io) {
        p.readBps = Delta(now.readBytes, prev->readBytes) / s.intervalSec;
        p.writeBps = Delta(now.writeBytes, prev->writeBytes) / s.intervalSec;
      }
    } else if (now.io) {
      p.readBps = p.writeBps = 0;
    }
    // Open files: counted on this process's turn (a browser holds hundreds), and
    // whenever the GPU clients are looked for.
    const bool scan = detail_ && (rescanDrm || !prev);
    if (p.uid == getuid() && !turn && !scan && prev) {
      p.fds = now.fds = prev->fds;
      now.drmFds = prev->drmFds;
      if (detail_) ReadGpuClients(p, now, prev, s.intervalSec);
    } else if (p.uid == getuid()) {
      std::snprintf(path, sizeof path, "/proc/%d/fd", pid);
      if (DIR *fd = ::opendir(path)) {
        int n = 0;
        // GPU clients are rare and long-lived: look for /dev/dri links only now and then.
        if (scan) now.drmFds.clear();
        while (dirent *x = ::readdir(fd)) {
          if (x->d_name[0] == '.') continue;
          ++n;
          if (scan) {
            char target[24];
            ssize_t len = ::readlinkat(::dirfd(fd), x->d_name, target, sizeof target);
            if (len >= 9 && std::memcmp(target, "/dev/dri/", 9) == 0) now.drmFds << std::atoi(x->d_name);
          }
        }
        ::closedir(fd);
        p.fds = now.fds = n;
      }
      if (detail_) ReadGpuClients(p, now, prev, s.intervalSec);
    }
    cur.emplace(p.key, now);
    s.processes << p;
  }
  ::closedir(d);
  procPrev_.swap(cur);
}

void Sampler::MarkWindowedProcesses(Snapshot &s) {
  // Asking Hyprland every fifth sample is plenty: windows open far slower
  // than processes change.
  if (--windowPollCountdown_ <= 0) {
    windowPollCountdown_ = 5;
    windows_.clear();
    for (const hypr::Window &w : hypr::Clients())
      if (!windows_.contains(w.pid)) windows_.insert(w.pid, {w.cls, w.title, w.address, w.workspace});
  }
  for (ProcessInfo &p : s.processes) {
    auto w = windows_.constFind(p.key.pid);
    p.hasWindow = w != windows_.constEnd();
    if (!p.hasWindow) continue;
    p.windowClass = w->cls;
    p.windowTitle = w->title;
    p.windowAddress = w->address;
    p.workspace = w->workspace;
  }
}

void Sampler::ReadThrottle(Snapshot &s) {
  // Core counters are per CPU; the package counter repeats on every CPU of a package, so count it once.
  qint64 total = -1;
  for (int i = 0; i < qMax(1, logical_); ++i) {
    const QString base = QString("/sys/devices/system/cpu/cpu%1/thermal_throttle/").arg(i);
    qint64 core = fs::readInt(base + "core_throttle_count");
    if (core < 0) continue;
    total = qMax<qint64>(total, 0) + core;
    if (i == 0) total += qMax<qint64>(0, fs::readInt(base + "package_throttle_count"));
  }
  s.throttleEvents = total;
}

void Sampler::ReadVolumes(Snapshot &s) {
  if (--volumeCountdown_ <= 0) {
    volumeCountdown_ = 10;
    volumes_.clear();
    static const QSet<QString> real = {"ext4", "ext3", "ext2", "btrfs", "xfs", "f2fs", "zfs", "bcachefs",
                                       "vfat", "exfat", "ntfs", "ntfs3", "jfs", "reiserfs", "nilfs2"};
    QSet<QString> devices;  // btrfs subvolumes share one device and one free-space figure
    std::string mounts = fs::readRaw("/proc/self/mounts");
    size_t pos = 0;
    while (pos < mounts.size()) {
      QStringList f = QString::fromStdString(Line(mounts, pos)).split(' ');
      if (f.size() < 3 || !real.contains(f[2]) || devices.contains(f[0])) continue;
      // /proc/mounts escapes space, tab, newline and backslash as \ooo.
      QString mount;
      for (int i = 0; i < f[1].size(); ++i) {
        bool octal = false;
        const int code = f[1][i] == '\\' && i + 3 < f[1].size() ? f[1].mid(i + 1, 3).toInt(&octal, 8) : 0;
        if (octal && code > 0) {
          mount += QChar(code);
          i += 3;
        } else {
          mount += f[1][i];
        }
      }
      struct statvfs st;
      if (::statvfs(mount.toLocal8Bit().constData(), &st) != 0 || st.f_blocks == 0) continue;
      devices.insert(f[0]);
      volumes_.push_back({mount, quint64(st.f_blocks) * st.f_frsize, quint64(st.f_bavail) * st.f_frsize});
    }
  }
  s.volumes = volumes_;
}

void Sampler::ReadGpuClients(ProcessInfo &p, ProcPrev &now, const ProcPrev *prev, double intervalSec) {
  if (now.drmFds.isEmpty()) return;
  // drm-usage-stats: one fdinfo per open DRM fd; dup'ed fds repeat a client, so count each id once.
  QSet<QString> seen, owned;  // owned: clients this process is the first to show, this sample
  QHash<QString, int> capacity;
  quint64 memory = 0;
  bool any = false;
  char path[64];
  for (int fd : now.drmFds) {
    std::snprintf(path, sizeof path, "/proc/%d/fdinfo/%d", p.key.pid, fd);
    std::string info = fs::readRaw(path, 16384);
    QString client, pdev, driver;
    QHash<QString, quint64> engines;
    quint64 resident = 0, legacy = 0;
    size_t pos = 0;
    while (pos < info.size()) {
      std::string l = Line(info, pos);
      if (l.compare(0, 4, "drm-") != 0) continue;
      size_t c = l.find(':');
      if (c == std::string::npos) continue;
      const QString key = QString::fromLatin1(l.data() + 4, int(c - 4));
      const char *v = l.c_str() + c + 1;
      char *unit = nullptr;
      const quint64 n = std::strtoull(v, &unit, 10);
      while (unit && (*unit == ' ' || *unit == '\t')) ++unit;
      const quint64 scale = !unit ? 1 : std::strncmp(unit, "KiB", 3) == 0 ? 1024ull
                            : std::strncmp(unit, "MiB", 3) == 0 ? 1024ull * 1024 : std::strncmp(unit, "GiB", 3) == 0 ? 1ull << 30 : 1;
      if (key == "client-id") client = QString::fromLatin1(v).trimmed();
      else if (key == "pdev") pdev = QString::fromLatin1(v).trimmed();
      else if (key == "driver") driver = QString::fromLatin1(v).trimmed();
      else if (key.startsWith("engine-capacity-")) capacity[key.mid(16)] = int(n);
      else if (key.startsWith("engine-")) engines[key.mid(7)] += n;                 // ns busy (i915, amdgpu, msm)
      else if (key.startsWith("total-cycles-")) engines["t:" + key.mid(13)] += n;   // xe: cycles of the GPU clock
      else if (key.startsWith("cycles-")) engines["c:" + key.mid(7)] += n;          // xe: cycles this client used
      else if (key.startsWith("resident-")) resident += n * scale;
      else if (key.startsWith("memory-")) legacy += n * scale;  // older amdgpu: drm-memory-vram/gtt
    }
    // drm-pdev is written for PCI devices only; an SoC GPU is known by its driver.
    const QString device = pdev.isEmpty() ? "driver:" + driver : pdev;
    const QString id = device + '/' + client;
    if (client.isEmpty() || seen.contains(id)) continue;
    seen.insert(id);
    // A client shared with another process (an inherited or passed fd) counts once for the GPU.
    if (!gpuClientsSeen_.contains(id)) {
      gpuClientsSeen_.insert(id);
      owned.insert(id);
    }
    any = true;
    // Kept per client: a client that appears (rescan) or goes away between two
    // samples must not turn its whole lifetime into one sample's busy time.
    for (auto e = engines.cbegin(); e != engines.cend(); ++e) now.engineNs[id + '|' + e.key()] = e.value();
    memory += resident ? resident : legacy;
  }
  if (!any) return;
  p.gpuMemory = memory;
  p.gpu = 0;
  if (!prev) return;
  QHash<QString, double> perEngine;  // engine name -> percent, summed over clients
  for (auto e = now.engineNs.cbegin(); e != now.engineNs.cend(); ++e) {
    const QString client = e.key().section('|', 0, 0), engine = e.key().section('|', 1);
    if (engine.startsWith("t:")) continue;
    auto before = prev->engineNs.constFind(e.key());
    if (before == prev->engineNs.constEnd()) continue;
    const double used = double(Delta(e.value(), *before));
    double pct;
    if (engine.startsWith("c:")) {
      const QString total = client + "|t:" + engine.mid(2);
      if (!prev->engineNs.contains(total)) continue;
      const double span = double(Delta(now.engineNs.value(total), prev->engineNs.value(total)));
      pct = span > 0 ? 100.0 * used / span : 0;
    } else {
      pct = 100.0 * used / (intervalSec * 1e9) / qMax(1, capacity.value(engine, 1));
    }
    const QString name = engine.startsWith("c:") ? engine.mid(2) : engine;
    perEngine[name] += pct;
    if (owned.contains(client)) gpuEngines_[client.section('/', 0, 0)][name] += pct;
  }
  double best = 0;
  for (double v : perEngine) best = qMax(best, v);
  p.gpu = std::clamp(best, 0.0, 100.0);
}

}  // namespace td
