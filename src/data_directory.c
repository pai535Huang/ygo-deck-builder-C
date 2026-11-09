#define _GNU_SOURCE

#include "data_directory.h"
#include "archive_path.h"

#include <archive.h>
#include <archive_entry.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <unistd.h>

static gboolean remove_at(int parent, const char *name) {
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return TRUE;
        if (errno == ENOTDIR || errno == ELOOP)
            return unlinkat(parent, name, 0) == 0 || errno == ENOENT;
        return FALSE;
    }

    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return FALSE;
    }

    gboolean ok = TRUE;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno) ok = FALSE;
            break;
        }
        if (g_str_equal(entry->d_name, ".") || g_str_equal(entry->d_name, ".."))
            continue;
        if (!remove_at(fd, entry->d_name)) ok = FALSE;
    }
    closedir(dir);
    return ok && (unlinkat(parent, name, AT_REMOVEDIR) == 0 || errno == ENOENT);
}

gboolean data_directory_remove(const char *path) {
    return path && remove_at(AT_FDCWD, path);
}

gboolean data_directory_replace(const char *path, const char *replacement,
                                const char *backup, GError **error) {
    if (renameat2(AT_FDCWD, replacement, AT_FDCWD, path, RENAME_EXCHANGE) == 0)
        return TRUE;
    if (errno == ENOENT) {
        if (g_rename(replacement, path) == 0) return TRUE;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot install %s: %s", path, g_strerror(errno));
        return FALSE;
    }
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot replace %s: %s", path, g_strerror(errno));
        return FALSE;
    }

    // Some filesystems do not support exchange. Preserve the previous directory
    // in the staging area until the replacement has been installed.
    gboolean backed_up = g_rename(path, backup) == 0;
    if (!backed_up && errno != ENOENT) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot back up %s: %s", path, g_strerror(errno));
        return FALSE;
    }
    if (g_rename(replacement, path) == 0) return TRUE;

    int saved_errno = errno;
    if (backed_up && g_rename(backup, path) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot restore %s. Previous data is at %s", path, backup);
        return FALSE;
    }
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                "Cannot install %s: %s", path, g_strerror(saved_errno));
    return FALSE;
}

gboolean data_directory_extract_zip(const char *zip, const char *directory,
                                    DataArchiveFilter filter, GError **error) {
    struct archive *archive = archive_read_new();
    archive_read_support_format_zip(archive);
    int status = archive_read_open_filename(archive, zip, 10240);
    if (status == ARCHIVE_OK) {
        struct archive_entry *entry;
        while ((status = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
            const char *name = archive_entry_pathname(entry);
            mode_t type = archive_entry_filetype(entry);
            if (!archive_entry_path_is_safe(name) ||
                (type != AE_IFREG && type != AE_IFDIR) ||
                archive_entry_hardlink(entry) || archive_entry_symlink(entry) ||
                (filter && !filter(name))) {
                status = archive_read_data_skip(archive);
            } else {
                g_autofree char *path = g_build_filename(directory, name, NULL);
                archive_entry_set_pathname(entry, path);
                status = archive_read_extract(archive, entry,
                    ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_SYMLINKS);
            }
            if (status != ARCHIVE_OK) break;
        }
    }
    if (status != ARCHIVE_EOF) {
        const char *reason = archive_error_string(archive);
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Cannot extract %s: %s", zip, reason ? reason : "invalid archive");
    }
    archive_read_free(archive);
    return status == ARCHIVE_EOF;
}
