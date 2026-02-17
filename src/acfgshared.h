#ifndef ACFGSHARED_H
#define ACFGSHARED_H

/**
  * Internal code shared between acfg and remotedb. Shall not be used by anyone else!
  */

#include "acfg.h"
#include "filereader.h"
#include "meta.h"

using namespace std;

namespace acng
{

void AddRemapInfo(bool bAsBackend, const string & token, const string &repname);
void AddRemapFlag(const string & token, const string &repname);
void _AddHooksFile(cmstring& vname);

namespace cfg
{

bool ParseOptionLine(const string &sLine, string &key, string &val);
void _FixPostPreSlashes(string &val);
tStrDeq ExpandFileTokens(cmstring &token);

// shortcut for frequently needed code, opens the config file, reads step-by-step
// and skips comment and empty lines
struct tCfgIter
{
        filereader reader;
        string sLine;
        string sFilename;
        tCfgIter(cmstring &fn);
        //inline operator bool() const { return reader.CheckGoodState(false, &sFilename); }
        bool Next();
};

extern string sPopularPath;

}

}

#endif // ACFGSHARED_H
