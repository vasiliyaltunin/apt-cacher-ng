#ifndef _META_H
#define _META_H

#include "actypes.h"
#include "actemplates.h"

#include <string>
#include <map>
#include <set>
#include <vector>
#include <deque>
#include <limits>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <atomic>

#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <cstdlib>
#include <errno.h>

#include "astrop.h"

#define EXTREME_MEMORY_SAVING false

namespace acng
{

class acbuf;

typedef std::pair<mstring, mstring> tStrPair;
typedef std::vector<mstring> tStrVec;
typedef std::set<mstring> tStrSet;
typedef std::deque<mstring> tStrDeq;
typedef unsigned short USHORT;
typedef unsigned char UCHAR;

#define CPATHSEPUNX '/'
#define SZPATHSEPUNIX "/"
#define CPATHSEPWIN '\\'
#define SZPATHSEPWIN "\\"

extern cmstring sPathSep, sPathSepUnix, hendl;

extern cmstring FAKEDATEMARK;

#define szRN "\r\n"

#ifdef WINDOWS
#define WIN32
#define SZPATHSEP SZPATHSEPWIN
#define CPATHSEP CPATHSEPWIN
#define szNEWLINE szRN
#else
#define SZPATHSEP SZPATHSEPUNIX
#define CPATHSEP CPATHSEPUNX
#define szNEWLINE "\n"
#endif

// some alternative versions of these flags

#ifndef O_NONBLOCK
#ifdef NOBLOCK
#define O_NONBLOCK NOBLOCK
#else
#ifdef O_NDELAY
#define O_NONBLOCK O_NDELAY
#endif
#endif
#endif

#ifndef O_NONBLOCK
#error "Unknown how to configure non-blocking mode (O_NONBLOCK) on this system"
#endif

//#define PATHSEP "/"
int getUUID();

#define SPACECHARS " \f\n\r\t\v"

typedef std::map<mstring, mstring> tStrMap;

LPCSTR GetTypeSuffix(cmstring& s);

void trimProto(mstring & sUri);
tStrPos findHostStart(const mstring & sUri);

#define ELVIS(x, y) (x ? x : y)
#define OPTSET(x, y) if(!x) x = y

// Sometimes I miss Perl...
tStrVec::size_type Tokenize(cmstring &in, const char* sep, tStrVec & out, bool bAppend=false, mstring::size_type nStartOffset=0);
/*inline void Join(mstring &out, const mstring & sep, const tStrVec & tokens)
{out.clear(); if(tokens.empty()) return; for(const auto& tok: tokens)out+=(sep + tok);}
*/
void StrSubst(mstring &contents, const mstring &from, const mstring &to, tStrPos start=0);

// TODO: __attribute__((externally_visible))
bool ParseKeyValLine(const mstring & sIn, mstring & sOutKey, mstring & sOutVal);
#define keyEq(a, b) (0 == strcasecmp((a), (b).c_str()))

#define POKE(x) for(;;) { ssize_t n=write(x, "", 1); if(n>0 || (EAGAIN!=errno && EINTR!=errno)) break;  }


void appendLong(mstring &s, long val);

mstring BytesToHexString(const uint8_t b[], unsigned short binLength);
bool CsAsciiToBin(LPCSTR a, uint8_t b[], unsigned short binLength);

typedef const unsigned char CUCHAR;
bool CsEqual(LPCSTR a, uint8_t b[], unsigned short binLength);

#if SIZEOF_LONG == 8
// _FILE_OFFSET_BITS mostly irrelevant. But if it's set, watch out for user's "experiments".
#if _FILE_OFFSET_BITS == 32
#error Unsupported: _FILE_OFFSET_BITS == 32 with large long size
#else
#define OFF_T_FMT "%" PRId64
#endif

#else // not a 64bit arch?

#if 64 == _FILE_OFFSET_BITS
#define OFF_T_FMT "%" PRId64
#endif

#if 32 == _FILE_OFFSET_BITS
#define OFF_T_FMT "%" PRId32
#endif

#endif // !64bit arch

#ifndef OFF_T_FMT // either set above or let the os/compiler deal with the mess
#define OFF_T_FMT "%ld"
#endif

// let the compiler optimize and keep best variant
off_t atoofft(LPCSTR p);

inline off_t atoofft(LPCSTR p, off_t nDefVal)
{
	return p ? atoofft(p) : nDefVal;
}

ACNG_API mstring offttosH(off_t n);
ACNG_API mstring offttosHdotted(off_t n);
tStrDeq ExpandFilePattern(cmstring& pattern, bool bSorted=false, bool bQuiet=false);

//void MakeAbsolutePath(mstring &dirToFix, const mstring &reldir);

mstring UrlEscape(cmstring &s);
void UrlEscapeAppend(cmstring &s, mstring &sTarget);
bool UrlUnescapeAppend(cmstring &from, mstring & to);
// Decode with result as return value, no error reporting
mstring UrlUnescape(cmstring &from);
mstring DosEscape(cmstring &s);
// just the bare minimum to make sure the string does not break HTML formating
mstring html_sanitize(cmstring& in);
mstring message_detox(string_view in, int pfx = -1);

ACNG_API mstring UserinfoEscape(cmstring &s);

#define pathTidy(s) { if(startsWithSz(s, "." SZPATHSEP)) s.erase(0, 2); tStrPos n(0); \
	for(n=0;stmiss!=n;) { n=s.find(SZPATHSEP SZPATHSEP, n); if(stmiss!=n) s.erase(n, 1);}; \
	for(n=0;stmiss!=n;) { n=s.find(SZPATHSEP "." SZPATHSEP, n); if(stmiss!=n) s.erase(n, 2);}; }

// appears in the STL container?
#define ContHas(stlcont, needle) ((stlcont).find(needle) != (stlcont).end())
#define ContHasLinear(stlcont, needle) ((stlcont).end() != (std::find((stlcont).begin(), (stlcont).end(), needle)))

#define StrHas(haystack, needle) (haystack.find(needle) != stmiss)
#define StrHasFrom(haystack, needle, startpos) (haystack.find(needle, startpos) != stmiss)
#define StrEraseEnd(s,len) (s).erase((s).size() - len)

off_t GetFileSize(cmstring & path, off_t defret);

ACNG_API mstring offttos(off_t n);
ACNG_API mstring ltos(long n);
ACNG_API mstring offttosH(off_t n);

//template<typename charp>
ACNG_API off_t strsizeToOfft(const char *sizeString); // XXX: if needed... charp sizeString, charp *next)

bool ParseHeadFromFile(cmstring& path, off_t* contLen, time_t* lastModified, mstring* origSrc);

void replaceChars(mstring &s, LPCSTR szBadChars, char goodChar);

extern ACNG_API cmstring sEmptyString;

void DelTree(cmstring &what);

bool IsAbsolute(cmstring &dirToFix);

mstring unEscape(cmstring &s);

std::string BytesToHexString(const uint8_t sum[], unsigned short lengthBin);
//bool HexToString(const char *a, mstring& ret);
bool Hex2buf(const char *a, size_t len, acbuf& ret);

// STFU helpers, (void) casts are not effective for certain functions
static inline void ignore_value (int i) { (void) i; }
static inline void ignore_ptr (void* p) { (void) p; }

static inline time_t GetTime()
{
	return ::time(0);
}

static const time_t END_OF_TIME(MAX_VAL(time_t)-2);

// represents a boolean value like a normal bool but also carries additional data
template <typename Textra, Textra defval>
struct extended_bool
{
	bool value;
	Textra xdata;
	inline operator bool() { return value; }
	inline extended_bool(bool val, Textra xtra = defval) : value(val), xdata(xtra) {};
};

void ACNG_API DelTree(cmstring &what);

struct ACNG_API tErrnoFmter: public mstring
{
	tErrnoFmter(LPCSTR prefix = nullptr) { fmt(errno, prefix);}
	tErrnoFmter(int errnoCode, LPCSTR prefix = nullptr) { fmt(errnoCode, prefix); }
private:
	void fmt(int errnoCode, LPCSTR prefix);
};

ACNG_API mstring EncodeBase64Auth(cmstring &sPwdString);
mstring EncodeBase64(LPCSTR data, unsigned len);

#if defined(HAVE_SSL) || defined(HAVE_TOMCRYPT)
#define HAVE_DECB64
bool DecodeBase64(LPCSTR pAscii, size_t len, acbuf& binData);
#endif

typedef std::deque<std::pair<std::string, std::string>> tLPS;

#ifdef __GNUC__
#define AC_LIKELY(x)   __builtin_expect(!!(x), true)
#define AC_UNLIKELY(x) __builtin_expect(!!(x), false)
#else
#define AC_LIKELY(x)   x
#define AC_UNLIKELY(x) x
#endif

// shortcut for the non-invasive lookup and copy of stuff from maps
#define ifThereStoreThere(x,y,z) { auto itFind = (x).find(y); if(itFind != (x).end()) z = itFind->second; }
#define ifThereStoreThereAndBreak(x,y,z) { auto itFind = (x).find(y); if(itFind != (x).end()) { z = itFind->second; break; } }

// from bgtask.cc
cmstring GetFooter();

template<typename T>
std::pair<T,T> pairSum(const std::pair<T,T>& a, const std::pair<T,T>& b)
{
	return std::pair<T,T>(a.first+b.first, a.second + b.second);
}

namespace cfg
{
extern int nettimeout;
}
struct CTimeVal
{
	struct timeval tv = {0,23};
public:
	// calculates for relative time (span)
	struct timeval* For(time_t tExpSec, suseconds_t tExpUsec = 23)
	{
		tv.tv_sec = tExpSec;
		tv.tv_usec = tExpUsec;
		return &tv;
	}
	struct timeval* ForNetTimeout()
	{
		tv.tv_sec = cfg::nettimeout;
		tv.tv_usec = 23;
		return &tv;
	}
	// calculates for absolute time
	struct timeval* Until(time_t tExpWhen, suseconds_t tExpUsec = 23)
	{
		tv.tv_sec = GetTime() + tExpWhen;
		tv.tv_usec = tExpUsec;
		return &tv;
	}
	// like above but with error checking
	struct timeval* SetUntil(time_t tExpWhen, suseconds_t tExpUsec = 23)
	{
		auto now(GetTime());
		if(now >= tExpWhen)
			return nullptr;
		tv.tv_sec = now + tExpWhen;
		tv.tv_usec = tExpUsec;
		return &tv;
	}
	// calculates for a timespan with max. length until tExpSec
	struct timeval* Remaining(time_t tExpSec, suseconds_t tExpUsec = 23)
	{
		auto exp = tExpSec - GetTime();
		tv.tv_sec = exp < 0 ? 0 : exp;
		tv.tv_usec = tExpUsec;
		return &tv;
	}
};


struct ltstring {
    bool operator()(const mstring &s1, const mstring &s2) const {
        return strcasecmp(s1.c_str(), s2.c_str()) < 0;
    }
};

class NoCaseStringMap : public std::map<mstring, mstring, ltstring>
{
};

static constexpr string_view svRN = szRN;
static constexpr string_view svLF = "\n";

#if !defined(HAVE_STRLCPY)
size_t strlcpy(char *tgt, const char *src, size_t tgtSize);
#endif
}

#endif // _META_H

