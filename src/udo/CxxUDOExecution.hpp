#ifndef H_udo_CxxUDOExecution
#define H_udo_CxxUDOExecution
//---------------------------------------------------------------------------
#include "thirdparty/tl/expected.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
/// A functor that is used as a callback in the UDO
template <typename Func>
struct CxxUDOFunctor {
   /// The function pointer
   Func* func;
   /// The state argument that is passed to the function as first argument
   void* stateArg;
};
//---------------------------------------------------------------------------
/// The functor addresses required for the precompiled C++ UDOs
struct CxxUDOFunctors {
   /// The functor for the produceOutputTuple callback
   CxxUDOFunctor<void(void*, void*)> produceOutputTupleFunctor;
   /// The functor for printDebug
   CxxUDOFunctor<void(void*, const char*, uint64_t)> printDebugFunctor;
   /// The functor for getRandom
   CxxUDOFunctor<uint64_t(void*)> getRandomFunctor;
};
//---------------------------------------------------------------------------
/// The allocation functions that will be used to link the C++ UDO
struct CxxUDOAllocationFuncs {
   std::add_pointer_t<void*(size_t)> malloc;
   std::add_pointer_t<void*(size_t, size_t)> calloc;
   std::add_pointer_t<void*(void*, size_t)> realloc;
   std::add_pointer_t<int(void**, size_t, size_t)> posixMemalign;
   std::add_pointer_t<void(void*)> free;
};
//---------------------------------------------------------------------------
/// The function pointers to a compiled and linked C++ UDO. Set to nullptr if a
/// function does not exist.
struct CxxUDOFunctions {
   /// The argument passed as a pointer to the global constructor
   struct GlobalConstructorArg {
      int argc;
      const char** argv;
   };

   /// The global constructor function pointer
   std::add_pointer_t<void(GlobalConstructorArg*)> globalConstructor;
   /// The global destructor function pointer
   std::add_pointer_t<void()> globalDestructor;
   /// The thread initialization function pointer
   std::add_pointer_t<void()> threadInit;
   /// The constructor function pointer
   std::add_pointer_t<void(void*, ...)> constructor;
   /// The destructor function pointer
   std::add_pointer_t<void(void*)> destructor;
   /// The consume function pointer
   std::add_pointer_t<void(void*, void*, void*, void*, void*)> consume;
   /// The extraWork function pointer
   std::add_pointer_t<uint32_t(void*, void*, uint32_t)> extraWork;
   /// The postProduce function pointer
   std::add_pointer_t<uint8_t(void*, void*, void*, void*)> postProduce;
};
//---------------------------------------------------------------------------
/// Link and execute a compiled C++ UDO
class CxxUDOExecution {
   private:
   struct Impl;

   /// The compiled UDO object file
   std::span<char> objectFile;
   /// The implementation
   std::unique_ptr<Impl> impl;

   public:
   /// Construct from a compiled UDO object file
   explicit CxxUDOExecution(std::span<char> objectFile);
   /// Destructor
   ~CxxUDOExecution();

   /// Link the object file
   tl::expected<void, std::string> link(CxxUDOAllocationFuncs allocationFuncs, int64_t tlsBlockOffset, uint64_t tlsBlockSize);
   /// Get the the functors. The may be modified for a new execution.
   CxxUDOFunctors& getFunctors() const;
   /// Initialize the memory and return the function pointers that are ready to
   /// be called.
   CxxUDOFunctions initialize();

   /// Create the constructor arguments that can be passed to libc. Return
   /// value is a pointer to a struct { int argc; char** argv; } that also
   /// contains extra data that libc needs, i.e. the environment and auxvals.
   static std::unique_ptr<std::byte[]> createLibcConstructorArg();
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
