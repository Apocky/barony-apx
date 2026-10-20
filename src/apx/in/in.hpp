// apx/in/in.hpp -- the input contract every lane codes against (spec 04b section 3 and 4).
// THIS IS THE COMBAT LANE'S STUB (src/apx/in/in_stub.cpp): the same signatures the INPUT lane (apx/input, phase C0)
// builds. When apx/input merges, take its in.hpp / in.cpp and delete in_stub.cpp. Until then the stub reads Barony's
// own input: SDL mouse button events are latched here (a click shorter than a frame or a tick is never lost) and
// Barony's "Attack"/"Defend" bindings give the held bit for other devices.
#pragma once
#include <SDL.h>

namespace apx
{
namespace in
{
	// digital actions (data/apx/input/actions.json); the stub fills Attack and Block, the rest read as zero
	enum Action : int { A_ATTACK = 0, A_BLOCK, A_DODGE, A_KICK, A_USE, A_CAST, A_SNEAK, A_SPRINT, A_COUNT };

	struct TickRecord
	{
		unsigned held = 0;                    // bit per Action held at the end of this tick's slice
		unsigned char pressCount[A_COUNT] = {};   // presses since the previous take()
		unsigned char releaseCount[A_COUNT] = {}; // releases since the previous take()
		float moveX = 0.f, moveY = 0.f;       // -1..1 (stub: zero; movement is still Barony's until apx::move)
		float lookDX = 0.f, lookDY = 0.f;     // look motion since the previous take(), pixels (+x right, +y down)
		float swingDX = 0.f, swingDY = 0.f;   // look motion while Attack was held or pressed in this slice
		bool isHeld(Action a) const { return (held >> a) & 1u; }
	};

	// once per sim tick per player; hands the record over exactly once
	const TickRecord& take(int player);
	bool uiOwnsMouse(int player);
	bool uiOwnsKeyboard(int player);

	// stub feed: called for every SDL event from the game's event loop (never consumes it)
	void onEvent(const SDL_Event& e);
	// counters for the D90 "input" scope: events latched / presses delivered
	unsigned long long latchedEvents();
	unsigned long long deliveredPresses();
} // namespace in
} // namespace apx
