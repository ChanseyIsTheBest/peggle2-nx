// Short video recordings of the game image for testing, taken from the presenter's guest output (what the game
// presents), so they work with the window minimized and nothing can cover them. Same design and file format as the
// banjo-nx recorder: tools/make_video.sh turns the raw file into an MP4 and a contact sheet.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace rex::ui {
class Presenter;
}

// Frames the game has presented (counted by the hook on its D3DDevice_Swap, peggle2_frame_hooks.cpp); the recorder
// waits on it.
extern std::atomic<uint64_t> g_peggle2_game_frames;

namespace peggle2::capture {

// Starts the recorder thread when peggle2_video_s > 0.
void Start(std::function<rex::ui::Presenter*()> get_presenter);

// Stops it (writes what it has, if anything).
void Stop();

}  // namespace peggle2::capture
