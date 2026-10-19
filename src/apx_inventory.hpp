// apx: our inventory (slice U1, decision D57; Apockalypsis handovers/tasks/BU1-inventory-bench-card.md).
// Storage stays Barony's (Item x/y, saved and networked); every player gets a virtual grid wide
// enough never to fill, and capacity is carried mass against a limit, never free slots.
// The vanilla grid is the CONTROL: APX_GRID=1 in the environment restores it unchanged.
#pragma once
#include <string>
#include <vector>
#include "main.hpp"

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

	// Step 2: the list frame (apx_inventory_ui.cpp). listUpdate runs after Inventory_t::updateInventory
	// and replaces the grid's slot frames while the inventory is open; listHandleMove takes Up/Down
	// from Player::GUI_t::handleInventoryMovement so the grid cursor and the list never both move.
	struct ListState { int selected = 0; int scroll = 0; int visibleRows = 0; Uint32 selectedUid = 0; int selectedKind = -1; int rowCount = 0; int rowsDrawn = 0; int moves = 0; };
	bool listActive(int player);
	void listUpdate(int player, ::Frame* inventoryFrame, ::Frame* invSlots, ::Frame* backpackSlots);
	bool listHandleMove(int player);
	const ListState& listState(int player);

	// -apxselftest=inventory: runs once on the test floor, writes JSONL, then quits the game.
	void selfTestArm(const char* which);
	void selfTestTick();
	const char* takeShotRequest(); // a screenshot file name the test wants this frame, or nullptr
}
