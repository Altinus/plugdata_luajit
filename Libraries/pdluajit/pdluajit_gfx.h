/*
 * pdluajit_gfx.h — Graphics bridge header for pdluajit
 *
 * Defines the function-pointer vtable for NanoVG drawing operations and
 * public C functions for paint/mouse callbacks. This header uses only
 * void* and C primitives — no LuaJIT or NanoVG dependencies.
 *
 * C++ side (LuaObject.h) fills the vtable with NanoVG wrappers.
 * Lua side declares the same struct via FFI and calls through it.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published
 * by the Free Software Foundation.
 */

#ifndef PDLUAJIT_GFX_H
#define PDLUAJIT_GFX_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Function-pointer vtable for NanoVG drawing operations.
 *
 * Each function takes a void* ctx (NVGcontext*) as first argument.
 * The Lua FFI declares this same struct and calls through the pointers.
 */
typedef struct pdluajit_gfx_vtable {
    /* State management */
    void (*save)(void *ctx);
    void (*restore)(void *ctx);
    void (*reset_transform)(void *ctx);     /* restore + save */

    /* Color and stroke */
    void (*set_color)(void *ctx, int r, int g, int b, int a);
    void (*stroke_width)(void *ctx, float width);

    /* Filled shapes */
    void (*fill_rect)(void *ctx, float x, float y, float w, float h);
    void (*fill_rounded_rect)(void *ctx, float x, float y, float w, float h, float radius);
    void (*fill_ellipse)(void *ctx, float x, float y, float w, float h);

    /* Stroked shapes */
    void (*stroke_rect)(void *ctx, float x, float y, float w, float h);
    void (*stroke_rounded_rect)(void *ctx, float x, float y, float w, float h, float radius);
    void (*stroke_ellipse)(void *ctx, float x, float y, float w, float h);

    /* Lines */
    void (*draw_line)(void *ctx, float x1, float y1, float x2, float y2);

    /* Path operations */
    void (*begin_path)(void *ctx);
    void (*move_to)(void *ctx, float x, float y);
    void (*line_to)(void *ctx, float x, float y);
    void (*quad_to)(void *ctx, float cx, float cy, float x, float y);
    void (*bezier_to)(void *ctx, float c1x, float c1y, float c2x, float c2y, float x, float y);
    void (*close_path)(void *ctx);
    void (*fill)(void *ctx);
    void (*stroke)(void *ctx);

    /* Transforms */
    void (*translate)(void *ctx, float tx, float ty);
    void (*scale)(void *ctx, float sx, float sy);

    /* Composite operations */
    /* fill_all: draws background rect with fill color + outline color + corner radius */
    void (*fill_all)(void *ctx, float w, float h,
                     unsigned int fill_rgba, unsigned int outline_rgba,
                     float corner_radius);

    /* Text */
    void (*draw_text)(void *ctx, const char *text, float x, float y,
                      float max_width, float font_height);

    /* Images */
    int (*create_image)(void *ctx, const char *filename, int imageFlags);
    void (*delete_image)(void *ctx, int image);
    void (*draw_image)(void *ctx, int image, float x, float y, float w, float h, float alpha);
    void (*set_fill_paint_image_pattern)(void *ctx, float ox, float oy, float ex, float ey, float angle, int image, float alpha);
    int (*create_image_rgba)(void *ctx, int w, int h, int imageFlags, const unsigned char *data);
    void (*update_image)(void *ctx, int image, const unsigned char *data);
} pdluajit_gfx_vtable;

/* ========================================================================= */
/* Public C functions — called by C++ (LuaJitObject)                         */
/* ========================================================================= */

/**
 * Check if a pdluajit object has a GUI (paint function defined).
 * @param x  Pointer to the t_pdluajit object (as void* to avoid header deps)
 * @return   Non-zero if the object has a paint() function
 */
int pdluajit_has_gui(void *x);

/**
 * Call the object's paint function with the given vtable and context.
 * Called from the render thread under sys_lock().
 *
 * @param x             Pointer to t_pdluajit
 * @param ctx           NVGcontext* (as void*)
 * @param vt            Pointer to filled pdluajit_gfx_vtable
 * @param width         Current widget width
 * @param height        Current widget height
 * @param outline_rgba  Outline color packed as uint32 (ABGR)
 */
void pdluajit_paint(void *x, void *ctx, const pdluajit_gfx_vtable *vt,
                    int width, int height, unsigned int outline_rgba);

/* Mouse event callbacks — called from render/message thread under sys_lock() */
void pdluajit_mouse_down(void *x, int mx, int my);
void pdluajit_mouse_up(void *x, int mx, int my);
void pdluajit_mouse_move(void *x, int mx, int my);
void pdluajit_mouse_drag(void *x, int mx, int my);

/* Size getters/setters */
int pdluajit_get_width(void *x);
int pdluajit_get_height(void *x);
void pdluajit_set_size(void *x, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* PDLUAJIT_GFX_H */
