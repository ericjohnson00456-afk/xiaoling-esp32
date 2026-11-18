#ifndef XIAOLING_MINIAPP_LUA_SCREEN_H
#define XIAOLING_MINIAPP_LUA_SCREEN_H

#include "miniapp.h"
#include "lua.h"

#if MINIAPP_HAS_SCREEN
/* The VM owns scene for the lifetime of these Lua closures. screen.present
 * stays with the runtime, which controls startup validation and UI delivery. */
void miniapp_lua_screen_register(lua_State *L, miniapp_scene_t *scene);
#endif

#endif
