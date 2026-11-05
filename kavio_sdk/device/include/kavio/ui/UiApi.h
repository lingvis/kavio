#pragma once

/** Public export declarations for the C++ UI API. */

#if defined(_WIN32)
#  if defined(KAVIO_UI_CORE_BUILD)
#    define KAVIO_UI_API __declspec(dllexport)
#  else
#    define KAVIO_UI_API __declspec(dllimport)
#  endif
#else
#  define KAVIO_UI_API __attribute__((visibility("default")))
#endif
