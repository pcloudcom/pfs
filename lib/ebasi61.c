#include <string.h>
#include "ebasi61.h"

static const char *ebasi61c="y7kXV05JFpHzRL48QYmbhSjufB2WsD1en9MTgPCxwKldqt6EOUIANa3Gvirco";
static const char *hexdigits="0123456789abcdef";
static const uint16_t *hexenc2;
static const uint8_t *ebasi61d;
static const uint8_t *hexd;
static const uint16_t *ebasi61d2;

int ebasi61_init(){
  uint8_t *dec;
  uint16_t *dec2;
  int unsigned i, j, off;
  dec=(uint8_t *)malloc(256);
  memset(dec, 0xff, 256);
  for (i=0; i<61; i++)
    dec[(unsigned char)ebasi61c[i]]=i;
  ebasi61d=dec;
  dec2=(uint16_t *)malloc(sizeof(uint16_t)*65536);
  for (i=0; i<256; i++)
    for (j=0; j<256; j++){
      off=i+j*256; /* i is the first byte */
      if (dec[i]+dec[j]<=120) /* both are valid characters*/
        dec2[off]=dec[i]+dec[j]*61;
      else if (dec[i]<=60)
        dec2[off]=dec[i];
      else
        dec2[off]=0;
    }
  ebasi61d2=dec2;
  dec=(uint8_t *)malloc(256);
  memset(dec, 0, 256);
  for (i='0'; i<='9'; i++)
    dec[i]=i-'0';
  for (i='a'; i<='z'; i++)
    dec[i]=i-'a'+10;
  for (i='A'; i<='Z'; i++)
    dec[i]=i-'A'+10;
  hexd=dec;
  dec2=(uint16_t *)malloc(sizeof(uint16_t)*256);
  for (i=0; i<16; i++)
    for (j=0; j<16; j++)
      dec2[i*16+j]=(uint16_t)hexdigits[i]+(uint16_t)hexdigits[j]*256;
  hexenc2=dec2;
  return 0;
}

#define EBASI_ENCODE(bits) void ebasi61_encode_uint##bits(uint##bits##_t n, char *dest, size_t *len){\
  size_t l=0;\
  while (n){\
    dest[l++]=ebasi61c[n%61];\
    n/=61;\
  }\
  *len=l;\
}

#define EBASI_ENCODE_FIX(bits) static void ebasi61_encode_uint##bits##_fixed(uint##bits##_t n, char *dest){\
  unsigned int i;\
  for (i=0; i<EBASI61_INT##bits##MAXLEN; i++){\
    *dest++=ebasi61c[n%61];\
    n/=61;\
  }\
}

EBASI_ENCODE(64);
EBASI_ENCODE(32);
EBASI_ENCODE(16);
EBASI_ENCODE(8);

EBASI_ENCODE_FIX(64);
EBASI_ENCODE_FIX(32);
EBASI_ENCODE_FIX(16);
EBASI_ENCODE_FIX(8);

void ebasi61_encode_binary(const void *data, size_t datalen, char *dest, size_t *len){
  char *idest;
  idest=dest;
  if (datalen&1)
    datalen++;
  while (datalen>=8){
    ebasi61_encode_uint64_fixed(*((uint64_t *)data), dest);
    dest+=EBASI61_INT64MAXLEN;
    datalen-=8;
    data+=8;
  }
  if (!datalen){
    *len=dest-idest;
    return;
  }
  if (datalen>=4){
    ebasi61_encode_uint32_fixed(*((uint32_t *)data), dest);
    dest+=EBASI61_INT32MAXLEN;
    datalen-=4;
    data+=4;
  }
  if (datalen>=2){
    ebasi61_encode_uint16_fixed(*((uint16_t *)data), dest);
    dest+=EBASI61_INT16MAXLEN;
    datalen-=2;
    data+=2;
  }
  *len=dest-idest;
}

void ebasi61_encode_hex(const char *data, size_t datalen, char *dest, size_t *len){
  uint64_t n;
  char *idest;
  uint32_t nn;
  int unsigned i;
  idest=dest;
  if (datalen&1)
    datalen++;
  while (datalen>=16){
    n=0;
    for (i=0; i<8; i++){
      n=n*256+hexd[(unsigned char)data[14-i*2]]*16+hexd[(unsigned char)data[15-i*2]];
    }
    ebasi61_encode_uint64_fixed(n, dest);
    dest+=EBASI61_INT64MAXLEN;
    datalen-=16;
    data+=16;
  }
  if (!datalen){
    *len=dest-idest;
    return;
  }
  if (datalen>=14){
    n=0;
    for (i=0; i<8; i++){
      n=n*256;
      if (14-i<datalen)
        n+=hexd[(unsigned char)data[14-i]]*16;
      if (15-i<datalen)
        n+=hexd[(unsigned char)data[15-i]];
    }
    ebasi61_encode_uint64_fixed(n, dest);
    dest+=EBASI61_INT64MAXLEN;
    *len=dest-idest;
    return;
  }
  if (datalen>=8){
    nn=0;
    for (i=0; i<4; i++){
      nn=nn*256+hexd[(unsigned char)data[6-i*2]]*16+hexd[(unsigned char)data[7-i*2]];
    }
    ebasi61_encode_uint32_fixed(nn, dest);
    dest+=EBASI61_INT32MAXLEN;
    datalen-=8;
    data+=8;
  }
  if (datalen>=4){
    nn=0;
    for (i=0; i<2; i++)
      nn=nn*256+hexd[(unsigned char)data[2-i*2]]*16+hexd[(unsigned char)data[3-i*2]];
    ebasi61_encode_uint16_fixed(nn, dest);
    dest+=EBASI61_INT16MAXLEN;
    datalen-=4;
    data+=4;
  }
  if (datalen==2){
    nn=hexd[(unsigned char)data[0]]*16+hexd[(unsigned char)data[1]];
    ebasi61_encode_uint8_fixed(nn, dest);
    dest+=EBASI61_INT8MAXLEN;
  }
  *len=dest-idest;
}

#define EBASI_DECODE(bits) uint##bits##_t ebasi61_decode_uint##bits(const char *str, size_t len){\
  uint##bits##_t ret, mul;\
  size_t i, lm;\
  ret=0;\
  mul=1;\
  if (len&1)\
    lm=len-1;\
  else\
    lm=len;\
  for (i=0; i<lm; i+=2){\
    ret+=mul*ebasi61d2[*((uint16_t *)(str+i))];\
    mul*=61*61;\
  }\
  if (len&1)\
    ret+=mul*ebasi61d[*((uint8_t *)(str+i))];\
  return ret;\
}

#define EBASI_DECODE_FIX(bits) static uint##bits##_t ebasi61_decode_uint##bits##_fixed(const char *str){\
  uint##bits##_t ret, mul;\
  size_t i;\
  ret=0;\
  mul=1;\
  for (i=0; i<(EBASI61_INT##bits##MAXLEN&1?EBASI61_INT##bits##MAXLEN-1:EBASI61_INT##bits##MAXLEN); i+=2){\
    ret+=mul*ebasi61d2[*((uint16_t *)(str+i))];\
    mul*=61*61;\
  }\
  if (EBASI61_INT##bits##MAXLEN&1)\
    ret+=mul*ebasi61d[*((uint8_t *)(str+i))];\
  return ret;\
}

EBASI_DECODE(64);
EBASI_DECODE(32);
EBASI_DECODE(16);
EBASI_DECODE(8);

EBASI_DECODE_FIX(64);
EBASI_DECODE_FIX(32);
EBASI_DECODE_FIX(16);
EBASI_DECODE_FIX(8);

void ebasi61_decode_binary(const char *data, size_t datalen, void *dest, size_t *len){
  void *idest;
  idest=dest;
  while (datalen>=EBASI61_INT64MAXLEN){
    *((uint64_t *)dest)=ebasi61_decode_uint64_fixed(data);
    dest+=8;
    datalen-=EBASI61_INT64MAXLEN;
    data+=EBASI61_INT64MAXLEN;
  }
  if (!datalen){
    *len=dest-idest;
    return;
  }
  if (datalen>=EBASI61_INT32MAXLEN){
    *((uint32_t *)dest)=ebasi61_decode_uint32_fixed(data);
    dest+=4;
    datalen-=EBASI61_INT32MAXLEN;
    data+=EBASI61_INT32MAXLEN;
  }
  if (datalen>=EBASI61_INT16MAXLEN){
    *((uint16_t *)dest)=ebasi61_decode_uint16_fixed(data);
    dest+=2;
    datalen-=EBASI61_INT16MAXLEN;
    data+=EBASI61_INT16MAXLEN;
  }
  if (datalen>=EBASI61_INT8MAXLEN){
    *((uint8_t *)dest)=ebasi61_decode_uint8_fixed(data);
    dest++;
  }
  *len=dest-idest;
}

void ebasi61_decode_hex(const char *data, size_t datalen, char *dest, size_t *len){
  uint64_t n;
  char *idest;
  uint32_t nn;
  int unsigned i;
  idest=dest;
  while (datalen>=EBASI61_INT64MAXLEN){
    n=ebasi61_decode_uint64_fixed(data);
    for (i=0; i<8; i++){
      *((uint16_t *)&dest[i*2])=hexenc2[n%256];
      n/=256;
    }
    dest+=16;
    datalen-=EBASI61_INT64MAXLEN;
    data+=EBASI61_INT64MAXLEN;
  }
  if (!datalen){
    *len=dest-idest;
    return;
  }
  if (datalen>=EBASI61_INT32MAXLEN){
    nn=ebasi61_decode_uint32_fixed(data);
    for (i=0; i<4; i++){
      *((uint16_t *)&dest[i*2])=hexenc2[nn%256];
      nn/=256;
    }
    dest+=8;
    datalen-=EBASI61_INT32MAXLEN;
    data+=EBASI61_INT32MAXLEN;
  }
  if (datalen>=EBASI61_INT16MAXLEN){
    nn=ebasi61_decode_uint16_fixed(data);
    for (i=0; i<2; i++){
      *((uint16_t *)&dest[i*2])=hexenc2[nn%256];
      nn/=256;
    }
    dest+=4;
    datalen-=EBASI61_INT16MAXLEN;
    data+=EBASI61_INT16MAXLEN;
  }
  if (datalen>=EBASI61_INT8MAXLEN){
    nn=ebasi61_decode_uint8_fixed(data);
    *((uint16_t *)dest)=hexenc2[nn];
    dest+=2;
    datalen-=EBASI61_INT8MAXLEN;
    data+=EBASI61_INT8MAXLEN;
  }
  *len=dest-idest;
}