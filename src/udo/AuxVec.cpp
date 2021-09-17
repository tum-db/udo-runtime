#include "udo/AuxVec.hpp"
#include <cassert>
#include <cstring>
#include <memory>
#include <sys/auxv.h>
#include <sys/random.h>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
using namespace std;
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
/// The number of auxv entries that getAuxVec will write (including the last null entry)
static constexpr size_t numAuxvEntries = 7;
//---------------------------------------------------------------------------
/// The size of an auxv entry
static constexpr size_t auxvEntrySize = 2 * sizeof(uint64_t);
//---------------------------------------------------------------------------
/// The number of random bytes generated for the random auxv entry
static constexpr size_t auxvNumRandomBytes = 16;
//---------------------------------------------------------------------------
size_t AuxVec::getAuxVecSize()
// Get the size of the aux vector that will be written by getAuxVec()
{
   return numAuxvEntries * auxvEntrySize + auxvNumRandomBytes;
}
//---------------------------------------------------------------------------
void AuxVec::getAuxVec(span<std::byte> auxVecOut)
// Get an aux vector that can be used for a new executable
{
   assert(auxVecOut.size() >= getAuxVecSize());

   bool isAligned = reinterpret_cast<uint64_t>(auxVecOut.data()) % alignof(uint64_t) == 0;

   uint64_t* auxIt;
   unique_ptr<char[]> auxTmp;

   if (isAligned) {
      auxIt = reinterpret_cast<uint64_t*>(auxVecOut.data());
   } else {
      auxTmp = make_unique<char[]>(getAuxVecSize());
      auxIt = reinterpret_cast<uint64_t*>(auxTmp.get());
   }

   byte* auxRandom = reinterpret_cast<byte*>(auxIt) + numAuxvEntries * auxvEntrySize;
   bool haveRandomBytes = ::getrandom(auxRandom, auxvNumRandomBytes, 0) == auxvNumRandomBytes;

   *(auxIt++) = AT_CLKTCK;
   *(auxIt++) = ::getauxval(AT_CLKTCK);
   *(auxIt++) = AT_HWCAP;
   *(auxIt++) = ::getauxval(AT_HWCAP);
   *(auxIt++) = AT_HWCAP2;
   *(auxIt++) = ::getauxval(AT_HWCAP2);
   *(auxIt++) = AT_PAGESZ;
   *(auxIt++) = ::getauxval(AT_PAGESZ);
   *(auxIt++) = AT_PLATFORM;
   *(auxIt++) = ::getauxval(AT_PLATFORM);
   if (haveRandomBytes) {
      *(auxIt++) = AT_RANDOM;
      *(auxIt++) = reinterpret_cast<uint64_t>(auxRandom);
   }
   *(auxIt++) = AT_NULL;
   *(auxIt++) = 0;

   assert(reinterpret_cast<byte*>(auxIt) <= auxRandom);

   if (!isAligned) {
      memcpy(auxVecOut.data(), auxTmp.get(), getAuxVecSize());
   }
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
