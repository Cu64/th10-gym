// Capture the game's state at a halted tick and publish it for the Python
// client.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

#include "hook_status.h"
#include "input_channel.h"
#include "snapshot_format.h"
#include "tick_gate.h"

namespace th10hook {

void init_snapshot_capture(SnapshotChannelMapping* channel,
                           InputChannelMapping* input_channel,
                           HANDLE ready_event);
// Check whether the game has reached Game Over or a cleared practice section.
TerminalOutcome read_terminal_outcome();
void publish_halted_snapshot(TickGate& gate, bool first_snapshot,
                             const CallbackSample& calc_post,
                             TerminalOutcome outcome,
                             const std::uint16_t (*consumed_input)[5]);

}  // namespace th10hook
