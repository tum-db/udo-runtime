#include "udo/udo_runtime.h"
#include "udo/CxxUDOAnalyzer.hpp"
#include "udo/CxxUDOCompiler.hpp"
#include "udo/CxxUDOExecution.hpp"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <bit>
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <catalog/pg_type_d.h>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
extern "C" {
//---------------------------------------------------------------------------
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
PG_MODULE_MAGIC;
#pragma GCC diagnostic pop
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
using namespace udo;
using namespace std;
//---------------------------------------------------------------------------
namespace {
//---------------------------------------------------------------------------
struct UDOImpl {
   /// The C++ UDO analyzer
   CxxUDOAnalyzer analyzer;
   /// Auxiliary storage for the types returned in udo_get_scalar_arguments
   vector<Oid> scalarArgTypesStorage;
   /// Auxiliary storage for the value returned in udo_get_input/output_attributes
   vector<udo_attribute_descr> attrsStorage;
   /// The compiled object file
   vector<char> objectFile;
   /// The execution (if loaded)
   optional<CxxUDOExecution> execution;
   /// The constructor arg (if requested)
   unique_ptr<byte[]> constructorArg;
   /// The last error message
   string errorMessage;

   /// Constructor that passes the args to the analyzer
   template <typename... Ts>
   UDOImpl(Ts&&... args) : analyzer(forward<Ts>(args)...) {}

   /// Set the size, alignment and pgTypeOid members of attr according to the
   /// given type. Return false if type is not supported.
   bool makeAttrType(llvm::Type* type, udo_attribute_descr& attr) const;
};
//---------------------------------------------------------------------------
bool UDOImpl::makeAttrType(llvm::Type* type, udo_attribute_descr& attr) const
// Set the size, alignment and pgTypeOid members of attr according to the given
// type. Return false if the type is not supported.
{
   if (type == analyzer.getAnalysis().stringType) {
      attr.size = 16;
      attr.alignment = 8;
      attr.pgTypeOid = TEXTOID;
   } else {
      switch (type->getTypeID()) {
         case llvm::Type::FloatTyID:
            attr.size = 4;
            attr.alignment = 4;
            attr.pgTypeOid = FLOAT4OID;
            break;
         case llvm::Type::DoubleTyID:
            attr.size = 8;
            attr.alignment = 8;
            attr.pgTypeOid = FLOAT8OID;
            break;
         case llvm::Type::IntegerTyID: {
            auto* intType = llvm::cast<llvm::IntegerType>(type);
            switch (intType->getBitWidth()) {
               case 1:
                  attr.size = 1;
                  attr.alignment = 1;
                  attr.pgTypeOid = BOOLOID;
                  break;
               case 16:
                  attr.size = 2;
                  attr.alignment = 2;
                  attr.pgTypeOid = INT2OID;
                  break;
               case 32:
                  attr.size = 4;
                  attr.alignment = 4;
                  attr.pgTypeOid = INT4OID;
                  break;
               case 64:
                  attr.size = 8;
                  attr.alignment = 8;
                  attr.pgTypeOid = INT8OID;
                  break;
               default:
                  return false;
            }
            break;
         }
         default:
            return false;
      }
   }
   return true;
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
udo_handle udo_cxxudo_init(const char* cxxSource, size_t cxxSourceLen, const char* udoClassName, size_t udoClassNameLen)
// Initialize a C++ UDO with the given C++ source code and the name of the
// class that implements the UDO.
{
   string funcSource(cxxSource, cxxSourceLen);
   string className(udoClassName, udoClassNameLen);
   auto* impl = new UDOImpl(move(funcSource), move(className));
   return reinterpret_cast<udo_handle>(impl);
}
//---------------------------------------------------------------------------
void udo_cxxudo_destroy(udo_handle handle)
// Destroy a handle created with `udo_cxxudo_init()`
{
   auto* impl = reinterpret_cast<UDOImpl*>(handle);
   delete impl;
}
//---------------------------------------------------------------------------
const char* udo_error_message(udo_handle handle)
// Get the error message of the last function call that returned an error
{
   auto* impl = reinterpret_cast<UDOImpl*>(handle);
   return impl->errorMessage.c_str();
}
//---------------------------------------------------------------------------
static unordered_map<uint64_t, UDOImpl*> cachedImpls;
//---------------------------------------------------------------------------
void udo_cache_handle(udo_handle handle, uint64_t cacheKey)
// Cache a handle with a given key
{
   auto* impl = reinterpret_cast<UDOImpl*>(handle);

   auto it = cachedImpls.find(cacheKey);
   if (it == cachedImpls.end()) {
      cachedImpls.emplace(cacheKey, impl);
   } else {
      delete it->second;
      it->second = impl;
   }
}
//---------------------------------------------------------------------------
udo_handle udo_get_cached_handle(uint64_t cacheKey)
// Get a cached handle
{
   auto it = cachedImpls.find(cacheKey);
   if (it == cachedImpls.end()) {
      return nullptr;
   } else {
      return reinterpret_cast<udo_handle>(it->second);
   }
}
//---------------------------------------------------------------------------
udo_errno udo_cxxudo_analyze(udo_handle handle)
// Analyze a C++ UDO
{
   auto* impl = reinterpret_cast<UDOImpl*>(handle);

   if (auto result = impl->analyzer.analyze(); !result) {
      impl->errorMessage = move(result).error();
      return UDO_INVALID_USER_CODE;
   }

   return UDO_SUCCESS;
}
//---------------------------------------------------------------------------
udo_errno udo_get_arguments(udo_handle handle, udo_arguments* scalarArgs)
// Get the arguments the UDO takes
{
   auto* impl = reinterpret_cast<UDOImpl*>(handle);
   auto& analysis = impl->analyzer.getAnalysis();

   impl->scalarArgTypesStorage.clear();

   if (analysis.constructor) {
      auto* funcType = analysis.constructor->getFunctionType();
      auto params = funcType->params();

      impl->scalarArgTypesStorage.reserve(params.size());

      // Start with i = 1 to skip the "this" argument
      for (size_t i = 1; i < params.size(); ++i) {
         udo_attribute_descr attr;
         if (!impl->makeAttrType(params[i], attr)) {
            impl->errorMessage = "Scalar argument ";
            impl->errorMessage += to_string(i);
            impl->errorMessage += " of C++ UDO has unsupported type";
            return UDO_INVALID_USER_CODE;
         }

         impl->scalarArgTypesStorage.push_back(attr.pgTypeOid);
      }
   }

   scalarArgs->numScalarArguments = impl->scalarArgTypesStorage.size();
   scalarArgs->numTableArguments = analysis.accept ? 1 : 0;
   scalarArgs->pgTypeOids = impl->scalarArgTypesStorage.data();

   return UDO_SUCCESS;
}
//---------------------------------------------------------------------------
udo_errno udo_get_output_attributes(udo_handle handle, udo_attribute_descr_array* attrDescrs)
// Get the attributes of the tuples the UDO generates
{
   auto* impl = reinterpret_cast<UDOImpl*>(handle);
   auto& analysis = impl->analyzer.getAnalysis();

   impl->attrsStorage.clear();
   impl->attrsStorage.reserve(analysis.output.size());

   for (auto& output : analysis.output) {
      auto& attr = impl->attrsStorage.emplace_back();
      attr.name = output.name.c_str();
      if (!impl->makeAttrType(output.type, attr)) {
         impl->errorMessage = "Unsuported type in C++ UDO in attribute ";
         impl->errorMessage += output.name;
         return UDO_INVALID_USER_CODE;
      }
   }

   attrDescrs->size = impl->attrsStorage.size();
   attrDescrs->attributes = impl->attrsStorage.data();

   return UDO_SUCCESS;
}
//---------------------------------------------------------------------------
udo_errno udo_get_input_attributes(udo_handle handle, udo_attribute_descr_array* attrDescrs)
// Get the attributes of the input the UDO expects, or NULL if it has no input
{
   auto* impl = reinterpret_cast<UDOImpl*>(handle);
   auto& analysis = impl->analyzer.getAnalysis();

   auto* inputType = llvm::cast<llvm::StructType>(analysis.inputTupleType);
   auto tupleElements = inputType->elements();
   if (tupleElements.empty()) {
      attrDescrs->size = 0;
      attrDescrs->attributes = nullptr;
      return UDO_SUCCESS;
   }

   impl->attrsStorage.clear();
   impl->attrsStorage.reserve(tupleElements.size());

   for (auto* type : tupleElements) {
      auto& attr = impl->attrsStorage.emplace_back();
      attr.name = nullptr;
      if (!impl->makeAttrType(type, attr)) {
         impl->errorMessage = "Unsuported type in input attribute of C++ UDO";
         return UDO_INVALID_USER_CODE;
      }
   }

   attrDescrs->size = impl->attrsStorage.size();
   attrDescrs->attributes = impl->attrsStorage.data();

   return UDO_SUCCESS;
}
//---------------------------------------------------------------------------
size_t udo_get_size(udo_handle handle)
// Get the size of the UDO object
{
   auto* impl = reinterpret_cast<UDOImpl*>(handle);
   auto& analysis = impl->analyzer.getAnalysis();
   return analysis.size;
}
//---------------------------------------------------------------------------
udo_errno udo_cxxudo_compile(udo_handle handle)
// Compile a C++ UDO to an object file after it was analyzed
{
   auto* impl = reinterpret_cast<UDOImpl*>(handle);

   if (!impl->objectFile.empty())
      return UDO_SUCCESS;

   CxxUDOCompiler compiler(impl->analyzer);

   if (auto result = compiler.preprocessModule(); !result) {
      impl->errorMessage = move(result).error();
      return UDO_COMPILE_ERROR;
   }

   if (auto result = compiler.compile(); result) {
      impl->objectFile = move(result).value();
   } else {
      impl->errorMessage = move(result).error();
      return UDO_COMPILE_ERROR;
   }

   return UDO_SUCCESS;
}
//---------------------------------------------------------------------------
udo_errno udo_cxxudo_link(udo_handle handle, udo_cxx_functors functors, udo_cxx_allocation_funcs allocationFuncs, int64_t tlsBlockOffset, uint64_t tlsBlockSize, udo_cxx_functions* functions)
// Link a compiled C++ UDO
{
   static_assert(sizeof(udo_cxx_functors) == sizeof(CxxUDOFunctors));
   static_assert(sizeof(udo_cxx_allocation_funcs) == sizeof(CxxUDOAllocationFuncs));
   static_assert(sizeof(udo_cxx_functions) == sizeof(CxxUDOFunctions));

   auto* impl = reinterpret_cast<UDOImpl*>(handle);

   if (!impl->execution) {
      impl->execution.emplace(impl->objectFile);
      if (auto result = impl->execution->link(bit_cast<CxxUDOAllocationFuncs>(allocationFuncs), tlsBlockOffset, tlsBlockSize); !result) {
         impl->errorMessage = move(result).error();
         impl->execution.reset();
         return UDO_LINK_ERROR;
      }
   }

   impl->execution->getFunctors() = bit_cast<CxxUDOFunctors>(functors);

   *functions = bit_cast<udo_cxx_functions>(impl->execution->initialize());

   return UDO_SUCCESS;
}
//---------------------------------------------------------------------------
void* udo_cxxudo_get_constructor_arg(udo_handle handle)
// Get a pointer that can be used as an argument to the global constructor
{
   auto* impl = reinterpret_cast<UDOImpl*>(handle);

   if (!impl->constructorArg)
      impl->constructorArg = CxxUDOExecution::createLibcConstructorArg();

   return impl->constructorArg.get();
}
//---------------------------------------------------------------------------
