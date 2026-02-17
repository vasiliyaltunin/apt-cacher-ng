
#include "meta.h"
#include "sockio.h"
#include "debug.h"
#include "acfg.h"

#include <unordered_map>
#include <mutex>

namespace acng
{
using namespace std;

// those data structures are used by main thread only
// helper structure with metadata which can be passed around

struct tEndingSocketInfo
{
	time_t absoluteTimeout;
	event* me;
};

void termsocket_now(int fd)
{
	::shutdown(fd, SHUT_RDWR);
	justforceclose(fd);
}

const struct timeval * GetTimeoutInterval()
{
	static struct timeval t { 0, 42};
	// this should result in something sane, roughly 5-10s
	if (!t.tv_sec)
		t.tv_sec = 2 + cfg::discotimeout / 4;
	return &t;
}
bool readCheckDead(int fd, unsigned cycles)
{
	char crapbuf[40];
	while (cycles--)
	{
		int r = recv(fd, crapbuf, sizeof(crapbuf), 0);
		if (r > 0)
			continue;
		if (0 == r)
			return true;
		return errno != EINTR && errno != EAGAIN;
	}
	return false;
}
void cbLingerRead(int fd, short what, void *pRaw)
{
	auto info((tEndingSocketInfo*)pRaw);
	bool isDead = false;
	if(what & EV_READ)
		isDead = readCheckDead(fd, 200);
	if (!isDead)
		isDead = GetTime() > info->absoluteTimeout;

	if (isDead)
	{
		termsocket_now(fd);
		if(info->me)
			event_free(info->me);
		delete info;
	}
}

/*! \brief Helper to flush data stream contents reliable and close the connection then
 * DUDES, who write TCP implementations... why can this just not be done easy and reliable? Why do we need hacks like the method below?
 For details, see: http://blog.netherlabs.nl/articles/2009/01/18/the-ultimate-so_linger-page-or-why-is-my-tcp-not-reliable
 * Using SO_LINGER is also dangerous, see https://www.nybek.com/blog/2015/04/29/so_linger-on-non-blocking-sockets
 *
 */
void termsocket_async(int fd, event_base* base)
{
	tEndingSocketInfo* dsctor (nullptr);
	try
	{
		LOGSTARTsx("::termsocket", fd);
		if (fd == -1)
			return;
		// initiate shutdown, i.e. sending FIN and giving the remote some time to confirm
		::shutdown(fd, SHUT_WR);
		if (readCheckDead(fd, 1))
		{
			justforceclose(fd);
			return;
		}
		LOG("waiting for peer to react");
		dsctor = new tEndingSocketInfo { GetTime() + cfg::discotimeout, nullptr};
		dsctor->me = event_new(base, fd, EV_READ | EV_PERSIST, cbLingerRead, dsctor);
		if (dsctor && dsctor->me && 0 == event_add(dsctor->me, GetTimeoutInterval()))
			return; // will cleanup in the callbacks
	} catch (...)
	{
	}
	// error cleanup, normally unreachable
	if (dsctor)
	{
		if (dsctor->me)
			event_free(dsctor->me);
		delete dsctor;
	}
	termsocket_now(fd);
}

void set_connect_sock_flags(evutil_socket_t fd)
{
		evutil_make_socket_nonblocking(fd);
#ifndef NO_TCP_TUNNING
		int yes(1);
		::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
		::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
}

}
