/* GStreamer
 * (c) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * (c) 2006 Jan Schmidt <thaytan@noraisin.net>
 * (c) 2008 Stefan Kost <ensonic@users.sf.net>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-autovideosrc
 * @see_also: autoaudiosrc, v4l2src, v4lsrc
 *
 * autovideosrc is a video src that automatically detects an appropriate
 * video source to use.  It does so by scanning the registry for all elements
 * that have <quote>Source</quote> and <quote>Video</quote> in the class field
 * of their element information, and also have a non-zero autoplugging rank.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v -m autovideosrc ! xvimagesink
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gstautovideosrc.h"
#include "gstautodetect.h"

/* Properties */
enum
{
  PROP_0,
  PROP_CAPS,
};

static GstStateChangeReturn
gst_auto_video_src_change_state (GstElement * element,
    GstStateChange transition);
static void gst_auto_video_src_dispose (GstAutoVideoSrc * src);
static void gst_auto_video_src_clear_kid (GstAutoVideoSrc * src);

static void gst_auto_video_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_auto_video_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

GST_BOILERPLATE (GstAutoVideoSrc, gst_auto_video_src, GstBin, GST_TYPE_BIN);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static void
gst_auto_video_src_base_init (gpointer klass)
{
  GstElementClass *eklass = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_pad_template (eklass,
      gst_static_pad_template_get (&src_template));
  gst_element_class_set_details_simple (eklass, "Auto video source",
      "Source/Video",
      "Wrapper video source for automatically detected video source",
      "Jan Schmidt <thaytan@noraisin.net>, "
      "Stefan Kost <ensonic@users.sf.net>");
}

static void
gst_auto_video_src_class_init (GstAutoVideoSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *eklass = GST_ELEMENT_CLASS (klass);

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose =
      (GObjectFinalizeFunc) GST_DEBUG_FUNCPTR (gst_auto_video_src_dispose);
  eklass->change_state = GST_DEBUG_FUNCPTR (gst_auto_video_src_change_state);
  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_auto_video_src_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_auto_video_src_get_property);

  /**
   * GstAutoVideoSrc:filter-caps
   *
   * This property will filter out candidate sources that can handle the specified
   * caps. By default only video sources that support raw rgb and yuv video
   * are selected.
   *
   * This property can only be set before the element goes to the READY state.
   *
   * Since: 0.10.14
   **/
  g_object_class_install_property (gobject_class, PROP_CAPS,
      g_param_spec_boxed ("filter-caps", "Filter caps",
          "Filter src candidates using these caps.", GST_TYPE_CAPS,
          G_PARAM_READWRITE));
}

static void
gst_auto_video_src_dispose (GstAutoVideoSrc * src)
{
  gst_auto_video_src_clear_kid (src);

  if (src->filter_caps)
    gst_caps_unref (src->filter_caps);
  src->filter_caps = NULL;

  G_OBJECT_CLASS (parent_class)->dispose ((GObject *) src);
}

static void
gst_auto_video_src_clear_kid (GstAutoVideoSrc * src)
{
  if (src->kid) {
    gst_element_set_state (src->kid, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (src), src->kid);
    src->kid = NULL;
  }
}

/*
 * Hack to make initial linking work; ideally, this'd work even when
 * no target has been assigned to the ghostpad yet.
 */

static void
gst_auto_video_src_reset (GstAutoVideoSrc * src)
{
  GstPad *targetpad;

  /* Remove any existing element */
  gst_auto_video_src_clear_kid (src);

  /* fakesrc placeholder */
  src->kid = gst_element_factory_make ("fakesrc", "tempsrc");
  gst_bin_add (GST_BIN (src), src->kid);

  /* pad */
  targetpad = gst_element_get_static_pad (src->kid, "src");
  gst_ghost_pad_set_target (GST_GHOST_PAD (src->pad), targetpad);
  gst_object_unref (targetpad);
}

static GstStaticCaps raw_caps =
    GST_STATIC_CAPS ("video/x-raw-yuv; video/x-raw-rgb");

static void
gst_auto_video_src_init (GstAutoVideoSrc * src, GstAutoVideoSrcClass * g_class)
{
  src->pad = gst_ghost_pad_new_no_target ("src", GST_PAD_SRC);
  gst_element_add_pad (GST_ELEMENT (src), src->pad);

  gst_auto_video_src_reset (src);

  /* set the default raw video caps */
  src->filter_caps = gst_static_caps_get (&raw_caps);

  /* mark as source */
  GST_OBJECT_FLAG_UNSET (src, GST_ELEMENT_IS_SINK);
}

static gboolean
gst_auto_video_src_factory_filter (GstPluginFeature * feature, gpointer data)
{
  guint rank;
  const gchar *klass;

  /* we only care about element factories */
  if (!GST_IS_ELEMENT_FACTORY (feature))
    return FALSE;

  /* video sources */
  klass = gst_element_factory_get_klass (GST_ELEMENT_FACTORY (feature));
  if (!(strstr (klass, "Source") && strstr (klass, "Video")))
    return FALSE;

  /* only select elements with autoplugging rank */
  rank = gst_plugin_feature_get_rank (feature);
  if (rank < GST_RANK_MARGINAL)
    return FALSE;

  return TRUE;
}

static gint
gst_auto_video_src_compare_ranks (GstPluginFeature * f1, GstPluginFeature * f2)
{
  gint diff;

  diff = gst_plugin_feature_get_rank (f2) - gst_plugin_feature_get_rank (f1);
  if (diff != 0)
    return diff;
  return strcmp (gst_plugin_feature_get_name (f2),
      gst_plugin_feature_get_name (f1));
}

static GstElement *
gst_auto_video_src_create_element_with_pretty_name (GstAutoVideoSrc * src,
    GstElementFactory * factory)
{
  GstElement *element;
  gchar *name, *marker;

  marker = g_strdup (GST_PLUGIN_FEATURE (factory)->name);
  if (g_str_has_suffix (marker, "src"))
    marker[strlen (marker) - 4] = '\0';
  if (g_str_has_prefix (marker, "gst"))
    g_memmove (marker, marker + 3, strlen (marker + 3) + 1);
  name = g_strdup_printf ("%s-actual-src-%s", GST_OBJECT_NAME (src), marker);
  g_free (marker);

  element = gst_element_factory_create (factory, name);
  g_free (name);

  return element;
}

static GstElement *
gst_auto_video_src_find_best (GstAutoVideoSrc * src)
{
  GList *list, *item;
  GstElement *choice = NULL;
  GstMessage *message = NULL;
  GSList *errors = NULL;
  GstBus *bus = gst_bus_new ();
  GstPad *el_pad = NULL;
  GstCaps *el_caps = NULL;
  gboolean no_match = TRUE;

  list = gst_registry_feature_filter (gst_registry_get_default (),
      (GstPluginFeatureFilter) gst_auto_video_src_factory_filter, FALSE, src);
  list = g_list_sort (list, (GCompareFunc) gst_auto_video_src_compare_ranks);

  GST_LOG_OBJECT (src, "Trying to find usable video devices ...");

  for (item = list; item != NULL; item = item->next) {
    GstElementFactory *f = GST_ELEMENT_FACTORY (item->data);
    GstElement *el;

    if ((el = gst_auto_video_src_create_element_with_pretty_name (src, f))) {
      GstStateChangeReturn ret;

      GST_DEBUG_OBJECT (src, "Testing %s", GST_PLUGIN_FEATURE (f)->name);

      /* If AutoVideoSrc has been provided with filter caps,
       * accept only sources that match with the filter caps */
      if (src->filter_caps) {
        el_pad = gst_element_get_static_pad (GST_ELEMENT (el), "src");
        el_caps = gst_pad_get_caps (el_pad);
        gst_object_unref (el_pad);
        GST_DEBUG_OBJECT (src,
            "Checking caps: %" GST_PTR_FORMAT " vs. %" GST_PTR_FORMAT,
            src->filter_caps, el_caps);
        no_match = !gst_caps_can_intersect (src->filter_caps, el_caps);
        gst_caps_unref (el_caps);

        if (no_match) {
          GST_DEBUG_OBJECT (src, "Incompatible caps");
          gst_object_unref (el);
          continue;
        } else {
          GST_DEBUG_OBJECT (src, "Found compatible caps");
        }
      }

      gst_element_set_bus (el, bus);
      ret = gst_element_set_state (el, GST_STATE_READY);
      if (ret == GST_STATE_CHANGE_SUCCESS) {
        GST_DEBUG_OBJECT (src, "This worked!");
        choice = el;
        break;
      }

      /* collect all error messages */
      while ((message = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR))) {
        GST_DEBUG_OBJECT (src, "error message %" GST_PTR_FORMAT, message);
        errors = g_slist_append (errors, message);
      }

      gst_element_set_state (el, GST_STATE_NULL);
      gst_object_unref (el);
    }
  }

  GST_DEBUG_OBJECT (src, "done trying");
  if (!choice) {
    if (errors) {
      /* FIXME: we forward the first error for now; but later on it might make
       * sense to actually analyse them */
      gst_message_ref (GST_MESSAGE (errors->data));
      GST_DEBUG_OBJECT (src, "reposting message %p", errors->data);
      gst_element_post_message (GST_ELEMENT (src), GST_MESSAGE (errors->data));
    } else {
      /* send warning message to application and use a fakesrc */
      GST_ELEMENT_WARNING (src, RESOURCE, NOT_FOUND, (NULL),
          ("Failed to find a usable video source"));
      choice = gst_element_factory_make ("fakesrc", "fake-video-src");
      if (g_object_class_find_property (G_OBJECT_GET_CLASS (choice), "sync"))
        g_object_set (choice, "sync", TRUE, NULL);
      gst_element_set_state (choice, GST_STATE_READY);
    }
  }
  gst_object_unref (bus);
  gst_plugin_feature_list_free (list);
  g_slist_foreach (errors, (GFunc) gst_mini_object_unref, NULL);
  g_slist_free (errors);

  return choice;
}

static gboolean
gst_auto_video_src_detect (GstAutoVideoSrc * src)
{
  GstElement *esrc;
  GstPad *targetpad;

  gst_auto_video_src_clear_kid (src);

  /* find element */
  GST_DEBUG_OBJECT (src, "Creating new kid");
  if (!(esrc = gst_auto_video_src_find_best (src)))
    goto no_src;

  src->kid = esrc;
  gst_bin_add (GST_BIN (src), esrc);

  /* attach ghost pad */
  GST_DEBUG_OBJECT (src, "Re-assigning ghostpad");
  targetpad = gst_element_get_static_pad (src->kid, "src");
  if (!gst_ghost_pad_set_target (GST_GHOST_PAD (src->pad), targetpad))
    goto target_failed;

  gst_object_unref (targetpad);
  GST_DEBUG_OBJECT (src, "done changing auto video source");

  return TRUE;

  /* ERRORS */
no_src:
  {
    GST_ELEMENT_ERROR (src, LIBRARY, INIT, (NULL),
        ("Failed to find a supported video source"));
    return FALSE;
  }
target_failed:
  {
    GST_ELEMENT_ERROR (src, LIBRARY, INIT, (NULL),
        ("Failed to set target pad"));
    gst_object_unref (targetpad);
    return FALSE;
  }
}

static GstStateChangeReturn
gst_auto_video_src_change_state (GstElement * element,
    GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstAutoVideoSrc *src = GST_AUTO_VIDEO_SRC (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_auto_video_src_detect (src))
        return GST_STATE_CHANGE_FAILURE;
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_auto_video_src_reset (src);
      break;
    default:
      break;
  }

  return ret;
}

static void
gst_auto_video_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAutoVideoSrc *src = GST_AUTO_VIDEO_SRC (object);

  switch (prop_id) {
    case PROP_CAPS:
      if (src->filter_caps)
        gst_caps_unref (src->filter_caps);
      src->filter_caps = gst_caps_copy (gst_value_get_caps (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_auto_video_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstAutoVideoSrc *src = GST_AUTO_VIDEO_SRC (object);

  switch (prop_id) {
    case PROP_CAPS:{
      gst_value_set_caps (value, src->filter_caps);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}
