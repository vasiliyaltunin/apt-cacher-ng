#ifndef REMOTEDB_H
#define REMOTEDB_H

#include "meta.h"
#include "remotedbtypes.h"
#include "ahttpurl.h"

namespace acng
{

struct tRepoData
{
        std::vector<tHttpUrl> m_backends;

        // dirty little helper to execute custom actions when a jobs associates or forgets this data set
        tRepoUsageHooks *m_pHooks = nullptr;
        std::vector<mstring> m_keyfiles;
        tHttpUrl m_deltasrc;
        tHttpUrl *m_pProxy = nullptr;
        virtual ~tRepoData();
};

class remotedb
{
public:
    /**
         * @brief GetInstance retrieves a singleton instance
         * @return Access to the global instance which is created on application startup
         */
        static remotedb& GetInstance();

        /*
         * Resolves a repository descriptor for the given URL, returns a reference to its descriptor
         * (actually a pair with first: name, second: descriptor).
         *
         * @return: true IFF a repository was found and the by-reference arguments are set
         */
        virtual tRepoResolvResult GetRepNameAndPathResidual(const tHttpUrl & uri);

        virtual const tRepoData * GetRepoData(cmstring &vname);

        virtual void PostConfig();

        virtual time_t BackgroundCleanup();

		virtual ~remotedb() =default;
private:
        remotedb() =default;
};

}

#endif // REMOTEDB_H
