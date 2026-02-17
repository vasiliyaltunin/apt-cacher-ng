//============================================================================
// Name        : acngfs.cpp
// Author      : Eduard Bloch
// Description : Simple FUSE-based filesystem for HTTP access (apt-cacher NG)
//============================================================================

#define FUSE_USE_VERSION 26

#include "debug.h"

#include "meta.h"
#include "header.h"
#include "caddrinfo.h"
#include "sockio.h"
#include "acbuf.h"
#include "acfg.h"
#include "lockable.h"
#include "cleaner.h"
#include "tcpconnect.h"
#include "ebrunner.h"
#include "fileitem.h"
#include "fileio.h"
#include "dlcon.h"
#include "acregistry.h"
#include "ac3rdparty.h"

#ifdef HAVE_SYS_MOUNT_H
#include <sys/param.h>
#include <sys/mount.h>
#endif
#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#include <fuse.h>

#ifdef HAVE_DLOPEN
#include <dlfcn.h>
#endif

#include <list>
#include <unordered_map>
#include <algorithm>

#include <event2/buffer.h>

#define HEADSZ 5000
#ifndef MIN
#define MIN(a,b) ( (a<=b)?a:b)
#endif

#include <thread>

#define MAX_DL_STREAMS 8
//#define MAX_DL_BUF_SIZE 1024*1024

// local chunk size
const size_t CBLOCK_SIZE = 1 << 20;

//#warning set 2m
#define META_CACHE_EXP_TIME 120

using namespace acng;
using namespace std;


namespace acng
{
namespace log
{
	extern mstring g_szLogPrefix;
}
}

//#define SPAM

#ifdef SPAM
#define _cerr(x) cerr << x
#warning printing spam all around
#else
#define _cerr(x)
#endif

#if !defined(BUFSIZ) || BUFSIZ > 8192
#define BUFSIZ 8192
#endif

#define HEAD_TAIL_LEN BUFSIZ

// some globals, set only once
static struct stat statTempl;
static struct statvfs stvfsTemp;
tHttpUrl baseUrl;
cmstring& altPath = cfg::cachedir;

bool g_bGoodServer=true;

struct tDlAgent
{
	thread runner;
	SHARED_PTR<dlcon> dler;
	time_t createdAt = GetTime();
	enum eKind
	{
		HEADER, TRAILER, MID
	} kind;

	tDlAgent()
	{
		dler = dlcon::CreateRegular(g_tcp_con_factory);
		runner = std::thread([&]() { dler->WorkLoop();} );
	}
	~tDlAgent()
	{
		if(dler)
			dler->SignalStop();

		if(runner.joinable())
			runner.join();
	}

};

//std::unordered_multimap<string, SHARED_PTR<tDlStream>> active_agents;
deque<pair<time_t, SHARED_PTR<tDlAgent>>> spare_agents;
mutex mx_streams;
using mg = lock_guard<mutex>;

SHARED_PTR<tDlAgent> getAgent()
{
	mg g(mx_streams);
#if 0 // nay, will be reconnected, cheaper than restarting the thread
	// dump those who were proably disconnected anyway
	auto exTime = GetTime() - 15;
	spare_agents.remove_if([exTime](decltype (spare_agents)::iterator it)
	{
		return (**it).createdAt < exTime;
	});
#endif
	if (!spare_agents.empty())
	{
		auto ret = move(spare_agents.back());
		spare_agents.pop_back();
		return ret.second;
	}
	return make_shared<tDlAgent>();
}

void returnAgent(SHARED_PTR<tDlAgent> && agent)
{
	mg g(mx_streams);
	spare_agents.push_back(make_pair(GetTime(), move(agent)));
	agent.reset();
}

void expireAgents(int, short, void*)
{
	mg g(mx_streams);
	if (spare_agents.empty())
		return;
	auto ex = GetTime() - 30;
	auto firstGood = std::find_if(spare_agents.begin(), spare_agents.end(),
						  [ex](const auto& el){ return el.first > ex; });
	spare_agents.erase(spare_agents.begin(), firstGood);
}

class tConsumingItem : public fileitem
{
public:
	evbuffer* buf;
	off_t pos;

	tConsumingItem(string_view path, tHttpDate knownDate,
				   off_t rangeStart, off_t rangeLen)
		: fileitem(path),  pos(rangeStart)
	{
		m_status = FIST_INITED;
		m_nSizeCachedInitial = rangeStart;
		m_responseModDate = knownDate;
		if (rangeLen < 0)
			m_spattr.bHeadOnly = true;
		else
			m_spattr.nRangeLimit = rangeStart + rangeLen - 1;
		m_spattr.bVolatile = true; // our client must check timestamp
		buf = evbuffer_new();
		usercount = 1;
	}
	~tConsumingItem()
	{
		if (buf)
			evbuffer_free(buf);
	}
	void markUnused()
	{
		usercount = 0;
	}

protected:
	bool DlAddData(string_view data, lockuniq &) override
	{
		LOGSTARTFUNCx(data.size(), m_nSizeChecked);
		m_status = FIST_DLRECEIVING;
		notifyAll();
		if (m_nSizeChecked < 0)
			m_nSizeChecked = 0;
		if (evbuffer_get_length(buf) >= CBLOCK_SIZE)
			return true;
		if (m_nSizeChecked < pos)
		{
			if (m_nSizeChecked + off_t(data.size()) < off_t(pos))
			{
				// no overlap, not reachable
				m_nSizeChecked += data.size();
				return true;
			}
			// reached, partial or full overlap
			auto drop = pos - m_nSizeChecked;
			data.remove_prefix(drop);
			m_nSizeChecked += drop;
		}
		auto avail = CBLOCK_SIZE - evbuffer_get_length(buf);
		auto toTake = min(avail, data.length());
		if (0 != evbuffer_add(buf, data.data(), toTake))
			return false;
		data.remove_prefix(toTake);
		// false to abort if too much data already
		if (data.empty())
			return true;
		return false;
	}
};

struct tDlDesc
{
	cmstring m_path;
	uint m_ftype;

	virtual int Read(char *retbuf, const char *path, off_t pos, size_t len) =0;
	virtual int Stat(struct stat *stbuf) =0;
	tDlDesc(cmstring &p, uint ftype) : m_path(p), m_ftype(ftype) {};
	virtual ~tDlDesc() {};
};

struct tDlDescLocal : public tDlDesc
{
	FILE *pFile;
	tDlDescLocal(cmstring &path, uint ftype) : tDlDesc(path, ftype), pFile(nullptr)
	{
	};

	int Stat(struct stat *stbuf) override
	{
		if (!stbuf)
			return -EIO;

		if(altPath.empty()) // hm?
			return -ENOENT;

		auto absPath = altPath + m_path;
		if (::stat(absPath.c_str(), stbuf))
			return -errno;

		// verify the file state
		off_t cl = -2;
		if (!ParseHeadFromStorage(absPath, &cl, nullptr, nullptr) || cl != stbuf->st_size)
			return -EIO;

		return 0;
	}

	virtual ~tDlDescLocal()
	{
		if(pFile)
			fclose(pFile);
		pFile=nullptr;
	};

	int Read(char *retbuf, const char *, off_t pos, size_t len) override
	{
		if (!pFile)
		{
			struct stat stbuf;
			if(Stat(&stbuf))
				return -EIO; // file incomplete or missing

			FILE *pf = fopen((altPath + m_path).c_str(), "rb");
			if (!pf)
				return -EIO;
			pFile = pf;
		}

		int copied=0;
		if(pFile && 0==fseeko(pFile, pos, SEEK_SET))
		{
			while(!feof(pFile) && !ferror(pFile) && len>0)
			{
				size_t r = ::fread(retbuf+copied, 1, len, pFile);
				copied+=r;
				len-=r;
			}
		}
		return ferror(pFile) ? -EIO : copied;
	}
};

using unique_evbuf = auto_raii<evbuffer*,evbuffer_free,nullptr>;

struct tFileMeta
{
	off_t m_size = -1; tHttpDate m_ctime; time_t validAt = 0;
#ifdef USE_HT_CACHE
	SHARED_PTR<unique_evbuf> head, tail;
#endif
	tFileMeta() =default;
	tFileMeta(off_t a, mstring b) : m_size(a), m_ctime(b) {};
	bool operator!=(tFileMeta other) const { return m_size != other.m_size || m_ctime != other.m_ctime;}
};
static class tRemoteInfoCache : public base_with_mutex, public map<string, tFileMeta>
{} remote_info_cache;

// all members must be trivially-copyable
struct tDlDescRemote : public tDlDesc
{
protected:
	tFileMeta m_meta;
	SHARED_PTR<tDlAgent> m_agent;
	tHttpUrl m_uri;
	bool m_bItemWasRead = false, m_bDataChangeError = false;
	std::recursive_mutex m_mx;

public:
	tDlDescRemote(cmstring &p, uint n) : tDlDesc(p,n)
	{

		m_uri = baseUrl;
		m_uri.sPath += m_path;

		// good moment to expire the caches, every time, should not cost much anyway
		g_tcp_con_factory.BackgroundCleanup();
		lockguard g(remote_info_cache);
		auto it = remote_info_cache.find(m_path);
		if (it != remote_info_cache.end())
		{
			if (GetTime() > it->second.validAt + META_CACHE_EXP_TIME)
			{
				remote_info_cache.erase(it);
				return;
			}
			m_meta = it->second;
			return;
		}
	};
	int Stat(struct stat *stbuf)
	{
		std::unique_lock<std::recursive_mutex> lck(m_mx);

		if (stbuf)
			*stbuf = statTempl;
		auto now(GetTime());
		if (now >= m_meta.validAt + META_CACHE_EXP_TIME)
		{
			// string_view path, off_t knownLength, tHttpDate knownDate, off_t rangeStart, off_t rangeLen
			auto item = make_shared<tConsumingItem>(m_path, m_meta.m_ctime, 0,
										#ifdef USE_HT_CACHE
										#warning this needs a special handler for the condition "not satisfiable range" where it retries without limit
													HEAD_TAIL_LEN
										#else
													-1 // head-only
										#endif
													);
			pair<fileitem::FiStatus, tRemoteStatus> res;

			try
			{
				if (!m_agent)
					m_agent = getAgent();
				m_agent->dler->AddJob(item, m_uri);
				auto exTime = GetTime() + 7;
				res = item->WaitForFinish(3, [exTime](){ return (GetTime() < exTime); });
				item->markUnused();
				returnAgent(move(m_agent));
			}
			catch (...)
			{
				item->markUnused();
			}


			if (res.first != fileitem::FIST_COMPLETE)
			{
				USRDBG(res.second.msg);
				return -EIO;
			}
			auto na = res.second.code >= 400;
			if (na || (item->m_responseModDate != m_meta.m_ctime || item->m_nContentLength != m_meta.m_size))
			{
				// reap all (meta) data that we might need
#ifdef USE_HT_CACHE
				m_meta.tail.reset();
				auto stolenBuf = item->buf;
				item->buf = 0;
				m_meta.head = make_shared<unique_evbuf>(stolenBuf);
#endif
				if (na)
				{
					m_meta.m_size = -1;
				}
				else
				{
					m_meta.m_size = item->m_nContentLength;
					m_meta.m_ctime = item->m_responseModDate;
					m_meta.validAt = now;
				}
				{
					// donate back to cache
					lockguard g(remote_info_cache);
					remote_info_cache[m_path] = m_meta;
				}
				if (m_bItemWasRead)
				{
					m_bDataChangeError = true;
					return -EIO;
				}
			}
		}
		if(stbuf)
		{
			if (m_meta.m_size < 0)
			{
				memset(stbuf, 0, sizeof(struct stat));
				return -ENOENT;
			}
			stbuf->st_size = m_meta.m_size;
			stbuf->st_mode &= ~S_IFDIR;
			stbuf->st_mode |= S_IFREG;
			stbuf->st_ctime = m_meta.m_ctime.value(1);
		}
		return 0;
	}

	unique_evbuf m_data;
	ssize_t m_dataPos = -1;
	size_t m_dataLen = 0;

	int Read(char *retbuf, const char *path, off_t pos, size_t len)
	{
		std::unique_lock<std::recursive_mutex> lck(m_mx);

		(void) path;
		if (Stat(nullptr))
			return -EBADF;

#if USE_HT_CACHE
		if (pos + len <= HEAD_TAIL_LEN && m_meta.head && m_meta.head.get()->valid())
		{

		}
#endif
		auto blockStartPos = CBLOCK_SIZE * (pos / CBLOCK_SIZE);
		auto posInBlock = pos % CBLOCK_SIZE;
		pair<fileitem::FiStatus, tRemoteStatus> res;
		auto retError = [&res](int where)
		{
			DBGQLOG("E: at " << where << ", " << res.first << "/" << res.second.code << "/" << res.second.msg);
			return -EIO;
		};

		if (off_t(m_dataPos) != off_t(blockStartPos))
		{
			m_dataPos = -1;
			m_dataLen = 0;
			m_data.reset();

			// ok, need to fetch it
			// string_view path, off_t knownLength, tHttpDate knownDate, off_t rangeStart, off_t rangeLen
			size_t toGet = std::min(off_t(CBLOCK_SIZE), off_t(m_meta.m_size - blockStartPos));
			auto item = make_shared<tConsumingItem>(m_path, m_meta.m_ctime, blockStartPos, toGet);

			try
			{
				if (!m_agent)
					m_agent = getAgent();
				m_agent->dler->AddJob(item, m_uri);
				res = item->WaitForFinish(5, [](){ return false; });
				item->markUnused();
				returnAgent(move(m_agent));
			}
			catch (...)
			{
				item->markUnused();
			}
			if (res.first != fileitem::FIST_COMPLETE)
			{
				// maybe it has sent to much? If so, we can still use the response
				if (evbuffer_get_length(item->buf) < toGet)
					return retError(__LINE__);
			}
			std::swap(m_data.m_p, item->buf);
			if (!m_data.valid())
				return retError(__LINE__);
			auto fetched = evbuffer_get_length(m_data.m_p);
			if (fetched != toGet)
				return retError(__LINE__);
			m_dataLen = toGet;
			m_dataPos = blockStartPos;
		}
		auto retCount = std::min(off_t(len), off_t(m_dataLen - posInBlock));
		evbuffer_ptr ep;
		if (0 != evbuffer_ptr_set(m_data.m_p, &ep, posInBlock, EVBUFFER_PTR_SET))
			return retError(__LINE__);
		if(-1 == evbuffer_copyout_from(m_data.m_p, &ep, retbuf, retCount))
			return retError(__LINE__);
		return retCount;
	}
};



/// If found as downloadable, present as a file, or become a directory otherwise.
static int acngfs_getattr(const char *path, struct stat *stbuf)
{
	if(!path)
		return -1;

	rex::eMatchType type = rex::GetFiletype(path);
	_cerr( "type: " << type);
	if (type == rex::FILE_SOLID || type == rex::FILE_VOLATILE)
	{
		if(0 == tDlDescLocal(path, type).Stat(stbuf))
			return 0;
		return tDlDescRemote(path, type).Stat(stbuf);
	}
	else
	{
		// shall be a directory because FUSE probes the path all the way down
		memcpy(stbuf, &statTempl, sizeof(statTempl));
		stbuf->st_mode &= ~S_IFMT; // delete mode flags and set them as needed
		stbuf->st_mode |= S_IFDIR;
		stbuf->st_size = 4;
		return 0;
	}
}

static int acngfs_fgetattr(const char *path, struct stat *stbuf,
	  struct fuse_file_info *)
{
	// FIXME: reuse the con later? or better not, size might change during operation
	return acngfs_getattr(path, stbuf);
}

static int acngfs_access(const char *, int mask)
{
	// non-zero (failure) when trying to write
   return mask&W_OK;
}

static int acngfs_opendir(const char *, struct fuse_file_info *)
{
	// let FUSE manage directories
	return 0;
}

static int acngfs_readdir(const char *, void *, fuse_fill_dir_t,
	  off_t, struct fuse_file_info *)
{   
	return -EPERM;
}

static int acngfs_releasedir(const char *, struct fuse_file_info *)
{
   return 0;
}

static int acngfs_open(const char *path, struct fuse_file_info *fi)
{
	//lockguard g(&mxTest);
	
	if (fi->flags & (O_WRONLY|O_RDWR|O_TRUNC|O_CREAT))
			return -EROFS;

	tDlDesc *p(nullptr);
	struct stat stbuf;
	rex::eMatchType ftype = rex::GetFiletype(path);

	try
	{
		// ok... if that is a remote object, can we still use local access instead?
		if(!altPath.empty() && rex::FILE_SOLID == ftype)
		{
				p = new tDlDescLocal(path, ftype);
				if(p)
				{
					if(0 == p->Stat(&stbuf))
						goto desc_opened;
					delete p;
					p = nullptr;
				}
		}


		p = new tDlDescRemote(path, ftype);

		if (!p) // running exception-free?
			return -EIO;

		if (0 != p->Stat(&stbuf))
		{
			delete p;
			return -EIO;
		}
	}
	catch(std::bad_alloc&)
	{
		return -EIO;
	}
	
	desc_opened:

	fi->fh = (uintptr_t) p;
	return 0;
}


static int acngfs_read(const char *path, char *buf, size_t size, off_t offset,
					   struct fuse_file_info *fi)
{
	auto p=(tDlDesc*) fi->fh;
	return p->Read(buf, path, offset, size);
}

static int acngfs_statfs(const char *, struct statvfs *stbuf)
{
	memcpy(stbuf, &stvfsTemp, sizeof(*stbuf));
	return 0;
}

static int acngfs_release(const char *, struct fuse_file_info *fi)
{
	if(fi->fh)
		delete (tDlDesc*)fi->fh;
	return 0;
}

struct fuse_operations acngfs_oper;

int my_fuse_main(int argc, char ** argv)
{
   return fuse_main(argc, argv, &acngfs_oper, nullptr);
}

void _ExitUsage() {
   cerr << "USAGE: acngfs BaseURL MountPoint [ACNG Configuration Assignments] [FUSE Mount Options]\n"
   << "Examples:\n\t  acngfs http://ftp.uni-kl.de/debian /var/local/aptfs proxy=cacheServer:3142"
   << "\n\t  acngfs http://ftp.uni-kl.de/debian /var/local/aptfs proxy=cacheServer:3142 cachedir=/var/cache/apt-cacher-ng/debrep\n\n"
        << "FUSE mount options summary:\n\n";
    char *argv[] = {strdup("..."), strdup("-h")};
    my_fuse_main( _countof(argv), argv);
    exit(EXIT_FAILURE);
}

#define barf(x) { cerr << endl << "ERROR: " << x <<endl; exit(1); }

int main(int argc, char *argv[])
{
	using namespace acng;
	ac3rdparty libInit;

	memset(&acngfs_oper, 0, sizeof(acngfs_oper));
	acngfs_oper.getattr	= acngfs_getattr;
	acngfs_oper.fgetattr	= acngfs_fgetattr;
	acngfs_oper.access	= acngfs_access;
	acngfs_oper.opendir	= acngfs_opendir;
	acngfs_oper.readdir	= acngfs_readdir;
	acngfs_oper.releasedir	= acngfs_releasedir;
	acngfs_oper.open	= acngfs_open;
	acngfs_oper.read	= acngfs_read;
	acngfs_oper.statfs	= acngfs_statfs;
	acngfs_oper.release	= acngfs_release;
	umask(0);

	cfg::agentname = "ACNGFS/" ACVERSION;
	cfg::requestapx = "X-Original-Source: 42\r\n";
	log::g_szLogPrefix = "acngfs";

	cfg::cachedir.clear();
	cfg::cacheDirSlash.clear();

	for(int i = 0; i < argc; ++i)
	{
		if (0==strcmp(argv[i], "--help") || 0==strcmp(argv[i], "-h") )
			_ExitUsage();
	}

	if(argc<3)
		barf("Not enough arguments, try --help.\n");

	vector<char*> fuseArgs(argv, argv + 1);
	fuseArgs.push_back(argv[2]);

	cfg::g_bQuiet = true; // STFU, we need to separate our options silently

	// check help request and weedout our arguments
	for(auto p=argv + 3; p < argv + argc; ++p)
	{
		if (0==strcmp(*p, "--help"))
			_ExitUsage();
		else if (!cfg::SetOption(*p, nullptr))
		{
			fuseArgs.push_back(*p);
		}
	}

	if(argv[1] && baseUrl.SetHttpUrl(argv[1]))
	{
		// FUSE adds starting / already, drop ours if present
		trimBack(baseUrl.sPath, "/");
#ifdef VERBOSE
		cout << "Base URL: " << baseUrl.ToString()<<endl;
#endif
	}
	else
	{
		cerr << "Invalid base URL, " << argv[1] << endl;
		exit(EXIT_FAILURE);
	}

	if (! cfg::GetProxyInfo() || cfg::GetProxyInfo()->sHost.empty())
	{
		cerr << "WARNING: proxy not specified or invalid, please set something like proxy=localserver:3142."
" Continuing without proxy server for now, this might cause severe performance degradation!" << endl;
		exit(EXIT_FAILURE);
	}

	cfg::PostProcConfig();
#ifdef DEBUG
	cfg::debug=0xff;
	cfg::verboselog=1;
	log::open();
#endif

	// all parameters processed, forwarded to fuse call below

	acng::rex::CompileExpressions();

	// test mount point
	if(!argv[2] || stat(argv[2], &statTempl) || statvfs(argv[2], &stvfsTemp))
		barf(endl << "Cannot access directory " << argv[2]);
	if(!S_ISDIR(statTempl.st_mode))
		barf(endl<< argv[2] << " is not a directory.");

	// restore application arguments

	evabaseFreeRunner eb(g_tcp_con_factory, false);

	auto expTimer = event_new(eb.getBase(), -1, EV_PERSIST, expireAgents, nullptr);
	struct timeval expTv { 30, 30 };
	evtimer_add(expTimer, &expTv);

#ifdef HAVE_DLOPEN
	auto pLib = dlopen("libfuse.so.2", RTLD_LAZY);
	if(!pLib)
	{
		cerr << "Couldn't find libfuse.so.2" <<endl;
		return -1;
	}
	auto pFuseSetup = (decltype(&fuse_setup)) dlsym(pLib, "fuse_setup");
	auto pFuseLoop = (decltype(&fuse_loop_mt)) dlsym(pLib, "fuse_loop_mt");
	if(!pFuseSetup || !pFuseLoop)
	{
		cerr << "Error loading libfuse.so.2" <<endl;
		return -2;
	}
#else
	auto pFuseSetup = &fuse_setup;
	auto pFuseLoop = &fuse_loop_mt;
#endif

	int mt = 1;
	auto fs = (*pFuseSetup) (fuseArgs.size(), &fuseArgs[0],
			&acngfs_oper, sizeof(acngfs_oper),
			&argv[2], &mt, nullptr);
	if (!fs)
		return 2;
	auto ret = (*pFuseLoop) (fs);
	// shutdown agents in reliable fashion
	spare_agents.clear();
	return ret;
}
