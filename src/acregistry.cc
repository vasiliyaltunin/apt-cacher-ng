#include "acregistry.h"
#include "debug.h"
#include "meta.h"
#include "acfg.h"
#include "cleaner.h"
#include "evabase.h"

#include <list>
#include <algorithm>

#define IN_ABOUT_ONE_DAY 100000

using namespace std;

namespace acng
{

class TFileItemRegistry : public IFileItemRegistry, public enable_shared_from_this<TFileItemRegistry>
{
		// IFileItemRegistry interface

		tFiGlobMap mapItems;

		struct TExpiredEntry
		{
				TFileItemHolder hodler;
				time_t timeExpired;
		};

		std::list<TExpiredEntry> prolongedLifetimeQ;
		acmutex prolongMx;
public:
		TFileItemHolder Create(cmstring &sPathUnescaped, ESharingHow how, const fileitem::tSpecialPurposeAttr &spattr) override;
		TFileItemHolder Create(tFileItemPtr spCustomFileItem, bool isShareable) override;
		time_t BackgroundCleanup() override;
		void dump_status() override;
		void AddToProlongedQueue(TFileItemHolder &&, time_t expTime) override;

		// IFileItemRegistry interface
public:
		void Unreg(fileitem& item) override
		{
				mapItems.erase(item.m_globRef);
				item.m_globRef = mapItems.end();
				item.m_owner.reset();
		}
};

void SetupServerItemRegistry()
{
	g_registry = std::make_shared<TFileItemRegistry>();
}

void TeardownServerItemRegistry()
{
	g_registry.reset();
}

TFileItemHolder::~TFileItemHolder()
{
	LOGSTARTFUNC;
	if (!m_ptr) // unregistered before? or not shared?
		return;

	auto local_ptr(m_ptr); // might disappear

	lockuniq mangerLock;
	auto manger = local_ptr->m_owner.lock();
	if (manger)
		mangerLock.assign(*manger, true);

	lockguard fitemLock(*local_ptr);

	if ( -- m_ptr->usercount > 0)
		return; // still in active use

	local_ptr->notifyAll();

#ifdef DEBUG
	if (m_ptr->m_status > fileitem::FIST_INITED &&
			m_ptr->m_status < fileitem::FIST_COMPLETE)
	{
		LOG(mstring("users gone while downloading?: ") + ltos(m_ptr->m_status));
	}
#endif

	// some file items will be held ready for some time
	if (manger
			&& !evabase::in_shutdown
			&& acng::cfg::maxtempdelay
			&& m_ptr->IsVolatile()
			&& m_ptr->m_status == fileitem::FIST_COMPLETE)
	{
		auto now = GetTime();
		auto expDate = time_t(acng::cfg::maxtempdelay)
				+ m_ptr->m_nTimeDlStarted ? m_ptr->m_nTimeDlStarted : now;

		if (expDate > now)
		{
			if (manger)
			{
				local_ptr->usercount++;
				manger->AddToProlongedQueue(TFileItemHolder(local_ptr), expDate);
			}
			return;
		}
	}

	// nothing, let's put the item into shutdown state
	if (m_ptr->m_status < fileitem::FIST_COMPLETE)
		m_ptr->m_status = fileitem::FIST_DLSTOP;
	m_ptr->m_responseStatus.msg = "Cache file item expired";
	m_ptr->m_responseStatus.code = 500;
	if (manger)
	{
		LOG("*this is last entry, deleting dl/fi mapping");
		manger->Unreg(*local_ptr);
	}

	// make sure it's not double-unregistered accidentally!
	m_ptr.reset();
}

void TFileItemRegistry::AddToProlongedQueue(TFileItemHolder&& p, time_t expTime)
{
		lockguard g(prolongMx);
		// act like the item is still in use
		prolongedLifetimeQ.emplace_back(TExpiredEntry {move(p), expTime});
		cleaner::GetInstance().ScheduleFor(prolongedLifetimeQ.front().timeExpired,
																		   cleaner::TYPE_EXFILEITEM);
}

TFileItemHolder TFileItemRegistry::Create(cmstring &sPathUnescaped, ESharingHow how, const fileitem::tSpecialPurposeAttr& spattr)
{
	LOGSTARTFUNCxs(sPathUnescaped, int(how));

	try
	{
		mstring sPathRel(fileitem_with_storage::NormalizePath(sPathUnescaped));
		lockguard lockGlobalMap(this);
		LOG("Normalized: " << sPathRel );
		auto regnew = [&]()
		{
			LOG("Registering as NEW file item...");
			auto sp(make_shared<fileitem_with_storage>(sPathRel));
			sp->usercount++;
			sp->m_spattr = spattr;
			auto res = mapItems.emplace(sPathRel, sp);
			ASSERT(res.second);

			sp->m_owner = shared_from_this();
			sp->m_globRef = res.first;

			return TFileItemHolder(sp);
		};

		auto it = mapItems.find(sPathRel);
		if (it == mapItems.end())
			return regnew();

		auto &fi = it->second;

		auto share = [&]()
		{
			LOG("Sharing existing file item");
			it->second->usercount++;
			return TFileItemHolder(it->second);
		};

		lockguard g(*fi);

		if (how == ESharingHow::ALWAYS_TRY_SHARING || fi->m_bCreateItemMustDisplace)
			return share();

		// detect items that got stuck somehow and move it out of the way
		auto now(GetTime());
		auto makeWay = false;
		if (how == ESharingHow::FORCE_MOVE_OUT_OF_THE_WAY)
			makeWay = true;
		else
		{
			dbgline;
			makeWay = now > (fi->m_nTimeDlStarted + cfg::stucksecs);
			// check the additional conditions for being perfectly identical
			if (!makeWay && fi->IsVolatile() != spattr.bVolatile)
			{
				// replace if previous was working in solid mode because it does less checks
				makeWay = ! fi->IsVolatile();
			}
			if (!makeWay && spattr.bHeadOnly != fi->IsHeadOnly())
			{
				// one is HEAD-only request, the other not, keep the variant in the index which gets more data, the other becomes disposable
				dbgline;
				if (fi->IsHeadOnly())
					makeWay = true;
				else
				{
					// new item is head-only but this is almost worthless, so the existing one wins and new one goes to the side track
					auto altAttr = spattr;
					altAttr.bNoStore = true;
					LOG("Creating as NOSTORE HEADONLY file item...");
					auto sp(make_shared<fileitem>(sPathRel));
					sp->usercount++;
					sp->m_spattr = altAttr;
					return TFileItemHolder(sp);
				}
			}
			if (!makeWay && spattr.nRangeLimit != fi->GetRangeLimit())
			{
				dbgline;
				makeWay = true;
			}
			// XXX: TODO - add validation when remote credentials are supported
		}
		if (!makeWay)
			return share();

		// okay, have to move a probably existing cache file out of the way,
		// therefore needing this evasive maneuver
		auto replPathRel = fi->m_sPathRel + "." + ltos(now);
		auto replPathAbs = SABSPATH(replPathRel);
		auto pathAbs = SABSPATH(fi->m_sPathRel);

		// XXX: this check is crap and cannot happen but better double-check!
		if (AC_UNLIKELY(fi->m_sPathRel.empty()))
			return TFileItemHolder();

		auto abandon_replace = [&]() {
			fi->m_sPathRel = replPathAbs;
			fi->m_eDestroy = fileitem::EDestroyMode::ABANDONED;

			fi->m_globRef = mapItems.end();
			fi->m_owner.reset();

			mapItems.erase(it);
			return regnew();
		};

		if(0 == link(pathAbs.c_str(), replPathAbs.c_str()))
		{
			// only if it was actually there!
			if (0 == unlink(pathAbs.c_str()) || errno != ENOENT)
				return abandon_replace();
			else // unlink failed but file was there
				USRERR("Failure to erase stale file item for "sv << pathAbs << " - errno: "sv << tErrnoFmter());
		}
		else
		{
			if (ENOENT == errno) // XXX: replPathAbs doesn't exist but ignore for now
				return abandon_replace();

			USRERR("Failure to move file "sv << pathAbs << " out of the way or cannot create "sv  << replPathAbs << " - errno: "sv << tErrnoFmter());
		}
	}
	catch (std::bad_alloc&)
	{
	}
	return TFileItemHolder();
}

// make the fileitem globally accessible
TFileItemHolder TFileItemRegistry::Create(tFileItemPtr spCustomFileItem, bool isShareable)
{
	LOGSTARTFUNCxs(spCustomFileItem->m_sPathRel);

	TFileItemHolder ret;

	if (!spCustomFileItem || spCustomFileItem->m_sPathRel.empty())
		return ret;

	dbgline;
	if(!isShareable)
	{
		ret.m_ptr = spCustomFileItem;
	}

	lockguard lockGlobalMap(this);

	dbgline;
	auto installed = mapItems.emplace(spCustomFileItem->m_sPathRel,
									  spCustomFileItem);

	if(!installed.second)
		return ret; // conflict, another agent is already active
	dbgline;
	spCustomFileItem->m_globRef = installed.first;
	spCustomFileItem->m_owner = shared_from_this();
	spCustomFileItem->usercount++;
	ret.m_ptr = spCustomFileItem;
	return ret;
}

// this method is supposed to be awaken periodically and detects items with ref count manipulated by
// the request storm prevention mechanism. Items shall be be dropped after some time if no other
// thread but us is using them.
time_t TFileItemRegistry::BackgroundCleanup()
{
	auto now = GetTime();
	LOGSTARTFUNCsx(now);
	// where the destructors eventually do their job on stack unrolling
	decltype(prolongedLifetimeQ) releasedQ;
	lockguard g(prolongMx);
	if (prolongedLifetimeQ.empty())
		return END_OF_TIME;
	auto notExpired = std::find_if(prolongedLifetimeQ.begin(), prolongedLifetimeQ.end(),
								   [now](const TExpiredEntry &el) {	return el.timeExpired > now;});
	// grab all before expired element, or even all
	releasedQ.splice(releasedQ.begin(), prolongedLifetimeQ, prolongedLifetimeQ.begin(), notExpired);
	return prolongedLifetimeQ.empty() ? END_OF_TIME : prolongedLifetimeQ.front().timeExpired;
}


void TFileItemRegistry::dump_status()
{
	tSS fmt;
	log::err("File descriptor table:\n");
	for(const auto& item : mapItems)
	{
		fmt.clear();
		fmt << "FREF: " << item.first << " [" << item.second->usercount << "]:\n";
		if(! item.second)
		{
			fmt << "\tBAD REF!\n";
			continue;
		}
		else
		{
			fmt << "\t" << item.second->m_sPathRel
				<< "\n\tDlRefCount: " << item.second->m_nDlRefsCount
				<< "\n\tState: " << (int)  item.second->m_status
				<< "\n\tFilePos: " << item.second->m_nIncommingCount << " , "
					//<< item.second->m_nRangeLimit << " , "
				<< item.second->m_nSizeChecked << " , "
					<< item.second->m_nSizeCachedInitial
					<< "\n\tGotAt: " << item.second->m_nTimeDlStarted << "\n\n";
		}
		log::err(fmt);
	}
	log::flush();
}

}
