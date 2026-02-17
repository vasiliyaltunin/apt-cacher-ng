
#include "debug.h"
#include "acfg.h"
#include "meta.h"
#include "astrop.h"
#include "header.h"
#include "config.h"
#include "acbuf.h"
#include "fileio.h"
#include "filereader.h"
#include "httpdate.h"

#include <map>
#include <iostream>
#include <algorithm>

#include <cstdio>

#include <strings.h>
#include <unistd.h>

using namespace std;

namespace acng
{

// Order matches the enum order!
constexpr string_view mapId2Headname[] =
{
    "Connection"sv,
    "Content-Length"sv,
    "If-Modified-Since"sv,
    "Range"sv,
    "If-Range"sv,

    "Content-Range"sv,
    "Last-Modified"sv,
    "Proxy-Connection"sv,
    "Transfer-Encoding"sv,
    "X-Original-Source"sv,

    "Authorization"sv,
    "X-Forwarded-For"sv,
    "Location"sv,
	"Content-Type"sv,
	"Cache-Control"sv,
};

header::header(const header &s)
:type(s.type), m_status(s.m_status)
{
	for (unsigned i = 0; i < HEADPOS_MAX; i++)
		h[i] = s.h[i] ? strdup(s.h[i]) : nullptr;
}

header::header(header &&s)
:type(s.type)
{
	m_status = move(s.m_status);
	std::swap(h, s.h);
}


header& header::operator=(const header& s)
{
	type = s.type;
	m_status =s.m_status;
	for (unsigned i = 0; i < HEADPOS_MAX; ++i)
	{
		if (h[i])
			free(h[i]);
		h[i] = s.h[i] ? strdup(s.h[i]) : nullptr;
	}
	return *this;
}


header& header::operator=(header&& s)
{
	type = s.type;
	m_status = move(s.m_status);
	std::swap(h, s.h);
	return *this;
}


header::~header()
{
	for(auto& p:h)
		free(p);
}

void header::clear()
{
	for(unsigned i=0; i<HEADPOS_MAX; i++)
		del((eHeadPos) i);
	m_status.code = -1;
	m_status.msg.clear();
	type=INVALID;
}

void header::del(eHeadPos i)
{
	free(h[i]);
    h[i] = nullptr;
}

int header::Load(string_view input, std::vector<std::pair<string_view,string_view> > *unkHeaderMap)
{
    auto pStart = input.data();

    if(input.length() < 9)
		return 0;
    if(!input.data())
		return -1;
    type = INVALID;
#define IFCUT(s, t) if(startsWith(input, s)) { type=t; input.remove_prefix(s.size()); }
	if(startsWith(input, "HTTP/1."sv) && input.length() > 7)
	{
		type = ANSWER;
		proto = (eHttpType) input[7];
		input.remove_prefix(8);
	}
	else IFCUT("GET "sv, GET)
			else IFCUT("HEAD "sv, HEAD)
			else IFCUT("POST "sv, POST)
			else IFCUT("CONNECT "sv, CONNECT)
			else
			return -1;

    tSplitByStrStrict split(input, svRN);
	bool first = true;
    auto lastSetId = HEADPOS_MAX;
	while (split.Next())
    {
		auto it = split.view();

        if (first)
        {
			if (type == ANSWER)
			{
				m_status = tRemoteStatus(it, -1, false);
				if (m_status.code < 0)
					return false;
			}
			else
			{
				trimBoth(it);
				if (it.size() < 10)
					return false;
				proto = (eHttpType) it.back();
				it.remove_suffix(9);
				trimBack(it);
				if (it.empty())
					return false;
				m_status.msg = it;
				m_status.code = -1;
			}

            first = false;
            continue;
        }
        if (it.empty()) // good end? Only if there is a newline ahead, otherwise it's not complete
        {
			if (split.Next()) // good, we are beyond it or the end
			{
				return split.view().data() - pStart;
			}
			// end is unreachable
			return 0;
        }

        string_view sv(it);
        trimBoth(sv);
        if (sv.data() != it.data()) // ok, a continuation?
        {
            if (lastSetId == HEADPOS_UNK_EXPORT)
                unkHeaderMap->emplace_back(string_view(), sv);
            else if (lastSetId != HEADPOS_MAX)
            {
				if (!strappend(h[lastSetId], " "sv, sv)) // OOM?
                    return -3;
            }
			else // garbage in the second line?
				return -2;
            // either appended to captured string or to exported extra map
            continue;
        }
        auto pos = sv.find(':');
        if (pos == stmiss)
		{
			// is this because of EOF or because of garbage?
			return split.Next() ? -4 : 0;
		}
        string_view value(sv.substr(pos + 1)), key(sv.substr(0, pos));
        trimBack(key);
        if (key.empty())
		{
			// EOF or garbage?
			return split.Next() ? -5 : 0;
		}
		trimFront(value);
        lastSetId = resolvePos(key);
        if (lastSetId == HEADPOS_MAX)
        {
            if (unkHeaderMap)
            {
                unkHeaderMap->emplace_back(key, value);
                lastSetId = HEADPOS_UNK_EXPORT;
            }
            continue;
        }
        if (value.empty()) // heh?
            del(lastSetId);
        else
            set(lastSetId, value.data(), value.length());
    }
	// regular finish but termination was not found?
	return 0;
}

header::eHeadPos header::resolvePos(string_view key)
{
    for(unsigned i = 0; i < eHeadPos::HEADPOS_MAX; ++i)
    {
        if (key.length() == mapId2Headname[i].length() &&
                0 == strncasecmp(mapId2Headname[i].data(), key.data(), key.length()))
        {
            return eHeadPos(i);
        }
    }
    return HEADPOS_MAX;
}

int header::LoadFromFile(const string &sPath)
{
	clear();
#if 0
	filereader buf;
	return buf.OpenFile(sPath, true) && LoadFromBuf(buf.GetBuffer(), buf.GetSize());
#endif
	acbuf buf;
	if(!buf.initFromFile(sPath.c_str()))
		return -1;
    return Load(buf.view());
}


void header::set(eHeadPos i, const char *val)
{
	if (h[i])
	{
		free(h[i]);
		h[i]=nullptr;
	}
	if(val)
		h[i] = strdup(val);
}

void header::set(eHeadPos i, const char *val, size_t len)
{
	if(!val)
	{
		free(h[i]);
		h[i]=nullptr;
		return;
	}
	h[i] = (char*) realloc(h[i], len+1);
	if(h[i])
	{
		memcpy(h[i], val, len);
		h[i][len]='\0';
	}
}

void header::set(eHeadPos key, const mstring &value)
{
	string::size_type l=value.size()+1;
	h[key]=(char*) realloc(h[key], l);
	if(h[key])
		memcpy(h[key], value.c_str(), l);
}

void header::prep(eHeadPos key, size_t len)
{
	h[key]=(char*) malloc(len);
}

void header::set(eHeadPos key, off_t nValue)
{	
	char buf[3*sizeof(off_t)];
	int len=sprintf(buf, OFF_T_FMT, nValue);
    set(key, buf, len);
}

tSS header::ToString() const
{
	tSS s;
	s << "HTTP/1." << proto << " " << m_status.code << " " << m_status.msg << "\r\n";
    for(unsigned i = 0; i < eHeadPos::HEADPOS_MAX; ++i)
    {
        if (h[i])
            s << mapId2Headname[i] << ": " << h[i] << "\r\n";
    }
    s<< "Date: " << tHttpDate(GetTime()).view() << "\r\n\r\n";
	return s;
}

int header::StoreToFile(cmstring &sPath) const
{
	int nByteCount(0);
	const char *szPath=sPath.c_str();
	int fd=open(szPath, O_WRONLY|O_CREAT|O_TRUNC, cfg::fileperms);
	if(fd<0)
	{
        fd =- errno;
		// maybe there is something in the way which can be removed?
		if(::unlink(szPath))
			return fd;

		fd=open(szPath, O_WRONLY|O_CREAT|O_TRUNC, cfg::fileperms);
		if(fd<0)
			return -errno;
	}
	
	auto hstr = ToString();
	const char *p = hstr.rptr();
	nByteCount=hstr.length();
	
	for(string::size_type pos=0; pos<(uint)nByteCount;)
	{
		int ret=write(fd, p+pos, nByteCount-pos);
		if(ret<0)
		{
			if(EAGAIN == errno || EINTR == errno)
				continue;
			if(EINTR == errno)
				continue;
			
			ret=errno;
			checkforceclose(fd);
			return -ret;
		}
		pos+=ret;
	}

	while(0!=close(fd))
	{
		if(errno != EINTR)
			return -errno;
	}
	
	return nByteCount;
}

// those are not allowed to be forwarded ever
static const auto tabooHeadersForCaching =
{ string("Host"), string("Cache-Control"), string("Proxy-Authorization"),
        string("Accept"), string("User-Agent"), string("Accept-Encoding") };
static const auto tabooHeadersPassThrough =
{ string("Host"), string("Cache-Control"), string("Proxy-Authorization"),
        string("Accept"), string("User-Agent") };

mstring header::ExtractCustomHeaders(string_view reqHead, bool isPassThrough)
{
    if (reqHead.empty())
        return sEmptyString;
    header h;
    string ret;
    // continuation of header line
    std::vector<std::pair<string_view,string_view> > unkHeaderMap;
    h.Load(reqHead, &unkHeaderMap);

    bool forbidden = false;
    const auto& taboo = isPassThrough ? tabooHeadersPassThrough : tabooHeadersForCaching;
    for(auto& it: unkHeaderMap)
    {
        if (it.first.empty())
        {
            if (forbidden) continue;
            ret.erase(ret.size()-2);
            ret += ' ';
            ret += it.second;
            ret += svRN;
            continue;
        }

        forbidden = taboo.end() != std::find_if(taboo.begin(),
                                                taboo.end(),
                                                [&](cmstring &x)
        { return scaseequals(x, it.first.data()); }
                );

        if(!forbidden)
        {
            ret += it.first;
            ret += ": ";
            ret += it.second;
            ret += svRN;
        }
    }
    return ret;
}

}
