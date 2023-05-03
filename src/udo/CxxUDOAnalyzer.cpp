#include "udo/CxxUDOAnalyzer.hpp"
#include "udo/ClangCompiler.hpp"
#include "udo/LLVMMetadata.hpp"
#include "udo/UDORuntime.hpp"
#include "udo/i18n.hpp"
#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/CXXInheritance.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclGroup.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/DeclarationName.h>
#include <clang/AST/GlobalDecl.h>
#include <clang/AST/TemplateBase.h>
#include <clang/AST/Type.h>
#include <clang/Basic/ABI.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/OperatorKinds.h>
#include <clang/CodeGen/CodeGenABITypes.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Sema/Sema.h>
#include <clang/Sema/SemaConsumer.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
using namespace std;
using namespace std::literals::string_view_literals;
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
static const char tc[] = "udo/CxxUDOAnalyzer";
//---------------------------------------------------------------------------
namespace {
//---------------------------------------------------------------------------
namespace clang_utils {
//---------------------------------------------------------------------------
string_view getName(const clang::NamedDecl* decl)
// Get the name of a decl as a string_view
{
   auto declName = decl->getDeclName();
   if (declName.isEmpty()) {
      return {};
   } else if (declName.isIdentifier()) {
      auto* identifier = declName.getAsIdentifierInfo();
      return {identifier->getNameStart(), identifier->getLength()};
   }
   return {};
}
//---------------------------------------------------------------------------
bool hasNestedName(const clang::CXXRecordDecl* decl, span<const string_view> name)
// Does the declaration have the given fully-qualified name?
{
   if (name.empty())
      return false;
   auto it = name.rbegin();
   if (getName(decl) != *it)
      return false;
   ++it;
   auto* ns = decl->getDeclContext();
   auto end = name.rend();
   for (; it != end && !ns->isTranslationUnit(); ++it) {
      auto* namedDecl = llvm::dyn_cast<clang::NamedDecl>(ns);
      if (!namedDecl)
         return false;
      if (getName(namedDecl) != *it)
         return false;
      ns = ns->getParent();
   }
   return it == end;
}
//---------------------------------------------------------------------------
bool isInNamespace(const clang::Decl* decl, const clang::NamespaceDecl* ns)
// Is the declaration in the given namespace?
{
   auto* parentNs = llvm::dyn_cast<const clang::NamespaceDecl>(decl->getDeclContext());
   return parentNs->getCanonicalDecl() == ns->getCanonicalDecl();
}
//---------------------------------------------------------------------------
bool isUIntType(const clang::ASTContext& context, clang::QualType type, unsigned numBits)
// Is the type an unsigned integer type with the specified number of bits?
{
   // We don't care about qualifiers here, we don't differentiate between const
   // uint32_t and uint32_t.
   if (!type->isUnsignedIntegerType())
      return false;

   if (context.getTypeSize(type) != numBits)
      return false;

   return true;
}
//---------------------------------------------------------------------------
bool isConstLvalueRefTo(clang::QualType refType, const clang::Type* expectedType)
// Is `refType` a const lvalue reference to `expectedType`?
{
   auto* lvalueRefType = refType->getAs<clang::LValueReferenceType>();
   if (!lvalueRefType)
      return false;

   auto pointeeType = lvalueRefType->getPointeeType();
   if (!pointeeType.isConstQualified())
      return false;

   if (pointeeType.getCanonicalType().getTypePtr() != expectedType)
      return false;

   return true;
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
template <typename T>
string err(T msg)
// Generate a RuntimeMessage with the given message
{
   return string(move(msg));
}
//---------------------------------------------------------------------------
class CxxUDOClangConsumer : public clang::SemaConsumer {
   private:
   /// The components (i.e. namespaces) of the name of the UDO subclass
   vector<string_view> udoName;
   /// The clang codegen
   unique_ptr<clang::CodeGenerator> codegen;
   /// Inline functions that are delayed and must still be passed to the codegen
   vector<clang::FunctionDecl*> delayedInlineFunctions;
   /// The error message (if an error occurred)
   optional<string> error;
   /// The declaration of the udo namespace
   clang::NamespaceDecl* udoNamespace = nullptr;

   public:
   /// The ast context
   clang::ASTContext* astContext = nullptr;
   /// The clang Sema
   clang::Sema* sema = nullptr;
   /// The printDebug function
   clang::FunctionDecl* printDebug = nullptr;
   /// The getRandom function
   clang::FunctionDecl* getRandom = nullptr;
   /// The String class
   clang::CXXRecordDecl* stringType = nullptr;
   /// The ExecutionState class
   clang::CXXRecordDecl* executionState = nullptr;
   /// The getThreadId function of ExecutionState
   clang::CXXMethodDecl* getThreadId = nullptr;
   /// The getLocalState function of ExecutionState
   clang::CXXMethodDecl* getLocalState = nullptr;
   /// The UDOperator (base) class template
   clang::CXXRecordDecl* udOperatorClass = nullptr;
   /// The emit() function template declaration of the UDOperator class
   clang::FunctionTemplateDecl* udOperatorEmit = nullptr;
   /// The subclass of UDOperator written by the user
   clang::CXXRecordDecl* udOperatorSubclass = nullptr;
   /// The InputTuple member type of the UDO subclass
   clang::CXXRecordDecl* inputTupleClass = nullptr;
   /// The OutputTuple member type of the UDO subclass
   clang::CXXRecordDecl* outputTupleClass = nullptr;
   /// The emit() template specialization for the UDO subclass
   clang::CXXMethodDecl* emit;
   /// The constructor of the subclass
   clang::CXXConstructorDecl* constructor = nullptr;
   /// The destructor of the subclass
   clang::CXXDestructorDecl* destructor = nullptr;
   /// The accept function of the subclass
   clang::CXXMethodDecl* accept = nullptr;
   /// The extraWork function of the subclass
   clang::CXXMethodDecl* extraWork = nullptr;
   /// The process function of the subclass
   clang::CXXMethodDecl* process = nullptr;
   /// The global constructor (for static initialization)
   llvm::Function* globalConstructor = nullptr;
   /// The global destructor (for static initialization)
   llvm::Function* globalDestructor = nullptr;

   /// Constructor
   CxxUDOClangConsumer(llvm::LLVMContext& context, clang::CompilerInstance& compiler, llvm::StringRef moduleName, string_view udoName) {
      while (!udoName.empty()) {
         auto pos = udoName.find("::"sv);
         if (pos == string_view::npos) {
            this->udoName.push_back(udoName);
            break;
         }
         this->udoName.push_back(udoName.substr(0, pos));
         udoName = udoName.substr(pos + 2);
      }
      assert(compiler.hasDiagnostics() && compiler.hasInvocation());
      auto& invocation = compiler.getInvocation();
      codegen = unique_ptr<clang::CodeGenerator>(clang::CreateLLVMCodeGen(compiler.getDiagnostics(), moduleName, nullptr, invocation.getHeaderSearchOpts(), invocation.getPreprocessorOpts(), invocation.getCodeGenOpts(), context));
   }

   /// Destructor
   ~CxxUDOClangConsumer() override {}

   /// Take the error message
   optional<string> takeError() {
      return move(error);
   }

   private:
   /// Get the llvm type of a clang type
   llvm::Type* getType(clang::QualType type) {
      auto& cgm = codegen->CGM();
      return clang::CodeGen::convertTypeForMemory(cgm, type);
   }

   /// Get the llvm type of a clang decl
   llvm::Type* getType(const clang::TypeDecl* decl) {
      return getType(clang::QualType{decl->getTypeForDecl(), 0});
   }

   /// Resolve a function which could be an alias
   static llvm::Function* resolveFunction(llvm::Constant* constant) {
      llvm::Function* func;
      if (auto* alias = llvm::dyn_cast<llvm::GlobalAlias>(constant); alias) {
         func = llvm::cast<llvm::Function>(alias->getAliasee());
      } else {
         func = llvm::cast<llvm::Function>(constant);
      }
      if (func->empty()) {
         return nullptr;
      } else {
         return func;
      }
   }

   /// Resolve a structor
   template <typename D, typename T>
   llvm::Function* resolveStructor(D* decl, std::initializer_list<T> structorTypes) {
      for (auto structor_type : structorTypes) {
         clang::GlobalDecl globalDecl(decl, structor_type);
         auto* constant = codegen->GetAddrOfGlobal(globalDecl, false);
         if (auto* func = resolveFunction(constant); func != nullptr) {
            return func;
         }
      }
      return nullptr;
   }

   /// Get the llvm function for a constructor
   llvm::Function* getFunction(clang::CXXConstructorDecl* decl) {
      return resolveStructor(decl, {clang::Ctor_Complete, clang::Ctor_Base});
   }

   /// Get the llvm function for a destructor
   llvm::Function* getFunction(clang::CXXDestructorDecl* decl) {
      return resolveStructor(decl, {clang::Dtor_Complete, clang::Dtor_Base});
   }

   /// Get the llvm function for a function decl
   llvm::Function* getFunction(clang::FunctionDecl* decl) {
      auto* constant = codegen->GetAddrOfGlobal(clang::GlobalDecl{decl}, false);
      return llvm::cast<llvm::Function>(constant);
   }

   public:
   /// Get the runtime functions
   CxxUDORuntimeFunctions getRuntimeFunctions() {
      CxxUDORuntimeFunctions runtimeFunctions{};
      if (printDebug)
         runtimeFunctions.printDebug = getFunction(printDebug);
      if (getRandom)
         runtimeFunctions.getRandom = getFunction(getRandom);
      return runtimeFunctions;
   }

   /// Get the string type
   llvm::Type* getStringType() {
      if (stringType)
         return getType(stringType);
      return nullptr;
   }

   /// Get the ExecutionState type
   llvm::Type* getExecutionState() {
      if (executionState)
         return getType(executionState);
      return nullptr;
   }

   /// Get the ExecutionState::getThreadId() function
   llvm::Function* getGetThreadId() {
      if (getThreadId)
         return getFunction(getThreadId);
      return nullptr;
   }

   /// Get the ExecutionState::getLocalState() function
   llvm::Function* getGetLocalState() {
      if (getLocalState)
         return getFunction(getLocalState);
      return nullptr;
   }

   /// Get the output attributes
   llvm::SmallVector<CxxUDOOutput, 8> getOutput() {
      llvm::SmallVector<CxxUDOOutput, 8> output;
      if (!outputTupleClass)
         return output;
      for (auto* member : outputTupleClass->fields())
         output.push_back({string(clang_utils::getName(member)), getType(member->getType())});
      return output;
   }

   /// Get the llvm type of udo::InputTuple
   llvm::Type* getInputTupleClassType() {
      if (inputTupleClass)
         return getType(inputTupleClass);
      return nullptr;
   }

   /// Get the llvm type of udo::OutputTuple
   llvm::Type* getOutputTupleClassType() {
      if (outputTupleClass)
         return getType(outputTupleClass);
      return nullptr;
   }

   /// Get the emit() function specialization called by the UDO subclass
   llvm::Function* getEmit() {
      if (udOperatorEmit)
         return getFunction(emit);
      return nullptr;
   }

   /// Get the constructor of the UDO subclass
   llvm::Function* getConstructor() {
      if (constructor)
         return getFunction(constructor);
      return nullptr;
   }

   /// Get the destructor of the UDO subclass
   llvm::Function* getDestructor() {
      if (destructor)
         return getFunction(destructor);
      return nullptr;
   }

   /// Get the accept function of the UDO subclass
   llvm::Function* getAccept() {
      if (accept)
         return getFunction(accept);
      return nullptr;
   }

   /// Get the extraWork function of the UDO subclass
   llvm::Function* getExtraWork() {
      if (extraWork)
         return getFunction(extraWork);
      return nullptr;
   }

   /// Get the process function of the UDO subclass
   llvm::Function* getProcess() {
      if (process)
         return getFunction(process);
      return nullptr;
   }

   /// Get the type of the UDO subclass
   llvm::Type* getUDOperatorSubclassType() {
      if (udOperatorSubclass)
         return getType(udOperatorSubclass);
      return nullptr;
   }

   /// Release the llvm module
   unique_ptr<llvm::Module> releaseModule() {
      return unique_ptr<llvm::Module>(codegen->ReleaseModule());
   }

   void Initialize(clang::ASTContext& astContext) override {
      this->astContext = &astContext;
      codegen->Initialize(astContext);
   }

   void InitializeSema(clang::Sema& sema) override {
      this->sema = &sema;
   }

   bool HandleTopLevelDecl(clang::DeclGroupRef D) override {
      for (auto* decl : D) {
         handleDecl(decl, 0);
      }
      return codegen->HandleTopLevelDecl(D);
   }

   void HandleInlineFunctionDefinition(clang::FunctionDecl* D) override {
      if (handleInlineFunction(D)) {
         codegen->HandleInlineFunctionDefinition(D);
      }
   }

   void HandleInterestingDecl(clang::DeclGroupRef D) override {
      codegen->HandleInterestingDecl(D);
   }

   void HandleTranslationUnit(clang::ASTContext& ast_context) override {
      codegen->HandleTranslationUnit(ast_context);
      auto* module = codegen->GetModule();
      if (!error && module) {
         makeGlobalConstructor(module);
         makeGlobalDestructor(module);
      }
   }

   void HandleTagDeclDefinition(clang::TagDecl* D) override {
      codegen->HandleTagDeclDefinition(D);
   }

   void HandleTagDeclRequiredDefinition(const clang::TagDecl* D) override {
      codegen->HandleTagDeclRequiredDefinition(D);
   }

   void HandleCXXImplicitFunctionInstantiation(clang::FunctionDecl* D) override {
      codegen->HandleCXXImplicitFunctionInstantiation(D);
   }

   void HandleTopLevelDeclInObjCContainer(clang::DeclGroupRef D) override {
      codegen->HandleTopLevelDeclInObjCContainer(D);
   }

   void HandleImplicitImportDecl(clang::ImportDecl* D) override {
      codegen->HandleImplicitImportDecl(D);
   }

   void CompleteTentativeDefinition(clang::VarDecl* D) override {
      codegen->CompleteTentativeDefinition(D);
   }

   void AssignInheritanceModel(clang::CXXRecordDecl* RD) override {
      codegen->AssignInheritanceModel(RD);
   }

   void HandleCXXStaticMemberVarInstantiation(clang::VarDecl* D) override {
      codegen->HandleCXXStaticMemberVarInstantiation(D);
   }

   void HandleVTable(clang::CXXRecordDecl* RD) override {
      codegen->HandleVTable(RD);
   }

   clang::ASTMutationListener* GetASTMutationListener() override {
      return codegen->GetASTMutationListener();
   }

   clang::ASTDeserializationListener* GetASTDeserializationListener() override {
      return codegen->GetASTDeserializationListener();
   }

   void PrintStats() override {
      codegen->PrintStats();
   }

   bool shouldSkipFunctionBody(clang::Decl* D) override {
      return codegen->shouldSkipFunctionBody(D);
   }

   private:
   /// Handle a declaration at a given nesting level. Define it with an
   /// explicit requires clause so that it can't be called with any subclasses
   /// of Decl (which is what the overloads of handleDecl should handle).
   template <typename T>
      requires std::is_same_v<T, clang::Decl>
   void handleDecl(T* D, size_t level) {
      if (error)
         return;
      switch (D->getKind()) {
         case clang::Decl::Namespace:
            handleDecl(llvm::cast<clang::NamespaceDecl>(D), level);
            break;
         case clang::Decl::CXXRecord:
            handleDecl(llvm::cast<clang::CXXRecordDecl>(D), level);
            break;
         case clang::Decl::Function:
            handleDecl(llvm::cast<clang::FunctionDecl>(D), level);
            break;
         default:
            break;
      }
   }

   /// Handle a namespace declaration at a given nesting level
   void handleDecl(clang::NamespaceDecl* decl, size_t level) {
      if (level == 0 && decl->isStdNamespace()) {
         // don't traverse into std namespace
         return;
      }
      if (level == 0 && udoNamespace == nullptr && clang_utils::getName(decl) == "udo"sv) {
         udoNamespace = decl->getCanonicalDecl();
      }
      for (auto* subDecl : decl->decls()) {
         handleDecl(subDecl, level + 1);
         if (error)
            return;
      }
   }

   /// Handle a class declaration at a given nesting level
   void handleDecl(clang::CXXRecordDecl* decl, size_t level) {
      using namespace clang_utils;
      if (level == 1 && udoNamespace && isInNamespace(decl, udoNamespace)) {
         if (!stringType && getName(decl) == "String"sv) {
            stringType = decl;
         } else if (!executionState && getName(decl) == "ExecutionState"sv) {
            executionState = decl;
            for (auto* func : decl->methods()) {
               auto name = getName(func);
               if (name == "getThreadId"sv)
                  getThreadId = func;
               else if (name == "getLocalState"sv)
                  getLocalState = func;
            }
         } else if (!udOperatorClass && getName(decl) == "UDOperator"sv) {
            udOperatorClass = llvm::cast<clang::CXXRecordDecl>(decl);
            handleUDOperatorClass(udOperatorClass);
         }
      } else if (level + 1 == udoName.size() && udOperatorClass && !udOperatorSubclass && hasNestedName(decl, udoName)) {
         handleUDOperatorSubclass(decl);
         return;
      }
      // If we delayed inline functions that turned out to not belong to a
      // valid subclass of UDFOperator, pass them to the codegen here.
      if (!delayedInlineFunctions.empty()) {
         for (auto* func : delayedInlineFunctions) {
            codegen->HandleInlineFunctionDefinition(func);
         }
         delayedInlineFunctions.clear();
      }
   }

   /// Handle a function declaration at a given nesting level
   void handleDecl(clang::FunctionDecl* decl, size_t level) {
      using namespace clang_utils;
      if (level == 1 && udoNamespace && isInNamespace(decl, udoNamespace) && !decl->isDefined()) {
         auto name = clang_utils::getName(decl);
         if (name == "printDebug"sv) {
            if (printDebug) {
               if (!error)
                  error = err(tr(tc, "unexpected declaration of printDebug"));
            } else {
               printDebug = decl;
            }
         } else if (name == "getRandom"sv) {
            if (getRandom) {
               if (!error)
                  error = err(tr(tc, "unexpected declaration of getRandom"));
            } else {
               getRandom = decl;
            }
         }
      }
   }

   /// Handle the UDOperator base class
   void handleUDOperatorClass(clang::CXXRecordDecl* classDecl) {
      using namespace clang_utils;
      for (auto* decl : classDecl->decls()) {
         auto* namedDecl = llvm::dyn_cast<clang::NamedDecl>(decl);
         if (!namedDecl)
            continue;
         if (!udOperatorEmit && getName(namedDecl) == "emit"sv) {
            udOperatorEmit = llvm::cast<clang::FunctionTemplateDecl>(namedDecl);
         }
      }
   }

   /// Handle an inline function definition.
   ///
   /// Returns true when HandleInlineFunctionDefinition(D) of codegen should be
   /// called and false otherwise. For inline methods defined on a subclass of
   /// UDFOperator we must ensure that the codegen will generate code for them
   /// so we have to force them as non-inline. Additionally we may have to
   /// create an implicit constructor or destructor. The codegen should only
   /// see the declaration of the class after we did all those changes. So when
   /// we find an inline function of an UDFOperator subclass we store them and
   /// only pass them to the codegen afterwards in
   /// handle_udfoperator_subclass().
   bool handleInlineFunction(clang::FunctionDecl* D) {
      if (!udOperatorClass)
         return true;
      delayedInlineFunctions.push_back(D);
      return false;
   }

   /// Force the code generation of a member function
   void forceMemberFuncCodegen(clang::CXXMethodDecl* decl) {
      // When a member function's body is defined directly in the class, the
      // function is implicitly declared inline. This means that its code is
      // only generated when it is actually used. Since we always need the
      // code, explicitly set the "used" attribute here which will force the
      // code to be generated.
      if (!decl->hasAttr<clang::UsedAttr>())
         decl->addAttr(clang::UsedAttr::CreateImplicit(*astContext, decl->getSourceRange().getBegin()));
   }

   /// Check whether the given parameter type is a valid type for an execution
   /// state argument
   bool isValidExecutionStateParam(clang::QualType paramType) const {
      // We don't care about the qualifiers here, the functions that take an
      // execution state can take by const if they want.
      if (paramType.getCanonicalType().getTypePtr() != executionState->getTypeForDecl())
         return false;
      return true;
   }

   /// Check the signature of the emit function:
   /// static void emit(udo::ExecutionState, const OutputTuple&)
   bool checkEmitSignature(clang::CXXMethodDecl* method) const {
      using namespace clang_utils;
      if (!method->isStatic())
         return false;

      auto* funcType = llvm::cast<clang::FunctionProtoType>(method->getFunctionType());
      if (!funcType->getReturnType().getTypePtr()->isVoidType())
         return false;

      return true;

      auto params = funcType->getParamTypes();
      if (params.size() != 2)
         return false;

      if (!isValidExecutionStateParam(params[0]))
         return false;
      if (!isConstLvalueRefTo(params[1], outputTupleClass->getTypeForDecl()))
         return false;

      return true;
   }

   /// Check the signature of the accept function:
   /// void accept(udo::ExecutionState, const InputTuple&)
   bool checkAcceptSignature(clang::CXXMethodDecl* method) const {
      using namespace clang_utils;

      // The method must not be static, but we allow it to be const
      if (method->isStatic())
         return false;

      auto* funcType = llvm::cast<clang::FunctionProtoType>(method->getFunctionType());
      if (!funcType->getReturnType().getTypePtr()->isVoidType())
         return false;

      auto params = funcType->getParamTypes();
      if (params.size() != 2)
         return false;

      if (!isValidExecutionStateParam(params[0]))
         return false;
      if (!isConstLvalueRefTo(params[1], inputTupleClass->getTypeForDecl()))
         return false;

      return true;
   }

   /// Check the signature of the extraWork function:
   /// uint32_t extraWork(udo::ExecutionState, uint32_t)
   bool checkExtraWorkSignature(clang::CXXMethodDecl* method) const {
      using namespace clang_utils;

      // The method must not be static, but we allow it to be const
      if (method->isStatic())
         return false;

      auto* funcType = llvm::cast<clang::FunctionProtoType>(method->getFunctionType());
      if (!isUIntType(*astContext, funcType->getReturnType(), 32))
         return false;

      auto params = funcType->getParamTypes();
      if (params.size() != 2)
         return false;

      if (!isValidExecutionStateParam(params[0]))
         return false;
      if (!isUIntType(*astContext, params[1], 32))
         return false;

      return true;
   }

   /// Check the signature of the process function:
   /// bool process(udo::ExecutionState)
   bool checkProcessSignature(clang::CXXMethodDecl* method) const {
      using namespace clang_utils;

      // The method must not be static, but we allow it to be const
      if (method->isStatic())
         return false;

      auto* funcType = llvm::cast<clang::FunctionProtoType>(method->getFunctionType());
      if (!funcType->getReturnType()->isBooleanType())
         return false;

      auto params = funcType->getParamTypes();
      if (params.size() != 1)
         return false;

      if (!isValidExecutionStateParam(params[0]))
         return false;

      return true;
   }

   /// Handle a class that could potentially be a subclass of UDOperator
   void handleUDOperatorSubclass(clang::CXXRecordDecl* decl) {
      if (!decl->hasDefinition())
         return;
      if (decl->isPolymorphic()) {
         error = err(tr(tc, "UDO class must not be polymorphic"));
         return;
      }

      // Check if UDOperator is a (public, unambiguous, non-virtual) base of the class
      if (decl->getNumBases() == 0) {
         error = err(tr(tc, "UDOperator must be a public, unambiguous, non-virtual base"));
         return;
      }

      decl = decl->getCanonicalDecl();

      auto traverseBases = [](clang::CXXRecordDecl* decl, auto& func) -> bool {
         queue<clang::CXXBaseSpecifier*> queue;
         for (auto& base : decl->bases())
            queue.push(&base);
         while (!queue.empty()) {
            auto* base = queue.front();
            queue.pop();
            if (!func(base))
               return false;
            auto* baseDecl = base->getType()->getAsCXXRecordDecl();
            if (baseDecl)
               for (auto& base : baseDecl->bases())
                  queue.push(&base);
         }
         return true;
      };

      bool foundBase = false;
      auto handleBase = [&](const clang::CXXBaseSpecifier* baseSpec) -> bool {
         if (baseSpec->isVirtual() || baseSpec->getAccessSpecifier() != clang::AS_public)
            return false;
         auto* baseDecl = baseSpec->getType()->getAsCXXRecordDecl();
         if (!baseDecl)
            return false;
         if (baseDecl == udOperatorClass) {
            if (foundBase) {
               error = err(tr(tc, "UDOperator must be unambiguous base class"));
               return false;
            }
            foundBase = true;
         }
         return true;
      };

      if (!traverseBases(decl, handleBase) || !foundBase) {
         if (!error)
            error = err(tr(tc, "UDOperator must be a public, unambiguous, non-virtual base"));
         return;
      }

      udOperatorSubclass = decl;

      // Get the input and output tuple types which should be type declarations in the derived class
      auto findSubclassMemberType = [&](string_view name) -> clang::CXXRecordDecl* {
         auto identIt = astContext->Idents.find(name);
         if (identIt == astContext->Idents.end())
            return nullptr;

         auto* ident = identIt->second;
         auto result = udOperatorSubclass->lookup(ident);
         if (result.empty())
            return nullptr;

         auto* typeDecl = result.find_first<clang::TypeDecl>();
         if (!typeDecl)
            return nullptr;

         auto declType = astContext->getCanonicalType(astContext->getTypeDeclType(typeDecl));
         if (declType.hasQualifiers())
            return nullptr;
         if (auto recordType = declType.getAs<clang::RecordType>())
            if (auto* cxxRecordDecl = llvm::dyn_cast<clang::CXXRecordDecl>(recordType->getDecl()))
               return cxxRecordDecl;

         return nullptr;
      };

      if (!(inputTupleClass = findSubclassMemberType("InputTuple"))) {
         error = err(tr(tc, "invalid or missing member type \"InputTuple\" in UDO class"));
         return;
      }
      if (!(outputTupleClass = findSubclassMemberType("OutputTuple"))) {
         error = err(tr(tc, "invalid or missing member type \"OutputTuple\" in UDO class"));
         return;
      }

      // Find the specialization of the emit function for this class
      {
         void* dummy;
         auto* specialization = udOperatorEmit->findSpecialization({clang::QualType(decl->getTypeForDecl(), 0)}, dummy);
         if (!specialization) {
            error = err(tr(tc, "UDO does not call emit() or does not use its class type as template argument"));
            return;
         }

         auto* specializationInfo = specialization->getTemplateSpecializationInfo();
         if (specializationInfo->isExplicitInstantiationOrSpecialization()) {
            error = err(tr(tc, "emit() must not be instantiated or specialized explicitly"));
            return;
         }

         auto* emitDecl = llvm::cast<clang::CXXMethodDecl>(specialization);

         if (!checkEmitSignature(emitDecl)) {
            error = err(tr(tc, "invalid signature of emit function"));
            return;
         }

         emit = emitDecl;
      }

      for (auto method : decl->methods()) {
         using namespace clang_utils;
         if (method->isUserProvided()) {
            auto methodName = getName(method);
            if (methodName == "accept"sv) {
               if (!checkAcceptSignature(method)) {
                  error = err(tr(tc, "invalid signature of accept function, expected signature: void accept(udo::ExecutionState, const InputTuple&)"));
                  return;
               }
               forceMemberFuncCodegen(method);
               accept = method;
            } else if (methodName == "extraWork"sv) {
               if (!checkExtraWorkSignature(method)) {
                  error = err(tr(tc, "invalid signature of extraWork function, expected signature: uint32_t extraWork(udo::ExecutionState, uint32_t)"));
                  return;
               }
               forceMemberFuncCodegen(method);
               extraWork = method;
            } else if (methodName == "process"sv) {
               if (!checkProcessSignature(method)) {
                  error = err(tr(tc, "invalid signature of process function, expected signature: bool process(udo::ExecutionState)"));
                  return;
               }
               forceMemberFuncCodegen(method);
               process = method;
            }
         }
      }

      // If the constructor is trivial, we don't need to generate any code
      // for it. But when it is not, we need to make sure that the code is
      // generated. Especially for the case where the constructor is
      // implicitly declared or defined we need to manually add the
      // declaration and definition.
      {
         clang::CXXConstructorDecl* subclassConstructor = nullptr;
         for (auto* constr : decl->ctors()) {
            if (constr->isCopyOrMoveConstructor())
               continue;
            if (!subclassConstructor) {
               subclassConstructor = constr;
            } else {
               // There are multiple constructors
               error = "Multiple constructors in C++-UDO not implemented yet";
               return;
            }
         }
         if (!subclassConstructor && decl->hasNonTrivialDefaultConstructor() && decl->needsImplicitDefaultConstructor())
            subclassConstructor = sema->DeclareImplicitDefaultConstructor(decl);
         // When the constructor was implicitly declared, we need to
         // generate the code for it. In clang constructor->isDefined()
         // returns true when the constructor is defaulted but does not
         // generate code for it, so we check for isImplicit() and
         // isDefaulted() instead here.
         if (subclassConstructor && !subclassConstructor->isUserProvided() && subclassConstructor->isImplicit() && subclassConstructor->isDefaulted()) {
            sema->DefineImplicitDefaultConstructor(decl->getLocation(), subclassConstructor);
            delayedInlineFunctions.push_back(subclassConstructor);
         }
         if (subclassConstructor) {
            forceMemberFuncCodegen(subclassConstructor);
            constructor = subclassConstructor;
         }
      }
      // Just like for the constructor we need to ensure the code for the
      // destructor is generated when it is non-trivial.
      if (decl->hasNonTrivialDestructor()) {
         auto* subclassDestructor = decl->getDestructor();
         assert(subclassDestructor);
         if (!subclassDestructor->isUserProvided() && subclassDestructor->isImplicit() && subclassDestructor->isDefaulted()) {
            sema->DefineImplicitDestructor(decl->getLocation(), subclassDestructor);
            delayedInlineFunctions.push_back(subclassDestructor);
         }
         forceMemberFuncCodegen(subclassDestructor);
         destructor = subclassDestructor;
      }
      assert(constructor || udOperatorSubclass->hasTrivialDefaultConstructor());
      assert(destructor || udOperatorSubclass->hasTrivialDestructor());
      for (auto* func : delayedInlineFunctions)
         codegen->HandleInlineFunctionDefinition(func);
      delayedInlineFunctions.clear();
   }

   struct LLVMStructor {
      llvm::Function* func;
      unsigned priority;
   };

   /// Extract the vector of structors from a global variable
   vector<LLVMStructor> extractStructors(llvm::GlobalVariable* globalVar) {
      auto* structorsArray = llvm::cast<llvm::ConstantArray>(globalVar->getInitializer());
      auto opIt = structorsArray->op_begin();
      auto opEnd = structorsArray->op_end();
      if (opIt == opEnd)
         return {};

      vector<LLVMStructor> structors;

      for (; opIt != opEnd; ++opIt) {
         auto* ctorElem = llvm::cast<llvm::ConstantStruct>(*opIt);
         auto& ctor = structors.emplace_back();
         ctor.func = llvm::cast<llvm::Function>(ctorElem->getOperand(1));
         ctor.priority = llvm::cast<llvm::ConstantInt>(ctorElem->getOperand(0))->getZExtValue();
      }

      stable_sort(structors.begin(), structors.end(), [](auto& a, auto& b) { return a.priority < b.priority; });

      // Remove the global variable to prevent the structors from being called
      // twice accidentally. This is the case when using compilestatic: The
      // codegen generates code that explicity calls the structors, but when
      // they are included in the shared library dlopen/dlclose will also call
      // them.
      globalVar->eraseFromParent();

      return structors;
   }

   /// Create a function that calls all global constructors
   void makeGlobalConstructor(llvm::Module* module) {
      auto* llvmCtors = module->getGlobalVariable("llvm.global_ctors");
      if (!llvmCtors)
         return;

      auto ctors = extractStructors(llvmCtors);

      // Create a function that calls the constructors in the order of their priority
      auto& context = module->getContext();
      auto* funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
      auto* func = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, "umbra.udo.globalConstructor", *module);
      auto* bb = llvm::BasicBlock::Create(context, "init", func);
      llvm::IRBuilder<> builder(bb);

      for (auto& ctor : ctors)
         builder.CreateCall(ctor.func);
      builder.CreateRetVoid();

      globalConstructor = func;
   }

   /// Create a function that calls the global destructors
   void makeGlobalDestructor(llvm::Module* module) {
      // Ensure that the __dso_handle is defined in the module if it is used
      auto& context = module->getContext();
      auto* dsoHandle = module->getGlobalVariable("__dso_handle");
      if (dsoHandle) {
         dsoHandle->removeFromParent();

         auto* int8Type = llvm::Type::getInt8Ty(context);
         auto* newDsoHandle = new llvm::GlobalVariable(*module, int8Type, true, llvm::GlobalVariable::PrivateLinkage, llvm::ConstantInt::get(int8Type, 0), "__dso_handle");

         dsoHandle->replaceAllUsesWith(newDsoHandle);
         delete dsoHandle;
         dsoHandle = newDsoHandle;
      }

      auto* llvmDtors = module->getGlobalVariable("llvm.global_dtors");
      if (!llvmDtors && !dsoHandle)
         return;

      vector<LLVMStructor> dtors;
      if (llvmDtors)
         dtors = extractStructors(llvmDtors);

      // Create a function that calls the destructors and __cxa_finalize
      auto* funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
      auto* func = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, "umbra.udo.globalDestructor", *module);
      auto* bb = llvm::BasicBlock::Create(context, "init", func);
      llvm::IRBuilder<> builder(bb);

      for (auto it = dtors.rbegin(), end = dtors.rend(); it != end; ++it)
         builder.CreateCall(it->func);

      if (dsoHandle) {
         // Declare the __cxa_finalize function that must be called with the __dso_handle
         auto* finalizeFuncType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), llvm::Type::getInt8PtrTy(context), false);
         auto* finalizeFunc = llvm::Function::Create(finalizeFuncType, llvm::Function::ExternalLinkage, "__cxa_finalize", *module);
         builder.CreateCall(finalizeFuncType, finalizeFunc, {dsoHandle});
      }

      builder.CreateRetVoid();

      globalDestructor = func;
   }
};
//---------------------------------------------------------------------------
/// The clang frontend action for C++-UDOs
class CxxUDOFrontendAction : public clang::ASTFrontendAction {
   public:
   /// The llvm context
   llvm::LLVMContext& context;
   /// The analysis that will be written to
   CxxUDOAnalysis& analysis;
   /// The name of the UDO class
   string_view udoName;
   /// The llvm module
   unique_ptr<llvm::Module> module;

   private:
   /// The clang diagnostics
   clang::DiagnosticsEngine* diagnostics;
   /// The clang consumer
   CxxUDOClangConsumer* consumer;
   /// The error (if an error occurred)
   optional<string> error;

   public:
   CxxUDOFrontendAction(llvm::LLVMContext& context, CxxUDOAnalysis& analysis, string_view udoName) : context(context), analysis(analysis), udoName(udoName) {}

   ~CxxUDOFrontendAction() override {}

   /// Take the error message
   optional<string> takeError() {
      return move(error);
   }

   protected:
   unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& compiler, llvm::StringRef filename) override {
      assert(compiler.hasDiagnostics());
      diagnostics = &compiler.getDiagnostics();
      auto ptr = std::make_unique<CxxUDOClangConsumer>(context, compiler, filename, udoName);
      consumer = ptr.get();
      return ptr;
   }

   void EndSourceFileAction() override {
      error = consumer->takeError();
      if (error || diagnostics->hasErrorOccurred() || !consumer->udOperatorSubclass)
         return;
      auto* astContext = consumer->astContext;
      assert(astContext->getCharWidth() == CHAR_BIT);
      analysis.runtimeFunctions = consumer->getRuntimeFunctions();
      analysis.stringType = consumer->getStringType();
      analysis.executionState = consumer->getExecutionState();
      analysis.getThreadId = consumer->getGetThreadId();
      analysis.getLocalState = consumer->getGetLocalState();
      analysis.output = consumer->getOutput();
      analysis.globalConstructor = consumer->globalConstructor;
      analysis.globalDestructor = consumer->globalDestructor;
      analysis.inputTupleType = consumer->getInputTupleClassType();
      analysis.outputTupleType = consumer->getOutputTupleClassType();
      analysis.emit = consumer->getEmit();
      auto typeInfo = astContext->getTypeInfo(consumer->udOperatorSubclass->getTypeForDecl());
      analysis.size = typeInfo.Width / CHAR_BIT;
      analysis.alignment = typeInfo.Align / CHAR_BIT;
      analysis.name = clang_utils::getName(consumer->udOperatorSubclass);
      analysis.llvmType = consumer->getUDOperatorSubclassType();
      analysis.constructor = consumer->getConstructor();
      analysis.destructor = consumer->getDestructor();
      analysis.accept = consumer->getAccept();
      analysis.extraWork = consumer->getExtraWork();
      analysis.process = consumer->getProcess();
      module = consumer->releaseModule();
   }
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
/// The implementation
struct CxxUDOAnalyzer::Impl {
   /// The llvm context, if owned by the analyzer
   unique_ptr<llvm::LLVMContext> ownedLLVMContext;
   /// The pointer to the context that is used
   llvm::LLVMContext* llvmContext;
   /// The analysis
   CxxUDOAnalysis analysis;
   /// The llvm module
   unique_ptr<llvm::Module> llvmModule;
};
//---------------------------------------------------------------------------
CxxUDOAnalyzer::CxxUDOAnalyzer(string funcSource, string udoClassName)
   : funcSource(move(funcSource)), udoClassName(move(udoClassName))
// Constructor
{
}
//---------------------------------------------------------------------------
// Move constructor
CxxUDOAnalyzer::CxxUDOAnalyzer(CxxUDOAnalyzer&& analyzer) noexcept = default;
//---------------------------------------------------------------------------
// Move assignment
CxxUDOAnalyzer& CxxUDOAnalyzer::operator=(CxxUDOAnalyzer&& analyzer) noexcept = default;
//---------------------------------------------------------------------------
CxxUDOAnalyzer::~CxxUDOAnalyzer()
// Destructor
{
}
//---------------------------------------------------------------------------
tl::expected<void, std::string> CxxUDOAnalyzer::analyze(llvm::LLVMContext* context, unsigned optimizationLevel)
// Analyze the function
{
   impl = make_unique<Impl>();
   if (context) {
      impl->llvmContext = context;
   } else {
      impl->ownedLLVMContext = make_unique<llvm::LLVMContext>();
      impl->llvmContext = &*impl->ownedLLVMContext;
   }

   ClangCompiler compiler(funcSource, optimizationLevel);
   CxxUDOFrontendAction frontendAction(*impl->llvmContext, impl->analysis, udoClassName);
   compiler.addFrontendAction(&frontendAction);

   if (auto result = compiler.compile(); !result)
      return result;

   if (auto error = frontendAction.takeError(); error)
      return tl::unexpected(move(error).value());

   //CxxUDOLogic::validateAnalysis(*func, impl->analysis);

   impl->llvmModule = move(frontendAction.module);

   return {};
}
//---------------------------------------------------------------------------
const CxxUDOAnalysis& CxxUDOAnalyzer::getAnalysis() const
// Get the analysis
{
   return impl->analysis;
}
//---------------------------------------------------------------------------
CxxUDOAnalysis& CxxUDOAnalyzer::getAnalysis()
// Get the analysis
{
   return impl->analysis;
}
//---------------------------------------------------------------------------
llvm::Module& CxxUDOAnalyzer::getModule() const
// Get the LLVM module
{
   return *impl->llvmModule;
}
//---------------------------------------------------------------------------
unique_ptr<llvm::Module> CxxUDOAnalyzer::takeModule()
// Take the LLVM module
{
   return move(impl->llvmModule);
}
//---------------------------------------------------------------------------
vector<char> CxxUDOAnalyzer::getSerializedAnalysis()
// Serialize the analysis into a buffer
{
   assert(impl);
   llvm_metadata::MetadataWriter writer(*impl->llvmModule);
   auto result = writer.writeNamedValue("udo.CxxUDO.Analysis"sv, impl->analysis);
   assert(result);
   static_cast<void>(result);

   llvm::SmallVector<char, 1> llvmBuffer;
   llvm::raw_svector_ostream stream(llvmBuffer);
   llvm::WriteBitcodeToFile(*impl->llvmModule, stream);

   vector<char> buffer(llvmBuffer.begin(), llvmBuffer.end());
   return buffer;
}
//---------------------------------------------------------------------------
namespace llvm_metadata {
//---------------------------------------------------------------------------
#define TRY(x) \
   if (auto result = (x); !result) return result;
//---------------------------------------------------------------------------
IOResult IO<CxxUDORuntimeFunctions>::enumEntries(StructContext& context, CxxUDORuntimeFunctions& value) {
   TRY(mapMember(context, value.printDebug));
   TRY(mapMember(context, value.getRandom));
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<CxxUDOOutput>::enumEntries(StructContext& context, CxxUDOOutput& value) {
   TRY(mapMember(context, value.name));
   TRY(mapMember(context, value.type));
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<CxxUDOAnalysis>::enumEntries(StructContext& context, CxxUDOAnalysis& value) {
   TRY(mapMember(context, value.runtimeFunctions));
   TRY(mapMember(context, value.stringType));
   TRY(mapMember(context, value.executionState));
   TRY(mapMember(context, value.getThreadId));
   TRY(mapMember(context, value.getLocalState));
   TRY(mapMember(context, value.output));
   TRY(mapMember(context, value.globalConstructor));
   TRY(mapMember(context, value.globalDestructor));
   TRY(mapMember(context, value.inputTupleType));
   TRY(mapMember(context, value.outputTupleType));
   TRY(mapMember(context, value.emit));
   TRY(mapMember(context, value.size));
   TRY(mapMember(context, value.alignment));
   TRY(mapMember(context, value.name));
   TRY(mapMember(context, value.llvmType));
   TRY(mapMember(context, value.constructor));
   TRY(mapMember(context, value.destructor));
   TRY(mapMember(context, value.accept));
   TRY(mapMember(context, value.extraWork));
   TRY(mapMember(context, value.process));
   TRY(mapMember(context, value.emitInAccept));
   TRY(mapMember(context, value.emitInProcess));
   return {};
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
