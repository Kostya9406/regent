#include "core/Benchmarks.h"

#include <QElapsedTimer>
#include <QDir>
#include <QThread>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTemporaryFile>
#include <algorithm>
#include <cstring>
#include <new>
#include <vector>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <sys/vfs.h>

namespace td::benchmarks {

Result Run(Kind kind, int durationMs, const std::atomic<bool> &cancel) {
  Result result;
  result.kind = kind;
  if (kind != Kind::Cpu && kind != Kind::Memory) {
    result.error = QStringLiteral("This benchmark requires its dedicated runner.");
    return result;
  }
  if (cancel.load()) {
    result.cancelled = true;
    return result;
  }
  durationMs = std::clamp(durationMs, 10, 10000);
  QElapsedTimer clock;
  quint64 work = 0;
  quint64 state = 0x123456789abcdefULL;
  try {
    if (kind == Kind::Cpu) {
      clock.start();
      do {
        // A dependency chain of xorshift64 operations measures integer work
        // on one CPU thread. The observable checksum prevents dead-code removal.
        for (int i = 0; i < 4096; ++i) {
          state ^= state << 13;
          state ^= state >> 7;
          state ^= state << 17;
        }
        work += 4096;
      } while (!cancel.load() && clock.elapsed() < durationMs);
    } else {
      constexpr size_t bufferSize = 16 * 1024 * 1024;
      constexpr size_t chunkSize = 1024 * 1024;
      std::vector<unsigned char> a(bufferSize), b(bufferSize);
      for (size_t offset = 0; offset < bufferSize && !cancel.load(); offset += chunkSize)
        std::memset(a.data() + offset, static_cast<int>(offset / chunkSize + 1), chunkSize);
      clock.start();
      size_t offset = 0;
      do {
        if (cancel.load()) break;
        // Alternate direction each complete pass. Each copy is read back
        // through volatile storage so an optimizer cannot remove the workload.
        std::memcpy(b.data() + offset, a.data() + offset, chunkSize);
        const volatile unsigned char *sample = b.data() + offset;
        state += sample[(work / chunkSize) % chunkSize];
        work += chunkSize;
        offset += chunkSize;
        if (offset == bufferSize) {
          a.swap(b);
          offset = 0;
        }
      } while (clock.elapsed() < durationMs);
    }
    result.seconds = clock.nsecsElapsed() / 1e9;
    result.score = result.seconds > 0 ? work / result.seconds : 0;
    // Scores count CPU iterations or copied bytes (one-way payload), respectively.
    result.score /= kind == Kind::Cpu ? 1e6 : 1024.0 * 1024 * 1024;
    result.checksum = state;
    // A Stop that lands after the full duration does not void a finished run.
    result.cancelled = cancel.load() && clock.elapsed() < durationMs;
  } catch (const std::bad_alloc &) {
    result.error = QStringLiteral("Not enough memory to allocate benchmark buffers.");
  }
  return result;
}

Result RunDisk(const QString &directory, int durationMs, const std::atomic<bool> &cancel) {
  Result result;
  result.kind = Kind::Disk;
  result.cancelled = cancel.load();
  if (result.cancelled) return result;
  durationMs = std::clamp(durationMs, 10, 10000);
  if (directory.isEmpty() || !QDir(directory).exists()) {
    result.error = QStringLiteral("Folder does not exist: ") + directory;
    return result;
  }
  QTemporaryFile file(QDir(directory).filePath(".regent-benchmark-XXXXXX"));
  if (!file.open()) {
    result.error = QStringLiteral("Cannot create a temporary benchmark file in this folder: ") + file.errorString();
    return result;
  }
  // tmpfs (/tmp on Arch) is memory: the "disk" figures would measure RAM.
  struct statfs fs {};
  constexpr long kTmpfsMagic = 0x01021994;
  const bool inMemory = ::statfs(QFile::encodeName(directory).constData(), &fs) == 0 && fs.f_type == kTmpfsMagic;
  constexpr qint64 cap = 32 * 1024 * 1024;
  constexpr qint64 chunk = 1024 * 1024;
  QByteArray block(chunk, '\0');
  // Reproducible nonzero data avoids trivial all-zero compression on filesystems.
  quint64 state = 0x123456789abcdefULL;
  for (qint64 i = 0; i < chunk && !cancel.load(); ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    block[i] = static_cast<char>(state);
  }
  QElapsedTimer clock;
  clock.start();
  qint64 written = 0;
  while (written < cap && !cancel.load() && clock.elapsed() < std::max(1, durationMs / 2)) {
    const qint64 n = file.write(block.constData(), chunk);
    if (n <= 0) { result.error = file.errorString(); break; }
    written += n;
  }
  if (!file.flush() && result.error.isEmpty()) result.error = file.errorString();
  result.seconds = clock.nsecsElapsed() / 1e9;
  result.score = result.seconds > 0 ? written / result.seconds / (1024 * 1024) : 0;
  qint64 read = 0;
  const double writeSeconds = result.seconds;
  if (result.error.isEmpty() && !cancel.load()) {
    if (!file.seek(0)) result.error = file.errorString();
    while (result.error.isEmpty() && read < written && !cancel.load() && clock.elapsed() < durationMs) {
      const qint64 n = file.read(block.data(), std::min(chunk, written - read));
      if (n <= 0) { result.error = QStringLiteral("Could not read the temporary benchmark file: ") + file.errorString(); break; }
      result.checksum += static_cast<unsigned char>(block[0]);
      read += n;
    }
    result.secondarySeconds = clock.nsecsElapsed() / 1e9 - writeSeconds;
    result.secondaryScore = result.secondarySeconds > 0 ? read / result.secondarySeconds / (1024 * 1024) : 0;
  }
  const bool complete = result.error.isEmpty() && written > 0 && read >= written;
  result.cancelled = cancel.load() && !complete;
  // One pass: a folder may be called "%1".
  result.detail = QStringLiteral("%4%1 MiB written, %2 MiB read; %3")
                      .arg(QString::number(written / (1024.0 * 1024), 'f', 1), QString::number(read / (1024.0 * 1024), 'f', 1),
                           directory, inMemory ? QStringLiteral("tmpfs, measures RAM not a disk; ") : QStringLiteral("Cached I/O; "));
  return result;  // QTemporaryFile removes only its own private file.
}

Result RunGpu(QOffscreenSurface *surface, int durationMs, const std::atomic<bool> &cancel) {
  Result result;
  result.kind = Kind::Gpu;
  result.cancelled = cancel.load();
  if (result.cancelled) return result;
  if (!surface || !surface->isValid() || !QOpenGLContext::supportsThreadedOpenGL()) {
    result.error = QStringLiteral("GPU benchmark unavailable: no threaded offscreen OpenGL surface.");
    return result;
  }
  QOpenGLContext context;
  context.setFormat(surface->format());
  if (!context.create() || !context.makeCurrent(surface)) {
    result.error = QStringLiteral("GPU benchmark unavailable: cannot create an OpenGL context.");
    return result;
  }
  QOpenGLFunctions *gl = context.functions();
  gl->initializeOpenGLFunctions();
  const auto *renderer = gl->glGetString(GL_RENDERER);
  result.detail = renderer ? QString::fromLatin1(reinterpret_cast<const char *>(renderer)) : QStringLiteral("Unknown renderer");
  const QString name = result.detail.toLower();
  if (name.contains("llvmpipe") || name.contains("softpipe") || name.contains("swrast") || name.contains("swiftshader") || name.contains("software")) {
    result.error = QStringLiteral("GPU benchmark unavailable: software renderer (%1).").arg(result.detail);
    context.doneCurrent();
    return result;
  }
  GLuint texture = 0, framebuffer = 0;
  gl->glGenTextures(1, &texture);
  gl->glBindTexture(GL_TEXTURE_2D, texture);
  gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  gl->glGenFramebuffers(1, &framebuffer);
  gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    result.error = QStringLiteral("GPU benchmark unavailable: cannot create the 512 × 512 render target.");
  } else {
    QElapsedTimer clock;
    clock.start();
    quint64 frames = 0;
    while (!cancel.load() && clock.elapsed() < std::clamp(durationMs, 10, 10000)) {
      gl->glClearColor((frames % 251) / 250.0f, 0.25f, 0.5f, 1.0f);
      gl->glClear(GL_COLOR_BUFFER_BIT);
      gl->glFinish();
      ++frames;
      if (gl->glGetError() != GL_NO_ERROR) {
        result.error = QStringLiteral("OpenGL reported an error during the GPU benchmark.");
        break;
      }
    }
    result.seconds = clock.nsecsElapsed() / 1e9;
    result.score = result.seconds > 0 ? frames / result.seconds : 0;
    result.checksum = frames;
    result.cancelled = cancel.load();
  }
  gl->glDeleteFramebuffers(1, &framebuffer);
  gl->glDeleteTextures(1, &texture);
  context.doneCurrent();
  return result;
}

namespace {

// The same xorshift chain as the single-thread test, so one core of the
// all-core score is comparable with it.
quint64 Spin(quint64 &state, int batches) {
  for (int b = 0; b < batches; ++b)
    for (int i = 0; i < 4096; ++i) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
    }
  return quint64(batches) * 4096;
}

int OnlineCpus() { return qMax(1, int(std::thread::hardware_concurrency())); }

Result RunThreads(Kind kind, int threads, int durationMs, const std::atomic<bool> &cancel,
                  std::atomic<quint64> *progress) {
  Result result;
  result.kind = kind;
  if (cancel.load()) {
    result.cancelled = true;
    return result;
  }
  threads = threads > 0 ? threads : OnlineCpus();
  std::vector<quint64> work(size_t(threads), 0), states(size_t(threads), 0);
  std::atomic<bool> stop{false};
  QElapsedTimer clock;
  clock.start();
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t)
    pool.emplace_back([&, t] {
      quint64 state = 0x123456789abcdefULL ^ quint64(t + 1) * 0x9e3779b97f4a7c15ULL, done = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const quint64 n = Spin(state, 16);
        done += n;
        if (progress) progress->fetch_add(n, std::memory_order_relaxed);
      }
      work[size_t(t)] = done;
      states[size_t(t)] = state;
    });
  while (!cancel.load() && clock.elapsed() < durationMs) QThread::msleep(std::clamp<qint64>(durationMs - clock.elapsed(), 1, 20));
  stop = true;
  for (auto &th : pool) th.join();
  result.seconds = clock.nsecsElapsed() / 1e9;
  quint64 total = 0;
  for (int t = 0; t < threads; ++t) {
    total += work[size_t(t)];
    result.checksum ^= states[size_t(t)];
  }
  result.score = result.seconds > 0 ? total / result.seconds / 1e6 : 0;
  result.cancelled = cancel.load() && clock.elapsed() < durationMs;
  result.detail = QStringLiteral("%1 threads").arg(threads);
  return result;
}

}  // namespace

Result RunCpuAll(int durationMs, const std::atomic<bool> &cancel, int threads) {
  return RunThreads(Kind::CpuAll, threads, std::clamp(durationMs, 10, 10000), cancel, nullptr);
}

Result RunSustained(int durationMs, const std::atomic<bool> &cancel, std::atomic<quint64> *progress) {
  return RunThreads(Kind::Sustained, 0, std::clamp(durationMs, 10, 300000), cancel, progress);
}

Result RunDiskDirect(const QString &directory, int durationMs, const std::atomic<bool> &cancel) {
  Result result;
  result.kind = Kind::DiskDirect;
  if (cancel.load()) {
    result.cancelled = true;
    return result;
  }
  durationMs = std::clamp(durationMs, 10, 10000);
  if (directory.isEmpty() || !QDir(directory).exists()) {
    result.error = QStringLiteral("Folder does not exist: ") + directory;
    return result;
  }
  // QTemporaryFile picks a private name and removes the file afterwards; the
  // measuring happens through a second descriptor opened with O_DIRECT.
  QTemporaryFile file(QDir(directory).filePath(".regent-direct-XXXXXX"));
  if (!file.open()) {
    result.error = QStringLiteral("Cannot create a temporary benchmark file in this folder: ") + file.errorString();
    return result;
  }
  // Switch the descriptor QTemporaryFile already holds to direct I/O: reopening
  // by name would follow whatever another user may have swapped in meanwhile.
  const int fd = ::dup(file.handle());
  if (fd >= 0 && ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_DIRECT) != 0) {
    const int err = errno;
    ::close(fd);
    errno = err;
    result.error = errno == EINVAL ? QStringLiteral("This filesystem does not support direct I/O (tmpfs?): choose a folder on the drive.")
                                   : QStringLiteral("Cannot switch the benchmark file to direct I/O: ") + QString::fromLocal8Bit(std::strerror(errno));
    return result;
  }
  if (fd < 0) {
    result.error = errno == EINVAL ? QStringLiteral("This filesystem does not support direct I/O (tmpfs?): choose a folder on the drive.")
                                   : QStringLiteral("Cannot open the benchmark file for direct I/O: ") + QString::fromLocal8Bit(std::strerror(errno));
    return result;
  }
  constexpr qint64 cap = 256 * 1024 * 1024, chunk = 4 * 1024 * 1024;
  void *raw = std::aligned_alloc(4096, chunk);
  if (!raw) {
    ::close(fd);
    result.error = QStringLiteral("Not enough memory to allocate benchmark buffers.");
    return result;
  }
  auto *block = static_cast<unsigned char *>(raw);
  quint64 state = 0x123456789abcdefULL;  // incompressible, so compressing filesystems still write it all
  for (qint64 i = 0; i < chunk; i += 8) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    std::memcpy(block + i, &state, 8);
  }
  QElapsedTimer clock;
  clock.start();
  qint64 written = 0;
  while (written < cap && !cancel.load() && clock.elapsed() < durationMs / 2) {
    const ssize_t n = ::pwrite(fd, block, chunk, written);
    if (n != chunk) {
      result.error = QStringLiteral("Direct write failed: ") + QString::fromLocal8Bit(std::strerror(n < 0 ? errno : EIO));
      break;
    }
    written += n;
  }
  // The drive must have the data before the clock stops.
  if (result.error.isEmpty() && ::fdatasync(fd) != 0) result.error = QStringLiteral("fdatasync failed");
  result.seconds = clock.nsecsElapsed() / 1e9;
  result.score = result.seconds > 0 ? written / result.seconds / (1024 * 1024) : 0;
  qint64 read = 0;
  if (result.error.isEmpty() && !cancel.load()) {
    QElapsedTimer readClock;
    readClock.start();
    while (read < written && !cancel.load()) {
      const ssize_t n = ::pread(fd, block, chunk, read);
      if (n <= 0) {
        result.error = QStringLiteral("Direct read failed: ") + QString::fromLocal8Bit(std::strerror(n < 0 ? errno : EIO));
        break;
      }
      result.checksum += block[0];
      read += n;
    }
    result.secondarySeconds = readClock.nsecsElapsed() / 1e9;
    result.secondaryScore = result.secondarySeconds > 0 ? read / result.secondarySeconds / (1024 * 1024) : 0;
  }
  ::close(fd);
  std::free(raw);
  result.cancelled = cancel.load() && !(result.error.isEmpty() && written > 0 && read >= written);
  result.detail = QStringLiteral("Direct I/O, page cache bypassed; %1 MiB written and read; %2")
                      .arg(QString::number(written / (1024 * 1024)), directory);
  return result;
}

ThrottleVerdict Judge(const QVector<ThrottleSample> &samples) {
  ThrottleVerdict v;
  if (samples.size() < 6) {
    v.text = QStringLiteral("Too short to judge: run for at least 30 seconds.");
    return v;
  }
  // The first second includes ramp-up: start from the second one.
  double base = 0;
  int n = 0;
  for (const auto &s : samples)
    if (s.t > 1 && s.t <= 6) {
      base += s.rate;
      ++n;
    }
  base = n ? base / n : samples[1].rate;
  double tail = 0;
  const int from = int(samples.size()) * 2 / 3;
  for (int i = from; i < samples.size(); ++i) tail += samples[i].rate;
  tail /= qMax(1, int(samples.size()) - from);
  v.dropPct = base > 0 ? std::max(0.0, (1 - tail / base) * 100) : 0;
  for (int i = 0; i + 2 < samples.size(); ++i)
    if (samples[i].t > 1 && samples[i].rate < base * 0.9 && samples[i + 1].rate < base * 0.9 &&
        samples[i + 2].rate < base * 0.9) {
      v.dropAtSec = samples[i].t;
      break;
    }
  qint64 first = -1, last = -1;
  for (const auto &s : samples) {
    v.maxTempC = std::max(v.maxTempC, s.tempC);
    if (s.throttle >= 0) {
      if (first < 0) first = s.throttle;
      last = s.throttle;
    }
  }
  v.events = first >= 0 ? std::max<qint64>(0, last - first) : 0;
  v.throttles = v.dropPct >= 10 || (v.events > 0 && v.dropPct >= 5);
  const QString temp = v.maxTempC > 0 ? QStringLiteral(", peak %1 °C").arg(v.maxTempC, 0, 'f', 0) : QString();
  const QString events = v.events > 0 ? QStringLiteral(", %1 throttle events").arg(v.events) : QString();
  if (v.throttles)
    v.text = QStringLiteral("Throttles: %1% slower by the end%2%3%4.")
                 .arg(v.dropPct, 0, 'f', 0)
                 .arg(v.dropAtSec >= 0 ? QStringLiteral(", from %1 s").arg(v.dropAtSec, 0, 'f', 0) : QString(), temp, events);
  else
    v.text = QStringLiteral("Stable: within %1% over %2 s%3%4.")
                 .arg(v.dropPct, 0, 'f', 0)
                 .arg(samples.last().t, 0, 'f', 0)
                 .arg(temp, events);
  return v;
}

}  // namespace td::benchmarks
