// apx/selftest_all.hpp -- -apxselftest=all: every in-game self-test in one process on one floor (D77).
//
// The harnesses (F4b impact, D74 tier damage, U1 inventory, F4a moves, F4c poise/zones/downed) used to run one
// per launch and quit when done. Armed together, a sequencer runs them as stages in a fixed order on the test
// floor; each harness waits for its stage (active()) and reports done() where it used to quit. At the end the
// sequencer writes one JSON summary and quits. Each check keeps its own control knob (APX_MOVES=0, ...).
#pragma once

#include <string>

namespace apx
{
namespace selftestall
{
	enum Stage : int { FEATURES = 0, IMPACT, TIERDMG, INVENTORY, MOVES, F4C, NUM_STAGES };

	void arm();                  // -apxselftest=all
	bool armed();
	bool active(Stage s);        // true while stage s runs
	const char* name(Stage s);

	// a harness reports its stage finished; fields is a JSON fragment without braces ("\"k\":1,\"j\":2") or ""
	void done(Stage s, bool pass, const std::string& fields);

	// stages that run inside one tick (features, impact, tierdmg) register a function; it fills fields
	using InlineStage = bool (*)(std::string& fields);

	// called as each stage begins, before the stage spawns anything (game.cpp: clear the floor's hostiles)
	using StageBeginHook = void (*)(Stage s);
	void setStageBeginHook(StageBeginHook fn);
	void setInline(Stage s, InlineStage fn);

	// once per game tick; ready = player 0 alive on the target floor and not in the intro
	void tick(bool ready);
} // namespace selftestall
} // namespace apx
