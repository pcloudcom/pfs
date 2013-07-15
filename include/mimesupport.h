#ifndef _MIMESUPPORT_H
#define _MIMESUPPORT_H

#include <stdint.h>
#include <stdlib.h>

typedef struct {
  const char *contenttype;
  const char *extensions;
  const char *icon;
  uint32_t id;
  uint8_t thumbsupport;
  uint8_t category;
  uint16_t flags;
} mimetype;

const mimetype *get_mime_by_id(uint32_t id) __attribute__ ((pure));
const mimetype *get_mime_by_name(const char *name, size_t namelen) __attribute__ ((pure));

#endif
