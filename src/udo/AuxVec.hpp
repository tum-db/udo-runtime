#ifndef H_udo_AuxVec
#define H_udo_AuxVec
//---------------------------------------------------------------------------
#include <span>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
/// Utilities to read the auxvec values the Linux kernel passes to ELF
/// executables. See man getauxval.
struct AuxVec {
   /// Get the size of the aux vector that will be written by getAuxVec()
   static size_t getAuxVecSize();
   /// Get an aux vector that can be used for a new executable
   static void getAuxVec(std::span<std::byte> auxVecOut);
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
