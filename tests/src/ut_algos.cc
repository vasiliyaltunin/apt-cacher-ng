#include "gtest/gtest.h"
//#include "dbman.h"
#include "csmapping.h"
#include "acfg.h"

#include "ahttpurl.h"
#include "astrop.h"

#include "gmock/gmock.h"

#include <unordered_map>

namespace acng
{
        void check_algos();
}

#if 0
TEST(algorithms, checksumming)
{
	ASSERT_NO_THROW(acng::check_algos());

	acng::tChecksum cs, csmd(acng::CSTYPES::MD5);
	cs.Set("a26a96c0c63589b885126a50df83689cc719344f4eb4833246ed57f42cf67695a2a3cefef5283276cbf07bcb17ed7069de18a79410a5e4bc80a983f616bf7c91");

	auto cser2 = acng::csumBase::GetChecker(acng::CSTYPES::SHA512);
	cser2->add("bf1942");
	auto cs2=cser2->finish();
	ASSERT_EQ(cs2, cs);

	ASSERT_NE(csmd, cs2);
}
#endif

TEST(algorithms,bin_str_long_match)
{
	using namespace acng;
	std::map<string_view,int> testPool = {
			{"abra/kadabra", 1},
			{"abba", 2},
			{"something/else", 3},
	};
	auto best_result = testPool.end();
	auto probe_count=0;
	// the costly lookup operation we want to call as few as possible
	auto matcher = [&](string_view tstr) ->bool
		{
		probe_count++;
		auto it = testPool.find(tstr);
		if(it==testPool.end())
			return false;
		// pickup the longest seen result
		if(best_result == testPool.end() || tstr.length() > best_result->first.length())
			best_result=it;
		return true;
		};

	fish_longest_match("something/else/matters/or/not", '/', matcher);
	ASSERT_NE(testPool.end(), best_result);
	ASSERT_EQ(best_result->first, "something/else");
	best_result = testPool.end();
	// not to be found
	fish_longest_match("something/elsewhere/matters/or/not", '/', matcher);
	ASSERT_EQ(testPool.end(), best_result);

	testPool.emplace("something", 42);
	fish_longest_match("something/elsewhere/matters/or/not", '/', matcher);
	ASSERT_NE(testPool.end(), best_result);
	ASSERT_EQ(best_result->first, "something");
	best_result = testPool.end();

	fish_longest_match("", '/', matcher);
	ASSERT_EQ(testPool.end(), best_result);

	fish_longest_match("/", '/', matcher);
	ASSERT_EQ(testPool.end(), best_result);

	acng::fish_longest_match("abbakus", '/', matcher);
	ASSERT_EQ(testPool.end(), best_result);

	acng::fish_longest_match("abba/forever.deb", '/', matcher);
	ASSERT_NE(testPool.end(), best_result);
	EXPECT_EQ(best_result->first, "abba");
	best_result = testPool.end();

	acng::fish_longest_match("abra/kadabra/veryverylongletusseewhathappenswhenthestirng,lengthiswaytoomuchorwhat", '/', matcher);
	ASSERT_NE(testPool.end(), best_result);
	EXPECT_EQ(best_result->first, "abra/kadabra");
	best_result = testPool.end();

	acng::fish_longest_match("veryverylongletusseewhathappenswhenthestirng,lengthiswaytoomuchorwhat", '/', matcher);
	ASSERT_EQ(testPool.end(), best_result);
	acng::fish_longest_match("something/else", '/', matcher);
	ASSERT_NE(testPool.end(), best_result);
	ASSERT_EQ(best_result->first, "something/else");
	best_result = testPool.end();

	// the longest match
	fish_longest_match("abra/kadabra", '/', matcher);
	ASSERT_NE(testPool.end(), best_result);
	ASSERT_EQ(best_result->first, "abra/kadabra");
	best_result = testPool.end();

	probe_count=0;
	fish_longest_match("bla bla blub ganz viele teile wer hat an der uhr ge dreh t ist es wir klich schon so frueh", ' ', matcher);
	ASSERT_EQ(testPool.end(), best_result);
	ASSERT_LE(probe_count, 5);
}

TEST(strop,views)
{
	using namespace acng;
	string_view a = "foo  ", b = "  foo";
	auto x=a;
	trimFront(x);
	ASSERT_EQ(x, "foo  ");
	x=a;
	trimBack(x);
	ASSERT_EQ(x, "foo");
	auto prex=b;
	trimFront(prex);
	ASSERT_EQ(prex, "foo");
	prex=b;
	trimBack(prex);
	ASSERT_EQ(prex, "  foo");

	string_view xtra("  ");
	trimFront(xtra);
	ASSERT_TRUE(xtra.empty());
	ASSERT_TRUE(xtra.data());
	ASSERT_FALSE(* xtra.data());
	xtra = "  ";
	// compiler shall use the same memory here
	ASSERT_EQ((void*) xtra.data(), (void*) "  ");
	trimBack(xtra);
	ASSERT_EQ((void*) xtra.data(), (void*) "  ");
	ASSERT_TRUE(xtra.empty());

	ASSERT_EQ("foo/bar", PathCombine(string_view(WITHLEN("foo")), string_view(WITHLEN("bar"))));
	ASSERT_EQ("foo/bar", PathCombine("foo", "/bar"));
	ASSERT_EQ("foo/bar", PathCombine(string_view(WITHLEN("foo/")), string_view(WITHLEN("/bar"))));
	ASSERT_EQ("foo/bar", PathCombine(string_view(WITHLEN("foo/")), string_view(WITHLEN("bar"))));

	tHttpUrl url;
	ASSERT_TRUE(url.SetHttpUrl("http://meh:1234/path/more/path?fragment"));
}

TEST(strop,splitter)
{
	using namespace acng;
	std::deque<string_view> exp {"foo", "bar", "blalba"};

    tSplitWalk tknzr("  foo bar blalba", SPACECHARS);
	std::deque<string_view> result;
	for(auto it:tknzr) result.emplace_back(it);
	ASSERT_EQ(result, exp);
//#error und jetzt fuer stricten splitter

	tknzr.reset("foo    bar blalba    ");
//	std::vector<string_view> result2(tknzr.begin(), tknzr.end());

	auto q = tknzr.to_deque();
	ASSERT_EQ(exp, q);

	ASSERT_EQ(result, q);

    tSplitWalkStrict strct("a;bb;;c", ";");
	std::deque<string_view> soll {"a", "bb", "", "c"};
	ASSERT_EQ(soll, strct.to_deque());
	strct.reset(";a;bb;;c");
	q = strct.to_deque();
	ASSERT_NE(soll, q);
	ASSERT_EQ(q.front(), "");
	q.pop_front();
	ASSERT_EQ(soll, q);
	strct.reset(";a;bb;;c;");
	q = strct.to_deque();
	ASSERT_EQ(q.size(), 6);
	ASSERT_EQ(q.front(), "");
	ASSERT_EQ(q.back(), "");

    tSplitWalk white("a b");
    ASSERT_TRUE(white.Next());
    ASSERT_EQ(white.right(), "b");

    tSplitWalk blue("a b    ");
    ASSERT_TRUE(blue.Next());
    ASSERT_EQ(blue.right(), "b");

    tSplitByStr xspliter("!!as!!!df!!gh", "!!");
    auto sq = xspliter.to_deque();
    ASSERT_EQ(3, sq.size());
    ASSERT_EQ(sq[1], "!df");
    ASSERT_EQ(sq[2], "gh");

    tSplitByStrStrict yspliter("!!as!!!df!!gh", "!!");
    ASSERT_TRUE(yspliter.Next());
    ASSERT_TRUE(yspliter.view().empty());
    ASSERT_TRUE(yspliter.Next());
    ASSERT_EQ(yspliter.view(), "as");
    ASSERT_TRUE(yspliter.Next());
    ASSERT_EQ(yspliter.view(), "!df");
    ASSERT_TRUE(yspliter.Next());
    ASSERT_EQ(yspliter.view(), "gh");
    ASSERT_FALSE(yspliter.Next());
}

TEST(algorithms, rex)
{
	using namespace acng;
	using namespace acng::rex;
	CompileExpressions();
	auto type = GetFiletype("http://debug.mirrors.debian.org/debian-debug/dists/sid-debug/main/i18n/Translation-de.xz");
	ASSERT_EQ(type, FILE_VOLATILE);
	type = GetFiletype("debrep/dists/unstable/contrib/dep11/by-hash/SHA256/60fe36491abedad8471a0fb3c4fe0b5d73df8b260545ee4aba1a26efa79cdceb");
	ASSERT_EQ(type, FILE_SOLID);
	auto misc = R"END(
http://ftp.ch.debian.org/debian/dists/unstable/InRelease
http://ftp.ch.debian.org/debian/dists/unstable/main/binary-amd64/Packages.xz
http://ftp.ch.debian.org/debian/dists/unstable/main/binary-i386/Packages.xz
http://ftp.ch.debian.org/debian/dists/unstable/main/binary-all/Packages.xz
http://ftp.ch.debian.org/debian/dists/unstable/main/i18n/Translation-de_DE.xz
http://ftp.ch.debian.org/debian/dists/unstable/main/i18n/Translation-de.xz
http://ftp.ch.debian.org/debian/dists/unstable/main/i18n/Translation-en.xz
http://ftp.ch.debian.org/debian/dists/unstable/main/dep11/Components-amd64.yml.xz
http://ftp.ch.debian.org/debian/dists/unstable/main/dep11/Components-all.yml.xz
http://ftp.ch.debian.org/debian/dists/unstable/main/dep11/icons-48x48.tar.xz
http://ftp.ch.debian.org/debian/dists/unstable/main/dep11/icons-64x64.tar.xz
http://ftp.ch.debian.org/debian/dists/unstable/main/Contents-amd64.xz
http://ftp.ch.debian.org/debian/dists/unstable/main/Contents-i386.xz
http://ftp.ch.debian.org/debian/dists/unstable/main/Contents-all.xz
http://ftp.ch.debian.org/debian/dists/unstable/non-free/binary-amd64/Packages.xz
http://ftp.ch.debian.org/debian/dists/unstable/non-free/binary-i386/Packages.gz
http://ftp.ch.debian.org/debian/dists/unstable/non-free/binary-all/Packages.bz2
http://ftp.ch.debian.org/debian/dists/unstable/non-free/i18n/Translation-de_DE.xz
http://ftp.ch.debian.org/debian/dists/unstable/non-free/i18n/Translation-de.xz
http://ftp.ch.debian.org/debian/dists/unstable/non-free/i18n/Translation-en.xz
http://ftp.ch.debian.org/debian/dists/unstable/non-free/dep11/Components-amd64.yml.xz
http://ftp.ch.debian.org/debian/dists/unstable/non-free/dep11/Components-all.yml.xz
http://ftp.ch.debian.org/debian/dists/unstable/non-free/dep11/icons-48x48.tar.xz
http://ftp.ch.debian.org/debian/dists/unstable/non-free/dep11/icons-64x64.tar.xz
http://ftp.ch.debian.org/debian/dists/unstable/non-free/Contents-amd64.xz
http://ftp.ch.debian.org/debian/dists/unstable/non-free/Contents-i386.xz
http://ftp.ch.debian.org/debian/dists/unstable/non-free/Contents-all.xz
http://ftp.ch.debian.org/debian/dists/unstable/contrib/binary-amd64/Packages.xz
http://ftp.ch.debian.org/debian/dists/unstable/contrib/binary-i386/Packages.xz
http://ftp.ch.debian.org/debian/dists/unstable/contrib/binary-all/Packages.xz
http://ftp.ch.debian.org/debian/dists/unstable/contrib/i18n/Translation-de_DE.xz
http://ftp.ch.debian.org/debian/dists/unstable/contrib/i18n/Translation-de.xz
http://ftp.ch.debian.org/debian/dists/unstable/contrib/i18n/Translation-en.xz
http://ftp.ch.debian.org/debian/dists/unstable/contrib/dep11/Components-amd64.yml.xz
http://ftp.ch.debian.org/debian/dists/unstable/contrib/dep11/Components-all.yml.xz
http://ftp.ch.debian.org/debian/dists/unstable/contrib/dep11/icons-48x48.tar.xz
http://ftp.ch.debian.org/debian/dists/unstable/contrib/dep11/icons-64x64.tar.xz
http://ftp.ch.debian.org/debian/dists/unstable/contrib/Contents-amd64.xz
http://ftp.ch.debian.org/debian/dists/unstable/contrib/Contents-i386.xz
http://ftp.ch.debian.org/debian/dists/unstable/contrib/Contents-all.xz
)END";
	for(tSplitWalk split(misc); split.Next();)
	{
		type = GetFiletype(split);
		EXPECT_EQ(type, FILE_VOLATILE);
	}
}
