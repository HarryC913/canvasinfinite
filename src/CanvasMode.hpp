#pragma once

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>

// CCanvasMode is the architectural spine of the plugin: the single authority
// that decides whether a given workspace is currently in infinite-canvas mode.
//
// The contract that guarantees non-destructiveness:
//   * Every input hook / render hook / dispatcher gates on isCanvas(ws).
//   * If no workspace is in canvas mode, the plugin changes NOTHING about
//     Hyprland's behaviour. enter()/leave() are the only mutating ops, and
//     leave() must fully restore the workspace to its pre-canvas state.
//
// Camera = Approach B (see FINDINGS.md): canvas windows are floated so the tiling
// algo stops re-positioning them, then pan moves their real positions. enter()
// saves each window's pre-canvas floating-state + geometry; leave() restores it.
class CCanvasMode {
  public:
    bool isCanvas(const PHLWORKSPACE& ws) const;

    // The master switch, driven by the canvas:toggle dispatcher (keybind).
    void toggle(const PHLWORKSPACE& ws);

    void enter(const PHLWORKSPACE& ws);
    void leave(const PHLWORKSPACE& ws);

    // Pan the viewport camera by a canvas-space delta (animated).
    void pan(const PHLWORKSPACE& ws, const Vector2D& delta);

    // Float a window that opened while its workspace is in canvas mode, so it joins
    // the canvas (pannable) instead of disrupting the layout as a lone tiled window.
    void onWindowOpened(const PHLWINDOW& w);

    // Enter/leave canvas on the active workspace of EVERY monitor at once.
    void toggleAllMonitors();
    // Pan every monitor's active canvas workspace by the same delta (both monitors move together).
    void panAllActive(const Vector2D& delta);

    bool anyActive() const { return !m_canvasWorkspaces.empty(); }

    // Native (pre-overview) monitor scale captured when canvas turned on. The DPI-block
    // hook clamps the client-facing fractional scale up to this during canvas mode, so the
    // overview's lowered monitor scale doesn't make apps (Steam/GTK/Qt) reflow their UI.
    float appScale() const { return m_appScale; }

    // Focus + centre the Nth canvas window (1-based), ordered largest-area first across all
    // canvas workspaces. Drives the SUPER+number window-switcher (via the canvas:jump dispatcher).
    void jumpToWindow(int n);

  private:
    // Per-window snapshot captured on enter(), replayed on leave().
    struct SSavedWin {
        PHLWINDOWREF win;
        bool         wasFloating = false;
        Vector2D     pos;
        Vector2D     size;
    };

    // Remembered canvas (floating) geometry per window, so a window returns to where
    // it was on the canvas across toggle off/on. Survives leave() (unlike m_saved).
    struct SCanvasGeom {
        PHLWINDOWREF win;
        Vector2D     pos;
        Vector2D     size;
    };

    // Keyed by workspace ID (stable across pointer churn).
    std::unordered_set<WORKSPACEID>                           m_canvasWorkspaces;
    std::unordered_map<WORKSPACEID, std::vector<SSavedWin>>   m_saved;     // tiled-restore data, erased on leave()
    std::unordered_map<WORKSPACEID, std::vector<SCanvasGeom>> m_canvasGeom; // remembered canvas layout, persists

    float m_appScale = 1.0F; // native monitor scale captured when canvas turned on (for the DPI-block)
};

inline UP<CCanvasMode> g_canvas;
