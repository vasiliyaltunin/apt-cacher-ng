#include "aconnect.h"
#include "meta.h"
#include "acfg.h"
#include "sockio.h"
#include "evabase.h"
#include "debug.h"
#include "portutils.h"

#include <future>
#include <algorithm>

#include <sys/types.h>
#include <sys/socket.h>

#include <event.h>

using namespace std;

namespace acng
{

string dnsError("Unknown DNS error");


void aconnector::Connect(cmstring& target, uint16_t port, unsigned timeout, tCallback cbReport)
{
	auto exTime = GetTime() + timeout;
	evabase::Post([exTime, target, port, cbReport](bool canceled)
	{
		if (canceled)
			return cbReport({ unique_fd(), "Canceled" });
		auto o = new aconnector;
		o->m_tmoutTotal = exTime;
		o->m_cback = move(cbReport);
		// capture the object there
		CAddrInfo::Resolve(target, port, [o](std::shared_ptr<CAddrInfo> res)
		{
			o->processDnsResult(move(res));
		});
	});
}

aconnector::tConnResult aconnector::Connect(cmstring &target, uint16_t port, unsigned timeout)
{
	std::promise<aconnector::tConnResult> reppro;
	Connect(target, port, timeout, [&](aconnector::tConnResult res)
	{
		reppro.set_value(move(res));
	});
	return reppro.get_future().get();
}

void aconnector::processDnsResult(std::shared_ptr<CAddrInfo> res)
{
	LOGSTARTFUNCs;
	if (!res)
		return retError(dnsError);
	auto errDns = res->getError();
	if (!errDns.empty())
		return retError(errDns);
	dbgline;
	m_targets = res->getTargets();
	if (m_targets.empty())
		return retError(dnsError);

	step(-1, 0);
}

void aconnector::cbStep(int fd, short what, void *arg) {
	((aconnector*)arg)->step(fd, what);
}

void aconnector::step(int fd, short what)
{
	LOGSTARTFUNCx(fd, what);
	if (what & EV_WRITE)
	{
		// ok, some real socket became ready on connecting, let's doublecheck it
		int err;
		socklen_t optlen = sizeof(err);
		if(getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*) &err, &optlen) == 0)
		{
			if (!err)
				return retSuccess(fd);

			disable(fd, err);
		}
		else
		{
			disable(fd, errno);
		}
	}
	// okay, socket not usable, create next candicate connection?
	time_t now = GetTime();
	if (now > m_tmoutTotal)
		return retError("Connection timeout");

	auto isFirst = fd == -1;

	if (isFirst)
	{
		m_timeNextCand = now + cfg::fasttimeout;
		m_cursor = m_targets.begin();
	}
	else if (now >= m_timeNextCand)
	{
		// okay, arming the next candidate
		m_timeNextCand = now + cfg::GetFirstConTimeout()->tv_sec;
		dbgline;
	}

	// open attempt for the selected next candidate, or any valid which comes after
	for (; m_cursor != m_targets.end(); m_cursor++)
	{
		// to move into m_eventFds on success
		unique_fd nextFd;
		unique_event pe;

		nextFd.m_p = ::socket(m_cursor->ai_family, SOCK_STREAM, 0);
		if (!nextFd.valid())
		{
			if (m_error2report.empty())
				m_error2report = tErrnoFmter();
			continue;
		}

		set_connect_sock_flags(nextFd.get());

		for (unsigned i=0; i < 50; ++i)
		{
			auto res = connect(nextFd.get(), (sockaddr*) & m_cursor->ai_addr, m_cursor->ai_addrlen);
			if (res == 0)
				return retSuccess(nextFd.release());
			if (errno != EINTR)
				break;
		}
		if (errno != EINPROGRESS)
		{
			setIfNotEmpty(m_error2report, tErrnoFmter(errno))
					continue;
		}
		auto tmout = isFirst ? cfg::GetFirstConTimeout() : cfg::GetFurtherConTimeout();
		pe.m_p = event_new(evabase::base, nextFd.get(), EV_WRITE | EV_PERSIST, cbStep, this);
		if (AC_LIKELY(pe.valid() && 0 == event_add(pe.get(), tmout)))
		{
			m_eventFds.push_back({move(nextFd), move(pe)});
			m_pending++;
			// advance to the next after timeout
			m_cursor++;
			dbgline;
			return;
		}
		setIfNotEmpty(m_error2report, "Out of memory"sv);
	}
	// not success if got here, any active connection pending?
	if (m_pending == 0)
	{
		return retError(m_error2report.empty() ? tErrnoFmter(EAFNOSUPPORT) : m_error2report);
	}
	else
	{
		LOG("pending connections: " << m_pending);
	}
}

void aconnector::retError(mstring msg)
{
	LOGSTARTFUNCx(msg);
	m_cback({unique_fd(), move(msg)});
	delete this;
}

void aconnector::retSuccess(int fd)
{
	// doing destructors work already since we need to visit nodes anyway to find and extract the winner
	auto it = std::find_if(m_eventFds.begin(), m_eventFds.end(), [fd](auto& el){return el.fd.get() == fd;});
	if (it == m_eventFds.end())
		m_cback({unique_fd(), "Internal error"});
	else
	{
		it->ev.reset();
		m_cback({move(it->fd), sEmptyString});
	}
	delete this;
}

void aconnector::disable(int fd, int ec)
{
	LOGSTARTFUNCx(fd);
	ASSERT(fd != -1);
	auto it = std::find_if(m_eventFds.begin(), m_eventFds.end(), [fd](auto& el){return el.fd.get() == fd;});
	if (it == m_eventFds.end())
		return;
	// error from primary always wins, grab before closing it
	if (it == m_eventFds.begin())
		setIfNotEmpty(m_error2report, tErrnoFmter(ec));
	// stop observing and release resources
	if (it->ev.get())
		m_pending--;
	m_eventFds.erase(it);
}

}
