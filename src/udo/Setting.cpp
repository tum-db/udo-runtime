#include "udo/Setting.hpp"
#include "udo/ScopeGuard.hpp"
#include "udo/i18n.hpp"
#include <cassert>
#include <charconv>
#include <exception>
#include <iostream>
#include <mutex>
#include <unordered_map>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2017 Thomas Neumann
//---------------------------------------------------------------------------
using namespace std;
//---------------------------------------------------------------------------
namespace udo {
//---------------------------------------------------------------------------
static constexpr string_view tc = "udo/Setting"sv;
//---------------------------------------------------------------------------
namespace settinghelper {
//---------------------------------------------------------------------------
bool DefaultParserFunctions<bool>::parse(bool& v, string_view nv) noexcept
// Parse a boolean
{
   if (!nv.empty()) {
      if ((nv[0] == '1') || (nv[0] == 'y') || (nv[0] == 'Y') || (nv[0] == 't') || (nv[0] == 'T')) {
         v = true;
         return true;
      }
      if ((nv[0] == '0') || (nv[0] == 'n') || (nv[0] == 'N') || (nv[0] == 'f') || (nv[0] == 'F')) {
         v = false;
         return true;
      }
      if (((nv[0] == 'o') || (nv[0] == 'O')) && (nv.size() > 1)) {
         if ((nv[1] == 'n') || (nv[1] == 'N')) {
            v = true;
            return true;
         }
         if ((nv[1] == 'f') || (nv[1] == 'F')) {
            v = false;
            return true;
         }
      }
   }
   return false;
}
//---------------------------------------------------------------------------
string DefaultParserFunctions<bool>::parserDescription() noexcept
// Parser description
{
   return "on/off";
}
//---------------------------------------------------------------------------
string DefaultParserFunctions<bool>::output(const bool& v) noexcept
// Output
{
   return v ? "on" : "off";
}
//---------------------------------------------------------------------------
bool DefaultParserFunctions<unsigned>::parse(unsigned& v, string_view nv) noexcept
// Parse an unsigned
{
   if (!nv.empty()) {
      v = 0;
      for (char c : nv)
         if ((c >= '0') && (c <= '9'))
            v = 10 * v + c - '0';
         else
            return false;
      return true;
   }
   return false;
}
//---------------------------------------------------------------------------
string DefaultParserFunctions<unsigned>::output(const unsigned& v) noexcept
// Output
{
   return to_string(v);
}
//---------------------------------------------------------------------------
string DefaultParserFunctions<unsigned>::parserDescription() noexcept
// Parser description
{
   return "unsigned";
}
//---------------------------------------------------------------------------
bool DefaultParserFunctions<int>::parse(int& v, string_view nv) noexcept
// Parse an unsigned
{
   if (!nv.empty()) {
      if (nv.front() == '-') {
         v = -1;
         nv = nv.substr(1);
      } else {
         v = 1;
      }
      unsigned res;
      if (!DefaultParserFunctions<unsigned>::parse(res, nv))
         return false;
      v *= res;
      return true;
   }
   return false;
}
//---------------------------------------------------------------------------
string DefaultParserFunctions<int>::output(const int& v) noexcept
// Output
{
   return to_string(v);
}
//---------------------------------------------------------------------------
string DefaultParserFunctions<int>::parserDescription() noexcept
// Parser description
{
   return "int";
}
//---------------------------------------------------------------------------
bool DefaultParserFunctions<uint64_t>::parse(uint64_t& v, string_view nv) noexcept
// Parse an unsigned
{
   if (!nv.empty()) {
      v = 0;
      for (char c : nv)
         if ((c >= '0') && (c <= '9'))
            v = 10 * v + c - '0';
         else
            return false;
      return true;
   }
   return false;
}
//---------------------------------------------------------------------------
string DefaultParserFunctions<uint64_t>::output(const uint64_t& v) noexcept
// Output
{
   return to_string(v);
}
//---------------------------------------------------------------------------
string DefaultParserFunctions<uint64_t>::parserDescription() noexcept
// Parser description
{
   return "uint64";
}
//---------------------------------------------------------------------------
bool DefaultParserFunctions<uint64_t>::parseWithUnits(uint64_t& v, string_view s) noexcept
// Parse a byte specification with units. Default unit is mb
{
   // Interpret the units
   uint64_t units = 1ull << 20u;
   if (!s.empty()) {
      char c = s.back();
      if ((c == 'B') || (c == 'b')) {
         units = 1;
         s = s.substr(0, s.length() - 1);
      } else if ((c == 'K') || (c == 'k')) {
         units = 1ull << 10u;
         s = s.substr(0, s.length() - 1);
      } else if ((c == 'M') || (c == 'm')) {
         units = 1ull << 20u;
         s = s.substr(0, s.length() - 1);
      } else if ((c == 'G') || (c == 'g')) {
         units = 1ull << 30u;
         s = s.substr(0, s.length() - 1);
      }
   }

   // Parse the value
   uint64_t n = 0;
   for (char c : s) {
      if ((c >= '0') && (c <= '9')) {
         n = 10 * n + c - '0';
      } else {
         return false;
      }
   }
   n = n * units;
   if (n < (1ull << 10u)) n = 1ull << 10u;

   v = n;
   return true;
}
//---------------------------------------------------------------------------
bool DefaultParserFunctions<double>::parse(double& v, string_view nv) noexcept
// Parse a double
{
   if (!nv.empty()) {
      v = 0;
      auto res = from_chars(nv.data(), nv.data() + nv.size(), v);
      return (res.ec == errc()) && (res.ptr == (nv.data() + nv.size()));
   }
   return false;
}
//---------------------------------------------------------------------------
string DefaultParserFunctions<double>::output(const double& v) noexcept
// Output
{
   auto buffer = array<char, 32>();
   auto res = to_chars(buffer.data(), buffer.data() + buffer.size(), v);
   assert(res.ec == errc());
   return string(buffer.data(), distance(buffer.data(), res.ptr));
}
//---------------------------------------------------------------------------
string DefaultParserFunctions<double>::parserDescription() noexcept
// Parser description
{
   return "double";
}
//---------------------------------------------------------------------------
bool DefaultParserFunctions<string>::parse(string& v, string_view nv) noexcept
// Parse a string
{
   v = nv;
   return true;
}
//---------------------------------------------------------------------------
string DefaultParserFunctions<string>::output(const string& v) noexcept
// Output
{
   return v;
}
//---------------------------------------------------------------------------
string DefaultParserFunctions<string>::parserDescription() noexcept
// Parser description
{
   return "string";
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
// The error reporting stream
std::ostream* SettingBase::errorOut = &cerr;
//---------------------------------------------------------------------------
SettingBase::SettingBase(string_view name, string_view description) noexcept
   : name(name), description(description)
// Constructor
{
   state.store(Uninitialized);
   registerSetting();
}
//---------------------------------------------------------------------------
SettingBase::~SettingBase()
// Destructor
{
   unregisterSetting();
}
//---------------------------------------------------------------------------
string SettingBase::formatErrorMessage(string_view badArg)
// Format an error message
{
   return trformat(tc, "invalid value for setting \"{0}\": \"{1}\".\n"
                       "Available values for {2}: {3}",
                   getName(), badArg, description, getParserDescription());
}
//---------------------------------------------------------------------------
void SettingBase::initializeImpl() noexcept
// The actual initialization logic
{
   // Acquire the lock
   State oldState;
   do { oldState = static_cast<State>(state.exchange(Locked)); } while (oldState == Locked);
   auto unlock = ScopeGuard([&] { state.store(oldState, memory_order_release); });

   // Do we have to initialize it?
   if (!(oldState == Uninitialized)) return;
   // Check environment
   string envname;
   envname.reserve(name.size());
   for (char c : name) {
      if ((c >= 'a') && (c <= 'z'))
         c -= 'a' - 'A';
      else if (c == '.')
         c = '_';
      envname += c;
   }
   auto s = getenv(envname.c_str());
   if (!s) {
      oldState = Undefined;
      return;
   }
   if (interpret(s)) {
      oldState = Defined;
      return;
   }

   *errorOut << formatErrorMessage(s) << endl;
   oldState = Undefined;
}
//---------------------------------------------------------------------------
void SettingBase::setImpl(const void* nv, bool defined) noexcept
// Helper to set locked
{
   // Acquire the lock
   while (state.exchange(Locked) == Locked) {}

   // Update
   doSet(nv);

   // Release the lock
   state.store(defined ? Defined : Undefined);
}
//---------------------------------------------------------------------------
string SettingBase::getDescription() const noexcept
// Get description
{
   return string(description) + " (" + getParserDescription() + ")";
}
//---------------------------------------------------------------------------
namespace {
//---------------------------------------------------------------------------
/// Container for map and mutex
struct SettingsMap {
   /// Mutex for map
   mutex mut;
   /// Map of setting name to setting
   unordered_map<string_view, SettingBase*> settings;
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
static SettingsMap& getSettingsMap() noexcept {
   static SettingsMap sm;
   return sm;
}
//---------------------------------------------------------------------------
void SettingBase::registerSetting() noexcept
// Get the setting, create if necessary
{
   auto& settingsMap = getSettingsMap();
   lock_guard guard(settingsMap.mut);

   auto mySettingIt = settingsMap.settings.emplace(name, this);
   if (!mySettingIt.second) {
      // LCOV_EXCL_START Duplicate setting names are a programming error and must not exist.
      cerr << "setting name " << name << " cannot be reused\n";
      terminate();
      // LCOV_EXCL_STOP
   }
}
//---------------------------------------------------------------------------
void SettingBase::unregisterSetting() noexcept
// Get the setting, create if necessary
{
   auto& settingsMap = getSettingsMap();
   lock_guard guard(settingsMap.mut);

   settingsMap.settings.erase(name);
}
//---------------------------------------------------------------------------
vector<reference_wrapper<SettingBase>> SettingBase::getAllSettings() noexcept
// Get list of settings
{
   auto& settingsMap = getSettingsMap();
   lock_guard guard(settingsMap.mut);

   vector<reference_wrapper<SettingBase>> returnValues;
   returnValues.reserve(settingsMap.settings.size());
   for (const auto& [k, v] : settingsMap.settings) {
      returnValues.emplace_back(*v);
   }
   return returnValues;
}
//---------------------------------------------------------------------------
SettingBase* SettingBase::getSetting(string_view name) noexcept
// Get the output of a single setting
{
   auto& settingsMap = getSettingsMap();
   lock_guard guard(settingsMap.mut);

   auto it = settingsMap.settings.find(name);
   if (it == settingsMap.settings.end()) return nullptr;
   return it->second;
}
//---------------------------------------------------------------------------
bool SettingBase::setFromString(string_view nv) noexcept
// Set from a string
{
   // Acquire the lock
   State oldState;
   do { oldState = static_cast<State>(state.exchange(Locked)); } while (oldState == Locked);

   // Interpret the value
   bool result = interpret(nv);

   // Release the lock
   state.store(result ? Defined : oldState);

   return result;
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
