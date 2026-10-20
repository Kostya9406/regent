#include "core/HwIds.h"

#include "core/Fs.h"

#include <QHash>

namespace td::hwids {

namespace {

struct Db {
  QHash<quint32, QString> vendors;   // vendor
  QHash<quint32, QString> devices;   // vendor << 16 | device
  QHash<quint32, QString> classes;   // class << 8 | subclass, and class << 8 | 0xff for the bare class
};

int Hex(const std::string &s, size_t pos, int digits) {
  if (pos + size_t(digits) > s.size()) return -1;
  int v = 0;
  for (int i = 0; i < digits; ++i) {
    char c = s[pos + size_t(i)];
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return -1;
    v = v * 16 + d;
  }
  return v;
}

// Both files share one layout: "vvvv  Name", "\tdddd  Name", "\t\tsub…";
// pci.ids adds "C cc  Class" / "\tss  Subclass" sections.
Db Load(const char *path) {
  Db db;
  std::string t = fs::readRaw(path, 8 << 20);
  int vendor = -1, cls = -1;
  size_t pos = 0;
  while (pos < t.size()) {
    size_t nl = t.find('\n', pos);
    if (nl == std::string::npos) nl = t.size();
    std::string l = t.substr(pos, nl - pos);
    pos = nl + 1;
    if (l.empty() || l[0] == '#') continue;
    if (l[0] == 'C' && l.size() > 6 && l[1] == ' ') {
      cls = Hex(l, 2, 2);
      vendor = -1;
      if (cls >= 0) db.classes.insert(quint32(cls) << 8 | 0xff, QString::fromStdString(l.substr(6)));
      continue;
    }
    if (l[0] != '\t') {
      int v = Hex(l, 0, 4);
      if (v < 0 || l.size() < 6) {  // a section we do not use (e.g. usb.ids "HID", "AT")
        vendor = cls = -1;
        continue;
      }
      vendor = v;
      cls = -1;
      db.vendors.insert(quint32(v), QString::fromStdString(l.substr(6)));
      continue;
    }
    if (l.size() > 1 && l[1] == '\t') continue;  // subsystem lines
    if (cls >= 0) {
      int sub = Hex(l, 1, 2);
      if (sub >= 0 && l.size() > 5) db.classes.insert(quint32(cls) << 8 | quint32(sub), QString::fromStdString(l.substr(5)));
    } else if (vendor >= 0) {
      int d = Hex(l, 1, 4);
      if (d >= 0 && l.size() > 7) db.devices.insert(quint32(vendor) << 16 | quint32(d), QString::fromStdString(l.substr(7)));
    }
  }
  return db;
}

const Db &Pci() {
  static const Db db = Load("/usr/share/hwdata/pci.ids");
  return db;
}
const Db &Usb() {
  static const Db db = Load("/usr/share/hwdata/usb.ids");
  return db;
}

}  // namespace

QString PciVendor(quint16 v) { return Pci().vendors.value(v); }
QString PciDevice(quint16 v, quint16 d) { return Pci().devices.value(quint32(v) << 16 | d); }
QString PciClass(quint8 c, quint8 s) {
  QString name = Pci().classes.value(quint32(c) << 8 | s);
  return name.isEmpty() ? Pci().classes.value(quint32(c) << 8 | 0xff) : name;
}
QString UsbVendor(quint16 v) { return Usb().vendors.value(v); }
QString UsbProduct(quint16 v, quint16 p) { return Usb().devices.value(quint32(v) << 16 | p); }

}  // namespace td::hwids
