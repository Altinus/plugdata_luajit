/*
 * luajit_rename.h — Symbol renaming for LuaJIT to avoid conflicts with Lua 5.4
 *
 * This header MUST be included before any LuaJIT headers (lua.h, lauxlib.h, lualib.h)
 * in both:
 *   1. The LuaJIT library build (via -include luajit_rename.h in CFLAGS)
 *   2. All source files that call LuaJIT API (pdluajit.c etc.)
 *
 * This renames all exported lua_*, luaL_*, luaopen_* and luaJIT_* symbols
 * so they do not collide with the Lua 5.4 symbols used by pd-lua.
 *
 * Prefix convention: lua_foo -> luajit_foo, luaL_foo -> luajitL_foo
 *
 * License: MIT (same as LuaJIT)
 */

#ifndef LUAJIT_RENAME_H
#define LUAJIT_RENAME_H

/* ========================================================================= */
/* lua.h — Core API                                                          */
/* ========================================================================= */

/* State management */
#define lua_newstate        luajit_newstate
#define lua_close           luajit_close
#define lua_newthread       luajit_newthread
#define lua_atpanic         luajit_atpanic
#define lua_version         luajit_version

/* Stack operations */
#define lua_gettop          luajit_gettop
#define lua_settop          luajit_settop
#define lua_pushvalue       luajit_pushvalue
#define lua_remove          luajit_remove
#define lua_insert          luajit_insert
#define lua_replace         luajit_replace
#define lua_checkstack      luajit_checkstack
#define lua_xmove           luajit_xmove

/* Type checks */
#define lua_isnumber        luajit_isnumber
#define lua_isstring        luajit_isstring
#define lua_iscfunction     luajit_iscfunction
#define lua_isuserdata      luajit_isuserdata
#define lua_type            luajit_type
#define lua_typename        luajit_typename

/* Comparison */
#define lua_equal           luajit_equal
#define lua_rawequal        luajit_rawequal
#define lua_lessthan        luajit_lessthan

/* Getters (stack -> C) */
#define lua_tonumber        luajit_tonumber
#define lua_tointeger       luajit_tointeger
#define lua_toboolean       luajit_toboolean
#define lua_tolstring       luajit_tolstring
#define lua_objlen          luajit_objlen
#define lua_tocfunction     luajit_tocfunction
#define lua_touserdata      luajit_touserdata
#define lua_tothread        luajit_tothread
#define lua_topointer       luajit_topointer

/* Pushers (C -> stack) */
#define lua_pushnil         luajit_pushnil
#define lua_pushnumber      luajit_pushnumber
#define lua_pushinteger     luajit_pushinteger
#define lua_pushlstring     luajit_pushlstring
#define lua_pushstring      luajit_pushstring
#define lua_pushvfstring    luajit_pushvfstring
#define lua_pushfstring     luajit_pushfstring
#define lua_pushcclosure    luajit_pushcclosure
#define lua_pushboolean     luajit_pushboolean
#define lua_pushlightuserdata luajit_pushlightuserdata
#define lua_pushthread      luajit_pushthread

/* Table access */
#define lua_gettable        luajit_gettable
#define lua_getfield        luajit_getfield
#define lua_rawget          luajit_rawget
#define lua_rawgeti         luajit_rawgeti
#define lua_createtable     luajit_createtable
#define lua_newuserdata     luajit_newuserdata
#define lua_getmetatable    luajit_getmetatable
#define lua_getfenv         luajit_getfenv

/* Table setters */
#define lua_settable        luajit_settable
#define lua_setfield        luajit_setfield
#define lua_rawset          luajit_rawset
#define lua_rawseti         luajit_rawseti
#define lua_setmetatable    luajit_setmetatable
#define lua_setfenv         luajit_setfenv

/* Call / execute */
#define lua_call            luajit_call
#define lua_pcall           luajit_pcall
#define lua_cpcall          luajit_cpcall
#define lua_load            luajit_load
#define lua_loadx           luajit_loadx
#define lua_dump            luajit_dump

/* Coroutines */
#define lua_yield           luajit_yield
#define lua_resume          luajit_resume
#define lua_status          luajit_status

/* GC */
#define lua_gc              luajit_gc

/* Misc */
#define lua_error           luajit_error
#define lua_next            luajit_next
#define lua_concat          luajit_concat

/* Allocator */
#define lua_getallocf       luajit_getallocf
#define lua_setallocf       luajit_setallocf

/* Misc (LuaJIT extension) */
#define lua_setlevel        luajit_setlevel

/* Debug interface */
#define lua_getstack        luajit_getstack
#define lua_getinfo         luajit_getinfo
#define lua_getlocal        luajit_getlocal
#define lua_setlocal        luajit_setlocal
#define lua_getupvalue      luajit_getupvalue
#define lua_setupvalue      luajit_setupvalue
#define lua_sethook         luajit_sethook
#define lua_gethook         luajit_gethook
#define lua_gethookmask     luajit_gethookmask
#define lua_gethookcount    luajit_gethookcount

/* LuaJIT 2.1 extensions (5.2 compat) */
#define lua_upvalueid       luajit_upvalueid
#define lua_upvaluejoin     luajit_upvaluejoin
#define lua_copy            luajit_copy
#define lua_tonumberx       luajit_tonumberx
#define lua_tointegerx      luajit_tointegerx
#define lua_isyieldable     luajit_isyieldable
#define lua_setcstacklimit  luajit_setcstacklimit

/* ========================================================================= */
/* lauxlib.h — Auxiliary library                                             */
/* ========================================================================= */

#define luaL_openlib        luajitL_openlib
#define luaL_register       luajitL_register
#define luaL_getmetafield   luajitL_getmetafield
#define luaL_callmeta       luajitL_callmeta
#define luaL_typerror       luajitL_typerror
#define luaL_argerror       luajitL_argerror
#define luaL_checklstring   luajitL_checklstring
#define luaL_optlstring     luajitL_optlstring
#define luaL_checknumber    luajitL_checknumber
#define luaL_optnumber      luajitL_optnumber
#define luaL_checkinteger   luajitL_checkinteger
#define luaL_optinteger     luajitL_optinteger
#define luaL_checkstack     luajitL_checkstack
#define luaL_checktype      luajitL_checktype
#define luaL_checkany       luajitL_checkany
#define luaL_newmetatable   luajitL_newmetatable
#define luaL_checkudata     luajitL_checkudata
#define luaL_where          luajitL_where
#define luaL_error          luajitL_error
#define luaL_checkoption    luajitL_checkoption
#define luaL_ref            luajitL_ref
#define luaL_unref          luajitL_unref
#define luaL_loadfile       luajitL_loadfile
#define luaL_loadbuffer     luajitL_loadbuffer
#define luaL_loadstring     luajitL_loadstring
#define luaL_newstate       luajitL_newstate
#define luaL_gsub           luajitL_gsub
#define luaL_findtable      luajitL_findtable
#define luaL_fileresult     luajitL_fileresult
#define luaL_execresult     luajitL_execresult
#define luaL_loadfilex      luajitL_loadfilex
#define luaL_loadbufferx    luajitL_loadbufferx
#define luaL_traceback      luajitL_traceback
#define luaL_setfuncs       luajitL_setfuncs
#define luaL_pushmodule     luajitL_pushmodule
#define luaL_testudata      luajitL_testudata
#define luaL_setmetatable   luajitL_setmetatable

/* Buffer operations */
#define luaL_buffinit       luajitL_buffinit
#define luaL_prepbuffer     luajitL_prepbuffer
#define luaL_addlstring     luajitL_addlstring
#define luaL_addstring      luajitL_addstring
#define luaL_addvalue       luajitL_addvalue
#define luaL_pushresult     luajitL_pushresult

/* ========================================================================= */
/* lualib.h — Standard library openers                                       */
/* ========================================================================= */

#define luaL_openlibs       luajitL_openlibs
#define luaopen_base        luajit_open_base
#define luaopen_math        luajit_open_math
#define luaopen_string      luajit_open_string
#define luaopen_table       luajit_open_table
#define luaopen_io          luajit_open_io
#define luaopen_os          luajit_open_os
#define luaopen_package     luajit_open_package
#define luaopen_debug       luajit_open_debug
#define luaopen_bit         luajit_open_bit
#define luaopen_jit         luajit_open_jit
#define luaopen_ffi         luajit_open_ffi
#define luaopen_string_buffer luajit_open_string_buffer

/* ========================================================================= */
/* luajit.h — LuaJIT-specific API                                           */
/* ========================================================================= */

#define luaJIT_setmode      luajit_setmode
#define luaJIT_profile_start    luajit_profile_start
#define luaJIT_profile_stop     luajit_profile_stop
#define luaJIT_profile_dumpstack luajit_profile_dumpstack

#endif /* LUAJIT_RENAME_H */
