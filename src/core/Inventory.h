#pragma once

// Static inventories read straight from disk: installed packages (the
// pacman local database, joined with .desktop entries) and kernel modules
// (/proc/modules, /sys/module and modules.dep / modules.builtin).

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

namespace td::inventory {

struct App {
  QString package;
  QString version;
  QString description;
  QString url;
  QString packager;
  QString reason;  // explicit | dependency
  QDateTime installed;
  quint64 size = 0;
  QStringList desktopNames;  // Name= of the package's .desktop files
  QString exec;              // Exec= of the first one
  QString desktopFile;
};

struct Module {
  QString name;
  bool loaded = false;
  bool builtin = false;
  bool outOfTree = false;
  quint64 size = 0;  // loaded size, from /proc/modules
  int refs = 0;
  QStringList usedBy;
  QStringList depends;
  QString path;       // relative path from modules.dep
  QString version;
  QString srcversion;
  QString taint;
};

struct Modules {
  QVector<Module> list;
  QString kernel;
  int loaded = 0, outOfTree = 0;
  quint64 loadedBytes = 0;
};

// A hardware device and the driver bound to it, from /sys/bus/{pci,usb}.
struct Device {
  QString bus;       // PCI, USB
  QString address;   // 0000:00:02.0, 1-4
  QString name;
  QString vendor;
  QString kind;      // PCI class / USB class or "Interface"
  QString driver;    // bound driver, empty when none
  QString module;    // kernel module providing it (empty when built-in)
  QString modalias;
  QString ids;       // vendor:device
  QStringList interfaceDrivers;  // USB: drivers of the device's interfaces
};

QVector<App> InstalledApps();  // empty when not an Arch-family system
Modules KernelModules();
QVector<Device> Devices();

}  // namespace td::inventory
