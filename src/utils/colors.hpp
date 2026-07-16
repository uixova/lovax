#ifndef COLORS_HPP
#define COLORS_HPP

#include <string>
#include <cstdio>

#if defined(_WIN32)
  #include <io.h>
  #define LOVAX_ISATTY _isatty
  #define LOVAX_FILENO _fileno
#else
  #include <unistd.h>
  #define LOVAX_ISATTY isatty
  #define LOVAX_FILENO fileno
#endif

namespace Lovax {
namespace Color {

// No color codes when output is not a terminal (file, pipe, golden tests).
// Keeps test output clean while staying colorful in a terminal.
inline bool stdoutIsTTY() {
    static bool tty = LOVAX_ISATTY(LOVAX_FILENO(stdout)) != 0;
    return tty;
}

inline bool stderrIsTTY() {
    static bool tty = LOVAX_ISATTY(LOVAX_FILENO(stderr)) != 0;
    return tty;
}

inline std::string reset()  { return stdoutIsTTY() ? "\033[0m"    : ""; }
inline std::string red()    { return stdoutIsTTY() ? "\033[1;31m" : ""; } // Errors
inline std::string green()  { return stdoutIsTTY() ? "\033[1;32m" : ""; } // Strings
inline std::string yellow() { return stdoutIsTTY() ? "\033[1;33m" : ""; } // Numbers
inline std::string blue()   { return stdoutIsTTY() ? "\033[1;34m" : ""; } // Booleans
inline std::string cyan()   { return stdoutIsTTY() ? "\033[1;36m" : ""; } // Info

// Separate check for the error stream (stderr)
inline std::string errRed()   { return stderrIsTTY() ? "\033[1;31m" : ""; }
inline std::string errReset() { return stderrIsTTY() ? "\033[0m"    : ""; }

} // namespace Color
} // namespace Lovax

#endif // COLORS_HPP
