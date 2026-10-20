#pragma once
class QWidget;
namespace td::ui {
// Preserves the source view/model/actions while presenting narrow rows as cards.
void InstallResponsiveTables(QWidget* page);
}  // namespace td::ui
