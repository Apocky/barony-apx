// apx/qadriver.cpp -- see qadriver.hpp.
#include "qadriver.hpp"
#include "in/in.hpp"
#include "in/in_glue.hpp"
#include "../main.hpp"
#include "../game.hpp"
#include "../stat.hpp"
#include "../entity.hpp"
#include "../player.hpp"
#include "../interface/interface.hpp"

#include <SDL_syswm.h>
#include <cstdio>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace apx
{
namespace qa
{
	namespace
	{
		bool g_on = false;
		std::string g_dir;
		size_t g_linesDone = 0;          // lines of cmd.txt already read
		Uint32 g_waitUntil = 0;           // SDL ticks: no new command before this
		std::string g_shot;               // pending screenshot path
		std::string g_lastCmd;
		int g_x = 0, g_y = 0;             // the game's pointer as the driver last placed it
		struct Timed { Uint32 at; SDL_Event ev; bool useRaw = false; apx::in::Raw raw; };
		Uint32 g_frames = 0;              // main-loop passes since arming (the gate derives the measured frame rate from it)
		std::deque<Timed> g_timed;        // key-ups and button-ups due later

		Uint32 windowId() { return screen ? SDL_GetWindowID(screen) : 0; }

		void pushKey(SDL_Scancode sc, bool down)
		{
			SDL_Event e; SDL_zero(e);
			e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
			e.key.windowID = windowId();
			e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
			e.key.keysym.scancode = sc;
			e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
			SDL_PushEvent(&e);
		}
		void pushMotion(int x, int y, int dx, int dy)
		{
			SDL_Event e; SDL_zero(e);
			e.type = SDL_MOUSEMOTION;
			e.motion.windowID = windowId();
			e.motion.x = x; e.motion.y = y; e.motion.xrel = dx; e.motion.yrel = dy;
			SDL_PushEvent(&e);
		}
		void pushButton(int x, int y, Uint8 button, bool down)
		{
			SDL_Event e; SDL_zero(e);
			e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
			e.button.windowID = windowId();
			e.button.button = button; e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
			e.button.clicks = 1; e.button.x = x; e.button.y = y;
			SDL_PushEvent(&e);
		}
		void later(Uint32 ms, const SDL_Event& e) { Timed t; t.at = SDL_GetTicks() + ms; t.ev = e; g_timed.push_back(t); }
		void laterRaw(Uint32 ms, const apx::in::Raw& r) { Timed t; t.at = SDL_GetTicks() + ms; SDL_zero(t.ev); t.useRaw = true; t.raw = r; g_timed.push_back(t); }

		// act <action> <ms> | press | release: a press of a named action. Into apx::in's own queue when it owns play input;
		// as raw SDL events when it does not (APX_INPUT_OLD=1, the control) or for sdlact. Both events are scheduled in
		// real time: a press shorter than the time to the next frame lands in ONE event batch, as a short real press does.
		void runAct(std::istringstream& in, bool forceSdl)
		{
			std::string name, what; in >> name >> what;
			apx::in::Raw d, u;
			if ( !apx::in::resolveAction(name.c_str(), &d, &u) ) { printlog("[apx-qa] act: no key or mouse binding for '%s'", name.c_str()); return; }
			const bool viaIn = apx::in::glue::active() && !forceSdl;
			int ms = 0; const bool press = what == "press", release = what == "release";
			if ( !press && !release ) { ms = atoi(what.c_str()); }
			auto sdlEv = [&](const apx::in::Raw& r)
			{
				SDL_Event e; SDL_zero(e);
				if ( r.type == apx::in::R_KeyDown || r.type == apx::in::R_KeyUp )
				{
					e.type = r.type == apx::in::R_KeyDown ? SDL_KEYDOWN : SDL_KEYUP; e.key.windowID = windowId();
					e.key.state = r.type == apx::in::R_KeyDown ? SDL_PRESSED : SDL_RELEASED;
					e.key.keysym.scancode = (SDL_Scancode)r.code; e.key.keysym.sym = SDL_GetKeyFromScancode((SDL_Scancode)r.code);
				}
				else
				{
					e.type = r.type == apx::in::R_MouseDown ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP; e.button.windowID = windowId();
					e.button.button = (Uint8)r.code; e.button.state = r.type == apx::in::R_MouseDown ? SDL_PRESSED : SDL_RELEASED;
					e.button.clicks = 1; e.button.x = g_x; e.button.y = g_y;
				}
				return e;
			};
			auto sched = [&](Uint32 at, const apx::in::Raw& r) { if ( viaIn ) { laterRaw(at, r); } else { later(at, sdlEv(r)); } };
			if ( !release ) { sched(1, d); }
			if ( !press ) { sched(1 + (Uint32)ms, u); }
			g_waitUntil = SDL_GetTicks() + (Uint32)(press || release ? 0 : ms) + 30;
		}

		void writeStatus()
		{
			FILE* f = fopen((g_dir + "/status.txt").c_str(), "w");
			if ( !f ) { return; }
			Stat* s = stats[0];
			Entity* me = players[0] ? players[0]->entity : nullptr;
			fprintf(f, "line %zu\nlast %s\nframe %u\nlevel %d\nhp %d/%d\nmp %d/%d\npos %.1f %.1f yaw %.2f\npointer %d %d\nintro %d\ninventory_open %d\n",
				g_linesDone, g_lastCmd.c_str(), (unsigned)ticks, (int)currentlevel,
				s ? s->HP : -1, s ? s->MAXHP : -1, s ? s->MP : -1, s ? s->MAXMP : -1,
				me ? me->x / 16.0 : -1.0, me ? me->y / 16.0 : -1.0, me ? me->yaw : 0.0,
				g_x, g_y, intro ? 1 : 0,
				(players[0] && players[0]->shootmode == false) ? 1 : 0);
			char ib[1200]; apx::in::glue::statusLines(ib, (int)sizeof(ib));
			fputs(ib, f);
			fprintf(f, "frames %u\nwall_ms %u\n", (unsigned)g_frames, (unsigned)SDL_GetTicks());
			fclose(f);
		}

		void run(const std::string& line)
		{
			std::istringstream in(line);
			std::string op; in >> op;
			if ( op.empty() || op[0] == '#' ) { return; }
			g_lastCmd = line;
			printlog("[apx-qa] %s", line.c_str());
			if ( op == "key" || op == "hold" )
			{
				std::string name; int ms = op == "key" ? 80 : 500; in >> name; in >> ms;
				SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
				if ( sc == SDL_SCANCODE_UNKNOWN ) { printlog("[apx-qa] unknown key '%s'", name.c_str()); return; }
				pushKey(sc, true);
				SDL_Event up; SDL_zero(up); up.type = SDL_KEYUP; up.key.windowID = windowId(); up.key.state = SDL_RELEASED;
				up.key.keysym.scancode = sc; up.key.keysym.sym = SDL_GetKeyFromScancode(sc);
				later((Uint32)ms, up);
				g_waitUntil = SDL_GetTicks() + (Uint32)ms + 30;
			}
			else if ( op == "kdown" || op == "kup" )
			{
				// non-blocking key press / release: hold several keys at once (W + Left Shift), release with kup
				std::string name; std::getline(in >> std::ws, name);
				while ( !name.empty() && (name.back() == ' ' || name.back() == '\r') ) { name.pop_back(); }
				SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
				if ( sc == SDL_SCANCODE_UNKNOWN ) { printlog("[apx-qa] unknown key '%s'", name.c_str()); return; }
				pushKey(sc, op == "kdown");
				g_waitUntil = SDL_GetTicks() + 20;
			}
			else if ( op == "mark" )
			{
				std::string label; in >> label;
				char kv[900]; apx::in::glue::markJson(kv, (int)sizeof(kv));
				FILE* mf = fopen((g_dir + "/marks.jsonl").c_str(), "a");
				if ( mf ) { fprintf(mf, "{\"label\":\"%s\",\"frames\":%u,\"wall_ms\":%u,%s}\n", label.c_str(), (unsigned)g_frames, (unsigned)SDL_GetTicks(), kv); fclose(mf); }
			}
			else if ( op == "act" ) { runAct(in, false); }
			else if ( op == "sdlact" ) { runAct(in, true); }
			else if ( op == "move" ) { in >> g_x >> g_y; pushMotion(g_x, g_y, 0, 0); g_waitUntil = SDL_GetTicks() + 30; }
			else if ( op == "click" || op == "rclick" )
			{
				std::string which; in >> g_x >> g_y >> which;
				const Uint8 b = (op == "rclick" || which == "right") ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
				pushMotion(g_x, g_y, 0, 0);
				pushButton(g_x, g_y, b, true);
				SDL_Event up; SDL_zero(up); up.type = SDL_MOUSEBUTTONUP; up.button.windowID = windowId();
				up.button.button = b; up.button.state = SDL_RELEASED; up.button.clicks = 1; up.button.x = g_x; up.button.y = g_y;
				later(60, up);
				g_waitUntil = SDL_GetTicks() + 120;
			}
			else if ( op == "down" || op == "up" )
			{
				std::string which; in >> g_x >> g_y >> which;
				const Uint8 b = which == "right" ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
				pushMotion(g_x, g_y, 0, 0);
				pushButton(g_x, g_y, b, op == "down");
				g_waitUntil = SDL_GetTicks() + 30;
			}
			else if ( op == "drag" )
			{
				int x0, y0, x1, y1; in >> x0 >> y0 >> x1 >> y1;
				pushMotion(x0, y0, 0, 0); pushButton(x0, y0, SDL_BUTTON_LEFT, true);
				const int steps = 8;
				for ( int i = 1; i <= steps; ++i )
				{
					SDL_Event m; SDL_zero(m); m.type = SDL_MOUSEMOTION; m.motion.windowID = windowId();
					m.motion.x = x0 + (x1 - x0) * i / steps; m.motion.y = y0 + (y1 - y0) * i / steps;
					m.motion.state = SDL_BUTTON_LMASK;
					later((Uint32)(20 * i), m);
				}
				SDL_Event up; SDL_zero(up); up.type = SDL_MOUSEBUTTONUP; up.button.windowID = windowId();
				up.button.button = SDL_BUTTON_LEFT; up.button.state = SDL_RELEASED; up.button.x = x1; up.button.y = y1;
				later(20 * steps + 40, up);
				g_x = x1; g_y = y1;
				g_waitUntil = SDL_GetTicks() + 20 * steps + 100;
			}
			else if ( op == "look" ) { int dx = 0, dy = 0; in >> dx >> dy; pushMotion(g_x, g_y, dx, dy); g_waitUntil = SDL_GetTicks() + 30; }
			else if ( op == "wheel" )
			{
				int n = 0; in >> n;
				SDL_Event e; SDL_zero(e); e.type = SDL_MOUSEWHEEL; e.wheel.windowID = windowId(); e.wheel.y = n;
				SDL_PushEvent(&e); g_waitUntil = SDL_GetTicks() + 30;
			}
			else if ( op == "wait" ) { int ms = 0; in >> ms; g_waitUntil = SDL_GetTicks() + (Uint32)ms; }
			else if ( op == "shot" ) { std::string n; in >> n; g_shot = g_dir + "/" + (n.empty() ? "shot" : n) + ".png"; g_waitUntil = SDL_GetTicks() + 200; }
			else if ( op == "console" )
			{
				std::string rest; std::getline(in, rest);
				while ( !rest.empty() && rest[0] == ' ' ) { rest.erase(0, 1); }
				consoleCommand(rest.c_str());
				g_waitUntil = SDL_GetTicks() + 100;
			}
			else if ( op == "quit" ) { mainloop = 0; }
			else { printlog("[apx-qa] unknown command '%s'", op.c_str()); }
		}
	}

	bool armed() { return g_on; }

	void send(const char* line)
	{
		if ( !g_on || !line ) { return; }
		std::ofstream(g_dir + "/cmd.txt", std::ios::app) << line << "\n";
	}

	bool idle()
	{
		if ( !g_on ) { return true; }
		if ( !g_timed.empty() || !SDL_TICKS_PASSED(SDL_GetTicks(), g_waitUntil) || !g_shot.empty() ) { return false; }
		std::ifstream f(g_dir + "/cmd.txt");
		std::string line; size_t n = 0;
		while ( std::getline(f, line) ) { ++n; }
		return n <= g_linesDone;
	}

	void arm(const char* dir)
	{
		g_on = true; g_dir = dir ? dir : ".";
		std::ofstream(g_dir + "/cmd.txt", std::ios::app).close(); // exists, keeps earlier lines
		printlog("[apx-qa] driver armed: commands from %s/cmd.txt, status in status.txt", g_dir.c_str());
	}

	void pump()
	{
		if ( !g_on ) { return; }
		const Uint32 now = SDL_GetTicks();
		++g_frames;
		while ( !g_timed.empty() && SDL_TICKS_PASSED(now, g_timed.front().at) )
		{
			if ( g_timed.front().useRaw ) { apx::in::event(g_timed.front().raw); } else { SDL_PushEvent(&g_timed.front().ev); }
			g_timed.pop_front();
		}
		if ( !g_shot.empty() || !SDL_TICKS_PASSED(now, g_waitUntil) ) { return; }
		std::ifstream f(g_dir + "/cmd.txt");
		std::string line; size_t n = 0;
		while ( std::getline(f, line) )
		{
			if ( n++ < g_linesDone ) { continue; }
			g_linesDone = n;
			if ( !line.empty() && line.back() == '\r' ) { line.pop_back(); }
			run(line);
			writeStatus();
			break; // one command per frame; waits and shots gate the next
		}
		static Uint32 lastStatus = 0;
		if ( now - lastStatus > 500 ) { lastStatus = now; writeStatus(); }
	}

	const char* takeShot()
	{
		static std::string out;
		if ( g_shot.empty() ) { return nullptr; }
		out = g_shot; g_shot.clear();
		printlog("[apx-qa] screenshot %s", out.c_str());
		return out.c_str();
	}

	void afterWindow(SDL_Window* w)
	{
		if ( !g_on || !w ) { return; }
#ifdef WINDOWS
		SDL_SysWMinfo info; SDL_VERSION(&info.version);
		if ( SDL_GetWindowWMInfo(w, &info) )
		{
			HWND h = info.info.win.window;
			LONG_PTR ex = GetWindowLongPtr(h, GWL_EXSTYLE);
			SetWindowLongPtr(h, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
			SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			printlog("[apx-qa] window set non-activating (never takes focus)");
		}
#endif
	}
} // namespace qa
} // namespace apx
