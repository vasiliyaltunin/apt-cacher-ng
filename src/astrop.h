/*
 * astrop.h
 *
 *  Created on: 28.02.2020
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_ASTROP_H_
#define INCLUDE_ASTROP_H_

#include "actypes.h"

#include <string>
#include <cstring>
#include <vector>
#include <deque>
#include <functional>

#include <strings.h> // strncasecmp

#define SPACECHARS " \f\n\r\t\v"

#define trimLine(x) { trimFront(x); trimBack(x); }

#define startsWith(where, what) (0==(where).compare(0, (what).size(), (what)))
#define endsWith(where, what) ((where).size()>=(what).size() && \
                0==(where).compare((where).size()-(what).size(), (what).size(), (what)))
#define startsWithSz(where, what) (0==(where).compare(0, sizeof((what))-1, (what)))
#define endsWithSzAr(where, what) ((where).size()>=(sizeof((what))-1) && \
                0==(where).compare((where).size()-(sizeof((what))-1), (sizeof((what))-1), (what)))
#define stripSuffix(where, what) if(endsWithSzAr(where, what)) where.erase(where.size()-sizeof(what)+1);
#define stripPrefixChars(where, what) where.erase(0, where.find_first_not_of(what))

#define setIfNotEmpty(where, cand) { if(where.empty()) where = cand; }
#define setIfNotEmpty2(where, cand, alt) { if(where.empty()) { if(!cand.empty()) where = cand; else where = alt; } }

#define WITHLEN(x) x, (_countof(x)-1)
#define WLsv(x) x.data(), x.size()

namespace acng
{

inline void trimFront(std::string &s, const string_view junk=SPACECHARS)
{
	auto pos = s.find_first_not_of(junk);
	if(pos == std::string::npos)
		s.clear();
	else if(pos>0)
		s.erase(0, pos);
}

inline void trimFront(string_view& s, string_view junk=SPACECHARS)
{
	auto pos = s.find_first_not_of(junk);
	s.remove_prefix(pos == std::string::npos ? s.length() : pos);
}


inline void trimBack(std::string &s, const string_view junk=SPACECHARS)
{
	auto pos = s.find_last_not_of(junk);
	if(pos == std::string::npos)
		s.clear();
	else if(pos>0)
		s.erase(pos+1);
}

inline void trimBack(string_view& s, string_view junk = SPACECHARS)
{
	auto pos = s.find_last_not_of(junk);
	s.remove_suffix(pos != std::string::npos ? s.size() - pos - 1 : s.length());
}

inline void trimBoth(std::string &s, const string_view junk = SPACECHARS)
{
	trimBack(s, junk);
	trimFront(s, junk);
}

inline void trimBoth(string_view &s, const string_view junk = SPACECHARS)
{
	trimBack(s, junk);
	trimFront(s, junk);
}

/** Not so efficient function which appends a string extension to a string already allocated on heap.
 *  */
bool strappend(char* &p, string_view appendix, string_view app2 = string_view());

//! iterator-like helper for string splitting, for convenient use with for-loops
// Intended for a single run-through, not repeatable
// This is the base template, there are specializations which search for "any of those delimiters" and "that string is a delimiter"
template<bool StringSeparator, bool StrictDelimiter>
class tSplitWalkBase
{
	string_view m_input;
    mutable std::string::size_type m_slice_len;
    string_view m_seps;
        bool m_first;

public:

	/**
	 * @param line The input
	 * @param separators Characters which are considered delimiters (any char in that string)
	 * @param strictDelimiter By default, a sequence of separator chars are considered as one delimiter. This is normally good for whitespace but bad for specific visible separators. Setting this flag makes them be considered separately, returning empty strings as value is possible then.
	 */
        inline tSplitWalkBase(string_view line, string_view separators = SPACECHARS)
    : m_input(line), m_slice_len(0), m_seps(separators), m_first(true)
	{}

	void reset(string_view line)
	{
		m_input=line;
        m_slice_len=0;
		m_first=true;
    }
    bool Next()
	{
        if (m_input.length() == m_slice_len)
			return false;
        m_input.remove_prefix(m_slice_len);

        if (StringSeparator)
        {
            if (StrictDelimiter)
            {
                if(!m_first)
                    m_input.remove_prefix(m_seps.length());
                else
                    m_first = false;
            }
            else
            {
                while (startsWith(m_input, m_seps))
                    m_input.remove_prefix(m_seps.length());
                if (m_input.empty())
                    return false;
            }
            m_slice_len = m_input.find(m_seps);
        }
        else
        {
            if (StrictDelimiter)
            {
                if(!m_first)
                    m_input.remove_prefix(1);
                else
                    m_first = false;
            }
            else
            {
                trimFront(m_input, m_seps);
                if(m_input.empty())
                    return false;
            }

            m_slice_len = m_input.find_first_of(m_seps);
        }

        if (m_slice_len == std::string::npos)
            m_slice_len = m_input.length();

        return true;
    }

	// access operators for the current slice
    inline std::string str() const { return std::string(m_input.data(), m_slice_len); }
	inline operator std::string() const { return str(); }
    inline string_view view() const { return string_view(m_input.data(), m_slice_len); }
        /**
         * @brief Report the remaining part of the string after current token
         * Unless strict delimiter mode is specified, will also trim the end of the resulting string_view.
         * @return Right part of the string, might be empty (but invalid) string_view() if not found
         */
        string_view right()
        {
            if (m_input.length() == m_slice_len)
                    return string_view();
            string_view ret(m_input.substr(m_slice_len));

            if (!m_first && !ret.empty())
            {
                ret.remove_prefix(StringSeparator ? m_seps.length() : 1);
             }

            if (StrictDelimiter)
                return ret;

            if (StringSeparator)
            {
                while(startsWith(m_input, m_seps))
                    m_input.remove_prefix(m_seps.length());

                while(endsWith(m_input, m_seps))
                    m_input.remove_suffix(m_seps.length());
            }
            else
            {
                trimBoth(ret, m_seps);
            }
            return ret;
        }

	struct iterator
	{

              using iterator_category = std::input_iterator_tag;
    using value_type = string_view;
    using difference_type = string_view;
    using pointer = const long*;
    using reference = string_view;

        tSplitWalkBase* _walker = nullptr;
		// default is end sentinel
		bool bEol = true;
		iterator() {}
        iterator(tSplitWalkBase& walker) : _walker(&walker) { bEol = !walker.Next(); }
		// just good enough for basic iteration and end detection
		bool operator==(const iterator& other) const { return (bEol && other.bEol); }
		bool operator!=(const iterator& other) const { return !(other == *this); }
		iterator& operator++() { bEol = !_walker->Next(); return *this; }
	    iterator operator++(int) {iterator retval = *this; ++(*this); return retval;}
		auto operator*() { return _walker->view(); }
	};
	iterator begin() {return iterator(*this); }
	iterator end() { return iterator(); }
	// little shortcut for collection creation; those methods are destructive to the parent, can only be used once
	std::vector<string_view> to_vector() { return std::vector<string_view>(begin(), end()); }
	std::vector<string_view> to_vector(unsigned nElementsToExpect)
		{
		std::vector<string_view> ret(nElementsToExpect);
		ret.assign(begin(), end());
		return ret;
		}
	std::deque<string_view> to_deque() { return std::deque<string_view>(begin(), end()); }

};

using tSplitWalk = tSplitWalkBase<false, false>;
using tSplitWalkStrict = tSplitWalkBase<false, true>;
using tSplitByStr = tSplitWalkBase<true, false>;
using tSplitByStrStrict = tSplitWalkBase<true, true>;

inline int strcasecmp(string_view a, string_view b)
{
	if(a.length() < b.length())
		return int(b.length()) +1;
	if(a.length() > b.length())
		return int(-a.length()) - 1;
	return strncasecmp(a.data(), b.data(), a.length());
}

std::string GetBaseName(const std::string &in);
std::string GetDirPart(const std::string &in);
std::pair<std::string,std::string> SplitDirPath(const std::string& in);
std::string PathCombine(string_view a, string_view b);

bool scaseequals(string_view a, string_view b);

void fish_longest_match(string_view stringToScan, const char sep,
		std::function<bool(string_view)> check_ex);

void replaceChars(std::string &s, const char* szBadChars, char goodChar);
inline std::string to_string(string_view s) {return std::string(s.data(), s.length());}

inline long svtol(string_view& sv, long errorValue = -1)
{
        char *endchar = nullptr;
        auto val = strtol(sv.data(), &endchar, 10);
        if (!endchar || !*endchar)
                return errorValue;
        sv.remove_prefix(endchar - sv.data());
        return val;
}

// C strings don't know length and std::string is wasteful if used optionally -> needing a Short Optional String
// typical payload is longer than SSO optimized range and shorter than 80 chars
class sostring
{
	char* data;
	sostring(const sostring&);
	sostring& operator=(const sostring&);
public:
	sostring() : data(nullptr) {}
	operator string_view() { return string_view(data + 1, (size_t) data[0]); }
	operator bool() { return data; }
	~sostring() { delete data; }
	sostring(string_view sv) {
		data = new char[sv.size()+2];
		data[0] = sv.size();
		memcpy(data+1, sv.data(), size_t(data[0]));
		data[data[0] + 1] ='\0';
	}
};

}


#endif /* INCLUDE_ASTROP_H_ */
