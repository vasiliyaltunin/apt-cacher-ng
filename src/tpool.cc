#include "tpool.h"
#include "meta.h"
#include "lockable.h"


#include <thread>

namespace acng {

class tpoolImpl : public tpool, public base_with_condition
{
	unsigned m_nMaxCount, m_nMaxSpare;
	unsigned m_nCurActive = 0, m_nCurSpare = 0;
	std::deque<std::function<void()>> m_freshWork;
	bool m_shutdown = false;

	// tpool interface
public:

	void ThreadAction()
	{
		lockuniq g(this);

		while (true)
		{
			if (m_shutdown)
			{
				m_nCurSpare--;
				break;
			}
			if (m_freshWork.empty())
			{
				wait(g);
				continue;
			}
			auto c = move(m_freshWork.front());
			m_freshWork.pop_front();

			m_nCurSpare--;
			m_nCurActive++;
			g.unLock();
			// run and release the work item, not in critical section!
			c();
			c = decltype (c)();
			g.reLock();
			m_nCurActive--;
			if (m_nCurSpare >= m_nMaxSpare || m_shutdown)
				break;
			m_nCurSpare++;
		}
		notifyAll();
	};


	bool schedule(std::function<void ()> action) override
	{
		setLockGuard;
		if (m_nCurActive + m_nCurActive >= m_nMaxCount)
		{
			return false;
		}
		if (m_nCurSpare < m_freshWork.size() + 1)
		{
			try
			{
				std::thread thr(&tpoolImpl::ThreadAction, this);
				thr.detach();
			}
			catch (...)
			{
				return false;
			}
			m_nCurSpare++;
		}
		try
		{
			m_freshWork.emplace_back(std::move(action));
			notifyAll();
		}
		catch (...)
		{
			return false;
		}
		return true;
	}

	void stop() override
	{
		lockuniq g(this);
		m_shutdown = true;
		notifyAll();
		while (m_nCurSpare + m_nCurActive)
			wait(g);
	}

	tpoolImpl(unsigned maxCount, unsigned maxSpare) : m_nMaxCount(maxCount), m_nMaxSpare(maxSpare)
	{
	}
};

std::shared_ptr<tpool> tpool::Create(unsigned maxCount, unsigned maxSpare)
{
	return std::make_shared<tpoolImpl>(maxCount, maxSpare);
}

}
