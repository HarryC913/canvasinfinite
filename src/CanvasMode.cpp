#include "CanvasMode.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>

#include <algorithm>


// Compare workspaces by ID — the cursor monitor's active workspace and a window's
// m_workspace are the same logical ws but not always the same shared_ptr.
static bool onWorkspace(const PHLWINDOW& w, const PHLWORKSPACE& ws) {
    return w && w->m_isMapped && w->m_workspace && ws && w->m_workspace->m_id == ws->m_id;
}

bool CCanvasMode::isCanvas(const PHLWORKSPACE& ws) const {
    return ws && m_canvasWorkspaces.contains(ws->m_id);
}

void CCanvasMode::toggle(const PHLWORKSPACE& ws) {
    if (!ws)
        return;
    if (isCanvas(ws))
        leave(ws);
    else
        enter(ws);
}

void CCanvasMode::enter(const PHLWORKSPACE& ws) {
    if (!ws || isCanvas(ws))
        return;

    m_canvasWorkspaces.insert(ws->m_id);

    auto& saved = m_saved[ws->m_id];
    saved.clear();

    // Pass 1: snapshot ALL geometry first. Floating one window re-tiles the rest,
    // which would shift positions mid-loop (windows ended up overlapping).
    for (const auto& w : g_pCompositor->m_windows) {
        if (!onWorkspace(w, ws))
            continue;
        saved.push_back({w, w->m_isFloating, w->m_realPosition->goal(), w->m_realSize->goal()});
    }

    // Pass 2: float each window, then place it where it was on the canvas last time
    // (remembered geometry), or — if we've never seen it — at its tiled spot.
    const auto& remembered = m_canvasGeom[ws->m_id];
    for (const auto& s : saved) {
        const auto w = s.win.lock();
        if (!w)
            continue;
        const auto t = w->layoutTarget();
        if (!t)
            continue;
        if (!s.wasFloating)
            g_layoutManager->changeFloatingMode(t);

        CBox box{s.pos, s.size}; // default: its tiled spot
        for (const auto& g : remembered) {
            if (g.win.lock() == w) {
                box = CBox{g.pos, g.size}; // remembered canvas position
                break;
            }
        }
        g_layoutManager->setTargetGeom(box, t);
    }
}

void CCanvasMode::toggleAllMonitors() {
    // If canvas is on for ANY monitor, turn it off everywhere; otherwise turn it
    // on for every monitor's active workspace. (enter/leave no-op on wrong state.)
    bool anyOn = false;
    for (const auto& mon : g_pCompositor->m_monitors)
        if (mon && isCanvas(mon->m_activeWorkspace))
            anyOn = true;

    for (const auto& mon : g_pCompositor->m_monitors) {
        if (!mon || !mon->m_activeWorkspace)
            continue;
        if (anyOn)
            leave(mon->m_activeWorkspace);
        else
            enter(mon->m_activeWorkspace);
    }
}

void CCanvasMode::onWindowOpened(const PHLWINDOW& w) {
    // A window opened on a canvas workspace: float it so it joins the canvas instead
    // of becoming a lone tiled window that fills the screen. Leaves it at its spawn
    // geometry (its natural spot); next leave() records it into m_canvasGeom.
    if (!w || !w->m_workspace || !isCanvas(w->m_workspace) || w->m_isFloating)
        return;
    if (const auto t = w->layoutTarget())
        g_layoutManager->changeFloatingMode(t);
}

void CCanvasMode::panAllActive(const Vector2D& delta) {
    // Hot path (fires on every pointer motion during a grab-drag): one pass over the
    // window list, moving every window that lives on a canvas workspace. Avoids the
    // old per-monitor × all-windows double scan.
    if (m_canvasWorkspaces.empty() || (delta.x == 0.0 && delta.y == 0.0))
        return;

    for (const auto& w : g_pCompositor->m_windows) {
        if (!w || !w->m_isMapped || !w->m_workspace || !m_canvasWorkspaces.contains(w->m_workspace->m_id))
            continue;
        const auto t = w->layoutTarget();
        if (!t)
            continue;
        g_layoutManager->moveTarget(delta, t);
        // Snap the rendered position straight to the new goal so the window tracks the
        // cursor 1:1 instead of easing behind it (the move animation lag). Direct value
        // assignment = no warp callbacks; the goal moved too, so it holds.
        if (w->m_realPosition)
            w->m_realPosition->value() = w->m_realPosition->goal();
    }
}

void CCanvasMode::pan(const PHLWORKSPACE& ws, const Vector2D& delta) {
    if (!isCanvas(ws))
        return;

    // moveTarget updates the layout target's stored position (not just the animated
    // m_realPosition), so the pan HOLDS instead of snapping back (the jiggle).
    for (const auto& w : g_pCompositor->m_windows) {
        if (!onWorkspace(w, ws))
            continue;
        if (const auto t = w->layoutTarget())
            g_layoutManager->moveTarget(delta, t);
    }
}

void CCanvasMode::leave(const PHLWORKSPACE& ws) {
    if (!ws || !isCanvas(ws))
        return;

    m_canvasWorkspaces.erase(ws->m_id);

    // Remember the current canvas layout (before un-floating moves anything) so the
    // next enter() restores it. Rebuild from the windows actually present → prunes
    // closed windows, captures newly-opened/panned ones.
    auto& geom = m_canvasGeom[ws->m_id];
    geom.clear();
    for (const auto& w : g_pCompositor->m_windows) {
        if (!onWorkspace(w, ws) || !w->m_realPosition || !w->m_realSize)
            continue;
        geom.push_back({w, w->m_realPosition->goal(), w->m_realSize->goal()});
    }

    // Un-float the windows we floated (Hyprland re-tiles them); restore any that
    // were already floating to their original box.
    if (auto it = m_saved.find(ws->m_id); it != m_saved.end()) {
        for (const auto& s : it->second) {
            const auto w = s.win.lock();
            if (!w || !w->m_isMapped)
                continue;
            const auto t = w->layoutTarget();
            if (!t)
                continue;
            if (w->m_isFloating != s.wasFloating)
                g_layoutManager->changeFloatingMode(t);
            if (s.wasFloating)
                g_layoutManager->setTargetGeom(CBox{s.pos, s.size}, t);
        }
        m_saved.erase(it);
    }

    if (const auto MON = ws->m_monitor.lock())
        g_layoutManager->recalculateMonitor(MON);
}
