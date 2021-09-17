#ifndef H_udo_UDORuntime
#define H_udo_UDORuntime
//---------------------------------------------------------------------------
#include <string_view>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
namespace udo {
/// The clang++ binary that is used by default for C++ UDOs
extern const std::string_view cxxUDODefaultClangxx;
/// The string that contains the header for C++ UDOs that they include as
/// <udo/UDOperator.hpp>
extern const std::string_view cxxUDOHeader;
}
#endif
