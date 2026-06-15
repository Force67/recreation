#ifndef RECREATION_CORE_MOVE_ONLY_FUNCTION_H_
#define RECREATION_CORE_MOVE_ONLY_FUNCTION_H_

#include <version>

// rec::MoveOnlyFunction<Sig> is std::move_only_function where the standard
// library provides it (libstdc++), and a minimal stand-in where it does not
// yet (the NDK's libc++, as of LLVM 18). The stand-in matches the subset the
// engine uses: construct from a move-only callable, move (not copy), invoke.
#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L

#include <functional>

namespace rec {
template <typename Signature>
using MoveOnlyFunction = std::move_only_function<Signature>;
}  // namespace rec

#else

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace rec {

template <typename Signature>
class MoveOnlyFunction;

template <typename R, typename... Args>
class MoveOnlyFunction<R(Args...)> {
 public:
  MoveOnlyFunction() noexcept = default;
  MoveOnlyFunction(std::nullptr_t) noexcept {}

  template <typename F, typename DF = std::decay_t<F>,
            typename = std::enable_if_t<!std::is_same_v<DF, MoveOnlyFunction> &&
                                        std::is_invocable_r_v<R, DF&, Args...>>>
  MoveOnlyFunction(F&& f) : holder_(std::make_unique<Holder<DF>>(std::forward<F>(f))) {}

  MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
  MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;
  MoveOnlyFunction(const MoveOnlyFunction&) = delete;
  MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

  explicit operator bool() const noexcept { return holder_ != nullptr; }

  R operator()(Args... args) { return holder_->Call(std::forward<Args>(args)...); }

 private:
  struct HolderBase {
    virtual ~HolderBase() = default;
    virtual R Call(Args... args) = 0;
  };
  template <typename F>
  struct Holder final : HolderBase {
    template <typename G>
    explicit Holder(G&& g) : fn(std::forward<G>(g)) {}
    R Call(Args... args) override { return fn(std::forward<Args>(args)...); }
    F fn;
  };

  std::unique_ptr<HolderBase> holder_;
};

}  // namespace rec

#endif  // __cpp_lib_move_only_function

#endif  // RECREATION_CORE_MOVE_ONLY_FUNCTION_H_
