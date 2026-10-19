// apx-gen bridge: fills a Barony floor from the apx-gen core (src/apx/gen, spec 02) when enabled.
// The vanilla generateDungeon stays the CONTROL: with the switch off nothing here runs.
#pragma once
#include <cstdint>
#include <string>

struct ApxGenConfig
{
	bool enabled = false;      // -apxgen or APXGEN=1
	int size = 128;            // -apxgen-size=N (tiles per side, 48..160)
	int populationPct = 100;   // -apxgen-pop=N (percent of the 1x density)
	std::string kitsRoot;      // -apxgen-root=DIR or APXGEN_ROOT; default: the vendored copy
	int control = 0;           // APXGEN_CONTROL=1: move each key behind its own lock (W2 must fail)
};
extern ApxGenConfig apxgen;

// Reads APXGEN* environment variables (command line wins; called before argument parsing).
void apxgenReadEnvironment();
// Handles one -apxgen* argument; true if consumed.
bool apxgenParseArgument(const char* arg);

// Generates the floor into the global map (tiles + entity rows). Returns the secret-exit code (0)
// or -1 on failure (logged; the caller then runs the vanilla generator and says so in the log).
int apxgenGenerateDungeon(const char* levelset, uint32_t seed);

// W1 instruments (always compiled; idle unless used):
void apxgenFrameTick();                        // game.cpp, once per frame after the fps average
void apxgenNetSent(int hostnum, int bytes);    // net.cpp sendPacket
// console: /apxfps_begin, /apxfps_end (p50/p99 frame ms over the window), /apxhall (teleport the
// local player to the centre of the floor's largest room), /apxnet (bytes/s sent per host since last call)
