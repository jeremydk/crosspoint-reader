#pragma once
#include <cstdint>

// Multi-consumer wrapper around the SDK's single-slot EInkDisplay::setIdleHook.
//
// The SDK exposes one global idle-hook slot that fires from inside the BUSY-poll
// of every e-ink refresh (~1 ms cadence, ~400-700 ms total per page turn). Only
// one consumer can register with the SDK directly. This dispatcher claims that
// slot once at first use and fans out to a fixed-size array of registered
// firmware-side workers, so multiple modules (chunked next-chapter prebuild,
// deferred SD writes, future book-open prebuild, ...) can each register their
// own idle work without coordinating through each other.
//
// Threading: hooks fire on whatever task drives the e-ink refresh -- in
// practice the render task. Registration is intended to happen from the same
// task (activity onEnter/onExit). No locking; callers are responsible for
// keeping their hook bodies fast (~<50-100 ms per call) so BUSY-line transition
// detection isn't delayed.
class IdleDispatcher {
 public:
  using Hook = void (*)(void* ctx);
  using Handle = uint8_t;
  static constexpr Handle INVALID_HANDLE = 0xFF;
  // Bumped if a future worker needs more slots. Sized to fit current consumers
  // plus a couple of spare so we don't have to revisit on every new worker.
  static constexpr uint8_t MAX_WORKERS = 4;

  // Global instance. Lazy-initializes on first call; registration with the SDK
  // hook happens in the constructor.
  static IdleDispatcher& instance();

  // Register a worker. Returns a handle for later removal, or INVALID_HANDLE
  // if all slots are taken (logged as ERR; caller's work won't run).
  Handle add(Hook hook, void* ctx);

  // Deregister. Safe to call with INVALID_HANDLE or a slot that was already
  // removed (idempotent).
  void remove(Handle handle);

 private:
  IdleDispatcher();

  struct Slot {
    Hook hook = nullptr;
    void* ctx = nullptr;
  };
  Slot slots_[MAX_WORKERS] = {};

  static void dispatchTrampoline(void* self);
  void dispatch();
};
