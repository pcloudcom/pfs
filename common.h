#ifndef _PFS_COMMON_H
#define _PFS_COMMON_H

#define FUSE_USE_VERSION 26

#include <fuse.h>

#define FS_BLOCK_SIZE 4096

extern uid_t myuid;
extern gid_t mygid;

typedef struct {
  size_t pagesize;
} pfs_settings;

extern pfs_settings fs_settings;

void reset_cache();

#endif