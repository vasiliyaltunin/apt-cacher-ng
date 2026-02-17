#include "ebrunner.h"
#include "evabase.h"
#include "cleaner.h"
#include "dlcon.h"
#include <thread>
#include <event.h>

using namespace std;

namespace acng
{
void SetupCleaner();

void suicide(evutil_socket_t, short, void *)
{
	abort();
}

class evabaseFreeRunner::Impl
{
public:
	SHARED_PTR<dlcon> dl;
	thread dlthr, evthr;
	unique_ptr<evabase> m_eb;
	event* m_killTimer;

	Impl(const IDlConFactory &pDlconFac, bool withDownloader, unsigned killTimeout)
		:m_eb(new evabase), m_killTimer(nullptr)
	{
		SetupCleaner();
		if (withDownloader)
			dl = dlcon::CreateRegular(pDlconFac);
		evthr = std::thread([&]() { m_eb->MainLoop(); });
		if (withDownloader)
			dlthr = std::thread([&]() {dl->WorkLoop();});
		if (killTimeout)
		{
			m_killTimer = evtimer_new(m_eb->base, suicide, nullptr);
			struct timeval expTv { killTimeout, 123 };
			evtimer_add(m_killTimer, &expTv);
		}
	}

	~Impl()
	{
		::acng::cleaner::GetInstance().Stop();

		if (m_killTimer)
			evtimer_del(m_killTimer);

		if (dl)
			dl->SignalStop();
		m_eb->SignalStop();

		if (dl)
			dlthr.join();
		evthr.join();
	}

};
evabaseFreeRunner::evabaseFreeRunner(const IDlConFactory &pDlconFac, bool withDownloader, unsigned killTimeout)
{
	m_pImpl = new Impl(pDlconFac, withDownloader, killTimeout);
}

evabaseFreeRunner::~evabaseFreeRunner()
{
	delete m_pImpl;
}

dlcon& evabaseFreeRunner::getDownloader()
{
	return * m_pImpl->dl;
}

event_base *evabaseFreeRunner::getBase()
{
	return m_pImpl->m_eb.get()->base;
}

}
