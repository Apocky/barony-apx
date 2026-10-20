// apx/vox/looknet.cpp -- see looknet.hpp.
#include "../../main.hpp"
#include "../../game.hpp"
#include "../../net.hpp"
#include "../../player.hpp"
#include "bx1.hpp"
#include "heldmodel.hpp"
#include "looknet.hpp"

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace apx
{
namespace vox
{
namespace looknet
{
	namespace
	{
		constexpr int kHead = 14;                       // "LOOK" u32 hash, u16 index, u16 count, u16 len
		constexpr int kChunk = NET_PACKET_SIZE - kHead - 16;
		struct Partial { int count = 0, got = 0; std::map<int, std::vector<uint8_t>> chunks; };
		std::map<uint32_t, Partial> g_partial;
		std::set<uint32_t> g_asked;                     // looks this client already requested
		Stats g_stats;

		void put16(uint8_t* p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
		void put32(uint8_t* p, uint32_t v) { for ( int k = 0; k < 4; ++k ) { p[k] = (uint8_t)(v >> (8 * k)); } }
		int get16(const uint8_t* p) { return p[0] | (p[1] << 8); }
		uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

		// send every chunk of a model to one address (exceptClient: the server's relay skips the sender)
		void sendTo(uint32_t look, const std::vector<uint8_t>& bytes, int toClientIndex)
		{
			const int count = (int)((bytes.size() + kChunk - 1) / kChunk);
			for ( int i = 0; i < count; ++i )
			{
				const int off = i * kChunk, len = (int)std::min<size_t>(kChunk, bytes.size() - off);
				memcpy(net_packet->data, "LOOK", 4);
				put32(net_packet->data + 4, look);
				put16(net_packet->data + 8, i);
				put16(net_packet->data + 10, count);
				put16(net_packet->data + 12, len);
				memcpy(net_packet->data + kHead, bytes.data() + off, len);
				static const bool corrupt = std::getenv("APX_LOOK_CORRUPT") && std::getenv("APX_LOOK_CORRUPT")[0] == '1'; // control: the receiver must reject
				if ( corrupt && len > 0 ) { net_packet->data[kHead + len / 2] ^= 0x5A; }
				net_packet->len = kHead + len;
				if ( toClientIndex < 0 ) { net_packet->address.host = net_server.host; net_packet->address.port = net_server.port; sendPacketSafe(net_sock, -1, net_packet, 0); }
				else { net_packet->address.host = net_clients[toClientIndex].host; net_packet->address.port = net_clients[toClientIndex].port; sendPacketSafe(net_sock, -1, net_packet, toClientIndex); }
				++g_stats.chunksSent;
			}
		}

		void relayToClients(uint32_t look, const std::vector<uint8_t>& bytes, int exceptPlayer)
		{
			for ( int c = 1; c < MAXPLAYERS; ++c )
			{
				if ( client_disconnected[c] || c == exceptPlayer || (players[c] && players[c]->isLocalPlayer()) ) { continue; }
				sendTo(look, bytes, c - 1);
			}
		}

		// assemble; when complete, check the content hash and store the model; returns the bytes (empty until complete)
		std::vector<uint8_t> receive(const uint8_t* d, int len)
		{
			if ( len < kHead ) { return {}; }
			const uint32_t look = get32(d + 4);
			const int index = get16(d + 8), count = get16(d + 10), n = get16(d + 12);
			if ( count <= 0 || count > 64 || index >= count || n > len - kHead ) { ++g_stats.rejected; return {}; }
			Partial& p = g_partial[look];
			p.count = count;
			if ( !p.chunks.count(index) ) { p.chunks[index] = std::vector<uint8_t>(d + kHead, d + kHead + n); ++p.got; }
			if ( p.got < p.count ) { return {}; }
			std::vector<uint8_t> all;
			for ( auto& kv : p.chunks ) { all.insert(all.end(), kv.second.begin(), kv.second.end()); }
			g_partial.erase(look);
			bx1::VoxModel m; std::string err;
			if ( !bx1::readBytes(all, m, err) || bx1::hash(m) != look ) { ++g_stats.rejected; printlog("[apx-vox] LOOK %08x rejected (%s)", look, err.empty() ? "hash mismatch" : err.c_str()); return {}; }
			if ( storeLook(m) == look ) { registerModel(m); lookArrived(look); ++g_stats.received; printlog("[apx-vox] LOOK %08x received (%zu bytes)", look, all.size()); }
			return all;
		}
	} // namespace

	const Stats& stats() { return g_stats; }

	namespace
	{
		// which client sent the current packet: match the address (-1: unknown)
		int senderPlayer()
		{
			for ( int c = 1; c < MAXPLAYERS; ++c ) { if ( !client_disconnected[c] && net_clients[c - 1].host == net_packet->address.host && net_clients[c - 1].port == net_packet->address.port ) { return c; } }
			return -1;
		}
	} // namespace

	void announce(uint32_t look, const bx1::VoxModel& m)
	{
		if ( multiplayer == SINGLE || !look ) { return; }
		static const bool skip = std::getenv("APX_LOOK_NOANNOUNCE") && std::getenv("APX_LOOK_NOANNOUNCE")[0] == '1'; // test: the clients miss it (as a later joiner does)
		if ( skip ) { printlog("[apx-vox] LOOK %08x not announced (APX_LOOK_NOANNOUNCE)", look); return; }
		const std::vector<uint8_t> bytes = bx1::toBytes(m);
		if ( multiplayer == CLIENT ) { sendTo(look, bytes, -1); }   // to the server, which relays
		else { relayToClients(look, bytes, -1); }                    // the host: to every client
		++g_stats.announced;
		printlog("[apx-vox] LOOK %08x announced (%zu bytes)", look, bytes.size());
	}

	void serverRecv()
	{
		const uint8_t* d = net_packet->data;
		const int len = net_packet->len;
		const int from = senderPlayer();
		const std::vector<uint8_t> all = receive(d, len);
		if ( !all.empty() ) { relayToClients(get32(d + 4), all, from); }
	}

	void request(uint32_t look)
	{
		if ( multiplayer != CLIENT || !look || g_asked.count(look) ) { return; }
		static const bool off = std::getenv("APX_LOOK_NOREQ") && std::getenv("APX_LOOK_NOREQ")[0] == '1'; // control: no request, the part stays procedural
		if ( off ) { return; }
		g_asked.insert(look);
		memcpy(net_packet->data, "LKRQ", 4);
		put32(net_packet->data + 4, look);
		net_packet->len = 8;
		net_packet->address.host = net_server.host; net_packet->address.port = net_server.port;
		sendPacketSafe(net_sock, -1, net_packet, 0);
		++g_stats.requested;
		printlog("[apx-vox] LKRQ %08x requested", look);
	}

	void serverRecvRequest()
	{
		if ( net_packet->len < 8 ) { return; }
		const uint32_t look = get32(net_packet->data + 4);
		const int from = senderPlayer();
		const bx1::VoxModel* m = findLook(look);
		if ( from < 1 || !m ) { ++g_stats.unknown; printlog("[apx-vox] LKRQ %08x from %d: %s", look, from, m ? "unknown sender" : "look not here"); return; }
		sendTo(look, bx1::toBytes(*m), from - 1);
		++g_stats.served;
		printlog("[apx-vox] LKRQ %08x served to player %d", look, from);
	}

	void clientRecv() { receive(net_packet->data, net_packet->len); }
} // namespace looknet
} // namespace vox
} // namespace apx
