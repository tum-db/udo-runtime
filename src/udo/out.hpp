#ifndef H_udo_out
#define H_udo_out
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2017 Thomas Neumann
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
/// Wrapper for out parameters in function calls
template <class T>
class out {
   private:
   /// The result
   T& result;

   public:
   /// Constructor
   explicit constexpr out(T& result) noexcept : result(result) {}

   /// Assignment
   constexpr out& operator=(T v) noexcept {
      result = static_cast<T&&>(v);
      return *this;
   }
   /// Access the value
   constexpr T& value() noexcept { return result; }
   /// Access the value
   constexpr const T& operator*() const noexcept { return result; }
   /// Access the value
   constexpr T* operator->() noexcept { return &result; }
};
//---------------------------------------------------------------------------
/// Wrapper for optional out parameters in function calls
template <class T>
class optional_out {
   private:
   /// The pointer type
   using Ptr = T*;

   /// The result
   const Ptr result;

   public:
   /// Constructor
   constexpr optional_out() noexcept : result(nullptr) {}
   /// Constructor
   constexpr optional_out(out<T> result) noexcept : result(&result.value()) {}
   /// Constructor
   constexpr explicit optional_out(T* result) noexcept : result(result) {}

   /// Assignment
   constexpr optional_out& operator=(T v) noexcept {
      *result = static_cast<T&&>(v);
      return *this;
   }
   /// Conditional assignment, report a value of requested
   constexpr void report(T v) noexcept {
      if (result) *result = static_cast<T&&>(v);
   }

   /// Has a value?
   constexpr bool has_value() const noexcept { return result; }
   /// Access the value
   constexpr T& value() noexcept { return *result; }
   /// Access the value
   constexpr const T& operator*() const noexcept { return *result; }
};
//---------------------------------------------------------------------------
/// Wrapper for inout parameters in function calls
template <class T>
class inout {
   private:
   /// The value
   T& v;

   public:
   /// Constructor
   explicit constexpr inout(T& value) noexcept : v(value) {}
   /// Constructor
   explicit constexpr inout(out<T> value) noexcept : v(value.value()) {}

   /// Assignment
   constexpr inout& operator=(T newV) noexcept {
      v = static_cast<T&&>(newV);
      return *this;
   }
   /// Access the value
   constexpr T& value() noexcept { return v; }
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
