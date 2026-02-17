/**
 * Most basic configuration preparation for the project build, after reading auto-config variables
 * and preparing internal configuration bits from that.
 */
#ifndef __CONFIG_H_
#define __CONFIG_H_

#include "acsyscap.h"

// safe fallbacks, should be defined by build system
#ifndef ACVERSION
#define ACVERSION "0.custom"
#endif
#ifndef CFGDIR
#define CFGDIR "/usr/local/etc/apt-cacher-ng"
#endif
#ifndef LIBDIR
#define LIBDIR "/usr/local/lib/apt-cacher-ng"
#endif

#define __STDC_FORMAT_MACROS

// added in Makefile... #define _FILE_OFFSET_BITS 64

namespace acng
{

#define SHARED_PTR std::shared_ptr
#define WEAK_PTR std::weak_ptr

#if defined _WIN32 || defined __CYGWIN__
  #define ACNG_SO_IMPORT __declspec(dllimport)
  #define ACNG_SO_EXPORT __declspec(dllexport)
  #define ACNG_SO_LOCAL
#else
  #if __GNUC__ >= 4
    #define ACNG_SO_IMPORT __attribute__ ((visibility ("default")))
    #define ACNG_SO_EXPORT __attribute__ ((visibility ("default")))
    #define ACNG_SO_LOCAL  __attribute__ ((visibility ("hidden")))
  #else
    #define ACNG_SO_IMPORT
    #define ACNG_SO_EXPORT
    #define ACNG_SO_LOCAL
  #endif
#endif

#ifdef ACNG_CORE_IN_SO
  #ifdef supacng_EXPORTS // defined by cmake for shared lib project
    #define ACNG_API ACNG_SO_EXPORT
  #else
    #define ACNG_API ACNG_SO_IMPORT
  #endif // ACNG_DLL_EXPORTS
  #define ACNG_LOCAL ACNG_SO_LOCAL
#else // ACNG_DLL is not defined, code is built in as usual
  #define ACNG_API
  #define ACNG_LOCAL
#endif // ACNG_DLL

}
#endif // __CONFIG_H
