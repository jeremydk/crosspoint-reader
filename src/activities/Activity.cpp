#include "Activity.h"

#include <esp_heap_caps.h>

#include "ActivityManager.h"

void Activity::onEnter() {
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
#ifdef ENABLE_SERIAL_LOG
  // Canonical activity-transition signal for the host test harness.
  LOG_INF("STATE", "activity=%s", name.c_str());
  // Per-activity heap signal; pair with the matching exit line for the
  // delta. Drift across many enter/exit cycles surfaces leaks without
  // an explicit CMD:HEAPDUMP.
  LOG_INF("HEAP_AT", "enter=%s free=%u largest=%u min_free=%u", name.c_str(),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
          (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
#endif
}

void Activity::onExit() {
  LOG_DBG("ACT", "Exiting activity: %s", name.c_str());
#ifdef ENABLE_SERIAL_LOG
  LOG_INF("HEAP_AT", "exit=%s free=%u largest=%u", name.c_str(),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
#endif
}

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

void Activity::requestUpdateAndWait() { activityManager.requestUpdateAndWait(); }

void Activity::onGoHome() { activityManager.goHome(); }

void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
