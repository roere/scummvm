# AppBase SCUMM WebAssembly build

This tree is the complete corresponding GPL source for the AppBase Maniac
Mansion / Zak McKracken runtime, based on ScummVM 2.9.0. No original game files,
manuals, save games, accounts or private configuration are included.

Use Emscripten SDK 4.0.10, with its SDL2 port. The SDK source archive is
https://github.com/emscripten-core/emsdk/archive/refs/tags/4.0.10.tar.gz
(SHA256 2497b55ddbba9bf9be2d18cfca3e973d40a0cfaa8d18f6caacb882a65b2faf1c).
After installing/activating 4.0.10 and sourcing emsdk_env.sh:

```sh
emconfigure ./configure --host=wasm32-unknown-emscripten --build=wasm32-unknown-emscripten --disable-all-engines --enable-engine=scumm --disable-detection-full --disable-seq-midi --disable-timidity --disable-mt32emu --disable-debug --enable-release
printf '\nLDFLAGS += -lidbfs.js -s EXPORTED_RUNTIME_METHODS=FS,IDBFS,callMain,addRunDependency,removeRunDependency,UTF8ToString\n' >> config.mk
emmake make -j4
```

Generated files are scummvm.js and scummvm.wasm. COPY this source tree and the
COPYING license alongside distributions of those files. The AppBase plugin
contains the equivalent source archive recipe, 72 sequential patches and pinned
before/after SHA256 manifests, plus an installer validating the resulting source
and binary files:
https://github.com/roere/AppBase-Plugins-Extra/tree/feature/scumm-mod-system/maniac-mansion/tools

Patch 052 queues presentation changes on the native engine loop. Graphics modes
normal, SuperEagle and HQ2x use one SDL surface at 2x. Audio preferences mute
native mixer categories. Neither setting changes the game's scripts, resources,
progress, pause state or save format.

Patch 071 adds optional render density (`appbase_render_scale`, integer 1..8)
for classic Amiga SCUMM v2 Zak and Maniac Mansion. Game logic, positions, virtual
screens and save coordinates remain at 320x200. Scale 1 uses the original output
path and does not load HD data. Higher factors upscale original pixels and can
compose optional indexed costume cels from `appbase_hd_costumes` (ABHD v1).
Native costume/limb/cel selection, actor ordering, palette and masks still apply.
The AppBase wrapper passes these settings at engine startup; the figures editor
can generate both native fallback costumes and HD sidecars at density 2, 3 or 4.
See the plugin's maniac-mansion/docs/render-scale.md for the binary format and
verification procedures. The WASM diagnostic exports are _scumm_render_version
and _scumm_render_get; neither changes the game state.

Patch 072 preserves the full backing-canvas resolution for AppBase HD render
factors greater than 1. The Emscripten SDL window does not clamp that canvas to
the reported desktop size; the wrapper fits its CSS display size instead.
Factor 1 retains the existing desktop/window bounds. This avoids discarding HD
costume detail when the internal surface exceeds the reported browser display.
