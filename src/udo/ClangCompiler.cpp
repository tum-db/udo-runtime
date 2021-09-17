#include "udo/ClangCompiler.hpp"
#include "udo/CxxUDOConfig.hpp"
#include "udo/LLVMCompiler.hpp"
#include "udo/ScopeGuard.hpp"
#include "udo/Setting.hpp"
#include "udo/UDORuntime.hpp"
#include <clang/Basic/Diagnostic.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/FrontendOptions.h>
#include <clang/Frontend/TextDiagnosticBuffer.h>
#include <clang/Frontend/Utils.h>
#include <clang/Lex/HeaderSearchOptions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Host.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <new>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
using namespace std;
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
static Setting<bool> printCxxUDOWarnings("printCxxUDOWarnings", "Print warnings of C++ UDO compilation", false);
static Setting<string> cxxUDOClangxx("cxxUDOClangxx", "Path to clang++ to be used by C++ UDOs", {});
//---------------------------------------------------------------------------
static string_view getClangxx()
// Get the path to clang++ (either from the cxxUDOClangxx setting or the default)
{
   auto& clangxxFromSetting = cxxUDOClangxx.get();
   if (clangxxFromSetting.empty())
      return cxxUDODefaultClangxx;
   else
      return clangxxFromSetting;
}
//---------------------------------------------------------------------------
ClangCompiler::ClangCompiler(string_view source, unsigned optimizationLevel)
   : optimizationLevel(optimizationLevel)
// Constructor
{
   addVirtualFile("/tmp/udo-runtime/udo/UDOperator.hpp"sv, cxxUDOHeader);
   addVirtualFile("/tmp/udo.cpp"sv, source);
}
//---------------------------------------------------------------------------
void ClangCompiler::addVirtualFile(string_view path, string_view source)
// Add a virtual file
{
   virtualFiles.push_back({path, source});
}
//---------------------------------------------------------------------------
void ClangCompiler::addFrontendAction(clang::FrontendAction* action)
// Add a frontend action
{
   frontendActions.push_back(action);
}
//---------------------------------------------------------------------------
static constexpr array cxxUDOBaseFlags = {"-xc++", "-std=c++20", "-nostdinc++", "-fPIC", "-ftls-model=initial-exec", "-march=native", "-fno-exceptions", "-Wall", "-Wextra"};
//---------------------------------------------------------------------------
static vector<const char*> getClangCmdline(unsigned optimizationLevel)
// Get the command line to the clang invocation
{
   vector<const char*> clangCmdline;
   clangCmdline.push_back(getClangxx().data());
   clangCmdline.insert(clangCmdline.end(), cxxUDOBaseFlags.begin(), cxxUDOBaseFlags.end());
   if (optimizationLevel == 0)
      clangCmdline.push_back("-Werror");
   if (optimizationLevel >= 2)
      clangCmdline.push_back("-DNDEBUG");
   switch (optimizationLevel) {
      case 0:
         clangCmdline.push_back("-O0");
         break;
      case 1:
         clangCmdline.push_back("-O1");
         break;
      case 2:
         clangCmdline.push_back("-O2");
         break;
      default:
         clangCmdline.push_back("-O3");
         break;
   }
   clangCmdline.push_back("/tmp/udo.cpp");

   return clangCmdline;
}
//---------------------------------------------------------------------------
tl::expected<void, string> ClangCompiler::compile()
// Compile the file
{
   LLVMCompiler::initializeLLVM();

   auto invocation = clang::createInvocationFromCommandLine(getClangCmdline(optimizationLevel));

   auto& frontendOpts = invocation->getFrontendOpts();
   // createInvocationFromCommandLine sets DisableFree to true which then
   // obviously leads to memory leaks. So, set it to false again here.
   frontendOpts.DisableFree = false;

   auto& headerOpts = invocation->getHeaderSearchOpts();
   {
      // We provide libc and libc++ so make sure that its headers are used
      vector<clang::HeaderSearchOptions::Entry> entries;
      entries.push_back({string(cxxUDODepsPrefix) + "/include/c++/v1", clang::frontend::System, 0, 0});
      entries.push_back({string(cxxUDODepsPrefix) + "/include", clang::frontend::System, 0, 0});
      headerOpts.UserEntries.insert(headerOpts.UserEntries.begin(), entries.begin(), entries.end());
   }
   headerOpts.AddPath("/tmp/udo-runtime", clang::frontend::Angled, false, false);

   auto& codegenOpts = invocation->getCodeGenOpts();
   codegenOpts.DiscardValueNames = false;

   auto& diagnosticOpts = invocation->getDiagnosticOpts();
   diagnosticOpts.ShowCarets = false;
   diagnosticOpts.ShowFixits = false;

   auto diagnosticPtr = std::make_unique<clang::TextDiagnosticBuffer>();
   auto& diagnostic = *diagnosticPtr;

   clang::CompilerInstance compiler;
   shared_ptr<clang::CompilerInvocation> invocationShared(invocation.release());
   compiler.setInvocation(move(invocationShared));
   compiler.createDiagnostics(diagnosticPtr.release());

   createVirtualFiles(compiler);

   if (!compiler.hasSourceManager()) {
      if (!compiler.hasFileManager())
         compiler.createFileManager();
      compiler.createSourceManager(compiler.getFileManager());
   }

   for (auto* action : frontendActions) {
      if (!compiler.ExecuteAction(*action)) {
         auto& error = *diagnostic.err_begin();
         std::string message;
         llvm::raw_string_ostream message_stream{message};
         error.first.print(message_stream, compiler.getSourceManager());
         message_stream << ": " << error.second;
         message_stream.str();
         return tl::unexpected(std::move(message));
      }

      if (printCxxUDOWarnings) {
         if (diagnostic.getNumWarnings() > 0) {
            llvm::errs() << "===============================\n"
                            "== udo compilation warnings: ==\n"
                            "===============================\n";
            for (auto it = diagnostic.warn_begin(); it != diagnostic.warn_end(); ++it) {
               it->first.print(llvm::errs(), compiler.getSourceManager());
               llvm::errs() << ": " << it->second << '\n';
            }
         }
      }
   }

   return {};
}
//---------------------------------------------------------------------------
/// Convert a string_view to a llvm::StringRef
static llvm::StringRef toStringRef(string_view sv) {
   return {sv.data(), sv.size()};
}
//---------------------------------------------------------------------------
void ClangCompiler::createVirtualFiles(clang::CompilerInstance& compiler)
// Create the virtual files in the compiler instance
{
   llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> memFilesystem(new llvm::vfs::InMemoryFileSystem);
   for (auto& file : virtualFiles) {
      auto pathRef = toStringRef(file.path);
      auto sourceBuffer = llvm::MemoryBuffer::getMemBuffer(toStringRef(file.source), pathRef);
      memFilesystem->addFile(pathRef, time(nullptr), move(sourceBuffer));
   }

   llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> overlayFs(new llvm::vfs::OverlayFileSystem(llvm::vfs::getRealFileSystem()));
   overlayFs->pushOverlay(memFilesystem);
   auto* fileManager = compiler.createFileManager(overlayFs);
   assert(fileManager);
   static_cast<void>(fileManager);
}
//---------------------------------------------------------------------------
void ClangCompiler::optimizeModule(llvm::Module& module, unsigned optimizationLevel)
// Run the optimizations on the module that clang would use
{
   llvm::PassManagerBuilder builder;
   builder.OptLevel = optimizationLevel;

   builder.Inliner = llvm::createFunctionInliningPass(builder.OptLevel, 0, false);

   llvm::legacy::FunctionPassManager functionPassManager(&module);
   llvm::legacy::PassManager modulePassManager;

   builder.populateFunctionPassManager(functionPassManager);
   builder.populateModulePassManager(modulePassManager);

   functionPassManager.doInitialization();
   for (auto& func : module)
      if (!func.isDeclaration())
         functionPassManager.run(func);
   functionPassManager.doFinalization();

   modulePassManager.run(module);
}
//---------------------------------------------------------------------------
/// The impl that contains the Driver
struct ClangCompiler::CompilationWrapper::Impl {
   /// The diagnostics engine
   clang::DiagnosticsEngine diagnosticsEngine;
   /// The driver
   clang::driver::Driver driver;
};
//---------------------------------------------------------------------------
ClangCompiler::CompilationWrapper::CompilationWrapper()
// Constructor
{
}
//---------------------------------------------------------------------------
// Move constructor
ClangCompiler::CompilationWrapper::CompilationWrapper(CompilationWrapper&& c) = default;
//---------------------------------------------------------------------------
// Move assignment
ClangCompiler::CompilationWrapper& ClangCompiler::CompilationWrapper::operator=(CompilationWrapper&& c) = default;
//---------------------------------------------------------------------------
ClangCompiler::CompilationWrapper::~CompilationWrapper()
// Destructor
{
}
//---------------------------------------------------------------------------
ClangCompiler::CompilationWrapper ClangCompiler::createCompilation(unsigned optimizationLevel)
// Create a clang compilation with the options used for C++ UDOs
{
   LLVMCompiler::initializeLLVM();

   // Construct the impl manually in-place by allocating the storage first and
   // then using placement new
   auto* implStorage = ::operator new(sizeof(CompilationWrapper::Impl));
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
   ScopeGuard deallocateImpl([implStorage] { ::operator delete(implStorage); });
#pragma GCC diagnostic pop
   auto* implPtr = static_cast<CompilationWrapper::Impl*>(implStorage);

   llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> diagnosticIds(new clang::DiagnosticIDs);
   llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> diagnosticOptions(new clang::DiagnosticOptions);
   new (&implPtr->diagnosticsEngine) clang::DiagnosticsEngine(move(diagnosticIds), move(diagnosticOptions), new clang::IgnoringDiagConsumer);
   // In case any of the following functions throw, the DiagnosticsEngine must
   // be destructed again
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
   ScopeGuard destructDiagnosticsEngine([implPtr] { implPtr->diagnosticsEngine.~DiagnosticsEngine(); });
#pragma GCC diagnostic pop

   auto targetTriple = llvm::sys::getDefaultTargetTriple();
   new (&implPtr->driver) clang::driver::Driver(toStringRef(getClangxx()), targetTriple, implPtr->diagnosticsEngine);

   // Impl is now constructed properly, so make a unique_ptr<Impl> out of it
   destructDiagnosticsEngine.dismiss();
   deallocateImpl.dismiss();
   unique_ptr<CompilationWrapper::Impl> impl(implPtr);

   CompilationWrapper wrapper;
   wrapper.impl = move(impl);

   auto clangPath = string(getClangxx());
   auto compilationPtr = wrapper.impl->driver.BuildCompilation(getClangCmdline(optimizationLevel));
   wrapper.compilation.reset(compilationPtr);

   return wrapper;
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
