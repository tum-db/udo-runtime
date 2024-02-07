#include "udo/LLVMCompiler.hpp"
#include "udo/Setting.hpp"
#include <llvm/Analysis/Lint.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/JITEventListener.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/Reassociate.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
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
namespace {
//---------------------------------------------------------------------------
/// Our optimization levels
enum class OptimizationLevel { Minimal,
                               LLVMO1,
                               LLVMO2,
                               LLVMO3,
                               LLVMOs,
                               LLVMOz };
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
static Setting<OptimizationLevel> llvmoptimizer("llvmoptimizer", "LLVM Optimization level for JITBackend", OptimizationLevel::Minimal,
                                                settinghelper::makeEnumParser(tuple{OptimizationLevel::Minimal, "Minimal optimizations", '0'},
                                                                              tuple{OptimizationLevel::LLVMO1, "LLVM O1: Optimize quickly", '1'},
                                                                              tuple{OptimizationLevel::LLVMO2, "LLVM O2: Fast execution", '2'},
                                                                              tuple{OptimizationLevel::LLVMO3, "LLVM O3: Really fast execution", '3'},
                                                                              tuple{OptimizationLevel::LLVMOs, "LLVM Os: Small code size and speed", 's'},
                                                                              tuple{OptimizationLevel::LLVMOz, "LLVM Oz: Minimize only code size", 'z'}));
//---------------------------------------------------------------------------
void LLVMCompiler::optimizeModule(llvm::Module& m)
// Optimize a module
{
   // See: PassBuilderBindings.cpp:LLVMRunPasses()
   auto passBuilder = llvm::PassBuilder();
   llvm::LoopAnalysisManager loopAnalysisManager;
   llvm::FunctionAnalysisManager functionAnalysisManager;
   llvm::CGSCCAnalysisManager cgsccAnalysisManager;
   llvm::ModuleAnalysisManager moduleAnalysisManager;
   passBuilder.registerLoopAnalyses(loopAnalysisManager);
   passBuilder.registerFunctionAnalyses(functionAnalysisManager);
   passBuilder.registerCGSCCAnalyses(cgsccAnalysisManager);
   passBuilder.registerModuleAnalyses(moduleAnalysisManager);
   passBuilder.crossRegisterProxies(loopAnalysisManager, functionAnalysisManager, cgsccAnalysisManager, moduleAnalysisManager);
   llvm::FunctionPassManager functionPassManager;

   auto optimizationLevel = llvmoptimizer.get();
   if (optimizationLevel == OptimizationLevel::Minimal) {
      // Stripped down O1 optimization pipeline
      // See: PassBuilder::buildO1FunctionSimplificationPipeline()
      functionPassManager.addPass(llvm::EarlyCSEPass(false));
      functionPassManager.addPass(llvm::SimplifyCFGPass());
      functionPassManager.addPass(llvm::InstCombinePass());
      functionPassManager.addPass(llvm::ReassociatePass());
      functionPassManager.addPass(llvm::GVNPass());
      functionPassManager.addPass(llvm::SimplifyCFGPass());
      functionPassManager.addPass(llvm::ADCEPass());
      functionPassManager.addPass(llvm::SimplifyCFGPass());

      llvm::ModulePassManager modulePassManager;
      modulePassManager.addPass(llvm::createModuleToFunctionPassAdaptor(move(functionPassManager)));
      modulePassManager.run(m, moduleAnalysisManager);

      return;
   }

   // LLVM optimization settings
   llvm::OptimizationLevel llvmOptLevel;
   switch (optimizationLevel) {
      case OptimizationLevel::Minimal: __builtin_unreachable(); break;
      case OptimizationLevel::LLVMO1: llvmOptLevel = llvm::OptimizationLevel::O1; break;
      case OptimizationLevel::LLVMO2: llvmOptLevel = llvm::OptimizationLevel::O2; break;
      case OptimizationLevel::LLVMO3: llvmOptLevel = llvm::OptimizationLevel::O3; break;
      case OptimizationLevel::LLVMOs: llvmOptLevel = llvm::OptimizationLevel::Os; break;
      case OptimizationLevel::LLVMOz: llvmOptLevel = llvm::OptimizationLevel::Oz; break;
   }
   auto modulePassManager = passBuilder.buildPerModuleDefaultPipeline(llvmOptLevel);
   modulePassManager.addPass(llvm::createModuleToFunctionPassAdaptor(move(functionPassManager)));
   modulePassManager.run(m, moduleAnalysisManager);
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
llvm::EngineBuilder& LLVMCompiler::setupBuilder(llvm::EngineBuilder& builder, bool cheapCompilation)
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

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wambiguous-reversed-operator"
#endif
   // Set cpu instruction features to guarantee the safe usage of new & llvm unknown cpu names
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

   return builder;
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

      // Look up all symbols
      for (const auto& [name, flags] : names) {
         if (!(*name).empty()) {
            // Try to find the symbol
            auto nameSv = string_view((*name).data(), (*name).size());
            void* addr = nullptr;
            if (auto iter = compiler.functionSymbols.find(string(nameSv)); iter != compiler.functionSymbols.end())
               addr = iter->second;
            if (!addr) {
               auto it = intrinsicSymbols.find(*name);
               if (it != intrinsicSymbols.end())
                  addr = it->second;
            }

            // Register if found
            if (addr) {
               added.insert(name);
               newSymbols[name] = {llvm::orc::ExecutorAddr(reinterpret_cast<uintptr_t>(addr)), llvm::JITSymbolFlags::Exported};
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
LLVMCompiler::LLVMCompiler(FunctionSymbols functionSymbols, unique_ptr<llvm::LLVMContext> context, unique_ptr<llvm::TargetMachine> targetMachine)
   : functionSymbols(move(functionSymbols)), context(move(context)),
     targetMachine(move(targetMachine)),
     es(make_unique<llvm::orc::UnsupportedExecutorProcessControl>()),
     objectLayer(es, []() { return make_unique<llvm::SectionMemoryManager>(); }),
     objectTransformLayer(es, objectLayer),
     compileLayer(es, objectTransformLayer, make_unique<llvm::orc::SimpleCompiler>(*this->targetMachine)),
     optimizeLayer(es, compileLayer, [](llvm::orc::ThreadSafeModule m, const llvm::orc::MaterializationResponsibility&) { optimizeModule(*m.getModuleUnlocked()); return m; }),
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
LLVMCompiler LLVMCompiler::createForJITBackend(FunctionSymbols functionSymbols, std::unique_ptr<llvm::LLVMContext> context, bool cheapCompilation)
// Create the compiler for the JITBackend
{
   llvm::EngineBuilder builder;
   auto* targetMachinePtr = setupBuilder(builder, cheapCompilation).selectTarget();
   unique_ptr<remove_pointer_t<decltype(targetMachinePtr)>> targetMachine(targetMachinePtr);
   return LLVMCompiler(move(functionSymbols), move(context), move(targetMachine));
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
