# canvasinfinite — roadmap / TODO

Done so far: toggle, grab-pan (1:1), multi-monitor, layout persistence across toggles,
full-display overview (companion script, always steps back to a visible zoom, spans all
monitors as one continuous canvas via seam-anchored repositioning), DPI-block (apps keep
native resolution during overview — no reflow), overview gated on canvas mode,
SUPER+number jumps to the Nth-largest window in canvas mode (workspace switch otherwise),
public repo.

## Features / ideas

- [ ] **Minimap while dragging.** Overlay showing window rects + current viewport during
  grab-pan, fading out when idle.
- [ ] **Smooth zoom animation.** Animate the DPI-matched ↔ overview transition. Hard: monitor
  scale is an instant output reconfigure (not animatable); only path is a transient render
  transform during the toggle (the renderModif route we shelved for being ugly).

## Sharing / polish

- [ ] `hyprpm.toml` so users can `hyprpm add` this repo.
- [ ] GitHub topics (hyprland, wayland, hyprland-plugin) for discoverability.
- [ ] Demo GIF in the README.
