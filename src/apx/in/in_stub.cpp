// apx/in/in_stub.cpp -- see in.hpp. Combat-lane stub of apx::in; deleted when apx/input lands.
#include "in.hpp"
#include "../../main.hpp"
#include "../../game.hpp"
#include "../../player.hpp"
#include "../../input.hpp"
#include <cstdlib>

namespace apx
{
namespace in
{
	namespace
	{
		struct Slot
		{
			TickRecord rec;           // accumulating since the last take()
			TickRecord out;           // what take() returned last
			bool mouseAttack = false; // left button state as events saw it
			bool mouseBlock = false;
			bool prevBinAttack = false, prevBinBlock = false;
			bool attackActiveSlice = false; // Attack held or pressed during this slice (swing motion window)
		};
		Slot g_slot[MAXPLAYERS];
		unsigned long long g_events = 0, g_presses = 0;

		bool envOld() { static const bool on = std::getenv("APX_INPUT_OLD") && std::getenv("APX_INPUT_OLD")[0] == '1'; return on; }

		// the player the mouse and keyboard belong to (single seat: player 0)
		int mousePlayer() { return 0; }

		bool gameOwnsMouse(int p) { return p >= 0 && p < MAXPLAYERS && players[p] && players[p]->shootmode && !gamePaused && !intro; }
	}

	bool uiOwnsMouse(int p) { return !gameOwnsMouse(p); }
	bool uiOwnsKeyboard(int p) { return command || !(p >= 0 && p < MAXPLAYERS && players[p] && players[p]->shootmode); }

	void onEvent(const SDL_Event& e)
	{
		if ( envOld() ) { return; }
		const int p = mousePlayer();
		Slot& s = g_slot[p];
		switch ( e.type )
		{
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
			{
				const bool down = e.type == SDL_MOUSEBUTTONDOWN;
				if ( e.button.button == SDL_BUTTON_LEFT )
				{
					if ( down && !gameOwnsMouse(p) ) { return; } // the UI owns this click
					if ( down == s.mouseAttack ) { return; }
					s.mouseAttack = down;
					++g_events;
					if ( down ) { if ( s.rec.pressCount[A_ATTACK] < 255 ) { ++s.rec.pressCount[A_ATTACK]; } s.attackActiveSlice = true; }
					else { if ( s.rec.releaseCount[A_ATTACK] < 255 ) { ++s.rec.releaseCount[A_ATTACK]; } }
				}
				else if ( e.button.button == SDL_BUTTON_RIGHT )
				{
					if ( down && !gameOwnsMouse(p) ) { return; }
					if ( down == s.mouseBlock ) { return; }
					s.mouseBlock = down;
					++g_events;
					if ( down ) { if ( s.rec.pressCount[A_BLOCK] < 255 ) { ++s.rec.pressCount[A_BLOCK]; } }
					else { if ( s.rec.releaseCount[A_BLOCK] < 255 ) { ++s.rec.releaseCount[A_BLOCK]; } }
				}
				break;
			}
			case SDL_MOUSEMOTION:
				if ( gameOwnsMouse(p) )
				{
					s.rec.lookDX += (float)e.motion.xrel;
					s.rec.lookDY += (float)e.motion.yrel;
					if ( s.mouseAttack || s.attackActiveSlice ) { s.rec.swingDX += (float)e.motion.xrel; s.rec.swingDY += (float)e.motion.yrel; }
				}
				break;
			default:
				break;
		}
	}

	const TickRecord& take(int p)
	{
		static TickRecord zero;
		if ( p < 0 || p >= MAXPLAYERS ) { return zero; }
		Slot& s = g_slot[p];
		TickRecord r = s.rec;
		s.rec = TickRecord();
		// other devices: Barony's bindings give the held bit; a rising or falling edge between takes counts as a press or release
		bool binAttack = false, binBlock = false;
		if ( !envOld() && gameOwnsMouse(p) )
		{
			Input& input = Input::inputs[p];
			binAttack = input.binary("Attack");
			binBlock = input.binary("Defend");
		}
		const bool mouseAttack = s.mouseAttack && gameOwnsMouse(p);
		if ( binAttack && !s.prevBinAttack && !s.mouseAttack ) { if ( r.pressCount[A_ATTACK] < 255 ) { ++r.pressCount[A_ATTACK]; } }
		if ( !binAttack && s.prevBinAttack && !s.mouseAttack ) { if ( r.releaseCount[A_ATTACK] < 255 ) { ++r.releaseCount[A_ATTACK]; } }
		if ( binBlock && !s.prevBinBlock && !s.mouseBlock ) { if ( r.pressCount[A_BLOCK] < 255 ) { ++r.pressCount[A_BLOCK]; } }
		if ( !binBlock && s.prevBinBlock && !s.mouseBlock ) { if ( r.releaseCount[A_BLOCK] < 255 ) { ++r.releaseCount[A_BLOCK]; } }
		s.prevBinAttack = binAttack;
		s.prevBinBlock = binBlock;
		r.held = 0;
		if ( mouseAttack || binAttack ) { r.held |= 1u << A_ATTACK; }
		if ( s.mouseBlock || binBlock ) { r.held |= 1u << A_BLOCK; }
		s.attackActiveSlice = (r.held >> A_ATTACK) & 1u;
		g_presses += r.pressCount[A_ATTACK];
		s.out = r;
		return s.out;
	}

	unsigned long long latchedEvents() { return g_events; }
	unsigned long long deliveredPresses() { return g_presses; }
} // namespace in
} // namespace apx
