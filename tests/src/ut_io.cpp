#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "fileio.h"
#include "acbuf.h"

#include <fcntl.h>

using namespace acng;

#define PERMS 00664

TEST(fileio,fsattr)
{
	Cstat st("/bin/sh");
	Cstat st2("/etc/resolv.conf");
	ASSERT_TRUE(st);
	ASSERT_EQ(st.st_dev, st2.st_dev);
}

TEST(fileio, operations)
{
	ASSERT_EQ(0, FileCopy("/etc/passwd", "passwd_here").value());
}

TEST(fileio, ut_acbuf)
{
	tSS x;
	auto path = "dummy.file";
	x << "y";
	ASSERT_EQ(1, x.dumpall(path, O_CREAT, PERMS, INT_MAX, true));
	x << "zzz";
	ASSERT_EQ(3, x.dumpall(path, O_CREAT, PERMS, INT_MAX, false));
	x << "y";
	ASSERT_EQ(1, x.dumpall(path, O_CREAT, PERMS, INT_MAX, true));
	Cstat st(path);
	ASSERT_EQ(1, st.st_size);
}
