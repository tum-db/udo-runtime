#ifndef H_udo_ScopeGuard
#define H_udo_ScopeGuard
//---------------------------------------------------------------------------
#include <optional>
#include <type_traits>
#include <utility>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2019 Philipp Fent
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
/// A dismissable ScopeGuard, executing a function on '}', if not previously disarmed
/// @see std::lock_guard
template <typename Function, typename = std::is_nothrow_invocable<Function>>
class ScopeGuard {
   private:
   /// The function executed when the scope ends
   std::optional<Function> onExit;

   public:
   /// Constructor
   template <typename FunctionArg>
   explicit ScopeGuard(FunctionArg&& f) noexcept : onExit(std::forward<FunctionArg>(f)) {}
   /// Destructor
   ~ScopeGuard() { reset(); }
   /// Explicitly end the scope by calling the function
   void reset() noexcept {
      if (onExit) {
         std::move(onExit).value()();
         onExit.reset();
      }
   }
   /// Dismiss execution of the function
   void dismiss() noexcept { onExit.reset(); }
};
//---------------------------------------------------------------------------
/// Deduction guide for the forwarding constructor
template <typename FunctionArg>
ScopeGuard(FunctionArg&&) -> ScopeGuard<std::remove_reference_t<FunctionArg>>;
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
