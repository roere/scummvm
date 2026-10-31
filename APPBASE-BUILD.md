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
contains the equivalent source archive recipe, 52 sequential patches and pinned
before/after SHA256 manifests, plus an installer validating the resulting source
and binary files:
https://github.com/roere/AppBase-Plugins-Extra/tree/main/maniac-mansion/tools

Patch 052 queues presentation changes on the native engine loop. Graphics modes
normal, SuperEagle and HQ2x use one SDL surface at 2x. Audio preferences mute
native mixer categories. Neither setting changes the game's scripts, resources,
progress, pause state or save format.
