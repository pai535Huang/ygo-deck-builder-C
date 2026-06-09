#ifndef PIXBUF_UTILS_H
#define PIXBUF_UTILS_H

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

GdkTexture* pixbuf_utils_texture_from_pixbuf(GdkPixbuf *pixbuf);
gboolean pixbuf_utils_paint_to_cairo(cairo_t *cr,
                                     const GdkPixbuf *pixbuf,
                                     double x,
                                     double y,
                                     cairo_filter_t filter);

#endif // PIXBUF_UTILS_H
