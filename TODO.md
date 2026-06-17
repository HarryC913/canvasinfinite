# canvasinfinite — roadmap / TODO

Done so far: toggle, grab-pan (1:1), multi-monitor, layout persistence across toggles,
full-display overview (companion script, always steps back to a visible zoom, spans all
monitors as one continuous canvas via seam-anchored repositioning), DPI-block (apps keep
native resolution during overview — no reflow), overview gated on canvas mode, public repo.

## Features / ideas

- [ ] **Minimap while dragging.** Overlay showing window rects + current viewport during
  grab-pan, fading out when idle.
- [ ] **Smooth zoom animation.** Animate the DPI-matched ↔ overview transition instead of an
  instant scale jump (without reintroducing the render-hack ugliness).
- [ ] **Canvas-mode window-switch binds.** In canvas mode, repurpose `SUPER+1..n` (normally
  workspace switch) to jump/focus windows. Proposed: `SUPER+1` = primary monitor / oldest
  window, higher numbers = smaller or newer.

## Sharing / polish

- [ ] `hyprpm.toml` so users can `hyprpm add` this repo.
- [ ] GitHub topics (hyprland, wayland, hyprland-plugin) for discoverability.
- [ ] Demo GIF in the README.
