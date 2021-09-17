cmake_minimum_required(VERSION 3.12)
project(udo_runtime)
#---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
#---------------------------------------------------------------------------
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wold-style-cast -Woverloaded-virtual -Wmissing-declarations -fno-rtti -fno-stack-protector -fno-semantic-interposition")
#---------------------------------------------------------------------------
# Configure LLVM dependency
set(LLVM_ROOT "" CACHE PATH "Specify path to build directory of a custom LLVM. Check that LLVM_DIR matches!")
find_package(LLVM REQUIRED CONFIG)
add_library(udoLLVM INTERFACE)
target_include_directories(udoLLVM SYSTEM INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/thirdparty/llvm ${LLVM_INCLUDE_DIRS})
target_compile_definitions(udoLLVM INTERFACE ${LLVM_DEFINITIONS})
target_link_libraries(udoLLVM INTERFACE LLVM)
#---------------------------------------------------------------------------
# LLVM vendoring
find_file(LLVM_STLEXTRAS "llvm/ADT/STLExtras.h" PATHS ${LLVM_INCLUDE_DIRS} REQUIRED NO_DEFAULT_PATH)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/thirdparty/llvm/llvm/ADT)
add_custom_command(
   OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/thirdparty/llvm/patched
   COMMAND cp ${LLVM_STLEXTRAS} ${CMAKE_CURRENT_BINARY_DIR}/thirdparty/llvm/llvm/ADT/STLExtras.h &&
      patch -s --no-backup-if-mismatch -p3 < ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/llvm/95d0d8e9e9d10da3cfa503fbba405e740aea3cc1.patch && touch patched
   WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/thirdparty/llvm
   DEPENDS thirdparty/llvm/95d0d8e9e9d10da3cfa503fbba405e740aea3cc1.patch
)
add_custom_target(udoLLVMVendor DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/thirdparty/llvm/patched)
add_dependencies(udoLLVM udoLLVMVendor)
#---------------------------------------------------------------------------
# Configure Clang dependency
find_package(Clang REQUIRED CONFIG HINTS ${LLVM_DIR} ${LLVM_INSTALL_PREFIX} "${LLVM_DIR}/.." NO_DEFAULT_PATH)
#---------------------------------------------------------------------------
# Configure Postgres dependency
set(PostgreSQL_ROOT "" CACHE PATH "Specify path to the prefix were PostgreSQL was installed.")
find_package(PostgreSQL REQUIRED)
#---------------------------------------------------------------------------
set(CXXUDO_DEPS_PREFIX "" CACHE PATH "Path to the prefix were the dependencies for C++ UDOs were installed")
if(NOT CXXUDO_DEPS_PREFIX)
   message(SEND_ERROR "CXXUDO_DEPS_PREFIX not set")
else()
   configure_file(src/udo/CxxUDOConfig.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/src/udo/CxxUDOConfig.hpp @ONLY)

   set(clang_libs
      clangAST
      clangASTMatchers
      clangAnalysis
      clangBasic
      clangCodeGen
      clangDriver
      clangEdit
      clangFrontend
      clangLex
      clangParse
      clangSema
      clangSerialization
   )
   add_library(udoClang INTERFACE)
   target_include_directories(udoClang SYSTEM INTERFACE ${CLANG_INCLUDE_DIRS})
   target_link_libraries(udoClang INTERFACE ${clang_libs})
endif()
#---------------------------------------------------------------------------
# Generate UDORuntime.cpp
find_program(CXXUDO_DEFAULT_CLANGXX NAMES clang++-${LLVMVERSION} clang++ HINTS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
file(READ runtime/UDOperator.hpp CXXUDO_HEADER)
configure_file(src/udo/UDORuntime.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/src/udo/UDORuntime.cpp @ONLY)
#---------------------------------------------------------------------------
add_library(udoruntime STATIC
   src/udo/AuxVec.cpp
   src/udo/ClangCompiler.cpp
   src/udo/CxxUDOAnalyzer.cpp
   src/udo/CxxUDOCompiler.cpp
   src/udo/CxxUDOExecution.cpp
   src/udo/DynamicTLS.cpp
   src/udo/LLVMCompiler.cpp
   src/udo/LLVMMetadata.cpp
   src/udo/Setting.cpp
   src/udo/UDOMemoryManager.cpp
   src/udo/i18n.cpp
   ${CMAKE_CURRENT_BINARY_DIR}/src/udo/UDORuntime.cpp
)
target_include_directories(udoruntime PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_SOURCE_DIR}/src
   ${CMAKE_CURRENT_BINARY_DIR}/src
)
set_target_properties(udoruntime PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
target_link_libraries(udoruntime udoLLVM udoClang)
#---------------------------------------------------------------------------
add_library(udoruntime_pg SHARED
   postgres/udo/udo_runtime.cpp
)
target_include_directories(udoruntime_pg PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/postgres
)
target_include_directories(udoruntime_pg SYSTEM PUBLIC
   ${PostgreSQL_INCLUDE_DIRS}
)
target_link_libraries(udoruntime_pg udoruntime)
#---------------------------------------------------------------------------
