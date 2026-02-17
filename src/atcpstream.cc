#include "atcpstream.h"
#include "ahttpurl.h"
#include "portutils.h"
#include "acfg.h"
#include "evabase.h"
#include "aconnect.h"
#include "fileio.h"

#include <map>

#include <event.h>
#include <event2/bufferevent.h>

using namespace std;

namespace acng {

class atcpstreamImpl;
multimap<string, lint_ptr<atcpstreamImpl>> g_con_cache;
#define CACHE_SIZE_MAX 42
void cbCachedKill(struct bufferevent *bev, short what, void *ctx);

class atcpstreamImpl : public atcpstream
{
	bufferevent* m_buf = nullptr;
	mstring sHost;
	uint16_t nPort;
	bool m_bProxyConnected;

	decltype (g_con_cache)::iterator m_cleanIt;

public:
	atcpstreamImpl(string host, uint16_t port, bool isProxy) :
		sHost(move(host)),
		nPort(port),
		m_bProxyConnected(isProxy)
	{
	}
	~atcpstreamImpl()
	{
		if (m_buf)
			bufferevent_free(m_buf);
	}
	// mooth-ball operations for storing in the cache
	void Hibernate(decltype (m_cleanIt) cacheRef)
	{
		if (!m_buf)
			return;
		m_cleanIt = cacheRef;
		bufferevent_setcb(m_buf, nullptr, nullptr, cbCachedKill, this);
		bufferevent_set_timeouts(m_buf, cfg::GetNetworkTimeout(), nullptr);
		bufferevent_enable(m_buf, EV_READ);
	}
	// restore operation after hibernation
	void Reuse()
	{
		bufferevent_disable(m_buf, EV_READ | EV_WRITE);
		bufferevent_setcb(m_buf, nullptr, nullptr, nullptr, this);
		m_cleanIt = g_con_cache.end();
	}

	void SetEvent(bufferevent *be) { m_buf = be; }

	// atcpstream interface
	bufferevent *GetEvent() override { return m_buf; }
	const string &GetHost() override { return sHost; }
	uint16_t GetPort() override { return nPort; }
	bool PeerIsProxy() override { return m_bProxyConnected; }
};

struct tPostConnOps : public tLintRefcounted
{
	atcpstream::tCallBack m_cback;
	mstring m_sHost;
	uint16_t m_nPort;
	bool m_bThroughProxy;
	int m_fd;
	~tPostConnOps()
	{
		checkforceclose(m_fd);
	}
	tPostConnOps(atcpstream::tCallBack cback, mstring sHost, uint16_t nPort, bool bThroughProxy, int fd)
		: m_cback(cback), m_sHost(sHost), m_nPort(nPort), m_bThroughProxy(bThroughProxy), m_fd(fd)
	{
	}
	void Go();
};

void cbCachedKill(struct bufferevent *, short , void *ctx)
{
	auto pin = as_lptr((atcpstreamImpl*) ctx);
	auto key = makeHostPortKey(pin->GetHost(), pin->GetPort());
	// XXX: this is not really efficient, maybe store the iterator in the class?
	auto hit = g_con_cache.equal_range(key);
	for (auto it = hit.first; it != hit.second; ++it)
	{
		if (it->second.get() == pin.get())
		{
			g_con_cache.erase(it);
			return;
		}
	}
}

void atcpstream::Return(lint_ptr<atcpstream> &stream)
{
	if (g_con_cache.size() < CACHE_SIZE_MAX)
	{
		auto ptr = static_lptr_cast<atcpstreamImpl>(stream);
		auto it = g_con_cache.insert(make_pair(makeHostPortKey(ptr->GetHost(), ptr->GetPort()), ptr));
		ptr->Hibernate(it);
	}
	stream.reset();
}


void atcpstream::Create(const tHttpUrl &url, bool forceFresh, int forceTimeout, const tCallBack& cback)
{
	auto key = url.GetHostPortKey();
	if (!forceFresh)
	{
		auto anyIt = g_con_cache.find(key);
		if (anyIt != g_con_cache.end())
		{
			auto ret = anyIt->second;
			g_con_cache.erase(anyIt);
			ret->Reuse();
			return cback(static_lptr_cast<atcpstream>(ret), string_view(), false);
		}
	}
	evabase::Post([host = url.sHost, port = url.GetPort(), doSsl = url.bSSL,
				  forceTimeout, cback]
				  (bool cancled)
	{
		if (cancled)
			return cback(lint_ptr<atcpstream>(), "Operation canceled", true);
		auto proxy = cfg::GetProxyInfo();
		auto timeout = forceTimeout > 0 ?
					forceTimeout :
					( cfg::GetProxyInfo() ? cfg::optproxytimeout : cfg::nettimeout);

		aconnector::Connect(proxy ? proxy->sHost : host,
							proxy ? proxy->GetPort() : port,
							timeout,
							[cback, host, port, proxy, doSsl]
							(aconnector::tConnResult res) mutable
		{
			if (!res.sError.empty())
				return cback(lint_ptr<atcpstream>(), res.sError, true);
			int nFd = res.fd.release();
			atcpstreamImpl *p(nullptr);
			try
			{
				auto p = make_lptr<atcpstreamImpl>(host, port, proxy);
				if (proxy && doSsl)
				{
					// XXX: tPostConnOps setup and set callbacks for CONNECT call
					return;
				}
				else if (doSsl)
				{
					// XXX: tPostConnOps setup and set callbacks for SSL setup
					return;
				}
				else
				{
					p->SetEvent(bufferevent_new(nFd, nullptr, nullptr, nullptr, nullptr));
					return cback(static_lptr_cast<atcpstream>(p), string_view(), true);
				}
			}
			catch (...)
			{
			}

			if (p)
				delete(p);
			checkforceclose(nFd);

			//auto pin = make_lptr<streamConnectCtx>( );
		});
	});
}


}
