// apx/vox/looknet.hpp -- BX1 (C10): player-built models travel to the other players ("LOOK" packets).
//
// A built model (its .vox bytes, at most about 2 KB) goes out in chunks of NET_PACKET_SIZE: a client sends it to the
// server, which stores it and relays it to every other client; the host sends to every client. A receiver puts the
// chunks together, checks that the bytes hash to the look id (a wrong or forged model is rejected), then stores and
// registers it, so a part with that look is drawn from it. Not yet: a player who joins later gets earlier looks only
// when a new copy is announced (until then that part is drawn from its procedural model).
#pragma once

#include <cstdint>

namespace apx
{
namespace bx1 { struct VoxModel; }
namespace vox
{
namespace looknet
{
	void announce(uint32_t look, const bx1::VoxModel& m); // after a build (no-op in single player)
	void serverRecv();                                      // net.cpp server handler for 'LOOK'
	void clientRecv();                                      // net.cpp client handler for 'LOOK'
	void request(uint32_t look);                            // client: ask the server for a missing look (once per look)
	void serverRecvRequest();                               // net.cpp server handler for 'LKRQ'
	struct Stats { int announced = 0, chunksSent = 0, received = 0, rejected = 0, requested = 0, served = 0, unknown = 0; };
	const Stats& stats();
} // namespace looknet
} // namespace vox
} // namespace apx
