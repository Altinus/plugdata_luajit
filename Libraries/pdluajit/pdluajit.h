/*
 * pdluajit.h — LuaJIT-powered DSP external for Pure Data / plugdata
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published
 * by the Free Software Foundation.
 */

#ifndef PDLUAJIT_H
#define PDLUAJIT_H

#include "m_pd.h"

/*
 * Include the rename header BEFORE LuaJIT headers to avoid symbol
 * conflicts with Lua 5.4 (used by pd-lua). See luajit_rename.h for details.
 */
#include "luajit_rename.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

/* Maximum total inlets/outlets (signal + data combined) */
#define PDLUAJIT_MAX_IOLETS 16

/* Inlet/outlet type constants (match Lua-side SIGNAL/DATA) */
#define PDLUAJIT_TYPE_DATA   0
#define PDLUAJIT_TYPE_SIGNAL 1

/** Proxy inlet — lightweight Pd object that forwards messages to owner. */
typedef struct _pdluajit_proxyinlet {
    t_pd                pd;         /* minimal Pd object (just class ptr) */
    struct _pdluajit    *owner;     /* owning pdluajit object */
    unsigned int        id;         /* inlet index (0-based) */
} t_pdluajit_proxyinlet;

typedef struct _pdluajit {
    t_object    x_obj;

    /* Inlet/outlet configuration */
    int         num_inlets;         /* total number of inlets */
    int         num_outlets;        /* total number of outlets */
    int         siginlets;          /* count of signal inlets */
    int         sigoutlets;         /* count of signal outlets */
    int         inlet_types[PDLUAJIT_MAX_IOLETS];   /* per-inlet type */
    int         outlet_types[PDLUAJIT_MAX_IOLETS];  /* per-outlet type */

    /* Pd inlet/outlet objects */
    t_pdluajit_proxyinlet proxy_in[PDLUAJIT_MAX_IOLETS]; /* proxy per inlet */
    t_inlet     *in[PDLUAJIT_MAX_IOLETS];           /* Pd inlet pointers */
    t_outlet    *out[PDLUAJIT_MAX_IOLETS];           /* Pd outlet pointers */

    /* LuaJIT state */
    lua_State   *L;                 /* per-object LuaJIT state */
    int         perform_ref;        /* registry ref: __perform_wrapper */
    int         dsp_ref;            /* registry ref: user's dsp() */
    int         has_error;          /* error flag, silences output */
    t_symbol    *script_name;       /* loaded script name */
    t_symbol    *script_path;       /* loaded script full path */
    t_canvas    *canvas;            /* parent canvas for path search */

    /* Graphics / GUI state */
    int         has_gui;            /* 1 if paint() is defined */
    int         gfx_width;          /* GUI widget width */
    int         gfx_height;         /* GUI widget height */
    int         paint_ref;          /* registry ref: __paint_wrapper */
    int         mouse_down_ref;     /* registry ref: mouse_down() */
    int         mouse_up_ref;       /* registry ref: mouse_up() */
    int         mouse_move_ref;     /* registry ref: mouse_move() */
    int         mouse_drag_ref;     /* registry ref: mouse_drag() */
    int         resized_ref;        /* registry ref: resized(w,h) */

    /* Pd clock for periodic callbacks */
    t_clock     *clock;             /* Pd clock object */
    int         tick_ref;           /* registry ref: tick() */
    double      clock_interval;     /* interval in ms, 0 = stopped */
} t_pdluajit;

/* Setup function — called once to register [pdluajit] class with Pd */
void pdluajit_setup(void);

#endif /* PDLUAJIT_H */
