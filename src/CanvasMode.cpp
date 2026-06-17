#include "CanvasMode.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/helpers/Color.hpp>

#include <hyprgraphics/image/Image.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

// cairo ARGB32 in memory == DRM_FORMAT_ARGB8888 (avoid pulling drm_fourcc.h include paths).
static constexpr uint32_t DRM_FMT_ARGB8888 = 0x34325241;

// Per-app-class icon cache (texture, or null = looked up and none found — don't retry).
// File-static so it survives canvas toggles; built lazily inside the render pass.
static std::unordered_map<std::string, SP<Render::ITexture>> g_iconCache;

static std::string lower(std::string s) {
    for (auto& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

// Resolve a window class to a freedesktop icon NAME via .desktop files (StartupWMClass or
// filename match, reading Icon=). Falls back to the class itself.
static std::string desktopIconName(const std::string& cls) {
    const std::string  lc = lower(cls);
    std::vector<std::string> dirs;
    if (const char* h = getenv("HOME"))
        dirs.push_back(std::string(h) + "/.local/share/applications");
    dirs.push_back("/usr/share/applications");
    dirs.push_back("/usr/local/share/applications");

    std::string weak; // a filename-match icon, used only if no StartupWMClass match found
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec))
            continue;
        for (const auto& e : fs::directory_iterator(d, ec)) {
            if (e.path().extension() != ".desktop")
                continue;
            std::ifstream f(e.path());
            std::string   line, icon, wmclass;
            while (std::getline(f, line)) {
                if (line.rfind("Icon=", 0) == 0)
                    icon = line.substr(5);
                else if (line.rfind("StartupWMClass=", 0) == 0)
                    wmclass = line.substr(15);
            }
            if (icon.empty())
                continue;
            if (!wmclass.empty() && lower(wmclass) == lc)
                return icon; // strongest match
            if (weak.empty() && lower(e.path().stem().string()) == lc)
                weak = icon;
        }
    }
    return weak.empty() ? cls : weak;
}

// Resolve an icon name to a file via the standard hicolor / pixmaps locations (direct path
// probes — fast, no recursive theme scan). Prefers SVG and larger sizes.
static std::string findIconFile(const std::string& name) {
    if (name.empty())
        return "";
    std::error_code ec;
    if (name.front() == '/' && fs::exists(name, ec))
        return name;

    std::vector<std::string> roots;
    if (const char* h = getenv("HOME"))
        roots.push_back(std::string(h) + "/.local/share/icons/hicolor");
    roots.push_back("/usr/share/icons/hicolor");

    static const char* SIZES[] = {"scalable", "512x512", "256x256", "128x128", "96x96", "64x64", "48x48", "32x32"};
    for (const auto& root : roots)
        for (const char* sz : SIZES)
            for (const char* ext : {"svg", "png"}) {
                const std::string p = root + "/" + sz + "/apps/" + name + "." + ext;
                if (fs::exists(p, ec))
                    return p;
            }
    for (const char* ext : {"svg", "png"}) {
        const std::string p = std::string("/usr/share/pixmaps/") + name + "." + ext;
        if (fs::exists(p, ec))
            return p;
    }
    return "";
}

// class -> GL texture, cached. Must be called with the GL context live (render pass).
static SP<Render::ITexture> iconTexture(const std::string& cls) {
    if (cls.empty())
        return nullptr;
    if (const auto it = g_iconCache.find(cls); it != g_iconCache.end())
        return it->second;

    SP<Render::ITexture> tex;
    const std::string    path = findIconFile(desktopIconName(cls));
    if (!path.empty()) {
        Hyprgraphics::CImage img(path, {64.0, 64.0});
        if (img.success())
            if (const auto surf = img.cairoSurface()) {
                cairo_surface_flush(surf->cairo());
                tex = makeShared<Render::GL::CGLTexture>(DRM_FMT_ARGB8888, surf->data(), (uint32_t)surf->stride(),
                                                         surf->size(), true);
            }
    }
    g_iconCache[cls] = tex; // cache even on failure so we don't re-probe every frame
    return tex;
}


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

void CCanvasMode::jumpToWindow(int n) {
    if (m_canvasWorkspaces.empty() || n < 1)
        return;

    // All mapped windows living on a canvas workspace (across every monitor).
    std::vector<PHLWINDOW> wins;
    for (const auto& w : g_pCompositor->m_windows)
        if (w && w->m_isMapped && w->m_workspace && m_canvasWorkspaces.contains(w->m_workspace->m_id))
            wins.push_back(w);
    if (n > (int)wins.size())
        return;

    // Largest-area first, so SUPER+1 is the biggest window, descending.
    std::sort(wins.begin(), wins.end(), [](const PHLWINDOW& a, const PHLWINDOW& b) {
        const auto sa = a->m_realSize->goal();
        const auto sb = b->m_realSize->goal();
        return sa.x * sa.y > sb.x * sb.y;
    });

    const auto w   = wins[n - 1];
    const auto mon = w->m_monitor.lock();
    if (!mon)
        return;

    // Bring it into view: pan its workspace so the window's centre lands at the monitor
    // centre (same snap-to-goal trick as the grab-pan, so it's immediate, not animated-laggy).
    const Vector2D delta = mon->middle() - (w->m_realPosition->goal() + w->m_realSize->goal() / 2.0);
    for (const auto& o : g_pCompositor->m_windows) {
        if (!o || !o->m_isMapped || !o->m_workspace || o->m_workspace->m_id != w->m_workspace->m_id)
            continue;
        if (const auto t = o->layoutTarget()) {
            g_layoutManager->moveTarget(delta, t);
            if (o->m_realPosition)
                o->m_realPosition->value() = o->m_realPosition->goal();
        }
    }

    // Move focus (and the cursor) to it. Reuse the built-in focuswindow dispatcher so focus
    // state, history and the active border all update the same way a normal focus would.
    g_pCompositor->warpCursorTo(mon->middle());
    if (g_pKeybindManager && g_pKeybindManager->m_dispatchers.contains("focuswindow"))
        g_pKeybindManager->m_dispatchers["focuswindow"](std::format("address:0x{:x}", (uintptr_t)w.get()));
}

void CCanvasMode::renderMinimap(const PHLMONITOR& mon, float alpha, const CHyprColor& accent) {
    if (!mon || m_canvasWorkspaces.empty() || alpha <= 0.01F)
        return;

    // Draw via render-pass elements (immediate-mode renderRect doesn't composite in 0.55).
    const auto addRect = [](const CBox& b, const CHyprColor& c, int round) {
        CRectPassElement::SRectData d;
        d.box   = b;
        d.color = c;
        d.round = round;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(d));
    };

    // A SUPER+number label drawn as a 7-segment digit out of small rects (no text textures).
    const auto drawDigit = [&](int digit, const CBox& b, const CHyprColor& col) {
        if (digit < 1 || digit > 9)
            return;
        static const uint8_t SEG[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
        const uint8_t       s  = SEG[digit];
        const double        X = b.x, Y = b.y, W = b.width, H = b.height;
        const double        t  = std::max(1.0, std::min(W, H) * 0.22);
        const double        hh = (H - t) / 2.0;
        if (s & 0x01) addRect(CBox(X, Y, W, t), col, 0);             // a  top
        if (s & 0x02) addRect(CBox(X + W - t, Y, t, hh + t), col, 0); // b  top-right
        if (s & 0x04) addRect(CBox(X + W - t, Y + hh, t, hh + t), col, 0); // c  bottom-right
        if (s & 0x08) addRect(CBox(X, Y + H - t, W, t), col, 0);     // d  bottom
        if (s & 0x10) addRect(CBox(X, Y + hh, t, hh + t), col, 0);    // e  bottom-left
        if (s & 0x20) addRect(CBox(X, Y, t, hh + t), col, 0);        // f  top-left
        if (s & 0x40) addRect(CBox(X, Y + hh, W, t), col, 0);        // g  middle
    };

    // A texture pass element (the app icon).
    const auto addTex = [](const SP<Render::ITexture>& t, const CBox& b, float a) {
        CTexPassElement::SRenderData d;
        d.tex = t;
        d.box = b;
        d.a   = a;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
    };

    // Whole-canvas bounding box: every canvas window + every active viewport, in global coords.
    struct SRect {
        Vector2D    pos, size;
        std::string cls;
    };
    std::vector<SRect> wins;
    Vector2D           bbMin(1e9, 1e9), bbMax(-1e9, -1e9);
    const auto         grow = [&](const Vector2D& p, const Vector2D& s) {
        bbMin.x = std::min(bbMin.x, p.x);
        bbMin.y = std::min(bbMin.y, p.y);
        bbMax.x = std::max(bbMax.x, p.x + s.x);
        bbMax.y = std::max(bbMax.y, p.y + s.y);
    };

    for (const auto& w : g_pCompositor->m_windows) {
        if (!w || !w->m_isMapped || !w->m_workspace || !m_canvasWorkspaces.contains(w->m_workspace->m_id))
            continue;
        const Vector2D p = w->m_realPosition->value(), s = w->m_realSize->value();
        wins.push_back({p, s, w->m_class.empty() ? w->m_initialClass : w->m_class});
        grow(p, s);
    }
    for (const auto& M : g_pCompositor->m_monitors)
        if (M && isCanvas(M->m_activeWorkspace))
            grow(M->m_position, M->m_size);

    if (wins.empty())
        return;
    const Vector2D bb = bbMax - bbMin;
    if (bb.x < 1.0 || bb.y < 1.0)
        return;

    // Largest-area first so the labels match the SUPER+number (canvas:jump) ordering.
    std::sort(wins.begin(), wins.end(),
              [](const SRect& a, const SRect& b) { return a.size.x * a.size.y > b.size.x * b.size.y; });

    // Theme: accent (active border) for windows/viewport; a number colour that contrasts it.
    const double     lum    = 0.3 * accent.r + 0.6 * accent.g + 0.1 * accent.b;
    const CHyprColor numCol = lum > 0.6 ? CHyprColor(0.06, 0.06, 0.08, 0.95F * alpha) : CHyprColor(0.96, 0.97, 1.0, 0.95F * alpha);
    const CHyprColor winCol(accent.r, accent.g, accent.b, 0.85F * alpha);
    const CHyprColor vpCol(accent.r, accent.g, accent.b, 0.18F * alpha);

    // Panel anchored bottom-right (monitor-local logical coords), sized to the canvas aspect.
    const double MARGIN = 24.0, PAD = 8.0;
    double       pw = std::min(360.0, mon->m_size.x * 0.28);
    double       ph = pw * (bb.y / bb.x);
    if (ph > mon->m_size.y * 0.34) {
        ph = mon->m_size.y * 0.34;
        pw = ph * (bb.x / bb.y);
    }
    const CBox panel(mon->m_size.x - pw - MARGIN, mon->m_size.y - ph - MARGIN, pw, ph);
    addRect(panel, CHyprColor(0.0, 0.0, 0.0, 0.55F * alpha), 10);

    const double   sc      = std::min((pw - 2 * PAD) / bb.x, (ph - 2 * PAD) / bb.y);
    const Vector2D origin(panel.x + PAD, panel.y + PAD);
    const auto     toPanel = [&](const Vector2D& g) { return origin + (g - bbMin) * sc; };

    // This monitor's viewport behind the windows (faint accent fill).
    const Vector2D vp = toPanel(mon->m_position);
    addRect(CBox(vp.x, vp.y, mon->m_size.x * sc, mon->m_size.y * sc), vpCol, 2);

    // Windows on top: a subtle plate, the app icon (so you can tell which is which), and the
    // SUPER+number badge in the corner.
    for (size_t i = 0; i < wins.size(); ++i) {
        const Vector2D tp = toPanel(wins[i].pos);
        const double   rw = std::max(2.0, wins[i].size.x * sc), rh = std::max(2.0, wins[i].size.y * sc);
        addRect(CBox(tp.x, tp.y, rw, rh), winCol, 2);

        // App icon, centred and square, filling most of the rect.
        if (const auto tex = iconTexture(wins[i].cls)) {
            const double s = std::min(rw, rh) * 0.82;
            if (s >= 6.0)
                addTex(tex, CBox(tp.x + (rw - s) / 2.0, tp.y + (rh - s) / 2.0, s, s), 0.95F * alpha);
        }

        // SUPER+number badge: small, top-left, on a dark chip so it reads over any icon.
        const double dh = std::min({rh * 0.42, rw * 0.42, 11.0});
        const double dw = dh * 0.6;
        if (i < 9 && dh >= 5.0 && rw > dw + 4.0 && rh > dh + 4.0) {
            const double bx = tp.x + 1.5, by = tp.y + 1.5;
            addRect(CBox(bx - 0.5, by - 0.5, dw + 3.0, dh + 3.0), CHyprColor(0.0, 0.0, 0.0, 0.55F * alpha), 2);
            drawDigit((int)i + 1, CBox(bx + 1.0, by + 1.0, dw, dh), numCol);
        }
    }
}
