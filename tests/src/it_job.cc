#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "httpdate.h"
#include "header.h"
#include "job.h"

#include <conn.h>

using namespace acng;
using namespace std;

class t_conn_dummy : public ISharedConnectionResources
{
    // ISharedConnectionResources interface
public:
    dlcon *SetupDownloader() override
    {
        return nullptr;
    }
    void LogDataCounts(cmstring &, mstring , off_t , off_t , bool ) override
    {
    }
	std::shared_ptr<IFileItemRegistry> GetItemRegistry() override
	{
		return std::shared_ptr<IFileItemRegistry>();
	}
} conn_dummy;

TEST(job, create)
{
    job j(conn_dummy);
    header h;
	j.Prepare(h, ""sv, "127.0.0.1");
    ASSERT_TRUE(j.m_sendbuf.view().find("403 Invalid path") != stmiss);
	auto hdata = "GET /na/asdfasdfsadf HTTP/1.1\r\n\r\n";
	auto res = h.Load(hdata);
    ASSERT_GT(res, 0);
	j.Prepare(h, hdata, "127.0.0.1");
    ASSERT_TRUE(j.m_sendbuf.view().find("HTTP/1.1 403 Forbidden file type or location") != stmiss);

#ifdef DEBUG
	j.Dispose();
#endif
}
