#ifndef H_udo_UDOMemoryManager
#define H_udo_UDOMemoryManager
//---------------------------------------------------------------------------
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
/// A memory manager used by UDOs to load external libraries such as libc or
/// libstdc++ for C++ UDOs
class UDOMemoryManager {
   public:
   /// The types of allocations
   enum class AllocationType {
      Data,
      ROData,
      Code
   };

   private:
   /// An allocated memory region
   struct AllocatedMemory {
      /// The pointer to the region
      std::byte* ptr;
      /// The size of the region
      uint64_t size;
      /// The type of this memory
      AllocationType type;
   };

   /// The current non-full page for a size class
   struct CurrentPage {
      /// The pointer to the beginning of the free list
      std::byte* freeList;
      /// The pointer to the first unused byte of the page
      std::byte* begin;
      /// The pointer to the first byte beyond the page
      std::byte* end;
   };

   // The code from the object files that we load for the libraries that UDOs
   // depend on expect to lie within 2 GiB of eatch other. This is because it
   // is usually compiled with -mcmodel=small which is the default for x86_64.
   // To ensure this, we just allocate a large 2 GiB region and rely on the
   // kernel to not actually allocate the pages until they are actually used.
   static constexpr uint64_t memorySize = 2 * (1ull << 30);
   /// The log 2 of the page size
   static constexpr uint64_t pageSizeLog2 = 12;
   /// The page size
   static constexpr uint64_t pageSize = 1ull << pageSizeLog2;
   /// The log2 of the smallest allocation size. It must be at least 8B so that
   /// a pointer for the free list can be embedded into any allocation.
   static constexpr uint64_t smallestAllocationLog2 = 4;
   /// The number of size classes. All allocations larger than pageSize are
   /// handled separately.
   static constexpr uint64_t numSizeClasses = pageSizeLog2 - smallestAllocationLog2;

   /// The actual memory allocated from the operating system
   std::byte* systemAllocatedMemory = nullptr;
   /// The start of the unused system memory
   std::byte* systemMemoryBegin = nullptr;
   /// All allocated pages
   std::vector<AllocatedMemory> allocatedMemory;
   /// The current data pages for all size classes
   std::array<CurrentPage, numSizeClasses> dataPageClasses = {};
   /// The current read-only data pages for all size classes
   std::array<CurrentPage, numSizeClasses> roDataPageClasses = {};
   /// The current code pages for all size classes
   std::array<CurrentPage, numSizeClasses> codePageClasses = {};

   /// The frozen data generated by calling freeze()
   std::unique_ptr<std::byte[]> frozenData;
   /// Is the memory in the initial, clean state?
   mutable bool isClean = false;

   /// Allocate new pages from the operating system
   std::byte* allocatePages(uint64_t numPages);

   public:
   /// Constructor
   UDOMemoryManager() = default;
   /// Destructor
   ~UDOMemoryManager();

   /// Allocate
   std::byte* allocate(uint64_t size, unsigned alignment, AllocationType allocationType);
   /// "Freeze" the state of the memory manager: The correct page permissions
   /// will be applied and all rw-pages are saved so that their contents can be
   /// restored by calling initialize(). Returns false when an error occurred.
   bool freeze();
   /// Initialize the rw-pages to the state when freeze() was called.
   void initialize() const;
};
//---------------------------------------------------------------------------
}
#endif
