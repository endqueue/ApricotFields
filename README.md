# ApricotFields

A general-purpose engine layer in C++ with a plain C API: a renderer, CPU
geometry math, ZIP archives, an audio mixer and network sync. It runs on
Vulkan, D3D12 and Metal through [SpudLib](https://github.com/endqueue/spudlib).

ApricotFields knows nothing about the application above it. An archive doesn't
know what it holds, the math doesn't know what a mesh is for, and sync doesn't
know what it keeps in step. Ids, bytes and keys belong to the caller and come
back to it unchanged. Domain logic of any kind lives in the caller.

```
The caller                    ← application logic
        ↓ C-ABI
ApricotFields                 ← you are here
        ↓ C-ABI
SpudLib                       ← GPU, files, memory, network, audio
        ↓
Vulkan / D3D12 / Metal
```

## Modules

| Module | Header | Prefix | What |
|---|---|---|---|
| Aprend | `render/aprend*.h` (all through `apricotfields.h`) | `aprend_` | Renderer over SpudGPU: buffers, textures, pipelines, command lists, swap chains, a lit draw list, post-processing (bloom, tone mapping, GTAO, outlines) |
| ApMath | `aprimath.h`, `math/apmathbvh.h`, `math/apmathpoly.h` | `apmath_` | Double-precision CPU ray casting over BVHs; exact plane polygons on an integer grid (booleans, offsets, straight skeletons, triangulation) |
| ApArchive | `archive/aparchive.h` | `aparchive_` | ZIP reading and writing through caller callbacks, with no I/O of its own |
| ApAudio | `audio/apaudio.h`, `audio/apaudiocues.h` | `apaudio_` | Mixer, cues and sound banks |
| ApSync | `sync/apsyncnet.h` | `apsync_` | Sessions, channels and snapshots over SpudNet |
| Core | `apricore.h` | `apri_` / `APRESULT` | The shared result codes, struct bases and debug names |

Every call that can fail returns an `APRESULT`. Headers state which calls are
safe from which threads. No module keeps global state or calls back on a
thread of its own: ApSync, which runs its network work on its own thread,
hands events over by polling.

## Status

Aprend has run a caller's full renderer on D3D12 (Windows), Vulkan (Windows,
Linux) and Metal (macOS, iOS). Its API is mid-overhaul and not yet stable. The
newer modules (ApMath, ApArchive, ApAudio, ApSync) are in use by callers above
and have headless tests. Each module's unfinished work is listed in
`AP<MODULE>_TODO.md` at the repo root.

## Building

Requirements: CMake 3.30+, a C++23 compiler (C++26 for D3D12), and `glslc`
from the Vulkan SDK on every platform. Built-in shaders are compiled to SPIR-V
at build time and embedded, and SpudGPU cross-compiles them for D3D12 and
Metal.

ApricotFields expects SpudLib checked out beside it (`../spudlib`), as in the
[eqdev workspace](https://github.com/endqueue/eqdev). Third-party
dependencies are taken from `../.deps/` when present and fetched at a pinned
tag otherwise.

```sh
cmake --preset apricotfields-macos-metal        # or -windows-d3d12, -windows-vulkan, -linux-vulkan
cmake --build --preset apricotfields-macos-metal-debug
ctest --test-dir build-macos-metal -C Debug
```

The `-asan` (AddressSanitizer + UBSan) and `-tsan` presets, on macOS and Linux,
build and test the same under the sanitizers. `Doxyfile` generates the API
reference from the headers.

## Tests

`tests/` holds headless tests that run without a GPU: math, archives, audio,
sync, and Aprend's shader reflection and range tracking. Tests that need a real
device draw offscreen and check the pixels they read back. Those are in
`apricottests`, beside this repository in the workspace.

## Contributing

Read [`CLAUDE.md`](CLAUDE.md) first. It holds the rules every change follows:
no caller-specific logic, argument checks one per line, no defaults in a desc,
struct bases, and the two code styles (Aprend's and the newer modules').

## License

[Apache License 2.0](LICENSE).
