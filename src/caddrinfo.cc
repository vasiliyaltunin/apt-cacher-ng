#include "meta.h"

#include <deque>
#include <memory>
#include <list>
#include <unordered_map>
#include <future>
#include <algorithm>

#include "caddrinfo.h"
#include "sockio.h"
#include "acfg.h"
#include "debug.h"
#include "lockable.h"
#include "evabase.h"
#include "acsmartptr.h"
#include "ahttpurl.h"
#include "portutils.h"

#include <ares.h>
#include <arpa/nameser.h>

using namespace std;

/**
 * The CAddrInfo has multiple jobs:
 * - cache the resolution results for a specific time span
 * - in case of parallel ongoing resolution requests, coordinate the wishes so that the result is reused (and all callers are notified)
 */

namespace acng
{
#define DNS_MAX_PER_REQUEST 5
static const unsigned DNS_CACHE_MAX = 255;
static const unsigned DNS_ERROR_KEEP_MAX_TIME = 10;
static const unsigned MAX_ADDR = 10;
static const string dns_error_status_prefix("DNS error, ");
// this shall remain global and forever, for last-resort notifications
auto fatalInternalError = std::make_shared<CAddrInfo>("Fatal Internal DNS error"sv);

#define MOVE_FRONT_THERE_TO_BACK_HERE(from, to) to.emplace_back(from.front()), from.pop_front()

std::string acng_addrinfo::formatIpPort(const sockaddr *pAddr, socklen_t addrLen, int ipFamily)
{
	char buf[300], pbuf[30];
	getnameinfo(pAddr, addrLen, buf, sizeof(buf), pbuf, sizeof(pbuf),
			NI_NUMERICHOST | NI_NUMERICSERV);
	return string(ipFamily == PF_INET6 ? "[" : "") +
			buf +
			(ipFamily == PF_INET6 ? "]" : "") +
			":" + pbuf;
}

bool acng_addrinfo::operator==(const acng_addrinfo &other) const
{
	return this == &other ||
			(other.ai_addrlen == ai_addrlen &&
			 other.ai_family == ai_family &&
			 memcmp(&ai_addr, &other.ai_addr, ai_addrlen) == 0);
}

acng::acng_addrinfo::operator mstring() const {
	return formatIpPort((const sockaddr *) &ai_addr, ai_addrlen, ai_family);
}

string makeHostPortKey(const string & sHostname, uint16_t nPort)
{
	string ret;
	ret.reserve(sHostname.length()+2);
	ret += char(nPort);
	ret += char(nPort>>8);
	ret += sHostname;
	return ret;
}

// using non-ordered map because of iterator stability, needed for expiration queue;
// something like boost multiindex would be more appropriate if there was complicated
// ordering on expiration date but that's not the case; OTOH could also use a multi-key
// index instead of g_active_resolver_index but then again, when the resolution is finished,
// that key data becomes worthless.
map<string,CAddrInfoPtr> dns_cache;
deque<decltype(dns_cache)::iterator> dns_exp_q;
using unique_aresinfo = auto_raii<struct ares_addrinfo*, ares_freeaddrinfo, nullptr>;

// descriptor of a running DNS lookup, passed around with libevent callbacks
struct tDnsResContext : public tLintRefcounted
{
	string sHost;
	uint16_t nPort;
	std::shared_ptr<CDnsBase> resolver;
	list<CAddrInfo::tDnsResultReporter> cbs;
	static map<string, tDnsResContext*> g_active_resolver_index;
	// holds results of each task and refcounter
	struct tResult
	{
		lint_ptr<tDnsResContext> ownerRef;
		int status = ARES_EBADFLAGS;
		unique_aresinfo results;
		tResult(const lint_ptr<tDnsResContext>& owner) : ownerRef(owner) {}
	};
	list<tResult> tasks;

	decltype(g_active_resolver_index)::iterator refMe;

	tDnsResContext(string sh, uint16_t np, CAddrInfo::tDnsResultReporter rep)
		: sHost(sh),
		  nPort(np),
		  resolver(evabase::GetDnsBase()),
		  cbs({move(rep)})
	{
		refMe = g_active_resolver_index.end();
	}
	/**
	  * Eventually commit the results to callers while releasing this.
	  */
	~tDnsResContext();

	void start()
	{
		ares_query(resolver->get(), (string("_http._tcp.") + sHost).c_str(),
				   ns_c_in, ns_t_srv, tDnsResContext::cb_srv_query, this);
		evabase::GetDnsBase()->sync();
	}

	void runCallbacks(SHARED_PTR<CAddrInfo> &res)
	{
		for(auto& el: cbs)
		{
			if (!el)
				continue;
			try
			{
				el(res);
			}
			catch (...) {}
			// el = CAddrInfo::tDnsResultReporter();
		}
		cbs.clear();
	}
	/**
	 * @brief Report an existing result object but first modify it into error
	 * @param Result object, expected to contain the results unless errMsg is set
	 * @param errMsg Drop result information, i.e. convert it to error report using this message
	 */
	void reportAsError(SHARED_PTR<CAddrInfo>& res, string_view errMsg)
	{
		res->m_sError = errMsg;
		res->m_sortedInfos.clear();
		runCallbacks(res);
	}
	void reportNewError(string_view errMsg)
	{
		auto res = make_shared<CAddrInfo>(errMsg);
		runCallbacks(res);
	}

	// C-style callback for the resolver
	static void cb_addrinfo(void *arg,
					   int status,
					   int timeouts,
					   struct ares_addrinfo *results);

	static void cb_srv_query(void *arg, int status,
							 int timeouts,
							 unsigned char *abuf,
							 int alen);
};

/**
 * Trash old entries and keep purging until there is enough space for at least one new entry.
 */
void CAddrInfo::clean_dns_cache()
{
	if(cfg::dnscachetime <= 0)
		return;
	auto now=GetTime();
	ASSERT(dns_cache.size() == dns_exp_q.size());
	while(dns_cache.size() >= DNS_CACHE_MAX-1
			|| (!dns_exp_q.empty() && dns_exp_q.front()->second->m_expTime >= now))
	{
		dns_cache.erase(dns_exp_q.front());
		dns_exp_q.pop_front();
	}
}

void tDnsResContext::cb_addrinfo(void *arg,
								 int status,
								 int /* timeouts */,
								 struct ares_addrinfo *results)
{
	auto taskCtx((tDnsResContext::tResult*)arg);
	// reseting refcount on the owning object too early can have side effects
	auto pin(move(taskCtx->ownerRef));
	taskCtx->results.m_p = results;
	taskCtx->status = status;
}

struct tWeightedSrvComp
{
	// a less-than b
	bool operator()(ares_srv_reply*& a, ares_srv_reply*&b)
	{
		if (a->priority < b->priority)
			return true;
		if (a->priority > b->priority)
			return false;
		return a->weight > b->weight;
	}
} comp;

void tDnsResContext::cb_srv_query(void *arg, int status, int, unsigned char *abuf, int alen)
{
	// comes in unreferenced, clear when leaving unless passed to other callbacks
	auto me = as_lptr((tDnsResContext*)arg);
	if (ARES_ECANCELLED == status || ARES_EDESTRUCTION == status)
		return me->reportNewError("Temporary DNS resolution error");
	if (ARES_ENOMEM == status)
		return me->reportNewError("Out of memory");

	auto invoke_getaddrinfo = [&] (cmstring& sHost)
	{
		static bool filter_specific = (cfg::conprotos[0] != PF_UNSPEC && cfg::conprotos[1] == PF_UNSPEC);
		static const ares_addrinfo_hints default_connect_hints =
		{
			// we provide plain port numbers, no resolution needed
			// also return only probably working addresses
			AI_NUMERICSERV | AI_ADDRCONFIG,
			filter_specific ? cfg::conprotos[0] : PF_UNSPEC,
			SOCK_STREAM, IPPROTO_TCP
		};
		ares_getaddrinfo(me->resolver->get(),
						 sHost.c_str(),
						 tPortFmter().fmt(me->nPort),
						 &default_connect_hints, tDnsResContext::cb_addrinfo,
						 & me->tasks.emplace_back(me));
	};
	deque<ares_srv_reply*> all;
	bool directResolvingIncluded = false;
	if (status == ARES_SUCCESS)
	{
		ares_srv_reply* pReply = nullptr;
		if (ARES_SUCCESS == ares_parse_srv_reply(abuf, alen, &pReply))
		{
			unsigned maxWeight = 1;
			// that's considering all weights from different priorities in the same way, assuming that too heavy differences won't hurt the precission afterwards
			for(auto p = pReply; p; p = p->next)
			{
				maxWeight = std::max((unsigned) p->weight, maxWeight);
				directResolvingIncluded |= (me->sHost == p->host);
			}
			// factor to bring the dimension of random() into our dimension so that in never overflows when scaling back
			auto scaleFac = (INT_MAX / maxWeight + 1);
			for(auto p = pReply; p; p = p->next)
			{
				p->weight *= (random() / scaleFac);
				if (p->host)
					all.emplace_back(p);
			}

			std::sort(all.begin(), all.end(), comp);
			auto consideredHosts = std::min(all.size(), size_t(DNS_MAX_PER_REQUEST));

			for (unsigned i = 0; i < consideredHosts; ++i)
			{
				invoke_getaddrinfo(all[i]->host);
			}
		}
		if (pReply)
		{
			ares_free_data(pReply);
		}
	}
	// no or bad SRV, or target not included among SRV targets -> add as last resort
	if (!directResolvingIncluded)
	{
		invoke_getaddrinfo(me->sHost);
	}
	me->resolver->sync();
}

map<string, tDnsResContext*> tDnsResContext::g_active_resolver_index;

tDnsResContext::~tDnsResContext()
{
	if (refMe != g_active_resolver_index.end())
		g_active_resolver_index.erase(refMe);

	try
	{
		auto ret = std::shared_ptr<CAddrInfo>(new CAddrInfo); // ctor is private
		tDtorEx invoke_cbs([&ret, this]() mutable
		{
			runCallbacks(ret);
			// ASAP, before releasing resolver
			//tasks.clear();
		});

		std::deque<acng_addrinfo> dedup, q4, q6;
		int errStatus = ARES_SUCCESS;

		for(const auto& taskCtx : tasks)
		{
			auto tres = taskCtx.results.get();
			if (errStatus == ARES_SUCCESS)
				errStatus = taskCtx.status;
			if (!tres)
			{
				continue;
			}

#ifdef DEBUG
			for (auto p = tres->nodes; p; p = p->ai_next)
				DBGQLOG("Resolved: " << acng_addrinfo::formatIpPort(p->ai_addr, p->ai_addrlen, p->ai_family));
#endif
			for (auto pCur = tres->nodes; pCur; pCur = pCur->ai_next)
			{
				if (pCur->ai_socktype != SOCK_STREAM || pCur->ai_protocol != IPPROTO_TCP)
					continue;

				acng_addrinfo svAddr(pCur);
				auto itExist = find(dedup.begin(), dedup.end(), svAddr);
				if (itExist != dedup.end())
					continue;
				dedup.emplace_back(svAddr);
			}
		}

#ifdef DEBUG
		for (auto& it: ret->m_sortedInfos)
			DBGQLOG("Refined: " << it);
#endif
		auto takeV4 = cfg::conprotos[0] == PF_INET || cfg::conprotos[0] == PF_UNSPEC
					  || cfg::conprotos[1] == PF_INET || cfg::conprotos[1] == PF_UNSPEC;
		auto takeV6 = cfg::conprotos[0] == PF_INET6 || cfg::conprotos[0] == PF_UNSPEC
					  || cfg::conprotos[1] == PF_INET6 || cfg::conprotos[1] == PF_UNSPEC;

		while (!dedup.empty() && (q4.size() + q6.size()) < MAX_ADDR)
		{
			if(takeV4 && dedup.front().ai_family == PF_INET)
				MOVE_FRONT_THERE_TO_BACK_HERE(dedup, q4);
			else if (takeV6 && dedup.front().ai_family == PF_INET6)
				MOVE_FRONT_THERE_TO_BACK_HERE(dedup, q6);
			else
				dedup.pop_front();
		}

		for(bool sel6 = cfg::conprotos[0] == PF_INET6 || cfg::conprotos[0] == PF_UNSPEC;
			q4.size() + q6.size() > 0;
			sel6 = !sel6)
		{
			if(sel6 && !q6.empty())
				MOVE_FRONT_THERE_TO_BACK_HERE(q6, ret->m_sortedInfos);
			else if (!sel6 && !q4.empty())
				MOVE_FRONT_THERE_TO_BACK_HERE(q4, ret->m_sortedInfos);
		}
		ASSERT(q4.empty() && q6.empty());

		if (ret->m_sortedInfos.empty())
		{
			switch (errStatus)
			{
			case ARES_ENOTIMP:
				reportAsError(ret, "Unsupported address family");
				return;
			case ARES_ENOTFOUND:
				reportAsError(ret, "Host not found");
				return;
			case ARES_ECANCELLED:
			case ARES_EDESTRUCTION:
				reportAsError(ret, "Temporary DNS resolution error");
				return;
			case ARES_ENOMEM: // similar but cache it for some time so things might improve
				reportAsError(ret, "Out of memory");
				return;
			default:
				// not critical, can keep this for a short time to avoid DNS storms
				ret->m_expTime = GetTime() + std::min(cfg::dnscachetime, (int) DNS_ERROR_KEEP_MAX_TIME);
				ret->m_sError = tErrnoFmter(errStatus);
				break;
			}
		}
		else
		{
			ret->m_expTime = GetTime() + cfg::dnscachetime;
		}

		if (cfg::dnscachetime > 0) // keep a copy for other users
		{
			ret->clean_dns_cache();
			auto newIt = dns_cache.emplace(makeHostPortKey(sHost, nPort), ret);
			dns_exp_q.push_back(newIt.first);
		}
		return;
	}
	catch(...) {}

	// this should be unreachable, and if it is, the report method should be harmless
	runCallbacks(fatalInternalError);
}

SHARED_PTR<CAddrInfo> CAddrInfo::Resolve(cmstring & sHostname, uint16_t nPort)
{
	promise<CAddrInfoPtr> reppro;
	auto reporter = [&reppro](CAddrInfoPtr result) { reppro.set_value(result); };
	Resolve(sHostname, nPort, move(reporter));
	auto res(reppro.get_future().get());
	return res ? res : fatalInternalError;
}
void CAddrInfo::Resolve(cmstring & sHostname, uint16_t nPort, tDnsResultReporter rep)
{
	// keep a reference on the dns base to extend its lifetime
	auto cb_invoke_dns_res = [sHostname, nPort, rep = move(rep)](bool canceled) mutable
	{
		LOGSTARTFUNCsx(sHostname);
		tDnsResContext *ctx = nullptr;

		try
		{
			if (AC_UNLIKELY(canceled || evabase::in_shutdown))
			{
				rep(make_shared<CAddrInfo>("system error"));
				return;
			}

			auto key = makeHostPortKey(sHostname, nPort);
			if(cfg::dnscachetime > 0)
			{
				auto caIt = dns_cache.find(key);
				if(caIt != dns_cache.end())
					return rep(caIt->second);
			}
			auto resIt = tDnsResContext::g_active_resolver_index.find(key);
			// join the waiting crowd, move all callbacks to there...
			if(resIt != tDnsResContext::g_active_resolver_index.end())
			{
				resIt->second->cbs.emplace_back(rep);
				rep = decltype (rep)();
				return;
			}

			// ok, this is fresh, invoke a completely new DNS lookup operation
			ctx = new tDnsResContext(sHostname, nPort, rep);
			rep = decltype (rep)();
			if (AC_UNLIKELY(!ctx || !ctx->resolver))
			{
				ctx->cbs.back()(make_shared<CAddrInfo>("503 Bad DNS configuration"));
				// and defuse the fallback caller
				ctx->cbs.clear();
				return;
			}
			ctx->refMe = tDnsResContext::g_active_resolver_index.emplace(key, ctx).first;
			ctx->start();
		}
		catch (const std::bad_alloc&)
		{
			if (ctx)
				delete ctx;

			if (rep)
				rep(fatalInternalError);
		}
	};

	evabase::Post(move(cb_invoke_dns_res));
}

void RejectPendingDnsRequests()
{
	for (auto& el: tDnsResContext::g_active_resolver_index)
	{
		if (el.second)
			el.second->reportNewError("System shutting down"sv);
	}
}

acng_addrinfo::acng_addrinfo(ares_addrinfo_node *src)
	: ai_family(src->ai_family), ai_addrlen(src->ai_addrlen)
{
	memcpy(&ai_addr, src->ai_addr, ai_addrlen);
}

}
