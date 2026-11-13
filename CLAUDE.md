# Rabbittus

The general-purpose engine layer of Apricot: rendering (Rend), math, archives,
audio, sync. See `../CLAUDE.md` (the `eqdev` workspace root) for how this repo fits
into the larger architecture; this file covers conventions specific to working
inside `Rabbittus` itself.

## The one rule everything else follows

**General purpose: a module knows nothing about what its caller is doing with it.**
Rabbittus Archive doesn't know what an archive holds, Rabbittus Sync doesn't know what is being kept
in step, Rabbittus Math doesn't know what a mesh is or why a ray is cast. Every header says
so in its opening comment, along with what the module deliberately does *not* do.

- No application-domain concept, name or assumption in here - CAD, BIM or any
  other. If a change needs to know what the application is, it belongs above this
  layer.
- **Rabbittus doesn't know who calls it.** Nothing here - code, comments,
  headers, docs - names a caller or assumes which one it is. Write "the caller".
- What the bytes, ids, masks and keys *mean* is the caller's. Take them as opaque
  values and hand them back unchanged.
- A new capability that only one caller wants is still written for any caller, or
  it goes in that caller instead.

## Module map

| Module | Header | Prefix | Notes |
|---|---|---|---|
| Core | `rbtcore.h` | `RBT_RESULT`, `RBT_`, `rbt_`, `RABBITTUS_VERSION` | The version, the shared result enum, struct bases and debug names |
| Rend | `render/rbtrend*.h` | `rbtrend_` / `RBTREND_` | Renderer over Carrottus GPU. Older code, mid-overhaul |
| Rabbittus Math | `rbtmath.h`, `math/rbtmathbvh.h`, `math/rbtmathpoly.h`, `math/rbtmatherror.h` | `Rbt*` types, `rbtmath_` / `RBTMATH_` | CPU ray casting, convex-volume tests, and spheres, capsules and boxes overlapped and swept (one GJK), against meshes, instance BVHs changed in place (set, refit, rebuild) and BVHs of solid boxes, double precision (2026-11-06); exact plane polygons on an integer grid (booleans, checks, per-edge offsets, weighted straight skeletons, triangulation); unrelated to Rend |
| Rabbittus Archive | `archive/rbtarchive.h` | `rbtarchive_` / `RBTARCHIVE_` | ZIP through caller callbacks; no I/O of its own. Entries whole or streamed (zlib-ng for streamed DEFLATE, libdeflate for whole), raw entries, updating, strict reading (`RBTARCHIVE_TODO.md`, items 1-10, 2026-10-30 on macOS) |
| Rabbittus Audio | `audio/rbtaudio.h`, `audio/rbtaudiocues.h` | `rbtaudio_` / `RBTAUDIO_` | Mixer (with a lookahead limiter and dithered 16-bit output), cues and sound banks; WAV, FLAC, Ogg Vorbis and Ogg Opus decoding; resampling and Ogg Opus encoding offline (item 17, 2026-11-06 on Windows); streamed sounds (FLAC, Vorbis, Opus through a caller's source, each real voice decoding into its own ring on the caller's scheduler; built 2026-10-31 on Windows, `RBTAUDIO_TODO.md` item 15); spatial audio (voices placed in the caller's world, heard from the nearest of its listeners through shared attenuations - distance curves, cones, doppler, a priority offset - worked out in the update; built 2026-11-08 on Windows, item 16) |
| Rabbittus Sync | `sync/rbtsyncnet.h` | `rbtsync_` / `RBTSYNC_` | Sessions, channels, snapshots over Carrottus Net |
| Rabbittus Jobs | `jobs/rbtjobs.h` | `rbtjobs_` / `RBTJOBS_` | A scheduler interface the caller implements (enqueue a function over a range of items, wait for it; Box2D v3's shape), with groups chained on (tasks that enqueue tasks into their group, waited for together; Grand Central Dispatch's `dispatch_group`; built 2026-11-06 on Windows), and a reference pool of threads a caller may create; the one place Rabbittus starts threads on request. Card L12, built 2026-10-30 on macOS (`RBTJOBS_TODO.md`) |
| Rabbittus Text | `text/rbttext.h` | `rbttext_` / `RBTTEXT_` | Faces, fallback chains, shaping and line layout, MTSDF atlas, single-stroke faces; no file I/O, no GPU. Over HarfBuzz, msdfgen, SheenBidi and libunibreak; built 2026-10-29 on macOS and Windows (MSVC: tests, the cross-platform bits test with macOS's CRCs, ASan, 10 minutes' fuzzing); Linux never built (`RBTTEXT_TODO.md`) |

`rbtrend.h` is Rend's umbrella: it includes every header in `render/` (fifteen) and
nothing of the other modules, and no header includes all of Rabbittus (card A27,
2026-10-28; it was `rabbittus.h` until 2026-10-29). `check.sh umbrella` checks it
reaches every header in `render/`. Callers include the module header they need.

Each module that has unfinished work keeps it in `AP<MODULE>_TODO.md` at the repo
root. Read the module's TODO before changing it, and update it in the same change
when you finish or add an item.

## Two generations of code - match the one you're in

Rend is older and written differently from the four newer modules. Don't carry
one style into the other. Its contract is the newer modules' since 2026-10-30
(`RBTREND_TODO.md`, "The contract conversion", cards A2, A10, A14): every call
that can fail returns `RBT_RESULT`, a creation hands its handle through `out_`,
and the reason goes into a per-call `RBTREND_ERROR`; nothing prints but the
Debug build's check of overlapping calls. Its layout and allocation are still
its own.

| | Rend (`src/render/`) | Rabbittus Archive, Rabbittus Audio, Rabbittus Sync, Rabbittus Math |
|---|---|---|
| Indentation, braces | tabs, attached braces | 4 spaces, Allman braces |
| Creating a handle | returns `RBT_RESULT`, handle through an `out_` parameter (set to null on failure) | the same |
| Allocation | `malloc` + in-place construction, `goto` cleanup | `new (std::nothrow)` and `delete`, then a `try` block - not the convention any more, see "Code conventions" |
| Failure detail | per-call `RBTREND_ERROR` (`rbtrend_fail`, `rbtrend_fail_cr`; a compile's and a submit's through `rbtrend_cmd_report` / `rbtrend_report`) | per-call `AP<MODULE>_ERROR` message struct |

Public headers use tabs in both generations.

`.clang-format` describes the Rend style. Running it over a newer-module file
rewrites the whole file, so don't run clang-format on a file unless asked; format
what you touch by hand to match its neighbours.

## Writing a new module, or a new call in a newer module

Follow `rbtarchive.h` and `rbtsyncnet.h`, which state these rules in a "Conventions"
block near the top. A new header gets the same block.

- **Results.** Every call that can fail returns `RBT_RESULT`. Use the general codes
  first; add an `RBT_RESULT_<MODULE>_*` code to `rbtcore.h` only for a failure the
  caller would branch on, in the module's own numbered block (Rabbittus Archive 1001+,
  Rabbittus Sync 2001+, Rabbittus Text 6001+; struct types Rabbittus Jobs 7000+). Never renumber an
  existing code.
- **Error messages.** A call that can fail for a reason worth a sentence takes an
  `AP<MODULE>_ERROR *` (NULL to skip). It is per call, not stored on the object, so
  reads a header allows to overlap (an archive reader's) stay safe.
- **Struct bases.** Every public struct that may grow starts with an
  `RBT_STRUCT_BASE base` (`RBT_STRUCT_BASE_OUT` for one Rabbittus fills in),
  with its own `RBT_STRUCT_TYPE_*` in `rbtcore.h`, in the module's block; every
  other public struct, in every module, Rend and `rbtmath.h` included, has a
  `/* Frozen: <why> ... */` line above its `typedef`. `rbtcore.h`, "Struct bases", has the rules. A released layout never
  changes: a new field is a new struct chained on. A call checks a based argument
  with `rbt_struct_check` / `rbt_struct_out_check` (`src/rbtstructbase.hpp`)
  right after its null check, and returns what it gives. This is Carrottus's
  scheme (`CR_STRUCT_BASE`) with Rabbittus' own types; it replaced a
  leading `struct_size` on 2026-10-19, and Rend took it on 2026-10-20
  (`rbtrendcontext.h`, "Struct bases", has how Rend applies it). The
  workspace's `check.sh struct-bases` checks the headers' side: based or
  frozen, base first, no `#if` in a struct, no based struct held by value,
  and each type registered in `RBT_STRUCT_TYPE` as the right kind and used
  in `src/`.
- **No defaults in a desc.** A limit or a time left at 0 is refused with
  `RBT_RESULT_INVALID_ARGUMENT` unless the field's comment says what 0 means.
- **Validate every argument before allocating anything**, then allocate (by hand,
  per "Code conventions"), then fill in inside a `try`. `rbtmath_mesh_bvh_create`
  shows the order; its `new (std::nothrow)` is the part not to copy.
- **No C++ exception leaves a C entry point.** Internal helpers may throw
  `std::bad_alloc` (they say so in a comment); every `extern "C"` function that can
  reach one catches it and returns `RBT_RESULT_OUT_OF_MEMORY`, freeing what it built.
- **Destroy accepts null.** Value getters on a null handle return 0.
- **Threads are part of the contract.** Every object is used by one caller at a
  time, from any thread, and different objects are independent: `rbtcore.h`,
  "Threads", states it once for the library (the workspace's "Threading", card
  X3). The rule is serialization, not thread identity: never write "the thread
  that created it". A call that is an exception (callable from any thread at any
  time, a read that may overlap others, a wake, code that runs on another
  thread) says so on a line beginning `Thread:` in its own comment; a header's
  `@par Threads` holds only what is particular to that module. Don't add a
  callback that runs on a module's own thread; Rabbittus Sync delivers events by polling
  for that reason.
- **A new call on an object opens with the Debug overlap check**
  (`RBT_ENTER`, `src/rbtoverlap.hpp`; Rend's `RBTREND_ENTER`), right
  after its handle's null check, on the object or the unit its header
  names - unless it is a destroy or marked `Thread:`. The object's struct
  holds an `rbt_overlap`. Nothing is checked in Release.
- **A destroyed handle is refused in Debug** (card X16, 2026-10-30;
  `src/rbtstale.hpp`). Every handle struct holds `RBT_STALE_TAG` after
  its `debug_name` (live from construction, dead from its destructor), a
  destroy that defers its free through the release queue writes it dead
  with `RBT_KILL` at the call, and every public call checks each handle
  right after its null check, its own `if`, refusing as the null check
  does: `if (RBT_STALE(x, "call", "x")) return <the null check's
  result>;`, which prints one line. Release has no field and no check. A
  new handle type gets the tag and a factory in
  `tests/misuse/misuse_factories.h` (Rend's in `../rabbittustests`).
  Since card X27 (2026-11-08), a Debug destroy of a handle with an owner
  ends in `RBT_RETIRE(owner, obj, T)` in place of the destructor and
  `free`: the block is kept in the owner's `RBT_QUARANTINE` until the
  owner is destroyed, so no new object lands on the dead tag. The owner
  is the Rend instance for every Rend handle, the reader for an
  Rabbittus Archive entry reader; every other handle is a root and is freed at
  once, as is a block a create frees on failure (`rbtstale.hpp` has the
  rule). A deferred destroy still waits for the GPU first: the release
  function retires. Release frees at once.
- **Platform gaps come from Carrottus's `CR*_EXT_*` macros**, never from a
  platform's name, and surface as `RBT_RESULT_UNSUPPORTED`.

## Containers: eqstd (decided 2026-10-28)

New containers and strings in `src/` come from `eqstd` (`../eqstd`, its
`TODO.md`; linked PRIVATE since 2026-10-29, used by Rabbittus Text): `eq::array`
for plain data, `eq::vector` and `eq::string` for the rest, never in a
public header. Old code converts as it is changed. Whether every desc takes
a caller's allocator waits for the 1.0 room check (card X15, decided
2026-11-12 for Linxer only; here, a chained struct then); until then they use the system
heap.

## Code conventions

- **Argument checks are written line by line: one `if`, one condition, one
  return.** They sit at the top of the function, in parameter order, before any
  allocation or call into Carrottus, and each reports exactly what was wrong: its own
  `RBT_RESULT`, and its own message where the call takes an `AP<MODULE>_ERROR`:

  ```cpp
  if (d.max_backlog_size == 0)
      return fail(error, RBT_RESULT_INVALID_ARGUMENT, "max_backlog_size is 0");
  if (d.handshake_timeout_ms == 0)
      return fail(error, RBT_RESULT_INVALID_ARGUMENT, "handshake_timeout_ms is 0");
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
  rbtrend_buffer_t *object = (rbtrend_buffer_t *)malloc(sizeof(rbtrend_buffer_t));
  if (!object)
      return nullptr;
  object = new (object) rbtrend_buffer_t();
  ```

  It is destroyed the same two steps in reverse: the destructor called explicitly,
  then `free`.

  ```cpp
  object->~rbtrend_buffer_t();
  free(object);
  ```
- **A type with no constructor:** `calloc`, check the pointer, and that is all.
  The zeroed memory is its initial state, and nothing is constructed. It is
  destroyed with `free` alone.

  ```cpp
  RBTREND_UNIFORM *uniforms = (RBTREND_UNIFORM *)calloc(count, sizeof(RBTREND_UNIFORM));
  if (!uniforms)
      return nullptr;
  ```
- "Has a constructor" includes a type that only has members which do: a struct
  holding a `std::vector`, `std::string` or `std::thread` must be constructed in
  place and have its destructor called, or those members are never set up or
  released. That is every handle struct in the newer modules, and every Rend
  handle struct (they declare constructors). The public C structs - descs,
  `RBTREND_UNIFORM`, the `Rbt*` math types - are plain.
- A function that returns `RBT_RESULT` returns `RBT_RESULT_OUT_OF_MEMORY` for the null
  pointer. Constructing in place doesn't remove the `try`: filling the object in
  can still throw `std::bad_alloc`, and the `catch` then calls the destructor and
  `free`, in that order.
- Every allocation has exactly one matching release, on every failure path as well
  as in the destroy call.
- Rend already does this, by hand or through the `RBTREND_MALLOC__T` /
  `RBTREND_CONSTRUCT__T` / `RBTREND_DESTRUCT__T` macros in
  `src/render/rbtrend_internal.hpp`; a few of its plain arrays use `malloc` where
  this says `calloc`. Rabbittus Archive, Rabbittus Audio, Rabbittus Sync and Rabbittus Math don't follow it yet:
  they use `new (std::nothrow)` and `delete`. Write new code this way; convert an
  old allocation only when you are already changing that function, and convert
  both ends together - memory from an allocating `new` must never reach `free`,
  nor `malloc` memory reach `delete`.
- About half of the existing checks are joined (`if (!instance || !desc)`), mostly
  in Rend. Write new checks line by line; split an old joined one only when you
  are already changing that function.
- **Data is plain structs, kept in arrays, not objects.** Behaviour is free
  functions over plain structs, as at the C boundary; inside `src/` too:
  - The element of an array a frame iterates (Rend's commands, a draw
    list's draws and batches, the pools' range tables) is trivially copyable
    and standard layout: `RBT_PLAIN_DATA(T)` (`src/rbtplaindata.hpp`), a
    static_assert beside its type. `./check.sh plain-data` fails on such an
    array whose element has none. A record a per-frame loop reads also has a
    size budget, `RBT_HOT_RECORD(T, bytes)` in cache lines, so a field
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
- **A named enum is `typedef enum NAME RBT_ENUM_BASE {`** (`rbtcore.h` says why): C++ then
  sees it with a fixed `uint32_t` underlying type, so a value a C caller stores
  outside its enumerators is one the check refuses, not undefined behaviour to
  read. UBSan's `-fsanitize=enum` caught the plain form (2026-10-20).
- Opaque handles are `typedef struct <name>_t *<name>;`. The struct is defined only
  in the module's `src/<module>/*_internal.hpp`, which nothing outside that
  directory includes.
- Lower-case for handles and functions (`rbtsync_session`), upper-case for value
  structs, enums and constants (`RBTSYNC_SESSION_DESC`, `RBTSYNC_CHANNEL_ORDERED`).
- Header comments are the contract: what the call does, its preconditions, its
  failures. Implementation notes are `//` comments in the `.cpp`.
- Every public header is Javadoc-style Doxygen (since 2026-10-25; Rabbittus Archive,
  Rabbittus Audio, Rabbittus Sync, Rabbittus Math and `rbtcore.h` were `/* */` with `[brackets]`
  before): a `@file` block with the header's `@par Threads` where it has a
  threading rule, then `/** */` on every declaration, struct member and
  enumerator, with `@brief`, `@param[in]`/`[out]`, `@return`, `@warning` and
  `@see`. A parameter is `@p name` in prose, a function `name()`, a member
  of the same struct `#member`, and a field or command member in backticks.
  `rbtrendimages.h` is the model. `/* Frozen: ... */` lines and section
  banners stay plain comments. The `Doxyfile` at the repo root builds them;
  it reports no warnings, checked by `./check.sh doxygen`.
- Math types crossing the API are the `Rbt*` structs from `rbtmath.h`. They are
  laid out identically to their GLM counterparts so `to_glm()` can reinterpret them
  in place; changing a member's order or type breaks that silently.
- Rend's descs and lit scene settings start with an `RBT_STRUCT_BASE base`,
  as the newer modules' do; its calls refuse a wrong base with
  `rbt_struct_check`'s code and a message, through `rbtrend_base_check()` in
  `src/render/rbtrend_internal.hpp` (`rbtrend_base_ok()`, which printed, went
  on 2026-10-30). A desc Rend
  builds for its own calls sets its base like any caller's. A getter that
  returns a desc sets the base itself. `RBTREND_COMMAND`'s `_params` is padded
  to a fixed 120 bytes, so the command stays 128 bytes as commands are added.
- A Rend handle struct's first member is its `debug_name` pointer
  (`rbtcore.h` explains why). Keep it first. It, and every desc's
  `debug_name`, exist in every build (since 2026-10-18; before, only under
  `_DEBUG`, which made a desc's layout differ between Debug and Release):
  no build switch changes a public struct's layout or which functions
  exist.

## Rend

- Rend calls Carrottus GPU's backend-agnostic C API only. It never includes a Vulkan,
  D3D12 or Metal header and never branches on the backend at runtime.
- Rend is handed its `crgpu_device` in `RBTREND_INSTANCE_DESC`. It doesn't
  create the Carrottus GPU instance, list adapters, choose one or create the device.
- A Carrottus GPU device has only the features it was created with, so Rend reads
  features from the device (`rbtrend_instance_t::features`), never the
  adapter's `supported_features`, and reads properties and limits from the
  device's adapter. A feature only Rend's own code uses, with a path
  without it, goes in `rbtrend_get_optional_gpu_features()` for the caller to
  ask for. A feature one call needs is named in that call's header comment
  and checked by the call, which refuses in Rend's way rather than passing
  Carrottus GPU's refusal on. `RBTREND_TODO.md` item 39 has the list.
- A choice Carrottus GPU leaves open is either a rendering matter, made in Rend
  explicitly, or one Rend takes from its own caller through a desc. It is never
  pushed back down into Carrottus as a default and never left implicit.
- Carrottus GPU's plan for hardware without dynamic rendering is a caller-owned
  `crgpu_framebuffer` (`carrottus/CLAUDE.md`; designed, not built). When it is
  built, Rend is that owner: `rbtrend_framebuffer` would hold and cache it, so the
  rest of Rend makes one begin/end call either way. Nothing in Rend does this
  yet.
- Picking is not Rend's job. GPU pick-ID buffers were removed on purpose.
- `rbtrendbuffers.h` is one `rbtrend_buffer` for every use (card A14, rewritten
  2026-10-30): a usage mask, `RBTREND_BUFFER_WRITES`, and per-frame copies in
  place of buffer sets. Mesh types are a caller's, never Rend's.
- Ray tracing (`rbtrendraytracing.h`, the approved
  `rbtrend_raytracing_draft.h`; built 2026-11-01 on Windows, `RBTREND_TODO.md`
  item 24): BLASes and TLASes are resources, named in instances by slot;
  the four commands record the builds and the traced passes. The
  acceleration structure is a descriptor type of Rend's own traced
  layouts and sets only (`rbtrend_shader_create_own()`,
  `rbtrend_binding_set_create_traced()`): a caller's shader, layout or set
  still refuses it. Its state is `rbtrend_rt_state`
  (`src/render/rbtrendraytracing_internal.hpp`, which says who writes each
  GPU-visible buffer and when); its shaders are `shaders/rbtrend_rt_*.slang`,
  the traced ones built for shader model 6.5 and SPIR-V's ray queries
  (`RABBITTUS_TRACE_SHADERS`).

## Reference: Hazel

Hazel (2025 renderer; Yan Chernikov / Studio Cherno) is a major reference point
for building this repo, and the engine this repo's competency is compared to:
what Hazel can do is the bar for what Rabbittus should be able to do. It is a
C++ engine whose reference copy is at `../.refs/HazelRenderer2025/Hazel/Hazel`
(source under `src/Hazel/`), read-only and not part of this repo; see
`../CLAUDE.md`, "External references".

**It is a relative reference: Hazel answers "how has this been solved", this file
answers "how is it written here".** Read Hazel for the technique, then write it
under this repo's rules. Where the two disagree, this file wins, every time.
"Hazel does it this way" is never a reason to break a rule here, and nothing is
copied across in Hazel's shape to be converted later.

Where it maps: `Hazel/Renderer` onto Rend most directly (pipelines and their
specifications, render and compute passes, framebuffers, command buffers, uniform
and storage buffer sets, shaders, materials, the scene renderer).

**Carrottus GPU, not NVRHI.** Hazel renders through NVRHI; this repo renders through
Carrottus GPU's C API and nothing else. Read Hazel's `nvrhi::` calls and its
`Hazel/Platform` code for what they achieve, then do it with what Carrottus GPU
exposes. NVRHI is never added as a dependency, and a capability NVRHI has that
Carrottus GPU lacks is not a reason to change Carrottus GPU to resemble it: the comparison
with Hazel is made at this layer only.

What a Hazel idiom becomes here:

| In Hazel | Here |
|---|---|
| `class Pipeline : public RefCounted`, held as `Ref<Pipeline>` | An opaque handle, `typedef struct <name>_t *<name>;`, with a create and a destroy call and one owner. No reference count crosses the API |
| `static Ref<X> Create(const XSpecification &)` | `<prefix>_x_create(...)` taking a desc pointer; how the handle comes back follows the generation of code you're in |
| `XSpecification` holding `std::string`, `Ref<>` members and default member values | A plain C desc: `<stdint.h>` types, handles, `const char *`. A struct base first in the newer modules, and no defaults |
| `static` state and getters on `Renderer` | State lives on the instance handle the caller passes in. No globals |
| `Renderer::Submit` lambdas and `RT_` functions | No closure crosses the API. Which calls may overlap is written in the header (`rbtcore.h`, "Threads") |
| `HZ_CORE_ASSERT(false, ...)` on a bad input | Argument checks line by line, each returning its own result |
| `Ref<X>::Create`, `new` | `malloc` and in-place construction, or `calloc`, per "Code conventions" |
| `namespace Hazel`, `PascalCase` methods, `m_` members | The module prefix; lower-case handles and functions, upper-case value structs and enums |
| `glm::` and `std::` types in a header | `Rbt*` structs and C types; no C++ type in `include/` |
| A renderer that knows its scene, assets and editor | No application-domain concept. The caller hands over what to draw |

The sections that hold these rules are "Writing a new module...", "Code
conventions" and "Public headers" above. A Hazel idiom not in the table is
handled the same way: find the rule it meets and follow the rule.

## Tests

Headless, no GPU, no window: `tests/ap<module>*_tests.*`, one or more executables per module,
registered with `add_test` behind `RABBITTUS_BUILD_TESTS`.

- Tests for a C header are written in C on purpose, to prove the header compiles
  and links as C. Keep them `.c`. The one exception is `tests/rbtarchive_tests.cpp`,
  which is C++ (it uses `std::vector`).
- No framework: a `CHECK(condition)` macro, a `failures` counter, `test_*`
  functions called from `main`, exit code 1 on any failure.
- A contract written before its implementation (`../ROADMAP.md`, section 1)
  ships its behavioural tests with it: a test whose call returns
  `RBT_RESULT_NOT_IMPLEMENTED` reports itself pending and stops, so the
  implementation turns it on without editing it (`tests/rbttext_tests.c`).
- A new module ships with its tests file; a new call ships with a test in the
  existing one.
- `tests/rbt_misuse_tests.c` calls every public function with zeroes, and
  in Debug (not under a sanitizer) every one taking a handle first with
  that handle destroyed (X16): it is generated from `abi.txt`
  (`python3 tests/misuse/make_misuse_tests.py` after
  `./check.sh --update-abi`; the factories are `tests/misuse/misuse_factories.h`),
  never edited by hand.
  `tests/rbtnoalloc_tests.c` holds each module's steady state to no heap
  allocation of ours, counted by `tests/alloctrace.h` (which puts each
  allocation down to the code that asked, the runtime's and drivers' apart;
  the libraries keep identical copies, `check.sh alloc-trace-copies`); a
  new per-frame path gets a scenario there, and Rend's frame is
  `../rabbittustests`' `test_frame_allocations`. Brute-force comparison against a slow obvious implementation is the
  preferred check for anything algorithmic (`test_mesh_against_brute_force`).

When asked to verify, on macOS: configure with the `rabbittus-macos-metal`
preset, build with the `rabbittus-macos-metal-debug` build preset, then run
`ctest --test-dir build-macos-metal -C Debug`. (The presets are multi-config;
there is no separate `-debug` configure preset any more.)

**Fuzzing.** `tests/fuzz/` holds a libFuzzer target per reader of bytes the
caller can't vouch for; today `rbtarchive_fuzz` (Rabbittus Archive's reader over an
archive in memory, which also writes back what it read and reads that
again), `rbttext_fuzz` (font files, Hershey fonts and saved atlases) and
`rbtaudio_fuzz` (WAV, FLAC, Vorbis and Opus files).
`tests/fuzz/rbtfuzz.hpp` says how they are built.

- Every normal build runs each as a ctest, `<target>_regressions`, replaying
  `tests/fuzz/corpus/<target>/` and `tests/fuzz/regressions/<target>/`. An
  input the fuzzer crashes on is fixed, then kept in the regressions folder,
  as small as it can be made.
- To fuzz, on macOS: `cmake --preset rabbittus-macos-metal-fuzz` and
  `cmake --build --preset rabbittus-macos-metal-fuzz-debug` (Homebrew's
  LLVM; Apple's Clang has no libFuzzer; on Linux
  `rabbittus-linux-vulkan-fuzz` with the system Clang), then from a
  scratch folder `build-macos-fuzz/Debug/<target> <scratch corpus>
  tests/fuzz/corpus/<target> -max_total_time=900`. New seeds go in with
  `<target> -merge=1 tests/fuzz/corpus/<target> <folder>`.
- On Windows: `rabbittus-windows-d3d12-fuzz` (LLVM's clang through its
  GNU-style driver, RelWithDebInfo; `../claude_made_decisions.md`,
  2026-10-29), run there 10 minutes on 2026-10-29 each, `rbtarchive_fuzz`
  and `rbttext_fuzz`. Build and run it from a Visual Studio developer
  prompt with `C:\Program Files\LLVM\bin` on `PATH`, and LLVM's
  `lib\clang\<version>\lib\windows` too to run a target.
- A new module that reads outside bytes gets a target.

## Build

- A new source or header file is added to the `add_library` list in
  `CMakeLists.txt` by hand; there is no glob.
- Third-party code is vendored under `../.deps/` (glm, libdeflate, zlib-ng
  for Rabbittus Archive's streamed DEFLATE since 2026-10-30, glaze,
  SPIRV-Cross, Clipper2, earcut.hpp; Rabbittus Text's HarfBuzz, msdfgen, SheenBidi
  and libunibreak; Rabbittus Audio's decoders, dr_flac, stb_vorbis, libogg, libopus
  and opusfile, since 2026-10-30) and linked `PRIVATE` unless a public
  header needs it. Where a vendored file writes a global or has a bug a
  fuzzer found, it is read as it is and a changed copy is written into the
  build directory at configure time, each change named and checked to
  still apply (stb_vorbis and dr_flac, as Hosek and Wilkie's data); the
  file itself is never edited.
  Rabbittus Text's four are built here from their sources as static libraries of
  their own (`rbttext_*`), pinned by tag and fetched when `../.deps/` has no
  copy, with floating-point contraction off for the reason Rabbittus Math's polygons
  have it (`RBTTEXT_TODO.md`, "Dependencies"). Clipper2 and earcut.hpp are
  used as sources only, through
  `declare_vendored_or_fetch_sources`: Clipper2's library sources are built
  here as `rbtmath_clipper2`, and earcut.hpp is a header. Both are pinned, and
  both and `src/math/rbtmathpoly.cpp` are built with floating-point
  contraction off, because what Rabbittus Math's polygons compute must be the same
  bits on every platform.
  SPIRV-Cross is not in the library (card X18, 2026-11-01): Rend's
  reflection is made at build time by `tools/rbtrend_reflect`, a program of
  its own for the build machine (built by `cmake/rbtrend_slang.cmake`
  through ExternalProject), over SPIRV-Cross's core, which only it and
  `rbtrendreflect_tests` link. libdeflate is pinned on
  purpose: its DEFLATE output is part of what makes an archive byte-identical.
  Every fetch fallback names a tag or a commit, never a branch, so a build
  without `../.deps/` is the same every time. A tag is cloned shallow and a
  commit whole (`declare_vendored_or_fetch_sources`): a shallow clone holds
  only the tip of what it names, so it lost stb's pinned commit once stb
  moved on, and every fresh build tree without `../.deps/stb` failed to
  configure (found on Windows 2026-11-03). No fetch clones a submodule:
  dr_libs' test-only `tests/external/miniaudio` stopped cloning, and every
  fresh build tree without `../.deps/dr_libs` failed to configure (found on
  Linux 2026-11-04). glaze is pinned to the commit
  of its release v9.0.0, and `../.deps/glaze` is checked out at that tag; to
  upgrade, move the two together, to a release.
- Don't set `CMAKE_MSVC_RUNTIME_LIBRARY` here; the workspace root sets it once.
- Rend's own shaders are Slang in `shaders/` (card A29; `<stem>_vert.slang`,
  `_frag`, `_comp`, its entry point named `<stem>`, never `main`), compiled
  at build time by Carrottus's shader step (`cr_add_shader()`, card X18),
  which `cmake/rbtrend_slang.cmake` wraps to add only the package, to SPIR-V,
  DXIL on Windows and a .metallib on Apple platforms, and embedded in the
  library as one
  `RBTREND_SHADER_PACKAGE` each (`<stem>.package.inc`, into `rbtrend_shaders/`
  in the build directory): the shader's reflection table, written from its
  SPIR-V at build time by `tools/rbtrend_reflect` and checked whole by
  Rend before it is read (`src/render/rbtrendreflect_table.hpp`, its
  layout versioned on both sides at once), and the code in the format the
  build's device takes. A new shader is added to `RABBITTUS_SHADERS`
  (a module it imports to `RABBITTUS_SHADER_MODULES`) and made with
  `rbtrend_shader_create()` from its package by the source that uses it, on
  first use. Nothing is written next to the executable and nothing is
  compiled or loaded at run time. Clip space is +Y up on every backend
  (card S19): a matrix Rend builds into clip space says so. A shader
  declares each set it reads exactly as its layout holds it, read or not
  (Carrottus GPU's "Every binding of a set", card S27): through the set's shared
  bindings module where it has one (`rbtrend_lit_bindings.slang`,
  `rbtrend_lit_material.slang`, `rbtrend_batch_inputs.slang`), and with no
  binding no shader of the layout reads, where the layout is made from the
  shaders. Metal refuses a module that doesn't.

## Saying what has and hasn't run

Much of this repo was written on one platform. The TODO files record exactly which
platforms have compiled and run each module. When you write or change code, update
that record truthfully: "written, not compiled" and "builds on macOS, Windows and
Linux never built" are useful; an unqualified "done" is not.
