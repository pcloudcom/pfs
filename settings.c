#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "common.h"
#include "settings.h"

#define SETTINGSCNT (sizeof(settings)/sizeof(setting))

typedef void (*get_setting_func)(char *, size_t *);
typedef int (*set_setting_func)(const char *, size_t);

typedef struct {
  const char *name;
  get_setting_func get;
  set_setting_func set;
} setting;

static void setting_get_size_t(char *data, size_t *sz, size_t num){
  *sz=snprintf(data, *sz, "%lu\n", (unsigned long)num);
}

static void setting_get_bool(char *data, size_t *sz, int b){
  *sz=2;
  if (b)
    data[0]='1';
  else
    data[0]='0';
  data[1]='\n';
  data[2]=0;
}

static void get_page_size(char *data, size_t *sz){
  setting_get_size_t(data, sz, fs_settings.pagesize);
}

static int set_page_size(const char *str, size_t len){
  size_t sz=atol(str);
  if (sz<1024 || sz>4*1024*1024 || (sz&(sz-1))!=0)
    return -EINVAL;
  fs_settings.pagesize=sz;
  reset_cache();
  return 0;
}

static void get_cache_size(char *data, size_t *sz){
  setting_get_size_t(data, sz, fs_settings.cachesize);
}

static int set_cache_size(const char *str, size_t len){
  size_t sz=atol(str);
  if (sz<fs_settings.pagesize*4)
    return -EINVAL;
  if (sz>MAX_CACHE_SIZE)
    return -EINVAL;
  fs_settings.cachesize=sz;
  reset_cache();
  return 0;
}

static void get_readahead_min(char *data, size_t *sz){
  setting_get_size_t(data, sz, fs_settings.readaheadmin);
}

static int set_readahead_min(const char *str, size_t len){
  size_t sz=atol(str);
  if (sz>fs_settings.readaheadmax)
    return -EINVAL;
  fs_settings.readaheadmin=sz;
  return 0;
}

static void get_readahead_max(char *data, size_t *sz){
  setting_get_size_t(data, sz, fs_settings.readaheadmax);
}

static int set_readahead_max(const char *str, size_t len){
  size_t sz=atol(str);
  if (sz<fs_settings.readaheadmin)
    return -EINVAL;
  fs_settings.readaheadmax=sz;
  return 0;
}

static void get_readahead_max_sec(char *data, size_t *sz){
  setting_get_size_t(data, sz, fs_settings.readaheadmaxsec);
}

static int set_readahead_max_sec(const char *str, size_t len){
  size_t sz=atol(str);
  fs_settings.readaheadmaxsec=sz;
  return 0;
}

static void get_use_ssl(char *data, size_t *sz){
  setting_get_bool(data, sz, fs_settings.usessl);
}

static int set_use_ssl(const char *str, size_t len){
  fs_settings.usessl=atoi(str)?1:0;
  return 0;
}

static setting settings[]={
  {"page_size", get_page_size, set_page_size},
  {"cache_size", get_cache_size, set_cache_size},
  {"readahead_min", get_readahead_min, set_readahead_min},
  {"readahead_max", get_readahead_max, set_readahead_max},
  {"readahead_max_sec", get_readahead_max_sec, set_readahead_max_sec},
  {"use_ssl", get_use_ssl, set_use_ssl}
};

static const char *setting_names[SETTINGSCNT+1];
struct stat setting_stat[SETTINGSCNT];
struct stat settings_stat;
static int names_init=0;

static void init_settings(){
  char buff[1024];
  time_t tm;
  size_t sz;
  int i;
  time(&tm);
  memset(setting_stat, 0, sizeof(setting_stat));
  memset(&settings_stat, 0, sizeof(settings_stat));
  for (i=0; i<SETTINGSCNT; i++){
    setting_names[i]=settings[i].name;
    sz=sizeof(buff);
    settings[i].get(buff, &sz);
    setting_stat[i].st_ctime=tm;
    setting_stat[i].st_mtime=tm;
    setting_stat[i].st_mode=S_IFREG | 0644;
    setting_stat[i].st_nlink=1;
    setting_stat[i].st_size=sz;
#if !defined(MINGW) && !defined(_WIN32)
    setting_stat[i].st_blocks=(sz+511)/512;
    setting_stat[i].st_blksize=FS_BLOCK_SIZE;
#endif
    setting_stat[i].st_uid=myuid;
    setting_stat[i].st_gid=mygid;
  }
  setting_names[SETTINGSCNT]=NULL;
  settings_stat.st_ctime=tm;
  settings_stat.st_mtime=tm;
  settings_stat.st_mode=S_IFDIR | 0755;
  settings_stat.st_nlink=2;
  settings_stat.st_size=SETTINGSCNT;
#if !defined(MINGW) && !defined(_WIN32)
  settings_stat.st_blocks=(settings_stat.st_size+511)/512;
  settings_stat.st_blksize=FS_BLOCK_SIZE;
#endif
  settings_stat.st_uid=myuid;
  settings_stat.st_gid=mygid;
  names_init=1;
}

const char **list_settings(){
  if (!names_init)
    init_settings();
  return setting_names;
}

static int get_setting_id(const char *name){
  int i;
  for (i=0; i<SETTINGSCNT; i++)
    if (!strcmp(name, settings[i].name))
      return i;
  return -1;
}

const struct stat *get_setting_stat(const char *name){
  if (!names_init)
    init_settings();
  if (name[0]){
    int id=get_setting_id(name+1);
    if (id!=-1)
      return &setting_stat[id];
    else
      return NULL;
  }
  else
    return &settings_stat;
}

int set_setting(const char *name, const char *val, size_t vallen){
  int id=get_setting_id(name);
  if (id!=-1){
    int ret=settings[id].set(val, vallen);
    if (!ret){
      char buff[4096];
      size_t sz;
      sz=sizeof(buff);
      settings[id].get(buff, &sz);
      setting_stat[id].st_size=sz;
#if !defined(MINGW) && !defined(_WIN32)
      setting_stat[id].st_blocks=(sz+511)/512;
#endif
    }
    return ret;
  }
  else
    return -ENOENT;
}

int get_setting(const char *name, char *val, size_t *vallen){
  int id=get_setting_id(name);
  if (id!=-1){
    settings[id].get(val, vallen);
    return 0;
  }
  else
    return -ENOENT;
}
