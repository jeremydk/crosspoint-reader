#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <PluginManifest.h>
#include <PluginRegistry.h>

#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"

UITheme UITheme::instance;

namespace {
// Classic theme is built into core so the firmware always has a fallback when
// no theme plugin is loaded (or when the user's saved selection points at a
// theme plugin that's been disabled).
const PluginThemeEntry* findThemeById(uint8_t id) {
  for (size_t i = 0; i < PluginRegistry::count(); ++i) {
    const PluginManifest* m = PluginRegistry::all()[i];
    if (!m) continue;
    for (uint8_t j = 0; j < m->themeCount; ++j) {
      if (m->themes[j].id == id) return &m->themes[j];
    }
  }
  return nullptr;
}
}  // namespace

std::vector<UITheme::RegisteredTheme> UITheme::getRegisteredThemes() {
  std::vector<RegisteredTheme> out;
  out.reserve(4);
  out.push_back({static_cast<uint8_t>(CrossPointSettings::UI_THEME::CLASSIC), StrId::STR_THEME_CLASSIC});
  for (size_t i = 0; i < PluginRegistry::count(); ++i) {
    const PluginManifest* m = PluginRegistry::all()[i];
    if (!m) continue;
    for (uint8_t j = 0; j < m->themeCount; ++j) {
      out.push_back({m->themes[j].id, m->themes[j].label});
    }
  }
  return out;
}

UITheme::UITheme() { setTheme(static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme)); }

void UITheme::reload() { setTheme(static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme)); }

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  const uint8_t id = static_cast<uint8_t>(type);
  if (id == CrossPointSettings::UI_THEME::CLASSIC) {
    LOG_DBG("UI", "Using Classic theme");
    currentTheme = std::make_unique<BaseTheme>();
    currentMetrics = &BaseMetrics::values;
    return;
  }
  if (const auto* entry = findThemeById(id); entry && entry->make && entry->metrics) {
    LOG_DBG("UI", "Using plugin theme id=%u", id);
    currentTheme = entry->make();
    currentMetrics = entry->metrics;
    return;
  }
  LOG_DBG("UI", "Theme id=%u unavailable; falling back to Classic", id);
  currentTheme = std::make_unique<BaseTheme>();
  currentMetrics = &BaseMetrics::values;
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return availableHeight / rowHeight;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  // Add status bar margin
  const bool showStatusBar = SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
                             SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
                             SETTINGS.statusBarBattery;
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showStatusBar ? (metrics.statusBarVerticalMargin) : 0) +
         (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}
