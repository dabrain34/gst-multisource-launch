/* GStreamer command line scalable application
 *
 * Copyright (C) 2019 Stéphane Cerveau <scerveau@collabora.com>
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


#include <stdio.h>
#include <stdlib.h>
#include <gst/gst.h>
#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#define DEFAULT_MUXER "multipartmux"
#define DEFAULT_SINK "fakesink"

#define SKIP(c) \
  while (*c) { \
    if ((*c == ' ') || (*c == '\n') || (*c == '\t') || (*c == '\r')) \
      c++; \
    else \
      break; \
  }

typedef struct _GstMultiSource
{
  GMainLoop *loop;
  guint signal_watch_intr_id;
  GIOChannel *io_stdin;
  gulong deep_notify_id;
  gchar *pipeline_description;
  gchar *muxer;
  gchar *sink;
  gboolean interactive;
  guint streams_selected;
  GstState state;
  gboolean auto_play;
  gboolean verbose;
  GstElement *pipeline;
  gboolean buffering;
  gboolean is_live;
} GstMultiSource;


#define PRINT(FMT, ARGS...) do { \
        gst_print (FMT "\n", ## ARGS); \
    } while (0)

void
quit_app (GstMultiSource * thiz)
{
  if (thiz->loop)
    g_main_loop_quit (thiz->loop);
}

#if defined(G_OS_UNIX) || defined(G_OS_WIN32)
/* As the interrupt handler is dispatched from GMainContext as a GSourceFunc
 * handler, we can react to this by posting a message. */
static gboolean
intr_handler (gpointer user_data)
{
  GstMultiSource *thiz = (GstMultiSource *) user_data;

  PRINT ("handling interrupt.");
  quit_app (thiz);
  /* remove signal handler */
  thiz->signal_watch_intr_id = 0;

  return G_SOURCE_REMOVE;
}
#endif

gboolean
set_player_state (GstMultiSource * thiz, GstState state)
{
  gboolean res = TRUE;
  GstStateChangeReturn ret;

  ret = gst_element_set_state (thiz->pipeline, state);

  switch (ret) {
    case GST_STATE_CHANGE_FAILURE:
      PRINT ("ERROR: pipeline doesn't want to pause.");
      res = FALSE;
      break;
    case GST_STATE_CHANGE_NO_PREROLL:
      PRINT ("pipeline is live and does not need PREROLL ...");
      thiz->is_live = TRUE;
      break;
    case GST_STATE_CHANGE_ASYNC:
      PRINT ("pipeline is PREROLLING ...");
      break;
      /* fallthrough */
    case GST_STATE_CHANGE_SUCCESS:
      if (thiz->state == GST_STATE_PAUSED)
        PRINT ("pipeline is PREROLLED ...");
      break;
  }
  return res;
}

static void
change_player_state (GstMultiSource * thiz, GstState state)
{
  if (thiz->state == state)
    return;

  thiz->state = state;
  PRINT ("player is %s", gst_element_state_get_name (state));
  switch (state) {
    case GST_STATE_READY:
      if (thiz->auto_play)
        set_player_state (thiz, GST_STATE_PAUSED);
      break;
    case GST_STATE_PAUSED:
      if (thiz->auto_play)
        set_player_state (thiz, GST_STATE_PLAYING);
      break;
    case GST_STATE_PLAYING:
      break;
    default:
      break;
  }
}
static gint
select_decodebin3_stream_cb (GstElement * decodebin,
    GstStreamCollection * collection, GstStream * stream,
    GstMultiSource * thiz)
{
  GstStreamType stype = gst_stream_get_stream_type (stream);
  /* select all the streams if stream_selected = 0*/
  if(thiz->streams_selected == 0)
    return -1;
  if (stype == GST_STREAM_TYPE_VIDEO
      && (thiz->streams_selected & GST_STREAM_TYPE_VIDEO))
    return 1;
  else if (stype == GST_STREAM_TYPE_AUDIO
      && (thiz->streams_selected & GST_STREAM_TYPE_AUDIO))
    return 1;
  else
    return 0;
}

static void
connect_select_stream_decodebin3(const GValue * item, gpointer user_data)
{
  GstElement *element = g_value_get_object (item);
  if (g_str_has_prefix (GST_ELEMENT_NAME (element), "decodebin3")) {
      g_signal_connect (element, "select-stream",
            G_CALLBACK (select_decodebin3_stream_cb), (GstMultiSource *) user_data);
  }
}

static void
add_select_stream_to_decodebin3 (GstMultiSource *thiz)
{
  GstIterator *it = gst_bin_iterate_recurse (GST_BIN (thiz->pipeline));

  while (gst_iterator_foreach (it, connect_select_stream_decodebin3,
                               thiz) == GST_ITERATOR_RESYNC)
    gst_iterator_resync (it);
  gst_iterator_free (it);
}

static gboolean
message_cb (GstBus * bus, GstMessage * message, gpointer user_data)
{
  GstMultiSource *thiz = (GstMultiSource *) user_data;
  PRINT ("Received new message %s from %s",
      GST_MESSAGE_TYPE_NAME (message), GST_OBJECT_NAME (message->src));
  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:{
      GError *err = NULL;
      gchar *name, *debug = NULL;

      name = gst_object_get_path_string (message->src);
      gst_message_parse_error (message, &err, &debug);

      PRINT ("ERROR: from element %s: %s\n", name, err->message);
      if (debug != NULL)
        PRINT ("Additional debug info:%s", debug);

      g_error_free (err);
      g_free (debug);
      g_free (name);

      g_main_loop_quit (thiz->loop);
      break;
    }
    case GST_MESSAGE_WARNING:{
      GError *err = NULL;
      gchar *name, *debug = NULL;

      name = gst_object_get_path_string (message->src);
      gst_message_parse_warning (message, &err, &debug);

      PRINT ("ERROR: from element %s: %s\n", name, err->message);
      if (debug != NULL)
        PRINT ("Additional debug info:\n%s\n", debug);

      g_error_free (err);
      g_free (debug);
      g_free (name);
      break;
    }
    case GST_MESSAGE_EOS:
      g_main_loop_quit (thiz->loop);
      break;
    case GST_MESSAGE_STATE_CHANGED:
    {
      GstState old, new, pending;
      if (GST_MESSAGE_SRC (message) == GST_OBJECT_CAST (thiz->pipeline)) {
        gst_message_parse_state_changed (message, &old, &new, &pending);
        change_player_state (thiz, new);
        {
          gchar * state_transition_name = g_strdup_printf ("%s_%s",
              gst_element_state_get_name (old), gst_element_state_get_name (new));
          gchar *dump_name = g_strconcat ("gst-multisource-launch.", state_transition_name,
              NULL);
          /* dump graph for (some) pipeline state changes */
          GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (thiz->pipeline),
              GST_DEBUG_GRAPH_SHOW_ALL, dump_name);
          g_free (dump_name);
          g_free (state_transition_name);
        }
      }
      break;
    }
    case GST_MESSAGE_BUFFERING:{
      gint percent;

      gst_message_parse_buffering (message, &percent);
      PRINT ("buffering  %d%% ", percent);

      /* no state management needed for live pipelines */
      if (thiz->is_live)
        break;

      if (percent == 100) {
        /* a 100% message means buffering is done */
        thiz->buffering = FALSE;
        /* if the desired state is playing, go back */
        if (thiz->state == GST_STATE_PLAYING) {
          PRINT ("Done buffering, setting pipeline to PLAYING ...");
          gst_element_set_state (thiz->pipeline, GST_STATE_PLAYING);
        }
      } else {
        /* buffering busy */
        if (!thiz->buffering && thiz->state == GST_STATE_PLAYING) {
          /* we were not buffering but PLAYING, PAUSE  the pipeline. */
          PRINT ("Buffering, setting pipeline to PAUSED ...");
          gst_element_set_state (thiz->pipeline, GST_STATE_PAUSED);
        }
        thiz->buffering = TRUE;
      }
      break;
    }
    case GST_MESSAGE_PROPERTY_NOTIFY:{
      const GValue *val;
      const gchar *name;
      GstObject *obj;
      gchar *val_str = NULL;
      gchar **ex_prop, *obj_name;

      if (!thiz->verbose)
        break;

      gst_message_parse_property_notify (message, &obj, &name, &val);

      /* Let's not print anything for excluded properties... */
      ex_prop = NULL;
      while (ex_prop != NULL && *ex_prop != NULL) {
        if (g_strcmp0 (name, *ex_prop) == 0)
          break;
        ex_prop++;
      }
      if (ex_prop != NULL && *ex_prop != NULL)
        break;

      obj_name = gst_object_get_path_string (GST_OBJECT (obj));
      if (val != NULL) {
        if (G_VALUE_HOLDS_STRING (val))
          val_str = g_value_dup_string (val);
        else if (G_VALUE_TYPE (val) == GST_TYPE_CAPS)
          val_str = gst_caps_to_string (g_value_get_boxed (val));
        else if (G_VALUE_TYPE (val) == GST_TYPE_TAG_LIST)
          val_str = gst_tag_list_to_string (g_value_get_boxed (val));
        else if (G_VALUE_TYPE (val) == GST_TYPE_STRUCTURE)
          val_str = gst_structure_to_string (g_value_get_boxed (val));
        else
          val_str = gst_value_serialize (val);
      } else {
        val_str = g_strdup ("(no value)");
      }

      PRINT ("%s: %s = %s", obj_name, name, val_str);
      g_free (obj_name);
      g_free (val_str);
      break;
    }
    default:
      break;
  }

  return TRUE;
}

void
add_branch (GstMultiSource * thiz, gchar * src_uri)
{
  GST_DEBUG ("Add branch with src %s with muxer %s", src_uri, thiz->muxer);
  if (!thiz->pipeline_description)
    thiz->pipeline_description =
        g_strdup_printf
        ("urisourcebin uri=%s ! decodebin3 ! %s name=muxer ! %s", src_uri,
        thiz->muxer, thiz->sink);
  else {
    gchar *old_pipeline_description = thiz->pipeline_description;
    thiz->pipeline_description =
        g_strdup_printf ("%s urisourcebin uri=%s ! decodebin3 ! muxer.",
        old_pipeline_description, src_uri);
    g_free (old_pipeline_description);
  }
}

/* Process keyboard input */
static gboolean
handle_keyboard (GIOChannel * source, GIOCondition cond, GstMultiSource * thiz)
{
  gchar *str = NULL;
  char op;

  if (g_io_channel_read_line (source, &str, NULL, NULL,
          NULL) == G_IO_STATUS_NORMAL) {

    gchar *cmd = str;
    SKIP (cmd)
        op = *cmd;
    cmd++;
    switch (op) {
      case 'q':
        quit_app (thiz);
        break;
      case 'p':
        if (thiz->state == GST_STATE_PAUSED)
          set_player_state (thiz, GST_STATE_PLAYING);
        else
          set_player_state (thiz, GST_STATE_PAUSED);
        break;
      case 's':
        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (thiz->pipeline),
                      GST_DEBUG_GRAPH_SHOW_ALL, "gst-multisource-launch.snap");
        break;
    }
  }
  g_free (str);
  return TRUE;
}

void
usage ()
{
  PRINT ("Available commands:\n"
      "  p - Toggle between Play and Pause\n" "  q - Quit");
}

int
main (int argc, char **argv)
{
  int res = EXIT_SUCCESS;
  GError *err = NULL;
  GOptionContext *ctx;
  GstMultiSource *thiz;
  GstBus *bus;
  gchar **full_branch_desc_array = NULL;
  gchar **branch_desc;
  gchar *muxer = NULL;
  gchar *sink = NULL;
  gboolean verbose = FALSE;
  gboolean audio_only = FALSE;
  gboolean video_only = FALSE;
  gboolean interactive = FALSE;
  gint repeat = 1;
  gint i = 0;

  GOptionEntry options[] = {
    {"source", 's', 0, G_OPTION_ARG_STRING_ARRAY, &full_branch_desc_array,
        "Add a RTP source", NULL}
    ,
    {"muxer", 'm', 0, G_OPTION_ARG_STRING, &muxer,
        "Add a RTP source", NULL}
    ,
    {"sink", 'S', 0, G_OPTION_ARG_STRING, &sink,
        "Add a RTP source", NULL}
    ,
    {"verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
        ("Output status information and property notifications"), NULL}
    ,
    {"audio-only", 'A', 0, G_OPTION_ARG_NONE, &audio_only,
        ("Select only audio tracks"), NULL}
    ,
    {"video-only", 'V', 0, G_OPTION_ARG_NONE, &video_only,
        ("Select only video tracks"), NULL}
    ,
    {"interactive", 'i', 0, G_OPTION_ARG_NONE, &interactive,
        ("Put on interactive mode with branches in GST_STATE_READY"), NULL}
    ,
    {NULL}
  };


  thiz = g_new0 (GstMultiSource, 1);

  ctx = g_option_context_new ("[ADDITIONAL ARGUMENTS]");
  g_option_context_add_main_entries (ctx, options, NULL);
  g_option_context_add_group (ctx, gst_init_get_option_group ());

  if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
    GST_ERROR ("Error initializing: %s\n", GST_STR_NULL (err->message));
    res = -1;
    goto done;
  }
  g_option_context_free (ctx);
  thiz->interactive = interactive;
  thiz->verbose = verbose;
  if (audio_only)
    thiz->streams_selected |= GST_STREAM_TYPE_AUDIO;
  if (video_only)
    thiz->streams_selected |= GST_STREAM_TYPE_VIDEO;

  if (!full_branch_desc_array) {
    g_printerr ("Usage: %s -s rtsp_source \n", argv[0]);
    goto done;
  }
  if (muxer)
    thiz->muxer = g_strdup (muxer);
  else
    thiz->muxer = g_strdup (DEFAULT_MUXER);

  if (sink)
    thiz->sink = g_strdup (sink);
  else
    thiz->sink = g_strdup (DEFAULT_SINK);

  for (branch_desc = full_branch_desc_array;
      branch_desc != NULL && *branch_desc != NULL; ++branch_desc) {
    for (i = 0; i < repeat; i++) {
      add_branch (thiz, *branch_desc);

    }
  }

  thiz->pipeline =
      gst_parse_launch_full (thiz->pipeline_description, NULL,
      GST_PARSE_FLAG_NONE, &err);

  if (err) {
    PRINT ("Unable to instantiate the transform branch %s with error %s",
        thiz->pipeline_description, err->message);
    goto done;
  }
  add_select_stream_to_decodebin3(thiz);

  bus = gst_pipeline_get_bus (GST_PIPELINE (thiz->pipeline));
  g_signal_connect (G_OBJECT (bus), "message", G_CALLBACK (message_cb), thiz);
  gst_bus_add_signal_watch (bus);
  gst_object_unref (GST_OBJECT (bus));

  thiz->state = GST_STATE_NULL;


  if (interactive) {
    thiz->io_stdin = g_io_channel_unix_new (fileno (stdin));
    g_io_add_watch (thiz->io_stdin, G_IO_IN, (GIOFunc) handle_keyboard, thiz);
    usage ();
  } else
    thiz->auto_play = TRUE;
  if (thiz->verbose) {
    thiz->deep_notify_id =
        gst_element_add_property_deep_notify_watch (thiz->pipeline, NULL, TRUE);
  }
  if (!set_player_state (thiz, GST_STATE_READY))
    goto done;

  thiz->loop = g_main_loop_new (NULL, FALSE);
#ifdef G_OS_UNIX
  thiz->signal_watch_intr_id =
      g_unix_signal_add (SIGINT, (GSourceFunc) intr_handler, thiz);
#endif
  g_main_loop_run (thiz->loop);

  /* No need to see all those pad caps going to NULL etc., it's just noise */

done:
  if (thiz->loop)
    g_main_loop_unref (thiz->loop);
  if (thiz->pipeline) {
    gst_element_set_state (thiz->pipeline, GST_STATE_READY);
    gst_element_set_state (thiz->pipeline, GST_STATE_NULL);
    gst_object_unref (thiz->pipeline);
  }
  if (thiz->deep_notify_id != 0)
    g_signal_handler_disconnect (thiz->pipeline, thiz->deep_notify_id);

  g_strfreev (full_branch_desc_array);
  g_free (thiz->muxer);
  g_free (thiz->pipeline_description);
  g_free (thiz);

  gst_deinit ();
  return res;
}
