#ifndef H_udo_CxxUDOCompiler
#define H_udo_CxxUDOCompiler
//---------------------------------------------------------------------------
#include "thirdparty/tl/expected.hpp"
#include "udo/LLVMMetadata.hpp"
#include <string>
#include <string_view>
#include <vector>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
namespace llvm {
class Function;
class GlobalVariable;
class StructType;
}
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
class CxxUDOAnalyzer;
//---------------------------------------------------------------------------
/// The functions that are used for the UDO which are derived from the
/// analysis.
struct CxxUDOLLVMFunctions {
   /// The type for a functor
   llvm::StructType* udoFunctorType;
   /// The global constructor
   llvm::Function* globalConstructor;
   /// The global destructor
   llvm::Function* globalDestructor;
   /// The thread initialization function
   llvm::Function* threadInit;
   /// The emit function
   llvm::Function* emit;
   /// The global variable that contains the functor to the callback for emit
   llvm::GlobalVariable* emitFunctor;
   /// The global variable that contains the functor to the printDebug function
   llvm::GlobalVariable* printDebugFunctor;
   /// The global variable that contains the functor to the getRandom function
   llvm::GlobalVariable* getRandomFunctor;
   /// The wrapper for the constructor
   llvm::Function* constructor;
   /// The wrapper for the destructor
   llvm::Function* destructor;
   /// The wrapper for accept
   llvm::Function* accept;
   /// The extraWork function
   llvm::Function* extraWork;
   /// The wrapper for process
   llvm::Function* process;

   /// Call a function for every member pointer to the llvm functions
   template <typename F>
   static void mapFunctionPointers(F mapFunc) {
      mapFunc(&CxxUDOLLVMFunctions::globalConstructor);
      mapFunc(&CxxUDOLLVMFunctions::globalDestructor);
      mapFunc(&CxxUDOLLVMFunctions::threadInit);
      mapFunc(&CxxUDOLLVMFunctions::emit);
      mapFunc(&CxxUDOLLVMFunctions::constructor);
      mapFunc(&CxxUDOLLVMFunctions::destructor);
      mapFunc(&CxxUDOLLVMFunctions::accept);
      mapFunc(&CxxUDOLLVMFunctions::extraWork);
      mapFunc(&CxxUDOLLVMFunctions::process);
      mapFunc(&CxxUDOLLVMFunctions::globalConstructor);
      mapFunc(&CxxUDOLLVMFunctions::globalDestructor);
      mapFunc(&CxxUDOLLVMFunctions::threadInit);
      mapFunc(&CxxUDOLLVMFunctions::emit);
      mapFunc(&CxxUDOLLVMFunctions::constructor);
      mapFunc(&CxxUDOLLVMFunctions::destructor);
      mapFunc(&CxxUDOLLVMFunctions::accept);
      mapFunc(&CxxUDOLLVMFunctions::extraWork);
      mapFunc(&CxxUDOLLVMFunctions::process);
   }

   /// Call a function for every llvm functions
   template <typename F>
   void mapFunctions(F mapFunc) {
      mapFunctionPointers([&](auto ptr) { mapFunc(this->*ptr); });
   }

   /// Call a function for every global value
   template <typename F>
   void mapGlobalValues(F mapGlobal) {
      mapGlobal(globalConstructor);
      mapGlobal(globalDestructor);
      mapGlobal(threadInit);
      mapGlobal(emit);
      mapGlobal(constructor);
      mapGlobal(destructor);
      mapGlobal(accept);
      mapGlobal(extraWork);
      mapGlobal(process);
      mapGlobal(globalConstructor);
      mapGlobal(globalDestructor);
      mapGlobal(threadInit);
      mapGlobal(emit);
      mapGlobal(emitFunctor);
      mapGlobal(printDebugFunctor);
      mapGlobal(getRandomFunctor);
      mapGlobal(constructor);
      mapGlobal(destructor);
      mapGlobal(accept);
      mapGlobal(extraWork);
      mapGlobal(process);
   }
};
//---------------------------------------------------------------------------
/// The compiler that takes an analyzed C++ UDO from `CxxUDOAnalyzer` and
/// compiles it to machine code and returns function pointers that can be
/// called.
class CxxUDOCompiler {
   public:
   /// The name of the functor type
   static constexpr std::string_view functorTypeName = "udo.CxxUDO.Functor";
   /// The name of the argument type of the global constructor
   static constexpr std::string_view globalConstructorArgTypeName = "udo.CxxUDO.GlobalConstructorArg";
   /// The name of the global constructor
   static constexpr std::string_view globalConstructorName = "udo.CxxUDO.GlobalConstructor";
   /// The name of the global destructor
   static constexpr std::string_view globalDestructorName = "udo.CxxUDO.GlobalDestructor";
   /// The name of the thread initialization function
   static constexpr std::string_view threadInitName = "udo.CxxUDO.ThreadInit";
   /// The name of the generated emit function
   static constexpr std::string_view emitName = "udo.CxxUDO.emit";
   /// The name of the emit callback functor
   static constexpr std::string_view emitFunctorName = "udo.CxxUDO.emitCallback";
   /// The name of the functor for printDebug
   static constexpr std::string_view printDebugFunctorName = "udo.CxxUDO.printDebug";
   /// The name of the functor for getRandom
   static constexpr std::string_view getRandomFunctorName = "udo.CxxUDO.getRandom";
   /// The name of the constructor of the UDO class
   static constexpr std::string_view constructorName = "udo.CxxUDO.Constructor";
   /// The name of the destructor of the UDO class
   static constexpr std::string_view destructorName = "udo.CxxUDO.Destructor";
   /// The name of the accept function of the UDO class
   static constexpr std::string_view acceptName = "udo.CxxUDO.accept";
   /// The name of the extraWork function of the UDO class
   static constexpr std::string_view extraWorkName = "udo.CxxUDO.extraWork";
   /// The name of the process function of the UDO class
   static constexpr std::string_view processName = "udo.CxxUDO.process";

   private:
   /// The analyzer
   CxxUDOAnalyzer& analyzer;

   public:
   /// Get the optimization level that is used for C++ UDOs
   static unsigned getOptLevel();

   /// Constructor
   explicit CxxUDOCompiler(CxxUDOAnalyzer& analyzer) : analyzer(analyzer) {}

   /// Preprocess the llvm module by creating all special extra functions that
   /// are used by the UDO execution.
   tl::expected<CxxUDOLLVMFunctions, std::string> preprocessModule();

   /// Compile the UDO to an object file.
   tl::expected<std::vector<char>, std::string> compile();
};
//---------------------------------------------------------------------------
namespace llvm_metadata {
//---------------------------------------------------------------------------
template <>
struct IO<CxxUDOLLVMFunctions> : StructMapper<CxxUDOLLVMFunctions> {
   static IOResult enumEntries(StructContext& context, CxxUDOLLVMFunctions& value);
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
