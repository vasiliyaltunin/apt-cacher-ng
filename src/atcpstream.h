#ifndef ATCPSTREAM_H
#define ATCPSTREAM_H

#include "acsmartptr.h"

#include <functional>

extern "C"
{
struct bufferevent;
}

namespace acng {

class tHttpUrl;

class atcpstream : public acng::tLintRefcounted
{
public:
	atcpstream() =default;
	// callback, parameters: created connection holder, error message, "isFresh" flag
	using tCallBack = std::function<void(lint_ptr<atcpstream>, string_view, bool)>;
	static void Create(const tHttpUrl&, bool forceFresh, int forceTimeout, const tCallBack& cback);
	static void Return(lint_ptr<atcpstream>& stream);
	virtual bufferevent* GetEvent() =0;
	virtual const std::string& GetHost() =0;
	virtual uint16_t GetPort() =0;
	virtual bool PeerIsProxy() =0;
};
}

#endif // ATCPSTREAM_H
