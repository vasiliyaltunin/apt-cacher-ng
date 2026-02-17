#ifndef AHTTPURL_H
#define AHTTPURL_H

#include "actypes.h"
#include "portutils.h"

namespace acng
{

extern std::string sDefPortHTTP, sDefPortHTTPS;
extern cmstring PROT_PFX_HTTPS, PROT_PFX_HTTP;

#define DEFAULT_PORT_HTTP 80
#define DEFAULT_PORT_HTTPS 443

class ACNG_API tHttpUrl
{

private:
		uint16_t nPort = 0;

public:
        bool SetHttpUrl(cmstring &uri, bool unescape = true);
		mstring ToURI(bool bEscaped, bool hostOnly = false) const;
        mstring sHost, sPath, sUserPass;

        bool bSSL=false;
        inline cmstring & GetProtoPrefix() const
        {
                return bSSL ? PROT_PFX_HTTPS : PROT_PFX_HTTP;
        }
        tHttpUrl(const acng::tHttpUrl& a)
        {
                sHost = a.sHost;
				nPort = a.nPort;
                sPath = a.sPath;
                sUserPass = a.sUserPass;
                bSSL = a.bSSL;
        }
        tHttpUrl & operator=(const tHttpUrl &a)
        {
                if(&a == this) return *this;
                sHost = a.sHost;
				nPort = a.nPort;
                sPath = a.sPath;
                sUserPass = a.sUserPass;
                bSSL = a.bSSL;
                return *this;
        }
        bool operator==(const tHttpUrl &a) const
        {
				return a.sHost == sHost && a.nPort == nPort && a.sPath == sPath
                                && a.sUserPass == sUserPass && a.bSSL == bSSL;
        }

		bool operator!=(const tHttpUrl &a) const
        {
                return !(a == *this);
        }
        inline void clear()
        {
                sHost.clear();
				nPort = 0;
                sPath.clear();
                sUserPass.clear();
                bSSL = false;
        }
		uint16_t GetDefaultPortForProto() const {
				return bSSL ? DEFAULT_PORT_HTTPS : DEFAULT_PORT_HTTP;
        }
		uint16_t GetPort(uint16_t defVal) const { return nPort ? nPort : defVal; };
		uint16_t GetPort() const { return GetPort(GetDefaultPortForProto()); }
		void SetPort(uint16_t nPort) { this->nPort = nPort; }

		inline tHttpUrl(cmstring &host, uint16_t port, bool ssl) :
						nPort(port), sHost(host), bSSL(ssl)
        {
        }
        inline tHttpUrl() =default;
		// special short version with only hostname and port number
		std::string GetHostPortKey() const;
};


}

#endif // AHTTPURL_H
