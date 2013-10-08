#include <string.h>
#include <stdlib.h>
#include "mimesupport.h"
#include "mimetypes.h"

#define MIME_CNT (sizeof(mimetypes)/sizeof(mimetype))
#define MIME_HASH_SIZE (MIME_CNT*2+51)

typedef struct _mimehashentry {
  struct _mimehashentry *next;
  const char *key;
  uint32_t keylen;
  uint32_t id;
} mimehashentry;

static mimehashentry *mimehash[MIME_HASH_SIZE];

static unsigned int hash_func(const char *key, uint32_t len){
  unsigned int i, hash = 0;
  
  for (i=0; i<len; i++)
    hash = key[i] + (hash << 6) + (hash << 16) - hash;
  
  return hash % MIME_HASH_SIZE;
}

static void hash_add(const char *key, uint32_t keylen, uint32_t id){
  mimehashentry *he;
  int h;
  h=hash_func(key, keylen);
  he=(mimehashentry *)malloc(sizeof(mimehashentry));
  he->next=mimehash[h];
  he->key=key;
  he->keylen=keylen;
  he->id=id;
  mimehash[h]=he;
}

__attribute__((constructor))
static void mime_init(){
  int unsigned i;
  const char *ch1, *ch2;
  for (i=0; i<MIME_HASH_SIZE; i++)
    mimehash[i]=NULL;
  for (i=0; i<MIME_CNT; i++){
    hash_add(mimetypes[i].contenttype, strlen(mimetypes[i].contenttype), i);
    if (mimetypes[i].extensions[0]){
      ch1=mimetypes[i].extensions;
      while ((ch2=index(ch1, ','))){
        hash_add(ch1, ch2-ch1, i);
        ch1=ch2+1;
      }
      hash_add(ch1, strlen(ch1), i);
    }
  }
}

__attribute__((destructor))
static void mime_free(){
  mimehashentry *he1, *he2;
  int unsigned i;
  for (i=0; i<MIME_HASH_SIZE; i++){
    he1=mimehash[i];
    while (he1){
      he2=he1->next;
      free(he1);
      he1=he2;
    }
  }
}

const mimetype *get_mime_by_id(uint32_t id){
  if (id<MIME_CNT)
    return &mimetypes[id];
  else
    return NULL;
}

const mimetype *get_mime_by_name(const char *name, size_t namelen){
  mimehashentry *he;
  int h;
  h=hash_func(name, namelen);
  he=mimehash[h];
  while (he){
    if (he->keylen==namelen && !memcmp(name, he->key, namelen))
      return &mimetypes[he->id];
    he=he->next;
  }
  return NULL;
}
