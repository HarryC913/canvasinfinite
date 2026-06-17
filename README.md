# hyprplane

A Hyprland plugin that turns your workspace into a pannable **infinite 2D canvas**. Toggle it on and your windows float on an endless plane you can grab and drag around — across every monitor at once — then toggle off to snap straight back to your normal tiling layout. Nothing is destroyed. Everything is remembered.

> **Ever wanted to make Hyprland less efficient?** This is the plugin for you. Want your compositor to render the same windows at arbitrary positions on a conceptual plane, wasting CPU cycles just so you can drag them around like a mood board? Want a minimap that draws tiny 7-segment digits in the render pass and resolves `.desktop` icons at runtime mid-frame? We've got you.

> *(This plugin was written by an AI. The AI apologises. It tried its best. The minimap icon resolver is — genuinely — the most unhinged thing the AI came up with all month. It works, though.)*

Unlike generic Wayland canvas tools, hyprplane hooks directly into Hyprland's C++ plugin API for native compositor-level window manipulation — real position moves, not overlay hacks. The companion overview script uses Hyprland's `keyword monitor` path for crisp compositor downscaling, not a fuzzy render-modifier pass.

> Status: **working & in active development.** Toggle, grab-pan, multi-monitor, layout persistence, full-display overview, DPI-block, and SUPER+number window jumping all work. Built and tested against **Hyprland 0.55.x**.

## Features

- **Toggle canvas mode** per keybind — non-destructive: flip it off and your exact tiling layout returns.
- **Grab-to-pan** — middle-mouse drag (or `Ctrl`+left drag) moves the whole canvas 1:1 with your cursor. Keyboard panning too.
- **All monitors at once** — toggling and panning acts on every monitor's active workspace simultaneously.
- **Layout persistence** — windows remember where you left them on the canvas across off/on toggles. New windows join the canvas automatically; closed windows are forgotten.
- **Full-display overview** — a companion script ([`scripts/canvas-overview`](scripts/canvas-overview)) zooms each monitor out via its scale (crisp compositor downscale, real resolution — not a render hack) so every window fits, then toggles back to native. Per-monitor, rotation-aware.
- **DPI-block** — apps (Steam, GTK, Qt) keep rendering at native resolution during overview, so there's no UI reflow even when the monitor scale drops.
- **SUPER+number window jumping** — in canvas mode, `SUPER+1..0` focus and centre the Nᵗʰ-largest window. Outside canvas mode they fall through to normal workspace switching.
- **Grab-pan minimap** — an overlay showing every canvas window's position, with app icons and 7-segment digit labels, fading in while you drag and out when idle. Completely unnecessary. We included it anyway.

## Requirements

- Hyprland **0.55.x** (the plugin is pinned to the running ABI via the API hash).
- Hyprland development headers (`hyprland` pkg-config), plus `hyprgraphics`, `pixman-1`, `libdrm`, `pangocairo`, `cairo` — these come with the Hyprland dev package on most distros.
- A C++26 compiler (GCC 14+/Clang 18+) and CMake ≥ 3.21.

## Build

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# -> build/hyprplane.so
```

## Install

### Option A: `hyprpm` (recommended)

```sh
hyprpm add https://github.com/HarryC913/hyprplane
hyprpm enable hyprplane
```

### Option B: Manual

1. Copy [`canvas.conf.example`](canvas.conf.example) to `~/.config/hypr/canvas.conf` and edit the plugin path to point at your built `build/hyprplane.so`.
2. Source it at the end of `~/.config/hypr/hyprland.conf`:
   ```
   source = ~/.config/hypr/canvas.conf
   ```
3. Reload Hyprland.

Or load it ad-hoc:

```sh
hyprctl plugin load "$PWD/build/hyprplane.so"
```

### Controls (default binds)

| Action | Binding |
| --- | --- |
| Toggle canvas mode (all monitors) | `SUPER` + `` ` `` |
| Pan the canvas | middle-mouse drag, or `Ctrl` + left-drag |
| Pan (keyboard) | `SUPER`+`Ctrl`+`J` / `K` |
| Toggle full-display overview | `SUPER`+`Ctrl`+`O` (runs `scripts/canvas-overview`) |
| Jump to Nᵗʰ-largest window | `SUPER`+`1` through `0` (canvas mode only) |

## How it works

Canvas windows are set floating so the tiling engine stops repositioning them; panning then moves their real positions (so render *and* input stay in sync). Toggling off restores the saved floating-state and re-tiles. Design notes, dead-ends, and Hyprland-internals gotchas are documented in [FINDINGS.md](FINDINGS.md).

## FAQ

**Q: Won't the AI just hallucinate bugs?**
A: Look at the commit history. This plugin survived Approach A (a whole dead-end with `m_renderOffset` that Hyprland fights back to zero), Approach B (real position moves), a `makeSnapshot` crash, and a `pkill -9 grim` screencopy wedge. The AI learned these the hard way. It is, arguably, unwell.

**Q: Is this actually useful?**
A: If you have a lot of windows and want to spread them out spatially instead of switching workspaces, yes. If you want to make your compositor do more work for the same result, also yes. We support both journeys.

**Q: Why not just use a tiling layout?**
A: The tiling layout doesn't let you drag your entire desktop sideways to reveal more desktop. hyprplane does. It's a completely different (and worse) paradigm. You'll love it.

## License

BSD 3-Clause — see [LICENSE](LICENSE).
