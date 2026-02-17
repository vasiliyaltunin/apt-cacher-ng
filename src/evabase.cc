#include "evabase.h"
#include "meta.h"
#include "debug.h"
#include "acfg.h"
#include "lockable.h"
#include "fileio.h"

#include <event2/util.h>

#ifdef HAVE_SD_NOTIFY
#include <systemd/sd-daemon.h>
#endif

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ares.h>

using namespace std;

#define DNS_ABORT_RETURNING_ERROR 1

//XXX: add an extra task once per hour or so, optimizing all caches

namespace acng
{

event_base* evabase::base = nullptr;
std::shared_ptr<CDnsBase> cachedDnsBase;
struct tResolvConfStamp
{
	dev_t fsId;
	ino_t fsInode;
	timespec changeTime;
} cachedDnsFingerprint { 0, 0, { 0, 1 } };

std::atomic<bool> evabase::in_shutdown = ATOMIC_VAR_INIT(false);

struct event *handover_wakeup;
const struct timeval timeout_asap{0,0};
deque<evabase::tCancelableAction> incoming_q, processing_q;
mutex handover_mx;
void RejectPendingDnsRequests();

namespace conserver
{
// forward declarations for the pointer checks
//void cb_resume(evutil_socket_t fd, short what, void* arg);
void do_accept(evutil_socket_t server_fd, short what, void* arg);
}

/**
 * Forcibly run each callback and signal shutdown.
 */
int collect_event_info(const event_base*, const event* ev, void* ret)
{
	t_event_desctor r;
	event_base *nix;
	short what;
	auto lret((deque<t_event_desctor>*)ret);
	event_get_assignment(ev, &nix, &r.fd, &what, &r.callback, &r.arg);
	lret->emplace_back(move(r));
	return 0;
}
struct tShutdownAction
{
	event_callback_fn filter_cb_ptr;
	std::function<void(t_event_desctor)> action;
};

std::vector<tShutdownAction> shutdownActions;

void evabase::addTeardownAction(event_callback_fn matchedCback, std::function<void (t_event_desctor)> action)
{
	shutdownActions.emplace_back( tShutdownAction {matchedCback, action});
}

CDnsBase::~CDnsBase()
{
	shutdown();
}

void cb_sync_ares(evutil_socket_t, short, void* arg)
{
	// who knows what it has done with its FDs, simply recreating them all to be safe
	auto p=(CDnsBase*) arg;
	p->dropEvents();
	p->setupEvents();
}

void cb_ares_action(evutil_socket_t fd, short what, void* arg)
{
	auto p=(CDnsBase*) arg;
	if (what&EV_TIMEOUT)
		ares_process_fd(p->get(), ARES_SOCKET_BAD, ARES_SOCKET_BAD);
	else
	{
		auto toread = (what&EV_READ) ? fd : ARES_SOCKET_BAD;
		auto towrite = (what&EV_WRITE) ? fd : ARES_SOCKET_BAD;
		ares_process_fd(p->get(), toread, towrite);
	}
	// need to run another cycle asap
	p->sync();
}

void CDnsBase::sync()
{
	if (!m_aresSyncEvent)
		m_aresSyncEvent = evtimer_new(evabase::base, cb_sync_ares, this);
	event_add(m_aresSyncEvent, &timeout_asap);
}

void CDnsBase::dropEvents()
{
	for (auto& el: m_aresEvents)
	{
		if (el)
			event_free(el);
	}
	m_aresEvents.clear();
}

void CDnsBase::setupEvents()
{
	ASSERT(m_channel);
	if (!m_channel)
		return;
	ares_socket_t socks[ARES_GETSOCK_MAXNUM];
	auto bitfield = ares_getsock(m_channel, socks, _countof(socks));
	struct timeval tvbuf;
	auto tmout = ares_timeout(m_channel, nullptr, &tvbuf);
	for(unsigned i = 0; i < ARES_GETSOCK_MAXNUM; ++i)
	{
		short what(0);
		if (ARES_GETSOCK_READABLE(bitfield, i))
			what = EV_READ;
		else if (ARES_GETSOCK_WRITABLE(bitfield, i))
			what = EV_WRITE;
		else
			continue;
		m_aresEvents.emplace_back(event_new(evabase::base, socks[i], what, cb_ares_action, this));
		event_add(m_aresEvents.back(), tmout);
	}
}

void CDnsBase::shutdown()
{
	if (m_channel)
	{
		// Defer this action so it's not impacting the callback processing somewhere
		evabase::Post([chan = m_channel](bool) {
			// graceful DNS resolver shutdown
			ares_destroy(chan);
		});
	}
	dropEvents();
	if (m_aresSyncEvent)
		event_free(m_aresSyncEvent), m_aresSyncEvent = nullptr;

	m_channel = nullptr;
}

std::shared_ptr<CDnsBase> evabase::GetDnsBase()
{
	return cachedDnsBase;
}

void evabase::CheckDnsChange()
{
	Cstat info(cfg::dnsresconf);
	if (!info) // file is missing anyway?
		return;

	if (cachedDnsFingerprint.changeTime.tv_sec == info.st_mtim.tv_sec
			&& cachedDnsFingerprint.changeTime.tv_nsec == info.st_mtim.tv_nsec
			&& cachedDnsFingerprint.fsId == info.st_dev
			&& cachedDnsFingerprint.fsInode == info.st_ino)
	{
		// still the same
		return;
	}

	ares_channel newDnsBase;
	switch(ares_init(&newDnsBase))
	{
	case ARES_SUCCESS:
		break;
	case ARES_EFILE:
		log::err("DNS system error, cannot read config file");
		return;
	case ARES_ENOMEM:
		log::err("DNS system error, out of memory");
		return;
	case ARES_ENOTINITIALIZED:
		log::err("DNS system error, faulty initialization sequence");
		return;
	default:
		log::err("DNS system error, internal error");
		return;
	}
	// ok, found new configuration and it can be applied
	if (cachedDnsBase)
		cachedDnsBase->shutdown();
	cachedDnsBase.reset(new CDnsBase(newDnsBase));
	cachedDnsFingerprint = tResolvConfStamp
	{ info.st_dev, info.st_ino, info.st_mtim };
}

ACNG_API int evabase::MainLoop()
{
	LOGSTARTFUNCs;

	CheckDnsChange(); // init DNS base

#ifdef HAVE_SD_NOTIFY
	sd_notify(0, "READY=1");
#endif

	int r = event_base_loop(evabase::base, EVLOOP_NO_EXIT_ON_EMPTY);

	auto push_loop = [eb = evabase::base]() {
		// push the loop a few times to make sure that the state change
		// is propagated to the background threads
		for (int i = 10; i >= 0; --i)
		{
			// if error or nothing more to do...
			if (0 != event_base_loop(eb, EVLOOP_NONBLOCK))
				break;
		}
	};

	in_shutdown = true;

	// try to shutdown DNS stuff with a nicer error message
	cachedDnsBase->shutdown();
	cachedDnsBase.reset();
	// make sure that there are no actions from abandoned DNS bases blocking the futures
	RejectPendingDnsRequests();
	push_loop();

	// send teardown hint to all event callbacks
	deque<t_event_desctor> todo;
	event_base_foreach_event(evabase::base, collect_event_info, &todo);
	for (const auto &ptr : todo)
	{
		for(auto& ac: shutdownActions)
		{
			if (ac.filter_cb_ptr == ptr.callback && ac.action)
				ac.action(ptr);
		}
	}

	push_loop();

#ifdef HAVE_SD_NOTIFY
	sd_notify(0, "READY=0");
#endif
	return r;
}

void evabase::SignalStop()
{
	Post([](bool)
	{
		if(evabase::base)
		event_base_loopbreak(evabase::base);
	});
}

void cb_handover(evutil_socket_t, short, void*)
{
	{
		lockguard g(handover_mx);
		processing_q.swap(incoming_q);
	}
	for(const auto& ac: processing_q)
		ac(evabase::in_shutdown);
	processing_q.clear();
}

void evabase::Post(tCancelableAction&& act)
{
	{
		lockguard g(handover_mx);
		incoming_q.emplace_back(move(act));
	}
	ASSERT(handover_wakeup);
	event_add(handover_wakeup, &timeout_asap);
}

evabase::evabase()
{
	evabase::base = event_base_new();
	handover_wakeup = evtimer_new(base, cb_handover, nullptr);
}

evabase::~evabase()
{
	if(evabase::base)
	{
		event_base_free(evabase::base);
		evabase::base = nullptr;
	}
}


}
