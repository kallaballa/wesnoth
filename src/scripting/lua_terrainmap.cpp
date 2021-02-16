/*
   Copyright (C) 2018 the Battle for Wesnoth Project https://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

#include "scripting/lua_terrainmap.hpp"
#include "scripting/lua_terrainfilter.hpp"

#include "formatter.hpp"
#include "global.hpp"
#include "log.hpp"
#include "map/location.hpp"
#include "map/map.hpp"
#include "scripting/lua_common.hpp"
#include "scripting/push_check.hpp"
#include "scripting/game_lua_kernel.hpp"
#include "resources.hpp"
#include "game_board.hpp"
#include "play_controller.hpp"

#include "lua/lauxlib.h"
#include "lua/lua.h"

static lg::log_domain log_scripting_lua("scripting/lua");
#define LOG_LUA LOG_STREAM(info, log_scripting_lua)
#define ERR_LUA LOG_STREAM(err, log_scripting_lua)

static const char terrainmapKey[] = "terrain map";
static const char terraincolKey[] = "terrain map column";
static const char maplocationKey[] = "special locations";

// Uservalue indices for the terrain map colum userdata
namespace terraincol {
	enum {MAP = 1, COL = 2};
}

using std::string_view;

////////  SPECIAL LOCATION  ////////

int impl_slocs_get(lua_State* L)
{
	gamemap_base& m = luaW_checkterrainmap(L, 1);
	string_view id = luaL_checkstring(L, 2);
	auto res = m.special_location(std::string(id));
	if(res.valid()) {
		luaW_pushlocation(L, res);
	} else {
		//functions with variable return numbers have been causing problem in the past
		lua_pushnil(L);
	}
	return 1;
}

int impl_slocs_set(lua_State* L)
{
	gamemap_base& m = luaW_checkterrainmap(L, 1);
	string_view id = luaL_checkstring(L, 2);
	map_location loc = luaW_checklocation(L, 3);

	m.set_special_location(std::string(id), loc);
	return 0;
}

int impl_slocs_len(lua_State *L)
{
	gamemap_base& m = luaW_checkterrainmap(L, 1);
	lua_pushnumber(L, m.special_locations().size());
	return 1;
}

int impl_slocs_next(lua_State *L)
{
	gamemap_base& m = luaW_checkterrainmap(L, lua_upvalueindex(1));
	const t_translation::starting_positions::left_map& left = m.special_locations().left;

	t_translation::starting_positions::left_const_iterator it;
	if (lua_isnoneornil(L, 2)) {
		it = left.begin();
	}
	else {
		it = left.find(luaL_checkstring(L, 2));
		if (it == left.end()) {
			return 0;
		}
		++it;
	}
	if (it == left.end()) {
		return 0;
	}
	lua_pushstring(L, it->first.c_str());
	luaW_pushlocation(L, it->second);
	return 2;
}

int impl_slocs_iter(lua_State *L)
{
	lua_settop(L, 1);
	lua_pushvalue(L, 1);
	lua_pushcclosure(L, &impl_slocs_next, 1);
	lua_pushvalue(L, 1);
	lua_pushnil(L);
	return 3;
}

////////  MAP  ////////

mapgen_gamemap::mapgen_gamemap(std::string_view s)
{
	if(s.empty()) {
		return;
	}
	//throws t_translation::error
	//todo: make read_game_map take a string_view
	tiles() = t_translation::read_game_map(s, special_locations(), t_translation::coordinate{ 1, 1 });
}

mapgen_gamemap::mapgen_gamemap(int w, int h, terrain_code t)
	: gamemap_base(w, h, t)
{

}

// This can produce invalid combinations in rare case
// where an overlay doesn't have an independent terrain definition,
// or if you set an overlay with no base and merge mode other than OVERLAY.
void simplemerge(t_translation::terrain_code old_t, t_translation::terrain_code& new_t, const terrain_type_data::merge_mode mode)
{
	switch(mode) {
		case terrain_type_data::OVERLAY:
			new_t = t_translation::terrain_code(old_t.base, new_t.overlay);
			break;
		case terrain_type_data::BASE:
			new_t = t_translation::terrain_code(new_t.base, old_t.overlay);
			break;
		case terrain_type_data::BOTH:
			new_t = t_translation::terrain_code(new_t.base, new_t.overlay);
			break;
	}
}

void mapgen_gamemap::set_terrain(const map_location& loc, const terrain_code & terrain, const terrain_type_data::merge_mode mode, bool)
{
	terrain_code old = get_terrain(loc);
	terrain_code t = terrain;
	simplemerge(old, t, mode);
	tiles().get(loc.x + border_size(), loc.y + border_size()) = t;
}

struct lua_map_ref {
	virtual gamemap_base& get_map() = 0;
	virtual ~lua_map_ref() {}
};

// Mapgen map reference, owned by Lua
struct lua_map_ref_gen : public lua_map_ref {
	mapgen_gamemap map;
	template<typename... T>
	lua_map_ref_gen(T&&... params) : map(std::forward<T>(params)...) {}
	gamemap_base& get_map() override {
		return map;
	}
};

// Main map reference, owned by the engine
struct lua_map_ref_main : public lua_map_ref {
	gamemap& map;
	lua_map_ref_main(gamemap& ref) : map(ref) {}
	gamemap_base& get_map() override {
		return map;
	}
};

// Non-owning map reference to either type (used for special location userdata)
struct lua_map_ref_locs : public lua_map_ref {
	gamemap_base& map;
	lua_map_ref_locs(gamemap_base& ref) : map(ref) {}
	gamemap_base& get_map() override {
		return map;
	}
};

bool luaW_isterrainmap(lua_State* L, int index)
{
	return luaL_testudata(L, index, terrainmapKey) != nullptr || luaL_testudata(L, index, maplocationKey) != nullptr;
}


gamemap_base* luaW_toterrainmap(lua_State *L, int index)
{
	if(luaW_isterrainmap(L, index)) {
		return &static_cast<lua_map_ref*>(lua_touserdata(L, index))->get_map();
	}
	return nullptr;
}

gamemap_base& luaW_checkterrainmap(lua_State *L, int index)
{
	if(luaW_isterrainmap(L, index)) {
		return static_cast<lua_map_ref*>(lua_touserdata(L, index))->get_map();
	}
	luaW_type_error(L, index, "terrainmap");
}

/**
 * Create a map.
 * - Arg 1: string describing the map data.
 * - or:
 * - Arg 1: int, width
 * - Arg 2: int, height
 * - Arg 3: string, terrain
*/
int intf_terrainmap_create(lua_State *L)
{
	if(lua_isnumber(L, 1) && lua_isnumber(L, 2)) {
		int w = lua_tonumber(L, 1);
		int h = lua_tonumber(L, 2);
		auto terrain = t_translation::read_terrain_code(luaL_checkstring(L, 3));
		new(L) lua_map_ref_gen(w, h, terrain);
	} else {
		string_view data_str = luaL_checkstring(L, 1);
		new(L) lua_map_ref_gen(data_str);
	}
	luaL_setmetatable(L, terrainmapKey);
	return 1;
}

int intf_terrainmap_get(lua_State* L)
{
	new(L) lua_map_ref_main(const_cast<gamemap&>(resources::gameboard->map()));
	luaL_setmetatable(L, terrainmapKey);
	return 1;
}

/**
 * Destroys a map object before it is collected (__gc metamethod).
 */
static int impl_terrainmap_collect(lua_State *L)
{
	lua_map_ref* m = static_cast<lua_map_ref*>(lua_touserdata(L, 1));
	m->lua_map_ref::~lua_map_ref();
	return 0;
}

static void luaW_push_terrain(lua_State* L, gamemap_base& map, map_location loc)
{
	auto t = map.get_terrain(loc);
	lua_pushstring(L, t_translation::write_terrain_code(t).c_str());
}

static void impl_merge_terrain(lua_State* L, int idx, gamemap_base& map, map_location loc)
{
	auto mode = terrain_type_data::BOTH;
	string_view t_str = luaL_checkstring(L, idx);
	auto ter = t_translation::read_terrain_code(t_str);
	if(ter.base == t_translation::NO_LAYER && ter.overlay != t_translation::NO_LAYER)
		mode = terrain_type_data::OVERLAY;
	if(auto gm = dynamic_cast<gamemap*>(&map)) {
		if(resources::gameboard) {
			bool result = resources::gameboard->change_terrain(loc, ter, mode, true);
			for(team& t : resources::gameboard->teams()) {
				t.fix_villages(*gm);
			}

			if(resources::controller) {
				resources::controller->get_display().needs_rebuild(result);
			}
		}
	} else map.set_terrain(loc, ter, mode);
}

static int impl_terrainmap_colget(lua_State* L)
{
	if(luaL_testudata(L, 1, terraincolKey)) {
		lua_getiuservalue(L, 1, terraincol::MAP);
		gamemap_base& map = luaW_checkterrainmap(L, -1);
		lua_getiuservalue(L, 1, terraincol::COL);
		int x = luaL_checkinteger(L, -1);
		int y = luaL_checkinteger(L, 2);
		luaW_push_terrain(L, map, {x, y, wml_loc()});
		return 1;
	}
	return 0;
}

static int impl_terrainmap_colset(lua_State* L) {
	if(luaL_testudata(L, 1, terraincolKey)) {
		lua_getiuservalue(L, 1, terraincol::MAP);
		gamemap_base& map = luaW_checkterrainmap(L, -1);
		lua_getiuservalue(L, 1, terraincol::COL);
		int x = luaL_checkinteger(L, -1);
		int y = luaL_checkinteger(L, 2);
		impl_merge_terrain(L, 3, map, {x, y, wml_loc()});
	}
	return 0;
}

/**
 * Gets some data on a map (__index metamethod).
 * - Arg 1: full userdata containing the map.
 * - Arg 2: string containing the name of the property.
 * - Ret 1: something containing the attribute.
 */
static int impl_terrainmap_get(lua_State *L)
{
	gamemap_base& tm = luaW_checkterrainmap(L, 1);
	map_location loc;
	if(lua_type(L, 2) == LUA_TNUMBER) {
		lua_newuserdatauv(L, 0, 2);
		lua_pushvalue(L, 1);
		lua_setiuservalue(L, -2, terraincol::MAP);
		lua_pushvalue(L, 2);
		lua_setiuservalue(L, -2, terraincol::COL);
		luaL_setmetatable(L, terraincolKey);
		return 1;
	} else if(luaW_tolocation(L, 2, loc)) {
		luaW_push_terrain(L, tm, loc);
		return 1;
	}
	
	char const *m = luaL_checkstring(L, 2);

	// Find the corresponding attribute.
	return_int_attrib("width", tm.total_width());
	return_int_attrib("height", tm.total_height());
	return_string_attrib("data", tm.to_string());

	if(strcmp(m, "special_locations") == 0) {
		new(L) lua_map_ref_locs(tm);
		luaL_setmetatable(L, maplocationKey);
		return 1;
	}
	if(luaW_getmetafield(L, 1, m)) {
		return 1;
	}
	return 0;
}

/**
 * Sets some data on a map (__newindex metamethod).
 * - Arg 1: full userdata containing the map.
 * - Arg 2: string containing the name of the property.
 * - Arg 3: something containing the attribute.
 */
static int impl_terrainmap_set(lua_State *L)
{
	gamemap_base& tm = luaW_checkterrainmap(L, 1);
	map_location loc;
	if(luaW_tolocation(L, 2, loc)) {
		impl_merge_terrain(L, 3, tm, loc);
		return 0;
	}
	char const *m = luaL_checkstring(L, 2);
	std::string err_msg = "unknown modifiable property of map: ";
	err_msg += m;
	return luaL_argerror(L, 2, err_msg.c_str());
}


/**
 * Sets a terrain code.
 * - Arg 1: map location.
 * - Arg 2: terrain code string.
 * - Arg 3: layer: (overlay|base|both, default=both)
*/
static int intf_set_terrain(lua_State *L)
{
	gamemap_base& tm = luaW_checkterrainmap(L, 1);
	map_location loc = luaW_checklocation(L, 2);
	string_view t_str = luaL_checkstring(L, 3);

	auto terrain = t_translation::read_terrain_code(t_str);
	auto mode = terrain_type_data::BOTH;

	if(!lua_isnoneornil(L, 4)) {
		string_view mode_str = luaL_checkstring(L, 4);
		if(mode_str == "base") {
			mode = terrain_type_data::BASE;
		}
		else if(mode_str == "overlay") {
			mode = terrain_type_data::OVERLAY;
		}
	}

	if(auto gm = dynamic_cast<gamemap*>(&tm)) {
		if(resources::gameboard) {
			bool result = resources::gameboard->change_terrain(loc, terrain, mode, true);
			
			for(team& t : resources::gameboard->teams()) {
				t.fix_villages(*gm);
			}

			if(resources::controller) {
				resources::controller->get_display().needs_rebuild(result);
			}
		}
	} else tm.set_terrain(loc, terrain, mode);
	return 0;
}

/**
 * Gets a terrain code.
 * - Arg 1: map location.
 * - Ret 1: string.
 */
static int intf_get_terrain(lua_State *L)
{
	gamemap_base& tm = luaW_checkterrainmap(L, 1);
	map_location loc = luaW_checklocation(L, 2);

	luaW_push_terrain(L, tm, loc);
	return 1;
}

static std::vector<gamemap::overlay_rule> read_rules_vector(lua_State *L, int index)
{
	std::vector<gamemap::overlay_rule> rules;
	for (int i = 1, i_end = lua_rawlen(L, index); i <= i_end; ++i)
	{
		lua_rawgeti(L, index, i);
		if(!lua_istable(L, -1)) {
			luaL_argerror(L, index, "rules must be a table of tables");
		}
		rules.push_back(gamemap::overlay_rule());
		auto& rule = rules.back();
		if(luaW_tableget(L, -1, "old")) {
			rule.old_ = t_translation::read_list(luaW_tostring(L, -1));
			lua_pop(L, 1);
		}

		if(luaW_tableget(L, -1, "new")) {
			rule.new_ = t_translation::read_list(luaW_tostring(L, -1));
			lua_pop(L, 1);
		}

		if(luaW_tableget(L, -1, "mode")) {
			auto str = luaW_tostring(L, -1);
			rule.mode_ = str == "base" ? terrain_type_data::BASE : (str == "overlay" ? terrain_type_data::OVERLAY : terrain_type_data::BOTH);
			lua_pop(L, 1);
		}

		if(luaW_tableget(L, -1, "terrain")) {
			const t_translation::ter_list terrain = t_translation::read_list(luaW_tostring(L, -1));
			if(!terrain.empty()) {
				rule.terrain_ = terrain[0];
			}
			lua_pop(L, 1);
		}

		if(luaW_tableget(L, -1, "use_old")) {
			rule.use_old_ = luaW_toboolean(L, -1);
			lua_pop(L, 1);
		}

		if(luaW_tableget(L, -1, "replace_if_failed")) {
			rule.replace_if_failed_ = luaW_toboolean(L, -1);
			lua_pop(L, 1);
		}

		lua_pop(L, 1);
	}
	return rules;
}
/**
 * Replaces part of the map.
 * - Arg 1: map location.
 * - Arg 2: map data string.
 * - Arg 3: table for optional named arguments
 *   - is_odd: boolean, if Arg2 has the odd map format (as if it was cut from a odd map location)
 *   - ignore_special_locations: boolean
 *   - rules: table of tables
*/
int intf_terrain_mask(lua_State *L)
{
	gamemap_base& map = luaW_checkterrainmap(L, 1);
	map_location loc = luaW_checklocation(L, 2);

	bool is_odd = false;
	bool ignore_special_locations = false;
	std::vector<gamemap::overlay_rule> rules;

	if(lua_istable(L, 4)) {
		is_odd = luaW_table_get_def(L, 4, "is_odd", false);
		ignore_special_locations = luaW_table_get_def(L, 4, "ignore_special_locations", false);

		if(luaW_tableget(L, 4, "rules")) {
			if(!lua_istable(L, -1)) {
				return luaL_argerror(L, 4, "rules must be a table");
			}
			rules = read_rules_vector(L, -1);
			lua_pop(L, 1);
		}
	}
	
	if(lua_isstring(L, 3)) {
		const std::string t_str = luaL_checkstring(L, 2);
		std::unique_ptr<gamemap_base> mask;
		if(auto gmap = dynamic_cast<gamemap*>(&map)) {
			auto mask_ptr = new gamemap("");
			mask_ptr->read(t_str, false);
			mask.reset(mask_ptr);
		} else {
			mask.reset(new mapgen_gamemap(t_str));
		}
		map.overlay(*mask, loc, rules, is_odd, ignore_special_locations);
	} else {
		gamemap_base& mask = luaW_checkterrainmap(L, 3);
		map.overlay(mask, loc, rules, is_odd, ignore_special_locations);
	}
	
	if(resources::gameboard) {
		if(auto gmap = dynamic_cast<gamemap*>(&map)) {
			for(team& t : resources::gameboard->teams()) {
				t.fix_villages(*gmap);
			}
		}
	}

	if(resources::controller) {
		resources::controller->get_display().needs_rebuild(true);
	}

	return 0;
}

namespace lua_terrainmap {
	std::string register_metatables(lua_State* L, bool use_tf)
	{
		std::ostringstream cmd_out;

		cmd_out << "Adding terrain map metatable...\n";

		luaL_newmetatable(L, terrainmapKey);
		lua_pushcfunction(L, impl_terrainmap_collect);
		lua_setfield(L, -2, "__gc");
		lua_pushcfunction(L, impl_terrainmap_get);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, impl_terrainmap_set);
		lua_setfield(L, -2, "__newindex");
		lua_pushstring(L, terrainmapKey);
		lua_setfield(L, -2, "__metatable");
		// terrainmap methods
		lua_pushcfunction(L, intf_set_terrain);
		lua_setfield(L, -2, "set_terrain");
		lua_pushcfunction(L, intf_get_terrain);
		lua_setfield(L, -2, "get_terrain");
		if(use_tf) {
			lua_pushcfunction(L, intf_mg_get_locations);
			lua_setfield(L, -2, "get_locations");
			lua_pushcfunction(L, intf_mg_get_tiles_radius);
			lua_setfield(L, -2, "get_tiles_radius");
		}
		lua_pushcfunction(L, intf_terrain_mask);
		lua_setfield(L, -2, "terrain_mask");
		
		luaL_newmetatable(L, terraincolKey);
		lua_pushcfunction(L, impl_terrainmap_colget);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, impl_terrainmap_colset);
		lua_setfield(L, -2, "__newindex");
		lua_pushstring(L, terraincolKey);
		lua_setfield(L, -2, "__metatable");

		cmd_out << "Adding special locations metatable...\n";

		luaL_newmetatable(L, maplocationKey);
		lua_pushcfunction(L, impl_slocs_get);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, impl_slocs_set);
		lua_setfield(L, -2, "__newindex");
		lua_pushcfunction(L, impl_slocs_len);
		lua_setfield(L, -2, "__len");
		lua_pushcfunction(L, impl_slocs_iter);
		lua_setfield(L, -2, "__pairs");
		lua_pushstring(L, maplocationKey);
		lua_setfield(L, -2, "__metatable");

		return cmd_out.str();
	}
}
