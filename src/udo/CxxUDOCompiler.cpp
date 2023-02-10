#include "udo/CxxUDOCompiler.hpp"
#include "udo/ClangCompiler.hpp"
#include "udo/CxxUDOAnalyzer.hpp"
#include "udo/LLVMCompiler.hpp"
#include "udo/LLVMUtil.hpp"
#include "udo/Setting.hpp"
#include "udo/i18n.hpp"
#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Object/Archive.h>
#include <llvm/Option/ArgList.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <fstream>
#include <string_view>
#include <vector>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
using namespace std;
//---------------------------------------------------------------------------
static const char tc[] = "udo/CxxUDOCompiler";
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
static Setting<bool> dumpCxxUDOObject("dumpCxxUDOObject", "Dump the object file of the compiled C++ UDO", false);
//---------------------------------------------------------------------------
#ifndef NDEBUG
static constexpr unsigned defaultOptLevel = 0;
#else
static constexpr unsigned defaultOptLevel = 3;
#endif
//---------------------------------------------------------------------------
static Setting<unsigned> cxxUDOOptLevel("cxxUDOOptLevel", "The optimization level used for C++ UDOs", defaultOptLevel);
//---------------------------------------------------------------------------
unsigned CxxUDOCompiler::getOptLevel()
// Get the optimization level that is used for C++ UDOs
{
   return cxxUDOOptLevel.get();
}
//---------------------------------------------------------------------------
static llvm::SmallVector<char, 0> compileModule(llvm::TargetMachine& targetMachine, llvm::Module& module)
// Compile an llvm module to an object file
{
   llvm::legacy::PassManager passManager;

   llvm::SmallVector<char, 0> objectFileBuffer;
   llvm::raw_svector_ostream objectFileStream(objectFileBuffer);
   llvm::raw_pwrite_stream* dwarfStream = nullptr;

   bool isNotSuccessful = targetMachine.addPassesToEmitFile(passManager, objectFileStream, dwarfStream, llvm::CodeGenFileType::CGFT_ObjectFile);
   assert(!isNotSuccessful);
   static_cast<void>(isNotSuccessful);

   passManager.run(module);

   return objectFileBuffer;
}
//---------------------------------------------------------------------------
tl::expected<CxxUDOLLVMFunctions, string> CxxUDOCompiler::preprocessModule()
// Preprocess the llvm module by creating all special extra functions that
// are used by the UDO execution.
{
   auto& analysis = analyzer.getAnalysis();
   auto& module = analyzer.getModule();
   auto& context = module.getContext();

   auto* voidPtr = llvm::Type::getInt8PtrTy(context);

   CxxUDOLLVMFunctions functions{};
   functions.udoFunctorType = llvm::StructType::create(context, {voidPtr, voidPtr}, functorTypeName);
   // functions.globalConstructor is called by the generated function below
   functions.globalDestructor = analysis.globalDestructor;
   functions.constructor = analysis.constructor;
   functions.destructor = analysis.destructor;
   functions.emit = analysis.emit;
   functions.accept = analysis.accept;
   functions.extraWork = analysis.extraWork;
   functions.process = analysis.process;

   // Overwrite the names of the functions so that we can find them again
#define N(name)        \
   if (functions.name) \
      functions.name->setName(name##Name);
   N(globalDestructor)
   N(constructor)
   N(destructor)
   N(emit)
   N(accept)
   N(extraWork)
   N(process)
#undef N

   // Create a global constructor that takes a pointer to struct { int argc; char** argv; } as an argument.
   {
      auto* voidType = llvm::Type::getVoidTy(context);
      auto* i32Type = llvm::Type::getInt32Ty(context);
      auto* constructorArgType = llvm::StructType::create({i32Type, voidPtr}, globalConstructorArgTypeName);

      auto* constructorType = llvm::FunctionType::get(voidType, {constructorArgType->getPointerTo()}, false);
      auto* globalConstructor = llvm::Function::Create(constructorType, llvm::Function::ExternalLinkage, asStringRef(globalConstructorName), module);

      auto* bb = llvm::BasicBlock::Create(context, "init", globalConstructor);
      llvm::IRBuilder<> builder(bb);

      auto* constructorArg = &*globalConstructor->arg_begin();
      constructorArg->setName("constructorArg");

      // Explicitly initialize all global variables again so that the module
      // can be executed multiple times.
      for (auto& globalVar : module.globals())
         if (!globalVar.isConstant() && !globalVar.isExternallyInitialized() && !globalVar.getName().startswith("llvm.") && globalVar.hasDefinitiveInitializer())
            builder.CreateStore(globalVar.getInitializer(), &globalVar);

      // Call __libc_start_main so that the libc works correctly. This is
      // required for all ifuncs to work (so memcpy, memset, etc.) and also for
      // malloc, for example.
      {
         // Signature:
         // using main_fn = int (*)(int argc, char** argv, char** env);
         // int __libc_start_main(
         //     main_fn main,       // The main function that is usually called by __libc_start_main.
         //                         // This is patched out in our libc, so we will set this to nullptr.
         //     int argc,           // Passed to main and other init functions
         //     char** argv,        // Passed to main and other init functions
         //     main_fn init,       // The init function that should be called before main, usually __libc_csu_init
         //     void (*) fini,      // The finish function which is registered with __cxa_atexit, usually __libc_csu_fini
         //     void (*) rtld_fini, // The finish function from the dynamic linker which may be passed
         //                         // by the kernel. We don't need it when statically linking.
         //     void*               // The end of the stack, i.e. its lowest address
         // )
         auto* funcType = llvm::FunctionType::get(i32Type, {voidPtr, i32Type, voidPtr, voidPtr, voidPtr, voidPtr, voidPtr}, false);
         auto* libcStartMain = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, "__libc_start_main", module);

         auto* nullptrValue = llvm::ConstantPointerNull::get(voidPtr);

         auto* argcPtr = builder.CreateConstInBoundsGEP2_32(constructorArgType, constructorArg, 0, 0);
         auto* argc = builder.CreateLoad(i32Type, argcPtr);
         auto* argvPtr = builder.CreateConstInBoundsGEP2_32(constructorArgType, constructorArg, 0, 1);
         auto* argv = builder.CreateLoad(voidPtr, argvPtr);

         builder.CreateCall(libcStartMain, {nullptrValue, argc, argv, nullptrValue, nullptrValue, nullptrValue, nullptrValue});
      }

      // Now call the global constructor of the C++ module if it exists
      if (analysis.globalConstructor)
         builder.CreateCall(analysis.globalConstructor, {});

      builder.CreateRetVoid();

      functions.globalConstructor = globalConstructor;
   }

   // Glibc requires threads to setup the locales, so create a thread
   // initializer function for that.
   {
      auto* voidType = llvm::Type::getVoidTy(context);
      auto* voidFuncType = llvm::FunctionType::get(voidType, {}, false);
      auto* threadInit = llvm::Function::Create(voidFuncType, llvm::Function::ExternalLinkage, asStringRef(threadInitName), module);

      auto* bb = llvm::BasicBlock::Create(context, "init", threadInit);
      llvm::IRBuilder<> builder(bb);

      auto* ctypeInit = llvm::Function::Create(voidFuncType, llvm::Function::ExternalLinkage, "__ctype_init", module);
      builder.CreateCall(ctypeInit);

      builder.CreateRetVoid();

      functions.threadInit = threadInit;
   }

   // Create the emit function so that it calls the functor that contains the
   // generated code for the parent operator
   {
      // Make sure that the function is not duplicated or inlined because we
      // want to do that manually in the CxxUDOLogic
      analysis.emit->addFnAttr(llvm::Attribute::NoDuplicate);
      analysis.emit->addFnAttr(llvm::Attribute::NoInline);

      // Create the global variable that holds the functor
      auto* callbackPtrVar = new llvm::GlobalVariable(module, functions.udoFunctorType, false, llvm::GlobalVariable::ExternalLinkage, nullptr, emitFunctorName);

      auto* bb = llvm::BasicBlock::Create(context, "init", analysis.emit);
      llvm::IRBuilder<> builder(bb);
      builder.SetInsertPoint(bb);

      // Generate the code to call the functor
      llvm::Value* executionState1;
      llvm::Value* executionState2;
      llvm::Value* tuple;
      {
         auto argIt = analysis.emit->arg_begin();
         executionState1 = &*argIt;
         ++argIt;
         executionState2 = &*argIt;
         ++argIt;
         tuple = &*argIt;
         ++argIt;
         assert(argIt == analysis.emit->arg_end());
      }
      auto* functorFuncPtr = builder.CreateConstGEP2_32(functions.udoFunctorType, callbackPtrVar, 0, 0);
      auto* functorFunc = builder.CreateLoad(voidPtr, functorFuncPtr);
      auto* functorFuncType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {functions.udoFunctorType->getPointerTo(), voidPtr, voidPtr, voidPtr}, false);
      builder.CreateCall(functorFuncType, functorFunc, {callbackPtrVar, executionState1, executionState2, tuple});

      builder.CreateRetVoid();

      functions.emitFunctor = callbackPtrVar;
   }

   // Generate the getLocalState function
   if (analysis.getLocalState && analysis.getLocalState->hasNUsesOrMore(1)) {
      auto* bb = llvm::BasicBlock::Create(context, "init", analysis.getLocalState);
      llvm::IRBuilder<> builder(bb);
      auto* executionStateArg = &*analysis.getLocalState->arg_begin();

      assert(analysis.executionState);
      auto* i32Type = llvm::Type::getInt32Ty(context);
      auto* localStatePtrPtr = builder.CreateInBoundsGEP(analysis.executionState, executionStateArg, {llvm::ConstantInt::get(i32Type, 0), llvm::ConstantInt::get(i32Type, 0), llvm::ConstantInt::get(i32Type, 0)});
      auto* localStatePtr = builder.CreateLoad(voidPtr, localStatePtrPtr);

      builder.CreateRet(localStatePtr);
   }

   // Generate the global variables for the functors to the runtime functions
   // and add code to the runtime functions that calls the functors.
   bool printDebugIsUsed = analysis.runtimeFunctions.printDebug->hasNUsesOrMore(2);
   if (!printDebugIsUsed) {
      for (auto* user : analysis.runtimeFunctions.printDebug->users()) {
         if (auto* call = llvm::dyn_cast<llvm::CallBase>(user)) {
            printDebugIsUsed = call->getParent()->getParent()->hasNUsesOrMore(1);
            break;
         }
      }
   }
   if (printDebugIsUsed) {
      // Don't inline the udo runtime functions
      analysis.runtimeFunctions.printDebug->addFnAttr(llvm::Attribute::NoInline);

      auto* functorVar = new llvm::GlobalVariable(module, functions.udoFunctorType, false, llvm::GlobalVariable::ExternalLinkage, nullptr, asStringRef(printDebugFunctorName));
      functions.printDebugFunctor = functorVar;

      auto* printDebugFunc = analysis.runtimeFunctions.printDebug;
      auto* bb = llvm::BasicBlock::Create(context, "init", printDebugFunc);
      llvm::IRBuilder<> builder(bb);
      auto* functorFuncPtr = builder.CreateConstGEP2_32(functions.udoFunctorType, functorVar, 0, 0);
      auto* functorFuncVoidPtr = builder.CreateLoad(voidPtr, functorFuncPtr, "functorPtr");
      auto* functorFuncType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {functions.udoFunctorType->getPointerTo(), voidPtr, llvm::Type::getInt64Ty(context)}, false);
      auto* functorFunc = builder.CreateBitCast(functorFuncVoidPtr, functorFuncType->getPointerTo());
      auto argsIt = printDebugFunc->arg_begin();
      auto msg_arg = &*argsIt;
      ++argsIt;
      auto size_arg = &*argsIt;
      builder.CreateCall(functorFuncType, functorFunc, {functorVar, msg_arg, size_arg});
      builder.CreateRetVoid();
   } else {
      auto userBegin = analysis.runtimeFunctions.printDebug->user_begin();
      auto userEnd = analysis.runtimeFunctions.printDebug->user_end();
      if (userBegin != userEnd) {
         if (auto* inst = llvm::dyn_cast<llvm::Instruction>(*userBegin)) {
            auto* func = inst->getParent()->getParent();
            func->eraseFromParent();
         }
      }
      analysis.runtimeFunctions.printDebug->eraseFromParent();
      analysis.runtimeFunctions.printDebug = nullptr;
   }

   if (analysis.runtimeFunctions.getRandom->hasNUsesOrMore(1)) {
      // Don't inline the udo runtime functions
      analysis.runtimeFunctions.getRandom->addFnAttr(llvm::Attribute::NoInline);

      auto* functorVar = new llvm::GlobalVariable(module, functions.udoFunctorType, false, llvm::GlobalVariable::ExternalLinkage, nullptr, asStringRef(getRandomFunctorName));
      functions.getRandomFunctor = functorVar;

      auto* getRandomFunc = analysis.runtimeFunctions.getRandom;
      auto* bb = llvm::BasicBlock::Create(context, "init", getRandomFunc);
      llvm::IRBuilder<> builder(bb);
      auto* functorFuncPtr = builder.CreateConstGEP2_32(functions.udoFunctorType, functorVar, 0, 0);
      auto* functorFuncVoidPtr = builder.CreateLoad(voidPtr, functorFuncPtr, "functorPtr");
      auto* functorFuncType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {functions.udoFunctorType->getPointerTo()}, false);
      auto* functorFunc = builder.CreateBitCast(functorFuncVoidPtr, functorFuncType->getPointerTo());
      auto* retValue = builder.CreateCall(functorFuncType, functorFunc, {functorVar});
      builder.CreateRet(retValue);
   } else {
      analysis.runtimeFunctions.getRandom->eraseFromParent();
      analysis.runtimeFunctions.getRandom = nullptr;
   }

   {
      llvm_metadata::MetadataWriter writer(module);
      if (auto result = writer.writeNamedValue("udo.CxxUDO.LLVMFunctions"sv, functions); !result)
         return tl::unexpected(move(result).error());
   }

   // Make sure that all global values that we need have external linkage so
   // that they are not removed by the optimizer
   functions.mapGlobalValues([&](auto* globalValue) {
      if (!globalValue)
         return;

      globalValue->setLinkage(llvm::GlobalValue::ExternalLinkage);
   });

   ClangCompiler::optimizeModule(module, cxxUDOOptLevel.get());

   return functions;
}
//---------------------------------------------------------------------------
tl::expected<vector<char>, string> CxxUDOCompiler::compile()
// Compile the UDO to machine code.
{
   auto& module = analyzer.getModule();

   llvm::EngineBuilder builder;
   auto targetMachinePtr = LLVMCompiler::setupBuilder(builder, false).setRelocationModel(llvm::Reloc::PIC_).setCodeModel(llvm::CodeModel::Small).selectTarget();
   unique_ptr<remove_pointer_t<decltype(targetMachinePtr)>> targetMachine(targetMachinePtr);

   if (targetMachine->getTargetTriple().getArch() != llvm::Triple::x86_64)
      return tl::unexpected(string(tr(tc, "C++ UDOs are only supported for x86_64")));

   if (dumpCxxUDOObject) {
      error_code ec;
      llvm::raw_fd_ostream fstream("cxxudo-dump.ll", ec);
      module.print(fstream, nullptr, false, true);
   }

   assert(!llvm::verifyModule(module));

   auto objectFile = compileModule(*targetMachine, module);

   if (dumpCxxUDOObject) {
      string_view objectFileSv = {objectFile.data(), objectFile.size()};
      ofstream fstream("cxxudo-dump.o");
      fstream << objectFileSv;
   }

   vector<char> objectFileVec(objectFile.begin(), objectFile.end());

   return objectFileVec;
}
//---------------------------------------------------------------------------
namespace llvm_metadata {
//---------------------------------------------------------------------------
#define TRY(x) \
   if (auto result = (x); !result) return result;
//---------------------------------------------------------------------------
IOResult IO<CxxUDOLLVMFunctions>::enumEntries(StructContext& context, CxxUDOLLVMFunctions& value) {
   TRY(mapMember(context, value.globalConstructor));
   TRY(mapMember(context, value.globalDestructor));
   TRY(mapMember(context, value.threadInit));
   TRY(mapMember(context, value.emit));
   TRY(mapMember(context, value.emitFunctor));
   TRY(mapMember(context, value.printDebugFunctor));
   TRY(mapMember(context, value.getRandomFunctor));
   TRY(mapMember(context, value.constructor));
   TRY(mapMember(context, value.destructor));
   TRY(mapMember(context, value.accept));
   TRY(mapMember(context, value.extraWork));
   TRY(mapMember(context, value.process));
   return {};
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
