# Phase 0 Findings — hyprplane (tested live on Hyprland 0.55.4)

Tested by actually building, loading, and exercising the plugin in the running
compositor (commit `a0136d8c`). Summary: **the toggle/gating architecture is
sound, but the core rendering approach must change from A to B.**

## ✅ Validated

| Thing | Result |
|---|---|
| Build against system headers | Clean. C++26, `pkg_check_modules(hyprland pixman-1 libdrm pangocairo)`. |
| Required exported symbols | `pluginAPIVersion`, `pluginInit`, `pluginExit`, **`__hyprland_api_get_hash`**. |
| Plugin loads/unloads live | `hyprctl plugin load/unload` → `ok`, no crash, compositor stable. |
| `CCanvasMode` toggle spine | `canvas:toggle` works live; gating returns "not in canvas mode" when off. |
| Dispatchers (`addDispatcherV2`) | Work, incl. arg-parsing (`canvas:pan <dx> <dy>`). |
| Event-bus scroll hook | Registers; `Cancellable<IPointer::SAxisEvent>` listener compiles+runs. |
| Non-destructiveness | With plugin off, zero behavioural change. User's 8 windows untouched. |

## ❗ Critical finding: Approach A (`m_renderOffset`) is NOT viable

**Test:** toggled canvas on, set the workspace `m_renderOffset` via `canvas:pan`,
captured screenshots at origin / mid-animation / settled.

**Result:** the pan was **transient** — windows shifted mid-animation (caught in
the immediate screenshot) then **reverted to origin** within ~1s. The active
workspace's render offset is driven back to 0 by Hyprland.

**Root cause (confirmed from v0.55.4 source):**
- `Renderer.cpp` renders each window at `m_realPosition->value() + m_renderOffset->value()` — so the offset *does* affect rendering.
- BUT input hit-testing (`Compositor.cpp::vectorToWindowUnified`) uses **only `m_realPosition->goal()` and ignores `m_renderOffset`**.
- Therefore `m_renderOffset` is a *transient animation-only* mechanism (workspace-switch swipes). Hyprland keeps the active workspace's offset at 0. Even if we re-asserted it every frame via a render hook, **rendering would shift but clicks would land at the unshifted positions** → input/visual desync. Unusable for an interactive canvas.

## ✅ Decision: pivot to Approach B (move window positions)

The camera is implemented by positioning windows at `canvasCoord - camera` via
their **real positions** (`CWindow::m_realPosition` / `m_position`). Render *and*
input both read the real position, so they stay in sync. This is how
scroller-style Hyprland plugins work.

```cpp
// pan = translate every mapped window on the canvas workspace
for (auto& w : g_pCompositor->m_windows) {           // Compositor.hpp:57
    if (!w || !w->m_isMapped || w->m_workspace != ws) continue;
    w->m_realPosition->setValue(w->m_realPosition->goal() + delta); // animated
}
```
Fields (verified, `desktop/view/Window.hpp`): `m_realPosition` (PHLANIMVAR<Vector2D>),
`m_position` (logical Vector2D), `m_workspace`, `m_isMapped`, `m_isFloating`.

**Consequence for the plan:** Approach B *requires* the Phase 1 floating algorithm
up front — under a tiling layout `recalculate()` snaps windows back, so canvas
windows must be on a no-op-recalculate floating algo (or marked floating) for pans
to hold. `enter()` must therefore save each window's floating state + geometry and
`leave()` restore it. This is the make-or-break non-destructiveness test.

## 🪤 Gotchas discovered (cost real time — remember these)

1. **`dlopen` caches by path.** `hyprctl plugin unload <p>` then `load <p>` returns
   `ok` but keeps running the *old* image (handle unchanged, new dispatchers
   missing) — `dlclose` doesn't unmap (lingering refcount). **Fix: load each
   rebuild from a unique filename** (`cp build/hyprplane.so /tmp/ci_$(date +%s).so`).
   A new handle (e.g. `...db80` vs `...f7a0`) confirms a real reload.
2. **`hyprctl dispatch` eats leading-`-` args.** `canvas:pan 0 -500` → "Invalid
   dispatcher" (parses `-500` as a flag). Use positive deltas from the CLI;
   negative deltas in `bind =` config lines are fine (parsed by Hyprland, not hyprctl).
3. **Reloads must be sequenced.** Back-to-back unload+load in one shell races
   (async); confirm `plugin list` shows "no plugins loaded" before re-loading.
4. **`Vector2D{x, 0}`** is an ambiguous ctor (int vs double) in 0.55 — use `0.0`.
5. **`WORKSPACEID`** lives in `SharedDefs.hpp`, not `DesktopTypes.hpp`.

## Dev loop that works

```bash
cmake --build build -j
hyprctl plugin unload "$(cat /tmp/ci_live 2>/dev/null)" 2>/dev/null
CP=/tmp/ci_$(date +%s).so; cp build/hyprplane.so "$CP"; echo "$CP" >/tmp/ci_live
hyprctl plugin load "$CP"
# headless visual check: grim before/after a canvas:pan, compare/view PNGs
```
Recovery: SDDM autologin is active and hyprctl-loaded plugins don't persist, so a
crash → restart → clean state.

## Phase 1 (Approach B) — tested live

Implemented `enter()` (snapshot + float windows), `pan()` (move `m_realPosition`),
`leave()` (restore floating-state + geometry, `recalculateMonitor`). Built, loaded
(new handle), no crash.

- ✅ **Non-destructive restore WORKS.** After toggle off, all 8 windows returned to
  EXACT pre-canvas positions AND sizes, re-tiled (`float=False`); user confirmed
  screen normal. `leave()` save/restore is sound. Float API used:
  `g_layoutManager->changeFloatingMode(Layout::CWindowTarget::create(w))`.
- ✅ Approach B pan moves windows and they hold (frames changed; positions persist).
- ❗ **Batch-floating collapses geometry.** `changeFloatingMode` on tiled windows
  gives them DEFAULT float size/pos → windows shrink/overlap into a mess on enter.
  Wrong canvas behaviour. FIX: on enter, after floating each window, immediately
  `setValueAndWarp` its saved size and set position to its tiled position, so canvas
  starts as a 1:1 snapshot of the current layout, then pans. (Or build the real
  `CCanvasAlgo` floating algorithm and assign the workspace to it.)
- ❗ **Op hazard: `pkill -9 grim` wedges wlr-screencopy** (all later grim hang).
  ALWAYS `timeout N grim ...`; never hard-kill a capture mid-frame. Recover screencopy
  with a relog (forcerendererreload did NOT fix it). Normal rendering unaffected.

## Phase 1 — WORKING recipe (verified via hyprctl clients)

The jiggle/no-float dead-ends were CONFOUNDED testing (cursor on the wrong
workspace + windows being closed mid-test), not real failures. The working,
simple approach:

- **float:** `g_layoutManager->changeFloatingMode(w->layoutTarget())` — flips the
  flag synchronously. (A throwaway `CWindowTarget::create(w)` does NOT work; must
  use the window's real `layoutTarget()`.)
- **keep geometry:** after floating, `setTargetGeom(CBox{pos,size}, t)`.
- **SNAPSHOT FIRST:** floating one window re-tiles the rest, so capture every
  window's pos/size in pass 1, then float+pin in pass 2 — else they overlap.
- **pan:** `g_layoutManager->moveTarget(delta, w->layoutTarget())` — moves the
  target's stored position so it HOLDS (raw `m_realPosition->setValue` is visual-
  only → snaps back = jiggle).
- **leave:** `changeFloatingMode(t)` to un-float the ones we floated, then
  `recalculateMonitor(mon)` → exact tiled layout returns.
- compare workspaces by `m_id`, NOT shared_ptr identity (cursor ws ptr != window ws ptr).

Verified: ON → all windows float at their exact tiled positions; pan ±N holds in
both axes; OFF → exact tiled restore. Bound to SUPER+grave (toggle), SUPER+CTRL+J/K
(pan), middle/Ctrl-drag (grab-pan).

## Zoom-out / overview — what worked and what didn't

Goal: zoom OUT to see the whole canvas at proper resolution. Three approaches tried:

1. **renderModif scale in a render hook** — scale the composited output via
   `m_renderData.renderModif`. Result: ugly. It scales *everything* (wallpaper too) into
   a black-boxed region; not a real overview.
2. **Window snapshots** (`makeSnapshot`/`renderSnapshot` scaled in the renderWindow hook)
   — renders correctly BUT `makeSnapshot` does nested rendering inside the render pass and
   **crashes Hyprland** (even once-per-session). Dead end. Do not call makeSnapshot from a
   render hook.
3. **Monitor scale** ✅ — lower each monitor's `scale` so the logical desktop grows and the
   compositor downscales it crisply (real resolution, wallpaper correct, off-screen windows
   become visible). Validated in a nested sandbox + on real hardware.

The catch: monitor scale only **persists** through Hyprland's keyword/config path. From a
plugin, `mon->applyMonitorRule(rule, true)` applies for ~1 frame then the config monitor-rule
list re-asserts the configured scale (the config manager isn't exposed to plugins). So scaling
is done by a **companion script** (`scripts/canvas-overview`) driving `hyprctl keyword monitor`
— the idiomatic, persistent path. It computes a fit-all scale per monitor from `hyprctl
clients` bbox, reconstructs each monitor's exact line (mode/refresh/pos/**transform** — portrait
verified) with the new scale, and saves originals to a state file to toggle back. `hyprctl
reload` is the backstop. Zoom is a view/monitor operation, so it lives in the script, not the
render plugin.
