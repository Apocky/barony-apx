// apx/selftest_all.cpp -- the -apxselftest=all sequencer (see selftest_all.hpp).
#include "../main.hpp"
#include "selftest_all.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace apx
{
namespace selftestall
{
	namespace
	{
		struct Result { bool ran = false; bool pass = false; long long ms = 0; std::string fields; };
		bool g_armed = false;
		int g_stage = -1;          // -1 waiting for the floor, NUM_STAGES finished
		int g_settle = 0;          // ticks on the floor before the first stage
		Result g_result[NUM_STAGES];
		InlineStage g_inline[NUM_STAGES] = {};
		StageBeginHook g_beginHook = nullptr;
		bool g_sweeping = false;
		std::chrono::steady_clock::time_point g_launch = std::chrono::steady_clock::now();
		std::chrono::steady_clock::time_point g_stageStart;
		bool g_finished = false;

		long long msSince(std::chrono::steady_clock::time_point t)
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
		}

		int stageSeconds()
		{
			const char* v = std::getenv("APX_SELFTEST_STAGE_S");
			const int s = v ? std::atoi(v) : 0;
			return s > 0 ? s : 120; // windowed, the moves stage takes ~97 s; headless all stages ~5 s
		}

		int budgetSeconds()
		{
			const char* v = std::getenv("APX_SELFTEST_BUDGET_S");
			const int s = v ? std::atoi(v) : 0;
			return s > 0 ? s : 240;
		}

		void begin(int s)
		{
			g_stage = s;
			g_stageStart = std::chrono::steady_clock::now();
			if ( s < NUM_STAGES ) { printlog("[apx-selftest] stage %s begins at %lld ms", name((Stage)s), msSince(g_launch)); }
			if ( s < NUM_STAGES && g_beginHook ) { g_beginHook((Stage)s); }
		}

		void finish(const char* why)
		{
			if ( g_finished ) { return; }
			g_finished = true;
			int pass = 0, fail = 0;
			for ( int s = 0; s < NUM_STAGES; ++s ) { (g_result[s].ran && g_result[s].pass ? pass : fail)++; }
			const long long total = msSince(g_launch);

			const char* d = std::getenv("APX_DIAG_DIR");
			const std::string dir = d && d[0] ? d : "C:/Apocrypha/diagnostics/barony-selftest";
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			std::string json = "{\"test\":\"selftest-all\",\"pass\":";
			json += fail == 0 ? "true" : "false";
			char buf[160];
			snprintf(buf, sizeof buf, ",\"passed\":%d,\"failed\":%d,\"elapsed_ms\":%lld,\"ended\":\"%s\",\"checks\":{", pass, fail, total, why);
			json += buf;
			for ( int s = 0; s < NUM_STAGES; ++s )
			{
				const Result& r = g_result[s];
				snprintf(buf, sizeof buf, "%s\"%s\":{\"pass\":%s,\"ran\":%s,\"ms\":%lld", s ? "," : "", name((Stage)s),
					r.ran && r.pass ? "true" : "false", r.ran ? "true" : "false", r.ms);
				json += buf;
				if ( !r.fields.empty() ) { json += ","; json += r.fields; }
				json += "}";
			}
			json += "}}";
			if ( FILE* f = fopen((dir + "/selftest-all.json").c_str(), "w") ) { fputs(json.c_str(), f); fputc('\n', f); fclose(f); }
			if ( FILE* f = fopen((dir + "/selftest-all.jsonl").c_str(), "a") ) { fputs(json.c_str(), f); fputc('\n', f); fclose(f); }
			printlog("[apx-selftest] summary pass=%d fail=%d elapsed_ms=%lld ended=%s file=%s/selftest-all.json", pass, fail, total, why, dir.c_str());
			mainloop = 0;
		}
	} // namespace

	void arm() { g_armed = true; g_launch = std::chrono::steady_clock::now(); }
	bool armed() { return g_armed; }
	bool active(Stage s) { return g_armed && !g_finished && g_stage == (int)s; }

	const char* name(Stage s)
	{
		static const char* names[NUM_STAGES] = { "features", "impact", "tierdmg", "voxel", "inventory", "moves", "f4c" };
		return s >= 0 && s < NUM_STAGES ? names[s] : "?";
	}

	void setInline(Stage s, InlineStage fn) { if ( s >= 0 && s < NUM_STAGES ) { g_inline[s] = fn; } }
	void setStageBeginHook(StageBeginHook fn) { g_beginHook = fn; }
	bool sweeping() { return g_sweeping; }

	void done(Stage s, bool pass, const std::string& fields)
	{
		if ( !active(s) ) { return; }
		Result& r = g_result[s];
		r.ran = true;
		r.pass = pass;
		r.ms = msSince(g_stageStart);
		r.fields = fields;
		printlog("[apx-selftest] stage %s %s in %lld ms {%s}", name(s), pass ? "PASS" : "FAIL", r.ms, fields.c_str());
		begin(g_stage + 1);
		if ( g_stage >= NUM_STAGES ) { finish("done"); }
	}

	void tick(bool ready)
	{
		if ( !g_armed || g_finished ) { return; }
		if ( msSince(g_launch) > budgetSeconds() * 1000LL )
		{
			printlog("[apx-selftest] budget of %d s used up during stage %s", budgetSeconds(), g_stage >= 0 ? name((Stage)g_stage) : "(waiting for the floor)");
			finish("timeout");
			return;
		}
		if ( g_stage < 0 )
		{
			if ( !ready ) { g_settle = 0; return; }
			if ( ++g_settle < 50 ) { return; } // let the floor settle for one second of game time
			begin(0);
		}
		// Barony spawns new monsters while a long stage runs (the moves stage is ~4900 ticks); one that walks up to the standing
		// player kills it (observed under load: "player 0 gone during this stage"). Sweep the test floor every half second.
		if ( g_stage == MOVES && g_beginHook && ready ) // only here: the f4c stage spawns hostile test targets itself
		{
			static int n = 0;
			if ( ++n % 25 == 0 ) { g_sweeping = true; g_beginHook((Stage)g_stage); g_sweeping = false; }
		}
		if ( !ready ) // player 0 died (or left the floor): the stage cannot go on, nor can the ones after it
		{
			const int s = g_stage;
			done((Stage)s, false, "\"error\":\"player 0 gone during this stage\"");
			for ( int k = s + 1; k < NUM_STAGES; ++k ) { g_result[k].fields = "\"error\":\"not run: player 0 gone\""; }
			finish("player_gone");
			return;
		}
		if ( msSince(g_stageStart) > stageSeconds() * 1000LL )
		{
			printlog("[apx-selftest] stage %s ran over its %d s", name((Stage)g_stage), stageSeconds());
			done((Stage)g_stage, false, "\"error\":\"stage timeout\"");
			if ( g_finished ) { return; }
		}
		while ( g_stage < NUM_STAGES && g_inline[g_stage] )
		{
			std::string fields;
			const bool pass = g_inline[g_stage](fields);
			done((Stage)g_stage, pass, fields);
		}
	}
} // namespace selftestall
} // namespace apx
