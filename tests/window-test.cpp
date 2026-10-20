#include "ui/MainWindow.h"
#include "ui/PageRecorder.h"
#include "ui/Page.h"
#include "ui/ResponsiveTables.h"
#include "ui/TableAtlas.h"
#include "ui/BenchmarkDashboard.h"
#include "core/Benchmarks.h"
#include "ui/Theme.h"
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QAbstractItemDelegate>
#include <QStyleOptionViewItem>
#include "ui/pages/FlightRecorderControls.h"
#include "core/Settings.h"
#include "ui/Roles.h"
#include "core/FlightRecorder.h"
#include <QListWidget>
#include <QToolButton>
#include <QLabel>
#include <QTreeView>
#include <QHeaderView>
#include <QTreeWidget>
#include <QSlider>
#include <QDialog>
#include <QLineEdit>
#include <QDir>
#include <QApplication>
#include <QAction>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QFileInfo>
#include <QTimer>

class ActionTree : public QTreeWidget {
public:
  QString keyTarget;
protected:
  void keyPressEvent(QKeyEvent *event) override {
    if(event->key()==Qt::Key_Delete) { keyTarget=currentItem()?currentItem()->text(0):QString();event->accept();return; }
    QTreeWidget::keyPressEvent(event);
  }
};

class WindowTest : public QObject {
  Q_OBJECT
private slots:
  void navigationAndPopout() {
    td::ui::MainWindow window;
    window.show();
    QVERIFY(window.PageIds().contains("flight-recorder"));
    QVERIFY(window.PageIds().contains("benchmarks"));
    for (const auto &id : window.PageIds()) {
      window.ShowPage(id);
      QTest::qWait(20);
    }
    window.ShowPage("performance");
    QAction *popout = nullptr;
    for (QAction *a : window.findChildren<QAction *>())
      if (a->text() == "Open page in a separate window") popout = a;
    QVERIFY(popout);
    popout->trigger();
    QTest::qWait(50);
    QWidget *detached = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets())
      if (w->windowTitle() == "Regent · Performance") detached = w;
    QVERIFY(detached);
    detached->close();
    QTest::qWait(50);
    window.ShowPage("summary");
  }
  void graphicViewPreservesTableAndContextTargets() {
    QWidget page;
    page.setProperty("pageId","services");page.resize(700,600);
    auto *layout=new QVBoxLayout(&page);
    auto *tree=new ActionTree;tree->setHeaderLabels({"Name","Status","PID"});
    auto *first=new QTreeWidgetItem(tree,{"one.service","running","100"});
    auto *second=new QTreeWidgetItem(tree,{"two.service","running","101"});
    auto *filtered=new QTreeWidgetItem(tree,{"filtered.service","stopped","102"});filtered->setHidden(true);
    first->setData(0,Qt::UserRole,true);second->setData(0,Qt::UserRole,true);
    tree->setCurrentItem(second);
    layout->addWidget(tree);auto *model=tree->model();auto *selection=tree->selectionModel();
    td::ui::InstallResponsiveTables(&page);page.show();QTest::qWait(500);
    auto *atlas=page.findChild<td::ui::TableAtlas*>();QVERIFY(atlas);QVERIFY(atlas->isVisible());
    QCOMPARE(atlas->EntryCount(),2);QVERIFY(!tree->isVisible());
    // Services open as rows: the selected row is marked in its state colour, the other is plain.
    QVERIFY(atlas->Dense());
    const auto rows=atlas->grab().toImage();
    QCOMPARE(rows.pixelColor(atlas->EntryRect(1).topLeft()),td::theme::Good());
    QCOMPARE(rows.pixelColor(atlas->EntryRect(0).topLeft()),td::theme::Bg());
    QVERIFY(atlas->EntryRect(0).height()<30);
    QTest::keyClick(atlas,Qt::Key_V);QVERIFY(!atlas->Dense());QVERIFY(atlas->EntryRect(0).height()>=40);
    QTest::qWait(300);  // the rows-to-cards cross-fade
    const auto graphic=atlas->grab().toImage();
    QCOMPARE(graphic.pixelColor(atlas->EntryRect(1).topLeft()),td::theme::Good());
    QCOMPARE(graphic.pixelColor(atlas->EntryRect(0).topLeft()),td::theme::Border());
    QTest::keyClick(atlas,Qt::Key_Delete);QCOMPARE(tree->keyTarget,QString("two.service"));
    QVERIFY(!tree->isColumnHidden(1));QCOMPARE(tree->model(),model);QCOMPARE(tree->selectionModel(),selection);
    QTest::mouseClick(atlas,Qt::LeftButton,Qt::NoModifier,atlas->EntryRect(0).center());
    QCOMPARE(tree->currentItem(),first);
    QString target;
    connect(tree,&QTreeWidget::customContextMenuRequested,&page,[&](const QPoint &pos){auto *item=tree->itemAt(pos);target=item?item->text(0):QString();});
    auto point=atlas->EntryRect(1).center();QContextMenuEvent menu(QContextMenuEvent::Mouse,point,atlas->mapToGlobal(point));
    QApplication::sendEvent(atlas,&menu);QCOMPARE(target,QString("two.service"));
    // Cards only: no table toggle, at any width.
    QVERIFY(!page.findChild<QToolButton*>("TableFold"));
    page.resize(1400,800);QTest::qWait(300);QVERIFY(!tree->isVisible());QVERIFY(atlas->isVisible());
    QVERIFY(!tree->isColumnHidden(1));
    // Ctrl+click adds a second card to the selection; the table's selection follows.
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    QTest::mouseClick(atlas,Qt::LeftButton,Qt::ControlModifier,atlas->EntryRect(0).center());
    QCOMPARE(tree->selectionModel()->selectedRows().size(),2);
  }
  void graphicIncludesRealParents() {
    QWidget host;host.resize(700,600);
    auto *tree=new QTreeWidget(&host);tree->setHeaderLabels({"Name","PID","CPU"});
    auto *group=new QTreeWidgetItem(tree,{"Apps","",""});
    auto *parent=new QTreeWidgetItem(group,{"browser","10","50%"});
    new QTreeWidgetItem(parent,{"child","11","2%"});tree->hide();
    auto *atlas=new td::ui::TableAtlas(tree,"processes",&host);atlas->setGeometry(host.rect());host.show();QTest::qWait(100);
    QCOMPARE(atlas->EntryCount(),2);  // the Apps section (opened once by itself) and the browser card
    atlas->setFocus();atlas->Refresh();
    QTest::mouseClick(atlas,Qt::LeftButton,Qt::NoModifier,atlas->EntryRect(1).center());
    QTest::keyClick(atlas,Qt::Key_Right);  // opens the browser's children
    QCOMPARE(atlas->EntryCount(),3);
    QTest::keyClick(atlas,Qt::Key_Left);
    QCOMPARE(atlas->EntryCount(),2);
    auto *folders=new QTreeWidget(&host);folders->setHeaderLabels({"Folder","Size"});
    auto *folder=new QTreeWidgetItem(folders,{"documents","500 MB"});new QTreeWidgetItem(folder,{"…"});folders->hide();
    auto *storage=new td::ui::TableAtlas(folders,"disk-space",&host);storage->setGeometry(host.rect());storage->show();QTest::qWait(100);
    QCOMPARE(storage->EntryCount(),1);
  }
  void benchmarkHistoryIsPainted() {
    td::ui::BenchmarkDashboard board;board.resize(900,285);board.show();QTest::qWait(30);
    const auto before=board.grab().toImage();
    std::atomic<bool> cancel{false};auto result=td::benchmarks::Run(td::benchmarks::Kind::Cpu,15,cancel);
    QVERIFY(result.score>0);board.Begin(result.kind,1000);QVERIFY(board.Running());board.Complete(result);
    QVERIFY(!board.Running());QCOMPARE(board.Score(result.kind),result.score);
    const auto after=board.grab().toImage();int changed=0;
    for(int x=18;x<45;++x)for(int y=166;y<212;++y)if(before.pixel(x,y)!=after.pixel(x,y))++changed;
    QVERIFY2(changed>20,"Measured CPU history must render visible pixel bars at the default panel height");
  }
  void barsUseThePagesNumbersNotTheText() {
    QWidget page;
    page.setProperty("pageId","installed-apps");page.resize(700,400);
    auto *layout=new QVBoxLayout(&page);
    auto *tree=new QTreeWidget;tree->setHeaderLabels({"Application","Size"});
    auto *a=new QTreeWidgetItem(tree,{"a","1 kB"});a->setData(1,td::ui::kValueRole,5e9);
    auto *b=new QTreeWidgetItem(tree,{"b","900 MB"});b->setData(1,td::ui::kValueRole,1.0);
    new QTreeWidgetItem(tree,{"c","2 MB"});  // no number given: read from the text
    layout->addWidget(tree);
    td::ui::InstallResponsiveTables(&page);page.show();QTest::qWait(300);
    auto *atlas=page.findChild<td::ui::TableAtlas*>();QVERIFY(atlas);QCOMPARE(atlas->EntryCount(),3);
    QCOMPARE(atlas->EntryWeight(0),5e9);QCOMPARE(atlas->EntryWeight(1),1.0);QCOMPARE(atlas->EntryWeight(2),2e6);
  }

  void mergedPagesRouteTheirParts() {
    td::ui::MainWindow window;
    window.show();window.resize(1200,800);QTest::qWait(200);
    // Hardware is two pages with parts; Users is a grouping of Processes.
    QVERIFY(!window.PageIds().contains("power-and-freq"));
    QVERIFY(!window.PageIds().contains("users"));
    QVERIFY(window.AllPageIds().contains("power-and-freq"));
    QVERIFY(window.AllPageIds().contains("drivers"));
    auto page=[&](const QString &id)->td::ui::Page*{
      for(auto *p:window.findChildren<td::ui::Page*>()) if(p->Id()==id) return p;
      return nullptr;
    };
    window.ShowPage("power-and-freq");QTest::qWait(50);
    QVERIFY(page("power-and-freq")->isVisible());QVERIFY(!page("performance")->CurrentPart()->isHidden() );
    QCOMPARE(page("performance")->CurrentPart()->Id(),QString("power-and-freq"));
    window.ShowPage("drivers");QTest::qWait(50);
    QVERIFY(page("drivers")->isVisible());QVERIFY(!page("system-info")->isVisible());
    window.ShowPage("users");QTest::qWait(50);
    QVERIFY(page("processes")->isVisible());
    QCOMPARE(td::Settings::Get().processGrouping(),QString("user"));
    td::Settings::Get().set("processGrouping","apps");
  }

  void rowsOrCardsKeyWorksFromAnyFocus() {
    td::ui::MainWindow window;
    window.show();window.resize(1100,800);
    window.ShowPage("processes");QTest::qWait(300);
    td::ui::TableAtlas *atlas=nullptr;
    for(auto *a:window.findChildren<td::ui::TableAtlas*>()) if(a->isVisible()) atlas=a;
    QVERIFY(atlas);
    const bool before=atlas->Dense();
    // The sidebar has focus: v still switches the list.
    auto *nav=window.findChild<QListWidget *>();QVERIFY(nav);
    nav->setFocus();QTest::qWait(20);
    QTest::keyClick(nav,Qt::Key_V);QCOMPARE(atlas->Dense(),!before);
    QTest::keyClick(atlas,Qt::Key_V);QCOMPARE(atlas->Dense(),before);
    // In the filter it is a letter to type.
    QLineEdit *filter=nullptr;
    for(auto *e:window.findChildren<QLineEdit*>()) if(e->isVisible()&&e->placeholderText().startsWith("Filter")) filter=e;
    QVERIFY(filter);
    filter->setFocus();QTest::keyClick(filter,Qt::Key_V);
    QCOMPARE(filter->text(),QString("v"));QCOMPARE(atlas->Dense(),before);
    filter->clear();
  }
  void responsiveViewsAndFlightDock() {
    QTemporaryDir output;
    td::Settings::Get().set("flightRecordingDirectory",output.path());
    td::ui::MainWindow window;
    window.show();
    window.resize(1400,900);
    QTest::qWait(100);
    auto *nav=window.findChild<QListWidget *>();
    QVERIFY(nav);
    for(int i=0;i<nav->count();++i)QVERIFY(nav->item(i)->isHidden()||nav->item(i)->text()!="Flight Recorder");
    // Focusing the sidebar while Flight Recorder is open must not jump to the first page.
    window.ShowPage("flight-recorder");nav->setFocus(Qt::TabFocusReason);QTest::qWait(50);
    for(auto *p:window.findChildren<td::ui::Page*>()) if(p->Id()=="flight-recorder") QVERIFY(p->isVisible());
    // Arrows from it step to its neighbours, not to the top of the list.
    QTest::keyClick(nav,Qt::Key_Down);
    for(auto *p:window.findChildren<td::ui::Page*>()) if(p->Id()=="benchmarks") QVERIFY(p->isVisible());
    window.ShowPage("flight-recorder");QTest::keyClick(nav,Qt::Key_Up);
    for(auto *p:window.findChildren<td::ui::Page*>()) if(p->Id()=="disk-space") QVERIFY(p->isVisible());
    window.ShowPage("summary");
    td::ui::Page *processes=nullptr,*flight=nullptr;
    for(auto *p:window.findChildren<td::ui::Page*>()) {
      if(p->Id()=="processes")processes=p;
      if(p->Id()=="flight-recorder")flight=p;
    }
    QVERIFY(processes&&flight);
    window.ShowPage("processes");
    auto *view=processes->findChild<QTreeView*>();
    QVERIFY(view);
    auto *sourceModel=view->model();
    auto *selection=view->selectionModel();
    window.resize(700,800);QTest::qWait(250);
    if(window.width()>700)for(auto *page:window.findChildren<td::ui::Page*>())qWarning()<<page->Id()<<page->minimumSizeHint();
    QVERIFY2(window.width()<=700,qPrintable(QString("Minimum width forced to %1").arg(window.width())));
    QVERIFY(!view->isVisible());
    QVERIFY(processes->findChild<td::ui::TableAtlas*>()->isVisible());
    QVERIFY(!view->isColumnHidden(1));
    QCOMPARE(view->model(),sourceModel);
    QCOMPARE(view->selectionModel(),selection);
    window.showFullScreen();QTest::qWait(250);
    QVERIFY(!view->isVisible());
    QVERIFY(processes->findChild<td::ui::TableAtlas*>()->isVisible());
    QVERIFY(!view->isColumnHidden(1));
    window.showNormal();
    window.resize(1400,900);QTest::qWait(250);
    QVERIFY(!view->isVisible());
    QVERIFY(processes->findChild<td::ui::TableAtlas*>()->isVisible());
    QVERIFY(!view->isColumnHidden(1));
    window.ShowPage("performance");window.resize(700,800);QTest::qWait(100);
    auto *devices=window.findChild<QListWidget*>("PerformanceDevices");
    auto *split=window.findChild<QSplitter*>("PerformanceSplit");
    QVERIFY(devices&&split);QCOMPARE(split->orientation(),Qt::Horizontal);QVERIFY(devices->count()>=4);
    QVERIFY2(devices->visualItemRect(devices->item(1)).bottom()<devices->viewport()->height(),"Memory selector must stay visible below CPU");
    devices->setCurrentRow(1);QCOMPARE(window.findChild<QStackedWidget*>("PerformanceDetails")->currentIndex(),1);
    devices->setCurrentRow(2);QCOMPARE(window.findChild<QStackedWidget*>("PerformanceDetails")->currentIndex(),2);
    for(const auto &id:window.PageIds()) {
      window.ShowPage(id);window.resize(700,800);QTest::qWait(30);
      window.resize(1400,900);QTest::qWait(30);
    }
    auto *controls=dynamic_cast<td::ui::FlightRecorderControls*>(flight);
    QVERIFY(controls);
    auto *start=window.findChild<QToolButton*>("FlightStart");
    auto *stop=window.findChild<QToolButton*>("FlightStop");
    auto *history=window.findChild<QToolButton*>("FlightHistory");
    auto *live=window.findChild<QToolButton*>("FlightLive");
    QVERIFY(start&&stop&&history&&live);
    window.ShowPage("summary");start->click();
    auto sample=std::make_shared<td::Snapshot>();sample->time=QDateTime::currentDateTime();sample->generation=100000;
    sample->memTotal=1024;sample->memAvailable=512;flight->Update(sample);
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("FlightTime")->text()!="00:00:00",2500);
    QVERIFY(controls->IsCapturing());
    history->click();QVERIFY(flight->isVisible());
    auto *slider=flight->findChild<QSlider*>();QVERIFY(slider);
    slider->setValue(0);live->click();QCOMPARE(slider->value(),slider->maximum());
    stop->click();QVERIFY(!controls->IsCapturing());
    const auto files=QDir(output.path()).entryList({"*.regent.json"},QDir::Files);
    QCOMPARE(files.size(),1);
    td::FlightRecorder loaded;QString error;
    QVERIFY2(loaded.Load(output.filePath(files.first()),&error),qPrintable(error));
    QVERIFY(!loaded.Frames().empty());
    auto *notice=window.findChild<QLabel*>("CaptureSavePath");QVERIFY(notice&&notice->isVisible());
    QVERIFY(notice->text().contains(output.filePath(files.first())));
    QVERIFY(window.findChild<QToolButton*>("CaptureFolder")->isVisible());
  }
  void paletteKeysAndUnits() {
    td::ui::MainWindow window;
    window.show();
    window.resize(1400, 900);
    QVERIFY(window.PageIds().contains("units"));
    // Ctrl+K: type, Enter runs the best match.
    QAction *palette = nullptr;
    for (QAction *a : window.findChildren<QAction *>())
      if (a->text().remove('&').startsWith("Command palette")) palette = a;
    QVERIFY(palette);
    palette->trigger();
    auto *dialog = window.findChild<QDialog *>("CommandPalette");
    QVERIFY(dialog && dialog->isVisible());
    auto *edit = dialog->findChild<QLineEdit *>();
    QVERIFY(edit);
    QTest::keyClicks(edit, "go uni");
    QTest::keyClick(edit, Qt::Key_Return);
    QVERIFY(!dialog->isVisible());
    td::ui::Page *units = nullptr, *processes = nullptr;
    for (auto *p : window.findChildren<td::ui::Page *>()) {
      if (p->Id() == "units") units = p;
      if (p->Id() == "processes") processes = p;
    }
    QVERIFY(units && units->isVisible());
    auto *unitTree = units->findChild<QTreeWidget *>();
    QTRY_VERIFY_WITH_TIMEOUT(unitTree->topLevelItemCount() > 0, 5000);
    // j/k move through a list like the arrows; / jumps to the filter, Esc back.
    window.ShowPage("processes");
    auto *view = processes->findChild<QTreeView *>();
    auto *cards = processes->findChild<td::ui::TableAtlas *>();
    QTRY_VERIFY_WITH_TIMEOUT(view->model()->rowCount() > 0 && cards->EntryCount() > 2, 5000);
    cards->setFocus();
    QTest::mouseClick(cards, Qt::LeftButton, Qt::NoModifier, cards->EntryRect(0).center() + QPoint(60, 0));
    const QModelIndex start = view->currentIndex().siblingAtColumn(0);
    QTest::keyClick(cards, Qt::Key_J);
    QCOMPARE(view->currentIndex().siblingAtColumn(0), view->indexBelow(start));
    QTest::keyClick(cards, Qt::Key_K);
    QCOMPARE(view->currentIndex().siblingAtColumn(0), start);
    QTest::keyClick(cards, Qt::Key_Slash);
    // The window's own focus child: offscreen windows are never "active" for QApplication.
    auto *filter = qobject_cast<QLineEdit *>(window.focusWidget());
    QVERIFY(filter);
    QVERIFY(filter->placeholderText().startsWith("Filter"));
    QTest::keyClick(filter, Qt::Key_Escape);
    QCOMPARE(window.focusWidget(), static_cast<QWidget *>(cards));
  }
  void closeFinalizesRecording() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) QSKIP("ffmpeg is not installed");
    QTemporaryDir output;
    QVERIFY(output.isValid());
    td::ui::MainWindow window;
    window.show();
    auto *recorder = window.findChild<td::ui::PageRecorder *>();
    QVERIFY(recorder);
    QWidget *target = nullptr;
    for (auto *page : window.findChildren<td::ui::Page *>())
      if (page->Id() == "summary") target = page;
    QVERIFY(target);
    QSignalSpy started(recorder, &td::ui::PageRecorder::Started);
    QSignalSpy finished(recorder, &td::ui::PageRecorder::Finished);
    QSignalSpy errors(recorder, &td::ui::PageRecorder::Error);
    QVERIFY(recorder->Start(target, output.filePath("close.webm")));
    QTRY_VERIFY_WITH_TIMEOUT(started.count() == 1, 5000);
    QTest::qWait(250);
    window.ShowPage("processes");
    QTest::qWait(250);
    QVERIFY(!window.close()); // asynchronous finalization defers the close
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 10000);
    QCOMPARE(errors.count(), 0);
    QVERIFY(!recorder->IsRecording());
    QVERIFY(QFileInfo(output.filePath("close.webm")).size() > 0);
  }
};

int main(int argc, char **argv) {
  QTemporaryDir config;
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  app.setStyle("Fusion");
  WindowTest test;
  const int result = QTest::qExec(&test, argc, argv);
  QThreadPool::globalInstance()->waitForDone();
  return result;
}
#include "window-test.moc"
