/*-------------------------------------------------------------------------------

	BARONY
	File: light.cpp
	Desc: light spawning code

	Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
	See LICENSE for details.

-------------------------------------------------------------------------------*/

#include "main.hpp"
#include "light.hpp"
#include "draw.hpp"
#ifndef EDITOR
#include "apx/vision.hpp"
#endif

// apx BVIS1 (D84): perceptual falloff with a faint tail past the old radius (APX_VIS_FALLOFF=0: linear); the editor keeps the old curve
static bool apxVisCurve() {
#ifndef EDITOR
    return apx::vision::falloffOn();
#else
    return false;
#endif
}
static Sint32 apxVisReach(Sint32 radius, float exp) {
#ifndef EDITOR
    return apx::vision::reach(radius, exp);
#else
    return radius;
#endif
}
// apx BVIS1: the old-curve delta map monsters read (see light.hpp)
std::vector<vec4_t> lightmapGameplayDelta;

void lightmapGameplayDeltaReset()
{
    lightmapGameplayDelta.assign((size_t)map.width * map.height, vec4_t{0.f, 0.f, 0.f, 0.f});
}

static void apxAddGameplayDelta(light_t* light, int soff, Sint32 extent, int doff, const vec4_t& dv)
{
    if (lightmapGameplayDelta.size() != (size_t)map.width * map.height) { lightmapGameplayDeltaReset(); }
    if (!light->tilesDelta) {
        const auto w = extent * 2 + 1;
        light->tilesDelta = (vec4_t*)calloc((size_t)w * w, sizeof(vec4_t));
        if (!light->tilesDelta) { return; }
    }
    auto& t = light->tilesDelta[soff];
    t.x += dv.x; t.y += dv.y; t.z += dv.z; t.w += dv.w;
    auto& d = lightmapGameplayDelta[doff];
    d.x += dv.x; d.y += dv.y; d.z += dv.z; d.w += dv.w;
}
static float apxVisLeft(float d2, float radius, float exp) {
#ifndef EDITOR
    return apx::vision::falloff(d2, radius, exp);
#else
    return 0.f;
#endif
}

/*-------------------------------------------------------------------------------

	lightSphereShadow

	Adds a circle of light to the lightmap at x and y with the supplied
	radius and color; casts shadows against walls

-------------------------------------------------------------------------------*/

light_t* lightSphereShadow(int index, Sint32 x, Sint32 y, Sint32 radius, float r, float g, float b, float a, float exp)
{
	const bool apxCurve = apxVisCurve();
	const Sint32 extent = apxCurve ? apxVisReach(radius, exp) : radius;
	light_t* light = newLight(index, x, y, extent);
    r = r * 255.f;
    g = g * 255.f;
    b = b * 255.f;
    a = a * 255.f;

	for (int v = y - extent; v <= y + extent; ++v) {
		for (int u = x - extent; u <= x + extent; ++u) {
			if (u >= 0 && v >= 0 && u < map.width && v < map.height) {
				const int dx = u - x;
				const int dy = v - y;
				const int dxabs = abs(dx);
				const int dyabs = abs(dy);
				real_t a0 = dyabs * .5;
				real_t b0 = dxabs * .5;
				int u2 = u;
				int v2 = v;
                
                // check origin is okay
				bool wallhit = true;
				const int mapindex = v * MAPLAYERS + u * MAPLAYERS * map.height;
				for (int z = 0; z < MAPLAYERS; z++) {
					if ( !map.tiles[mapindex + z] || map.tiles[mapindex + z] == TRANSPARENT_TILE ) {
						wallhit = false;
						break;
					}
				}
				if (wallhit == true) {
					continue;
				}
                
                // line test
                if (dxabs >= dyabs) { // the line is more horizontal than vertical
					for (int i = 0; i < dxabs; ++i) {
						u2 -= sgn(dx);
						b0 += dyabs;
						if (b0 >= dxabs) {
							b0 -= dxabs;
							v2 -= sgn(dy);
						}
						if (u2 >= 0 && u2 < map.width && v2 >= 0 && v2 < map.height) {
							if ( map.tiles[OBSTACLELAYER + v2 * MAPLAYERS + u2 * MAPLAYERS * map.height]
                                && map.tiles[OBSTACLELAYER + v2 * MAPLAYERS + u2 * MAPLAYERS * map.height] != TRANSPARENT_TILE ) {
								wallhit = true;
								break;
							}
						}
					}
				}
                else { // the line is more vertical than horizontal
					for (int i = 0; i < dyabs; ++i) {
						v2 -= sgn(dy);
						a0 += dxabs;
						if (a0 >= dyabs) {
							a0 -= dyabs;
							u2 -= sgn(dx);
						}
						if (u2 >= 0 && u2 < map.width && v2 >= 0 && v2 < map.height) {
							if (map.tiles[OBSTACLELAYER + v2 * MAPLAYERS + u2 * MAPLAYERS * map.height]
                                && map.tiles[OBSTACLELAYER + v2 * MAPLAYERS + u2 * MAPLAYERS * map.height] != TRANSPARENT_TILE) {
								wallhit = true;
								break;
							}
						}
					}
				}
                
                // light tile if it passed line test
				if (wallhit == false || (wallhit == true && u2 == u && v2 == v)) {
                    const float dist = exp != 1.f ? powf(dx * dx + dy * dy, exp) : dx * dx + dy * dy;
                    const auto falloff = apxCurve ? 1.f - apxVisLeft((float)(dx * dx + dy * dy), (float)radius, exp) : std::min<float>(dist / radius, 1.0f);
                    const auto soff = (dy + extent) + (dx + extent) * (extent * 2 + 1);
                    auto& s = light->tiles[soff];
					s.x += r - r * falloff;
                    s.y += g - g * falloff;
                    s.z += b - b * falloff;
                    s.w += a - a * falloff;
                    if (apxCurve && index == 0) { // apx BVIS1: monsters keep seeing the old curve
                        const float old = std::min<float>(dist / radius, 1.0f);
                        const vec4_t dv = { (r - r * old) - (r - r * falloff), (g - g * old) - (g - g * falloff), (b - b * old) - (b - b * falloff), (a - a * old) - (a - a * falloff) };
                        apxAddGameplayDelta(light, soff, extent, v + u * map.height, dv);
                    }
                    const auto doff = v + u * map.height;
                    if (index) {
                        auto& d = lightmaps[index][doff];
                        d.x += s.x;
                        d.y += s.y;
                        d.z += s.z;
                        d.w += s.w;
                    } else {
                        for (int c = 0; c < MAXPLAYERS + 1; ++c) {
                            auto& d = lightmaps[c][doff];
                            d.x += s.x;
                            d.y += s.y;
                            d.z += s.z;
                            d.w += s.w;
                        }
                    }
                }
			}
		}
	}
	return light;
}

/*-------------------------------------------------------------------------------

	lightSphere

	Adds a circle of light to the lightmap at x and y with the supplied
	radius and color; casts no shadows

-------------------------------------------------------------------------------*/

light_t* lightSphere(int index, Sint32 x, Sint32 y, Sint32 radius, float r, float g, float b, float a, float exp)
{
	const bool apxCurve = apxVisCurve();
	const Sint32 extent = apxCurve ? apxVisReach(radius, exp) : radius;
	light_t* light = newLight(index, x, y, extent);
    r = r * 255.f;
    g = g * 255.f;
    b = b * 255.f;
    a = a * 255.f;

	for (int v = y - extent; v <= y + extent; ++v) {
		for (int u = x - extent; u <= x + extent; ++u) {
			if (u >= 0 && v >= 0 && u < map.width && v < map.height) {
				const int dx = u - x;
				const int dy = v - y;
                const float dist = exp != 1.f ? powf(dx * dx + dy * dy, exp) : dx * dx + dy * dy;
                const auto falloff = apxCurve ? 1.f - apxVisLeft((float)(dx * dx + dy * dy), (float)radius, exp) : std::min<float>(dist / radius, 1.0f);
                const auto soff = (dy + extent) + (dx + extent) * (extent * 2 + 1);
                auto& s = light->tiles[soff];
                s.x += r - r * falloff;
                s.y += g - g * falloff;
                s.z += b - b * falloff;
                s.w += a - a * falloff;
                if (apxCurve && index == 0) { // apx BVIS1: monsters keep seeing the old curve
                    const float old = std::min<float>(dist / radius, 1.0f);
                    const vec4_t dv = { (r - r * old) - (r - r * falloff), (g - g * old) - (g - g * falloff), (b - b * old) - (b - b * falloff), (a - a * old) - (a - a * falloff) };
                    apxAddGameplayDelta(light, soff, extent, v + u * map.height, dv);
                }
                const auto doff = v + u * map.height;
                if (index) {
                    auto& d = lightmaps[index][doff];
                    d.x += s.x;
                    d.y += s.y;
                    d.z += s.z;
                    d.w += s.w;
                } else {
                    for (int c = 0; c < MAXPLAYERS + 1; ++c) {
                        auto& d = lightmaps[c][doff];
                        d.x += s.x;
                        d.y += s.y;
                        d.z += s.z;
                        d.w += s.w;
                    }
                }
			}
		}
	}
	return light;
}

#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include "files.hpp"

std::unordered_map<std::string, LightDef> lightDefs;
bool loadLights(bool forceLoadBaseDirectory) {
    if ( !PHYSFS_getRealDir("/data/lights.json") )
    {
        printlog("[JSON]: Error: Could not find file: data/lights.json");
        return false;
    }

    std::string inputPath = PHYSFS_getRealDir("/data/lights.json");
    if ( forceLoadBaseDirectory )
    {
        inputPath = datadir;
    }
    else
    {
        if ( inputPath != datadir )
        {
            loadLights(true); // force load the base directory first, then modded paths later.
        }
        else
        {
            forceLoadBaseDirectory = true;
        }
    }

    inputPath.append("/data/lights.json");

    File* fp = FileIO::open(inputPath.c_str(), "rb");
    if ( !fp )
    {
        printlog("[JSON]: Error: Could not open json file %s", inputPath.c_str());
        return false;
    }

    if ( forceLoadBaseDirectory )
    {
        lightDefs.clear();
    }
    
    char buf[65536];
    int count = (int)fp->read(buf, sizeof(buf[0]), sizeof(buf));
    buf[count] = '\0';
    rapidjson::StringStream is(buf);
    FileIO::close(fp);

    rapidjson::Document d;
    d.ParseStream(is);
    
    const auto& lights = d["lights"];
    if (lights.IsObject()) {
        for (const auto& it : lights.GetObject()) {
            LightDef def;
            const auto& name = it.name.GetString();
            const auto& radius = it.value["radius"]; def.radius = radius.GetInt();
            const auto& r = it.value["r"]; def.r = r.GetFloat();
            const auto& g = it.value["g"]; def.g = g.GetFloat();
            const auto& b = it.value["b"]; def.b = b.GetFloat();
            if ( it.value.HasMember("a") )
            {
                def.a = it.value["a"].GetFloat();
            }
            else
            {
                def.a = 0.f;
            }
            const auto& exp = it.value["falloff_exp"]; def.falloff_exp = exp.GetFloat();
            const auto& shadows = it.value["shadows"]; def.shadows = shadows.GetBool();
            lightDefs[name] = def;
        }
    }
    
    return true;
}

#ifndef EDITOR
#include "interface/consolecommand.hpp"
static ConsoleCommand ccmd_reloadLights("/reloadlights", "reload light json",
    [](int argc, const char* argv[]){
    loadLights();
    });
#endif

light_t* addLight(Sint32 x, Sint32 y, const char* name, int range_bonus, int index) {
    if (!name || !name[0]) {
        return nullptr;
    }
    auto find = lightDefs.find(name);
    if (find == lightDefs.end()) {
        return nullptr;
    }
    const auto& def = find->second;
    if (def.shadows) {
        return lightSphereShadow(index, x, y, std::max(def.radius + range_bonus, 1), def.r, def.g, def.b, def.a, def.falloff_exp);
    } else {
        return lightSphere(index, x, y, std::max(def.radius + range_bonus, 1), def.r, def.g, def.b, def.a, def.falloff_exp);
    }
}
