
#ifndef _ACFG_H
#define _ACFG_H

#include "config.h"
#include "actypes.h"
#include <map>
#include <bitset>

#define NUM_PBKDF2_ITERATIONS 1
// 1757961
#define ACNG_DEF_PORT 3142

namespace acng
{

class tHttpUrl;
class NoCaseStringMap;

namespace cfg
{
static const int RESERVED_DEFVAL = -4223;

static const int REDIRMAX_DEFAULT = 5;

extern mstring cachedir, logdir, confdir, udspath, user, group, pidfile, suppdir,
reportpage, vfilepat, pfilepat, wfilepat, agentname, adminauth, adminauthB64,
bindaddr, sUmask,
tmpDontcacheReq, tmpDontcachetgt, tmpDontcache, mirrorsrcs, requestapx,
cafile, capath, spfilepat, svfilepat, badredmime, sigbuscmd, connectPermPattern;

extern mstring pfilepatEx, vfilepatEx, wfilepatEx, spfilepatEx, svfilepatEx; // for customization by user

extern ACNG_API mstring dnsresconf;

extern ACNG_API int port, debug, numcores, offlinemode, foreground, verbose, stupidfs, forcemanaged, keepnver,
verboselog, extreshhold, exfailabort, tpstandbymax, tpthreadmax, dnscachetime, dlbufsize, usewrap,
exporigin, logxff, oldupdate, recompbz2, nettimeout, updinterval, forwardsoap, dirperms, fileperms,
maxtempdelay, redirmax, vrangeops, stucksecs, persistoutgoing, pipelinelen, exsupcount,
optproxytimeout, patrace, maxdlspeed, maxredlsize, dlretriesmax, nsafriendly, trackfileuse, exstarttradeoff,
fasttimeout, discotimeout, allocspace, dnsopts, minilog, follow404;

// processed config settings
extern const tHttpUrl* GetProxyInfo();
extern void MarkProxyFailure();
extern mstring agentheader;

extern int conprotos[2];

extern bool ccNoStore, ccNoCache;

bool ACNG_API SetOption(const mstring &line, NoCaseStringMap *pDupeChecker);
void ACNG_API dump_config(bool includingDelicateValues=false);
void ACNG_API ReadConfigDirectory(const char*, bool bReadErrorIsFatal=true);

//! Prepare various things resulting from variable combinations, etc.
void ACNG_API PostProcConfig();

bool DegradedMode();
void DegradedMode(bool newValue);

ACNG_API const struct timeval * GetNetworkTimeout();

/**
 * @brief GetFirstConTimeout
 * @return Deliver precalculated timeout structure with initial fast timeout AND a fraction of second on top
 */
ACNG_API const struct timeval * GetFirstConTimeout();
/**
 * @brief GetFurtherConTimeout
 * Like GetFirstConTimeout but deliver a probe interval for creation of further connections
 * @return
 */
ACNG_API const struct timeval * GetFurtherConTimeout();

extern std::map<mstring,mstring> localdirs;
cmstring & GetMimeType(cmstring &path);
#define TCP_PORT_MAX 65536
extern std::bitset<TCP_PORT_MAX> *pUserPorts;

extern mstring cacheDirSlash; // guaranteed to have a trailing path separator

void dump_trace();
ACNG_API int * GetIntPtr(LPCSTR key);
ACNG_API mstring * GetStringPtr(LPCSTR key);

int CheckAdminAuth(LPCSTR auth);

extern bool g_bQuiet, g_bNoComplex;

static const cmstring privStoreRelSnapSufix("_xstore/rsnap");
static const cmstring privStoreRelQstatsSfx("_xstore/qstats");

} // namespace cfg

namespace rex
{

enum NOCACHE_PATTYPE : bool
{
	NOCACHE_REQ,
	NOCACHE_TGT
};

enum eMatchType : int8_t
{
	FILE_INVALID = -1, // WARNING: this is forward-declared elsewhere!
	FILE_SOLID = 0, FILE_VOLATILE, FILE_WHITELIST,
	NASTY_PATH, PASSTHROUGH,
	FILE_SPECIAL_SOLID,
	FILE_SPECIAL_VOLATILE,
	ematchtype_max
};
bool Match(cmstring &in, eMatchType type);

eMatchType GetFiletype(const mstring &);
bool MatchUncacheable(const mstring &, NOCACHE_PATTYPE);
bool CompileUncExpressions(NOCACHE_PATTYPE type, cmstring& pat);
bool CompileExpressions();
}
LPCSTR ACNG_API ReTest(LPCSTR s);

#define CACHE_BASE (acng::cfg::cacheDirSlash)
#define CACHE_BASE_LEN (CACHE_BASE.length()) // where the relative paths begin
#define SZABSPATH(x) (CACHE_BASE+(x)).c_str()
#define SABSPATH(x) (CACHE_BASE+(x))

#define SABSPATHEX(x, y) (CACHE_BASE+(x) + (y))
#define SZABSPATHEX(x, y) (CACHE_BASE+(x) + (y)).c_str()

bool AppendPasswordHash(mstring &stringWithSalt, LPCSTR plainPass, size_t passLen);

}

#endif
