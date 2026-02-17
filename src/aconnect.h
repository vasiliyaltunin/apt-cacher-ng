#ifndef ACONNECT_H
#define ACONNECT_H

#include "caddrinfo.h"
#include "fileio.h"
#include <list>

namespace acng {

/**
 * @brief Short living object used to establish TCP connection to a target
 *
 * The object is self-destructing after execution.
 */
class aconnector
{
public:
    /**
     * Thread context: ST, IO thread
     */
	~aconnector() =default;

	struct tConnResult
	{
		unique_fd fd;
		std::string sError;
	};

	// file descriptor, error message, forcedSsl flag
	using tCallback = std::function<void (tConnResult)>;

    /**
         * @brief Start connection asynchronously and report result via callback
         * @param target Connection target
         * @param port
         * @param cbReport
         *
         * Thread context: ST, IO thread
         */
	static void Connect(cmstring& target, uint16_t port, unsigned timeout, tCallback cbReport);
    /**
         * @brief Connect to a target in synchronous fashion
         * @param target
         * @param port
         * @return Connected file socket OR non-empty error string
         *
         * Thread context: not IO thread, reentrant, blocking!
         */
	static tConnResult Connect(cmstring& target, uint16_t port, unsigned timeout);

private:
    // forbid copy operations
    aconnector(const aconnector&) = delete;
    /**
     * Thread context: ST, IO thread
     */
    aconnector() =default;

    tCallback m_cback;
	std::deque<acng_addrinfo> m_targets;
    // linear search is sufficient for this amount of elements
	struct tProbeInfo
	{
		unique_fd fd;
		unique_event ev;
	};
	std::list<tProbeInfo> m_eventFds;
    unsigned m_pending = 0;
    time_t m_tmoutTotal, m_timeNextCand;
    mstring m_error2report;

	decltype (m_targets)::iterator m_cursor;

    void processDnsResult(std::shared_ptr<CAddrInfo>);

    static void cbStep(int fd, short what, void* arg);
    void step(int fd, short what);
    void retError(mstring);
    void retSuccess(int fd);
    void disable(int fd, int ec);
};

}
#endif // ACONNECT_H
