#ifndef H_udo_LLVMCompiler
#define H_udo_LLVMCompiler
//---------------------------------------------------------------------------
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/IRTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/ObjectTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Target/TargetMachine.h>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2016 Thomas Neumann
//---------------------------------------------------------------------------
namespace llvm {
class EngineBuilder;
}
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
/// A compiler that compiles an llvm module to machine code
class LLVMCompiler {
   public:
   using FunctionSymbols = std::unordered_map<std::string, void*>;

   /// Initialize LLVM
   static void initializeLLVM();
   /// Optimize the llvm module
   static void optimizeModule(llvm::Module& m);
   /// Remove unused functions in the llvm module
   static void removeUnusedFunctions(llvm::Module& module);
   /// Do the common setup for the EngineBuilder
   static llvm::EngineBuilder& setupBuilder(llvm::EngineBuilder& builder, bool cheapCompilation);

   /// The function symbols that are created from outside
   FunctionSymbols functionSymbols;
   /// The context
   llvm::orc::ThreadSafeContext context;
   /// The target machine
   std::unique_ptr<llvm::TargetMachine> targetMachine;
   /// The execution session
   llvm::orc::ExecutionSession es;
   /// The object layer
   llvm::orc::RTDyldObjectLinkingLayer objectLayer;
   /// The object transform layer
   llvm::orc::ObjectTransformLayer objectTransformLayer;
   /// The compile layout
   llvm::orc::IRCompileLayer compileLayer;
   /// The optimize layer
   llvm::orc::IRTransformLayer optimizeLayer;
   /// The main JITDylib
   llvm::orc::JITDylib& mainDylib;

   /// Constructor
   LLVMCompiler(FunctionSymbols functionSymbols, std::unique_ptr<llvm::LLVMContext> context, std::unique_ptr<llvm::TargetMachine> targetMachine);
   /// Destructor
   ~LLVMCompiler();

   /// Create the compiler for the JITBackend
   static LLVMCompiler createForJITBackend(FunctionSymbols functionSymbols, std::unique_ptr<llvm::LLVMContext> context, bool cheapCompilation = false);

   /// Compile a module
   void compile(std::unique_ptr<llvm::Module> module);
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
