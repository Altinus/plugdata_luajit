/*
 * pdluajit.c — LuaJIT-powered DSP external for Pure Data / plugdata
 *
 * Creates the [pdluajit] object. User scripts use the .pd_luajit extension.
 *
 * Usage:
 *   [pdluajit scriptname]          — loads scriptname.pd_luajit
 *   [pdluajit scriptname arg1 ..]  — loads with extra creation arguments
 *
 * User scripts define:
 *   inlets  = {SIGNAL, SIGNAL, DATA, DATA}
 *   outlets = {SIGNAL, DATA}
 *   function perform(ins, outs, n)  — DSP callback (required if signal io)
 *   function dsp(sr, bs)            — called when DSP starts (optional)
 *   function in_1_bang()            — bang on inlet 1 (optional)
 *   function in_1_float(x)          — float on inlet 1 (optional)
 *   function in_3_float(x)          — float on data inlet 3 (optional)
 *   function in_n(n, sel, atoms)    — catch-all (optional)
 *   pd.outlet(1, "float", {val})    — send to data outlet 1
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published
 * by the Free Software Foundation.
 */

#include "pdluajit.h"
#include "pdluajit_gfx.h"
#include <string.h>
#include <stdio.h>

static t_class *pdluajit_class;
static t_class *pdluajit_proxyinlet_class;

/* In PDINSTANCE builds (VST3, multi-instance), gensym() is per-instance.
 * class_new() names must use the global pd_maininstance's symbol table,
 * otherwise the message dispatcher cannot find the class -> crash.
 * Standalone (single instance) is unaffected. */
static t_symbol *pdluajit_global_gensym(const char *s)
{
#ifdef PDINSTANCE
    t_pdinstance *prev = pd_get_instance();
    pd_set_instance(&pd_maininstance);
#endif
    t_symbol *sym = gensym(s);
#ifdef PDINSTANCE
    pd_set_instance(prev);
#endif
    return sym;
}

/* ========================================================================= */
/* Bootstrap Lua code                                                        */
/* ========================================================================= */

static const char *bootstrap_code =
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef float t_float;')\n"
    "local cast = ffi.cast\n"
    "local float_ptr_t = ffi.typeof('t_float*')\n"
    "\n"
    "SIGNAL = 1\n"
    "DATA = 0\n"
    "\n"
    "function __perform_wrapper(n_in, n_out, n, ...)\n"
    "    local args = {...}\n"
    "    local ins = {}\n"
    "    local outs = {}\n"
    "    for i = 1, n_in do\n"
    "        ins[i] = cast(float_ptr_t, args[i])\n"
    "    end\n"
    "    for i = 1, n_out do\n"
    "        outs[i] = cast(float_ptr_t, args[n_in + i])\n"
    "    end\n"
    "    perform(ins, outs, n)\n"
    "end\n"
    "\n"
    "pd = pd or {}\n"
    "pd.SIGNAL = 1\n"
    "pd.DATA = 0\n"
    "pd.load_ffi = function(name)\n"
    "    if not pd.scriptdir then error('pd.scriptdir is not available') end\n"
    "    return ffi.load(pd.scriptdir .. '/' .. name)\n"
    "end\n";

/* ========================================================================= */
/* Graphics bootstrap code — FFI cdef + Graphics class + __paint_wrapper     */
/* ========================================================================= */

static const char *gfx_bootstrap_code =
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef struct pdluajit_gfx_vtable {\n"
    "    void (*save)(void *ctx);\n"
    "    void (*restore)(void *ctx);\n"
    "    void (*reset_transform)(void *ctx);\n"
    "    void (*set_color)(void *ctx, int r, int g, int b, int a);\n"
    "    void (*stroke_width)(void *ctx, float width);\n"
    "    void (*fill_rect)(void *ctx, float x, float y, float w, float h);\n"
    "    void (*fill_rounded_rect)(void *ctx, float x, float y, float w, float h, float radius);\n"
    "    void (*fill_ellipse)(void *ctx, float x, float y, float w, float h);\n"
    "    void (*stroke_rect)(void *ctx, float x, float y, float w, float h);\n"
    "    void (*stroke_rounded_rect)(void *ctx, float x, float y, float w, float h, float radius);\n"
    "    void (*stroke_ellipse)(void *ctx, float x, float y, float w, float h);\n"
    "    void (*draw_line)(void *ctx, float x1, float y1, float x2, float y2);\n"
    "    void (*begin_path)(void *ctx);\n"
    "    void (*move_to)(void *ctx, float x, float y);\n"
    "    void (*line_to)(void *ctx, float x, float y);\n"
    "    void (*quad_to)(void *ctx, float cx, float cy, float x, float y);\n"
    "    void (*bezier_to)(void *ctx, float c1x, float c1y, float c2x, float c2y, float x, float y);\n"
    "    void (*close_path)(void *ctx);\n"
    "    void (*fill)(void *ctx);\n"
    "    void (*stroke)(void *ctx);\n"
    "    void (*translate)(void *ctx, float tx, float ty);\n"
    "    void (*scale)(void *ctx, float sx, float sy);\n"
    "    void (*fill_all)(void *ctx, float w, float h,\n"
    "                     unsigned int fill_rgba, unsigned int outline_rgba,\n"
    "                     float corner_radius);\n"
    "    void (*draw_text)(void *ctx, const char *text, float x, float y,\n"
    "                      float max_width, float font_height);\n"
    "    int (*create_image)(void *ctx, const char *filename, int imageFlags);\n"
    "    void (*delete_image)(void *ctx, int image);\n"
    "    void (*draw_image)(void *ctx, int image, float x, float y, float w, float h, float alpha);\n"
    "    void (*set_fill_paint_image_pattern)(void *ctx, float ox, float oy, float ex, float ey, float angle, int image, float alpha);\n"
    "    int (*create_image_rgba)(void *ctx, int w, int h, int imageFlags, const unsigned char *data);\n"
    "    void (*update_image)(void *ctx, int image, const unsigned char *data);\n"
    "} pdluajit_gfx_vtable;\n"
    "]]\n"
    "\n"
    "local band = require('bit').band\n"
    "local bor  = require('bit').bor\n"
    "local lshift = require('bit').lshift\n"
    "local vt_ptr_t = ffi.typeof('const pdluajit_gfx_vtable*')\n"
    "\n"
    "-- Graphics class: wraps ctx + vtable pointers\n"
    "local Graphics = {}\n"
    "Graphics.__index = Graphics\n"
    "\n"
    "function Graphics.new(ctx, vt, w, h, outline_rgba)\n"
    "    local self = setmetatable({}, Graphics)\n"
    "    self._ctx = ctx\n"
    "    self._vt = ffi.cast(vt_ptr_t, vt)\n"
    "    self._w = w\n"
    "    self._h = h\n"
    "    self._outline_rgba = outline_rgba\n"
    "    -- default fill color: white opaque (ABGR: a=255,b=255,g=255,r=255)\n"
    "    self._fill_rgba = 0xFFFFFFFF\n"
    "    self._stroke_w = 1.0\n"
    "    return self\n"
    "end\n"
    "\n"
    "-- Pack RGBA into uint32 ABGR layout for fill_all: a | (b<<8) | (g<<16) | (r<<24)\n"
    "local function pack_rgba_abgr(r, g, b, a)\n"
    "    return bor(band(a, 0xFF),\n"
    "              lshift(band(b, 0xFF), 8),\n"
    "              lshift(band(g, 0xFF), 16),\n"
    "              lshift(band(r, 0xFF), 24))\n"
    "end\n"
    "\n"
    "function Graphics:set_color(r, g, b, a)\n"
    "    a = a or 255\n"
    "    self._fill_rgba = pack_rgba_abgr(r, g, b, a)\n"
    "    self._vt.set_color(self._ctx, r, g, b, a)\n"
    "end\n"
    "\n"
    "function Graphics:stroke_width(w)\n"
    "    self._stroke_w = w\n"
    "    self._vt.stroke_width(self._ctx, w)\n"
    "end\n"
    "\n"
    "function Graphics:fill_all()\n"
    "    self._vt.fill_all(self._ctx, self._w, self._h,\n"
    "                      self._fill_rgba, self._outline_rgba, 2.75)\n"
    "end\n"
    "\n"
    "function Graphics:fill_rect(x, y, w, h)\n"
    "    self._vt.fill_rect(self._ctx, x, y, w, h)\n"
    "end\n"
    "\n"
    "function Graphics:fill_rounded_rect(x, y, w, h, r)\n"
    "    self._vt.fill_rounded_rect(self._ctx, x, y, w, h, r or 0)\n"
    "end\n"
    "\n"
    "function Graphics:fill_ellipse(x, y, w, h)\n"
    "    self._vt.fill_ellipse(self._ctx, x, y, w, h)\n"
    "end\n"
    "\n"
    "function Graphics:stroke_rect(x, y, w, h)\n"
    "    self._vt.stroke_rect(self._ctx, x, y, w, h)\n"
    "end\n"
    "\n"
    "function Graphics:stroke_rounded_rect(x, y, w, h, r)\n"
    "    self._vt.stroke_rounded_rect(self._ctx, x, y, w, h, r or 0)\n"
    "end\n"
    "\n"
    "function Graphics:stroke_ellipse(x, y, w, h)\n"
    "    self._vt.stroke_ellipse(self._ctx, x, y, w, h)\n"
    "end\n"
    "\n"
    "function Graphics:draw_line(x1, y1, x2, y2)\n"
    "    self._vt.draw_line(self._ctx, x1, y1, x2, y2)\n"
    "end\n"
    "\n"
    "function Graphics:begin_path()\n"
    "    self._vt.begin_path(self._ctx)\n"
    "end\n"
    "\n"
    "function Graphics:move_to(x, y)\n"
    "    self._vt.move_to(self._ctx, x, y)\n"
    "end\n"
    "\n"
    "function Graphics:line_to(x, y)\n"
    "    self._vt.line_to(self._ctx, x, y)\n"
    "end\n"
    "\n"
    "function Graphics:quad_to(cx, cy, x, y)\n"
    "    self._vt.quad_to(self._ctx, cx, cy, x, y)\n"
    "end\n"
    "\n"
    "function Graphics:bezier_to(c1x, c1y, c2x, c2y, x, y)\n"
    "    self._vt.bezier_to(self._ctx, c1x, c1y, c2x, c2y, x, y)\n"
    "end\n"
    "\n"
    "function Graphics:close_path()\n"
    "    self._vt.close_path(self._ctx)\n"
    "end\n"
    "\n"
    "function Graphics:fill()\n"
    "    self._vt.fill(self._ctx)\n"
    "end\n"
    "\n"
    "function Graphics:stroke()\n"
    "    self._vt.stroke(self._ctx)\n"
    "end\n"
    "\n"
    "function Graphics:translate(tx, ty)\n"
    "    self._vt.translate(self._ctx, tx, ty)\n"
    "end\n"
    "\n"
    "function Graphics:scale(sx, sy)\n"
    "    self._vt.scale(self._ctx, sx, sy)\n"
    "end\n"
    "\n"
    "function Graphics:reset_transform()\n"
    "    self._vt.reset_transform(self._ctx)\n"
    "end\n"
    "\n"
    "function Graphics:save()\n"
    "    self._vt.save(self._ctx)\n"
    "end\n"
    "\n"
    "function Graphics:restore()\n"
    "    self._vt.restore(self._ctx)\n"
    "end\n"
    "\n"
    "function Graphics:draw_text(text, x, y, max_width, font_height)\n"
    "    self._vt.draw_text(self._ctx, tostring(text), x, y,\n"
    "                       max_width or 1000, font_height or 12)\n"
    "end\n"
    "\n"
    "function Graphics:width() return self._w end\n"
    "function Graphics:height() return self._h end\n"
    "\n"
    "function Graphics:create_image(filename)\n"
    "    if pd.scriptdir then\n"
    "        filename = pd.scriptdir .. '/' .. filename\n"
    "    end\n"
    "    -- NVG_IMAGE_GENERATE_MIPMAPS = 1, NVG_IMAGE_REPEATX = 2, NVG_IMAGE_REPEATY = 4\n"
    "    return self._vt.create_image(self._ctx, tostring(filename), 1)\n"
    "end\n"
    "\n"
    "function Graphics:delete_image(image)\n"
    "    if image and type(image) == 'number' and image > 0 then\n"
    "        self._vt.delete_image(self._ctx, image)\n"
    "    end\n"
    "end\n"
    "\n"
    "function Graphics:draw_image(image, x, y, w, h, alpha)\n"
    "    if image and type(image) == 'number' and image > 0 then\n"
    "        self._vt.draw_image(self._ctx, image, x, y, w, h, alpha or 1.0)\n"
    "    end\n"
    "end\n"
    "\n"
    "function Graphics:set_fill_image_pattern(ox, oy, ex, ey, angle, image, alpha)\n"
    "    if image and type(image) == 'number' and image > 0 then\n"
    "        self._vt.set_fill_paint_image_pattern(self._ctx, ox, oy, ex, ey, angle or 0, image, alpha or 1.0)\n"
    "    end\n"
    "end\n"
    "\n"
    "function Graphics:create_image_rgba(w, h, data_ptr, flags)\n"
    "    return self._vt.create_image_rgba(self._ctx, w, h, flags or 0,\n"
    "           ffi.cast('const unsigned char*', data_ptr))\n"
    "end\n"
    "\n"
    "function Graphics:update_image(image, data_ptr)\n"
    "    if image and type(image) == 'number' and image > 0 then\n"
    "        self._vt.update_image(self._ctx, image,\n"
    "               ffi.cast('const unsigned char*', data_ptr))\n"
    "    end\n"
    "end\n"
    "\n"
    "-- __paint_wrapper: called from C with lightuserdata args\n"
    "local _last_nvg_ctx = nil\n"
    "function __paint_wrapper(ctx, vt, w, h, outline_rgba)\n"
    "    if ctx ~= _last_nvg_ctx then\n"
    "        _last_nvg_ctx = ctx\n"
    "        local inv = rawget(_G, 'invalidate')\n"
    "        if inv then inv() end\n"
    "    end\n"
    "    local g = Graphics.new(ctx, vt, w, h, outline_rgba)\n"
    "    paint(g)\n"
    "end\n";

/* ========================================================================= */
/* Forward declarations                                                      */
/* ========================================================================= */

static void pdluajit_dispatch(t_pdluajit *x, unsigned int inlet,
                               t_symbol *s, int argc, t_atom *argv);

/* ========================================================================= */
/* Proxy inlet class                                                         */
/* ========================================================================= */

static void pdluajit_proxyinlet_anything(t_pdluajit_proxyinlet *p,
                                          t_symbol *s, int argc, t_atom *argv)
{
    pdluajit_dispatch(p->owner, p->id, s, argc, argv);
}

/** Forward handler for signal inlets.
 *  Pd's signal inlets drop non-signal/non-float messages unless the
 *  destination has a "fwd" method. inlet_fwd() packs the original selector
 *  into argv[0] as A_SYMBOL, followed by the original arguments. */
static void pdluajit_proxyinlet_fwd(t_pdluajit_proxyinlet *p,
                                     t_symbol *s, int argc, t_atom *argv)
{
    if (argc >= 1 && argv[0].a_type == A_SYMBOL) {
        t_symbol *orig_sel = atom_getsymbol(&argv[0]);
        pdluajit_dispatch(p->owner, p->id, orig_sel, argc - 1, argv + 1);
    }
}

static void pdluajit_proxyinlet_init(t_pdluajit_proxyinlet *p,
                                      t_pdluajit *owner, unsigned int id)
{
    p->pd = pdluajit_proxyinlet_class;
    p->owner = owner;
    p->id = id;
}

static void pdluajit_proxyinlet_setup(void)
{
    pdluajit_proxyinlet_class = class_new(
        pdluajit_global_gensym("pdluajit proxy inlet"),
        0, 0,
        sizeof(t_pdluajit_proxyinlet),
        CLASS_PD, 0);
    if (pdluajit_proxyinlet_class) {
        class_addanything(pdluajit_proxyinlet_class,
                          (t_method)pdluajit_proxyinlet_anything);
        class_addmethod(pdluajit_proxyinlet_class,
                        (t_method)pdluajit_proxyinlet_fwd,
                        pdluajit_global_gensym("fwd"), A_GIMME, 0);
    }
}

/* ========================================================================= */
/* Lua-accessible API: pd.post, pd.error, pd.outlet                          */
/* ========================================================================= */

static int pdluajit_lua_post(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    post("pdluajit: %s", msg);
    return 0;
}

static int pdluajit_lua_error(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "__pdluajit_obj");
    t_pdluajit *x = (t_pdluajit *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (x)
        pd_error(x, "pdluajit: %s", msg);
    else
        error("pdluajit: %s", msg);
    return 0;
}

/** pd.outlet(n, sel, atoms) — send a message to data outlet n (1-based).
 *
 *  pd.outlet(1, "bang", {})
 *  pd.outlet(1, "float", {3.14})
 *  pd.outlet(1, "symbol", {"hello"})
 *  pd.outlet(1, "list", {1, 2, 3})
 *  pd.outlet(1, "foo", {"bar", 42})
 */
static int pdluajit_lua_outlet(lua_State *L)
{
    /* Retrieve object pointer from registry */
    lua_getfield(L, LUA_REGISTRYINDEX, "__pdluajit_obj");
    t_pdluajit *x = (t_pdluajit *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!x) return luaL_error(L, "pd.outlet: no object context");

    /* Arg 1: outlet number (1-based) */
    int n = (int)luaL_checkinteger(L, 1);
    if (n < 1 || n > x->num_outlets)
        return luaL_error(L, "pd.outlet: outlet %d out of range (1..%d)",
                          n, x->num_outlets);

    int idx = n - 1;
    if (x->outlet_types[idx] != PDLUAJIT_TYPE_DATA)
        return luaL_error(L, "pd.outlet: outlet %d is SIGNAL, not DATA", n);

    /* Arg 2: selector string */
    const char *sel = luaL_checkstring(L, 2);
    t_outlet *out = x->out[idx];

    if (strcmp(sel, "bang") == 0) {
        outlet_bang(out);
    }
    else if (strcmp(sel, "float") == 0) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_rawgeti(L, 3, 1);
        t_float val = (t_float)luaL_checknumber(L, -1);
        lua_pop(L, 1);
        outlet_float(out, val);
    }
    else if (strcmp(sel, "symbol") == 0) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_rawgeti(L, 3, 1);
        const char *sym = luaL_checkstring(L, -1);
        lua_pop(L, 1);
        outlet_symbol(out, gensym(sym));
    }
    else {
        /* "list" or custom selector: convert Lua table to atoms */
        luaL_checktype(L, 3, LUA_TTABLE);
        int len = (int)lua_objlen(L, 3);
        t_atom *atoms = NULL;
        if (len > 0)
            atoms = (t_atom *)getbytes(len * sizeof(t_atom));
        for (int i = 0; i < len; i++) {
            lua_rawgeti(L, 3, i + 1);
            if (lua_isnumber(L, -1))
                SETFLOAT(&atoms[i], (t_float)lua_tonumber(L, -1));
            else if (lua_isstring(L, -1))
                SETSYMBOL(&atoms[i], gensym(lua_tostring(L, -1)));
            else
                SETFLOAT(&atoms[i], 0);
            lua_pop(L, 1);
        }
        if (strcmp(sel, "list") == 0)
            outlet_list(out, &s_list, len, atoms);
        else
            outlet_anything(out, gensym(sel), len, atoms);
        if (atoms)
            freebytes(atoms, len * sizeof(t_atom));
    }
    return 0;
}

/** pd.rgba(r, g, b [, a]) — pack RGBA into uint32 ARGB for image pixels.
 *  Returns (a<<24 | r<<16 | g<<8 | b), suitable for uint32_t pixel buffers
 *  used with create_image_rgba / update_image.  Alpha defaults to 255. */
static int pdluajit_lua_rgba(lua_State *L)
{
    unsigned int r = (unsigned int)luaL_checkinteger(L, 1) & 0xFFu;
    unsigned int g = (unsigned int)luaL_checkinteger(L, 2) & 0xFFu;
    unsigned int b = (unsigned int)luaL_checkinteger(L, 3) & 0xFFu;
    unsigned int a = lua_isnoneornil(L, 4) ? 255u
                   : (unsigned int)lua_tointeger(L, 4) & 0xFFu;
    uint32_t packed = (a << 24) | (r << 16) | (g << 8) | b;
    lua_pushnumber(L, (lua_Number)packed);
    return 1;
}

/** pd.repaint() — request a GUI repaint from the render thread. */
static int pdluajit_lua_repaint(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__pdluajit_obj");
    t_pdluajit *x = (t_pdluajit *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (x && x->has_gui)
        plugdata_forward_message(0, x, gensym("lua_repaint"), 0, NULL);
    return 0;
}

/** pd.set_size(w, h) — set the GUI widget dimensions. */
static int pdluajit_lua_set_size(lua_State *L)
{
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);

    lua_getfield(L, LUA_REGISTRYINDEX, "__pdluajit_obj");
    t_pdluajit *x = (t_pdluajit *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!x) return 0;

    x->gfx_width = w;
    x->gfx_height = h;

    t_atom args[2];
    SETFLOAT(&args[0], (t_float)w);
    SETFLOAT(&args[1], (t_float)h);
    plugdata_forward_message(0, x, gensym("lua_resized"), 2, args);
    return 0;
}

/* ========================================================================= */
/* Clock callback                                                            */
/* ========================================================================= */

static void pdluajit_clock_tick(t_pdluajit *x)
{
    if (!x || !x->L || x->has_error || x->tick_ref == LUA_NOREF) return;

    lua_State *L = x->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, x->tick_ref);
    if (lua_pcall(L, 0, 0, 0)) {
        pd_error(x, "pdluajit: tick: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    /* Reschedule if interval > 0 */
    if (x->clock_interval > 0)
        clock_delay(x->clock, x->clock_interval);
}

/** pd.clock_set(interval_ms) — start periodic tick() callback.
 *  pd.clock_set(0) or pd.clock_unset() — stop the clock. */
static int pdluajit_lua_clock_set(lua_State *L)
{
    double interval = luaL_checknumber(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, "__pdluajit_obj");
    t_pdluajit *x = (t_pdluajit *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!x) return 0;

    /* Cache tick() ref if not yet cached */
    if (x->tick_ref == LUA_NOREF) {
        lua_getglobal(L, "tick");
        if (lua_isfunction(L, -1))
            x->tick_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        else {
            lua_pop(L, 1);
            return luaL_error(L, "pd.clock_set: tick() function not defined");
        }
    }

    x->clock_interval = interval;
    if (interval > 0) {
        if (!x->clock)
            x->clock = clock_new(x, (t_method)pdluajit_clock_tick);
        clock_delay(x->clock, interval);
    } else {
        if (x->clock)
            clock_unset(x->clock);
    }
    return 0;
}

static int pdluajit_lua_clock_unset(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__pdluajit_obj");
    t_pdluajit *x = (t_pdluajit *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (x) {
        x->clock_interval = 0;
        if (x->clock)
            clock_unset(x->clock);
    }
    return 0;
}

/* ========================================================================= */
/* Public graphics API — called from C++ (LuaJitObject)                      */
/* ========================================================================= */

int pdluajit_has_gui(void *ptr)
{
    t_pdluajit *x = (t_pdluajit *)ptr;
    return x ? x->has_gui : 0;
}

void pdluajit_paint(void *ptr, void *ctx, const pdluajit_gfx_vtable *vt,
                    int width, int height, unsigned int outline_rgba)
{
    t_pdluajit *x = (t_pdluajit *)ptr;
    if (!x || !x->L || x->has_error || x->paint_ref == LUA_NOREF) return;

    lua_State *L = x->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, x->paint_ref);
    lua_pushlightuserdata(L, ctx);
    lua_pushlightuserdata(L, (void *)vt);
    lua_pushinteger(L, width);
    lua_pushinteger(L, height);
    lua_pushinteger(L, (lua_Integer)outline_rgba);

    /* NOTE: caller (updateFramebuffers) already holds sys_lock — do NOT lock again */
    if (lua_pcall(L, 5, 0, 0)) {
        pd_error(x, "pdluajit: paint: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static void pdluajit_call_mouse_ref(t_pdluajit *x, int ref, int mx, int my)
{
    if (!x || !x->L || x->has_error || ref == LUA_NOREF) return;

    lua_State *L = x->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushinteger(L, mx);
    lua_pushinteger(L, my);

    /* NOTE: caller (enqueueFunctionAsync) already holds sys_lock — do NOT lock again */
    if (lua_pcall(L, 2, 0, 0)) {
        pd_error(x, "pdluajit: mouse callback: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

void pdluajit_mouse_down(void *ptr, int mx, int my)
{
    pdluajit_call_mouse_ref((t_pdluajit *)ptr,
                            ((t_pdluajit *)ptr)->mouse_down_ref, mx, my);
}

void pdluajit_mouse_up(void *ptr, int mx, int my)
{
    pdluajit_call_mouse_ref((t_pdluajit *)ptr,
                            ((t_pdluajit *)ptr)->mouse_up_ref, mx, my);
}

void pdluajit_mouse_move(void *ptr, int mx, int my)
{
    pdluajit_call_mouse_ref((t_pdluajit *)ptr,
                            ((t_pdluajit *)ptr)->mouse_move_ref, mx, my);
}

void pdluajit_mouse_drag(void *ptr, int mx, int my)
{
    pdluajit_call_mouse_ref((t_pdluajit *)ptr,
                            ((t_pdluajit *)ptr)->mouse_drag_ref, mx, my);
}

int pdluajit_get_width(void *ptr)
{
    t_pdluajit *x = (t_pdluajit *)ptr;
    return x ? x->gfx_width : 0;
}

int pdluajit_get_height(void *ptr)
{
    t_pdluajit *x = (t_pdluajit *)ptr;
    return x ? x->gfx_height : 0;
}

void pdluajit_set_size(void *ptr, int w, int h)
{
    t_pdluajit *x = (t_pdluajit *)ptr;
    if (x) {
        x->gfx_width = w;
        x->gfx_height = h;
    }
}

static void pdluajit_register_api(lua_State *L, t_pdluajit *x)
{
    /* Store object pointer in registry for pd.error() / pd.outlet() */
    lua_pushlightuserdata(L, x);
    lua_setfield(L, LUA_REGISTRYINDEX, "__pdluajit_obj");

    /* Create/extend pd table */
    lua_getglobal(L, "pd");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }

    lua_pushcfunction(L, pdluajit_lua_post);
    lua_setfield(L, -2, "post");

    lua_pushcfunction(L, pdluajit_lua_error);
    lua_setfield(L, -2, "error");

    lua_pushcfunction(L, pdluajit_lua_outlet);
    lua_setfield(L, -2, "outlet");

    lua_pushcfunction(L, pdluajit_lua_repaint);
    lua_setfield(L, -2, "repaint");

    lua_pushcfunction(L, pdluajit_lua_set_size);
    lua_setfield(L, -2, "set_size");

    lua_pushcfunction(L, pdluajit_lua_rgba);
    lua_setfield(L, -2, "rgba");

    lua_pushcfunction(L, pdluajit_lua_clock_set);
    lua_setfield(L, -2, "clock_set");

    lua_pushcfunction(L, pdluajit_lua_clock_unset);
    lua_setfield(L, -2, "clock_unset");

    lua_setglobal(L, "pd");
}

/* ========================================================================= */
/* Script loading                                                            */
/* ========================================================================= */

static int pdluajit_find_script(t_pdluajit *x, const char *name,
                                char *fullpath, int pathlen, char *outdir, int outdirlen)
{
    char filename[MAXPDSTRING];
    char dirresult[MAXPDSTRING];
    char *nameresult;

    if (strstr(name, ".pd_luajit"))
        snprintf(filename, MAXPDSTRING, "%s", name);
    else
        snprintf(filename, MAXPDSTRING, "%s.pd_luajit", name);

    int fd = canvas_open(x->canvas, filename, "", dirresult,
                         &nameresult, MAXPDSTRING, 0);
    if (fd >= 0) {
        sys_close(fd);
        snprintf(fullpath, pathlen, "%s/%s", dirresult, nameresult);
        if (outdir) {
            strncpy(outdir, dirresult, outdirlen);
            outdir[outdirlen - 1] = '\0';
        }
        return 1;
    }

    pd_error(x, "pdluajit: cannot find script '%s'", filename);
    return 0;
}

static void pdluajit_load_script(t_pdluajit *x, t_symbol *name)
{
    char fullpath[MAXPDSTRING];
    char scriptdir[MAXPDSTRING];

    if (!pdluajit_find_script(x, name->s_name, fullpath, MAXPDSTRING, scriptdir, MAXPDSTRING))
        return;

    lua_State *L = x->L;

    x->has_error = 0;

    /* ---- Stop clock before reload to prevent stale tick() calls ---- */
    if (x->clock)
        clock_unset(x->clock);
    if (x->tick_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, x->tick_ref);
        x->tick_ref = LUA_NOREF;
    }
    x->clock_interval = 0;

    /* Expose script directory to Lua */
    lua_getglobal(L, "pd");
    if (lua_istable(L, -1)) {
        lua_pushstring(L, scriptdir);
        lua_setfield(L, -2, "scriptdir");
    }
    lua_pop(L, 1);

    /* Load and execute user script */
    if (luaL_dofile(L, fullpath)) {
        pd_error(x, "pdluajit: load error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        x->has_error = 1;
        return;
    }

    /* Cache __perform_wrapper reference */
    if (x->perform_ref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, x->perform_ref);
    lua_getglobal(L, "__perform_wrapper");
    if (lua_isfunction(L, -1))
        x->perform_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else {
        lua_pop(L, 1);
        x->perform_ref = LUA_NOREF;
    }

    /* Check perform() exists (required only if there are signal io) */
    lua_getglobal(L, "perform");
    if (!lua_isfunction(L, -1)) {
        /* Not an error if no signal io; checked later after inlet setup */
    }
    lua_pop(L, 1);

    /* Cache dsp() reference */
    if (x->dsp_ref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, x->dsp_ref);
    lua_getglobal(L, "dsp");
    if (lua_isfunction(L, -1))
        x->dsp_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else {
        lua_pop(L, 1);
        x->dsp_ref = LUA_NOREF;
    }

    /* ---- Graphics ref caching ---- */

    /* Check if paint() is defined -> enable GUI mode */
    if (x->paint_ref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, x->paint_ref);
    x->paint_ref = LUA_NOREF;
    x->has_gui = 0;

    lua_getglobal(L, "paint");
    if (lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        x->has_gui = 1;
        /* Cache __paint_wrapper ref */
        lua_getglobal(L, "__paint_wrapper");
        if (lua_isfunction(L, -1))
            x->paint_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        else
            lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }

    /* Cache mouse callback refs */
#define CACHE_MOUSE_REF(field, name) \
    do { \
        if (x->field != LUA_NOREF) \
            luaL_unref(L, LUA_REGISTRYINDEX, x->field); \
        lua_getglobal(L, name); \
        if (lua_isfunction(L, -1)) \
            x->field = luaL_ref(L, LUA_REGISTRYINDEX); \
        else { \
            lua_pop(L, 1); \
            x->field = LUA_NOREF; \
        } \
    } while(0)

    CACHE_MOUSE_REF(mouse_down_ref, "mouse_down");
    CACHE_MOUSE_REF(mouse_up_ref,   "mouse_up");
    CACHE_MOUSE_REF(mouse_move_ref, "mouse_move");
    CACHE_MOUSE_REF(mouse_drag_ref, "mouse_drag");
    CACHE_MOUSE_REF(resized_ref,    "resized");
#undef CACHE_MOUSE_REF

    x->script_name = name;
    x->script_path = gensym(fullpath);
    post("pdluajit: loaded %s", fullpath);

    /* Call initialize(args) on reload — ensures buffers are re-allocated */
    lua_getglobal(L, "initialize");
    if (lua_isfunction(L, -1)) {
        lua_getglobal(L, "pd");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "args");
            lua_remove(L, -2);  /* remove pd table, keep args */
        } else {
            lua_pop(L, 1);
            lua_pushnil(L);
        }
        if (lua_pcall(L, 1, 0, 0)) {
            pd_error(x, "pdluajit: initialize (reload): %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}

/* ========================================================================= */
/* Dispatch helpers                                                          */
/* ========================================================================= */

/** Push atoms as a Lua table {val1, val2, ...} */
static void pdluajit_push_atoms_table(lua_State *L, int argc, t_atom *argv)
{
    lua_createtable(L, argc, 0);
    for (int i = 0; i < argc; i++) {
        if (argv[i].a_type == A_FLOAT)
            lua_pushnumber(L, atom_getfloat(&argv[i]));
        else if (argv[i].a_type == A_SYMBOL)
            lua_pushstring(L, atom_getsymbol(&argv[i])->s_name);
        else
            lua_pushnumber(L, 0);
        lua_rawseti(L, -2, i + 1);
    }
}

/** Check if a global function exists; leaves it on stack if yes */
static int pdluajit_try_method(lua_State *L, const char *name)
{
    lua_getglobal(L, name);
    if (lua_isfunction(L, -1))
        return 1;
    lua_pop(L, 1);
    return 0;
}

/** Push type-specific arguments for in_N_sel / in_n_sel dispatch.
 *  Returns number of arguments pushed. */
static int pdluajit_push_typed_args(lua_State *L, t_symbol *s,
                                     int argc, t_atom *argv)
{
    if (s == &s_bang) {
        return 0;
    }
    else if (s == &s_float && argc >= 1) {
        lua_pushnumber(L, atom_getfloat(&argv[0]));
        return 1;
    }
    else if (s == &s_symbol && argc >= 1) {
        lua_pushstring(L, atom_getsymbol(&argv[0])->s_name);
        return 1;
    }
    else if (s == &s_list) {
        pdluajit_push_atoms_table(L, argc, argv);
        return 1;
    }
    else {
        /* custom selector: remaining args as table */
        pdluajit_push_atoms_table(L, argc, argv);
        return 1;
    }
}

/* ========================================================================= */
/* Inlet message dispatch                                                    */
/*                                                                           */
/* Resolution order (matches pdlua):                                         */
/*   1. in_N_sel(...)       — specific inlet, specific selector              */
/*   2. in_n_sel(n, ...)    — any inlet, specific selector                   */
/*   3. in_N(sel, atoms)    — specific inlet, any selector                   */
/*   4. in_n(n, sel, atoms) — catch-all                                      */
/* ========================================================================= */

static void pdluajit_dispatch(t_pdluajit *x, unsigned int inlet,
                               t_symbol *s, int argc, t_atom *argv)
{
    if (x->has_error || !x->L) return;

    lua_State *L = x->L;
    char method[64];
    int inlet_lua = (int)inlet + 1;  /* Lua 1-based */
    int nargs;

    /* 1. Try in_N_sel — most specific */
    snprintf(method, sizeof(method), "in_%d_%s", inlet_lua, s->s_name);
    if (pdluajit_try_method(L, method)) {
        nargs = pdluajit_push_typed_args(L, s, argc, argv);
        if (lua_pcall(L, nargs, 0, 0)) {
            pd_error(x, "pdluajit: %s: %s", method, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        return;
    }

    /* 2. Try in_n_sel — any inlet, specific selector */
    snprintf(method, sizeof(method), "in_n_%s", s->s_name);
    if (pdluajit_try_method(L, method)) {
        lua_pushinteger(L, inlet_lua);
        nargs = 1 + pdluajit_push_typed_args(L, s, argc, argv);
        if (lua_pcall(L, nargs, 0, 0)) {
            pd_error(x, "pdluajit: %s: %s", method, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        return;
    }

    /* 3. Try in_N — specific inlet, any selector */
    snprintf(method, sizeof(method), "in_%d", inlet_lua);
    if (pdluajit_try_method(L, method)) {
        lua_pushstring(L, s->s_name);
        pdluajit_push_atoms_table(L, argc, argv);
        if (lua_pcall(L, 2, 0, 0)) {
            pd_error(x, "pdluajit: %s: %s", method, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        return;
    }

    /* 4. Try in_n — catch-all */
    if (pdluajit_try_method(L, "in_n")) {
        lua_pushinteger(L, inlet_lua);
        lua_pushstring(L, s->s_name);
        pdluajit_push_atoms_table(L, argc, argv);
        if (lua_pcall(L, 3, 0, 0)) {
            pd_error(x, "pdluajit: in_n: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        return;
    }
}

/* ========================================================================= */
/* DSP perform                                                               */
/* ========================================================================= */

static t_int *pdluajit_perform(t_int *w)
{
    t_pdluajit *x = (t_pdluajit *)(w[1]);
    int n = (int)(w[2]);
    t_float **sig_vecs = (t_float **)(w + 3);
    int sum = x->siginlets + x->sigoutlets;

    /* On error or no perform function: zero-fill outputs */
    if (x->has_error || x->perform_ref == LUA_NOREF) {
        for (int i = 0; i < x->sigoutlets; i++) {
            t_float *out = sig_vecs[x->siginlets + i];
            for (int j = 0; j < n; j++) out[j] = 0.0f;
        }
        return w + sum + 3;
    }

    lua_State *L = x->L;

    /* Push __perform_wrapper(n_in, n_out, n, ptr1, ptr2, ...) */
    lua_rawgeti(L, LUA_REGISTRYINDEX, x->perform_ref);
    lua_pushinteger(L, x->siginlets);
    lua_pushinteger(L, x->sigoutlets);
    lua_pushinteger(L, n);

    for (int i = 0; i < sum; i++)
        lua_pushlightuserdata(L, sig_vecs[i]);

    int nargs = 3 + sum;

    if (lua_pcall(L, nargs, 0, 0)) {
        pd_error(x, "pdluajit: perform: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        x->has_error = 1;
        /* Zero-fill on error */
        for (int i = 0; i < x->sigoutlets; i++) {
            t_float *out = sig_vecs[x->siginlets + i];
            for (int j = 0; j < n; j++) out[j] = 0.0f;
        }
    }

    return w + sum + 3;
}

/* ========================================================================= */
/* DSP setup                                                                 */
/* ========================================================================= */

static void pdluajit_dsp(t_pdluajit *x, t_signal **sp)
{
    int sum = x->siginlets + x->sigoutlets;
    if (sum == 0) return;

    int blocksize = (int)sp[0]->s_n;

    /* Call user's dsp(samplerate, blocksize) if defined */
    if (x->dsp_ref != LUA_NOREF) {
        lua_rawgeti(x->L, LUA_REGISTRYINDEX, x->dsp_ref);
        lua_pushnumber(x->L, sys_getsr());
        lua_pushinteger(x->L, blocksize);
        if (lua_pcall(x->L, 2, 0, 0)) {
            pd_error(x, "pdluajit: dsp: %s", lua_tostring(x->L, -1));
            lua_pop(x->L, 1);
        }
    }

    /* Update pd.samplerate and pd.blocksize in Lua */
    lua_getglobal(x->L, "pd");
    if (lua_istable(x->L, -1)) {
        lua_pushnumber(x->L, sys_getsr());
        lua_setfield(x->L, -2, "samplerate");
        lua_pushinteger(x->L, blocksize);
        lua_setfield(x->L, -2, "blocksize");
    }
    lua_pop(x->L, 1);

    /* Build signal vector for dsp_addv */
    int sigvecsize = sum + 2;
    t_int *sigvec = (t_int *)getbytes(sigvecsize * sizeof(t_int));
    sigvec[0] = (t_int)x;
    sigvec[1] = (t_int)blocksize;
    for (int i = 0; i < sum; i++)
        sigvec[i + 2] = (t_int)sp[i]->s_vec;

    dsp_addv(pdluajit_perform, sigvecsize, sigvec);
    freebytes(sigvec, sigvecsize * sizeof(t_int));

    x->has_error = 0;
}

/* ========================================================================= */
/* Class-level message handlers (sent directly to object, not via inlet)     */
/* ========================================================================= */

static void pdluajit_load(t_pdluajit *x, t_symbol *name)
{
    pdluajit_load_script(x, name);
}

static void pdluajit_reload(t_pdluajit *x)
{
    if (x->script_name)
        pdluajit_load_script(x, x->script_name);
    else
        pd_error(x, "pdluajit: no script loaded to reload");
}

static void pdluajit_lua_resized_wrapper(t_pdluajit *x, t_symbol *s, int argc, t_atom *argv)
{
    if (argc >= 2 && x->resized_ref != LUA_NOREF && x->L && !x->has_error) {
        lua_State *L = x->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, x->resized_ref);
        lua_pushinteger(L, (lua_Integer)atom_getfloat(&argv[0]));
        lua_pushinteger(L, (lua_Integer)atom_getfloat(&argv[1]));
        /* NOTE: called from enqueueFunctionAsync which holds sys_lock */
        if (lua_pcall(L, 2, 0, 0)) {
            pd_error(x, "pdluajit: resized callback: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

static void pdluajit_menu_open(t_pdluajit *x)
{
    if (x->script_path) {
        t_atom arg;
        SETSYMBOL(&arg, x->script_path);
        plugdata_forward_message(0, x, gensym("open_textfile"), 1, &arg);
    } else {
        pd_error(x, "pdluajit: no script loaded to open");
    }
}

/* ========================================================================= */
/* Constructor / Destructor                                                  */
/* =========================================================================  */

static void *pdluajit_new(t_symbol *s, int argc, t_atom *argv)
{
    t_pdluajit *x = (t_pdluajit *)pd_new(pdluajit_class);

    /* Initialize defaults — no inlets/outlets unless script defines them */
    x->num_inlets = 0;
    x->num_outlets = 0;
    x->siginlets = 0;
    x->sigoutlets = 0;
    memset(x->inlet_types, 0, sizeof(x->inlet_types));
    memset(x->outlet_types, 0, sizeof(x->outlet_types));
    x->L = NULL;
    x->perform_ref = LUA_NOREF;
    x->dsp_ref = LUA_NOREF;
    x->has_error = 0;
    x->script_name = NULL;
    x->script_path = NULL;
    x->canvas = canvas_getcurrent();
    x->has_gui = 0;
    x->gfx_width = 100;
    x->gfx_height = 100;
    x->paint_ref = LUA_NOREF;
    x->mouse_down_ref = LUA_NOREF;
    x->mouse_up_ref = LUA_NOREF;
    x->mouse_move_ref = LUA_NOREF;
    x->mouse_drag_ref = LUA_NOREF;
    x->resized_ref = LUA_NOREF;
    x->clock = NULL;
    x->tick_ref = LUA_NOREF;
    x->clock_interval = 0;

    /* First argument must be script name */
    if (argc < 1 || argv[0].a_type != A_SYMBOL) {
        pd_error(x, "pdluajit: usage: [pdluajit scriptname]");
        return x;
    }

    t_symbol *script_sym = atom_getsymbol(&argv[0]);

    /* Create LuaJIT state */
    x->L = luaL_newstate();
    if (!x->L) {
        pd_error(x, "pdluajit: failed to create LuaJIT state");
        return x;
    }
    luaL_openlibs(x->L);

    /* Register pd.post / pd.error / pd.outlet */
    pdluajit_register_api(x->L, x);

    /* Execute bootstrap code (sets SIGNAL/DATA globals, FFI wrapper) */
    if (luaL_dostring(x->L, bootstrap_code)) {
        pd_error(x, "pdluajit: bootstrap error: %s",
                 lua_tostring(x->L, -1));
        lua_pop(x->L, 1);
        x->has_error = 1;
        return x;
    }

    /* Execute graphics bootstrap (FFI cdef, Graphics class, __paint_wrapper) */
    if (luaL_dostring(x->L, gfx_bootstrap_code)) {
        pd_error(x, "pdluajit: gfx bootstrap error: %s",
                 lua_tostring(x->L, -1));
        lua_pop(x->L, 1);
        x->has_error = 1;
        return x;
    }

    /* Expose creation arguments as pd.args BEFORE loading script,
     * so that initialize(pd.args) inside pdluajit_load_script has them. */
    lua_getglobal(x->L, "pd");
    if (lua_istable(x->L, -1)) {
        lua_createtable(x->L, argc > 1 ? argc - 1 : 0, 0);
        for (int i = 1; i < argc; i++) {
            if (argv[i].a_type == A_FLOAT)
                lua_pushnumber(x->L, atom_getfloat(&argv[i]));
            else if (argv[i].a_type == A_SYMBOL)
                lua_pushstring(x->L, atom_getsymbol(&argv[i])->s_name);
            lua_rawseti(x->L, -2, i);
        }
        lua_setfield(x->L, -2, "args");
    }
    lua_pop(x->L, 1);

    /* Load user script (sets globals, caches refs, calls initialize) */
    pdluajit_load_script(x, script_sym);

    /* ---- Parse inlet configuration ---- */
    lua_getglobal(x->L, "inlets");
    if (lua_istable(x->L, -1)) {
        int n = (int)lua_objlen(x->L, -1);
        if (n < 0) n = 0;
        if (n > PDLUAJIT_MAX_IOLETS) n = PDLUAJIT_MAX_IOLETS;
        x->num_inlets = n;
        x->siginlets = 0;
        for (int i = 0; i < n; i++) {
            lua_rawgeti(x->L, -1, i + 1);
            x->inlet_types[i] = (lua_tointeger(x->L, -1) == PDLUAJIT_TYPE_SIGNAL)
                                  ? PDLUAJIT_TYPE_SIGNAL : PDLUAJIT_TYPE_DATA;
            if (x->inlet_types[i] == PDLUAJIT_TYPE_SIGNAL)
                x->siginlets++;
            lua_pop(x->L, 1);
        }
    }
    /* else: no inlets (CLASS_NOINLET, no implicit inlet) */
    lua_pop(x->L, 1);

    /* ---- Parse outlet configuration ---- */
    lua_getglobal(x->L, "outlets");
    if (lua_istable(x->L, -1)) {
        int n = (int)lua_objlen(x->L, -1);
        if (n < 0) n = 0;
        if (n > PDLUAJIT_MAX_IOLETS) n = PDLUAJIT_MAX_IOLETS;
        x->num_outlets = n;
        x->sigoutlets = 0;
        for (int i = 0; i < n; i++) {
            lua_rawgeti(x->L, -1, i + 1);
            x->outlet_types[i] = (lua_tointeger(x->L, -1) == PDLUAJIT_TYPE_SIGNAL)
                                   ? PDLUAJIT_TYPE_SIGNAL : PDLUAJIT_TYPE_DATA;
            if (x->outlet_types[i] == PDLUAJIT_TYPE_SIGNAL)
                x->sigoutlets++;
            lua_pop(x->L, 1);
        }
    }
    /* else: no outlets */
    lua_pop(x->L, 1);

    /* Check perform() is defined if we have signal io */
    if (x->siginlets > 0 || x->sigoutlets > 0) {
        lua_getglobal(x->L, "perform");
        if (!lua_isfunction(x->L, -1)) {
            pd_error(x, "pdluajit: script '%s' must define perform() "
                     "when using signal inlets/outlets", script_sym->s_name);
            x->has_error = 1;
        }
        lua_pop(x->L, 1);
    }

    /* ---- Create inlets ---- */
    /* CLASS_NOINLET suppresses the implicit first inlet.
     * All inlets (including index 0) are created as proxy inlets. */
    for (int i = 0; i < x->num_inlets; i++) {
        pdluajit_proxyinlet_init(&x->proxy_in[i], x, i);
        if (x->inlet_types[i] == PDLUAJIT_TYPE_SIGNAL) {
            x->in[i] = inlet_new(&x->x_obj, &x->proxy_in[i].pd,
                                  &s_signal, &s_signal);
        } else {
            x->in[i] = inlet_new(&x->x_obj, &x->proxy_in[i].pd, 0, 0);
        }
    }

    /* ---- Create outlets ---- */
    for (int i = 0; i < x->num_outlets; i++) {
        t_symbol *sym = (x->outlet_types[i] == PDLUAJIT_TYPE_SIGNAL)
                         ? &s_signal : NULL;
        x->out[i] = outlet_new(&x->x_obj, sym);
    }

    return x;
}

static void pdluajit_free(t_pdluajit *x)
{
    if (x->L) {
        if (x->perform_ref != LUA_NOREF)
            luaL_unref(x->L, LUA_REGISTRYINDEX, x->perform_ref);
        if (x->dsp_ref != LUA_NOREF)
            luaL_unref(x->L, LUA_REGISTRYINDEX, x->dsp_ref);
        if (x->paint_ref != LUA_NOREF)
            luaL_unref(x->L, LUA_REGISTRYINDEX, x->paint_ref);
        if (x->mouse_down_ref != LUA_NOREF)
            luaL_unref(x->L, LUA_REGISTRYINDEX, x->mouse_down_ref);
        if (x->mouse_up_ref != LUA_NOREF)
            luaL_unref(x->L, LUA_REGISTRYINDEX, x->mouse_up_ref);
        if (x->mouse_move_ref != LUA_NOREF)
            luaL_unref(x->L, LUA_REGISTRYINDEX, x->mouse_move_ref);
        if (x->mouse_drag_ref != LUA_NOREF)
            luaL_unref(x->L, LUA_REGISTRYINDEX, x->mouse_drag_ref);
        if (x->resized_ref != LUA_NOREF)
            luaL_unref(x->L, LUA_REGISTRYINDEX, x->resized_ref);
        if (x->tick_ref != LUA_NOREF)
            luaL_unref(x->L, LUA_REGISTRYINDEX, x->tick_ref);
        lua_close(x->L);
        x->L = NULL;
    }
    if (x->clock) {
        clock_free(x->clock);
        x->clock = NULL;
    }
    /* Pd handles inlet/outlet cleanup when the object is freed */
}

/* ========================================================================= */
/* Setup — register [pdluajit] and proxy inlet class with Pd                 */
/* ========================================================================= */

void pdluajit_setup(void)
{
    /* Register proxy inlet class first */
    pdluajit_proxyinlet_setup();

    pdluajit_class = class_new(pdluajit_global_gensym("pdluajit"),
        (t_newmethod)pdluajit_new,
        (t_method)pdluajit_free,
        sizeof(t_pdluajit),
        CLASS_NOINLET,
        A_GIMME, 0);

    class_addmethod(pdluajit_class, (t_method)pdluajit_dsp,
                    pdluajit_global_gensym("dsp"), A_CANT, 0);
    class_addmethod(pdluajit_class, (t_method)pdluajit_load,
                    pdluajit_global_gensym("load"), A_SYMBOL, 0);
    class_addmethod(pdluajit_class, (t_method)pdluajit_reload,
                    pdluajit_global_gensym("reload"), A_NULL, 0);
    class_addmethod(pdluajit_class, (t_method)pdluajit_lua_resized_wrapper,
                    pdluajit_global_gensym("lua_resized"), A_GIMME, 0);
    class_addmethod(pdluajit_class, (t_method)pdluajit_menu_open,
                    pdluajit_global_gensym("menu-open"), A_NULL, 0);

    post("pdluajit: LuaJIT DSP external for plugdata");
}
