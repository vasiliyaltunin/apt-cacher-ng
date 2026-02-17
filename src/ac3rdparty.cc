#include "config.h"
#include "ac3rdparty.h"

#include <mutex>
#include <deque>

#include <event2/thread.h>
#include <event2/event.h>

#ifdef HAVE_SSL
#include <openssl/evp.h>
#include "openssl/ssl.h"
#include "openssl/err.h"
#include <openssl/crypto.h>
#endif

#include <ares.h>

namespace acng {

#ifdef HAVE_SSL
std::deque<std::mutex> g_ssl_locks;
void thread_lock_cb(int mode, int which, const char *, int)
{
		if (which >= int(g_ssl_locks.size()))
				return; // weird
		if (mode & CRYPTO_LOCK)
				g_ssl_locks[which].lock();
		else
				g_ssl_locks[which].unlock();
}

//! Global init helper (might be non-reentrant)
void ACNG_API globalSslInit()
{
		static bool inited=false;
		if(inited)
				return;
		inited = true;
		SSL_load_error_strings();
		ERR_load_BIO_strings();
		ERR_load_crypto_strings();
		ERR_load_SSL_strings();
		OpenSSL_add_all_algorithms();
		SSL_library_init();

		g_ssl_locks.resize(CRYPTO_num_locks());
	CRYPTO_set_id_callback(get_thread_id_cb);
	CRYPTO_set_locking_callback(thread_lock_cb);
}
void ACNG_API globalSslDeInit()
{
		g_ssl_locks.clear();
}
#else
void ACNG_API globalSslInit() {}
void ACNG_API globalSslDeInit() {}
#endif


ac3rdparty::ac3rdparty()
{
	ares_library_init(ARES_LIB_INIT_ALL);
	evthread_use_pthreads();
	globalSslInit();
}

ac3rdparty::~ac3rdparty()
{
	globalSslDeInit();
	libevent_global_shutdown();
	ares_library_cleanup();
}

}
