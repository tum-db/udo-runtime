#ifndef H_udo_StringLiteral
#define H_udo_StringLiteral
//---------------------------------------------------------------------------
#include <cassert>
#include <string_view>
//---------------------------------------------------------------------------
// Umbra
// (c) 2023 Philipp Fent
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
/// A string_view of a string literal. Guaranteed to be null terminated.
class StringLiteral : public std::string_view {
   public:
   /// Constructor
   constexpr StringLiteral() = default;
   /// Constructor
   template <size_t N>
   constexpr StringLiteral(const char (&literal)[N]) : std::string_view(literal, N - 1) {
      assert(literal[N - 1] == '\0');
   }
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
