#ifndef H_udo_LLVMMetadata
#define H_udo_LLVMMetadata
//---------------------------------------------------------------------------
#include "thirdparty/tl/expected.hpp"
#include <climits>
#include <concepts>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2021 Moritz Sichert
//---------------------------------------------------------------------------
namespace llvm {
class BasicBlock;
class Function;
class GlobalVariable;
class Instruction;
class LLVMContext;
class Metadata;
class Module;
class Type;
}
//---------------------------------------------------------------------------
namespace udo::llvm_metadata {
//---------------------------------------------------------------------------
class MetadataReader;
class MetadataWriter;
//---------------------------------------------------------------------------
/// The return type of the IO functions
using IOResult = tl::expected<void, std::string>;
//---------------------------------------------------------------------------
/// The base template for LLVM metadata helpers
template <typename T>
struct IO {
   /// Read the value
   [[nodiscard]] static IOResult input(MetadataReader& reader, T& value);
   /// Write the value
   [[nodiscard]] static IOResult output(MetadataWriter& writer, const T& value);
};
//---------------------------------------------------------------------------
/// A helper class to read metadata from LLVM modules
class MetadataReader {
   public:
   /// The llvm module
   llvm::Module& module;
   /// The current metadata node
   llvm::Metadata* current;

   private:
   /// Read a node from a named metadata node
   IOResult readNamedNode(std::string_view name);

   public:
   /// Constructor
   MetadataReader(llvm::Module& module) : module(module), current(nullptr) {}

   /// Get the llvm context
   llvm::LLVMContext& getContext();

   /// Read a value from the current metadata
   template <typename T>
   [[nodiscard]] IOResult readValue(T& value) {
      return IO<std::remove_reference_t<T>>::input(*this, value);
   }
   /// Read a value from a named metadata node
   template <typename T>
   [[nodiscard]] IOResult readNamedValue(std::string_view name, T& value) {
      if (auto result = readNamedNode(name); !result)
         return result;
      return readValue(value);
   }

   /// Extract all nodes from the current node
   [[nodiscard]] IOResult readNodes(std::vector<llvm::Metadata*>& nodes);
   /// Extract multiple nodes from the current node
   [[nodiscard]] IOResult readNodes(std::vector<llvm::Metadata*>& nodes, size_t size);
};
//---------------------------------------------------------------------------
/// A helper class to write metadata to LLVM modules
class MetadataWriter {
   public:
   /// The llvm module
   llvm::Module& module;
   /// The current metadata node
   llvm::Metadata* current;

   private:
   /// Write the metadata with a name
   IOResult writeNamedNode(std::string_view name);

   public:
   /// Constructor
   MetadataWriter(llvm::Module& module) : module(module), current(nullptr) {}

   /// Get the llvm context
   llvm::LLVMContext& getContext();

   /// Write a value from to the metadata
   template <typename T>
   [[nodiscard]] IOResult writeValue(const T& value) {
      return IO<T>::output(*this, value);
   }
   /// Write a value to a named metadata
   template <typename T>
   [[nodiscard]] IOResult writeNamedValue(std::string_view name, const T& value) {
      if (auto result = writeValue(value); !result)
         return result;
      return writeNamedNode(name);
   }

   /// Write multiple nodes to the current node
   [[nodiscard]] IOResult writeNodes(const std::vector<llvm::Metadata*>& nodes);
};
//---------------------------------------------------------------------------
template <>
struct IO<intmax_t> {
   static IOResult input(MetadataReader& reader, intmax_t& value, size_t bitSize);
   static IOResult input(MetadataReader& reader, intmax_t& value) {
      return input(reader, value, sizeof(intmax_t) * CHAR_BIT);
   }
   static IOResult output(MetadataWriter& writer, intmax_t value, size_t bitSize);
   static IOResult output(MetadataWriter& writer, intmax_t value) {
      return output(writer, value, sizeof(intmax_t) * CHAR_BIT);
   }
};
//---------------------------------------------------------------------------
template <>
struct IO<uintmax_t> {
   static IOResult input(MetadataReader& reader, uintmax_t& value, size_t bitSize);
   static IOResult input(MetadataReader& reader, uintmax_t& value) {
      return input(reader, value, sizeof(uintmax_t) * CHAR_BIT);
   }
   static IOResult output(MetadataWriter& writer, uintmax_t value, size_t bitSize);
   static IOResult output(MetadataWriter& writer, uintmax_t value) {
      return output(writer, value, sizeof(uintmax_t) * CHAR_BIT);
   }
};
//---------------------------------------------------------------------------
template <std::integral T>
struct IO<T> {
   using maxintType = std::conditional_t<std::is_signed_v<T>, intmax_t, uintmax_t>;

   static IOResult input(MetadataReader& reader, T& value) {
      maxintType maxintValue;
      auto result = IO<maxintType>::input(reader, maxintValue, sizeof(T) * CHAR_BIT);
      if (result)
         value = static_cast<T>(maxintValue);
      return result;
   }
   static IOResult output(MetadataWriter& writer, T value) {
      return IO<maxintType>::output(writer, static_cast<maxintType>(value), sizeof(T) * CHAR_BIT);
   }
};
//---------------------------------------------------------------------------
template <>
struct IO<std::string_view> {
   static IOResult input(MetadataReader& reader, std::string_view& value);
   static IOResult output(MetadataWriter& writer, std::string_view value);
};
//---------------------------------------------------------------------------
template <>
struct IO<std::string> {
   static IOResult input(MetadataReader& reader, std::string& value);
   static IOResult output(MetadataWriter& writer, const std::string& value);
};
//---------------------------------------------------------------------------
template <>
struct IO<llvm::GlobalVariable*> {
   static IOResult input(MetadataReader& reader, llvm::GlobalVariable*& value);
   static IOResult output(MetadataWriter& writer, llvm::GlobalVariable* value);
};
//---------------------------------------------------------------------------
template <>
struct IO<llvm::Function*> {
   static IOResult input(MetadataReader& reader, llvm::Function*& value);
   static IOResult output(MetadataWriter& writer, llvm::Function* value);
};
//---------------------------------------------------------------------------
template <>
struct IO<llvm::BasicBlock*> {
   static IOResult input(MetadataReader& reader, llvm::BasicBlock*& value);
   static IOResult output(MetadataWriter& writer, llvm::BasicBlock* value);
};
//---------------------------------------------------------------------------
template <>
struct IO<llvm::Type*> {
   static IOResult input(MetadataReader& reader, llvm::Type*& value);
   static IOResult output(MetadataWriter& writer, llvm::Type* value);
};
//---------------------------------------------------------------------------
template <>
struct IO<llvm::Instruction*> {
   static IOResult input(MetadataReader& reader, llvm::Instruction*& value);
   static IOResult output(MetadataWriter& writer, llvm::Instruction* value);
};
//---------------------------------------------------------------------------
namespace impl {
//---------------------------------------------------------------------------
template <typename T>
concept ContainerIOType = requires(const T& constRef, T& ref) {
                             typename T::value_type;
                             { constRef.begin() } -> std::input_iterator;
                             { constRef.end() } -> std::input_iterator;
                             ref.emplace_back();
                          };
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
template <typename C>
struct ContainerIO {
   static IOResult input(MetadataReader& reader, C& value) {
      std::vector<llvm::Metadata*> metadataNodes;
      if (auto result = reader.readNodes(metadataNodes); !result)
         return result;

      for (auto* node : metadataNodes) {
         reader.current = node;
         if (auto result = reader.readValue(value.emplace_back()); !result)
            return result;
      }

      return {};
   }

   static IOResult output(MetadataWriter& writer, const C& value) {
      std::vector<llvm::Metadata*> metadataNodes;
      metadataNodes.reserve(value.size());

      for (const auto& elem : value) {
         if (auto result = writer.writeValue(elem); !result)
            return result;
         metadataNodes.push_back(writer.current);
      }

      return writer.writeNodes(metadataNodes);
   }
};
//---------------------------------------------------------------------------
template <impl::ContainerIOType T>
struct IO<T> : public ContainerIO<T> {
};
//---------------------------------------------------------------------------
template <typename... Ts>
struct IO<std::tuple<Ts...>> {
   private:
   template <size_t I, size_t... Is>
   static IOResult inputTuple(MetadataReader& reader, std::tuple<Ts...>& value, const std::vector<llvm::Metadata*>& metadataNodes, std::index_sequence<I, Is...>) {
      reader.current = metadataNodes[I];
      auto result = reader.readValue(std::get<I>(value));
      if (!result)
         return result;

      return inputTuple(reader, value, metadataNodes, std::index_sequence<Is...>{});
   }

   static IOResult inputTuple(MetadataReader& /*reader*/, std::tuple<Ts...>& /*value*/, const std::vector<llvm::Metadata*>& /*metadataNodes*/, std::index_sequence<>) {
      return {};
   }

   template <size_t I, size_t... Is>
   static IOResult outputTuple(MetadataWriter& writer, const std::tuple<Ts...>& value, std::vector<llvm::Metadata*>& metadataNodes, std::index_sequence<I, Is...>) {
      auto result = writer.writeValue(std::get<I>(value));
      if (!result)
         return result;
      metadataNodes[I] = writer.current;

      return outputTuple(writer, value, metadataNodes, std::index_sequence<Is...>{});
   }

   static IOResult outputTuple(MetadataWriter& /*writer*/, const std::tuple<Ts...>& /*value*/, std::vector<llvm::Metadata*>& /*metadataNodes*/, std::index_sequence<>) {
      return {};
   }

   public:
   static IOResult input(MetadataReader& reader, std::tuple<Ts...>& value) {
      std::vector<llvm::Metadata*> metadataNodes;
      metadataNodes.resize(sizeof...(Ts));
      if (auto result = reader.readNodes(metadataNodes, sizeof...(Ts)); !result)
         return result;

      return inputTuple(reader, value, metadataNodes, std::make_index_sequence<sizeof...(Ts)>{});
   }

   static IOResult output(MetadataWriter& writer, const std::tuple<Ts...>& value) {
      std::vector<llvm::Metadata*> metadataNodes;
      metadataNodes.resize(sizeof...(Ts));
      auto result = outputTuple(writer, value, metadataNodes, std::make_index_sequence<sizeof...(Ts)>{});
      if (!result)
         return result;

      return writer.writeNodes(metadataNodes);
   }
};
//---------------------------------------------------------------------------
template <typename T, typename U>
struct IO<std::pair<T, U>> {
   static IOResult input(MetadataReader& reader, std::pair<T, U>& value) {
      return reader.readValue(std::tie(value.first, value.second));
   }
   static IOResult output(MetadataWriter& writer, const std::pair<T, U>& value) {
      return writer.writeValue(std::tie(value.first, value.second));
   }
};
//---------------------------------------------------------------------------
/// The context for a StructMapper
class StructContext {
   private:
   friend class StructMapperBase;

   /// The module
   llvm::Module* module;
   /// The metadata for the struct members
   std::vector<llvm::Metadata*> members;
   /// Reading?
   bool reading;

   public:
   /// Reading?
   bool isReading() const { return reading; }
};
//---------------------------------------------------------------------------
/// Base class for StructMapper
class StructMapperBase {
   private:
   /// A helper to pass the input/output functions to handleMember
   template <typename T>
   struct HandleMemberHelper {
      static IOResult input(MetadataReader& reader, void* value) {
         return IO<T>::input(reader, *static_cast<T*>(value));
      }
      static IOResult output(MetadataWriter& writer, const void* value) {
         return IO<T>::output(writer, *static_cast<const T*>(value));
      }
   };

   /// A helper to pass the sameClass function to handleInstruction
   template <typename T>
   struct HandleInstructionHelper {
      static bool sameClass(void* ptr) {
         return T::classof(static_cast<llvm::Instruction*>(ptr));
      }
   };

   /// Handle a struct member
   static IOResult handleMember(StructContext& context, void* value, IOResult (*input)(MetadataReader&, void*), IOResult (*output)(MetadataWriter&, const void*));
   /// Handle an instruction
   static IOResult handleInstruction(StructContext& context, void* value, bool (*sameClass)(void*));

   protected:
   /// A helper to pass enumEntries to inputStruct/outputStruct
   template <typename T>
   struct EnumEntriesHelper {
      static IOResult enumEntries(StructContext& context, void* value) {
         return IO<T>::enumEntries(context, *static_cast<T*>(value));
      }
   };

   /// Read the struct
   static IOResult inputStruct(MetadataReader& reader, void* value, IOResult (*enumEntries)(StructContext&, void*));
   /// Write the struct
   static IOResult outputStruct(MetadataWriter& writer, const void* value, IOResult (*enumEntries)(StructContext&, void*));

   public:
   /// Handle a struct member
   template <class T>
   [[nodiscard]] static IOResult mapMember(StructContext& context, T& value) {
      return handleMember(context, &value, &HandleMemberHelper<T>::input, &HandleMemberHelper<T>::output);
   }

   /// Handle an instruction
   template <class T>
   [[nodiscard]] static IOResult mapInstruction(StructContext& context, T*& value) {
      return handleInstruction(context, &value, &HandleInstructionHelper<T>::sameClass);
   }
};
//---------------------------------------------------------------------------
/// Base class for IO<T> when T is a struct.
/// Must only define enumEntries(StructContext&, T& value) which then should
/// call mapMember() for each member.
template <class T>
struct StructMapper : public StructMapperBase {
   static IOResult input(MetadataReader& reader, T& value) {
      return inputStruct(reader, &value, &EnumEntriesHelper<T>::enumEntries);
   }
   static IOResult output(MetadataWriter& writer, const T& value) {
      return outputStruct(writer, &value, &EnumEntriesHelper<T>::enumEntries);
   }
};
//---------------------------------------------------------------------------
}
#endif
