#ifndef _RADMSTRUCTURES_H
#define _RADMSTRUCTURES_H

#include <stdint.h>

#define NOALIGN __attribute__((packed))
#define NOALIGN8 __attribute__((packed, aligned(8)))

#define RADM_PORT 8377
#define SECRET "pik4o"

#define RADM_CREATE_DIR     1
#define RADM_DELETE_DIR     2
#define RADM_SEND_FILE      3
#define RADM_SEND_FILE_META 4
#define RADM_SEND_DIR_META  5
#define RADM_DELETE_FILE    6
#define RADM_RENAME         7
#define RADM_RECV_FILE      8
#define RADM_COPY_FILE      9
#define RADM_FILE_CHECKSUM  10

#define RADM_NEW_SENDFILE   20
#define RADM_NEW_FSYNC      21
#define RADM_NEW_DELETE     22
#define RADM_NEW_CHECKSUM   23
#define RADM_NEW_COPY       24
#define RADM_NEW_RECVFILE   25
#define RADM_NEW_CREATEFILE 26
#define RADM_NEW_CLOSEFILE  27
#define RADM_NEW_SETBLOBID  28
#define RADM_NEW_DELETETEMP 29
#define RADM_NEW_CREATEFROM 30
#define RADM_NEW_TRUNCATE   31
#define RADM_NEW_SEEK       32
#define RADM_NEW_TELL       33
#define RADM_NEW_WRITE      34
#define RADM_NEW_READ       35
#define RADM_NEW_PWRITE     36
#define RADM_NEW_PREAD      37
#define RADM_NEW_STAT       38
#define RADM_NEW_PREPARE    39
#define RADM_NEW_THUMB      40
#define RADM_NEW_PART_CHECK 41
#define RADM_NEW_RPRT_CHECK 42

#define RADM_OPTS_FSYNC       (1<<0)
#define RADM_OPTS_ODIRECT     (1<<1)
#define RADM_OPTS_COPYONCLOSE 0
#define RADM_OPTS_COPYONOPEN  (1<<2)
#define RADM_OPTS_APPEND      (1<<3)
#define RADM_OPTS_RANDIO      (1<<4)

#define radm_len_struct(type) struct {uint32_t len; type st;} __attribute__((packed))

typedef struct {
  uint64_t tm;
  uint32_t rnd;
  char pass[32];
} NOALIGN8 radm_auth;

typedef struct {
  char ok[2];
  char _pad[6];
  uint64_t filesize;
  char sha1hex[40];
  char md5hex[32];
} NOALIGN radm_close_resp;

typedef struct {
  char ok[2];
  char _pad[6];
  uint64_t filesize;
  uint32_t width;
  uint32_t height;
  char sha1hex[40];
  char md5hex[32];
} NOALIGN radm_thumb_resp;

typedef struct {
  char ok[2];
  char _pad[6];
  uint64_t pos;
} NOALIGN radm_seek_resp;

typedef struct {
  char ok[2];
  char _pad[6];
  uint64_t length;
} NOALIGN radm_read_resp;

#define radm_recv_resp radm_read_resp
#define radm_part_check_resp radm_close_resp
#define radm_rprt_check_resp radm_close_resp

typedef struct {
  char ok[2];
  char _pad[6];
  uint64_t size;
} NOALIGN radm_stat_resp;

#define radm_tell_resp radm_seek_resp

typedef struct {
  uint32_t type;
  uint32_t uid;
  uint32_t gid;
  uint32_t mode;
  uint32_t atime;
  uint32_t ctime;
  uint32_t mtime;
  char path[];
} NOALIGN radm_createdir;

typedef struct {
  uint32_t type;
  char path[];
} NOALIGN radm_deletedir;

typedef struct {
  uint32_t type;
  char path[];
} NOALIGN radm_deletefile;

typedef struct {
  uint32_t type;
  char path[];
} NOALIGN radm_filechecksum;

typedef struct {
  uint32_t type;
  char path[];
} NOALIGN radm_receivefile;

typedef struct {
  uint32_t type;
  uint32_t uid;
  uint32_t gid;
  uint32_t mode;
  uint32_t atime;
  uint32_t ctime;
  uint32_t mtime;
  uint32_t filesize;
  char path[];
} NOALIGN radm_sendfile;

typedef struct {
  uint32_t type;
  char ip[16];
  char path[];
} NOALIGN radm_copy;

typedef struct {
  uint32_t type;
  uint32_t path1len;
  uint32_t path2len;
} NOALIGN radm_rename;

typedef struct {
  uint32_t type;
  uint32_t opts;
  uint64_t blobid;
  uint64_t filesize;
} NOALIGN radm_new_sendfile;

typedef struct {
  uint32_t type;
  uint32_t opts;
  uint64_t blobid;
} NOALIGN radm_new_blobid;

typedef struct {
  uint32_t type;
  uint32_t opts;
  uint64_t blobid;
  char ip[];
} NOALIGN radm_new_copy;

typedef struct {
  uint32_t type;
  uint32_t opts;
  uint64_t blobid;
  uint64_t offset;
  uint64_t len;
} NOALIGN radm_new_recvfile;

#define radm_new_rprt_check radm_new_recvfile 

typedef struct {
  uint32_t type;
  uint32_t opts;
} NOALIGN radm_new_createfile;

typedef struct {
  uint32_t type;
  uint32_t opts;
} NOALIGN radm_new_closefile;

typedef struct {
  uint32_t type;
} NOALIGN radm_new_deletetemp;

typedef struct {
  uint32_t type;
  uint32_t opts;
  uint64_t length;
} NOALIGN radm_new_truncate;

typedef struct {
  uint32_t type;
  uint32_t opts;
  uint64_t offset;
  uint32_t whence;
} NOALIGN radm_new_seek;

typedef struct {
  uint32_t type;
  uint32_t opts;
  uint64_t offset;
  uint64_t length;
} NOALIGN radm_new_pread;

typedef struct {
  uint32_t type;
  uint32_t opts;
  uint64_t blobid;
  uint32_t width;
  uint32_t height;
  uint32_t thopts;
  char ext[4];
} NOALIGN radm_new_thumb;

#define radm_new_fsync radm_new_blobid
#define radm_new_delete radm_new_blobid
#define radm_new_checksum radm_new_blobid
#define radm_new_setblobid radm_new_blobid
#define radm_new_createfrom radm_new_blobid
#define radm_new_prepare radm_new_blobid
#define radm_new_tell radm_new_deletetemp
#define radm_new_write radm_new_truncate
#define radm_new_read radm_new_truncate
#define radm_new_pwrite radm_new_pread
#define radm_new_stat radm_new_deletetemp
#define radm_new_part_check radm_new_pread

#endif
