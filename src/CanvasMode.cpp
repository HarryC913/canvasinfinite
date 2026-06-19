#include "CanvasMode.hpp"
#include "globals.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

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

    // Capture the native (pre-overview) monitor scale so the DPI-block hook can clamp the
    // client-facing fractional scale up to it while canvas is on. Reset when starting fresh
    // (no canvas ws yet), then take the max across every monitor we turn canvas on for.
    if (m_canvasWorkspaces.empty())
        m_appScale = 1.0F;
    if (const auto MON = ws->m_monitor.lock())
        m_appScale = std::max(m_appScale, MON->m_scale);

    m_canvasWorkspaces.insert(ws->m_id);

    auto& saved = m_saved[ws->m_id];
    saved.clear();

    // Pass 1: snapshot ALL geometry first. Floating one window re-tiles the rest,
    // which would shift positions mid-loop (windows ended up overlapping).
    for (const auto& w : g_pCompositor->m_windows) {
        if (!onWorkspace(w, ws))
            continue;
        if (w->m_fullscreenState.internal != FSMODE_NONE)
            continue; // leave fullscreen windows alone — floating them misbehaves
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
    const auto t = w->layoutTarget();
    if (!t)
        return;
    g_layoutManager->changeFloatingMode(t);

    // Drop it in the centre of the current view (the monitor's middle in canvas coords)
    // instead of its tiled spot, unless disabled.
    if (cfg::spawnCenter())
        if (const auto mon = w->m_monitor.lock()) {
            const Vector2D size = w->m_realSize->goal();
            g_layoutManager->setTargetGeom(CBox{mon->middle() - size / 2.0, size}, t);
        }
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

std::vector<PHLWINDOW> CCanvasMode::orderedWindows() const {
    std::vector<PHLWINDOW> v;
    for (const auto& w : g_pCompositor->m_windows)
        if (w && w->m_isMapped && w->m_workspace && m_canvasWorkspaces.contains(w->m_workspace->m_id))
            v.push_back(w); // m_windows is creation order → this is the "age" order as-is

    switch (cfg::jumpOrder()) {
        case 1: // spatial: left-to-right, then top-to-bottom
            std::sort(v.begin(), v.end(), [](const PHLWINDOW& a, const PHLWINDOW& b) {
                const auto pa = a->m_realPosition->goal(), pb = b->m_realPosition->goal();
                return pa.x != pb.x ? pa.x < pb.x : pa.y < pb.y;
            });
            break;
        case 2: // age: oldest first (m_windows order) — leave as collected
            break;
        default: // 0 = largest-area first
            std::sort(v.begin(), v.end(), [](const PHLWINDOW& a, const PHLWINDOW& b) {
                const auto sa = a->m_realSize->goal(), sb = b->m_realSize->goal();
                return sa.x * sa.y > sb.x * sb.y;
            });
            break;
    }
    return v;
}

void CCanvasMode::jumpToWindow(int n) {
    if (m_canvasWorkspaces.empty() || n < 1)
        return;

    const auto wins = orderedWindows();
    if (n > (int)wins.size())
        return;

    const auto w   = wins[n - 1];
    const auto mon = w->m_monitor.lock();
    if (!mon)
        return;

    // Bring it into view by panning its whole workspace so the window's centre lands at the
    // monitor centre. moveTarget sets the (animated) goal and we DON'T snap value→goal here, so
    // every window eases to its new spot together — the viewport visibly slides over, as if the
    // canvas were dragging itself to the target. (Grab-pan snaps for 1:1; the jump wants the ease.)
    const Vector2D delta = mon->middle() - (w->m_realPosition->goal() + w->m_realSize->goal() / 2.0);
    for (const auto& o : g_pCompositor->m_windows) {
        if (!o || !o->m_isMapped || !o->m_workspace || o->m_workspace->m_id != w->m_workspace->m_id)
            continue;
        if (const auto t = o->layoutTarget())
            g_layoutManager->moveTarget(delta, t);
    }

    // Move focus (and the cursor) to it. Reuse the built-in focuswindow dispatcher so focus
    // state, history and the active border all update the same way a normal focus would.
    g_pCompositor->warpCursorTo(mon->middle());
    if (g_pKeybindManager && g_pKeybindManager->m_dispatchers.contains("focuswindow"))
        g_pKeybindManager->m_dispatchers["focuswindow"](std::format("address:0x{:x}", (uintptr_t)w.get()));
}

// --- named layout persistence -----------------------------------------------------------
static std::string layoutDir() {
    if (const char* s = getenv("XDG_STATE_HOME"); s && *s)
        return std::string(s) + "/hyprplane";
    const char* h = getenv("HOME");
    return std::string(h ? h : "/tmp") + "/.local/state/hyprplane";
}
static std::string layoutPath(const std::string& name) {
    std::string safe;
    for (const char c : name)
        safe += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    if (safe.empty())
        safe = "default";
    return layoutDir() + "/" + safe + ".layout";
}

// Canvas windows in m_windows (creation) order — stable for occurrence-based identity matching.
static std::vector<PHLWINDOW> canvasWindowsInOrder(const std::unordered_set<WORKSPACEID>& wss) {
    std::vector<PHLWINDOW> v;
    for (const auto& w : g_pCompositor->m_windows)
        if (w && w->m_isMapped && w->m_workspace && wss.contains(w->m_workspace->m_id))
            v.push_back(w);
    return v;
}

void CCanvasMode::saveLayout(const std::string& name) {
    if (m_canvasWorkspaces.empty())
        return;
    std::error_code ec;
    fs::create_directories(layoutDir(), ec);
    std::ofstream f(layoutPath(name), std::ios::trunc);
    if (!f)
        return;

    // Format: count, then per window 4 lines: class, title, occurrence, "x y w h".
    const auto wins = canvasWindowsInOrder(m_canvasWorkspaces);
    f << wins.size() << "\n";
    std::unordered_map<std::string, int> occ;
    for (const auto& w : wins) {
        const std::string cls = w->m_class, title = w->m_title;
        const int         o   = occ[cls + "\x1f" + title]++;
        const Vector2D    p = w->m_realPosition->goal(), s = w->m_realSize->goal();
        f << cls << "\n" << title << "\n" << o << "\n" << (int)p.x << " " << (int)p.y << " " << (int)s.x << " " << (int)s.y << "\n";
    }
}

void CCanvasMode::loadLayout(const std::string& name) {
    if (m_canvasWorkspaces.empty())
        return;
    std::ifstream f(layoutPath(name));
    if (!f)
        return;
    std::string head;
    if (!std::getline(f, head))
        return;
    int n = 0;
    try {
        n = std::stoi(head);
    } catch (...) { return; }

    const auto wins = canvasWindowsInOrder(m_canvasWorkspaces);
    for (int i = 0; i < n; ++i) {
        std::string cls, title, occs, geom;
        if (!std::getline(f, cls) || !std::getline(f, title) || !std::getline(f, occs) || !std::getline(f, geom))
            break;
        int occ = 0;
        try {
            occ = std::stoi(occs);
        } catch (...) {}
        double             x = 0, y = 0, wd = 0, ht = 0;
        std::istringstream(geom) >> x >> y >> wd >> ht;
        if (wd < 1 || ht < 1)
            continue;

        // Match the occ-th currently-open canvas window with this class+title.
        int       seen = 0;
        PHLWINDOW match;
        for (const auto& w : wins)
            if (w->m_class == cls && w->m_title == title) {
                if (seen++ == occ) {
                    match = w;
                    break;
                }
            }
        if (match)
            if (const auto t = match->layoutTarget())
                g_layoutManager->setTargetGeom(CBox{Vector2D(x, y), Vector2D(wd, ht)}, t);
    }
}

void CCanvasMode::gather() {
    if (m_canvasWorkspaces.empty())
        return;
    const auto mon = g_pCompositor->getMonitorFromCursor();
    if (!mon || !isCanvas(mon->m_activeWorkspace))
        return;
    const auto wsid = mon->m_activeWorkspace->m_id;

    // The focused monitor's canvas windows, largest first (placed first → end up behind).
    std::vector<PHLWINDOW> wins;
    for (const auto& w : orderedWindows())
        if (w->m_workspace && w->m_workspace->m_id == wsid)
            wins.push_back(w);
    if (wins.empty())
        return;

    // Centred diagonal cascade so every window lands back in the current view.
    const Vector2D centre = mon->middle();
    const double   step   = 48.0;
    const double   off0   = (wins.size() - 1) / 2.0;
    for (size_t i = 0; i < wins.size(); ++i) {
        const auto t = wins[i]->layoutTarget();
        if (!t)
            continue;
        const Vector2D size = wins[i]->m_realSize->goal();
        const Vector2D pos  = centre - size / 2.0 + Vector2D(((double)i - off0) * step, ((double)i - off0) * step);
        g_layoutManager->setTargetGeom(CBox{pos, size}, t);
        if (wins[i]->m_realPosition)
            wins[i]->m_realPosition->value() = wins[i]->m_realPosition->goal();
    }
}
