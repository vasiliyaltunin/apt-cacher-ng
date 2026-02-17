
#include "acbuf.h"
#include "fileio.h"
#include "sockio.h"
#include "acfg.h"
#include "meta.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace acng
{
bool acbuf::setsize(unsigned int c) {
	if(m_nCapacity==c)
		return true;

	char *p = (char*) realloc(m_buf, c+1);
	if(!p)
		return false;

	m_buf=p;
	m_nCapacity=c;
	m_buf[c]=0; // terminate to make string operations safe
    return true;
}

bool acbuf::initFromFile(const char *szPath, off_t limit)
{
    Cstat st(szPath);

	unique_fd fd(open(szPath, O_RDONLY));
	if (!fd.valid())
		return false;
	clear();
    if(!setsize(std::min(limit, st.st_size)))
		return false;
	while (freecapa() > 0)
	{
		if (sysread(fd.m_p) < 0)
			return false;
	}
    return size() == st.st_size;
}

ssize_t acbuf::dumpall(int fd, ssize_t limit) {

	if (size_t(limit) > size())
        limit = size();

	auto ret = limit;

    while (limit)
    {
        errno = 0;
        auto n = ::write(fd, rptr(), limit);

        if (n > limit) // heh?
		{
			errno = EOVERFLOW;
			return -1;
		}

        if (n <= 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                continue;
			ret = -1;
			break;
        }

        drop(n);
        limit -= n;

		if (limit <=0)
			break;
    }
	return ret;
}

ssize_t acbuf::dumpall(const char *path, int flags, int perms, ssize_t limit, bool doTruncate)
{
    unique_fd tmp(open(path, O_WRONLY | O_BINARY | flags, perms));
	if (!tmp.valid())
		return -1; // keep the errno

	auto ret = dumpall(tmp.m_p, limit);
	if (ret == -1)
	{
		// rescue the errno of the original error
		auto e = errno;
		checkforceclose(tmp.m_p);
		errno = e;
		return -1;
    }
    while (tmp.valid())
    {
		if (doTruncate)
		{
			auto pos = lseek(tmp.m_p, 0, SEEK_CUR);
			if (pos < 0)
				return -1;
			pos = ftruncate(tmp.m_p, pos);
			if (pos < 0)
				return pos;
		}
        if (0 == ::close(tmp.m_p))
        {
            tmp.release();
			return ret;
        }
        if (errno != EINTR)
			return -1;
    };
	return ret;
}

int acbuf::sysread(int fd, unsigned int maxlen)
{
	size_t todo(std::min(maxlen, freecapa()));
	int n;
	do {
		n=::read(fd, m_buf+w, todo);
	} while( (n<0 && EINTR == errno) /* || (EAGAIN == errno && n<=0) */ ); // cannot handle EAGAIN here, let the caller check errno
    if(n<0)
    	return -errno;
    if(n>0)
        w+=n;
    return(n);
}

bool tSS::send(int nConFd, mstring* sErrorStatus)
{
	while (!empty())
	{
		auto n = ::send(nConFd, rptr(), size(), 0);
		if (n > 0)
		{
			drop(n);
			continue;
		}
		if (n <= 0)
		{
			if (EINTR == errno || EAGAIN == errno)
			{
				fd_set wfds;
				FD_ZERO(&wfds);
				FD_SET(nConFd, &wfds);
				auto r=::select(nConFd + 1, nullptr, &wfds, nullptr, CTimeVal().ForNetTimeout());
				if(!r && errno != EINTR)
				{
					if(sErrorStatus)
						*sErrorStatus = "Socket timeout";
					return false;
				}
				continue;
			}

			if(sErrorStatus)
				*sErrorStatus = tErrnoFmter("Socket error, ");
			return false;
		}
	}
	return true;
}

bool tSS::recv(int nConFd, mstring* sErrorStatus)
{
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(nConFd, &rfds);
	auto r = ::select(nConFd + 1, &rfds, nullptr, nullptr, CTimeVal().ForNetTimeout());
	if (!r)
	{
		if(errno == EINTR)
			return true;

		if(sErrorStatus)
			*sErrorStatus = "Socket timeout";
		return false;
	}
	// must be readable
	r = ::recv(nConFd, wptr(), freecapa(), 0);
	if(r<=0)
	{
		if(sErrorStatus)
			*sErrorStatus = tErrnoFmter("Socket error, ");
		return false;
	}
	got(r);
	return true;
}

}
