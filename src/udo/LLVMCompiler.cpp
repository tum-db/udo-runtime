#include "udo/LLVMCompiler.hpp"
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <optional>
#include <utility>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2016 Thomas Neumann
//---------------------------------------------------------------------------
using namespace std;
using namespace std::literals::string_view_literals;
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
/// Guard to initialize LLVM only once
static once_flag llvmInitialized;
/// The CPU name
static string cpuName;
//---------------------------------------------------------------------------
void LLVMCompiler::initializeLLVM()
// Initialize LLVM
{
   call_once(llvmInitialized, [] {
      llvm::InitializeNativeTarget();
      llvm::InitializeNativeTargetAsmPrinter();
      llvm::InitializeNativeTargetAsmParser();

      cpuName = llvm::sys::getHostCPUName();
      llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
   });
}
//---------------------------------------------------------------------------
void LLVMCompiler::optimizeModule(llvm::Module* m, span<OptimizationPass> passes, atomic<bool>* cancellation, [[maybe_unused]] bool compileStatic)
// Optimize a module
{
   llvm::legacy::FunctionPassManager functionPassManager(m);
   optional<llvm::legacy::PassManager> modulePassManager = nullopt;

#ifndef NDEBUG
   functionPassManager.add(llvm::createVerifierPass());
#endif
   functionPassManager.add(llvm::createInstructionCombiningPass());
   functionPassManager.add(llvm::createReassociatePass());
   functionPassManager.add(llvm::createGVNPass());
   functionPassManager.add(llvm::createCFGSimplificationPass());
   functionPassManager.add(llvm::createAggressiveDCEPass());
   functionPassManager.add(llvm::createCFGSimplificationPass());

   // Sanitize functions if we have an asan build
   // TODO: to fix this we would have to register @asan.module_ctor like clang does
#if 0
   if (CompilerToolchain::addressSanitizerActive) {
      for (auto& f : *m)
         if (!f.isDeclaration())
            f.addFnAttr(llvm::Attribute::SanitizeAddress);
      if (!compileStatic) {
         modulePassManager.emplace();
         // Run asan function pass on module, since that's what clang does
         // Future versions of LLVM might again run this FunctionPass on the FunctionPassManager
         modulePassManager->add(llvm::createAddressSanitizerFunctionPass());
      }
   }
#endif

   functionPassManager.doInitialization();

   for (auto& f : *m) {
      if (cancellation && cancellation->load()) break;
      functionPassManager.run(f);
   }
   if (modulePassManager)
      modulePassManager->run(*m);

   for (auto& pass : passes)
      pass(*m);
}
//---------------------------------------------------------------------------
void LLVMCompiler::removeUnusedFunctions(llvm::Module& module)
// Remove unused functions in the llvm module
{
   llvm::GlobalDCEPass dce;
   llvm::FunctionAnalysisManager fam;
   llvm::ModuleAnalysisManager mam;
   mam.registerPass([&] { return llvm::FunctionAnalysisManagerModuleProxy(fam); });
   dce.run(module, mam);
}
//---------------------------------------------------------------------------
llvm::EngineBuilder&& LLVMCompiler::setupBuilder(llvm::EngineBuilder&& builder, bool cheapCompilation)
// Do the common setup for the EngineBuilder
{
   // Tune down optimization level if needed
   llvm::TargetOptions options;
   if (cheapCompilation) {
      options.EnableFastISel = true;
      builder.setOptLevel(llvm::CodeGenOpt::None);
   }
   builder.setTargetOptions(options);
   builder.setMCPU(cpuName);
   builder.setEmulatedTLS(false);

   // Set cpu instruction features to guarantee the safe usage of new & llvm unknown cpu names
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wambiguous-reversed-operator"
#endif
   if (auto features = llvm::StringMap<bool>{}; llvm::sys::getHostCPUFeatures(features)) {
      auto mattrs = llvm::SmallVector<llvm::StringRef, 32>{};
      for (auto& feature : features)
         if (feature.second)
            mattrs.emplace_back(feature.first());
      builder.setMAttrs(mattrs);
   }
#ifdef __clang__
#pragma clang diagnostic pop
#endif

   return move(builder);
}
//---------------------------------------------------------------------------
namespace {
//---------------------------------------------------------------------------
/// Lookup Logic
struct JITDefinitionGenerator : public llvm::orc::DefinitionGenerator {
   /// The compiler
   LLVMCompiler& compiler;
   /// Constructor
   JITDefinitionGenerator(LLVMCompiler& compiler) : compiler(compiler) {}
   /// The lookup logic
   llvm::Error tryToGenerate(llvm::orc::LookupState& /*LS*/, llvm::orc::LookupKind /*K*/, llvm::orc::JITDylib& jd, llvm::orc::JITDylibLookupFlags /*JDLookupFlags*/, const llvm::orc::SymbolLookupSet& names) override {
      // The symbols that llvm sometimes needs to implement its intrinsics
      const static unordered_map<string_view, void*> intrinsicSymbols({
         {"memcpy"sv, reinterpret_cast<void*>(&memcpy)},
         {"memmove"sv, reinterpret_cast<void*>(&memmove)},
         {"memset"sv, reinterpret_cast<void*>(&memset)},
      });

      llvm::orc::SymbolNameSet added;
      llvm::orc::SymbolMap newSymbols;

      // Look up all symbol
      for (const auto& [name, flags] : names) {
         if (!(*name).empty()) {
            // Try to find the symbol
            void* addr = nullptr;
            if (auto iter = compiler.functionSymbols.find((*name).str()); iter != compiler.functionSymbols.end())
               addr = iter->second;
            if (!addr) {
               auto it = intrinsicSymbols.find(*name);
               if (it != intrinsicSymbols.end())
                  addr = it->second;
            }

            // Register if found
            if (addr) {
               added.insert(name);
               newSymbols[name] = llvm::JITEvaluatedSymbol(static_cast<llvm::JITTargetAddress>(reinterpret_cast<uintptr_t>(addr)), llvm::JITSymbolFlags::Exported);
            }
         }
      }

      // Register new symbols if any
      if (!newSymbols.empty())
         llvm::cantFail(jd.define(llvm::orc::absoluteSymbols(move(newSymbols))));
      return llvm::Error::success();
   }
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
LLVMCompiler::LLVMCompiler(unordered_map<string, void*> functionSymbols, unique_ptr<llvm::LLVMContext> context, unique_ptr<llvm::TargetMachine> targetMachine, vector<OptimizationPass> optimizationPasses, atomic<bool>* cancellation)
   : functionSymbols(move(functionSymbols)), context(move(context)),
     targetMachine(move(targetMachine)),
     optimizationPasses(move(optimizationPasses)),
     objectLayer(es, []() { return make_unique<llvm::SectionMemoryManager>(); }),
     compileLayer(es, objectLayer, make_unique<llvm::orc::SimpleCompiler>(*this->targetMachine)),
     optimizeLayer(es, compileLayer, [optimizationPasses = span(this->optimizationPasses), cancellation](llvm::orc::ThreadSafeModule m, const llvm::orc::MaterializationResponsibility&) { optimizeModule(m.getModuleUnlocked(),optimizationPasses,cancellation,false); return m; }),
     mainDylib(llvm::cantFail(es.createJITDylib("<main>")))
// Constructor
{
   // Hook up the lookup logic
   mainDylib.addGenerator(make_unique<JITDefinitionGenerator>(*this));
}
//---------------------------------------------------------------------------
LLVMCompiler::~LLVMCompiler()
// Destructor
{
   llvm::cantFail(es.endSession());
}
//---------------------------------------------------------------------------
LLVMCompiler LLVMCompiler::createForJITBackend(std::unordered_map<std::string, void*> functionSymbols, std::unique_ptr<llvm::LLVMContext> context, vector<OptimizationPass> optimizationPasses, bool cheapCompilation, std::atomic<bool>* cancellation)
// Create the compiler for the JITBackend
{
   auto* targetMachinePtr = setupBuilder(llvm::EngineBuilder{}, cheapCompilation).selectTarget();
   unique_ptr<remove_pointer_t<decltype(targetMachinePtr)>> targetMachine(targetMachinePtr);
   return LLVMCompiler(move(functionSymbols), move(context), move(targetMachine), move(optimizationPasses), cancellation);
}
//---------------------------------------------------------------------------
void LLVMCompiler::compile(unique_ptr<llvm::Module> module)
// Compile the module
{
   llvm::cantFail(optimizeLayer.add(mainDylib, llvm::orc::ThreadSafeModule(move(module), this->context)));
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
