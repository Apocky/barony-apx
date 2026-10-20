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
//   look <dx> <dy>           relative mouse motion (turn the view)
//   wheel <n>                mouse wheel ticks (+ up, - down)
//   wait <ms>                pause the script
//   shot <name>              screenshot to <dir>/<name>.png (the game's own back-buffer capture)
//   console <text>           run a Barony console command (cheats allowed in QA, e.g. /spawnitem ...)
//   ui <id> [right|double]   click the centre of a D92 stable id's rect (apx/ui), real SDL events
//   type <text>              SDL_TEXTINPUT events, one per character (a focused text field)
//   uikey <action>          drive player 0's window focus model: up down left right a b x y lb rb delete
//   uidump [name]            write every visible id + rect and the window's view state to <dir>/<name>.txt
//   uicost_reset             restart the frame-cost sample (CPU ms of apx::ui while the window is open)
//   quit                     close the game
// Status after every command: <dir>/status.txt (line done, frame, level, HP/MP, position, pointer, open UI).
#pragma once
#include <SDL.h>

namespace apx
{
namespace qa
{
	bool armed();
	void noteAttack();             // Entity::attack by a player (status `attacks`: a UI click must never add one)
	void arm(const char* dir);
	void pump();                   // once per frame, before SDL_PollEvent: pushes due input events
	const char* takeShot();        // a file path when a screenshot is wanted now, else nullptr
	void afterWindow(SDL_Window* w); // make the window non-activating and park it out of the way
} // namespace qa
} // namespace apx
