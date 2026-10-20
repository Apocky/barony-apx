// apx/qadriver.hpp -- -apxqa=<dir>: an interactive QA driver. A tester (human or agent) appends commands to
// <dir>/cmd.txt; the game executes them as REAL SDL input events pushed into its own event queue, so the
// game's UI and input code handle them exactly as if a mouse and keyboard produced them. The OS mouse and
// focus are never touched (apx::noInputGrab), and the window is made non-activating.
//
// Commands (one per line; # starts a comment):
//   key <name> [ms]          press and release a key (SDL scancode name, e.g. I, Escape, Return, W, Space)
//   hold <name> <ms>         hold a key for ms milliseconds (walk with W, etc.)
//   move <x> <y>             move the game's mouse pointer to window pixel (x, y)
//   click <x> <y> [right]    move there, press and release the left (or right) button
//   drag <x0> <y0> <x1> <y1> left-button drag
//   down <x> <y> [right]     move there and press the left (or right) button, held until "up"
//   up <x> <y> [right]       move there and release it
//   look <dx> <dy>           relative mouse motion (turn the view)
//   wheel <n>                mouse wheel ticks (+ up, - down)
//   wait <ms>                pause the script
//   waitlevel <n> [ms]       pause until floor n is loaded and player 0 stands on it (default timeout 90 s); logs reached or TIMED OUT
//   shot <name>              screenshot to <dir>/<name>.png (the game's own back-buffer capture)
//   console <text>           run a Barony console command (cheats allowed in QA, e.g. /spawnitem ...)
//   quit                     close the game
// Status after every command: <dir>/status.txt (line done, frame, level, HP/MP, position, pointer, open UI).
#pragma once
#include <SDL.h>

namespace apx
{
namespace qa
{
	bool armed();
	void arm(const char* dir);
	void pump();                   // once per frame, before SDL_PollEvent: pushes due input events
	const char* takeShot();        // a file path when a screenshot is wanted now, else nullptr
	void afterWindow(SDL_Window* w); // make the window non-activating and park it out of the way
	void send(const char* line);   // append a command to cmd.txt (an in-game self-test driving the same queue)
	bool idle();                   // every command run, no button/key release still due, no wait running
} // namespace qa
} // namespace apx
