#include "httpdate.h"
#include "meta.h"
#include "filereader.h"
#include "acfg.h"

using namespace std;

namespace acng {

static const char* fmts[] =
{
        "%a, %d %b %Y %H:%M:%S GMT",
        "%A, %d-%b-%y %H:%M:%S GMT",
        "%a %b %d %H:%M:%S %Y"
};

string_view zeroDateBuf = "Do, 01 Jan 1970 01:00:00 GMT";

bool tHttpDate::ParseDate(const char *s, struct tm *tm)
{
    if(!s || !tm)
        return false;
    for(const auto& fmt : fmts)
    {
		memset(tm, 0, sizeof(struct tm));
		auto pEnd = ::strptime(s, fmt, tm);
		if(pEnd && (pEnd - s) > 23)
			return true;
    }
    return false;
}

time_t tHttpDate::ParseDate(const char *s, time_t onError)
{
    struct tm t;
	if (ParseDate(s, &t))
		return mktime(&t);
	return onError;
}

unsigned tHttpDate::FormatTime(char *buf, size_t bufLen, const struct tm * src)
{
	if(bufLen < 30)
        return 0;
    //asctime_r(&src, buf);
	auto len = strftime(buf, bufLen, fmts[0], src);
    if (len >= bufLen || len < 10)
    {
        buf[0] = 0;
        return 0;
    }
    buf[len] = '\0';
    return len;
}

unsigned tHttpDate::FormatTime(char *buf, size_t bufLen, const time_t cur)
{
    if(bufLen < 26)
        return 0;
    struct tm tmp;
    gmtime_r(&cur, &tmp);
    return FormatTime(buf, bufLen, &tmp);
}

tHttpDate::tHttpDate(time_t val) : tHttpDate()
{
	if (val >= 0)
		length = FormatTime(buf, sizeof(buf), val);
}
/*
acng::tHttpDate::operator mstring() const
{
    if (isnorm)
        return mstring(buf, length);
    else
    {
        return tHttpDate(buf, true);
    }
}
*/
tHttpDate::tHttpDate(const char* val, bool forceNorm) : tHttpDate()
{
    if (!val || !*val)
        return;

    struct tm tbuf;
    size_t srcLen(0);
    if (!forceNorm)
    {
        srcLen = strlcpy(buf, val, sizeof(buf));
        forceNorm = srcLen >= sizeof(buf);
    }

    if (!forceNorm)
    {
        length = srcLen;
        return;
    }

    // too long or forced to normalize :-(
    if (!ParseDate(val, &tbuf))
    {
        unset();
        return;
    }

    length = FormatTime(buf, sizeof(buf), &tbuf);
    if (length)
        isnorm = true;
    else
        unset();
}

tHttpDate::tHttpDate(string_view val, bool forceNorm)
{
    unset();

    // easy bad case or regular case
    if (val.empty())
        return;
    if (!forceNorm && val.size() < sizeof(buf))
    {
        length = val.size();
        val.copy(buf, length);
        buf[(size_t) length] = '\0';
        return;
    }

    struct tm tbuf;
    mstring terminated(val);
    // too long or forced to normalize :-(
    if (!ParseDate(terminated.c_str(), &tbuf))
        return;

    length = FormatTime(buf, sizeof(buf), &tbuf);
    if (length)
        isnorm = true;
}

bool tHttpDate::operator==(const tHttpDate &other) const {
    if (isSet() != other.isSet())
        return false;
    if (0 == strncmp(buf, other.buf, sizeof(buf)))
        return true;
    return value(-1) == other.value(-2);
}

bool tHttpDate::operator==(const char *other) const
{
    bool otherSet = other && *other;
    if (isSet())
    {
        if (!otherSet)
            return false;
        if (0 == strncmp(other, buf, sizeof(buf)))
            return true;
        return value(-1) == ParseDate(other, -2);
    }
    else // also equal if both are not set
        return !otherSet;
}

string_view contLenPfx(WITHLEN("Content-Length: ")), laMoPfx(WITHLEN("Last-Modified: ")), origSrcPfx(WITHLEN("X-Original-Source: "));

bool ParseHeadFromStorage(cmstring &path, off_t *contLen, tHttpDate *lastModified, mstring *origSrc)
{
    acbuf buf;
    if(!buf.initFromFile(path.c_str()))
        return -1;

    tSplitByStrStrict spliter(buf.view(), svRN);
    if (!spliter.Next())
        return false;
    if (!startsWithSz(spliter.view(), "HTTP/1.1 200"))
        return false;
    for(auto it: spliter)
    {
        if (it.empty())
            return true;

        if (contLen && startsWithSz(it, "Content-Length:"))
        {
            *contLen = atoofft(it.data()+15, -1);
            contLen = nullptr;
        }
        else if (lastModified && startsWith(it, laMoPfx))
        {
            it.remove_prefix(laMoPfx.size());
            trimBoth(it);
            *lastModified = tHttpDate(it);
            lastModified = nullptr;
        }
        else if (origSrc && startsWith(it, origSrcPfx))
        {
            it.remove_prefix(origSrcPfx.size());
            trimBoth(it);
            *origSrc = it;
            origSrc = nullptr;
        }
    }
    return true;
}

bool StoreHeadToStorage(cmstring &path, off_t contLen, tHttpDate *lastModified, mstring *origSrc)
{
    if (path.empty())
        return false;
#if 0
    string temp1;
    temp1 = path;
    temp1[temp1.size()-1] = '-';
    auto temp2 = path;
    temp2[temp2.size()-1] = '+';
#endif
    tSS fmt(250);
    fmt << "HTTP/1.1 200 OK\r\n"sv;
	if (contLen >= 0)
		fmt << "Content-Length: "sv << contLen << svRN;
    if (lastModified && lastModified->isSet())
        fmt << "Last-Modified: "sv << lastModified->view() << svRN;
    if (origSrc && !origSrc->empty())
        fmt << "X-Original-Source: "sv << *origSrc << svRN;
    fmt << svRN;
	return fmt.dumpall(path.c_str(), O_CREAT, cfg::fileperms, INT_MAX, true);
}

tRemoteStatus::tRemoteStatus(string_view s, int errorCode, bool stripHttpPrefix)
{
	tSplitWalk split(s);
	bool ok = split.Next();
	if (ok && stripHttpPrefix)
	{
		if (startsWithSz(split.view(), "HTTP/1"))
			ok = split.Next();
	}
	if (ok)
	{
		auto tok = split.view();
		ok = !tok.empty() && 0 != (code = svtol(tok, 0));
	}
	if (ok)
	{
		msg = split.right();
		ok = !msg.empty();
	}
	if (!ok)
	{
		code = errorCode;
		msg = "Invalid header line";
	}
}

}
