// FreeSWITCH glue only: parameter parsing, media-bug plumbing, event emission.
// Every decision that can be tested without FreeSWITCH lives in core/
// (CONSTITUTION Article 7.3).
#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "audiofork/config.hpp"
#include "audiofork/fork_session.hpp"
#include "audiofork/ports.hpp"
#include "audiofork/slab_pool.hpp"
#include "audiofork_net/shard_pool.hpp"

using audiofork::AudioFormat;
using audiofork::Clock;
using audiofork::ConstByteSpan;
using audiofork::Endpoint;
using audiofork::EventSink;
using audiofork::ForkEvent;
using audiofork::ForkEventType;
using audiofork::ForkParams;
using audiofork::ForkSession;
using audiofork::MixType;
using audiofork::ModuleConfig;
using audiofork::MutableByteSpan;
using audiofork::SlabPool;
using audiofork::SystemClock;
using audiofork::net::ShardPool;

namespace {

constexpr const char* kEventSubclassPrefix = "mod_audio_fork::";
constexpr const char* kPrivateKey = "mod_audio_fork";
constexpr const char* kDtmfHookKey = "mod_audio_fork_dtmf";

// Only its address matters: the channel private table stores pointers, and this
// one is never dereferenced.
int g_dtmf_hook_sentinel = 0;

const char* EventName(ForkEventType type) {
  switch (type) {
    case ForkEventType::kConnect:
      return "connect";
    case ForkEventType::kConnectFailed:
      return "connect_failed";
    case ForkEventType::kStartFailed:
      return "start_failed";
    case ForkEventType::kReconnecting:
      return "reconnecting";
    case ForkEventType::kResume:
      return "resume";
    case ForkEventType::kOverrun:
      return "overrun";
    case ForkEventType::kDegraded:
      return "degraded";
    case ForkEventType::kJson:
      return "json";
    case ForkEventType::kJsonError:
      return "json_error";
    case ForkEventType::kDisconnect:
      return "disconnect";
    case ForkEventType::kStop:
      return "stop";
    case ForkEventType::kError:
      return "error";
    case ForkEventType::kPlaybackStart:
      return "playback_start";
    case ForkEventType::kPlaybackStop:
      return "playback_stop";
    case ForkEventType::kPlaybackCleared:
      return "playback_cleared";
    case ForkEventType::kMark:
      return "mark";
  }
  return "error";
}

class FsEventSink : public EventSink {
 public:
  void Emit(const ForkEvent& event) override {
    switch_event_t* fs_event = nullptr;
    const std::string subclass = std::string(kEventSubclassPrefix) + EventName(event.type);
    if (switch_event_create_subclass(&fs_event, SWITCH_EVENT_CUSTOM, subclass.c_str()) !=
        SWITCH_STATUS_SUCCESS) {
      return;
    }
    switch_event_add_header_string(fs_event, SWITCH_STACK_BOTTOM, "Unique-ID",
                                   event.call_uuid.c_str());
    switch_event_add_header_string(fs_event, SWITCH_STACK_BOTTOM, "Fork-ID", event.fork_id.c_str());
    if (!event.detail.empty()) {
      switch_event_add_header_string(fs_event, SWITCH_STACK_BOTTOM, "Detail", event.detail.c_str());
    }
    if (event.gap_ms != 0) {
      switch_event_add_header(fs_event, SWITCH_STACK_BOTTOM, "Gap-Ms", "%llu",
                              static_cast<unsigned long long>(event.gap_ms));
    }
    if (event.dropped_ms != 0) {
      switch_event_add_header(fs_event, SWITCH_STACK_BOTTOM, "Dropped-Ms", "%llu",
                              static_cast<unsigned long long>(event.dropped_ms));
    }
    switch_event_fire(&fs_event);
  }
};

// Per-media-bug state. Owned by the bug's user_data (a raw pointer, the C ABI's
// currency) and destroyed only on the bug's CLOSE callback.
struct ForkBug {
  std::shared_ptr<ForkSession> session;
  AudioFormat wire_format;
  std::uint32_t session_rate = 0;
  switch_audio_resampler_t* resampler = nullptr;
  switch_audio_resampler_t* playback_resampler = nullptr;
  std::uint32_t playback_resampler_rate = 0;
  std::vector<std::int16_t> scratch;
  std::vector<std::int16_t> playback_scratch;
};

// The composition root: exactly one of each, created at load and destroyed at
// unload in reverse order (CONSTITUTION Article 4.1-4.2).
struct ModuleState {
  ModuleConfig config;
  std::unique_ptr<SlabPool> pool;
  std::unique_ptr<ShardPool> shards;
  FsEventSink events;
  SystemClock clock;

  std::mutex registry_mutex;
  std::unordered_map<std::string, std::vector<std::shared_ptr<ForkSession>>> registry;
  std::atomic<std::uint64_t> start_failed{0};
};

ModuleState* g_state = nullptr;

ModuleConfig LoadConfig() {
  ModuleConfig config;
  switch_xml_t xml = nullptr;
  switch_xml_t cfg = nullptr;
  if ((xml = switch_xml_open_cfg("audio_fork.conf", &cfg, nullptr)) == nullptr) {
    return audiofork::SanitizeConfig(config);
  }
  if (switch_xml_t settings = switch_xml_child(cfg, "settings"); settings != nullptr) {
    for (switch_xml_t param = switch_xml_child(settings, "param"); param != nullptr;
         param = param->next) {
      const char* name = switch_xml_attr_soft(param, "name");
      const char* value = switch_xml_attr_soft(param, "value");
      if (name == nullptr || value == nullptr) {
        continue;
      }
      const int number = atoi(value);
      if (!strcasecmp(name, "shard-count")) {
        config.shard_count = static_cast<std::size_t>(std::max(number, 0));
      } else if (!strcasecmp(name, "send-buffer-seconds")) {
        config.send_buffer = std::chrono::milliseconds{number * 1000};
      } else if (!strcasecmp(name, "playback-high-watermark-ms")) {
        config.playback_high_watermark = std::chrono::milliseconds{number};
      } else if (!strcasecmp(name, "playback-low-watermark-ms")) {
        config.playback_low_watermark = std::chrono::milliseconds{number};
      } else if (!strcasecmp(name, "coalesce-max-ms")) {
        config.coalesce_max = std::chrono::milliseconds{number};
      } else if (!strcasecmp(name, "emergency-buffer-seconds")) {
        config.emergency_buffer = std::chrono::milliseconds{number * 1000};
      } else if (!strcasecmp(name, "global-memory-cap-mb")) {
        config.global_memory_cap_bytes =
            static_cast<std::size_t>(std::max(number, 1)) * 1024 * 1024;
      } else if (!strcasecmp(name, "reconnect-backoff-min-ms")) {
        config.reconnect_min = std::chrono::milliseconds{number};
      } else if (!strcasecmp(name, "reconnect-backoff-max-ms")) {
        config.reconnect_max = std::chrono::milliseconds{number};
      } else if (!strcasecmp(name, "max-forks-per-call")) {
        config.max_forks_per_call = static_cast<std::size_t>(std::max(number, 1));
      } else if (!strcasecmp(name, "tls-ca-file")) {
        config.tls.ca_file = value;
      } else if (!strcasecmp(name, "tls-cert-file")) {
        config.tls.cert_file = value;
      } else if (!strcasecmp(name, "tls-key-file")) {
        config.tls.key_file = value;
      } else if (!strcasecmp(name, "tls-verify")) {
        config.tls.verify = switch_true(value) != 0;
      }
    }
  }
  switch_xml_free(xml);
  const bool half_a_client_certificate =
      config.tls.cert_file.empty() != config.tls.key_file.empty();
  const ModuleConfig sanitized = audiofork::SanitizeConfig(config);
  if (half_a_client_certificate) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                      "tls-cert-file and tls-key-file must both be set; "
                      "continuing without a client certificate\n");
  }
  if (!sanitized.tls.verify) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                      "tls-verify is off: wss:// forks accept any server certificate\n");
  }
  return sanitized;
}

bool ParseEndpoint(const char* url, Endpoint& endpoint, std::string& error) {
  if (url == nullptr) {
    error = "missing url";
    return false;
  }
  const std::optional<Endpoint> parsed = audiofork::ParseWsUrl(url, error);
  if (!parsed.has_value()) {
    return false;
  }
  endpoint = *parsed;
  return true;
}

bool ParseMixType(const char* text, MixType& mix, std::string& error) {
  if (text == nullptr || !strcasecmp(text, "mono")) {
    mix = MixType::kMono;
    return true;
  }
  if (!strcasecmp(text, "mixed")) {
    mix = MixType::kMixed;
    return true;
  }
  if (!strcasecmp(text, "stereo")) {
    mix = MixType::kStereo;
    return true;
  }
  error = "mix-type must be mono, mixed or stereo";
  return false;
}

ForkSession::Tuning MakeTuning(const ModuleConfig& config, const AudioFormat& format) {
  ForkSession::Tuning tuning;
  tuning.send_cap_bytes = format.BytesForDuration(config.send_buffer);
  tuning.handoff_bytes = format.BytesForDuration(config.handoff_buffer);
  tuning.coalesce_max_bytes = format.BytesForDuration(config.coalesce_max);
  tuning.emergency_cap_bytes = format.BytesForDuration(config.emergency_buffer);
  tuning.drain_timeout = config.drain_timeout;
  tuning.backoff = {config.reconnect_min, config.reconnect_max, 2.0, 0.25};
  // playback is sized in mono at the fork rate: the server sends one stream for
  // the caller's ear regardless of how many channels we fork out
  const AudioFormat playback_format{format.sample_rate, 1};
  tuning.playback_high_watermark_bytes =
      playback_format.BytesForDuration(config.playback_high_watermark);
  tuning.playback_low_watermark_bytes =
      playback_format.BytesForDuration(config.playback_low_watermark);
  tuning.playback_handoff_bytes = playback_format.BytesForDuration(config.coalesce_max);
  return tuning;
}

// Playback arrives at whatever rate the server declared; FreeSWITCH needs the
// session rate. Rebuilt if the server changes rate mid-call.
bool EnsurePlaybackResampler(ForkBug& bug, std::uint32_t from_rate) {
  if (from_rate == bug.session_rate) {
    if (bug.playback_resampler != nullptr) {
      switch_resample_destroy(&bug.playback_resampler);
      bug.playback_resampler_rate = 0;
    }
    return true;
  }
  if (bug.playback_resampler != nullptr && bug.playback_resampler_rate == from_rate) {
    return true;
  }
  if (bug.playback_resampler != nullptr) {
    switch_resample_destroy(&bug.playback_resampler);
  }
  if (switch_resample_create(
          &bug.playback_resampler, static_cast<int>(from_rate), static_cast<int>(bug.session_rate),
          static_cast<uint32_t>(bug.playback_scratch.size() * sizeof(std::int16_t)),
          SWITCH_RESAMPLE_QUALITY, 1) != SWITCH_STATUS_SUCCESS) {
    bug.playback_resampler_rate = 0;
    return false;
  }
  bug.playback_resampler_rate = from_rate;
  return true;
}

void PushResampled(ForkBug& bug, const std::int16_t* samples, std::size_t sample_count) {
  if (bug.resampler == nullptr) {
    // a rejected push is counted as a media drop inside the session
    (void)bug.session->PushAudio(ConstByteSpan(reinterpret_cast<const std::uint8_t*>(samples),
                                               sample_count * sizeof(std::int16_t)));
    return;
  }
  switch_resample_process(bug.resampler, const_cast<std::int16_t*>(samples),
                          static_cast<uint32_t>(sample_count));
  if (bug.resampler->to_len == 0) {
    return;
  }
  (void)bug.session->PushAudio(
      ConstByteSpan(reinterpret_cast<const std::uint8_t*>(bug.resampler->to),
                    static_cast<std::size_t>(bug.resampler->to_len) * sizeof(std::int16_t)));
}

switch_bool_t OnMediaBug(switch_media_bug_t* bug, void* user_data, switch_abc_type_t type) {
  auto* state = static_cast<ForkBug*>(user_data);
  switch (type) {
    case SWITCH_ABC_TYPE_INIT:
      return SWITCH_TRUE;

    case SWITCH_ABC_TYPE_READ: {
      switch_frame_t frame = {};
      frame.data = state->scratch.data();
      frame.buflen = static_cast<uint32_t>(state->scratch.size() * sizeof(std::int16_t));
      while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
        if (frame.datalen == 0) {
          break;
        }
        PushResampled(*state, static_cast<const std::int16_t*>(frame.data),
                      frame.datalen / sizeof(std::int16_t));
      }
      return SWITCH_TRUE;
    }

    case SWITCH_ABC_TYPE_WRITE_REPLACE: {
      if (!state->session->playback_enabled()) {
        return SWITCH_TRUE;
      }
      switch_frame_t* frame = switch_core_media_bug_get_write_replace_frame(bug);
      if (frame == nullptr || frame->data == nullptr || frame->datalen == 0) {
        return SWITCH_TRUE;
      }
      const AudioFormat format = state->session->playback_format();
      if (!EnsurePlaybackResampler(*state, format.sample_rate)) {
        return SWITCH_TRUE;
      }

      if (state->playback_resampler == nullptr) {
        // rates match: fill in place, leaving any tail untouched so live call
        // audio is never overwritten with silence
        const std::size_t filled = state->session->ReadPlayback(
            MutableByteSpan(static_cast<std::uint8_t*>(frame->data), frame->datalen));
        if (filled > 0) {
          switch_core_media_bug_set_write_replace_frame(bug, frame);
        }
        return SWITCH_TRUE;
      }

      const std::size_t wanted = std::max<std::size_t>(
          (static_cast<std::size_t>(frame->datalen) * format.sample_rate) / state->session_rate,
          sizeof(std::int16_t));
      const std::size_t capacity = state->playback_scratch.size() * sizeof(std::int16_t);
      const std::size_t filled = state->session->ReadPlayback(
          MutableByteSpan(reinterpret_cast<std::uint8_t*>(state->playback_scratch.data()),
                          std::min(wanted, capacity)));
      if (filled == 0) {
        return SWITCH_TRUE;
      }
      switch_resample_process(state->playback_resampler, state->playback_scratch.data(),
                              static_cast<uint32_t>(filled / sizeof(std::int16_t)));
      if (state->playback_resampler->to_len == 0) {
        return SWITCH_TRUE;
      }
      const std::size_t produced = std::min(
          static_cast<std::size_t>(state->playback_resampler->to_len) * sizeof(std::int16_t),
          static_cast<std::size_t>(frame->datalen));
      std::memcpy(frame->data, state->playback_resampler->to, produced);
      switch_core_media_bug_set_write_replace_frame(bug, frame);
      return SWITCH_TRUE;
    }

    case SWITCH_ABC_TYPE_CLOSE:
      // the media bug's reference: stop the fork, then let the shard's
      // reference carry it through close (DESIGN.md §4)
      if (state->session) {
        state->session->Stop();
      }
      if (state->resampler != nullptr) {
        switch_resample_destroy(&state->resampler);
      }
      if (state->playback_resampler != nullptr) {
        switch_resample_destroy(&state->playback_resampler);
      }
      delete state;
      return SWITCH_TRUE;

    default:
      return SWITCH_TRUE;
  }
}

void ForgetFork(const std::string& uuid, const std::shared_ptr<ForkSession>& session) {
  const std::scoped_lock lock(g_state->registry_mutex);
  auto it = g_state->registry.find(uuid);
  if (it == g_state->registry.end()) {
    return;
  }
  auto& forks = it->second;
  forks.erase(std::remove(forks.begin(), forks.end(), session), forks.end());
  if (forks.empty()) {
    g_state->registry.erase(it);
  }
}

// Runs on a FreeSWITCH session thread: SendDtmf hops to the shard, so nothing
// here touches lws. The hook outlives the forks it was installed for —
// FreeSWITCH only drops it when it destroys the session — so an empty registry
// is the normal quiet case, not an error.
switch_status_t OnRecvDtmf(switch_core_session_t* session, const switch_dtmf_t* dtmf,
                           switch_dtmf_direction_t direction) noexcept {
  (void)direction;
  if (g_state == nullptr || session == nullptr || dtmf == nullptr) {
    return SWITCH_STATUS_SUCCESS;
  }
  try {
    std::vector<std::shared_ptr<ForkSession>> forks;
    {
      const std::scoped_lock lock(g_state->registry_mutex);
      const auto it = g_state->registry.find(switch_core_session_get_uuid(session));
      if (it == g_state->registry.end()) {
        return SWITCH_STATUS_SUCCESS;
      }
      forks = it->second;
    }
    // FreeSWITCH counts DTMF duration in samples on a fixed 8kHz clock
    // (SWITCH_DEFAULT_DTMF_DURATION = 2000 samples = 250ms)
    const std::uint32_t duration_ms = dtmf->duration / 8;
    for (const auto& fork : forks) {
      (void)fork->SendDtmf(dtmf->digit, duration_ms);
    }
  } catch (...) {
    // no exception may cross the C ABI (CONSTITUTION Article 6.1)
  }
  // always success: the other consumers of this digit must still see it
  return SWITCH_STATUS_SUCCESS;
}

// fork_id stays empty: the fork never got far enough to have one.
void EmitStartFailed(const std::string& uuid, const std::string& reason) {
  g_state->start_failed.fetch_add(1, std::memory_order_relaxed);
  ForkEvent event;
  event.type = ForkEventType::kStartFailed;
  event.call_uuid = uuid;
  event.detail = reason;
  g_state->events.Emit(event);
}

switch_status_t StartFork(switch_core_session_t* session, const char* url, const char* mix_text,
                          const char* rate_text, const char* metadata, std::string& error) {
  switch_channel_t* channel = switch_core_session_get_channel(session);
  const std::string uuid = switch_core_session_get_uuid(session);

  Endpoint endpoint;
  MixType mix = MixType::kMono;
  if (!ParseEndpoint(url, endpoint, error) || !ParseMixType(mix_text, mix, error)) {
    return SWITCH_STATUS_FALSE;
  }

  {
    const std::scoped_lock lock(g_state->registry_mutex);
    const auto it = g_state->registry.find(uuid);
    if (it != g_state->registry.end() && it->second.size() >= g_state->config.max_forks_per_call) {
      error = "max forks per call reached";
      return SWITCH_STATUS_FALSE;
    }
  }

  switch_codec_implementation_t read_impl = {};
  switch_core_session_get_read_impl(session, &read_impl);
  const std::uint32_t session_rate = read_impl.actual_samples_per_second;

  AudioFormat wire_format;
  wire_format.sample_rate = rate_text != nullptr && atoi(rate_text) > 0
                                ? static_cast<std::uint32_t>(atoi(rate_text))
                                : session_rate;
  wire_format.channels = mix == MixType::kStereo ? 2 : 1;

  ForkParams params;
  params.call_uuid = uuid;
  params.fork_id =
      switch_core_session_sprintf(session, "%s-%d", uuid.c_str(), switch_epoch_time_now(nullptr));
  params.endpoint = endpoint;
  params.format = wire_format;
  params.mix_type = mix;
  params.metadata_json = metadata == nullptr ? "" : metadata;

  const ForkSession::Tuning tuning = MakeTuning(g_state->config, wire_format);
  if (!g_state->pool->CanLease(
          ForkSession::MinimumSlabs(tuning, g_state->pool->stats().slab_size_bytes))) {
    error = "global memory cap reached";
    EmitStartFailed(uuid, error);
    return SWITCH_STATUS_FALSE;
  }

  auto fork = g_state->shards->StartFork(params, tuning, g_state->events, g_state->clock);
  if (fork == nullptr) {
    error = "fork could not be created";
    EmitStartFailed(uuid, error);
    return SWITCH_STATUS_FALSE;
  }
  fork->set_on_finished([uuid, weak = std::weak_ptr<ForkSession>(fork)] {
    if (auto finished = weak.lock()) {
      ForgetFork(uuid, finished);
    }
  });

  auto bug_state = std::make_unique<ForkBug>();
  bug_state->session = fork;
  bug_state->wire_format = wire_format;
  bug_state->session_rate = session_rate;
  bug_state->scratch.resize(SWITCH_RECOMMENDED_BUFFER_SIZE / sizeof(std::int16_t));
  bug_state->playback_scratch.resize(SWITCH_RECOMMENDED_BUFFER_SIZE / sizeof(std::int16_t));
  if (wire_format.sample_rate != session_rate) {
    if (switch_resample_create(
            &bug_state->resampler, session_rate, wire_format.sample_rate,
            static_cast<uint32_t>(bug_state->scratch.size() * sizeof(std::int16_t)),
            SWITCH_RESAMPLE_QUALITY, 1) != SWITCH_STATUS_SUCCESS) {
      fork->Stop();
      error = "resampler could not be created";
      return SWITCH_STATUS_FALSE;
    }
  }

  // WRITE_REPLACE is what puts server audio in the caller's ear
  switch_media_bug_flag_t flags = SMBF_READ_STREAM | SMBF_WRITE_REPLACE;
  if (mix != MixType::kMono) {
    flags |= SMBF_WRITE_STREAM;
  }
  if (mix == MixType::kStereo) {
    flags |= SMBF_STEREO;
  }

  switch_media_bug_t* bug = nullptr;
  ForkBug* raw_bug_state = bug_state.get();
  if (switch_core_media_bug_add(session, "audio_fork", nullptr, OnMediaBug, raw_bug_state, 0, flags,
                                &bug) != SWITCH_STATUS_SUCCESS) {
    fork->Stop();
    error = "media bug could not be added";
    return SWITCH_STATUS_FALSE;
  }
  // ownership handed to the bug: released in its CLOSE callback
  (void)bug_state.release();
  switch_channel_set_private(channel, kPrivateKey, bug);

  // one hook per channel, however many forks the call carries
  if (switch_channel_get_private(channel, kDtmfHookKey) == nullptr &&
      switch_core_event_hook_add_recv_dtmf(session, OnRecvDtmf) == SWITCH_STATUS_SUCCESS) {
    switch_channel_set_private(channel, kDtmfHookKey, &g_dtmf_hook_sentinel);
  }

  {
    const std::scoped_lock lock(g_state->registry_mutex);
    g_state->registry[uuid].push_back(std::move(fork));
  }
  return SWITCH_STATUS_SUCCESS;
}

switch_status_t StopForks(switch_core_session_t* session) {
  switch_channel_t* channel = switch_core_session_get_channel(session);
  const std::string uuid = switch_core_session_get_uuid(session);

  std::vector<std::shared_ptr<ForkSession>> forks;
  {
    const std::scoped_lock lock(g_state->registry_mutex);
    if (const auto it = g_state->registry.find(uuid); it != g_state->registry.end()) {
      forks = it->second;
    }
  }
  for (auto& fork : forks) {
    fork->Stop();
  }
  if (auto* bug =
          static_cast<switch_media_bug_t*>(switch_channel_get_private(channel, kPrivateKey));
      bug != nullptr) {
    switch_channel_set_private(channel, kPrivateKey, nullptr);
    switch_core_media_bug_remove(session, &bug);
  }
  if (switch_channel_get_private(channel, kDtmfHookKey) != nullptr) {
    switch_channel_set_private(channel, kDtmfHookKey, nullptr);
    (void)switch_core_event_hook_remove_recv_dtmf(session, OnRecvDtmf);
  }
  // forks that already retired on their own still leave the channel to clean
  // up, but there was nothing here to stop
  return forks.empty() ? SWITCH_STATUS_FALSE : SWITCH_STATUS_SUCCESS;
}

// switch_separate_string cuts the JSON payload at its first space, so the
// payload is read from the untouched command line instead of from argv.
const char* SendTextPayload(const char* cmd) {
  const char* cursor = cmd;
  for (int token = 0; token < 2; ++token) {
    while (*cursor == ' ') {
      ++cursor;
    }
    while (*cursor != '\0' && *cursor != ' ') {
      ++cursor;
    }
  }
  while (*cursor == ' ') {
    ++cursor;
  }
  return cursor;
}

switch_status_t SendTextToForks(switch_core_session_t* session, const char* payload,
                                std::string& error) {
  if (payload == nullptr || *payload == '\0' || !nlohmann::json::accept(payload)) {
    error = "send_text payload must be valid JSON";
    return SWITCH_STATUS_FALSE;
  }

  std::vector<std::shared_ptr<ForkSession>> forks;
  {
    const std::scoped_lock lock(g_state->registry_mutex);
    const auto it = g_state->registry.find(switch_core_session_get_uuid(session));
    if (it == g_state->registry.end()) {
      error = "no fork running on this channel";
      return SWITCH_STATUS_FALSE;
    }
    forks = it->second;
  }

  bool delivered = false;
  for (const auto& fork : forks) {
    delivered = fork->SendText(payload) || delivered;
  }
  if (!delivered) {
    error = "fork is stopping";
    return SWITCH_STATUS_FALSE;
  }
  return SWITCH_STATUS_SUCCESS;
}

}  // namespace

extern "C" {

SWITCH_MODULE_LOAD_FUNCTION(mod_audio_fork_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_fork_shutdown);
SWITCH_MODULE_DEFINITION(mod_audio_fork, mod_audio_fork_load, mod_audio_fork_shutdown, nullptr);

#define AUDIO_FORK_API_SYNTAX \
  "<uuid> start <wss-url> <mix-type> <rate> [metadata] | <uuid> stop | <uuid> send_text <json>"

SWITCH_STANDARD_API(uuid_audio_fork_api) {
  (void)session;
  if (zstr(cmd)) {
    stream->write_function(stream, "-ERR usage: %s\n", AUDIO_FORK_API_SYNTAX);
    return SWITCH_STATUS_SUCCESS;
  }
  char* mycmd = strdup(cmd);
  std::array<char*, 6> argv{};
  const int argc =
      switch_separate_string(mycmd, ' ', argv.data(), static_cast<unsigned>(argv.size()));
  if (argc < 2) {
    stream->write_function(stream, "-ERR usage: %s\n", AUDIO_FORK_API_SYNTAX);
    free(mycmd);
    return SWITCH_STATUS_SUCCESS;
  }

  switch_core_session_t* target = switch_core_session_locate(argv[0]);
  if (target == nullptr) {
    stream->write_function(stream, "-ERR no such channel %s\n", argv[0]);
    free(mycmd);
    return SWITCH_STATUS_SUCCESS;
  }

  std::string error;
  switch_status_t status = SWITCH_STATUS_FALSE;
  if (!strcasecmp(argv[1], "start")) {
    status = StartFork(target, argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr,
                       argc > 4 ? argv[4] : nullptr, argc > 5 ? argv[5] : nullptr, error);
  } else if (!strcasecmp(argv[1], "stop")) {
    status = StopForks(target);
    if (status != SWITCH_STATUS_SUCCESS) {
      error = "no fork running on this channel";
    }
  } else if (!strcasecmp(argv[1], "send_text")) {
    status = SendTextToForks(target, SendTextPayload(cmd), error);
  } else {
    error = "unknown subcommand";
  }

  switch_core_session_rwunlock(target);
  free(mycmd);

  if (status == SWITCH_STATUS_SUCCESS) {
    stream->write_function(stream, "+OK\n");
  } else {
    stream->write_function(stream, "-ERR %s\n", error.empty() ? "failed" : error.c_str());
  }
  return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(audio_fork_status_api) {
  (void)cmd;
  (void)session;
  std::size_t calls = 0;
  std::size_t forks = 0;
  std::uint64_t media_dropped = 0;
  std::uint64_t buffer_dropped = 0;
  std::uint64_t sent = 0;
  std::uint64_t reconnects = 0;
  std::size_t buffered = 0;
  std::uint64_t playback_played = 0;
  std::uint64_t barge_ins = 0;
  std::uint64_t texts_dropped = 0;
  {
    const std::scoped_lock lock(g_state->registry_mutex);
    calls = g_state->registry.size();
    for (const auto& [uuid, sessions] : g_state->registry) {
      forks += sessions.size();
      for (const auto& fork : sessions) {
        const ForkSession::Stats stats = fork->stats();
        media_dropped += stats.media_dropped_bytes;
        buffer_dropped += stats.buffer_dropped_bytes;
        sent += stats.sent_bytes;
        reconnects += stats.reconnects;
        buffered += stats.buffered_bytes;
        playback_played += stats.playback_bytes_played;
        barge_ins += stats.barge_ins;
        texts_dropped += stats.pending_texts_dropped;
      }
    }
  }
  const SlabPool::Stats pool = g_state->pool->stats();
  stream->write_function(
      stream,
      "{\"calls\":%lu,\"forks\":%lu,\"shards\":%lu,\"sent_bytes\":%llu,"
      "\"media_dropped_bytes\":%llu,\"buffer_dropped_bytes\":%llu,\"reconnects\":%llu,"
      "\"buffered_bytes\":%lu,\"playback_bytes_played\":%llu,\"barge_ins\":%llu,"
      "\"pending_texts_dropped\":%llu,"
      "\"pool_allocated_bytes\":%lu,\"pool_leased_slabs\":%lu}\n",
      static_cast<unsigned long>(calls), static_cast<unsigned long>(forks),
      static_cast<unsigned long>(g_state->shards->shard_count()),
      static_cast<unsigned long long>(sent), static_cast<unsigned long long>(media_dropped),
      static_cast<unsigned long long>(buffer_dropped), static_cast<unsigned long long>(reconnects),
      static_cast<unsigned long>(buffered), static_cast<unsigned long long>(playback_played),
      static_cast<unsigned long long>(barge_ins), static_cast<unsigned long long>(texts_dropped),
      static_cast<unsigned long>(pool.allocated_bytes),
      static_cast<unsigned long>(pool.leased_slabs));
  return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_audio_fork_load) {
  *module_interface = switch_loadable_module_create_module_interface(pool, modname);

  auto state = std::make_unique<ModuleState>();
  state->config = LoadConfig();

  auto slabs =
      SlabPool::Create({state->config.slab_size_bytes, state->config.global_memory_cap_bytes});
  if (!slabs.has_value()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                      "mod_audio_fork: slab pool could not be created\n");
    return SWITCH_STATUS_FALSE;
  }
  state->pool = std::make_unique<SlabPool>(*std::move(slabs));
  state->shards = ShardPool::Start(state->config, *state->pool);
  if (state->shards == nullptr) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                      "mod_audio_fork: shard pool could not be started\n");
    return SWITCH_STATUS_FALSE;
  }

  switch_api_interface_t* api = nullptr;
  SWITCH_ADD_API(api, "uuid_audio_fork", "fork call audio to a websocket server",
                 uuid_audio_fork_api, AUDIO_FORK_API_SYNTAX);
  SWITCH_ADD_API(api, "audio_fork", "mod_audio_fork counters as JSON", audio_fork_status_api,
                 "status");
  switch_console_set_complete("add uuid_audio_fork ::console::list_uuid start");
  switch_console_set_complete("add uuid_audio_fork ::console::list_uuid stop");

  g_state = state.release();
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                    "mod_audio_fork loaded: %lu shards, %lus send buffer\n",
                    static_cast<unsigned long>(g_state->shards->shard_count()),
                    static_cast<unsigned long>(g_state->config.send_buffer.count() / 1000));
  return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_fork_shutdown) {
  if (g_state == nullptr) {
    return SWITCH_STATUS_SUCCESS;
  }
  {
    const std::scoped_lock lock(g_state->registry_mutex);
    for (auto& [uuid, sessions] : g_state->registry) {
      for (auto& fork : sessions) {
        fork->Stop();
      }
    }
  }
  // reverse construction order: shards (and their threads) before the pool
  // they lease slabs from
  std::unique_ptr<ModuleState> state(g_state);
  g_state = nullptr;
  state->shards.reset();
  state->pool.reset();
  return SWITCH_STATUS_SUCCESS;
}

}  // extern "C"
