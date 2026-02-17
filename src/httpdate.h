#ifndef THTTPDATE_H
#define THTTPDATE_H

#include "actypes.h"

namespace acng {

extern const char* zeroDate;

/**
 * @brief The tHttpDate class is a lazy container of a HTTP-style timestamp
 *
 * It acts on the presumption that parsing dates is expensive and is rarely needed, and that almost all input data comes in a normalized format anyway.
 * Data might be reformated if needed but avoided if possible.
 * Comparing with others might happen stringwise if possible.
 * Value-copy of this should be the regular case.
 */
class ACNG_API tHttpDate
{
    char buf[30];
    char isnorm, length;
public:
    tHttpDate() { unset(); }
    /**
     * @brief tHttpDate default pass-over constructor
     * This takes the string as-is as long as it fits. It it does not fit (because of some legacy format) or is enforced by user,
     * normalize the value and keep the normalied form.
     *
     * The resulting contents might be invalid (if string is short) or even unset (if data is invalid).
     *
     * @param szDate Input date string
     * @param forceNorm Attempt to bring the date to normal form, whatever is inside.
     */
    explicit tHttpDate(const char* szDate, bool forceNorm = false);
    explicit tHttpDate(string_view svDate, bool forceNorm = false);
    /** Formats straight to GOOD type date */
    explicit tHttpDate(time_t utcDate);

    /** Report value in whatever format it has inside, but guaranteed to be zero-terminated anyway. Empty string_view if not set. */
	const string_view view() const { return isSet() ? string_view(buf, length) : string_view(); }
	//const ::tm tm(time_t onError);
    //operator string_view() const;
    time_t value(time_t ifBad) const { return isSet() ? ParseDate(buf, ifBad) : ifBad; }
    bool isSet() const { return length && *buf; }
    void unset() { buf[0] = length = isnorm = 0; }

    // messy stuff
    bool operator<(const tHttpDate& other) const { return other >= *this; }
    bool operator>(const tHttpDate& other) const { return other <= *this; }
    bool operator<=(const tHttpDate& other) const { return value(-1) <= other.value(-2); }
    bool operator>=(const tHttpDate& other) const { return value(-1) >= other.value(-2); };
    bool operator==(const tHttpDate& other) const;
	bool operator!=(const tHttpDate& other) const { return !(other == *this); }
    bool operator==(const char* other) const;

    //tHttpDate& operator=(const char*);
    //tHttpDate& operator=(time_t value);

    static unsigned FormatTime(char *buf, size_t bufLen, time_t cur);
    /**
     * @brief FormatTime Format the date by recommended format to the desired buffer
     * The specified buffer must be at least 30 chars long
     * @param buf Output buffer
     * @param bufLen Length of the buffer
     * @param src Date struct
     * @return Length of the created string or 0 if failed
     */
    static unsigned FormatTime(char *buf, size_t bufLen, const struct tm *src);
    static bool ParseDate(const char *, struct tm*);
    static time_t ParseDate(const char *, time_t onError);

private:
    //bool operator==(const tHttpDate& other) const;
    //bool operator!=(const tHttpDate& other) const;
    //bool operator==(const char*) const;
    //bool operator!=(const char* other) const { return !(*this == other); }

};

struct ACNG_API tRemoteStatus
{
	int code = 500;
	std::string msg;

	bool isRedirect() const
	{
        switch(code)
        {
        case 301:
        case 302:
        case 303:
        case 307:
        case 308:
            return true;
        default:
            return false;
        }
    }

	bool mustNotHaveBody() const
    {
        // no response for not-modified or similar, following http://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.4
        switch(code)
        {
        case 304:
        case 204:
            return true;
        default:
            return (code >= 100 && code < 200);
        }
    }
	tRemoteStatus(string_view, int errorCode, bool stripHttpPrefix);
    tRemoteStatus(int code, mstring s) : code(code), msg(move(s)) {}
//	tRemoteStatus(tRemoteStatus &&src) : code(src.code), msg(move(src.msg)) {}
    tRemoteStatus() =default;
};

/**
 * @brief ParseHeadFromStorage
 * @param path Absolute path of the head file
 * @param contLen Content-Length to add (optional)
 * @param lastModified Last-Modified date to add (optional)
 * @param origSrc Original source mark
 * @return True if succeeded
 */
bool ACNG_API ParseHeadFromStorage(cmstring &path, off_t *contLen, tHttpDate *lastModified, mstring *origSrc);
bool ACNG_API StoreHeadToStorage(cmstring &path, off_t contLen, tHttpDate *lastModified, mstring *origSrc);

}

#endif // THTTPDATE_H
