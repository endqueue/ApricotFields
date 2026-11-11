# Rabbittus

A general-purpose engine layer in C++ with a plain C API: a renderer, CPU
geometry math, ZIP archives, an audio mixer and network sync. It runs on
Vulkan, D3D12 and Metal through [Carrottus](https://github.com/endqueue/carrottus).

Rabbittus knows nothing about the application above it. An archive doesn't
know what it holds, the math doesn't know what a mesh is for, and sync doesn't
know what it keeps in step. Ids, bytes and keys belong to the caller and come
back to it unchanged. Domain logic of any kind lives in the caller.

```
The caller                    ← application logic
        ↓ C-ABI
Rabbittus                 ← you are here
        ↓ C-ABI
Carrottus                       ← GPU, files, memory, network, audio
        ↓
Vulkan / D3D12 / Metal
```

## Modules

| Module | Header | Prefix | What |
|---|---|---|---|
| Rend | `render/rbtrend*.h` (all through `rbtrend.h`) | `rbtrend_` | Renderer over Carrottus GPU: buffers, textures, pipelines, command lists, swap chains, a lit draw list, post-processing (bloom, tone mapping, GTAO, outlines) |
| Rabbittus Math | `rbtmath.h`, `math/rbtmathbvh.h`, `math/rbtmathpoly.h` | `rbtmath_` | Double-precision CPU ray casting over BVHs; exact plane polygons on an integer grid (booleans, offsets, straight skeletons, triangulation) |
| Rabbittus Archive | `archive/rbtarchive.h` | `rbtarchive_` | ZIP reading and writing through caller callbacks, with no I/O of its own |
| Rabbittus Audio | `audio/rbtaudio.h`, `audio/rbtaudiocues.h` | `rbtaudio_` | Mixer, cues and sound banks |
| Rabbittus Sync | `sync/rbtsyncnet.h` | `rbtsync_` | Sessions, channels and snapshots over Carrottus Net |
| Rabbittus Text | `text/rbttext.h` | `rbttext_` | Fonts from the caller's bytes, fallback chains, shaping (HarfBuzz), bidirectional text and line breaking, and an MTSDF glyph atlas; single-stroke (Hershey) faces as line segments. Run on macOS only so far |
| Rabbittus Jobs | `jobs/rbtjobs.h` | `rbtjobs_` | Parallel work through a scheduler the caller implements (enqueue a ranged task, wait for it), and a reference pool of threads a caller may create. Run on macOS only so far |
| Core | `rbtcore.h` | `rbt_` / `RBT_RESULT` | The shared result codes, struct bases and debug names |

Every call that can fail returns an `RBT_RESULT`. Headers state which calls are
safe from which threads. No module keeps global state or calls back on a
thread of its own: Rabbittus Sync, which runs its network work on its own thread,
hands events over by polling.

## Status

Rend has run a caller's full renderer on D3D12 (Windows), Vulkan (Windows,
Linux) and Metal (macOS, iOS). Its API is mid-overhaul and not yet stable. The
newer modules (Rabbittus Math, Rabbittus Archive, Rabbittus Audio, Rabbittus Sync) are in use by callers above
and have headless tests. Each module's unfinished work is listed in
`AP<MODULE>_TODO.md` at the repo root.

## Building

Requirements: CMake 3.30+, a C++23 compiler (C++26 for D3D12), and from the
Vulkan SDK `slangc` and `spirv-val` on every platform, and `dxc` on Windows
(Apple's `metal` and `metallib` on Apple platforms). Built-in shaders are
Slang, compiled at build time to each graphics API's own format (SPIR-V,
DXIL, a .metallib; `cmake/rbtrend_slang.cmake`) and embedded; nothing is
compiled at run time.

Rabbittus expects Carrottus checked out beside it (`../carrottus`), as in the
[eqdev workspace](https://github.com/endqueue/eqdev). Third-party
dependencies are taken from `../.deps/` when present and fetched at a pinned
tag otherwise.

```sh
cmake --preset rabbittus-macos-metal        # or -windows-d3d12, -windows-vulkan, -linux-vulkan
cmake --build --preset rabbittus-macos-metal-debug
ctest --test-dir build-macos-metal -C Debug
```

The `-asan` (AddressSanitizer + UBSan) and `-tsan` presets, on macOS and Linux,
build and test the same under the sanitizers. `Doxyfile` generates the API
reference from the headers.

## Tests

`tests/` holds headless tests that run without a GPU: math, archives, audio,
sync, and Rend's shader reflection and range tracking. Tests that need a real
device draw offscreen and check the pixels they read back. Those are in
`rabbittustests`, beside this repository in the workspace.

## Contributing

Read [`CLAUDE.md`](CLAUDE.md) first. It holds the rules every change follows:
no caller-specific logic, argument checks one per line, no defaults in a desc,
struct bases, and the two code styles (Rend's and the newer modules').

## License

[Apache License 2.0](LICENSE).
