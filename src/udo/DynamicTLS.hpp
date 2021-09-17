#ifndef H_udo_DynamicTLS
#define H_udo_DynamicTLS
//---------------------------------------------------------------------------
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
/// An allocator that can dynamically allocate and access thread-local storage.
/// It must be initialized with a pre-allocated TLS block.
class DynamicTLS {
   public:
   /// An allocated TLS section
   struct AllocatedTLSSection {
      /// The absolute TLS offset
      int64_t tlsOffset;
      /// The offset into the pre-allocated TLS storage
      uint64_t storageOffset;
      /// The size of the section
      uint64_t size;
      /// The initialization image
      std::unique_ptr<char[]> initializationImage;
   };

   private:
   /// The base TLS offset of the pre-allocated TLS block
   int64_t tlsBlockOffset;
   /// The size of the TLS block
   uint64_t tlsBlockSize;
   /// The bitmap of free regions. Has one bit for every 8 bytes in the TLS storage.
   std::vector<uint64_t> freeBitmap;
   /// The allocated TLS sections
   std::vector<AllocatedTLSSection> allocatedTLSSections;

   /// Find first free region in the free map of the given size, claim it, and
   /// return the offset
   uint64_t findFree(uint64_t size);
   /// Free a region
   void setFree(uint64_t offset, uint64_t size);

   public:
   /// Constructor
   DynamicTLS(int64_t tlsBlockOffset, uint64_t tlsBlockSize);
   /// Destructor
   ~DynamicTLS();

   /// Get the allocated TLS sections
   std::span<const AllocatedTLSSection> getAllocatedTLSSections() const {
      return allocatedTLSSections;
   }

   /// Allocate a TLS section with the given size and alignment. Returns
   /// nullptr if memory couldn't be allocated.
   const AllocatedTLSSection* allocate(uint64_t size, unsigned alignment);

   /// Access the TLS storage for the current thread at the given offset
   void* accessTLS(uint64_t offset) const;
   /// Initialize the TLS of the current thread by writing the data from the
   /// initialization images
   void initializeTLS() const;
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
