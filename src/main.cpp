#include "globals.hpp"
#include "CanvasMode.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/protocols/FractionalScale.hpp>

#include <fstream>
#include <cstdio>
#include <cstdlib>

// Kept alive for the life of the plugin; resetting them unregisters the hooks.
static CHyprSignalListener g_buttonListener;
static CHyprSignalListener g_moveListener;
static CHyprSignalListener g_openListener;

// --- DPI-block ------------------------------------------------------------------
// The overview lowers each monitor's scale so the desktop zooms out. Fractional-scale-
// aware apps (Steam, GTK/Qt) react to the lowered scale by re-rendering their UI at that
// DPI — an ugly reflow. CFractionalScaleProtocol::sendScale is exactly the call that tells
// a client its scale. We hook it and, while canvas is on, clamp the reported scale UP to
// the native scale, so apps keep rendering at native resolution and the compositor just
// downscales the result. Outside canvas mode the hook is a pass-through (gated on anyActive).
static CFunctionHook* g_sendScaleHook = nullptr;
typedef void (*PsendScale)(CFractionalScaleProtocol*, SP<CWLSurfaceResource>, const float&);

static void hkSendScale(CFractionalScaleProtocol* thisptr, SP<CWLSurfaceResource> surf, const float& scale) {
    float s = scale;
    if (g_canvas && g_canvas->anyActive() && s < g_canvas->appScale())
        s = g_canvas->appScale();
    (*(PsendScale)g_sendScaleHook->m_original)(thisptr, surf, s);
}

// Marker file the companion overview script checks: it only ENTERS overview when canvas is
// on (this file exists). We create it when any canvas workspace is active and remove it
// otherwise, so SUPER+CTRL+grave is inert outside canvas mode.
static std::string markerPath() {
    const char* x = getenv("XDG_RUNTIME_DIR");
    return std::string(x ? x : "/tmp") + "/canvasinfinite-active";
}
static void syncCanvasMarker() {
    const auto p = markerPath();
    if (g_canvas && g_canvas->anyActive()) {
        std::ofstream f(p, std::ios::trunc); // opening creates the marker file
    } else
        std::remove(p.c_str());
}

// Grab-to-pan state (middle-mouse or Ctrl+left drag).
static bool     g_grabbing = false;
static Vector2D g_grabLast;

static constexpr uint32_t BTN_LEFT_ID   = 0x110;
static constexpr uint32_t BTN_MIDDLE_ID = 0x112;

static bool modHeld(uint32_t bit) {
    const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
    return KB && (KB->getModifiers() & bit);
}
static bool ctrlHeld() {
    return modHeld(4u); // CTRL bit (same convention as binds)
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// hyprctl dlsym's this from the .so and refuses to load if it doesn't match the
// running Hyprland's hash. Returns the composite hash of the headers we built
// against (commit + aquamarine/hyprutils/... versions).
APICALL EXPORT const char* __hyprland_api_get_hash() {
    return __hyprland_api_get_client_hash();
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO pluginInit(HANDLE handle) {
    PHANDLE   = handle;
    g_canvas  = makeUnique<CCanvasMode>();

    // --- the master switch -------------------------------------------------
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:toggle", [](std::string) -> SDispatchResult {
        g_canvas->toggleAllMonitors();
        syncCanvasMarker(); // let the overview script know canvas is on/off
        HyprlandAPI::addNotification(PHANDLE, g_canvas->anyActive() ? "[canvas] ON" : "[canvas] OFF",
                                     CHyprColor{0.2F, 0.8F, 1.0F, 1.0F}, 1500);
        return {.success = true, .error = ""};
    });

    // --- keyboard / programmatic pan (Phase 2a), GATED on canvas mode ------
    // Binds call e.g.  canvas:pan 0 -200 . Also used to verify panning headless.
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:pan", [](std::string arg) -> SDispatchResult {
        if (!g_canvas->anyActive())
            return {.success = false, .error = "canvas: not in canvas mode"};

        const auto SP = arg.find(' ');
        if (SP == std::string::npos)
            return {.success = false, .error = "canvas:pan expects: <dx> <dy>"};
        try {
            g_canvas->panAllActive(Vector2D{std::stod(arg.substr(0, SP)), std::stod(arg.substr(SP + 1))});
        } catch (...) { return {.success = false, .error = "canvas:pan: bad args"}; }
        return {.success = true, .error = ""};
    });

    // --- SUPER+number: jump to a window in canvas mode, else normal workspace switch ---
    // Bind SUPER+N to `canvas:jump N`. In canvas mode it focuses+centres the Nth-largest
    // window; otherwise it falls back to the stock `workspace N` so switching is unchanged.
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:jump", [](std::string arg) -> SDispatchResult {
        int n = 0;
        try {
            n = std::stoi(arg);
        } catch (...) { return {.success = false, .error = "canvas:jump: expects a number"}; }

        if (g_canvas && g_canvas->anyActive()) {
            g_canvas->jumpToWindow(n);
            return {.success = true, .error = ""};
        }
        if (g_pKeybindManager && g_pKeybindManager->m_dispatchers.contains("workspace"))
            return g_pKeybindManager->m_dispatchers["workspace"](arg);
        return {.success = true, .error = ""};
    });

    // --- grab-to-pan: middle-mouse OR Ctrl+left drag, GATED on canvas mode --
    // Press starts a grab; motion pans the canvas so it follows the cursor (like
    // dragging a map). Only active in canvas mode, so normal clicks are untouched.
    g_buttonListener = Event::bus()->m_events.input.mouse.button.listen(
        [](IPointer::SButtonEvent e, Event::SCallbackInfo& info) {
            if (!g_canvas->anyActive())
                return; // inert outside canvas mode

            const bool panBtn = e.button == BTN_MIDDLE_ID || (e.button == BTN_LEFT_ID && ctrlHeld());
            if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                if (!panBtn)
                    return;
                g_grabbing     = true;
                g_grabLast     = g_pInputManager->getMouseCoordsInternal();
                info.cancelled = true; // swallow press (no middle-paste / ctrl-click select)
            } else if (g_grabbing) {   // release ends the grab
                g_grabbing     = false;
                info.cancelled = true;
            }
        });

    g_moveListener = Event::bus()->m_events.input.mouse.move.listen(
        [](Vector2D, Event::SCallbackInfo& info) {
            if (!g_grabbing)
                return; // don't cancel — let the cursor move so we can track it

            const Vector2D now   = g_pInputManager->getMouseCoordsInternal();
            const Vector2D delta = now - g_grabLast;
            g_grabLast           = now;

            if (g_canvas->anyActive())
                g_canvas->panAllActive(delta); // both monitors pan together
            else
                g_grabbing = false;            // canvas turned off mid-drag
        });

    // A window opened while canvas is on → float it so it joins the canvas.
    g_openListener = Event::bus()->m_events.window.open.listen([](const PHLWINDOW& w) {
        if (g_canvas)
            g_canvas->onWindowOpened(w);
    });

    // --- DPI-block: hook the fractional-scale send so overview doesn't reflow apps -----
    for (const auto& m : HyprlandAPI::findFunctionsByName(PHANDLE, "sendScale")) {
        if (m.demangled.contains("CFractionalScaleProtocol::sendScale")) {
            g_sendScaleHook = HyprlandAPI::createFunctionHook(PHANDLE, m.address, (void*)&hkSendScale);
            break;
        }
    }
    if (g_sendScaleHook)
        g_sendScaleHook->hook();

    HyprlandAPI::addNotification(PHANDLE, "[canvasinfinite] loaded — toggle: SUPER+grave, middle/Ctrl-drag to pan",
                                 CHyprColor{0.2F, 1.0F, 0.4F, 1.0F}, 4000);

    return {"canvasinfinite", "Infinite scroll canvas (Phase 0 proof)", "hcopeland", "0.0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_canvas && g_canvas->anyActive())
        g_canvas->toggleAllMonitors(); // un-float / re-tile windows so unload leaves a clean layout
    std::remove(markerPath().c_str()); // canvas is off now
    if (g_sendScaleHook)
        g_sendScaleHook->unhook();
    g_buttonListener.reset();
    g_moveListener.reset();
    g_openListener.reset();
    g_canvas.reset();
}
