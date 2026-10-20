// apx: our inventory (slice U1, decision D57; Apockalypsis handovers/tasks/BU1-inventory-bench-card.md).
// Storage stays Barony's (Item x/y, saved and networked); every player gets a virtual grid wide
// enough never to fill, and capacity is carried mass against a limit, never free slots.
// The vanilla grid is the CONTROL: APX_GRID=1 in the environment restores it unchanged.
#pragma once
#include <string>
#include <vector>
#include "main.hpp"
#include "apx_inv_logic.hpp"

class Item;
class Frame;

namespace apx_inv
{
	constexpr int VIRTUAL_SLOTS = 512;   // the virtual grid holds at least this many items
	bool vanillaGrid();                  // APX_GRID=1: Barony's grid and slot capacity
	int virtualRows(int sizeX);          // rows so that sizeX * rows >= VIRTUAL_SLOTS

	// One list row: what the list frame shows and what the mass gate sums.
	struct Row
	{
		Uint32 uid = 0;       // 0 for the gold row
		int type = -1;        // ItemType, -1 for gold
		int kind = 0;         // Kind below
		int count = 0;
		Sint32 mass = 0;      // Barony weight units (Item::getWeight, gold: Stat::getGoldWeight)
		int status = 0;
		int beatitude = 0;
		bool equipped = false;
		Sint32 x = 0, y = 0;
	};
	enum Kind : int { WEAPONS, ARMOUR, TOOLS, CONSUMABLES, MATERIALS, BOOKS, GOLD, KIND_MAX };
	const char* kindName(int kind);
	int kindOf(const Item& item);

	std::vector<Row> rows(int player);           // every carried item except spells, plus gold
	Sint32 carriedMass(int player);              // Barony's own sum: PlayerMovement_t::getCharacterWeight
	Sint32 massLimit(int player);                // 1000 + 100 * max(0, STR) units (Barony's zero-speed point)
	bool canCarry(int player, const Item& item); // the pickup rule; always true under APX_GRID=1
	std::string checkRows(const std::vector<Row>& r, Sint32 carried); // "" when the rows sum to carried
	std::string massText(Sint32 units);          // "12.3 kg" (ASSUMED 1 unit = 0.1 kg)

	// D92 (spec 08b): the U1 list frame, its D81/D88 layouts and its row/tooltip/bench shell were removed;
	// the inventory window is src/apx/ui/inventory.cpp. Kept here: storage (virtual grid), the mass rule,
	// and the bench actions of apx_bench.cpp (breakItem, benchKeep) that the F6 provider and probes use.
	void breakItem(int player, Item* item);
	int benchKeep(int player);                         // the component chosen to keep in Salvage (-1 none)

	// -apxselftest=inventory: runs once on the test floor, writes JSONL, then quits the game.
	void selfTestArm(const char* which);
	void selfTestTick();
	const char* takeShotRequest(); // a screenshot file name the test wants this frame, or nullptr
}
