// Addresses and code bytes of the supported game, retail TH10 1.00a.
#pragma once

#include <cstddef>
#include <cstdint>

namespace th10hook::retail {

inline constexpr char kExecutableSha256[] =
    "2f14760b6fbbf57549541583283badb9a19a4222b90f0a146d5aa17f01dc9040";
inline constexpr std::uint32_t kImageBase = 0x00400000;
inline constexpr std::uint32_t kPlayerAdapterRva = 0x00026500;
inline constexpr std::uint8_t kPlayerAdapterBytes[] = {0x51, 0xe8, 0x2a, 0xf2,
                                                       0xff, 0xff, 0xc3};

// Global variables.
inline constexpr std::uintptr_t kPlayerPointer = 0x00477834;
inline constexpr std::uintptr_t kReplayManagerPointer = 0x00477838;
inline constexpr std::uintptr_t kInputWords = 0x00474e5c;
// Eligible game-manager updates since the last stage reset.
inline constexpr std::uintptr_t kStageTick = 0x00474c88;
// The same count, also cleared when the game's active record changes.
inline constexpr std::uintptr_t kSegmentTick = 0x00474c8c;
inline constexpr std::uintptr_t kLives = 0x00474c70;
inline constexpr std::uintptr_t kPower = 0x00474c48;
inline constexpr std::uintptr_t kGameModeFlags = 0x00474ca0;
inline constexpr std::uintptr_t kBombManagerPointer = 0x004776ec;
inline constexpr std::uintptr_t kBulletManagerPointer = 0x004776f0;
inline constexpr std::uintptr_t kBossManagerPointer = 0x00477704;
inline constexpr std::uintptr_t kLaserManagerPointer = 0x0047781c;
inline constexpr std::uintptr_t kGameManagerPointer = 0x00477810;
inline constexpr std::uintptr_t kMenuOwnerPointer = 0x00477830;

// Fields of the player object.
inline constexpr std::uint32_t kPlayerX = 0x3c0;
inline constexpr std::uint32_t kPlayerY = 0x3c4;
inline constexpr std::uint32_t kPlayerState = 0x458;
inline constexpr std::uint32_t kPlayerProtectionTimer = 0x4310;
inline constexpr std::uint32_t kPlayerObjectSize = 0x4314;
// The replay manager's mode field. Zero means live play.
inline constexpr std::uint32_t kReplayMode = 0x10;

struct Hook {
  std::uint32_t rva;
  std::uint8_t original_bytes[5];
  const char* wrapper;
  const char* name;
};

inline constexpr Hook kHooks[] = {
    {0x3942f,
     {0xe8, 0xcc, 0x07, 0x01, 0x00},
     "Th10CalcChainHookWrapper",
     "Render calc-chain CALL 0x43942F"},
    {0x2a3d3,
     {0xe8, 0xf8, 0xf3, 0xff, 0xff},
     "Th10ReplayAdapterHookWrapper",
     "Replay adapter CALL 0x42A3D3"},
    {0x26501,
     {0xe8, 0x2a, 0xf2, 0xff, 0xff},
     "Th10PlayerAdapterHookWrapper",
     "Player adapter CALL 0x426501"},
    {0x38d31,
     {0xe8, 0x5a, 0x06, 0x00, 0x00},
     "Th10WholeTickStepWrapper",
     "Whole-tick step CALL 0x438D31"},
};

}  // namespace th10hook::retail
