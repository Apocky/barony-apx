/*-------------------------------------------------------------------------------

	BARONY
	File: light.hpp
	Desc: prototypes for light.cpp, light-related types and prototypes

	Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
	See LICENSE for details.

-------------------------------------------------------------------------------*/
#pragma once

typedef struct light_t
{
	Sint32 x, y;
	Sint32 radius;
	vec4_t* tiles;
	vec4_t* tilesDelta; // apx BVIS1: (old curve - new curve) per tile for an index-0 light; nullptr when unused
    int index; // which lightmap this actually exists in

	// a pointer to the light's location in a list
	node_t* node;
} light_t;

light_t* lightSphereShadow(int index, Sint32 x, Sint32 y, Sint32 radius, float r, float g, float b, float a, float exp);
light_t* lightSphere(int index, Sint32 x, Sint32 y, Sint32 radius, float r, float g, float b, float a, float exp);
light_t* newLight(int index, Sint32 x, Sint32 y, Sint32 radius);
// apx BVIS1: what monsters see keeps the old light curve. lightmaps[0] + lightmapGameplayDelta is the light the
// old curve would give (index-0 lights only); empty when the new curve is off. Cleared with the map.
extern std::vector<vec4_t> lightmapGameplayDelta;
void lightmapGameplayDeltaReset();
light_t* addLight(Sint32 x, Sint32 y, const char* name, int range_bonus = 0, int index = 0);
bool loadLights(bool forceLoadBaseDirectory = false);

struct LightDef {
    int radius = 0;
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
	float a = 0.f;
    float falloff_exp = 1.f;
    bool shadows = false;
};
extern std::unordered_map<std::string, LightDef> lightDefs;
