/*
 * call-handler.c
 * Copyright (C) 2011 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <gst/gst.h>
#include <telepathy-glib/telepathy-glib.h>
#include <farstream/fs-element-added-notifier.h>
#include <farstream/fs-utils.h>
#include <telepathy-farstream/telepathy-farstream.h>

#include "echo-call-info-dbus.h"

#define CALLS_OBJECT_PATH "/org/freedesktop/Telepathy/Phoenix/Calls"
#define CALLS_NAME "org.freedesktop.Telepathy.Phoenix.Calls"

typedef enum {
  CALL_MODE_ECHO = 0,
  CALL_MODE_TEST_INPUTS
} CallMode;

typedef struct {
  GstElement *pipeline;
  guint buswatch;
  TpChannel *proxy;
  TfChannel *channel;
  GList *notifiers;
  CallMode mode;
  GstElement *audio_src;
  GstElement *video_src;
  EciObjectSkeleton *call_object;
  EciCallInfo *call_info;
} ChannelContext;

GMainLoop *loop;
GDBusObjectManagerServer *object_manager;

static gboolean
bus_watch_cb (GstBus *bus,
    GstMessage *message,
    gpointer user_data)
{
  ChannelContext *context = user_data;

  if (context->channel != NULL)
    tf_channel_bus_message (context->channel, message);

  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR)
    {
      GError *error = NULL;
      gchar *debug = NULL;
      gst_message_parse_error (message, &error, &debug);
      g_printerr ("ERROR from element %s: %s\n",
          GST_OBJECT_NAME (message->src), error->message);
      g_printerr ("Debugging info: %s\n", (debug) ? debug : "none");
      g_error_free (error);
      g_free (debug);
    }

  return TRUE;
}

static void
src_pad_unlinked_cb (GstPad *pad,
    GstPad *peer,
    gpointer user_data)
{
  ChannelContext *context = user_data;
  GstElement *element;
  GstPad *element_srcpad;

  g_debug ("Src pad unlinked");

  element = gst_pad_get_parent_element (peer);
  element_srcpad = gst_element_get_static_pad (element, "src");

  if (gst_pad_is_linked (element_srcpad))
    {
      GstPad *peer = gst_pad_get_peer (element_srcpad);
      gst_pad_unlink (element_srcpad, peer);
      gst_object_unref (peer);
    }

  gst_element_set_locked_state (element, TRUE);
  gst_element_set_state (element, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (context->pipeline), element);
  gst_object_unref (element);
}


static void
setup_echo_sink (TfContent *content, GstPad *pad, ChannelContext *context)
{
  GstPad *element_sinkpad;
  GstPad *element_srcpad;
  GstPad *content_sinkpad;
  GstElement *element;
  GstStateChangeReturn ret;

  g_object_get (content, "sink-pad", &content_sinkpad, NULL);

  element = gst_element_factory_make ("queue", NULL);

  gst_bin_add (GST_BIN (context->pipeline), element);

  element_sinkpad = gst_element_get_static_pad (element, "sink");
  element_srcpad = gst_element_get_static_pad (element, "src");

  ret = gst_element_set_state (element, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      tp_channel_close_async (TP_CHANNEL (context->proxy), NULL, NULL);
      g_warning ("Failed to start sink pipeline !?");
      goto err;
    }

  if (GST_PAD_LINK_FAILED (gst_pad_link (pad, element_sinkpad)))
    {
      tp_channel_close_async (TP_CHANNEL (context->proxy), NULL, NULL);
      g_warning ("Couldn't link src pad to queue !?");
      goto err;
    }

  if (GST_PAD_LINK_FAILED (gst_pad_link (element_srcpad, content_sinkpad)))
    {
      tp_channel_close_async (TP_CHANNEL (context->proxy), NULL, NULL);
      g_warning ("Couldn't link queue to sink pad !?");
      goto err;
    }

  g_signal_connect (pad, "unlinked",
      G_CALLBACK (src_pad_unlinked_cb), context);

out:
  gst_object_unref (element_sinkpad);
  gst_object_unref (element_srcpad);
  gst_object_unref (content_sinkpad);

  return;

err:
  src_pad_unlinked_cb (pad, element_sinkpad, context);
  goto out;
}

static void
setup_fake_sink (TfContent *content, GstPad *pad, ChannelContext *context)
{
  GstElement *element = gst_element_factory_make ("fakesink", NULL);
  GstPad *sinkpad;
  GstStateChangeReturn ret;

  gst_bin_add (GST_BIN (context->pipeline), element);

  ret = gst_element_set_state (element, GST_STATE_PLAYING);
  sinkpad = gst_element_get_static_pad (element, "sink");
  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      tp_channel_close_async (TP_CHANNEL (context->proxy), NULL, NULL);
      g_warning ("Failed to start sink pipeline !?");
      goto err;
    }

  if (GST_PAD_LINK_FAILED (gst_pad_link (pad, sinkpad)))
    {
      tp_channel_close_async (TP_CHANNEL (context->proxy), NULL, NULL);
      g_warning ("Couldn't link src pad to queue !?");
      goto err;
    }

  return;

err:
  gst_bin_remove (GST_BIN (context->pipeline), element);
  gst_object_unref (element);
  gst_object_unref (sinkpad);
}

static void
src_pad_added_cb (TfContent *content,
    TpHandle handle,
    FsStream *stream,
    GstPad *pad,
    FsCodec *codec,
    gpointer user_data)
{
  ChannelContext *context = user_data;
  gchar *cstr = fs_codec_to_string (codec);
  FsMediaType mtype;

  g_debug ("New src pad: %s", cstr);
  g_free (cstr);

  g_object_get (content, "media-type", &mtype, NULL);

  switch (mtype)
    {
      case FS_MEDIA_TYPE_AUDIO:
        eci_call_info_set_receiving_audio (context->call_info, TRUE);
        break;
      case FS_MEDIA_TYPE_VIDEO:
        eci_call_info_set_receiving_video (context->call_info, TRUE);
        break;
    }

  switch (context->mode)
    {
      case CALL_MODE_ECHO:
        setup_echo_sink (content, pad, context);
        break;
      case CALL_MODE_TEST_INPUTS:
        setup_fake_sink (content, pad, context);
        break;
    }
}

static gboolean
start_sending_cb (TfContent *content, gpointer user_data)
{
  ChannelContext *context = user_data;
  GstPad *srcpad, *sinkpad;
  FsMediaType mtype;
  GstElement *element;
  GstStateChangeReturn ret;
  gboolean res = FALSE;

  g_debug ("Start sending");

  /* When echoing the source will get setup when we start receiving data */
  if (context->mode == CALL_MODE_ECHO)
    return TRUE;

  g_object_get (content,
    "sink-pad", &sinkpad,
    "media-type", &mtype,
    NULL);

  switch (mtype)
    {
      case FS_MEDIA_TYPE_AUDIO:
        if (context->audio_src)
          goto out;

        element = gst_parse_bin_from_description (
          "audiotestsrc is-live=1", TRUE, NULL);
        context->audio_src = element;
        break;
      case FS_MEDIA_TYPE_VIDEO:
        if (context->video_src)
          goto out;

        element = gst_parse_bin_from_description (
          "videotestsrc is-live=1 ! video/x-raw-yuv, width=320, height=240",
          TRUE, NULL);
        context->video_src = element;
        break;
      default:
        g_warning ("Unknown media type");
        goto out;
    }


  gst_bin_add (GST_BIN (context->pipeline), element);
  srcpad = gst_element_get_static_pad (element, "src");

  if (GST_PAD_LINK_FAILED (gst_pad_link (srcpad, sinkpad)))
    {
      tp_channel_close_async (TP_CHANNEL (context->proxy), NULL, NULL);
      g_warning ("Couldn't link source pipeline !?");
      goto out2;
    }

  ret = gst_element_set_state (element, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      tp_channel_close_async (TP_CHANNEL (context->proxy), NULL, NULL);
      g_warning ("source pipeline failed to start!?");
      goto out2;
    }

  res = TRUE;

out2:
  g_object_unref (srcpad);
out:
  g_object_unref (sinkpad);

  return res;
}



static void
content_added_cb (TfChannel *channel,
    TfContent *content,
    gpointer user_data)
{
  ChannelContext *context = user_data;

  g_debug ("Content added");

  g_signal_connect (content, "start-sending",
      G_CALLBACK (start_sending_cb), context);
  g_signal_connect (content, "src-pad-added",
      G_CALLBACK (src_pad_added_cb), context);
}

static void
conference_added_cb (TfChannel *channel,
  GstElement *conference,
  gpointer user_data)
{
  ChannelContext *context = user_data;
  GKeyFile *keyfile;

  g_debug ("Conference added");

  /* Add notifier to set the various element properties as needed */
  keyfile = fs_utils_get_default_element_properties (conference);
  if (keyfile != NULL)
    {
      FsElementAddedNotifier *notifier;
      g_debug ("Loaded default codecs for %s", GST_ELEMENT_NAME (conference));

      notifier = fs_element_added_notifier_new ();
      fs_element_added_notifier_set_properties_from_keyfile (notifier, keyfile);
      fs_element_added_notifier_add (notifier, GST_BIN (context->pipeline));

      context->notifiers = g_list_prepend (context->notifiers, notifier);
    }


  gst_bin_add (GST_BIN (context->pipeline), conference);
  gst_element_set_state (conference, GST_STATE_PLAYING);
}


static void
conference_removed_cb (TfChannel *channel,
  GstElement *conference,
  gpointer user_data)
{
  ChannelContext *context = user_data;

  gst_element_set_locked_state (conference, TRUE);
  gst_element_set_state (conference, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (context->pipeline), conference);
}

static void
new_tf_channel_cb (GObject *source,
  GAsyncResult *result,
  gpointer user_data)
{
  ChannelContext *context = user_data;
  GError *error = NULL;

  g_debug ("New TfChannel");

  context->channel = tf_channel_new_finish (source, result, &error);

  if (context->channel == NULL)
    {
      g_error ("Failed to create channel: %s", error->message);
      g_clear_error (&error);
    }

  g_signal_connect (context->channel, "fs-conference-added",
    G_CALLBACK (conference_added_cb), context);


  g_signal_connect (context->channel, "fs-conference-removed",
    G_CALLBACK (conference_removed_cb), context);

  g_signal_connect (context->channel, "content-added",
    G_CALLBACK (content_added_cb), context);
}

static void
proxy_invalidated_cb (TpProxy *proxy,
    guint domain,
    gint code,
    gchar *message,
    gpointer user_data)
{
  ChannelContext *context = user_data;

  g_debug ("Channel closed");
  if (context->pipeline != NULL)
    {
      gst_element_set_state (context->pipeline, GST_STATE_NULL);
      g_object_unref (context->pipeline);
    }

  if (context->channel != NULL)
    g_object_unref (context->channel);

  g_list_foreach (context->notifiers, (GFunc) g_object_unref, NULL);
  g_list_free (context->notifiers);

  g_object_unref (context->proxy);

   g_dbus_object_manager_server_unexport (object_manager,
    g_dbus_object_get_object_path (G_DBUS_OBJECT (context->call_object)));

  g_object_unref (context->call_info);
  g_object_unref (context->call_object);

  g_slice_free (ChannelContext, context);
}

static void
call_state_changed_cb (TpCallChannel *self,
  guint state,
  guint flags,
  TpCallStateReason reason,
  GHashTable *details,
  ChannelContext *context)
{
  switch (state)
    {
      case TP_CALL_STATE_INITIALISED:
        if (!tp_channel_get_requested (TP_CHANNEL (self)))
          tp_call_channel_accept_async (self, NULL, NULL);
        break;
      case TP_CALL_STATE_ENDED:
        g_debug ("Call ended");
        tp_channel_close_async (TP_CHANNEL (self), NULL, NULL);
    }
}

static void
new_call_channel_cb (TpSimpleHandler *handler,
    TpAccount *account,
    TpConnection *connection,
    GList *channels,
    GList *requests_satisfied,
    gint64 user_action_time,
    TpHandleChannelsContext *handler_context,
    gpointer user_data)
{
  ChannelContext *context;
  TpCallChannel *call;
  GstBus *bus;
  GstElement *pipeline;
  GstStateChangeReturn ret;
  GList *rl;
  gchar *path;

  g_debug ("New channel");

  call = channels->data;

  pipeline = gst_pipeline_new (NULL);

  ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);

  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      tp_channel_close_async (TP_CHANNEL (call), NULL, NULL);
      g_object_unref (pipeline);
      g_warning ("Failed to start an empty pipeline !?");
      return;
    }

  context = g_slice_new0 (ChannelContext);
  context->pipeline = pipeline;
  context->call_info = eci_call_info_skeleton_new ();

  g_object_set (context->call_info,
    "receiving-audio", FALSE,
    "receiving-video", FALSE,
    "channel", tp_proxy_get_object_path (call),
    NULL);

  path = g_strdup_printf ("%s%s",
    CALLS_OBJECT_PATH,
    tp_proxy_get_object_path (call));
  context->call_object = eci_object_skeleton_new (path);
  g_free(path);

  eci_object_skeleton_set_call_info (context->call_object,
    context->call_info);

  g_dbus_object_manager_server_export (object_manager,
    G_DBUS_OBJECT_SKELETON (context->call_object));

  for (rl = requests_satisfied; rl != NULL; rl = g_list_next (rl))
    {
      const gchar *mode;
      const GHashTable *hints =
        tp_channel_request_get_hints (TP_CHANNEL_REQUEST (rl->data));

      mode = tp_asv_get_string (hints, "call-mode");
      if (!tp_strdiff (mode, "test-inputs"))
        context->mode = CALL_MODE_TEST_INPUTS;
      else if (!tp_strdiff (mode, "echo"))
        context->mode = CALL_MODE_ECHO;
    }

  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  context->buswatch = gst_bus_add_watch (bus, bus_watch_cb, context);
  g_object_unref (bus);

  tf_channel_new_async (TP_CHANNEL (call), new_tf_channel_cb, context);

  tp_handle_channels_context_accept (handler_context);

  g_signal_connect (call, "state-changed",
    G_CALLBACK (call_state_changed_cb), context);
  if (tp_channel_get_requested (TP_CHANNEL (call)))
    {
      if (tp_call_channel_get_state (call,
          NULL, NULL, NULL) == TP_CALL_STATE_PENDING_INITIATOR)
        tp_call_channel_accept_async (call, NULL, NULL);
    }
  else
    {
      if (tp_call_channel_get_state (call, NULL, NULL, NULL) ==
          TP_CALL_STATE_INITIALISED)
        tp_call_channel_accept_async (call, NULL, NULL);
    }

  context->proxy = g_object_ref (call);
  g_signal_connect (call, "invalidated",
    G_CALLBACK (proxy_invalidated_cb),
    context);
}

int
main (int argc, char **argv)
{
  TpBaseClient *client;
  TpAccountManager *am;
  GDBusConnection *connection;

  g_type_init ();
  gst_init (&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  am = tp_account_manager_dup ();

  client = tp_simple_handler_new_with_am (am,
    FALSE,
    FALSE,
    "Phoenix.EchoCall",
    FALSE,
    new_call_channel_cb,
    NULL,
    NULL);

  tp_base_client_take_handler_filter (client,
    tp_asv_new (
       TP_PROP_CHANNEL_CHANNEL_TYPE, G_TYPE_STRING,
          TP_IFACE_CHANNEL_TYPE_CALL,
       TP_PROP_CHANNEL_TARGET_HANDLE_TYPE, G_TYPE_UINT,
          TP_HANDLE_TYPE_CONTACT,
        TP_PROP_CHANNEL_TYPE_CALL_INITIAL_AUDIO, G_TYPE_BOOLEAN,
          TRUE,
       NULL));

  tp_base_client_take_handler_filter (client,
    tp_asv_new (
       TP_PROP_CHANNEL_CHANNEL_TYPE, G_TYPE_STRING,
          TP_IFACE_CHANNEL_TYPE_CALL,
       TP_PROP_CHANNEL_TARGET_HANDLE_TYPE, G_TYPE_UINT,
          TP_HANDLE_TYPE_CONTACT,
        TP_PROP_CHANNEL_TYPE_CALL_INITIAL_VIDEO, G_TYPE_BOOLEAN,
          TRUE,
       NULL));

  tp_base_client_add_handler_capabilities_varargs (client,
    TP_IFACE_CHANNEL_TYPE_CALL "/video/h264",
    TP_IFACE_CHANNEL_TYPE_CALL "/shm",
    TP_IFACE_CHANNEL_TYPE_CALL "/ice",
    TP_IFACE_CHANNEL_TYPE_CALL "/gtalk-p2p",
    NULL);

  tp_base_client_register (client, NULL);

  connection = g_bus_get_sync (G_BUS_TYPE_STARTER,
    NULL, NULL);
  g_assert (connection != NULL);
  object_manager = g_dbus_object_manager_server_new (
    CALLS_OBJECT_PATH);
  g_dbus_object_manager_server_set_connection (object_manager,
    connection);

  g_bus_own_name_on_connection (connection,
    CALLS_NAME,
    G_BUS_NAME_OWNER_FLAGS_REPLACE,
    NULL, NULL, NULL, NULL);

  g_main_loop_run (loop);

  g_object_unref (am);
  g_object_unref (client);
  g_main_loop_unref (loop);

  return 0;
}
