#ifndef _JOB_H
#define _JOB_H

#include "config.h"
#include "acbuf.h"
#include <sys/types.h>
#include "acregistry.h"

#include <set>

namespace acng
{

class ISharedConnectionResources;
class header;

class job
{
public:

    enum eJobResult : short
	{
		R_DONE = 0, R_AGAIN = 1, R_DISCON = 2, R_NOTFORUS = 3
    };

	job(ISharedConnectionResources &pParent);
	~job();

	void Prepare(const header &h, string_view headBuf, cmstring& callerHostname);

	/*
	 * Start or continue returning the file.
	 */
	eJobResult SendData(int confd, bool haveMoreJobs);

    SUTPRIVATE:

    typedef enum : short
    {
        STATE_NOT_STARTED,
        STATE_SEND_DATA,
        STATE_SEND_CHUNK_HEADER,
        STATE_SEND_CHUNK_DATA,
		STATE_DONE,
		// special states for custom behavior notification
		STATE_DISCO_ASAP, // full failure
		STATE_SEND_BUF_NOT_FITEM // send sendbuf and finish
    } eActivity;

	TFileItemHolder m_pItem;

	unique_fd m_filefd;    
    bool m_bIsHttp11 = true;
	bool m_bIsHeadOnly = false;
    ISharedConnectionResources &m_pParentCon;

	enum EKeepAliveMode : uint8_t
	{
		// stay away from boolean, for easy ORing
		CLOSE = 0x10,
		KEEP,
		UNSPECIFIED
	} m_keepAlive = UNSPECIFIED;

    eActivity m_activity = STATE_NOT_STARTED;

	tSS m_sendbuf;
    mstring m_sFileLoc; // local_relative_path_to_file
    mstring m_xff;
	uint8_t m_eMaintWorkType;

    tHttpDate m_ifMoSince;
    off_t m_nReqRangeFrom = -1, m_nReqRangeTo = -1;
    off_t m_nSendPos = 0;
    off_t m_nChunkEnd = -1;
    off_t m_nAllDataCount = 0;

	job(const job&);
	job& operator=(const job&);

	void CookResponseHeader();
    void AddPtHeader(cmstring& remoteHead);
	fileitem::FiStatus _SwitchToPtItem();
	void SetEarlySimpleResponse(string_view message, bool nobody = false);
	void PrepareLocalDownload(const mstring &visPath, const mstring &fsBase,
			const mstring &fsSubpath);

    bool ParseRange(const header& h);
	eJobResult HandleSuddenError();
    void AppendMetaHeaders();
    tSS& PrependHttpVariant();
public:
#ifdef DEBUG
	// mark as somehow successfull prior to deletion
	void Dispose() { m_nAllDataCount++; }
#endif
};

class tTraceData: public std::set<mstring>, public base_with_mutex
{
public:
	static tTraceData& getInstance();
};

}

#endif
