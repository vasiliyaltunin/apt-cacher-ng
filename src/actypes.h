#ifndef ACTYPES_H
#define ACTYPES_H

#include "config.h"
#include "sut.h"

#if defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE < 200112L)
#undef _POSIX_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#ifdef _MSC_VER
#define __func__ __FUNCTION__
#endif


#ifdef __GNUC__
#define WARN_UNUSED  __attribute__ ((warn_unused_result))
#else
#define WARN_UNUSED
#endif

// little STFU helper
#if __GNUC__ >= 7
#define __just_fall_through [[fallthrough]]
#else
#define __just_fall_through
#endif

#if __cplusplus >= 201703L
#include <string_view>
#else
#include <experimental/string_view>
#endif

#include <string>
#include <limits>
#include <memory>

namespace acng {

#if __cplusplus >= 201703L
using string_view = std::string_view;
#else
using string_view = std::experimental::basic_string_view;
#endif
#define citer const_iterator

typedef const char * LPCSTR;

#define MIN_VAL(x) (std::numeric_limits< x >::min())
#define MAX_VAL(x) (std::numeric_limits< x >::max())

#define STRINGIFY(a) STR(a)
#define STR(a) #a

#ifndef _countof
#define _countof(x) sizeof(x)/sizeof(x[0])
#endif

using mstring = std::string;
using cmstring = const std::string;
typedef mstring::size_type tStrPos;
constexpr static tStrPos stmiss(cmstring::npos);

}
#endif // ACTYPES_H
