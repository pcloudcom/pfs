#ifndef _PFS_SETTINGS_H
#define _PFS_SETTINGS_H

#include "common.h"

#if defined(MINGW) || defined(_WIN32)
struct iovec
{
  void*   iov_base;
  size_t  iov_len;
};
#else
#include <sys/uio.h>
#endif

#define SETTINGS_PATH "/.pfs_settings"

#define MIN_CACHE_SIZE 0
#define MAX_CACHE_SIZE ((sizeof(size_t)<=4)? (size_t)2*1024*1024*1024 : 16L*1024*1024*1024)

#define RECONNECT_TIMEOUT 15

const char **list_settings();
const struct stat *get_setting_stat(const char *name);
int set_setting(const char *name, const char *val, size_t vallen);
int get_setting(const char *name, char *val, size_t *vallen);
void event_writev(const struct iovec *iov, int iovcnt);

#endif
