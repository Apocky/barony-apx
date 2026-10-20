// tests/apx/test_in.cpp -- offline units for apx::in (D93 C0). No SDL, no Barony.
//   test_in <data/apx/input dir>            exit 0 = all pass
//   test_in <dir> --control                 the old raw-level sampling consumer instead of apx::in: must FAIL (exit 1)
#include "../../src/apx/in/in.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace apx::in;

static int g_fail = 0, g_n = 0;
#define CHECK(c, ...) do { ++g_n; if ( !(c) ) { ++g_fail; printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while ( 0 )

// SDL scancode values for the keys the shipped bindings use
static int scan(const char* n)
{
	static const std::map<std::string, int> m = { { "Space", 44 }, { "Return", 40 }, { "Escape", 41 }, { "Tab", 43 }, { "`", 53 },
		{ "Left Ctrl", 224 }, { "Left Shift", 225 }, { "Q", 20 }, { "E", 8 }, { "F", 9 }, { "V", 25 }, { "C", 6 }, { "I", 12 }, { "M", 16 },
		{ "W", 26 }, { "A", 4 }, { "S", 22 }, { "D", 7 }, { "1", 30 }, { "2", 31 }, { "3", 32 }, { "4", 33 }, { "5", 34 }, { "6", 35 },
		{ "7", 36 }, { "8", 37 }, { "9", 38 }, { "0", 39 } };
	auto it = m.find(n); return it == m.end() ? 0 : it->second;
}
static int symOf(int sc) { return 1000 + sc; }

static bool g_game = true, g_vmouse = false, g_vkey = false, g_amouse = false, g_akey = false;
static bool inGame(int) { return g_game; }
static bool vMouse(int) { return g_vmouse; }
static bool vKey(int) { return g_vkey; }
static bool aMouse(int) { return g_amouse; }
static bool aKey(int) { return g_akey; }
static int kbp() { return 0; }
static int padp(int) { return 0; }

struct Sink : CompatSink
{
	std::vector<std::string> log;
	std::map<int, bool> key; bool mouse[8] = {};
	void setKey(int sym, bool d) override { key[sym] = d; log.push_back("k" + std::to_string(sym) + (d ? "1" : "0")); }
	void setMouse(int b, bool d) override { mouse[b] = d; log.push_back("m" + std::to_string(b) + (d ? "1" : "0")); }
	void pulseWheel(bool up) override { log.push_back(up ? "wu" : "wd"); }
	void setMouseRel(float dx, float dy) override { if ( dx == 0.f && dy == 0.f ) { return; } log.push_back("rel" + std::to_string((int)dx) + "," + std::to_string((int)dy)); }
};

static Raw key(bool down, const char* n, bool rep = false) { Raw r; r.type = down ? R_KeyDown : R_KeyUp; r.code = scan(n); r.sym = symOf(r.code); r.repeat = rep; return r; }
static Raw mouse(bool down, int b) { Raw r; r.type = down ? R_MouseDown : R_MouseUp; r.code = b; return r; }
static Raw move(float dx, float dy) { Raw r; r.type = R_MouseMove; r.dx = dx; r.dy = dy; return r; }

static void setup(const std::string& dir)
{
	Env e; e.keyboardPlayer = kbp; e.padPlayer = padp; e.inGame = inGame; e.vanillaMouse = vMouse; e.vanillaKeyboard = vKey;
	e.apxuiMouse = aMouse; e.apxuiKeyboard = aKey; e.scancodeFromName = scan; e.symFromScancode = symOf;
	std::string err;
	const bool ok = init(dir.c_str(), "", e, &err);
	if ( !ok ) { printf("init failed: %s\n", err.c_str()); exit(2); }
	if ( !err.empty() ) { printf("binding notes: %s\n", err.c_str()); ++g_fail; }
	g_game = true; g_vmouse = g_vkey = g_amouse = g_akey = false;
	beginFrame(1);   // the host runs the owner gate at the top of every frame
}

int main(int argc, char** argv)
{
	const std::string dir = argc > 1 ? argv[1] : "data/apx/input";
	const bool control = argc > 2 && !strcmp(argv[2], "--control");
	Sink sink;

	// ---- 1. a tap between two ticks is not lost (the core claim)
	setup(dir); sink = Sink();
	beginFrame(1000);
	event(mouse(true, 1)); event(mouse(false, 1));            // 5 ms click inside one frame
	endFrame(sink);
	TickRecord r = takeNow(0);
	if ( control )
	{
		// the OLD consumer: samples the level at the tick. The press is gone: that is the failure apx::in fixes.
		const bool levelAtTick = false; // mousestatus/Input state after down+up in one batch
		CHECK(levelAtTick, "control: old raw path saw the tap");
	}
	else
	{
		CHECK(r.pressCount[B_Attack] == 1 && r.releaseCount[B_Attack] == 1 && !r.isHeld(B_Attack) && r.tapped(B_Attack), "tap p=%d r=%d held=%d", r.pressCount[B_Attack], r.releaseCount[B_Attack], (int)r.isHeld(B_Attack));
		CHECK(takeNow(0).pressCount[B_Attack] == 0, "a record is handed out exactly once");
	}
	if ( control ) { printf("control: %d checks, %d failed\n", g_n, g_fail); return g_fail ? 1 : 0; }

	// ---- 2. hold across ticks, press counted once, release counted once
	setup(dir);
	event(key(true, "Left Shift")); r = takeNow(0);
	CHECK(r.isHeld(B_Sprint) && r.pressCount[B_Sprint] == 1, "sprint press");
	r = takeNow(0); CHECK(r.isHeld(B_Sprint) && r.pressCount[B_Sprint] == 0, "sprint still held, no new press");
	event(key(true, "Left Shift", true)); r = takeNow(0); CHECK(r.pressCount[B_Sprint] == 0, "key repeat is not a press");
	event(key(false, "Left Shift")); r = takeNow(0); CHECK(!r.isHeld(B_Sprint) && r.releaseCount[B_Sprint] == 1, "sprint release");

	// ---- 3. several presses inside one window are all counted
	setup(dir);
	for ( int i = 0; i < 7; ++i ) { event(mouse(true, 1)); event(mouse(false, 1)); }
	r = takeNow(0); CHECK(r.pressCount[B_Attack] == 7 && r.releaseCount[B_Attack] == 7, "7 taps -> %d/%d", r.pressCount[B_Attack], r.releaseCount[B_Attack]);

	// ---- 4. two sources of one action: press once while any is held
	setup(dir);
	event(key(true, "Return")); event(key(true, "Return", false)); r = takeNow(0);
	CHECK(r.pressCount[B_Chat] == 1 && r.pressCount[B_UiAccept] == 1, "return feeds Chat and UiAccept");

	// ---- 5. movement axes from keys, mouse look sums, swing during Attack
	setup(dir);
	event(key(true, "W")); event(key(true, "D")); r = takeNow(0);
	CHECK(r.moveY == 1.f && r.moveX == 1.f, "wd move %g %g", r.moveX, r.moveY);
	event(key(true, "S")); r = takeNow(0); CHECK(r.moveY == 0.f, "w+s cancel");
	event(key(false, "S")); event(key(false, "W")); event(key(false, "D"));
	event(move(3, -2)); event(mouse(true, 1)); event(move(5, 7)); r = takeNow(0);
	CHECK(r.lookDX == 8.f && r.lookDY == 5.f, "look sum %g %g", r.lookDX, r.lookDY);
	CHECK(r.swingDX == 5.f && r.swingDY == 7.f, "swing %g %g", r.swingDX, r.swingDY);
	CHECK(r.mouseDX == 8.f && r.mouseDY == -2.f + 7.f, "raw mouse %g %g", r.mouseDX, r.mouseDY);
	event(mouse(false, 1)); takeNow(0);

	// ---- 6. owner gate: a UI that owns the pointer gets the clicks, the game gets none; counted as gated, not lost
	setup(dir); sink = Sink();
	g_amouse = true; beginFrame(2000);
	CHECK(uiOwnsMouse(0) && mouseOwner(0) == OWN_APXUI, "apx ui owns the mouse");
	event(mouse(true, 1)); event(mouse(false, 1)); endFrame(sink); r = takeNow(0);
	CHECK(r.pressCount[B_Attack] == 0 && counters().gatedPresses == 1, "gated %llu", (unsigned long long)counters().gatedPresses);
	CHECK(sink.log.empty(), "an apx-ui-owned click never reaches the legacy state (%zu writes)", sink.log.size());
	CHECK(counters().lost() == 0, "lost %llu", (unsigned long long)counters().lost());

	// ---- 7. owner switch delivers releases; a button held into the game is ignored until released
	setup(dir); sink = Sink();
	event(mouse(true, 1)); endFrame(sink); r = takeNow(0); CHECK(r.isHeld(B_Attack), "attack held");
	CHECK(sink.mouse[1], "legacy sees the held button");
	g_amouse = true; beginFrame(3000);
	r = takeNow(0); CHECK(!r.isHeld(B_Attack) && r.releaseCount[B_Attack] == 1, "switch to ui delivered the release (held=%d rel=%d)", (int)r.isHeld(B_Attack), r.releaseCount[B_Attack]);
	endFrame(sink); CHECK(!sink.mouse[1], "and the legacy side got its release too");
	g_amouse = false; beginFrame(3100); r = takeNow(0);
	CHECK(!r.isHeld(B_Attack) && r.pressCount[B_Attack] == 0, "back to the game: the held button is not a new press");
	event(mouse(false, 1)); r = takeNow(0); CHECK(r.releaseCount[B_Attack] == 0, "its release is swallowed");
	event(mouse(true, 1)); r = takeNow(0); CHECK(r.pressCount[B_Attack] == 1, "a fresh press works again");
	event(mouse(false, 1)); takeNow(0);
	// the other direction: pressed while the ui owned it, released after it gave it back
	setup(dir); g_amouse = true; beginFrame(1); event(mouse(true, 1)); g_amouse = false; beginFrame(2);
	r = takeNow(0); CHECK(r.pressCount[B_Attack] == 0 && !r.isHeld(B_Attack), "press made under the ui does not leak into the game");
	CHECK(counters().heldIgnored >= 1, "held-ignored counted");

	// ---- 8. keyboard: a text field takes the keys, the game gets none; with a window open but no text field the keys work
	setup(dir); sink = Sink();
	g_vkey = true; beginFrame(1); event(key(true, "W")); event(key(true, "Left Shift")); r = takeNow(0);
	CHECK(r.moveY == 0.f && r.pressCount[B_Sprint] == 0, "text field active: no move, no sprint");
	endFrame(sink); CHECK(sink.key[symOf(26)], "typing still reaches the legacy keystate (vanilla owns the field)");
	g_vkey = false; beginFrame(2); event(key(false, "W")); event(key(false, "Left Shift"));
	g_vmouse = true; beginFrame(3); event(key(true, "W")); r = takeNow(0);
	CHECK(r.moveY == 1.f, "vanilla window open (mouse owned by it) but no text field: W still walks");
	g_vmouse = false; beginFrame(4);

	// ---- 9. compat feed: a tap stays visible for one sim tick, a long press follows the key
	setup(dir); sink = Sink();
	event(mouse(true, 1)); event(mouse(false, 1)); endFrame(sink);
	CHECK(sink.log.size() == 1 && sink.log[0] == "m11", "frame feed sets the button (%s)", sink.log.empty() ? "-" : sink.log[0].c_str());
	tickAll(sink); CHECK(sink.log.size() == 1, "first tick still sees it");
	tickAll(sink); CHECK(sink.log.size() == 2 && sink.log[1] == "m10", "second tick clears it");
	sink = Sink(); event(key(true, "Space")); endFrame(sink); event(key(false, "Space")); endFrame(sink);
	CHECK(sink.log.size() == 1 && sink.log[0] == "k10441", "released before any tick sampled it: stays set (%zu)", sink.log.size());
	tickAll(sink); tickAll(sink); CHECK(sink.log.size() == 2 && sink.log[1] == "k10440", "cleared after one tick");
	sink = Sink(); event(key(true, "Space")); endFrame(sink); tickAll(sink); event(key(false, "Space")); endFrame(sink);
	CHECK(sink.log.size() == 2 && sink.log[1] == "k10440", "a tick sampled it: the release goes through at once");
	sink = Sink(); event(key(true, "Q")); endFrame(sink); event(key(true, "Q", true)); endFrame(sink);
	CHECK(sink.log.size() == 2, "key repeat re-sets the legacy state (vanilla did); no action press");
	event(key(false, "Q")); endFrame(sink);

	// ---- 10. compat feed obeys the apx-ui owner for keyboard too
	setup(dir); sink = Sink(); g_akey = true; beginFrame(1);
	event(key(true, "E")); endFrame(sink); CHECK(sink.log.empty(), "apx ui text field: the legacy state gets nothing");
	g_akey = false; beginFrame(2); event(key(false, "E")); endFrame(sink); CHECK(sink.log.empty(), "release of a blocked key is not delivered either");

	// ---- 11. wheel: a pulse on the action side, a pulse to the legacy side
	setup(dir); sink = Sink();
	Raw w; w.type = R_Wheel; w.value = -1.f; event(w); endFrame(sink); r = takeNow(0);
	CHECK(r.pressCount[B_HotbarNext] == 1 && r.releaseCount[B_HotbarNext] == 1 && sink.log.size() == 1 && sink.log[0] == "wd", "wheel down -> HotbarNext");

	// ---- 12. fuzz: nothing is lost, counters balance
	setup(dir); sink = Sink();
	unsigned s = 12345u; unsigned long long injected = 0, seen = 0; bool down[3] = {};
	auto rnd = [&]() { s = s * 1664525u + 1013904223u; return s >> 8; };
	for ( int i = 0; i < 20000; ++i )
	{
		const int k = rnd() % 3;
		if ( !down[k] ) { event(k == 0 ? mouse(true, 1) : k == 1 ? key(true, "Left Shift") : key(true, "Space")); down[k] = true; ++injected; }
		else if ( rnd() % 2 ) { event(k == 0 ? mouse(false, 1) : k == 1 ? key(false, "Left Shift") : key(false, "Space")); down[k] = false; }
		if ( rnd() % 5 == 0 ) { endFrame(sink); }
		if ( rnd() % 3 == 0 )
		{
			tickAll(sink); const TickRecord& t = take(0);
			seen += t.pressCount[B_Attack] + t.pressCount[B_Sprint] + t.pressCount[B_Dodge];
		}
	}
	tickAll(sink); { const TickRecord& t = take(0); seen += t.pressCount[B_Attack] + t.pressCount[B_Sprint] + t.pressCount[B_Dodge]; }
	CHECK(seen == injected, "fuzz injected %llu seen %llu", injected, seen);
	CHECK(counters().lost() == 0, "fuzz lost %llu", (unsigned long long)counters().lost());

	// ---- 13. row round trip and replay are bit-exact
	setup(dir); sink = Sink();
	std::vector<std::string> rows;
	unsigned s2 = 777u; auto rnd2 = [&]() { s2 = s2 * 1664525u + 1013904223u; return s2 >> 8; };
	for ( int t = 0; t < 300; ++t )
	{
		if ( rnd2() % 4 == 0 ) { event(mouse(true, 1)); }
		if ( rnd2() % 4 == 0 ) { event(mouse(false, 1)); }
		if ( rnd2() % 6 == 0 ) { event(key(true, "W")); }
		if ( rnd2() % 6 == 0 ) { event(key(false, "W")); }
		event(move((float)(rnd2() % 9) - 4.f, (float)(rnd2() % 7) * 0.37f));
		tickAll(sink); rows.push_back(recordRow(0, take(0), true, t * 0.5, t * 1.25, 0.1 * t));
	}
	std::string f1 = "test_in_rows.jsonl";
	{ FILE* f = fopen(f1.c_str(), "wb"); for ( auto& rr : rows ) { fputs(rr.c_str(), f); fputc('\n', f); } fclose(f); }
	std::string rerr; setup(dir);
	CHECK(replayOpen(f1.c_str(), &rerr), "replayOpen %s", rerr.c_str());
	Sink sink2; int mism = 0;
	for ( size_t t = 0; t < rows.size(); ++t )
	{
		tickAll(sink2);
		const std::string again = recordRow(0, take(0), true, t * 0.5, t * 1.25, 0.1 * t);
		if ( again != rows[t] ) { if ( ++mism <= 3 ) { printf("row %zu differs\n  %s\n  %s\n", t, rows[t].c_str(), again.c_str()); } }
	}
	CHECK(mism == 0, "replay rows differing: %d of %zu", mism, rows.size());
	CHECK(replayDone(), "replay consumed every row");
	remove(f1.c_str());

	printf("test_in: %d checks, %d failed\n", g_n, g_fail);
	return g_fail ? 1 : 0;
}
