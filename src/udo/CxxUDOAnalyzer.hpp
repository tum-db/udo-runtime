#ifndef H_udo_CxxUDOAnalyzer
#define H_udo_CxxUDOAnalyzer
//---------------------------------------------------------------------------
#include "thirdparty/tl/expected.hpp"
#include "udo/LLVMMetadata.hpp"
#include <llvm/ADT/SmallVector.h>
#include <memory>
#include <string>
#include <vector>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
namespace llvm {
class Function;
class LLVMContext;
class Module;
class StructType;
class Type;
}
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
/// An output attribute of a C++ UDO
struct CxxUDOOutput {
   /// The name of the attribute
   std::string name;
   /// The llvm type of the attribute
   llvm::Type* type;
};
//---------------------------------------------------------------------------
/// All functions that can be used by a C++ UDO
struct CxxUDORuntimeFunctions {
   /// The void printDebug(const char*, uint64_t) function
   llvm::Function* printDebug;
   /// The uint64_t getRandom() function
   llvm::Function* getRandom;
};
//---------------------------------------------------------------------------
/// The result of analyzing a C++ UDO
struct CxxUDOAnalysis {
   /// The declarations of the runtime functions
   CxxUDORuntimeFunctions runtimeFunctions;
   /// The string type
   llvm::Type* stringType;
   /// The output attributes
   llvm::SmallVector<CxxUDOOutput, 8> output;
   /// The global constructor (for static initialization)
   llvm::Function* globalConstructor;
   /// The global destructor (for static initialization)
   llvm::Function* globalDestructor;
   /// The InputTuple type
   llvm::Type* inputTupleType;
   /// The type of the output
   llvm::Type* outputTupleType;
   /// The produceOutputTuple function
   llvm::Function* produceOutputTuple;
   /// The size of the UDO
   size_t size;
   /// The alignment of the UDO
   size_t alignment;
   /// The name of the UDO
   std::string name;
   /// The llvm type of the UDO
   llvm::Type* llvmType;
   /// The constructor of the UDO
   llvm::Function* constructor;
   /// The destructor of the UDO
   llvm::Function* destructor;
   /// The consume function of the UDO
   llvm::Function* consume;
   /// The extraWork function of the UDO
   llvm::Function* extraWork;
   /// The postProduce function of the UDO
   llvm::Function* postProduce;
   /// Is produceOutputTuple() called in consume()?
   bool produceInConsume;
   /// Is produceOutputTuple() called in postProduce()?
   bool produceInPostProduce;
};
//---------------------------------------------------------------------------
/// The analyzer for C++ UDOs
class CxxUDOAnalyzer {
   private:
   struct Impl;

   /// The source code of the function
   std::string funcSource;
   /// The name of the class that implements the UDO
   std::string udoClassName;
   /// The implementation
   std::unique_ptr<Impl> impl;

   public:
   /// Constructor
   explicit CxxUDOAnalyzer(std::string funcSource, std::string udoClassName);
   /// Move constructor
   CxxUDOAnalyzer(CxxUDOAnalyzer&& analyzer);
   /// Move assignment
   CxxUDOAnalyzer& operator=(CxxUDOAnalyzer&& analyzer);
   /// Destructor
   ~CxxUDOAnalyzer();

   /// Analyze the function
   tl::expected<void, std::string> analyze(llvm::LLVMContext* context = nullptr, unsigned optimizationLevel = 3);
   /// Get the analysis
   const CxxUDOAnalysis& getAnalysis() const;
   /// Get the analysis
   CxxUDOAnalysis& getAnalysis();
   /// Get the LLVM module
   llvm::Module& getModule() const;
   /// Take the module
   std::unique_ptr<llvm::Module> takeModule();

   /// Serialize the analysis into a buffer
   std::vector<char> getSerializedAnalysis();
};
//---------------------------------------------------------------------------
namespace llvm_metadata {
//---------------------------------------------------------------------------
template <>
struct IO<CxxUDORuntimeFunctions> : StructMapper<CxxUDORuntimeFunctions> {
   static IOResult enumEntries(StructContext& context, CxxUDORuntimeFunctions& value);
};
//---------------------------------------------------------------------------
template <>
struct IO<CxxUDOOutput> : StructMapper<CxxUDOOutput> {
   static IOResult enumEntries(StructContext& context, CxxUDOOutput& value);
};
//---------------------------------------------------------------------------
template <>
struct IO<CxxUDOAnalysis> : StructMapper<CxxUDOAnalysis> {
   static IOResult enumEntries(StructContext& context, CxxUDOAnalysis& value);
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
