#ifndef IMAGE_LOADER_H
#define IMAGE_LOADER_H

#include <gtk/gtk.h>
#include <libsoup/soup.h>

void init_image_cache(void);
void cleanup_image_cache(void);

GdkPixbuf *load_from_disk_cache(int card_id);

void load_card_picture_async(SoupSession *session, GtkWidget *target, GtkStack *stack,
                             int img_id, gboolean thumbnail, gboolean local_only,
                             const char *local_path);

#endif
