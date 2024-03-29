#ifndef H_udo_i18n
#define H_udo_i18n
//---------------------------------------------------------------------------
#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include "udo/StringLiteral.hpp"
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2016 Thomas Neumann
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
namespace i18n {
//---------------------------------------------------------------------------
/// The main translation logic
StringLiteral translate(std::string_view context, StringLiteral text);
//---------------------------------------------------------------------------
/// A formating argument
struct Arg {
   /// The format function
   void (*formater)(std::string& out, const char* format, unsigned formatLen, const void* value);
   /// The value
   const void* value;
};
//---------------------------------------------------------------------------
/// Formating dispatch
template <class T>
struct Format {};
//---------------------------------------------------------------------------
template <>
struct Format<const char*> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <>
struct Format<char*> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <>
struct Format<std::string> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <>
struct Format<std::string_view> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <std::size_t N>
struct Format<char[N]> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value) {
      static_assert(N > 0);
      std::string_view sv(static_cast<const char*>(value), N - 1);
      Format<std::string_view>::format(out, format, formatLen, &sv);
   }
};
template <>
struct Format<StringLiteral> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <>
struct Format<bool> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <>
struct Format<char> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <>
struct Format<int8_t> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <>
struct Format<uint8_t> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <>
struct Format<int16_t> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <>
struct Format<uint16_t> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <>
struct Format<int32_t> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <>
struct Format<uint32_t> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <>
struct Format<int64_t> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
template <>
struct Format<uint64_t> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
// clang-format off
// Aliases for (unsigned) long / long long
template <class T> requires(std::signed_integral<T> && sizeof(T) == 8)
struct Format<T> : Format<int64_t> {};
template <class T> requires(std::unsigned_integral<T> && sizeof(T) == 8)
struct Format<T> : Format<uint64_t> {};
// clang-format on
template <>
struct Format<double> {
   static void format(std::string& out, const char* format, unsigned formatLen, const void* value);
};
//---------------------------------------------------------------------------
/// Format a string
std::string format(StringLiteral text, const Arg* args, unsigned argCount);
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
/// Translate a string
inline StringLiteral tr(std::string_view context, StringLiteral text) { return i18n::translate(context, text); }
//---------------------------------------------------------------------------
/// Do not translate, but just tag the text for translation. The real translation happens in trdynamic
constexpr inline StringLiteral trstatic(std::string_view /*context*/, StringLiteral text) { return text; } // unreachable at runtime, happens at compile time
/// Translate a string, but hide it from the extractor utility. The text argument must have been tagged with trstatic before
inline StringLiteral trdynamic(std::string_view context, StringLiteral text) { return i18n::translate(context, text); }
//---------------------------------------------------------------------------
/// Translate the message and format the arguments interpreting the patterns
template <typename... Args>
std::string trformat(std::string_view context, StringLiteral text, const Args&... args) {
   i18n::Arg a[sizeof...(Args)] = {{&i18n::Format<Args>::format, &args}...};
   return i18n::format(i18n::translate(context, text), a, sizeof...(Args));
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
