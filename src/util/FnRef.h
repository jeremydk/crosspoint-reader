#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

// Non-owning function reference. Drop-in replacement for `const std::function<Sig>&`
// parameters when you don't need to store the callable past the call.
//
// std::function heap-allocates closures that exceed its small-buffer threshold
// (typically 16 bytes on libstdc++). Under -fno-exceptions on ESP32 that alloc
// can abort() on OOM, and even when it doesn't, the per-call churn fragments
// the heap (CLAUDE.md §"Template and std::function Bloat").
//
// FnRef is two pointers (thunk + ctx). It binds to any callable via implicit
// conversion at the call site — the callable lives on the caller's stack and
// FnRef carries its address. Lifetime: the underlying callable must outlive
// the FnRef. For typical "pass a lambda into a function" use, the lambda
// outlives the call by virtue of being a temporary in the full-expression.
//
// Example:
//
//   void draw(FnRef<int(int)> getX);
//   draw([this](int i) { return items[i].x; });  // no heap allocation
//
//   FnRef<int(int)> opt = nullptr;  // null reference; operator bool returns false
//
template <typename Sig>
class FnRef;

template <typename R, typename... Args>
class FnRef<R(Args...)> {
 public:
  FnRef() = default;
  FnRef(std::nullptr_t) noexcept {}

  template <typename F, typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, FnRef>>>
  FnRef(F&& f) noexcept
      : thunk_(&invoke<std::remove_reference_t<F>>), ctx_(const_cast<void*>(static_cast<const void*>(&f))) {}

  explicit operator bool() const noexcept { return thunk_ != nullptr; }

  R operator()(Args... args) const { return thunk_(ctx_, std::forward<Args>(args)...); }

 private:
  template <typename F>
  static R invoke(void* ctx, Args... args) {
    return (*static_cast<F*>(ctx))(std::forward<Args>(args)...);
  }

  R (*thunk_)(void*, Args...) = nullptr;
  void* ctx_ = nullptr;
};
