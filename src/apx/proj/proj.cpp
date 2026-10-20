// apx/proj/proj.cpp -- see proj.hpp. Physical projectiles: sub-stepped flight through Barony's own collision (clipMove), first-creature stop
// unless the stress beats the material's fracture, ricochet under 20 degrees, hits through the combat core.
#include "../../main.hpp"
#include "../../game.hpp"
#include "../../stat.hpp"
#include "../../entity.hpp"
#include "../../items.hpp"
#include "../../magic/magic.hpp"
#include "../../monster.hpp"
#include "../../collision.hpp"
#include "../../player.hpp"
#include "../../light.hpp"
#include "../../engine/audio/sound.hpp"
#include "../combat/impact.hpp"
#include "../magic/magic_core.hpp"
#include "proj.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

// The combat lane's core (branch apx/combat, spec 04b section 3). Present in the tree after that lane merges; until then the STUB below
// implements the same contract with Barony's applyGenericMagicDamage.
#if defined(__has_include)
#if __has_include("../combat/core.hpp")
#include "../combat/core.hpp"
#define APX_PROJ_COMBAT_CORE 1
#endif
#endif

namespace apx
{
namespace proj
{
namespace
{
	struct PS
	{
		ProjDef def;
		Uint32 uid = 0, ownerUid = 0;
		float vx = 0.f, vy = 0.f;
		int age = 0, bounces = 0, pierced = 0;
		Uint32 ignore[8] = {};          // bodies it is flying through (the owner at first, then every pierced body)
		int nIgnore = 0;
		bool active = false;
	};
	std::vector<PS> g_ps;
	int g_free = -1;
	Counters g_c;
	using Clock = std::chrono::steady_clock;
	double nowUs() { return std::chrono::duration<double, std::micro>(Clock::now().time_since_epoch()).count(); }
	double g_tickUs = 0.0;              // behaviors run so far this game tick
	int g_tickLive = 0, g_maxLive = 0;
	Uint32 g_lastTick = (Uint32)-1;
	std::vector<float> g_cost;          // us per game tick that had projectiles
	size_t g_costNext = 0;

	void closeSample()
	{
		if ( g_tickLive > 0 )
		{
			if ( g_cost.size() < 20000 ) { g_cost.push_back((float)g_tickUs); }
			else { g_cost[g_costNext++ % g_cost.size()] = (float)g_tickUs; }
			g_maxLive = std::max(g_maxLive, g_tickLive);
		}
		g_tickUs = 0.0;
		g_tickLive = 0;
	}

	bool isCreature(Entity* e) { return e && (e->behavior == &actMonster || e->behavior == &actPlayer); }

	// ---- the hit: the combat lane's applyHit, or the stub --------------------------------------------------
	struct HitOut { int damage = 0; bool killed = false, fracture = false; };

	HitOut dealHit(Entity* owner, Entity* target, Entity* projEntity, const ProjDef& d)
	{
		HitOut out;
		Stat* ts = target->getStats();
		const int hp0 = target->getHP();
#ifdef APX_PROJ_COMBAT_CORE
		// CONTRACT (04b section 3): the single damage entry point; the spell's own number is the precomputed damage, the rule stages run on it.
		apx::combat::Hit h;
		h.attacker = owner;
		h.target = target;
		h.kind = d.kind;
		h.momentum = d.momentum;
		h.area = d.area;
		h.element = d.element >= 0 ? d.element + 1 : 0;
		h.flags = apx::combat::HF_PRECOMPUTED | apx::combat::HF_NO_ZONES;
		h.damage = d.damage;
		h.path = apx::combat::PATH_ARROW;
		const apx::combat::HitResult r = apx::combat::applyHit(h);
		out.damage = r.damage;
		out.killed = r.killed;
		out.fracture = r.fracture;
		++g_c.coreHits;
		(void)projEntity;
#else
		// STUB -- remove when apx/combat/core.hpp lands (the COMBAT lane's apx::combat::applyHit). Same contract, Barony's damage path.
		if ( owner && ts )
		{
			applyGenericMagicDamage(owner, target, *projEntity, d.user2 > 0 ? d.user2 : SPELL_FORCEBOLT, d.damage, true);
		}
		out.damage = std::max(0, hp0 - target->getHP());
		out.killed = ts && ts->HP <= 0;
		++g_c.stubHits;
#endif
		(void)hp0;
		return out;
	}

	bool ignored(const PS& s, Uint32 uid)
	{
		for ( int i = 0; i < s.nIgnore; ++i ) { if ( s.ignore[i] == uid ) { return true; } }
		return false;
	}
	void addIgnore(PS& s, Uint32 uid)
	{
		if ( ignored(s, uid) ) { return; }
		if ( s.nIgnore < 8 ) { s.ignore[s.nIgnore++] = uid; }
		else { s.ignore[(s.pierced + s.nIgnore) % 8] = uid; }
	}

	bool wallAt(real_t x, real_t y)
	{
		const int tx = (int)x >> 4, ty = (int)y >> 4;
		if ( x < 0 || y < 0 || tx >= (int)map.width || ty >= (int)map.height ) { return true; }
		return map.tiles[OBSTACLELAYER + ty * MAPLAYERS + tx * MAPLAYERS * map.height] != 0;
	}

	struct StepResult { bool wall = false; bool axisX = false; Entity* body = nullptr; };

	// One sub-step of (vx, vy). While the projectile still overlaps a body it is flying through it moves by hand (tile walls only);
	// otherwise Barony's clipMove does all of the collision (doors, colliders, walls, bodies).
	StepResult moveStep(Entity* my, PS& s, real_t vx, real_t vy)
	{
		StepResult r;
		bool manual = false;
		for ( int i = 0; i < s.nIgnore; )
		{
			Entity* e = uidToEntity(s.ignore[i]);
			if ( !e ) { s.ignore[i] = s.ignore[--s.nIgnore]; continue; }
			const real_t pad = s.def.radius + std::fabs(vx) + std::fabs(vy) + 1.0;
			if ( std::fabs(my->x - e->x) <= e->sizex + pad && std::fabs(my->y - e->y) <= e->sizey + pad ) { manual = true; ++i; }
			else { s.ignore[i] = s.ignore[--s.nIgnore]; }
		}
		hit.entity = nullptr;
		if ( !manual )
		{
			const real_t full = std::sqrt(vx * vx + vy * vy);
			const real_t moved = clipMove(&my->x, &my->y, vx, vy, my);
			if ( hit.entity ) { r.body = hit.entity; }
			else if ( moved < full - 1e-6 )
			{
				// clipMove slid along the wall (side says which axis moved) or stopped dead in a corner (side 0)
				r.wall = true;
				r.axisX = hit.side == HORIZONTAL ? true : (hit.side == VERTICAL ? false : std::fabs(vx) >= std::fabs(vy));
			}
			return r;
		}
		const bool bx = wallAt(my->x + vx, my->y), by = wallAt(my->x, my->y + vy);
		if ( bx || by || wallAt(my->x + vx, my->y + vy) )
		{
			r.wall = true;
			r.axisX = bx || !by;
			return r;
		}
		my->x += vx;
		my->y += vy;
		return r;
	}

	void endProjectile(Entity* my, PS& s, int why)
	{
		if ( s.def.onEnd )
		{
			EndEvent ev = { uidToEntity(s.ownerUid), (float)my->x, (float)my->y, why, s.def.user, s.def.user2, s.def.element };
			s.def.onEnd(ev);
		}
		my->removeLightField();
		s.active = false;
		s.uid = 0;
		const int idx = (int)(&s - g_ps.data());
		s.ownerUid = (Uint32)g_free;
		g_free = idx;
		list_RemoveNode(my->mynode);
	}

	void actApxProj(Entity* my)
	{
		const int idx = my->skill[0] - 1;
		if ( idx < 0 || idx >= (int)g_ps.size() || !g_ps[idx].active || g_ps[idx].uid != my->getUID() )
		{
			my->removeLightField();
			list_RemoveNode(my->mynode);
			return;
		}
		if ( ::ticks != g_lastTick ) { g_lastTick = ::ticks; }
		const double t0 = nowUs();
		PS& s = g_ps[idx];
		const ProjDef& d = s.def;
		++g_tickLive;
		Entity* owner = uidToEntity(s.ownerUid);
		if ( s.ownerUid && !owner ) { /* the owner died: the bolt flies on */ }
		bool done = false;
		int why = 0;

		const float speed = std::sqrt(s.vx * s.vx + s.vy * s.vy);
		const int n = std::max(1, (int)std::ceil(speed / std::max(0.5f, d.radius * 0.5f)));
		const real_t sx = s.vx / n, sy = s.vy / n;
		for ( int i = 0; i < n && !done; ++i )
		{
			++g_c.substeps;
			const StepResult r = moveStep(my, s, sx, sy);
			if ( r.body )
			{
				Entity* b = r.body;
				if ( isCreature(b) )
				{
					if ( ignored(s, b->getUID()) ) { continue; }
					if ( (!d.friendly && owner && owner->checkFriend(b)) || b == owner )
					{
						addIgnore(s, b->getUID());
						my->x += sx; my->y += sy;   // pass through a friend
						continue;
					}
					// the impact rule decides: stress against the material's fracture, for this kind
					bool fracture = false;
					if ( Stat* bs = b->getStats() )
					{
						const int mi = apx::combat::monsterMaterial(bs->type);
						const apx::combat::Move mv = { (apx::combat::Kind)d.kind, d.momentum, d.area };
						const apx::combat::Material& mat = apx::combat::material(mi >= 0 ? mi : 0);
						fracture = apx::combat::resolve(mv, mat).fracture;
					}
					const HitOut ho = dealHit(owner, b, my, d);
					++g_c.hits;
					const bool pierce = fracture || s.pierced < d.pierceMax;
					if ( d.onHit )
					{
						HitEvent ev = { owner, b, (float)my->x, (float)my->y, ho.damage, pierce, ho.killed, d.user, d.user2, d.element };
						d.onHit(ev);
					}
					if ( pierce )
					{
						++g_c.pierces;
						if ( fracture ) { ++g_c.fracturePierces; }
						++s.pierced;
						addIgnore(s, b->getUID());
						my->x += sx; my->y += sy;
						continue;
					}
					++g_c.stops;
					done = true; why = 2;
					break;
				}
				else
				{
					// a door, a collider, a chest: it stops the projectile like a wall
					++g_c.wallStops;
					done = true; why = 1;
					break;
				}
			}
			else if ( r.wall )
			{
				const float ang = apx::magic::core::wallAngleDeg((float)s.vx, (float)s.vy, r.axisX);
				if ( d.ricochet && ang < d.ricochetDeg && s.bounces < d.maxBounces )
				{
					if ( r.axisX ) { s.vx = -s.vx; } else { s.vy = -s.vy; }
					s.vx *= d.bounceKeep; s.vy *= d.bounceKeep;
					my->yaw = atan2(s.vy, s.vx);
					++s.bounces;
					++g_c.ricochets;
					break;   // resume next tick with the new heading (the rest of this tick's sub-steps are the bounce)
				}
				++g_c.wallStops;
				done = true; why = 1;
				break;
			}
		}

		++s.age;
		if ( !done && s.age >= d.lifeTicks ) { done = true; why = 0; ++g_c.expired; }
		if ( !done )
		{
			my->vel_x = s.vx; my->vel_y = s.vy;
			my->removeLightField();
			if ( d.light ) { my->light = addLight((int)(my->x / 16), (int)(my->y / 16), d.light); }
			if ( d.onStep && d.trailEvery > 0 && s.age % d.trailEvery == 0 )
			{
				StepEvent ev = { owner, my, (float)my->x, (float)my->y, s.age, d.user, d.user2, d.element };
				d.onStep(ev);
			}
			if ( Entity* p = spawnMagicParticle(my) ) { p->flags[SPRITE] = my->flags[SPRITE]; }
		}
		else
		{
			endProjectile(my, s, why);
		}
		g_tickUs += nowUs() - t0;
	}
} // namespace

Entity* spawn(const ProjDef& def, Entity* owner, Vec3 pos, float yaw)
{
	int idx;
	if ( g_free >= 0 ) { idx = g_free; g_free = (int)g_ps[idx].ownerUid; }
	else { g_ps.emplace_back(); idx = (int)g_ps.size() - 1; }
	Entity* e = newEntity(def.sprite, 1, map.entities, nullptr);
	if ( !e ) { g_ps[idx].active = false; g_ps[idx].ownerUid = (Uint32)g_free; g_free = idx; return nullptr; }
	PS& s = g_ps[idx];
	s = PS();
	s.def = def;
	s.active = true;
	s.uid = e->getUID();
	s.ownerUid = owner ? owner->getUID() : 0;
	s.vx = std::cos(yaw) * def.speed;
	s.vy = std::sin(yaw) * def.speed;
	if ( owner ) { addIgnore(s, owner->getUID()); }
	e->parent = owner ? owner->getUID() : 0;
	e->x = pos.x; e->y = pos.y; e->z = pos.z;
	e->sizex = 1; e->sizey = 1;
	e->yaw = yaw;
	e->vel_x = s.vx; e->vel_y = s.vy;
	e->flags[UPDATENEEDED] = true;
	e->flags[PASSABLE] = true;
	e->behavior = &actApxProj;
	e->skill[0] = idx + 1;
	e->skill[2] = -7;               // like Barony's missile: a client does nothing with this entity
	++g_c.spawned;
	return e;
}

int strike(Entity* owner, Entity* target, const ProjDef& def, float x, float y, Entity* source, bool* killed)
{
	if ( !target ) { return 0; }
	Entity* tmp = nullptr;
	if ( !source )
	{
		tmp = newEntity(-1, 1, map.entities, nullptr);
		if ( !tmp ) { return 0; }
		tmp->x = x; tmp->y = y; tmp->z = -1;
		tmp->flags[PASSABLE] = true; tmp->flags[INVISIBLE] = true; tmp->flags[NOUPDATE] = true;
		source = tmp;
	}
	const HitOut ho = dealHit(owner, target, source, def);
	++g_c.hits;
	if ( killed ) { *killed = ho.killed; }
	if ( tmp ) { list_RemoveNode(tmp->mynode); }
	return ho.damage;
}

void frame()
{
	closeSample();
}
int live()
{
	int n = 0;
	for ( auto& s : g_ps ) { n += s.active ? 1 : 0; }
	return n;
}
void clearAll()
{
	for ( auto& s : g_ps ) { s.active = false; }
	g_ps.clear();
	g_free = -1;
}
const Counters& counters() { return g_c; }
void perfDump(double* p50us, double* p99us, int* samples, int* maxLive)
{
	std::vector<float> v = g_cost;
	std::sort(v.begin(), v.end());
	if ( p50us ) { *p50us = v.empty() ? 0.0 : v[v.size() / 2]; }
	if ( p99us ) { *p99us = v.empty() ? 0.0 : v[std::min(v.size() - 1, (size_t)(v.size() * 0.99))]; }
	if ( samples ) { *samples = (int)v.size(); }
	if ( maxLive ) { *maxLive = g_maxLive; }
}
bool usingCombatCore()
{
#ifdef APX_PROJ_COMBAT_CORE
	return true;
#else
	return false;
#endif
}

} // namespace proj
} // namespace apx
