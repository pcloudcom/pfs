#ifndef _PFS_SETTINGS_H
#define _PFS_SETTINGS_H

#include "common.h"

#define SETTINGS_PATH "/.pfs_settings"

const char **list_settings();
const struct stat *get_setting_stat(const char *name);
int set_setting(const char *name, const char *val, size_t vallen);
int get_setting(const char *name, char *val, size_t *vallen);

#endif
