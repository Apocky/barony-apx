// apx/in/in_glue.cpp -- see in_glue.hpp.
#include "in_glue.hpp"
#include "../datadir.hpp" // the one data-folder rule
#include "in.hpp"
#include "../ui/ui.hpp"
#include "../diag.hpp"
#include "../../main.hpp"
#include "../../game.hpp"
#include "../../player.hpp"
#include "../../stat.hpp"
#include "../../entity.hpp"
#include "../../input.hpp"
#include "../../mod_tools.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef APX_PARTS_DATA_DIR
#define APX_PARTS_DATA_DIR "data/apx"
#endif

namespace apx
{
namespace in
{
	namespace glue
	{
		namespace
		{
			bool g_ready = false;
			bool g_active = false;
			Uint32 g_lastSec = 0;
			unsigned long long g_legacy[2] = { 0, 0 };
			bool g_legacyPrev[2][kMaxPlayers] = {};
			unsigned long long g_secEvents = 0;

			struct Sink : CompatSink
			{
				void setKey(int sym, bool down) override { keystatus[sym] = down; Input::keys[sym] = down; }
				void setMouse(int b, bool down) override
				{
					if ( b >= 1 && b < (int)NUM_MOUSE_STATUS ) { mousestatus[b] = down ? 1 : 0; }
					if ( b >= 1 && b < 16 ) { Input::mouseButtons[b] = down; }
				}
				void pulseWheel(bool up) override
				{
					mousestatus[up ? SDL_BUTTON_WHEELUP : SDL_BUTTON_WHEELDOWN] = 1;
					Input::mouseButtons[up ? Input::MOUSE_WHEEL_UP : Input::MOUSE_WHEEL_DOWN] = 1;
				}
				void setMouseRel(float dx, float dy) override { mousexrel = (Sint32)dx; mouseyrel = (Sint32)dy; }
			} g_sink;

			// ---- the host facts apx::in asks for
			int envKeyboardPlayer() { return inputs.getPlayerIDAllowedKeyboard(); }
			int envPadPlayer(int instanceId)
			{
				SDL_GameController* pad = SDL_GameControllerFromInstanceID(instanceId);
				if ( !pad ) { return -1; }
				for ( auto& c : game_controllers )
				{
					if ( c.isActive() && c.getControllerDevice() == pad )
					{
						for ( int p = 0; p < MAXPLAYERS; ++p ) { if ( inputs.getControllerID(p) == c.getID() ) { return p; } }
					}
				}
				return -1;
			}
			bool envInGame(int p)
			{
				if ( p < 0 || p >= MAXPLAYERS || intro || gamePaused || !players[p] ) { return false; }
				return players[p]->isLocalPlayer() && players[p]->entity && !client_disconnected[p];
			}
			bool envVanillaMouse(int p) { return !players[p]->shootmode; }                 // a vanilla window/menu is open
			bool envVanillaKeyboard(int) { return SDL_IsTextInputActive() || command; }    // typing into a vanilla field or the console
			bool envApxMouse(int p)
			{
				if ( apx::ui::wantsMouse(p) ) { return true; }
#ifdef USE_IMGUI
				if ( ImGui_t::requestingMouse() ) { return true; }                         // our ImGui windows (dev menu, tools)
#endif
				return false;
			}
			bool envApxKeyboard(int p)
			{
				if ( apx::ui::wantsKeyboard(p) ) { return true; }
#ifdef USE_IMGUI
				if ( ImGui_t::isInit && ImGui_t::getIO().WantTextInput ) { return true; }
#endif
				return false;
			}
			int envScancode(const char* name) { return (int)SDL_GetScancodeFromName(name); }
			int envSym(int sc) { return (int)SDL_GetKeyFromScancode((SDL_Scancode)sc); }
			bool envPos(int p, double* x, double* y, double* yaw)
			{
				if ( p < 0 || p >= MAXPLAYERS || !players[p] || !players[p]->entity ) { return false; }
				*x = players[p]->entity->x; *y = players[p]->entity->y; *yaw = players[p]->entity->yaw;
				return true;
			}

			std::string dataDir()
			{
				if ( const char* e = getenv("APX_INPUT_DIR") ) { if ( *e ) { return e; } }
				return apx::dataDir() + "/input"; // the one data-folder rule (datadir.hpp)
			}
		}

		bool active() { return g_active; }
		bool vanillaWrites() { return !g_active; }
		bool swallowLook() { return g_active && swallowsMouseLook(); }

		void init(const char* replayFile)
		{
			if ( g_ready ) { return; }
			g_ready = true;
			const char* oldEnv = getenv("APX_INPUT_OLD");
			setOldPath(oldEnv && oldEnv[0] == '1');
			Env env;
			env.keyboardPlayer = envKeyboardPlayer; env.padPlayer = envPadPlayer; env.inGame = envInGame;
			env.vanillaMouse = envVanillaMouse; env.vanillaKeyboard = envVanillaKeyboard;
			env.apxuiMouse = envApxMouse; env.apxuiKeyboard = envApxKeyboard;
			env.scancodeFromName = envScancode; env.symFromScancode = envSym; env.playerPos = envPos;
			const std::string dir = dataDir();
			std::string err;
			if ( !apx::in::init(dir.c_str(), ".", env, &err) /* user overrides: ./bindings.user.json (Barony keeps its config in the working folder) */ )
			{
				printlog("[apx-in] ERROR: input data failed to load from %s: %s. apx::in is OFF, the old input path runs (this is a downgrade).", dir.c_str(), err.c_str());
				apxdiag::event("input_error", ("\"msg\":\"" + apxdiag::jsonEscape(err.c_str()) + "\"").c_str());
				g_active = false;
				return;
			}
			if ( !err.empty() ) { printlog("[apx-in] binding notes: %s", err.c_str()); }
			g_active = !oldPath();
			printlog("[apx-in] data %s; %s", dir.c_str(), g_active ? "apx::in owns play input (compat feed to the legacy state)" : "APX_INPUT_OLD=1: the OLD raw path runs (control)");
			if ( const char* lg = getenv("APX_INPUT_LOG") )
			{
				if ( lg[0] == '1' )
				{
					const char* fn = getenv("APX_INPUT_LOG_FILE");
					const std::string path = fn && *fn ? fn : "input-log.jsonl";
					if ( logOpen(path.c_str()) ) { printlog("[apx-in] per-tick log -> %s", path.c_str()); }
				}
			}
			if ( replayFile && replayFile[0] )
			{
				std::string e2;
				if ( replayOpen(replayFile, &e2) ) { printlog("[apx-in] REPLAY of %s: live play input is ignored", replayFile); }
				else { printlog("[apx-in] ERROR: replay %s failed: %s", replayFile, e2.c_str()); }
			}
		}

		void shutdown() { logClose(); }

		void frameBegin()
		{
			if ( !g_active ) { return; }
			apx::in::beginFrame(SDL_GetTicks());
		}

		void observe(const SDL_Event& e)
		{
			if ( !g_active ) { return; }
			Raw r;
			switch ( e.type )
			{
			case SDL_KEYDOWN: case SDL_KEYUP:
				r.type = e.type == SDL_KEYDOWN ? R_KeyDown : R_KeyUp; r.code = (int)e.key.keysym.scancode; r.sym = (int)e.key.keysym.sym;
				r.repeat = e.key.repeat != 0; r.tsMs = e.key.timestamp; break;
			case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
				r.type = e.type == SDL_MOUSEBUTTONDOWN ? R_MouseDown : R_MouseUp; r.code = e.button.button; r.tsMs = e.button.timestamp; break;
			case SDL_MOUSEWHEEL: r.type = R_Wheel; r.value = (float)e.wheel.y; r.tsMs = e.wheel.timestamp; break;
			case SDL_MOUSEMOTION: r.type = R_MouseMove; r.dx = (float)e.motion.xrel; r.dy = (float)e.motion.yrel; r.tsMs = e.motion.timestamp; break;
			case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
				r.type = e.type == SDL_CONTROLLERBUTTONDOWN ? R_PadDown : R_PadUp; r.code = e.cbutton.button; r.pad = e.cbutton.which; r.tsMs = e.cbutton.timestamp; break;
			case SDL_CONTROLLERAXISMOTION:
				r.type = R_PadAxis; r.code = e.caxis.axis; r.value = e.caxis.value / 32768.f; r.pad = e.caxis.which; r.tsMs = e.caxis.timestamp; break;
			default: return;
			}
			apx::in::event(r);
		}

		void frameEnd()
		{
			if ( !g_active ) { return; }
			apx::in::endFrame(g_sink);
			const Uint32 now = SDL_GetTicks();
			if ( now - g_lastSec >= 1000 )
			{
				g_lastSec = now;
				const unsigned long long ev = counters().events;
				const std::string kv = countersJson() + ",\"ev_s\":" + std::to_string(ev - g_secEvents) + ",\"owner_mouse\":" + std::to_string((int)mouseOwner(clientnum)) + ",\"owner_key\":" + std::to_string((int)keyboardOwner(clientnum));
				g_secEvents = ev;
				apxdiag::event("input", kv.c_str());
			}
			if ( replaying() && replayDone() )
			{
				printlog("[apx-in] replay finished (%u ticks); closing", tickCounter());
				logClose();
				mainloop = 0;
			}
		}

		void gameTick()
		{
			if ( !g_active ) { return; }
			apx::in::tickAll(g_sink);
		}

		void afterInputUpdate()
		{
			// The legacy-consumer probe: what Barony's own bindings (Input::mouseButtons / Input::keys) see at the tick.
			if ( !g_ready ) { return; }
			const int p = clientnum;
			if ( p < 0 || p >= MAXPLAYERS || !envInGame(p) ) { return; }
			const bool m = Input::inputs[p].binary("Attack");
			const bool k = Input::keys[SDLK_LSHIFT];
			if ( m && !g_legacyPrev[0][p] ) { ++g_legacy[0]; }
			if ( k && !g_legacyPrev[1][p] ) { ++g_legacy[1]; }
			g_legacyPrev[0][p] = m; g_legacyPrev[1][p] = k;
		}

		unsigned long long legacyEdges(int which) { return (which >= 0 && which < 2) ? g_legacy[which] : 0; }

		void markJson(char* buf, int n)
		{
			const Counters& c = counters();
			const int p = clientnum;
			snprintf(buf, (size_t)n, "\"mode\":\"%s\",\"ingame\":%d,\"tick\":%u,\"press\":%llu,\"taken\":%llu,\"taken_attack\":%llu,\"taken_sprint\":%llu,\"gated\":%llu,\"lost\":%llu,\"switch\":%llu,\"switch_rel\":%llu,\"legacy_attack\":%llu,\"legacy_lshift\":%llu,\"owner_mouse\":%d,\"owner_key\":%d",
				g_active ? "new" : "old", envInGame(p) ? 1 : 0, (unsigned)ticks, (unsigned long long)c.pressesLatched, (unsigned long long)c.pressesDelivered,
				(unsigned long long)c.takenPress[B_Attack], (unsigned long long)c.takenPress[B_Sprint], (unsigned long long)c.gatedPresses, (unsigned long long)c.lost(),
				(unsigned long long)c.ownerSwitches, (unsigned long long)c.switchReleases, g_legacy[0], g_legacy[1], (int)mouseOwner(p), (int)keyboardOwner(p));
		}

		void statusLines(char* buf, int n)
		{
			const Counters& c = counters();
			const int p = clientnum;
			snprintf(buf, (size_t)n, "ingame %d\ninput_mode %s\nin_press %llu\nin_taken %llu\nin_taken_attack %llu\nin_taken_sprint %llu\nin_taken_use %llu\nin_gated %llu\nin_lost %llu\nin_switch %llu\nin_switch_rel %llu\nlegacy_attack %llu\nlegacy_lshift %llu\nowner_mouse %d\nowner_key %d\n",
				envInGame(p) ? 1 : 0, g_active ? "new" : "old", (unsigned long long)c.pressesLatched, (unsigned long long)c.pressesDelivered,
				(unsigned long long)c.takenPress[B_Attack], (unsigned long long)c.takenPress[B_Sprint], (unsigned long long)c.takenPress[B_Use],
				(unsigned long long)c.gatedPresses, (unsigned long long)c.lost(), (unsigned long long)c.ownerSwitches, (unsigned long long)c.switchReleases,
				g_legacy[0], g_legacy[1], (int)mouseOwner(p), (int)keyboardOwner(p));
		}
	}
}
}
