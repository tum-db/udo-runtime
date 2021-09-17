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
static void inlineConsume(llvm::CallBase* call)
// Inline a function and mark the original as unreachable
{
   auto* func = call->getParent()->getParent();
   auto* calleeFunc = call->getCalledFunction();
   auto& context = func->getParent()->getContext();
   llvm::InlineFunctionInfo inlineInfo;
   llvm::InlineFunction(*call, inlineInfo);
   calleeFunc->dropAllReferences();
   auto* bb = llvm::BasicBlock::Create(context, "unreachable", calleeFunc);
   llvm::IRBuilder<> builder(bb);
   builder.CreateUnreachable();
}
//---------------------------------------------------------------------------
static llvm::Function* wrapWithNewName(llvm::Function* func, string_view newName)
// Create a wrapper function with external linkage with the new name that just
// calls the given function. Returns nullptr if func is nullptr.
{
   if (!func)
      return nullptr;

   auto* module = func->getParent();
   auto& context = module->getContext();
   auto* funcType = func->getFunctionType();

   auto* newFunc = llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, asStringRef(newName), *module);
   newFunc->setAttributes(func->getAttributes());

   auto* bb = llvm::BasicBlock::Create(context, "init", newFunc);
   llvm::IRBuilder<> builder(bb);

   vector<llvm::Value*> args;
   args.reserve(funcType->getNumParams());

   for (auto& arg : newFunc->args())
      args.push_back(&arg);

   auto* call = builder.CreateCall(func, args);

   if (funcType->getReturnType()->isVoidTy())
      builder.CreateRetVoid();
   else
      builder.CreateRet(call);

   return newFunc;
}

//---------------------------------------------------------------------------
static llvm::Function* patchFunction(llvm::Function* func, string_view newName)
// Patch the function so that it takes the global and local states as the
// first two arguments
{
   auto& context = func->getContext();
   auto* voidPtr = llvm::Type::getInt8PtrTy(context);
   auto* funcType = func->getFunctionType();
   vector<llvm::Type*> patchedFunctionParamTypes;
   patchedFunctionParamTypes.reserve(funcType->getNumParams() + 2);
   patchedFunctionParamTypes.push_back(voidPtr);
   patchedFunctionParamTypes.push_back(voidPtr);
   for (auto* paramType : funcType->params())
      patchedFunctionParamTypes.push_back(paramType);
   assert(!funcType->isVarArg());
   auto* patchedFuncType = llvm::FunctionType::get(funcType->getReturnType(), patchedFunctionParamTypes, false);

   auto* module = func->getParent();
   llvm::Function* patchedFunc;

   patchedFunc = llvm::Function::Create(patchedFuncType, func->getLinkage(), asStringRef(newName), module);

   patchedFunc->setAttributes(func->getAttributes());
   auto argIt = patchedFunc->arg_begin();
   auto argEnd = patchedFunc->arg_end();
   argIt->setName("globalState");
   ++argIt;
   argIt->setName("localState");
   ++argIt;
   auto* bb = llvm::BasicBlock::Create(context, "init", patchedFunc);
   llvm::IRBuilder<> builder(bb);
   vector<llvm::Value*> args;
   args.reserve(argEnd - argIt);
   for (; argIt != argEnd; ++argIt)
      args.push_back(&*argIt);
   auto* call = builder.CreateCall(funcType, func, args);
   if (funcType->getReturnType()->isVoidTy())
      builder.CreateRetVoid();
   else
      builder.CreateRet(call);

   inlineConsume(call);

   return patchedFunc;
};
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
   functions.globalDestructor = wrapWithNewName(analysis.globalDestructor, globalDestructorName);
   functions.constructor = wrapWithNewName(analysis.constructor, constructorName);
   functions.destructor = wrapWithNewName(analysis.destructor, destructorName);
   functions.extraWork = wrapWithNewName(analysis.extraWork, extraWorkName);

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
         auto* argc = builder.CreateLoad(argcPtr);
         auto* argvPtr = builder.CreateConstInBoundsGEP2_32(constructorArgType, constructorArg, 0, 1);
         auto* argv = builder.CreateLoad(argvPtr);

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

   if (analysis.consume)
      functions.consume = patchFunction(analysis.consume, consumeName);
   if (analysis.postProduce)
      functions.postProduce = patchFunction(analysis.postProduce, postProduceName);

   // Patch the produceOutputTuple function
   llvm::GlobalVariable* globalStateDummyValue;
   llvm::GlobalVariable* localStateDummyValue;
   {
      // Make sure that the function is not duplicated or inlined
      analysis.produceOutputTuple->addFnAttr(llvm::Attribute::NoDuplicate);
      analysis.produceOutputTuple->addFnAttr(llvm::Attribute::NoInline);

      // Create the global variable that holds the functor
      auto* callbackPtrVar = new llvm::GlobalVariable(module, functions.udoFunctorType, false, llvm::GlobalVariable::ExternalLinkage, nullptr, asStringRef(produceOutputTupleFunctorName));

      auto* bb = llvm::BasicBlock::Create(context, "init", analysis.produceOutputTuple);
      llvm::IRBuilder<> builder(bb);
      globalStateDummyValue = new llvm::GlobalVariable(module, llvm::Type::getInt8Ty(context), false, llvm::GlobalVariable::PrivateLinkage, nullptr, "globalStateDummy");
      localStateDummyValue = new llvm::GlobalVariable(module, llvm::Type::getInt8Ty(context), false, llvm::GlobalVariable::PrivateLinkage, nullptr, "localStateDummy");

      // Generate the code to call the functor
      builder.SetInsertPoint(bb);
      auto* functorFuncPtr = builder.CreateConstGEP2_32(functions.udoFunctorType, callbackPtrVar, 0, 0);
      auto* functorFuncVoidPtr = builder.CreateLoad(functorFuncPtr, "functorPtr");
      auto* functorFuncType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {functions.udoFunctorType->getPointerTo(), voidPtr, voidPtr, voidPtr}, false);
      auto* functorFunc = builder.CreateBitCast(functorFuncVoidPtr, functorFuncType->getPointerTo());
      auto* tupleArg = builder.CreateBitCast(&*analysis.produceOutputTuple->arg_begin(), voidPtr);
      builder.CreateCall(functorFuncType, functorFunc, {callbackPtrVar, globalStateDummyValue, localStateDummyValue, tupleArg});
      builder.CreateRetVoid();

      functions.produceOutputTupleFunctor = callbackPtrVar;
   }

   {
      auto* patchedFunc = patchFunction(analysis.produceOutputTuple, produceOutputTupleName);
      auto argsIt = patchedFunc->arg_begin();
      llvm::Value* globalState = &*argsIt;
      ++argsIt;
      llvm::Value* localState = &*argsIt;
      globalStateDummyValue->replaceAllUsesWith(globalState);
      localStateDummyValue->replaceAllUsesWith(localState);
      globalStateDummyValue->eraseFromParent();
      localStateDummyValue->eraseFromParent();

      assert(analysis.produceOutputTuple->getNumUses() <= 1);
      if (!analysis.produceOutputTuple->user_empty()) {
         auto* call = llvm::cast<llvm::CallInst>(analysis.produceOutputTuple->user_back());
         auto* func = call->getParent()->getParent();
         vector<llvm::Value*> newCallArgs;
         newCallArgs.reserve(call->arg_size() + 2);
         auto argsIt = func->arg_begin();
         llvm::Value* globalState = &*argsIt;
         ++argsIt;
         llvm::Value* localState = &*argsIt;
         newCallArgs.push_back(globalState);
         newCallArgs.push_back(localState);
         for (auto& arg : call->args()) {
            newCallArgs.push_back(arg.get());
         }
         auto* newCall = llvm::CallInst::Create(patchedFunc->getFunctionType(), patchedFunc, newCallArgs);
         call->replaceAllUsesWith(newCall);
         newCall->insertAfter(call);
         call->eraseFromParent();
      }

      functions.produceOutputTuple = patchedFunc;
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
      auto* functorFuncVoidPtr = builder.CreateLoad(functorFuncPtr, "functorPtr");
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
      auto* functorFuncVoidPtr = builder.CreateLoad(functorFuncPtr, "functorPtr");
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

   auto targetMachinePtr = LLVMCompiler::setupBuilder(llvm::EngineBuilder{}, false).setRelocationModel(llvm::Reloc::PIC_).setCodeModel(llvm::CodeModel::Small).selectTarget();
   unique_ptr<remove_pointer_t<decltype(targetMachinePtr)>> targetMachine(targetMachinePtr);

   if (targetMachine->getTargetTriple().getArch() != llvm::Triple::x86_64)
      return tl::unexpected(tr(tc, "C++ UDOs are only supported for x86_64"));

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
   TRY(mapMember(context, value.produceOutputTuple));
   TRY(mapMember(context, value.produceOutputTupleFunctor));
   TRY(mapMember(context, value.printDebugFunctor));
   TRY(mapMember(context, value.getRandomFunctor));
   TRY(mapMember(context, value.constructor));
   TRY(mapMember(context, value.destructor));
   TRY(mapMember(context, value.consume));
   TRY(mapMember(context, value.extraWork));
   TRY(mapMember(context, value.postProduce));
   return {};
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
