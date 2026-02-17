#include "config.h"
#include "meta.h"
#include "acfg.h"
#include "aclogger.h"
#include "tcpconnect.h"
#include "acbuf.h"
#include "aclogger.h"
#include "dirwalk.h"
#include "debug.h"
#include "dlcon.h"
#include "fileio.h"
#include "acregistry.h"
#include "sockio.h"
#include "bgtask.h"
#include "ac3rdparty.h"
#include "filereader.h"
#include "csmapping.h"
#include "cleaner.h"
#include "ebrunner.h"

#include <functional>
#include <thread>
#include <iostream>
#include <fstream>
#include <string>
#include <list>
#include <queue>

#include <cstdbool>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <regex.h>
#include <errno.h>

using namespace std;
using namespace acng;

bool g_bVerbose = false;

#define SUICIDE_TIMEOUT 3600

namespace acng {
extern std::shared_ptr<cleaner> g_victor;
namespace log
{
	extern mstring g_szLogPrefix;
}
}

// from sockio.cc in more recent versions
bool isUdsAccessible(cmstring& path)
{
	Cstat s(path);
	return s && S_ISSOCK(s.st_mode) && 0 == access(path.c_str(), W_OK);
}

struct ACNG_API IFitemFactory
{
	virtual SHARED_PTR<fileitem> Create() =0;
	virtual ~IFitemFactory() =default;
};
struct ACNG_API CPrintItemFactory : public IFitemFactory
{
	virtual SHARED_PTR<fileitem> Create()
	{
		class tPrintItem: public fileitem
			{
			public:
				tPrintItem() : fileitem("<STREAM>")
				{
				}
                virtual FiStatus Setup() override
				{
					return m_status = FIST_INITED;
				}
				virtual unique_fd GetFileFd() override
				{
					return unique_fd();
				}
				ssize_t SendData(int, int, off_t&, size_t) override
				{
					return 0;
				}

			protected:
                bool DlStarted(string_view raw, const tHttpDate& dat, cmstring& orig, tRemoteStatus status, off_t nseek, off_t ntotal) override
                {
					static auto opt_dbg = getenv("ACNGTOOL_DEBUG_DOWNLOAD");
					if (opt_dbg && *opt_dbg)
                        std::cerr << raw << std::endl;
                    return fileitem::DlStarted(raw, dat, orig, status, nseek, ntotal);
				}
				void DlFinish(bool) override
				{
					m_status = FIST_COMPLETE;
				}
				bool DlAddData(acng::string_view chunk, lockuniq&) override
				{
					return (chunk.size() == fwrite(chunk.data(), sizeof(char), chunk.size(), stdout));
				}
			};
		return make_shared<tPrintItem>();
	}
};

// That is relevant to push the download agent logics correctly and are shown in logs;
// not relevant for the actual connection since it's rerouted through TUdsFactory
#define FAKE_UDS_HOSTNAME "UNIX-DOMAIN-SOCKET"

/**
 * Create a special processor which looks for error markers in the download stream and
 * reports the result afterwards.
 */

class tRepItem: public fileitem
{
	mstring m_prevRest;
	string m_key = maark;
	tStrDeq m_warningCollector;

public:

	tRepItem() : fileitem("<STREAM>")
	{
		m_nSizeChecked = m_nSizeCachedInitial = 0;
	}
	virtual FiStatus Setup() override
	{
		return m_status = FIST_INITED;
	}

protected:
	/*
	 *
<span class="ERROR">Found errors during processing, aborting as requested.</span>
<!--
41d_a6aeb8-26dfa2Errors found, aborting expiration...
-->
<br><b>End of log output. Please reload to run again.</b>
*/
	bool DlAddData(acng::string_view chunk, lockuniq&) override
	{
		// glue with the old prefix if needed, later move remainder back the rest buffer if needed
		string_view input;
		if (!m_prevRest.empty())
		{
			m_prevRest += chunk;
			input = m_prevRest;
		}
		else
			input = chunk;

		while (true)
		{
			auto pos = input.find(m_key);
			if (pos == stmiss)
			{
				// drop all remaining input except for the unfinished line
				pos = input.rfind('\n');
				if (pos != stmiss)
				{
					input.remove_prefix(pos + 1);
					m_prevRest = mstring(input);
				}
				return true;
			}
			input.remove_prefix(pos);
			auto nEnd = input.find('\n');
			if (nEnd == stmiss)
			{
				// our message but unfinished
				m_prevRest = mstring(input);
				return true;
			}
			// decode header and get a plain message view
			auto msgType = ControLineType(input[m_key.size()]);
			string_view msg(input.data() + m_key.size() + 1, nEnd - m_key.size() - 1);
			input.remove_prefix(msg.size());
			if (msg.empty())
				continue;
			if (msgType == ControLineType::BeforeError)
			{
				m_warningCollector.emplace_back(msg);
			}
			else if(msgType == ControLineType::Error)
			{
					for (auto l : m_warningCollector)
						cerr << l << endl;
					cerr << msg << endl;
			}
		}
		return true;
	}
};

bool DownloadItem(tHttpUrl url, dlcon &dlConnector, const SHARED_PTR<fileitem> &fi)
{
	dlConnector.AddJob(fi, move(url));

    auto fistatus = fi->WaitForFinish();
    return fistatus.first == fileitem::FIST_COMPLETE && fistatus.second.code == 200;
	// just be sure to set a proper error code
//	if(fistatus.first != fileitem::FIST_COMPLETE && fistatus.second.code < 400)
//		fi->GetHeader().frontLine = "909 Incomplete download";
}
int wcat(LPCSTR url, LPCSTR proxy, IFitemFactory*, const IDlConFactory &pdlconfa);

static void usage(int retCode = 0, LPCSTR cmd = nullptr)
{
	if(cmd)
	{
		if(0 == strcmp(cmd, "shrink"))
			cerr << "USAGE: acngtool shrink numberX [-f | -n] [-x] [-v] [variable assignments...]" <<endl <<
			"-f: delete files"<< endl <<
			"-n: dry run, display results" << endl <<
			"-v: more verbosity" << endl <<
			"-x: also drop index files (can be dangerous)" <<endl <<
			"Suffix X can be k,K,m,M,g,G (for kb,KiB,mb,MiB,gb,GiB)" << endl;
	}
	else
		(retCode ? cout : cerr) <<
		"Usage: acngtool command parameter... [options]\n\n"
			"command := { printvar, cfgdump, retest, patch, curl, encb64, maint, shrink }\n"
			"parameter := (specific to command)\n"
			"options := (see apt-cacher-ng options)\n"
			"extra options := -h, --verbose\n"
#if SUPPWHASH
#warning FIXME
			"-H: read a password from STDIN and print its hash\n"
#endif
"\n";
			exit(retCode);
}

struct pkgEntry
{
	std::string path;
	time_t lastDate;
	blkcnt_t blocks;
	// for prio.queue, oldest shall be on top
	bool operator<(const pkgEntry &other) const
	{
		return lastDate > other.lastDate;
	}
};

int shrink(off_t wantedSize, bool dryrun, bool apply, bool verbose, bool incIfiles)
{
	if(!dryrun && !apply)
	{
		cerr << "Error: needs -f or -n options" << endl;
		return 97;
	}
	if(dryrun && apply)
	{
		cerr << "Error: -f and -n are mutually exclusive" <<endl;
		return 107;
	}
//	cout << "wanted: " << wantedSize << endl;
	std::priority_queue<pkgEntry/*, vector<pkgEntry>, cmpLessDate */ > delQ;
	std::unordered_map<string, pair<time_t,off_t> > related;

	blkcnt_t totalBlocks = 0;

	IFileHandler::FindFiles(cfg::cachedir,
			[&delQ, &totalBlocks, &related, &incIfiles](cmstring & path, const struct stat& finfo) -> bool
			{
		// reference date used in the prioqueue heap
		auto dateLatest = max(finfo.st_ctim.tv_sec, finfo.st_mtim.tv_sec);
		auto isHead = endsWithSzAr(path, ".head");
		string pkgPath, otherName;
		if(isHead)
		{
			pkgPath = path.substr(0, path.length()-5);
			otherName = pkgPath;
		}
		else
		{
			pkgPath = path;
			otherName = path + ".head";
		}
		auto ftype = rex::GetFiletype(pkgPath);
		if((ftype==rex::FILE_SPECIAL_VOLATILE || ftype == rex::FILE_VOLATILE) && !incIfiles)
			return true;
		// anything else is considered junk

		auto other = related.find(otherName);
		if(other == related.end())
		{
			// the related file will appear soon
			related.insert(make_pair(path, make_pair(dateLatest, finfo.st_blocks)));
			return true;
		}
		// care only about stamps on .head files (track mode)
		// or ONLY about data file's timestamp (not-track mode)
		if( (cfg::trackfileuse && !isHead) || (!cfg::trackfileuse && isHead))
			dateLatest = other->second.first;

		auto bothBlocks = (finfo.st_blocks + other->second.second);
		related.erase(other);

		totalBlocks += bothBlocks;
		delQ.push({pkgPath, dateLatest, bothBlocks});

		return true;
			}
	, true, false);

	// there might be some unmatched remains...
	for(auto kv: related)
		delQ.push({kv.first, kv.second.first, kv.second.second});
	related.clear();

	auto foundSizeString = offttosHdotted(totalBlocks*512);
	blkcnt_t wantedBlocks = wantedSize / 512;

	if(totalBlocks < wantedBlocks)
	{
		if(verbose)
			cout << "Requested size smaller than current size, nothing to do." << endl;
		return 0;
	}

	if(verbose)
	{
		cout << "Found " << foundSizeString << " bytes of relevant data, reducing to "
		<< offttosHdotted(wantedSize) << " (~"<< (wantedBlocks*100/totalBlocks) << "%)"
		<< endl;
	}
	while(!delQ.empty())
	{
		bool todel = (totalBlocks > wantedBlocks);
		if(todel)
			totalBlocks -= delQ.top().blocks;
		const char *msg = 0;
		if(verbose || dryrun)
			msg = (todel ? "Delete: " : "Keep: " );
		auto& delpath(delQ.top().path);
		if(msg)
			cout << msg << delpath << endl << msg << delpath << ".head" << endl;
		if(todel && apply)
		{
			unlink(delpath.c_str());
			unlink(mstring(delpath + ".head").c_str());
		}
		delQ.pop();
	}
	if(verbose)
	{
		cout << "New size: " << offttosHdotted(totalBlocks*512) << " (before: "
		<< foundSizeString << ")" << endl;
	}
	return 0;
}

#if SUPPWHASH

int hashpwd()
{
#ifdef HAVE_SSL
	string plain;
	uint32_t salt=0;
	for(unsigned i=10; i; --i)
	{
		if(RAND_bytes(reinterpret_cast<unsigned char*>(&salt), 4) >0)
			break;
		else
			salt=0;
		sleep(1);
	}
	if(!salt) // ok, whatever...
	{
		uintptr_t pval = reinterpret_cast<uintptr_t>(&plain);
		srandom(uint(time(0)) + uint(pval) +uint(getpid()));
		salt=random();
		timespec ts;
		clock_gettime(CLOCK_BOOTTIME, &ts);
		for(auto c=(ts.tv_nsec+ts.tv_sec)%1024 ; c; c--)
			salt=random();
	}
	string crypass = BytesToHexString(reinterpret_cast<const uint8_t*>(&salt), 4);
#ifdef DEBUG
	plain="moopa";
#else
	cin >> plain;
#endif
	trimString(plain);
	if(!AppendPasswordHash(crypass, plain.data(), plain.size()))
		return EXIT_FAILURE;
	cout << crypass <<endl;
	return EXIT_SUCCESS;
#else
	cerr << "OpenSSL not available, hashing functionality disabled." <<endl;
	return EXIT_FAILURE;
#endif
}


bool AppendPasswordHash(string &stringWithSalt, LPCSTR plainPass, size_t passLen)
{
	if(stringWithSalt.length()<8)
		return false;

	uint8_t sum[20];
	if(1!=PKCS5_PBKDF2_HMAC_SHA1(plainPass, passLen,
			(unsigned char*) (stringWithSalt.data()+stringWithSalt.size()-8), 8,
			NUM_PBKDF2_ITERATIONS,
			sizeof(sum), (unsigned char*) sum))
		return false;
	stringWithSalt+=EncodeBase64((LPCSTR)sum, 20);
	stringWithSalt+="00";
#warning dbg
	// checksum byte
	uint8_t pCs=0;
	for(char c : stringWithSalt)
		pCs+=c;
	stringWithSalt+=BytesToHexString(&pCs, 1);
	return true;
}
#endif

typedef deque<acng::string_view> tPatchSequence;

// might need to access the last line externally
unsigned long rangeStart(0), rangeLast(0);

inline bool patchChunk(tPatchSequence& idx, LPCSTR pline, tPatchSequence diffPayload)
{
	bool append = false;
	char *pEnd = nullptr;
	auto n = strtoul(pline, &pEnd, 10);
	if (!pEnd || pline == pEnd)
		return false;
	rangeStart = n;

	switch (*pEnd)
	{
	case ',':
	{
		pline = pEnd + 1;
		n = strtoul(pline, &pEnd, 10);
		if (!pEnd || pline == pEnd || (*pEnd != 'c' && *pEnd != 'd'))
			return false;
		rangeLast = n;
		break;
	}
	case 'a':
		append = true;
		__just_fall_through;
	case 'c':
	case 'd':
	{
		// that is just one line?
		rangeLast = rangeStart = n;
		break;
	}
	default:
		return false;
	}

	if (rangeStart > idx.size() || rangeLast > idx.size() || rangeStart > rangeLast)
		return false;

	if (append)
		idx.insert(idx.begin() + (size_t) rangeStart + 1, diffPayload.begin(), diffPayload.end());
	else
	{
		size_t i = 0;
		for (; i < diffPayload.size(); ++i, ++rangeStart)
		{
			if (rangeStart <= rangeLast)
				idx[rangeStart] = diffPayload[i];
			else
				break; // new stuff bigger than replaced range
		}
		if (i < diffPayload.size()) // not enough space :-(
			idx.insert(idx.begin() + (size_t) rangeStart, diffPayload.begin() + i, diffPayload.end());
		else if (rangeStart - 1 != rangeLast) // less data now?
			idx.erase(idx.begin() + (size_t) rangeStart, idx.begin() + (size_t) rangeLast + 1);
	}
	return true;
}

/**
 * Helper which implements a custom connection class that runs through a specified Unix Domain
 * Socket (see base class for the name).
 */
struct TUdsFactory : public ::acng::IDlConFactory
{
	void RecycleIdleConnection(tDlStreamHandle &) const override
	{
		// keep going, no recycling/restoring
	}
	tDlStreamHandle CreateConnected(cmstring&, uint16_t, mstring& sErrorOut, bool*,
			tRepoUsageHooks*, bool, int, bool) const override
	{
		struct udsconnection: public tcpconnect
		{
			bool failed = false;
			udsconnection() : tcpconnect(nullptr)
			{
				// some static and dummy parameters, and invalidate SSL for sure
				m_ssl = nullptr;
				m_bio = nullptr;
				m_sHostName = FAKE_UDS_HOSTNAME;
				m_nPort = 0;

				m_conFd = socket(PF_UNIX, SOCK_STREAM, 0);
				if (m_conFd < 0)
				{
					failed = true;
					return;
				}
				struct sockaddr_un addr;
				addr.sun_family = PF_UNIX;
				strcpy(addr.sun_path, cfg::udspath.c_str());
				socklen_t adlen = cfg::udspath.length() + 1 + offsetof(struct sockaddr_un, sun_path);
				if (connect(m_conFd, (struct sockaddr*) &addr, adlen))
				{
					DBGQLOG(tErrnoFmter("connect result: "));
					checkforceclose(m_conFd);
					failed = true;
					return;
				}
				// basic identification needed
				tSS ids;
				ids << "GET / HTTP/1.0\r\nX-Original-Source: localhost\r\n\r\n";
				if (!ids.send(m_conFd))
				{
					failed = true;
					return;
				}
			}
		};
		auto ret = make_shared<udsconnection>();
		// mimic regular processing of a bad result here!
		if(ret && ret->failed) ret.reset();
		if(!ret) sErrorOut = "912 Cannot establish control connection";
		return ret;
	}
};

int maint_job()
{
	if (cfg::reportpage.empty())
	{
		cerr << "ReportPage is not configured in the server config, aborting..." <<endl;
		return -1;
	}

	// base target URL, can be adapted for TCP requests
	tHttpUrl url;
	url.sUserPass = cfg::adminauth;
	LPCSTR req = getenv("ACNGREQ");
	url.sPath = "/" + cfg::reportpage + (req ? req : "?doExpire=Start+Expiration&abortOnErrors=aOe");

	auto isInsecForced = []() { auto se = getenv("ACNG_INSECURE"); return se && *se; };

	// by default, use the socket connection; if credentials require it -> enforce it
	bool have_cred = !url.sUserPass.empty(),
			have_uds = !cfg::udspath.empty(),
			try_tcp = !have_cred;
	bool uds_ok = have_uds && isUdsAccessible(cfg::udspath);

	if(have_cred)
	{
		if(isInsecForced()) // so try TCP anyway
		{
			try_tcp = true;
		}
		else if(have_uds && !uds_ok)
		{
			cerr << "This operation transmits credentials but the socket (" << cfg::udspath
				 << ") is currently not accessible!" << endl;
			return EXIT_FAILURE;
		}
		else if(!have_uds)
		{
			cerr << "This operation transmits credentials but SocketPath is not configured to a safe location in the server configuration. "
					"Please set SocketPath to a safe location, or set ACNG_INSECURE environment variable to override this check."
					<<endl;
			return EXIT_FAILURE;
		}
		// ok, otherwise use Unix Domain Socket
	}

	bool response_ok = false;
	if(have_uds && uds_ok)
	{
		DBGQLOG("Trying UDS path")
				auto fi = make_shared<tRepItem>();

		url.sHost = FAKE_UDS_HOSTNAME;
		url.SetPort(0);

		TUdsFactory udsFac;
		evabaseFreeRunner eb(udsFac, true, SUICIDE_TIMEOUT);
		response_ok = DownloadItem(url, eb.getDownloader(), fi);
		DBGQLOG("UDS result: " << response_ok)
	}
	if(!response_ok && try_tcp)
	{
		DBGQLOG("Trying TCP path")
				// never use a proxy here (insecure?), those are most likely local IPs
				cfg::SetOption("Proxy=", nullptr);
		cfg::nettimeout = 30;
		vector<string> hostips;
		Tokenize(cfg::bindaddr, SPACECHARS, hostips, false);
		if(hostips.empty())
			hostips.emplace_back("127.0.0.1");
		for (const auto &tgt : hostips)
		{
			url.sHost = tgt;
			url.SetPort(cfg::port);

			evabaseFreeRunner eb(g_tcp_con_factory, true, SUICIDE_TIMEOUT);
			auto fi = make_shared<tRepItem>();
			response_ok = DownloadItem(url, eb.getDownloader(), fi);
			if (response_ok)
				break;
		}
	}
	if(!response_ok)
	{
		cerr << "Could not make a valid request to the server. Please visit "
				<< url.ToURI(false) << " and check special conditions." <<endl;
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

int patch_file(string sBase, string sPatch, string sResult)
{
	filereader frBase, frPatch;
	if(!frBase.OpenFile(sBase, true) || !frPatch.OpenFile(sPatch, true))
		return -2;
	auto buf = frBase.GetBuffer();
	auto size = frBase.GetSize();
	tPatchSequence lines;
	lines.emplace_back(buf, 0); // dummy entry to avoid -1 calculations because of ed numbering style
	for (auto p = buf; p < buf + size;)
	{
		LPCSTR crNext = strchr(p, '\n');
		if (crNext)
		{
			lines.emplace_back(p, crNext + 1 - p);
			p = crNext + 1;
		}
		else
		{
			lines.emplace_back(p, buf + size - p);
			break;
		}
	}

	auto pbuf = frPatch.GetBuffer();
	auto psize = frPatch.GetSize();
	tPatchSequence chunk;
	LPCSTR patchCmd =0;
	size_t patchCmdLen = 0;
	for (auto p = pbuf; p < pbuf + psize;)
	{
		// accumulate lines until chunk is consumed
		LPCSTR crNext = strchr(p, '\n');
		size_t len = 0;
		LPCSTR line=p;
		if (crNext)
		{
			len = crNext + 1 - p;
			p = crNext + 1;
		}
		else
		{
			len = pbuf + psize - p;
			p = pbuf + psize + 1; // break signal, actually
		}
		p=crNext+1;

		bool hunkOK = (len == 2 && *line == '.');
		if(!hunkOK)
		{
			if(!patchCmdLen)
			{
				if(!strncmp("s/.//\n", line, 6))
				{
					// oh, that's the fix-the-last-line command :-(
					if(rangeStart)
					{
						lines[rangeStart] = ".\n";
					}
					continue;
				}
				else if(line[0] == 'w')
					continue; // don't care, we know the target

				patchCmdLen = len;
				patchCmd = line;

				if(len>2 && line[len-2] == 'd')
					hunkOK = true; // no terminator to expect
			}
			else
				chunk.emplace_back(line, len);
		}

		if(hunkOK)
		{
			if(!patchChunk(lines, patchCmd, chunk))
			{
				cerr << "Bad patch line: ";
				cerr.write(patchCmd, patchCmdLen);
				exit(EINVAL);
			}
			chunk.clear();
			patchCmdLen = 0;
		}
	}
	ofstream res(sResult.c_str());
	if(!res.is_open())
		return -3;

	for(const auto& kv : lines)
		res.write(kv.data(), kv.size());
	res.flush();
//	dump_proc_status_always();
	return res.good() ? 0 : -4;
}


struct parm {
	unsigned minArg, maxArg; // if maxArg is UINT_MAX, there will be a final call with NULL argument
	std::function<void(LPCSTR)> f;
};

// some globals shared across the functions
int g_exitCode(0);
LPCSTR g_missingCfgDir = nullptr;

void parse_options(int argc, const char **argv, function<void (LPCSTR)> f)
{
	LPCSTR szCfgDir=CFGDIR;
	std::vector<LPCSTR> validargs, nonoptions;
	bool ignoreCfgErrors = false;

	for (auto p=argv; p<argv+argc; p++)
	{
		if (!strncmp(*p, "-h", 2))
			usage();
		else if (!strcmp(*p, "-c"))
		{
			++p;
			if (p < argv + argc)
				szCfgDir = *p;
			else
				usage(2);
		}
		else if(!strcmp(*p, "--verbose"))
			g_bVerbose=true;
		else if(!strcmp(*p, "-i"))
			ignoreCfgErrors = true;
		else if(**p) // not empty
			validargs.emplace_back(*p);

#if SUPPWHASH
#warning FIXME
		else if (!strncmp(*p, "-H", 2))
			exit(hashpwd());
#endif
	}

	if(szCfgDir)
	{
		Cstat info(szCfgDir);
		if(!info || !S_ISDIR(info.st_mode))
			g_missingCfgDir = szCfgDir;
		else
			cfg::ReadConfigDirectory(szCfgDir, ignoreCfgErrors);
	}

	tStrVec non_opt_args;

	for(auto& keyval : validargs)
	{
		cfg::g_bQuiet = true;
		if(!cfg::SetOption(keyval, 0))
			nonoptions.emplace_back(keyval);
		cfg::g_bQuiet = false;
	}

	cfg::PostProcConfig();

#ifdef DEBUG
	log::g_szLogPrefix = "acngtool";
	log::open();
#endif

	for(const auto& x: nonoptions)
		f(x);
}


#if SUPPWHASH
void ssl_init()
{
#ifdef HAVE_SSL
	SSL_load_error_strings();
	ERR_load_BIO_strings();
	ERR_load_crypto_strings();
	ERR_load_SSL_strings();
	OpenSSL_add_all_algorithms();
	SSL_library_init();
#endif
}
#endif

/*
void assert_cfgdir()
{
	if(!g_missingCfgDir)
		return;
	cerr << "Failed to open config directory: " << g_missingCfgDir <<endl;
	exit(EXIT_FAILURE);
}
*/

void warn_cfgdir()
{
	if (g_missingCfgDir)
		cerr << "Warning: failed to open config directory: " << g_missingCfgDir <<endl;
}

std::unordered_map<string, parm> parms = {
#if 0
   {
		"urltest",
		{ 1, 1, [](LPCSTR p)
			{
				std::cout << EncodeBase64Auth(p);
			}
		}
	}
	,
#endif
#if 0
   {
		"bin2hex",
		{ 1, 1, [](LPCSTR p)
			{
         filereader f;
         if(f.OpenFile(p, true))
            exit(EIO);
         std::cout << BytesToHexString(f.GetBuffer(), f.GetSize()) << std::endl;
         exit(EXIT_SUCCESS);
			}
		}
	}
	,
#endif
#if 0 // def HAVE_DECB64
	{
     "decb64",
     { 1, 1, [](LPCSTR p)
        {
#ifdef DEBUG
           cerr << "decoding " << p <<endl;
#endif
           acbuf res;
           if(DecodeBase64(p, strlen(p), res))
           {
              std::cout.write(res.rptr(), res.size());
              exit(0);
           }
           exit(1);
        }
     }
  }
	,
#endif
	{
		"encb64",
		{ 1, 1, [](LPCSTR p)
			{
#ifdef DEBUG
			cerr << "encoding " << p <<endl;
#endif
				std::cout << EncodeBase64Auth(p);
			}
		}
	}
	,
		{
			"cfgdump",
			{ 0, 0, [](LPCSTR) {
				warn_cfgdir();
						     cfg::dump_config(false);
					     }
			}
		}
	,
		{
			"curl",
			{ 1, UINT_MAX, [](LPCSTR p)
				{
					if(!p)
						return;

					CPrintItemFactory fac;
					auto ret=wcat(p, getenv("http_proxy"), &fac, g_tcp_con_factory);
					if(!g_exitCode)
						g_exitCode = ret;

				}
			}
		},
		{
			"retest",
			{
				1, 1, [](LPCSTR p)
				{
					warn_cfgdir();
					std::cout << ReTest(p) << std::endl;
				}
			}
		}
	,
		{
			"printvar",
			{
				1, 1, [](LPCSTR p)
				{
					warn_cfgdir();
					auto ps(cfg::GetStringPtr(p));
					if(ps) { cout << *ps << endl; return; }
					auto pi(cfg::GetIntPtr(p));
					if(pi) {
						cout << *pi << endl;
						return;
					}
					g_exitCode=23;
				}
			}
		},
		{ 
			"patch",
			{
				3, 3, [](LPCSTR p)
				{
					static tStrVec iop;
					iop.emplace_back(p);
					if(iop.size() == 3)
						g_exitCode+=patch_file(iop[0], iop[1], iop[2]);
				}
			}
		}

	,
		{
			"maint",
			{
				0, 0, [](LPCSTR)
				{
					warn_cfgdir();
					g_exitCode+=maint_job();
				}
			}
		}
   ,
   {
		   "shrink",
		   {
				   1, UINT_MAX, [](LPCSTR p)
				   {
					   static bool dryrun(false), apply(false), verbose(false), incIfiles(false);
					   static off_t wantedSize(4000000000);
					   if(!p)
						   g_exitCode += shrink(wantedSize, dryrun, apply, verbose, incIfiles);
					   else if(*p > '0' && *p<='9')
						   wantedSize = strsizeToOfft(p);
					   else if(*p == '-')
					   {
						   for(++p;*p;++p)
						   {
							   if(*p == 'f') apply = true;
							   else if(*p == 'n') dryrun = true;
							   else if (*p == 'x') incIfiles = true;
							   else if (*p == 'v') verbose = true;
						   }
					   }
				   }
		   }
   }
};

int main(int argc, const char **argv)
{
	using namespace acng;
	ac3rdparty libInit;
	g_victor.reset(new cleaner(false, SHARED_PTR<IFileItemRegistry>()));

	string exe(argv[0]);
	unsigned aOffset=1;
	if(endsWithSzAr(exe, "expire-caller.pl"))
	{
		aOffset=0;
		argv[0] = "maint";
	}
	cfg::g_bQuiet = false;
	cfg::g_bNoComplex = true; // no DB for just single variables
	cfg::minilog = true;	// no fancy timestamps and only STDERR output

  parm* parm = nullptr;
  LPCSTR mode = nullptr;
  unsigned xargCount = 0;

	parse_options(argc-aOffset, argv+aOffset, [&](LPCSTR p)
			{
		bool bFirst = false;
		if(!mode)
			bFirst = (0 != (mode = p));
		else
			xargCount++;
		if(!parm)
			{
			auto it = parms.find(mode);
			if(it == parms.end())
				usage(1);
			parm = & it->second;
			}
		if(xargCount > parm->maxArg)
			usage(2);
		if(!bFirst)
			parm->f(p);
			});
	if(!mode || !parm)
		usage(3);

	if(!xargCount) // should run the code at least once?
	{
		if(parm->minArg) // uh... needs argument(s)
			usage(4, mode);
		parm->f(nullptr);
	}
	else if(parm->maxArg == UINT_MAX) // or needs to terminate it?
		parm->f(nullptr);
	return g_exitCode;
}

int wcat(LPCSTR surl, LPCSTR proxy, IFitemFactory* fac, const IDlConFactory &pDlconFac)
{
	cfg::dnscachetime=0;
	cfg::persistoutgoing=0;
	cfg::badredmime.clear();
	cfg::redirmax=10;

	if(proxy)
		if(cfg::SetOption(string("proxy:")+proxy, nullptr))
			return -1;
	tHttpUrl url;
	if(!surl)
		return 2;
	string xurl(surl);
	if(!url.SetHttpUrl(xurl, false))
		return -2;

	evabaseFreeRunner eb(pDlconFac, true);

	auto fi=fac->Create();
	eb.getDownloader().AddJob(fi, move(url));
    auto fistatus = fi->WaitForFinish();
    if(fistatus.first == fileitem::FIST_COMPLETE && fistatus.second.code == 200)
		return EXIT_SUCCESS;

	// don't reveal passwords
	auto xpos=xurl.find('@');
	if(xpos!=stmiss)
		xurl.erase(0, xpos+1);
    cerr << "Error: cannot fetch " << xurl <<" : "  << fistatus.second.msg << endl;
    if (fistatus.second.code >= 500)
		return EIO;
    if (fistatus.second.code >= 400)
		return EACCES;

	return EXIT_FAILURE;
}

#if 0

void do_stuff_before_config()
{
	LPCSTR envvar(nullptr);

	cerr << "Pandora: " << sizeof(regex_t) << endl;
	/*
	// PLAYGROUND
	if (argc < 2)
		return -1;

	acng::cfg:tHostInfo hi;
	cout << "Parsing " << argv[1] << ", result: " << hi.SetUrl(argv[1]) << endl;
	cout << "Host: " << hi.sHost << ", Port: " << hi.sPort << ", Path: "
			<< hi.sPath << endl;
	return 0;

	bool Bz2compressFile(const char *, const char*);
	return !Bz2compressFile(argv[1], argv[2]);

	char tbuf[40];
	FormatCurrentTime(tbuf);
	std::cerr << tbuf << std::endl;
	exit(1);
	*/
	envvar = getenv("PARSEIDX");
	if (envvar)
	{
		int parseidx_demo(LPCSTR);
		exit(parseidx_demo(envvar));
	}

	envvar = getenv("GETSUM");
	if (envvar)
	{
		uint8_t csum[20];
		string s(envvar);
		off_t resSize;
		bool ok = filereader::GetChecksum(s, CSTYPE_SHA1, csum, false, resSize /*, stdout*/);
		if(!ok)
		{
			perror("");
			exit(1);
		}
		for (unsigned i = 0; i < sizeof(csum); i++)
			printf("%02x", csum[i]);
		printf("\n");
		envvar = getenv("REFSUM");
		if (ok && envvar)
		{
			if(CsEqual(envvar, csum, sizeof(csum)))
			{
				printf("IsOK\n");
				exit(0);
			}
			else
			{
				printf("Diff\n");
				exit(1);
			}
		}
		exit(0);
	}
}

#endif
#if 0
#warning line reader test enabled
	if (cmd == "wcl")
	{
		if (argc < 3)
			usage(2);
		filereader r;
		if (!r.OpenFile(argv[2], true))
		{
			cerr << r.getSErrorString() << endl;
			return EXIT_FAILURE;
		}
		size_t count = 0;
		auto p = r.GetBuffer();
		auto e = p + r.GetSize();
		for (;p < e; ++p)
			count += (*p == '\n');
		cout << count << endl;

		exit(EXIT_SUCCESS);
	}
#endif
#if 0
#warning header parser enabled
	if (cmd == "htest")
	{
		header h;
		h.LoadFromFile(argv[2]);
		cout << string(h.ToString()) << endl;

		h.clear();
		filereader r;
		r.OpenFile(argv[2]);
		std::vector<std::pair<std::string, std::string>> oh;
		h.Load(r.GetBuffer(), r.GetSize(), &oh);
		for(auto& r : oh)
			cout << "X:" << r.first << " to " << r.second;
		exit(0);
	}
#endif
#if 0
#warning benchmark enabled
	if (cmd == "benchmark")
	{
		dump_proc_status_always();
		cfg::g_bQuiet = true;
		cfg::g_bNoComplex = false;
		parse_options(argc - 2, argv + 2, true);
		cfg::PostProcConfig();
		string s;
		tHttpUrl u;
		int res=0;
/*
		acng::cfg:tRepoResolvResult hm;
		tHttpUrl wtf;
		wtf.SetHttpUrl(non_opt_args.front());
		acng::cfg:GetRepNameAndPathResidual(wtf, hm);
*/
		while(cin)
		{
			std::getline(cin, s);
			s += "/xtest.deb";
			if(u.SetHttpUrl(s))
			{
				cfg::tRepoResolvResult xdata;
				cfg::GetRepNameAndPathResidual(u, xdata);
				cout << s << " -> "
						<< (xdata.psRepoName ? "matched" : "not matched")
						<< endl;
			}
		}
		dump_proc_status_always();
		exit(res);
	}
#endif
