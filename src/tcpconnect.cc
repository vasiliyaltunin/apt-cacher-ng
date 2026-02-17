/*
 * tcpconnect.cpp
 *
 *  Created on: 27.02.2010
 *      Author: ed
 */

#include "tcpconnect.h"
#include "debug.h"
#include "meta.h"
#include "ahttpurl.h"
#include "acfg.h"
#include "caddrinfo.h"
#include "fileio.h"
#include "fileitem.h"
#include "cleaner.h"
#include "evabase.h"
#include "aconnect.h"
#include "portutils.h"

#include <tuple>

#include <signal.h>
#include <sys/select.h>

#ifdef HAVE_SSL
#include <openssl/evp.h>
#include "openssl/bio.h"
#include "openssl/ssl.h"
#include "openssl/err.h"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#endif

using namespace std;

#ifdef DEBUG
#include <atomic>
atomic_int nConCount(0), nDisconCount(0), nReuseCount(0);
#endif

namespace acng
{

ACNG_API dl_con_factory g_tcp_con_factory;

tcpconnect::tcpconnect(tRepoUsageHooks *pObserver) : m_pStateObserver(pObserver)
{
	if(pObserver)
		pObserver->OnAccess();
}

tcpconnect::~tcpconnect()
{
	LOGSTART("tcpconnect::~tcpconnect, terminating outgoing connection class");
	Disconnect();
#ifdef HAVE_SSL
	if(m_ctx)
	{
		SSL_CTX_free(m_ctx);
		m_ctx=0;
	}
#endif
	if(m_pStateObserver)
	{
		m_pStateObserver->OnRelease();
		m_pStateObserver=nullptr;

	}
}

void tcpconnect::Disconnect()
{
	LOGSTARTFUNCx(m_sHostName);

#ifdef DEBUG
	nDisconCount.fetch_add(m_conFd >=0);
#endif

#ifdef HAVE_SSL
	if(m_bio)
		BIO_free_all(m_bio), m_bio=nullptr;
#endif

	m_lastFile.reset();

	termsocket_quick(m_conFd);
}
acmutex spareConPoolMx;
multimap<tuple<string,uint16_t SSL_OPT_ARG(bool) >,
		std::pair<tDlStreamHandle, time_t> > spareConPool;

ACNG_API void CloseAllCachedConnections()
{
	lockguard g(spareConPoolMx);
	spareConPool.clear();
}

tDlStreamHandle dl_con_factory::CreateConnected(cmstring &sHostname, uint16_t nPort,
		mstring &sErrOut, bool *pbSecondHand, tRepoUsageHooks *pStateTracker
		,bool bSsl, int timeout, bool nocache) const
{
	LOGSTARTFUNCsx(sHostname, nPort, bSsl);

	tDlStreamHandle p;
#ifndef HAVE_SSL
	if(bSsl)
	{
		log::err("E_NOTIMPLEMENTED: SSL");
		return p;
	}
#endif

	bool bReused=false;
	auto key = make_tuple(sHostname, nPort SSL_OPT_ARG(bSsl) );

	if(!nocache)
	{
		// mutex context
		lockguard __g(spareConPoolMx);
		auto it=spareConPool.find(key);
		if(spareConPool.end() != it)
		{
			p=it->second.first;
			spareConPool.erase(it);
			bReused = true;
			ldbg("got connection " << p.get() << " from the idle pool");

			// it was reset in connection recycling, restart now
			if(pStateTracker)
			{
				p->m_pStateObserver = pStateTracker;
				pStateTracker->OnAccess();
			}
#ifdef DEBUG
			nReuseCount.fetch_add(1);
#endif
		}
	}

	if(!p)
	{
		p.reset(new tcpconnect(pStateTracker));
		if(p)
		{
			p->m_sHostName = sHostname;
			p->m_nPort = nPort;
			auto res = aconnector::Connect(sHostname, nPort, timeout);

			if (!res.sError.empty())
			{
				sErrOut = move(res.sError);
				p.reset();
			}
			else if(res.fd.get() == -1)
			{
				sErrOut = "General Connection Error"sv;
				p.reset();
			}
			else
			{
				p->m_conFd = res.fd.release();
			}
		}

#ifdef HAVE_SSL
		if(p && bSsl && !p->SSLinit(sErrOut))
		{
			p.reset();
			LOG("ssl init error");
		}
#endif
	}

	if(pbSecondHand)
		*pbSecondHand = bReused;

	return p;
}

void dl_con_factory::RecycleIdleConnection(tDlStreamHandle & handle) const
{
	if(!handle)
		return;

	LOGSTARTFUNCxs(handle->m_sHostName);

	if(handle->m_pStateObserver)
	{
		handle->m_pStateObserver->OnRelease();
		handle->m_pStateObserver = nullptr;
	}

	if(! cfg::persistoutgoing)
	{
		ldbg("not caching outgoing connections, drop " << handle.get());
		handle.reset();
		return;
	}

#ifdef DEBUG
	if(check_read_state(handle->GetFD()))
	{
		acbuf checker;
		checker.setsize(300000);
		checker.sysread(handle->GetFD());

	}
#endif

	auto& host = handle->GetHostname();
	if (!host.empty())
	{
		time_t now = GetTime();
		lockguard __g(spareConPoolMx);
		ldbg("caching connection " << handle.get());

		// a DOS?
		if (spareConPool.size() < 50)
		{
			spareConPool.emplace(make_tuple(host, handle->GetPort()
					SSL_OPT_ARG(handle->m_bio) ), make_pair(handle, now));
#ifndef MINIBUILD
			cleaner::GetInstance().ScheduleFor(now + TIME_SOCKET_EXPIRE_CLOSE, cleaner::TYPE_EXCONNS);
#endif
		}
	}

	handle.reset();
}

time_t dl_con_factory::BackgroundCleanup()
{
	lockguard __g(spareConPoolMx);
	time_t now=GetTime();

	fd_set rfds;
	FD_ZERO(&rfds);
	int nMaxFd=0;

	// either drop the old ones, or stuff them into a quick select call to find the good sockets
	for (auto it = spareConPool.begin(); it != spareConPool.end();)
	{
		if (now >= (it->second.second + TIME_SOCKET_EXPIRE_CLOSE))
			it = spareConPool.erase(it);
		else
		{
			int fd = it->second.first->GetFD();
			FD_SET(fd, &rfds);
			nMaxFd = max(nMaxFd, fd);
			++it;
		}
	}
	// if they have to send something, that must the be the CLOSE signal
	int r=select(nMaxFd + 1, &rfds, nullptr, nullptr, CTimeVal().For(0, 1));
	// on error, also do nothing, or stop when r fds are processed
	for (auto it = spareConPool.begin(); r>0 && it != spareConPool.end(); r--)
	{
		if(FD_ISSET(it->second.first->GetFD(), &rfds))
			it = spareConPool.erase(it);
		else
			++it;
	}

	return spareConPool.empty() ? END_OF_TIME : GetTime()+TIME_SOCKET_EXPIRE_CLOSE/4+1;
}

void tcpconnect::KillLastFile()
{
#ifndef MINIBUILD
	tFileItemPtr p = m_lastFile.lock();
	if (!p)
		return;
	p->MarkFaulty();
#endif
}

void dl_con_factory::dump_status()
{
	lockguard __g(spareConPoolMx);
	tSS msg;
	msg << "TCP connection cache:\n";
	for (const auto& x : spareConPool)
	{
		if(! x.second.first)
		{
			msg << "[BAD HANDLE] recycle at " << x.second.second << "\n";
			continue;
		}

		msg << x.second.first->m_conFd << ": for "
				<< get<0>(x.first) << ":" << get<1>(x.first)
				<< ", recycled at " << x.second.second
				<< "\n";
	}
#ifdef DEBUG
	msg << "dbg counts, con: " << nConCount.load()
			<< " , discon: " << nDisconCount.load()
			<< " , reuse: " << nReuseCount.load() << "\n";
#endif

	log::err(msg);
}
#ifdef HAVE_SSL
bool tcpconnect::SSLinit(mstring &sErr)
{
	SSL * ssl(nullptr);
	mstring ebuf;

	auto withSslHeadPfx = [&sErr](const char *perr)
					{
        sErr="SSL error: ";
		sErr+=(perr?perr:"Generic SSL failure");
		return false;
					};
	auto withLastSslError = [&withSslHeadPfx]()
						{
		auto nErr = ERR_get_error();
		auto serr = ERR_reason_error_string(nErr);
		return withSslHeadPfx(serr);
						};
	auto withSslRetCode = [&withSslHeadPfx, &withLastSslError, &ssl](int hret)
				{
		auto nErr = SSL_get_error(ssl, hret);
		auto serr =  ERR_reason_error_string(nErr);
		return serr ? withSslHeadPfx(serr) : withLastSslError();
				};

	// cleaned up in the destructor on EOL
	if(!m_ctx)
	{
		m_ctx = SSL_CTX_new(SSLv23_client_method());
		if (!m_ctx) return withLastSslError();

		SSL_CTX_load_verify_locations(m_ctx,
				cfg::cafile.empty() ? nullptr : cfg::cafile.c_str(),
			cfg::capath.empty() ? nullptr : cfg::capath.c_str());
	}

	ssl = SSL_new(m_ctx);
	if (!m_ctx) return withLastSslError();

	bool disableNameValidation = cfg::nsafriendly == 1;// || (bGuessedTls * cfg::nsafriendly == 2);
	bool disableAllValidation = cfg::nsafriendly == 1; // || (bGuessedTls * (cfg::nsafriendly == 2 || cfg::nsafriendly == 3));

	// for SNI
	SSL_set_tlsext_host_name(ssl, m_sHostName.c_str());

	if (!disableNameValidation)
	{
		auto param = SSL_get0_param(ssl);
		/* Enable automatic hostname checks */
		X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
		X509_VERIFY_PARAM_set1_host(param, m_sHostName.c_str(), 0);
		/* Configure a non-zero callback if desired */
		SSL_set_verify(ssl, SSL_VERIFY_PEER, 0);
	}

	// mark it connected and prepare for non-blocking mode
 	SSL_set_connect_state(ssl);
 	SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY
 			| SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
 			| SSL_MODE_ENABLE_PARTIAL_WRITE);

 	auto hret=SSL_set_fd(ssl, m_conFd);
 	if(hret != 1) return withSslRetCode(hret);

 	while(true)
 	{
 		hret=SSL_connect(ssl);
		if (hret == 1)
			break;

		if (hret == 0)
			return withSslRetCode(hret);

		if (hret == -1)
		{
			auto nError = SSL_get_error(ssl, hret);
			fd_set rfds, wfds;
			FD_ZERO(&rfds);
			FD_ZERO(&wfds);
			switch (nError)
			{
			case SSL_ERROR_WANT_READ:
				FD_SET(m_conFd, &rfds);
				break;
			case SSL_ERROR_WANT_WRITE:
				FD_SET(m_conFd, &wfds);
				break;
			default:
				return withSslRetCode(nError);
			}
			int nReady = select(m_conFd + 1, &rfds, &wfds, nullptr, CTimeVal().ForNetTimeout());
			if (nReady == 1)
				continue;
			if (nReady == 0)
				return withSslHeadPfx("Socket timeout");
			if (nReady < 0)
			{
				ebuf = tErrnoFmter("Socket error");
				return withSslHeadPfx(ebuf.c_str());
			}
		}
		else
			return withLastSslError();

 	}
 	if(m_bio) BIO_free_all(m_bio);
 	m_bio = BIO_new(BIO_f_ssl());
 	if(!m_bio) return withSslHeadPfx("IO initialization error");
 	// not sure we need it but maybe the handshake can access this data
	BIO_set_conn_hostname(m_bio, m_sHostName.c_str());
	BIO_set_conn_port(m_bio, tPortFmter().fmt(m_nPort));
 	BIO_set_ssl(m_bio, ssl, BIO_NOCLOSE);

 	BIO_set_nbio(m_bio, 1);
	set_nb(m_conFd);

	if(!disableAllValidation)
	{
		X509* server_cert = nullptr;
		hret=SSL_get_verify_result(ssl);
		if( hret != X509_V_OK)
			return withSslHeadPfx(X509_verify_cert_error_string(hret));
		server_cert = SSL_get_peer_certificate(ssl);
		if(server_cert)
		{
			// XXX: maybe extract the real name to a buffer and report it additionally?
			// X509_NAME_oneline(X509_get_subject_name (server_cert), cert_str, sizeof (cert_str));
			X509_free(server_cert);
		}
		else // The handshake was successful although the server did not provide a certificate
			return withSslHeadPfx("Incompatible remote certificate");
	}
	return true;
}

#endif

bool tcpconnect::StartTunnel(const tHttpUrl& realTarget, mstring& sError,
		cmstring *psAuthorization, bool bDoSSL)
{
	/*
	  CONNECT server.example.com:80 HTTP/1.1
      Host: server.example.com:80
      Proxy-Authorization: basic aGVsbG86d29ybGQ=
	 */
	tSS fmt;
	fmt << "CONNECT " << realTarget.sHost << ":" << realTarget.GetPort()
			<< " HTTP/1.1\r\nHost: " << realTarget.sHost << ":" << realTarget.GetPort()
			<< "\r\n";
	if(psAuthorization && !psAuthorization->empty())
	{
			fmt << "Proxy-Authorization: Basic "
					<< EncodeBase64Auth(*psAuthorization) << "\r\n";
	}
	fmt << "\r\n";

	try
	{
		if (!fmt.send(m_conFd, &sError))
			return false;

		fmt.clear();
		while (true)
		{
			fmt.setsize(4000);
			if (!fmt.recv(m_conFd, &sError))
				return false;
			if(fmt.freecapa()<=0)
			{
                sError = "Remote proxy error";
				return false;
			}

			header h;
            auto n = h.Load(fmt.view());
			if(!n)
				continue;

			auto st = h.getStatusCode();
			if (n <= 0 || st == 404 /* just be sure it doesn't send crap */)
			{
                sError = "Tunnel setup failed";
				return false;
			}

			if (st < 200 || st >= 300)
			{
				sError = h.getStatusMessage();
				return false;
			}
			break;
		}

		m_sHostName = realTarget.sHost;
		m_nPort = realTarget.GetPort();
#ifdef HAVE_SSL
		if (bDoSSL && !SSLinit(sError))
		{
			m_sHostName.clear();
			return false;
		}
#else
		(void) bDoSSL;
#endif
	}
	catch(...)
	{
		return false;
	}
	return true;
}


}
