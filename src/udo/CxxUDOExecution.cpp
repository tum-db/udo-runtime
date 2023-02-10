#include "udo/CxxUDOExecution.hpp"
#include "udo/AuxVec.hpp"
#include "udo/ClangCompiler.hpp"
#include "udo/CxxUDOCompiler.hpp"
#include "udo/CxxUDOConfig.hpp"
#include "udo/DynamicTLS.hpp"
#include "udo/LLVMUtil.hpp"
#include "udo/Setting.hpp"
#include "udo/UDOMemoryManager.hpp"
#include "udo/i18n.hpp"
#include "udo/out.hpp"
#include <clang/Driver/Compilation.h>
#include <clang/Driver/ToolChain.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/RuntimeDyld.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
using namespace std;
//---------------------------------------------------------------------------
static const char tc[] = "udo/CxxUDOExecution";
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
static Setting<bool> debugCxxUDO("debugCxxUDO", "Print debug information for the compilation of C++ UDOs", false);
//---------------------------------------------------------------------------
namespace {
//---------------------------------------------------------------------------
/// The memory manager for C++ UDOs that can handle TLS allocations
class CxxUDOMemoryManager : public llvm::RuntimeDyld::MemoryManager {
   private:
   /// The memory manager
   UDOMemoryManager memoryManager;
   /// The TLS allocations
   DynamicTLS tlsAllocations;

   public:
   /// Constructor
   CxxUDOMemoryManager(int64_t tlsBlockOffset, uint64_t tlsBlockSize)
      : tlsAllocations(tlsBlockOffset, tlsBlockSize) {}

   /// Allocate space for a code section
   uint8_t* allocateCodeSection(uintptr_t size, unsigned alignment, unsigned sectionID, llvm::StringRef sectionName) override;
   /// Allocate space for a data section
   uint8_t* allocateDataSection(uintptr_t size, unsigned alignment, unsigned sectionID, llvm::StringRef sectionName, bool isReadOnly) override;
   /// Allocate a memory block of (at least) the given size to be used for
   /// thread-local storage (TLS).
   TLSSection allocateTLSSection(uintptr_t size, unsigned alignment, unsigned sectionID, llvm::StringRef sectionName) override;

   /// Register EH frames for C++ exceptions
   void registerEHFrames(uint8_t* /*addr*/, uint64_t /*loadAddr*/, size_t /*size*/) override {}
   /// Deregister the EH frames
   void deregisterEHFrames() override {}

   /// Finalize the memory by applying the correct permissions. Returns true if an error occurred.
   bool finalizeMemory(std::string* errMsg) override;

   /// Get the memory manager
   const UDOMemoryManager& getMemoryManager() const {
      return memoryManager;
   }
   /// Get the dynamic TLS allocations
   const DynamicTLS& getTLSAllocations() const {
      return tlsAllocations;
   }
};
//---------------------------------------------------------------------------
uint8_t* CxxUDOMemoryManager::allocateCodeSection(uintptr_t size, unsigned alignment, unsigned /*sectionID*/, llvm::StringRef /*sectionName*/)
// Allocate space for a code section
{
   return reinterpret_cast<uint8_t*>(memoryManager.allocate(size, alignment, UDOMemoryManager::AllocationType::Code));
}
//---------------------------------------------------------------------------
uint8_t* CxxUDOMemoryManager::allocateDataSection(uintptr_t size, unsigned alignment, unsigned /*sectionID*/, llvm::StringRef /*sectionName*/, bool isReadOnly)
// Allocate space for a data section
{
   return reinterpret_cast<uint8_t*>(memoryManager.allocate(size, alignment, isReadOnly ? UDOMemoryManager::AllocationType::ROData : UDOMemoryManager::AllocationType::Data));
}
//---------------------------------------------------------------------------
CxxUDOMemoryManager::TLSSection CxxUDOMemoryManager::allocateTLSSection(uintptr_t size, unsigned alignment, unsigned /*sectionID*/, llvm::StringRef /*sectionName*/)
// Allocate a memory block of (at least) the given size to be used for
// thread-local storage (TLS).
{
   auto* allocatedSection = tlsAllocations.allocate(size, alignment);
   if (!allocatedSection)
      return {};

   TLSSection section;
   section.InitializationImage = reinterpret_cast<uint8_t*>(allocatedSection->initializationImage.get());
   section.Offset = allocatedSection->tlsOffset;
   return section;
}
//---------------------------------------------------------------------------
bool CxxUDOMemoryManager::finalizeMemory(std::string* /*errMsg*/)
// Finalize the memory by applying the correct permissions. Returns true if an error occurred.
{
   return !memoryManager.freeze();
}
//---------------------------------------------------------------------------
/// The JIT symbol resolver for the precompiled C++ UDOs
class PrecompiledCxxUDOResolver : public llvm::JITSymbolResolver {
   private:
   /// An object file of a static library
   struct ObjectFile {
      /// The actual object file
      unique_ptr<llvm::object::ObjectFile> objectFile;
      /// Was the object already loaded into the linker?
      bool isLoaded = false;
   };

   /// A loaded static library
   struct StaticLibrary {
      /// The memory buffer of the file
      unique_ptr<llvm::MemoryBuffer> memoryBuffer;
      /// The archive representing the library
      unique_ptr<llvm::object::Archive> archive;
      /// The object files of the archive. Use a deque so that pointers to its
      /// elements will not be invalidated.
      deque<ObjectFile> objectFiles;
   };

   /// A symbol from an object file
   struct ObjectSymbol {
      /// The object file the symbol belongs to
      ObjectFile* objectFile = nullptr;
      /// The name of the symbol
      string_view name;
      /// The flags of the symbol
      llvm::JITSymbolFlags flags;
      /// Is this symbol undefined?
      bool undefined = false;
   };

   /// The linker
   llvm::RuntimeDyld& linker;
   /// The predefined, "external" symbols
   unordered_map<string_view, void*> predefinedSymbols;
   /// The loaded libraries
   unordered_map<string_view, StaticLibrary> staticLibs;
   /// The mapping from a symbol to its object file
   unordered_map<string_view, ObjectSymbol> symbolObjects;

   /// Try to load a symbol from the loaded libraries
   llvm::JITEvaluatedSymbol loadSymbol(ObjectSymbol& symbol);

   public:
   /// Constructor
   PrecompiledCxxUDOResolver(llvm::RuntimeDyld& linker, CxxUDOFunctors* functorStorage, CxxUDOAllocationFuncs allocationFuncs);

   /// Add a library
   tl::expected<void, string> addLibrary(string_view path);

   /// Lookup an individual symbol
   bool lookup(string_view name, optional_out<llvm::JITEvaluatedSymbol> symbol = {});

   /// Lookup the symbols and call the onResolved callback with the result
   void lookup(const LookupSet& symbols, OnResolvedFunction onResolved) override;

   /// Return which symbols out of the given ones should be looked up elsewhere
   /// by the caller
   llvm::Expected<LookupSet> getResponsibilitySet(const LookupSet& symbols) override;

   /// Specify if this resolver can return valid symbols with zero value. This
   /// is true for the PrecompiledCxxUDOResolver as it may return nullptr for
   /// undefined weak symbols.
   bool allowsZeroSymbols() override { return true; }
};
//---------------------------------------------------------------------------
namespace {
//---------------------------------------------------------------------------
extern "C" int udoDlFindObject(void* address, void* result)
// This is a stub for the dl_find_object function from glibc. It writes
// unwinding information for a given address to `result` and returns 0. On
// error it returns -1. This is used by the gcc unwinder but it can handle the
// case were this function returns -1, so we just do that.
{
   static_cast<void>(address);
   static_cast<void>(result);
   return -1;
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
PrecompiledCxxUDOResolver::PrecompiledCxxUDOResolver(llvm::RuntimeDyld& linker, CxxUDOFunctors* functorStorage, CxxUDOAllocationFuncs allocationFuncs)
   : linker(linker)
// Constructor
{
   // Our custom glibc enables its "Umbra mode" only if the symbol
   // __umbra_glibc is defined. Its value is irrelevant, as long as it is
   // defined and not 0. Use an invalid address here so that is not
   // accidentially used.
   predefinedSymbols.emplace("__umbra_glibc", reinterpret_cast<void*>((~0ull) << 8));

   // Register the malloc functions
   predefinedSymbols.emplace("malloc", reinterpret_cast<void*>(allocationFuncs.malloc));
   predefinedSymbols.emplace("calloc", reinterpret_cast<void*>(allocationFuncs.calloc));
   predefinedSymbols.emplace("realloc", reinterpret_cast<void*>(allocationFuncs.realloc));
   predefinedSymbols.emplace("posix_memalign", reinterpret_cast<void*>(allocationFuncs.posixMemalign));
   predefinedSymbols.emplace("free", reinterpret_cast<void*>(allocationFuncs.free));

   // Register the functor symbols
   predefinedSymbols.emplace(CxxUDOCompiler::emitFunctorName, &functorStorage->emitFunctor);
   predefinedSymbols.emplace(CxxUDOCompiler::printDebugFunctorName, &functorStorage->printDebugFunctor);
   predefinedSymbols.emplace(CxxUDOCompiler::getRandomFunctorName, &functorStorage->getRandomFunctor);

   // Define the dl_find_object symbol. See the udoDlFindObject function for
   // more information.
   predefinedSymbols.emplace("_dl_find_object", reinterpret_cast<void*>(&udoDlFindObject));
}
//---------------------------------------------------------------------------
tl::expected<void, string> PrecompiledCxxUDOResolver::addLibrary(string_view path)
// Add a library
{
   if (staticLibs.count(path))
      // We assume that the libraries never change once loaded, so skip loading
      // it if it already exists.
      return {};

   if (debugCxxUDO)
      llvm::errs() << "opening static library " << asStringRef(path) << '\n';

   unique_ptr<llvm::MemoryBuffer> libBuffer;
   {
      auto result = llvm::MemoryBuffer::getFile(string(path));
      if (!result)
         return tl::unexpected(trformat(tc, "couldn't open static library {0}: {1}", path, result.getError().message()));
      libBuffer = move(*result);
   }

   unique_ptr<llvm::object::Archive> libArchive;
   {
      auto result = llvm::object::Archive::create(*libBuffer);
      if (!result)
         return tl::unexpected(trformat(tc, "error while reading static library {0}", path));
      libArchive = move(*result);
   }

   deque<ObjectFile> objectFiles;
   llvm::Error error = llvm::Error::success();
   for (auto& child : libArchive->children(error)) {
      unique_ptr<llvm::object::ObjectFile> libObject;
      {
         auto result = child.getAsBinary();
         if (!result)
            return tl::unexpected(trformat(tc, "error while reading object from static library {0}", path));
         // Ignore archive contents that are not object files
         if (auto objectFilePtr = llvm::dyn_cast<llvm::object::ObjectFile>(result->get()); objectFilePtr) {
            result->release();
            libObject.reset(objectFilePtr);
         }
      }
      if (!libObject)
         continue;

      auto& objectFile = objectFiles.emplace_back();
      objectFile.objectFile = move(libObject);
      bool isElf = objectFile.objectFile->isELF();

      if (debugCxxUDO) {
         uint64_t tlsSize = 0;
         if (isElf) {
            for (auto& section : objectFile.objectFile->sections()) {
               llvm::object::ELFSectionRef elfSection(section);
               if (elfSection.getFlags() & llvm::ELF::SHF_TLS) {
                  tlsSize += elfSection.getSize();
               }
            }
         }
         if (tlsSize > 0)
            llvm::errs() << "Need " << tlsSize << "B of TLS storage for " << objectFile.objectFile->getFileName() << '\n';
      }

      for (auto& symbol : objectFile.objectFile->symbols()) {
         // If we can't determine the name of a symbol, then of course we can't
         // use it for linking
         auto name = [&] {
            auto result = symbol.getName();
            if (result)
               return asStringView(*result);
            else
               return string_view();
         }();
         if (name.empty())
            continue;

         bool isUndefined = false;
         if (auto section = symbol.getSection())
            if (*section == objectFile.objectFile->section_end())
               isUndefined = true;

         if (isElf) {
            llvm::object::ELFSymbolRef elfSymbol(symbol);
            switch (elfSymbol.getBinding()) {
               case llvm::ELF::STB_GLOBAL:
               case llvm::ELF::STB_WEAK:
               case llvm::ELF::STB_GNU_UNIQUE:
                  // STB_GNU_UNIQUE is a GNU extension for global symbols
                  break;
               default:
                  // Only global symbols (weak symbols are also considered
                  // global) should be used to link multiple object files
                  continue;
            }
            switch (elfSymbol.getELFType()) {
               case llvm::ELF::STT_NOTYPE: {
                  if (elfSymbol.getBinding() == llvm::ELF::STB_WEAK) {
                     // Remember undefined weak symbols as they should resolve
                     // to 0 if they are never defined.
                     isUndefined = true;
                     break;
                  } else {
                     continue;
                  }
               }
               case llvm::ELF::STT_FUNC:
               case llvm::ELF::STT_OBJECT:
                  // Regular global function or data symbol
               case llvm::ELF::STT_TLS:
                  // Thread-local symbol that CxxUDOMemoryManager can also handle
               case llvm::ELF::STT_GNU_IFUNC:
                  // IFUNC symbols are handled by RuntimeDyld

                  // Skip all other symbols if they are undefined
                  if (isUndefined)
                     continue;
                  break;
               default:
                  continue;
            }
         } else {
            // We are only interested in functions and data
            auto symbolType = symbol.getType();
            if (!symbolType)
               continue;
            switch (*symbolType) {
               case llvm::object::SymbolRef::ST_Data:
               case llvm::object::SymbolRef::ST_Function:
                  break;
               default:
                  continue;
            }
         }

         llvm::JITSymbolFlags flags;
         if (auto result = llvm::JITSymbolFlags::fromObjectSymbol(symbol)) {
            flags = *result;
         } else {
            // Ignore symbols with unknown flags
            continue;
         }

         ObjectSymbol newSymbol;
         newSymbol.objectFile = &objectFile;
         newSymbol.name = name;
         newSymbol.undefined = isUndefined;
         newSymbol.flags = flags;

         if (auto it = symbolObjects.find(name); it == symbolObjects.end()) {
            symbolObjects.emplace(name, newSymbol);
         } else {
            auto& symbol = it->second;
            // Overwrite the existing symbol only if it's weak and undefined or
            // if it's weak and the new one isn't.
            if ((symbol.undefined && symbol.flags.isWeak()) || (!newSymbol.flags.isWeak() && symbol.flags.isWeak()))
               symbol = newSymbol;
         }
      }
   }

   if (error)
      return tl::unexpected(trformat(tc, "error while reading object file from static library {0}", path));

   auto& staticLib = staticLibs[path];
   staticLib.memoryBuffer = move(libBuffer);
   staticLib.archive = move(libArchive);
   staticLib.objectFiles = move(objectFiles);

   return {};
}
//---------------------------------------------------------------------------
bool PrecompiledCxxUDOResolver::lookup(string_view name, optional_out<llvm::JITEvaluatedSymbol> symbol)
// Lookup an individual symbol
{
   {
      auto it = predefinedSymbols.find(name);
      if (it != predefinedSymbols.end()) {
         if (symbol.has_value()) {
            llvm::JITSymbolFlags symbolFlags(llvm::JITSymbolFlags::Absolute | llvm::JITSymbolFlags::Exported);
            llvm::JITEvaluatedSymbol resolvedSymbol(reinterpret_cast<llvm::JITTargetAddress>(it->second), symbolFlags);
            symbol.report(move(resolvedSymbol));
         }
         return true;
      }
   }

   {
      auto it = symbolObjects.find(name);
      if (it != symbolObjects.end()) {
         if (symbol.has_value()) {
            auto resolvedSymbol = loadSymbol(it->second);
            symbol.report(move(resolvedSymbol));
         }
         return true;
      }
   }

   return false;
}
//---------------------------------------------------------------------------
llvm::JITEvaluatedSymbol PrecompiledCxxUDOResolver::loadSymbol(ObjectSymbol& symbol)
// Try to load a symbol from the loaded libraries
{
   // The object file only needs to be loaded if the symbol is actually defined in there
   if (!symbol.undefined && !symbol.objectFile->isLoaded) {
      if (debugCxxUDO)
         llvm::errs() << "loading object file " << symbol.objectFile->objectFile->getFileName() << " for symbol " << symbol.name << '\n';

      auto objectFileInfo = linker.loadObject(*symbol.objectFile->objectFile);
      symbol.objectFile->isLoaded = true;

      if (debugCxxUDO) {
         for (auto& section : symbol.objectFile->objectFile->sections()) {
            if (section.isText() && section.getSize() > 0) {
               auto beginAddress = objectFileInfo->getSectionLoadAddress(section);
               auto endAddress = beginAddress + section.getSize();
               llvm::errs() << reinterpret_cast<void*>(beginAddress) << '\t' << reinterpret_cast<void*>(endAddress) << '\t' << symbol.objectFile->objectFile->getFileName() << '\n';
            }
         }
      }
   }

   llvm::JITEvaluatedSymbol jitSymbol;
   if (symbol.undefined) {
      // Undefined symbols are only added to symbolObjects if they are weak.
      assert(symbol.flags.isWeak());
      jitSymbol = nullptr;
   } else {
      auto linkerSymbol = linker.getSymbol(asStringRef(symbol.name));
      // The symbol is a global defined symbol, so the linker should be
      // able to find it.
      assert(!linkerSymbol.getFlags().hasError());
      jitSymbol = llvm::JITEvaluatedSymbol(linkerSymbol.getAddress(), symbol.flags);
   }

   return jitSymbol;
}
//---------------------------------------------------------------------------
void PrecompiledCxxUDOResolver::lookup(const LookupSet& symbols, OnResolvedFunction onResolved)
// Lookup the symbols and call the onResolved callback with the result
{
   LookupResult lookupResult;

   for (auto& symbol : symbols) {
      llvm::JITEvaluatedSymbol jitSymbol;
      if (!lookup(asStringView(symbol), out(jitSymbol))) {
         onResolved(llvm::Error(::make_unique<llvm::StringError>(llvm::Twine("Can't find symbol ") + symbol, error_code())));
         return;
      }
      lookupResult.emplace(symbol, move(jitSymbol));
   }

   onResolved(move(lookupResult));
}
//---------------------------------------------------------------------------
llvm::Expected<PrecompiledCxxUDOResolver::LookupSet> PrecompiledCxxUDOResolver::getResponsibilitySet(const LookupSet& symbols)
// Return which symbols out of the given ones should be looked up elsewhere
// by the caller
{
   LookupSet returnedSymbols = symbols;

   for (auto& entry : predefinedSymbols)
      returnedSymbols.erase(asStringRef(entry.first));

   return returnedSymbols;
}
//---------------------------------------------------------------------------
/// The data for a compiled C++ UDO
struct CompiledData {
   /// The memory manager
   CxxUDOMemoryManager memoryManager;
   /// The linker
   llvm::RuntimeDyld linker;
   /// The resolver for the C++ UDO functions
   PrecompiledCxxUDOResolver precompiledResolver;

   /// Constructor
   CompiledData(CxxUDOFunctors* functorStorage, CxxUDOAllocationFuncs allocationFuncs, int64_t tlsBlockOffset, uint64_t tlsBlockSize)
      : memoryManager(tlsBlockOffset, tlsBlockSize), linker(memoryManager, precompiledResolver), precompiledResolver(linker, functorStorage, allocationFuncs) {}

   // Get the address of a symbol
   void* lookup(string_view name);
};
//---------------------------------------------------------------------------
void* CompiledData::lookup(string_view name)
// Get the address of a symbol
{
   llvm::JITEvaluatedSymbol symbol;
   if (!precompiledResolver.lookup(name, out(symbol)))
      symbol = linker.getSymbol(asStringRef(name));

   return reinterpret_cast<void*>(symbol.getAddress());
}
//---------------------------------------------------------------------------
} // namespace
//---------------------------------------------------------------------------
unique_ptr<byte[]> CxxUDOExecution::createLibcConstructorArg()
// Create the constructor arguments that can be passed to libc. Return
// value is a pointer to a struct { int argc; char** argv; } that also
// contains extra data that libc needs, i.e. the environment and auxvals.
{
   constexpr size_t constructorArgSize = 16; // argc and argv members
   constexpr size_t argvSize = 2 * sizeof(char*);
   constexpr size_t envpSize = sizeof(char*);
   size_t auxvSize = AuxVec::getAuxVecSize();
   auto constructorArg = make_unique<byte[]>(constructorArgSize + argvSize + envpSize + auxvSize);

   const char** argv = reinterpret_cast<const char**>(constructorArg.get() + constructorArgSize);
   const char** envp = reinterpret_cast<const char**>(constructorArg.get() + constructorArgSize + argvSize);
   byte* auxv = constructorArg.get() + constructorArgSize + argvSize + envpSize;

   assert(reinterpret_cast<uintptr_t>(auxv) % alignof(uint64_t) == 0);

   // Set argc to 1
   {
      int32_t argc = 1;
      memcpy(constructorArg.get(), &argc, sizeof(argc));
   }
   // Set argv to the pointer just after the constructor arg
   memcpy(constructorArg.get() + 8, &argv, sizeof(argv));

   argv[0] = "cxxudo";
   argv[1] = nullptr;

   // Don't set any environment variables
   envp[0] = nullptr;

   // glibc requires some auxvec values to work properly, so set them here
   AuxVec::getAuxVec(span(auxv, auxvSize));

   return constructorArg;
}
//---------------------------------------------------------------------------
/// The implementation of `CxxUDOExecution`
struct CxxUDOExecution::Impl {
   /// The functors. The pointer of this value will be linked into the UDO
   /// object file, so we put it in a unique_ptr to make sure that its address
   /// doesn't change.
   unique_ptr<CxxUDOFunctors> functorStorage;
   /// The compiled data
   optional<CompiledData> compiledData;
   /// The pointers to the compiled functions
   CxxUDOFunctions compiledFunctions;
};
//---------------------------------------------------------------------------
CxxUDOExecution::CxxUDOExecution(span<char> objectFile)
   : objectFile(objectFile), impl(make_unique<Impl>())
// Construct from a compiled UDO object file
{
}
//---------------------------------------------------------------------------
CxxUDOExecution::~CxxUDOExecution()
// Destructor
{
}
//---------------------------------------------------------------------------
tl::expected<void, std::string> CxxUDOExecution::link(CxxUDOAllocationFuncs allocationFuncs, int64_t tlsBlockOffset, uint64_t tlsBlockSize)
// Link the object file
{
   impl->functorStorage = make_unique<CxxUDOFunctors>();

   impl->compiledData.emplace(impl->functorStorage.get(), allocationFuncs, tlsBlockOffset, tlsBlockSize);

   // Add the static libraries required for C++
   vector<string> staticLibs;
   {
      auto compilation = ClangCompiler::createCompilation(CxxUDOCompiler::getOptLevel());
      auto& args = compilation->getArgs();
      auto& toolChain = compilation->getDefaultToolChain();

      bool needsLibunwind = false;
      switch (toolChain.GetRuntimeLibType(args)) {
         case clang::driver::ToolChain::RLT_CompilerRT:
            staticLibs.push_back(toolChain.getCompilerRT(args, "builtins"));
            needsLibunwind = true;
            break;
         case clang::driver::ToolChain::RLT_Libgcc:
            staticLibs.push_back(toolChain.GetFilePath("libgcc_eh.a"));
            staticLibs.push_back(toolChain.GetFilePath("libgcc.a"));
            needsLibunwind = toolChain.GetUnwindLibType(args) == clang::driver::ToolChain::UNW_CompilerRT;
            break;
      }

      if (needsLibunwind)
         staticLibs.push_back(toolChain.GetFilePath("libunwind.a"));

      staticLibs.push_back(string(cxxUDODepsPrefix) + "/lib/libm-2.33.a");
      staticLibs.push_back(string(cxxUDODepsPrefix) + "/lib/libmvec.a");

      staticLibs.push_back(string(cxxUDODepsPrefix) + "/lib/libc.a");
      staticLibs.push_back(string(cxxUDODepsPrefix) + "/lib/libpthread.a");

      staticLibs.push_back(string(cxxUDODepsPrefix) + "/lib/libc++abi.a");
      staticLibs.push_back(string(cxxUDODepsPrefix) + "/lib/libc++.a");
   }

   for (auto& lib : staticLibs)
      if (auto result = impl->compiledData->precompiledResolver.addLibrary(lib); !result)
         return result;

   llvm::MemoryBufferRef objectFileBufferRef({objectFile.data(), objectFile.size()}, "cxxudo.o");
   unique_ptr<llvm::object::ObjectFile> objectFile;
   {
      auto result = llvm::object::ObjectFile::createObjectFile(objectFileBufferRef);
      if (!result)
         return tl::unexpected(string(tr(tc, "invalid object file for C++ UDO")));
      objectFile = move(*result);
   }

   auto& linker = impl->compiledData->linker;
   linker.loadObject(*objectFile);
   linker.finalizeWithMemoryManagerLocking();

   if (linker.hasError()) {
      auto error = asStringView(linker.getErrorString());
      return tl::unexpected(trformat(tc, "error when linking C++ UDO: {0}", error));
   }

   return {};
}
//---------------------------------------------------------------------------
CxxUDOFunctors& CxxUDOExecution::getFunctors() const
// Get the the functors. The may be modified for a new execution.
{
   return *impl->functorStorage;
}
//---------------------------------------------------------------------------
CxxUDOFunctions CxxUDOExecution::initialize()
// Initialize the memory and return the function pointers that are ready to
// be called.
{
   auto& compiledData = *impl->compiledData;
   compiledData.memoryManager.getMemoryManager().initialize();
   compiledData.memoryManager.getTLSAllocations().initializeTLS();

   CxxUDOFunctions functions;
#define R(name) \
   functions.name = reinterpret_cast<decltype(functions.name)>(compiledData.lookup(CxxUDOCompiler::name##Name));
   R(globalConstructor)
   R(globalDestructor)
   R(threadInit)
   R(constructor)
   R(destructor)
   R(accept)
   R(extraWork)
   R(process)
#undef R

   return functions;
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
