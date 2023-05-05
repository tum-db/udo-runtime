#ifndef H_udo_UDORuntime
#define H_udo_UDORuntime
//---------------------------------------------------------------------------
#include <span>
#include <string_view>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
/// The clang++ binary that is used by default for C++ UDOs
extern const std::string_view cxxUDODefaultClangxx;
//---------------------------------------------------------------------------
/// A C++ header file that can be used by C++ UDOs
struct CxxUDOHeader {
   /// The filename of the header
   std::string_view filename;
   /// The file content of the header
   std::string_view content;
};
//---------------------------------------------------------------------------
/// All headers that are available to C++ UDOs
extern const std::span<const CxxUDOHeader> cxxUDOHeaders;
//---------------------------------------------------------------------------
}
#endif
