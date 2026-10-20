// Standalone regression tests; see README-benchmarks.md for the qmake command.
#include "core/Benchmarks.h"
#include "ui/Page.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace td::ui { Page *CreateBenchmarksPage(QWidget *parent); }

namespace {

void Require(bool condition, const char *message) {
  if (condition) return;
  std::fprintf(stderr, "FAIL: %s\n", message);
  std::exit(1);
}

void CheckCore() {
  std::atomic<bool> cancel{false};
  for (auto kind : {td::benchmarks::Kind::Cpu, td::benchmarks::Kind::Memory}) {
    const auto result = td::benchmarks::Run(kind, 25, cancel);
    Require(result.error.isEmpty(), "benchmark reports no allocation error");
    Require(!result.cancelled, "normal run completes");
    Require(result.seconds >= 0.024 && result.seconds < 2.0, "duration is bounded");
    Require(result.score > 0, "completed test has positive throughput");
    Require(result.checksum != 0, "workload publishes an observable checksum");
    cancel = true;
    const auto alreadyCancelled = td::benchmarks::Run(kind, 10000, cancel);
    Require(alreadyCancelled.cancelled && alreadyCancelled.score == 0, "pre-cancelled test does no work");
    cancel = false;
    QElapsedTimer clock;
    clock.start();
    std::thread stopper([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      cancel = true;
    });
    const auto interrupted = td::benchmarks::Run(kind, 10000, cancel);
    stopper.join();
    Require(interrupted.cancelled, "in-flight test observes cancellation");
    Require(clock.elapsed() < 2000, "cancellation does not wait for the requested 10 seconds");
    cancel = false;
  }
  const auto unavailable = td::benchmarks::RunGpu(nullptr, 10, cancel);
  Require(!unavailable.error.isEmpty() && unavailable.score == 0, "missing GPU context is unavailable rather than a fake score");

  QTemporaryDir directory;
  Require(directory.isValid(), "test temporary directory exists");
  const auto disk = td::benchmarks::RunDisk(directory.path(), 100, cancel);
  Require(disk.error.isEmpty() && !disk.cancelled, "temporary disk benchmark completes");
  Require(disk.score > 0 && disk.secondaryScore > 0, "disk reports real write and read throughput");
  Require(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(), "disk benchmark removes its private file");
  const auto badDisk = td::benchmarks::RunDisk(directory.filePath("missing-folder"), 100, cancel);
  Require(!badDisk.error.isEmpty() && badDisk.score == 0, "unavailable disk folder produces an error");
  cancel = true;
  const auto cancelledDisk = td::benchmarks::RunDisk(directory.path(), 100, cancel);
  Require(cancelledDisk.cancelled, "pre-cancelled disk test does no I/O");
}

td::ui::Page *StartPage(const QString &buttonText) {
  auto *page = td::ui::CreateBenchmarksPage(nullptr);
  Require(page->Id() == "benchmarks", "page factory returns the benchmarks page");
  for (auto *button : page->findChildren<QPushButton *>()) {
    if (button->text() != buttonText) continue;
    button->click();
    Require(!button->isEnabled(), "start disables controls while the worker runs");
    return page;
  }
  Require(false, "benchmark start control exists");
  return nullptr;
}

void CheckPageDestruction(const QString &buttonText) {
  auto *page = StartPage(buttonText);
  QEventLoop loop;
  QTimer::singleShot(30, &loop, [&] {
    QElapsedTimer timer;
    timer.start();
    delete page;
    Require(timer.elapsed() < 2000, "page teardown cancels and joins the active worker promptly");
    loop.quit();
  });
  loop.exec();
}

void CheckNetwork() {
  // Only the explicit loopback URL is requested; no public server is contacted.
  QTcpServer server;
  Require(server.listen(QHostAddress::LocalHost, 0), "local HTTP test server starts");
  int requests = 0;
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
    auto *socket = server.nextPendingConnection();
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
      const QByteArray request = socket->readAll();
      if (socket->property("responded").toBool()) return;
      socket->setProperty("responded", true);
      ++requests;
      if (request.contains(" /stall ")) {
        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\n\r\nb");
        return;  // Hold the connection open until the client cancels.
      }
      if (request.contains(" /error ")) {
        socket->write("HTTP/1.1 500 Test failure\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        socket->disconnectFromHost();
        return;
      }
      const QByteArray body(1024 * 1024, 'b');
      socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n");
      socket->write(body);
      socket->disconnectFromHost();
    });
    QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
  });
  auto *page = td::ui::CreateBenchmarksPage(nullptr);
  QPushButton *start = nullptr;
  QLineEdit *url = nullptr;
  for (auto *button : page->findChildren<QPushButton *>())
    if (button->text() == "Test Internet") start = button;
  for (auto *field : page->findChildren<QLineEdit *>())
    if (field->accessibleName() == "Internet benchmark download URL") url = field;
  auto *rows = page->findChild<QTreeWidget *>();
  Require(start && url && rows, "network page controls exist");
  start->click();
  Require(requests == 0 && rows->topLevelItemCount() == 0, "blank URL never starts a download");
  url->setText(QStringLiteral("http://127.0.0.1:%1/payload").arg(server.serverPort()));
  start->click();
  const auto waitForRows = [&](int count) {
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (rows->topLevelItemCount() >= count) loop.quit(); });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    Require(rows->topLevelItemCount() == count, "network publishes the expected number of results");
  };
  waitForRows(1);
  Require(requests == 1, "network test requests only the supplied URL");
  Require(rows->topLevelItemCount() == 1, "network test publishes one result");
  Require(rows->topLevelItem(0)->text(5) == "Completed", "loopback download completes without an error");
  Require(rows->topLevelItem(0)->text(2).toDouble() > 0, "network result has positive measured throughput");
  Require(rows->topLevelItem(0)->text(8).startsWith("1048576 bytes;"), "download counts the exact payload bytes");
  url->setText(QStringLiteral("http://127.0.0.1:%1/error").arg(server.serverPort()));
  start->click();
  waitForRows(2);
  Require(rows->topLevelItem(1)->text(5) != "Completed" && rows->topLevelItem(1)->text(2) == "—", "HTTP errors are reported rather than scored");
  url->setText(QStringLiteral("http://127.0.0.1:%1/stall").arg(server.serverPort()));
  start->click();
  QPushButton *stop = nullptr;
  for (auto *button : page->findChildren<QPushButton *>())
    if (button->text() == "Stop") stop = button;
  Require(stop, "network cancellation control exists");
  QTimer::singleShot(30, stop, &QPushButton::click);
  waitForRows(3);
  Require(rows->topLevelItem(2)->text(5) == "Cancelled" && rows->topLevelItem(2)->text(2) == "—", "Stop cancels an in-flight download without scoring partial data");
  start->click();
  QEventLoop teardown;
  QTimer::singleShot(30, &teardown, [&] { delete page; teardown.quit(); });
  teardown.exec();
}

}  // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  CheckCore();
  CheckPageDestruction(QStringLiteral("CPU"));
  CheckPageDestruction(QStringLiteral("Memory"));
  CheckPageDestruction(QStringLiteral("Test disk"));
  CheckPageDestruction(QStringLiteral("GPU"));
  CheckNetwork();

  // Exercise aboutToQuit as well as direct page teardown. No production
  // process is inspected or modified; the entire suite runs in isolation.
  auto *page = StartPage(QStringLiteral("CPU"));
  QTimer::singleShot(30, &app, &QCoreApplication::quit);
  app.exec();
  QElapsedTimer clock;
  clock.start();
  delete page;
  Require(clock.elapsed() < 2000, "page can be destroyed safely after application shutdown");
  std::puts("Benchmark core and page lifecycle tests passed.");
}
