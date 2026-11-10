#pragma once

#include "hook_status.h"
#include "tick_gate.h"

#include <cstdint>
#include <cstring>

namespace th10hook {

using th10hook::CallbackSample;

// Check that one original whole tick ran the expected callbacks in order.
class TickVerifier {
 public:
  void begin() { std::memset(this, 0, sizeof(*this)); }

  void observe(const CallbackSample& record) {
    if (record.callback_kind == kCallbackCalcPre) {
      observe_calc_pre(record);
      return;
    }
    if (!has_calc_pre || record.thread_id != calc_pre.thread_id) return;

    // CalcPost shares CalcPre's parent. Replay and Player samples are children
    // of this Calc callback.
    if (record.callback_kind == kCallbackCalcPost) {
      observe_calc_post(record);
      return;
    }
    if (record.parent_callback_id != calc_pre.callback_id) return;
    switch (record.callback_kind) {
      case kCallbackReplayPre:
        retain_first(record, replay_pre, replay_pre_count);
        break;
      case kCallbackReplayPost:
        retain_first(record, replay_post, replay_post_count);
        break;
      case kCallbackPlayerPre:
        retain_first(record, player_pre, player_pre_count);
        break;
      case kCallbackPlayerPost:
        retain_first(record, player_post, player_post_count);
        break;
    }
  }

  bool complete_chain() {
    // The game can clear the segment tick mid-stage. The stage tick and the
    // full callback chain must still show one gameplay tick.
    return valid_active_callbacks() &&
           calc_pre.parent_callback_id == calc_post.parent_callback_id &&
           (calc_post.segment_tick - calc_pre.segment_tick == 1 ||
            calc_post.segment_tick == 0);
  }

  bool terminal_calc_pair() const {
    return has_calc_pre && has_calc_post && calc_pre_count == 1 &&
           !verification_failed && !post_mismatch && !calc_pre.flags &&
           !calc_post.flags && !replay_pre_count && !replay_post_count &&
           !player_pre_count && !player_post_count &&
           calc_pre.sequence + 1 == calc_post.sequence &&
           calc_pre.callback_id != 0 &&
           calc_pre.callback_id == calc_post.callback_id &&
           calc_pre.parent_callback_id == calc_post.parent_callback_id &&
           calc_pre.thread_id != 0 &&
           calc_pre.thread_id == calc_post.thread_id &&
           calc_pre.player_pointer != 0 &&
           calc_pre.player_pointer == calc_post.player_pointer &&
           calc_pre.player_state == calc_post.player_state &&
           calc_pre.stage_tick == calc_post.stage_tick &&
           calc_pre.segment_tick == calc_post.segment_tick &&
           calc_pre.input_current == 0 && calc_post.input_current == 0;
  }

 private:
  void observe_calc_pre(const CallbackSample& record) {
    ++calc_pre_count;
    if (has_calc_pre && !has_calc_post) verification_failed = true;
    calc_pre = record;
    has_calc_pre = true;
    calc_post = {};
    has_calc_post = false;
    replay_pre = replay_post = player_pre = player_post = {};
    replay_pre_count = replay_post_count = 0;
    player_pre_count = player_post_count = 0;
    post_mismatch = false;
  }

  void observe_calc_post(const CallbackSample& record) {
    if (record.callback_id != calc_pre.callback_id ||
        record.parent_callback_id != calc_pre.parent_callback_id) {
      post_mismatch = true;
      if (calc_pre.player_state == 1 || record.player_state == 1)
        verification_failed = true;
      return;
    }
    calc_post = record;
    has_calc_post = true;
  }

  static void retain_first(const CallbackSample& record, CallbackSample& first,
                           std::uint32_t& count) {
    if (++count == 1) first = record;
  }

  bool valid_active_callbacks() const {
    const auto known_state = [](std::int32_t state) {
      return state >= 0 && state <= 4;
    };
    if (!validated_chain() || !known_state(calc_pre.player_state) ||
        !known_state(calc_post.player_state) ||
        !known_state(replay_pre.player_state) ||
        !known_state(replay_post.player_state) ||
        !known_state(player_pre.player_state) ||
        !known_state(player_post.player_state))
      return false;
    return true;
  }

  bool validated_chain() const {
    if (!has_calc_pre || !has_calc_post || calc_pre_count != 1 ||
        post_mismatch || calc_pre.flags || calc_post.flags ||
        !calc_pre.callback_id ||
        calc_pre.callback_id != calc_post.callback_id ||
        calc_pre.thread_id != calc_post.thread_id || !calc_pre.player_pointer ||
        calc_pre.player_pointer != calc_post.player_pointer ||
        calc_post.stage_tick - calc_pre.stage_tick != 1)
      return false;
    if (replay_pre_count != 1 || replay_post_count != 1 ||
        player_pre_count != 1 || player_post_count != 1 || replay_pre.flags ||
        replay_post.flags || player_pre.flags || player_post.flags ||
        !replay_pre.callback_id ||
        replay_pre.callback_id != replay_post.callback_id ||
        !player_pre.callback_id ||
        player_pre.callback_id != player_post.callback_id ||
        replay_pre.parent_callback_id != calc_pre.callback_id ||
        replay_post.parent_callback_id != calc_pre.callback_id ||
        player_pre.parent_callback_id != calc_pre.callback_id ||
        player_post.parent_callback_id != calc_pre.callback_id ||
        replay_pre.thread_id != calc_pre.thread_id ||
        replay_post.thread_id != calc_pre.thread_id ||
        player_pre.thread_id != calc_pre.thread_id ||
        player_post.thread_id != calc_pre.thread_id ||
        player_pre.player_pointer != calc_pre.player_pointer ||
        player_post.player_pointer != calc_post.player_pointer)
      return false;
    return calc_pre.sequence < replay_pre.sequence &&
           replay_pre.sequence < replay_post.sequence &&
           replay_post.sequence < player_pre.sequence &&
           player_pre.sequence < player_post.sequence &&
           player_post.sequence < calc_post.sequence;
  }

 public:
  GateTickState snapshot() const {
    return GateTickState{calc_post.sequence,
                         calc_post.stage_tick,
                         calc_post.segment_tick,
                         calc_post.player_pointer,
                         static_cast<std::uint32_t>(calc_post.player_state),
                         calc_post.player_x,
                         calc_post.player_y};
  }
  std::uint32_t post_sequence() const { return calc_post.sequence; }
  std::uint32_t callback_id() const { return calc_post.callback_id; }

  bool copy_terminal_calc(CallbackSample& calc) const {
    if (!terminal_calc_pair()) return false;
    calc = calc_post;
    return true;
  }

  bool copy_tick_end(th10hook::CallbackSample& calc,
                     std::uint16_t input_words[5]) const {
    if (!has_calc_pre || !has_calc_post || calc_pre_count != 1 ||
        player_pre_count != 1 || player_pre.flags != 0 ||
        player_pre.callback_id == 0 ||
        player_pre.parent_callback_id != calc_pre.callback_id ||
        player_pre.thread_id != calc_pre.thread_id)
      return false;
    calc = calc_post;
    input_words[0] = player_pre.input_current;
    input_words[1] = player_pre.input_previous;
    input_words[2] = player_pre.input_repeated;
    input_words[3] = player_pre.input_pressed_or_aux;
    input_words[4] = player_pre.input_released_or_flags;
    return true;
  }

  bool has_calc_pre;
  bool has_calc_post;
  bool verification_failed;

 private:
  th10hook::CallbackSample calc_pre;
  th10hook::CallbackSample calc_post;
  th10hook::CallbackSample replay_pre;
  th10hook::CallbackSample replay_post;
  th10hook::CallbackSample player_pre;
  th10hook::CallbackSample player_post;
  std::uint32_t replay_pre_count;
  std::uint32_t replay_post_count;
  std::uint32_t player_pre_count;
  std::uint32_t player_post_count;
  std::uint32_t calc_pre_count;
  bool post_mismatch;
};

}  // namespace th10hook
