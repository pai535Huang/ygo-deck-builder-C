#include "pixbuf_utils.h"

#include <stdint.h>

GdkTexture* pixbuf_utils_texture_from_pixbuf(GdkPixbuf *pixbuf) {
    if (!pixbuf) return NULL;

    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int channels = gdk_pixbuf_get_n_channels(pixbuf);
    int bits = gdk_pixbuf_get_bits_per_sample(pixbuf);
    int stride = gdk_pixbuf_get_rowstride(pixbuf);
    if (width <= 0 || height <= 0 || bits != 8 || (channels != 3 && channels != 4)) {
        return NULL;
    }

    GBytes *bytes = gdk_pixbuf_read_pixel_bytes(pixbuf);
    if (!bytes) return NULL;

    GdkMemoryFormat format = (channels == 4) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8;
    GdkTexture *texture = gdk_memory_texture_new(width, height, format, bytes, stride);
    g_bytes_unref(bytes);
    return texture;
}

gboolean pixbuf_utils_paint_to_cairo(cairo_t *cr,
                                     const GdkPixbuf *pixbuf,
                                     double x,
                                     double y,
                                     cairo_filter_t filter) {
    if (!cr || !pixbuf) return FALSE;

    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int channels = gdk_pixbuf_get_n_channels(pixbuf);
    int bits = gdk_pixbuf_get_bits_per_sample(pixbuf);
    int src_stride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar *src_pixels = gdk_pixbuf_read_pixels(pixbuf);
    if (width <= 0 || height <= 0 || bits != 8 ||
        (channels != 3 && channels != 4) || !src_pixels) {
        return FALSE;
    }

    int dst_stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    if (dst_stride <= 0) return FALSE;

    guchar *dst_pixels = g_malloc0((gsize)dst_stride * (gsize)height);
    for (int row = 0; row < height; row++) {
        const guchar *src = src_pixels + (gsize)row * (gsize)src_stride;
        uint32_t *dst = (uint32_t *)(dst_pixels + (gsize)row * (gsize)dst_stride);

        for (int col = 0; col < width; col++) {
            guint r = src[col * channels];
            guint g = src[col * channels + 1];
            guint b = src[col * channels + 2];
            guint a = (channels == 4) ? src[col * channels + 3] : 255;

            if (a != 255) {
                r = (r * a + 127) / 255;
                g = (g * a + 127) / 255;
                b = (b * a + 127) / 255;
            }

            dst[col] = ((uint32_t)a << 24) |
                       ((uint32_t)r << 16) |
                       ((uint32_t)g << 8) |
                       (uint32_t)b;
        }
    }

    cairo_surface_t *surface = cairo_image_surface_create_for_data(dst_pixels,
                                                                   CAIRO_FORMAT_ARGB32,
                                                                   width,
                                                                   height,
                                                                   dst_stride);
    cairo_status_t status = cairo_surface_status(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        g_free(dst_pixels);
        return FALSE;
    }

    cairo_save(cr);
    cairo_set_source_surface(cr, surface, x, y);
    cairo_pattern_t *pattern = cairo_get_source(cr);
    if (pattern) cairo_pattern_set_filter(pattern, filter);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_surface_destroy(surface);
    g_free(dst_pixels);
    return TRUE;
}
