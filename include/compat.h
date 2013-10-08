#ifndef COMPAT_H_INCLUDED
#define COMPAT_H_INCLUDED

#if defined(MINGW) || defined(_WIN32)

#include <process.h>

#define sleep(X) Sleep ((X)* 1000)
#define index(X, Y) strchr(X, Y)
#define rindex(X, Y) strrchr(X, Y)

#define ENOTCONN WSAENOTCONN
#define ST_NOSUID 2

#define getuid() 0
#define getgid() 0

#else
//nothing at this point
#endif

#endif // COMPAT_H_INCLUDED
