#include "udo/UDOMemoryManager.hpp"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <limits>
#include <sys/mman.h>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
using namespace std;
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
template <typename T>
static constexpr T log2(T value)
// Compute log2, rounding down. Undefined for 0
{
   assert(value > 0);
   return static_cast<T>(sizeof(T) * CHAR_BIT - countl_zero(value) - 1);
}
//---------------------------------------------------------------------------
template <typename T>
static constexpr T log2_ceil(T value)
// Compute log2, rounding up. Undefined for a <= 1
{
   assert(value > 1);
   return static_cast<T>(sizeof(T) * CHAR_BIT - countl_zero(value - 1));
}
//---------------------------------------------------------------------------
UDOMemoryManager::~UDOMemoryManager()
// Destructor
{
   if (systemAllocatedMemory)
      ::munmap(systemAllocatedMemory, memorySize);
}
//---------------------------------------------------------------------------
byte* UDOMemoryManager::allocatePages(uint64_t numPages)
// Allocate new pages from the operating system
{
   if (!systemAllocatedMemory) {
      auto* result = ::mmap(nullptr, memorySize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (result == MAP_FAILED)
         return nullptr;
      systemAllocatedMemory = static_cast<byte*>(result);
      systemMemoryBegin = systemAllocatedMemory;
   }
   auto* newBegin = systemMemoryBegin + numPages * pageSize;
   if (newBegin > systemAllocatedMemory + memorySize)
      return nullptr;

   auto* ptr = systemMemoryBegin;
   systemMemoryBegin = newBegin;
   return ptr;
}
//---------------------------------------------------------------------------
byte* UDOMemoryManager::allocate(uint64_t size, unsigned alignment, AllocationType allocationType)
// Allocate
{
   if (size == 0)
      return nullptr;

   if (alignment == 0)
      alignment = 1;
   else if (alignment > pageSize)
      // We can't align by more than the page size, so limit it to that
      alignment = pageSize;

   assert(popcount(alignment) == 1);

   auto alignmentLog2 = log2(alignment);
   auto sizeLog2 = size <= 1 ? 0 : log2_ceil(size);
   if (sizeLog2 < smallestAllocationLog2)
      sizeLog2 = smallestAllocationLog2;

   // Ensure that the allocation is a multiple of the alignment
   uint64_t allocationSize = 1ull << max<uint64_t>(alignmentLog2, sizeLog2);

   auto updateFreeLists = [](array<CurrentPage, numSizeClasses>& sizeClasses, byte* unusedMemoryEnd, size_t size) {
      if (size < (1ull << smallestAllocationLog2))
         return;

      for (unsigned i = 0; i < numSizeClasses; ++i) {
         auto sizeClass = 1ull << (smallestAllocationLog2 + numSizeClasses - i - 1);
         if (sizeClass > size)
            continue;

         auto& page = sizeClasses[numSizeClasses - i - 1];
         // Prepend the unused memory of this size class to the free list by
         // first writing the current pointer to it and then updating the
         // current pointer.
         auto* newHead = unusedMemoryEnd - sizeClass;
         memcpy(newHead, &page.freeList, sizeof(byte*));
         page.freeList = newHead;

         size -= sizeClass;
         unusedMemoryEnd -= sizeClass;
      }
   };

   if (allocationSize >= pageSize) {
      // Allocate the pages directly, don't try to reuse any memory. We don't
      // check the alignment here as the returned pointer will be page aligned
      // anyway. We make sure that the allocation is a multiple of the page size.
      auto numPages = ((size - 1) / pageSize + 1);
      allocationSize = numPages * pageSize;

      auto* ptr = allocatePages(numPages);
      if (!ptr)
         return nullptr;

      allocatedMemory.push_back({ptr, allocationSize, allocationType});

      array<CurrentPage, numSizeClasses>* sizeClasses = nullptr;
      switch (allocationType) {
         case AllocationType::Data:
            sizeClasses = &dataPageClasses;
            break;
         case AllocationType::ROData:
            sizeClasses = &roDataPageClasses;
            break;
         case AllocationType::Code:
            sizeClasses = &codePageClasses;
            break;
      }

      updateFreeLists(*sizeClasses, ptr + allocationSize, allocationSize - size);

      return ptr;
   } else {
      assert(sizeLog2 - smallestAllocationLog2 < numSizeClasses);

      array<CurrentPage, numSizeClasses>* sizeClasses = nullptr;
      switch (allocationType) {
         case AllocationType::Data:
            sizeClasses = &dataPageClasses;
            break;
         case AllocationType::ROData:
            sizeClasses = &roDataPageClasses;
            break;
         case AllocationType::Code:
            sizeClasses = &codePageClasses;
            break;
      }
      auto& page = (*sizeClasses)[sizeLog2 - smallestAllocationLog2];

      if (page.freeList) {
         // Get the next pointer of the free list before returning this memory
         byte* next = nullptr;
         memcpy(&next, page.freeList, sizeof(byte*));

         auto* ptr = page.freeList;
         assert(reinterpret_cast<uintptr_t>(ptr) % alignment == 0);

         page.freeList = next;

         updateFreeLists(*sizeClasses, ptr + allocationSize, allocationSize - size);

         return ptr;
      }

      if (page.begin == page.end) {
         auto* pagePtr = allocatePages(1);
         if (!pagePtr)
            return nullptr;

         allocatedMemory.push_back({pagePtr, pageSize, allocationType});

         page.begin = pagePtr;
         page.end = pagePtr + pageSize;
      }

      assert(page.begin + allocationSize <= page.end);

      auto* ptr = page.begin;
      assert(reinterpret_cast<uintptr_t>(ptr) % alignment == 0);
      page.begin += allocationSize;

      updateFreeLists(*sizeClasses, ptr + allocationSize, allocationSize - size);

      return ptr;
   }
}
//---------------------------------------------------------------------------
bool UDOMemoryManager::freeze()
// "Freeze" the state of the memory manager: The correct page permissions
// will be applied and all rw-pages are saved so that their contents can be
// restored by calling initialize().
{
   size_t totalFrozenSize = 0;

   for (auto& mem : allocatedMemory) {
      switch (mem.type) {
         case AllocationType::Data:
            totalFrozenSize += mem.size;
            break;
         case AllocationType::ROData: {
            auto result = ::mprotect(mem.ptr, mem.size, PROT_READ);
            if (result < 0)
               return false;
            break;
         }
         case AllocationType::Code: {
            auto result = ::mprotect(mem.ptr, mem.size, PROT_READ | PROT_EXEC);
            if (result < 0)
               return false;
            break;
         }
      }
   }

   // Doing this instead of using make_unique does not zero the memory. This is
   // unnecessary as we overwrite the memory below anyway.
   frozenData = unique_ptr<byte[]>(new byte[totalFrozenSize]);

   size_t offset = 0;
   for (auto& mem : allocatedMemory) {
      if (mem.type == AllocationType::Data) {
         memcpy(frozenData.get() + offset, mem.ptr, mem.size);
         offset += mem.size;
      }
   }

   isClean = true;

   return true;
}
//---------------------------------------------------------------------------
void UDOMemoryManager::initialize() const
// Initialize the rw-pages to the state when freeze() was called.
{
   // The first time initialize() is used, the memory is considered clean, so
   // in that case we don't need to do anything. Just remember that
   // initialize() was called once by setting isClean to false.
   if (isClean) {
      isClean = false;
      return;
   }

   // freeze() must be called before initialize()
   assert(frozenData);

   size_t offset = 0;
   for (auto& mem : allocatedMemory) {
      if (mem.type == AllocationType::Data) {
         memcpy(mem.ptr, frozenData.get() + offset, mem.size);
         offset += mem.size;
      }
   }
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
