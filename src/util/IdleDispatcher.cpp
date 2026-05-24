#include "IdleDispatcher.h"

#include <EInkDisplay.h>
#include <Logging.h>

IdleDispatcher& IdleDispatcher::instance() {
  static IdleDispatcher inst;
  return inst;
}

IdleDispatcher::IdleDispatcher() {
  EInkDisplay::setIdleHook(&IdleDispatcher::dispatchTrampoline, this);
}

IdleDispatcher::Handle IdleDispatcher::add(Hook hook, void* ctx) {
  for (uint8_t i = 0; i < MAX_WORKERS; ++i) {
    if (slots_[i].hook == nullptr) {
      slots_[i] = {hook, ctx};
      return i;
    }
  }
  LOG_ERR("IDL", "All %u idle dispatcher slots taken; cannot register more", MAX_WORKERS);
  return INVALID_HANDLE;
}

void IdleDispatcher::remove(Handle handle) {
  if (handle >= MAX_WORKERS) return;
  slots_[handle] = {};
}

void IdleDispatcher::dispatchTrampoline(void* self) {
  if (self) static_cast<IdleDispatcher*>(self)->dispatch();
}

void IdleDispatcher::dispatch() {
  // Fires every 1 ms during BUSY-poll. Workers are responsible for cheap
  // fast-exit when there's no work; this loop just walks the fixed slot array.
  for (uint8_t i = 0; i < MAX_WORKERS; ++i) {
    if (slots_[i].hook) slots_[i].hook(slots_[i].ctx);
  }
}
