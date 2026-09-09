/* SPDX-License-Identifier: Apache-2.0 */
#include "protocol.hpp"
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <glib-unix.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace recovery_ui2::media;

struct Player {
  GMainLoop *loop = nullptr;
  GstElement *playbin = nullptr;
  uint8_t *pixels = nullptr;
  uint32_t sequence = 0;
  bool pending = false;
  bool playing = false;
  char title[512]{};
};

static bool Send(Message &message) {
  message.magic = kMagic;
  return send(4, &message, sizeof(message), MSG_DONTWAIT | MSG_NOSIGNAL) ==
      static_cast<ssize_t>(sizeof(message));
}

static void Status(Player *player, Kind kind = Kind::kStatus,
                   const char *text = nullptr) {
  Message message; message.kind = kind; message.value = player->playing;
  gint64 position = 0, duration = 0;
  if (player->playbin) {
    gst_element_query_position(player->playbin, GST_FORMAT_TIME, &position);
    gst_element_query_duration(player->playbin, GST_FORMAT_TIME, &duration);
  }
  message.position_ms = position > 0 ? position / GST_MSECOND : 0;
  message.duration_ms = duration > 0 ? duration / GST_MSECOND : 0;
  snprintf(message.text, sizeof(message.text), "%s",
           text ? text : (player->title[0] ? player->title : "AERA Media"));
  if (kind == Kind::kError) {
    fprintf(stderr, "AERA Media playback error: %s\n", message.text);
    fflush(stderr);
  }
  if (!Send(message)) g_main_loop_quit(player->loop);
}

static GstFlowReturn VideoSample(GstAppSink *sink, gpointer data) {
  auto *player = static_cast<Player *>(data);
  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_EOS;
  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstMapInfo map{};
  if (!player->pending && gst_buffer_map(buffer, &map, GST_MAP_READ) &&
      map.size >= kFrameBytes) {
    const uint32_t next = player->sequence + 1;
    memcpy(player->pixels + FrameSlot(next) * kFrameBytes, map.data, kFrameBytes);
    gst_buffer_unmap(buffer, &map);
    Message message; message.kind = Kind::kFrame; message.sequence = next;
    message.x = kWidth; message.y = kHeight; message.value = kFrameBytes;
    if (Send(message)) { player->sequence = next; player->pending = true; }
    else g_main_loop_quit(player->loop);
  }
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

static gboolean Bus(GstBus *, GstMessage *message, gpointer data) {
  auto *player = static_cast<Player *>(data);
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
      GError *error = nullptr; gchar *debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      Status(player, Kind::kError, error ? error->message : "Playback failed");
      if (error) g_error_free(error); g_free(debug); break;
    }
    case GST_MESSAGE_EOS:
      player->playing = false; Status(player, Kind::kEnded, "Playback finished"); break;
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC(message) == GST_OBJECT(player->playbin)) {
        GstState old_state, new_state, pending;
        gst_message_parse_state_changed(message, &old_state, &new_state, &pending);
        player->playing = new_state == GST_STATE_PLAYING;
        Status(player);
      }
      break;
    default: break;
  }
  return G_SOURCE_CONTINUE;
}

static bool SafePath(const char *path) {
  if (!path || strlen(path) < 9 || strlen(path) >= 500 ||
      strncmp(path, "/sdcard/", 8)) return false;
  return !strstr(path, "/../") && strcmp(path + strlen(path) - 3, "/..") != 0;
}

static gboolean Input(gint, GIOCondition condition, gpointer data) {
  auto *player = static_cast<Player *>(data);
  if (condition & (G_IO_HUP | G_IO_ERR)) { g_main_loop_quit(player->loop); return G_SOURCE_REMOVE; }
  Message message;
  const auto count = recv(4, &message, sizeof(message), MSG_DONTWAIT | MSG_TRUNC);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return G_SOURCE_CONTINUE;
  if (count != sizeof(message) || !Valid(message, false)) {
    g_main_loop_quit(player->loop); return G_SOURCE_REMOVE;
  }
  switch (message.kind) {
    case Kind::kOpen: {
      if (!SafePath(message.text)) { Status(player, Kind::kError, "Invalid media path"); break; }
      GError *error = nullptr;
      gchar *uri = gst_filename_to_uri(message.text, &error);
      if (!uri) {
        Status(player, Kind::kError, error ? error->message : "Could not open file");
        if (error) g_error_free(error); break;
      }
      const char *leaf = strrchr(message.text, '/');
      snprintf(player->title, sizeof(player->title), "%s", leaf ? leaf + 1 : message.text);
      g_object_set(player->playbin, "uri", uri, nullptr); g_free(uri);
      gst_element_set_state(player->playbin, GST_STATE_PLAYING);
      player->playing = true; Status(player); break;
    }
    case Kind::kPlay:
      gst_element_set_state(player->playbin, GST_STATE_PLAYING); break;
    case Kind::kPause:
      gst_element_set_state(player->playbin, GST_STATE_PAUSED); break;
    case Kind::kSeekRelative: {
      gint64 position = 0;
      if (gst_element_query_position(player->playbin, GST_FORMAT_TIME, &position)) {
        position = std::max<gint64>(0, position + gint64(message.x) * GST_SECOND);
        gst_element_seek_simple(player->playbin, GST_FORMAT_TIME,
            GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), position);
      }
      break;
    }
    case Kind::kAck:
      if (player->pending && message.sequence == player->sequence) player->pending = false;
      break;
    case Kind::kClose:
      g_main_loop_quit(player->loop); return G_SOURCE_REMOVE;
    default: break;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean Tick(gpointer data) { Status(static_cast<Player *>(data)); return G_SOURCE_CONTINUE; }

int main() {
  if (fcntl(3, F_GETFD) < 0 || fcntl(4, F_GETFD) < 0) return 78;
  void *mapped = mmap(nullptr, kSharedBytes, PROT_READ | PROT_WRITE, MAP_SHARED, 3, 0);
  if (mapped == MAP_FAILED) return 78;
  gst_init(nullptr, nullptr);
  Player player; player.pixels = static_cast<uint8_t *>(mapped);
  player.loop = g_main_loop_new(nullptr, FALSE);
  player.playbin = gst_element_factory_make("playbin", "player");
  GstElement *appsink = gst_element_factory_make("appsink", "video");
  GstElement *audio = gst_element_factory_make("autoaudiosink", "audio");
  if (!player.loop || !player.playbin || !appsink || !audio) return 78;
  GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA",
      "width", G_TYPE_INT, kWidth, "height", G_TYPE_INT, kHeight, nullptr);
  g_object_set(appsink, "caps", caps, "emit-signals", TRUE, "drop", TRUE,
               "max-buffers", 1U, "sync", TRUE, nullptr);
  gst_caps_unref(caps);
  g_signal_connect(appsink, "new-sample", G_CALLBACK(VideoSample), &player);
  g_object_set(player.playbin, "video-sink", appsink, "audio-sink", audio, nullptr);
  GstBus *bus = gst_element_get_bus(player.playbin);
  gst_bus_add_watch(bus, Bus, &player); gst_object_unref(bus);
  g_unix_fd_add(4, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR), Input, &player);
  g_timeout_add(250, Tick, &player);
  Status(&player, Kind::kStatus, "Choose a song or video");
  g_main_loop_run(player.loop);
  gst_element_set_state(player.playbin, GST_STATE_NULL);
  gst_object_unref(player.playbin); g_main_loop_unref(player.loop);
  munmap(mapped, kSharedBytes); return 0;
}
