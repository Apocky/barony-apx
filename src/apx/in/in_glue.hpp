// apx/in/in_glue.hpp -- the Barony side of apx::in (SDL events in, legacy state out). game.cpp calls these.
#pragma once
#include <SDL.h>

namespace apx
{
namespace in
{
	namespace glue
	{
		void init(const char* replayFile);      // after the command line and SDL are up; loads the data, opens log/replay
		void shutdown();
		bool active();                           // apx::in feeds play (loaded and not APX_INPUT_OLD=1)
		bool vanillaWrites();                    // true: the old code writes keystatus/mousestatus/Input from raw events (the control, or load failed)
		void frameBegin();                       // before the QA pump and the event loop: the owner gate
		void observe(const SDL_Event& e);        // every SDL event, first
		void frameEnd();                         // after the event loop: compat feed, per-second D90 row
		void gameTick();                         // top of gameLogic, before Input::update: take + compat tick step
		void afterInputUpdate();                 // after Input::update: the legacy-consumer probe
		unsigned long long legacyEdges(int which); // 0 = mouse1 (Attack), 1 = Left Shift: rising edges the old consumers saw
		void statusLines(char* buf, int n);      // for the QA status file
	}
}
}
