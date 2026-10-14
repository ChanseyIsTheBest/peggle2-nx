// Counts the frames the game presents, for the video recorder (peggle2_capture.cpp waits for each new one).
//
// D3DDevice_Swap is sub_8261FE50: the only function of default.xex that calls the kernel's VdSwap (found by searching
// the generated code for __imp__VdSwap, 2026-10-05). The hook calls the original and counts; it changes no PPC
// register.
//
// How the wrapping works: each recompiled function sub_XXXXXXXX is a weak alias of __imp__sub_XXXXXXXX. The strong
// definition here wins for every caller (direct, and indirect through the function table), and __imp__ is still the
// original.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "peggle2_capture.h"
#include "peggle2_pc_profiler.h"
#if PEGGLE2_NATIVE_RENDERER
#include "me/me_native_system.h"
#endif

// peggle2_fps = 60 (docs/peggle2-notes.md, "60 frames a second").
//
// The Xbox 360 game presents every second vblank (its Direct3D device's presentation interval is 2), and its frame
// step (the function at 0x82 42F8A8) runs two logic updates of 1/60 s for every frame it draws. Presenting every
// vblank alone would therefore run the game at twice its speed. With the key at 60 the port sets the interval to
// one vblank and the frame step (through tools/fps_updates.py) asks peggle2_fps_updates how many updates the frame
// gets: one for each 1/60 s that passed since the last frame, never fewer than one or more than the game's own two.
// So a frame that took one vblank gets one update, a frame that took two gets two (as on the Xbox 360), and the
// game's speed is the same at 60, at 30 and at anything between; below 30 it slows down as the Xbox 360 game does.
REXCVAR_DEFINE_INT32(peggle2_fps, 30, "Peggle2",
                     "Frames a second the game presents: 30 (the Xbox 360 game: a frame every second vblank, two "
                     "logic updates per frame) or 60 (every vblank, one logic update per 1/60 s that passed: the "
                     "same game speed, twice the pictures)");

namespace {

bool Fps60() {
  static const bool on = REXCVAR_GET(peggle2_fps) >= 60;
  return on;
}

}  // namespace

// Called by the game's frame step before its second logic update (tools/fps_updates.py): 2 = run it, 1 = skip it.
// Game thread only.
extern "C" int peggle2_fps_updates(void) {
  if (!Fps60()) return 2;
  using Clock = std::chrono::steady_clock;
  constexpr double kUpdate = 1.0 / 60.0;
  static Clock::time_point last = Clock::now();
  static double owed = 0.0;  // game time not yet run, in seconds
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("[fps] peggle2_fps = 60: a frame every vblank, one logic update per 1/60 s that passed");
  }
  const Clock::time_point now = Clock::now();
  owed += std::chrono::duration<double>(now - last).count();
  last = now;
  // The frame has run its first update already. A second one when one and a half updates or more are owed (the
  // frame took two vblanks); what stays owed is kept small either way, so a long pause is not caught up later.
  const int updates = owed >= 1.5 * kUpdate ? 2 : 1;
  owed -= updates * kUpdate;
  if (owed > kUpdate) owed = kUpdate;
  if (owed < -kUpdate) owed = -kUpdate;
  // Every 10 s: the frames and the updates of the window (60 updates a second = the game's speed is held).
  static Clock::time_point window = now;
  static uint32_t frames = 0, ran = 0;
  ++frames;
  ran += uint32_t(updates);
  if (const double seconds = std::chrono::duration<double>(now - window).count(); seconds >= 10.0) {
    REXLOG_INFO("[fps] last {:.1f} s: {:.1f} frames and {:.1f} logic updates a second ({} frames with two updates)",
                seconds, frames / seconds, ran / seconds, ran - frames);
    window = now;
    frames = ran = 0;
  }
  return updates;
}

REXCVAR_DEFINE_INT32(peggle2_diag_swap_stack, 0, "Peggle2",
                     "PC diagnostic: log the host call stack (the game's call chain) of this swap; 0 = off");

extern "C" REX_FUNC(__imp__sub_8261FE50);
REX_HOOK_RAW(sub_8261FE50) {  // D3DDevice_Swap
  if (const int32_t at = REXCVAR_GET(peggle2_diag_swap_stack);
      at > 0 && g_peggle2_game_frames.load(std::memory_order_relaxed) == static_cast<uint64_t>(at)) {
    peggle2::pc_profiler::LogHostStack("D3DDevice_Swap");
  }
  // The device's presentation interval (the word at +14000: 2 = every second vblank; the swap reads it when it
  // queues the flip, in the function at 0x82 61F990).
  if (Fps60()) {
    uint8_t* const interval = base + ctx.r3.u32 + 14000;
    const uint8_t two[4] = {0, 0, 0, 2}, one[4] = {0, 0, 0, 1};
    if (!std::memcmp(interval, two, 4)) std::memcpy(interval, one, 4);
  }
  __imp__sub_8261FE50(ctx, base);
  g_peggle2_game_frames.fetch_add(1, std::memory_order_relaxed);
#if PEGGLE2_NATIVE_RENDERER
  // The native ring thread waits in a WAIT_REG_MEM for the game's frame pacing word most of each frame
  // (docs/peggle2-notes.md): check it again now rather than at the end of masseffect_native_wait_reg_mem_us.
  me::native::SignalGuestProgress();
#endif
}
