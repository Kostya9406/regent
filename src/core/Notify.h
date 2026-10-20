#pragma once

// Desktop notifications for alerts. Omarchy's own sender when it is installed
// (its glyphs and click-to-run), otherwise org.freedesktop.Notifications.
// Only one Regent process per session notifies: whoever holds the lock in
// $XDG_RUNTIME_DIR, so the window and the bar's --stream never both speak.

#include "core/Alerts.h"

#include <QLockFile>
#include <memory>

namespace td::notify {

void Send(const alerts::Alert &alert);

class Speaker {
public:
  Speaker();
  bool Holds();  // tries again each call until it gets the lock
private:
  std::unique_ptr<QLockFile> lock_;
  bool held_ = false;
};

}  // namespace td::notify
