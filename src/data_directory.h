#ifndef DATA_DIRECTORY_H
#define DATA_DIRECTORY_H

#include <glib.h>

gboolean data_directory_remove(const char *path);
gboolean data_directory_replace(const char *path, const char *replacement,
                                const char *backup, GError **error);

typedef gboolean (*DataArchiveFilter)(const char *name);
gboolean data_directory_extract_zip(const char *zip, const char *directory,
                                    DataArchiveFilter filter, GError **error);

#endif
