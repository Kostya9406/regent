#include "ui/pages/Pages.h"
#include <QApplication>
#include <QPushButton>
#include <QSlider>
#include <QTreeWidget>
#include <QMessageBox>
#include <QTimer>
#include <QPixmap>
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
void answer(QMessageBox::StandardButton button) {
  QTimer::singleShot(0, [button] {
    for (QWidget *widget : QApplication::topLevelWidgets())
      if (auto *box = qobject_cast<QMessageBox *>(widget)) box->button(button)->click();
  });
}
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  auto *page = td::ui::CreateFlightRecorderPage(nullptr);
  page->resize(950, 650); page->show(); app.processEvents();
  REQUIRE(page->CanClose());
  auto snapshot = std::make_shared<td::Snapshot>();
  snapshot->time = QDateTime::currentDateTimeUtc();
  snapshot->generation = 1; snapshot->cpuUtil = 24; snapshot->memTotal = 1000; snapshot->memAvailable = 500;
  td::ProcessInfo p; p.key = {111, 222}; p.name = "first"; p.cpu = 24; p.threads = 1;
  snapshot->processes.append(p);
  page->Update(snapshot);
  QPushButton *start = nullptr, *clear = nullptr;
  for (auto *button : page->findChildren<QPushButton *>()) {
    if (button->text() == "Start recording") start = button;
    if (button->text() == "Clear") clear = button;
  }
  REQUIRE(start && clear); start->click();
  auto *slider = page->findChild<QSlider *>();
  auto *tree = page->findChild<QTreeWidget *>();
  REQUIRE(slider && tree && tree->topLevelItemCount() == 1);
  auto next = std::make_shared<td::Snapshot>(*snapshot);
  next->generation = 2; next->time = next->time.addSecs(1); next->processes[0].name = "second";
  page->Update(next);
  REQUIRE(slider->maximum() == 1 && slider->value() == 1);
  slider->setValue(0);
  REQUIRE(tree->topLevelItem(0)->text(0) == "first");
  auto third = std::make_shared<td::Snapshot>(*next);
  third->generation = 3; third->time = third->time.addSecs(1);
  page->Update(third);
  REQUIRE(slider->value() == 0 && tree->topLevelItem(0)->text(0) == "first");
  REQUIRE(!page->grab().isNull()); // exercise history plot painting
  answer(QMessageBox::Cancel); REQUIRE(!page->CanClose());
  answer(QMessageBox::Cancel); clear->click(); REQUIRE(slider->maximum() == 2);
  answer(QMessageBox::Discard); clear->click(); REQUIRE(!slider->isEnabled());
  REQUIRE(page->CanClose());
  delete page;
  std::cout << "Flight Recorder UI: plot paint, timeline selection preservation, unsaved Cancel/Discard, clear passed\n";
}
