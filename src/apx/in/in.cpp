// apx/in/in.cpp -- STUB of apx::in (see in.hpp). Reads Barony's current input; replaced by the INPUT lane.
#include "../../main.hpp"
#include "../../game.hpp"
#include "../../stat.hpp"
#include "../../entity.hpp"
#include "../../player.hpp"
#include "../../input.hpp"
#include "in.hpp"
#include <SDL.h>

namespace apx
{
namespace in
{
	namespace
	{
		TickRecord g_rec[MAXPLAYERS];
		Uint32 g_tick[MAXPLAYERS] = { (Uint32)-1, (Uint32)-1, (Uint32)-1, (Uint32)-1 };
	}

	bool uiOwnsMouse(int p) { return p >= 0 && p < MAXPLAYERS && players[p] && !players[p]->shootmode; }
	bool uiOwnsKeyboard(int p) { return p >= 0 && p < MAXPLAYERS && players[p] && players[p]->usingCommand(); }

	const TickRecord& take(int p)
	{
		static const TickRecord none;
		if ( p < 0 || p >= MAXPLAYERS || !players[p] ) { return none; }
		if ( g_tick[p] == ticks ) { return g_rec[p]; }
		g_tick[p] = ticks;
		TickRecord r;
		Player& pl = *players[p];
		Input& input = Input::inputs[p];
		if ( !pl.usingCommand() && pl.bControlEnabled )
		{
			if ( !inputs.hasController(p) )
			{
				r.moveX = (float)(input.binary("Move Right") - input.binary("Move Left"));
				r.moveY = (float)(input.binary("Move Forward") - input.binary("Move Backward"));
				if ( keystatus[SDLK_LSHIFT] ) { r.held |= 1ull << B_Sprint; }
			}
			else
			{
				r.moveX = inputs.getController(p)->getLeftXPercentForPlayerMovement(p);
				r.moveY = inputs.getController(p)->getLeftYPercentForPlayerMovement(p);
			}
			if ( pl.shootmode )
			{
				r.lookDX = inputs.getMouseFloat(p, Inputs::ANALOGUE_XREL);
				r.lookDY = inputs.getMouseFloat(p, Inputs::ANALOGUE_YREL);
			}
		}
		if ( stats[p] && stats[p]->sneaking == 1 ) { r.held |= 1ull << B_Sneak; }
		g_rec[p] = r;
		return g_rec[p];
	}
}
}
