#include "ui/Safe.h"
#include "ui/pages/Pages.h"

#include "core/Benchmarks.h"
#include "ui/BenchmarkDashboard.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QPainter>
#include <QPainterPath>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOffscreenSurface>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <atomic>
#include <algorithm>
#include <memory>
#include <tuple>

namespace td::ui {
namespace {

// The current power-profiles-daemon profile, or "" when it is not running.
QString PowerProfile() {
  for (auto [service, path, iface] : {std::tuple{"org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles",
                                                 "org.freedesktop.UPower.PowerProfiles"},
                                      std::tuple{"net.hadess.PowerProfiles", "/net/hadess/PowerProfiles", "net.hadess.PowerProfiles"}}) {
    QDBusMessage m = QDBusMessage::createMethodCall(service, path, "org.freedesktop.DBus.Properties", "Get");
    m << QString(iface) << QString("ActiveProfile");
    const QDBusMessage r = QDBusConnection::systemBus().call(m, QDBus::Block, 500);
    if (r.type() == QDBusMessage::ReplyMessage) return r.arguments().value(0).value<QDBusVariant>().variant().toString();
  }
  return {};
}

// What the machine was doing while a test ran: the same score means little
// without the profile, the power source, the heat and the background load.
struct Conditions {
  QString profile;
  bool onAc = true;
  int batteryPct = -1;
  double backgroundCpu = -1;  // whole-machine CPU just before the start
  double tempStart = -1, tempMax = -1;
  double mhzSum = 0;
  int mhzSamples = 0;
  qint64 throttleStart = -1, throttleEnd = -1;

  void Begin(const Snapshot *s) {
    *this = {};
    profile = PowerProfile();
    if (!s) return;
    onAc = s->power.onAc;
    batteryPct = s->power.hasBattery ? s->power.batteryPct : -1;
    backgroundCpu = s->cpuUtil;
    tempStart = tempMax = s->cpuTemp;
    throttleStart = throttleEnd = s->throttleEvents;
  }
  void Track(const Snapshot &s) {
    tempMax = std::max(tempMax, s.cpuTemp);
    if (s.cpuMhz > 0) {
      mhzSum += s.cpuMhz;
      ++mhzSamples;
    }
    throttleEnd = s.throttleEvents;
  }
  QString Text() const {
    QStringList parts;
    if (!profile.isEmpty()) parts << profile;
    parts << (onAc ? QObject::tr("AC") : QObject::tr("battery %1%").arg(batteryPct));
    if (tempStart > 0) parts << QObject::tr("%1→%2 °C").arg(tempStart, 0, 'f', 0).arg(tempMax, 0, 'f', 0);
    if (mhzSamples) parts << QObject::tr("%1 GHz avg").arg(mhzSum / mhzSamples / 1000, 0, 'f', 2);
    if (backgroundCpu >= 0) parts << QObject::tr("background %1%").arg(backgroundCpu, 0, 'f', 0);
    if (throttleStart >= 0) {
      const qint64 n = throttleEnd - throttleStart;
      parts << (n > 0 ? QObject::tr("throttled ×%1").arg(n) : QObject::tr("no throttling"));
    }
    return parts.join(" · ");
  }
};

// The sustained run, second by second: throughput against its start, the clock and the heat.
class ThrottleChart : public QWidget {
public:
  explicit ThrottleChart(QWidget *parent = nullptr) : QWidget(parent) {
    setObjectName("ThrottleChart");
    setMinimumHeight(190);
    setAccessibleName(tr("Sustained CPU test: throughput, clock and temperature over time"));
  }
  void SetSamples(const QVector<benchmarks::ThrottleSample> &samples, int durationSec, const QString &verdict) {
    samples_ = samples;
    durationSec_ = qMax(1, durationSec);
    verdict_ = verdict;
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.fillRect(rect(), theme::Base());
    p.setFont(theme::Mono(11, true));
    p.setPen(theme::Accent());
    p.drawText(12, 20, tr("SUSTAINED LOAD · all cores"));
    p.setFont(theme::Mono(10));
    int x = 12;
    for (auto [label, color] : {std::pair{tr("throughput % of start"), theme::Cpu()}, std::pair{tr("clock"), theme::Clock()},
                                std::pair{tr("°C"), theme::Temp()}}) {
      p.fillRect(x, 31, 10, 3, color);
      p.setPen(theme::Dim());
      p.drawText(x + 14, 37, label);
      x += 18 + QFontMetrics(p.font()).horizontalAdvance(label) + 12;
    }
    const QRectF plot(44, 48, qMax(10, width() - 58), qMax(10, height() - 92));
    for (int pct : {0, 50, 100}) {
      const double y = plot.bottom() - plot.height() * pct / 110;
      p.setPen(theme::Border());
      p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
      p.setPen(theme::Dim());
      p.drawText(QRectF(0, y - 8, 38, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(pct) + '%');
    }
    if (!samples_.isEmpty()) {
      double base = 0, maxMhz = 0;
      int n = 0;
      for (const auto &s : samples_) {
        if (s.t > 1 && s.t <= 6) { base += s.rate; ++n; }
        maxMhz = std::max(maxMhz, s.mhz);
      }
      base = n ? base / n : samples_.first().rate;
      auto line = [&](auto value, QColor color) {
        QPainterPath path;
        bool started = false;
        for (const auto &s : samples_) {
          const double v = value(s);
          if (v < 0) continue;
          const QPointF pt(plot.left() + plot.width() * std::min(1.0, s.t / durationSec_),
                           plot.bottom() - plot.height() * std::clamp(v, 0.0, 110.0) / 110);
          started ? path.lineTo(pt) : path.moveTo(pt);
          started = true;
        }
        p.setPen(QPen(color, 2));
        p.drawPath(path);
      };
      p.setRenderHint(QPainter::Antialiasing);
      line([&](const auto &s) { return base > 0 ? 100 * s.rate / base : -1.0; }, theme::Cpu());
      line([&](const auto &s) { return maxMhz > 0 && s.mhz > 0 ? 100 * s.mhz / maxMhz : -1.0; }, theme::Clock());
      line([](const auto &s) { return s.tempC; }, theme::Temp());  // °C on the same 0…110 axis
      p.setRenderHint(QPainter::Antialiasing, false);
    }
    p.setPen(theme::Dim());
    p.drawText(QRectF(plot.left(), plot.bottom() + 4, plot.width(), 16), Qt::AlignRight,
               tr("%1 s").arg(durationSec_));
    p.setFont(theme::Mono(11, true));
    p.setPen(verdict_.startsWith("Throttles") ? theme::Bad() : verdict_.startsWith("Stable") ? theme::Good() : theme::Text());
    p.drawText(QRect(12, height() - 24, width() - 24, 20), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(p.font()).elidedText(verdict_, Qt::ElideRight, width() - 24));
  }

private:
  QVector<benchmarks::ThrottleSample> samples_;
  int durationSec_ = 60;
  QString verdict_;
};

class BenchmarksPage : public Page {
public:
  explicit BenchmarksPage(QWidget *parent) : Page(parent) {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0,0,0,0);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    scroll->setWidget(body);
    outer->addWidget(scroll);
    layout->setContentsMargins(8, 8, 8, 8);
    dashboard_ = new BenchmarkDashboard;
    layout->addWidget(dashboard_);
    auto *description = new QLabel(tr("CPU: one integer thread. Memory: copy between two 16 MiB buffers. GPU: offscreen OpenGL fills at 512 × 512. "
                                    "Disk: cached sequential I/O in a private temporary file, up to 32 MiB, removed afterwards. "
                                    "Internet: download only from the HTTP(S) URL you enter, up to 64 MiB. "
                                    "Tests stop at the selected duration. Scores depend on background load, caches and power settings."));
    description->setWordWrap(true);
    description->setToolTip(description->text());
    description->setText(tr("Choose a test to build measured scores and history. CPU uses one thread; disk results include the file cache."));
    layout->addWidget(description);
    auto *controls = new QHBoxLayout;
    cpu_ = new QPushButton(tr("CPU"));
    memory_ = new QPushButton(tr("Memory"));
    gpu_ = new QPushButton(tr("GPU"));
    disk_ = new QPushButton(tr("Test disk"));
    internet_ = new QPushButton(tr("Test Internet"));
    cpuAll_ = new QPushButton(tr("All cores"));
    cpuAll_->setToolTip(tr("The CPU test on every core at once; compared with one core it shows how well work scales"));
    sustained_ = new QPushButton(tr("Throttle test"));
    sustained_->setToolTip(tr("All cores under load for a while: does the CPU slow down as it heats?"));
    sustainedLength_ = new QComboBox;
    for (int sec : {30, 60, 120, 300}) sustainedLength_->addItem(tr("%1 s").arg(sec), sec);
    sustainedLength_->setCurrentIndex(1);
    sustainedLength_->setAccessibleName(tr("Throttle test length"));
    direct_ = new QCheckBox(tr("Bypass cache (direct I/O, 256 MiB)"));
    direct_->setToolTip(tr("Measures the drive itself instead of the page cache: slower, honest numbers. Writes up to 256 MiB."));
    stop_ = new QPushButton(tr("Stop"));
    stop_->setEnabled(false);
    duration_ = new QSpinBox;
    duration_->setRange(1, 10);
    duration_->setValue(3);
    duration_->setSuffix(tr(" s"));
    duration_->setAccessibleName(tr("Benchmark duration"));
    gpu_->setToolTip(tr("Offscreen OpenGL fills of 512 × 512 per second: compares this GPU with itself over time"));
    // Each row reads as a sentence: which tests, and how long they run.
    auto *quickLabel = new QLabel(tr("Quick tests,"));
    auto *eachLabel = new QLabel(tr("each:"));
    controls->addWidget(quickLabel);
    controls->addWidget(duration_);
    controls->addWidget(eachLabel);
    controls->addWidget(cpu_);
    controls->addWidget(cpuAll_);
    controls->addWidget(memory_);
    controls->addWidget(gpu_);
    controls->addStretch();
    layout->addLayout(controls);
    auto *throttleControls = new QHBoxLayout;
    throttleControls->addWidget(new QLabel(tr("Throttle test, all cores for")));
    throttleControls->addWidget(sustainedLength_);
    sustained_->setText(tr("Run throttle test"));
    throttleControls->addWidget(sustained_);
    throttleControls->addStretch();
    layout->addLayout(throttleControls);
    chart_ = new ThrottleChart;
    chart_->hide();  // appears with the first throttle test
    layout->addWidget(chart_);
    auto *diskControls = new QHBoxLayout;
    diskControls->addWidget(new QLabel(tr("Disk folder:")));
    // Not /tmp: on Arch/Omarchy it is tmpfs, i.e. RAM. The file is private and removed afterwards.
    directory_ = new QLineEdit(QDir::homePath());
    directory_->setAccessibleName(tr("Disk benchmark folder"));
    diskControls->addWidget(directory_);
    browse_ = new QPushButton(tr("Browse…"));
    diskControls->addWidget(browse_);
    diskControls->addWidget(disk_);
    layout->addLayout(diskControls);
    layout->addWidget(direct_);
    auto *networkControls = new QHBoxLayout;
    networkControls->addWidget(new QLabel(tr("Download URL:")));
    url_ = new QLineEdit;
    url_->setPlaceholderText(tr("Enter an http:// or https:// download URL"));
    url_->setAccessibleName(tr("Internet benchmark download URL"));
    networkControls->addWidget(url_);
    networkControls->addWidget(internet_);
    layout->addLayout(networkControls);
    auto *runControls = new QHBoxLayout;
    runControls->addWidget(stop_);
    export_ = new QPushButton(tr("Export CSV…"));
    export_->setEnabled(false);
    runControls->addStretch();
    runControls->addWidget(export_);
    layout->addLayout(runControls);
    status_ = new QLabel(tr("Ready"));
    status_->setTextFormat(Qt::PlainText);  // renderer names, folders and server errors
    status_->setWordWrap(true);  // renderer names and I/O errors are long; never widen the window
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(status_);
    results_ = new QTreeWidget;
    results_->setHeaderLabels({tr("Time"), tr("Test"), tr("Score"), tr("Unit"), tr("Seconds"), tr("Status"), tr("Conditions"),
                               tr("Checksum"), tr("Details")});
    results_->setAlternatingRowColors(true);
    results_->setRootIsDecorated(false);
    results_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(results_);
    connect(cpu_, &QPushButton::clicked, this, [this] { Start(benchmarks::Kind::Cpu); });
    connect(memory_, &QPushButton::clicked, this, [this] { Start(benchmarks::Kind::Memory); });
    connect(gpu_, &QPushButton::clicked, this, [this] { Start(benchmarks::Kind::Gpu); });
    connect(disk_, &QPushButton::clicked, this, [this] {
      Start(direct_->isChecked() ? benchmarks::Kind::DiskDirect : benchmarks::Kind::Disk);
    });
    connect(cpuAll_, &QPushButton::clicked, this, [this] { Start(benchmarks::Kind::CpuAll); });
    connect(sustained_, &QPushButton::clicked, this, [this] { Start(benchmarks::Kind::Sustained); });
    sampler_.setInterval(1000);
    connect(&sampler_, &QTimer::timeout, this, [this] { SampleSustained(); });
    connect(internet_, &QPushButton::clicked, this, [this] { StartNetwork(); });
    connect(browse_, &QPushButton::clicked, this, [this] {
      const QString path = QFileDialog::getExistingDirectory(this, tr("Disk benchmark folder"), directory_->text());
      if (!path.isEmpty()) directory_->setText(path);
    });
    connect(stop_, &QPushButton::clicked, this, [this] {
      cancel_ = true;
      stop_->setEnabled(false);
      status_->setText(tr("Stopping…"));
      if (reply_) reply_->abort();
    });
    connect(export_, &QPushButton::clicked, this, [this] { Export(); });
    connect(&poll_, &QTimer::timeout, this, [this] { FinishIfReady(); });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] { StopAndWait(); });
    poll_.setInterval(100);
    network_ = new QNetworkAccessManager(this);
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
      networkBounded_ = true;
      if (reply_) reply_->abort();
    });
  }

  ~BenchmarksPage() override { StopAndWait(); }
  QString Id() const override { return "benchmarks"; }
  QString Title() const override { return tr("Benchmarks"); }
  void Update(const SnapshotPtr &s) override {
    latest_ = s;
    if (s && (worker_ || reply_)) conditions_.Track(*s);
  }
  // While a test runs the conditions need every snapshot, on screen or not.
  bool WantsBackgroundUpdates() const override { return worker_ || reply_; }

private:
  QString TestName(benchmarks::Kind kind) const {
    switch (kind) {
      case benchmarks::Kind::Cpu: return tr("CPU integer (single thread)");
      case benchmarks::Kind::Memory: return tr("Memory copy");
      case benchmarks::Kind::Gpu: return tr("OpenGL clear (512 × 512)");
      case benchmarks::Kind::Disk: return tr("Disk cached write");
      case benchmarks::Kind::Internet: return tr("Internet download");
      case benchmarks::Kind::CpuAll: return tr("CPU integer (all cores)");
      case benchmarks::Kind::DiskDirect: return tr("Disk direct write");
      case benchmarks::Kind::Sustained: return tr("Throttle test (all cores)");
    }
    return {};
  }

  void SetBusy(bool busy) {
    for (auto *button : {cpu_, memory_, gpu_, disk_, internet_, browse_, cpuAll_, sustained_}) button->setEnabled(!busy);
    sustainedLength_->setEnabled(!busy);
    direct_->setEnabled(!busy);
    duration_->setEnabled(!busy);
    directory_->setEnabled(!busy);
    url_->setEnabled(!busy);
    stop_->setEnabled(busy);
  }

  void Start(benchmarks::Kind kind) {
    if (worker_ || reply_) return;
    cancel_ = false;
    const int durationMs = kind == benchmarks::Kind::Sustained ? sustainedLength_->currentData().toInt() * 1000
                                                               : duration_->value() * 1000;
    conditions_.Begin(latest_.get());
    SetBusy(true);
    dashboard_->Begin(kind, durationMs);
    QString note;
    if (conditions_.backgroundCpu >= 20)
      note = tr(" Background load is %1%: scores will be lower; close busy apps for a fair run.")
                 .arg(conditions_.backgroundCpu, 0, 'f', 0);
    status_->setText(tr("Running %1 test (%2 seconds)…").arg(TestName(kind)).arg(durationMs / 1000) + note);
    if (kind == benchmarks::Kind::Sustained) {
      progress_ = 0;
      lastProgress_ = 0;
      samples_.clear();
      lastSampleAt_ = 0;
      sustainedClock_.start();
      sustainedSeconds_ = durationMs / 1000;
      chart_->show();
      chart_->SetSamples({}, sustainedSeconds_, tr("Running…"));
      sampler_.start();
    }
    if (kind == benchmarks::Kind::Gpu) {
      surface_ = std::make_unique<QOffscreenSurface>();
      surface_->create();
    }
    const QString directory = directory_->text();
    worker_ = QThread::create([this, kind, durationMs, directory] {
      if (kind == benchmarks::Kind::Disk) result_ = benchmarks::RunDisk(directory, durationMs, cancel_);
      else if (kind == benchmarks::Kind::DiskDirect) result_ = benchmarks::RunDiskDirect(directory, durationMs, cancel_);
      else if (kind == benchmarks::Kind::CpuAll) result_ = benchmarks::RunCpuAll(durationMs, cancel_);
      else if (kind == benchmarks::Kind::Sustained) result_ = benchmarks::RunSustained(durationMs, cancel_, &progress_);
      else if (kind == benchmarks::Kind::Gpu) result_ = benchmarks::RunGpu(surface_.get(), durationMs, cancel_);
      else result_ = benchmarks::Run(kind, durationMs, cancel_);
    });
    worker_->start();
    poll_.start();
  }

  void StopAndWait() {
    poll_.stop();
    deadline_.stop();
    cancel_ = true;
    if (reply_) {
      reply_->disconnect(this);
      reply_->abort();
      delete reply_;
      reply_ = nullptr;
    }
    if (worker_) {
      worker_->wait();
      delete worker_;
      worker_ = nullptr;
    }
    surface_.reset();
  }

  void FinishIfReady() {
    if (!worker_ || worker_->isRunning()) return;
    // Joining synchronizes access to the worker's result.
    worker_->wait();
    delete worker_;
    worker_ = nullptr;
    surface_.reset();
    poll_.stop();
    if (result_.kind == benchmarks::Kind::Sustained) {
      sampler_.stop();
      SampleSustained();  // the last partial second
      const auto verdict = benchmarks::Judge(samples_);
      result_.detail = verdict.text + ' ' + result_.detail;
      chart_->SetSamples(samples_, sustainedSeconds_, result_.cancelled ? tr("Cancelled") : verdict.text);
    }
    SetBusy(false);
    RecordResult();
  }

  void SampleSustained() {
    const double t = sustainedClock_.nsecsElapsed() / 1e9;
    const double dt = t - lastSampleAt_;
    if (dt < 0.2 && !samples_.isEmpty()) return;
    const quint64 now = progress_.load();
    benchmarks::ThrottleSample s;
    s.t = t;
    s.rate = dt > 0 ? (now - lastProgress_) / dt / 1e6 : 0;
    if (latest_) {
      s.mhz = latest_->cpuMhz > 0 ? latest_->cpuMhz : -1;
      s.tempC = latest_->cpuTemp;
      s.throttle = latest_->throttleEvents;
    }
    lastProgress_ = now;
    lastSampleAt_ = t;
    samples_ << s;
    chart_->SetSamples(samples_, sustainedSeconds_, tr("Running… %1 s").arg(t, 0, 'f', 0));
  }

  void RecordResult() {
    dashboard_->Complete(result_);
    const QString status = !result_.error.isEmpty() ? result_.error : result_.cancelled ? tr("Cancelled") : tr("Completed");
    status_->setText(status);
    emit Status(status);
    using K = benchmarks::Kind;
    const K k = result_.kind;
    const QString conditions = conditions_.Text();
    new QTreeWidgetItem(results_, {
        QDateTime::currentDateTime().toString(Qt::ISODate),
        TestName(k),
        result_.cancelled || !result_.error.isEmpty() ? QStringLiteral("—") : QString::number(result_.score, 'f', 3),
        k == K::Cpu || k == K::CpuAll || k == K::Sustained ? QStringLiteral("M iterations/s")
          : k == K::Memory ? QStringLiteral("GiB/s")
          : k == K::Gpu ? QStringLiteral("fills/s")
          : k == K::Internet ? QStringLiteral("Mbit/s") : QStringLiteral("MiB/s"),
        QString::number(result_.seconds, 'f', 3), status, conditions, QString::number(result_.checksum, 16), result_.detail});
    if ((k == K::Disk || k == K::DiskDirect) && result_.secondaryScore >= 0) {
      new QTreeWidgetItem(results_, {QDateTime::currentDateTime().toString(Qt::ISODate),
          k == K::Disk ? tr("Disk cached read") : tr("Disk direct read"),
          result_.cancelled || !result_.error.isEmpty() ? QStringLiteral("—") : QString::number(result_.secondaryScore, 'f', 3),
          QStringLiteral("MiB/s"), QString::number(result_.secondarySeconds, 'f', 3), status, conditions,
          QString::number(result_.checksum, 16), result_.detail});
    }
    export_->setEnabled(true);
  }

  void StartNetwork() {
    if (worker_ || reply_) return;
    const QUrl url(url_->text().trimmed());
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "http" && url.scheme() != "https")) {
      status_->setText(tr("Enter a valid HTTP(S) download URL first."));
      return;
    }
    cancel_ = false;
    networkBounded_ = false;
    downloaded_ = 0;
    SetBusy(true);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setTransferTimeout(duration_->value() * 1000);
    networkClock_.start();
    conditions_.Begin(latest_.get());
    dashboard_->Begin(benchmarks::Kind::Internet, duration_->value() * 1000);
    reply_ = network_->get(request);
    reply_->setReadBufferSize(256 * 1024);
    status_->setText(tr("Downloading; maximum 64 MiB or %1 seconds…").arg(duration_->value()));
    connect(reply_, &QIODevice::readyRead, this, [this] { ConsumeNetwork(true); });
    connect(reply_, &QNetworkReply::finished, this, [this] { FinishNetwork(); });
    deadline_.start(duration_->value() * 1000);
  }

  void ConsumeNetwork(bool abortAtLimit) {
    constexpr qint64 cap = 64 * 1024 * 1024;
    while (reply_ && reply_->bytesAvailable() > 0 && downloaded_ < cap) {
      const QByteArray chunk = reply_->read(std::min<qint64>(64 * 1024, cap - downloaded_));
      if (chunk.isEmpty()) break;
      downloaded_ += chunk.size();
    }
    if (downloaded_ >= cap && abortAtLimit && reply_) {
      networkBounded_ = true;
      reply_->abort();
    }
  }

  void FinishNetwork() {
    deadline_.stop();
    ConsumeNetwork(false);
    result_ = {};
    result_.kind = benchmarks::Kind::Internet;
    result_.seconds = networkClock_.nsecsElapsed() / 1e9;
    result_.score = result_.seconds > 0 ? downloaded_ * 8 / result_.seconds / 1e6 : 0;
    result_.cancelled = cancel_.load();
    // Without the query: signed download links carry tokens that would end up in the CSV.
    result_.detail = tr("%1 bytes; %2").arg(QString::number(downloaded_),
                                            reply_->url().toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment));
    const int code = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!result_.cancelled) {
      if (code >= 300 && code < 400)
        result_.error = tr("Redirect received; enter the final download URL explicitly.");
      else if (reply_->error() != QNetworkReply::NoError && !(networkBounded_ && reply_->error() == QNetworkReply::OperationCanceledError))
        result_.error = reply_->errorString();
      else if (downloaded_ == 0)
        result_.error = tr("No download payload received before the time limit.");
    }
    reply_->deleteLater();
    reply_ = nullptr;
    SetBusy(false);
    RecordResult();
  }

  void Export() {
    const QString path = QFileDialog::getSaveFileName(this, tr("Export benchmark results"), "benchmarks.csv", tr("CSV files (*.csv)"));
    if (path.isEmpty()) return;
    QByteArray csv("Time,Test,Score,Unit,Seconds,Status,Conditions,Checksum,Details\r\n");
    for (int row = 0; row < results_->topLevelItemCount(); ++row) {
      QStringList fields;
      for (int column = 0; column < results_->columnCount(); ++column) {
        QString value = results_->topLevelItem(row)->text(column);
        // A cell a spreadsheet would run as a formula (a folder or URL can start with "="): keep it text.
        if (!value.isEmpty() && QStringLiteral("=+-@\t\r").contains(value[0])) value.prepend('\'');
        value.replace('"', "\"\"");
        fields << '"' + value + '"';
      }
      csv += fields.join(',').toUtf8() + "\r\n";
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(csv) != csv.size() || !file.commit())
      safe::warning(this, tr("Export failed"), file.errorString());
    else
      emit Status(tr("Saved benchmark results to %1").arg(path));
  }

  BenchmarkDashboard *dashboard_;
  QPushButton *cpu_, *memory_, *gpu_, *disk_, *internet_, *browse_, *stop_, *export_, *cpuAll_, *sustained_;
  QComboBox *sustainedLength_;
  QCheckBox *direct_;
  ThrottleChart *chart_;
  QTimer sampler_;
  std::atomic<quint64> progress_{0};
  quint64 lastProgress_ = 0;
  double lastSampleAt_ = 0;
  int sustainedSeconds_ = 60;
  QElapsedTimer sustainedClock_;
  QVector<benchmarks::ThrottleSample> samples_;
  SnapshotPtr latest_;
  Conditions conditions_;
  QLineEdit *directory_, *url_;
  QSpinBox *duration_;
  QLabel *status_;
  QTreeWidget *results_;
  QTimer poll_;
  QThread *worker_ = nullptr;
  std::atomic<bool> cancel_{false};
  benchmarks::Result result_;
  std::unique_ptr<QOffscreenSurface> surface_;
  QNetworkAccessManager *network_;
  QNetworkReply *reply_ = nullptr;
  QTimer deadline_;
  QElapsedTimer networkClock_;
  qint64 downloaded_ = 0;
  bool networkBounded_ = false;
};

}  // namespace

Page *CreateBenchmarksPage(QWidget *parent) { return new BenchmarksPage(parent); }

}  // namespace td::ui
