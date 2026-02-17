/**
  * Part of apt-cacher-ng
  * Copyright (c) 2007-2021 Eduard Bloch
  * Implements portable conversion of errno codes to readable strings.
  */
#include "meta.h"
#include "acbuf.h"
#include <string.h>

namespace acng {

// let the compiler decide between GNU and XSI version
inline void add_msg(int r, int err, const char* buf, mstring *p)
{
	if(r)
		p->append(tSS() << "UNKNOWN ERROR: " << err);
	else
		p->append(buf);
}

inline void add_msg(const char *msg, int , const char* , mstring *p)
{
	p->append(msg);
}

// XXX: use string_view? or use string and move it?
void tErrnoFmter::fmt(int err, const char *prefix)
{
	char buf[64];
	buf[0]=buf[sizeof(buf)-1]=0x0;
	if(prefix)
		assign(prefix);
	add_msg(strerror_r(err, buf, sizeof(buf)-1), err, buf, this);
}

}
