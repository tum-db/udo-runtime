#ifndef H_postgres_udo_runtime
#define H_postgres_udo_runtime
//---------------------------------------------------------------------------
#include <postgres.h>
//---------------------------------------------------------------------------
#include <fmgr.h>
#include <stddef.h>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif
//---------------------------------------------------------------------------
/// The possible errors of the UDO runtime library
enum udo_errno {
   UDO_SUCCESS = 0,
   UDO_INVALID_USER_CODE,
   UDO_COMPILE_ERROR,
   UDO_LINK_ERROR,
};
//---------------------------------------------------------------------------
/// A functor that is used as a callback in the UDO
typedef struct udo_functor {
   /// The function pointer
   void* func;
   /// The state argument that is passed to the function as first argument
   void* stateArg;
} udo_functor;
//---------------------------------------------------------------------------
/// The functor addresses required for the precompiled C++ UDOs
typedef struct udo_cxx_functors {
   /// The functor for the emit callback
   udo_functor emitFunctor;
   /// The functor for printDebug
   udo_functor printDebugFunctor;
   /// The functor for getRandom
   udo_functor getRandomFunctor;
} udo_cxx_functors;
//---------------------------------------------------------------------------
/// The allocation functions that will be used to link the C++ UDO
typedef struct udo_cxx_allocation_funcs {
   void* malloc;
   void* calloc;
   void* realloc;
   void* posixMemalign;
   void* free;
} udo_cxx_allocation_funcs;
//---------------------------------------------------------------------------
/// The function pointers to a compiled and linked C++ UDO. Set to nullptr if a
/// function does not exist.
typedef struct udo_cxx_functions {
   /// The global constructor function pointer
   void* globalConstructor;
   /// The global destructor function pointer
   void* globalDestructor;
   /// The thread initialization function pointer
   void* threadInit;
   /// The constructor function pointer
   void* constructor;
   /// The destructor function pointer
   void* destructor;
   /// The accept function pointer
   void* accept;
   /// The extraWork function pointer
   void* extraWork;
   /// The process function pointer
   void* process;
} udo_cxx_functions;
//---------------------------------------------------------------------------
/// The arguments of a UDO
typedef struct udo_arguments {
   /// The number of scalar arguments
   size_t numScalarArguments;
   /// The number of table arguments (currently only 0 or 1)
   size_t numTableArguments;
   /// The Postgres types of the scalar arguments
   Oid* pgTypeOids;
} udo_arguments;
//---------------------------------------------------------------------------
/// The description of an attribute used by the UDO
typedef struct udo_attribute_descr {
   /// The name of the attribute, may be NULL
   const char* name;
   /// The in-memory size of the type
   size_t size;
   /// The alignment of the type
   size_t alignment;
   /// The Postgres type of the attribute
   Oid pgTypeOid;
} udo_attribute_descr;
//---------------------------------------------------------------------------
/// A collection of attributes
typedef struct udo_attribute_descr_array {
   /// The number of elements in the array
   size_t size;
   /// The array that contains the attributes
   udo_attribute_descr* attributes;
} udo_attribute_descr_array;
//---------------------------------------------------------------------------
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
PG_MODULE_MAGIC;
#pragma GCC diagnostic pop
//---------------------------------------------------------------------------
struct udo_opaque_impl;
//---------------------------------------------------------------------------
/// An opage handle that is used to to track all objects create by this
/// library.
typedef struct udo_opaque_impl* udo_handle;
//---------------------------------------------------------------------------
/// Initialize a C++ UDO with the given C++ source code and the name of the
/// class that implements the UDO.
udo_handle udo_cxxudo_init(const char* cxxSource, size_t cxxSourceLen, const char* udoClassName, size_t udoClassNameLen);
//---------------------------------------------------------------------------
/// Destroy a handle created with `udo_cxxudo_init()`
void udo_cxxudo_destroy(udo_handle handle);
//---------------------------------------------------------------------------
/// Get the error message of the last function call that returned an error
const char* udo_error_message(udo_handle handle);
//---------------------------------------------------------------------------
/// Cache a handle with a given key
void udo_cache_handle(udo_handle handle, uint64_t cacheKey);
//---------------------------------------------------------------------------
/// Get a cached handle
udo_handle udo_get_cached_handle(uint64_t cacheKey);
//---------------------------------------------------------------------------
/// Analyze a C++ UDO
udo_errno udo_cxxudo_analyze(udo_handle handle);
//---------------------------------------------------------------------------
/// Get the arguments the UDO takes
udo_errno udo_get_arguments(udo_handle handle, udo_arguments* args);
//---------------------------------------------------------------------------
/// Get the attributes of the tuples the UDO generates
udo_errno udo_get_output_attributes(udo_handle handle, udo_attribute_descr_array* attrDescrs);
//---------------------------------------------------------------------------
/// Get the attributes of the input the UDO expects, or NULL if it has no input
udo_errno udo_get_input_attributes(udo_handle handle, udo_attribute_descr_array* attrDescrs);
//---------------------------------------------------------------------------
/// Get the size of the UDO object
size_t udo_get_size(udo_handle handle);
//---------------------------------------------------------------------------
/// Compile a C++ UDO to an object file after it was analyzed
udo_errno udo_cxxudo_compile(udo_handle handle);
//---------------------------------------------------------------------------
/// Link a compiled C++ UDO
udo_errno udo_cxxudo_link(udo_handle handle, udo_cxx_functors functors, udo_cxx_allocation_funcs allocationFuncs, int64_t tlsBlockOffset, uint64_t tlsBlockSize, udo_cxx_functions* functions);
//---------------------------------------------------------------------------
/// Get a pointer that can be used as an argument to the global constructor
void* udo_cxxudo_get_constructor_arg(udo_handle handle);
//---------------------------------------------------------------------------
#ifdef __cplusplus
}
#endif
//---------------------------------------------------------------------------
#endif
