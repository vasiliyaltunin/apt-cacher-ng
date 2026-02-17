#ifndef __EVABASE_H__
#define __EVABASE_H__

#include "config.h"
#include "actemplates.h"
#include <memory>
#include <functional>

#include <event.h>

extern "C"
{
// XXX: forward-declaring only to avoid including ares.h right here; maybe can still do that and use ares_channel typedef directly.
struct ares_channeldata;
}

namespace acng
{

struct CDnsBase : public std::enable_shared_from_this<CDnsBase>
{
	ares_channeldata* get() const { return m_channel; }
	void shutdown();
	~CDnsBase();
	bool Init();

	// ares helpers
	void sync();
	void dropEvents();
	void setupEvents();


private:
	friend class evabase;
	ares_channeldata* m_channel = nullptr;
	//	void Deinit();
	CDnsBase(decltype(m_channel) pBase) : m_channel(pBase) {}

	// activated when we want something from ares or ares from us
	event *m_aresSyncEvent = nullptr;
	std::vector<event*> m_aresEvents;
};

struct t_event_desctor {
	evutil_socket_t fd;
	event_callback_fn callback;
	void *arg;
};

/**
 * This class is an adapter for general libevent handling, roughly fitting it into conventions of the rest of ACNG.
 * Partly static and partly dynamic, for pure convenience! Expected to be a singleton anyway.
 */
class ACNG_API evabase
{
public:
static event_base *base;
static std::atomic<bool> in_shutdown;

static std::shared_ptr<CDnsBase> GetDnsBase();
static void CheckDnsChange();

/**
 * Runs the main loop for a program around the event_base loop.
 * When finished, clean up some resources left behind (fire off specific events
 * which have actions that would cause blocking otherwise).
 */
int MainLoop();

static void SignalStop();

using tCancelableAction = std::function<void(bool)>;

/**
 * Push an action into processing queue. In case operation is not possible, runs the action with the cancel flag (bool argument set to true)
 */
static void Post(tCancelableAction&&);

static void addTeardownAction(event_callback_fn matchedCback, std::function<void(t_event_desctor)> action);

evabase();
~evabase();
};

}

#endif
