#ifndef PORTUTILS_H
#define PORTUTILS_H

#include "actypes.h"
#include <cstdio>

#define DEFAULT_PORT_HTTP 80
#define DEFAULT_PORT_HTTPS 443
extern std::string sDefPortHTTP, sDefPortHTTPS;

namespace acng {

/*
 * Simple and most (?) efficiently optimized helper (even with older GCC)
 */
struct tPortFmter
{
	LPCSTR fmt(uint16_t nPort)
	{
		if (nPort == 80)
			return "80";
		if (nPort == 443)
			return "443";
		snprintf(buf, sizeof(buf), "%hi", nPort);
		return buf;
	}
private:
	char buf[6];
};

std::string makeHostPortKey(const std::string & sHostname, uint16_t nPort);

}

#endif // PORTUTILS_H
