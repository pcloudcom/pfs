#define FUSE_USE_VERSION 26
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <fuse.h>
#include <ctype.h>
#include <errno.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include "common.h"
#include "settings.h"

#define SETTING_BUFF 4096
#define INITIAL_COND_TIMEOUT_SEC 3

#if !defined(MINGW) && !defined(_WIN32)
#  include <sys/mman.h>
#  include <signal.h>
#endif

#include "binapi.h"
#include "pfs.h"

#if defined(MINGW) || defined(_WIN32)

#define sleep(x) Sleep(1000*(x))
#define milisleep(x) Sleep((x))
#define index(str, c) strchr(str, c)
#define rindex(str, c) strrchr(str, c)

struct tm *gmtime_r(const time_t *timep, struct tm *result)
{
    struct tm * res = gmtime(timep);
    *result = *res;
    return result;
}

int dumb_socketpair(SOCKET socks[2], int make_overlapped);

#ifndef ENOTCONN
#   define ENOTCONN        107
#endif

#ifndef ST_NOSUID
#   define ST_NOSUID        2
#endif

char mount_point;
#else

#define milisleep(x) usleep((x)*1000)

#endif

pfs_settings fs_settings={
  .pagesize=64*1024,
  .cachesize=1024*1024*1024,
  .readaheadmin=64*1024,
  .readaheadmax=8*1024*1024,
  .readaheadmaxsec=12,
  .maxunackedbytes=256*1024,
  .usessl=0,
  .timeout=30,
  .retrycnt=5
};

static time_t cachesec=30;

static time_t laststatfs=0;

static int checkifhashmatch=0;

static const char *cachefile=NULL;

static uint64_t quota, usedquota;

static char *auth="";

uid_t myuid=0;
gid_t mygid=0;

static int fs_inited = 0;

#define PAGE_GC_PERCENT 10

#define FS_MAX_WRITE 256*1024

#define HASH_SIZE 4099

#define TASK_TYPE_WAIT 1
#define TASK_TYPE_CALL 2

#define MAX_FILE_STREAMS 16

#define NOT_CONNECTED_ERR -ENOTCONN

#define list_add(list, elem) do {(elem)->next=(list); (elem)->prev=&(list); (list)=(elem); if ((elem)->next) (elem)->next->prev=&(elem)->next;} while (0)
#define list_del(elem) do {*(elem)->prev=(elem)->next; if ((elem)->next) (elem)->next->prev=(elem)->prev;} while (0)
#define new(type) (type *)malloc(sizeof(type))

#define md5_debug(_str, _len) ({unsigned char __md5b[16]; char *__ret, *__ptr; int __i; MD5((unsigned char *)(_str), (_len), __md5b); __ret=malloc(34);\
                      __ptr=__ret; for (__i=0; __i<16; __i++){*__ptr++=hexdigits[__md5b[__i]/16];*__ptr++=hexdigits[__md5b[__i]%16];}\
                      *__ptr=0; __ret;})

#define dec_refcnt(_en) do {if (--(_en)->tfile.refcnt==0 && (_en)->isdeleted) {free_file_cache(_en); free(_en->name); free(_en);} } while (0)

#define fd_magick_start(__of) {\
  binparam fdparam;\
  char __buff[32];\
  int __useidx;\
  if ((__of)->fd){\
    fdparam.paramtype=PARAM_NUM;\
    fdparam.paramnamelen=2;\
    fdparam.paramname="fd";\
    fdparam.un.num=(__of)->fd;\
    __useidx=0;\
  }\
  else {\
    int __idx;\
    pthread_mutex_lock(&indexlock);\
    __idx=(int64_t)filesopened-(int64_t)(__of)->openidx;\
    fdparam.paramtype=PARAM_STR;\
    fdparam.paramnamelen=2;\
    fdparam.paramname="fd";\
    fdparam.opts=sprintf(__buff, "-%d", __idx);\
    fdparam.un.str=__buff;\
    __useidx=1;\
  }\

#define fd_magick_stop() \
    if (__useidx)\
      pthread_mutex_unlock(&indexlock);\
  }

typedef void (*task_callback)(void *, binresult *);

typedef struct _task {
  struct _task *next;
  uint64_t taskid;
  binresult *result;
  pthread_mutex_t *mutex;
  pthread_cond_t *cond;
  task_callback call;
  uint32_t type;
  char ready;
} task;

struct _node;
struct _openfile;
struct _cacheentry;

typedef struct _file {
  uint64_t fileid;
  uint64_t size;
  uint64_t hash;
  struct _cacheentry *cache;
  uint32_t refcnt;
} file;

typedef struct {
  uint64_t folderid;
  struct _node **nodes;
  uint32_t nodecnt;
  uint32_t nodealloc;
  uint32_t foldercnt;
} folder;

typedef struct _node {
  struct _node *next;
  struct _node **prev;
  struct _node *parent;
  char *name;
  time_t createtime;
  time_t modifytime;
  union {
    folder tfolder;
    file tfile;
  };
  char isfolder;
  char isdeleted;
  char hidefromlisting;
} node;

typedef struct {
  uint32_t frompage;
  uint32_t topage;
  size_t length;
  size_t id;
} offstream;

typedef struct _openfile{
  uint64_t fd;
  uint64_t unackdata;
  uint64_t openidx;
  node *file;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  offstream streams[MAX_FILE_STREAMS];
  size_t laststreamid;
  size_t bytesthissec;
  size_t currentspeed;
  time_t currentsec;
  uint32_t unackcomd;
  uint32_t refcnt;
  uint32_t connectionid;
  int error;
  int waitref;
  int waitcmd;
  char issetting;
} openfile;

#define ismodified bytesthissec
#define currentsize laststreamid

typedef struct {
  size_t pagesize;
  size_t cachesize;
  size_t numpages;
  size_t headersize;
} cacheheader;

typedef struct _cacheentry{
  struct _cacheentry *next;
  struct _cacheentry **prev;
  uint64_t fileid;
  uint64_t filehash;
  time_t lastuse;
  time_t fetchtime;
  pthread_cond_t cond;
  uint32_t realsize;
  uint32_t offset;
  uint32_t pageid;
  uint16_t sleeping;
  uint16_t locked;
  char free;
  char waiting;
} cacheentry;

typedef struct {
  cacheentry *page;
  openfile *of;
  uint32_t tries;
} pagefile;

typedef struct {
  openfile *of;
  off_t offset;
  size_t length;
  uint32_t tries;
  char buff[];
} writetask;

#define wait_for_allowed_calls() do {pthread_mutex_lock(&calllock); pthread_mutex_unlock(&calllock); } while (0)

static cacheheader *cachehead;
static cacheentry *cacheentries;
static void *cachepages;

static cacheentry *freecache=NULL;

static uint64_t taskid=1;
static uint64_t filesopened=0;
static task *tasks=NULL;
static pthread_mutex_t calllock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pageslock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t taskslock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t writelock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t indexlock=PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t datamutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t datacond=PTHREAD_COND_INITIALIZER;

static pthread_mutex_t treelock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t treecond=PTHREAD_COND_INITIALIZER;

static pthread_mutex_t wakelock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wakecond=PTHREAD_COND_INITIALIZER;

static pthread_mutex_t unacklock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t unackcond=PTHREAD_COND_INITIALIZER;


static size_t unackedbytes=0;
static size_t unackedsleepers=0;

static int unsigned treesleep=0;
static int processingtask=0;
static uint32_t connectionid=0;
static int need_reconnect=0;

static int diffwakefd, datawakefd;

static time_t timeoff;

static node *rootfolder;

static node *files[HASH_SIZE];
static node *folders[HASH_SIZE];

static apisock *sock, *diffsock;

static const char *hexdigits="0123456789abcdef";

#define cmd(_cmd, ...) \
  ({\
    binparam __params[]={__VA_ARGS__}; \
    do_cmd(_cmd, strlen(_cmd), NULL, 0, __params, sizeof(__params)/sizeof(binparam), NULL, NULL); \
  })

#define cmd_data(_cmd, _data, _datalen, ...) \
  ({\
    binparam __params[]={__VA_ARGS__}; \
    do_cmd(_cmd, strlen(_cmd), _data, _datalen, __params, sizeof(__params)/sizeof(binparam), NULL, NULL); \
  })

#define cmd_callback(_cmd, _callbackf, _callbackptr, ...) \
  ({\
    binparam __params[]={__VA_ARGS__}; \
    do_cmd(_cmd, strlen(_cmd), NULL, 0, __params, sizeof(__params)/sizeof(binparam), _callbackf, _callbackptr); \
  })

#define cmd_data_callback(_cmd, _data, _datalen, _callbackf, _callbackptr, ...) \
  ({\
    binparam __params[]={__VA_ARGS__}; \
    do_cmd(_cmd, strlen(_cmd), _data, _datalen, __params, sizeof(__params)/sizeof(binparam), _callbackf, _callbackptr); \
  })

static void time_format(time_t tm, char *result){
  static const char month_names[12][4]={"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  static const char day_names[7][4] ={"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  struct tm dt;
  int unsigned y;
  gmtime_r(&tm, &dt);
  memcpy(result, day_names[dt.tm_wday], 3);
  result+=3;
  *result++=',';
  *result++=' ';
  *result++=dt.tm_mday/10+'0';
  *result++=dt.tm_mday%10+'0';
  *result++=' ';
  memcpy(result, month_names[dt.tm_mon], 3);
  result+=3;
  *result++=' ';
  y=dt.tm_year+1900;
  *result++='0'+y/1000;
  y=y%1000;
  *result++='0'+y/100;
  y=y%100;
  *result++='0'+y/10;
  y=y%10;
  *result++='0'+y;
  *result++=' ';
  *result++=dt.tm_hour/10+'0';
  *result++=dt.tm_hour%10+'0';
  *result++=':';
  *result++=dt.tm_min/10+'0';
  *result++=dt.tm_min%10+'0';
  *result++=':';
  *result++=dt.tm_sec/10+'0';
  *result++=dt.tm_sec%10+'0';
  memcpy(result, " +0000", 7); // copies the null byte
}

void do_debug(const char *file, const char *function, int unsigned line, int unsigned level, const char *fmt, ...){
  static const struct {
    int unsigned level;
    const char *name;
  } debug_levels[]=DEBUG_LEVELS;
  static FILE *log=NULL;
  char dttime[32], format[512];
  va_list ap;
  const char *errname;
  int unsigned i;
  unsigned int pid;
  time_t currenttime;
  errname="BAD_ERROR_CODE";
  for (i=0; i<sizeof(debug_levels)/sizeof(debug_levels[0]); i++)
    if (debug_levels[i].level==level){
      errname=debug_levels[i].name;
      break;
    }
  if (!log){
    log=fopen(DEBUG_FILE, "a+");
    if (!log)
      return;
  }
  time(&currenttime);
  time_format(currenttime, dttime);
#if !defined(MINGW) && !defined(_WIN32)
  pid = (unsigned int)pthread_self();
#else
  pid = (unsigned int)pthread_self().p;
#endif
  snprintf(format, sizeof(format), "%s pid %u %s: %s:%u (function %s): %s\n", dttime, pid, errname, file, line, function, fmt);
  format[sizeof(format)-1]=0;
  va_start(ap, fmt);
  vfprintf(log, format, ap);
  va_end(ap);
  fflush(log);
}

static binresult *do_find_res(binresult *res, const char *key){
  int unsigned i;
  if (!res || res->type!=PARAM_HASH)
    return NULL;
  for (i=0; i<res->length; i++)
    if (!strcmp(res->hash[i].key, key))
      return res->hash[i].value;
  return NULL;
}

#define find_res_check do_find_res
#define find_res(res, key) ({\
    binresult *res__=res;\
    const char *key__=key;\
    if (!res__)\
      debug(D_WARNING, "find_res called with NULL result");\
    res__=do_find_res(res__, key__);\
    if (!res__)\
      debug(D_ERROR, "find_res could not find key %s", key__);\
    res__;\
  })

#if defined(MINGW) || defined(_WIN32)

#define DELTA_EPOCH_IN_MICROSECS  11644473600000000ULL

#ifndef _TIMEZONE_DEFINED /* also in sys/time.h */
#define _TIMEZONE_DEFINED
struct timezone {
  int tz_minuteswest;
  int tz_dsttime;
};
#endif /* _TIMEZONE_DEFINED */

int gettimeofday(struct timeval *tv, struct timezone *tz){
  FILETIME ft;
  uint64_t tmpres = 0;
  static int tzflag = 0;

  if (NULL != tv){
    GetSystemTimeAsFileTime(&ft);

    tmpres |= ft.dwHighDateTime;
    tmpres <<= 32;
    tmpres |= ft.dwLowDateTime;

    tmpres /= 10;  /*convert into microseconds*/
    /*converting file time to unix epoch*/
    tmpres -= DELTA_EPOCH_IN_MICROSECS;
    tv->tv_sec = (long)(tmpres / 1000000UL);
    tv->tv_usec = (long)(tmpres % 1000000UL);
  }

  if (NULL != tz){
    if (!tzflag){
      _tzset();
      tzflag++;
    }
    tz->tz_minuteswest = _timezone / 60;
    tz->tz_dsttime = _daylight;
  }

  return 0;
}

#endif

static int pthread_cond_wait_sec(pthread_cond_t *cond, pthread_mutex_t *mutex, time_t sec){
  struct timespec abstime;
  struct timeval now;
  // Sorry - this may not compile on win
  gettimeofday(&now, NULL);
  abstime.tv_sec=now.tv_sec+sec;
  abstime.tv_nsec=now.tv_usec*1000UL;
  return pthread_cond_timedwait(cond, mutex, &abstime);
}

static int pthread_cond_wait_timeout(pthread_cond_t *cond, pthread_mutex_t *mutex){
  if (fs_settings.timeout)
    return pthread_cond_wait_sec(cond, mutex, fs_settings.timeout);
  else
    return pthread_cond_wait(cond, mutex);
}

// needs to returns connected pipes (or sockets), generally pipefd[0] will be used for reading and pipefd[1] for writing
static int get_pipe(int pipefd[2]){
#ifdef WIN32
  return dumb_socketpair((SOCKET*)pipefd, 1);
#else
  return pipe(pipefd);
#endif
}

// should work on all platforms with select
static int ready_read(int cnt, int *socks, struct timeval *timeout){
  fd_set rfds;
  int max, i, ret;
  FD_ZERO(&rfds);
  max=0;
  for (i=0; i<cnt; i++){
    if (socks[i]>max)
      max=socks[i];
    FD_SET(socks[i], &rfds);
  }
  max++;
  do {
    ret=select(max, &rfds, NULL, NULL, timeout);
  } while (ret==-1 && errno==EINTR);
  if (ret<=0){
    return -1;
  }
  for (i=0; i<cnt; i++)
    if (FD_ISSET(socks[i], &rfds))
      return i;
  // should not happen
  return 0;
}


// read & write may need to be send/recv on some platforms, on ones that support pipe, send/recv are not appropriate
ssize_t pipe_read(int fd, void *buf, size_t count){
#ifdef WIN32
  return recv(fd, buf, count, 0);
#else
  return read(fd, buf, count);
#endif // WIN32
}

ssize_t pipe_write(int fd, const void *buf, size_t count){
#ifdef WIN32
  return send(fd, buf, count, 0);
#else
  return write(fd, buf, count);
#endif // WIN32
}

static int try_to_wake_diff(){
  pipe_write(diffwakefd, "W", 1);
  return 0;
}

static int try_to_wake_data(){
  int res;
  debug(D_NOTICE, "try_to_wake_data - in");
  pthread_mutex_lock(&wakelock);
  pipe_write(datawakefd, "w", 1);
  res=pthread_cond_wait_sec(&wakecond, &wakelock, 15);
  pthread_mutex_unlock(&wakelock);
  debug(D_NOTICE, "try_to_wake_data - out %d", res);
  return res;
}

static int remove_task(task *ptask, uint64_t id){
  task *t, **pt;
  pthread_mutex_lock(&taskslock);
  t=tasks;
  pt=&tasks;
  while (t){
    if (t==ptask && t->taskid==id){
      *pt=t->next;
      break;
    }
    pt=&t->next;
    t=t->next;
  }
  pthread_mutex_unlock(&taskslock);
  return t!=NULL;
}

static binresult *do_cmd(const char *command, size_t cmdlen, const void *data, size_t datalen, binparam *params, size_t paramcnt,
                         task_callback callback, void *callbackptr){
  uint64_t myid;
  pthread_mutex_t mymutex;
  pthread_cond_t mycond;
  binparam nparams[paramcnt+1];
  task mytask, *ptask;
  binresult *res;
  int cnt;
  debug(D_NOTICE, "Do-cmd enter %s, %c", command, callback?'C':'D');

  pthread_mutex_init(&mymutex, NULL);
  pthread_cond_init(&mycond, NULL);

  if (callback){
    ptask=new(task);
    ptask->type=TASK_TYPE_CALL;
    ptask->call=callback;
    ptask->result=(binresult *)callbackptr;
  }
  else{
    ptask=&mytask;
    ptask->mutex=&mymutex;
    ptask->cond=&mycond;
    ptask->type=TASK_TYPE_WAIT;
    ptask->ready=0;
    pthread_mutex_lock(&mymutex);
  }
  pthread_mutex_lock(&taskslock);
  myid=ptask->taskid=taskid++;
  ptask->next=tasks;
  tasks=ptask;
  pthread_mutex_unlock(&taskslock);
  debug(D_NOTICE, "Do-cmd send %lu", (long unsigned int)myid);
  memcpy(nparams+1, params, paramcnt*sizeof(binparam));
  nparams[0].paramname="id";
  nparams[0].paramnamelen=2;
  nparams[0].paramtype=PARAM_NUM;
  nparams[0].un.num=ptask->taskid;
  debug(D_NOTICE, "Do-cmd - pre-writelock");
  pthread_mutex_lock(&writelock);
  debug(D_NOTICE, "Do-cmd - writelocked");
  res=NULL;
  cnt=0;
  while (!res && cnt++<=3){
    debug(D_NOTICE, "Do-cmd - sending data...");
    if (datalen)
      res=do_send_command(sock, command, cmdlen, nparams, paramcnt+1, datalen, 0);
    else
      res=do_send_command(sock, command, cmdlen, nparams, paramcnt+1, -1, 0);
    if (!res && try_to_wake_data()){
      debug(D_WARNING, "Do-cmd - failed to send data for command %s - reconnecting %d", command, cnt);
/*
      reconnect_if_needed is designed to be called only from the receive_thread, all others can just call try_to_wake_data(),
      doing both actually runs reconnect_if_needed() in both threads which is generally not a good idea.
      reconnect_if_needed();
*/
      break;
    }
  }
  if (res && datalen){
    if (writeall(sock, data, datalen)){
      debug(D_WARNING, "Do-cmd - writeall failed for command %s", command);
      res=NULL;
    }
  }
  debug(D_NOTICE, "Do-cmd - pre writeUNlock");
  pthread_mutex_unlock(&writelock);
  debug(D_NOTICE, "Do-cmd - writeUNlocked");

  if (!callback){
    if (res){
      debug(D_NOTICE, "##### Do-cmd wait %s, %" PRIu64, command, myid);
      if (pthread_cond_wait_sec(&mycond, &mymutex, INITIAL_COND_TIMEOUT_SEC)){
        debug(D_WARNING, "##### Do-cmd wait %s, %" PRIu64 " first timeout, try to wake", command, myid);
        if (try_to_wake_data() || pthread_cond_wait_timeout(&mycond, &mymutex)){
          if (remove_task(ptask, myid))
            debug(D_WARNING, "##### Do-cmd %s, %" PRIu64 " second timeout, task removed", command, myid);
          else
            debug(D_WARNING, "##### Do-cmd %s, %" PRIu64 " second timeout, task not", command, myid);
          pthread_mutex_unlock(&mymutex);
          pthread_cond_destroy(&mycond);
          pthread_mutex_destroy(&mymutex);
          return NULL;
        }
      }
      debug(D_NOTICE, "##### Do-cmd got %s", command);
      res=ptask->result;
    }
    else
      res=NULL;
    pthread_mutex_unlock(&mymutex);
  }
  else if (!res){
    if (remove_task(ptask, myid)){
      free(ptask);
      callback(callbackptr, NULL);
    }
  }

  pthread_cond_destroy(&mycond);
  pthread_mutex_destroy(&mymutex);
  debug(D_NOTICE, "Do-cmd exit %s, %p", command, res);
  return res;
}

static void cancel_tasks(task *t){
  task *t2, *tn;
  debug(D_WARNING, "called");
  t2=NULL;
  // reverse list so oldest tasks get cancelled/rescheduled first
  while (t){
    tn=t->next;
    t->next=t2;
    t2=t;
    t=tn;
  }
  while (t2){
    t=t2;
    t2=t->next;
    debug(D_WARNING, "cancelling task %lu.", (unsigned long)t->taskid);
    if (t->type==TASK_TYPE_WAIT){
      t->result=NULL;
      debug(D_NOTICE, "task TASK_TYPE_WAIT");
      pthread_mutex_lock(t->mutex);
      pthread_cond_signal(t->cond);
      pthread_mutex_unlock(t->mutex);
      debug(D_NOTICE, "task TASK_TYPE_WAIT signalled");
    }
    else if (t->type==TASK_TYPE_CALL){
      debug(D_NOTICE, "task TASK_TYPE_CALL - %p", t);
      t->call((void *)t->result, NULL);
      free(t);
      debug(D_NOTICE, "task TASK_TYPE_CALL called");
    }
  }
  debug(D_NOTICE, "leave");
}

static void cancel_all_and_reconnect(){
  binresult *res;
  task *t;
  apisock null;
  debug(D_WARNING, "cancel_all_and_reconnect");
//  cancel_all();
  null.ssl=NULL;
  null.sock=-1;
  pthread_mutex_lock(&taskslock);
  pthread_mutex_lock(&writelock);
  debug(D_NOTICE, "cancel_all_and_reconnect - after write lock");
  api_close(sock);
  do{
    if (fs_settings.usessl)
      sock=api_connect_ssl();
    else
      sock=api_connect();
    if (!sock){
      debug(D_WARNING, "cancel_all_and_reconnect - failed to connect");
      sock=&null;
      pthread_mutex_unlock(&writelock);
      pthread_mutex_unlock(&taskslock);
      sleep(1);
//      cancel_all();
      pthread_mutex_lock(&taskslock);
      pthread_mutex_lock(&writelock);
      sock=NULL;
    }
    else {
      res=send_command(sock, "userinfo", P_STR("auth", auth));
      if (!res){
        debug(D_WARNING, "cancel_all_and_reconnect - failed to login");
        api_close(sock);
        sock=NULL;
      }
      else {
        if (find_res(res, "result")->num!=0){
          debug(D_ERROR, "cancel_all_and_reconnect - problem on login, exiting");
          pthread_mutex_unlock(&writelock);
          pthread_mutex_unlock(&taskslock);
          exit(1);
        }
        free(res);
      }
    }
  } while (!sock);
  // sleep(1); - why do we need that sleep?
  t=tasks;
  tasks=NULL;
  pthread_mutex_unlock(&writelock);
  pthread_mutex_unlock(&taskslock);
  pthread_mutex_lock(&indexlock);
  connectionid++;
  pthread_mutex_unlock(&indexlock);
  cancel_tasks(t);
  debug(D_NOTICE, "cancel_all_and_reconnect leave");
}

static void stop_and_wait_pending(){
  int c=2;
  pthread_mutex_lock(&calllock);
  pthread_mutex_lock(&taskslock);
  while (tasks || processingtask || c--){
    pthread_mutex_unlock(&taskslock);
    milisleep(10);
    pthread_mutex_lock(&taskslock);
  }
  pthread_mutex_unlock(&taskslock);
}

static void resume_tasks(){
  pthread_mutex_unlock(&calllock);
}

static void reconnect_if_needed(){
  binresult *res;
  struct timeval timeout;
  debug(D_NOTICE, "data thread awake");
  pthread_mutex_lock(&writelock);
  res=send_command_nb(sock, "nop");
  pthread_mutex_unlock(&writelock);
  if (!res){
    debug(D_WARNING, "reconnecting data because write failed");
    return cancel_all_and_reconnect();
  }
  timeout.tv_sec=RECONNECT_TIMEOUT;
  timeout.tv_usec=0;
  if (ready_read(1, &sock->sock, &timeout)!=0){
    debug(D_WARNING, "reconnecting data because socket timeouted");
    return cancel_all_and_reconnect();
  }
  debug(D_NOTICE, "no reconnection needed, socket alive");
}

static void *receive_thread(void *ptr){
  binresult *res, *id, *sub;
  task *t, **pt;
  struct timeval timeout;
  int wakefds[2], monitorfds[2], r, rhasdata;
  char b;
  if (get_pipe(wakefds))
    return NULL;
  datawakefd=wakefds[1];
  monitorfds[0]=wakefds[0];
  while (1){
    monitorfds[1]=sock->sock;
    if (hasdata(sock))
      r=1;
    else{
      if (tasks){
        timeout.tv_sec=RECONNECT_TIMEOUT;
        timeout.tv_usec=0;
        r=ready_read(2, monitorfds, &timeout);
        if (r==-1){
          debug(D_WARNING, "read socket timeouted, reconnecting");
          cancel_all_and_reconnect();
          continue;
        }
      }
      else{
        timeout.tv_sec=1;
        timeout.tv_usec=0;
        r=ready_read(2, monitorfds, &timeout);
        if (r==-1)
          continue;
      }
    }
    if (r==1)
      res=get_result(sock);
    else if (r==0){
      pipe_read(wakefds[0], &b, 1);
      debug(D_WARNING, "wake message received, calling reconnect_if_needed");
      reconnect_if_needed();
      pthread_mutex_lock(&wakelock);
      pthread_cond_broadcast(&wakecond);
      pthread_mutex_unlock(&wakelock);
      continue;
    }
    else{
      debug(D_BUG, "ready_read returned %d, should not happen", r);
      break; // should not happen
    }
    if (!res){
      debug(D_WARNING, "get_result returned NULL, reconnecting");
      cancel_all_and_reconnect();
      continue;
    }
    id=find_res_check(res, "id");
    if (!id || id->type!=PARAM_NUM){
      free(res);
      debug(D_WARNING, "receive_thread - no ID, could be a nop or truncate");
      continue;
    }
    debug(D_NOTICE, "receive_thread received %lu", (unsigned long)id->num);
    pthread_mutex_lock(&taskslock);
    pt=&tasks;
    t=tasks;
    while (t){
      if (t->taskid==id->num){
        *pt=t->next;
        processingtask++;
        break;
      }
      pt=&t->next;
      t=t->next;
    }
    pthread_mutex_unlock(&taskslock);
    if (!t){
      free(res);
      debug(D_BUG, "could not find task %lu", (long unsigned)id->num);
      continue;
    }
    sub=find_res_check(res, "data");
    rhasdata=sub && sub->type==PARAM_DATA;
    /* !!! IMPORTANT !!!
     * if we have TASK_TYPE_WAIT, t is on the stack of the thread waiting on t->cond, therefore no free
     * if we have TASK_TYPE_CALL, t is allcated and we need to free. callback does not have to free the result
     */
    if (t->type==TASK_TYPE_WAIT){
      t->result=res;
      if (rhasdata){
        pthread_mutex_lock(&datamutex);
      }
      pthread_mutex_lock(t->mutex);
      pthread_cond_signal(t->cond);
      pthread_mutex_unlock(t->mutex);
      if (rhasdata){
        pthread_cond_wait(&datacond, &datamutex);
        pthread_mutex_unlock(&datamutex);
      }
    }
    else if (t->type==TASK_TYPE_CALL){
//      debug(D_NOTICE, "receive thread calling task - %p\n", t);
      t->call((void *)t->result, res);
//      debug(D_NOTICE, "receive thread task called - %p\n", t);
      free(res);
      free(t);
    }
    else
      free(res);
    pthread_mutex_lock(&taskslock);
    processingtask--;
    pthread_mutex_unlock(&taskslock);
    if (need_reconnect){
      need_reconnect=0;
      cancel_all_and_reconnect();
    }
//    debug(D_NOTICE, "receive_thread - end loop\n");
  }
  return NULL;
}

static void send_event_message(uint64_t diffid, uint32_t utype, ...){
  struct iovec iovs[64];
  va_list ap;
  const char *str;
  size_t len, msglen;
  uint32_t ulen;
  int o;
  va_start(ap, utype);
  msglen=0;
  o=3;
  while ((str=va_arg(ap, const char *))) {
    len=strlen(str);
    msglen+=len;
    iovs[o].iov_base=(char *)str;
    iovs[o].iov_len=len;
    o++;
  }
  va_end(ap);
  ulen=msglen;
  iovs[0].iov_base=&diffid;
  iovs[0].iov_len=sizeof(diffid);
  iovs[1].iov_base=&utype;
  iovs[1].iov_len=sizeof(utype);
  iovs[2].iov_base=&ulen;
  iovs[2].iov_len=sizeof(ulen);
  event_writev(iovs, o);
}

static node *get_file_by_id(uint64_t fileid){
  node *f;
  f=files[fileid%HASH_SIZE];
  while (f){
    if (f->tfile.fileid==fileid)
      return f;
    f=f->next;
  }
  return NULL;
}

static node *get_folder_by_id(uint64_t folderid){
  node *f;
  f=folders[folderid%HASH_SIZE];
  while (f){
    if (f->tfolder.folderid==folderid)
      return f;
    f=f->next;
  }
  return NULL;
}

#if defined(MINGW) || defined(_WIN32)

static void build_full_path(char** full_path, node *f){
  if (f->parent){
    build_full_path(full_path, f->parent);
    (*full_path)[0] = '/';
    strcpy(*full_path+1, f->name);
    (*full_path) += strlen(f->name)+1;
  }
}

#define  PIPE_NAME L"\\\\.\\pipe\\pfsnotifypipe"

#define SHCNE_CREATE (0x00000002)
#define SHCNE_DELETE (0x00000004)
#define SHCNE_MKDIR (0x00000008)
#define SHCNE_RMDIR (0x00000010)
#define SHCNE_ATTRIBUTES (0x00000800)
#define SHCNE_UPDATEDIR (0x00001000)
#define SHCNE_UPDATEITEM (0x00002000)


typedef struct
{
    uint32_t size;
    uint32_t event1;
    uint32_t event2;
    char message[4*MAX_PATH];
} notification;

static void send_notification_pipe(const notification* notify){
  DWORD  mode, written, total;
  const wchar_t* pipename = PIPE_NAME;
  static HANDLE hPipe = INVALID_HANDLE_VALUE;

  if (hPipe == INVALID_HANDLE_VALUE)
    hPipe = CreateFile(pipename, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

  while (1){
    if (hPipe != INVALID_HANDLE_VALUE) break;
    if (GetLastError() != ERROR_PIPE_BUSY){
      debug(D_WARNING, "failed to open notification pipe %lu", GetLastError());
      return;
    }
    if (!WaitNamedPipe(PIPE_NAME, 500)){
      debug(D_WARNING, "Failed to open pipe - timed out.");
      return;
    }
  }
  mode = PIPE_READMODE_BYTE;
  if (SetNamedPipeHandleState(hPipe, &mode,  NULL, NULL)){
    total = 0;
    while (total < notify->size){
      if (!WriteFile(hPipe, (void*)((char*)notify+total), notify->size-total, &written, NULL)){
        debug(D_WARNING, "Failed to write data to the pipe. %lu", GetLastError());
        CloseHandle(hPipe);
        hPipe = INVALID_HANDLE_VALUE;
        break;
      }
      total += written;
    }
    FlushFileBuffers(hPipe);
  }
}

static void send_notification(node * item, LONG event1, LONG event2){
  notification n;
  char * path = n.message;
  n.event1 = event1;
  n.event2 = event2;
  n.message[0] = 0;

  build_full_path(&path, item);
  debug(D_NOTICE, n.message);
  n.size = strlen(n.message)+1+3*sizeof(uint32_t);

  send_notification_pipe(&n);
}

static void notify_create_item(node * item){
  send_notification(item, SHCNE_CREATE, 0);
}

static void notify_modify_item(node * item){
  send_notification(item, item->isfolder?SHCNE_UPDATEDIR:SHCNE_UPDATEITEM, SHCNE_ATTRIBUTES);
}

static void notify_delete_item(node * item){
  send_notification(item, item->isfolder?SHCNE_RMDIR:SHCNE_DELETE, 0);
}
#endif

static void diff_create_folder(binresult *meta, time_t mtime){
  binresult *name;
  node *folder, *f;
  uint64_t parentid;
  name=find_res(meta, "name");
  folder=new(node);
  folder->name=malloc(name->length+1);
  memcpy(folder->name, name->str, name->length+1);
  folder->createtime=find_res(meta, "created")->num+timeoff;
  folder->modifytime=find_res(meta, "modified")->num+timeoff;
  folder->tfolder.folderid=find_res(meta, "folderid")->num;
  folder->tfolder.nodes=NULL;
  folder->tfolder.nodecnt=0;
  folder->tfolder.nodealloc=0;
  folder->tfolder.foldercnt=0;
  folder->isfolder=1;
  folder->isdeleted=0;
  folder->hidefromlisting=0;
  parentid=find_res(meta, "parentfolderid")->num;
  pthread_mutex_lock(&treelock);
  f=get_folder_by_id(parentid);
  if (!f){
    pthread_mutex_unlock(&treelock);
    free(folder->name);
    free(folder);
    return;
  }
  if (f->tfolder.nodecnt>=f->tfolder.nodealloc){
    f->tfolder.nodealloc+=64;
    f->tfolder.nodes=realloc(f->tfolder.nodes, sizeof(node *)*f->tfolder.nodealloc);
  }
  f->tfolder.nodes[f->tfolder.nodecnt++]=folder;
  f->tfolder.foldercnt++;
  f->modifytime=mtime;
  folder->parent=f;
  list_add(folders[folder->tfolder.folderid%HASH_SIZE], folder);

#if defined(MINGW) || defined(_WIN32)
  notify_create_item(folder);
#endif

  if (treesleep)
    pthread_cond_broadcast(&treecond);
  pthread_mutex_unlock(&treelock);
}

static void remove_from_parent(node *nd){
  node *parent;
  uint32_t i;
  parent=nd->parent;
  if (nd->isfolder)
    parent->tfolder.foldercnt--;
  for (i=0; i<parent->tfolder.nodecnt; i++)
    if (parent->tfolder.nodes[i]==nd){
      parent->tfolder.nodes[i]=parent->tfolder.nodes[parent->tfolder.nodecnt-1];
      parent->tfolder.nodecnt--;
      return;
    }
}

static void free_file_cache(node *file){
  cacheentry *ce, *cn;
  pthread_mutex_lock(&pageslock);
  ce=file->tfile.cache;
  while (ce){
    cn=ce->next;
    ce->free=1;
    list_add(freecache, ce);
    ce=cn;
  }
  file->tfile.cache=NULL;
  pthread_mutex_unlock(&pageslock);
}

static void delete_file(node *file, int removefromparent){
  if (removefromparent)
    remove_from_parent(file);
  list_del(file);
  if (file->tfile.refcnt){
    file->isdeleted=1;
    file->parent=NULL;
  }
  else {
#if defined(MINGW) || defined(_WIN32)
  notify_delete_item(file);
#endif
    free_file_cache(file);
    free(file->name);
    free(file);
  }
}

static void diff_create_file(binresult *meta, time_t mtime){
  binresult *name;
  node *file, *f;
  uint64_t parentid;

  name=find_res(meta, "name");
  if (!name)
    return;
  file=new(node);
  file->name=malloc(name->length+1);
  memcpy(file->name, name->str, name->length+1);
  file->createtime=find_res(meta, "created")->num+timeoff;
  file->modifytime=find_res(meta, "modified")->num+timeoff;
  file->tfile.fileid=find_res(meta, "fileid")->num;
  file->tfile.size=find_res(meta, "size")->num;
  file->tfile.hash=find_res(meta, "hash")->num;
  file->tfile.cache=NULL;
  file->tfile.refcnt=0;
  file->isfolder=0;
  file->isdeleted=0;
  file->hidefromlisting=0;
  parentid=find_res(meta, "parentfolderid")->num;
  pthread_mutex_lock(&treelock);

  name=find_res_check(meta, "deletedfileid");
  if (name){
    uint64_t old_id=name->num;
    debug(D_NOTICE, "create-> deleted old file");
    f=get_file_by_id(old_id);
    if (f){
      f->parent->modifytime=mtime;
      debug(D_NOTICE, "deleted old file %s", f->name);
      delete_file(f, 1);
    }
  }

  f=get_folder_by_id(parentid);
  if (!f){
    pthread_mutex_unlock(&treelock);
    free(file->name);
    free(file);
    return;
  }
  if (f->tfolder.nodecnt>=f->tfolder.nodealloc){
    f->tfolder.nodealloc+=64;
    f->tfolder.nodes=realloc(f->tfolder.nodes, sizeof(node *)*f->tfolder.nodealloc);
  }
  f->tfolder.nodes[f->tfolder.nodecnt++]=file;
  f->modifytime=mtime;
  file->parent=f;
  list_add(files[file->tfile.fileid%HASH_SIZE], file);

#if defined(MINGW) || defined(_WIN32)
  notify_create_item(file);
#endif

  if (treesleep)
    pthread_cond_broadcast(&treecond);
  pthread_mutex_unlock(&treelock);
}

static void diff_modifyfile_file(binresult *meta, time_t mtime){
  uint64_t fileid;
  binresult *res;
  node *f;
//  debug(D_NOTICE, "modify file in \n");
  fileid=find_res(meta, "fileid")->num;
  pthread_mutex_lock(&treelock);


  res = find_res_check(meta, "deletedfileid");
  if (res){
    uint64_t old_id = res->num;
    debug(D_NOTICE, "deleted old file");
    f=get_file_by_id(old_id);
    if (f){
      f->parent->modifytime=mtime;
      debug(D_NOTICE, "deleted old file %s", f->name);
      delete_file(f, 1);
    }
  }

  f=get_file_by_id(fileid);
  if (f){
    f->createtime=find_res(meta, "created")->num+timeoff;
    f->modifytime=find_res(meta, "modified")->num+timeoff;
    f->tfile.size=find_res(meta, "size")->num;
    f->tfile.hash=find_res(meta, "hash")->num;
    res=find_res(meta, "name");
    if (res){
//      debug(D_NOTICE, "name -> %s\n", res->str);
      f->name=realloc(f->name, res->length+1);
      memcpy((void*)f->name, res->str, res->length+1);
    }
    res = find_res(meta, "parentfolderid");
    if (res){
      uint64_t parent = res->num;
      if (parent != f->parent->tfolder.folderid){
        node* par;
        debug(D_NOTICE, "file change parent");
        par=get_folder_by_id(parent);
        if (!par){
          pthread_mutex_unlock(&treelock);
          debug(D_WARNING, "modify file out - no parent?!?");
          return;
        }
        if (par->tfolder.nodecnt>=par->tfolder.nodealloc){
          par->tfolder.nodealloc+=64;
          par->tfolder.nodes=realloc(par->tfolder.nodes, sizeof(node *)*par->tfolder.nodealloc);
        }
        par->tfolder.nodes[par->tfolder.nodecnt++]=f;
        par->modifytime=mtime;

#if defined(MINGW) || defined(_WIN32)
        notify_modify_item(f->parent);
#endif
        remove_from_parent(f);
        f->parent=par;
     }
    }

#if defined(MINGW) || defined(_WIN32)
    notify_modify_item(f->parent);
#endif
  }

  if (treesleep)
    pthread_cond_broadcast(&treecond);
  pthread_mutex_unlock(&treelock);
//  debug(D_NOTICE, "modify file out OK\n");
}

static void diff_modifyfile_folder(binresult* meta, time_t mtime){
  uint64_t folderid;
  binresult *res;
  node *f;
//  debug(D_NOTICE, "modify folder in\n");
  folderid=find_res(meta, "folderid")->num;

  pthread_mutex_lock(&treelock);
  f=get_folder_by_id(folderid);
  if (f){
    res = find_res(meta, "created");
    if (res) f->createtime=res->num+timeoff;
    res = find_res(meta, "modified");
    if (res) f->modifytime=res->num+timeoff;
    res=find_res(meta, "name");
    if (res){
//      debug(D_NOTICE, "folder name -> %s\n", res->str);
      f->name=realloc(f->name, res->length+1);
      memcpy(f->name, res->str, res->length+1);
    }
    res = find_res(meta, "parentfolderid");
    if (res){
      uint64_t parent = res->num;
      if (parent != f->parent->tfolder.folderid){
        node* par;
        debug(D_NOTICE, "folder - change parent %u -> %u", (uint32_t)f->parent->tfolder.folderid, (uint32_t)parent);
        par=get_folder_by_id(parent);
        if (!par){
          pthread_mutex_unlock(&treelock);
          debug(D_WARNING, "modify folder out - no parent");
          return;
        }
        if (par->tfolder.nodecnt>=par->tfolder.nodealloc){
          par->tfolder.nodealloc+=64;
          par->tfolder.nodes=realloc(par->tfolder.nodes, sizeof(node *)*par->tfolder.nodealloc);
        }
        par->tfolder.nodes[par->tfolder.nodecnt++]=f;
        par->tfolder.foldercnt++;
        par->modifytime=mtime;

        remove_from_parent(f);
#if defined(MINGW) || defined(_WIN32)
        notify_modify_item(f->parent);
#endif
        f->parent=par;
      }
    }
#if defined(MINGW) || defined(_WIN32)
    notify_modify_item(f->parent);
#endif
  }

  if (treesleep)
    pthread_cond_broadcast(&treecond);
  pthread_mutex_unlock(&treelock);
//  debug(D_NOTICE, "modify folder out - OK\n");
}

static void delete_folder(node *folder, int removefromparent){
  uint32_t i;
  for (i=0; i<folder->tfolder.nodecnt; i++)
    if (folder->tfolder.nodes[i]->isfolder)
      delete_folder(folder->tfolder.nodes[i], 0);
    else
      delete_file(folder->tfolder.nodes[i], 0);
  free(folder->tfolder.nodes);
  if (removefromparent)
    remove_from_parent(folder);

#if defined(MINGW) || defined(_WIN32)
  notify_delete_item(folder);
#endif

  list_del(folder);
  free(folder->name);
  free(folder);
}

static void diff_delete_folder(binresult *meta, time_t mtime){
  uint64_t folderid;
  node *f;
  folderid=find_res(meta, "folderid")->num;
  pthread_mutex_lock(&treelock);
  f=get_folder_by_id(folderid);
  if (f){
    f->parent->modifytime=mtime;
    delete_folder(f, 1);
  }
  pthread_mutex_unlock(&treelock);
}

static void diff_delete_file(binresult *meta, time_t mtime){
  uint64_t fileid;
  node *f;
  fileid=find_res(meta, "fileid")->num;
  pthread_mutex_lock(&treelock);
  f=get_file_by_id(fileid);
  if (f){
    f->parent->modifytime=mtime;
    delete_file(f, 1);
  }
  pthread_mutex_unlock(&treelock);
}

static void process_diff(binresult *diff){
  binresult *event, *meta, *sn;
  const char *estr;
  time_t tm;
  event=find_res(diff, "event");
  meta=find_res_check(diff, "metadata");
  tm=find_res(diff, "time")->num+timeoff;
  debug(D_NOTICE, "diff -> %s", event->str);
  if (!event || event->type!=PARAM_STR)
    return;
  estr=event->str;
  if (meta && meta->type==PARAM_HASH){
    if (!strcmp(estr, "createfolder"))
      diff_create_folder(meta, tm);
    else if (!strcmp(estr, "createfile"))
      diff_create_file(meta, tm);
    else if (!strcmp(estr, "modifyfile"))
      diff_modifyfile_file(meta, tm);
    else if (!strcmp(estr, "modifyfolder"))
      diff_modifyfile_folder(meta, tm);
    else if (!strcmp(estr, "deletefolder"))
      diff_delete_folder(meta, tm);
    else if (!strcmp(estr, "deletefile"))
      diff_delete_file(meta, tm);
    return;
  }
  meta=find_res_check(diff, "share");
  if (meta && meta->type==PARAM_HASH){
    uint64_t diffid=find_res(diff, "diffid")->num;
    char *snstr;
    int fr=0;
    if ((sn=find_res_check(meta, "sharename")))
      snstr=(char *)sn->str;
    else if ((sn=find_res_check(meta, "folderid"))){
      pthread_mutex_lock(&treelock);
      node *f=get_folder_by_id(sn->num);
      if (!f){
        pthread_mutex_unlock(&treelock);
        return;
      }
      snstr=strdup(f->name);
      pthread_mutex_unlock(&treelock);
      fr=1;
    }
    else
      return;

    if (!strcmp(estr, "requestsharein"))
      send_event_message(diffid, 1, "User ", find_res(meta, "frommail")->str, " shared folder \"", snstr, "\" with you.", NULL);
    else if (!strcmp(estr, "acceptedshareout"))
      send_event_message(diffid, 0, "User ", find_res(meta, "tomail")->str, " accepted your share of folder \"", snstr, "\".", NULL);
    else if (!strcmp(estr, "declinedshareout"))
      send_event_message(diffid, 0, "User ", find_res(meta, "tomail")->str, " rejected your share of folder \"", snstr, "\".", NULL);
    else if (!strcmp(estr, "cancelledsharein"))
      send_event_message(diffid, 1, "User ", find_res(meta, "frommail")->str, " canceled share of folder \"", snstr, "\" with you.", NULL);
    else if (!strcmp(estr, "removedsharein"))
      send_event_message(diffid, 1, "User ", find_res(meta, "frommail")->str, " stopped share of folder \"", snstr, "\" with you.", NULL);
    else if (!strcmp(estr, "modifiedsharein"))
      send_event_message(diffid, 1, "User ", find_res(meta, "frommail")->str, " changed your access permissions to folder \"", snstr, "\".", NULL);
    if (fr)
      free(snstr);
    return;
  }
//  if (!strcmp(estr, "modifyuserinfo"))
//    send_event_message(16, NULL);
}

static void *diff_thread(void *ptr){
  uint64_t diffid;
  binresult *res, *sub, *entries, *entry;
  int unsigned i;
  int wakefds[2], monitorfds[2], r;
  char b;
  if (get_pipe(wakefds))
    return NULL;
  diffid=0;
  diffwakefd=wakefds[1];
  monitorfds[0]=diffsock->sock;
  monitorfds[1]=wakefds[0];
  while (1){
    debug(D_NOTICE, "sending diff");
    res=send_command_nb(diffsock, "diff", P_NUM("diffid", diffid), P_BOOL("block", 1), P_STR("timeformat", "timestamp"));
    if (res){
      if (hasdata(diffsock))
        r=0;
      else{
        r=ready_read(2, monitorfds, NULL);
      }
      if (r==0){
        res=get_result(diffsock);
      }
      else if (r==1){
        debug(D_WARNING, "diff got wake - reconnecting");
        pipe_read(wakefds[0], &b, 1);
        res=NULL;
      }
    }
    if (!res){
      debug(D_WARNING, "diff thread - reconnecting");
      send_event_message(0, 2, NULL);
      api_close(diffsock);
      do {
        if (fs_settings.usessl)
          diffsock=api_connect_ssl();
        else
          diffsock=api_connect();
        if (!diffsock)
          sleep(1);
      } while (!diffsock);
      res=send_command(diffsock, "userinfo", P_STR("auth", auth));
      if (!res)
        continue;
      sub=find_res(res, "result");
      if (sub && sub->type==PARAM_NUM && sub->num==0){
        free(res);
        monitorfds[0]=diffsock->sock;
        send_event_message(0, 3, NULL);
        debug(D_NOTICE, "diff thread - reconnected!");
        continue;
      }
      free(res);
      debug(D_ERROR, "diff thread - failed to reconnect, quitting!");
      exit(1);
    }
    sub=find_res(res, "result");
    if (!sub || sub->type!=PARAM_NUM || sub->num!=0){
      free(res);
      continue;
    }
    entries=find_res(res, "entries");
    debug(D_NOTICE, "diff thread - %u entries received", entries->length);
    pthread_mutex_lock(&treelock);
    for (i=0; i<entries->length; i++){
      entry=entries->array[i];
      process_diff(entry);
      diffid=find_res(entry, "diffid")->num;
    }
    pthread_mutex_unlock(&treelock);
    diffid=find_res(res, "diffid")->num;
    free(res);
  }
  return NULL;
}

int convert_error(uint64_t fserr){
  if (fserr==2001)
    return -ENAMETOOLONG;
  else if (fserr==2002)
    return -ENOENT;
  else if (fserr==2003)
    return -EACCES;
  else if (fserr==2004)
    return -EEXIST;
  else if (fserr==2005)
    return -ENOENT;
  else if (fserr==2006)
    return -ENOTEMPTY;
  else if (fserr==2007)
    return -EBUSY;
  else if (fserr==2008)
    return -ENOSPC;
  else if (fserr==2009)
    return -ENOENT;
  else if (fserr==2010)
    return -ENOENT;

  else
    return -EACCES;
}

static node *get_node_by_path(const char *path){
  const char *slash;
  node *cdir;
  size_t len;
  int last, i;
  if (*path!='/')
    return NULL;
  path++;
  cdir=rootfolder;
  last=0;
  if (!*path)
    return cdir;
  while (1){
again:
    slash=index(path, '/');
    if (slash){
      len=slash-path;
    }
    else{
      len=strlen(path);
      last=1;
    }
    for (i=0; i<cdir->tfolder.nodecnt; i++)
      if (!memcmp(path, cdir->tfolder.nodes[i]->name, len) && cdir->tfolder.nodes[i]->name[len]==0){
        if (last)
          return cdir->tfolder.nodes[i];
        else if (cdir->tfolder.nodes[i]->isfolder){
          cdir=cdir->tfolder.nodes[i];
          path=slash+1;
          goto again;
        }
      }
    return NULL;
  }
}

static node *get_parent_folder(const char *path, const char **name){
  const char *lslash;
  char *cpath;
  node *ret;
  size_t len;
  lslash=rindex(path, '/');
  *name=lslash+1;
  if (lslash==path)
    return get_node_by_path("/");
  len=lslash-path;
  cpath=(char *)malloc(len+1);
  memcpy(cpath, path, len);
  cpath[len]=0;
  ret=get_node_by_path(cpath);
  free(cpath);
  return ret;
}

static int fs_getattr(const char *path, struct stat *stbuf){
  node *entry;
  debug(D_NOTICE, "fs_getattr, %s\n", path);
  if (!strncmp(path, SETTINGS_PATH, strlen(SETTINGS_PATH))){
    const struct stat *st;
    st=get_setting_stat(path+strlen(SETTINGS_PATH));
    if (!st)
      return -ENOENT;
    memcpy(stbuf, st, sizeof(struct stat));
    return 0;
  }
  memset(stbuf, 0, sizeof(struct stat));
  pthread_mutex_lock(&treelock);
  entry=get_node_by_path(path);
  if (!entry){
    debug(D_NOTICE, "no entry %s!", path);
    pthread_mutex_unlock(&treelock);
    return -ENOENT;
  }
#ifdef _DARWIN_FEATURE_64_BIT_INODE
  stbuf->st_birthtime=entry->createtime;
  stbuf->st_ctime=entry->modifytime;
  stbuf->st_mtime=entry->modifytime;
#else
  stbuf->st_ctime=entry->createtime;
  stbuf->st_mtime=entry->modifytime;
#endif
  if (entry->isfolder){
    stbuf->st_mode=S_IFDIR | 0755;
    stbuf->st_nlink=entry->tfolder.foldercnt+2;
    stbuf->st_size=entry->tfolder.nodecnt;
#if !defined(MINGW) && !defined(_WIN32)
    stbuf->st_blocks=1;
#endif
  }
  else{
    stbuf->st_mode=S_IFREG | 0644;
    stbuf->st_nlink=1;
    stbuf->st_size=entry->tfile.size;
#if !defined(MINGW) && !defined(_WIN32)
    stbuf->st_blocks=(entry->tfile.size+511)/512;
#endif
  }
  pthread_mutex_unlock(&treelock);
#if !defined(MINGW) && !defined(_WIN32)
  stbuf->st_blksize=FS_BLOCK_SIZE;
#endif
  stbuf->st_uid=myuid;
  stbuf->st_gid=mygid;
  return 0;
}

static int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
       off_t offset, struct fuse_file_info *fi){
  node *folder;
  int unsigned i;
  debug(D_NOTICE, "fs_readdir %s", path);
  if (!strcmp(path, SETTINGS_PATH)){
    const char **settings=list_settings();
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    while (*settings)
      filler(buf, *settings++, NULL, 0);
    return 0;
  }
  pthread_mutex_lock(&treelock);
  folder=get_node_by_path(path);
  if (!folder){
    pthread_mutex_unlock(&treelock);
//    debug(D_NOTICE, "fs_readdir !NOENT\n");
    return -ENOENT;
  }
  if (!folder->isfolder){
    pthread_mutex_unlock(&treelock);
//    debug(D_NOTICE, "fs_readdir !NOTDIR\n");
    return -ENOTDIR;
  }
  filler(buf, ".", NULL, 0);
  filler(buf, "..", NULL, 0);
  for (i=0; i<folder->tfolder.nodecnt; i++)
    if (folder->tfolder.nodes[i]->name[0] && !folder->tfolder.nodes[i]->hidefromlisting)
      filler(buf, folder->tfolder.nodes[i]->name, NULL, 0);
  pthread_mutex_unlock(&treelock);
  return 0;
}

static int fs_statfs(const char *path, struct statvfs *stbuf){
  node *entry;
  binresult *res;
  uint64_t q, uq;
  time_t tm;
  debug(D_NOTICE, "fs_statfs %s", path);
  pthread_mutex_lock(&treelock);
  entry=get_node_by_path(path);
  pthread_mutex_unlock(&treelock);
  if (!entry)
    return -ENOENT;
  memset(stbuf, 0, sizeof(struct statvfs));
  time(&tm);
  if (laststatfs+cachesec>tm){
    q=quota;
    uq=usedquota;
  }
  else {
    q=0;
    uq=0;
    wait_for_allowed_calls();
    res=cmd("userinfo");
    if (res){
      if (find_res(res, "result")->num==0){
        quota=q=find_res(res, "quota")->num;
        usedquota=uq=find_res(res, "usedquota")->num;
        laststatfs=tm;
      }
      free(res);
    }
    else{
      debug(D_WARNING, "statfs problem");
      return NOT_CONNECTED_ERR;
    }
  }
  stbuf->f_bsize=FS_BLOCK_SIZE;
  stbuf->f_frsize=FS_BLOCK_SIZE;
  stbuf->f_blocks=q/FS_BLOCK_SIZE;
  stbuf->f_bfree=stbuf->f_blocks-uq/FS_BLOCK_SIZE;
  stbuf->f_bavail=stbuf->f_bfree;
  stbuf->f_flag=ST_NOSUID;
  stbuf->f_namemax=1024;
  return 0;
}

static openfile *new_file(){
  openfile *f;
  f=new(openfile);
  memset(f, 0, sizeof(openfile));
  pthread_mutex_init(&f->mutex, NULL);
  pthread_cond_init(&f->cond, NULL);
  f->currentspeed=fs_settings.readaheadmax;
  return f;
}

static int wait_tree_cond(){
  treesleep++;
  if (pthread_cond_wait_sec(&treecond, &treelock, INITIAL_COND_TIMEOUT_SEC)){
    if (try_to_wake_diff() || pthread_cond_wait_timeout(&treecond, &treelock)){
      treesleep--;
      return -1;
    }
  }
  treesleep--;
  return 0;
}

static int fs_creat(const char *path, mode_t mode, struct fuse_file_info *fi){
  node *entry;
  binresult *res, *sub;
  const char *name;
  openfile *of;
  uint64_t folderid, fd, fileid;
  uint32_t connid;
  debug(D_NOTICE, "create - %s", path);
  wait_for_allowed_calls();
  pthread_mutex_lock(&treelock);
  entry=get_parent_folder(path, &name);
  if (!entry){
    pthread_mutex_unlock(&treelock);
    debug(D_WARNING, "create - no parent entry %s", path);
    return -ENOENT;
  }
  if (!entry->isfolder){
    pthread_mutex_unlock(&treelock);
    debug(D_WARNING, "create - parent is not dir entry %s", path);
    return -ENOTDIR;
  }
  folderid=entry->tfolder.folderid;
  pthread_mutex_unlock(&treelock);
  debug(D_NOTICE, "creating a file flags=%x", fi->flags);
  connid=connectionid;
  res=cmd("file_open", P_NUM("flags", fi->flags|0x0040), P_NUM("folderid", folderid), P_STR("name", name));
  if (!res){
    debug(D_WARNING, "create - not connected");
    return -EIO;
  }
  sub=find_res(res, "result");
  debug(D_NOTICE, "file created!");
  if (!sub || sub->type!=PARAM_NUM){
    free(res);
    debug(D_BUG, "create - failed to create %s", path);
    return -ENOENT;
  }
  if (sub->num!=0){
    int err=convert_error(sub->num);
    free(res);
    debug(D_WARNING, "fs_creat error %d %s", err, path);
    return err;
  }
  fd=find_res(res, "fd")->num;
  fileid=find_res(res, "fileid")->num;
  free(res);
  pthread_mutex_lock(&treelock);
  debug(D_NOTICE, "waiting for notification...");
  while (1){
    entry=get_file_by_id(fileid);
    if (entry){
      entry->tfile.refcnt++;
      break;
    }
    else if (wait_tree_cond()){
      pthread_mutex_unlock(&treelock);
      return NOT_CONNECTED_ERR;
    }
  }
  pthread_mutex_unlock(&treelock);
  of=new_file();
  of->fd=fd;
  of->file=entry;
  of->connectionid=connid;
  debug(D_NOTICE, "fs_creat - file %s file - %p", path, of);
  fi->fh=(uintptr_t)of;
  debug(D_NOTICE, "create - out OK");
  return 0;
}

static int cache_comp(const void *_c1, const void *_c2){
  const cacheentry *c1=*((const cacheentry **)_c1);
  const cacheentry *c2=*((const cacheentry **)_c2);
  return (long int)c1->lastuse-(long int)c2->lastuse;
}

static void gc_pages(){
  size_t numpages, needpages, i;
  cacheentry *entries[cachehead->numpages];
retry:
  numpages=0;
  for (i=0; i<cachehead->numpages; i++)
    if (cacheentries[i].waiting+cacheentries[i].locked+cacheentries[i].free+cacheentries[i].sleeping==0){
      entries[numpages++]=&cacheentries[i];
    }
  if (!numpages){
    /* unlikely, cache is so small that we can't schedule the request */
    pthread_mutex_unlock(&pageslock);
    sleep(1);
    pthread_mutex_lock(&pageslock);
    goto retry;
  }
  needpages=1+cachehead->numpages*PAGE_GC_PERCENT/100;
  if (needpages>=numpages){
    for (i=0; i<numpages; i++){
      list_del(entries[i]);
      entries[i]->free=1;
      list_add(freecache, entries[i]);
    }
    return;
  }
  /* here we can optimize to use something faster than qsort, to only find first needpages elements */
  qsort(entries, numpages, sizeof(cacheentry *), cache_comp);
  for (i=0; i<needpages; i++){
//    debug(D_NOTICE, "purging page of file %"PRIu64" with offset %u, last used %u\n", entries[i]->fileid, entries[i]->offset, (uint32_t)entries[i]->lastuse);
    list_del(entries[i]);
    entries[i]->free=1;
    list_add(freecache, entries[i]);
  }
}

cacheentry *get_pages(unsigned numpages){
  cacheentry *ret, *e;
  ret=NULL;
//  debug(D_NOTICE, "get_pages in\n");
  while (numpages){
    if (!freecache)
      gc_pages();
    e=freecache;
    list_del(e);
    list_add(ret, e);
    numpages--;
  }
//  debug(D_NOTICE, "get_pages out\n");
  return ret;
}

static void dec_openfile_refcnt_locked(openfile *of){
  if (!of->refcnt){
    debug(D_BUG, "dec_openfile_refcnt_locked - no ref %s !!!", of->file->name);
    return;
  }
  if (--of->refcnt==0 && of->waitref)
    pthread_cond_broadcast(&of->cond);
}

static void dec_openfile_refcnt(openfile *of){
  pthread_mutex_lock(&of->mutex);
  dec_openfile_refcnt_locked(of);
  pthread_mutex_unlock(&of->mutex);
}

static void fs_open_finished(void *_of, binresult *res);

static void check_for_reopen(openfile *of){
  if (of->connectionid==connectionid)
    return;
  debug(D_WARNING, "reopening file %s", of->file->name);
  pthread_mutex_lock(&indexlock);
  of->refcnt++;
  of->fd=0;
  of->error=0;
  of->openidx=filesopened++;
  of->connectionid=connectionid;
  cmd_callback("file_open", fs_open_finished, of, P_NUM("flags", 0), P_NUM("fileid", of->file->tfile.fileid));
  pthread_mutex_unlock(&indexlock);
}

static void schedule_readahead_finished(void *_pf, binresult *res);

static void reschedule_readahead(pagefile *pf){
  openfile *of;
  cacheentry *page;
  page=pf->page;
  of=pf->of;
  check_for_reopen(of);
  debug(D_WARNING, "rescheduling read of page %lu (offset %lu) of file %s, tries=%u", (long unsigned)page->offset, (long unsigned)page->offset*cachehead->pagesize, of->file->name, pf->tries);
  pf->tries++;
  fd_magick_start(of);
  cmd_callback("file_pread", schedule_readahead_finished, pf, fdparam, P_NUM("offset", page->offset*cachehead->pagesize), P_NUM("count", cachehead->pagesize));
  fd_magick_stop();
}

static void move_first_task_to_tail(){
  task *mt, *t;
  pthread_mutex_lock(&taskslock);
  mt=tasks;
  if (mt && mt->next){
    tasks=mt->next;
    mt->next=NULL;
    t=tasks;
    while (1){
      if (t->next==NULL){
        t->next=mt;
        break;
      }
      t=t->next;
    }
  }
  pthread_mutex_unlock(&taskslock);
}

static void reset_conn_id(openfile *of){
  of->connectionid=UINT32_MAX;
  if (of->fd){
    pthread_mutex_lock(&writelock);
    send_command_nb(sock, "file_close", P_NUM("fd", of->fd));
    pthread_mutex_unlock(&writelock);
  }
}

static void schedule_readahead_finished(void *_pf, binresult *res){
  binresult *rs;
  pagefile *pf;
  openfile *of;
  cacheentry *page;
  void *pagedata;
  size_t len;
  ssize_t ret;
  time_t tm;
  pf=(pagefile *)_pf;
  page=pf->page;
  of=pf->of;
  if (!res && pf->tries<fs_settings.retrycnt)
    return reschedule_readahead(pf);
  debug(D_NOTICE, "in");
  if (!res){
    debug(D_WARNING, "no res page %lu (offset %lu) of file %s, tries=%u", (long unsigned)page->offset, (long unsigned)page->offset*cachehead->pagesize, of->file->name, pf->tries);
    goto err;
  }
  rs=find_res(res, "result");
  if (!rs || rs->type!=PARAM_NUM || (rs->num && rs->num!=1007 && rs->num!=5004)){ // 1007 - invalid or closed file descriptor, 5004 Read error. Try reopening the file.
    if (!rs || rs->type!=PARAM_NUM)
      debug(D_BUG, "bad rs!");
    else
      debug(D_WARNING, "error %u!", (int unsigned)rs->num);
    goto err;
  }
  if (rs->num==1007 || rs->num==5004){
    if (pf->tries<fs_settings.retrycnt){
      reset_conn_id(of);
      return reschedule_readahead(pf);
    }
    else
      goto err;
  }
  pagedata=cachepages+page->pageid*cachehead->pagesize;
  len=find_res(res, "data")->num;

  debug(D_NOTICE, "request 0x%08x!", (int)len);
  ret=readall_timeout(sock, pagedata, len, PAGE_READ_TIMEOUT);
  debug(D_NOTICE, "read %d!", (int)ret);
  if (ret==-1){
    debug(D_WARNING, "failed reading page %lu (offset %lu) of file %s, tries=%u", (long unsigned)page->offset, (long unsigned)page->offset*cachehead->pagesize, of->file->name, pf->tries);
    need_reconnect=1;
    if (pf->tries<fs_settings.retrycnt){
      of->connectionid=UINT32_MAX;
      reschedule_readahead(pf);
      move_first_task_to_tail();
      return;
    }
    else
      goto err;
  }
  page->realsize=ret;
  time(&tm);
  page->lastuse=tm;
  page->fetchtime=tm;
  if (ret==0)
    page->lastuse=0;
  page->filehash=of->file->tfile.hash;
//  debug(D_NOTICE, "lock pages\n");
  pthread_mutex_lock(&pageslock);
//  debug(D_NOTICE, "locked pages\n");
  page->waiting=0;
  if (page->sleeping){
//    debug(D_NOTICE, "broadcasting...\n");
    pthread_cond_broadcast(&page->cond);
  }
  pthread_mutex_unlock(&pageslock);
  dec_openfile_refcnt(of);
  debug(D_NOTICE, "out OK");
  free(_pf);
  return;
err:
  debug(D_WARNING, "failed! setting error to %d of file %s", NOT_CONNECTED_ERR, of->file->name);
  of->error=NOT_CONNECTED_ERR;
  pthread_mutex_lock(&pageslock);
  page->waiting=0;
  page->lastuse=0;
  if (page->sleeping){
//    debug(D_NOTICE, "broadcasting...\n");
    pthread_cond_broadcast(&page->cond);
  }
  pthread_mutex_unlock(&pageslock);
  dec_openfile_refcnt(of);
  debug(D_NOTICE, "out Err");
  free(_pf);
}

static int schedule_readahead(openfile *of, off_t offset, size_t length, size_t lock_length){
  cacheentry *ce, *pages, **last;
  pagefile *pf;
  time_t tm;
  int unsigned numpages, lockpages, needpages, i;
  char dontneed[length/cachehead->pagesize+8];
  debug(D_NOTICE, "offset %"PRIi64", len %u", offset, (uint32_t)length);
  if (offset>of->file->tfile.size || !length){
    debug(D_WARNING, "invalid offset");
    return 0;
  }
  length+=offset%cachehead->pagesize;
  if (lock_length)
    lock_length+=offset%cachehead->pagesize;
  offset=(offset/cachehead->pagesize)*cachehead->pagesize;
  if (offset+length>of->file->tfile.size)
    length=of->file->tfile.size-offset;
  length=((length+cachehead->pagesize-1)/cachehead->pagesize)*cachehead->pagesize;
  if (lock_length)
    lock_length=((lock_length+cachehead->pagesize-1)/cachehead->pagesize)*cachehead->pagesize;
  if (lock_length>length)
    lock_length=length;
  // don't use sizeof(dontneed) or exactly the number of bytes from the declaration, length could grow
  memset(dontneed, 0, length/cachehead->pagesize+4);
  time(&tm);
  numpages=length/cachehead->pagesize;
  lockpages=lock_length/cachehead->pagesize;
  needpages=0;
  pthread_mutex_lock(&pageslock);
  ce=of->file->tfile.cache;
  last=&of->file->tfile.cache;
  while (ce){
    if (ce->offset*cachehead->pagesize>=offset && ce->offset*cachehead->pagesize<offset+length){
      ce->lastuse=tm;
      dontneed[ce->offset-offset/cachehead->pagesize]=1;
      if (ce->offset-offset/cachehead->pagesize<lockpages)
        ce->locked++;
    }
    last=&ce->next;
    ce=ce->next;
  }
  for (i=0; i<numpages; i++)
    if (!dontneed[i])
      needpages++;
  if (!needpages){
    pthread_mutex_unlock(&pageslock);
    debug(D_NOTICE, "out - 0");
    return 0;
  }
  pages=get_pages(needpages);
  ce=pages;
  for (i=0; i<numpages; i++)
    if (!dontneed[i]){
      ce->offset=offset/cachehead->pagesize+i;
      ce->fileid=of->file->tfile.fileid;
      ce->lastuse=tm;
      ce->waiting=1;
      ce->free=0;
      if (i<lockpages)
        ce->locked=1;
      else
        ce->locked=0;
      ce=ce->next;
    }
  pages->prev=last;
  *last=pages;
  pthread_mutex_unlock(&pageslock);
  ce=pages;
  while (ce){
    pf=new(pagefile);
    pf->page=ce;
    pf->of=of;
    pf->tries=0;
    pthread_mutex_lock(&of->mutex);
    of->refcnt++;
    pthread_mutex_unlock(&of->mutex);
    fd_magick_start(of);
    cmd_callback("file_pread", schedule_readahead_finished, pf, fdparam, P_NUM("offset", ce->offset*cachehead->pagesize), P_NUM("count", cachehead->pagesize));
    fd_magick_stop();
    ce=ce->next;
  }
  debug(D_NOTICE, "out 0");
  return 0;
}

static void fs_open_finished(void *_of, binresult *res){
  openfile *of;
  binresult *sub;
  of=(openfile *)_of;
  pthread_mutex_lock(&of->mutex);
  if (res){
    sub=find_res(res, "result");
    if (!sub || sub->type!=PARAM_NUM){
      debug(D_BUG, "fs_open_finished - EIO");
      of->error=-EIO;
      if (of->waitcmd)
        pthread_cond_broadcast(&of->cond);
    }
    else if (sub->num!=0){
      debug(D_WARNING, "fs_open_finished - error %u", (int unsigned)sub->num);
      of->error=convert_error(sub->num);
      if (of->waitcmd)
        pthread_cond_broadcast(&of->cond);
    }
    else{
      of->fd=find_res(res, "fd")->num;
    }
  }
  dec_openfile_refcnt_locked(of);
  pthread_mutex_unlock(&of->mutex);
}

static int open_setting(const char *name, struct fuse_file_info *fi){
  openfile *of;
  size_t sz;
  int ret;
  of=(openfile *)malloc(sizeof(openfile)+SETTING_BUFF);
  memset(of, 0, sizeof(openfile)+SETTING_BUFF);
  sz=SETTING_BUFF;
  ret=get_setting(name, (char *)(of+1), &sz);
  if (ret){
    free(of);
    return ret;
  }
  of->currentsize=sz;
  of->issetting=1;
  fi->fh=(uintptr_t)of;
  return 0;
}

static int fs_open(const char *path, struct fuse_file_info *fi){
  node *entry;
  openfile *of;
  debug(D_NOTICE, "fs_open %s", path);
  if (!strncmp(path, SETTINGS_PATH "/", strlen(SETTINGS_PATH)+1))
    return open_setting(path+strlen(SETTINGS_PATH)+1, fi);
  wait_for_allowed_calls();
  pthread_mutex_lock(&treelock);
  entry=get_node_by_path(path);
  if (!entry){
    debug(D_WARNING, "open - no entry %s", path);
    pthread_mutex_unlock(&treelock);
    return -ENOENT;
  }
  if (entry->isfolder){
    debug(D_WARNING, "open - is folder %s", path);
    pthread_mutex_unlock(&treelock);
    return -EISDIR;
  }
  entry->tfile.refcnt++;
  pthread_mutex_unlock(&treelock);
  of=new_file();
  of->fd=0;
  of->file=entry;
  // no need to lock of->mutex as nobody has a pointer to of for now
  of->refcnt++;
  debug(D_NOTICE, "fs_open - file path %s, file %p, flags %x", path, of, fi->flags);
  fi->fh=(uintptr_t)of;
  pthread_mutex_lock(&indexlock);
  of->openidx=filesopened++;
  of->connectionid=connectionid;
  cmd_callback("file_open", fs_open_finished, of, P_NUM("flags", fi->flags), P_NUM("fileid", entry->tfile.fileid));
  pthread_mutex_unlock(&indexlock);
  if ((fi->flags&3)==O_RDONLY)
    schedule_readahead(of, 0, fs_settings.readaheadmin, 0);
//  fi->direct_io=1;
  return 0;
}

static void *wait_refcnt_thread(void *_of){
  openfile *of=(openfile *)_of;
  pthread_mutex_lock(&of->mutex);
  if (of->refcnt){
    of->waitref=1;
    pthread_cond_wait(&of->cond, &of->mutex);
  }
  pthread_mutex_unlock(&of->mutex);
  pthread_cond_destroy(&of->cond);
  pthread_mutex_destroy(&of->mutex);
  free(of);
  return NULL;
}

static void fs_release_finished(void *_of, binresult *res){
  openfile *of=(openfile *)_of;
  binresult *result;
  debug(D_NOTICE, "fs_release_finished %p, %p!", _of, res);
  if (res){
    result=find_res(res, "result");
    if (result->num!=0)
      debug(D_BUG, "file_close failed with error %u", (unsigned int)result->num);
  }
  if (of->file)
    dec_refcnt(of->file);
  if (of->refcnt){
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, wait_refcnt_thread, of);
  }
  else {
    pthread_cond_destroy(&of->cond);
    pthread_mutex_destroy(&of->mutex);
    free(of);
  }
}

static int fs_release(const char *path, struct fuse_file_info *fi){
  openfile *of;
  debug(D_NOTICE, "fs_release %s", path);
  of=(openfile *)((uintptr_t)fi->fh);
  if (of->issetting){
    free(of);
    return 0;
  }
  wait_for_allowed_calls();
  fd_magick_start(of);
  cmd_callback("file_close", fs_release_finished, of, fdparam);
  fd_magick_stop();
  return 0;
}

static void check_old_data_finished(void *_pf, binresult *res);

static void reschedule_check_old_data(pagefile *pf){
  openfile *of;
  cacheentry *page;
  unsigned char md5b[MD5_DIGEST_LENGTH];
  char md5x[MD5_DIGEST_LENGTH*2];
  int unsigned j;
  page=pf->page;
  of=pf->of;
  check_for_reopen(of);
  debug(D_WARNING, "rescheduling check of page %lu (offset %lu) of file %s, tries=%u", (long unsigned)page->offset, (long unsigned)page->offset*cachehead->pagesize, of->file->name, pf->tries);
  pf->tries++;
  MD5((unsigned char *)cachepages+page->pageid*cachehead->pagesize, page->realsize, md5b);
  for (j=0; j<MD5_DIGEST_LENGTH; j++){
    md5x[j*2]=hexdigits[md5b[j]/16];
    md5x[j*2+1]=hexdigits[md5b[j]%16];
  }
  fd_magick_start(of);
  cmd_callback("file_pread_ifmod", check_old_data_finished, pf, fdparam, P_LSTR("md5", md5x, MD5_DIGEST_LENGTH*2),
                     P_NUM("offset", page->offset*cachehead->pagesize), P_NUM("count", cachehead->pagesize));
  fd_magick_stop();
}

static void check_old_data_finished(void *_pf, binresult *res){
  binresult *rs;
  pagefile *pf;
  openfile *of;
  cacheentry *page;
  void *pagedata;
  size_t len;
  ssize_t ret;
  time_t tm;
  pf=(pagefile *)_pf;
  page=pf->page;
  of=pf->of;
  if (!res && pf->tries<fs_settings.retrycnt)
    return reschedule_check_old_data(pf);
  //  debug(D_NOTICE, "check_old_data_finished\n");
  if (!res)
    goto err;
  rs=find_res(res, "result");
  if (!rs || rs->type!=PARAM_NUM)
    goto err;
  time(&tm);
  page->filehash=of->file->tfile.hash;
  if (rs->num==6000){
//    debug(D_NOTICE, "page pageid=%u not modified\n", page->pageid);
    page->lastuse=tm;
    page->fetchtime=tm;
    pthread_mutex_lock(&pageslock);
    page->waiting=0;
    if (page->sleeping){
//      debug(D_NOTICE, "broadcasting...\n");
      pthread_cond_broadcast(&page->cond);
    }
    pthread_mutex_unlock(&pageslock);
    dec_openfile_refcnt(of);
    free(_pf);
    return;
  }
  else if (rs->num)
    goto err;

//  debug(D_NOTICE, "page pageid=%u modified\n", page->pageid);

  pagedata=cachepages+page->pageid*cachehead->pagesize;
  len=find_res(res, "data")->num;
//  debug(D_NOTICE, "check_old_data_finished request 0x%08x!\n", (int)len);
  ret=readall_timeout(sock, pagedata, len, PAGE_READ_TIMEOUT);
  if (ret==-1){
    debug(D_WARNING, "failed reading page %lu (offset %lu) of file %s, tries=%u", (long unsigned)page->offset, (long unsigned)page->offset*cachehead->pagesize, of->file->name, pf->tries);
    need_reconnect=1;
    if (pf->tries<fs_settings.retrycnt){
      of->connectionid=UINT32_MAX;
      reschedule_readahead(pf);
      move_first_task_to_tail();
      return;
    }
    else
      goto err;
  }
  page->realsize=ret;
  page->lastuse=tm;
  page->fetchtime=tm;
  if (ret==0)
    page->lastuse=0;
  pthread_mutex_lock(&pageslock);
  page->waiting=0;
  if (page->sleeping){
//    debug(D_NOTICE, "broadcasting...\n");
    pthread_cond_broadcast(&page->cond);
  }
  pthread_mutex_unlock(&pageslock);
//  debug(D_NOTICE, "check_old_data_finished out - ok\n");
  dec_openfile_refcnt(of);
  free(_pf);
  return;
err:
  debug(D_WARNING, "failed! setting error to %d of file %s", NOT_CONNECTED_ERR, of->file->name);
  of->error=NOT_CONNECTED_ERR;
  pthread_mutex_lock(&pageslock);
  list_del(page);
  page->waiting=0;
  page->lastuse=0;
  if (page->sleeping){
//    debug(D_NOTICE, "broadcasting...\n");
    pthread_cond_broadcast(&page->cond);
  }
  pthread_mutex_unlock(&pageslock);
  dec_openfile_refcnt(of);
  free(_pf);
//  debug(D_NOTICE, "check_old_data_finished out - err\n");
}

 void check_old_data(openfile *of, off_t offset, size_t size){
  cacheentry *entries[size/cachehead->pagesize+2];
  cacheentry *ce;
  pagefile *pf;
  uint32_t frompageoff, topageoff;
  size_t pagecnt, i, j;
  time_t tm;
  unsigned char md5b[MD5_DIGEST_LENGTH];
  char md5x[MD5_DIGEST_LENGTH*2];
//  debug(D_NOTICE, "check_old_data - off: %lu, size %u\n", offset, (uint32_t)size);
  if (!size)
    return;
  frompageoff=offset/cachehead->pagesize;
  topageoff=((offset+size+cachehead->pagesize-1)/cachehead->pagesize)-1;
  time(&tm);
  pagecnt=0;
  pthread_mutex_lock(&pageslock);
  ce=of->file->tfile.cache;
  while (ce){
    if (ce->offset>=frompageoff && ce->offset<=topageoff && ce->fetchtime+cachesec<=tm && !ce->waiting && (checkifhashmatch || ce->fetchtime==0 || ce->filehash!=of->file->tfile.hash)){
      ce->waiting=1;
      entries[pagecnt++]=ce;
    }
    ce=ce->next;
  }
  pthread_mutex_unlock(&pageslock);
  for (i=0; i<pagecnt; i++){
    MD5((unsigned char *)cachepages+entries[i]->pageid*cachehead->pagesize, entries[i]->realsize, md5b);
    for (j=0; j<MD5_DIGEST_LENGTH; j++){
      md5x[j*2]=hexdigits[md5b[j]/16];
      md5x[j*2+1]=hexdigits[md5b[j]%16];
    }
//    debug(D_NOTICE, "scheduling verify of pageid=%u\n", entries[i]->pageid);
    pf=new(pagefile);
    pf->of=of;
    pf->page=entries[i];
    pf->tries=0;
    pthread_mutex_lock(&of->mutex);
    of->refcnt++;
    pthread_mutex_unlock(&of->mutex);
    fd_magick_start(of);
    cmd_callback("file_pread_ifmod", check_old_data_finished, pf, fdparam, P_LSTR("md5", md5x, MD5_DIGEST_LENGTH*2),
                     P_NUM("offset", entries[i]->offset*cachehead->pagesize), P_NUM("count", cachehead->pagesize));
    fd_magick_stop();
  }
//  debug(D_NOTICE, "check_old_data - out\n");
}

static int fs_read_setting(openfile *of, char *buf, size_t size, off_t offset){
  if (offset>=of->currentsize)
    return 0;
  if (offset+size>of->currentsize)
    size=of->currentsize-offset;
  memcpy(buf, ((char *)(of+1))+offset, size);
  return size;
}

static int fs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi){
  cacheentry *ce;
  openfile *of;
  uint32_t frompageoff, topageoff, bytes;
  size_t diff, readahead;
  time_t tm;
  int ret, i;
  of=(openfile *)((uintptr_t)fi->fh);

  if (of->error){
    debug(D_WARNING, "error is set to %d", of->error);
    return of->error;
  }

  if (of->issetting)
    return fs_read_setting(of, buf, size, offset);

  wait_for_allowed_calls();

  check_for_reopen(of);

  debug(D_NOTICE, "fs_read %s, off: %"PRIi64", size: %u", path, offset, (uint32_t)size);

  readahead=0;
  frompageoff=offset/cachehead->pagesize;
  topageoff=((offset+size+cachehead->pagesize-1)/cachehead->pagesize)-1;

  for (i=0; i<MAX_FILE_STREAMS; i++)
    if (of->streams[i].frompage<=frompageoff && of->streams[i].topage+2>=frompageoff){
      of->streams[i].id=++of->laststreamid;
      readahead=of->streams[i].length;
      of->streams[i].frompage=frompageoff;
      of->streams[i].topage=topageoff;
      of->streams[i].length+=size;
      break;
    }
  if (i==MAX_FILE_STREAMS){
    size_t min;
    int mi;
    debug(D_NOTICE, "run out of streams !!!");
    min=~(size_t)0;
    mi=0;
    for (i=0; i<MAX_FILE_STREAMS; i++)
      if (of->streams[i].id<min){
        min=of->streams[i].id;
        mi=i;
      }
    of->streams[mi].id=++of->laststreamid;
    of->streams[mi].frompage=frompageoff;
    of->streams[mi].topage=topageoff;
    of->streams[mi].length=size;
    i=mi;
  }
  time(&tm);
  if (of->currentsec==tm)
    of->bytesthissec+=size;
  else{
    of->currentspeed=(of->currentspeed+of->bytesthissec/(tm-of->currentsec))/2;
    of->bytesthissec=size;
    of->currentsec=tm;
  }
  if (readahead>of->currentspeed*fs_settings.readaheadmaxsec)
    readahead=of->currentspeed*fs_settings.readaheadmaxsec;
  if (readahead<fs_settings.readaheadmin)
    readahead=fs_settings.readaheadmin;
  else if (readahead>fs_settings.readaheadmax)
    readahead=fs_settings.readaheadmax;

  debug(D_NOTICE, "requested data with offset=%lu and size=%lu (pages %u-%u), current speed=%lu, reading ahead=%lu",
        (long unsigned)offset, (long unsigned)size, frompageoff, topageoff, (long unsigned)of->currentspeed, (long unsigned)readahead);

  if (size<readahead/2){
    if (cachesec)
      check_old_data(of, offset, readahead);
    else
      check_old_data(of, offset, size);
    if (schedule_readahead(of, offset, readahead, size)){
      debug(D_WARNING, "schedule_readahead returned error");
      return NOT_CONNECTED_ERR;
    }
  }
  else{
    if (cachesec)
      check_old_data(of, offset, size+readahead);
    else
      check_old_data(of, offset, size);
    if (schedule_readahead(of, offset, size+readahead, size)){
      debug(D_WARNING, "schedule_readahead returned error");
      return NOT_CONNECTED_ERR;
    }
  }
  ret=0;
  diff=offset-(offset/cachehead->pagesize)*cachehead->pagesize;
  pthread_mutex_lock(&pageslock);
  ce=of->file->tfile.cache;
  while (ce){
    if (ce->offset>=frompageoff && ce->offset<=topageoff){
      debug(D_NOTICE, "page with offset %u w=%u", ce->offset, ce->waiting);
      if (ce->waiting){
        ce->sleeping++;
        debug(D_NOTICE, "waiting page=%u", ce->pageid);
        if (pthread_cond_wait_sec(&ce->cond, &pageslock, INITIAL_COND_TIMEOUT_SEC)){
          debug(D_WARNING, "initial timeout on page %u of file %s", ce->pageid, of->file->name);
          pthread_mutex_unlock(&pageslock);
          i=try_to_wake_data();
          pthread_mutex_lock(&pageslock);
          if(ce->waiting && (i || pthread_cond_wait_timeout(&ce->cond, &pageslock))){
            ce->sleeping--;
            pthread_mutex_unlock(&pageslock);
            debug(D_WARNING, "second timeout on page %u of file %s, returning error", ce->pageid, of->file->name);
            return NOT_CONNECTED_ERR;
          }
        }
        debug(D_NOTICE, "got page=%u", ce->pageid);
        ce->sleeping--;
        of->streams[i].length*=2;
      }
      if (of->error){
        pthread_mutex_unlock(&pageslock);
        debug(D_WARNING, "error is set to %d", of->error);
        return of->error;
      }
      //debug(D_NOTICE, "size=%u offset=%llu diff=%u rs=%u\n", size, offset, diff, ce->realsize);
//      debug(D_NOTICE, "page with offset %u w=%u f=%u pageid=%u\n", ce->offset, ce->waiting, frompageoff, ce->pageid);
      if (ce->offset==frompageoff){
        bytes=ce->realsize-diff;
        if (bytes>size)
          bytes=size;
        memcpy(buf, cachepages+ce->pageid*cachehead->pagesize+diff, bytes);
        ret+=bytes;
      }
      else {
        bytes=ce->realsize;
        if ((ce->offset-frompageoff)*cachehead->pagesize-diff+bytes>size)
          bytes=size-((ce->offset-frompageoff)*cachehead->pagesize-diff);
        memcpy(buf+(ce->offset-frompageoff)*cachehead->pagesize-diff, cachepages+ce->pageid*cachehead->pagesize, bytes);
        ret+=bytes;
      }
      ce->lastuse=tm;
      if (ce->locked)
        ce->locked--;
    }
    ce=ce->next;
  }
  pthread_mutex_unlock(&pageslock);
  if (of->error){
    debug(D_WARNING, "error is set to %d", of->error);
    ret=of->error;
  }
  debug(D_NOTICE, "fs_read %s - %d", path, ret);
  return ret;
}


/*
static int fs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi){
  openfile *of;
  binresult *res, *data;
  int ret;
  debug(D_NOTICE, "fs_read in %s\n", path);
  of=(openfile *)((uintptr_t)fi->fh);
  fd_magick_start(of);
  res=cmd("file_pread", fdparam, P_NUM("offset", offset), P_NUM("count", size));
  fd_magick_stop();
  if (!res)
    return -EINVAL;
  data=find_res(res, "data");
  if (data && data->type==PARAM_DATA){
    ret=readall(sock, buf, data->num);
    pthread_mutex_lock(&datamutex);
    pthread_cond_signal(&datacond);
    pthread_mutex_unlock(&datamutex);
    free(res);
    debug(D_NOTICE, "read - exit %d\n", ret);
    if (ret==-1)
      return -EIO;
    else
      return ret;
  }
  else {
    debug(D_NOTICE, "bad data \n");
    binresult *sub;
    int err;
    sub=find_res(res, "result");
    if (!sub || sub->type!=PARAM_NUM){
      free(res);
      return -EIO;
    }
    err=convert_error(sub->num);
    free(res);
    debug(D_NOTICE, "read - exit %d\n", err);
    return err;
  }
}
*/

static void fs_write_finished(void *_wt, binresult *res);

static void reschedule_write(writetask *wt){
  check_for_reopen(wt->of);
  debug(D_WARNING, "rescheduling write of %u bytes of file %s, tries=%u", (int unsigned)wt->length, wt->of->file->name, wt->tries);
  wt->tries++;
  fd_magick_start(wt->of);
  cmd_data_callback("file_pwrite", wt->buff, wt->length, fs_write_finished, wt, fdparam, P_NUM("offset", wt->offset));
  fd_magick_stop();
}


static void fs_write_finished(void *_wt, binresult *res){
  writetask *wt;
  openfile *of;
  binresult *sub;
  wt=(writetask *)_wt;
  of=wt->of;

  if (!res && wt->tries<fs_settings.retrycnt)
    return reschedule_write(wt);

  pthread_mutex_lock(&unacklock);
  unackedbytes-=wt->length;
  if (unackedsleepers && unackedbytes<fs_settings.maxunackedbytes)
    pthread_cond_broadcast(&unackcond);
  pthread_mutex_unlock(&unacklock);

  pthread_mutex_lock(&of->mutex);
  if (!res){
    debug(D_WARNING, "failed! setting error to %d of file %s", NOT_CONNECTED_ERR, of->file->name);
    of->error=NOT_CONNECTED_ERR;
    if (of->waitcmd)
      pthread_cond_broadcast(&of->cond);
    goto decref;
  }
  sub=find_res(res, "result");
  if (!sub || sub->type!=PARAM_NUM){
    debug(D_BUG, "EIO");
    of->error=-EIO;
    if (of->waitcmd)
      pthread_cond_broadcast(&of->cond);
  }
  else if (sub->num!=0){
    if ((sub->num==1007 || sub->num==5003) && wt->tries<fs_settings.retrycnt){
      reset_conn_id(of);
      return reschedule_write(wt);
    }
    debug(D_ERROR, "error %u", (int unsigned)sub->num);
    of->error=convert_error(sub->num);
    if (of->waitcmd)
      pthread_cond_broadcast(&of->cond);
  }
  else{
    of->unackcomd--;
    of->unackdata-=find_res(res, "bytes")->num;
    if (!of->unackcomd && of->waitcmd)
      pthread_cond_broadcast(&of->cond);
  }
decref:
  dec_openfile_refcnt_locked(of);
  pthread_mutex_unlock(&of->mutex);
  free(_wt);
}

static int fs_write_setting(openfile *of, const char *buf, size_t size, off_t offset){
  if (offset>=SETTING_BUFF)
    return 0;
  if (offset+size>SETTING_BUFF)
    size=SETTING_BUFF-offset;
  if (offset+size>of->currentsize)
    of->currentsize=offset+size;
  of->ismodified=1;
  memcpy(((char *)(of+1))+offset, buf, size);
  return size;
}

static int fs_write(const char *path, const char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi){
  cacheentry *ce;
  openfile *of;
  writetask *wt;
  uint32_t frompageoff, topageoff;
  int triedwake;
  of=(openfile *)((uintptr_t)fi->fh);
  debug(D_NOTICE, "fs_write %s %"PRIu64" size %d", path, offset, (int)size);

  if (of->issetting)
    return fs_write_setting(of, buf, size, offset);

  wait_for_allowed_calls();

  triedwake=0;
  pthread_mutex_lock(&unacklock);
  while (unackedbytes>=fs_settings.maxunackedbytes){
    unackedsleepers++;
    if (triedwake){
      if (pthread_cond_wait_timeout(&unackcond, &unacklock)){
        unackedsleepers--;
        pthread_mutex_unlock(&unacklock);
        debug(D_WARNING, "fs_write - not connected %s", path);
        return NOT_CONNECTED_ERR;
      }
    }
    else{
      if (pthread_cond_wait_sec(&unackcond, &unacklock, INITIAL_COND_TIMEOUT_SEC)){
        triedwake=1;
        unackedsleepers--;
        pthread_mutex_unlock(&unacklock);
        try_to_wake_data();
        pthread_mutex_lock(&unacklock);
        unackedsleepers++;
      }
    }
    unackedsleepers--;
  }
  unackedbytes+=size;
  pthread_mutex_unlock(&unacklock);

  check_for_reopen(of);

  pthread_mutex_lock(&of->mutex);
  if (of->error){
    pthread_mutex_unlock(&of->mutex);
    debug(D_WARNING, "fs_write - %s error %d", path, of->error);
    return of->error;
  }
  of->unackdata+=size;
  of->unackcomd++;
  of->refcnt++;
  pthread_mutex_unlock(&of->mutex);
  wt=(writetask *)malloc(sizeof(writetask)+size);
  wt->of=of;
  wt->offset=offset;
  wt->length=size;
  wt->tries=0;
  memcpy(wt->buff, buf, size);
  fd_magick_start(of);
  cmd_data_callback("file_pwrite", buf, size, fs_write_finished, wt, fdparam, P_NUM("offset", offset));
  fd_magick_stop();
  frompageoff=offset/cachehead->pagesize;
  topageoff=((offset+size+cachehead->pagesize-1)/cachehead->pagesize)-1;
  pthread_mutex_lock(&pageslock);
  ce=of->file->tfile.cache;
  while (ce){
    if (ce->offset>=frompageoff && ce->offset<=topageoff){
      ce->fetchtime=0;
      ce->filehash=0;
    }
    ce=ce->next;
  }
  pthread_mutex_unlock(&pageslock);
  if (offset+size>of->file->tfile.size)
    of->file->tfile.size=offset+size;
  debug(D_NOTICE, "fs_write - %lu bytes", (long unsigned)size);
  return size;
}

static void fs_ftruncate_finished(void *_of, binresult *res){
  openfile *of;
  binresult *sub;
  of=(openfile *)_of;
  debug(D_NOTICE, "fs_ftruncate_finished - %p", _of);
  pthread_mutex_lock(&of->mutex);
  if (!res){
    debug(D_WARNING, "failed! setting error to %d of file %s", NOT_CONNECTED_ERR, of->file->name);
    of->error=NOT_CONNECTED_ERR;
    pthread_cond_broadcast(&of->cond);
    dec_openfile_refcnt_locked(of);
    pthread_mutex_unlock(&of->mutex);
    return;
  }
  sub=find_res(res, "result");
  if (!sub || sub->type!=PARAM_NUM){
    debug(D_BUG, "truncate - EIO");
    of->error=-EIO;
    pthread_cond_broadcast(&of->cond);
  }
  else if (sub->num!=0){
    debug(D_ERROR, "truncate - Error %u", (int unsigned)sub->num);
    of->error=convert_error(sub->num);
    pthread_cond_broadcast(&of->cond);
  }
  else{
    of->unackcomd--;
    if (!of->unackcomd)
      pthread_cond_broadcast(&of->cond);
  }
  dec_openfile_refcnt_locked(of);
  pthread_mutex_unlock(&of->mutex);
}

static int fs_ftruncate(const char *path, off_t size, struct fuse_file_info *fi){
  openfile *of;
  binresult *res;
  of=(openfile *)((uintptr_t)fi->fh);
  if (of->issetting){
    of->currentsize=0;
    of->ismodified=1;
    return 0;
  }

  wait_for_allowed_calls();

  debug(D_NOTICE, "fs_ftruncate %s -> %"PRIi64"", path, size);

  pthread_mutex_lock(&of->mutex);
  if (of->error){
    debug(D_NOTICE, "fs_ftruncate error on file.");
    pthread_mutex_unlock(&of->mutex);
    return of->error;
  }
  of->unackcomd++;
  of->refcnt++;
  pthread_mutex_unlock(&of->mutex);
  fd_magick_start(of);
  res=cmd_callback("file_truncate", fs_ftruncate_finished, of, fdparam, P_NUM("length", size));
  fd_magick_stop();
  if (res){
    of->file->tfile.size=size;
    return 0;
  }
  else{
    debug(D_NOTICE, "fs_ftruncate error error.");
    return -EIO;
  }
}

static int of_sync(openfile *of, const char *path){
  debug(D_NOTICE, "fs_sync %p -> %d", of, (int)of->error);
  if (of->issetting){
    if (of->ismodified){
      ((char *)(of+1))[of->currentsize]=0;
      return set_setting(path+strlen(SETTINGS_PATH)+1, (char *)(of+1), of->currentsize);
    }
    else
      return 0;
  }
  pthread_mutex_lock(&of->mutex);
  if (of->error || !of->unackcomd){
    pthread_mutex_unlock(&of->mutex);
    return of->error;
  }
  while (!of->error && of->unackcomd){
    of->waitcmd++;
    pthread_cond_wait(&of->cond, &of->mutex);
    of->waitcmd--;
  }
  pthread_mutex_unlock(&of->mutex);
  return of->error;
}

static int fs_flush(const char *path, struct fuse_file_info *fi){
  return of_sync((openfile *)((uintptr_t)fi->fh), path);
}

static int fs_fsync(const char *path, int datasync, struct fuse_file_info *fi){
  return of_sync((openfile *)((uintptr_t)fi->fh), path);
}

static int fs_truncate(const char *path, off_t size){
  node *entry;
  uint64_t fileid;

  debug(D_NOTICE, "fs_truncate %s", path);

  wait_for_allowed_calls();

  pthread_mutex_lock(&treelock);
  entry=get_node_by_path(path);
  if (!entry){
    pthread_mutex_unlock(&treelock);
    return -ENOENT;
  }
  if (entry->isfolder){
    pthread_mutex_unlock(&treelock);
    return -EISDIR;
  }
  fileid=entry->tfile.fileid;
  entry->tfile.size=size;
  pthread_mutex_unlock(&treelock);
  pthread_mutex_lock(&indexlock);
  pthread_mutex_lock(&writelock);
  filesopened++;
  send_command_nb(sock, "file_open", P_NUM("flags", 2), P_NUM("fileid", fileid));
  send_command_nb(sock, "file_truncate", P_STR("fd", "-1"), P_NUM("length", size));
  send_command_nb(sock, "file_close", P_STR("fd", "-1"));
  pthread_mutex_unlock(&writelock);
  pthread_mutex_unlock(&indexlock);
  return 0;
}

static int fs_mkdir(const char *path, mode_t mode){
  node *entry;
  binresult *res, *sub;
  const char *name;
  uint64_t folderid;
  debug(D_NOTICE, "fs_mkdir %s", path);
  wait_for_allowed_calls();
  pthread_mutex_lock(&treelock);
  entry=get_parent_folder(path, &name);
  if (!entry){
    pthread_mutex_unlock(&treelock);
    return -ENOENT;
  }
  if (!entry->isfolder){
    pthread_mutex_unlock(&treelock);
    return -ENOTDIR;
  }
  folderid=entry->tfolder.folderid;
  pthread_mutex_unlock(&treelock);
//  debug(D_NOTICE, "create folder in %llu, %s\n", folderid, name);
  res=cmd("createfolder", P_NUM("folderid", folderid), P_STR("name", name));
  if (!res)
    return NOT_CONNECTED_ERR;
  sub=find_res(res, "result");
  if (!sub || sub->type!=PARAM_NUM){
    free(res);
    return -ENOENT;
  }
  if (sub->num!=0){
    int err=convert_error(sub->num);
    free(res);
    return err;
  }
  folderid=find_res(find_res(res, "metadata"), "folderid")->num;
  free(res);
  pthread_mutex_lock(&treelock);
  while (1){
    entry=get_folder_by_id(folderid);
    if (entry)
      break;
    else if (wait_tree_cond()){
      pthread_mutex_unlock(&treelock);
      return NOT_CONNECTED_ERR;
    }
  }
  pthread_mutex_unlock(&treelock);
  return 0;
}

static int fs_rmdir(const char *path){
  node *entry;
  binresult *res, *sub;
  uint64_t folderid;
  debug(D_NOTICE, "fs_rmdir %s", path);
  wait_for_allowed_calls();
  pthread_mutex_lock(&treelock);
  entry=get_node_by_path(path);
  if (!entry){
    pthread_mutex_unlock(&treelock);
    return -ENOENT;
  }
  if (!entry->isfolder){
    pthread_mutex_unlock(&treelock);
    return -ENOTDIR;
  }
  folderid=entry->tfolder.folderid;
  pthread_mutex_unlock(&treelock);
  res=cmd("deletefolder", P_NUM("folderid", folderid));
  if (!res)
    return NOT_CONNECTED_ERR;
  sub=find_res(res, "result");
  if (!sub || sub->type!=PARAM_NUM){
    free(res);
    return -ENOENT;
  }
  if (sub->num!=0){
    int err=convert_error(sub->num);
    free(res);
    return err;
  }
  free(res);
  pthread_mutex_lock(&treelock);
  entry=get_node_by_path(path);
  if (entry)
    entry->hidefromlisting=1;
  pthread_mutex_unlock(&treelock);
  return 0;
}

static int fs_unlink(const char *path){
  node *entry;
  binresult *res, *sub;
  uint64_t fileid;

  debug(D_NOTICE, "fs_unlink %s", path);

  wait_for_allowed_calls();

  pthread_mutex_lock(&treelock);
  entry=get_node_by_path(path);
  if (!entry){
    pthread_mutex_unlock(&treelock);
    return -ENOENT;
  }
  if (entry->isfolder){
    pthread_mutex_unlock(&treelock);
    return -EISDIR;
  }
  fileid=entry->tfile.fileid;
  pthread_mutex_unlock(&treelock);
  res=cmd("deletefile", P_NUM("fileid", fileid));
  if (!res)
    return NOT_CONNECTED_ERR;
  sub=find_res(res, "result");
  if (!sub || sub->type!=PARAM_NUM){
    free(res);
    return -ENOENT;
  }
  if (sub->num!=0){
    int err=convert_error(sub->num);
    debug(D_NOTICE, "Failed to delete file %s - server returned: %d, err: %d\n", path, (int)sub->num, err);
    free(res);
    return err;
  }
  free(res);
  pthread_mutex_lock(&treelock);
  entry=get_node_by_path(path);
  if (entry)
    entry->hidefromlisting=1;
  pthread_mutex_unlock(&treelock);
  return 0;
}

static int rename_file(uint64_t fileid, const char *new_path){
  binresult *res, *sub;
  node *entry;
  int result = 0;
  char fixed_path[1024];

  pthread_mutex_lock(&treelock);
  entry=get_node_by_path(new_path);
  if (entry && entry->isfolder){
    size_t len = strlen(new_path);
    if (new_path[len-1] != '/'){
      strncpy(fixed_path, new_path, len-1);
      fixed_path[len] = '/';
      fixed_path[len+1] = 0;
      debug(D_NOTICE, "rename file - path changed from %s to %s", new_path, fixed_path);
      new_path = fixed_path;
    }
  }
  pthread_mutex_unlock(&treelock);

  res=cmd("renamefile", P_NUM("fileid", fileid), P_STR("topath", new_path));
  if (!res){
    return NOT_CONNECTED_ERR;
  }
  sub=find_res(res, "result");
  if (!sub || sub->type!=PARAM_NUM){
    free(res);
    return -ENOENT;
  }
  if (sub->num!=0){
    result=convert_error(sub->num);
  }
  free(res);
  return result;
}

static int rename_folder(uint64_t folderid, const char *new_path){
  binresult *res, *sub;
  int result = 0;
//  debug(D_NOTICE, "rename folder to %s \n", new_path);
  res=cmd("renamefolder", P_NUM("folderid", folderid), P_STR("topath", new_path));
  if (!res){
    debug(D_NOTICE, "rename_folder - not connected");
    return NOT_CONNECTED_ERR;
  }
  sub=find_res(res, "result");
  if (!sub || sub->type!=PARAM_NUM){
    free(res);
    debug(D_BUG, "rename_folder - no result?");
    return -ENOENT;
  }
  if (sub->num!=0){
    result=convert_error(sub->num);
  }
  free(res);
  debug(D_NOTICE, "rename folder - out %d", result);
  return result;
}

static int fs_rename(const char *old_path, const char *new_path){
  node *entry;
  int result;
  uint64_t srcid;
  debug(D_NOTICE, "rename from %s fo %s", old_path, new_path);
  wait_for_allowed_calls();
  pthread_mutex_lock(&treelock);
  entry=get_node_by_path(old_path);
  if (!entry){
    pthread_mutex_unlock(&treelock);
    return -ENOENT;
  }
  srcid = entry->isfolder ? entry->tfolder.folderid : entry->tfile.fileid;
  pthread_mutex_unlock(&treelock);

  if (entry->isfolder){
    result = rename_folder(srcid, new_path);
  }
  else{
    result = rename_file(srcid, new_path);
  }
  if (!result){
//    debug(D_NOTICE, "waiting rename...\n");
    pthread_mutex_lock(&treelock);
    while (1){
      if (get_node_by_path(new_path)){
        break;
      }
      else if (wait_tree_cond()){
        pthread_mutex_unlock(&treelock);
        return NOT_CONNECTED_ERR;
      }
    }
    pthread_mutex_unlock(&treelock);
//    debug(D_NOTICE, "rename done...\n");
  }
  return result;
}

static int fs_chmod(const char *path, mode_t mode){
  return 0;
}

int fs_utimens(const char *path, const struct timespec tv[2]){
  return 0;
}

static void init_cache(){
  if (cachefile){
    //TODO: handle file backed cache
  }
  else {
    size_t numpages, headersize;
    ssize_t i;
    cacheentry *entry;
    do{
      numpages=fs_settings.cachesize/fs_settings.pagesize;
      headersize=((sizeof(cacheheader)+sizeof(cacheentry)*numpages+4095)/4096)*4096;
#if defined(MAP_ANONYMOUS) || defined(MAP_ANON)
#if defined(MAP_ANONYMOUS)
      cachehead=(cacheheader *)mmap(NULL, fs_settings.cachesize+headersize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
#else
      cachehead=(cacheheader *)mmap(NULL, fs_settings.cachesize+headersize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
#endif
      if (cachehead==MAP_FAILED){
          if (fs_settings.cachesize > MIN_CACHE_SIZE)
            fs_settings.cachesize/=2;
          else
            exit(1);
      }
    } while (cachehead==MAP_FAILED);
#else
      cachehead=(cacheheader *)malloc(fs_settings.cachesize+headersize);
      if (!cachehead){
          if (fs_settings.cachesize > MIN_CACHE_SIZE)
            fs_settings.cachesize/=2;
          else
            exit(-ENOMEM);
      }
    } while (!cachehead);
    debug(D_NOTICE, "cache allocated size:%lu, page: %lu, pages: %lu",
          (unsigned long)fs_settings.cachesize, (unsigned long)fs_settings.pagesize, (unsigned long)numpages);
    memset(cachehead, 0, fs_settings.cachesize+headersize);
#endif
    cachepages=((char *)cachehead)+headersize;
    cacheentries=(cacheentry *)(cachehead+1);
    cachehead->cachesize=fs_settings.cachesize;
    cachehead->pagesize=fs_settings.pagesize;
    cachehead->numpages=numpages;
    cachehead->headersize=headersize;
    for (i=numpages-1; i>=0; i--){
      entry=cacheentries+i;
      entry->pageid=i;
      entry->free=1;
      pthread_cond_init(&entry->cond, NULL);
      list_add(freecache, entry);
    }
  }
}

void reset_cache(){
  size_t i;
  stop_and_wait_pending();
  pthread_mutex_lock(&pageslock);
  for (i=0; i<cachehead->numpages; i++)
    list_del(&cacheentries[i]);
#if defined(MAP_ANONYMOUS) || defined(MAP_ANON)
  munmap(cachehead, cachehead->cachesize+cachehead->headersize);
#else
  free(cachehead);
#endif
  if (cachefile)
    unlink(cachefile);
  freecache=NULL;
  init_cache();
  pthread_mutex_unlock(&pageslock);
  resume_tasks();
}

void *fs_init(struct fuse_conn_info *conn){
  pthread_t thread;
  pthread_attr_t attr;
  pthread_mutexattr_t mattr;

  if (fs_inited)
    return NULL;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&thread, &attr, receive_thread, NULL);
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&thread, &attr, diff_thread, NULL);

  pthread_mutexattr_init(&mattr);
  pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&pageslock, &mattr);

  pthread_mutex_init(&calllock, NULL);
  pthread_mutex_init(&taskslock, NULL);

  pthread_mutexattr_init(&mattr);
  pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&writelock, &mattr);

  pthread_mutex_init(&indexlock, NULL);
  pthread_mutex_init(&datamutex, NULL);
  pthread_cond_init(&datacond, NULL);

  pthread_mutexattr_init(&mattr);
  pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&treelock, &mattr);
  pthread_cond_init(&treecond, NULL);

  pthread_mutexattr_init(&mattr);
  pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&wakelock, &mattr);
  pthread_cond_init(&wakecond, NULL);

#if defined(FUSE_CAP_ASYNC_READ)
  conn->want|=FUSE_CAP_ASYNC_READ;
#endif
#if defined(FUSE_CAP_ATOMIC_O_TRUNC)
  conn->want|=FUSE_CAP_ATOMIC_O_TRUNC;
#endif
#if defined(FUSE_CAP_BIG_WRITES)
  conn->want|=FUSE_CAP_BIG_WRITES;
#endif
/* FUSE's readahead is not good enough, as it uses our read function, which requires roundtrip to the server, on the other
 * hand our readahead implementation is purely async, it just schedules the reads.
 */
  conn->max_readahead=0;
  conn->max_write=FS_MAX_WRITE;
  init_cache();
  fs_inited = 1;
  return NULL;
}

static struct fuse_operations fs_oper={
  .init     = fs_init,
  .getattr  = fs_getattr,
  .readdir  = fs_readdir,
  .statfs   = fs_statfs,
  .create   = fs_creat,
  .open     = fs_open,
  .release  = fs_release,
  .read     = fs_read,
  .write    = fs_write,
  .ftruncate= fs_ftruncate,
  .truncate = fs_truncate,
  .flush    = fs_flush,
  .fsync    = fs_fsync,
  .mkdir    = fs_mkdir,
  .rmdir    = fs_rmdir,
  .unlink   = fs_unlink,
  .rename   = fs_rename,
  .chmod    = fs_chmod,
  .utimens  = fs_utimens
};

static void sha1_hex(char *sha1hex, ...){
  const char *str;
  size_t len;
  va_list ap;
  SHA_CTX ctx;
  unsigned char sha1b[SHA_DIGEST_LENGTH];
  SHA1_Init(&ctx);
  va_start(ap, sha1hex);
  while ((str=va_arg(ap, const char *))){
    len=strlen(str);
    SHA1_Update(&ctx, str, len);
  }
  va_end(ap);
  SHA1_Final(sha1b, &ctx);
  for (len=0; len<SHA_DIGEST_LENGTH; len++){
    sha1hex[len*2]=hexdigits[sha1b[len]/16];
    sha1hex[len*2+1]=hexdigits[sha1b[len]%16];
  }
  sha1hex[SHA_DIGEST_LENGTH*2]=0;
}

static char *tolower_str(const char *str){
  char *ret;
  size_t i, len;
  len=strlen(str)+1;
  ret=(char *)malloc(len);
  for (i=0; i<len; i++)
    ret[i]=tolower(str[i]);
  return ret;
}

static int get_auth(const char* username, const char* pass)
{
  binresult *res, *sub, *au;
  char *digest=NULL;
  char *userlow;
  static char localauth[64+8];
  char sha1username[SHA_DIGEST_LENGTH*2+2], sha1pass[SHA_DIGEST_LENGTH*2+2];
  debug(D_NOTICE, "auth: %s, %s\n", username, pass);
  res=send_command(sock, "getdigest");
  if (!res)
    goto err1;
  sub=find_res(res, "digest");
  if (!sub || sub->type!=PARAM_STR)
    goto err1;
  digest=strdup(sub->str);
  free(res);
  userlow=tolower_str(username);
  sha1_hex(sha1username, userlow, NULL);
  free(userlow);
  sha1_hex(sha1pass, pass, sha1username, digest, NULL);
  res=send_command(sock, "userinfo", P_STR("username", username), P_STR("digest", digest), P_LSTR("passworddigest", sha1pass, SHA_DIGEST_LENGTH*2), P_BOOL("getauth", 1));
  free(digest);
  digest=NULL;
  if (res){
    sub=find_res(res, "result");
    if (!sub || sub->type!=PARAM_NUM || sub->num!=0)
      debug(D_NOTICE, "auth failed! - %u", (uint32_t)sub->num);
    else {
      au=find_res(res, "auth");
      if (au){
        strncpy(localauth, au->str, 64+7);
        debug(D_NOTICE, "got auth %s", localauth);
        auth = localauth;
        free(res);
        return 0;
      }
    }
  }
err1:
  free(digest);
  free(res);
  return -1;
}

void die_with_usage() {
  fprintf(stderr, "Usage: mount.pfs (--auth AUTHSTR| --username USER --password PASSWORD) [options] MOUNTPOINT\n");
  fprintf(stderr, "Supported options:\n");
  fprintf(stderr, "\t--cache_size - cache size in bytes\n");
  fprintf(stderr, "\t--page_size - page size in bytes\n");
  fprintf(stderr, "\t--ssl - use SSL for connecting\n");
  exit(2);
}

int pfs_main(int argc, char **argv, const pfs_params* params){
  int r = 0;
  binresult *res, *subres;

  debug(D_NOTICE, "starting - argc: %d", argc);
  for (r = 0; r < argc; ++r)
    debug(D_NOTICE, "\t %s", argv[r]);
  if (params->username && params->pass)
    debug(D_NOTICE, "username %s, password %s", params->username, params->pass);
  if (params->auth)
    debug(D_NOTICE, "auth %s", params->auth);
  if (params->cache_size)
    debug(D_NOTICE, "cache size: %lu B", (unsigned long)params->cache_size);
  if (params->page_size)
    debug(D_NOTICE, "cache page size: %lu B", (unsigned long)params->page_size);


  if ( !( params->auth || (params->username && params->pass)) )
    die_with_usage();

  if (params->cache_size){
    if (params->cache_size >= MIN_CACHE_SIZE && params->cache_size <= MAX_CACHE_SIZE)
      fs_settings.cachesize = params->cache_size;
  }
  if (params->page_size){
    debug(D_NOTICE, "set pagesize - not implemented!");
  }

  fs_settings.usessl = params->use_ssl;
  if (fs_settings.usessl){
    debug(D_NOTICE, "use ssl is ON");
    sock=api_connect_ssl();
    diffsock=api_connect_ssl();
  }
  else{
    sock=api_connect();
    diffsock=api_connect();
  }
  if (!sock || !diffsock){
    fprintf(stderr, "Cannot connect to server");
    return ENOTCONN;
  }

  if (params->username && params->pass){
    get_auth(params->username, params->pass);
  }else if(params->auth){
    auth = (char*)params->auth;
  }

  res=send_command(sock, "userinfo", P_STR("auth", auth));
  if(!res){
    fprintf(stderr, "Login failed\n");
    return EACCES;
  }
  subres=find_res(res, "result");
  if (!subres || subres->type!=PARAM_NUM || subres->num!=0){
    fprintf(stderr, "Login failed (%s)\n", find_res(res, "error")->str);
    return EACCES;
  }
  free(res);
  res=send_command(diffsock, "userinfo", P_STR("auth", auth));
  if(!res){
    fprintf(stderr, "Login failed\n");
    return EACCES;
  }
  subres=find_res(res, "result");
  if (!subres || subres->type!=PARAM_NUM || subres->num!=0){
    fprintf(stderr, "Login failed (%s)\n", find_res(res, "error")->str);
    return EACCES;
  }
  free(res);
  rootfolder=new(node);
  rootfolder->parent=NULL;
  rootfolder->name="";
  rootfolder->createtime=0;
  rootfolder->modifytime=0;
  rootfolder->tfolder.folderid=0;
  rootfolder->tfolder.nodes=NULL;
  rootfolder->tfolder.nodecnt=0;
  rootfolder->tfolder.nodealloc=0;
  rootfolder->tfolder.foldercnt=0;
  rootfolder->isfolder=1;
  list_add(folders[0], rootfolder);
#if !defined(MINGW) && !defined(_WIN32)
  myuid=getuid();
  mygid=getgid();
#else
mount_point = argv[1][0];
#endif
  r = fuse_main(argc, argv, &fs_oper, NULL);
  return r;
}

#ifndef SERVICE
static int parse_pfs_param(int * i, int argc, char ** argv, pfs_params* params){
  if ((!strcmp(argv[*i], "-u") || !strcmp(argv[*i], "--username")) && *i+1 < argc) {
    ++*i;
    params->username = argv[*i];
    ++*i;
    return 1;
  }
  if ((!strcmp(argv[*i], "-p") || !strcmp(argv[*i], "--password")) && *i+1 < argc) {
    ++*i;
    params->pass = strdup(argv[*i]);
    memset(argv[*i], 0, strlen(argv[*i]));
    ++*i;
    return 1;
  }
  if ((!strcmp(argv[*i], "-a") || !strcmp(argv[*i], "--auth")) && *i+1 < argc){
    ++*i;
    params->auth = strdup(argv[*i]);
    memset(argv[*i], 0, strlen(argv[*i]));
    ++*i;
    return 1;
  }
  if (!strcmp(argv[*i], "-s") || !strcmp(argv[*i], "--ssl")){
    ++*i;
    params->use_ssl = 1;
    return 1;
  }
  if ((!strcmp(argv[*i], "-c") || !strcmp(argv[*i], "--cache")) && *i+1<argc){
    ++*i;
    params->cache_size=((size_t)1024)*1024*atoi(argv[*i]);
    ++*i;
    return 1;
  }
  if ((!strcmp(argv[*i], "-g") || !strcmp(argv[*i], "--page")) && *i+1<argc){
    ++*i;
    params->page_size=atoi(argv[*i]);
    ++*i;
    return 1;
  }
  return 0;
}

static int parse_args(int argc, char** argv, char ** parsed_argv, pfs_params * params){
  int i = 0;
  int param_cnt = 0;

  while (i < argc){
    if (!parse_pfs_param(&i, argc, argv, params)){
      parsed_argv[param_cnt++] = argv[i++];
    }
  }
  return param_cnt;
}

int main(int argc, char **argv){
  pfs_params params;
  int parsed_argc;
  char* parsed_argv[argc];
#if defined(SIGPIPE) && defined(SIG_IGN)
  signal(SIGPIPE, SIG_IGN);
#endif
  memset(&params, 0, sizeof(params));
  parsed_argc = parse_args(argc, argv, parsed_argv, &params);
  return pfs_main(parsed_argc, parsed_argv, &params);
}
#endif
