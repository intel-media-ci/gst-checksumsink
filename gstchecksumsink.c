/* GStreamer
 * Copyright (C) 2010 David Schleef <ds@schleef.org>
 * Copyright (C) 2015 Sreerenj Balachandran <sreerenj.balachandran@intel.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "gstchecksumsink.h"

static gboolean gst_cksum_image_sink_start (GstBaseSink * sink);
static gboolean gst_cksum_image_sink_stop (GstBaseSink * sink);
static gboolean gst_cksum_image_sink_set_caps (GstBaseSink * base_sink,
    GstCaps * caps);
static gboolean gst_cksum_image_sink_propose_allocation (GstBaseSink *
    base_sink, GstQuery * query);
static GstFlowReturn gst_cksum_image_sink_render (GstBaseSink * sink,
    GstBuffer * buffer);
static void gst_cksum_image_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_cksum_image_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

enum
{
  PROP_0,
  PROP_HASH,
  PROP_FILE_CHECKSUM,
  PROP_FRAME_CHECKSUM,
  PROP_PLANE_CHECKSUM,
  PROP_RAW_OUTPUT,
  PROP_RAW_LOCATION,
  PROP_EOS_AFTER
};

static GstStaticPadTemplate gst_cksum_image_sink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE
        ("{NV12, I420, YV12, AYUV, YUY2, ARGB, BGRA, BGRx, RGBA, Y42B, Y444, Y210, Y410, "
         "P010_10LE, I422_10LE, Y444_10LE, GRAY8, VUYA, UYVY, P012_LE, Y212_LE, "
         "Y412_LE, I420_10LE }")));

/* class initialization */

#define GST_TYPE_CKSUM_IMAGE_SINK_HASH (gst_cksum_image_sink_hash_get_type ())
static GType
gst_cksum_image_sink_hash_get_type (void)
{
  static GType gtype = 0;

  if (gtype == 0) {
    static const GEnumValue values[] = {
      {G_CHECKSUM_MD5, "MD5", "md5"},
      {G_CHECKSUM_SHA1, "SHA-1", "sha1"},
      {G_CHECKSUM_SHA256, "SHA-256", "sha256"},
      {G_CHECKSUM_SHA512, "SHA-512", "sha512"},
      {0, NULL, NULL},
    };

    gtype = g_enum_register_static ("GstCksumImageSinkHash", values);
  }
  return gtype;
}

#define CAT_PERFORMANCE _get_perf_category()
static inline GstDebugCategory *
_get_perf_category (void)
{
  static GstDebugCategory *cat = NULL;

  if (g_once_init_enter (&cat)) {
    GstDebugCategory *c;

    GST_DEBUG_CATEGORY_GET (c, "GST_PERFORMANCE");
    g_once_init_leave (&cat, c);
  }
  return cat;
}

GST_DEBUG_CATEGORY_STATIC (gst_cksum_image_sink_debug);
#define GST_CAT_DEFAULT gst_cksum_image_sink_debug

#define gst_cksum_image_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstCksumImageSink, gst_cksum_image_sink,
    GST_TYPE_VIDEO_SINK, GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT,
        "checksumsink2", 0, "checksum image sink"));

static void
gst_cksum_image_sink_class_init (GstCksumImageSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *base_sink_class = GST_BASE_SINK_CLASS (klass);

  gobject_class->set_property = gst_cksum_image_sink_set_property;
  gobject_class->get_property = gst_cksum_image_sink_get_property;
  base_sink_class->start = GST_DEBUG_FUNCPTR (gst_cksum_image_sink_start);
  base_sink_class->stop = GST_DEBUG_FUNCPTR (gst_cksum_image_sink_stop);
  base_sink_class->set_caps = gst_cksum_image_sink_set_caps;
  base_sink_class->propose_allocation = gst_cksum_image_sink_propose_allocation;
  base_sink_class->render = gst_cksum_image_sink_render;

  g_object_class_install_property (gobject_class, PROP_HASH,
      g_param_spec_enum ("hash", "Hash", "Checksum type",
          GST_TYPE_CKSUM_IMAGE_SINK_HASH, G_CHECKSUM_MD5,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_FILE_CHECKSUM,
      g_param_spec_boolean ("file-checksum", "File checksum",
          "calculate checksum for the whole raw data file (MD5 only)",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_FRAME_CHECKSUM,
      g_param_spec_boolean ("frame-checksum", "Frame checksum",
          "calculate checksum per frame", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PLANE_CHECKSUM,
      g_param_spec_boolean ("plane-checksum", "Plane checksum",
          "calculate checksum per plane", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_RAW_OUTPUT,
      g_param_spec_boolean ("dump-output", "Dump output",
          "save decoded raw frames into file (YV12 and I420 only)",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_RAW_LOCATION,
      g_param_spec_string ("dump-location", "File Location",
          "Location of the file to write decoded raw frames",
          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_EOS_AFTER,
      g_param_spec_int ("eos-after", "EOS After", "EOS after N buffers",
          -1, G_MAXINT, -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_cksum_image_sink_sink_template));

  gst_element_class_set_static_metadata (element_class, "Checksum Image sink",
      "Debug/Sink", "Calculates a checksum for video frames",
      "David Schleef <ds@schleef.org>, "
      "Sreerenj Balachandran <sreerenj.balachandran@intel.com>");
}

static void
gst_cksum_image_sink_init (GstCksumImageSink * checksumsink)
{
  gst_base_sink_set_sync (GST_BASE_SINK (checksumsink), FALSE);
  checksumsink->hash = G_CHECKSUM_MD5;
  checksumsink->frame_checksum = TRUE;
  checksumsink->fd = -1;
  checksumsink->eos_after = -1;
}

static void
gst_cksum_image_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstCksumImageSink *checksumsink = GST_CKSUM_IMAGE_SINK (object);

  switch (prop_id) {
    case PROP_HASH:
      checksumsink->hash = g_value_get_enum (value);
      break;
    case PROP_FILE_CHECKSUM:
      checksumsink->file_checksum = g_value_get_boolean (value);
      break;
    case PROP_FRAME_CHECKSUM:
      checksumsink->frame_checksum = g_value_get_boolean (value);
      break;
    case PROP_PLANE_CHECKSUM:
      checksumsink->plane_checksum = g_value_get_boolean (value);
      break;
    case PROP_RAW_OUTPUT:
      checksumsink->dump_output = g_value_get_boolean (value);
      break;
    case PROP_RAW_LOCATION:
      checksumsink->raw_file_name = g_strdup (g_value_get_string (value));
      break;
    case PROP_EOS_AFTER:
      checksumsink->eos_after = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_cksum_image_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstCksumImageSink *checksumsink = GST_CKSUM_IMAGE_SINK (object);

  switch (prop_id) {
    case PROP_HASH:
      g_value_set_enum (value, checksumsink->hash);
      break;
    case PROP_FILE_CHECKSUM:
      g_value_set_boolean (value, checksumsink->file_checksum);
      break;
    case PROP_FRAME_CHECKSUM:
      g_value_set_boolean (value, checksumsink->frame_checksum);
      break;
    case PROP_PLANE_CHECKSUM:
      g_value_set_boolean (value, checksumsink->plane_checksum);
      break;
    case PROP_RAW_OUTPUT:
      g_value_set_boolean (value, checksumsink->dump_output);
      break;
    case PROP_RAW_LOCATION:
      g_value_set_string (value, checksumsink->raw_file_name);
      break;
    case PROP_EOS_AFTER:
      g_value_set_int (value, checksumsink->eos_after);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
open_raw_file (GstCksumImageSink * checksumsink)
{
  GError *err;

  if (!checksumsink->file_checksum && !checksumsink->dump_output)
    return TRUE;

  if (checksumsink->fd != -1)
    return TRUE;
  else if (checksumsink->raw_file_name)
    checksumsink->fd = g_open (checksumsink->raw_file_name, O_WRONLY | O_CREAT,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
  else
    checksumsink->fd =
        g_file_open_tmp ("tmp_XXXXXX.yuv", &checksumsink->raw_file_name, &err);

  if (checksumsink->fd == -1) {
    GST_ELEMENT_ERROR (checksumsink, RESOURCE, OPEN_WRITE,
        ("failed to create output file"),
        ("reason: %s", err ? err->message : ""));
    g_error_free (err);
    return FALSE;
  }

  GST_INFO_OBJECT (checksumsink, "raw file name: %s",
      checksumsink->raw_file_name);
  return TRUE;
}

static gboolean
gst_cksum_image_sink_start (GstBaseSink * sink)
{
  GstCksumImageSink *checksumsink = GST_CKSUM_IMAGE_SINK (sink);

  if (!open_raw_file (checksumsink))
    return FALSE;

  return TRUE;
}

static gboolean
checksum_raw_file (GstCksumImageSink * checksumsink)
{
  GChecksum *csum;
  FILE *fd;
  guchar data[64 * BUFSIZ + 1];
  size_t len;
  gboolean ret;

  if (!checksumsink->file_checksum)
    return TRUE;
  if (!checksumsink->raw_file_name) {
    GST_WARNING_OBJECT (checksumsink, "unspecified raw file");
    return FALSE;
  }

  ret = FALSE;
  fd = fopen (checksumsink->raw_file_name, "r");
  if (!fd) {
    GST_WARNING_OBJECT (checksumsink, "failed to open %s: %s",
        checksumsink->raw_file_name, strerror (errno));
    goto remove_file;
  }

  csum = g_checksum_new (G_CHECKSUM_MD5);

  while ((len = fread (data, 1, 64 * BUFSIZ, fd)) > 0)
    g_checksum_update (csum, data, len);

  g_print ("%s\n", g_checksum_get_string (csum));

  g_checksum_free (csum);
  fclose (fd);

  ret = TRUE;

remove_file:
  /* don't remove if we expect to keep the raw output */
  if (!checksumsink->dump_output && g_unlink (checksumsink->raw_file_name) != 0) {
    GST_WARNING_OBJECT (checksumsink, "failed to remove %s: %s",
        checksumsink->raw_file_name, strerror (errno));
  }

  return ret;
}

static gboolean
gst_cksum_image_sink_stop (GstBaseSink * sink)
{
  GstCksumImageSink *checksumsink = GST_CKSUM_IMAGE_SINK (sink);

  if (checksumsink->fd != -1) {
    fsync (checksumsink->fd);
    close (checksumsink->fd);
  }

  checksum_raw_file (checksumsink);

  g_clear_pointer (&checksumsink->raw_file_name, g_free);

  g_clear_pointer (&checksumsink->data, g_free);
  checksumsink->data_size = 0;

  return TRUE;
}

static gboolean
gst_cksum_image_sink_set_caps (GstBaseSink * base_sink, GstCaps * caps)
{
  GstCksumImageSink *checksumsink = GST_CKSUM_IMAGE_SINK (base_sink);
  GstVideoInfo vinfo;

  if (!caps)
    return FALSE;
  if (!gst_video_info_from_caps (&vinfo, caps))
    return FALSE;

  checksumsink->vinfo = vinfo;

  return TRUE;
}

static gboolean
gst_cksum_image_sink_propose_allocation (GstBaseSink * base_sink,
    GstQuery * query)
{
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  return TRUE;
}

static guint8 *
alloc_data (GstCksumImageSink * checksumsink, gsize size)
{
  if (checksumsink->data && checksumsink->data_size == size)
    return checksumsink->data;

  if (checksumsink->data && checksumsink->data_size != size) {
    checksumsink->data = g_realloc (checksumsink->data, size);
    checksumsink->data_size = size;
    return checksumsink->data;
  }

  if (!checksumsink->data && size > 0) {
    checksumsink->data = g_malloc (size);
    checksumsink->data_size = size;
    return checksumsink->data;
  }

  return NULL;
}

static GstFlowReturn
gst_cksum_image_sink_render (GstBaseSink * sink, GstBuffer * buffer)
{
  GstCksumImageSink *checksumsink = GST_CKSUM_IMAGE_SINK (sink);
  gchar *csum;
  GstVideoFrame frame;
  GstVideoInfo *vinfo;
  guint8 *data;
  gsize size;

  if (checksumsink->eos_after == 0) {
    GST_DEBUG_OBJECT (checksumsink, "Force EOS as requested");
    return GST_FLOW_EOS;
  } else if (checksumsink->eos_after > 0) {
    checksumsink->eos_after--;
  }

  vinfo = &checksumsink->vinfo;
  if (!gst_video_frame_map (&frame, vinfo, buffer, GST_MAP_READ)) {
    GST_ERROR_OBJECT (checksumsink, "failed to map frame");
    return GST_FLOW_ERROR;
  }

  if (!(data = alloc_data (checksumsink, GST_VIDEO_FRAME_SIZE (&frame)))) {
    GST_ERROR_OBJECT (checksumsink, "failed to allocate buffer");
    return GST_FLOW_ERROR;
  }

  {
    guint j, n_planes, plane;
    guint8 *dp;

    dp = data;
    size = 0;
    n_planes = GST_VIDEO_FRAME_N_PLANES (&frame);

    for (plane = 0; plane < n_planes; plane++) {
      gpointer pd = GST_VIDEO_FRAME_PLANE_DATA (&frame, plane);

      /* FIXME: assumes subsampling of component N is the same as
       * plane N, which is currently true for all formats we have but
       * it might not be in the future. */
      gint w = GST_VIDEO_FRAME_COMP_WIDTH (&frame, plane)
          * GST_VIDEO_FRAME_COMP_PSTRIDE (&frame, plane);
      /* FIXME: workaround for complex formats like v210, UYVP and
       * IYU1 that have pstride == 0 */
      if (w == 0)
        w = GST_VIDEO_FRAME_PLANE_STRIDE (&frame, plane);
      gint h = GST_VIDEO_FRAME_COMP_HEIGHT (&frame, plane);
      gint ps = GST_VIDEO_FRAME_PLANE_STRIDE (&frame, plane);

      /* current plane size and data required for plane_checksum */
      guint8 *cdp = dp;
      gsize psz = 0;

      GST_CAT_DEBUG_OBJECT (CAT_PERFORMANCE, checksumsink,
          "copy plane %d, w:%d h:%d ", plane, w, h);
      for (j = 0; j < h; j++) {
        memcpy (dp, pd, w);
        dp += w;
        pd += ps;
        size += w;
        psz += w;
      }

      if (checksumsink->plane_checksum) {
        csum = g_compute_checksum_for_data (checksumsink->hash, cdp, psz);
        g_print ("%s  ", csum);
        g_free (csum);
      }
    }

    if (checksumsink->plane_checksum)
      g_print ("\n");

    if (checksumsink->frame_checksum) {
      csum = g_compute_checksum_for_data (checksumsink->hash, data, size);
      g_print ("FrameChecksum %s\n", csum);
      g_free (csum);
    }
  }

  if (checksumsink->file_checksum || checksumsink->dump_output) {
    GST_MEMDUMP ("frame", data, size);
    do {
      if (size != GST_VIDEO_FRAME_SIZE (&frame)) {
        GST_WARNING ("size are different! %lu != %lu", size,
            GST_VIDEO_FRAME_SIZE (&frame));
      }
      ssize_t written = write (checksumsink->fd, data, size);
      if (written == -1) {
        GST_ELEMENT_ERROR (checksumsink, RESOURCE, WRITE,
            ("Failed to write to the file: %s", g_strerror (errno)), (NULL));
        return GST_FLOW_ERROR;
      } else if (written < size) {
        data = &data[written + 1];
        size = size - written;
      } else if (written == size) {
        break;
      }
    } while (TRUE);
  }

  gst_video_frame_unmap (&frame);

  return GST_FLOW_OK;
}
