#include "core/Inventory.h"

#include "core/Fs.h"
#include "core/HwIds.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QHash>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>

#include <sys/utsname.h>
#include <tuple>

namespace td::inventory {

namespace {

// %KEY% blocks of a pacman "desc" file.
QHash<QString, QStringList> ParseDesc(const QString &path) {
  QHash<QString, QStringList> m;
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return m;
  QString key;
  QTextStream in(&f);
  while (!in.atEnd()) {
    QString l = in.readLine();
    if (l.startsWith('%') && l.endsWith('%')) key = l.mid(1, l.size() - 2);
    else if (!l.isEmpty() && !key.isEmpty()) m[key] << l;
  }
  return m;
}

QHash<QString, QString> DesktopKeys(const QString &path) {
  QHash<QString, QString> m;
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return m;
  bool in = false;
  QTextStream ts(&f);
  while (!ts.atEnd()) {
    QString l = ts.readLine().trimmed();
    if (l.startsWith('[')) {
      in = l == "[Desktop Entry]";
      continue;
    }
    int eq = l.indexOf('=');
    if (in && eq > 0 && !m.contains(l.left(eq))) m.insert(l.left(eq), l.mid(eq + 1));
  }
  return m;
}

}  // namespace

QVector<App> InstalledApps() {
  QVector<App> out;
  const QString db = "/var/lib/pacman/local";
  for (const QString &dir : QDir(db).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    QString base = db + '/' + dir;
    auto d = ParseDesc(base + "/desc");
    if (d.isEmpty()) continue;
    App a;
    a.package = d.value("NAME").value(0);
    a.version = d.value("VERSION").value(0);
    a.description = d.value("DESC").value(0);
    a.url = d.value("URL").value(0);
    a.packager = d.value("PACKAGER").value(0);
    a.reason = d.value("REASON").value(0) == "1" ? QObject::tr("dependency") : QObject::tr("explicit");
    a.installed = QDateTime::fromSecsSinceEpoch(d.value("INSTALLDATE").value(0).toLongLong());
    a.size = d.value("SIZE").value(0).toULongLong();
    // Which .desktop entries does it ship? The files list says.
    QFile files(base + "/files");
    if (files.open(QIODevice::ReadOnly | QIODevice::Text)) {
      QTextStream in(&files);
      while (!in.atEnd()) {
        QString l = in.readLine();
        if (!l.startsWith("usr/share/applications/") || !l.endsWith(".desktop")) continue;
        auto k = DesktopKeys('/' + l);
        if (k.value("NoDisplay") == "true" || k.value("Type", "Application") != "Application") continue;
        a.desktopNames << k.value("Name", QFileInfo(l).baseName());
        if (a.exec.isEmpty()) {
          a.exec = k.value("Exec");
          a.desktopFile = '/' + l;
        }
      }
    }
    out << a;
  }
  return out;
}

Modules KernelModules() {
  Modules m;
  utsname u{};
  uname(&u);
  m.kernel = QString::fromLatin1(u.release);
  QString modDir = "/lib/modules/" + m.kernel;
  QHash<QString, int> idx;
  auto key = [](QString n) { return n.replace('-', '_'); };

  // Everything the kernel could load…
  QString dep = QString::fromStdString(fs::readRaw(modDir + "/modules.dep", 64 << 20));
  for (const QString &line : dep.split('\n', Qt::SkipEmptyParts)) {
    int c = line.indexOf(':');
    QString path = line.left(c);
    QString name = QFileInfo(path).fileName().section('.', 0, 0);
    Module mod;
    mod.name = key(name);
    mod.path = path;
    for (const QString &d : line.mid(c + 1).split(' ', Qt::SkipEmptyParts))
      mod.depends << key(QFileInfo(d).fileName().section('.', 0, 0));
    idx.insert(mod.name, int(m.list.size()));
    m.list << mod;
  }
  // …plus what is compiled in…
  QString builtin = QString::fromStdString(fs::readRaw(modDir + "/modules.builtin", 16 << 20));
  for (const QString &path : builtin.split('\n', Qt::SkipEmptyParts)) {
    Module mod;
    mod.name = key(QFileInfo(path).fileName().section('.', 0, 0));
    mod.builtin = true;
    mod.path = path;
    if (!idx.contains(mod.name)) {
      idx.insert(mod.name, int(m.list.size()));
      m.list << mod;
    }
  }
  // …and what is loaded now.
  QString pm = QString::fromStdString(fs::readRaw("/proc/modules"));
  for (const QString &line : pm.split('\n', Qt::SkipEmptyParts)) {
    QStringList f = line.split(' ');
    if (f.size() < 4) continue;
    QString name = f[0];
    if (!idx.contains(name)) {
      Module mod;
      mod.name = name;
      idx.insert(name, int(m.list.size()));
      m.list << mod;
    }
    Module &mod = m.list[idx.value(name)];
    mod.loaded = true;
    mod.size = f[1].toULongLong();
    mod.refs = f[2].toInt();
    if (f[3] != "-") mod.usedBy = f[3].split(',', Qt::SkipEmptyParts);
    QString sys = "/sys/module/" + name;
    mod.version = fs::readText(sys + "/version");
    mod.srcversion = fs::readText(sys + "/srcversion");
    mod.taint = fs::readText(sys + "/taint");
    mod.outOfTree = mod.taint.contains('O');
    ++m.loaded;
    m.outOfTree += mod.outOfTree;
    m.loadedBytes += mod.size;
  }
  std::sort(m.list.begin(), m.list.end(), [](const Module &a, const Module &b) {
    return a.loaded != b.loaded ? a.loaded : a.name < b.name;
  });
  return m;
}

namespace {

QString LinkName(const QString &path) { return QFileInfo(path).symLinkTarget().section('/', -1); }

QString UsbClass(int c) {
  switch (c) {
    case 0x01: return QObject::tr("Audio");
    case 0x02: return QObject::tr("Communications");
    case 0x03: return QObject::tr("Human interface device");
    case 0x05: return QObject::tr("Physical");
    case 0x06: return QObject::tr("Imaging");
    case 0x07: return QObject::tr("Printer");
    case 0x08: return QObject::tr("Mass storage");
    case 0x09: return QObject::tr("Hub");
    case 0x0a: return QObject::tr("CDC data");
    case 0x0b: return QObject::tr("Smart card");
    case 0x0e: return QObject::tr("Video");
    case 0x0f: return QObject::tr("Personal healthcare");
    case 0x10: return QObject::tr("Audio/Video");
    case 0xe0: return QObject::tr("Wireless controller");
    case 0xef: return QObject::tr("Miscellaneous");
    case 0xfe: return QObject::tr("Application specific");
    case 0xff: return QObject::tr("Vendor specific");
    default: return QObject::tr("Composite");
  }
}

}  // namespace

QVector<Device> Devices() {
  QVector<Device> out;
  for (const QString &a : fs::list("/sys/bus/pci/devices")) {
    QString b = "/sys/bus/pci/devices/" + a;
    quint16 v = quint16(fs::readText(b + "/vendor").toUInt(nullptr, 16));
    quint16 d = quint16(fs::readText(b + "/device").toUInt(nullptr, 16));
    uint cls = fs::readText(b + "/class").toUInt(nullptr, 16);
    Device dev;
    dev.bus = "PCI";
    dev.address = a;
    dev.vendor = hwids::PciVendor(v);
    dev.name = hwids::PciDevice(v, d);
    if (dev.name.isEmpty()) dev.name = QString("%1:%2").arg(v, 4, 16, QChar('0')).arg(d, 4, 16, QChar('0'));
    dev.kind = hwids::PciClass(quint8(cls >> 16), quint8(cls >> 8));
    dev.driver = LinkName(b + "/driver");
    dev.module = LinkName(b + "/driver/module");
    dev.modalias = fs::readText(b + "/modalias");
    dev.ids = QString("%1:%2").arg(v, 4, 16, QChar('0')).arg(d, 4, 16, QChar('0'));
    out << dev;
  }
  for (const QString &a : fs::list("/sys/bus/usb/devices")) {
    if (a.contains(':')) continue;  // interfaces are folded into their device below
    QString b = "/sys/bus/usb/devices/" + a;
    if (!fs::exists(b + "/idVendor")) continue;
    quint16 v = quint16(fs::readText(b + "/idVendor").toUInt(nullptr, 16));
    quint16 p = quint16(fs::readText(b + "/idProduct").toUInt(nullptr, 16));
    Device dev;
    dev.bus = "USB";
    dev.address = a;
    dev.vendor = fs::readText(b + "/manufacturer");
    if (dev.vendor.isEmpty()) dev.vendor = hwids::UsbVendor(v);
    dev.name = fs::readText(b + "/product");
    if (dev.name.isEmpty()) dev.name = hwids::UsbProduct(v, p);
    if (dev.name.isEmpty()) dev.name = QString("%1:%2").arg(v, 4, 16, QChar('0')).arg(p, 4, 16, QChar('0'));
    dev.ids = QString("%1:%2").arg(v, 4, 16, QChar('0')).arg(p, 4, 16, QChar('0'));
    dev.driver = LinkName(b + "/driver");
    int devClass = fs::readText(b + "/bDeviceClass").toInt(nullptr, 16);
    QStringList kinds;
    // Interfaces are "<device>:<config>.<n>"; a root hub "usbN" names them "N-0:…".
    QString ifacePrefix = a.startsWith("usb") ? a.mid(3) + "-0:" : a + ':';
    for (const QString &i : fs::list(b)) {
      if (!i.startsWith(ifacePrefix)) continue;
      QString ib = b + '/' + i;
      QString drv = LinkName(ib + "/driver");
      if (!drv.isEmpty() && !dev.interfaceDrivers.contains(drv)) dev.interfaceDrivers << drv;
      QString k = UsbClass(fs::readText(ib + "/bInterfaceClass").toInt(nullptr, 16));
      if (!kinds.contains(k)) kinds << k;
      if (dev.modalias.isEmpty()) dev.modalias = fs::readText(ib + "/modalias");
      if (dev.module.isEmpty()) dev.module = LinkName(ib + "/driver/module");
    }
    dev.kind = devClass != 0 ? UsbClass(devClass) : kinds.join(", ");
    out << dev;
  }
  std::sort(out.begin(), out.end(), [](const Device &x, const Device &y) {
    return std::tie(x.bus, x.address) < std::tie(y.bus, y.address);
  });
  return out;
}

}  // namespace td::inventory
