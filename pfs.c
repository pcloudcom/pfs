#define FUSE_USE_VERSION 26
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <fuse.h>
#include <errno.h>
#include <openssl/md5.h>
#include "common.h"
#include "settings.h"

#define SETTING_BUFF 4096
#define INITIAL_COND_TIMEOUT_SEC 3

#if !defined(MINGW) && !defined(_WIN32)
#  include <sys/mman.h>
#endif

#include "binapi.h"
#include "pfs.h"

#if defined(MINGW) || defined(_WIN32)

#define sleep(x) Sleep(1000*(x))
#define milisleep(x) Sleep((x))
#define index(str, c) strchr(str, c)
#define rindex(str, c) strrchr(str, c)

#ifndef ENOTCONN
#   define ENOTCONN        107
#endif

#ifndef ST_NOSUID
#   define ST_NOSUID        2
#endif

#else

#define milisleep(x) usleep((x)*1000)

#endif

pfs_settings fs_settings={
  .pagesize=64*1024,
  .cachesize=1024*1024*1024,
  .readaheadmin=64*1024,
  .readaheadmax=8*1024*1024,
  .readaheadmaxsec=12,
  .usessl=0,
  .timeout=15
};

static time_t cachesec=30;

static time_t laststatfs=0;

static int checkifhashmatch=0;

static const char *cachefile=NULL;

static uint64_t quota, usedquota;

#if !defined(MINGW) && !defined(_WIN32)
static char *auth="Ec7QkEjFUnzZ7Z8W2YH1qLgxY7gGvTe09AH0i7V3kX";
#else
static char *auth="OcRE1WxMyzzZnZ0e96nIT5TIbed5RrDbNshpjWheN7";
#endif

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
  if (__of->fd){\
    fdparam.paramtype=PARAM_NUM;\
    fdparam.paramnamelen=2;\
    fdparam.paramname="fd";\
    fdparam.un.num=__of->fd;\
    __useidx=0;\
  }\
  else {\
    int __idx;\
    pthread_mutex_lock(&indexlock);\
    __idx=(int64_t)filesopened-(int64_t)of->openidx;\
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
  uint32_t type;
  task_callback call;
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
} pagefile;

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

static int unsigned treesleep=0;
static int processingtask=0;

static time_t timeoff;

static node *rootfolder;

static node *files[HASH_SIZE];
static node *folders[HASH_SIZE];

static apisock *sock, *diffsock;

static const char *hexdigits="0123456789abcdef";

static binresult *find_res(binresult *res, const char *key){
  int unsigned i;
  if (!res || res->type!=PARAM_HASH)
    return NULL;
  for (i=0; i<res->length; i++)
    if (!strcmp(res->hash[i].key, key))
      return res->hash[i].value;
  return NULL;
}

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

static int pthread_cond_wait_sec(pthread_cond_t *cond, pthread_mutex_t *mutex, time_t sec){
  struct timespec abstime;
  // Sorry - this may not compile on win
  clock_gettime(CLOCK_REALTIME, &abstime);
  abstime.tv_sec+=sec;
  return pthread_cond_timedwait(cond, mutex, &abstime);
}

static int pthread_cond_wait_timeout(pthread_cond_t *cond, pthread_mutex_t *mutex){
  if (fs_settings.timeout)
    return pthread_cond_wait_sec(cond, mutex, fs_settings.timeout);
  else
    return pthread_cond_wait(cond, mutex);
}

static int try_to_wake_diff(){
  // TODO: write something here
  return 0;
}

static int try_to_wake_data(){
  // TODO: write something here
  return 0;
}

static void remove_task(task *ptask){
  task *t, **pt;
  pthread_mutex_lock(&taskslock);
  t=tasks;
  pt=&tasks;
  while (t){
    if (t==ptask){
      *pt=t->next;
      break;
    }
    pt=&t->next;
    t=t->next;
  }
  pthread_mutex_unlock(&taskslock);
}
  
static binresult *do_cmd(const char *command, size_t cmdlen, const void *data, size_t datalen, binparam *params, size_t paramcnt,
                         task_callback callback, void *callbackptr){
  pthread_mutex_t mymutex;
  pthread_cond_t mycond;
  binparam nparams[paramcnt+1];
  task mytask, *ptask;
  binresult *res;
  int cnt;
  debug("Do-cmd enter %s, %c\n", command, callback?'C':'D');
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
    pthread_mutex_lock(&mymutex);
  }
  pthread_mutex_lock(&taskslock);
  debug("Do-cmd send %u\n", (uint32_t)taskid);
  ptask->taskid=taskid++;
  ptask->next=tasks;
  tasks=ptask;
  pthread_mutex_unlock(&taskslock);
  memcpy(nparams+1, params, paramcnt*sizeof(binparam));
  nparams[0].paramname="id";
  nparams[0].paramnamelen=2;
  nparams[0].paramtype=PARAM_NUM;
  nparams[0].un.num=ptask->taskid;
  pthread_mutex_lock(&writelock);
  res=NULL;
  cnt=0;
  while (!res && cnt++<=3){
    if (datalen)
      res=do_send_command(sock, command, cmdlen, nparams, paramcnt+1, datalen, 0);
    else
      res=do_send_command(sock, command, cmdlen, nparams, paramcnt+1, -1, 0);
    if (!res && try_to_wake_data())
      break;
  }
  if (res && datalen){
    if (writeall(sock, data, datalen)){
      debug("do_cmd - writeall failed\n");
      res=NULL;
    }
  }
  pthread_mutex_unlock(&writelock);

  if (!callback){
   if (res){
     debug("##### Do-cmd wait %s\n", command);
     if (pthread_cond_wait_sec(&mycond, &mymutex, INITIAL_COND_TIMEOUT_SEC) && (try_to_wake_data() || pthread_cond_wait_timeout(&mycond, &mymutex))){
       pthread_mutex_unlock(&mymutex);
       remove_task(ptask);
       pthread_cond_destroy(&mycond);
       pthread_mutex_destroy(&mymutex);
       return NULL;      
     }
     debug("##### Do-cmd got  %s\n", command);
   }
   pthread_mutex_unlock(&mymutex);
   res=ptask->result;
  }
  else if (!res){
    remove_task(ptask);
    free(ptask);
    callback(callbackptr, NULL);
  }

  pthread_cond_destroy(&mycond);
  pthread_mutex_destroy(&mymutex);
  debug("Do-cmd exit %s, %p\n", command, res);
  return res;
}

static void cancel_all(){
  task *t;
  debug("cancel_all\n");
  pthread_mutex_lock(&taskslock);
  while (tasks){
    t=tasks;
    tasks=t->next;
    pthread_mutex_unlock(&taskslock);
    debug("cancel_all - get task.\n");
    if (t->type==TASK_TYPE_WAIT){
      t->result=NULL;
      debug("cancel_all - task TASK_TYPE_WAIT\n");
      pthread_mutex_lock(t->mutex);
      pthread_cond_signal(t->cond);
      pthread_mutex_unlock(t->mutex);
      debug("cancel_all - task TASK_TYPE_WAIT signalled\n");
    }
    else if (t->type==TASK_TYPE_CALL){
      debug("cancel_all - task TASK_TYPE_CALL - %p\n", t);
      t->call((void *)t->result, NULL);
      free(t);
      debug("cancel all - task TASK_TYPE_CALL called\n");
    }
    pthread_mutex_lock(&taskslock);
  }
  pthread_mutex_unlock(&taskslock);
  debug("cancel_all leave\n");
}

static void cancel_all_and_reconnect(){
  binresult *res;
  apisock null;
  debug("cancel_all_and_reconnect\n");
  cancel_all();
  null.ssl=NULL;
  null.sock=-1;
  pthread_mutex_lock(&writelock);
  debug("cancel_all_and_reconnect - after write lock\n");
  api_close(sock);
  do{
    if (fs_settings.usessl)
      sock=api_connect_ssl();
    else
      sock=api_connect();
    if (!sock){
      debug("cancel_all_and_reconnect - failed to connect\n");
      sock=&null;
      pthread_mutex_unlock(&writelock);
      sleep(1);
      cancel_all();
      pthread_mutex_lock(&writelock);
      sock=NULL;
    }
    else {
      res=send_command(sock, "userinfo", P_STR("auth", auth));
      if (!res){
        debug("cancel_all_and_reconnect - failed to login\n");
        api_close(sock);
        sock=NULL;
      }
      else {
        if (find_res(res, "result")->num!=0){
          debug("cancel_all_and_reconnect - problem on login\n");
          pthread_mutex_unlock(&writelock);
          while (1){ //?????
            cancel_all();
            sleep(1);
          }
        }
        free(res);
      }
    }
  } while (!sock);
  pthread_mutex_unlock(&writelock);
  cancel_all();
  debug("cancel_all_and_reconnect leave\n");
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


static void *receive_thread(void *ptr){
  binresult *res, *id, *sub;
  task *t, **pt;
  int hasdata;
  while (1){
    res=get_result(sock);
    if (!res){
      cancel_all_and_reconnect();
      continue;
    }
    id=find_res(res, "id");
    if (!id || id->type!=PARAM_NUM){
      free(res);
      debug("receive_thread - no ID\n");
      continue;
    }
    debug("receive_thread received %u. \n", (uint32_t)id->num);
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
      debug("receive_thread - no task\n");
      continue;
    }
    sub=find_res(res, "data");
    hasdata=sub && sub->type==PARAM_DATA;
    /* !!! IMPORTANT !!!
     * if we have TASK_TYPE_WAIT, t is on the stack of the thread waiting on t->cond, therefore no free
     * if we have TASK_TYPE_CALL, t is allcated and we need to free. callback does not have to free the result
     */
    if (t->type==TASK_TYPE_WAIT){
      t->result=res;
      if (hasdata){
        pthread_mutex_lock(&datamutex);
      }
      pthread_mutex_lock(t->mutex);
      pthread_cond_signal(t->cond);
      pthread_mutex_unlock(t->mutex);
      if (hasdata){
        pthread_cond_wait(&datacond, &datamutex);
        pthread_mutex_unlock(&datamutex);
      }
    }
    else if (t->type==TASK_TYPE_CALL){
//      debug("receive thread calling task - %p\n", t);
      t->call((void *)t->result, res);
//      debug("receive thread task called - %p\n", t);
      free(res);
      free(t);
    }
    else
      free(res);
    pthread_mutex_lock(&taskslock);
    processingtask--;
    pthread_mutex_unlock(&taskslock);
//    debug("receive_thread - end loop\n");
  }
  return NULL;
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

static void diff_create_folder(binresult *meta, time_t mtime){
  binresult *name;
  node *folder, *f;
  uint64_t parentid;
  name=find_res(meta, "name");
  folder=(node *)malloc(sizeof(node));
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
  ce=file->tfile.cache;
  while (ce){
    cn=ce->next;
    ce->free=1;
#ifdef MADV_DONTNEED
    madvise(cachepages+ce->pageid*cachehead->pagesize, cachehead->pagesize, MADV_DONTNEED);
#endif
    list_add(freecache, ce);
    ce=cn;
  }
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
  file=(node *)malloc(sizeof(node));
  file->name=malloc(name->length+1);
  memcpy(file->name, name->str, name->length+1);
  file->createtime=find_res(meta, "created")->num+timeoff;
  file->modifytime=find_res(meta, "modified")->num+timeoff;
  file->tfile.fileid=find_res(meta, "fileid")->num;
  file->tfile.size=find_res(meta, "size")->num;
  file->tfile.hash=find_res(meta, "hash")->num;
  file->tfile.cache=NULL;
  file->isfolder=0;
  file->isdeleted=0;
  parentid=find_res(meta, "parentfolderid")->num;
  pthread_mutex_lock(&treelock);

  name=find_res(meta, "deletedfileid");
  if (name){
    uint64_t old_id=name->num;
    debug("create-> deleted old file\n");
    f=get_file_by_id(old_id);
    if (f){
      f->parent->modifytime=mtime;
      debug("deleted old file %s\n", f->name);
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
  if (treesleep)
    pthread_cond_broadcast(&treecond);
  pthread_mutex_unlock(&treelock);
}

static void diff_modifyfile_file(binresult *meta, time_t mtime){
  uint64_t fileid;
  binresult *res;
  node *f;
//  debug("modify file in \n");
  fileid=find_res(meta, "fileid")->num;
  pthread_mutex_lock(&treelock);


  res = find_res(meta, "deletedfileid");
  if (res){
    uint64_t old_id = res->num;
    debug("deleted old file\n");
    f=get_file_by_id(old_id);
    if (f){
      f->parent->modifytime=mtime;
      debug("deleted old file %s\n", f->name);
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
//      debug("name -> %s\n", res->str);
      f->name=realloc(f->name, res->length+1);
      memcpy((void*)f->name, res->str, res->length+1);
    }
    res = find_res(meta, "parentfolderid");
    if (res){
      uint64_t parent = res->num;
      if (parent != f->parent->tfolder.folderid){
        node* par;
        debug("file change parent\n");
        par=get_folder_by_id(parent);
        if (!par){
          pthread_mutex_unlock(&treelock);
          debug("modify file out - no parent?!? \n");
          return;
        }
        if (par->tfolder.nodecnt>=par->tfolder.nodealloc){
          par->tfolder.nodealloc+=64;
          par->tfolder.nodes=realloc(par->tfolder.nodes, sizeof(node *)*par->tfolder.nodealloc);
        }
        par->tfolder.nodes[par->tfolder.nodecnt++]=f;
        par->modifytime=mtime;

        remove_from_parent(f);
        f->parent=par;
     }
    }
  }
  if (treesleep)
    pthread_cond_broadcast(&treecond);
  pthread_mutex_unlock(&treelock);
//  debug("modify file out OK\n");
}

static void diff_modifyfile_folder(binresult* meta, time_t mtime){
  uint64_t folderid;
  binresult *res;
  node *f;
//  debug("modify folder in\n");
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
//      debug("folder name -> %s\n", res->str);
      f->name=realloc(f->name, res->length+1);
      memcpy(f->name, res->str, res->length+1);
    }
    res = find_res(meta, "parentfolderid");
    if (res){
      uint64_t parent = res->num;
      if (parent != f->parent->tfolder.folderid){
        node* par;
        debug("folder - change parent %u -> %u\n", (uint32_t)f->parent->tfolder.folderid, (uint32_t)parent);
        par=get_folder_by_id(parent);
        if (!par){
          pthread_mutex_unlock(&treelock);
          debug("modify folder out - no parent\n");
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
        f->parent=par;
      }
    }
  }
  if (treesleep)
    pthread_cond_broadcast(&treecond);
  pthread_mutex_unlock(&treelock);
//  debug("modify folder out - OK\n");
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
  binresult *event, *meta;
  time_t tm;
  event=find_res(diff, "event");
  meta=find_res(diff, "metadata");
  tm=find_res(diff, "time")->num+timeoff;
//  debug("diff -> %s\n", event->str);
  if (event && event->type==PARAM_STR && meta && meta->type==PARAM_HASH){
    if (!strcmp(event->str, "createfolder"))
      diff_create_folder(meta, tm);
    else if (!strcmp(event->str, "createfile"))
      diff_create_file(meta, tm);
    else if (!strcmp(event->str, "modifyfile"))
      diff_modifyfile_file(meta, tm);
    else if (!strcmp(event->str, "modifyfolder"))
      diff_modifyfile_folder(meta, tm);
    else if (!strcmp(event->str, "deletefolder"))
      diff_delete_folder(meta, tm);
    else if (!strcmp(event->str, "deletefile"))
      diff_delete_file(meta, tm);
  }
}

static void *diff_thread(void *ptr){
  uint64_t diffid;
  binresult *res, *sub, *entries, *entry;
  int unsigned i;
  diffid=0;
  while (1){
//    debug("send diff\n");
    res=send_command(diffsock, "diff", P_NUM("diffid", diffid), P_BOOL("block", 1), P_STR("timeformat", "timestamp"));
    if (!res){
      debug("diff thread - reconnecting\n");
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
      if (!sub || sub->type!=PARAM_NUM || sub->num==0){
        free(res);
        continue;
      }
      free(res);
      debug("diff thread - reconnected!\n");
      return NULL;
    }
    sub=find_res(res, "result");
    if (!sub || sub->type!=PARAM_NUM || sub->num!=0){
      free(res);
      continue;
    }
    entries=find_res(res, "entries");
    for (i=0; i<entries->length; i++){
      entry=entries->array[i];
      process_diff(entry);
      diffid=find_res(entry, "diffid")->num;
    }
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
  if (lslash==path){
    *name=path+1;
    return get_node_by_path("/");
  }
  *name=lslash+1;
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
  debug("fs_getattr, %s\n", path);
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
    debug("no entry %s! \n", path);
    pthread_mutex_unlock(&treelock);
    return -ENOENT;
  }
  stbuf->st_ctime=entry->createtime;
  stbuf->st_mtime=entry->modifytime;
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
  debug ("fs_readdir %s\n", path);
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
//    debug("fs_readdir !NOENT\n");
    return -ENOENT;
  }
  if (!folder->isfolder){
    pthread_mutex_unlock(&treelock);
//    debug("fs_readdir !NOTDIR\n");
    return -ENOTDIR;
  }
  filler(buf, ".", NULL, 0);
  filler(buf, "..", NULL, 0);
  for (i=0; i<folder->tfolder.nodecnt; i++)
    filler(buf, folder->tfolder.nodes[i]->name, NULL, 0);
  pthread_mutex_unlock(&treelock);
  return 0;
}

static int fs_statfs(const char *path, struct statvfs *stbuf){
  node *entry;
  binresult *res;
  uint64_t q, uq;
  time_t tm;
  debug ("fs_statfs %s\n", path);
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
      debug("statfs problem\n");
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
  debug("create - %s \n", path);
  wait_for_allowed_calls();
  pthread_mutex_lock(&treelock);
  entry=get_parent_folder(path, &name);
  if (!entry){
    pthread_mutex_unlock(&treelock);
    debug("create - no parent entry %s \n", path);
    return -ENOENT;
  }
  if (!entry->isfolder){
    pthread_mutex_unlock(&treelock);
    debug("create - parent is not dir entry %s \n", path);
    return -ENOTDIR;
  }
  folderid=entry->tfolder.folderid;
  pthread_mutex_unlock(&treelock);
  debug("creating a file flags=%x\n", fi->flags);
  res=cmd("file_open", P_NUM("flags", fi->flags|0x0040), P_NUM("folderid", folderid), P_STR("name", name));
  if (!res){
    debug("create - not connected\n");
    return -EIO;
  }
  sub=find_res(res, "result");
  debug("file created!\n");
  if (!sub || sub->type!=PARAM_NUM){
    free(res);
    debug("create - failed to create %s\n", path);
    return -ENOENT;
  }
  if (sub->num!=0){
    int err=convert_error(sub->num);
    free(res);
    debug ("fs_creat error %d %s\n", err, path);
    return err;
  }
  fd=find_res(res, "fd")->num;
  fileid=find_res(res, "fileid")->num;
  free(res);
  pthread_mutex_lock(&treelock);
  debug("waiting for notification...\n");
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
  debug("fs_creat - file %s file - %p\n", path, of);
  fi->fh=(uintptr_t)of;
  debug("create - out OK\n");
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
    if (cacheentries[i].waiting+cacheentries[i].locked+cacheentries[i].free==0){
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
//    debug("purging page of file %"PRIu64" with offset %u, last used %u\n", entries[i]->fileid, entries[i]->offset, (uint32_t)entries[i]->lastuse);
    list_del(entries[i]);
    entries[i]->free=1;
    list_add(freecache, entries[i]);
  }
}

cacheentry *get_pages(unsigned numpages){
  cacheentry *ret, *e;
  ret=NULL;
//  debug("get_pages in\n");
  while (numpages){
    if (!freecache)
      gc_pages();
    e=freecache;
    list_del(e);
    list_add(ret, e);
    numpages--;
  }
//  debug("get_pages out\n");
  return ret;
}

static void dec_openfile_refcnt_locked(openfile *of){
  if (!of->refcnt){
    debug("dec_openfile_refcnt_locked - no ref!!! \n");
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
  free(_pf);
  debug("schedule_readahead_finished in\n");
  if (!res){
    debug("schedule_readahead_finished no res!\n");
    goto err;
  }
  rs=find_res(res, "result");
  if (!rs || rs->type!=PARAM_NUM || rs->num){
    debug("schedule_readahead_finished bad rs!\n");
    goto err;
  }
  pagedata=cachepages+page->pageid*cachehead->pagesize;
  len=find_res(res, "data")->num;

  debug("schedule_readahead_finished request 0x%08x!\n", (int)len);
  ret=readall(sock, pagedata, len);
  debug("schedule_readahead_finished read %d!\n", (int)ret);
  if (ret==-1)
    goto err;
  page->realsize=ret;
  time(&tm);
  page->lastuse=tm;
  page->fetchtime=tm;
  if (ret==0)
    page->lastuse=0;
  page->filehash=of->file->tfile.hash;
//  debug("lock pages\n");
  pthread_mutex_lock(&pageslock);
//  debug("locked pages\n");
  page->waiting=0;
  if (page->sleeping){
//    debug("broadcasting...\n");
    pthread_cond_broadcast(&page->cond);
  }
  pthread_mutex_unlock(&pageslock);
  dec_openfile_refcnt(of);
  debug("schedule_readahead_finished out OK\n");
  return;
err:
  debug("schedule_readahead_finished failed! NC error\n");
  of->error=NOT_CONNECTED_ERR;
  pthread_mutex_lock(&pageslock);
  list_del(page);
  page->waiting=0;
  page->lastuse=0;
  if (page->sleeping){
//    debug("broadcasting...\n");
    pthread_cond_broadcast(&page->cond);
  }
  pthread_mutex_unlock(&pageslock);
  dec_openfile_refcnt(of);
  debug("schedule_readahead_finished out Err\n");
}

static int schedule_readahead(openfile *of, off_t offset, size_t length, size_t lock_length){
  cacheentry *ce, *pages, **last;
  pagefile *pf;
  time_t tm;
  int unsigned numpages, lockpages, needpages, i;
  char dontneed[fs_settings.readaheadmax/cachehead->pagesize];
  int ret;
  debug("schedule_readahead offset %lu, len %u\n", offset, (uint32_t)length);
  if (offset>of->file->tfile.size){
    debug("schedule_readahead - invalid offset\n");
    return 0;
  }
  length+=offset%cachehead->pagesize;
  lock_length+=offset%cachehead->pagesize;
  offset=(offset/cachehead->pagesize)*cachehead->pagesize;
  if (offset+length>of->file->tfile.size)
    length=of->file->tfile.size-offset;
  length=((length+cachehead->pagesize-1)/cachehead->pagesize)*cachehead->pagesize;
  lock_length=((lock_length+cachehead->pagesize-1)/cachehead->pagesize)*cachehead->pagesize;
  if (lock_length>length)
    lock_length=length;
  memset(dontneed, 0, fs_settings.readaheadmax/cachehead->pagesize);
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
    debug ("schedule_readahead out - 0\n");
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
      ce=ce->next;
    }
  pages->prev=last;
  *last=pages;
  pthread_mutex_unlock(&pageslock);
  ce=pages;
  ret=0;
  while (ce){
    pf=new(pagefile);
    pf->page=ce;
    pf->of=of;
    pthread_mutex_lock(&of->mutex);
    of->refcnt++;
    pthread_mutex_unlock(&of->mutex);
    fd_magick_start(of);
    cmd_callback("file_pread", schedule_readahead_finished, pf, fdparam, P_NUM("offset", ce->offset*cachehead->pagesize), P_NUM("count", cachehead->pagesize));
    fd_magick_stop();
    ce=ce->next;
  }
  debug ("schedule_readahead out : %d\n", ret);
  return ret;
}

static void fs_open_finished(void *_of, binresult *res){
  openfile *of;
  binresult *sub;
  of=(openfile *)_of;
  sub=find_res(res, "result");
  if (!res) return;
  pthread_mutex_lock(&of->mutex);
  if (!sub || sub->type!=PARAM_NUM){
    debug("fs_open_finished - EIO\n");
    of->error=-EIO;
    if (of->waitcmd)
      pthread_cond_broadcast(&of->cond);
  }
  else if (sub->num!=0){
    debug("fs_open_finished - error %u\n", (uint32_t)sub->num);
    of->error=convert_error(sub->num);
    if (of->waitcmd)
      pthread_cond_broadcast(&of->cond);
  }
  else{
    of->fd=find_res(res, "fd")->num;
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
  debug("fs_open %s\n", path);
  if (!strncmp(path, SETTINGS_PATH "/", strlen(SETTINGS_PATH)+1))
    return open_setting(path+strlen(SETTINGS_PATH)+1, fi);
  wait_for_allowed_calls();
  pthread_mutex_lock(&treelock);
  entry=get_node_by_path(path);
  if (!entry){
    debug("open - no entry %s \n", path);
    pthread_mutex_unlock(&treelock);
    return -ENOENT;
  }
  if (entry->isfolder){
    debug("open - is folder %s \n", path);
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
  debug("fs_open - file path %s, file %p, flags %x\n", path, of, fi->flags);
  fi->fh=(uintptr_t)of;
  pthread_mutex_lock(&indexlock);
  of->openidx=filesopened++;
  cmd_callback("file_open", fs_open_finished, of, P_NUM("flags", fi->flags), P_NUM("fileid", entry->tfile.fileid));
  pthread_mutex_unlock(&indexlock);
  if ((fi->flags&3)==O_RDONLY)
    schedule_readahead(of, 0, fs_settings.readaheadmin, 0);
//  fi->direct_io=1;
  return 0;
}

static void fs_release_finished(void *_of, binresult *res){
  openfile *of;
  of=(openfile *)_of;
  debug("fs_release_finished %p, %p! \n", _of, res);
  if (of->file)
    dec_refcnt(of->file);
  pthread_mutex_lock(&of->mutex);
  if (!res) debug("fs_release_finished - lock mutex\n");
  of->waitref=1;
  if (!res) debug("fs_release_finished - waiting condition %u\n", of->refcnt);
  while (res && of->refcnt)
    pthread_cond_wait(&of->cond, &of->mutex);
  pthread_mutex_unlock(&of->mutex);
  if (!res) debug("fs_release_finished - destroy mutex\n");
  pthread_cond_destroy(&of->cond);
  pthread_mutex_destroy(&of->mutex);
  free(of);
}

static int fs_release(const char *path, struct fuse_file_info *fi){
  openfile *of;
  debug("fs_release %s\n", path);
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
  free(_pf);
  //  debug("check_old_data_finished\n");
  if (!res)
    goto err;
  rs=find_res(res, "result");
  if (!rs || rs->type!=PARAM_NUM)
    goto err;
  time(&tm);
  page->filehash=of->file->tfile.hash;
  if (rs->num==6000){
//    debug("page pageid=%u not modified\n", page->pageid);
    page->lastuse=tm;
    page->fetchtime=tm;
    pthread_mutex_lock(&pageslock);
    page->waiting=0;
    if (page->sleeping){
//      debug("broadcasting...\n");
      pthread_cond_broadcast(&page->cond);
    }
    pthread_mutex_unlock(&pageslock);
    dec_openfile_refcnt(of);
    return;
  }
  else if (rs->num)
    goto err;

//  debug("page pageid=%u modified\n", page->pageid);

  pagedata=cachepages+page->pageid*cachehead->pagesize;
  len=find_res(res, "data")->num;
//  debug("check_old_data_finished request 0x%08x!\n", (int)len);
  ret=readall(sock, pagedata, len);
  if (ret==-1)
    goto err;
  page->realsize=ret;
  page->lastuse=tm;
  page->fetchtime=tm;
  if (ret==0)
    page->lastuse=0;
  pthread_mutex_lock(&pageslock);
  page->waiting=0;
  if (page->sleeping){
//    debug("broadcasting...\n");
    pthread_cond_broadcast(&page->cond);
  }
  pthread_mutex_unlock(&pageslock);
//  debug("check_old_data_finished out - ok\n");
  dec_openfile_refcnt(of);
  return;
err:
  debug("check_old_data_finished error (null)\n");
  of->error=NOT_CONNECTED_ERR;
  pthread_mutex_lock(&pageslock);
  list_del(page);
  page->waiting=0;
  page->lastuse=0;
  if (page->sleeping){
//    debug("broadcasting...\n");
    pthread_cond_broadcast(&page->cond);
  }
  pthread_mutex_unlock(&pageslock);
  dec_openfile_refcnt(of);
//  debug("check_old_data_finished out - err\n");
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
//  debug("check_old_data - off: %lu, size %u\n", offset, (uint32_t)size);
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
//    debug("scheduling verify of pageid=%u\n", entries[i]->pageid);
    pf=(pagefile *)malloc(sizeof(pagefile));
    pf->of=of;
    pf->page=entries[i];
    pthread_mutex_lock(&of->mutex);
    of->refcnt++;
    pthread_mutex_unlock(&of->mutex);
    fd_magick_start(of);
    cmd_callback("file_pread_ifmod", check_old_data_finished, pf, fdparam, P_LSTR("md5", md5x, MD5_DIGEST_LENGTH*2),
                     P_NUM("offset", entries[i]->offset*cachehead->pagesize), P_NUM("count", cachehead->pagesize));
    fd_magick_stop();
  }
//  debug("check_old_data - out\n");
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
  /* It might make sense to place a lock (of->mutex is good candidate) around the following operation on one hand as this might be
   * executing in parralel. On the other hand, corrupted streams table will only lead to miscalculated readahed, but still winthin
   * boundaries, so generally no harm.
   */

  if (of->issetting)
    return fs_read_setting(of, buf, size, offset);

  wait_for_allowed_calls();

  debug ("fs_read %s, off: %lu, size: %u\n", path, offset, (uint32_t)size);

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
    //debug("run out of streams !!!\n");
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
    of->bytesthissec=0;
    of->currentsec=tm;
  }
  if (readahead>of->currentspeed*fs_settings.readaheadmaxsec)
    readahead=of->currentspeed*fs_settings.readaheadmaxsec;
  if (readahead<fs_settings.readaheadmin)
    readahead=fs_settings.readaheadmin;
  else if (readahead>fs_settings.readaheadmax)
    readahead=fs_settings.readaheadmax;

//  debug("requested data with offset=%lu and size=%u (pages %u-%u), current speed=%u, reading ahead=%u\n", offset, size, frompageoff, topageoff, of->currentspeed, readahead);

  if (size<readahead/2){
    if (cachesec)
      check_old_data(of, offset, readahead);
    else
      check_old_data(of, offset, size);
    if (schedule_readahead(of, offset, readahead, size)){
//      debug("read - err 1\n");
      return NOT_CONNECTED_ERR;
    }
  }
  else{
    if (cachesec)
      check_old_data(of, offset, size+readahead);
    else
      check_old_data(of, offset, size);
    if (schedule_readahead(of, offset, size+readahead, size)){
//      debug("read - err 2\n");
      return NOT_CONNECTED_ERR;
    }
  }
  ret=0;
  diff=offset-(offset/cachehead->pagesize)*cachehead->pagesize;
  pthread_mutex_lock(&pageslock);
  ce=of->file->tfile.cache;
  while (ce){
    if (ce->offset>=frompageoff && ce->offset<=topageoff){
//      debug("page with offset %u w=%u\n", ce->offset, ce->waiting);
      if (ce->waiting){
        ce->sleeping++;
//        debug("waiting page=%u\n", ce->pageid);
        if (pthread_cond_wait_sec(&ce->cond, &pageslock, INITIAL_COND_TIMEOUT_SEC) && (try_to_wake_data() || pthread_cond_wait_timeout(&ce->cond, &pageslock))){
          ce->sleeping--;
          pthread_mutex_unlock(&pageslock);
          return NOT_CONNECTED_ERR;
        }
//        debug("got page=%u\n", ce->pageid);
        ce->sleeping--;
        of->streams[i].length*=2;
      }
      //debug("size=%u offset=%llu diff=%u rs=%u\n", size, offset, diff, ce->realsize);
//      debug("page with offset %u w=%u f=%u pageid=%u\n", ce->offset, ce->waiting, frompageoff, ce->pageid);
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
    ret=of->error;
    of->error=0;
  }
//  debug ("fs_read %s - %d\n", path, ret);
  return ret;
}


/*
static int fs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi){
  openfile *of;
  binresult *res, *data;
  int ret;
  debug("fs_read in %s\n", path);
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
    debug ("read - exit %d\n", ret);
    if (ret==-1)
      return -EIO;
    else
      return ret;
  }
  else {
    debug("bad data \n");
    binresult *sub;
    int err;
    sub=find_res(res, "result");
    if (!sub || sub->type!=PARAM_NUM){
      free(res);
      return -EIO;
    }
    err=convert_error(sub->num);
    free(res);
    debug ("read - exit %d\n", err);
    return err;
  }
}
*/


static void fs_write_finished(void *_of, binresult *res){
  openfile *of;
  binresult *sub;
  of=(openfile *)_of;
  pthread_mutex_lock(&of->mutex);
  if (!res){
    debug("fs_write_finished - error - %u\n", of->waitcmd);
    of->error=NOT_CONNECTED_ERR;
    if (of->waitcmd)
      pthread_cond_broadcast(&of->cond);
    goto decref;
  }
  sub=find_res(res, "result");
  if (!sub || sub->type!=PARAM_NUM){
    debug("fs_write_finished EIO\n");
    of->error=-EIO;
    if (of->waitcmd)
      pthread_cond_broadcast(&of->cond);
  }
  else if (sub->num!=0){
    debug("fs_write_finished error\n");
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
  uint32_t frompageoff, topageoff;
  of=(openfile *)((uintptr_t)fi->fh);
  debug ("fs_write %s\n", path);

  if (of->issetting)
    return fs_write_setting(of, buf, size, offset);

  wait_for_allowed_calls();

  pthread_mutex_lock(&of->mutex);
  if (of->error){
    pthread_mutex_unlock(&of->mutex);
    return of->error;
  }
  of->unackdata+=size;
  of->unackcomd++;
  of->refcnt++;
  pthread_mutex_unlock(&of->mutex);
  fd_magick_start(of);
  cmd_data_callback("file_pwrite", buf, size, fs_write_finished, of, fdparam, P_NUM("offset", offset));
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
  debug ("fs_write - %lu bytes\n", (long unsigned)size);
  return size;
}

static void fs_ftruncate_finished(void *_of, binresult *res){
  openfile *of;
  binresult *sub;
  of=(openfile *)_of;
  debug("fs_ftruncate_finished - %p\n", _of);
  pthread_mutex_lock(&of->mutex);
  if (!res){
    debug("fs_ftruncate_finished error\n");
    of->error=NOT_CONNECTED_ERR;
    of->refcnt--;
    pthread_cond_broadcast(&of->cond);
    pthread_mutex_unlock(&of->mutex);
    return;
  }
  sub=find_res(res, "result");
  if (!sub || sub->type!=PARAM_NUM){
    debug("truncate - EIO\n");
    of->error=-EIO;
    pthread_cond_broadcast(&of->cond);
  }
  else if (sub->num!=0){
    debug("truncate - Error\n");
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

  debug ("fs_ftruncate %s -> %lu\n", path, size);

  pthread_mutex_lock(&of->mutex);
  if (of->error){
    debug("fs_ftruncate error on file.\n");
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
    debug("fs_ftruncate error error.\n");
    return -EIO;
  }
}

static int of_sync(openfile *of, const char *path){
  debug("fs_sync %p -> %d\n", of, (int)of->error);
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

  debug ("fs_truncate %s\n", path);

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
  pthread_mutex_lock(&writelock);
  send_command_nb(sock, "file_open", P_NUM("flags", 2), P_NUM("fileid", fileid));
  send_command_nb(sock, "file_truncate", P_STR("fd", "-1"), P_NUM("length", size));
  send_command_nb(sock, "file_close", P_STR("fd", "-1"));
  pthread_mutex_unlock(&writelock);
  return 0;
}

static int fs_mkdir(const char *path, mode_t mode){
  node *entry;
  binresult *res, *sub;
  const char *name;
  uint64_t folderid;
  debug("fs_mkdir %s\n", path);
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
//  debug("create folder in %llu, %s\n", folderid, name);
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
  debug("fs_rmdir %s\n", path);
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
  return 0;
}

static int fs_unlink(const char *path){
  node *entry;
  binresult *res, *sub;
  uint64_t fileid;

  debug ("fs_unlink %s\n", path);

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
    debug("Failed to delete file %s - server returned: %d, err: %d\n", path, (int)sub->num, err);
    free(res);
    return err;
  }
  free(res);
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
      debug("rename file - path changed from %s to %s\n", new_path, fixed_path);
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
//  debug("rename folder to %s \n", new_path);
  res=cmd("renamefolder", P_NUM("folderid", folderid), P_STR("topath", new_path));
  if (!res){
    debug("rename_folder - not connected\n");
    return NOT_CONNECTED_ERR;
  }
  sub=find_res(res, "result");
  if (!sub || sub->type!=PARAM_NUM){
    free(res);
    debug("rename_folder - no result?\n");
    return -ENOENT;
  }
  if (sub->num!=0){
    result=convert_error(sub->num);
  }
  free(res);
  debug("rename folder - out %d\n", result);
  return result;
}

static int fs_rename(const char *old_path, const char *new_path){
  node *entry;
  int result;
  uint64_t srcid;
  debug("rename from %s fo %s\n", old_path, new_path);
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
//    debug("waiting rename...\n");
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
//    debug("rename done...\n");
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
#if !defined(MAP_ANONYMOUS) && !defined(MAP_ANON)
    do{
#endif
    numpages=fs_settings.cachesize/fs_settings.pagesize;
    headersize=((sizeof(cacheheader)+sizeof(cacheentry)*numpages+4095)/4096)*4096;
#if defined(MAP_ANONYMOUS)
    cachehead=(cacheheader *)mmap(NULL, fs_settings.cachesize+headersize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
#elif defined(MAP_ANON)
    cachehead=(cacheheader *)mmap(NULL, fs_settings.cachesize+headersize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
#else
    cachehead=(cacheheader *)malloc(fs_settings.cachesize+headersize);
    if (!cachehead){
        if (fs_settings.cachesize > MIN_CACHE_SIZE)
          fs_settings.cachesize/=2;
        else
          exit(-ENOMEM);
    }
    } while (!cachehead);
    debug("cache allocated size:%lu, page: %lu, pages: %lu\n",
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
  pthread_mutex_init(&writelock, NULL);
  pthread_mutex_init(&indexlock, NULL);
  pthread_mutex_init(&datamutex, NULL);
  pthread_cond_init(&datacond, NULL);

  pthread_mutexattr_init(&mattr);
  pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&treelock, &mattr);
  pthread_cond_init(&treecond, NULL);

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
  .utimens = fs_utimens
};

static int get_auth(const char* username, const char* pass)
{
  binresult *res, *sub, *au;
  static char localauth[64+8];
  debug("auth: %s, %s\n", username, pass);
  res=send_command(sock, "userinfo", P_STR("username", username), P_STR("password", pass), P_BOOL("getauth", 1));
  if (res){
    sub=find_res(res, "result");
    if (!sub || sub->type!=PARAM_NUM || sub->num!=0){
      debug("auth failed! - %u\n", (uint32_t)sub->num);
      free(res);
      return 1;
    }
    au=find_res(res, "auth");
    if (au){
      strncpy(localauth, au->str, 64+7);
      debug("got auth %s\n", localauth);
      auth = localauth;
      free(res);
      return 0;
    }
    free(res);
  }
  return 1;
}

int pfs_main(int argc, char **argv, const pfs_params* params){
  int r = 0;
  binresult *res, *subres;

  debug ("starting - argc: %d\n", argc);
  for (r = 0; r < argc; ++r)
    debug("\t %s \n", argv[r]);
  if (params->username && params->pass)
    debug("username %s, password %s\n", params->username, params->pass);
  if (params->auth)
    debug("auth %s\n", params->auth);
  if (params->cache_size)
    debug("cache size: %u B\n", params->cache_size);
  if (params->page_size)
    debug("cache page size: %u B\n", params->page_size);


  if (params->cache_size){
    if (params->cache_size >= MIN_CACHE_SIZE && params->cache_size <= MAX_CACHE_SIZE)
      fs_settings.cachesize = params->cache_size;
  }
  if (params->page_size){
    debug("set pagesize - not implemented!\n");
  }

  fs_settings.usessl = params->use_ssl;
  if (fs_settings.usessl){
    debug("use ssl is ON\n");
    sock=api_connect_ssl();
    diffsock=api_connect_ssl();
  }
  else{
    sock=api_connect();
    diffsock=api_connect();
  }
  if (!sock || !diffsock){
    fprintf(stderr, "Cannot connect to server\n");
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
#endif
  r = fuse_main(argc, argv, &fs_oper, NULL);
  return r;
}

#ifndef SERVICE
static int parse_pfs_param(int * i, int argc, char ** argv, pfs_params* params){
  if ((!strcmp(argv[*i], "-u") || !strcmp(argv[*i], "--user")) && *i+1 < argc) {
    ++*i;
    params->username = argv[*i];
    ++*i;
    return 1;
  }
  if ((!strcmp(argv[*i], "-p") || !strcmp(argv[*i], "--password")) && *i+1 < argc) {
    ++*i;
    params->pass = argv[*i];
    ++*i;
    return 1;
  }
  if ((!strcmp(argv[*i], "-a") || !strcmp(argv[*i], "--auth")) && *i+1 < argc){
    ++*i;
    params->auth = argv[*i];
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
  memset(&params, 0, sizeof(params));
  parsed_argc = parse_args(argc, argv, parsed_argv, &params);
  return pfs_main(parsed_argc, parsed_argv, &params);
}
#endif
