/* GdkPixbuf library - WebP Image Loader
 *
 * Copyright (C) 2011 Alberto Ruiz
 * Copyright (C) 2011 David Mazary
 *
 * Authors: Alberto Ruiz <aruiz@gnome.org>
 *          David Mazary <dmaz@vt.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <webp/decode.h>
#include <string.h>

#define GDK_PIXBUF_ENABLE_BACKEND
#include <gdk-pixbuf/gdk-pixbuf.h>
#undef  GDK_PIXBUF_ENABLE_BACKEND

/* Progressive loader context */
typedef struct {
        GdkPixbufModuleSizeFunc size_func;
        GdkPixbufModuleUpdatedFunc update_func;
        GdkPixbufModulePreparedFunc prepare_func;
        gpointer user_data;
        GdkPixbuf *pixbuf;
        gboolean got_header;
        WebPIDecoder *idec;
        guchar *decbuf;
        gint last_y;
        GError **error;
} WebPContext;

static void
destroy_data (guchar *pixels, gpointer data)
{
  g_free (pixels);
}

/* Shared library entry point */
static GdkPixbuf *
gdk_pixbuf__webp_image_load (FILE *f, GError **error)
{
        GdkPixbuf * volatile pixbuf = NULL;
        guint32 data_size;
        guint8 *out;
        gint w, h, ok;
        gpointer data;

        /* Get data size */
        fseek (f, 0, SEEK_END);
        data_size = ftell(f);
        fseek (f, 0, SEEK_SET);

        /* Get data */
        data = g_malloc (data_size);
        ok = (fread (data, data_size, 1, f) == 1);
        if (!ok)
        {
                /*TODO: Return GError*/
                g_free (data);
                return;
        }
        out = WebPDecodeRGB (data, data_size, &w, &h);
        g_free (data);

        if (!out)
        {
                /*TODO: Return GError*/
                return;
        }

        /*FIXME: libwebp does not support alpha channel detection, once supported
        * we need to make the alpha channel conditional to save memory if possible */
        pixbuf = gdk_pixbuf_new_from_data ((const guchar *)out,
                                           GDK_COLORSPACE_RGB,
                                           FALSE ,
                                           8,
                                           w, h,
                                           3 * w,
                                           destroy_data,
                                           NULL);

        if (!pixbuf) {
                /*TODO: Return GError?*/
                return;
        }
        return pixbuf;
}

static gpointer
gdk_pixbuf__webp_image_begin_load (GdkPixbufModuleSizeFunc size_func,
                                   GdkPixbufModulePreparedFunc prepare_func,
                                   GdkPixbufModuleUpdatedFunc update_func,
                                   gpointer user_data,
                                   GError **error)
{
        WebPContext *context = g_new0 (WebPContext, 1);
        context->size_func = size_func;
        context->prepare_func = prepare_func;
        context->update_func  = update_func;
        context->user_data = user_data;
        return context;
}

static gboolean
gdk_pixbuf__webp_image_stop_load (gpointer context, GError **error)
{
        WebPContext *data = (WebPContext *) context;
        g_return_val_if_fail(data != NULL, TRUE);
        if (data->pixbuf) {
                g_object_unref (data->pixbuf);
        }
        if (data->idec) {
                WebPIDelete (data->idec);
        }
        if (data->decbuf) {
                g_free (data->decbuf);
        }
        return TRUE;
}

static gboolean
gdk_pixbuf__webp_image_load_increment (gpointer context,
                                       const guchar *buf, guint size,
                                       GError **error)
{
        gint width, height, stride, i, j;
        guchar *dptr;
        WebPContext *data = (WebPContext *) context;
        g_return_val_if_fail(data != NULL, FALSE);

        if (!data->got_header) {
                gint rc;
                rc = WebPGetInfo (buf, size, &width, &height);
                if (rc == 0) {
                        g_set_error (error,
                                     GDK_PIXBUF_ERROR,
                                     GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                     "Cannot read WebP image header.");
                        return FALSE;
                }
                data->got_header = TRUE;
                if (data->size_func) {
                        (* data->size_func) (&width, &height,
                                             data->user_data);
                }
                data->pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB,
                                               FALSE,
                                               8,
                                               width,
                                               height);
                data->decbuf = g_try_malloc (width * height * 3);
                if (!data->decbuf) {
                        g_set_error (error,
                                     GDK_PIXBUF_ERROR,
                                     GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
                                     "Cannot allocate memory for decoded image data.");
                        return FALSE;
                }
                data->idec = WebPINewRGB (MODE_RGB,
                                          data->decbuf,
                                          width * height * 3,
                                          width * 3);
                if (!data->idec) {
                        g_set_error (error,
                                     GDK_PIXBUF_ERROR,
                                     GDK_PIXBUF_ERROR_FAILED,
                                     "Cannot create WebP decoder.");
                        return FALSE;
                }
                if (data->prepare_func) {
                        (* data->prepare_func) (data->pixbuf,
                                                NULL,
                                                data->user_data);
                }
        }
        const VP8StatusCode status = WebPIAppend (data->idec, buf, size);
        if (status != VP8_STATUS_SUSPENDED && status != VP8_STATUS_OK) {
                g_set_error (error,
                             GDK_PIXBUF_ERROR,
                             GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                             "WebP decoder failed with status code %d.",
                             status);
                return FALSE;
        }
        guint8 *dec_output;
        dec_output = WebPIDecGetRGB (data->idec, &data->last_y, &width, &height, &stride);
        if (dec_output == NULL && status != VP8_STATUS_SUSPENDED) {
                g_set_error(error,
                            GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_BAD_OPTION,
                            "Bad inputs to WebP decoder.");
                return FALSE;
        }
        dptr = gdk_pixbuf_get_pixels (data->pixbuf);
        guint8 *row;
        for (i = 0; i < data->last_y; ++i) {
                row = dec_output + (i * stride);
                for (j = 0; j < width/2; j += 3) {
                        dptr[i * stride + j + 0] = row[j + 0];
                        dptr[i * stride + j + 1] = row[j + 1];
                        dptr[i * stride + j + 2] = row[j + 2];
                }
        }
        if (data->update_func) {
                (* data->update_func) (data->pixbuf, 0, 0,
                                       width,
                                       data->last_y,
                                       data->user_data);
        }
        return TRUE;
}

void
fill_vtable (GdkPixbufModule *module)
{
        module->load = gdk_pixbuf__webp_image_load;
        module->begin_load = gdk_pixbuf__webp_image_begin_load;
        module->stop_load = gdk_pixbuf__webp_image_stop_load;
        module->load_increment = gdk_pixbuf__webp_image_load_increment;
}

void
fill_info (GdkPixbufFormat *info)
{
        /* WebP TODO: figure out how to represent the full pattern */
        static GdkPixbufModulePattern signature[] = {
                { "RIFF", NULL, 100 },
                { NULL, NULL, 0 }
        };

        static gchar *mime_types[] = {
                "image/webp",
                NULL
        };

        static gchar *extensions[] = {
                "webp",
                NULL
        };

        info->name        = "webp";
        info->signature   = signature;
        info->description = "The WebP image format";
        info->mime_types  = mime_types;
        info->extensions  = extensions;
        info->flags       = GDK_PIXBUF_FORMAT_THREADSAFE;
        info->license     = "LGPL";
}
