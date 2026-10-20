// apx/headless.hpp -- -apxheadless: run the game with no visible window, no audio, no vsync, no frame
// limiter and no drawing, logic fast-forwarded by a fixed batch of ticks per main-loop pass. For test
// jobs (floor hashes, parity, sims) that many can run at once and that no key press can reach.
// Hooks (one-line guards): game.cpp (args, tick batch, limiter, mouse capture), init.cpp (window flags,
// vsync), opengl.cpp (glDrawWorld, GO_SwapBuffers), draw.cpp (drawEntities3D), ui/GameUI.cpp (UI draw).
// Logic that lives inside those draw paths (the crosshair pick) does not run when headless.
#pragma once

namespace apx
{
	extern bool noInputGrab;           // any automated launch: never warp or grab the user's real mouse (set in main)
	extern bool headlessOn;            // -apxheadless on the command line, or APX_HEADLESS=1
	void headlessInit(bool flag);      // call once after the command line is parsed, before initApp()
	int headlessBatch();               // logic ticks per main-loop pass (APX_HEADLESS_BATCH, default 50)
	void headlessLog();                // one log line describing the mode (once the log file is open)
}
