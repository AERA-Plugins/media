/* SPDX-License-Identifier: Apache-2.0 */
#include "protocol.hpp"
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <gst/video/video-frame.h>
#include <gst/video/video.h>
#include <glib-unix.h>
#include <algorithm>
#include <atomic>
#include <cmath>
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
  GstElement *appsink = nullptr;
  uint8_t *pixels = nullptr;
  std::atomic<uint32_t> sequence{0};
  std::atomic<bool> pending{false};
  std::atomic<bool> thumbnail_mode{false};
  std::atomic<bool> thumbnail_sent{false};
  std::atomic<int32_t> thumbnail_token{0};
  bool playing = false;
  int rotation = 0;
  bool flip = false;
  char title[512]{};
};

static void ReadOrientation(const GstTagList *tags, int &rotation,
                            bool &flip) {
  rotation = 0;
  flip = false;
  if (!tags) return;
  gchar *orientation = nullptr;
  const bool found = gst_tag_list_get_string(
      tags, GST_TAG_IMAGE_ORIENTATION, &orientation);
  if (found && orientation) {
    flip = !strncmp(orientation, "flip-", 5);
    if (strstr(orientation, "rotate-90")) rotation = 90;
    else if (strstr(orientation, "rotate-180")) rotation = 180;
    else if (strstr(orientation, "rotate-270")) rotation = 270;
  }
  g_free(orientation);
}

static void FitFrame(double width, double height, int landscape_width,
                     int landscape_height, int &result_width,
                     int &result_height) {
  if (width <= 0 || height <= 0) {
    result_width = landscape_width;
    result_height = landscape_height;
    return;
  }
  const bool portrait = height > width;
  const double maximum_width = portrait ? landscape_height : landscape_width;
  const double maximum_height = portrait ? landscape_width : landscape_height;
  const double scale = std::min(maximum_width / width,
                                maximum_height / height);
  result_width = std::max(2, static_cast<int>(std::lround(width * scale)) & ~1);
  result_height = std::max(2, static_cast<int>(std::lround(height * scale)) & ~1);
}

static void ConfigureVideo(Player *player, const char *uri,
                           int landscape_width = kLandscapeWidth,
                           int landscape_height = kLandscapeHeight) {
  int width = landscape_width;
  int height = landscape_height;
  player->rotation = 0;
  player->flip = false;
  GError *error = nullptr;
  GstDiscoverer *discoverer = gst_discoverer_new(5 * GST_SECOND, &error);
  if (discoverer) {
    GstDiscovererInfo *info =
        gst_discoverer_discover_uri(discoverer, uri, &error);
    if (info) {
      GList *videos = gst_discoverer_info_get_video_streams(info);
      if (videos) {
        auto *video = GST_DISCOVERER_VIDEO_INFO(videos->data);
        double display_width = gst_discoverer_video_info_get_width(video);
        double display_height = gst_discoverer_video_info_get_height(video);
        const guint par_n = gst_discoverer_video_info_get_par_num(video);
        const guint par_d = gst_discoverer_video_info_get_par_denom(video);
        if (par_n && par_d) display_width *= double(par_n) / double(par_d);
        const GstTagList *tags = gst_discoverer_stream_info_get_tags(
            GST_DISCOVERER_STREAM_INFO(video));
        if (!tags) tags = gst_discoverer_info_get_tags(info);
        ReadOrientation(tags, player->rotation, player->flip);
        FitFrame(display_width, display_height, landscape_width,
                 landscape_height, width, height);
      }
      gst_discoverer_stream_info_list_free(videos);
      gst_discoverer_info_unref(info);
    }
    g_object_unref(discoverer);
  }
  if (error) g_error_free(error);

  GstCaps *caps = gst_caps_new_simple(
      "video/x-raw", "format", G_TYPE_STRING, "BGRA",
      "width", G_TYPE_INT, width, "height", G_TYPE_INT, height,
      "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr);
  g_object_set(player->appsink, "caps", caps, nullptr);
  gst_caps_unref(caps);
}

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

static gboolean PauseThumbnail(gpointer data) {
  auto *player = static_cast<Player *>(data);
  if (player->thumbnail_mode.load(std::memory_order_acquire)) {
    gst_element_set_state(player->playbin, GST_STATE_PAUSED);
    player->playing = false;
  }
  return G_SOURCE_REMOVE;
}

static GstFlowReturn VideoSample(GstAppSink *sink, gpointer data) {
  auto *player = static_cast<Player *>(data);
  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_EOS;
  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstCaps *caps = gst_sample_get_caps(sample);
  GstVideoInfo info;
  gst_video_info_init(&info);
  GstVideoFrame frame;
  const bool thumbnail =
      player->thumbnail_mode.load(std::memory_order_acquire);
  if (!player->pending.load(std::memory_order_acquire) && caps &&
      gst_video_info_from_caps(&info, caps) &&
      ValidFrame(GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info),
                 FrameBytes(GST_VIDEO_INFO_WIDTH(&info),
                            GST_VIDEO_INFO_HEIGHT(&info))) &&
      gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
    const int width = GST_VIDEO_FRAME_WIDTH(&frame);
    const int height = GST_VIDEO_FRAME_HEIGHT(&frame);
    const bool swap = player->rotation == 90 || player->rotation == 270;
    const int output_width = swap ? height : width;
    const int output_height = swap ? width : height;
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
      const uint32_t bytes = FrameBytes(output_width, output_height);
    if (std::abs(stride) >= width * 4 &&
        ValidFrame(output_width, output_height, bytes) &&
        (!thumbnail ||
         !player->thumbnail_sent.exchange(true, std::memory_order_acq_rel))) {
      const uint32_t next =
          player->sequence.load(std::memory_order_relaxed) + 1;
      uint8_t *destination =
          player->pixels + FrameSlot(next) * kFrameBytes;
      const uint8_t *source = static_cast<const uint8_t *>(
          GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
      if (!player->rotation && !player->flip) {
        for (int row = 0; row < height; ++row)
          memcpy(destination + row * width * 4, source + row * stride,
                 width * 4);
      } else {
        for (int y = 0; y < output_height; ++y) {
          uint8_t *output = destination + y * output_width * 4;
          for (int x = 0; x < output_width; ++x) {
            int source_x = x;
            int source_y = y;
            if (player->rotation == 90) {
              source_x = y;
              source_y = height - 1 - x;
            } else if (player->rotation == 180) {
              source_x = width - 1 - x;
              source_y = height - 1 - y;
            } else if (player->rotation == 270) {
              source_x = width - 1 - y;
              source_y = x;
            }
            if (player->flip) source_x = width - 1 - source_x;
            memcpy(output + x * 4, source + source_y * stride + source_x * 4,
                   4);
          }
        }
      }
      Message message;
      message.kind = thumbnail ? Kind::kThumbnailFrame : Kind::kFrame;
      message.sequence = next;
      message.x = output_width; message.y = output_height;
      message.value = bytes;
      if (thumbnail)
        message.position_ms =
            player->thumbnail_token.load(std::memory_order_relaxed);
      // Publish the outstanding sequence before sending. The host can ACK on
      // another core immediately after send(), so updating this state after
      // the syscall can lose a fast ACK and freeze video on that frame.
      player->sequence.store(next, std::memory_order_relaxed);
      player->pending.store(true, std::memory_order_release);
      if (!Send(message)) {
        player->pending.store(false, std::memory_order_release);
        g_main_loop_quit(player->loop);
      } else if (thumbnail) {
        g_main_context_invoke(nullptr, PauseThumbnail, player);
      }
    }
    gst_video_frame_unmap(&frame);
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
      /* A playbin cannot be safely retargeted while the previous decode graph
       * is still draining.  READY is asynchronous and left old appsink/audio
       * callbacks alive when a second file was selected.  Tear the graph all
       * the way down and wait for that transition before changing its URI and
       * negotiated output caps. */
      player->playing = false;
      player->thumbnail_mode.store(false, std::memory_order_release);
      player->thumbnail_sent.store(false, std::memory_order_release);
      gst_element_set_state(player->playbin, GST_STATE_NULL);
      gst_element_get_state(player->playbin, nullptr, nullptr,
                            2 * GST_SECOND);
      player->pending.store(false, std::memory_order_release);
      ConfigureVideo(player, uri);
      g_object_set(player->playbin, "uri", uri, nullptr); g_free(uri);
      gst_element_set_state(player->playbin, GST_STATE_PLAYING);
      player->playing = true; Status(player); break;
    }
    case Kind::kThumbnail: {
      if (!SafePath(message.text)) break;
      GError *error = nullptr;
      gchar *uri = gst_filename_to_uri(message.text, &error);
      if (!uri) {
        if (error) g_error_free(error);
        break;
      }
      player->playing = false;
      gst_element_set_state(player->playbin, GST_STATE_NULL);
      gst_element_get_state(player->playbin, nullptr, nullptr,
                            2 * GST_SECOND);
      player->pending.store(false, std::memory_order_release);
      player->thumbnail_token.store(message.x, std::memory_order_relaxed);
      player->thumbnail_sent.store(false, std::memory_order_release);
      player->thumbnail_mode.store(true, std::memory_order_release);
      ConfigureVideo(player, uri, 360, 220);
      g_object_set(player->playbin, "uri", uri, nullptr);
      g_free(uri);
      gst_element_set_state(player->playbin, GST_STATE_PLAYING);
      break;
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
      if (player->pending.load(std::memory_order_acquire) &&
          message.sequence ==
              player->sequence.load(std::memory_order_relaxed))
        player->pending.store(false, std::memory_order_release);
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
  if (!player.loop || !player.playbin || !appsink || !audio)
    return 78;
  player.appsink = appsink;
  GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA",
      "width", G_TYPE_INT, kLandscapeWidth,
      "height", G_TYPE_INT, kLandscapeHeight,
      "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr);
  g_object_set(appsink, "caps", caps, "emit-signals", TRUE, "drop", TRUE,
               "max-buffers", 1U, "sync", TRUE, nullptr);
  gst_caps_unref(caps);
  g_signal_connect(appsink, "new-sample", G_CALLBACK(VideoSample), &player);
  g_object_set(player.playbin, "video-sink", appsink,
               "audio-sink", audio, nullptr);
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
