#include "core/SamplingEngine.h"

#include "core/Sampler.h"

#include <QTimer>

namespace td {

class SamplingEngine::Worker : public QObject {
  Q_OBJECT
public:
  QTimer *timer = nullptr;
  Sampler sampler;

public slots:
  void Init(int intervalMs) {
    timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, &Worker::Tick);
    sampler.Collect();  // prime the counters so the first published rates are real
    QTimer::singleShot(250, this, &Worker::Tick);
    SetInterval(intervalMs);
  }
  void SetInterval(int ms) {
    if (ms <= 0) timer->stop();
    else timer->start(ms);
  }
  void Tick() { emit Ready(sampler.Collect()); }

signals:
  void Ready(td::SnapshotPtr s);
};

SamplingEngine::SamplingEngine(QObject *parent) : QObject(parent) {
  qRegisterMetaType<SnapshotPtr>();
  worker_ = new Worker;
  worker_->moveToThread(&thread_);
  connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
  connect(worker_, &Worker::Ready, this, [this](SnapshotPtr s) {
    latest_ = s;
    emit Published(s);
  });
}

SamplingEngine::~SamplingEngine() {
  thread_.quit();
  thread_.wait();
}

void SamplingEngine::Start(int intervalMs) {
  thread_.setObjectName("sampling");
  thread_.start();
  QMetaObject::invokeMethod(worker_, "Init", Qt::QueuedConnection, Q_ARG(int, intervalMs));
}

void SamplingEngine::SetInterval(int intervalMs) {
  QMetaObject::invokeMethod(worker_, "SetInterval", Qt::QueuedConnection, Q_ARG(int, intervalMs));
}

void SamplingEngine::RefreshNow() {
  QMetaObject::invokeMethod(worker_, "Tick", Qt::QueuedConnection);
}

}  // namespace td

#include "SamplingEngine.moc"
