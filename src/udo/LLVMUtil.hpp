#ifndef H_udo_LLVMUtil
#define H_udo_LLVMUtil
//---------------------------------------------------------------------------
#include <llvm/ADT/StringRef.h>
#include <string_view>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
inline llvm::StringRef asStringRef(std::string_view str)
// Convert a string_view to a llvm::StringRef
{
   return llvm::StringRef(str.data(), str.size());
}
//---------------------------------------------------------------------------
inline std::string_view asStringView(llvm::StringRef str)
// Convert a llvm::StringRef to a string_view
{
   return std::string_view(str.data(), str.size());
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
