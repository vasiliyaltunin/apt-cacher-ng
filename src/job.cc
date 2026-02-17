
#include "job.h"

#include "debug.h"
#include "meta.h"
#include "acfg.h"
#include "remotedb.h"
#include "conn.h"
#include "acfg.h"
#include "fileitem.h"
#include "dlcon.h"
#include "sockio.h"
#include "fileio.h" // for ::stat and related macros
#include "maintenance.h"
#include "evabase.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <limits>
#include <queue>

#include <dirent.h>
#include <event2/buffer.h>
#include <errno.h>

using namespace std;

//#define FORCE_CHUNKED

#define PT_BUF_MAX 64000

// hunting Bug#955793: [Heisenbug] evbuffer_write_atmost returns -1 w/o updating errno
//#define DBG_DISCONNECT //std::cerr << "DISCO? " << __LINE__ << std::endl;

namespace acng
{
namespace cfg
{
namespace rex {
enum eMatchType : int8_t;
}
}
string_view sHttp11("HTTP/1.1");

tTraceData traceData;
void cfg::dump_trace()
{
	log::err("Paths with uncertain content types:");
	lockguard g(traceData);
	for (const auto& s : traceData)
		log::err(s);
}
tTraceData& tTraceData::getInstance()
{
	return traceData;
}

/*
 * Unlike the regular store-and-forward file item handler, this ones does not store anything to
 * harddisk. Instead, it uses the download buffer and lets job object send the data straight from
 * it to the client.
 *
 * The header is put in its original raw format aside, and is reprocessed again by the job while filtering the offending header values ONLY.
 */
class tPassThroughFitem : public fileitem
{
protected:
	evbuffer* m_q;

public:
	tPassThroughFitem(std::string s) : fileitem(s), m_q(evbuffer_new())
	{
		LOGSTARTFUNC;
		if(!m_q)
			throw std::bad_alloc();
		m_nSizeChecked = m_nSizeCachedInitial = -1;
	};
	~tPassThroughFitem()
	{
		evbuffer_free(m_q);
	}
	virtual FiStatus Setup() override
	{
		//m_nSizeChecked = m_nSizeCachedInitial = 0;
		return m_status = FIST_INITED;
	}

	string m_sHeader;
	const std::string& GetRawResponseHeader() override { return m_sHeader; }

	void DlFinish(bool) override
	{
		LOGSTARTFUNC;
		ASSERT(m_obj_mutex.try_lock() == false);

		notifyAll();
		m_status = FIST_COMPLETE;
	}

	unique_fd GetFileFd() override { return unique_fd(); }; // something, don't care for now

	bool DlAddData(string_view chunk, lockuniq& uli) override
	{
		LOGSTARTFUNCx(chunk.size(), m_status);
		ASSERT(m_obj_mutex.try_lock() == false);

		// something might care, most likely... also about BOUNCE action
		notifyAll();

		if (m_status < FIST_DLRECEIVING)
		{
			m_status = FIST_DLRECEIVING;
			m_nSizeChecked = 0;
		}

		m_nIncommingCount += chunk.size();
		m_nSizeChecked += chunk.size();

		dbgline;
		if (m_status > fileitem::FIST_COMPLETE || m_status < FIST_DLGOTHEAD)
			return false;

		dbgline;
		m_status = FIST_DLRECEIVING;
		while (!chunk.empty())
		{
			// abandoned by the user?
			if (m_status >= FIST_DLERROR)
				LOGRET(false);
			if (evabase::in_shutdown)
				LOGRET(false);
			auto in_buffer = evbuffer_get_length(m_q);
			off_t nAddLimit = PT_BUF_MAX - in_buffer;
			auto nToAppend = std::min(nAddLimit, off_t(chunk.size()));
			if (0 == nToAppend)
			{
				wait_for(uli, 5, 400);
				continue;
			}
			LOG("appending " << nToAppend << " to queue");
			bool failed = evbuffer_add(m_q, chunk.data(), nToAppend);
			if (failed)
				LOGRET(false);
			chunk.remove_prefix(nToAppend);
		}

		LOGRET(true);
	}

	ssize_t SendData(int out_fd, int, off_t &nSendPos, size_t nMax2SendNow) override
	{
		LOGSTARTFUNC;
		lockuniq g(this);
		notifyAll();
		if (m_status > FIST_COMPLETE || evabase::in_shutdown)
			return -1;
		return evbuffer_dumpall(m_q, out_fd, nSendPos, nMax2SendNow);
	}

	// fileitem interface
protected:
	bool DlStarted(string_view rawHeader, const tHttpDate &, cmstring &origin, tRemoteStatus status, off_t seekPos, off_t bytesAnnounced) override
	{
		LOGSTARTFUNC;
		ASSERT(m_obj_mutex.try_lock() == false);
		if (m_status > FIST_COMPLETE)
			return false;
		else if (m_status < FIST_DLGOTHEAD)
			m_status = FIST_DLGOTHEAD;
		else if (m_nSizeChecked > 0 && m_nSizeChecked != seekPos)
			return false;
		else if (m_nSizeChecked <=0 && seekPos > 0)
			return false;

		m_sHeader = rawHeader;
		m_responseOrigin = origin;
		m_responseStatus = status;
		m_nContentLength = bytesAnnounced;
		return true;
	}
};

// base class for a fileitem replacement with custom storage for local data
class tGeneratedFitemBase : public fileitem
{
public:
	unique_fd GetFileFd() override { return unique_fd(); }; // something, don't care for now

	tSS m_data;

	tGeneratedFitemBase(const string &sFitemId, tRemoteStatus status, cmstring& origUrl = sEmptyString)
		: fileitem(sFitemId), m_data(256)
	{
		m_status=FIST_DLRECEIVING;
		m_responseOrigin = origUrl;
		m_responseStatus = status;
		m_contentType = "text/html";
	}
	ssize_t SendData(int out_fd, int, off_t &nSendPos, size_t nMax2SendNow)
	override
	{
		if (AC_UNLIKELY(m_status > FIST_COMPLETE || out_fd < 0))
		{
			errno = EBADFD;
			return -1;
		}
		if (AC_UNLIKELY(nMax2SendNow > m_data.size()))
		{
			errno = EOVERFLOW;
			return -1;
		}
		auto ret = m_data.dumpall(out_fd, nMax2SendNow);
		if (AC_LIKELY(ret > 0))
			nSendPos += ret;
		return ret;
	}
	inline void seal()
	{
		// finish the building and seal the item
		m_nSizeChecked = m_data.size();
		m_nContentLength = m_nSizeChecked;
		m_status = FIST_COMPLETE;
	}
};

static const string miscError(" [HTTP error, code: ");

job::job(ISharedConnectionResources &pParent) : m_pParentCon(pParent), m_eMaintWorkType(tSpecialRequest::workNotSpecial)
{

}

job::~job()
{
	LOGSTART("job::~job");

	// shall have sent ANYTHING in response
	ASSERT(m_nAllDataCount != 0);

	int stcode = 200;
	off_t inCount = 0;
	if (m_pItem.get())
	{
		lockguard g(* m_pItem.get());
		stcode = m_pItem.get()->m_responseStatus.code;
		inCount = m_pItem.get()->TakeTransferCount();
	}

	bool bErr = m_sFileLoc.empty() || stcode >= 400;

	m_pParentCon.LogDataCounts(
				m_sFileLoc + (bErr ? (miscError + ltos(stcode) + ']') : sEmptyString),
				move(m_xff), inCount,
				m_nAllDataCount, bErr);
}


inline void job::PrepareLocalDownload(const string &visPath,
									  const string &fsBase, const string &fsSubpath)
{
	mstring absPath = fsBase+SZPATHSEP+fsSubpath;
	Cstat stbuf(absPath);
	if (!stbuf)
	{
		switch(errno)
		{
		case EACCES:
			SetEarlySimpleResponse("403 Permission denied");
			break;
		case EBADF:
		case EFAULT:
		case ENOMEM:
		case EOVERFLOW:
		default:
			SetEarlySimpleResponse("500 Internal server error");
			break;
		case ELOOP:
			SetEarlySimpleResponse("500 Infinite link recursion");
			break;
		case ENAMETOOLONG:
			SetEarlySimpleResponse("500 File name too long");
			break;
		case ENOENT:
		case ENOTDIR:
			SetEarlySimpleResponse("404, File or directory not found");
			break;
		}
		return;
	}

	if(S_ISDIR(stbuf.st_mode))
	{
		// unconfuse the browser
		if(!endsWithSzAr(visPath, SZPATHSEPUNIX))
		{
			class dirredirect : public tGeneratedFitemBase
			{
			public:	dirredirect(const string &visPath)
					: tGeneratedFitemBase(visPath, {301, "Moved Permanently"}, visPath + "/")
				{
					m_data << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>301 Moved Permanently</title></head><body><h1>Moved Permanently</h1>"
					"<p>The document has moved <a href=\""+visPath+"/\">here</a>.</p></body></html>";
					seal();
				}
			};
			m_pItem = m_pParentCon.GetItemRegistry()->Create(make_shared<dirredirect>(visPath), false);
			return;
		}

		class listing: public tGeneratedFitemBase
		{
		public:
			listing(const string &visPath) :
				tGeneratedFitemBase(visPath, {200, "OK"})
			{
				seal(); // for now...
			}
		};
		auto p = make_shared<listing>(visPath);
		m_pItem = m_pParentCon.GetItemRegistry()->Create(p, false);
		tSS & page = p->m_data;

		page << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>Index of "
				<< visPath << "</title></head>"
		"<body><h1>Index of " << visPath << "</h1>"
		"<table><tr><th>&nbsp;</th><th>Name</th><th>Last modified</th><th>Size</th></tr>"
		"<tr><th colspan=\"4\"><hr></th></tr>";

		DIR *dir = opendir(absPath.c_str());
		if (!dir) // weird, whatever... ignore...
			page<<"ERROR READING DIRECTORY";
		else
		{
			// quick hack with sorting by custom keys, good enough here
			priority_queue<tStrPair, std::vector<tStrPair>, std::greater<tStrPair>> sortHeap;
			for(struct dirent *pdp(0);0!=(pdp=readdir(dir));)
			{
				if (0!=::stat(mstring(absPath+SZPATHSEP+pdp->d_name).c_str(), &stbuf))
					continue;

				bool bDir=S_ISDIR(stbuf.st_mode);

				char datestr[32]={0};
				struct tm tmtimebuf;
				strftime(datestr, sizeof(datestr)-1,
						 "%d-%b-%Y %H:%M", localtime_r(&stbuf.st_mtime, &tmtimebuf));

				string line;
				if(bDir)
					line += "[DIR]";
				else if(startsWithSz(cfg::GetMimeType(pdp->d_name), "image/"))
					line += "[IMG]";
				else
					line += "[&nbsp;&nbsp;&nbsp;]";
				line += string("</td><td><a href=\"") + pdp->d_name
						+ (bDir? "/\">" : "\">" )
						+ pdp->d_name
						+"</a></td><td>"
						+ datestr
						+ "</td><td align=\"right\">"
						+ (bDir ? string("-") : offttosH(stbuf.st_size));
				sortHeap.push(make_pair(string(bDir?"a":"b")+pdp->d_name, line));
				//dbgprint((mstring)line);
			}
			closedir(dir);
			while(!sortHeap.empty())
			{
				page.add(WITHLEN("<tr><td valign=\"top\">"));
				page << sortHeap.top().second;
				page.add(WITHLEN("</td></tr>\r\n"));
				sortHeap.pop();
			}

		}
		page << "<tr><td colspan=\"4\">" <<GetFooter();
		page << "</td></tr></table></body></html>";
		p->seal();
		return;
	}
	if(!S_ISREG(stbuf.st_mode))
	{
		SetEarlySimpleResponse("403 Unsupported data type");
		return;
	}
	/*
	 * This variant of file item handler sends a local file. The
	 * header data is generated as needed, the relative cache path variable
	 * is reused for the real path.
	 */
	class tLocalGetFitem : public fileitem_with_storage
	{
	public:
		tLocalGetFitem(string sLocalPath, struct stat &stdata) : fileitem_with_storage(sLocalPath)
		{
			m_status=FIST_COMPLETE;
			m_nSizeChecked=m_nSizeCachedInitial=stdata.st_size;
			m_spattr.bVolatile=false;
			m_responseStatus = { 200, "OK"};
			m_nContentLength = m_nSizeChecked = stdata.st_size;
			m_responseModDate = tHttpDate(stdata.st_mtim.tv_sec);
			cmstring &sMimeType=cfg::GetMimeType(sLocalPath);
			if(!sMimeType.empty())
				m_contentType = sMimeType;
		};
		unique_fd GetFileFd() override
		{
			int fd=open(m_sPathRel.c_str(), O_RDONLY);
#ifdef HAVE_FADVISE
			// optional, experimental
			if(fd>=0)
				posix_fadvise(fd, 0, m_nSizeChecked, POSIX_FADV_SEQUENTIAL);
#endif
			return unique_fd(fd);
		}
	};
	m_pItem = m_pParentCon.GetItemRegistry()->Create(make_shared<tLocalGetFitem>(absPath, stbuf), false);
}

inline bool job::ParseRange(const header& h)
{

	/*
	 * Range: bytes=453291-
	 * ...
	 * Content-Length: 7271829
	 * Content-Range: bytes 453291-7725119/7725120
	 */

	const char *pRange = h.h[header::RANGE];
	// working around a bug in old curl versions
	if (!pRange)
		pRange = h.h[header::CONTENT_RANGE];
	if (pRange)
	{
		int nRangeItems = sscanf(pRange, "bytes=" OFF_T_FMT
								 "-" OFF_T_FMT, &m_nReqRangeFrom, &m_nReqRangeTo);
		// working around bad (old curl style) requests
		if (nRangeItems <= 0)
		{
			nRangeItems = sscanf(pRange, "bytes "
			OFF_T_FMT "-" OFF_T_FMT, &m_nReqRangeFrom, &m_nReqRangeTo);
		}

		if (nRangeItems < 1) // weird...
			m_nReqRangeFrom = m_nReqRangeTo = -2;
		else
			return true;
	}
	return false;
}

void job::Prepare(const header &h, string_view headBuf, cmstring& callerHostname) {

	LOGSTARTFUNC;

#ifdef DEBUGLOCAL
	cfg::localdirs["stuff"]="/tmp/stuff";
	log::dbg(m_pReqHead->ToString());
#endif

	string sReqPath, sPathResidual;
	tHttpUrl theUrl; // parsed URL

	fileitem::FiStatus fistate(fileitem::FIST_FRESH);
	bool bPtMode(false);

	if (h.h[header::XFORWARDEDFOR])
	{
		m_xff = h.h[header::XFORWARDEDFOR];
	}

	// some macros, to avoid goto style
	auto report_invport = [this]()
	{
		SetEarlySimpleResponse("403 Configuration error (confusing proxy mode) or prohibited port (see AllowUserPorts)"sv);
	};
	auto report_overload = [this](int line)
	{
		USRDBG("overload error, line " << line);
		SetEarlySimpleResponse("503 Server overload, try later"sv);
	};
	auto report_badcache = [this]()
	{
		SetEarlySimpleResponse("503 Error with cache data, please consult apt-cacher.err"sv);
	};
	auto report_invpath = [this]()
	{
		SetEarlySimpleResponse("403 Invalid path specification"sv);
	};
	auto report_notallowed = [this]()
	{
		SetEarlySimpleResponse("403 Forbidden file type or location"sv);
	};

	if(h.type!=header::GET)
	{
		m_bIsHeadOnly = h.type == header::eHeadType::HEAD;
		if(!m_bIsHeadOnly)
			return report_invpath();
	}

	UrlUnescapeAppend(h.getRequestUrl(), sReqPath);

	m_bIsHttp11 = h.proto == header::HTTP_11;

	USRDBG( "Decoded request URI: " << sReqPath);

	if(h.h[header::CONNECTION])
	{
		if (0 == strncasecmp(h.h[header::CONNECTION], WITHLEN("close")))
			m_keepAlive = CLOSE;
		if (0 == strncasecmp(h.h[header::CONNECTION], WITHLEN("Keep-Alive")))
			m_keepAlive = KEEP;
	}

	constexpr string_view fname = "/_actmp";

	// "clever" file system browsing attempt?
	if(rex::Match(sReqPath, rex::NASTY_PATH)
	   || stmiss != sReqPath.find(fname.data(), 0, fname.size())
	   || startsWithSz(sReqPath, "/_"))
	{
		LOG("ERROR: internal path or FS break-out attempt");
		return report_notallowed();
	}

	try
	{
		if (startsWithSz(sReqPath, "/HTTPS///"))
			sReqPath.replace(0, 6, PROT_PFX_HTTPS);
		// special case: proxy-mode AND special prefix are there
		if(0==strncasecmp(sReqPath.c_str(), WITHLEN("http://https///")))
			sReqPath.replace(0, 13, PROT_PFX_HTTPS);

		if(!theUrl.SetHttpUrl(sReqPath, false))
		{
			m_eMaintWorkType=tSpecialRequest::workUSERINFO;
			LOG("work type: USERINFO");
			return;
		}
		LOG("refined path: " << theUrl.sPath << "\n on host: " << theUrl.sHost);

		// extract the actual port from the URL
		unsigned nPort = theUrl.GetPort();

		if(cfg::pUserPorts)
		{
			if(!cfg::pUserPorts->test(nPort))
				return report_invport();
		}
		else if(nPort != 80)
			return report_invport();

		// kill multiple slashes
		for(tStrPos pos=0; stmiss != (pos = theUrl.sPath.find("//", pos, 2)); )
			theUrl.sPath.erase(pos, 1);

		bPtMode=rex::MatchUncacheable(theUrl.ToURI(false), rex::NOCACHE_REQ);

		LOG("input uri: "<<theUrl.ToURI(false)<<" , dontcache-flag? " << bPtMode
			<< ", admin-page: " << cfg::reportpage);

		if(!cfg::reportpage.empty() || theUrl.sHost == "style.css")
		{
			m_eMaintWorkType = tSpecialRequest::DispatchMaintWork(sReqPath,
																  h.h[header::AUTHORIZATION]);
			if(m_eMaintWorkType != tSpecialRequest::workNotSpecial)
			{
				m_sFileLoc = sReqPath;
				return;
			}
		}

		using namespace rex;

		{
			auto it = cfg::localdirs.find(theUrl.sHost);
			if (it != cfg::localdirs.end())
			{
				PrepareLocalDownload(sReqPath, it->second, theUrl.sPath);
				ParseRange(h);
				return;
			}
		}

		auto data_type = rex::eMatchType::FILE_INVALID;

		if (h.h[header::CACHE_CONTROL])
		{
			for (auto p = h.h[header::CACHE_CONTROL]; !!(p = strstr(p, "no")) ; p+=2)
			{
				// as per https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Cache-Control :
				// force the cache data to be validated
				if (0 == strcasecmp(p+2, "-cache"))
				{
					data_type = rex::eMatchType::FILE_VOLATILE;
				}
				else if (0 == strcasecmp(p+2, "-store"))
				{
					// the behavior should be basically the same, including considering cached files first
					bPtMode = true;
				}
			}
		}

		// we can proxy the directory requests, but only if they are identifiable as directories
		// (path ends in /) and are not matched as local directory server above and
		// the special acngfs hack is not detected
		if(endsWithSzAr(theUrl.sPath, "/"))
		{
			//isDir = true;
			if(!h.h[header::XORIG])
			{
#if 0


				m_type = GetFiletype(theUrl.sPath);
				// it's still a directory, can assume it to be scanable with volatile contents
				if(m_type == FILE_INVALID)
					m_type = FILE_VOLATILE;
#else
				data_type = FILE_VOLATILE;
				bPtMode=true;
#endif
			}
			if (data_type == FILE_INVALID)
			{
				LOG("generic user information page for " << theUrl.sPath);
				m_eMaintWorkType = tSpecialRequest::workUSERINFO;
				return;
			}
		}

		// in PT mode we don't care about how to handle it, it's what user wants to do
		if(data_type == FILE_INVALID && bPtMode)
			data_type = FILE_SOLID;

		if(data_type == FILE_INVALID)
			data_type = GetFiletype(theUrl.sPath);

		if ( data_type == FILE_INVALID )
		{
			if(!cfg::patrace)
			{
				// just for the log
				m_sFileLoc = theUrl.sPath;
				return report_notallowed();
			}

			// ok, collect some information helpful to the user
			data_type = FILE_VOLATILE;
			lockguard g(traceData);
			traceData.insert(theUrl.sPath);
		}
		
		// got something valid, has type now, trace it
		USRDBG("Processing new job, " << h.getRequestUrl());
		auto repoSrc = remotedb::GetInstance().GetRepNameAndPathResidual(theUrl);
		if(repoSrc.psRepoName && !repoSrc.psRepoName->empty())
			m_sFileLoc = *repoSrc.psRepoName + SZPATHSEP + repoSrc.sRestPath;
		else
			m_sFileLoc=theUrl.sHost+theUrl.sPath;

		fileitem::tSpecialPurposeAttr attr {
			! cfg::offlinemode && data_type == FILE_VOLATILE,
					m_bIsHeadOnly,
					false,
					m_nReqRangeTo,
					""
        };

		m_ifMoSince = tHttpDate(h.h[header::IF_MODIFIED_SINCE]);

		ParseRange(h);

		m_pItem = m_pParentCon.GetItemRegistry()->Create(m_sFileLoc,
														 attr.bVolatile ?
															 ESharingHow::AUTO_MOVE_OUT_OF_THE_WAY :
															 ESharingHow::ALWAYS_TRY_SHARING, attr);
		if( ! m_pItem.get())
		{
			USRERR("Error creating file item for " << m_sFileLoc << " -- check file permissions!");
			return report_badcache();
		}

		if(cfg::DegradedMode())
			return SetEarlySimpleResponse("403 Cache server in degraded mode");

		fistate = m_pItem.get()->Setup();
		LOG("Got initial file status: " << (int) fistate);

		if (bPtMode && fistate != fileitem::FIST_COMPLETE)
			fistate = _SwitchToPtItem();

		// might need to update the filestamp because nothing else would trigger it
		if(cfg::trackfileuse && fistate >= fileitem::FIST_DLGOTHEAD && fistate < fileitem::FIST_DLERROR)
			m_pItem.get()->UpdateHeadTimestamp();

		if(fistate==fileitem::FIST_COMPLETE)
			return; // perfect, done here

		if(cfg::offlinemode) { // make sure there will be no problems later in SendData or prepare a user message
			// error or needs download but freshness check was disabled, so it's really not complete.
			return SetEarlySimpleResponse("503 Unable to download in offline mode");
		}
		dbgline;
		if( fistate < fileitem::FIST_DLGOTHEAD) // needs a downloader
		{
			dbgline;
			if(!m_pParentCon.SetupDownloader())
			{
				USRDBG( "Error creating download handler for "<<m_sFileLoc);
				return report_overload(__LINE__);
			}

			dbgline;

			auto bHaveBackends = (repoSrc.repodata && !repoSrc.repodata->m_backends.empty());

			if (cfg::forcemanaged && !bHaveBackends)
				return report_notallowed();

			if (!bPtMode)
			{
				// XXX: this only checks the first found backend server, what about others?
				auto testUri = bHaveBackends ?
							repoSrc.repodata->m_backends.front().ToURI(
								false) + repoSrc.sRestPath :
							theUrl.ToURI(false);
				if (rex::MatchUncacheable(testUri, rex::NOCACHE_TGT))
					_SwitchToPtItem();
			}
			// if backend config not valid, download straight from the specified source

			string extraHeaders;
			if (cfg::exporigin && !callerHostname.empty())
			{
				extraHeaders = "X-Forwarded-For: "sv;
				if (!m_xff.empty())
				{
					extraHeaders += m_xff;
					extraHeaders += ", "sv;
				}
				extraHeaders += callerHostname;
				extraHeaders += svRN;
			}
			if (bPtMode && headBuf.length() > 0)
				extraHeaders += header::ExtractCustomHeaders(headBuf, bPtMode);

			if (bHaveBackends
					? m_pParentCon.SetupDownloader()->AddJob(m_pItem.get(), move(repoSrc), bPtMode, move(extraHeaders))
					: m_pParentCon.SetupDownloader()->AddJob(m_pItem.get(), move(theUrl), bPtMode, move(extraHeaders)))
			{
				ldbg("Download job enqueued for " << m_sFileLoc);
			}
			else
			{
				USRERR("PANIC! Error creating download job for " << m_sFileLoc);
				return report_overload(__LINE__);
			}
		}
	}
	catch (const std::bad_alloc&) // OOM, may this ever happen here?
	{
		USRDBG("Out of memory");
		return report_overload(__LINE__);
	}
	catch(const std::out_of_range&) // better safe...
	{
		return report_invpath();
	}
}

job::eJobResult job::SendData(int confd, bool haveMoreJobs)
{
	LOGSTARTFUNCx(m_activity, confd, haveMoreJobs);

	// use this return helper for better tracking, actually the caller should never return
	auto return_discon = [&]()
	{
		LOG("EXPLICIT DISCONNECT");
		m_activity = STATE_DISCO_ASAP;
		return R_DISCON;
	};
	auto return_stream_ok = [&]()
	{
		LOG("CLEAN JOB FINISH");
		if(m_keepAlive == KEEP)
			return m_activity = STATE_DONE, R_DONE;
		if(m_keepAlive == CLOSE)
		{
			return return_discon();
		}
		// unspecified?
		if (m_bIsHttp11)
			return m_activity = STATE_DONE, R_DONE;
		return return_discon();
	};

	if (confd < 0)
	{
		return return_discon(); // shouldn't be here
	}
	if (m_eMaintWorkType != tSpecialRequest::eMaintWorkType::workNotSpecial)
	{
		tSpecialRequest::RunMaintWork((tSpecialRequest::eMaintWorkType) m_eMaintWorkType, m_sFileLoc, confd, &m_pParentCon);
		return return_discon(); // just stop and close connection
	}

	if (!m_sendbuf.empty())
	{
		ldbg("prebuf sending: "<< m_sendbuf.c_str());
		auto r = send(confd, m_sendbuf.rptr(), m_sendbuf.size(),
					  MSG_MORE * (m_activity == STATE_NOT_STARTED || m_activity == STATE_SEND_CHUNK_DATA || haveMoreJobs));

		if (r == -1)
		{
			if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)
				return R_AGAIN;
			return return_discon();
		}
		m_nAllDataCount += r;
		m_sendbuf.drop(r);
		if (!m_sendbuf.empty())
		{
			return R_AGAIN;
		}
	}

	off_t nBodySizeSoFar(0);
	fileitem::FiStatus fistate(fileitem::FIST_DLERROR);

	auto fi = m_pItem.get();

	/**
	 * this is left only by a) item state mutating to error (returns false) or b) enough data becomes available (returns true)
	 */
	auto AwaitSendableState = [&]()
	{
		dbgline;
		if (!fi)
			return false;

		// make sure to collect enough data to continue
		lockuniq g(*fi);
		while(true)
		{
			dbgline;
			fistate = fi->GetStatusUnlocked(nBodySizeSoFar);
			if (fistate >= fileitem::FIST_COMPLETE)
				break;
			dbgline;
			if (nBodySizeSoFar > m_nSendPos)
				break;
			if (m_bIsHeadOnly && fistate >= fileitem::FIST_DLGOTHEAD)
				break;
			// XXX: in 2023 or later, add a 5s timeout and send a 102 or so for waiting. Because older version of apt-cacher-ng might not understand it and fail.
			bool timedOut = fi->wait_for(g, cfg::nettimeout, 1);
			if (timedOut)
				return false;
		}
		LOG(int(fistate));
		return fistate <= fileitem::FIST_COMPLETE;
	};
	LOG(int(m_activity));
	switch (m_activity)
	{
	case (STATE_DONE):
	{
		return return_stream_ok();
	}
	case (STATE_NOT_STARTED):
	{
		if (!AwaitSendableState())
			return HandleSuddenError();

		dbgline;
		CookResponseHeader();
		// prebuf was filled OR state was changed -> come back to send header and continue with the selected activity
		return R_AGAIN;
	}
	case (STATE_SEND_BUF_NOT_FITEM):
	{
		// got here when our head and optional data was send via sendbuf
		return return_stream_ok();
		break;
	}
	case (STATE_DISCO_ASAP):
	{
		return R_DISCON;
		break;
	}
	case (STATE_SEND_DATA):
	{
		if (m_bIsHeadOnly) // there is no data!
			return return_stream_ok();

		if (!AwaitSendableState())
			return HandleSuddenError();
		if (!m_filefd.valid())
			m_filefd.reset(fi->GetFileFd());
		// It's invalid for pt-jobs and it's okay
		// if (!m_filefd.valid())
		//	return HandleSuddenError();
		auto limit = nBodySizeSoFar - m_nSendPos;
		if (m_nReqRangeTo >= 0)
			limit = min(m_nReqRangeTo + 1 - m_nSendPos, limit);
		if (limit <= 0)
			return R_DISCON;
		ldbg("~senddata: to " << nBodySizeSoFar << ", OLD m_nSendPos: " << m_nSendPos);
		int n = fi->SendData(confd, m_filefd.get(), m_nSendPos, limit);
		ldbg("~senddata: " << n << " new m_nSendPos: " << m_nSendPos);
		if (n < 0)
			return return_discon();
		m_nAllDataCount += n;
		if (fistate == fileitem::FIST_COMPLETE && m_nSendPos == nBodySizeSoFar)
			return return_stream_ok();
		else if (m_nReqRangeTo >= 0 && m_nSendPos >= m_nReqRangeTo + 1)
			return return_stream_ok();
		return R_AGAIN;
	}
	case (STATE_SEND_CHUNK_HEADER):
	{
		if (!AwaitSendableState())
			return HandleSuddenError();

		if (fistate == fileitem::FIST_COMPLETE && m_nSendPos == nBodySizeSoFar)
		{
			m_sendbuf << "0\r\n\r\n";
			m_activity = STATE_DONE;
			return R_AGAIN;
		}
		m_filefd.reset(fi->GetFileFd());
		if (!m_filefd.valid())
			return HandleSuddenError();

		m_nChunkEnd = nBodySizeSoFar;
		m_sendbuf << tSS::hex << (m_nChunkEnd - m_nSendPos) << "\r\n";
		m_activity = STATE_SEND_CHUNK_DATA;
		return R_AGAIN;
	}
	case (STATE_SEND_CHUNK_DATA):
	{
		// this is only entered after STATE_SEND_CHUNK_HEADER
		int n = fi->SendData(confd, m_filefd.get(), m_nSendPos, m_nChunkEnd - m_nSendPos);
		ldbg("~sendchunk: " << n << " new m_nSendPos: " << m_nSendPos);
		if (n < 0)
			return HandleSuddenError();
		m_nAllDataCount += n;
		if (m_nSendPos == m_nChunkEnd)
		{
			m_sendbuf << svRN;
			m_activity = STATE_SEND_CHUNK_HEADER;
		}
		return R_AGAIN;
	}
	}
	return return_discon();
}

inline void job::AddPtHeader(cmstring& remoteHead)
{
	const static std::string dummyTE("\nX-No-Trans-Encode:"), badTE("\nTransfer-Encoding:");
	auto szHeadBegin = remoteHead.c_str();
	// don't care about its contents, with exception of chunked transfer-encoding
	// since it's too messy to support, use the plain close-on-end strategy here
	auto szTEHeader = strcasestr(szHeadBegin, badTE.c_str());
	if(szTEHeader == nullptr)
		m_sendbuf << remoteHead;
	else
	{
		m_keepAlive = CLOSE;
		m_sendbuf.add(szHeadBegin, szTEHeader - szHeadBegin);
		// as long as the te string
		m_sendbuf<<dummyTE;
		auto szRest = szTEHeader + badTE.length();
		m_sendbuf.add(szRest, szHeadBegin + remoteHead.length() - szRest);
	}
	if(strcasestr(szHeadBegin, "Connection: close\r\n"))
		m_keepAlive = CLOSE;
}

job::eJobResult job::HandleSuddenError()
{
	LOGSTARTFUNC;
	// response ongoing, can only reject the client now?
	if (m_nAllDataCount)
	{
		m_activity = STATE_DISCO_ASAP;
		return R_DISCON;
	}
	m_activity = STATE_SEND_BUF_NOT_FITEM;

	if (!m_pItem.get())
		return SetEarlySimpleResponse("500 Error creating cache object"), R_AGAIN;

	tRemoteStatus st;
	{
		lockguard g(*m_pItem.get());
		st = m_pItem.get()->m_responseStatus;
	}

	// do we have anything harmles useful to tell the user here?
	if (st.code < 500) // that's too strange, this should not cause a fatal error in item, report something generic instead
		SetEarlySimpleResponse("500 Remote or cache error");
	else
		SetEarlySimpleResponse(ltos(st.code) + " " + (st.msg.empty() ? "Unknown internal error" : st.msg));
	return R_AGAIN;
}

inline void job::CookResponseHeader()
{
	LOGSTARTFUNC;

	// the default continuation unless changed
	m_activity = STATE_SEND_DATA;

	auto quickResponse = [this] (string_view msg, bool nobody = false, decltype(m_activity) nextActivity = STATE_SEND_BUF_NOT_FITEM)
	{
		SetEarlySimpleResponse(msg, nobody);
		m_activity = nextActivity;
	};
	auto fi = m_pItem.get();
	if (!fi)
		return quickResponse("500 Invalid cache object", false, STATE_DISCO_ASAP);

	lockguard g(*fi);

	auto& remoteHead = fi->GetRawResponseHeader();
	if(!remoteHead.empty())
		return AddPtHeader(remoteHead);

	m_sendbuf.clean() << tSS::dec;

	// we might need to massage this before responding
	auto status = fi->m_responseStatus;
	off_t ds;
	auto fist = fi->GetStatusUnlocked(ds);

	auto addStatusLineFromItem = [&] ()
	{
		return PrependHttpVariant() << status.code << " " << status.msg << svRN;
	};
	auto isRedir = status.isRedirect();
	if (isRedir || status.mustNotHaveBody())
	{
		addStatusLineFromItem();
		if (isRedir)
			m_sendbuf << "Location: " << fi->m_responseOrigin << svRN;
		m_activity = STATE_SEND_BUF_NOT_FITEM;
		return AppendMetaHeaders();
	}
	// everything else is either an error, or not-modified, or must have content
	if (status.code != 200)
	{
		return quickResponse(ltos(status.code) + " " + status.msg);
	}

	if (m_ifMoSince.isSet() && fi->m_responseModDate.isSet() && m_ifMoSince >= fi->m_responseModDate)
	{
		return quickResponse("304 Not Modified", true);
	}
	auto src = fi->m_responseOrigin.empty() ? "" : (string("X-Original-Source: ") + fi->m_responseOrigin + "\r\n");

	// detect special condition where we need chunked transfer - no optimizations
	// possible or considered here (because too nasty to track later errors,
	// better just resend it, this is a rare case anyway)
	auto contLen = fi->m_nContentLength;
#ifdef FORCE_CHUNKED
#warning FORCE_CHUNKED active!
	auto goChunked = true;
#else
	auto goChunked =
			contLen < 0 && fist == fileitem::FIST_DLRECEIVING
					// except for when only a range is wanted and that range is already available
					&& !(m_nReqRangeTo > 0 && ds >= m_nReqRangeTo);
#endif
	if (goChunked)
	{
		// set for full transfer in chunked mode
		m_activity = STATE_SEND_CHUNK_HEADER;
		m_nReqRangeTo = -1;
		m_nReqRangeFrom = 0;
		addStatusLineFromItem()
				<< "Content-Type: " << fi->m_contentType << svRN;
		if (fi->m_responseModDate.isSet() && ! fi->m_responseModDate.view().empty())
		{
			m_sendbuf << "Last-Modified: " << fi->m_responseModDate.view() << svRN;
		}
		m_sendbuf << "Transfer-Encoding: chunked\r\n" << src;
		AppendMetaHeaders();
		return;
	}
	// also check whether it's a sane range (if set)
	if (contLen >= 0 && (m_nReqRangeFrom >= contLen || m_nReqRangeTo + 1 > contLen))
		return quickResponse("416 Requested Range Not Satisfiable");
	// okay, date is good, no chunking needed, can serve a range request smoothly (beginning is available) or fall back to 200?
	if (m_nReqRangeFrom >= 0 && ds > m_nReqRangeFrom && contLen > 0)
	{
		m_nSendPos = m_nReqRangeFrom;
		auto cl = m_nReqRangeTo < 0 ? contLen - m_nReqRangeFrom : m_nReqRangeTo + 1 - m_nReqRangeFrom;
		auto last = m_nReqRangeTo > 0 ? m_nReqRangeTo : contLen - 1;
		PrependHttpVariant() << "206 Partial Content"sv << svRN
							 << "Content-Type: "sv << fi->m_contentType << svRN;
		if (fi->m_responseModDate.isSet() && ! fi->m_responseModDate.view().empty())
		{
			m_sendbuf << "Last-Modified: "sv << fi->m_responseModDate.view() << svRN;
		}
		m_sendbuf << "Content-Range: bytes "sv << m_nReqRangeFrom << "-" << last << "/" << contLen << svRN
				  << "Content-Length: "sv << cl << svRN
				  << src;
		AppendMetaHeaders();
		return;
	}
	// everything else is plain full-body response
	m_nReqRangeTo = -1;
	m_nReqRangeFrom = 0;
	PrependHttpVariant() << "200 OK" << svRN
						 << "Content-Type: " << fi->m_contentType << svRN;
	if (fi->m_responseModDate.isSet() && ! fi->m_responseModDate.view().empty())
	{
		m_sendbuf << "Last-Modified: " << fi->m_responseModDate.view() << svRN;
	}
	m_sendbuf << "Content-Length: " << contLen << svRN
			  << src;

	AppendMetaHeaders();
	return;
}

fileitem::FiStatus job::_SwitchToPtItem()
{
	// Changing to local pass-through file item
	LOGSTART("job::_SwitchToPtItem");
	// exception-safe sequence
	m_pItem = m_pParentCon.GetItemRegistry()->Create(make_shared<tPassThroughFitem>(m_sFileLoc), false);
	return m_pItem.get()->Setup();
}

tSS& job::PrependHttpVariant()
{
	return m_sendbuf << tSS::dec << (m_bIsHttp11 ? "HTTP/1.1 "sv : "HTTP/1.0 "sv);
}

void job::SetEarlySimpleResponse(string_view message, bool nobody)
{
	LOGSTARTFUNC
			m_sendbuf.clear();

	m_activity = STATE_SEND_BUF_NOT_FITEM;

	if (nobody)
	{
		PrependHttpVariant() << message << svRN;
		AppendMetaHeaders();
		return;
	}

	tSS body;

	body << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>" << message
		 << "</title>\n</head>\n<body><h1>" << message
		 << "</h1></body>" << GetFooter() << "</html>";

	PrependHttpVariant() << message
						 << "\r\nContent-Length: " << body.size()
						 << "\r\nContent-Type: text/html\r\n";
	AppendMetaHeaders();
	m_sendbuf << body;

}
void job::AppendMetaHeaders()
{
	if(m_keepAlive == KEEP)
		m_sendbuf << "Connection: Keep-Alive\r\n";
	else if(m_keepAlive == CLOSE)
		m_sendbuf << "Connection: close\r\n";
#ifdef DEBUG
	static atomic_int genHeadId(0);
	m_sendbuf << "X-Debug: " << int(genHeadId++) << svRN;
#endif
	/*
	if (contentType.empty() && m_pItem.get())
		contentType = m_pItem.get()->m_contentType;
	if (!contentType.empty())
		m_sendbuf << "Content-Type: " << contentType << "\r\n";
	*/
	m_sendbuf << "Date: " << tHttpDate(GetTime()).view()
			  << "\r\nServer: Debian Apt-Cacher NG/" ACVERSION "\r\n"
	"\r\n";
}
}
