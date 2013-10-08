#ifndef PHS_H_INCLUDED
#define PHS_H_INCLUDED

#define REGISTRY_KEY_PCLOUD    "SOFTWARE\\PCloud\\pCloud"

#define D_NONE     0
#define D_BUG      10
#define D_CRITICAL 20
#define D_ERROR    30
#define D_WARNING  40
#define D_NOTICE   50

#define DEBUG_LEVELS {\
 {D_BUG, "BUG"},\
 {D_CRITICAL, "CRITICAL ERROR"},\
 {D_ERROR, "ERROR"},\
 {D_WARNING, "WARNING"},\
 {D_NOTICE, "NOTICE"}\
}

#ifndef DEBUG_LEVEL
#define DEBUG_LEVEL D_WARNING
#endif

#define DEBUG_FILE "/tmp/pfs.log"

#define debug(level, ...) do {if (level<=DEBUG_LEVEL) do_debug(__FILE__, __FUNCTION__, __LINE__, level, __VA_ARGS__);} while (0)

void do_debug(const char *file, const char *function, int unsigned line, int unsigned level, const char *fmt, ...);

typedef struct
{
    const char * auth;
    const char * username;
    const char * pass;
    int use_ssl;
    size_t cache_size;
    size_t page_size;
} pfs_params;

#endif // PHS_H_INCLUDED
