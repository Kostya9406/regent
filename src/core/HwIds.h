#pragma once

// Names from hwdata's pci.ids / usb.ids (/usr/share/hwdata), loaded once on
// first use. Thread-safe after construction; lookups never touch the disk.

#include <QString>

namespace td::hwids {

QString PciVendor(quint16 vendor);
QString PciDevice(quint16 vendor, quint16 device);
QString PciClass(quint8 cls, quint8 subclass);
QString UsbVendor(quint16 vendor);
QString UsbProduct(quint16 vendor, quint16 product);

}  // namespace td::hwids
