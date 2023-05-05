#ifndef H_udo_ClangCompiler
#define H_udo_ClangCompiler
//---------------------------------------------------------------------------
#include "thirdparty/tl/expected.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <vector>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
namespace clang {
class CompilerInstance;
class FrontendAction;
namespace driver {
class Compilation;
}
}
namespace llvm {
class Module;
}
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
/// A helper class to compile C++ UDOs by using the clang C++ library
class ClangCompiler {
   private:
   /// A virtual file
   struct VirtualFile {
      std::string path;
      std::string_view source;
   };

   /// The optimization level
   unsigned optimizationLevel;
   /// The virtual files
   std::vector<VirtualFile> virtualFiles;
   /// The compiler frontend actions
   std::vector<clang::FrontendAction*> frontendActions;

   /// Create the virtual files in the compiler instance
   void createVirtualFiles(clang::CompilerInstance& compiler);

   public:
   /// Constructor
   ClangCompiler(std::string_view source, unsigned optimizationLevel = 3);

   /// Add a virtual file
   void addVirtualFile(std::string path, std::string_view source);
   /// Add a frontend action
   void addFrontendAction(clang::FrontendAction* action);
   /// Compile the file
   [[nodiscard]] tl::expected<void, std::string> compile();

   /// A wrapper for a clang Compilation that owns the Driver which in turns
   /// owns the Compilation object
   class CompilationWrapper {
      private:
      friend ClangCompiler;

      struct Impl;
      /// The impl that contains the Driver
      std::unique_ptr<Impl> impl;
      /// The clang compilation
      std::unique_ptr<clang::driver::Compilation> compilation;

      /// Constructor
      CompilationWrapper();

      public:
      /// Move constructor
      CompilationWrapper(CompilationWrapper&& c) noexcept;
      /// Move assignment
      CompilationWrapper& operator=(CompilationWrapper&& c) noexcept;
      /// Destructor
      ~CompilationWrapper();

      /// Get the compilation
      clang::driver::Compilation& operator*() const { return *compilation; }
      /// Get the compilation
      clang::driver::Compilation* operator->() const { return compilation.get(); }
   };

   /// Run the optimizations on the module that clang would use
   static void optimizeModule(llvm::Module& module, unsigned optimizationLevel = 3);
   /// Create a clang compilation with the options used for C++ UDOs
   static CompilationWrapper createCompilation(unsigned optimizationLevel = 3);
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
