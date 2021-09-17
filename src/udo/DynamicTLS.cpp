#include "udo/DynamicTLS.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstring>
#include <tuple>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
using namespace std;
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
static uint64_t getAllocationSize(uint64_t size)
// Get the allocation size for a given size
{
   if (size > 16 && size <= 24)
      return 24;
   else
      // Use the next power of two so that the allocation is easier.
      return bit_ceil(size);
}
//---------------------------------------------------------------------------
uint64_t DynamicTLS::findFree(uint64_t size)
// Find first free region of the given size, claim it, and return the offset
{
retry:
   if (size == 8) {
      // Here we only need to look whether a single bit is unset
      for (unsigned bitmapIndex = 0; bitmapIndex < freeBitmap.size(); ++bitmapIndex) {
         auto bitmapEntryAtomic = atomic_ref(freeBitmap[bitmapIndex]);
         auto bitmapEntry = bitmapEntryAtomic.load();
         if (bitmapEntry != ~0ull) {
            // Find the first unset bit and claim it
            unsigned i = countr_one(bitmapEntry);
            auto oldEntry = bitmapEntryAtomic.fetch_or(1ull << i);
            if ((oldEntry & (1ull << i)) != 0)
               // The bit was set by another thread, so retry
               goto retry;
            return static_cast<uint64_t>(bitmapIndex * 64 + i) * 8;
         }
      }

      // No free areas found
      return ~0ull;
   }

   auto searchWithPositions = [&](span<const uint64_t> possiblePositions) -> tuple<bool, uint64_t> {
      for (unsigned bitmapIndex = 0; bitmapIndex < freeBitmap.size(); ++bitmapIndex) {
         auto bitmapEntryAtomic = atomic_ref(freeBitmap[bitmapIndex]);
         auto bitmapEntry = bitmapEntryAtomic.load();
         for (uint64_t position : possiblePositions) {
            if ((bitmapEntry & position) == 0) {
               auto oldByte = bitmapEntryAtomic.fetch_or(position);
               if ((oldByte & position) != 0) {
                  // Some or all of the bits we wanted to use were set by
                  // another thread. First, reset the remaining bits that
                  // were set by us and then retry.
                  bitmapEntryAtomic.fetch_xor(position ^ (oldByte & position));
                  return {false, 0};
               }
               auto bitOffset = countr_zero(position);
               return {true, static_cast<uint64_t>(bitmapIndex * 64 + bitOffset) * 8};
            }
         }
      }

      // No free areas found
      return {true, ~0ull};
   };

   // Here we hard-code the search for a few common sizes
   switch (getAllocationSize(size)) {
      case 16: {
         array<uint64_t, 32> positions;
         for (unsigned i = 0; i < 32; ++i)
            positions[i] = 0b11ull << (2 * i);
         auto [success, result] = searchWithPositions(positions);
         if (!success)
            goto retry;
         return result;
      }
      case 24: {
         array<uint64_t, 62> positions;
         for (unsigned i = 0; i < 62; ++i)
            positions[i] = 0b111ull << i;
         auto [success, result] = searchWithPositions(positions);
         if (!success)
            goto retry;
         return result;
      }
      case 32: {
         array<uint64_t, 16> positions;
         for (unsigned i = 0; i < 16; ++i)
            positions[i] = 0b1111ull << (4 * i);
         auto [success, result] = searchWithPositions(positions);
         if (!success)
            goto retry;
         return result;
      }
      case 64: {
         array<uint64_t, 8> positions;
         for (unsigned i = 0; i < 8; ++i)
            positions[i] = 0b1111'1111ull << (8 * i);
         auto [success, result] = searchWithPositions(positions);
         if (!success)
            goto retry;
         return result;
      }
      case 128: {
         array<uint64_t, 4> positions;
         for (unsigned i = 0; i < 4; ++i)
            positions[i] = 0b1111'1111'1111'1111ull << (16 * i);
         auto [success, result] = searchWithPositions(positions);
         if (!success)
            goto retry;
         return result;
      }
      case 256: {
         array<uint64_t, 2> positions{0xffff'ffffull, 0xffff'ffff'0000'0000ull};
         auto [success, result] = searchWithPositions(positions);
         if (!success)
            goto retry;
         return result;
      }
      case 512: {
         array<uint64_t, 1> positions{~0ull};
         auto [success, result] = searchWithPositions(positions);
         if (!success)
            goto retry;
         return result;
      }
   }

   // TODO: The size spans more than one entry in the bitmap so we need to use
   // multiple adjacent bitmap entries.
   return ~0ull;
}
//---------------------------------------------------------------------------
void DynamicTLS::setFree(uint64_t offset, uint64_t size)
// Free a region
{
   size = getAllocationSize(size);
   while (size > 0) {
      auto bitmapByteAtomic = atomic_ref(freeBitmap[offset / 8 / 64]);
      uint64_t bitOffset = (offset / 8) % 64;
      uint64_t numBits = min<uint64_t>(size / 8, 64 - bitOffset);
      uint64_t bitsMask = ((1ul << numBits) - 1) << bitOffset;

      auto oldValue = bitmapByteAtomic.fetch_xor(bitsMask);
      assert((oldValue & bitsMask) == bitsMask);
      static_cast<void>(oldValue);

      offset += numBits * 8;
      size -= numBits * 8;
   }
}
//---------------------------------------------------------------------------
DynamicTLS::DynamicTLS(int64_t tlsBlockOffset, uint64_t tlsBlockSize)
   : tlsBlockOffset(tlsBlockOffset), tlsBlockSize(tlsBlockSize)
// Constructor
{
   if (tlsBlockSize > 0)
      freeBitmap.resize(((tlsBlockSize - 1) / 8 + 1) / 64 + 1);
}
//---------------------------------------------------------------------------
DynamicTLS::~DynamicTLS()
// Destructor
{
}
//---------------------------------------------------------------------------
const DynamicTLS::AllocatedTLSSection* DynamicTLS::allocate(uint64_t size, unsigned alignment)
// Allocate a TLS section with the given size and alignment
{
   if (size == 0)
      return nullptr;

   if (size < 8)
      size = 8;
   if (alignment < 8)
      alignment = 8;
   // Ensure that the size is a multiple of the alignment
   size = (size + alignment - 1) & ((~alignment) + 1);

   uint64_t allocationOffset = findFree(size);
   if (allocationOffset == ~0ull)
      return nullptr;

   auto& allocatedSection = allocatedTLSSections.emplace_back();
   allocatedSection.tlsOffset = tlsBlockOffset + allocationOffset;
   allocatedSection.storageOffset = allocationOffset;
   allocatedSection.size = size;
   allocatedSection.initializationImage = make_unique<char[]>(size);

   return &allocatedSection;
}
//---------------------------------------------------------------------------
void* DynamicTLS::accessTLS(uint64_t offset) const
// Access the TLS storage for the current thread at the given offset
{
   byte* ptr;
#if defined(__x86_64__) && defined(__ELF__)
   asm("mov %%fs:0, %0"
       : "=r"(ptr));
#else
#error "Unsupported target for thread-local storage"
#endif
   return ptr + tlsBlockOffset + offset;
}
//---------------------------------------------------------------------------
void DynamicTLS::initializeTLS() const
// Initialize the TLS of the current thread by writing the data from the
// initialization images
{
   auto* basePtr = static_cast<byte*>(accessTLS(0));
   for (auto& section : allocatedTLSSections)
      memcpy(basePtr + section.storageOffset, section.initializationImage.get(), section.size);
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
