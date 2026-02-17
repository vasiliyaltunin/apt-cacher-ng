#ifndef _HEADER_H
#define _HEADER_H

#include <unordered_set>
#include <vector>
#include "acbuf.h"
#include "httpdate.h"

namespace acng
{

class ACNG_API header {
public:
	enum eHeadType : char
	{
		INVALID = 'I',
		HEAD = 'H',
		GET = 'G',
		POST = 'P',
		CONNECT = 'C',
		ANSWER = 'A'
	};
	enum eHttpType : char
	{
		HTTP_10 = '0',
		HTTP_11 = '1'
	};
	enum eHeadPos : char
	{
		CONNECTION,			// 0
		CONTENT_LENGTH,
		IF_MODIFIED_SINCE,
		RANGE,
		IFRANGE,				// 4

		CONTENT_RANGE,
		LAST_MODIFIED,
		PROXY_CONNECTION,
		TRANSFER_ENCODING,
		XORIG,

		AUTHORIZATION,		// 10
		XFORWARDEDFOR,
		LOCATION,
		CONTENT_TYPE,
		CACHE_CONTROL,		//14

		// unreachable entry and size reference
		HEADPOS_MAX,
		// special value, only a flag to remember that data is stored to external header
		HEADPOS_UNK_EXPORT
	};

	char *h[HEADPOS_MAX] = {0};
	eHeadType type = INVALID;
	eHttpType proto = HTTP_11;

	header() =default;
	~header();
	header(const header &);
	header(header &&);
	header& operator=(const header&);
	header& operator=(header&&);

	int LoadFromFile(const mstring & sPath);

	//! returns byte count or negative errno value
	int StoreToFile(cmstring &sPath) const;

	void set(eHeadPos, const mstring &value);
	void set(eHeadPos, const char *val);
	void set(eHeadPos, const char *s, size_t len);
	void set(eHeadPos, off_t nValue);
	void prep(eHeadPos, size_t length);
	void del(eHeadPos);
	void copy(const header &src, eHeadPos pos) { set(pos, src.h[pos]); };

	static mstring ExtractCustomHeaders(string_view reqHead, bool isPassThrough);
	const std::string& getStatusMessage() const { return m_status.msg; }
	const std::string& getRequestUrl() const { return m_status.msg; }
	const tRemoteStatus& getStatus() const { return m_status; }
	int getStatusCode() const { return m_status.code; }
	bool isAnswer() const { return m_status.code >= 0; }
	void setStatus(int cod, string_view msg) { m_status = {cod, std::string(msg)}; }
	void clear();

	tSS ToString() const;

	/**
	   * Read buffer to parse one string. Optional offset where to begin to
	   * scan.
	   *
	   * @param src Pointer to raw input
	   * @param length Maximum considered input length
	   * @param unkHeaderMap Optional, series of string_view pairs containing key and values. If key is empty, record's value is a continuation of the previous value.
	   * @return Length of processed data, 0: incomplete, needs more data, <0: error, >0: length of the processed data
	   */
	int Load(string_view sv, std::vector<std::pair<string_view,string_view> > *unkHeaderMap = nullptr);

private:
	eHeadPos resolvePos(string_view key);
	tRemoteStatus m_status;
};

#if 0
// draft for the future
struct tHeader
{
	string_view view;
};
struct tFrontLine
{
	tRemoteStatus status;
	header::eHeadType mode;
	header::eHttpType htype;
};

bool ParseHead(tFrontLine *pStatus, std::initializer_list<std::pair<const string_view&, tHeader&>> todo);
#endif
}

#endif
