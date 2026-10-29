# ApricotFields

The general-purpose engine layer of Apricot: rendering (Aprend), math, archives,
audio, sync. See `../CLAUDE.md` (the `eqdev` workspace root) for how this repo fits
into the larger architecture; this file covers conventions specific to working
inside `ApricotFields` itself.

## The one rule everything else follows

**General purpose: a module knows nothing about what its caller is doing with it.**
ApArchive doesn't know what an archive holds, ApSync doesn't know what is being kept
in step, ApMath doesn't know what a mesh is or why a ray is cast. Every header says
so in its opening comment, along with what the module deliberately does *not* do.

- No application-domain concept, name or assumption in here - CAD, BIM or any
  other. If a change needs to know what the application is, it belongs above this
  layer.
- **ApricotFields doesn't know who calls it.** Nothing here - code, comments,
  headers, docs - names a caller or assumes which one it is. Write "the caller".
- What the bytes, ids, masks and keys *mean* is the caller's. Take them as opaque
  values and hand them back unchanged.
- A new capability that only one caller wants is still written for any caller, or
  it goes in that caller instead.

## Module map

| Module | Header | Prefix | Notes |
|---|---|---|---|
| Core | `apricore.h` | `APRESULT`, `APRI_`, `apri_`, `APRICOTFIELDS_VERSION` | The version, the shared result enum, struct bases and debug names |
| Aprend | `render/aprend*.h` | `aprend_` / `APREND_` | Renderer over SpudGPU. Older code, mid-overhaul |
| ApMath | `aprimath.h`, `math/apmathbvh.h`, `math/apmathpoly.h` | `Apri*` types, `apmath_` / `APMATH_` | CPU ray casting, double precision; exact plane polygons on an integer grid (booleans, checks, per-edge offsets, weighted straight skeletons, triangulation); unrelated to Aprend |
| ApArchive | `archive/aparchive.h` | `aparchive_` / `APARCHIVE_` | ZIP through caller callbacks; no I/O of its own |
| ApAudio | `audio/apaudio.h`, `audio/apaudiocues.h` | `apaudio_` / `APAUDIO_` | Mixer, cues and sound banks |
| ApSync | `sync/apsyncnet.h` | `apsync_` / `APSYNC_` | Sessions, channels, snapshots over SpudNet |
| ApText | `text/aptext.h` | `aptext_` / `APTEXT_` | Faces, fallback chains, shaping and line layout, MTSDF atlas, single-stroke faces; no file I/O, no GPU. **Contract only, not implemented**: every call checks its arguments and returns `APRESULT_NOT_IMPLEMENTED` (`APTEXT_TODO.md`) |

`apricotfields.h` is not a full umbrella: it includes every Aprend header (fifteen) and
nothing of the other modules. Callers include the module header they need.

Each module that has unfinished work keeps it in `AP<MODULE>_TODO.md` at the repo
root. Read the module's TODO before changing it, and update it in the same change
when you finish or add an item.

## Two generations of code - match the one you're in

Aprend is older and written differently from the four newer modules. Don't carry
one style into the other, and don't convert Aprend to the newer shape unless asked.
The whole conversion is decided (2026-10-28, `APREND_TODO.md`, "The contract
conversion"), to be done before the first release; until it is, this holds.

| | Aprend (`src/render/`) | ApArchive, ApAudio, ApSync, ApMath |
|---|---|---|
| Indentation, braces | tabs, attached braces | 4 spaces, Allman braces |
| Creating a handle | returns the handle, `NULL` on failure | returns `APRESULT`, handle through an `out_` parameter (set to null on failure) |
| Allocation | `malloc` + in-place construction, `goto` cleanup | `new (std::nothrow)` and `delete`, then a `try` block - not the convention any more, see "Code conventions" |
| Failure detail | `printf` of the `SPUDRESULT` | per-call `AP<MODULE>_ERROR` message struct |

Public headers use tabs in both generations.

`.clang-format` describes the Aprend style. Running it over a newer-module file
rewrites the whole file, so don't run clang-format on a file unless asked; format
what you touch by hand to match its neighbours.

## Writing a new module, or a new call in a newer module

Follow `aparchive.h` and `apsyncnet.h`, which state these rules in a "Conventions"
block near the top. A new header gets the same block.

- **Results.** Every call that can fail returns `APRESULT`. Use the general codes
  first; add an `APRESULT_<MODULE>_*` code to `apricore.h` only for a failure the
  caller would branch on, in the module's own numbered block (ApArchive 1001+,
  ApSync 2001+, ApText 6001+). Never renumber an existing code.
- **Error messages.** A call that can fail for a reason worth a sentence takes an
  `AP<MODULE>_ERROR *` (NULL to skip). It is per call, not stored on the object, so
  reads a header allows to overlap (an archive reader's) stay safe.
- **Struct bases.** Every public struct that may grow starts with an
  `APRI_STRUCT_BASE base` (`APRI_STRUCT_BASE_OUT` for one ApricotFields fills in),
  with its own `APRI_STRUCT_TYPE_*` in `apricore.h`, in the module's block; every
  other public struct, in every module, Aprend and `aprimath.h` included, has a
  `/* Frozen: <why> ... */` line above its `typedef`. `apricore.h`, "Struct bases", has the rules. A released layout never
  changes: a new field is a new struct chained on. A call checks a based argument
  with `apri_struct_check` / `apri_struct_out_check` (`src/apristructbase.hpp`)
  right after its null check, and returns what it gives. This is SpudLib's
  scheme (`SPUD_STRUCT_BASE`) with ApricotFields' own types; it replaced a
  leading `struct_size` on 2026-10-19, and Aprend took it on 2026-10-20
  (`aprendcontext.h`, "Struct bases", has how Aprend applies it). The
  workspace's `check.sh struct-bases` checks the headers' side: based or
  frozen, base first, no `#if` in a struct, no based struct held by value,
  and each type registered in `APRI_STRUCT_TYPE` as the right kind and used
  in `src/`.
- **No defaults in a desc.** A limit or a time left at 0 is refused with
  `APRESULT_INVALID_ARGUMENT` unless the field's comment says what 0 means.
- **Validate every argument before allocating anything**, then allocate (by hand,
  per "Code conventions"), then fill in inside a `try`. `apmath_mesh_bvh_create`
  shows the order; its `new (std::nothrow)` is the part not to copy.
- **No C++ exception leaves a C entry point.** Internal helpers may throw
  `std::bad_alloc` (they say so in a comment); every `extern "C"` function that can
  reach one catches it and returns `APRESULT_OUT_OF_MEMORY`, freeing what it built.
- **Destroy accepts null.** Value getters on a null handle return 0.
- **Threads are part of the contract.** Every object is used by one caller at a
  time, from any thread, and different objects are independent: `apricore.h`,
  "Threads", states it once for the library (the workspace's "Threading", card
  X3). The rule is serialization, not thread identity: never write "the thread
  that created it". A call that is an exception (callable from any thread at any
  time, a read that may overlap others, a wake, code that runs on another
  thread) says so on a line beginning `Thread:` in its own comment; a header's
  `@par Threads` holds only what is particular to that module. Don't add a
  callback that runs on a module's own thread; ApSync delivers events by polling
  for that reason.
- **Platform gaps come from SpudLib's `SPUD*_EXT_*` macros**, never from a
  platform's name, and surface as `APRESULT_UNSUPPORTED`.

## Containers: eqstd (decided 2026-10-28)

New containers and strings in `src/` come from `eqstd` (`../eqstd`, its
`TODO.md`; built 2026-10-28, not yet used here): `eq::array` for plain data, `eq::vector` and
`eq::string` for the rest, never in a public header. Old code converts as
it is changed. Whether every desc takes a caller's allocator is paused
(card X15); until then they use the system heap.

## Code conventions

- **Argument checks are written line by line: one `if`, one condition, one
  return.** They sit at the top of the function, in parameter order, before any
  allocation or call into SpudLib, and each reports exactly what was wrong: its own
  `APRESULT`, and its own message where the call takes an `AP<MODULE>_ERROR`:

  ```cpp
  if (d.max_backlog_size == 0)
      return fail(error, APRESULT_INVALID_ARGUMENT, "max_backlog_size is 0");
  if (d.handshake_timeout_ms == 0)
      return fail(error, APRESULT_INVALID_ARGUMENT, "handshake_timeout_ms is 0");
  ```

  Don't join checks with `||` or `&&`. A joined check can only give one code and
  one message, so the caller can't tell which argument failed. If two arguments
  would return the same code today, they still get a line each, so one can be given
  a more specific code or message later without restructuring.
- Where the `return` goes (same line or the next) and the brace style follow the
  file's generation, per the table above.
- **No allocating `new` and no `delete`: allocate and free by hand.** Which
  allocator depends on whether the object's type has a constructor.
  `./check.sh conventions` counts each allocating `new` (`std::make_unique`
  too) and `delete` in `src/`; the old ones below are in `check.baseline`.
- **A type with a constructor:** `malloc`, check the pointer, then construct the
  object in place in that memory. `malloc`, not `calloc`, because the constructor
  is what initialises it. The in-place form needs `<new>`.

  ```cpp
  aprend_uniform_buffer_t *object = (aprend_uniform_buffer_t *)malloc(sizeof(aprend_uniform_buffer_t));
  if (!object)
      return nullptr;
  object = new (object) aprend_uniform_buffer_t();
  ```

  It is destroyed the same two steps in reverse: the destructor called explicitly,
  then `free`.

  ```cpp
  object->~aprend_uniform_buffer_t();
  free(object);
  ```
- **A type with no constructor:** `calloc`, check the pointer, and that is all.
  The zeroed memory is its initial state, and nothing is constructed. It is
  destroyed with `free` alone.

  ```cpp
  aprend_uniform *uniforms = (aprend_uniform *)calloc(count, sizeof(aprend_uniform));
  if (!uniforms)
      return nullptr;
  ```
- "Has a constructor" includes a type that only has members which do: a struct
  holding a `std::vector`, `std::string` or `std::thread` must be constructed in
  place and have its destructor called, or those members are never set up or
  released. That is every handle struct in the newer modules, and every Aprend
  handle struct (they declare constructors). The public C structs - descs,
  `aprend_uniform`, the `Apri*` math types - are plain.
- A function that returns `APRESULT` returns `APRESULT_OUT_OF_MEMORY` for the null
  pointer. Constructing in place doesn't remove the `try`: filling the object in
  can still throw `std::bad_alloc`, and the `catch` then calls the destructor and
  `free`, in that order.
- Every allocation has exactly one matching release, on every failure path as well
  as in the destroy call.
- Aprend already does this, by hand or through the `APREND_MALLOC__T` /
  `APREND_CONSTRUCT__T` / `APREND_DESTRUCT__T` macros in
  `src/render/aprend_internal.hpp`; a few of its plain arrays use `malloc` where
  this says `calloc`. ApArchive, ApAudio, ApSync and ApMath don't follow it yet:
  they use `new (std::nothrow)` and `delete`. Write new code this way; convert an
  old allocation only when you are already changing that function, and convert
  both ends together - memory from an allocating `new` must never reach `free`,
  nor `malloc` memory reach `delete`.
- About half of the existing checks are joined (`if (!instance || !desc)`), mostly
  in Aprend. Write new checks line by line; split an old joined one only when you
  are already changing that function.
- **Data is plain structs, kept in arrays, not objects.** Behaviour is free
  functions over plain structs, as at the C boundary; inside `src/` too:
  - The element of an array a frame iterates (Aprend's commands, a draw
    list's draws and batches, the pools' range tables) is trivially copyable
    and standard layout: `APRI_PLAIN_DATA(T)` (`src/apriplaindata.hpp`), a
    static_assert beside its type. `./check.sh plain-data` fails on such an
    array whose element has none. A record a per-frame loop reads also has a
    size budget, `APRI_HOT_RECORD(T, bytes)` in cache lines, so a field
    added to it is a decision.
  - No class hierarchies or `virtual`, no `std::shared_ptr` or
    `std::function`, no node-based containers (`std::map`, `std::set`,
    `std::list`, `std::unordered_*`) for anything iterated per frame, and no
    array of pointers to objects allocated one at a time: each is an
    indirect call or a pointer chase per element. Where the data is
    singular, cold or shaped from outside (a long-lived handle, a parse at
    load time, a third-party interface) they are fine. `./check.sh
    conventions` counts each use; the allowed ones and the old code still to
    convert are in `check.baseline` with reasons. Existing code is converted
    only when it is changed for another reason.

## Public headers

- Plain C: `#if __cplusplus` / `extern "C"`, `<stdint.h>` and `<stdbool.h>`, no C++
  types. glm, glaze, libdeflate and the standard library never appear in `include/`.
- **A named enum is `typedef enum NAME APRI_ENUM_BASE {`** (`apricore.h` says why): C++ then
  sees it with a fixed `uint32_t` underlying type, so a value a C caller stores
  outside its enumerators is one the check refuses, not undefined behaviour to
  read. UBSan's `-fsanitize=enum` caught the plain form (2026-10-20).
- Opaque handles are `typedef struct <name>_t *<name>;`. The struct is defined only
  in the module's `src/<module>/*_internal.hpp`, which nothing outside that
  directory includes.
- Lower-case for handles and functions (`apsync_session`), upper-case for value
  structs, enums and constants (`APSYNC_SESSION_DESC`, `APSYNC_CHANNEL_ORDERED`).
- Header comments are the contract: what the call does, its preconditions, its
  failures. Implementation notes are `//` comments in the `.cpp`.
- Every public header is Javadoc-style Doxygen (since 2026-10-25; ApArchive,
  ApAudio, ApSync, ApMath and `apricore.h` were `/* */` with `[brackets]`
  before): a `@file` block with the header's `@par Threads` where it has a
  threading rule, then `/** */` on every declaration, struct member and
  enumerator, with `@brief`, `@param[in]`/`[out]`, `@return`, `@warning` and
  `@see`. A parameter is `@p name` in prose, a function `name()`, a member
  of the same struct `#member`, and a field or command member in backticks.
  `aprendimages.h` is the model. `/* Frozen: ... */` lines and section
  banners stay plain comments. The `Doxyfile` at the repo root builds them;
  it reports no warnings, checked by `./check.sh doxygen`.
- Math types crossing the API are the `Apri*` structs from `aprimath.h`. They are
  laid out identically to their GLM counterparts so `to_glm()` can reinterpret them
  in place; changing a member's order or type breaks that silently.
- Aprend's descs and lit scene settings start with an `APRI_STRUCT_BASE base`,
  as the newer modules' do; its calls refuse a wrong base in their own way
  (NULL, false, or nothing recorded, after printing why), through
  `aprend_base_ok()` in `src/render/aprend_internal.hpp`. A desc Aprend
  builds for its own calls sets its base like any caller's. A getter that
  returns a desc sets the base itself. `APREND_COMMAND`'s `_params` is padded
  to a fixed 120 bytes, so the command stays 128 bytes as commands are added.
- An Aprend handle struct's first member is its `debug_name` pointer
  (`apricore.h` explains why). Keep it first. It, and every desc's
  `debug_name`, exist in every build (since 2026-10-18; before, only under
  `_DEBUG`, which made a desc's layout differ between Debug and Release):
  no build switch changes a public struct's layout or which functions
  exist.

## Aprend

- Aprend calls SpudGPU's backend-agnostic C API only. It never includes a Vulkan,
  D3D12 or Metal header and never branches on the backend at runtime.
- Aprend is handed its `spudgpu_device` in `aprend_instance_desc`. It doesn't
  create the SpudGPU instance, list adapters, choose one or create the device.
- A SpudGPU device has only the features it was created with, so Aprend reads
  features from the device (`aprend_instance_t::features`), never the
  adapter's `supported_features`, and reads properties and limits from the
  device's adapter. A feature only Aprend's own code uses, with a path
  without it, goes in `aprend_get_optional_gpu_features()` for the caller to
  ask for. A feature one call needs is named in that call's header comment
  and checked by the call, which refuses in Aprend's way rather than passing
  SpudGPU's refusal on. `APREND_TODO.md` item 39 has the list.
- A choice SpudGPU leaves open is either a rendering matter, made in Aprend
  explicitly, or one Aprend takes from its own caller through a desc. It is never
  pushed back down into SpudLib as a default and never left implicit.
- SpudGPU's plan for hardware without dynamic rendering is a caller-owned
  `spudgpu_framebuffer` (`spudlib/CLAUDE.md`; designed, not built). When it is
  built, Aprend is that owner: `aprend_framebuffer` would hold and cache it, so the
  rest of Aprend makes one begin/end call either way. Nothing in Aprend does this
  yet.
- Picking is not Aprend's job. GPU pick-ID buffers were removed on purpose.
- `aprendbuffers.h` carries its author's note that it is due a rewrite. Extend it
  only as far as the task needs.

## Reference: Hazel

Hazel (2025 renderer; Yan Chernikov / Studio Cherno) is a major reference point
for building this repo, and the engine this repo's competency is compared to:
what Hazel can do is the bar for what ApricotFields should be able to do. It is a
C++ engine whose reference copy is at `../.refs/HazelRenderer2025/Hazel/Hazel`
(source under `src/Hazel/`), read-only and not part of this repo; see
`../CLAUDE.md`, "External references".

**It is a relative reference: Hazel answers "how has this been solved", this file
answers "how is it written here".** Read Hazel for the technique, then write it
under this repo's rules. Where the two disagree, this file wins, every time.
"Hazel does it this way" is never a reason to break a rule here, and nothing is
copied across in Hazel's shape to be converted later.

Where it maps: `Hazel/Renderer` onto Aprend most directly (pipelines and their
specifications, render and compute passes, framebuffers, command buffers, uniform
and storage buffer sets, shaders, materials, the scene renderer).

**SpudGPU, not NVRHI.** Hazel renders through NVRHI; this repo renders through
SpudGPU's C API and nothing else. Read Hazel's `nvrhi::` calls and its
`Hazel/Platform` code for what they achieve, then do it with what SpudGPU
exposes. NVRHI is never added as a dependency, and a capability NVRHI has that
SpudGPU lacks is not a reason to change SpudGPU to resemble it: the comparison
with Hazel is made at this layer only.

What a Hazel idiom becomes here:

| In Hazel | Here |
|---|---|
| `class Pipeline : public RefCounted`, held as `Ref<Pipeline>` | An opaque handle, `typedef struct <name>_t *<name>;`, with a create and a destroy call and one owner. No reference count crosses the API |
| `static Ref<X> Create(const XSpecification &)` | `<prefix>_x_create(...)` taking a desc pointer; how the handle comes back follows the generation of code you're in |
| `XSpecification` holding `std::string`, `Ref<>` members and default member values | A plain C desc: `<stdint.h>` types, handles, `const char *`. A struct base first in the newer modules, and no defaults |
| `static` state and getters on `Renderer` | State lives on the instance handle the caller passes in. No globals |
| `Renderer::Submit` lambdas and `RT_` functions | No closure crosses the API. Which calls may overlap is written in the header (`apricore.h`, "Threads") |
| `HZ_CORE_ASSERT(false, ...)` on a bad input | Argument checks line by line, each returning its own result |
| `Ref<X>::Create`, `new` | `malloc` and in-place construction, or `calloc`, per "Code conventions" |
| `namespace Hazel`, `PascalCase` methods, `m_` members | The module prefix; lower-case handles and functions, upper-case value structs and enums |
| `glm::` and `std::` types in a header | `Apri*` structs and C types; no C++ type in `include/` |
| A renderer that knows its scene, assets and editor | No application-domain concept. The caller hands over what to draw |

The sections that hold these rules are "Writing a new module...", "Code
conventions" and "Public headers" above. A Hazel idiom not in the table is
handled the same way: find the rule it meets and follow the rule.

## Tests

Headless, no GPU, no window: `tests/ap<module>*_tests.*`, one or more executables per module,
registered with `add_test` behind `APRICOTFIELDS_BUILD_TESTS`.

- Tests for a C header are written in C on purpose, to prove the header compiles
  and links as C. Keep them `.c`. The one exception is `tests/aparchive_tests.cpp`,
  which is C++ (it uses `std::vector`).
- No framework: a `CHECK(condition)` macro, a `failures` counter, `test_*`
  functions called from `main`, exit code 1 on any failure.
- A contract written before its implementation (`../ROADMAP.md`, section 1)
  ships its behavioural tests with it: a test whose call returns
  `APRESULT_NOT_IMPLEMENTED` reports itself pending and stops, so the
  implementation turns it on without editing it (`tests/aptext_tests.c`).
- A new module ships with its tests file; a new call ships with a test in the
  existing one. Brute-force comparison against a slow obvious implementation is the
  preferred check for anything algorithmic (`test_mesh_against_brute_force`).

When asked to verify, on macOS: configure with the `apricotfields-macos-metal`
preset, build with the `apricotfields-macos-metal-debug` build preset, then run
`ctest --test-dir build-macos-metal -C Debug`. (The presets are multi-config;
there is no separate `-debug` configure preset any more.)

**Fuzzing.** `tests/fuzz/` holds a libFuzzer target per reader of bytes the
caller can't vouch for; today `aparchive_fuzz` (ApArchive's reader over an
archive in memory, which also writes back what it read and reads that
again). `tests/fuzz/aprifuzz.hpp` says how they are built.

- Every normal build runs each as a ctest, `<target>_regressions`, replaying
  `tests/fuzz/corpus/<target>/` and `tests/fuzz/regressions/<target>/`. An
  input the fuzzer crashes on is fixed, then kept in the regressions folder,
  as small as it can be made.
- To fuzz, on macOS: `cmake --preset apricotfields-macos-metal-fuzz` and
  `cmake --build --preset apricotfields-macos-metal-fuzz-debug` (Homebrew's
  LLVM; Apple's Clang has no libFuzzer; on Linux
  `apricotfields-linux-vulkan-fuzz` with the system Clang), then from a
  scratch folder `build-macos-fuzz/Debug/<target> <scratch corpus>
  tests/fuzz/corpus/<target> -max_total_time=900`. New seeds go in with
  `<target> -merge=1 tests/fuzz/corpus/<target> <folder>`.
- On Windows: `apricotfields-windows-d3d12-fuzz` (LLVM's clang through its
  GNU-style driver, RelWithDebInfo; `../claude_made_decisions.md`,
  2026-10-29), run there 10 minutes on 2026-10-29.
- A new module that reads outside bytes gets a target.

## Build

- A new source or header file is added to the `add_library` list in
  `CMakeLists.txt` by hand; there is no glob.
- Third-party code is vendored under `../.deps/` (glm, libdeflate, glaze,
  SPIRV-Cross, Clipper2, earcut.hpp) and linked `PRIVATE` unless a public
  header needs it. Clipper2 and earcut.hpp are used as sources only, through
  `declare_vendored_or_fetch_sources`: Clipper2's library sources are built
  here as `apmath_clipper2`, and earcut.hpp is a header. Both are pinned, and
  both and `src/math/apmathpoly.cpp` are built with floating-point
  contraction off, because what ApMath's polygons compute must be the same
  bits on every platform.
  SPIRV-Cross's core is Aprend's SPIR-V reflection; SpudLib builds the same
  target on Metal and D3D12, so `CMakeLists.txt` only builds it when no
  target exists yet. libdeflate is pinned on
  purpose: its DEFLATE output is part of what makes an archive byte-identical.
  Every fetch fallback names a tag or a commit, never a branch, so a build
  without `../.deps/` is the same every time. glaze is pinned to the commit
  of its release v9.0.0, and `../.deps/glaze` is checked out at that tag; to
  upgrade, move the two together, to a release.
- Don't set `CMAKE_MSVC_RUNTIME_LIBRARY` here; the workspace root sets it once.
- Aprend's own shaders are GLSL in `shaders/`, compiled by glslc at build time
  on every platform, Apple included, and embedded in the library as C arrays
  (`-mfmt=c`, into `aprend_shaders/` in the build directory). A new shader is
  added to `APRICOTFIELDS_SHADERS` and included by `src/render/aprendbuiltin.cpp`,
  which makes it into the instance's built-in objects on first use. Nothing
  is written next to the executable and nothing is loaded at run time.

## Saying what has and hasn't run

Much of this repo was written on one platform. The TODO files record exactly which
platforms have compiled and run each module. When you write or change code, update
that record truthfully: "written, not compiled" and "builds on macOS, Windows and
Linux never built" are useful; an unqualified "done" is not.
