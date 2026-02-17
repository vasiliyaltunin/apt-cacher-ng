
#ifndef _FILEITEM_H
#define _FILEITEM_H

#include <string>
#include <atomic>

#include "config.h"
#include "lockable.h"
#include "header.h"
#include "fileio.h"
#include "httpdate.h"
#include <map>

namespace acng
{
extern const std::string sEmptyString;
class fileitem;
struct tDlJob;
class cacheman;
typedef std::shared_ptr<fileitem> tFileItemPtr;
typedef std::map<mstring, tFileItemPtr> tFiGlobMap;
struct tAppStartStop;

class IFileItemRegistry;
class TFileItemRegistry;

//! Base class containing all required data and methods for communication with the download sources
class ACNG_API fileitem : public base_with_condition
{
	friend struct tDlJob;
    friend class cacheman;
	friend class TFileItemRegistry;
public:

    // items carrying those attributes might be shared under controlled circumstances only
    struct tSpecialPurposeAttr
    {
        bool bVolatile = false;
		bool bHeadOnly = false;
				/**
				 * @brief bNoStore Don't store metadata or attempt to touch cached data in the aftermath
				 * Most useful in combination with bHeadOnly
				 */
		bool bNoStore = false;
        off_t nRangeLimit = -1;
        mstring credentials;
    };

	// Life cycle (process states) of a file description item
	enum FiStatus : uint8_t
	{

	FIST_FRESH, FIST_INITED, FIST_DLPENDING, FIST_DLGOTHEAD, FIST_DLRECEIVING,
	FIST_COMPLETE,
	// error cases: downloader reports its error or last user told downloader to stop
	FIST_DLERROR,
	FIST_DLSTOP // assumed to not have any users left
	};

    /**
     * @brief The EDestroyMode enum
     * Defines which data is needed to be deleted when this item terminates.
     * Ordered by "severity", including how much data will be lost afterwards.
     * @see DlSetError
     */
	enum EDestroyMode : uint8_t
	{
		KEEP
        , DELETE_KEEP_HEAD /* if damaged but maint. code shall find the traces laster */
        , TRUNCATE
        , ABANDONED /* similar to DELETE but head might be gone already might gone already */
        , DELETE
    };

	fileitem(string_view sPathRel);
	virtual ~fileitem() =default;
	
	// initialize file item, return the status
    virtual FiStatus Setup() { return FIST_DLERROR; };
	
	virtual unique_fd GetFileFd();
	uint64_t TakeTransferCount();
	uint64_t GetTransferCountUnlocked() { return m_nIncommingCount; }
	// send helper like wrapper for sendfile. Just declare virtual here to make it better customizable later.
	virtual ssize_t SendData(int confd, int filefd, off_t &nSendPos, size_t nMax2SendNow)
	{
		(void) confd; (void) filefd; (void) nSendPos; (void) nMax2SendNow;
		return -1;
	};

	FiStatus GetStatus() { setLockGuard; return m_status; }
	FiStatus GetStatusUnlocked(off_t &nGoodDataSize) { nGoodDataSize = m_nSizeChecked; return m_status; }
    FiStatus GetStatusUnlocked() { return m_status; }

//	//! returns true if complete or DL not started yet but partial file is present and contains requested range and file contents is static
//	bool CheckUsableRange_unlocked(off_t nRangeLastByte);

	// returns when the state changes to complete or error
    std::pair<FiStatus, tRemoteStatus> WaitForFinish();

	/**
	 * @brief WaitForFinish with timeout reporting and feedback.
	 * Timeout is only a recommendation, the check function might be called sooner and multiple times.
	 * @param timeout Interval to wait until timeout
	 * @param cbOnTimeout User function, reporting true to keep waiting or false to abort
	 * @return Last seen file item state and reported remote status
	 */
	std::pair<FiStatus, tRemoteStatus> WaitForFinish(unsigned timeout, const std::function<bool()> &onWaitInterrupted);
	
	/// mark the item as complete as-is, assuming that seen size is correct
	void SetupComplete();

    void UpdateHeadTimestamp();

    bool IsVolatile() { return m_spattr.bVolatile; }
    bool IsHeadOnly() { return m_spattr.bHeadOnly; }
    off_t GetRangeLimit() { return m_spattr.nRangeLimit; }

	uint64_t m_nIncommingCount = 0;

	// whatever we found in the cached data initially
    off_t m_nSizeCachedInitial = -1;
    // initially from the file data, then replaced when download has started
    off_t m_nContentLength = -1;

    tRemoteStatus m_responseStatus;
    /** This member has multiple uses; for 302/304 codes, it contains the Location value */
    mstring m_responseOrigin;
    // trade-off between unneccessary parsing and on-the-heap storage
    tHttpDate m_responseModDate;

    string_view m_contentType = "octet/stream";

protected:

	bool m_bPreallocated = false;
	/**
	 * The item is usable but data might be fishy, so that file must be removed/replaced on opening.
	 */
	bool m_bWriterMustReplaceFile = false;
	/**
	 * Such item can exist as long as it's used by one client, new creators
	 * for this location must get it out of the way.
	 */
	bool m_bCreateItemMustDisplace = false;

	unsigned m_nDlRefsCount = 0;

    tSpecialPurposeAttr m_spattr;

	off_t m_nSizeChecked = -1;
	std::atomic<int> usercount = ATOMIC_VAR_INIT(0);
	FiStatus m_status = FIST_FRESH;
	EDestroyMode m_eDestroy = EDestroyMode::KEEP;
	mstring m_sPathRel;
	time_t m_nTimeDlStarted = 0;

	/*************************************
	 *
	 * Dl* methods are used by the downloader. All access must happen in locked mode.
	 *
	 *************************************/

	/**
	 * Mark the beginning of a download, with a header to be consumed and
	 * stored. Responsible to update m_nSizeChecked and m_status accordingly.
	 *
	 * Fileitem must be locked before.
	 *
	 * @param h Already preprocessed header
     * @param rawHeader Original header contents as memory chunk
     * @param semiRawHeader The incoming processing header which was already partly processed for the remaining parameters
	 * @return true when accepted
	 */
    virtual bool DlStarted(string_view rawHeader, const tHttpDate& modDate, cmstring& origin, tRemoteStatus status, off_t bytes2seek, off_t bytesAnnounced);
	/**
	* @return false to abort processing (report error)
	*
	* Fileitem must be locked before by unique lock pointed by uli object.
	*
	*/
	virtual bool DlAddData(string_view, lockuniq&)  { return false;};
	/**
	 * @brief Mark the download as finished, and verify that sizeChecked as sane at that moment or move to error state.
	 */
	virtual void DlFinish(bool forceUpdateHeader);

	/**
	 * @brief Mark this item as defect, optionally so that its data will be invalidated in cache when released
	 *
	 * @param destroyMode Decides the future of data existing in the cache
	 */

	virtual void DlSetError(const tRemoteStatus& errState, EDestroyMode destroyMode);

	// flag for shared objects and a self-reference for fast and exact deletion, together with m_globRef
	std::weak_ptr<IFileItemRegistry> m_owner;
	tFiGlobMap::iterator m_globRef;

	friend class TFileItemHolder;

	// callback to store the header data on disk, if implemented
	virtual bool SaveHeader(bool) { return false; }

public:
	/// public proxy to DlSetError with truncation, locking!!
	void MarkFaulty(bool deleteItCompletely = false);
	/// optional method, returns raw header if needed in special implementations
	virtual const std::string& GetRawResponseHeader() { return sEmptyString; }

	virtual void DlRefCountAdd();
	virtual void DlRefCountDec(const tRemoteStatus& reason);

};

enum class ESharingHow
{
	ALWAYS_TRY_SHARING,
	AUTO_MOVE_OUT_OF_THE_WAY,
	FORCE_MOVE_OUT_OF_THE_WAY
};

// dl item implementation with storage on disk
class fileitem_with_storage : public fileitem
{
public:
	inline fileitem_with_storage(cmstring &s) : fileitem(s) {}
    virtual ~fileitem_with_storage();

    FiStatus Setup() override;

	// send helper like wrapper for sendfile. Just declare virtual here to make it better customizable later.
	virtual ssize_t SendData(int confd, int filefd, off_t &nSendPos, size_t nMax2SendNow) override;

	static mstring NormalizePath(cmstring &sPathRaw);

protected:
	void MoveRelease2Sidestore();
	int m_filefd = -1;

	bool DlAddData(string_view chunk, lockuniq&) override;

	bool withError(string_view message, fileitem::EDestroyMode destruction
			= fileitem::EDestroyMode::KEEP);

	bool SaveHeader(bool truncatedKeepOnlyOrigInfo) override;
private:
	bool SafeOpenOutFile();
};


}
#endif
