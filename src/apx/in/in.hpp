// apx/in/in.hpp -- D93 phase C0: apx::in, the one owner of play input. ASCII; no SDL in this header or in in.cpp,
// so the pure core compiles offline (tests/apx/test_in.cpp). The Barony glue is in_glue.cpp.
//
// Contract (specs/barony-fork/04b section 3):
//   const TickRecord& take(int player)   once per sim tick; hands the latched record out exactly once
//   bool uiOwnsMouse(int player), uiOwnsKeyboard(int player)
// Latching: per action, held now plus pressCount/releaseCount since the last take(); a press and a release between
// two ticks arrive as a tap (1 press, 1 release, not held). Analog values and mouse motion are summed.
// Owner gate (once per frame, beginFrame): per device and player the owner is GAME, VANILLA (a vanilla menu or
// window in use) or APXUI (apx::ui wants the device: pointer over our UI, a UI drag, a text field). Only GAME
// feeds actions. An owner switch delivers releases for everything held and ignores held sources until released.
// Compat feed (TEMPORARY, shrinks as systems are rewritten): code that still reads Barony's keystatus/mousestatus/
// Input state gets them from this module's latched state (a tap stays visible for one sim tick), never from raw
// SDL; APXUI-owned devices are not fed. APX_INPUT_OLD=1 turns the whole module's play path off (the CONTROL).
#pragma once
#include <cstdint>
#include <string>

namespace apx
{
namespace in
{
	constexpr int kMaxPlayers = 4;

	enum Button : int
	{
		B_Attack, B_Block, B_Dodge, B_Kick, B_Use, B_Cast, B_Sneak, B_Sprint, B_Inventory, B_Map, B_Menu,
		B_Hotbar1, B_Hotbar2, B_Hotbar3, B_Hotbar4, B_Hotbar5, B_Hotbar6, B_Hotbar7, B_Hotbar8, B_Hotbar9, B_Hotbar10,
		B_HotbarNext, B_HotbarPrev, B_Chat, B_Console, B_UiAccept, B_UiBack, B_UiNext, B_UiPrev,
		B_COUNT
	};
	enum Axis : int { X_MoveX, X_MoveY, X_LookX, X_LookY, X_COUNT };

	const char* buttonName(int b);          // nullptr when out of range
	int buttonId(const char* name);         // -1 when unknown
	int axisId(const char* name);

	struct TickRecord
	{
		uint32_t tick = 0;                  // apx::in's own tick counter (one per take-all)
		uint64_t held = 0;                  // bit per Button: held at the end of the window
		uint16_t pressCount[B_COUNT] = {};  // presses since the previous take()
		uint16_t releaseCount[B_COUNT] = {};
		uint32_t lastPressMs[B_COUNT] = {}; // event time (ms) of the newest press / release in the window
		uint32_t lastReleaseMs[B_COUNT] = {};
		float moveX = 0.f, moveY = 0.f;     // -1..1 now (keys summed and clamped; stick analog)
		float lookDX = 0.f, lookDY = 0.f;   // summed look (mouse px * sensitivity + stick per tick)
		float swingDX = 0.f, swingDY = 0.f; // the part of look that happened while Attack was held (wind-up steering)
		bool isHeld(int b) const { return (held >> b) & 1u; }
		bool tapped(int b) const { return pressCount[b] && releaseCount[b] && !isHeld(b); }
	};

	// Raw device events (the glue converts SDL events; the QA driver and tests inject them the same way).
	enum RawType : int { R_KeyDown, R_KeyUp, R_MouseDown, R_MouseUp, R_Wheel, R_MouseMove, R_PadDown, R_PadUp, R_PadAxis };
	struct Raw
	{
		RawType type = R_KeyDown;
		int code = 0;           // scancode / mouse button (1..5) / pad button / pad axis
		int sym = 0;            // key: SDL keycode (the compat feed is keyed by it)
		bool repeat = false;    // key auto-repeat (compat only, never a press)
		float value = 0.f;      // wheel ticks (+ up) / axis -1..1
		float dx = 0.f, dy = 0.f; // mouse motion
		uint32_t tsMs = 0;
		int pad = -1;           // pad slot for pad events
	};

	enum Owner : int { OWN_GAME = 0, OWN_VANILLA = 1, OWN_APXUI = 2 };
	enum Dev : int { DEV_KEY = 0, DEV_MOUSE = 1, DEV_PAD = 2, DEV_COUNT };

	// What the host game tells apx::in (function pointers; null = a sensible default).
	struct Env
	{
		int (*keyboardPlayer)() = nullptr;                // player that owns keyboard + mouse
		int (*padPlayer)(int padSlot) = nullptr;          // -1 = none
		bool (*inGame)(int player) = nullptr;             // the player has control (not a menu, not paused)
		bool (*vanillaMouse)(int player) = nullptr;       // a vanilla menu/window uses the pointer
		bool (*vanillaKeyboard)(int player) = nullptr;    // a vanilla text field is active
		bool (*apxuiMouse)(int player) = nullptr;         // apx::ui (and our ImGui windows) want the pointer
		bool (*apxuiKeyboard)(int player) = nullptr;
		int (*scancodeFromName)(const char* name) = nullptr;
		int (*symFromScancode)(int scancode) = nullptr;
		bool (*playerPos)(int player, double* x, double* y, double* yaw) = nullptr; // for the input log
	};

	// Compat sink: how the glue writes Barony's legacy state.
	struct CompatSink
	{
		virtual void setKey(int sym, bool down) = 0;
		virtual void setMouse(int button, bool down) = 0;     // 1..3 plus held state
		virtual void pulseWheel(bool up) = 0;
		virtual void addMouseRel(float dx, float dy) = 0;     // replay only
		virtual ~CompatSink() {}
	};

	struct Counters
	{
		uint64_t events = 0, pressesLatched = 0, pressesDelivered = 0, releasesLatched = 0, releasesDelivered = 0;
		uint64_t gatedPresses = 0;      // presses refused because a UI owned the device (counted, not lost)
		uint64_t ownerSwitches = 0, switchReleases = 0;
		uint64_t heldIgnored = 0;       // sources that were held when the game gained the device
		uint64_t saturated = 0;         // a count hit its 16-bit cap (counted as lost)
		uint64_t takes = 0;
		uint64_t takenPress[B_COUNT] = {}; // presses delivered through take(), per action
		uint64_t pending = 0;          // latched, not yet taken (filled by counters())
		uint64_t lost() const;          // pressesLatched - pressesDelivered - pending, plus saturated
	};

	// ---- lifecycle
	// dataDir holds actions.json and bindings.json; userDir (may be empty) holds bindings.user.json.
	bool init(const char* dataDir, const char* userDir, const Env& env, std::string* err);
	void reset();                           // drop all state (tests)
	bool oldPath();                         // APX_INPUT_OLD=1: the control, apx::in does not feed play
	void setOldPath(bool v);
	bool loaded();

	// ---- per frame
	void beginFrame(uint32_t nowMs);        // owner gate; delivers releases on a switch
	void event(const Raw& e);               // every device event, first
	void endFrame(CompatSink& sink);        // compat feed after the frame's events (a tap becomes visible)

	// ---- per sim tick
	void tickAll(CompatSink& sink);         // top of gameLogic: take for every in-game player, log, compat tick step
	const TickRecord& take(int player);     // the record handed out by the last tickAll (or a fresh take)
	TickRecord takeNow(int player);         // latch + hand out now (tests; tickAll uses it)

	bool uiOwnsMouse(int player);
	bool uiOwnsKeyboard(int player);
	Owner mouseOwner(int player);
	Owner keyboardOwner(int player);

	// ---- QA / tests
	// Resolve an action name to the first key or mouse button bound to it; fills down/up events. Returns the player.
	bool resolveAction(const char* action, Raw* down, Raw* up);
	const Counters& counters();
	std::string countersJson();             // one-line JSON members for the D90 "input" event
	uint32_t tickCounter();

	// ---- input log and replay
	bool logOpen(const char* path);
	void logClose();
	bool logEnabled();
	bool replayOpen(const char* path, std::string* err);
	bool replaying();
	bool replayDone();
	// Format helpers (public for the test).
	std::string recordRow(int player, const TickRecord& r, bool haveExtra, double x, double y, double yaw);
	bool parseRow(const std::string& line, int* player, TickRecord* out);
} // namespace in
} // namespace apx
