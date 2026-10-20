#include "ui/Safe.h"
#include "ui/pages/Pages.h"
#include "ui/pages/FlightRecorderControls.h"
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDir>
#include "core/FlightRecorder.h"
#include "core/FlightReport.h"
#include "core/Settings.h"
#include "ui/Roles.h"
#include "ui/Theme.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QSaveFile>
#include <QSysInfo>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

namespace td::ui {
namespace {

class RecordingGraph : public QWidget {
public:
  RecordingGraph(const FlightRecorder *recorder, QWidget *parent)
      : QWidget(parent), recorder_(recorder) {
    setMinimumHeight(170);
    setMaximumHeight(270);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setAccessibleName(tr("Recorded CPU and memory usage history"));
    setToolTip(tr("CPU and memory usage from 0 to 100%. Click a sample to inspect it."));
  }
  void SetSelected(int index) { selected_ = index; update(); }
  void SetCaption(const QString &caption) { caption_ = caption; update(); }
  void SetCompare(const FlightRecorder *other) { compare_ = other; update(); }
  void SetAnomalies(QVector<FlightRecorder::Anomaly> a) { anomalies_ = std::move(a); update(); }
  std::function<void(int)> select;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    // The same panel as the card lists: dotted ground, an accent title, a dim caption.
    painter.fillRect(rect(), theme::Base());
    const QColor dot = theme::Mix(theme::Base(), theme::Border(), 0.35);
    for (int gx = 6; gx < width(); gx += 9)
      for (int gy = 6; gy < height(); gy += 9) painter.fillRect(gx, gy, 1, 1, dot);
    painter.setFont(theme::Mono(11, true));
    painter.setPen(theme::Accent());
    painter.drawText(QRect(12, 8, width() - 24, 18), Qt::AlignLeft | Qt::AlignVCenter, tr("FLIGHT / TIMELINE"));
    painter.setFont(theme::Mono(10));
    const int legend = width() - 12;
    painter.setPen(theme::Memory());
    painter.drawText(QRect(legend - 80, 8, 80, 18), Qt::AlignRight | Qt::AlignVCenter, tr("■ MEMORY"));
    painter.setPen(theme::Cpu());
    painter.drawText(QRect(legend - 140, 8, 54, 18), Qt::AlignRight | Qt::AlignVCenter, tr("■ CPU"));
    painter.setPen(theme::Dim());
    painter.drawText(QRect(12, 28, width() - 24, 18), Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(painter.font()).elidedText(caption_, Qt::ElideRight, width() - 24));
    painter.setPen(theme::Border());
    painter.drawLine(12, 50, width() - 12, 50);
    const QRectF plot(42, 64, qMax(1, width() - 56), qMax(1, height() - 90));
    for (int percent : {0, 50, 100}) {
      if (recorder_->Frames().empty()) break;  // no axes under the "how to start" text
      const double y = plot.bottom() - plot.height() * percent / 100;
      painter.setPen(theme::Border());
      painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
      painter.setPen(theme::Dim());
      painter.drawText(QRectF(0, y - 8, 36, 16), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(percent) + '%');
    }
    const auto &frames = recorder_->Frames();
    if (frames.empty()) {
      painter.setPen(theme::Dim());
      painter.drawText(rect().adjusted(12, 56, -12, -12), Qt::AlignCenter,
                       tr("Nothing recorded yet\n\n● in the bottom bar or Ctrl+Shift+F records.\n"
                          "Auto-record (⋯ menu) starts by itself when the system struggles."));
      return;
    }
    const auto x = [&plot, &frames](int index) {
      return frames.size() == 1 ? plot.center().x() : plot.left() + plot.width() * index / (frames.size() - 1);
    };
    painter.setRenderHint(QPainter::Antialiasing);
    for (int series = 0; series < 2; ++series) {
      QPainterPath line;
      for (int i = 0; i < int(frames.size()); ++i) {
        const auto &f = frames.at(i);
        const double percent = series == 0 ? f.cpu : (f.memoryTotal ? 100.0 * f.memoryUsed / f.memoryTotal : 0);
        const QPointF point(x(i), plot.bottom() - plot.height() * std::clamp(percent, 0.0, 100.0) / 100);
        if (i == 0) line.moveTo(point);
        else line.lineTo(point);
        if (frames.size() == 1) {
          painter.setPen(QPen(series == 0 ? theme::Cpu() : theme::Memory(), 4));
          painter.drawPoint(point);
        }
      }
      painter.setPen(QPen(series == 0 ? theme::Cpu() : theme::Memory(), 2));
      painter.drawPath(line);
    }
    // A second session, from its own start, dashed under the same axes.
    if (compare_ && !compare_->Frames().empty()) {
      const auto &other = compare_->Frames();
      const int span = int(qMax(frames.size(), other.size())) - 1;
      for (int series = 0; series < 2; ++series) {
        QPainterPath line;
        for (int i = 0; i < int(other.size()); ++i) {
          const auto &f = other.at(i);
          const double percent = series == 0 ? f.cpu : (f.memoryTotal ? 100.0 * f.memoryUsed / f.memoryTotal : 0);
          const QPointF point(span <= 0 ? plot.center().x() : plot.left() + plot.width() * i / span,
                              plot.bottom() - plot.height() * std::clamp(percent, 0.0, 100.0) / 100);
          if (i == 0) line.moveTo(point);
          else line.lineTo(point);
        }
        QColor c = series == 0 ? theme::Cpu() : theme::Memory();
        c.setAlpha(150);
        painter.setPen(QPen(c, 1.5, Qt::DashLine));
        painter.drawPath(line);
      }
    }
    // Flagged moments: a red notch on top of the plot.
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (const auto &a : anomalies_) {
      if (a.index >= int(frames.size())) continue;
      const int ax = int(x(a.index));
      painter.fillRect(ax - 3, int(plot.top()) - 8, 7, 5, theme::Bad());
      painter.fillRect(ax - 1, int(plot.top()) - 3, 3, 3, theme::Bad());
    }
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(theme::Text(), 1, Qt::DashLine));
    const double cursor = x(std::clamp(selected_, 0, int(frames.size()) - 1));
    painter.drawLine(QPointF(cursor, plot.top()), QPointF(cursor, plot.bottom()));
    painter.setPen(theme::Dim());
    painter.drawText(QRectF(plot.left(), plot.bottom() + 5, plot.width() / 2, 18), Qt::AlignLeft,
                     frames.front().time.toLocalTime().toString("HH:mm:ss"));
    painter.drawText(QRectF(plot.center().x(), plot.bottom() + 5, plot.width() / 2, 18), Qt::AlignRight,
                     frames.back().time.toLocalTime().toString("HH:mm:ss"));
  }
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton || recorder_->Frames().empty() || !select) return;
    const double position = std::clamp((event->position().x() - 42) / qMax(1, width() - 56), 0.0, 1.0);
    select(int(std::lround(position * (recorder_->Frames().size() - 1))));
  }

private:
  const FlightRecorder *recorder_;
  const FlightRecorder *compare_ = nullptr;
  QVector<FlightRecorder::Anomaly> anomalies_;
  int selected_ = 0;
  QString caption_;
};

class RecordingItem : public QTreeWidgetItem {
public:
  using QTreeWidgetItem::QTreeWidgetItem;
  bool operator<(const QTreeWidgetItem &other) const override {
    const int column = treeWidget()->sortColumn();
    if (column >= 1 && column <= 4)
      return data(column, Qt::UserRole).toDouble() < other.data(column, Qt::UserRole).toDouble();
    return QTreeWidgetItem::operator<(other);
  }
};

class FlightRecorderPage : public Page, public FlightRecorderControls {
public:
  explicit FlightRecorderPage(QWidget *parent) : Page(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 8);
    auto *controls = new QHBoxLayout;
    capture_ = new QPushButton(tr("Start recording"));
    capture_->setCheckable(true);
    connect(capture_, &QPushButton::toggled, this, [this](bool enabled) {
      capture_->setText(enabled ? tr("Stop recording") : tr("Start recording"));
      if (enabled) {
        captureClock_.start();
        if (imported_) {
          recorder_.Clear();
          imported_ = false;
          elapsedMs_ = 0;
        }
        follow_ = true;
        if (latest_ && recorder_.Capture(*latest_)) dirty_ = true;
      }
      if (!enabled && captureClock_.isValid()) { elapsedMs_ += captureClock_.elapsed(); captureClock_.invalidate(); }
      if (!enabled) {  // stopped by hand or by calm: the next recording is the user's own
        auto_ = false;
        autoClock_.invalidate();
      }
      emit CaptureStateChanged(enabled);
      Refresh();
    });
    controls->addWidget(capture_);
    auto *live = new QPushButton(tr("Latest sample"));
    connect(live, &QPushButton::clicked, this, [this] { follow_ = true; Refresh(); });
    controls->addWidget(live);
    auto *clear = new QPushButton(tr("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] { ClearCapture(); });
    controls->addWidget(clear);
    auto *save = new QPushButton(tr("Export…"));
    connect(save, &QPushButton::clicked, this, [this] { SaveRecording(); });
    controls->addWidget(save);
    auto *load = new QPushButton(tr("Import…"));
    connect(load, &QPushButton::clicked, this, [this] { ImportCapture(); });
    controls->addWidget(load);
    controls->addStretch();
    auto *legacyControls = new QWidget(this);
    legacyControls->setLayout(controls);
    legacyControls->hide(); // Transport lives in the persistent bottom dock.

    graph_ = new RecordingGraph(&recorder_, this);
    graph_->select = [this](int index) { timeline_->setValue(index); };
    layout->addWidget(graph_);
    timeline_ = new QSlider(Qt::Horizontal);
    timeline_->setToolTip(tr("Select a recorded sample to inspect processes at that time"));
    connect(timeline_, &QSlider::valueChanged, this, [this] {
      follow_ = false;
      Show();
    });
    layout->addWidget(timeline_);
    anomalies_ = new QListWidget;
    anomalies_->setObjectName("FlightAnomalies");
    anomalies_->setMaximumHeight(92);
    anomalies_->setFont(theme::Mono(10));
    anomalies_->setAccessibleName(tr("Flagged moments"));
    anomalies_->setToolTip(tr("Moments with high CPU, memory, pressure or temperature, and the likely cause. Enter jumps there."));
    connect(anomalies_, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) {
      timeline_->setValue(it->data(Qt::UserRole).toInt());
    });
    connect(anomalies_, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
      timeline_->setValue(it->data(Qt::UserRole).toInt());
    });
    layout->addWidget(anomalies_);
    tree_ = new QTreeWidget;
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setHeaderLabels({tr("Process"), tr("PID"), tr("CPU %"), tr("Memory MiB"), tr("Threads"), tr("User"), tr("State"), tr("Command line")});
    tree_->header()->resizeSection(0, 180);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(2, Qt::DescendingOrder);
    layout->addWidget(tree_, 1);
    note_ = new QLabel(tr("Keeps the last 600 samples (64 MiB at most). Exports include process names, users and command lines."));
    auto *note = note_;
    note->setObjectName("Dim");
    note->setWordWrap(true);
    layout->addWidget(note);
    preroll_.SetMaxFrames(kPrerollFrames);
    Refresh();
  }

  QMenu *PageMenu() override {
    if (menu_) return menu_;
    menu_ = new QMenu(tr("F&light Recorder"), this);
    menu_->addAction(tr("&Compare with a saved session…"), this, [this] { LoadCompare(); });
    menu_->addAction(tr("C&lear comparison"), this, [this] {
      compare_.Clear();
      hasCompare_ = false;
      graph_->SetCompare(nullptr);
      Refresh();
    });
    QAction *report = menu_->addAction(tr("Export HTML &report…"), this, [this] { ExportReport(); });
    report->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    QAction *autoRec = menu_->addAction(tr("&Auto-record when something goes wrong"));
    autoRec->setCheckable(true);
    autoRec->setChecked(AutoEnabled());
    autoRec->setToolTip(tr("Keeps the last two minutes in memory; high CPU, heat or memory pressure starts a recording "
                           "with that history, saved once things calm down"));
    connect(autoRec, &QAction::toggled, this, [](bool on) { Settings::Get().set("flightAutoRecord", on); });
    return menu_;
  }

  void StartCapture() override { capture_->setChecked(true); }
  void StopCapture() override { capture_->setChecked(false); }
  void FollowLatest() override { follow_ = true; Refresh(); }
  bool IsCapturing() const override { return capture_->isChecked(); }
  qint64 CaptureElapsedMs() const override { return elapsedMs_ + (captureClock_.isValid() ? captureClock_.elapsed() : 0); }
  int SampleCount() const override { return int(recorder_.Frames().size()); }
  bool SaveCapture(const QString &path = {}) override { return SaveRecording(path); }
  void ClearCapture() override {
    if (!ConfirmUnsaved()) return;
    StopCapture(); recorder_.Clear(); imported_ = false; follow_ = true; dirty_ = false; elapsedMs_ = 0;
    capture_->setText(tr("Start recording")); Refresh(); emit CaptureStateChanged(false);
  }
  void ImportCapture() override {
      const QString path = QFileDialog::getOpenFileName(this, tr("Import recording"), {}, tr("Regent recordings (*.json)"));
      if (path.isEmpty()) return;
      if (!ConfirmUnsaved()) return;
      QString error;
      if (!recorder_.Load(path, &error)) {
        safe::warning(this, tr("Import recording"), error);
        return;
      }
      capture_->setChecked(false);
      imported_ = true;
      dirty_ = false;
      capture_->setText(tr("Start new recording"));
      follow_ = true;
      Refresh();
      emit Status(tr("Recording imported from %1").arg(path));
      elapsedMs_ = 0;
      if (recorder_.Frames().size() > 1)
        elapsedMs_ = qMax<qint64>(0, recorder_.Frames().front().time.msecsTo(recorder_.Frames().back().time));
      emit CaptureStateChanged(false);
  }

  QString Id() const override { return "flight-recorder"; }
  QString Title() const override { return tr("Flight Recorder"); }
  bool WantsBackgroundUpdates() const override { return true; }
  bool CanClose() override { return ConfirmUnsaved(); }
  void FocusFilter() override { if (filter_) filter_->setFocus(); }
  void BuildHeader(QHBoxLayout *header) override {
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter recorded processes"));
    filter_->setClearButtonEnabled(true);
    connect(filter_, &QLineEdit::textChanged, this, [this] { Show(); });
    header->addWidget(filter_);
  }
  void Update(const SnapshotPtr &snapshot) override {
    latest_ = snapshot;
    if (snapshot && !capture_->isChecked() && AutoEnabled()) {
      preroll_.Capture(*snapshot);
      QString reason;
      if (Triggered(*snapshot, &reason)) StartAuto(reason);
    } else if (snapshot && capture_->isChecked() && auto_ && Calm(*snapshot)) {
      auto_ = false;
      autoEnded_.start();
      emit Status(tr("Things calmed down: saving the automatic Flight Recorder session"));
      emit AutoCaptureFinished();
      return;
    }
    if (!snapshot || !capture_->isChecked()) {
      if (isVisible()) Refresh();
      return;
    }
    // Keep the same timestamp selected as the bounded history rolls forward.
    const auto &frames = recorder_.Frames();
    QDateTime selected;
    if (!follow_ && !frames.empty()) selected = frames.at(timeline_->value()).time;
    if (!recorder_.Capture(*snapshot)) {
      if (isVisible()) Refresh();
      return;
    }
    dirty_ = true;
    if (selected.isValid()) {
      int index = 0;
      for (const auto &frame : recorder_.Frames()) {
        if (frame.time == selected) break;
        ++index;
      }
      if (index == int(recorder_.Frames().size())) index = 0;
      timeline_->blockSignals(true);
      timeline_->setValue(index);
      timeline_->blockSignals(false);
    }
    Refresh();
  }

private:
  static constexpr int kPrerollFrames = 120;

  static bool AutoEnabled() { return Settings::Get().value("flightAutoRecord", false).toBool(); }

  // Sustained trouble only: a compile that pegs the CPU for a second is not news.
  bool Triggered(const Snapshot &s, QString *reason) {
    const double dt = qBound(0.0, s.intervalSec, 30.0);
    const double tempLimit = Settings::Get().value("alertCpuTempC", 90).toDouble();
    hotCpu_ = s.cpuUtil >= 90 ? hotCpu_ + dt : 0;
    hotTemp_ = s.cpuTemp >= tempLimit ? hotTemp_ + dt : 0;
    hotPressure_ = s.memPressure >= 20 ? hotPressure_ + dt : 0;
    const bool lowMemory = s.memTotal && s.memAvailable * 20 < s.memTotal;
    if (autoEnded_.isValid() && autoEnded_.elapsed() < 10 * 60 * 1000) return false;  // one per ten minutes
    if (dirty_ && !recorder_.Frames().empty()) return false;  // never clobber an unsaved manual recording
    if (hotCpu_ >= 10) *reason = tr("CPU above 90% for 10 s");
    else if (hotTemp_ >= 5) *reason = tr("CPU at %1").arg(Settings::Get().FormatTemp(s.cpuTemp));
    else if (hotPressure_ >= 5) *reason = tr("memory pressure %1%").arg(s.memPressure, 0, 'f', 0);
    else if (lowMemory) *reason = tr("less than 5% memory available");
    return !reason->isEmpty();
  }

  bool Calm(const Snapshot &s) {
    const double dt = qBound(0.0, s.intervalSec, 30.0);
    const double tempLimit = Settings::Get().value("alertCpuTempC", 90).toDouble();
    const bool calm = s.cpuUtil < 60 && (s.cpuTemp < 0 || s.cpuTemp < tempLimit - 5) && s.memPressure < 5 &&
                      (!s.memTotal || s.memAvailable * 10 > s.memTotal);
    calm_ = calm ? calm_ + dt : 0;
    return calm_ >= 60 || (autoClock_.isValid() && autoClock_.elapsed() > 10 * 60 * 1000);
  }

  void StartAuto(const QString &reason) {
    // The pre-roll becomes the recording, so it starts two minutes before the trouble.
    recorder_ = preroll_;
    recorder_.SetMaxFrames(FlightRecorder::MaxFrames);
    preroll_.Clear();
    imported_ = false;
    elapsedMs_ = recorder_.Frames().size() > 1
                     ? qMax<qint64>(0, recorder_.Frames().front().time.msecsTo(recorder_.Frames().back().time)) : 0;
    auto_ = true;
    calm_ = 0;
    hotCpu_ = hotTemp_ = hotPressure_ = 0;
    autoClock_.start();
    capture_->setChecked(true);
    dirty_ = true;
    emit Status(tr("Flight Recorder started by itself: %1").arg(reason));
  }

  void LoadCompare() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Compare with"), {}, tr("Regent recordings (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (!compare_.Load(path, &error)) {
      safe::warning(this, tr("Compare with"), error);
      return;
    }
    hasCompare_ = true;
    compareName_ = QFileInfo(path).fileName();
    graph_->SetCompare(&compare_);
    Refresh();
  }

  void ExportReport() {
    if (recorder_.Frames().empty()) {
      emit Status(tr("No captured samples to report yet."));
      return;
    }
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export HTML report"),
        dir + "/flight-report-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".html",
        tr("Web page (*.html)"));
    if (path.isEmpty()) return;
    QSaveFile f(path);
    const QByteArray html = report::Html(recorder_.Frames(), QSysInfo::machineHostName()).toUtf8();
    if (!f.open(QIODevice::WriteOnly) || f.write(html) != html.size() || !f.commit()) {
      safe::warning(this, tr("Export HTML report"), f.errorString());
      return;
    }
    emit Status(tr("Report saved to %1").arg(path));
  }

  bool SaveRecording(QString path = {}) {
    if (recorder_.Frames().empty()) { emit Status(tr("No captured samples to save yet.")); return false; }
    if (path.isEmpty()) path = QFileDialog::getSaveFileName(this, tr("Export recording"), "recording.regent.json", tr("Regent recordings (*.json)"));
    if (path.isEmpty()) return false;
    QString error;
    if (!recorder_.Save(path, &error)) {
      safe::warning(this, tr("Export recording"), error);
      return false;
    }
    dirty_ = false;
    emit CaptureSaved(QFileInfo(path).absoluteFilePath());
    emit Status(tr("Recording exported to %1").arg(path));
    return true;
  }

  bool ConfirmUnsaved() {
    if (!dirty_ || recorder_.Frames().empty()) return true;
    const auto answer = safe::warning(this, tr("Unsaved recording"),
      tr("Save the captured Flight Recorder history before continuing?"),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) return SaveRecording();
    return answer == QMessageBox::Discard;
  }

  void Refresh() {
    const int count = int(recorder_.Frames().size());
    if (isVisible()) RefreshAnomalies();
    timeline_->blockSignals(true);
    timeline_->setRange(0, qMax(0, count - 1));
    timeline_->setEnabled(count > 0);
    if (follow_) timeline_->setValue(qMax(0, count - 1));
    timeline_->blockSignals(false);
    if (isVisible()) Show();
  }

  void RefreshAnomalies() {
    const auto &frames = recorder_.Frames();
    const QString key = frames.empty() ? QString() : QString("%1|%2").arg(frames.front().time.toMSecsSinceEpoch()).arg(frames.size());
    if (key == anomaliesKey_) return;
    anomaliesKey_ = key;
    const auto list = FlightRecorder::Anomalies(frames);
    graph_->SetAnomalies(list);
    anomalies_->clear();
    for (const auto &a : list) {
      auto *it = new QListWidgetItem(QString("%1  %2%3").arg(frames.at(a.index).time.toLocalTime().toString("HH:mm:ss"), a.text,
                                                            a.culprit.isEmpty() ? QString() : "  ·  " + a.culprit));
      it->setData(Qt::UserRole, a.index);
      it->setForeground(a.kind == "temp" || a.kind == "memory" ? theme::Bad() : theme::Text());
      anomalies_->addItem(it);
    }
    if (list.isEmpty()) {
      auto *it = new QListWidgetItem(frames.empty() ? tr("Flagged moments appear here while recording")
                                                    : tr("Nothing unusual: no high CPU, memory, pressure or temperature"));
      it->setFlags(Qt::NoItemFlags);
      anomalies_->addItem(it);
    }
  }

  void Show() {
    graph_->SetSelected(timeline_->value());
    const auto &frames = recorder_.Frames();
    const QString filter = filter_ ? filter_->text().trimmed() : QString();
    // The table shows one frame: rebuild it only when that frame or the filter
    // changes, so a stopped recording (or a pinned sample) keeps its selection
    // and scroll position while new snapshots arrive.
    const QString key = frames.empty() ? QString()
                                       : QString("%1|%2|%3").arg(frames.front().time.toMSecsSinceEpoch())
                                             .arg(frames.at(timeline_->value()).time.toMSecsSinceEpoch()).arg(filter);
    const bool rebuild = key != shownKey_ || frames.empty();
    shownKey_ = key;
    QVariant currentPid, currentStart;
    if (QTreeWidgetItem *cur = tree_->currentItem()) {
      currentPid = cur->data(1, Qt::UserRole);
      currentStart = cur->data(0, Qt::UserRole);
    }
    const int scroll = tree_->verticalScrollBar()->value();
    if (rebuild) {
      tree_->setUpdatesEnabled(false);
      tree_->setSortingEnabled(false);
      tree_->clear();
    }
    // Until something is recorded the timeline says how to start; the empty
    // slider, moments list and process table would only take room.
    // The table is shown as a card panel that wraps it: hide the panel.
    QWidget *table = tree_;
    while (table != this && table->parentWidget() && table->objectName() != "ResponsiveTable") table = table->parentWidget();
    for (QWidget *w : {static_cast<QWidget *>(timeline_), static_cast<QWidget *>(anomalies_),
                       table == this ? static_cast<QWidget *>(tree_) : table, static_cast<QWidget *>(note_)})
      w->setVisible(!frames.empty());
    graph_->setMaximumHeight(frames.empty() ? QWIDGETSIZE_MAX : 270);
    if (frames.empty()) {
      graph_->SetCaption(capture_->isChecked() ? tr("Recording · waiting for the first sample") : tr("Stopped"));
    } else {
      const auto &frame = frames.at(timeline_->value());
      graph_->SetCaption(tr("%1 · sample %2 of %3 · CPU %4% · memory %5 / %6 MiB · %7 processes · %8")
        .arg(frame.time.toLocalTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
        .arg(timeline_->value() + 1).arg(frames.size()).arg(frame.cpu, 0, 'f', 1)
        .arg(frame.memoryUsed / 1048576.0, 0, 'f', 1).arg(frame.memoryTotal / 1048576.0, 0, 'f', 1)
        .arg(frame.processes.size()).arg(capture_->isChecked() ? tr("Recording") : tr("Stopped"))
        + (frame.omitted > 0 ? tr(" · %1 least busy processes not recorded").arg(frame.omitted) : QString())
        + CompareText());
      if (!rebuild) return;
      for (const auto &p : frame.processes) {
        if (!filter.isEmpty() && !p.name.contains(filter, Qt::CaseInsensitive) &&
            !p.cmdline.contains(filter, Qt::CaseInsensitive) && !p.user.contains(filter, Qt::CaseInsensitive) &&
            !QString::number(p.key.pid).contains(filter)) continue;
        auto *item = new RecordingItem(tree_, {p.name, QString::number(p.key.pid), QString::number(p.cpu, 'f', 1),
          QString::number(p.rss / 1048576.0, 'f', 1), QString::number(p.threads), p.user, QString(p.state), p.cmdline});
        item->setData(0, Qt::UserRole, QVariant::fromValue(p.key.start));
        item->setData(1, Qt::UserRole, p.key.pid);
        item->setData(2, Qt::UserRole, p.cpu);
        item->setData(2, kValueRole, p.cpu);
        item->setData(3, Qt::UserRole, QVariant::fromValue(p.rss));
        item->setData(3, kValueRole, double(p.rss));
        item->setData(4, Qt::UserRole, p.threads);
        item->setToolTip(0, tr("PID %1 · process start ticks %2").arg(p.key.pid).arg(p.key.start));
      }
    }
    tree_->setSortingEnabled(true);
    if (currentPid.isValid())
      for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = tree_->topLevelItem(i);
        if (it->data(1, Qt::UserRole) == currentPid && it->data(0, Qt::UserRole) == currentStart) {
          tree_->setCurrentItem(it);
          break;
        }
      }
    tree_->verticalScrollBar()->setValue(scroll);
    tree_->setUpdatesEnabled(true);
  }

  QString CompareText() const {
    if (!hasCompare_) return {};
    const auto a = FlightRecorder::Summarize(recorder_.Frames()), b = FlightRecorder::Summarize(compare_.Frames());
    return tr("\nCompared with %1 (dashed): CPU avg %2% vs %3% · CPU peak %4% vs %5% · memory peak %6% vs %7%")
        .arg(compareName_)
        .arg(a.avgCpu, 0, 'f', 1).arg(b.avgCpu, 0, 'f', 1)
        .arg(a.maxCpu, 0, 'f', 0).arg(b.maxCpu, 0, 'f', 0)
        .arg(a.maxMemPct, 0, 'f', 0).arg(b.maxMemPct, 0, 'f', 0);
  }

  QElapsedTimer captureClock_;
  qint64 elapsedMs_ = 0;
  FlightRecorder recorder_;
  FlightRecorder preroll_;  // the last two minutes, kept while not recording (auto-record)
  FlightRecorder compare_;
  bool hasCompare_ = false;
  QString compareName_;
  QListWidget *anomalies_;
  QString anomaliesKey_ = QStringLiteral("unset");  // differs from every real key, so the first call fills the list
  QMenu *menu_ = nullptr;
  bool auto_ = false;
  double hotCpu_ = 0, hotTemp_ = 0, hotPressure_ = 0, calm_ = 0;
  QElapsedTimer autoClock_, autoEnded_;
  SnapshotPtr latest_;
  QPushButton *capture_;
  QSlider *timeline_;
  RecordingGraph *graph_;
  QTreeWidget *tree_;
  QLabel *note_;
  QLineEdit *filter_ = nullptr;
  bool follow_ = true;
  bool imported_ = false;
  bool dirty_ = false;
  QString shownKey_;  // frame and filter the table was last built for
};
}  // namespace

Page *CreateFlightRecorderPage(QWidget *parent) { return new FlightRecorderPage(parent); }

}  // namespace td::ui
