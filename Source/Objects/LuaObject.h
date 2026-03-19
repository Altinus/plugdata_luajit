/*
 // Copyright (c) 2021-2025 Timothy Schoen
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */
#pragma once

extern "C" {
#include <pd-lua/lua/lua.h>

#define PLUGDATA 1
#include <pd-lua/pdlua.h>
#undef PLUGDATA

#ifdef ENABLE_LUAJIT
#include <pdluajit/pdluajit_gfx.h>
#endif

void pdlua_gfx_mouse_down(t_pdlua* o, int x, int y);
void pdlua_gfx_mouse_up(t_pdlua* o, int x, int y);
void pdlua_gfx_mouse_move(t_pdlua* o, int x, int y);
void pdlua_gfx_mouse_drag(t_pdlua* o, int x, int y);
void pdlua_gfx_repaint(t_pdlua* o, int firsttime);
}

class LuaObject final : public ObjectBase
    , private Value::Listener {
    Colour currentColour;

    t_symbol* pdluaxSymbol;
    bool isSelected = false;
    Value zoomScale;
    std::unique_ptr<Component> textEditor;
    std::unique_ptr<Dialog> saveDialog;

    UnorderedSegmentedMap<int, NVGFramebuffer> framebuffers;

    struct LuaGuiMessage {
        t_symbol* symbol;
        SmallArray<t_atom> data;
        int size;

        LuaGuiMessage() { }

        LuaGuiMessage(t_symbol* sym, int const argc, t_atom* argv)
            : symbol(sym)
        {
            data = SmallArray<t_atom>(argv, argv + argc);
            size = argc;
        }

        LuaGuiMessage(LuaGuiMessage const& other) noexcept
        {
            symbol = other.symbol;
            size = other.size;
            data = other.data;
        }

        LuaGuiMessage& operator=(LuaGuiMessage const& other) noexcept
        {
            // Check for self-assignment
            if (this != &other) {
                symbol = other.symbol;
                size = other.size;
                data = other.data;
            }

            return *this;
        }
    };

    UnorderedSegmentedMap<int, HeapArray<LuaGuiMessage>> guiCommandBuffer;
    UnorderedSegmentedMap<int, moodycamel::ReaderWriterQueue<LuaGuiMessage>> guiMessageQueue;

    static inline auto allDrawTargets = UnorderedMap<t_pdlua*, SmallArray<LuaObject*>>();

public:
    LuaObject(pd::WeakReference obj, Object* parent)
        : ObjectBase(obj, parent)
    {
        if (auto pdlua = ptr.get<t_pdlua>()) {
            pdlua->gfx.plugdata_draw_callback = &drawCallback;
            allDrawTargets[pdlua.get()].add(this);

            libpd_set_instance(&pd_maininstance);
            pdluaxSymbol = gensym("pdluax");
            pd->setThis();
        }

        object->editor->nvgSurface.addBufferedObject(this);
        parentHierarchyChanged();
    }

    ~LuaObject() override
    {
        pd->lockAudioThread();
        auto& listeners = allDrawTargets[ptr.getRawUnchecked<t_pdlua>()];
        listeners.erase(std::ranges::remove(listeners, this).begin(), listeners.end());
        pd->unlockAudioThread();

        zoomScale.removeListener(this);
        object->editor->nvgSurface.removeBufferedObject(this);
    }

    // We can only attach the zoomscale to canvas after the canvas has been added to its own parent
    // When initialising, this isn't always the case
    void parentHierarchyChanged() override
    {
        // We need to get the actual zoom from the top level canvas, not of the graph this happens to be inside of
        auto const* topLevelCanvas = cnv;
        while (auto const* nextCanvas = topLevelCanvas->findParentComponentOfClass<Canvas>()) {
            topLevelCanvas = nextCanvas;
        }

        zoomScale.referTo(topLevelCanvas->zoomScale);
        zoomScale.addListener(this);
        sendRepaintMessage();
    }

    Rectangle<int> getPdBounds() override
    {
        if (auto gobj = ptr.get<t_pdlua>()) {
            auto* patch = cnv->patch.getRawPointer();

            int x = 0, y = 0, w = 0, h = 0;
            pd::Interface::getObjectBounds(patch, gobj.cast<t_gobj>(), &x, &y, &w, &h);

            return Rectangle<int>(x, y, gobj->gfx.width + 2, gobj->gfx.height + 2);
        }

        return { };
    }

    void setPdBounds(Rectangle<int> const b) override
    {
        if (auto gobj = ptr.get<t_pdlua>()) {
            auto* patch = object->cnv->patch.getRawPointer();

            pd::Interface::moveObject(patch, gobj.cast<t_gobj>(), b.getX(), b.getY());
            gobj->gfx.width = b.getWidth() - 2;
            gobj->gfx.height = b.getHeight() - 2;
        }

        sendRepaintMessage();
    }

    void updateSizeProperty() override
    {
    }

    void getMenuOptions(PopupMenu& menu) override
    {
        menu.addItem("Open lua editor", [_this = SafePointer(this)] {
            if (!_this)
                return;
            _this->sendMessage("menu-open");
        });
        menu.addItem("Reload lua object", [_this = SafePointer(this)] {
            if (!_this)
                return;

            // prevent potential crash if this was selected
            _this->cnv->editor->sidebar->hideParameters();

            if (auto pdlua = _this->ptr.get<t_pd>()) {
                // Reload the lua script
                pd_typedmess(_this->pdluaxSymbol->s_thing, gensym("reload"), 0, nullptr);

                // Recreate this object
                if (auto patch = _this->cnv->patch.getPointer()) {
                    pd::Interface::recreateTextObject(patch.get(), pdlua.cast<t_gobj>());
                }
                _this->cnv->synchronise();
            }
        });
    }

    void mouseDown(MouseEvent const& e) override
    {
        pd->enqueueFunctionAsync<t_pdlua>(ptr, [x = e.x, y = e.y](t_pdlua* pdlua) {
            sys_lock();
            pdlua_gfx_mouse_down(pdlua, x, y);
            sys_unlock();
        });
    }

    void mouseDrag(MouseEvent const& e) override
    {
        pd->enqueueFunctionAsync<t_pdlua>(ptr, [x = e.x, y = e.y](t_pdlua* pdlua) {
            sys_lock();
            pdlua_gfx_mouse_drag(pdlua, x, y);
            sys_unlock();
        });
    }

    void mouseMove(MouseEvent const& e) override
    {
        pd->enqueueFunctionAsync<t_pdlua>(ptr, [x = e.x, y = e.y](t_pdlua* pdlua) {
            sys_lock();
            pdlua_gfx_mouse_move(pdlua, x, y);
            sys_unlock();
        });
    }

    void mouseUp(MouseEvent const& e) override
    {
        pd->enqueueFunctionAsync<t_pdlua>(ptr, [x = e.x, y = e.y](t_pdlua* pdlua) {
            sys_lock();
            pdlua_gfx_mouse_up(pdlua, x, y);
            sys_unlock();
        });
    }

    void sendRepaintMessage()
    {
        pd->enqueueFunctionAsync<t_pdlua>(ptr, [](t_pdlua* pdlua) {
            sys_lock();
            pdlua_gfx_repaint(pdlua, 0);
            sys_unlock();
        });
    }

    void resized() override
    {
        sendRepaintMessage();
    }

    void lookAndFeelChanged() override
    {
        sendRepaintMessage();
    }

    void render(NVGcontext* nvg) override
    {
        NVGScopedState scopedState(nvg);

        auto scale = nvgCurrentPixelScale(nvg) * getValue<float>(zoomScale);
        nvgScale(nvg, 1.0f / scale, 1.0f / scale);
        nvgTransformQuantize(nvg);

        for (auto& [layer, fb] : framebuffers) {
            fb.render(nvg, Rectangle<int>(std::ceil(getWidth() * scale), std::ceil(getHeight() * scale)));
        }
    }

    void valueChanged(Value& v) override
    {
        sendRepaintMessage();
    }

    void handleGuiMessage(NVGcontext* nvg, int const layer, t_symbol* sym, int const argc, t_atom* argv)
    {
        auto const hashsym = hash(sym->s_name);
        // First check functions that don't need an active graphics context, of modify the active graphics context
        switch (hashsym) {
        case hash("lua_start_paint"): {
            if (getLocalBounds().isEmpty())
                break;

            auto const pixelScale = nvgCurrentPixelScale(nvg);
            auto const zoom = getValue<float>(zoomScale);
            auto const imageScale = zoom * pixelScale;
            int const imageWidth = std::ceil(getWidth() * imageScale);
            int const imageHeight = std::ceil(getHeight() * imageScale);
            if (!imageWidth || !imageHeight)
                return;

            framebuffers[layer].bind(nvg, imageWidth, imageHeight);

            nvgViewport(0, 0, imageWidth, imageHeight);
            nvgClear(nvg);
            nvgBeginFrame(nvg, getWidth() * zoom, getHeight() * zoom, pixelScale);
            nvgScale(nvg, zoom, zoom);
            nvgSave(nvg);
            return;
        }
        case hash("lua_end_paint"): {
            if (!framebuffers[layer].isValid())
                return;

            auto const pixelScale = nvgCurrentPixelScale(nvg);
            auto const scale = getValue<float>(zoomScale) * pixelScale;
            nvgGlobalScissor(nvg, 0, 0, getWidth() * scale, getHeight() * scale);
            nvgEndFrame(nvg);
            framebuffers[layer].unbind();
            repaint();
            return;
        }
        case hash("lua_resized"): {
            if (argc >= 2) {
                if (auto pdlua = ptr.get<t_pdlua>()) {
                    pdlua->gfx.width = atom_getfloat(argv);
                    pdlua->gfx.height = atom_getfloat(argv + 1);
                }
                MessageManager::callAsync([_object = SafePointer(object)] {
                    if (_object)
                        _object->updateBounds();
                });
            }
            return;
        }
        default:
            break;
        }

        switch (hashsym) {
        case hash("lua_set_color"): {
            if (argc == 1) {
                int const colourID = std::min<int>(atom_getfloat(argv), 2);

                currentColour = StackArray<Colour, 3> { PlugDataColours::guiObjectBackgroundColour,
                    PlugDataColours::canvasTextColour,
                    PlugDataColours::guiObjectInternalOutlineColour }[colourID];
                nvgFillColor(nvg, nvgColour(currentColour));
                nvgStrokeColor(nvg, nvgColour(currentColour));
            }
            if (argc >= 3) {
                Colour const color(static_cast<uint8>(atom_getfloat(argv)),
                    static_cast<uint8>(atom_getfloat(argv + 1)),
                    static_cast<uint8>(atom_getfloat(argv + 2)));

                currentColour = color.withAlpha(argc >= 4 ? atom_getfloat(argv + 3) : 1.0f);
                nvgFillColor(nvg, nvgColour(currentColour));
                nvgStrokeColor(nvg, nvgColour(currentColour));
            }
            break;
        }
        case hash("lua_stroke_line"): {
            if (argc >= 4) {
                float const x1 = atom_getfloat(argv);
                float const y1 = atom_getfloat(argv + 1);
                float const x2 = atom_getfloat(argv + 2);
                float const y2 = atom_getfloat(argv + 3);
                float const lineThickness = atom_getfloat(argv + 4);

                nvgStrokeWidth(nvg, lineThickness);
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, x1, y1);
                nvgLineTo(nvg, x2, y2);
                nvgStroke(nvg);
            }
            break;
        }
        case hash("lua_fill_ellipse"): {
            if (argc >= 3) {
                float const x = atom_getfloat(argv);
                float const y = atom_getfloat(argv + 1);
                float const w = atom_getfloat(argv + 2);
                float const h = atom_getfloat(argv + 3);

                nvgBeginPath(nvg);
                nvgEllipse(nvg, x + w / 2, y + h / 2, w / 2, h / 2);
                nvgFill(nvg);
            }
            break;
        }
        case hash("lua_stroke_ellipse"): {
            if (argc >= 4) {
                float const x = atom_getfloat(argv);
                float const y = atom_getfloat(argv + 1);
                float const w = atom_getfloat(argv + 2);
                float const h = atom_getfloat(argv + 3);
                float const lineThickness = atom_getfloat(argv + 4);

                nvgStrokeWidth(nvg, lineThickness);
                nvgBeginPath(nvg);
                nvgEllipse(nvg, x + w / 2, y + h / 2, w / 2, h / 2);
                nvgStroke(nvg);
            }
            break;
        }
        case hash("lua_fill_rect"): {
            if (argc >= 4) {
                float const x = atom_getfloat(argv);
                float const y = atom_getfloat(argv + 1);
                float const w = atom_getfloat(argv + 2);
                float const h = atom_getfloat(argv + 3);

                nvgFillRect(nvg, x, y, w, h);
            }
            break;
        }
        case hash("lua_stroke_rect"): {
            if (argc >= 5) {
                float const x = atom_getfloat(argv);
                float const y = atom_getfloat(argv + 1);
                float const w = atom_getfloat(argv + 2);
                float const h = atom_getfloat(argv + 3);
                float const lineThickness = atom_getfloat(argv + 4);

                nvgStrokeWidth(nvg, lineThickness);
                nvgStrokeRect(nvg, x, y, w, h);
            }
            break;
        }
        case hash("lua_fill_rounded_rect"): {
            if (argc >= 4) {
                float const x = atom_getfloat(argv);
                float const y = atom_getfloat(argv + 1);
                float const w = atom_getfloat(argv + 2);
                float const h = atom_getfloat(argv + 3);
                float const cornerRadius = atom_getfloat(argv + 4);

                nvgFillRoundedRect(nvg, x, y, w, h, cornerRadius);
            }
            break;
        }

        case hash("lua_stroke_rounded_rect"): {
            if (argc >= 6) {
                float const x = atom_getfloat(argv);
                float const y = atom_getfloat(argv + 1);
                float const w = atom_getfloat(argv + 2);
                float const h = atom_getfloat(argv + 3);
                float const cornerRadius = atom_getfloat(argv + 4);
                float const lineThickness = atom_getfloat(argv + 5);

                nvgStrokeWidth(nvg, lineThickness);
                nvgBeginPath(nvg);
                nvgRoundedRect(nvg, x, y, w, h, cornerRadius);
                nvgStroke(nvg);
            }
            break;
        }
        case hash("lua_draw_line"): {
            if (argc >= 4) {
                float const x1 = atom_getfloat(argv);
                float const y1 = atom_getfloat(argv + 1);
                float const x2 = atom_getfloat(argv + 2);
                float const y2 = atom_getfloat(argv + 3);
                float const lineThickness = atom_getfloat(argv + 4);

                nvgStrokeWidth(nvg, lineThickness);
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, x1, y1);
                nvgLineTo(nvg, x2, y2);
                nvgStroke(nvg);
            }
            break;
        }
        case hash("lua_draw_text"): {
            if (argc >= 4) {
                float const x = atom_getfloat(argv + 1);
                float const y = atom_getfloat(argv + 2);
                float const w = atom_getfloat(argv + 3);
                float const fontHeight = atom_getfloat(argv + 4);

                nvgBeginPath(nvg);
                nvgFontSize(nvg, fontHeight);
                nvgTextAlign(nvg, NVG_ALIGN_TOP | NVG_ALIGN_LEFT);
                nvgTextBox(nvg, x, y, w, atom_getsymbol(argv)->s_name, nullptr);
            }
            break;
        }
        case hash("lua_fill_path"): {
            nvgBeginPath(nvg);
            nvgMoveTo(nvg, atom_getfloat(argv), atom_getfloat(argv + 1));
            for (int i = 1; i < argc / 2; i++) {
                float const x = atom_getfloat(argv + i * 2);
                float const y = atom_getfloat(argv + i * 2 + 1);
                nvgLineTo(nvg, x, y);
            }

            nvgClosePath(nvg);
            nvgFill(nvg);
            break;
        }
        case hash("lua_stroke_path"): {
            nvgBeginPath(nvg);
            auto const strokeWidth = atom_getfloat(argv);

            int const numPoints = (argc - 1) / 2;
            nvgMoveTo(nvg, atom_getfloat(argv + 1), atom_getfloat(argv + 2));
            for (int i = 1; i < numPoints; i++) {
                float const x = atom_getfloat(argv + i * 2 + 1);
                float const y = atom_getfloat(argv + i * 2 + 2);
                nvgLineTo(nvg, x, y);
            }

            nvgStrokeWidth(nvg, strokeWidth);
            nvgStroke(nvg);
            break;
        }
        case hash("lua_fill_all"): {
            auto const bounds = getLocalBounds();
            auto const outlineColour = isSelected ? cnv->selectedOutlineCol : cnv->objectOutlineCol;

            nvgDrawRoundedRect(nvg, bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), nvgColour(currentColour), outlineColour, Corners::objectCornerRadius);
            break;
        }

        case hash("lua_translate"): {
            if (argc >= 2) {
                float const tx = atom_getfloat(argv);
                float const ty = atom_getfloat(argv + 1);
                nvgTranslate(nvg, tx, ty);
            }
            break;
        }
        case hash("lua_scale"): {
            if (argc >= 2) {
                float const sx = atom_getfloat(argv);
                float const sy = atom_getfloat(argv + 1);
                nvgScale(nvg, sx, sy);
            }
            break;
        }
        case hash("lua_reset_transform"): {
            nvgRestore(nvg);
            nvgSave(nvg);
            break;
        }
        default:
            break;
        }
    }

    // We need to update the framebuffer in a place where the current graphics context is active (for multi-window support, thanks to Alex for figuring that out)
    // but we also need to be outside of calls to beginFrame/endFrame
    // So we have this separate callback function that occurs after activating the GPU context, but before starting the frame
    void updateFramebuffers(NVGcontext* nvg) override
    {
        LuaGuiMessage guiMessage;
        for (auto& [layer, layerQueue] : guiMessageQueue) {
            if (layer == -1) // non-layer related messages
            {
                while (layerQueue.try_dequeue(guiMessage)) {
                    handleGuiMessage(nvg, layer, guiMessage.symbol, guiMessage.size, guiMessage.data.data());
                }
                continue;
            }

            while (layerQueue.try_dequeue(guiMessage)) {
                guiCommandBuffer[layer].add(guiMessage);
            }

            auto const* startMesage = pd->generateSymbol("lua_start_paint");
            auto const* endMessage = pd->generateSymbol("lua_end_paint");

            int startIdx = -1, endIdx = -1;
            bool updateScene = false;
            for (int i = guiCommandBuffer[layer].size() - 1; i >= 0; i--) {
                if (guiCommandBuffer[layer][i].symbol == startMesage)
                    startIdx = i;
                if (guiCommandBuffer[layer][i].symbol == endMessage)
                    endIdx = i + 1;

                if (startIdx != -1 && endIdx != -1) {
                    updateScene = true;
                    break;
                }
            }

            if (updateScene) {
                if (endIdx > startIdx) {
                    for (int i = startIdx; i < endIdx; i++) {
                        handleGuiMessage(nvg, layer, guiCommandBuffer[layer][i].symbol, guiCommandBuffer[layer][i].size, guiCommandBuffer[layer][i].data.data());
                    }
                }
                guiCommandBuffer[layer].erase(guiCommandBuffer[layer].begin(), guiCommandBuffer[layer].begin() + endIdx);
            }

            if (isSelected != object->isSelected() || !framebuffers[layer].isValid()) {
                isSelected = object->isSelected();
                sendRepaintMessage();
            }
        }
    }

    static void drawCallback(void* target, int const layer, t_symbol* sym, int argc, t_atom* argv)
    {
        for (auto* object : allDrawTargets[static_cast<t_pdlua*>(target)]) {
            object->guiMessageQueue[layer].enqueue({ sym, argc, argv });
        }
    }

    void receiveObjectMessage(hash32 const symbol, SmallArray<pd::Atom> const& atoms) override
    {
        if (symbol == hash("open_textfile") && atoms.size() >= 1) {
            openTextEditor(File(atoms[0].toString()));
        }
    }

    void openTextEditor(File fileToOpen)
    {
        if (fileToOpen.getFileName() == "pd.lua") {
            return;
        }

        if (textEditor) {
            textEditor->toFront(true);
            return;
        }

        if (cnv->editor->openTextEditors.contains(ptr))
            return;

        auto onClose = [_this = SafePointer(this), this, fileToOpen](String const& newText, bool const hasChanged) {
            if (!_this)
                return;

            if (!hasChanged) {
                cnv->editor->openTextEditors.remove_all(ptr);
                textEditor.reset(nullptr);
                return;
            }

            Dialogs::showAskToSaveDialog(
                &saveDialog, textEditor.get(), "", [_this = SafePointer(this), newText, fileToOpen](int const result) mutable {
                    if (!_this)
                        return;
                    if (result == 2) {
                        fileToOpen.replaceWithText(newText);
                        if (auto pdlua = _this->ptr.get<t_pd>()) {
                            pd_typedmess(_this->pdluaxSymbol->s_thing, gensym("reload"), 0, nullptr);
                            // Recreate this object
                            if (auto patch = _this->cnv->patch.getPointer()) {
                                pd::Interface::recreateTextObject(patch.get(), pdlua.cast<t_gobj>());
                            }
                        }
                        _this->cnv->editor->openTextEditors.remove_all(_this->ptr);
                        _this->textEditor.reset(nullptr);
                        _this->cnv->synchronise();
                    }
                    if (result == 1) {
                        _this->cnv->editor->openTextEditors.remove_all(_this->ptr);
                        _this->textEditor.reset(nullptr);
                    }
                },
                15, false);
        };

        auto onSave = [_this = SafePointer(this), this, fileToOpen](String const& newText) {
            if (!_this)
                return;

            fileToOpen.replaceWithText(newText);
            if (auto pdlua = ptr.get<t_pd>()) {
                pd_typedmess(pdluaxSymbol->s_thing, gensym("reload"), 0, nullptr);
            }
            sendRepaintMessage();
        };

        auto const scaleFactor = getApproximateScaleFactorForComponent(cnv->editor);
        textEditor.reset(Dialogs::showTextEditorDialog(fileToOpen.loadFileAsString(), "lua: " + getText(), onClose, onSave, scaleFactor, true));

        if (textEditor)
            cnv->editor->openTextEditors.add_unique(ptr);
    }
};

#ifdef ENABLE_LUAJIT

/* ========================================================================= */
/* LuaJitGfx — NanoVG vtable wrappers for pdluajit graphics                 */
/* ========================================================================= */

namespace LuaJitGfx {

static void vt_save(void* ctx)
{
    nvgSave(static_cast<NVGcontext*>(ctx));
}

static void vt_restore(void* ctx)
{
    nvgRestore(static_cast<NVGcontext*>(ctx));
}

static void vt_reset_transform(void* ctx)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    nvgRestore(nvg);
    nvgSave(nvg);
}

static void vt_set_color(void* ctx, int r, int g, int b, int a)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    NVGcolor c = nvgRGBA(static_cast<unsigned char>(r), static_cast<unsigned char>(g),
                          static_cast<unsigned char>(b), static_cast<unsigned char>(a));
    nvgFillColor(nvg, c);
    nvgStrokeColor(nvg, c);
}

static void vt_stroke_width(void* ctx, float width)
{
    nvgStrokeWidth(static_cast<NVGcontext*>(ctx), width);
}

static void vt_fill_rect(void* ctx, float x, float y, float w, float h)
{
    nvgFillRect(static_cast<NVGcontext*>(ctx), x, y, w, h);
}

static void vt_fill_rounded_rect(void* ctx, float x, float y, float w, float h, float radius)
{
    nvgFillRoundedRect(static_cast<NVGcontext*>(ctx), x, y, w, h, radius);
}

static void vt_fill_ellipse(void* ctx, float x, float y, float w, float h)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    nvgBeginPath(nvg);
    nvgEllipse(nvg, x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f);
    nvgFill(nvg);
}

static void vt_stroke_rect(void* ctx, float x, float y, float w, float h)
{
    nvgStrokeRect(static_cast<NVGcontext*>(ctx), x, y, w, h);
}

static void vt_stroke_rounded_rect(void* ctx, float x, float y, float w, float h, float radius)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    nvgBeginPath(nvg);
    nvgRoundedRect(nvg, x, y, w, h, radius);
    nvgStroke(nvg);
}

static void vt_stroke_ellipse(void* ctx, float x, float y, float w, float h)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    nvgBeginPath(nvg);
    nvgEllipse(nvg, x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f);
    nvgStroke(nvg);
}

static void vt_draw_line(void* ctx, float x1, float y1, float x2, float y2)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    nvgBeginPath(nvg);
    nvgMoveTo(nvg, x1, y1);
    nvgLineTo(nvg, x2, y2);
    nvgStroke(nvg);
}

static void vt_begin_path(void* ctx)
{
    nvgBeginPath(static_cast<NVGcontext*>(ctx));
}

static void vt_move_to(void* ctx, float x, float y)
{
    nvgMoveTo(static_cast<NVGcontext*>(ctx), x, y);
}

static void vt_line_to(void* ctx, float x, float y)
{
    nvgLineTo(static_cast<NVGcontext*>(ctx), x, y);
}

static void vt_quad_to(void* ctx, float cx, float cy, float x, float y)
{
    nvgQuadTo(static_cast<NVGcontext*>(ctx), cx, cy, x, y);
}

static void vt_bezier_to(void* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
    nvgBezierTo(static_cast<NVGcontext*>(ctx), c1x, c1y, c2x, c2y, x, y);
}

static void vt_close_path(void* ctx)
{
    nvgClosePath(static_cast<NVGcontext*>(ctx));
}

static void vt_fill(void* ctx)
{
    nvgFill(static_cast<NVGcontext*>(ctx));
}

static void vt_stroke(void* ctx)
{
    nvgStroke(static_cast<NVGcontext*>(ctx));
}

static void vt_translate(void* ctx, float tx, float ty)
{
    nvgTranslate(static_cast<NVGcontext*>(ctx), tx, ty);
}

static void vt_scale(void* ctx, float sx, float sy)
{
    nvgScale(static_cast<NVGcontext*>(ctx), sx, sy);
}

static void vt_fill_all(void* ctx, float w, float h,
                         unsigned int fill_rgba, unsigned int outline_rgba,
                         float corner_radius)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    NVGcolor fillCol;
    fillCol.rgba32 = fill_rgba;
    NVGcolor outlineCol;
    outlineCol.rgba32 = outline_rgba;
    nvgDrawRoundedRect(nvg, 0, 0, w, h, fillCol, outlineCol, corner_radius);
}

static void vt_draw_text(void* ctx, char const* text, float x, float y,
                          float max_width, float font_height)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    nvgFontSize(nvg, font_height);
    nvgTextAlign(nvg, NVG_ALIGN_TOP | NVG_ALIGN_LEFT);
    nvgBeginPath(nvg);
    nvgTextBox(nvg, x, y, max_width, text, nullptr);
}

static int vt_create_image(void* ctx, const char* filename, int imageFlags)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    auto file = File(String::fromUTF8(filename));
    if (!file.existsAsFile())
        return 0;

    auto img = ImageFileFormat::loadFrom(file);
    if (!img.isValid())
        return 0;

    img = img.convertedToFormat(Image::ARGB);
    Image::BitmapData bmp(img, Image::BitmapData::readOnly);

    return nvgCreateImageARGB_sRGB(nvg, img.getWidth(), img.getHeight(),
                                    imageFlags, bmp.data);
}

static void vt_delete_image(void* ctx, int image)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    nvgDeleteImage(nvg, image);
}

static void vt_draw_image(void* ctx, int image, float x, float y, float w, float h, float alpha)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    NVGpaint imgPaint = nvgImagePattern(nvg, x, y, w, h, 0, image, alpha);
    nvgBeginPath(nvg);
    nvgRect(nvg, x, y, w, h);
    nvgFillPaint(nvg, imgPaint);
    nvgFill(nvg);
}

static void vt_set_fill_paint_image_pattern(void* ctx, float ox, float oy, float ex, float ey, float angle, int image, float alpha)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    NVGpaint imgPaint = nvgImagePattern(nvg, ox, oy, ex, ey, angle, image, alpha);
    nvgFillPaint(nvg, imgPaint);
}

static int vt_create_image_rgba(void* ctx, int w, int h, int imageFlags, const unsigned char* data)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    return nvgCreateImageARGB(nvg, w, h, imageFlags, data);
}

static void vt_update_image(void* ctx, int image, const unsigned char* data)
{
    auto* nvg = static_cast<NVGcontext*>(ctx);
    nvgUpdateImage(nvg, image, data);
}

static inline const pdluajit_gfx_vtable vtable = {
    vt_save,
    vt_restore,
    vt_reset_transform,
    vt_set_color,
    vt_stroke_width,
    vt_fill_rect,
    vt_fill_rounded_rect,
    vt_fill_ellipse,
    vt_stroke_rect,
    vt_stroke_rounded_rect,
    vt_stroke_ellipse,
    vt_draw_line,
    vt_begin_path,
    vt_move_to,
    vt_line_to,
    vt_quad_to,
    vt_bezier_to,
    vt_close_path,
    vt_fill,
    vt_stroke,
    vt_translate,
    vt_scale,
    vt_fill_all,
    vt_draw_text,
    vt_create_image,
    vt_delete_image,
    vt_draw_image,
    vt_set_fill_paint_image_pattern,
    vt_create_image_rgba,
    vt_update_image
};

} // namespace LuaJitGfx

/* ========================================================================= */
/* LuaJitObject — GUI object class for pdluajit scripts with paint()         */
/* ========================================================================= */

class LuaJitObject final : public ObjectBase
    , private Value::Listener {

    bool isSelected = false;
    Value zoomScale;
    std::unique_ptr<Component> textEditor;
    std::unique_ptr<Dialog> saveDialog;

    NVGFramebuffer framebuffer;
    bool fbDirty = true;

public:
    LuaJitObject(pd::WeakReference obj, Object* parent)
        : ObjectBase(obj, parent)
    {
        object->editor->nvgSurface.addBufferedObject(this);
        parentHierarchyChanged();
    }

    ~LuaJitObject() override
    {
        zoomScale.removeListener(this);
        object->editor->nvgSurface.removeBufferedObject(this);
    }

    void parentHierarchyChanged() override
    {
        auto const* topLevelCanvas = cnv;
        while (auto const* nextCanvas = topLevelCanvas->findParentComponentOfClass<Canvas>()) {
            topLevelCanvas = nextCanvas;
        }
        zoomScale.referTo(topLevelCanvas->zoomScale);
        zoomScale.addListener(this);
        fbDirty = true;
    }

    Rectangle<int> getPdBounds() override
    {
        if (auto gobj = ptr.get<t_gobj>()) {
            auto* patch = cnv->patch.getRawPointer();
            int x = 0, y = 0, w = 0, h = 0;
            pd::Interface::getObjectBounds(patch, gobj.get(), &x, &y, &w, &h);
            return Rectangle<int>(x, y,
                pdluajit_get_width(gobj.get()) + 2,
                pdluajit_get_height(gobj.get()) + 2);
        }
        return {};
    }

    void setPdBounds(Rectangle<int> const b) override
    {
        if (auto gobj = ptr.get<t_gobj>()) {
            auto* patch = object->cnv->patch.getRawPointer();
            pd::Interface::moveObject(patch, gobj.get(), b.getX(), b.getY());
            pdluajit_set_size(gobj.get(), b.getWidth() - 2, b.getHeight() - 2);
        }
        fbDirty = true;
    }

    void updateSizeProperty() override { }

    bool hideInGraph() override { return false; }

    void getMenuOptions(PopupMenu& menu) override
    {
        menu.addItem("Open lua editor", [_this = SafePointer(this)] {
            if (!_this)
                return;
            if (auto obj = _this->ptr.get<t_pd>()) {
                _this->pd->sendDirectMessage(obj.get(), "menu-open", {});
            }
        });
        menu.addItem("Reload lua object", [_this = SafePointer(this)] {
            if (!_this)
                return;
            _this->cnv->editor->sidebar->hideParameters();
            if (auto pdobj = _this->ptr.get<t_pd>()) {
                pd_typedmess(pdobj.get(), gensym("reload"), 0, nullptr);
                if (auto patch = _this->cnv->patch.getPointer()) {
                    pd::Interface::recreateTextObject(patch.get(), pdobj.cast<t_gobj>());
                }
                _this->cnv->synchronise();
            }
        });
    }

    void mouseDown(MouseEvent const& e) override
    {
        pd->enqueueFunctionAsync<t_pd>(ptr, [x = e.x, y = e.y](t_pd* obj) {
            sys_lock();
            pdluajit_mouse_down(obj, x, y);
            sys_unlock();
        });
    }

    void mouseUp(MouseEvent const& e) override
    {
        pd->enqueueFunctionAsync<t_pd>(ptr, [x = e.x, y = e.y](t_pd* obj) {
            sys_lock();
            pdluajit_mouse_up(obj, x, y);
            sys_unlock();
        });
    }

    void mouseMove(MouseEvent const& e) override
    {
        pd->enqueueFunctionAsync<t_pd>(ptr, [x = e.x, y = e.y](t_pd* obj) {
            sys_lock();
            pdluajit_mouse_move(obj, x, y);
            sys_unlock();
        });
    }

    void mouseDrag(MouseEvent const& e) override
    {
        pd->enqueueFunctionAsync<t_pd>(ptr, [x = e.x, y = e.y](t_pd* obj) {
            sys_lock();
            pdluajit_mouse_drag(obj, x, y);
            sys_unlock();
        });
    }

    void render(NVGcontext* nvg) override
    {
        NVGScopedState scopedState(nvg);

        auto scale = nvgCurrentPixelScale(nvg) * getValue<float>(zoomScale);
        nvgScale(nvg, 1.0f / scale, 1.0f / scale);
        nvgTransformQuantize(nvg);

        framebuffer.render(nvg, Rectangle<int>(
            static_cast<int>(std::ceil(getWidth() * scale)),
            static_cast<int>(std::ceil(getHeight() * scale))));
    }

    void valueChanged(Value& v) override
    {
        fbDirty = true;
    }

    void resized() override
    {
        fbDirty = true;

        pd->enqueueFunctionAsync<t_pd>(ptr, [w = getWidth() - 2, h = getHeight() - 2](t_pd* obj) {
            t_atom args[2];
            SETFLOAT(&args[0], w);
            SETFLOAT(&args[1], h);
            sys_lock();
            pd_typedmess(obj, gensym("lua_resized"), 2, args);
            sys_unlock();
        });
    }

    void lookAndFeelChanged() override
    {
        fbDirty = true;
    }

    void updateFramebuffers(NVGcontext* nvg) override
    {
        bool const selChanged = (isSelected != object->isSelected());
        if (selChanged) {
            isSelected = object->isSelected();
            fbDirty = true;
        }

        if (!fbDirty && framebuffer.isValid())
            return;

        fbDirty = false;

        if (getLocalBounds().isEmpty())
            return;

        auto const pixelScale = nvgCurrentPixelScale(nvg);
        auto const zoom = getValue<float>(zoomScale);
        auto const imageScale = zoom * pixelScale;
        int const imageWidth = static_cast<int>(std::ceil(getWidth() * imageScale));
        int const imageHeight = static_cast<int>(std::ceil(getHeight() * imageScale));
        if (!imageWidth || !imageHeight)
            return;

        auto const outlineNvg = isSelected ? cnv->selectedOutlineCol : cnv->objectOutlineCol;
        unsigned int outline_rgba = outlineNvg.rgba32;

        framebuffer.bind(nvg, imageWidth, imageHeight);

        nvgViewport(0, 0, imageWidth, imageHeight);
        nvgClear(nvg);
        nvgBeginFrame(nvg, getWidth() * zoom, getHeight() * zoom, pixelScale);
        nvgScale(nvg, zoom, zoom);
        nvgSave(nvg);

        pd->setThis();
        sys_lock();
        pdluajit_paint(ptr.getRawUnchecked<void>(), nvg, &LuaJitGfx::vtable,
                        getWidth(), getHeight(), outline_rgba);
        sys_unlock();

        nvgGlobalScissor(nvg, 0, 0, getWidth() * imageScale, getHeight() * imageScale);
        nvgEndFrame(nvg);
        NVGFramebuffer::unbind();
        repaint();
    }

    void receiveObjectMessage(hash32 const symbol, SmallArray<pd::Atom> const& atoms) override
    {
        switch (symbol) {
        case hash("lua_repaint"): {
            fbDirty = true;
            repaint();
            break;
        }
        case hash("lua_resized"): {
            if (atoms.size() >= 2) {
                MessageManager::callAsync([_object = SafePointer(object)] {
                    if (_object)
                        _object->updateBounds();
                });
            }
            break;
        }
        case hash("open_textfile"): {
            if (atoms.size() >= 1)
                openTextEditor(File(atoms[0].toString()));
            break;
        }
        default:
            break;
        }
    }

    void openTextEditor(File fileToOpen)
    {
        if (textEditor) {
            textEditor->toFront(true);
            return;
        }

        if (cnv->editor->openTextEditors.contains(ptr))
            return;

        auto onClose = [_this = SafePointer(this), this, fileToOpen](String const& newText, bool const hasChanged) {
            if (!_this)
                return;

            if (!hasChanged) {
                cnv->editor->openTextEditors.remove_all(ptr);
                textEditor.reset(nullptr);
                return;
            }

            Dialogs::showAskToSaveDialog(
                &saveDialog, textEditor.get(), "", [_this = SafePointer(this), newText, fileToOpen](int const result) mutable {
                    if (!_this)
                        return;
                    if (result == 2) {
                        fileToOpen.replaceWithText(newText);
                        if (auto pdobj = _this->ptr.get<t_pd>()) {
                            pd_typedmess(pdobj.get(), gensym("reload"), 0, nullptr);
                            if (auto patch = _this->cnv->patch.getPointer()) {
                                pd::Interface::recreateTextObject(patch.get(), pdobj.cast<t_gobj>());
                            }
                        }
                        _this->cnv->editor->openTextEditors.remove_all(_this->ptr);
                        _this->textEditor.reset(nullptr);
                        _this->cnv->synchronise();
                    }
                    if (result == 1) {
                        _this->cnv->editor->openTextEditors.remove_all(_this->ptr);
                        _this->textEditor.reset(nullptr);
                    }
                },
                15, false);
        };

        auto onSave = [_this = SafePointer(this), this, fileToOpen](String const& newText) {
            if (!_this)
                return;
            fileToOpen.replaceWithText(newText);
            if (auto pdobj = ptr.get<t_pd>()) {
                pd_typedmess(pdobj.get(), gensym("reload"), 0, nullptr);
            }
            fbDirty = true;
            repaint();
        };

        auto const scaleFactor = getApproximateScaleFactorForComponent(cnv->editor);
        textEditor.reset(Dialogs::showTextEditorDialog(fileToOpen.loadFileAsString(), "luajit: " + getText(), onClose, onSave, scaleFactor, true));

        if (textEditor)
            cnv->editor->openTextEditors.add_unique(ptr);
    }
};

#endif // ENABLE_LUAJIT

// A non-GUI Lua object, that we would still like to have clickable for opening the editor
class LuaTextObject final : public TextObjectBase {
public:
    std::unique_ptr<Component> textEditor;
    std::unique_ptr<Dialog> saveDialog;
    t_symbol* pdluaxSymbol;

    LuaTextObject(pd::WeakReference ptr, Object* object)
        : TextObjectBase(ptr, object)
    {
        libpd_set_instance(&pd_maininstance);
        pdluaxSymbol = gensym("pdluax");
        pd->setThis();
    }

    void mouseDown(MouseEvent const& e) override
    {
        if (!e.mods.isLeftButtonDown())
            return;

        if (getValue<bool>(object->locked)) {
            auto objectText = getText();
            if (objectText != "pdlua" && objectText != "pdluax" && objectText != "pdluajit") {
                sendMessage("menu-open");
            }
        }
    }

    void receiveObjectMessage(hash32 const symbol, SmallArray<pd::Atom> const& atoms) override
    {
        if (symbol == hash("open_textfile") && atoms.size() >= 1) {
            openTextEditor(File(atoms[0].toString()));
        }
    }

    void getMenuOptions(PopupMenu& menu) override
    {
        auto objectText = getText();
        if (objectText != "pdlua" && objectText != "pdluax" && objectText != "pdluajit") {
            menu.addItem("Open lua editor", [_this = SafePointer(this)] {
                if (!_this)
                    return;
                _this->sendMessage("menu-open");
            });

            menu.addItem("Reload lua object", [_this = SafePointer(this)] {
                if (!_this)
                    return;
                if (auto pdlua = _this->ptr.get<t_pd>()) {
                    auto objectText = _this->getText();
                    if (objectText.startsWith("pdluajit")) {
                        pd_typedmess(pdlua.get(), gensym("reload"), 0, nullptr);
                    } else {
                        // Reload the lua script
                        pd_typedmess(_this->pdluaxSymbol->s_thing, gensym("reload"), 0, nullptr);
                    }

                    // Recreate this object
                    if (auto patch = _this->cnv->patch.getPointer()) {
                        pd::Interface::recreateTextObject(patch.get(), pdlua.cast<t_gobj>());
                    }
                }
            });
        }
    }

    void openTextEditor(File fileToOpen)
    {
        if (fileToOpen == ProjectInfo::appDataDir.getChildFile("Extra").getChildFile("pdlua").getChildFile("pd.lua")) {
            return;
        }

        if (textEditor) {
            textEditor->toFront(true);
            return;
        }

        if (cnv->editor->openTextEditors.contains(ptr))
            return;

        auto onClose = [_this = SafePointer(this), this, fileToOpen](String const& newText, bool const hasChanged) {
            if (!_this)
                return;
            if (!hasChanged) {
                cnv->editor->openTextEditors.remove_all(ptr);
                textEditor.reset(nullptr);
                return;
            }

            Dialogs::showAskToSaveDialog(
                &saveDialog, textEditor.get(), "", [this, newText, fileToOpen](int const result) mutable {
                    if (result == 2) {
                        fileToOpen.replaceWithText(newText);
                        if (auto pdlua = ptr.get<t_pd>()) {
                            sys_lock();
                            auto objectText = getText();
                            if (objectText.startsWith("pdluajit")) {
                                pd_typedmess(pdlua.get(), gensym("reload"), 0, nullptr);
                            } else {
                                pd_typedmess(pdluaxSymbol->s_thing, gensym("reload"), 0, nullptr);
                            }
                            sys_unlock();
                            // Recreate this object
                            if (auto patch = cnv->patch.getPointer()) {
                                pd::Interface::recreateTextObject(patch.get(), pdlua.cast<t_gobj>());
                            }
                        }
                        cnv->editor->openTextEditors.remove_all(ptr);
                        textEditor.reset(nullptr);
                        cnv->synchronise();
                    }
                    if (result == 1) {
                        cnv->editor->openTextEditors.remove_all(ptr);
                        textEditor.reset(nullptr);
                    }
                },
                15, false);
        };

        auto onSave = [_this = SafePointer(this), this, fileToOpen](String const& newText) {
            if (!_this)
                return;
            fileToOpen.replaceWithText(newText);
            if (auto pdlua = ptr.get<t_pd>()) {
                sys_lock();
                auto objectText = getText();
                if (objectText.startsWith("pdluajit")) {
                    pd_typedmess(pdlua.get(), gensym("reload"), 0, nullptr);
                } else if(pdluaxSymbol->s_thing) {
                    pd_typedmess(pdluaxSymbol->s_thing, gensym("reload"), 0, nullptr);
                }
                sys_unlock();
            }
        };

        auto const scaleFactor = getApproximateScaleFactorForComponent(cnv->editor);
        textEditor.reset(Dialogs::showTextEditorDialog(fileToOpen.loadFileAsString(), "lua: " + getText(), onClose, onSave, scaleFactor, true));

        if (textEditor)
            cnv->editor->openTextEditors.add_unique(ptr);
    }
};
