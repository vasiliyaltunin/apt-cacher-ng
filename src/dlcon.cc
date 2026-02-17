//#include <netinet/in.h>
//#include <netdb.h>

#include "tcpconnect.h"
#include "lockable.h"
#include "fileitem.h"
#include "acfg.h"
#include "meta.h"
#include "remotedb.h"
#include "acbuf.h"

#include <unistd.h>
#include <sys/time.h>
#include <atomic>
#include <algorithm>
#include <list>
#include <regex>

#include "debug.h"

#include "config.h"
#include "acfg.h"
#include "dlcon.h"

#include "fileitem.h"
#include "fileio.h"
#include "sockio.h"
#include "evabase.h"

#ifdef HAVE_LINUX_EVENTFD
#include <sys/eventfd.h>
#endif

using namespace std;

// evil hack to simulate random disconnects
//#define DISCO_FAILURE

#define MAX_RETRY cfg::dlretriesmax

namespace acng
{

static cmstring sGenericError("502 Bad Gateway");

std::atomic_uint g_nDlCons(0);
class CDlConn;

struct tDlJob
{
	tFileItemPtr m_pStorage;
	mstring sErrorMsg;
	CDlConn &m_parent;

	enum EStreamState : uint8_t
	{
		STATE_GETHEADER,
		//STATE_REGETHEADER,
		STATE_PROCESS_DATA,
		STATE_GETCHUNKHEAD,
		STATE_PROCESS_CHUNKDATA,
		STATE_GET_CHUNKTRAILER,
		STATE_FINISHJOB
	}
	m_DlState = EStreamState::STATE_GETHEADER;

	enum class EResponseEval
	{
		GOOD, BUSY_OR_ERROR, RESTART_NEEDED
	};

	inline bool HasBrokenStorage()
	{
		return (!m_pStorage
				|| m_pStorage->GetStatus() > fileitem::FIST_COMPLETE);
	}

#define HINT_MORE 0
#define HINT_DONE 1
#define HINT_RECONNECT_NOW 2
#define EFLAG_JOB_BROKEN 4
#define EFLAG_MIRROR_BROKEN 8
#define EFLAG_STORE_COLLISION 16
#define HINT_SWITCH 32
#define EFLAG_LOST_CON 64
#define HINT_KILL_LAST_FILE 128
#define HINT_TGTCHANGE 256
#define HINT_RECONNECT_SOON 512

	const tRepoData *m_pRepoDesc = nullptr;

	/*!
	 * Returns a reference to http url where host and port and protocol match the current host
	 * Other fields in that member have undefined contents. ;-)
	 */
	inline const tHttpUrl& GetPeerHost()
	{
		return m_pCurBackend ? *m_pCurBackend : m_remoteUri;
	}

	inline tRepoUsageHooks* GetConnStateTracker()
	{
		return m_pRepoDesc ? m_pRepoDesc->m_pHooks : nullptr;
	}

	string m_extraHeaders;

	tHttpUrl m_remoteUri;
	const tHttpUrl *m_pCurBackend = nullptr;

	bool m_bBackendMode = false;
	// input parameters
	bool m_bIsPassThroughRequest = false;
	bool m_bAllowStoreData = true;
	bool m_bFileItemAssigned = false;

	int m_nRedirRemaining = cfg::redirmax;

    // most of those attributes are relevant to the downloader, keep a local snapshot of them
    fileitem::tSpecialPurposeAttr m_fiAttr;

	// state attribute
	off_t m_nRest = 0;

	// flag to use ranges and also define start if >= 0
	off_t m_nUsedRangeStartPos = -1;

	inline tDlJob(CDlConn *p, const tFileItemPtr& pFi, tHttpUrl &&src, bool isPT, mstring extraHeaders) :
					m_pStorage(pFi), m_parent(*p),
					m_extraHeaders(move(extraHeaders)),
					m_bIsPassThroughRequest(isPT)
	{
		LOGSTARTFUNC;
		ldbg("uri: " << src.ToURI(false));
		if (m_pStorage)
			m_pStorage->DlRefCountAdd();
		m_remoteUri = move(src);
        lockguard g(*pFi);
        m_fiAttr = pFi->m_spattr;
	}

	inline tDlJob(CDlConn *p, const tFileItemPtr& pFi, tRepoResolvResult &&repoSrc, bool isPT, mstring extraHeaders) :
					m_pStorage(pFi), m_parent(*p),
					m_extraHeaders(move(extraHeaders)),
					m_bIsPassThroughRequest(isPT)
	{
		LOGSTARTFUNC;
		ldbg("repo: " << uintptr_t(repoSrc.repodata) << ", restpath: " << repoSrc.sRestPath);
		if (m_pStorage)
			m_pStorage->DlRefCountAdd();
		m_remoteUri.sPath = move(repoSrc.sRestPath);
		m_pRepoDesc = move(repoSrc.repodata);
		m_bBackendMode = true;
		lockguard g(*pFi);
		m_fiAttr = pFi->m_spattr;
	}

	// Default move ctor is ok despite of pointers, we only need it in the beginning, list-splice operations should not move the object around
	tDlJob(tDlJob &&other) = default;

	~tDlJob()
	{
		LOGSTART("tDlJob::~tDlJob");
		if (m_pStorage)
		{
			dbgline;
            m_pStorage->DlRefCountDec({503, sErrorMsg.empty() ?
                    "Download Expired" : move(sErrorMsg)});
		}
	}

	void ResetStreamState()
	{
		m_nRest = 0;
		m_DlState = STATE_GETHEADER;
		m_nUsedRangeStartPos = -1;
	}

	inline string RemoteUri(bool bUrlEncoded)
	{
		if (m_pCurBackend)
		{
			return m_pCurBackend->ToURI(bUrlEncoded)
					+ (bUrlEncoded ?
							UrlEscape(m_remoteUri.sPath) : m_remoteUri.sPath);
		}
		return m_remoteUri.ToURI(bUrlEncoded);
	}

	inline bool RewriteSource(const char *pNewUrl)
	{
		LOGSTART("tDlJob::RewriteSource");
		if (--m_nRedirRemaining <= 0)
		{
            sErrorMsg = "Redirection loop";
			return false;
		}

		if (!pNewUrl || !*pNewUrl)
		{
            sErrorMsg = "Bad redirection";
			return false;
		}

		auto sLocationDecoded = UrlUnescape(pNewUrl);

		tHttpUrl newUri;
		auto is_abs_path = startsWithSz(sLocationDecoded, "/");
		auto is_url = is_abs_path ? false : newUri.SetHttpUrl(sLocationDecoded, false);

		// stop the backend probing in any case, then judge by type

		if (is_url)
		{
			m_bBackendMode = false;
			m_pCurBackend = nullptr;
			m_remoteUri = move(newUri);
			return true;
		}

		if (m_bBackendMode && !m_pCurBackend) {
			sErrorMsg = "Bad redirection target in backend mode";
			return false; // strange...
		}
			

		// take last backend as fixed target (base)
		if (m_bBackendMode) {
			// recreate actual download URL in fixed mode
			m_bBackendMode = false;
			if (is_abs_path) {
				m_remoteUri = move(*m_pCurBackend);
				m_remoteUri.sPath = sLocationDecoded;
				return true;
			}
			auto sfx = m_remoteUri.sPath;
			m_remoteUri = *m_pCurBackend;
			m_remoteUri.sPath += sfx;
		}
		else {
			// at the same host... use the absolute URL?
			if (is_abs_path)
			{
				m_remoteUri.sPath = sLocationDecoded;
				return true;
			}
		}

		// not absolute, not URL, construct the resource link around the previous base URL
		auto spos = m_remoteUri.sPath.find_last_of('/');
		if (spos == stmiss)
			m_remoteUri.sPath = '/';
		else
			m_remoteUri.sPath.erase(++spos);

		m_remoteUri.sPath += sLocationDecoded;
		return true;
	}

	bool SetupJobConfig(mstring &sReasonMsg,
			tStrMap &blacklist)
	{
		LOGSTART("CDlConn::SetupJobConfig");

		// using backends? Find one which is not blacklisted
		if (m_bBackendMode)
		{
			// keep the existing one if possible
			if (m_pCurBackend)
			{
				LOG(
						"Checking [" << m_pCurBackend->sHost << "]:" << m_pCurBackend->GetPort());
				const auto bliter = blacklist.find(m_pCurBackend->GetHostPortKey());
				if (bliter == blacklist.end())
					LOGRET(true);
			}

			// look in the constant list, either it's usable or it was blacklisted before
			for (const auto &bend : m_pRepoDesc->m_backends)
			{
				const auto bliter = blacklist.find(bend.GetHostPortKey());
				if (bliter == blacklist.end())
				{
					m_pCurBackend = &bend;
					LOGRET(true);
				}

				// uh, blacklisted, remember the last reason
				if (sReasonMsg.empty())
				{
					sReasonMsg = bliter->second;
					LOG(sReasonMsg);
				}
			}
			if (sReasonMsg.empty())
				sReasonMsg = "Mirror blocked due to repeated errors";
			LOGRET(false);
		}

		// ok, not backend mode. Check the mirror data (vs. blacklist)
		auto bliter = blacklist.find(GetPeerHost().GetHostPortKey());
		if (bliter == blacklist.end())
			LOGRET(true);

		sReasonMsg = bliter->second;
		LOGRET(false);
	}

	// needs connectedHost, blacklist, output buffer from the parent, proxy mode?
	inline void AppendRequest(tSS &head, const tHttpUrl *proxy)
	{
		LOGSTARTFUNC;

#define CRLF "\r\n"

        if (m_fiAttr.bHeadOnly)
		{
			head << "HEAD ";
			m_bAllowStoreData = false;
		}
		else
		{
			head << "GET ";
			m_bAllowStoreData = true;
		}

		if (proxy)
			head << RemoteUri(true);
		else // only absolute path without scheme
		{
			if (m_pCurBackend) // base dir from backend definition
				head << UrlEscape(m_pCurBackend->sPath);

			head << UrlEscape(m_remoteUri.sPath);
		}

		ldbg(RemoteUri(true));

		head << " HTTP/1.1" CRLF
				<< cfg::agentheader
				<< "Host: " << GetPeerHost().sHost << CRLF;

		if (proxy) // proxy stuff, and add authorization if there is any
		{
			ldbg("using proxy");
			if (!proxy->sUserPass.empty())
			{
				// could cache it in a static string but then again, this makes it too
				// easy for the attacker to extract from memory dump
				head << "Proxy-Authorization: Basic "
						<< EncodeBase64Auth(proxy->sUserPass) << CRLF;
			}
			// Proxy-Connection is a non-sensical copy of Connection but some proxy
			// might listen only to this one so better add it
			head << (cfg::persistoutgoing ?
							"Proxy-Connection: keep-alive" CRLF :
							"Proxy-Connection: close" CRLF);
		}

		const auto &pSourceHost = GetPeerHost();
		if (!pSourceHost.sUserPass.empty())
		{
			head << "Authorization: Basic "
					<< EncodeBase64Auth(pSourceHost.sUserPass) << CRLF;
		}

		m_nUsedRangeStartPos = -1;

        lockguard g(*m_pStorage);

		m_nUsedRangeStartPos = m_pStorage->m_nSizeChecked >= 0 ?
				m_pStorage->m_nSizeChecked : m_pStorage->m_nSizeCachedInitial;

		if (AC_UNLIKELY(m_nUsedRangeStartPos < -1))
			m_nUsedRangeStartPos = -1;

		/*
		 * Validate those before using them, with extra caution for ranges with date check
		 * on volatile files. Also make sure that Date checks are only used
		 * in combination with range request, otherwise it doesn't make sense.
		 */
        if (m_fiAttr.bVolatile)
		{
			if (cfg::vrangeops <= 0)
			{
				m_nUsedRangeStartPos = -1;
			}
            else if (m_nUsedRangeStartPos == m_pStorage->m_nContentLength
					&& m_nUsedRangeStartPos > 1)
			{
				m_nUsedRangeStartPos--; // the probe trick
			}

            if (!m_pStorage->m_responseModDate.isSet()) // date unusable but needed for volatile files?
				m_nUsedRangeStartPos = -1;
		}

		if (m_fiAttr.nRangeLimit >= 0)
		{
			if(m_nUsedRangeStartPos < 0)
			{
				m_fiAttr.nRangeLimit = 0;
			}
			else if(AC_UNLIKELY(m_fiAttr.nRangeLimit < m_nUsedRangeStartPos))
			{
				// must be BS, fetch the whole remainder!
				m_fiAttr.nRangeLimit = -1;
			}
		}

		if (m_nUsedRangeStartPos > 0)
		{
			if (m_pStorage->m_responseModDate.isSet())
				head << "If-Range: " << m_pStorage->m_responseModDate.view() << CRLF;
			head << "Range: bytes=" << m_nUsedRangeStartPos << "-";
            if (m_fiAttr.nRangeLimit > 0)
                head << m_fiAttr.nRangeLimit;
			head << CRLF;
		}

        if (m_pStorage->IsVolatile())
			head << "Cache-Control: " /*no-store,no-cache,*/ "max-age=0" CRLF;

		head << cfg::requestapx << m_extraHeaders
				<< "Accept: application/octet-stream" CRLF;
		if (!m_bIsPassThroughRequest)
		{
			head << "Accept-Encoding: identity" CRLF
					"Connection: " << (cfg::persistoutgoing ?
							"keep-alive" CRLF : "close" CRLF);

		}
		head << CRLF;
#ifdef SPAM
	//head.syswrite(2);
#endif

	}

	inline uint_fast8_t NewDataHandler(acbuf &inBuf)
	{
		LOGSTART("tDlJob::NewDataHandler");
		while (true)
		{
			off_t nToStore = min((off_t) inBuf.size(), m_nRest);
			if (0 == nToStore)
				break;

			if (m_bAllowStoreData)
			{
				ldbg("To store: " <<nToStore);
				lockuniq g(*m_pStorage);
				if (!m_pStorage->DlAddData(string_view(inBuf.rptr(), nToStore), g))
				{
                    sErrorMsg = "Cannot store";
					return HINT_RECONNECT_NOW | EFLAG_JOB_BROKEN;
				}
			}
			m_nRest -= nToStore;
			inBuf.drop(nToStore);
		}

		ldbg("Rest: " << m_nRest);

		if (m_nRest != 0)
			return HINT_MORE; // will come back

		m_DlState =
				(STATE_PROCESS_DATA == m_DlState) ?
						STATE_FINISHJOB : STATE_GETCHUNKHEAD;
		return HINT_SWITCH;
	}

	/*!
	 *
	 * Process new incoming data and write it down to disk or other receivers.
	 */
	unsigned ProcessIncomming(acbuf &inBuf, bool bOnlyRedirectionActivity)
	{
		LOGSTARTFUNC;

		if (AC_UNLIKELY(!m_pStorage))
		{
            sErrorMsg = "Bad cache item";
			return HINT_RECONNECT_NOW | EFLAG_JOB_BROKEN;
		}

		for (;;) // returned by explicit error (or get-more) return
		{
			ldbg("switch: " << (int)m_DlState);

			if (STATE_GETHEADER == m_DlState)
			{
				ldbg("STATE_GETHEADER");
				header h;
				if (inBuf.size() == 0)
					return HINT_MORE;

				dbgline;

                auto hDataLen = h.Load(inBuf.view());
				// XXX: find out why this was ever needed; actually we want the opposite,
				// download the contents now and store the reported file as XORIG for future
				// https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Content-Location
				if (0 == hDataLen)
					return HINT_MORE;
				if (hDataLen < 0)
				{
					sErrorMsg = "Invalid header";
					LOG(sErrorMsg);
					// can be followed by any junk... drop that mirror, previous file could also contain bad data
					return EFLAG_MIRROR_BROKEN | HINT_RECONNECT_NOW
							| HINT_KILL_LAST_FILE;
				}

				ldbg("contents: " << string_view(inBuf.rptr(), hDataLen));

				if (h.type != header::ANSWER)
				{
					dbgline;
                    sErrorMsg = "Unexpected response type";
					// smells fatal...
					return EFLAG_MIRROR_BROKEN | EFLAG_LOST_CON | HINT_RECONNECT_NOW;
				}
				dbgline;

				unsigned ret = 0;

				auto pCon = h.h[header::CONNECTION];
				if (!pCon)
					pCon = h.h[header::PROXY_CONNECTION];

				if (pCon && 0 == strcasecmp(pCon, "close"))
				{
					ldbg("Peer wants to close connection after request");
					ret |= HINT_RECONNECT_SOON;
				}

				// processing hint 102, or something like 103 which we can ignore
				if (h.getStatusCode() < 200)
				{
					inBuf.drop(size_t(hDataLen));
					return ret | HINT_MORE;
				}

				if (cfg::redirmax) // internal redirection might be disabled
				{
					if (h.getStatus().isRedirect())
					{
						if (!RewriteSource(h.h[header::LOCATION]))
							return ret | EFLAG_JOB_BROKEN;

						// drop the redirect page contents if possible so the outer loop
						// can scan other headers
						off_t contLen = atoofft(h.h[header::CONTENT_LENGTH], 0);
						if (contLen <= inBuf.size())
							inBuf.drop(contLen);
						return ret | HINT_TGTCHANGE; // no other flags, caller will evaluate the state
					}

					// for non-redirection responses process as usual

					// unless it's a probe run from the outer loop, in this case we
					// should go no further
					if (bOnlyRedirectionActivity)
						return ret | EFLAG_LOST_CON | HINT_RECONNECT_NOW;
				}

				// explicitly blacklist mirror if key file is missing
				if (h.getStatusCode() >= 400 && m_pRepoDesc && m_remoteUri.sHost.empty())
				{
					for (const auto &kfile : m_pRepoDesc->m_keyfiles)
					{
						if (endsWith(m_remoteUri.sPath, kfile))
						{
                            sErrorMsg = "Keyfile N/A, mirror blacklisted";
							return ret | HINT_RECONNECT_NOW | EFLAG_MIRROR_BROKEN;
						}
					}
				}

				off_t contentLength = atoofft(h.h[header::CONTENT_LENGTH], -1);

                if (m_fiAttr.bHeadOnly)
				{
					dbgline;
					m_DlState = STATE_FINISHJOB;
				}
				else if (h.h[header::TRANSFER_ENCODING]
						&& 0
								== strcasecmp(
										h.h[header::TRANSFER_ENCODING],
										"chunked"))
				{
					dbgline;
					m_DlState = STATE_GETCHUNKHEAD;
					h.del(header::TRANSFER_ENCODING); // don't care anymore
				}
				else if (contentLength < 0)
				{
					dbgline;
                    sErrorMsg = "Missing Content-Length";
					return ret | HINT_RECONNECT_NOW | EFLAG_JOB_BROKEN;
				}
				else
				{
					dbgline;
					// may support such endless stuff in the future but that's too unreliable for now
					m_nRest = contentLength;
					m_DlState = STATE_PROCESS_DATA;
				}

				// detect bad auto-redirectors (auth-pages, etc.) by the mime-type of their target
				if (cfg::redirmax && !cfg::badredmime.empty()
						&& cfg::redirmax != m_nRedirRemaining
						&& h.h[header::CONTENT_TYPE]
						&& strstr(h.h[header::CONTENT_TYPE],
								cfg::badredmime.c_str())
						&& h.getStatusCode() < 300) // contains the final data/response
				{
                    if (m_pStorage->IsVolatile())
					{
						// volatile... this is still ok, just make sure time check works next time
						h.set(header::LAST_MODIFIED, FAKEDATEMARK);
					}
					else
					{
						// this was redirected and the destination is BAD!
						h.setStatus(501, "Redirected to invalid target");
					}
				}

				// ok, can pass the data to the file handler
				auto storeResult = CheckAndSaveHeader(move(h),
						string_view(inBuf.rptr(), hDataLen), contentLength);
				inBuf.drop(size_t(hDataLen));

				if (m_pStorage && m_pStorage->m_spattr.bHeadOnly)
					m_nRest = 0;

				if (storeResult == EResponseEval::RESTART_NEEDED)
					return ret | EFLAG_LOST_CON | HINT_RECONNECT_NOW; // recoverable

				if (storeResult == EResponseEval::BUSY_OR_ERROR)
				{
					ldbg("Item dl'ed by others or in error state --> drop it, reconnect");
					m_DlState = STATE_PROCESS_DATA;
                    sErrorMsg = "Busy Cache Item";
					return ret | HINT_RECONNECT_NOW | EFLAG_JOB_BROKEN
							| EFLAG_STORE_COLLISION;
				}
			}
			else if (m_DlState == STATE_PROCESS_CHUNKDATA
					|| m_DlState == STATE_PROCESS_DATA)
			{
				// similar states, just handled differently afterwards
				ldbg("STATE_GETDATA");
				auto res = NewDataHandler(inBuf);
				if (HINT_SWITCH != res)
					return res;
			}
			else if (m_DlState == STATE_FINISHJOB)
			{
				ldbg("STATE_FINISHJOB");
				lockguard g(*m_pStorage);
				m_pStorage->DlFinish(false);
				m_DlState = STATE_GETHEADER;
				return HINT_DONE;
			}
			else if (m_DlState == STATE_GETCHUNKHEAD)
			{
				ldbg("STATE_GETCHUNKHEAD");
				// came back from reading, drop remaining newlines?
				while (inBuf.size() > 0)
				{
					char c = *(inBuf.rptr());
					if (c != '\r' && c != '\n')
						break;
					inBuf.drop(1);
				}
				const char *crlf(0), *pStart(inBuf.c_str());
				if (!inBuf.size()
						|| nullptr == (crlf = strstr(pStart, "\r\n")))
				{
					inBuf.move();
					return HINT_MORE;
				}
				unsigned len(0);
				if (1 != sscanf(pStart, "%x", &len))
				{
                    sErrorMsg = "Invalid stream";
					return EFLAG_JOB_BROKEN; // hm...?
				}
				inBuf.drop(crlf + 2 - pStart);
				if (len > 0)
				{
					m_nRest = len;
					m_DlState = STATE_PROCESS_CHUNKDATA;
				}
				else
					m_DlState = STATE_GET_CHUNKTRAILER;
			}
			else if (m_DlState == STATE_GET_CHUNKTRAILER)
			{
				if (inBuf.size() < 2)
					return HINT_MORE;
				const char *pStart(inBuf.c_str());
				const char *crlf(strstr(pStart, "\r\n"));
				if (!crlf)
					return HINT_MORE;

				if (crlf == pStart) // valid empty line -> done here
				{
					inBuf.drop(2);
					m_DlState = STATE_FINISHJOB;
				}
				else
					inBuf.drop(crlf + 2 - pStart); // drop line and watch for others
			}
		}
		ASSERT(!"unreachable");
        sErrorMsg = "Bad state";
		return EFLAG_JOB_BROKEN;
	}

	EResponseEval CheckAndSaveHeader(header&& h, string_view rawHeader, off_t contLen)
	{
		LOGSTARTFUNC;

		auto& remoteStatus = h.getStatus();

		lockguard g(*m_pStorage);

		auto& sPathRel = m_pStorage->m_sPathRel;
		auto& fiStatus = m_pStorage->m_status;

		auto mark_assigned = [&]() {

			m_bFileItemAssigned = true;
			m_pStorage->m_nTimeDlStarted = GetTime();
		};

        auto withError = [&](string_view message,
                fileitem::EDestroyMode destruction = fileitem::EDestroyMode::KEEP) {
            m_bAllowStoreData = false;
			mark_assigned();
			USRERR(sPathRel << " response or storage error [" << message << "], last errno: " << tErrnoFmter());
            m_pStorage->DlSetError({503, mstring(message)}, destruction);
            return EResponseEval::BUSY_OR_ERROR;
        };

		USRDBG( "Download started, storeHeader for " << sPathRel << ", current status: " << (int) fiStatus);

		m_pStorage->m_nIncommingCount += rawHeader.size();

		if(fiStatus >= fileitem::FIST_COMPLETE)
		{
			USRDBG( "Download was completed or aborted, not restarting before expiration");
			return EResponseEval::BUSY_OR_ERROR;
		}

		if (!m_bFileItemAssigned && fiStatus > fileitem::FIST_DLPENDING)
		{
			// it's active but not served by us
			return EResponseEval::BUSY_OR_ERROR;
		}

		string sLocation;

        switch(remoteStatus.code)
		{
		case 200:
		{
			// forget the offset!!
			m_nUsedRangeStartPos = 0;
			break;
		}
		case 206:
		{
			/*
			 * Range: bytes=453291-
			 * ...
			 * Content-Length: 7271829
			 * Content-Range: bytes 453291-7725119/7725120
			 *
			 * RFC:
			 * HTTP/1.1 206 Partial content
       Date: Wed, 15 Nov 1995 06:25:24 GMT
       Last-Modified: Wed, 15 Nov 1995 04:58:08 GMT
       Content-Range: bytes 21010-47021/47022
       Content-Length: 26012
       Content-Type: image/gif
			 */
			h.setStatus(200, "OK");
			const char *p=h.h[header::CONTENT_RANGE];

			if(!p)
                return withError("Missing Content-Range in Partial Response");

			const static std::regex re("bytes(\\s*|=)(\\d+)-(\\d+|\\*)/(\\d+|\\*)");

			std::cmatch reRes;
			if (!std::regex_search(p, reRes, re))
			{
                return withError("Bad range");
			}
			auto tcount = reRes.size();
			if (tcount != 5)
			{
                return withError("Bad range format");
			}
			// * would mean -1
            contLen = atoofft(reRes[4].first, -1);
			auto startPos = atoofft(reRes[2].first, -1);

			// identify the special probe request which reports what we already knew
            if (m_pStorage->IsVolatile() &&
					m_pStorage->m_nSizeCachedInitial > 0 &&
                    contLen == m_pStorage->m_nSizeCachedInitial &&
					m_pStorage->m_nSizeCachedInitial - 1 == startPos &&
                    m_pStorage->m_responseModDate == h.h[header::LAST_MODIFIED])
			{
				m_bAllowStoreData = false;
				mark_assigned();
				m_pStorage->m_nSizeChecked = m_pStorage->m_nContentLength = m_pStorage->m_nSizeCachedInitial;
				m_pStorage->DlFinish(true);
				return EResponseEval::GOOD;
			}
			// in other cases should resume and the expected position, or else!
			if (startPos == -1 ||
					m_nUsedRangeStartPos != startPos ||
					startPos < m_pStorage->m_nSizeCachedInitial)
			{
                return withError("Server reports unexpected range");
            }
			break;
		}
		case 416:
			// that's bad; it cannot have been completed before (the -1 trick)
			// however, proxy servers with v3r4 cl3v3r caching strategy can cause that
			// if if-mo-since is used and they don't like it, so attempt a retry in this case
			if(m_pStorage->m_nSizeChecked < 0)
			{
				USRDBG( "Peer denied to resume previous download (transient error) " << sPathRel );
				m_pStorage->m_nSizeCachedInitial = 0; // XXX: this is ok as hint to request cooking but maybe add dedicated flag
				m_pStorage->m_bWriterMustReplaceFile = true;
				return EResponseEval::RESTART_NEEDED;
			}
			else
			{
				// -> kill cached file ASAP
				m_bAllowStoreData=false;
                return withError("Disagreement on file size, cleaning up", fileitem::EDestroyMode::TRUNCATE);
			}
			break;
		default: //all other codes don't have a useful body
			if (m_bFileItemAssigned) // resuming case
			{
				// got an error from the replacement mirror? cannot handle it properly
				// because some job might already have started returning the data
				USRDBG( "Cannot resume, HTTP code: " << remoteStatus.code);
				return withError(remoteStatus.msg);
			}

            if (remoteStatus.isRedirect())
            {
				if (!h.h[header::LOCATION] || !h.h[header::LOCATION][0])
					return withError("Invalid redirection (missing location)");
				sLocation = h.h[header::LOCATION];
            }
			// don't tell clients anything about the body
			m_bAllowStoreData = false;
            contLen = -1;
		}

		if(cfg::debug & log::LOG_MORE)
			log::misc(string("Download of ")+sPathRel+" started");

		mark_assigned();

        if (!m_pStorage->DlStarted(rawHeader, tHttpDate(h.h[header::LAST_MODIFIED]),
                                   sLocation.empty() ? RemoteUri(false) : sLocation,
                                   remoteStatus,
                                   m_nUsedRangeStartPos, contLen))
		{
			return EResponseEval::BUSY_OR_ERROR;
		}

        if (!m_bAllowStoreData)
        {
			// XXX: better ensure that it's processed from outside loop and use custom return code?
			m_pStorage->DlFinish(true);
		}
		return EResponseEval::GOOD;
	}

private:
	// not to be copied ever
	tDlJob(const tDlJob&);
	tDlJob& operator=(const tDlJob&);
};


class CDlConn : public dlcon
{
	typedef std::list<tDlJob> tDljQueue;
	friend struct ::acng::tDlJob;
	friend class ::acng::dlcon;

	tDljQueue m_new_jobs;
	const IDlConFactory &m_conFactory;

#ifdef HAVE_LINUX_EVENTFD
	int m_wakeventfd = -1;
#define fdWakeRead m_wakeventfd
#define fdWakeWrite m_wakeventfd
#else
	int m_wakepipe[2] =
	{ -1, -1 };
#define fdWakeRead m_wakepipe[0]
#define fdWakeWrite m_wakepipe[1]
#endif
	// cheaper way to trigger wake flag checks and also notify about termination request, without locking mutex all the time
	atomic_int m_ctrl_hint = ATOMIC_VAR_INIT(0);
	mutex m_handover_mutex;

	/// blacklist for permanently failing hosts, with error message
	tStrMap m_blacklist;
	tSS m_sendBuf, m_inBuf;

	unsigned ExchangeData(mstring &sErrorMsg, tDlStreamHandle &con,
			tDljQueue &qActive);

	// Disable pipelining for the next # requests. Actually used as crude workaround for the
	// concept limitation (because of automata over a couple of function) and its
	// impact on download performance.
	// The use case: stupid web servers that redirect all requests do that step-by-step, i.e.
	// they get a bunch of requests but return only the first response and then flush the buffer
	// so we process this response and wish to switch to the new target location (dropping
	// the current connection because we don't keep it somehow to background, this is the only
	// download agent we have). This manner perverts the whole principle and causes permanent
	// disconnects/reconnects. In this case, it's beneficial to disable pipelining and send
	// our requests one-by-one. This is done for a while (i.e. the valueof(m_nDisablePling)/2 )
	// times before the operation mode returns to normal.
	int m_nTempPipelineDisable = 0;

	// the default behavior or using or not using the proxy. Will be set
	// if access proxies shall no longer be used.
	bool m_bProxyTot = false;

	// this is a binary factor, meaning how many reads from buffer are OK when
	// speed limiting is enabled
	unsigned m_nSpeedLimiterRoundUp = (unsigned(1) << 16) - 1;
	unsigned m_nSpeedLimitMaxPerTake = MAX_VAL(unsigned);
	unsigned m_nLastDlCount = 0;

	void wake();
	void drain_event_stream();

	void WorkLoop() override;

public:

	CDlConn(const IDlConFactory &pConFactory);

	~CDlConn();

	void SignalStop() override;

	// dlcon interface
public:
	bool AddJob(const std::shared_ptr<fileitem> &fi, tHttpUrl src, bool isPT, mstring extraHeaders) override;
	bool AddJob(const std::shared_ptr<fileitem> &fi, tRepoResolvResult repoSrc, bool isPT, mstring extraHeaders) override;
};

#ifdef HAVE_LINUX_EVENTFD
inline void CDlConn::wake()
{
	LOGSTART("CDlConn::wake");
	if (fdWakeWrite == -1)
		return;
	while (true)
	{
		auto r = eventfd_write(fdWakeWrite, 1);
		if (r == 0 || (errno != EINTR && errno != EAGAIN))
			break;
	}

}

inline void CDlConn::drain_event_stream()
{
	LOGSTARTFUNC
	eventfd_t xtmp;
	for (int i = 0; i < 1000; ++i)
	{
		auto tmp = eventfd_read(fdWakeRead, &xtmp);
		if (tmp == 0)
			return;
		if (errno != EAGAIN)
			return;
	}
}

#else
void CDlConn::wake()
{
	LOGSTART("CDlConn::wake");
	POKE(fdWakeWrite);
}
inline void CDlConn::awaken_check()
{
	LOGSTART("CDlConn::awaken_check");
	for (char tmp; ::read(m_wakepipe[0], &tmp, 1) > 0;) ;
}

#endif


bool CDlConn::AddJob(const std::shared_ptr<fileitem> &fi, tHttpUrl src, bool isPT, mstring extraHeaders)
{
	if (m_ctrl_hint < 0 || evabase::in_shutdown)
		return false;
	{
		lockguard g(m_handover_mutex);
		m_new_jobs.emplace_back(this, fi, move(src), isPT, move(extraHeaders));
	}
	m_ctrl_hint++;
	wake();
	return true;
}

bool CDlConn::AddJob(const std::shared_ptr<fileitem> &fi, tRepoResolvResult repoSrc, bool isPT, mstring extraHeaders)
{
	if (m_ctrl_hint < 0 || evabase::in_shutdown)
		return false;
	if (! repoSrc.repodata || repoSrc.repodata->m_backends.empty())
		return false;
	if (repoSrc.sRestPath.empty())
		return false;
	{
		lockguard g(m_handover_mutex);
		m_new_jobs.emplace_back(this, fi, move(repoSrc), isPT, move(extraHeaders));
	}
	m_ctrl_hint++;
	wake();
	return true;
}

CDlConn::CDlConn(const IDlConFactory &pConFactory) :
	m_conFactory(pConFactory)
{
	LOGSTART("CDlConn::dlcon");
#ifdef HAVE_LINUX_EVENTFD
	m_wakeventfd = eventfd(0, EFD_NONBLOCK);
	if (m_wakeventfd == -1)
		m_ctrl_hint = -1;
#else
	if (0 == pipe(m_wakepipe))
	{
		set_nb(m_wakepipe[0]);
		set_nb(m_wakepipe[1]);
	}
	else
	{
		m_wakepipe[0] = m_wakepipe[1] = -1;
		m_ctrl_hint = -1;
	}
#endif
	g_nDlCons++;
}

CDlConn::~CDlConn()
{
	LOGSTART("CDlConn::~dlcon, Destroying dlcon");
#ifdef HAVE_LINUX_EVENTFD
	checkforceclose(m_wakeventfd);
#else
	checkforceclose(m_wakepipe[0]);
	checkforceclose(m_wakepipe[1]);
#endif
	g_nDlCons--;
}

void CDlConn::SignalStop()
{
	LOGSTART("CDlConn::SignalStop");
	// stop all activity as soon as possible
	m_ctrl_hint = -1;
	wake();
}

inline unsigned CDlConn::ExchangeData(mstring &sErrorMsg,
									  tDlStreamHandle &con, tDljQueue &inpipe)
{
	LOGSTARTFUNC;
	LOG("qsize: " << inpipe.size() << ", sendbuf size: " << m_sendBuf.size() << ", inbuf size: " << m_inBuf.size());

	fd_set rfds, wfds;
	int r = 0;
	int fd = con ? con->GetFD() : -1;
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);

	if (inpipe.empty())
		m_inBuf.clear(); // better be sure about dirty buffer from previous connection

	// no socket operation needed in this case but just process old buffer contents
	bool bReEntered = !m_inBuf.empty();

	loop_again:

	for (;;)
	{
		FD_SET(fdWakeRead, &rfds);
		int nMaxFd = fdWakeRead;

		if (fd != -1)
		{
			FD_SET(fd, &rfds);
			nMaxFd = std::max(fd, nMaxFd);

			if (!m_sendBuf.empty())
			{
				ldbg("Needs to send " << m_sendBuf.size() << " bytes");
				FD_SET(fd, &wfds);
			}
#ifdef HAVE_SSL
			else if (con->GetBIO() && BIO_should_write(con->GetBIO()))
			{
				ldbg(
						"NOTE: OpenSSL wants to write although send buffer is empty!");
				FD_SET(fd, &wfds);
			}
#endif
		}

		ldbg("select dlcon");

		// jump right into data processing but only once
		if (bReEntered)
		{
			bReEntered = false;
			goto proc_data;
		}

		r = select(nMaxFd + 1, &rfds, &wfds, nullptr,
				CTimeVal().ForNetTimeout());
		ldbg("returned: " << r << ", errno: " << tErrnoFmter());
		if (m_ctrl_hint < 0)
			return HINT_RECONNECT_NOW;

		if (r == -1)
		{
			if (EINTR == errno)
				continue;
			if (EBADF == errno) // that some times happen for no obvious reason
				return HINT_RECONNECT_NOW;
			tErrnoFmter fer("FAILURE: select, ");
			LOG(fer);
			sErrorMsg = string("Internal malfunction, ") + fer;
			return HINT_RECONNECT_NOW | EFLAG_JOB_BROKEN | EFLAG_MIRROR_BROKEN;
		}
		else if (r == 0) // looks like a timeout
		{
			sErrorMsg = "Connection timeout";
			LOG(sErrorMsg);

			// was there anything to do at all?
			if (inpipe.empty())
				return HINT_SWITCH;

			return (HINT_RECONNECT_NOW| EFLAG_JOB_BROKEN);
		}

		if (FD_ISSET(fdWakeRead, &rfds))
		{
			drain_event_stream();
			return HINT_SWITCH;
		}

		if (fd >= 0)
		{
			if (FD_ISSET(fd, &wfds))
			{
				FD_CLR(fd, &wfds);

#ifdef HAVE_SSL
				if (con->GetBIO())
				{
					int s = BIO_write(con->GetBIO(), m_sendBuf.rptr(),
							m_sendBuf.size());
					ldbg(
							"tried to write to SSL, " << m_sendBuf.size() << " bytes, result: " << s);
					if (s > 0)
						m_sendBuf.drop(s);
				}
				else
				{
#endif
					ldbg("Sending data...\n" << m_sendBuf);
					int s = ::send(fd, m_sendBuf.data(), m_sendBuf.length(),
					MSG_NOSIGNAL);
					ldbg(
							"Sent " << s << " bytes from " << m_sendBuf.length() << " to " << con.get());
					if (s < 0)
					{
						// EAGAIN is weird but let's retry later, otherwise reconnect
						if (errno != EAGAIN && errno != EINTR)
						{
							sErrorMsg = "Send failed";
							return EFLAG_LOST_CON;
						}
					}
					else
						m_sendBuf.drop(s);

				}
#ifdef HAVE_SSL
			}
#endif
		}

		if (fd >= 0 && (FD_ISSET(fd, &rfds)
#ifdef HAVE_SSL
				|| (con->GetBIO() && BIO_should_read(con->GetBIO()))
#endif
				))
		{
			if (cfg::maxdlspeed != cfg::RESERVED_DEFVAL)
			{
				auto nCntNew = g_nDlCons.load();
				if (m_nLastDlCount != nCntNew)
				{
					m_nLastDlCount = nCntNew;

					// well, split the bandwidth
					auto nSpeedNowKib = uint(cfg::maxdlspeed) / nCntNew;
					auto nTakesPerSec = nSpeedNowKib / 32;
					if (!nTakesPerSec)
						nTakesPerSec = 1;
					m_nSpeedLimitMaxPerTake = nSpeedNowKib * 1024
							/ nTakesPerSec;
					auto nIntervalUS = 1000000 / nTakesPerSec;
					auto nIntervalUS_copy = nIntervalUS;
					// creating a bitmask
					for (m_nSpeedLimiterRoundUp = 1, nIntervalUS /= 2;
							nIntervalUS; nIntervalUS >>= 1)
						m_nSpeedLimiterRoundUp = (m_nSpeedLimiterRoundUp << 1)
								| 1;
					m_nSpeedLimitMaxPerTake = uint(
							double(m_nSpeedLimitMaxPerTake)
									* double(m_nSpeedLimiterRoundUp)
									/ double(nIntervalUS_copy));
				}
				// waiting for the next time slice to get data from buffer
				timeval tv;
				if (0 == gettimeofday(&tv, nullptr))
				{
					auto usNext = tv.tv_usec | m_nSpeedLimiterRoundUp;
					usleep(usNext - tv.tv_usec);
				}
			}
#ifdef HAVE_SSL
			if (con->GetBIO())
			{
				r = BIO_read(con->GetBIO(), m_inBuf.wptr(),
						std::min(m_nSpeedLimitMaxPerTake, m_inBuf.freecapa()));
				if (r > 0)
					m_inBuf.got(r);
				else
					// <=0 doesn't mean an error, only a double check can tell
					r = BIO_should_read(con->GetBIO()) ? 1 : -errno;
			}
			else
#endif
			{
				r = m_inBuf.sysread(fd, m_nSpeedLimitMaxPerTake);
			}

#ifdef DISCO_FAILURE
#warning DISCO_FAILURE active!
			if((random() & 0xff) < 10)
			{
				LOG("\n#################\nFAKING A FAILURE\n###########\n");
				r=0;
				errno = EROFS;
				//r = -errno;
//				shutdown(con.get()->GetFD(), SHUT_RDWR);
			}
#endif

			if (r == -EAGAIN || r == -EWOULDBLOCK)
			{
				ldbg("why EAGAIN/EINTR after getting it from select?");
//				timespec sleeptime = { 0, 432000000 };
//				nanosleep(&sleeptime, nullptr);
				goto loop_again;
			}
			else if (r == 0)
			{
				dbgline;
				sErrorMsg = "Connection closed, check DlMaxRetries";
				LOG(sErrorMsg);
				return EFLAG_LOST_CON;
			}
			else if (r < 0) // other error, might reconnect
			{
				dbgline;
				// pickup the error code for later and kill current connection ASAP
				sErrorMsg = tErrnoFmter();
				return EFLAG_LOST_CON;
			}

			proc_data:

			if (inpipe.empty())
			{
				ldbg("FIXME: unexpected data returned?");
				sErrorMsg = "Unexpected data";
				return EFLAG_LOST_CON;
			}

			while (!m_inBuf.empty())
			{

				//ldbg("Processing job for " << inpipe.front().RemoteUri(false));
				dbgline;
				unsigned res = inpipe.front().ProcessIncomming(m_inBuf, false);
				//ldbg("... incoming data processing result: " << res << ", emsg: " << inpipe.front().sErrorMsg);
				LOG("res = " << res);

				if (res & EFLAG_MIRROR_BROKEN)
				{
					ldbg("###### BROKEN MIRROR ####### on " << con.get());
				}

				if (HINT_MORE == res)
					goto loop_again;

				if (HINT_DONE & res)
				{
					// just in case that server damaged the last response body
					con->KnowLastFile(WEAK_PTR<fileitem>(inpipe.front().m_pStorage));

					inpipe.pop_front();
					if (HINT_RECONNECT_NOW & res)
						return HINT_RECONNECT_NOW; // with cleaned flags

					LOG(
							"job finished. Has more? " << inpipe.size() << ", remaining data? " << m_inBuf.size());

					if (inpipe.empty())
					{
						LOG("Need more work");
						return HINT_SWITCH;
					}

					LOG("Extract more responses");
					continue;
				}

				if (HINT_TGTCHANGE & res)
				{
					/* If the target was modified for internal redirection then there might be
					 * more responses of that kind in the queue. Apply the redirection handling
					 * to the rest as well if possible without having side effects.
					 */
					auto it = inpipe.begin();
					for (++it; it != inpipe.end(); ++it)
					{
						unsigned rr = it->ProcessIncomming(m_inBuf, true);
						// just the internal rewriting applied and nothing else?
						if ( HINT_TGTCHANGE != rr)
						{
							// not internal redirection or some failure doing it
							m_nTempPipelineDisable = 30;
							return (HINT_TGTCHANGE | HINT_RECONNECT_NOW);
						}
					}
					// processed all inpipe stuff but if the buffer is still not empty then better disconnect
					return HINT_TGTCHANGE | (m_inBuf.empty() ? 0 : HINT_RECONNECT_NOW);
				}

				// else case: error handling, pass to main loop
				if (HINT_KILL_LAST_FILE & res)
					con->KillLastFile();
				setIfNotEmpty(sErrorMsg, inpipe.front().sErrorMsg);
				return res;
			}
			return HINT_DONE; // input buffer consumed
		}
	}

	ASSERT(!"Unreachable");
	sErrorMsg = "Internal failure";
	return EFLAG_JOB_BROKEN | HINT_RECONNECT_NOW;
}

void CDlConn::WorkLoop()
{
	LOGSTART("CDlConn::WorkLoop");
	string sErrorMsg;
	m_inBuf.clear();

	if (!m_inBuf.setsize(cfg::dlbufsize))
	{
		log::err("Out of memory");
		return;
	}

	if (fdWakeRead < 0 || fdWakeWrite < 0)
	{
		USRERR("Error creating pipe file descriptors");
		return;
	}

	tDljQueue next_jobs, active_jobs;

	tDtorEx allJobReleaser([&]()
	{	next_jobs.clear(); active_jobs.clear();});

	tDlStreamHandle con;
	unsigned loopRes = 0;

	bool bStopRequesting = false; // hint to stop adding request headers until the connection is restarted
	bool bExpectRemoteClosing = false;
	int nLostConTolerance = MAX_RETRY;

	auto BlacklistMirror = [&](tDlJob &job)
	{
		LOGSTARTFUNCx(job.GetPeerHost().ToURI(false));
		m_blacklist[job.GetPeerHost().GetHostPortKey()] = sErrorMsg;
	};

	auto prefProxy = [&](tDlJob &cjob) -> const tHttpUrl*
	{
		if(m_bProxyTot)
		return nullptr;

		if(cjob.m_pRepoDesc && cjob.m_pRepoDesc->m_pProxy
				&& !cjob.m_pRepoDesc->m_pProxy->sHost.empty())
		{
			return cjob.m_pRepoDesc->m_pProxy;
		}
		return cfg::GetProxyInfo();
	};
	int lastCtrlMark = -2;
	while (true) // outer loop: jobs, connection handling
	{
		// init state or transfer loop jumped out, what are the needed actions?
		LOG("New next_jobs: " << next_jobs.size());

		if (m_ctrl_hint < 0 || evabase::in_shutdown) // check for ordered shutdown
		{
			/* The no-more-users checking logic will purge orphaned items from the inpipe
			 * queue. When the connection is dirty after that, it will be closed in the
			 * ExchangeData() but if not then it can be assumed to be clean and reusable.
			 */
			if (active_jobs.empty())
			{
				if (con)
					m_conFactory.RecycleIdleConnection(con);
				return;
			}
		}
		int newCtrlMark = m_ctrl_hint;
		if (newCtrlMark != lastCtrlMark)
		{
			lastCtrlMark = newCtrlMark;
			lockguard g(m_handover_mutex);
			next_jobs.splice(next_jobs.begin(), m_new_jobs);
		}
		dbgline;
		if (next_jobs.empty() && active_jobs.empty())
			goto go_select;
		// parent will notify RSN
		dbgline;
		if (!con)
		{
			dbgline;
			// cleanup after the last connection - send buffer, broken next_jobs, ...
			m_sendBuf.clear();
			m_inBuf.clear();
			active_jobs.clear();

			bStopRequesting = false;

			for (tDljQueue::iterator it = next_jobs.begin();
					it != next_jobs.end();)
			{
				if (it->SetupJobConfig(sErrorMsg, m_blacklist))
					++it;
				else
				{
					setIfNotEmpty2(it->sErrorMsg, sErrorMsg,
                            "Broken mirror or incorrect configuration");
					it = next_jobs.erase(it);
				}
			}
			if (next_jobs.empty())
			{
				LOG("no next_jobs left, start waiting")
				goto go_select;
				// nothing left, might receive new next_jobs soon
			}

			bool bUsed = false;
			ASSERT(!next_jobs.empty());
			auto doconnect = [&](const tHttpUrl &tgt, int timeout, bool fresh)
			{
				bExpectRemoteClosing = false;

				for(auto& j: active_jobs)
					j.ResetStreamState();
				for(auto& j: next_jobs)
					j.ResetStreamState();

				return m_conFactory.CreateConnected(tgt.sHost,
						tgt.GetPort(),
						sErrorMsg,
						&bUsed,
						next_jobs.front().GetConnStateTracker(),
						IFSSLORFALSE(tgt.bSSL),
						timeout, fresh);
			};

			auto &cjob = next_jobs.front();
			auto proxy = prefProxy(cjob);
			auto &peerHost = cjob.GetPeerHost();

#ifdef HAVE_SSL
			if (peerHost.bSSL)
			{
				if (proxy)
				{
					con = doconnect(*proxy,
							cfg::optproxytimeout > 0 ?
									cfg::optproxytimeout : cfg::nettimeout,
							false);
					if (con)
					{
						if (!con->StartTunnel(peerHost, sErrorMsg,
								&proxy->sUserPass, true))
							con.reset();
					}
				}
				else
					con = doconnect(peerHost, cfg::nettimeout, false);
			}
			else
#endif
			{
				if (proxy)
				{
					con = doconnect(*proxy,
							cfg::optproxytimeout > 0 ?
									cfg::optproxytimeout : cfg::nettimeout,
							false);
				}
				else
					con = doconnect(peerHost, cfg::nettimeout, false);
			}

			if (!con && proxy && cfg::optproxytimeout > 0)
			{
				ldbg("optional proxy broken, disable");
				m_bProxyTot = true;
				proxy = nullptr;
				cfg::MarkProxyFailure();
				con = doconnect(peerHost, cfg::nettimeout, false);
			}

			ldbg("connection valid? " << bool(con) << " was fresh? " << !bUsed);

			if (con)
			{
				ldbg("target? [" << con->GetHostname() << "]:" << con->GetPort());

				// must test this connection, just be sure no crap is in the pipe
				if (bUsed && check_read_state(con->GetFD()))
				{
					ldbg("code: MoonWalker");
					con.reset();
					continue;
				}
			}
			else
			{
				BlacklistMirror(cjob);
				continue; // try the next backend
			}
		}

		// connection should be stable now, prepare all jobs and/or move to pipeline
		while (!bStopRequesting && !next_jobs.empty()
				&& int(active_jobs.size()) <= cfg::pipelinelen)
		{
			auto &frontJob = next_jobs.front();

			if (!frontJob.SetupJobConfig(sErrorMsg, m_blacklist))
			{
				// something weird happened to it, drop it and let the client care
				next_jobs.pop_front();
				continue;
			}

			auto &tgt = frontJob.GetPeerHost();
			// good case, direct or tunneled connection
			bool match = (tgt.sHost == con->GetHostname()
						  && (tgt.GetPort() == con->GetPort()
							  // or don't care port
							  || !con->GetPort())
						  );
			const tHttpUrl *proxy = nullptr; // to be set ONLY if PROXY mode is used

			// if not exact and can be proxied, and is this the right proxy?
			if (!match)
			{
				proxy = prefProxy(frontJob);
				if (proxy)
				{
					/*
					 * SSL over proxy uses HTTP tunnels (CONNECT scheme) so the check
					 * above should have matched before.
					 */
					if (!tgt.bSSL)
						match = (proxy->sHost == con->GetHostname()
								&& proxy->GetPort() == con->GetPort());
				}
				// else... host changed and not going through the same proxy -> fail
			}

			if (!match)
			{
				LOG(
						"host mismatch, new target: " << tgt.sHost << ":" << tgt.GetPort());
				bStopRequesting = true;
				break;
			}

			frontJob.AppendRequest(m_sendBuf, proxy);
			LOG("request headers added to buffer");
			auto itSecond = next_jobs.begin();
			active_jobs.splice(active_jobs.end(), next_jobs, next_jobs.begin(),
					++itSecond);

			if (m_nTempPipelineDisable > 0)
			{
				bStopRequesting = true;
				--m_nTempPipelineDisable;
				break;
			}
		}

		//ldbg("Request(s) cooked, buffer contents: " << m_sendBuf);
		//ASSERT(!m_sendBuf.empty());
		// FIXME: this is BS in case of SSL (rekeying) but what was the idea?

		go_select:

		// inner loop: plain communication until something happens. Maybe should use epoll here?
		loopRes = ExchangeData(sErrorMsg, con, active_jobs);

		ldbg("loopRes: "<< loopRes
			 << " = " << tSS::BitPrint(" | ",
									   loopRes,
									   BITNNAME(HINT_RECONNECT_NOW)
									   , BITNNAME(HINT_DONE)
									   , BITNNAME(HINT_KILL_LAST_FILE)
									   , BITNNAME(HINT_MORE)
									   , BITNNAME(HINT_SWITCH)
									   , BITNNAME(HINT_TGTCHANGE)
									   , BITNNAME(EFLAG_JOB_BROKEN)
									   , BITNNAME(EFLAG_LOST_CON)
									   , BITNNAME(EFLAG_STORE_COLLISION)
									   , BITNNAME(EFLAG_MIRROR_BROKEN)
									   , BITNNAME(HINT_RECONNECT_SOON)
		) );

		bExpectRemoteClosing |= (loopRes & HINT_RECONNECT_SOON);

		if (m_ctrl_hint < 0 || evabase::in_shutdown)
			return;

		/* check whether we have a pipeline stall. This may happen because a) we are done or
		 * b) because of the remote hostname change or c) the client stopped sending tasks.
		 * Anyhow, that's a reason to put the connection back into the shared pool so either we
		 * get it back from the pool in the next workloop cycle or someone else gets it and we
		 * get a new connection for the new host later.
		 * */
		if (active_jobs.empty())
		{
			// all requests have been processed (client done, or pipeline stall, who cares)
			dbgline;

			// no matter what happened, that stop flag is now irrelevant
			bStopRequesting = false;

			// no error bits set, not busy -> this connection is still good, recycle properly
			constexpr auto all_err = HINT_RECONNECT_NOW | HINT_RECONNECT_SOON | EFLAG_JOB_BROKEN | EFLAG_LOST_CON
					| EFLAG_MIRROR_BROKEN;
			if (con && !(loopRes & all_err))
			{
				dbgline;
				m_conFactory.RecycleIdleConnection(con);
				continue;
			}
		}

		/*
		 * Here we go if the inpipe is still not processed or there have been errors
		 * needing special handling.
		 */

		if ((HINT_RECONNECT_NOW | EFLAG_LOST_CON) & loopRes)
		{
			dbgline;
			con.reset();
			m_inBuf.clear();
			m_sendBuf.clear();
		}

		if (loopRes & HINT_TGTCHANGE)
		{
			// short queue continues next_jobs with rewritten targets, so
			// reinsert them into the new task list and continue

			// if conn was not reset above then it should be in good shape
			m_conFactory.RecycleIdleConnection(con);
			goto move_jobs_back_to_q;
		}

		if ((EFLAG_LOST_CON & loopRes) && !active_jobs.empty())
		{
			dbgline;
			// disconnected by OS... give it a chance, or maybe not...
			if (! bExpectRemoteClosing)
			{
				dbgline;
				if (--nLostConTolerance <= 0)
				{
					dbgline;
					BlacklistMirror(active_jobs.front());
					nLostConTolerance = MAX_RETRY;
				}
			}
			con.reset();

			if (! (HINT_DONE & loopRes))
			{
				// trying to resume that job secretly, unless user disabled the use of range (we
				// cannot resync the sending position ATM, throwing errors to user for now)
				if (cfg::vrangeops <= 0 && active_jobs.front().m_fiAttr.bVolatile)
					loopRes |= EFLAG_JOB_BROKEN;
				else
					active_jobs.front().m_DlState = tDlJob::STATE_GETHEADER;
			}
		}

		if (loopRes & (HINT_DONE | HINT_MORE))
		{
			sErrorMsg.clear();
			continue;
		}

		//
		// regular required post-processing done here, now handle special conditions
		//

		if (HINT_SWITCH == loopRes)
			continue;

		// resolving the "fatal error" situation, push the pipelined job back to new, etc.

		if ((EFLAG_MIRROR_BROKEN & loopRes) && !active_jobs.empty())
			BlacklistMirror(active_jobs.front());

		if ((EFLAG_JOB_BROKEN & loopRes) && !active_jobs.empty())
		{
			setIfNotEmpty(active_jobs.front().sErrorMsg, sErrorMsg);

			active_jobs.pop_front();

			if (EFLAG_STORE_COLLISION & loopRes)
			{
				// stupid situation, both users downloading the same stuff - and most likely in the same order
				// if one downloader runs a step ahead (or usually many steps), drop all items
				// already processed by it and try to continue somewhere else.
				// This way, the overall number of collisions and reconnects is minimized

				for (auto pJobList :
				{ &active_jobs, &next_jobs })
				{
					auto &joblist(*pJobList);
					for (auto it = joblist.begin(); it != joblist.end();)
					{
						// someone else is doing it -> drop
						if (it->m_pStorage
								&& it->m_pStorage->GetStatus()
										>= fileitem::FIST_DLRECEIVING)
							it = joblist.erase(it);
						else
							++it;
					}
				};
			}
		}

		move_jobs_back_to_q: next_jobs.splice(next_jobs.begin(), active_jobs);
	}
}

std::shared_ptr<dlcon> dlcon::CreateRegular(const IDlConFactory &pConFactory)
{
	return make_shared<CDlConn>(pConFactory);
}

}
