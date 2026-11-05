#pragma once

/** Export macros used by the public C++ application API. */

#if defined(_WIN32)
#  if defined(KAVIO_APP_RUNTIME_BUILD)
#    define KAVIO_APP_API __declspec(dllexport)
#  else
#    define KAVIO_APP_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define KAVIO_APP_API __attribute__((visibility("default")))
#else
#  define KAVIO_APP_API
#endif
