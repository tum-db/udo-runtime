#include "udo/LLVMMetadata.hpp"
#include "udo/i18n.hpp"
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>
#include <algorithm>
#include <cassert>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
using namespace std;
//---------------------------------------------------------------------------
namespace udo::llvm_metadata {
//---------------------------------------------------------------------------
static const char tc[] = "udo/LLVMMetadata";
//---------------------------------------------------------------------------
template <typename T>
static IOResult error(T value) {
   return tl::unexpected(string(move(value)));
}
//---------------------------------------------------------------------------
llvm::LLVMContext& MetadataReader::getContext()
// Get the llvm context
{
   return module.getContext();
}
//---------------------------------------------------------------------------
IOResult MetadataReader::readNamedNode(string_view name)
// Read a node from a named metadata node
{
   auto* nmd = module.getNamedMetadata(llvm::StringRef(name.data(), name.size()));
   if (!nmd)
      return error(tr(tc, "did not find named metadata"));
   if (nmd->getNumOperands() != 1)
      return error(tr(tc, "unexpected number of operands in named metadata"));

   auto* operand = nmd->getOperand(0);
   if (operand->getNumOperands() != 1)
      return error(tr(tc, "unexpected number of operands in named metadata"));
   current = operand->getOperand(0);

   return {};
}
//---------------------------------------------------------------------------
IOResult MetadataReader::readNodes(vector<llvm::Metadata*>& nodes)
// Extract all nodes from the current node
{
   auto* mdTuple = llvm::dyn_cast<llvm::MDTuple>(current);
   if (!mdTuple)
      return error(tr(tc, "unexpected metadata type"));

   for (auto& op : mdTuple->operands())
      nodes.push_back(op);
   return {};
}
//---------------------------------------------------------------------------
IOResult MetadataReader::readNodes(vector<llvm::Metadata*>& nodes, size_t size)
// Extract multiple nodes from the current node
{
   auto* mdTuple = llvm::dyn_cast<llvm::MDTuple>(current);
   if (!mdTuple)
      return error(tr(tc, "unexpected metadata type"));

   if (mdTuple->getNumOperands() != size)
      return error(tr(tc, "mismatching number of operands"));

   for (auto& op : mdTuple->operands())
      nodes.push_back(op);
   return {};
}
//---------------------------------------------------------------------------
llvm::LLVMContext& MetadataWriter::getContext()
// Get the llvm context
{
   return module.getContext();
}
//---------------------------------------------------------------------------
IOResult MetadataWriter::writeNamedNode(string_view name)
// Write the metadata with a name
{
   auto* nmd = module.getOrInsertNamedMetadata(llvm::StringRef(name.data(), name.size()));
   nmd->clearOperands();
   nmd->addOperand(llvm::MDNode::get(getContext(), {current}));
   return {};
}
//---------------------------------------------------------------------------
IOResult MetadataWriter::writeNodes(const vector<llvm::Metadata*>& nodes)
// Write multiple nodes to the current node
{
   current = llvm::MDTuple::get(getContext(), nodes);
   return {};
}
//---------------------------------------------------------------------------
static IOResult readIntMd(MetadataReader& reader, llvm::ConstantInt*& intMdOut, size_t bitSize)
// Read the metadata for a generic constant int
{
   auto* constMd = llvm::dyn_cast<llvm::ConstantAsMetadata>(reader.current);
   if (!constMd)
      return error(tr(tc, "unexpected metadata type"));

   auto* intMd = llvm::dyn_cast<llvm::ConstantInt>(constMd->getValue());
   if (!intMd)
      return error(tr(tc, "unexpected metadata value type"));

   if (intMd->getBitWidth() != bitSize)
      return error(tr(tc, "unexpected size of int"));

   intMdOut = intMd;
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<intmax_t>::input(MetadataReader& reader, intmax_t& value, size_t bitSize) {
   llvm::ConstantInt* intMd{};
   if (auto result = readIntMd(reader, intMd, bitSize); !result)
      return result;
   value = intMd->getSExtValue();
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<intmax_t>::output(MetadataWriter& writer, intmax_t value, size_t bitSize) {
   auto* intType = llvm::IntegerType::get(writer.getContext(), bitSize);
   auto* intConst = llvm::ConstantInt::getSigned(intType, value);
   writer.current = llvm::ValueAsMetadata::getConstant(intConst);
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<uintmax_t>::input(MetadataReader& reader, uintmax_t& value, size_t bitSize) {
   llvm::ConstantInt* intMd{};
   if (auto result = readIntMd(reader, intMd, bitSize); !result)
      return result;
   value = intMd->getZExtValue();
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<uintmax_t>::output(MetadataWriter& writer, uintmax_t value, size_t bitSize) {
   auto* intType = llvm::IntegerType::get(writer.getContext(), bitSize);
   auto* intConst = llvm::ConstantInt::get(intType, value);
   writer.current = llvm::ValueAsMetadata::getConstant(intConst);
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<string_view>::input(MetadataReader& reader, string_view& value) {
   auto* strMd = llvm::dyn_cast<llvm::MDString>(reader.current);
   if (!strMd)
      return error(tr(tc, "unexpected metadata type"));

   auto ref = strMd->getString();
   value = string_view{ref.data(), ref.size()};
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<string_view>::output(MetadataWriter& writer, string_view value) {
   llvm::StringRef valueRef(value.data(), value.size());
   writer.current = llvm::MDString::get(writer.getContext(), valueRef);
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<string>::input(MetadataReader& reader, string& value) {
   string_view sv;
   auto result = IO<string_view>::input(reader, sv);
   if (result)
      value = sv;
   return result;
}
//---------------------------------------------------------------------------
IOResult IO<string>::output(MetadataWriter& writer, const string& value) {
   return IO<string_view>::output(writer, value);
}
//---------------------------------------------------------------------------
IOResult IO<llvm::GlobalVariable*>::input(MetadataReader& reader, llvm::GlobalVariable*& value) {
   auto* constMd = llvm::dyn_cast<llvm::ConstantAsMetadata>(reader.current);
   if (!constMd)
      return error(tr(tc, "unexpected metadata type"));

   auto* mdValue = constMd->getValue();

   if (llvm::isa<llvm::ConstantInt>(mdValue)) {
      value = nullptr;
   } else {
      auto* globalVar = llvm::dyn_cast<llvm::GlobalVariable>(mdValue);
      if (!globalVar)
         return error(tr(tc, "unexpected metadata value type"));

      value = globalVar;
   }
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<llvm::GlobalVariable*>::output(MetadataWriter& writer, llvm::GlobalVariable* value) {
   if (value) {
      writer.current = llvm::ValueAsMetadata::getConstant(value);
   } else {
      writer.current = llvm::ValueAsMetadata::getConstant(llvm::ConstantInt::getFalse(writer.getContext()));
   }
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<llvm::Function*>::input(MetadataReader& reader, llvm::Function*& value) {
   auto* constMd = llvm::dyn_cast<llvm::ConstantAsMetadata>(reader.current);
   if (!constMd)
      return error(tr(tc, "unexpected metadata type"));

   auto* mdValue = constMd->getValue();

   if (llvm::isa<llvm::ConstantInt>(mdValue)) {
      value = nullptr;
   } else {
      auto* func = llvm::dyn_cast<llvm::Function>(constMd->getValue());
      if (!func)
         return error(tr(tc, "unexpected metadata value type"));

      value = func;
   }
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<llvm::Function*>::output(MetadataWriter& writer, llvm::Function* value) {
   if (value) {
      writer.current = llvm::ValueAsMetadata::getConstant(value);
   } else {
      writer.current = llvm::ValueAsMetadata::getConstant(llvm::ConstantInt::getFalse(writer.getContext()));
   }
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<llvm::BasicBlock*>::input(MetadataReader& reader, llvm::BasicBlock*& value) {
   tuple<llvm::Function*, size_t> bbMetadata;
   if (auto result = reader.readValue(bbMetadata); !result)
      return result;
   auto [func, index] = bbMetadata;

   unsigned i = 0;
   for (auto& block : *func) {
      if (i == index) {
         value = &block;
         return {};
      }
      ++i;
   }

   return error(tr(tc, "block index out of range"));
}
//---------------------------------------------------------------------------
IOResult IO<llvm::BasicBlock*>::output(MetadataWriter& writer, llvm::BasicBlock* value) {
   llvm::Function* func = value->getParent();
   size_t index = 0;
   for (auto& block : *func) {
      if (&block == value)
         return writer.writeValue(tuple<llvm::Function*, size_t>(func, index));
      ++index;
   }
   return error(tr(tc, "basic block not found in function"));
}
//---------------------------------------------------------------------------
IOResult IO<llvm::Instruction*>::input(MetadataReader& reader, llvm::Instruction*& value) {
   tuple<llvm::BasicBlock*, size_t> instrMetadata{};
   if (auto result = reader.readValue(instrMetadata); !result)
      return result;
   auto [bb, index] = instrMetadata;

   unsigned i = 0;
   for (auto& instr : *bb) {
      if (i == index) {
         value = &instr;
         return {};
      }
      ++i;
   }

   return error(tr(tc, "instruction index out of range"));
}
//---------------------------------------------------------------------------
IOResult IO<llvm::Instruction*>::output(MetadataWriter& writer, llvm::Instruction* value) {
   llvm::BasicBlock* bb = value->getParent();
   size_t index = 0;
   for (auto& instr : *bb) {
      if (&instr == value)
         return writer.writeValue(tuple<llvm::BasicBlock*, size_t>(bb, index));
      ++index;
   }
   return error(tr(tc, "instruction not found in basic block"));
}
//---------------------------------------------------------------------------
IOResult IO<llvm::Type*>::input(MetadataReader& reader, llvm::Type*& value) {
   llvm::Function* dummyFunc{};
   if (auto result = reader.readValue(dummyFunc); !result)
      return result;

   value = dummyFunc->getFunctionType()->getReturnType();
   return {};
}
//---------------------------------------------------------------------------
IOResult IO<llvm::Type*>::output(MetadataWriter& writer, llvm::Type* value) {
   auto& context = writer.module.getContext();
   auto* dummyFuncType = llvm::FunctionType::get(value, false);
   auto* dummyFunc = llvm::Function::Create(dummyFuncType, llvm::Function::PrivateLinkage, "__metadataTypeDummyFunc", writer.module);
   auto* bb = llvm::BasicBlock::Create(context, {}, dummyFunc);
   new llvm::UnreachableInst(context, bb);
   return writer.writeValue(dummyFunc);
}
//---------------------------------------------------------------------------
IOResult StructMapperBase::handleMember(StructContext& context, void* value, IOResult (*input)(MetadataReader&, void*), IOResult (*output)(MetadataWriter&, const void*)) {
   if (context.isReading()) {
      if (context.members.empty())
         return error(tr(tc, "unexpected end of struct members while reading member"));
      MetadataReader reader(*context.module);
      reader.current = context.members.back();
      auto result = input(reader, value);
      if (!result)
         return result;
      context.members.pop_back();
   } else {
      MetadataWriter writer(*context.module);
      auto result = output(writer, value);
      if (!result)
         return result;
      assert(writer.current);
      context.members.push_back(writer.current);
   }
   return {};
}
//---------------------------------------------------------------------------
IOResult StructMapperBase::handleInstruction(StructContext& context, void* value, bool (*sameClass)(void*))
// Handle an instruction
{
   auto*& instruction = *static_cast<llvm::Instruction**>(value);
   auto result = mapMember(context, instruction);
   if (!result)
      return result;
   if (context.isReading()) {
      if (!sameClass(instruction))
         return error(tr(tc, "unexpected instruction type"));
   }
   return {};
}
//---------------------------------------------------------------------------
IOResult StructMapperBase::inputStruct(MetadataReader& reader, void* value, IOResult (*enumEntries)(StructContext&, void*))
// Read the struct
{
   StructContext context;
   context.module = &reader.module;
   context.reading = true;
   if (auto result = reader.readNodes(context.members); !result)
      return result;
   reverse(context.members.begin(), context.members.end());

   if (auto result = enumEntries(context, value); !result)
      return result;

   if (!context.members.empty())
      return error(tr(tc, "unexpected remaining members while reading struct"));

   return {};
}
//---------------------------------------------------------------------------
IOResult StructMapperBase::outputStruct(MetadataWriter& writer, const void* value, IOResult (*enumEntries)(StructContext&, void*))
// Write the struct
{
   StructContext context;
   context.module = &writer.module;
   context.reading = false;

   if (auto result = enumEntries(context, const_cast<void*>(value)); !result)
      return result;

   if (auto result = writer.writeNodes(context.members); !result)
      return result;

   return {};
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
