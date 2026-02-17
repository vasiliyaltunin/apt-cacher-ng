#include "csmapping.h"
#include "meta.h"

bool acng::tFingerprint::SetCs(const acng::mstring &hexString, acng::CSTYPES eCstype)
{
	auto l = hexString.size();
	if(!l || l%2) // weird sizes...
		return false;
	if(eCstype == CSTYPE_INVALID)
	{
		eCstype = GuessCStype(hexString.size() / 2);
		if(eCstype == CSTYPE_INVALID)
			return false;
	}
	else if(l != 2*GetCSTypeLen(eCstype))
		return false;

	csType=eCstype;
	return CsAsciiToBin(hexString.c_str(), csum, l/2);
}

bool acng::tFingerprint::Set(acng::tSplitWalk& splitInput, acng::CSTYPES wantedType)
{
	if(!splitInput.Next())
		return false;
	if(!SetCs(splitInput.str(), wantedType))
		return false;
	if(!splitInput.Next())
		return false;
	size = atoofft(splitInput.str().c_str(), -1);
	if(size < 0)
		return false;
	return true;
}

acng::mstring acng::tFingerprint::GetCsAsString() const
{
	return BytesToHexString(csum, GetCSTypeLen(csType));
}

acng::tFingerprint::operator mstring() const
{
	return GetCsAsString()+"_"+offttos(size);
}

bool acng::tFingerprint::CheckFile(acng::cmstring &sFile) const
{
	if(size != GetFileSize(sFile, -2))
		return false;
	tFingerprint probe;
	if(!probe.ScanFile(sFile, csType, false, nullptr))
		return false;
	return probe == *this;
}

bool acng::tRemoteFileInfo::SetFromPath(acng::cmstring &sPath, acng::cmstring &sBaseDir)
{
	if (sPath.empty())
		return false;

	tStrPos pos = sPath.rfind(SZPATHSEPUNIX);
	if (pos == stmiss)
	{
		sFileName = sPath;
		sDirectory = sBaseDir;
	}
	else
	{
		sFileName = sPath.substr(pos + 1);
		sDirectory = sBaseDir + sPath.substr(0, pos + 1);
	}
	return true;
}

bool acng::tRemoteFileInfo::SetSize(acng::LPCSTR szSize)
{
	auto l = atoofft(szSize, -2);
	if(l < 0)
		return false;
	fpr.size = l;
	return true;
}
