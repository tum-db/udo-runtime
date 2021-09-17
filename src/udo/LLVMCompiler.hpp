#ifndef H_udo_LLVMCompiler
#define H_udo_LLVMCompiler
//---------------------------------------------------------------------------
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/IRTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Target/TargetMachine.h>
#include <atomic>
#include <functional>
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
   /// A function representing an optimization pass on an llvm module
   using OptimizationPass = std::function<void(llvm::Module&)>;

   /// Initialize LLVM
   static void initializeLLVM();
   /// Optimize the llvm module
   static void optimizeModule(llvm::Module* m, std::span<OptimizationPass> passes, std::atomic<bool>* cancellation, bool compileStatic);
   /// Remove unused functions in the llvm module
   static void removeUnusedFunctions(llvm::Module& module);
   /// Do the common setup for the EngineBuilder
   static llvm::EngineBuilder&& setupBuilder(llvm::EngineBuilder&& builder, bool cheapCompilation);

   /// The function symbols that are created from outside
   std::unordered_map<std::string, void*> functionSymbols;
   /// The context
   llvm::orc::ThreadSafeContext context;
   /// The target machine
   std::unique_ptr<llvm::TargetMachine> targetMachine;
   /// The additional optimization passes to run on the module
   std::vector<OptimizationPass> optimizationPasses;
   /// The execution session
   llvm::orc::ExecutionSession es;
   /// The object layer
   llvm::orc::RTDyldObjectLinkingLayer objectLayer;
   /// The compile layer
   llvm::orc::IRCompileLayer compileLayer;
   /// The optimize layer
   llvm::orc::IRTransformLayer optimizeLayer;
   /// The main JITDylib
   llvm::orc::JITDylib& mainDylib;

   /// Constructor
   LLVMCompiler(std::unordered_map<std::string, void*> functionSymbols, std::unique_ptr<llvm::LLVMContext> context, std::unique_ptr<llvm::TargetMachine> targetMachine, std::vector<OptimizationPass> optimizationPasses = {}, std::atomic<bool>* cancellation = nullptr);
   /// Destructor
   ~LLVMCompiler();

   /// Create the compiler for the JITBackend
   static LLVMCompiler createForJITBackend(std::unordered_map<std::string, void*> functionSymbols, std::unique_ptr<llvm::LLVMContext> context, std::vector<OptimizationPass> optimizationPasses = {}, bool cheapCompilation = false, std::atomic<bool>* cancellation = nullptr);

   /// Compile a module
   void compile(std::unique_ptr<llvm::Module> module);
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
