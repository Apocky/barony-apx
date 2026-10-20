// apx/diag.hpp -- D90: always-on LOCAL diagnostics, from process start, in every build.
//
// Writes diag/session-<yyyymmdd-hhmmss>-<pid>.jsonl in the working directory (one JSON object per line, ASCII).
// NOTHING is sent anywhere: this module has no network code, and the files stay on the player's machine until the
// player chooses to send them.
//
// Rows ("t" = row type; every row has ms = wall ms since process start, tick, lvl):
//   session  build sha/date, version, os, cpu, ram, argv, pid, working dir
//   hw       cpu name/cores, ram, gpu (after the GL context exists), displays
//   settings resolution, fullscreen, vsync, fps limit, dlc flags, language, ...
//   knobs    every APX_* env var, command line flags, the "[apx] features" line
//   sec      per second: frames, frame ms p50/p99/max, work ms p50/p99/max (frame minus the limiter sleep),
//            per-subsystem ms (logic, ai_monster, ai_player, ent_other, render, ui, swap, limiter, net, steam, load,
//            vision, voxel, tactics, flow, director), overhead_us (this module's own cost), err/warn totals
//   spike    a frame whose work time passed the threshold, with the subsystems that ran and their ms
//   ev       events: level, ctxmenu_open/close/select, bench_open/bench_action, death, ... (see event())
//   err      a log line classified error/warning (first 3 per distinct message, then counted)
//   errsum   every minute: top distinct messages with counts
//   crash    exception code/address, module+offset, symbolized stack, the last events (also crash-*.txt and a .dmp)
// Caps: 12 MB per session (past it only err/spike/crash/sec-every-10s are written), 10 sessions kept,
//       10 crash files kept. APX_DIAG=0 turns the module off (the control in the gate).
#pragma once
#include <cstdint>
#include <string>

struct _EXCEPTION_POINTERS;

namespace apxdiag
{
	enum Sys
	{
		S_LOGIC, S_AI_MONSTER, S_AI_PLAYER, S_ENT_OTHER, S_RENDER, S_UI, S_SWAP, S_LIMITER, S_NET, S_STEAM, S_LOAD,
		S_VISION, S_VOXEL, S_TACTICS, S_FLOW, S_DIRECTOR, S_OTHER, S_N
	};
	extern bool on;
	extern bool sampleTick;                       // true on the ticks whose per-entity categories are timed (every 8th)

	void init(int argc, char** argv);             // first thing in main()
	void shutdown();                              // normal exit
	void frame();                                 // top of every main-loop iteration
	void logLine(const char* line);               // from printlog: classify, count, mirror
	void event(const char* type, const char* kv = ""); // kv = pre-built JSON members: "\"a\":1,\"b\":\"x\""
	void eventf(const char* type, const char* fmt, ...);
	int64_t nowUs();
	void addUs(int sys, int64_t us);
	long crash(_EXCEPTION_POINTERS* e, const char* why); // writes crash txt + row; the caller still writes the dump
	void plantedCrash();                          // APX_DIAG_PLANT=crash test hook (deliberate null write)
	void plantedChecks(uint32_t tick);            // APX_DIAG_PLANT=stall|error|crash:<tick> test hooks
	int selfTest();                               // pure-logic tests; returns the number of failures

	struct Scope
	{
		int s;
		int64_t t0;
		bool live;
		explicit Scope(int sys, bool force = true) : s(sys), t0(0), live(on && force) { if ( live ) { t0 = nowUs(); } }
		~Scope() { if ( live ) { addUs(s, nowUs() - t0); } }
	};
}

// Scoped timer on the main thread. APX_SCOPE_SAMPLED only times every 8th tick (per-entity categories).
#define APX_SCOPE_CAT2(a, b) a##b
#define APX_SCOPE_CAT(a, b) APX_SCOPE_CAT2(a, b)
#define APX_SCOPE(SYS) apxdiag::Scope APX_SCOPE_CAT(apxScope_, __LINE__)(apxdiag::SYS)
#define APX_SCOPE_SAMPLED(SYS) apxdiag::Scope APX_SCOPE_CAT(apxScope_, __LINE__)(apxdiag::SYS, apxdiag::sampleTick)
