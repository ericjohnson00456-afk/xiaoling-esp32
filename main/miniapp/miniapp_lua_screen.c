#include "miniapp_lua_screen.h"

#if MINIAPP_HAS_SCREEN
#include "lauxlib.h"

static miniapp_scene_t *miniapp_lua_scene(lua_State *L) {
    return lua_touserdata(L, lua_upvalueindex(1));
}

static lua_Integer miniapp_integer(lua_State *L, int index, lua_Integer min, lua_Integer max) {
    lua_Integer value = luaL_checkinteger(L, index);
    if (value < min || value > max) {
        luaL_error(L, "argument %d is out of range", index);
    }
    return value;
}

/* Graphics options are bounded and copied into the scene; no Lua references
 * cross into the UI task. */
static lua_Integer miniapp_graphics_option(lua_State *L, int index, const char *key,
                                           lua_Integer fallback, lua_Integer min, lua_Integer max) {
    if (lua_isnoneornil(L, index)) {
        return fallback;
    }
    luaL_checktype(L, index, LUA_TTABLE);
    lua_getfield(L, index, key);
    lua_Integer value = lua_isnil(L, -1) ? fallback : miniapp_integer(L, -1, min, max);
    lua_pop(L, 1);
    return value;
}

static bool miniapp_graphics_bool(lua_State *L, int index, const char *key, bool fallback) {
    if (lua_isnoneornil(L, index)) {
        return fallback;
    }
    luaL_checktype(L, index, LUA_TTABLE);
    lua_getfield(L, index, key);
    bool value = fallback;
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TBOOLEAN);
        value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

static miniapp_shape_t *miniapp_graphics_shape(lua_State *L, miniapp_shape_kind_t kind) {
    miniapp_scene_t *scene = miniapp_lua_scene(L);
    if (scene->shape_count >= MINIAPP_MAX_SHAPES) {
        luaL_error(L, "screen shape limit is %d", MINIAPP_MAX_SHAPES);
    }
    miniapp_shape_t *shape = &scene->shapes[scene->shape_count];
    memset(shape, 0, sizeof(*shape));
    shape->kind = kind;
    shape->order = scene->shape_count + scene->text_count;
    return shape;
}

static void miniapp_graphics_style(lua_State *L, miniapp_shape_t *shape, int index) {
    shape->opacity = miniapp_graphics_option(L, index, "opacity", 255, 0, 255);
    shape->width = miniapp_graphics_option(L, index, "width", 1, 1, 64);
    shape->rounded = miniapp_graphics_bool(L, index, "rounded", false);
    shape->fill = miniapp_graphics_bool(L, index, "fill", true);
    shape->radius = miniapp_graphics_option(L, index, "radius", 0, 0,
                                            MINIAPP_SCREEN_WIDTH > MINIAPP_SCREEN_HEIGHT
                                                ? MINIAPP_SCREEN_WIDTH
                                                : MINIAPP_SCREEN_HEIGHT);
    shape->border_color =
        miniapp_graphics_option(L, index, "border_color", shape->color, 0, 0xffffff);
    if (shape->kind == MINIAPP_SHAPE_RECT || shape->kind == MINIAPP_SHAPE_CIRCLE) {
        shape->width =
            miniapp_graphics_option(L, index, "border_width", shape->fill ? 0 : 1, 0, 64);
    }
}

static int miniapp_lua_screen_begin(lua_State *L) {
    miniapp_scene_t *scene = miniapp_lua_scene(L);
    uint32_t background = (uint32_t)luaL_checkinteger(L, 1) & 0xffffffu;
    bool ordered = miniapp_graphics_bool(L, 2, "ordered", false);
    scene->background = background;
    scene->shape_count = 0;
    scene->text_count = 0;
    scene->ordered = ordered;
    return 0;
}

static int miniapp_lua_screen_rect(lua_State *L) {
    miniapp_shape_t *rect = miniapp_graphics_shape(L, MINIAPP_SHAPE_RECT);
    rect->x = (int16_t)miniapp_integer(L, 1, 0, MINIAPP_SCREEN_WIDTH - 1);
    rect->y = (int16_t)miniapp_integer(L, 2, 0, MINIAPP_SCREEN_HEIGHT - 1);
    rect->w = (int16_t)miniapp_integer(L, 3, 1, MINIAPP_SCREEN_WIDTH);
    rect->h = (int16_t)miniapp_integer(L, 4, 1, MINIAPP_SCREEN_HEIGHT);
    rect->color = (uint32_t)luaL_checkinteger(L, 5) & 0xffffffu;
    if (rect->x + rect->w > MINIAPP_SCREEN_WIDTH || rect->y + rect->h > MINIAPP_SCREEN_HEIGHT) {
        return luaL_error(L, "invalid rectangle");
    }
    miniapp_graphics_style(L, rect, 6);
    miniapp_lua_scene(L)->shape_count++;
    return 0;
}

static int miniapp_lua_screen_circle(lua_State *L) {
    miniapp_shape_t *shape = miniapp_graphics_shape(L, MINIAPP_SHAPE_CIRCLE);
    shape->x = miniapp_integer(L, 1, -MINIAPP_SCREEN_WIDTH, 2 * MINIAPP_SCREEN_WIDTH);
    shape->y = miniapp_integer(L, 2, -MINIAPP_SCREEN_HEIGHT, 2 * MINIAPP_SCREEN_HEIGHT);
    shape->w =
        miniapp_integer(L, 3, 1,
                        MINIAPP_SCREEN_WIDTH > MINIAPP_SCREEN_HEIGHT ? MINIAPP_SCREEN_WIDTH
                                                                     : MINIAPP_SCREEN_HEIGHT);
    shape->color = (uint32_t)luaL_checkinteger(L, 4) & 0xffffffu;
    miniapp_graphics_style(L, shape, 5);
    miniapp_lua_scene(L)->shape_count++;
    return 0;
}

static int miniapp_lua_screen_line(lua_State *L) {
    miniapp_shape_t *shape = miniapp_graphics_shape(L, MINIAPP_SHAPE_LINE);
    shape->x = miniapp_integer(L, 1, -MINIAPP_SCREEN_WIDTH, 2 * MINIAPP_SCREEN_WIDTH);
    shape->y = miniapp_integer(L, 2, -MINIAPP_SCREEN_HEIGHT, 2 * MINIAPP_SCREEN_HEIGHT);
    shape->w = miniapp_integer(L, 3, -MINIAPP_SCREEN_WIDTH, 2 * MINIAPP_SCREEN_WIDTH);
    shape->h = miniapp_integer(L, 4, -MINIAPP_SCREEN_HEIGHT, 2 * MINIAPP_SCREEN_HEIGHT);
    shape->color = (uint32_t)luaL_checkinteger(L, 5) & 0xffffffu;
    miniapp_graphics_style(L, shape, 6);
    miniapp_lua_scene(L)->shape_count++;
    return 0;
}

static int miniapp_lua_screen_arc(lua_State *L) {
    miniapp_shape_t *shape = miniapp_graphics_shape(L, MINIAPP_SHAPE_ARC);
    shape->x = miniapp_integer(L, 1, -MINIAPP_SCREEN_WIDTH, 2 * MINIAPP_SCREEN_WIDTH);
    shape->y = miniapp_integer(L, 2, -MINIAPP_SCREEN_HEIGHT, 2 * MINIAPP_SCREEN_HEIGHT);
    shape->w =
        miniapp_integer(L, 3, 1,
                        MINIAPP_SCREEN_WIDTH > MINIAPP_SCREEN_HEIGHT ? MINIAPP_SCREEN_WIDTH
                                                                     : MINIAPP_SCREEN_HEIGHT);
    shape->start_angle = miniapp_integer(L, 4, 0, 360);
    shape->end_angle = miniapp_integer(L, 5, 0, 360);
    shape->color = (uint32_t)luaL_checkinteger(L, 6) & 0xffffffu;
    miniapp_graphics_style(L, shape, 7);
    if (shape->width > shape->w) {
        return luaL_error(L, "arc width exceeds radius");
    }
    miniapp_lua_scene(L)->shape_count++;
    return 0;
}

static int miniapp_lua_screen_text(lua_State *L) {
    miniapp_scene_t *scene = miniapp_lua_scene(L);
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);
    if (scene->text_count >= MINIAPP_MAX_TEXTS) {
        return luaL_error(L, "screen.text limit is %d", MINIAPP_MAX_TEXTS);
    }
    if (len > MINIAPP_TEXT_MAX) {
        return luaL_error(L, "text is too long");
    }
    miniapp_text_t *item = &scene->texts[scene->text_count];
    memset(item, 0, sizeof(*item));
    item->x = (int16_t)miniapp_integer(L, 2, 0, MINIAPP_SCREEN_WIDTH - 1);
    item->y = (int16_t)miniapp_integer(L, 3, 0, MINIAPP_SCREEN_HEIGHT - MINIAPP_TEXT_FONT_PX);
    item->color = (uint32_t)luaL_checkinteger(L, 4) & 0xffffffu;
    item->opacity = miniapp_graphics_option(L, 5, "opacity", 255, 0, 255);
    item->width = miniapp_graphics_option(L, 5, "width", 0, 0, MINIAPP_SCREEN_WIDTH - item->x);
    if (!lua_isnoneornil(L, 5)) {
        static const char *const aligns[] = {"left", "center", "right", NULL};
        lua_getfield(L, 5, "align");
        item->align = luaL_checkoption(L, -1, "left", aligns);
        lua_pop(L, 1);
    }
    if (item->align && !item->width) {
        return luaL_error(L, "text alignment requires width");
    }
    item->order = scene->shape_count + scene->text_count;
    memcpy(item->text, text, len);
    scene->text_count++;
    return 0;
}

void miniapp_lua_screen_register(lua_State *L, miniapp_scene_t *scene) {
    static const luaL_Reg functions[] = {
        {"begin", miniapp_lua_screen_begin},
        {"rect", miniapp_lua_screen_rect},
        {"text", miniapp_lua_screen_text},
        {"circle", miniapp_lua_screen_circle},
        {"line", miniapp_lua_screen_line},
        {"arc", miniapp_lua_screen_arc},
        {NULL, NULL},
    };
    lua_getglobal(L, "screen");
    lua_pushlightuserdata(L, scene);
    luaL_setfuncs(L, functions, 1);
    lua_pop(L, 1);
}
#endif
