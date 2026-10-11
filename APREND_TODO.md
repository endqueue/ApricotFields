# Aprend TODO

`include/render/aprend*.h` is a renderer over SpudGPU: buffers, textures and
their views, samplers, framebuffers, shaders and graphics pipelines, binding
layouts and sets, a
recorded command list that is compiled and then submitted, and a swap chain. It
is the older generation of code in this repo (see `CLAUDE.md`, "Two generations
of code") and is mid-overhaul. This file holds what is known to be wrong or
unfinished in it.

## State of the code

- **Compiled on macOS on 2026-10-10; run there in part.** Everything in this section
  built with no errors and no warnings: Aprend with Clang, against
  SpudGPU's Metal backend, through trellislib's `trellislib-macos-metal`
  preset. That is two builds the same day: the first covered the three
  lists that follow, the second everything after them. Each needed one fix
  outside this repo. The first: `spudgpu.h` had lost the declarations of the
  command allocator and command list calls and `spudgpu_queue_wait_idle` in
  the fence rewrite, and they were put back. The second: a rename had run
  over the cube view constants in SpudGPU's Metal image backend.
  Still to do, and what "compiled" below doesn't mean:
  - One application has run on it, on macOS over Metal, 2026-10-11, and
    drew correctly. That exercises what that application uses and no more:
    instance creation, shaders and graphics pipelines (one of them with a
    push constant range, created and never drawn with), a uniform buffer
    in a binding set, vertex buffers written directly (unified memory) and
    destroyed through the release queue, 2D color and depth views, passes
    to a texture, and the swap chain. Each "none of it has run" below
    still stands for everything else: no push constant set, nothing
    staged, no compute, no indirect draw, no view of part of a texture, no
    layered pass. Nothing checked the release queue for an early release
    beyond the picture being right.
  - The repo's headless tests pass, and none of them touches Aprend.
  - Not compiled with GCC, and nothing has been built or run on Linux.
- **Compiled and run on Windows on 2026-10-11.** Everything in this section
  down to "View aspects and stencil sampling" built there, with the SpudGPU
  it depends on, and one application ran on it and exited with code 0. That
  is the first build of this code outside macOS and the first of a SpudGPU
  backend other than Metal. Which backend, D3D12 or Vulkan, is not recorded
  here, and the other is still not compiled. As on macOS, it exercises what
  that application uses and no more (the list above); whether the device
  had memory of its own, which is what would have run the staged path, is
  not recorded either.
- **The changes of 2026-10-09 are compiled on macOS only**:
  - `aprend_index_buffer_get_stride`, `aprend_buffer_element_type_get_size` and
    `aprend_buffer_layout_get_element_index` defined (the header declared them
    and nothing defined them).
  - `aprend_uniform_buffer_update_by_name`, `aprend_texture2d_resize` and
    `aprend_texture3d_resize` implemented (they returned false).
  - `aprend_send_command` copies the arrays of `SET_VERTEX_BUFFERS`,
    `SET_VIEWPORTS` and `SET_SCISSOR_RECTS` into the command list.
  - Texture update and readback and the framebuffer clears write a texture's
    tracked layout only after their submission succeeds, and leave the texture
    in the layout they found it in.
  - Storage buffers are created host-visible and host-coherent, and
    `aprend_storage_buffer_update` checks its range against the buffer's size.
  - `aprend_graphics_pipeline_desc` takes `_cull_mode` and `_front_face_ccw` in
    place of `_backface_culling`.
- **The changes of 2026-10-10 are compiled on macOS only**:
  - `aprend_uniform_set` is replaced by `aprend_binding_layout` and
    `aprend_binding_set`, over every SpudGPU descriptor type and all four set
    slots (was item 9). A layout owns the pools its sets come from, 32 sets to
    a pool, and a destroyed set's descriptor set is reused.
  - `aprend_sampler`, and `APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE` for a
    view that is sampled.
  - `APREND_COMMAND_SET_BINDING_SET` replaces `SET_UNIFORM_SET`. Sets are bound
    at the draw, and a pass's sampled and storage textures are moved into
    their layouts before the pass begins. Layout tracking covers 3D textures.
  - None of it has run: no texture has been sampled and no storage buffer or
    storage image has been read through a set.
- **The move to SpudGPU's memory kinds, 2026-10-10, is compiled on macOS
  only**. `SPUDGPU_MEMORY_FLAGS` is gone and every buffer names one
  `SPUDGPU_MEMORY_KIND`; where the bullets below say host-visible or
  device-local, read the kind:
  - Uniform buffers and every staging buffer written by the CPU are `UPLOAD`;
    the texture readback staging buffers are `READBACK`.
  - A vertex or index buffer is `UPLOAD` on a device whose memory is the
    system's and `DEVICE` with a staging buffer otherwise, as before.
  - A storage buffer is `DEVICE_MAPPABLE` where
    `SPUDGPU_DEVICE_PROPERTIES::device_mappable_memory` says the device has
    it, and `DEVICE` with a staging buffer otherwise (was item 2).
    `aprend_instance_create` reads the property beside `unified_memory`.
  - Every write through a mapping is followed by `spudgpu_flush_buffer`:
    `aprend_buffer_store_write`, `aprend_uniform_buffer_update` (whose buffer
    stays mapped) and both texture update paths. The readbacks already called
    `spudgpu_invalidate_buffer`.
  - `memory_flags` is gone from `aprend_texture2d_desc` and
    `aprend_texture3d_desc`: a SpudGPU image has no memory kind.
  - SpudGPU's heap and resource flags are gone too, and with them any
    zeroing: a new buffer's or image's contents are undefined on every
    backend. `aprend_uniform_buffer_create` zeroes its block for that
    reason. A vertex, index or storage buffer created with no data stays
    unwritten, as its header already said, and so does every texture level.
- **A second set of changes on 2026-10-10 is compiled on macOS only**. The
  numbers are the items they closed:
  - Vertex, index and uniform buffers ask for host-visible and host-coherent
    memory, as storage buffers do (was item 2).
  - Desc fields nothing read are gone (was item 4): `store_locally`,
    `initial_data` and its pitches on both texture descs, `_line_width` on the
    pipeline desc, `swap_chain_target` on the framebuffer desc, and the app
    and engine names and versions on `aprend_instance_desc`, which is now the
    device alone. A view's `APREND_TEXTURE_VIEW_TYPE` is checked against the
    texture's usage bits, and a sampled view of a depth and stencil format is
    of the depth alone.
  - `_vertex_entry_point` and `_fragment_entry_point` on the pipeline desc,
    required and copied (was item 5).
  - The texture update and readback calls and the framebuffer clears take the
    queue they submit on (part of item 6).
  - Removed: the unused SPIR-V loader, the instance's unused command list, the
    forward declaration of a pipeline layout type, three unused includes.
    `apricotfields.h` includes all seven Aprend headers (part of item 7).
  - Push constants (was item 10): `_push_constant_ranges` on the pipeline
    desc and `APREND_COMMAND_PUSH_CONSTANTS`.
  - Vertex bindings (was item 11): `_vertex_bindings` replaces
    `_vertex_layout`, each binding per vertex or per instance, read from the
    vertex buffer slot of the same number. A draw fails the compile if a slot
    its pipeline reads is empty or holds a buffer of another stride.
  - A pipeline is set inside a pass and lasts until the pass ends.
    `SET_SHADER_PIPELINE` outside a pass, and a draw in a pass that has set
    none, fail the compile. Before, a pipeline carried from one pass to the
    next in Aprend's tracking while the backend had none bound.
  - A pass takes only the binding sets its draws read (was item 32's first
    point).
  - A uniform buffer is sized from its uniforms' offsets, with std140's array
    stride, and `aprend_uniform_buffer_update_by_name` writes an array's
    elements at that stride (item 1's small fix).
  - Texture update and readback take a mip level, and the 2D ones an array
    layer (part of item 3).
  - Uploads follow the device's memory (item 13). Vertex, index and
    storage buffers are host-visible and written directly on a device whose
    memory is the system's. On a device with its own they are device-local,
    with a host-visible staging buffer beside each; a write goes to staging,
    and `aprend_command_list_submit` copies the dirty range across in an
    upload list submitted ahead of the list's own. Uniform buffers are
    host-visible on every device. `aprend_instance_create` reads
    `SPUDGPU_DEVICE_PROPERTIES::unified_memory`, and the copy uses
    `SPUDGPU_RESOURCE_STATE_COPY_DEST`: both are new in SpudGPU, and built
    for Metal only. The vertex and index update calls check their range.
  - Frame pacing (item 12). The instance has a fence; every submission
    Aprend makes (command lists and the immediate operations) signals it
    through `spudgpu_queue_submit`, and `aprend_instance_next_frame` waits on
    it for the frame that last used the index. `aprend_instance_wait_idle`
    waits for everything submitted so far. It rests on SpudGPU's fence being a counter on every backend,
    which was rewritten the same day and is built for Metal only.
  - Buffer sets (item 12). `aprend_instance_desc::frames_in_flight`, a frame
    index moved on by `aprend_instance_next_frame`, and
    `aprend_uniform_buffer_set` and `aprend_storage_buffer_set` with one copy
    for each frame. A binding set entry takes a set in place of a buffer; such
    a binding set keeps a descriptor set for each frame, and a command list
    binds the one of the frame it is compiled in and is refused at submit in
    any other.
  - None of it has run: no push constant has been set, no per-instance buffer
    read, no mip below 0 written, and no staged buffer copied: every device
    this has been near is unified, so the staged path is the least tested
    code in Aprend.
- **Compute, written 2026-10-10 and compiled on macOS only** (item 14):
  - `aprend_shader` accepts the compute stage and remembers its stage; a
    pipeline refuses a shader given as another stage than its own.
  - `aprend_compute_pipeline`: a shader, entry point, binding layouts and push
    constant ranges, over the same `aprend_binding_layout` objects a graphics
    pipeline declares.
  - `APREND_COMMAND_SET_COMPUTE_PIPELINE` and `APREND_COMMAND_DISPATCH`,
    outside a pass. `SET_BINDING_SET` serves both, and `PUSH_CONSTANTS`
    outside a pass writes the compute pipeline's block.
  - Right before a dispatch, the textures of its sets are moved into their
    layouts and its storage buffers and storage images are ordered after
    earlier writes. A pass does the same ordering for the sets its draws
    read; before, a storage image got an image barrier and a storage buffer
    nothing.
  - A binding set records every storage buffer it holds, not only the staged
    ones.
  - None of it has run: no compute shader has been dispatched on any backend.
- **Indirect draws and mesh shaders, written 2026-10-10 and compiled on
  macOS only** (item 15):
  - A storage buffer, or a storage buffer set, created with
    `APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS` is the argument buffer of
    `APREND_COMMAND_DRAW_INDIRECT` and `APREND_COMMAND_DRAW_INDEXED_INDIRECT`,
    whose entries are checked against the buffer's size when the command is
    sent. The same buffer goes in a storage slot for a compute shader to
    fill. There is no indirect buffer type.
  - A command list tracks the state of such a buffer: it is moved to the
    indirect argument state before a pass that draws from it and back before
    a dispatch or pass that has it in a binding set, and
    `aprend_command_list_submit` puts it in the storage state ahead of the
    list's own commands.
  - `aprend_shader` accepts the mesh stage. A graphics pipeline takes a
    `mesh_shader` in place of its `vertex_shader`, has no vertex input, is
    refused on a device without mesh shading, and is run with
    `APREND_COMMAND_DISPATCH_MESH`. A draw command with a mesh shader
    pipeline, or a mesh dispatch with any other, fails the compile.
  - An indexed draw, indirect or not, fails the compile if no
    `SET_INDEX_BUFFER` came before it.
  - `APREND_RENDERING_FLAG_EXECUTES_BUNDLES` and `BEGIN_RENDERING`'s `_flags`
    are gone (item 33).
  - None of it has run.
- **The release queue, written 2026-10-10 and compiled on macOS
  only** (item 12):
  - An instance has a queue of what was destroyed while the GPU might still
    use it. Each entry carries the submit serial current when it was
    retired and is released, front first, once the instance's fence has
    reached it: after each submission, in `aprend_instance_next_frame` and
    `aprend_instance_wait_idle`, at each destroy call, and at
    `aprend_instance_destroy`, which waits first.
  - Every destroy call of a GPU-backed handle goes through it: the four
    buffer types, both texture types, texture views, samplers, shaders, both
    pipeline types, binding layouts and sets, and command lists. None of
    them needs the GPU to be finished any more. Uniform, vertex, index and
    storage buffers and binding sets now store their instance for it.
  - A resized texture's old image, and a 3D texture's old view, are retired
    on it.
  - Texture updates and framebuffer clears are submitted and not waited for;
    their list, allocator and staging buffer are retired. A readback still
    waits.
  - An instance submits on one queue and refuses another.
  - The queue has a mutex and the submit serial is atomic, so a destroy call
    is safe from any thread: it pushes an entry under the lock and releases
    nothing. Releasing is done outside the lock by the thread using the
    instance.
  - None of it has run. The first thing to check is that nothing is
    released early: a release queue that is wrong shows as rare corruption.
- **The thread contract is in the headers** (2026-10-10, comments only):
  `aprendcontext.h` holds the rule and the other six say what is particular
  to them. It describes the code as it is and changes none of it.
- **Views of a range, cubes and per-mip layouts, written 2026-10-10 and
  compiled on macOS only** (items 16 and 17):
  - `aprend_texture_view_desc` replaces the `type` argument of the two view
    create calls: a use, a shape (2D, 2D array, cube, cube array, 3D) and a
    mip and layer range, with `APREND_TEXTURE_VIEW_REMAINING` for "to the
    end". A 2D texture with several layers is viewed as a 2D array; before,
    it was asked for as a plain 2D view, which is invalid on Vulkan.
  - `aprend_texture2d_desc::cube_capable`. A cube is a texture and a view.
  - A texture tracks a layout for each mip of each layer, and a command list
    an entry for each it touches. A view is moved over its range, with one
    barrier where the range is in one layout. Conflicts in a pass or a
    dispatch are by overlap, so two views of one texture that share no mip
    of a layer can be read and written together.
  - The texture update, readback and clear calls move only the mip of the
    layer they touch.
  - A storage image's ordering barrier is SpudGPU's layout barrier from
    GENERAL to GENERAL over the view's range, in place of a pipeline barrier
    over the whole image.
  - SpudGPU changes it rests on, built for Metal only: cube-capable images, D3D12
    state for each subresource, Vulkan's whole-image barrier covering every
    mip and layer, and the storage layout meaning the storage state on D3D12.
  - None of it has run. A view of part of a texture has never been created.
- **Layered passes, written 2026-10-10 and compiled on macOS only**
  (item 16):
  - A view of type RENDER_TAGET or DEPTH_STENCIL may be a 2D array of one
    mip, on a device that reports
    `SPUDGPU_DEVICE_PROPERTIES::layered_rendering` (read once into the
    instance); on any other, creating one fails.
  - BEGIN_RENDERING over 2D array views is a layered pass: the compile
    refuses targets that differ in shape or layer count, and the layer count
    goes to SpudGPU as `spudgpu_rendering_begin_desc::layer_count` (0 for a
    pass over 2D views). Layouts and conflicts needed nothing new: a target
    is already moved, and checked for overlap, over its view's range.
  - SpudGPU changes it rests on, built for Metal only: `layer_count`, the
    `layered_rendering` property (which enables `shaderOutputLayer` on
    Vulkan where the device has it), and Metal pipelines always stating
    their primitive class, which Metal requires in a layered pass.
  - No shader that writes a layer has been through SpudGPU's SPIR-V
    cross-compile to HLSL or MSL, and no layered pass has run on any backend.
- **View aspects and stencil sampling, written 2026-10-11 and not compiled**
  on any platform (item 16):
  - `aprend_texture_view_desc::aspect` (COLOR, DEPTH, STENCIL,
    DEPTH_STENCIL), required and checked against the texture's format and
    the view's type. Before, Aprend chose it from the format, and a sampled
    view of a depth and stencil texture was always the depth.
  - A SHADER_RESOURCE view with the STENCIL aspect is what a shader samples
    the stencil through, as an unsigned integer.
  - SpudGPU changes it rests on, equally unbuilt: named aspect bits and one
    rule for a view's aspect; Metal's stencil view format; and on D3D12 a
    view that keeps a shader resource desc and an unordered access desc
    beside its attachment desc, readable formats for depth and stencil, and
    typeless depth resources. The last three also mean a texture that is
    rendered to and sampled, color or depth, was not correctly sampled on
    D3D12 before; that backend has never been built or run.
  - No stencil has been sampled on any backend.
- **`aprend_binding_set_update`, written 2026-10-11 and not compiled** on
  any platform (item 9): points some of a set's slots at other resources
  and keeps the handle. The set keeps a copy of the entries it was built
  from; an update merges the given ones in, builds a whole new set, swaps
  its contents into the handle, and destroys the new handle, which carries
  the old descriptor sets onto the release queue. Never run.
- **Transient memory, written 2026-10-11 and not compiled** on any platform
  (`aprendtransient.h`, new; items 13 and 23):
  - `aprend_transient_allocate` hands out a span of mapped upload memory
    for the current frame; the caller writes vertices or indices into it and
    binds it with `APREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER` (20) or
    `_INDEX_BUFFER` (21). The GPU reads the bytes where they were written.
  - The arena is on the instance (`aprendtransient_internal.hpp`,
    `aprendtransient.cpp`): a run of chunks for each frame index, bump
    allocated. `aprend_instance_next_frame` rewinds the run of the index it
    moves to, after its wait for that frame's submissions. Nothing is
    tracked for a span.
  - `aprend_instance_desc` gains `transient_chunk_size` (0 is an instance
    with no transient memory) and `transient_memory_limit` (0 is no limit).
    A span larger than a chunk gets one of its own, destroyed at the rewind.
  - Every submission flushes what the frame's spans hold. A compile refuses
    a span of another frame or of an earlier turn of this one, and a submit
    refuses a list that binds spans in any frame but the one it was compiled
    in.
  - The compile now tracks a stride for each vertex slot and whether an
    index buffer is set, in place of the buffer handles, so a slot may hold
    either kind. That touches the draw checks of every list, transient or
    not.
  - Never run. No span has been bound on any backend, and drawing from
    upload memory through a view at an offset is untested on all three.
- **Vertex and index pools, written 2026-10-11 and not compiled** on any
  platform (`aprendbuffers.h`; `aprendpools.cpp`, `aprendpools_internal.hpp`
  and `aprendranges_internal.hpp`, new; item 13):
  - `aprend_vertex_pool` and `aprend_index_pool` own a few large vertex or
    index buffers, the blocks, and hand out ranges of them
    (`aprend_vertex_range`, `aprend_index_range`) that last until freed. For
    a caller with thousands of small meshes: a buffer each is a device
    allocation each, which a device limits in number and in smallest size.
  - A range names its block's buffer and its start. It is drawn as any
    buffer is, with the start in `_start_vertex_location`,
    `_start_index_location` or `_base_vertex_location`; a block is an
    ordinary `aprend_vertex_buffer` or `aprend_index_buffer`, and nothing in
    the command list knows a pool exists.
  - A range is written once, at allocation. That is what makes the write
    safe while the block is being drawn from: the bytes were free.
  - A free is deferred by the instance's fence, in the pool: the range
    carries the submit serial of its free and is handed out again once the
    fence has reached it. It doesn't go on the release queue, since nothing
    is destroyed; the pool looks at what is waiting at each allocate, trim
    and get_usage.
  - Blocks are added one at a time and given back only by the trim calls. A
    range longer than a block has a block to itself. `memory_limit` in the
    desc; no default block size.
  - `aprendranges_internal.hpp` is the list of a block's free elements, with
    nothing of Aprend or SpudGPU in it, and `tests/aprendranges_tests.cpp`
    checks it against one flag an element. That test is written and not
    compiled either.
  - Never run. No range has been allocated, drawn from or freed on any
    backend.
- **No tests of anything that needs a device.** There is no
  `tests/aprend_tests.*`: the repo's tests are headless with no GPU. The one
  part of Aprend that runs without a device has a test
  (`tests/aprendranges_tests.cpp`, the pools' range list).

## Wrong things

### 1. Uniform layouts are declared by hand

`aprend_uniform_buffer_create` sizes the buffer from the caller's offsets, with
std140's 16-byte stride for an array, and places nothing. What is left is that
the offsets, types and counts are the caller's to get right, and nothing checks
them against the shader:

- A uniform declared at the wrong offset is read from the wrong bytes.
- A `mat3`, a struct, or an array of structs has no `APREND_UNIFORM_TYPE`.
- A `vec3` followed by a scalar shares its 16 bytes in std140, which a caller
  working from sizes alone gets wrong.

The fix is SPIR-V reflection: layouts, offsets and block sizes read from the
shader. It belongs with the rewrite `aprendbuffers.h` already says it is due
(item 18).

### 2. A host-visible storage buffer on D3D12

Closed 2026-10-10, compiled on macOS only. A storage buffer written directly is
`SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE`, which SpudGPU reports per device
(`SPUDGPU_DEVICE_PROPERTIES::device_mappable_memory`) and which D3D12 backs
with a custom heap, not an upload heap. A device without it takes the staged
path for storage buffers alone. The D3D12 heap has never run: an integrated
GPU under D3D12 is the case to test first.

### 3. Nothing fills a texture's mips but the caller

A texture created with `mip_levels` 0 gets every level, and each starts
undefined. `aprend_texture*_update` writes any one level, so a caller with its
own mip data can fill the chain. A caller without it can't: nothing generates
a level from the one above (item 31). Until then such a texture wants
`mip_levels` 1, or a sampler whose `max_lod` is 0.

### 6. Immediate operations: what is left

Texture updates and framebuffer clears no longer block (see "State of the
code"): they are submitted as one of the instance's counted submissions and
their list, allocator and staging buffer go on the release queue. Left over:

- **A readback blocks**, and has to: the caller reads the result. One that
  hands the result over later (a pending read the caller polls) is the only
  way it wouldn't, and nothing asks for it.
- **Every call still makes a command allocator, a command list and, for an
  update, a staging buffer**, and releases them a moment later. A caller
  that updates many textures in a frame pays that each time. Wanted: updates
  recorded into a shared upload list for the frame, from a staging buffer
  that is reused (item 13).
- **An update is its own submission.** It can't be placed between two
  commands of a command list.

### 7. The default shaders are unused

`shaders/apricot_default.vert` and `.frag` are compiled by the build on Windows
and Linux and loaded by nothing. Either they go, with the glslc block in
`CMakeLists.txt`, or something uses them; which depends on item 22.

### 8. Older-generation habits still to convert

Per `CLAUDE.md`, only when the function is already being changed: joined
argument checks (`if (!instance || !desc)`), failure reported by `printf` with no
result code, a few unchecked `malloc`s (the layout copy in
`aprend_vertex_buffer_create`, the file buffer in
`aprend_shader_read_from_file_spirv`), and `std::vector` growth inside C entry
points with nothing catching `std::bad_alloc`.

Unchecked in the command list: `SET_INDEX_BUFFER` with a NULL buffer is
dereferenced at compile. Vertex buffers, the index buffer, viewports and
scissor rects sent before a pass are passed to SpudGPU where they stand,
outside the pass, and whether they are still set inside it is the backend's.

## Real gaps: what a renderer at Hazel's level has

Hazel (`../.refs/HazelRenderer2025/Hazel/Hazel/src/Hazel/Renderer`) is the bar;
see `CLAUDE.md`, "Reference: Hazel", for how its idioms are written here. Each
item names where Hazel does it. Unless it says otherwise, SpudGPU already
exposes what the item needs.

### 9. Binding: what is left

Binding sets cover every descriptor type and set slot (see "State of the
code"). Left over:

- **Changing a set costs a set.** `aprend_binding_set_update` (see "State
  of the code") builds new descriptor sets from every entry, changed or not,
  and retires the old ones. Writing only the changed slots needs SpudGPU to
  copy descriptors from one set to another, which it doesn't offer.
- **Nothing tells a compiled list its set was updated.** A list compiled
  before the update and submitted after binds descriptor sets that may have
  gone back to the layout. The header says to compile again; nothing checks
  (item 32 has the same gap for textures).
- **A layout has no debug name**: `aprend_binding_layout_create` takes no desc.
- Nothing checks a layout against the SPIR-V (item 18).
- **A combined image sampler slot doesn't work on SpudGPU's Metal backend**,
  which writes the texture half only. A layout that has to run there declares
  a sampled image slot and a sampler slot instead. Aprend accepts the combined
  type on every backend and says nothing.

### 10. Push constants: what is left

A pipeline declares ranges and `APREND_COMMAND_PUSH_CONSTANTS` writes them (see
"State of the code"). The bytes are written for the pipeline bound then and
are not tracked: after another `SET_SHADER_PIPELINE` or `SET_COMPUTE_PIPELINE`
the caller sends them again. A graphics pipeline's are written inside a pass
and a compute pipeline's outside one.

### 11. Vertex bindings: what is left

A pipeline declares up to `APREND_MAX_VERTEX_BINDINGS` bindings, each per vertex
or per instance (see "State of the code"). Left over:

- **No matrix element type.** A per-instance `mat4` is four `VEC4` elements,
  at four locations.
- **A per-instance binding steps every instance.** SpudGPU's binding has no
  step rate, so one element for every N instances can't be asked for.
- **Locations are positional**: element N across the bindings is location N.
  A shader that skips a location can't be described. Reflection (item 18)
  replaces this.

### 12. Frames in flight: what is left

Buffer sets give each frame in flight its own copy of a uniform or storage
buffer (see "State of the code"), so the frame being written doesn't share a
buffer with one the GPU is reading. Left over:

- **Pacing counts only what goes through Aprend**, on one queue. Every
  submission is made by `aprend_instance_submit`: command lists, and the
  immediate operations, which then wait for their own serial. Each signals
  the instance's fence to the next serial, and `aprend_instance_next_frame`
  waits for the last serial of the frame that used the index before. Work a
  caller submits to SpudGPU itself is outside it. An instance submits on one
  queue, the queue of its first submission, and a submission on another is
  refused (written 2026-10-10, compiled on macOS only): the serials are an order of
  completion on a single queue only. A second queue, for copies or compute,
  would need a fence and serials of its own.
- **The swap chain still waits for its queue to go idle** before a resize or
  a destroy. That is every submission on the queue, the caller's included,
  which is what replacing back buffers needs; `aprend_instance_wait_idle`
  would cover Aprend's alone.
- **The wait has no time limit and no way to report failure**:
  `aprend_instance_next_frame` returns the index either way and prints.
- **A command list compiled against an object isn't seen by the release
  queue.** The queue releases an object once everything submitted up to its
  destroy call has finished. Compiling a list records nothing there, so a
  list compiled before the destroy and submitted after it runs with the
  object possibly gone. It is the caller's not to do that, as it was. A
  generation count on a handle that a list compares at submit (item 32)
  would turn it into a refused submit.
- **Threads: the contract is written, and nothing enforces or widens it.**
  `aprendcontext.h` ("Threads") says an instance and everything of it is
  used by one thread at a time, why, and the three things several threads
  may do at once; each other header says what is particular to it. A
  destroy call is safe from any thread (see "State of the code"). Wanted,
  in this order, once what is written has been built and run:
  - A debug-build check that remembers the thread an instance is used from
    and reports a call from another, since a broken rule is otherwise a
    silent race.
  - A SpudGPU command allocator for each command list, in place of the
    instance's one, so different lists can be compiled from different
    threads as they can already be recorded. Not before something records
    in parallel: it costs an allocator a list.
  - What two instances on one device may do at once is SpudGPU's to say,
    and `spudgpu.h` says nothing about threads beyond one command list.
- **A destroy call releases nothing itself.** It queues, and the queue is
  drained by a submission, `aprend_instance_next_frame` or
  `aprend_instance_wait_idle`. So with the GPU idle and nothing more
  submitted, what is destroyed stays allocated until one of those or
  `aprend_instance_destroy`. That is the price of a destroy being safe from
  any thread. A caller that wants the memory back at once calls
  `aprend_instance_wait_idle` after the destroy: with the GPU already idle
  it returns immediately and releases everything waiting, and it only blocks
  while work is still in flight. No separate non-blocking call is needed;
  the one case it would add, draining while work is in flight without
  waiting, is what the next submission does anyway.
- **With no memory to queue an entry, the object is leaked** and a line
  printed. There is no thread on which a destroy call could safely release
  it instead.
- **A handle destroyed after its instance touches freed memory.** Before, it
  only outlived its device, which was already wrong.
- **Released objects are held a little longer.** A resize every frame keeps
  the last few images alive until the frames using them finish.
- **The swap chain is not deferred**: it still waits for its queue.
- **Single buffers are as they were.** A uniform or storage buffer that isn't
  in a set, and every vertex and index buffer, is still one allocation that
  must not be written while a submitted frame reads it. There is no vertex or
  index buffer set; a caller that rewrites geometry every frame keeps one
  buffer for each frame itself.
- **A copy is not carried forward.** A copy holds what was written to it
  `frames_in_flight` frames ago. A caller that updates part of a set each
  frame reads stale bytes in the rest. Hazel's sets behave the same; a set
  that copied forward what wasn't rewritten would hide the cost.
- **A list that binds a per-frame set is compiled every frame.** It reads the
  frame's copies through that frame's descriptor set, fixed at compile. A set
  bound through a dynamic offset would lift this; SpudGPU has none.
- **Descriptor sets multiply**: a binding set with a buffer set in it takes
  `frames_in_flight` sets from the layout's pool, 32 to a pool.

Hazel: `UniformBufferSet`, `StorageBufferSet`, `Renderer::SubmitResourceFree`,
`RendererConfig::FramesInFlight`.

### 13. Uploads: what is left

Vertex, index and storage buffers follow the device's memory (see "State of
the code"). Left over:

- **Staged writes don't use the transient arena yet.** Its chunks are made
  as copy sources for this, and it is the per-frame half of the shared
  upload path: a write whose copy is submitted in the same frame could take
  a span and record an exact copy. What it can't hold is a write whose copy
  is submitted in a later frame (a buffer written and not drawn yet), since
  the frame's spans are taken back at the next turn. That needs spans that
  are counted, or the staging buffer kept for those.
- **A staged buffer keeps its staging copy for life**, so on a device with
  its own memory a buffer written once costs its size twice. The release
  queue (item 12) can now free a staging buffer once its copy has run, and
  that is not built, because of what a store promises: its staging buffer
  holds everything ever written, so one merged dirty range is always right.
  A staging buffer made again for a later write holds only that write. It
  needs either exact ranges in place of the one merged range, or a store
  that is told at creation it will be written once. A shared staging buffer
  for many small writes is the same decision.
- **A write must not be made while submitted work that uses the buffer is
  unfinished.** On a unified device the write lands under the GPU; on a staged
  one the copy that hasn't run yet reads the new bytes. The same rule either
  way. Buffer sets lift it for uniform and storage buffers (item 12); the
  release queue doesn't, since nothing is destroyed.
- **One dirty range per buffer.** Two small writes far apart copy everything
  between them. Correct, and more than is needed.
- **A pool block is staged like any buffer.** On a device with its own
  memory each block keeps a staging copy for life and has one dirty range,
  so two ranges allocated far apart in a block copy everything between them.
  A pool range is written exactly once, which is the "store that is told it
  will be written once" above: a pool could take a transient span for each
  write, record an exact copy, and keep no staging copy at all. Not built.
- **A pool is never compacted and frees nothing by itself.** Ranges don't
  move, a freed range of a wrong pool or one freed twice is caught only
  where the free call can see it, and a range freed twice with an
  allocation between the two frees is not caught.
- **Uniform buffers stay host-visible on every device**, on purpose: they are
  small and rewritten every frame, and a staged copy per buffer per frame
  costs more than the bus read it saves. A large uniform buffer written once
  would be better staged; nothing asks for it.
- **A texture upload is still its own allocator, list and staging buffer**,
  though it no longer blocks (item 6).
- **A compiled list holds a pointer to each staged buffer it uses**, as it
  does to each texture (item 32). Destroying the buffer and then submitting
  the list is a use after free.
- **How SpudGPU's memory kinds land**, for whoever touches this next. The
  contract of each is in `spudgpu.h` (`SPUDGPU_MEMORY_KIND`); this is only
  what is underneath:

| Kind | Vulkan | D3D12 | Metal |
|---|---|---|---|
| `DEVICE` | A device-local memory type | Default heap | Private storage |
| `UPLOAD` | A host-visible memory type; coherent or not, the flush covers both | Upload heap | Shared storage, write-combined |
| `READBACK` | A host-visible memory type, cached if there is one | Readback heap | Shared storage |
| `DEVICE_MAPPABLE` | A memory type both device-local and host-visible, on an integrated device | Custom heap, pool L0, on a UMA adapter | Shared storage, on a device with unified memory |

### 14. Compute: what is left

Compute is in (see "State of the code"): compute shaders,
`aprend_compute_pipeline`, and `SET_COMPUTE_PIPELINE` and `DISPATCH` in the
same command list as rendering, outside a pass. Left over:

- **Every storage buffer and storage image of a dispatch's sets gets an
  ordering barrier before it**, and the same before a pass for the sets its
  draws read. A set doesn't say whether the shader writes a slot, so Aprend
  can't leave out the ones that are only read. Correct, and more than is
  needed; reflection (item 18) would say which are written.
- **A buffer a shader writes has no way back to the CPU**: there is no
  storage buffer readback.
- **A compute pipeline lasts until the next pass or present**, and is set
  again after either. That is the rule that holds on every backend; on some
  it would have survived.
- **Dispatches are direct only.** No indirect dispatch.
- **No compute pass object.** Hazel's `ComputePass` sets inputs by name and
  validates them against reflection, and `DispatchCompute` takes a material;
  those are items 18 to 20 here. Its resource states are tracked by NVRHI;
  Aprend's own tracking does that job.
- **SpudGPU on Vulkan orders storage writes for the compute, vertex and
  fragment stages only.** A storage resource written from another stage is
  not covered by the barrier Aprend records.

Hazel: `PipelineCompute`, `ComputePass`, `Renderer::BeginComputePass`,
`Renderer::DispatchCompute`, and their use in `SceneRenderer.cpp`.

### 15. Indirect draws and mesh shaders: what is left

Both are in (see "State of the code"). What was listed here and isn't Aprend's
to finish has moved to the SpudGPU group: bundles and bindless (item 33), and
indirect dispatch, a draw count from the GPU and task shaders (item 34). What
is left is how the built part behaves:

- **Buffer state is tracked only for a storage buffer that is also an
  argument buffer.** A command list moves such a buffer between the state a
  storage slot uses it in and the one an indirect draw reads it in, and
  submit puts it in the first ahead of the list. Every other buffer is used
  in one state for life and isn't tracked. A buffer with a third use would
  need its states added to this, not a second mechanism.
- **A buffer is in one state for a whole pass**, so it can't be the argument
  buffer of a draw and in a storage slot the same pass's draws read. Reading
  the arguments from the shader as well (to index per-draw data by them)
  needs a read-only state for both at once, which Aprend doesn't use.
- **An argument buffer adds a barrier to every submit** that uses it, to give
  the list a known state to start from, even when nothing was written.
- **An index buffer, like a vertex buffer, is tracked across passes.** An
  indexed draw is refused only if no `SET_INDEX_BUFFER` came before it
  anywhere in the list, not in its own pass; whether a buffer set before a
  pass is still set inside it is the backend's (item 8).
- **Mesh shading is a per-device fact.** A mesh shader pipeline fails to
  create on a device without it, with a message; Aprend has no query of its
  own, since the caller holds the device and can ask SpudGPU.

### 16. Views of a range, cubes: what is left

A view covers a range of its texture's mips and layers and has a shape, and a
2D texture can be cube-capable (see "State of the code"). Left over:

- **A pass renders to one mip.** Through 2D views it renders to one layer;
  through 2D array views to all their layers at once, the layer chosen in
  the vertex or mesh shader (see "State of the code"). What that leaves:
  - Rendering to several mips in one pass isn't offered, and no graphics
    API offers it.
  - A device without `SPUDGPU_DEVICE_PROPERTIES::layered_rendering` can't
    make a 2D array view to render to, and Aprend has no fallback: the
    caller renders a layer at a time through 2D views there. Aprend has no
    query of its own for it; the caller reads the device's properties.
  - The layer can't be chosen in a geometry shader, which Aprend doesn't
    take and Metal doesn't have.
  - Multiview (one draw repeated to each layer by the device, a view index
    in the shader) isn't offered. SpudGPU has nothing for it.
  - The framebuffer's attachments and its clear calls are still mip 0 of
    layer 0; an `aprend_framebuffer` is never layered.
- **A storage image slot is one mip, and stays so.** It is not a gap: an
  image store takes no mip level in SPIR-V, and a D3D12 unordered access
  view is one mip slice. Writing several mips in one dispatch is an array
  of slots (`image2D mips[N]`, `aprend_binding_desc::_count`) with a one-mip
  view in each, which the binding set and the per-mip layouts already take;
  `aprendimages.h` says so. No dispatch has run with one.
- **1D textures and 1D views: not planned until something needs them.** A
  lookup table, ramp or gradient is a 2D texture one texel high, declared
  `sampler2D` and sampled at `vec2(u, 0.5)`: the same memory and the same
  result. A 1D type would have to be the narrowest of the three APIs', and
  Metal refuses a 1D texture as a render target, with mips, and in a
  compressed format. Hazel has none either: its `TextureType` is 2D and
  cube, and its lookup tables are `Texture2D`. SpudGPU has the 1D image and
  view types; Aprend doesn't use them.
- **A view's format is its texture's.** No view in another format of the
  same size. The one exception is built: a view of a depth and stencil
  texture says which aspect it covers, so the stencil can be sampled (see
  "State of the code").
- **The stencil can't be sampled in a pass that tests against the same
  mip's depth.** A mip of a layer has one layout, not one for each aspect
  (item 17), so sampling either aspect moves both out of the attachment
  layout. Read-only depth with the stencil sampled needs a layout for each
  aspect and SpudGPU's read-only depth and stencil layouts.
- **Not taken from Hazel, on purpose.** Its `Image2D` makes and keeps
  per-layer and per-mip views itself (`CreatePerLayerImageViews`,
  `GetMipImageView`); here the caller makes the views it needs and owns
  them. Its `TextureCube` is a class of its own and an asset type; here a
  cube is a texture and a view.
- **SpudGPU's Rust bindings don't have `cube_capable`**; their image
  descriptor was already out of step with the header.

Hazel: `ImageViewSpecification`, `ImageSpecification` (`Mips`, `Layers`),
`TextureCube`.

### 17. Layout tracking: what is left

A layout is tracked for each mip of each layer (see "State of the code"), so
one mip can be sampled while another is rendered to or written by a compute
shader. Left over:

- **A list keeps an entry for every mip of every layer it touches**, found by
  a linear search. A texture with a full mip chain and many layers, used by
  many lists, makes that slow before it makes it wrong.
- **A range whose subresources differ is moved one barrier each.** Runs of
  equal layouts inside it aren't merged.
- **A resize to a different mip count replaces the layout storage**, so a
  list compiled before the resize holds pointers into what was freed. With
  the same mip count the storage is kept. The header says to recompile after
  a resize; nothing checks (item 32).
- **The stencil of a depth and stencil texture follows its depth.** There is
  one layout for a mip of a layer, not one for each aspect.

### 32. Texture layouts: what the command list gets wrong or can't do

Numbered out of order, as item 31. Item 17 is the one limit that needs a
redesign; these are the rest, all in `aprendcommands.cpp`. A pass takes only
the sets its draws read (see "State of the code").

- **A depth texture can't be tested against and sampled in one pass.** A
  sampled texture goes to `SHADER_READ_ONLY` and a target to its attachment
  layout, and a texture in both is refused. Reading depth in the shader while
  it is the pass's depth target, with depth writes off, needs a read-only
  depth layout, which `SPUDGPU_IMAGE_LAYOUT` doesn't have, and a rule for when
  it is used.
- **A storage image gets a barrier at every pass**, written or not, because
  the set doesn't know whether the shader writes it. Correct, and more than a
  read-only use needs.
- **A compiled list holds a pointer into each texture it uses**, to the
  texture's tracked layout, and the SpudGPU image it had at compile. Destroying
  a texture before submitting a list compiled against it is a use after free.
  After a resize the submit check usually refuses the list, since the tracked
  layout goes back to `UNDEFINED`, but not if the list also found it
  `UNDEFINED`: then it submits barriers for an image that no longer exists.
  The release queue doesn't help: it sees submissions, not compiles (item
  12). Wanted: a texture knows it has changed (a generation count the list
  compares), or the header's "recompile after a resize" is checked.
- **A list leaves a sampled texture in `SHADER_READ_ONLY` and a storage image
  in `GENERAL`.** Nothing puts it back. Right for most uses; written down
  because the immediate operations do put a texture back.

### 18. No shader reflection

Vertex layouts, uniform bindings and uniform offsets are declared by hand and
nothing checks them against the SPIR-V, so a mismatch is found on screen.
Wanted: reflection at shader creation giving sets, bindings, block members with
offsets and sizes, and vertex inputs; pipelines and binding sets validated
against it; inputs addressed by name. This is also the real fix for item 1 and
the rewrite `aprendbuffers.h` asks for. Hazel:
`Platform/Vulkan/ShaderCompiler/VulkanShaderCompiler.h` (`Reflect`),
`Shader.h` (`ShaderBuffer`, `ShaderUniform`), `DescriptorSetManager::Validate`.

### 19. No material

Nothing pairs a shader with its parameter values and textures. Wanted, after
items 9 and 18: an object holding a uniform block's bytes and the bound
textures, set by name. Hazel: `Material.h`.

### 20. No render pass or compute pass object

A pass today is a `BEGIN_RENDERING` command the caller fills in each frame,
followed by its own pipeline, set, viewport and scissor commands. Wanted: a pass
that owns its pipeline, target and inputs, validates them once, and is begun
with one command. Hazel: `RenderPass.h`, `ComputePass.h`,
`Renderer::BeginRenderPass`.

`aprend_framebuffer` belongs here too: nothing connects one to
`BEGIN_RENDERING` or to a pipeline's attachment formats, so the caller reads its
views out and builds the targets by hand.

### 21. No draw list, batching or instancing

The caller issues every draw. Wanted: a caller-fed list of draws (mesh, material,
transform) that is sorted, batched by mesh and material into instanced draws
with transforms in a storage buffer, and replayed into each pass that needs it.
No scene, asset or application concept: the caller hands over what to draw.
Needs items 9, 11 and 12. Hazel: `SceneRenderer` (`MeshKey`, `DrawCommand`,
`m_SBSInstanceTransforms`), `Renderer::RenderMesh`.

### 22. Shaders are precompiled SPIR-V only

No runtime compilation, includes, macros, cache or reload; the build compiles two
GLSL files with glslc on Windows and Linux and nothing on Apple. Whether Aprend
should compile shaders at all, or only take SPIR-V and reflection data from its
caller, is undecided. Hazel: `VulkanShaderCompiler`, `ShaderPreprocessor`,
`VulkanShaderCache`, `ShaderPack`.

### 23. No 2D, line, text or debug drawing

Hazel: `Renderer2D` (batched quads, lines, circles, MSDF text), `DebugRenderer`.
Needs items 9 and 12.

The memory for it is there: transient spans (`aprendtransient.h`, see "State
of the code") are what a batch of quads or glyphs is written into each
frame. Not there: the batcher, the shaders, a font atlas, and uniform data
from transient memory (a binding set reads a buffer, not a span).

### 24. No lit pipeline

No lighting, shadows or post-processing of any kind; a pipeline today outputs
whatever its two shaders compute from one uniform block. Hazel's chain, in
order: directional and spot shadow maps, pre-depth, tiled light culling
(compute), skybox, PBR geometry, GTAO, SSR, bloom, composite
(`SceneRenderer::FlushDrawList`). Each stage is its own piece of work once
items 9 to 21 exist; this entry is the placeholder for splitting them out.

## Needs something SpudGPU doesn't expose yet

What SpudGPU exposes follows from the GPU APIs it translates and is decided
there, not by this list. These are what Aprend can't do until it does.

### 25. Multiple render targets per pipeline

`BEGIN_RENDERING` takes up to `APREND_MAX_COLOR_TARGETS` color targets, but
`spudgpu_shader_pipeline_desc` has one `color_attachment_format` and one
`blend_attachment`, so a pipeline can't describe more than one. Aprend's own
desc mirrors that. Wanted for any G-buffer or a pass with a second output.

### 26. Depth bias

No depth bias on the pipeline desc. Shadow maps need it.

### 27. MSAA and resolve

No sample count on images or pipelines and no resolve attachment.
`aprend_texture2d_create` refuses `sample_count > 1` and `BEGIN_RENDERING`
refuses a `resolve_view` for this reason. Hazel doesn't use MSAA either, so this
is not part of the bar.

### 28. Stencil state

Stencil load and store ops exist on the depth target, but there is no stencil
test or op state on the pipeline. Hazel leaves stencil off as well.

### 29. GPU timing and debug markers

No timer or pipeline statistics queries and no debug marker calls, so there is
no per-pass GPU time and no labelled captures. Hazel:
`RenderCommandBuffer` (`RT_BeginTimerQuery`, `RT_BeginMarker`).

### 30. Hardware without dynamic rendering

`CLAUDE.md`, "Aprend": SpudGPU's plan is a caller-owned `spudgpu_framebuffer`,
designed and not built. When it exists, `aprend_framebuffer` holds and caches
it.

### 31. Mip generation

Numbered out of order so the other items keep their numbers. It was listed as
a gap Aprend could close with `spudgpu_cmd_blit_image`, level N into level
N + 1 at half the size. It can't yet: only SpudGPU's Vulkan backend resamples
in a blit. The Metal and D3D12 backends copy regions of one size and refuse
the rest, so a call built on it would do nothing on two backends of three.

Two ways it gets built:

- **SpudGPU's blit resamples on every backend.** Then, per level N and for
  every array layer: N to `TRANSFER_SRC` and N + 1 to `TRANSFER_DST` with
  `spudgpu_cmd_image_barrier_subresource`, a blit at half the size and never
  below 1 (a 3D texture halves its depth too), and the whole chain to one
  layout at the end. Integer and depth formats can't be blitted with a linear
  filter everywhere, and SpudGPU has no query for it.
- **Aprend draws each level**: a pass per level that samples the one above.
  A view of one mip and layout tracking per mip are there now (items 16 and
  17), so a caller can already do this with its own shader. For Aprend to do
  it needs a shader Aprend ships, which is item 22's open question.

Either way there are two forms, both asked for by the caller and never done
by `aprend_texture*_update` on its own: an immediate one for a texture filled
from the CPU, and a command outside any pass for one just rendered to. The
filter is an argument.

Hazel: `Texture2D::GenerateMips`.

### 33. Bundles and bindless: not on every backend

Numbered out of order, as item 31. SpudGPU has both on Vulkan and D3D12 and
compiles both out on Metal (`SPUDGPU_EXT_BUNDLES`,
`SPUDGPU_EXT_BINDLESS_DESCRIPTOR_INDEXING`), so Aprend can't offer either as
the same thing everywhere, and offers neither.

- **Bundles: not planned.** A bundle is a second kind of command list,
  recorded ahead and replayed inside a pass. Metal has no such thing, so an
  Aprend API for it would be compiled out on one backend and a caller would
  have two ways to draw the same thing. What a bundle is for, recording
  draws once and replaying them, an Aprend command list already does: it is
  compiled once and submitted as often as its textures and buffers allow.
  `APREND_RENDERING_FLAG_EXECUTES_BUNDLES` and the `_flags` of
  `BEGIN_RENDERING` were removed on 2026-10-10 (written, compiled on macOS only): the
  flag told SpudGPU a pass would execute bundles when no command could
  record or execute one. If bundles are wanted after all, the flag comes
  back with the commands that give it something to mean.
- **Bindless: waits on SpudGPU's Metal backend.** One table of every texture
  and buffer, indexed from the shader, in place of a binding set for each
  material. It changes what a binding layout is, so it is a design of its
  own, and it can't be the only path while one backend lacks it. Worth
  designing once Metal has it, and with the draw list (item 21), which is
  what would use it.

### 34. Indirect dispatch, a draw count from the GPU, task shaders

Numbered out of order, as item 31. Each is a call or a path SpudGPU doesn't
have on every backend:

- **Indirect dispatch**, for compute and for mesh shaders: the group counts
  read from a buffer. SpudGPU has no such call.
- **A draw count from the GPU.** `DRAW_INDIRECT`'s `_draw_count` is fixed
  when the command is recorded, so a compute pass that culls can zero an
  entry's instance count and can't shorten the list. SpudGPU has no
  count-buffer draw, and its Metal backend loops over the entries on the
  CPU, where a count read from the GPU isn't known.
- **Task shaders.** A mesh shader pipeline has a mesh stage and a fragment
  stage. SpudGPU's pipeline desc has a task module, and its Metal backend
  doesn't run one.

## Suggested order

Each step is usable on its own, and later ones lean on earlier ones. Items 4
and 5 are closed and their numbers are not reused.

1. Run what is written, and compile it where it hasn't been. It builds on
   macOS as of 2026-10-10 (see "State of the code"); left of this step:
   - Run it: nothing since 2026-10-09 has drawn a frame, in Aprend or in the
     SpudGPU changes it now depends on.
   - Compile it on Linux, and on Windows with the backend that hasn't been
     built there (see "State of the code").
   - Run it on a device with its own memory as well, for the staged path.
2. What the release queue made possible and isn't built: freeing staging
   copies and a shared upload path (items 13 and 6). Rewriting a binding
   set in place (item 9) is written.
3. The rest of item 32: what the command list still gets wrong about
   texture layouts.
4. Item 18, which settles item 1 and unblocks 19.
5. Items 19, 20, 21: material, pass and draw list.
6. Items 25, 26, 29 and 31 as SpudGPU gains them, and item 2; then 23 and 24.

Items 7, 22, 27, 28, 30, 33 and 34 wait until something needs them, decides
them or SpudGPU gains them.
