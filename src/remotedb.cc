
#include "remotedb.h"
#include "meta.h"
#include "acfg.h"
#include "acfgshared.h"

#include "filereader.h"
#include "aclogger.h"
#include "lockable.h"
#include "cleaner.h"
#include "debug.h"
#include "portutils.h"

#include <list>
#include <iostream>

// XXX: legacy from acfg.h, improve?
#define BARF(x) {if(!g_bQuiet) { cerr << x << endl;} exit(EXIT_FAILURE); }

using namespace std;

namespace acng
{

using namespace cfg;

std::map<cmstring, tRepoData> repoparms;
typedef decltype(repoparms)::iterator tPairRepoNameData;
// maps hostname:port -> { <pathprefix,repopointer>, ... }
std::unordered_map<string, list<pair<cmstring,tPairRepoNameData>>> mapUrl2pVname;

unsigned ReadBackendsFile(const string & sFile, const string &sRepName);
unsigned ReadRewriteFile(const string & sFile, cmstring& sRepName);

inline decltype(repoparms)::iterator GetRepoEntryRef(const string & sRepName)
{
	auto it = repoparms.find(sRepName);
	if(repoparms.end() != it)
		return it;
	// strange...
	auto rv(repoparms.insert(make_pair(sRepName,tRepoData())));
	return rv.first;
}



void AddRemapFlag(const string & token, const string &repname)
{
	mstring key, value;
	if(!ParseOptionLine(token, key, value))
		return;

	tRepoData &where = repoparms[repname];

	if(key=="keyfile")
	{
		if(value.empty())
			return;
		if (cfg::debug & log::LOG_FLUSH)
			cerr << "Fatal keyfile for " <<repname<<": "<<value <<endl;

		where.m_keyfiles.emplace_back(value);
	}
	else if(key=="deltasrc")
	{
		if(value.empty())
			return;

		if(!endsWithSzAr(value, "/"))
			value+="/";

		if(!where.m_deltasrc.SetHttpUrl(value))
			cerr << "Couldn't parse Debdelta source URL, ignored " <<value <<endl;
	}
	else if(key=="proxy")
	{
		static std::list<tHttpUrl> alt_proxies;
		tHttpUrl cand;
		if(value.empty() || cand.SetHttpUrl(value))
		{
			alt_proxies.emplace_back(cand);
			where.m_pProxy = & alt_proxies.back();
		}
		else
		{
			cerr << "Warning, failed to parse proxy setting " << value << " , "
					<< endl << "ignoring it" <<endl;
		}
	}
}

void AddRemapInfo(bool bAsBackend, const string & token,
		const string &repname)
{
	if (0!=token.compare(0, 5, "file:"))
	{
		tHttpUrl url;
		if(! url.SetHttpUrl(token))
			BARF(token + " <-- bad URL detected");
		_FixPostPreSlashes(url.sPath);

		if (bAsBackend)
			repoparms[repname].m_backends.emplace_back(url);
		else
			mapUrl2pVname[url.GetHostPortKey()].emplace_back(
					url.sPath, GetRepoEntryRef(repname));
	}
	else
	{
		auto func = bAsBackend ? ReadBackendsFile : ReadRewriteFile;
		unsigned count = 0;
		for(auto& src : ExpandFileTokens(token))
			count += func(src, repname);
		if(!count)
			for(auto& src : ExpandFileTokens(token + ".default"))
				count = func(src, repname);
		if(!count && !g_bQuiet)
			cerr << "WARNING: No configuration was read from " << token << endl;
	}
}

struct tHookHandler: public tRepoUsageHooks, public base_with_mutex
{
	string cmdRel, cmdCon;
	time_t downDuration, downTimeNext;

	int m_nRefCnt;

	tHookHandler(cmstring&) :
		downDuration(30), downTimeNext(0), m_nRefCnt(0)
	{
//		cmdRel = "logger JobRelease/" + name;
//		cmdCon = "logger JobConnect/" + name;
	}
	virtual void OnRelease() override
	{
		setLockGuard;
		if (0 >= --m_nRefCnt)
		{
			//system(cmdRel.c_str());
			downTimeNext = ::time(0) + downDuration;
			cleaner::GetInstance().ScheduleFor(downTimeNext, cleaner::TYPE_ACFGHOOKS);
		}
	}
	virtual void OnAccess() override
	{
		setLockGuard;
		if (0 == m_nRefCnt++)
		{
			if(downTimeNext) // huh, already ticking? reset
				downTimeNext=0;
			else if (!cmdCon.empty())
			{
				if (system(cmdCon.c_str()))
					USRERR("Warning: " << cmdCon << " returned with error code.");
			}

		}
	}
};

void _AddHooksFile(cmstring& vname)
{
	tCfgIter itor(cfg::confdir+"/"+vname+".hooks");
	if(!itor.reader.CheckGoodState(false))
		return;

	struct tHookHandler &hs = *(new tHookHandler(vname));
	mstring key,val;
	while (itor.Next())
	{
		if(!ParseOptionLine(itor.sLine, key, val))
			continue;

		const char *p = key.c_str();
		trimBoth(val, SPACECHARS "\0");
		if (strcasecmp("PreUp", p) == 0)
		{
			hs.cmdCon = val;
		}
		else if (strcasecmp("Down", p) == 0)
		{
			hs.cmdRel = val;
		}
		else if (strcasecmp("DownTimeout", p) == 0)
		{
			errno = 0;
			unsigned n = strtoul(val.c_str(), nullptr, 10);
			if (!errno)
				hs.downDuration = n;
		}
	}
	repoparms[vname].m_pHooks = &hs;
}

tRepoResolvResult remotedb::GetRepNameAndPathResidual(const tHttpUrl & in)
{
	tRepoResolvResult result;

	// get all the URLs matching THE HOSTNAME
	auto rangeIt=mapUrl2pVname.find(in.GetHostPortKey());
	if(rangeIt == mapUrl2pVname.end())
		return result;

	tStrPos bestMatchLen(0);
	auto pBestHit = repoparms.end();

	// now find the longest directory part which is the suffix of requested URL's path
	for (auto& repo : rangeIt->second)
	{
		// rewrite rule path must be a real prefix
		// it's also surrounded by /, ensured during construction
		const string & prefix=repo.first; // path of the rewrite entry
		tStrPos len=prefix.length();
		if (len>bestMatchLen && in.sPath.size() > len && 0==in.sPath.compare(0, len, prefix))
		{
			bestMatchLen=len;
			pBestHit=repo.second;
		}
	}

	if(pBestHit != repoparms.end())
	{
		result.psRepoName = & pBestHit->first;
		result.sRestPath = in.sPath.substr(bestMatchLen);
		result.repodata = & pBestHit->second;
	}
	return result;
}

const tRepoData * remotedb::GetRepoData(cmstring &vname)
{
	auto it=repoparms.find(vname);
	if(it==repoparms.end())
		return nullptr;
	return & it->second;
}

unsigned ReadBackendsFile(const string & sFile, const string &sRepName)
{
	unsigned nAddCount=0;
	string key, val;
	tHttpUrl entry;

	tCfgIter itor(sFile);
	if(debug&6)
		cerr << "Reading backend file: " << sFile <<endl;

	if(!itor.reader.CheckGoodState(false))
	{
		if(debug&6)
			cerr << "No backend data found, " << sFile<< " ignored."<<endl;
		return 0;
	}

	while(itor.Next())
	{
		if(debug & log::LOG_DEBUG)
			cerr << "Backend URL: " << itor.sLine <<endl;

		trimBack(itor.sLine);

		if( entry.SetHttpUrl(itor.sLine)
				||	( itor.sLine.empty() && ! entry.sHost.empty() && ! entry.sPath.empty()) )
		{
			_FixPostPreSlashes(entry.sPath);
#ifdef DEBUG
			cerr << "Backend: " << sRepName << " <-- " << entry.ToURI(false) <<endl;
#endif
			repoparms[sRepName].m_backends.emplace_back(entry);
			nAddCount++;
			entry.clear();
		}
		else if(ParseKeyValLine(itor.sLine, key, val))
		{
			if(keyEq("Site", key))
				entry.sHost=val;
			else if(keyEq("Archive-http", key) || keyEq("X-Archive-http", key))
				entry.sPath=val;
		}
		else
		{
			BARF("Bad backend description, around line " << sFile << ":"
					<< itor.reader.GetCurrentLine());
		}
	}
	return nAddCount;
}

tRepoData::~tRepoData()
{
	delete m_pHooks;
}


//! @brief Fires hook callbacks in the background thread
time_t remotedb::BackgroundCleanup()
{
	time_t ret(END_OF_TIME), now(time(0));
	for (const auto& parm : repoparms)
	{
		if (!parm.second.m_pHooks)
			continue;
		tHookHandler & hooks = *(static_cast<tHookHandler*> (parm.second.m_pHooks));
		lockguard g(hooks);
		if (hooks.downTimeNext)
		{
			if (hooks.downTimeNext <= now) // is valid & time to execute?
			{
				if(!hooks.cmdRel.empty())
				{
					if (cfg::debug & log::LOG_MORE)
						log::misc(hooks.cmdRel, 'X');
					if (cfg::debug & log::LOG_FLUSH)
						log::flush();

					if (system(hooks.cmdRel.c_str()))
					{
						USRERR("Warning: " << hooks.cmdRel << " returned with error code.");
					}
					hooks.downTimeNext = 0;
				}
			}
			else // it's in future, take the earliest
				ret = min(ret, hooks.downTimeNext);
		}
	}
	return ret;
}

shared_ptr<remotedb> g_repoDb;

remotedb &remotedb::GetInstance()
{
	if (!g_repoDb)
		g_repoDb.reset(new remotedb);
	return *g_repoDb;
}

void remotedb::PostConfig()
{
	mapUrl2pVname.rehash(mapUrl2pVname.size());
	if (debug & log::LOG_DEBUG)
	{
		unsigned nUrls = 0;
		for (const auto &x : mapUrl2pVname)
			nUrls += x.second.size();

		if ((debug & log::LOG_MORE) && repoparms.size() > 0)
	{
			cerr << "Loaded " << repoparms.size() << " backend descriptors\nLoaded mappings for "
					<< mapUrl2pVname.size() << " hosts and " << nUrls << " paths\n";
	}
	}

}


/* This parses also legacy files, i.e. raw RFC-822 formated mirror catalogue from the
 * Debian archive maintenance repository.
 */
unsigned ReadRewriteFile(const string & sFile, cmstring& sRepName)
{
	unsigned nAddCount=0;
	filereader reader;
	if(debug>4)
		cerr << "Reading rewrite file: " << sFile <<endl;
	reader.OpenFile(sFile, false, 1);
	reader.CheckGoodState(true, &sFile);

	tStrVec hosts, paths;
	string sLine, key, val;
	tHttpUrl url;

	while (reader.GetOneLine(sLine))
	{
		trimFront(sLine);

		if (0 == sLine.compare(0, 1, "#"))
			continue;

		if (url.SetHttpUrl(sLine))
		{
			_FixPostPreSlashes(url.sPath);

			mapUrl2pVname[url.GetHostPortKey()].emplace_back(url.sPath,
					GetRepoEntryRef(sRepName));
#ifdef DEBUG
			cerr << "Mapping: " << url.ToURI(false) << " -> " << sRepName << endl;
#endif

			++nAddCount;
			continue;
		}

		// otherwise deal with the complicated RFC-822 format for legacy reasons

		if (sLine.empty()) // end of block, eof, ... -> commit it
		{
			if (hosts.empty() && paths.empty())
				continue; // dummy run or whitespace in a URL style list
			if ( !hosts.empty() && paths.empty())
			{
				cerr << "Warning, missing path spec for the site " << hosts[0] <<", ignoring mirror."<< endl;
				continue;
			}
			if ( !paths.empty() && hosts.empty())
			{
				BARF("Parse error, missing Site: field around line "
						<< sFile << ":"<< reader.GetCurrentLine());
			}
			for (const auto& host : hosts)
			{
				for (const auto& path : paths)
				{
					//mapUrl2pVname[*itHost+*itPath]= &itHostiVec->first;
					tHttpUrl url;
					url.sHost=host;
					url.sPath=path;
					mapUrl2pVname[url.GetHostPortKey()].emplace_back(url.sPath,
							GetRepoEntryRef(sRepName));

#ifdef DEBUG
						cerr << "Mapping: "<< host << path
						<< " -> "<< sRepName <<endl;
#endif

						++nAddCount;
				}
			}
			hosts.clear();
			paths.clear();
			continue;
		}

		if(!ParseKeyValLine(sLine, key, val))
		{
			BARF("Error parsing rewrite definitions, around line "
					<< sFile << ":"<< reader.GetCurrentLine() << " : " << sLine);
		}

		// got something, interpret it...
		if( keyEq("Site", key) || keyEq("Alias", key) || keyEq("Aliases", key))
			Tokenize(val, SPACECHARS, hosts, true);

		if(keyEq("Archive-http", key) || keyEq("X-Archive-http", key))
		{
			// help STL saving some memory
			if(sPopularPath==val)
				paths.emplace_back(sPopularPath);
			else
			{
				_FixPostPreSlashes(val);
				paths.emplace_back(val);
			}
			continue;
		}
	}

	return nAddCount;
}

}
