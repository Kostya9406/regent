#pragma once

#include "ui/Page.h"

namespace td::ui {

Page *CreateSummaryPage(QWidget *parent);
Page *CreatePerformancePage(QWidget *parent);
Page *CreateProcessesPage(QWidget *parent);
Page *CreateSystemInfoPage(QWidget *parent);
Page *CreateStartupPage(QWidget *parent);
Page *CreateServicesPage(QWidget *parent);
Page *CreateUnitsPage(QWidget *parent);
Page *CreatePowerFreqPage(QWidget *parent);
Page *CreateConnectionsPage(QWidget *parent);
Page *CreateInstalledAppsPage(QWidget *parent);
Page *CreateDriversPage(QWidget *parent);
Page *CreateDiskSpacePage(QWidget *parent);
Page *CreateFlightRecorderPage(QWidget *parent);
Page *CreateBenchmarksPage(QWidget *parent);
// One sidebar entry over several pages, switched by buttons in the header
// (Alt+the underlined letter). Each part keeps its own id, so --page, alerts
// and the palette can still name it.
struct Part {
  QString label;  // with an & mnemonic
  Page *page;
};
Page *CreateCompositePage(const QString &id, const QString &title, const QVector<Part> &parts, QWidget *parent);

}  // namespace td::ui
