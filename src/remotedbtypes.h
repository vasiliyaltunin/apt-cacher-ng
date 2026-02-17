#ifndef REMOTEDBTYPES_H
#define REMOTEDBTYPES_H

/**
  * Liteweight header used for shared functionality across remote communication code.
  */

#include "actypes.h"

namespace acng
{

struct tRepoData;

struct tRepoUsageHooks
{
        virtual void OnAccess()=0;
        virtual void OnRelease()=0;
        virtual ~tRepoUsageHooks() =default;
};

struct tRepoResolvResult {
        cmstring* psRepoName=nullptr;
        mstring sRestPath;
        const tRepoData* repodata=nullptr;
};


}

#endif // REMOTEDBTYPES_H
