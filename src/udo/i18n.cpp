#include "udo/i18n.hpp"
#include <cstdio>
#include <limits>
//---------------------------------------------------------------------------
// UDO runtime
// (c) 2016 Thomas Neumann
//---------------------------------------------------------------------------
using namespace std;
//---------------------------------------------------------------------------
namespace udo { namespace i18n {
//---------------------------------------------------------------------------
const char* translate(const char* /*context*/, const char* text)
// The main translation logic
{
   // A no-op for now, we can add that later.
   // It should check if the (context,text) pair exists in the current translation file, and returns the new string if it does
   return text;
}
//---------------------------------------------------------------------------
void Format<const char*>::format(string& out, const char* /*format*/, unsigned /*formatLen*/, const void* value) { out += *(static_cast<const char* const*>(value)); }
void Format<char*>::format(string& out, const char* /*format*/, unsigned /*formatLen*/, const void* value) { out += *(static_cast<const char* const*>(value)); }
void Format<string>::format(string& out, const char* /*format*/, unsigned /*formatLen*/, const void* value) { out += *(static_cast<const string*>(value)); }
void Format<string_view>::format(string& out, const char* /*format*/, unsigned /*formatLen*/, const void* value) { out += *(static_cast<const string_view*>(value)); }
void Format<bool>::format(string& out, const char* /*format*/, unsigned /*formatLen*/, const void* value) {
   const char* s = (*(static_cast<const bool*>(value))) ? "true" : "false";
   out += s;
}
void Format<char>::format(string& out, const char* /*format*/, unsigned /*formatLen*/, const void* value) {
   char c = *static_cast<const char*>(value);
   out += c;
}
//---------------------------------------------------------------------------
static bool parseUnsigned(const char* from, const char* to, unsigned& result)
// Try to parse an unsigned number
{
   unsigned r = 0;
   for (const char* iter = from; iter < to; ++iter) {
      unsigned v = (*iter) - '0';
      if (v > 9)
         return false;
      r = 10 * r + v;
   }
   result = r;
   return true;
}
//---------------------------------------------------------------------------
static void formatUInt(string& out, const char* format, unsigned formatLen, uint64_t value)
// Format an unsigned value
{
   unsigned precision = 0;
   if (formatLen) {
      if (parseUnsigned(format + 1, format + formatLen, precision)) switch (*format) {
            case 'N':
            case 'n': {
               // Compute the required length
               unsigned length = 0;
               for (uint64_t v = value; v; v /= 10) ++length;
               if (!length)
                  length = 1;
               length += (length - 1) / 3;
               if (precision >= 4)
                  precision += (precision - 1) / 3;
               if (length < precision)
                  length = precision;

               // Grow the target string
               unsigned outLen = out.length();
               out.resize(outLen + length);
               char *writerLimit = &out[outLen], *writer = writerLimit + length - 1;

               // Write the number, inserting ',' every 3 digits
               unsigned digits = 0;
               for (uint64_t v = value; v; v /= 10) {
                  *(writer--) = (v % 10) + '0';
                  if ((++digits) == 3) {
                     if (writer > writerLimit)
                        *(writer--) = ',';
                     digits = 0;
                  }
               }
               while (writer >= writerLimit) {
                  *(writer--) = '0';
                  if ((++digits) == 3) {
                     if (writer > writerLimit)
                        *(writer--) = ',';
                     digits = 0;
                  }
               }
               return;
            }
            case 'P':
            case 'p':
               formatUInt(out, nullptr, 0, value);
               out += '%';
               return;
            case 'X':
            case 'x': { // Hex
               // Compute the required length
               unsigned length = 0;
               for (uint64_t v = value; v; v /= 16)
                  ++length;
               if (!length)
                  length = 1;
               if (length < precision)
                  length = precision;

               // Grow the target string
               unsigned outLen = out.length();
               out.resize(outLen + length);
               char *writerLimit = &out[outLen], *writer = writerLimit + length - 1;

               // And write it
               const char* hex = ((*format) == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
               for (uint64_t v = value; v; v /= 16)
                  *(writer--) = hex[v & 15];
               while (writer >= writerLimit)
                  *(writer--) = '0';
               return;
            }
            default: break;
         }
   }

   // Compute the required length
   unsigned length = 0;
   for (uint64_t v = value; v; v /= 10)
      ++length;
   if (!length)
      length = 1;
   if (length < precision)
      length = precision;

   // Grow the target string
   unsigned outLen = out.length();
   out.resize(outLen + length);
   char *writerLimit = &out[outLen], *writer = writerLimit + length - 1;

   // And write it
   for (uint64_t v = value; v; v /= 10)
      *(writer--) = (v % 10) + '0';
   while (writer >= writerLimit)
      *(writer--) = '0';
}
//---------------------------------------------------------------------------
static void formatInt(string& out, const char* format, unsigned formatLen, int64_t value)
// Format a signed value
{
   // A positive number?
   if (value >= 0)
      return formatUInt(out, format, formatLen, value);

   // No, can we simple add a sign
   out += '-';
   return formatUInt(out, format, formatLen, -static_cast<uint64_t>(value));
}
//---------------------------------------------------------------------------
void Format<int8_t>::format(string& out, const char* format, unsigned formatLen, const void* value) { formatInt(out, format, formatLen, *static_cast<const int8_t*>(value)); }
void Format<uint8_t>::format(string& out, const char* format, unsigned formatLen, const void* value) { formatUInt(out, format, formatLen, *static_cast<const int8_t*>(value)); }
void Format<int16_t>::format(string& out, const char* format, unsigned formatLen, const void* value) { formatInt(out, format, formatLen, *static_cast<const int16_t*>(value)); }
void Format<uint16_t>::format(string& out, const char* format, unsigned formatLen, const void* value) { formatUInt(out, format, formatLen, *static_cast<const int16_t*>(value)); }
void Format<int32_t>::format(string& out, const char* format, unsigned formatLen, const void* value) { formatInt(out, format, formatLen, *static_cast<const int32_t*>(value)); }
void Format<uint32_t>::format(string& out, const char* format, unsigned formatLen, const void* value) { formatUInt(out, format, formatLen, *static_cast<const int32_t*>(value)); }
void Format<int64_t>::format(string& out, const char* format, unsigned formatLen, const void* value) { formatInt(out, format, formatLen, *static_cast<const int64_t*>(value)); }
void Format<uint64_t>::format(string& out, const char* format, unsigned formatLen, const void* value) { formatUInt(out, format, formatLen, *static_cast<const int64_t*>(value)); }
//---------------------------------------------------------------------------
void Format<double>::format(string& out, const char* format, unsigned formatLen, const void* value)
// Format a double
{
   double v = *static_cast<const double*>(value);

   unsigned precision = 0;
   bool explicitPrecision = false;
   constexpr unsigned margin = 30;
   if (formatLen) {
      bool ok = true;
      if (formatLen > 1) {
         ok = parseUnsigned(format + 1, format + formatLen, precision);
         if (precision > 1024)
            precision = 1024;
         explicitPrecision = true;
      }
      if (ok) switch (*format) {
            case 'E':
            case 'e': {
               if (!explicitPrecision)
                  precision = 6;
               char buffer[precision + margin];
               snprintf(buffer, precision + margin, "%.*e", precision, v);
               buffer[precision + margin - 1] = 0;
               out += buffer;
               return;
            }
            case 'F':
            case 'f': {
               if (!explicitPrecision)
                  precision = 6;
               char buffer[precision + margin];
               snprintf(buffer, precision + margin, "%.*f", precision, v);
               buffer[precision + margin - 1] = 0;
               out += buffer;
               return;
            }
            case 'N':
            case 'n': {
               if (!explicitPrecision)
                  precision = 6;
               char buffer[precision + margin];
               unsigned len = snprintf(buffer, precision + margin, "%.*f", precision, v);
               if (len >= precision + margin) len = precision + margin - 1;
               buffer[len] = 0;
               const char *firstDigit = nullptr, *dot = nullptr;
               for (unsigned index = 0; index != len; ++index) {
                  char c = buffer[index];
                  if (c == '.') {
                     dot = buffer + index;
                     break;
                  }
                  if ((!firstDigit) && ((c >= '0') && (c <= '9'))) firstDigit = buffer + index;
               }
               if (firstDigit && (!dot))
                  dot = buffer + len;
               if (firstDigit && ((dot - firstDigit) > 3)) {
                  for (const char* iter = buffer; iter != firstDigit; ++iter)
                     out += *iter;
                  unsigned step = (((dot - firstDigit) - 1) % 3);
                  for (const char* iter = firstDigit; iter != dot;) {
                     out += *(iter++);
                     if ((!step) && (iter != dot)) {
                        out += ',';
                        step = 2;
                     } else
                        --step;
                  }
                  out += dot;
               } else {
                  out += buffer;
               }
               return;
            }
            case 'P':
            case 'p': {
               if (!explicitPrecision)
                  precision = 6;
               char buffer[precision + margin];
               snprintf(buffer, precision + margin, "%.*f%%", precision, v * 100.0);
               buffer[precision + margin - 1] = 0;
               out += buffer;
               return;
            }
            case 'R':
            case 'r': {
               char buffer[2 * margin];
               for (precision = 6; precision <= 18; precision += 3) {
                  snprintf(buffer, 2 * margin, "%.*g", precision, v);
                  buffer[2 * margin - 1] = 0;
                  if (atof(buffer) == v) break;
               }
               out += buffer;
               return;
            }
            default: break;
         }
   }

   if (!explicitPrecision)
      precision = 15;
   char buffer[precision + margin];
   snprintf(buffer, precision + margin, "%.*g", precision, v);
   buffer[precision + margin - 1] = 0;
   out += buffer;
}
//---------------------------------------------------------------------------
string format(const char* text, const Arg* args, unsigned argCount)
// Format a string
{
   string result, part;
   for (const char* iter = text;;) {
      char c = *iter;

      // End of string?
      if (!c) break;

      // Escaped opening bracket?
      if ((c == '{') && (iter[1] == '{')) {
         result += c;
         iter += 2;
         continue;
      }

      // Escaped closing bracket?
      if ((c == '}') && (iter[1] == '}')) {
         result += c;
         iter += 2;
         continue;
      }

      // Non-bracket?
      if (c != '{') {
         const char* start = iter;
         for (++iter;; ++iter) {
            char c = *iter;
            if ((!c) || (c == '{') || (c == '}'))
               break;
         }
         result.append(start, iter - start);
         continue;
      }

      // Parse {id... (terminated by , : or }
      const char* start = ++iter;
      for (; true; ++iter) {
         c = *iter;
         if ((!c) || (c == ',') || (c == ':') || (c == '}'))
            break;
      }
      unsigned id = 0;
      bool idOk = parseUnsigned(start, iter, id);

      // Parse the width
      int width = 0;
      char padding = ' ';
      if (c == ',') {
         ++iter;
         bool neg = false;
         if (*iter == '0') {
            padding = '0';
            ++iter;
         }
         if (*iter == '-') {
            neg = true;
            ++iter;
         }
         const char* start = iter;
         for (; true; ++iter) {
            c = *iter;
            if ((!c) || (c == ':') || (c == '}'))
               break;
         }
         unsigned w = 0;
         if (parseUnsigned(start, iter, w))
            width = w;
         if (neg)
            width = -width;
      }

      // Parse format
      const char* format = nullptr;
      unsigned formatLen = 0;
      if (c == ':') {
         format = ++iter;
         for (; true; ++iter) {
            c = *iter;
            if ((!c) || (c == '}'))
               break;
         }
         formatLen = iter - format;
      }

      // Could we parse the string successfully?
      if (c != '}')
         break;
      ++iter;
      if ((!idOk) || (id >= argCount))
         continue;

      // Format, adding padding as needed
      if (width > 0) {
         args[id].formater(part, format, formatLen, args[id].value);
         unsigned realLen = part.length(), expectedLen = width;
         if (realLen < expectedLen)
            result.append(expectedLen - realLen, padding);
         result += part;
         part.clear();
      } else {
         unsigned old = result.length();
         args[id].formater(result, format, formatLen, args[id].value);
         if (width < 0) {
            unsigned realLen = result.length() - old, expectedLen = -width;
            if (realLen < expectedLen)
               result.append(expectedLen - realLen, padding);
         }
      }
   }
   return result;
}
//---------------------------------------------------------------------------
}}
//---------------------------------------------------------------------------
