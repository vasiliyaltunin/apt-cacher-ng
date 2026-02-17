
#include "debug.h"
#include "meta.h"
#include "fileitem.h"
#include "header.h"
#include "acfg.h"
#include "acbuf.h"
#include "fileio.h"

#include <algorithm>

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>

using namespace std;

#define ASSERT_HAVE_LOCK ASSERT(m_obj_mutex.try_lock() == false);

namespace acng
{

// this is kept here as global anchor but it can be never set in special setups!
ACNG_API std::shared_ptr<IFileItemRegistry> g_registry;

fileitem::fileitem(string_view sPathRel) :
	// good enough to not trigger the makeWay check but also not cause overflows
		m_sPathRel(sPathRel)
{
}

void fileitem::DlRefCountAdd()
{
	setLockGuard;
	m_nDlRefsCount++;
}

void fileitem::DlRefCountDec(const tRemoteStatus& reason)
{
	setLockGuard;
	LOGSTARTFUNC
	notifyAll();

	m_nDlRefsCount--;
	if (m_nDlRefsCount > 0)
		return; // someone will care...

	// ... otherwise: the last downloader disappeared, needing to tell observers

	if (m_status < FIST_COMPLETE)
	{
		DlSetError(reason, m_eDestroy);
		USRDBG("Download of " << m_sPathRel << " aborted");
	}
}

uint64_t fileitem::TakeTransferCount()
{
	uint64_t ret = m_nIncommingCount;
	m_nIncommingCount = 0;
	return ret;
}

unique_fd fileitem::GetFileFd()
{
	LOGSTART("fileitem::GetFileFd");
	setLockGuard;
	USRDBG("Opening " << m_sPathRel);
	int fd = open(SZABSPATH(m_sPathRel), O_RDONLY);

#ifdef HAVE_FADVISE
	// optional, experimental
	if (fd != -1)
		posix_fadvise(fd, 0, m_nSizeChecked, POSIX_FADV_SEQUENTIAL);
#endif

	return unique_fd(fd);
}

off_t GetFileSize(cmstring &path, off_t defret)
{
	struct stat stbuf;
	return (0 == ::stat(path.c_str(), &stbuf)) ? stbuf.st_size : defret;
}
/*
void fileitem::ResetCacheState()
{
	setLockGuard;
	m_nSizeSeen = 0;
	m_nSizeChecked = 0;
	m_status = FIST_FRESH;
	m_bAllowStoreData = true;
	m_head.clear();
}
*/

fileitem::FiStatus fileitem_with_storage::Setup()
{
	LOGSTARTFUNC;

	setLockGuard;

	if (m_status > FIST_FRESH)
		return m_status;

	auto error_clean = [this]()
	{
		m_nSizeCachedInitial = m_nSizeChecked = m_nContentLength = -1;
		m_bWriterMustReplaceFile = true;
		return m_status = FIST_INITED;
	};

	m_status = FIST_INITED;

	cmstring sPathAbs(CACHE_BASE + m_sPathRel);
	m_nSizeCachedInitial = GetFileSize(sPathAbs, -1);
	m_nSizeChecked = -1;

	if (!ParseHeadFromStorage(sPathAbs + ".head", &m_nContentLength, &m_responseModDate, &m_responseOrigin))
	{
		return error_clean();
	}

	LOG("good head");
	// report this for all good loading; for volatile items, only becomes relevant when volatile check is performed
	m_responseStatus = { 200, "OK" };

	if (!IsVolatile())
	{
		if (m_spattr.bHeadOnly)
		{
			return m_status = FIST_DLGOTHEAD;
		}

		// non-volatile files, so could accept the length, do some checks first
		if (m_nContentLength >= 0)
		{
			// file larger than it could ever be?
			if (m_nContentLength < m_nSizeCachedInitial)
				return error_clean();

			// is it complete? and 0 value also looks weird, try to verify later
			if (m_nSizeCachedInitial == m_nContentLength)
			{
				m_nSizeChecked = m_nSizeCachedInitial;
				m_status = FIST_COMPLETE;
			}
			else
			{
				// otherwise wait for remote to confirm its presence too
				m_spattr.bVolatile = true;
			}
		}
		else
		{
			// no content length known, let's check the remote size
			m_spattr.bVolatile = true;
		}
	}
	LOG("resulting status: " << (int) m_status);
	return m_status;
}

void fileitem::UpdateHeadTimestamp()
{
	if(m_sPathRel.empty())
		return;
	utimes(SZABSPATH(m_sPathRel + ".head"), nullptr);
}

std::pair<fileitem::FiStatus, tRemoteStatus> fileitem::WaitForFinish()
{
	lockuniq g(this);
	while (m_status < FIST_COMPLETE)
		wait(g);
	return std::pair<fileitem::FiStatus, tRemoteStatus>(m_status, m_responseStatus);
}

std::pair<fileitem::FiStatus, tRemoteStatus>
fileitem::WaitForFinish(unsigned timeout, const std::function<bool()> &waitInterrupted)
{
	lockuniq g(this);
	while (m_status < FIST_COMPLETE)
	{
		if(wait_for(g, timeout, 1)) // on timeout
		{
			if (waitInterrupted)
			{
				if(waitInterrupted())
					continue;
				return std::pair<fileitem::FiStatus, tRemoteStatus>(FIST_DLERROR,  {500, "E_TIMEOUT"});
			}
		}
	}
	return std::pair<fileitem::FiStatus, tRemoteStatus>(m_status, m_responseStatus);
}

inline void _LogWithErrno(const char *msg, const string & sFile)
{
	tErrnoFmter f;
	USRERR(sFile << " storage error [" << msg << "], last errno: " << f);
}

bool fileitem_with_storage::withError(string_view message, fileitem::EDestroyMode destruction)
{
	USRERR(m_sPathRel << " storage error [" << message << "], check file AND directory permissions, last errno: " << tErrnoFmter());
	DlSetError({500, "Cache Error, check apt-cacher.err"}, destruction);
	return false;
}

bool fileitem_with_storage::SaveHeader(bool truncatedKeepOnlyOrigInfo)
{
	auto headPath = SABSPATHEX(m_sPathRel, ".head");
	if (truncatedKeepOnlyOrigInfo)
		return StoreHeadToStorage(headPath, -1, nullptr, &m_responseOrigin);
	return StoreHeadToStorage(headPath, m_nContentLength, &m_responseModDate, &m_responseOrigin);
};

bool fileitem::DlStarted(string_view rawHeader, const tHttpDate& modDate, cmstring& origin, tRemoteStatus status, off_t bytes2seek, off_t bytesAnnounced)
{
	LOGSTARTFUNCxs( modDate.view(), status.code, status.msg, bytes2seek, bytesAnnounced);
	ASSERT_HAVE_LOCK
	m_nIncommingCount += rawHeader.size();
	notifyAll();

	USRDBG( "Download started, storeHeader for " << m_sPathRel << ", current status: " << (int) m_status);

	if(m_status >= FIST_DLGOTHEAD)
	{
		dbgline;
		// what's happening? Hot restart? Can only jump to previous position of state appears sane.
		if (m_nContentLength != bytesAnnounced && m_nContentLength != -1)
			return false;
		dbgline;
		if (modDate != m_responseModDate)
			return false;
		dbgline;
		if (bytes2seek > m_nSizeChecked)
			return false;
	}
	else
	{
		m_nContentLength = -1;
	}
	dbgline;

	m_status=FIST_DLGOTHEAD;
	// if range was confirmed then can already start forwarding that much
	if (bytes2seek >= 0)
	{
		if (m_nSizeChecked >= 0 && bytes2seek < m_nSizeChecked)
			return false;
		m_nSizeChecked = bytes2seek;
	}

	m_responseStatus = move(status);
	m_responseOrigin = move(origin);
	m_responseModDate = modDate;
	m_nContentLength = bytesAnnounced;
	return true;
}

bool fileitem_with_storage::DlAddData(string_view chunk, lockuniq&)
{
	LOGSTARTFUNC;
	ASSERT_HAVE_LOCK;
	// something might care, most likely... also about BOUNCE action
	notifyAll();

	m_nIncommingCount += chunk.size();
	LOG("adding chunk of " << chunk.size() << " bytes at " << m_nSizeChecked);

	// is this the beginning of the stream?
	if(m_filefd == -1 && !SafeOpenOutFile())
		return false;

	if (AC_UNLIKELY(m_filefd == -1 || m_status < FIST_DLGOTHEAD))
		return withError("Suspicious fileitem status");

	if (m_status > FIST_COMPLETE) // DLSTOP, DLERROR
		return false;

	while (!chunk.empty())
	{
		int r = write(m_filefd, chunk.data(), chunk.size());
		if (r == -1)
		{
			if (EINTR != errno && EAGAIN != errno)
				return withError("Write error");
		}
		m_nSizeChecked += r;
		chunk.remove_prefix(r);
	}
	return true;
}

bool fileitem_with_storage::SafeOpenOutFile()
{
	LOGSTARTFUNC;
	checkforceclose(m_filefd);

	if (AC_UNLIKELY(m_spattr.bNoStore))
		return false;

	MoveRelease2Sidestore();

	auto sPathAbs(SABSPATH(m_sPathRel));

	int flags = O_WRONLY | O_CREAT | O_BINARY;

	mkbasedir(sPathAbs);

	auto replace_file = [&]()
	{
		checkforceclose(m_filefd);
		// special case where the file needs be replaced in the most careful way
		m_bWriterMustReplaceFile = false;
		auto dir = GetDirPart(sPathAbs) + "./";
		auto tname = dir + ltos(rand()) + ltos(rand()) + ltos(rand());
		auto tname2 = dir + ltos(rand()) + ltos(rand()) +ltos(rand());
		// keep the file descriptor later if needed
		unique_fd tmp(open(tname.c_str(), flags, cfg::fileperms));
		if (tmp.m_p == -1)
			return withError("Cannot create cache files");
		fdatasync(tmp.m_p);
		bool didNotExist = false;
		if (0 != rename(sPathAbs.c_str(), tname2.c_str()))
		{
			if (errno == ENOENT)
				didNotExist = true;
			else
				return withError("Cannot move cache files");
		}
		if (0 != rename(tname.c_str(), sPathAbs.c_str()))
			return withError("Cannot rename cache files");
		if (!didNotExist)
			unlink(tname2.c_str());
		std::swap(m_filefd, tmp.m_p);
		return m_filefd != -1;
	};

	// if ordered or when acting on chunked transfers
	if (m_bWriterMustReplaceFile || m_nContentLength < 0)
	{
		if (!replace_file())
			return false;
	}
	if (m_filefd == -1)
		m_filefd = open(sPathAbs.c_str(), flags, cfg::fileperms);
	// maybe the old file was a symlink pointing at readonly file
	if (m_filefd == -1 && ! replace_file())
		return false;

	ldbg("file opened?! returned: " << m_filefd);

	auto sizeOnDisk = lseek(m_filefd, 0, SEEK_END);
	if (sizeOnDisk == -1)
		return withError("Cannot seek in cache files");

	// remote files may shrink! We could write in-place and truncate later,
	// however replacing whole thing from the start seems to be safer option
	if (sizeOnDisk > m_nContentLength)
	{
		dbgline;
		if(! replace_file())
			return false;
		sizeOnDisk = 0;
	}

	// either confirm start at zero or verify the expected file state on disk
	if (m_nSizeChecked < 0)
		m_nSizeChecked = 0;

	// make sure to be at the right location, this sometimes could go wrong (probably via MoveRelease2Sidestore)
	lseek(m_filefd, m_nSizeChecked, SEEK_SET);

	// that's in case of hot resuming
	if(m_nSizeChecked > sizeOnDisk)
	{
		// hope that it has been validated before!
		return withError("Checked size beyond EOF");
	}

	auto sHeadPath(sPathAbs + ".head");
	ldbg("Storing header as " + sHeadPath);
	if (!SaveHeader(false))
		return withError("Cannot store header");

	// okay, have the stream open
	m_status = FIST_DLRECEIVING;

	/** Tweak FS to receive a file of remoteSize in one sequence,
	 * considering current m_nSizeChecked as well.
	 */
	if (cfg::allocspace > 0 && m_nContentLength > 0)
	{
		// XXX: we have the stream size parsed before but storing that member all the time just for this purpose isn't exactly great
		auto preservedSequenceLen = m_nContentLength - m_nSizeChecked;
		if (preservedSequenceLen > (off_t) cfg::allocspace)
			preservedSequenceLen = cfg::allocspace;
		if (preservedSequenceLen > 0)
		{
			falloc_helper(m_filefd, m_nSizeChecked, preservedSequenceLen);
			m_bPreallocated = true;
		}
	}

	return true;
}

void fileitem::MarkFaulty(bool killFile)
{
	setLockGuard;
	DlSetError({500, "Bad Cache Item"}, killFile ? EDestroyMode::DELETE : EDestroyMode::TRUNCATE);
}


ssize_t fileitem_with_storage::SendData(int out_fd, int in_fd, off_t &nSendPos, size_t count)
{
	if(out_fd == -1 || in_fd == -1)
		return -1;

#ifndef HAVE_LINUX_SENDFILE
	return sendfile_generic(out_fd, in_fd, &nSendPos, count);
#else
	//if (m_status <= FIST_DLRECEIVING)
	//	return sendfile_generic(out_fd, in_fd, &nSendPos, count);

	auto r = sendfile(out_fd, in_fd, &nSendPos, count);

	if(r<0 && (errno == ENOSYS || errno == EINVAL))
		return sendfile_generic(out_fd, in_fd, &nSendPos, count);

	return r;
#endif
}

mstring fileitem_with_storage::NormalizePath(cmstring &sPathRaw)
{
	return cfg::stupidfs ? DosEscape(sPathRaw) : sPathRaw;
}


fileitem_with_storage::~fileitem_with_storage()
{
	if (AC_UNLIKELY(m_spattr.bNoStore))
		return;

	checkforceclose(m_filefd);

	// done if empty, otherwise might need to perform pending self-destruction
	if (m_sPathRel.empty())
		return;

	mstring sPathAbs, sPathHead;
	auto calcPath = [&]() {
		sPathAbs = SABSPATH(m_sPathRel);
		sPathHead = SABSPATH(m_sPathRel) + ".head";
	};

	switch (m_eDestroy)
	{
	case EDestroyMode::KEEP:
	{
		if(m_bPreallocated)
		{
			Cstat st(sPathAbs);
			if (st)
				ignore_value(truncate(sPathAbs.c_str(), st.st_size)); // CHECKED!
		}
		break;
	}
	case EDestroyMode::TRUNCATE:
	{
		calcPath();
		if (0 != ::truncate(sPathAbs.c_str(), 0))
			unlink(sPathAbs.c_str());
		fileitem_with_storage::SaveHeader(true);
		break;
	}
	case EDestroyMode::ABANDONED:
	{
		calcPath();
		unlink(sPathAbs.c_str());
		break;
	}
	case EDestroyMode::DELETE:
	{
		calcPath();
		unlink(sPathAbs.c_str());
		unlink(sPathHead.c_str());
		break;
	}
	case EDestroyMode::DELETE_KEEP_HEAD:
	{
		calcPath();
		unlink(sPathAbs.c_str());
		fileitem_with_storage::SaveHeader(true);
		break;
	}
	}
}

// special file? When it's rewritten from start, save the old version aside
void fileitem_with_storage::MoveRelease2Sidestore()
{
	if(m_nSizeChecked)
		return;
	if(!endsWithSzAr(m_sPathRel, "/InRelease") && !endsWithSzAr(m_sPathRel, "/Release"))
		return;
	auto srcAbs = CACHE_BASE + m_sPathRel;
	Cstat st(srcAbs);
	if(st)
	{
		auto tgtDir = CACHE_BASE + cfg::privStoreRelSnapSufix + sPathSep + GetDirPart(m_sPathRel);
		mkdirhier(tgtDir);
		auto sideFileAbs = tgtDir + ltos(st.st_ino) + ltos(st.st_mtim.tv_sec)
				+ ltos(st.st_mtim.tv_nsec);
		FileCopy(srcAbs, sideFileAbs);
	}
}


void fileitem::DlFinish(bool forceUpdateHeader)
{
	LOGSTARTFUNC;
	ASSERT_HAVE_LOCK;

	if (AC_UNLIKELY(m_spattr.bNoStore))
		return;

	notifyAll();

	if (m_status > FIST_COMPLETE)
	{
		LOG("already completed");
		return;
	}

	// XXX: double-check whether the content length in header matches checked size?

	m_status = FIST_COMPLETE;

	if (cfg::debug & log::LOG_MORE)
		log::misc(tSS() << "Download of " << m_sPathRel << " finished");

	dbgline;

	// we are done! Fix header after chunked transfers?
	if (m_nContentLength < 0 || forceUpdateHeader)
	{
		if (m_nContentLength < 0)
			m_nContentLength = m_nSizeChecked;

		if (m_eDestroy == KEEP)
			SaveHeader(false);
	}
}

void fileitem::DlSetError(const tRemoteStatus& errState, fileitem::EDestroyMode kmode)
{
	ASSERT_HAVE_LOCK
	notifyAll();
	/*
	 * Maybe needs to fuse them, OTOH hard to tell which is the more severe or more meaningful
	 *
	if (m_responseStatus.code < 300)
		m_responseStatus.code = errState.code;
	if (m_responseStatus.msg.empty() || m_responseStatus.msg == "OK")
		m_responseStatus.msg = errState.msg;
		*
		*/
	m_responseStatus = errState;
	m_status = FIST_DLERROR;
	DBGQLOG("Declared FIST_DLERROR: " << m_responseStatus.code << " " << m_responseStatus.msg);
	if (kmode < m_eDestroy)
		m_eDestroy = kmode;
}

}
