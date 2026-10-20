#pragma once

#include <Qt>

namespace td::ui {

// The number behind a list cell's text ("460.6 MB" -> 482967552, "2.3%" -> 2.3):
// the card and row views draw their bars from it instead of reading the text back.
constexpr int kValueRole = Qt::UserRole + 64;

}  // namespace td::ui
