#ifndef H_udo_Setting
#define H_udo_Setting
//---------------------------------------------------------------------------
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2017 Thomas Neumann
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
template <class T>
class SettingParser {
   public:
   /// Destructor
   virtual ~SettingParser() = default;
   /// Parse given string to value
   virtual bool parse(T& v, std::string_view nv) noexcept = 0;
   /// Description for parser
   virtual std::string parserDescription() noexcept = 0;
   /// Generate readable output string from value
   virtual std::string output(const T& v) noexcept = 0;
};
//---------------------------------------------------------------------------
namespace settinghelper {
//---------------------------------------------------------------------------
template <typename T>
class DefaultParserFunctions {};
template <>
class DefaultParserFunctions<bool> {
   public:
   static bool parse(bool& v, std::string_view nv) noexcept;
   static std::string parserDescription() noexcept;
   static std::string output(const bool& v) noexcept;
};
template <>
class DefaultParserFunctions<unsigned> {
   public:
   static bool parse(unsigned& v, std::string_view nv) noexcept;
   static std::string parserDescription() noexcept;
   static std::string output(const unsigned& v) noexcept;
};
template <>
class DefaultParserFunctions<uint64_t> {
   public:
   static bool parse(uint64_t& v, std::string_view nv) noexcept;
   static std::string parserDescription() noexcept;
   static std::string output(const uint64_t& v) noexcept;
};
template <>
class DefaultParserFunctions<double> {
   public:
   static bool parse(double& v, std::string_view nv) noexcept;
   static std::string parserDescription() noexcept;
   static std::string output(const double& v) noexcept;
};
template <>
class DefaultParserFunctions<std::string> {
   public:
   static bool parse(std::string& v, std::string_view nv) noexcept;
   static std::string parserDescription() noexcept;
   static std::string output(const std::string& v) noexcept;
};
template <typename T>
class DefaultParser : public SettingParser<T> {
   bool parse(T& v, std::string_view nv) noexcept override { return DefaultParserFunctions<T>::parse(v, nv); }
   std::string parserDescription() noexcept override { return DefaultParserFunctions<T>::parserDescription(); }
   std::string output(const T& v) noexcept override { return DefaultParserFunctions<T>::output(v); }
};
//---------------------------------------------------------------------------
/// Helper class for parsing enums
template <typename T, std::size_t N>
class EnumParser : public SettingParser<T> {
   public:
   // An element consists of (value, description, input character)
   using Element = std::tuple<T, std::string, char>;
   /// The elements
   std::array<Element, N> elements;

   // Constructor
   constexpr EnumParser(std::array<Element, N> elements) : elements(elements) {}

   /// Parse value
   bool parse(T& v, std::string_view nv) noexcept override {
      if (nv.empty()) return false;
      for (const auto& [e, d, c] : elements) {
         if (nv[0] == c) {
            v = e;
            return true;
         }
      }
      return false;
   }
   /// Parser description
   std::string parserDescription() noexcept override {
      std::string description;
      bool first = true;
      for (const auto& [e, d, c] : elements) {
         if (first)
            first = false;
         else
            description += ", ";
         description += d + ":'" + c + "'";
      }
      return description;
   }

   /// Get output character
   char outputChar(const T& v) noexcept {
      for (const auto& [e, d, c] : elements) {
         if (v == e) {
            return c;
         }
      }
      return '?'; // unreachable, unless value is corrupted
   }
   /// Get output string
   std::string output(const T& v) noexcept override { return std::string(1, outputChar(v)); }
};
//---------------------------------------------------------------------------
template <typename... Elements>
auto makeEnumParser(Elements... elements) noexcept
// Build a parser for enums, takes tuples of form (T value, string desc, char inputCharacter)
{
   using T = typename std::tuple_element<0, typename std::tuple_element<0, std::tuple<Elements...>>::type>::type;
   constexpr auto N = sizeof...(Elements);
   using Element = typename EnumParser<T, N>::Element;
   return std::unique_ptr<SettingParser<T>>(std::make_unique<EnumParser<T, N>>(std::array<Element, N>{elements...}));
}
//---------------------------------------------------------------------------
template <typename T, typename ParseFunc = decltype(&DefaultParserFunctions<T>::parse), typename DescFunc = decltype(&DefaultParserFunctions<T>::parserDescription), typename OutputFunc = decltype(&DefaultParserFunctions<T>::output)>
std::unique_ptr<SettingParser<T>> makeParser(ParseFunc&& parse = &DefaultParserFunctions<T>::parse, DescFunc&& desc = &DefaultParserFunctions<T>::parserDescription, OutputFunc&& output = &DefaultParserFunctions<T>::output) noexcept
// Helper for building a parser given custom functions
{
   // Define a custom parser that uses the given functions
   class Parser : public SettingParser<T> {
      ParseFunc p;
      DescFunc d;
      OutputFunc o;

      public:
      Parser(ParseFunc&& p, DescFunc&& d, OutputFunc&& o) noexcept : p(std::move(p)), d(std::move(d)), o(std::move(o)) {}
      bool parse(T& v, std::string_view nv) noexcept override { return p(v, nv); };
      std::string parserDescription() noexcept override { return d(); }
      std::string output(const T& v) noexcept override { return o(v); }
   };
   return std::make_unique<Parser>(std::move(parse), std::move(desc), std::move(output));
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
/// Base class to simplify implementing settings
class SettingBase {
   protected:
   /// The setting name
   std::string_view name;
   /// The setting description
   std::string_view description;
   /// Possible states
   enum State : char { Uninitialized = 0,
                       Locked,
                       Defined,
                       Undefined };
   /// The state
   std::atomic<char> state;

   /// Constructor
   SettingBase(std::string_view name, std::string_view description) noexcept;
   /// Destructor
   ~SettingBase();

   /// The actual initialization logic
   void initializeImpl() noexcept;
   /// Make sure that the value is initialized
   void ensureInitialized() noexcept {
      if (state.load() < Defined) initializeImpl();
   }
   /// Helper to set locked
   void setImpl(const void* nv, bool defined) noexcept;

   /// Register a setting
   void registerSetting() noexcept;
   /// Unregister a setting
   void unregisterSetting() noexcept;

   /// Interpret a string
   virtual bool interpret(std::string_view value) noexcept = 0;
   /// Set the value
   virtual void doSet(const void* nv) noexcept = 0;
   /// Get parser description
   virtual std::string getParserDescription() const noexcept = 0;

   public:
   /// Get name
   std::string_view getName() const noexcept { return name; }
   /// Get description
   std::string getDescription() const noexcept;
   /// Get the readable value
   virtual std::string getOutput() noexcept = 0;
   /// Set from a string
   bool setFromString(std::string_view nv) noexcept;
   /// Reset to default value
   virtual void reset() noexcept = 0;

   /// Get list of settings sorted by name
   static std::vector<std::reference_wrapper<SettingBase>> getAllSettings() noexcept;
   /// Get a single setting
   static SettingBase* getSetting(std::string_view name) noexcept;
};
//---------------------------------------------------------------------------
/// A system-wide setting. Usually used for debugging
template <class T>
class Setting : public SettingBase {
   public:
   /// A parser function
   using Parser = bool (*)(T&, const std::string&) noexcept;
   /// An output function
   using Output = std::string (*)(const T&) noexcept;

   private:
   /// The default value (as chosen by implementer)
   T defaultValue;
   /// The value
   T value;
   /// The parser
   std::unique_ptr<SettingParser<T>> parser;

   /// Interpret a string
   bool interpret(std::string_view nv) noexcept override { return parser->parse(value, nv); }
   /// Set the value
   void doSet(const void* nv) noexcept override { value = *static_cast<const T*>(nv); }

   public:
   /// Constructor
   Setting(std::string_view name, std::string_view description, T value = {}, std::unique_ptr<SettingParser<T>>&& parser = std::make_unique<settinghelper::DefaultParser<T>>()) noexcept : SettingBase(name, description), defaultValue(value), value(std::move(value)), parser(move(parser)) {}

   /// Access the value
   explicit operator const T&() noexcept {
      ensureInitialized();
      return value;
   }
   /// Access the value
   const T& get() noexcept {
      ensureInitialized();
      return value;
   }
   /// Explicitly set the value
   Setting& operator=(const T& nv) {
      setImpl(&nv, true);
      return *this;
   }
   /// Set value
   void set(const T& nv) noexcept { setImpl(&nv, false); }
   /// Reset to default value
   void reset() noexcept override { setImpl(&defaultValue, false); }

   /// Get parser description
   std::string getParserDescription() const noexcept override { return parser->parserDescription(); }
   /// Get the readable value
   std::string getOutput() noexcept override {
      ensureInitialized();
      return parser->output(value);
   }
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
#endif
