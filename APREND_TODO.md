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
  - The repo's headless tests passed then. Two of them are Aprend's and
    need no device (`aprendranges_tests`, `aprendreflect_tests`, both
    registered with ctest); what is known of each run is in the last
    bullet of this section.
  - Which compiler built it on Linux is not recorded here (see the Linux
    bullet below), so "compiled with GCC" is still unconfirmed.
- **Compiled and run on Windows on 2026-10-11.** Everything in this section
  down to "View aspects and stencil sampling" built there that day, with the
  SpudGPU it depends on, and one application ran on it and exited with code
  0. The rest of the section was built there by 2026-10-12 (the bullet
  after the Linux one). That
  is the first build of this code outside macOS and the first of a SpudGPU
  backend other than Metal. The backend was D3D12 (Trellis_Avalonia);
  Vulkan has not been built on Windows. As on macOS, it exercises what
  that application uses and no more (the list above); whether the device
  had memory of its own, which is what would have run the staged path, is
  not recorded either.
- **Compiled and run on Linux, recorded 2026-10-12.** Aprend built there
  against SpudGPU's Vulkan backend and one application ran on it. That is the first build and the first run of Aprend on Vulkan,
  and with it one application has run on all three backends: Metal on
  macOS, D3D12 on Windows and Vulkan on Linux. The Wayland driver issue that held the Linux
  machine no longer holds it. Not recorded: the compiler, the device, and
  whether the device had memory of its own (the staged path). As on the
  other two, it exercises what that application uses and no more (the list
  above).
- **Everything in this section is compiled on all three backends as of
  2026-10-12.** The Windows (D3D12) and Linux (Vulkan) runs were built
  from the current sources, as was the macOS (Metal) one, so every file in
  the library's source list, and the SpudGPU it rests on, has been through
  MSVC, the Linux compiler and Clang. "Compiled" in the bullets below
  means that and no more: "never run" and "none of it has run" still
  stand wherever the application doesn't use the thing, on every backend.
  The tests under `tests/` are not part of those builds; what is known of
  each is said where it is described.
- **The changes of 2026-10-09 are compiled on all three backends**:
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
- **The changes of 2026-10-10 are compiled on all three backends**:
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
- **The move to SpudGPU's memory kinds, 2026-10-10, is compiled on all
  three backends**. `SPUDGPU_MEMORY_FLAGS` is gone and every buffer names one
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
- **A second set of changes on 2026-10-10 is compiled on all three backends**. The
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
    on all three backends since. The vertex and index update calls check their range.
  - Frame pacing (item 12). The instance has a fence; every submission
    Aprend makes (command lists and the immediate operations) signals it
    through `spudgpu_queue_submit`, and `aprend_instance_next_frame` waits on
    it for the frame that last used the index. `aprend_instance_wait_idle`
    waits for everything submitted so far. It rests on SpudGPU's fence being a counter on every backend,
    which was rewritten the same day and is built on all three since.
    A wait for serial 0, a frame index nothing was submitted in or an
    instance destroyed before its first submission, returns at once without
    calling SpudGPU (2026-10-19): the fence starts at 0, and D3D12's debug
    layer warned at every such wait. Run on Windows, D3D12 and Vulkan.
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
- **Compute, written 2026-10-10 and compiled on all three backends** (item 14):
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
  all three backends** (item 15):
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
- **The release queue, written 2026-10-10 and compiled on all three
  backends** (item 12):
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
    waits. (Since 2026-10-15 they are recorded into the frame's upload
    batch instead: see the bullet of that date.)
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
  compiled on all three backends** (items 16 and 17):
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
  - SpudGPU changes it rests on, built on all three backends: cube-capable images, D3D12
    state for each subresource, Vulkan's whole-image barrier covering every
    mip and layer, and the storage layout meaning the storage state on D3D12.
  - None of it has run. A view of part of a texture has never been created.
- **Layered passes, written 2026-10-10 and compiled on all three
  backends** (item 16):
  - A view of type RENDER_TAGET or DEPTH_STENCIL may be a 2D array of one
    mip, on a device that reports
    `SPUDGPU_DEVICE_PROPERTIES::layered_rendering` (read once into the
    instance); on any other, creating one fails.
  - BEGIN_RENDERING over 2D array views is a layered pass: the compile
    refuses targets that differ in shape or layer count, and the layer count
    goes to SpudGPU as `spudgpu_rendering_begin_desc::layer_count` (0 for a
    pass over 2D views). Layouts and conflicts needed nothing new: a target
    is already moved, and checked for overlap, over its view's range.
  - SpudGPU changes it rests on, built on all three backends: `layer_count`, the
    `layered_rendering` property (which enables `shaderOutputLayer` on
    Vulkan where the device has it), and Metal pipelines always stating
    their primitive class, which Metal requires in a layered pass.
  - No shader that writes a layer has been through SpudGPU's SPIR-V
    cross-compile to HLSL or MSL, and no layered pass has run on any backend.
- **View aspects and stencil sampling, written 2026-10-11 and compiled on
  all three backends** (item 16):
  - `aprend_texture_view_desc::aspect` (COLOR, DEPTH, STENCIL,
    DEPTH_STENCIL), required and checked against the texture's format and
    the view's type. Before, Aprend chose it from the format, and a sampled
    view of a depth and stencil texture was always the depth.
  - A SHADER_RESOURCE view with the STENCIL aspect is what a shader samples
    the stencil through, as an unsigned integer.
  - SpudGPU changes it rests on, built on all three backends: named aspect
    bits and one rule for a view's aspect; Metal's stencil view format; and
    on D3D12 a view that keeps a shader resource desc and an unordered
    access desc beside its attachment desc, readable formats for depth and
    stencil, and typeless depth resources. The last three also mean a
    texture that is rendered to and sampled, color or depth, was not
    correctly sampled on D3D12 before. The D3D12 run of 2026-10-11 was
    without these changes; the build recorded on 2026-10-12 has them.
  - No stencil has been sampled on any backend.
- **`aprend_binding_set_update`, written 2026-10-11 and compiled on all
  three backends** (item 9): points some of a set's slots at other resources
  and keeps the handle. The set keeps a copy of the entries it was built
  from; an update merges the given ones in, builds a whole new set, swaps
  its contents into the handle, and destroys the new handle, which carries
  the old descriptor sets onto the release queue. Never run.
- **Transient memory, written 2026-10-11 and compiled on all three
  backends** (`aprendtransient.h`, new; items 13 and 23):
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
- **Every header is Javadoc-style Doxygen** (2026-10-11, comments only):
  `aprendcontext.h`, `aprendbuffers.h`, `aprendcommands.h`, `aprendframes.h`
  and `aprendswapchain.h` were rewritten to the style `aprendimages.h` and
  `aprendpipeline.h` already had, and the rest of `aprendtransient.h` and
  `apricotfields.h` with them. No declaration changed. The calls that had
  no comment have one now, written from the implementation: the framebuffer
  and swap chain calls, the buffer destroy and getter calls, the command
  list's create, destroy and reset, and each draw command. Doxygen reports
  no warnings over `include/render`.
- **Vertex and index pools, written 2026-10-11 and compiled on all three
  backends** (`aprendbuffers.h`; `aprendpools.cpp`, `aprendpools_internal.hpp`
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
  - Every range has an entry in the pool's table of allocations and
    carries the entry's number and generation. A free is checked against
    the entry, so a range freed twice is refused, also when its elements
    have been handed out again in between, and so is a range of another
    pool. The freed ranges waiting for the GPU are a list through the
    entries, so a free needs no memory.
  - Blocks are added one at a time and given back only by the trim calls. A
    range longer than a block has a block to itself. `memory_limit` in the
    desc; no default block size.
  - `aprendranges_internal.hpp` is the list of a block's free elements, with
    nothing of Aprend or SpudGPU in it, and `tests/aprendranges_tests.cpp`
    checks it against one flag an element. Whether that test has been
    built or run is not recorded.
  - Never run. No range has been allocated, drawn from or freed on any
    backend.
- **Materials, passes and draw lists, written 2026-10-11 and compiled on
  all three backends** (items 19, 20 and 21). Before that they were
  type-checked only: GCC 11 on
  Linux with `-fsyntax-only -Wall -Wextra`, with and without `_DEBUG`, over
  every file in `src/render`, and `apricotfields.h` as C11: no errors and
  no new warnings. They are linked into the library now, and have run on
  all three backends (last bullet). Doxygen has
  not been run over the three new headers, so "no warnings over
  `include/render`" above is not known to hold for them.
  - `aprendmaterial.h`, `aprendmaterial.cpp` (item 19): `aprend_material`
    is a binding layout and what each of its bindings holds, by name
    (revised the same day by item 18: names and blocks come from the
    layout). A uniform block the layout knows is one the material owns,
    unless the desc names it external: a CPU copy of its bytes, set by
    member name or as bytes, and the block on the GPU (revised again: see
    "The material arena" below). Textures, samplers and the caller's own buffers are set by slot
    name and array element. Its binding set is built at the first compile
    that binds it and rebuilt (`aprend_binding_set_update`) at the next
    compile after a resource changes.
  - `APREND_COMMAND_SET_MATERIAL` (22): SET_BINDING_SET with the material's
    set. `aprend_command_list_compile` builds every named material's set
    before it records anything, since a pass looks ahead at its sets; that
    is the one thing a compile now changes outside the list. A list keeps
    the materials it binds, and `aprend_command_list_submit` copies their
    changed blocks first.
  - `aprendpass.h`, `aprendpass.cpp` (item 20): `aprend_render_pass` is a
    graphics pipeline, its targets (views, or an `aprend_framebuffer`
    whose attachments are read when the pass is begun, so it follows a
    resize) with their ops, and the binding sets of the slots it fixes.
    Formats, view types, shapes and layer counts, and each set's layout are
    checked at creation. `APREND_COMMAND_BEGIN_RENDER_PASS` (23) is
    expanded when sent into BEGIN_RENDERING, SET_SHADER_PIPELINE, a
    viewport and scissor over the whole target, and SET_BINDING_SET for
    each fixed set; END_RENDERING ends it. `aprend_compute_pass` and
    `APREND_COMMAND_SET_COMPUTE_PASS` (24) are the same for a compute
    pipeline. A pass is a CPU description: nothing of it is on the GPU, and
    it is freed at destroy.
  - `aprenddrawlist.h`, `aprenddrawlist.cpp` (item 21): `aprend_draw_list`
    takes (mesh, material, instance bytes) draws. `aprend_draw_list_build`
    orders them (by material then mesh, or as added), merges draws of one
    mesh and material into one instanced draw, and writes every instance's
    bytes, in batch order, into one transient span. `APREND_COMMAND_DRAW_
    LIST` (25) is expanded when sent: the span at the list's instance slot,
    then for each batch SET_MATERIAL, SET_VERTEX_BUFFERS and
    SET_INDEX_BUFFER where they change, and DRAW_INSTANCED[_INDEXED] with
    the batch's first instance as `_start_instance_location`.
  - **Run on all three backends** (2026-10-17). First on Metal (Apple M5
    Pro, unified memory):
    `../apricottests`' `test_material`, `test_render_pass` and
    `test_draw_list` passed on their first build, Debug under the Metal
    validation settings and Release, with no validation message and no
    leak. Nothing in Aprend needed fixing. What they covered: a layout made
    from two shaders and found by name; materials set by member name (a
    bare path, and `u_material.layers[1].add` qualified by its binding),
    a texture and a sampler; a value changed between two submissions of
    one frame, each submission drawing its own; a texture swapped, the
    set rebuilt at the next compile; an incomplete material refused at the
    compile; instanced draws from `_start_instance_location` 1 (the first
    of the two checks this entry asked for). A render pass over a view,
    over a framebuffer with a borrowed texture, with a fixed set changed
    after a recording (the recording keeps the old set) and with its slot
    left open; five descs refused; a compute pass with a fixed set. A draw
    list in both orders: three batches from four draws, one of them
    indexed, replayed into two passes of one list; the last added on top
    in submission order; a list changed since its build, a draw with no
    material and a list built in the frame before, each refused. The same
    day they passed on D3D12 and Vulkan on Windows, on an Intel Iris Xe
    and an NVIDIA RTX A2000 (memory of its own, so the material arena's
    copies and the draw list's instance bytes reached device memory
    there), with every other test (reported).
- **Lit surfaces, item 24, written 2026-10-17; run on Metal.**
  `include/render/aprendlit.h` (lit surfaces), `include/render/aprendpost.h`
  (adapting exposure), `src/render/aprendlit.cpp`, `aprendlit_internal.hpp`,
  the commands in `aprendcommands.cpp`, and 25 shaders in `shaders/` sharing
  `.glslh` includes (glslc `-I`; CMake rebuilds each when one changes).
  - **The shape.** Aprend ships the shading, never the geometry: the
    surface fragment shader reads a position, normal, uv and tangent from
    any vertex shader, the scene at set 0 and the material at set 1 (item
    24 has why). The API is a handful of objects and calls, each value
    checked and each refusal printed: shaders and layouts (the
    instance's); a lit material (an `aprend_material` with typed values,
    maps and sampler); an environment; a scene (view, sun and its
    cascaded shadow, up to 128 point and spot lights, spot shadows,
    ambient light, ambient occlusion), which is a surface material, a
    block-only material per sun cascade and per spot shadow for the
    shadow passes, and Aprend's own for culling and occlusion; an
    exposure. Fitting functions with no GPU in them: the sun's cascades
    for a view (`aprend_lit_fit_sun_cascades`, stable: bounding spheres,
    texel snapping) and a spot's shadow matrix. Commands: LIT_ENVIRONMENT
    (a cube or equirectangular picture), LIT_SKY (Preetham's daylight),
    LIT_AMBIENT_OCCLUSION, AUTO_EXPOSURE, TONE_MAP (clamp, ACES, Khronos
    PBR Neutral, AgX; an exposure's adapted value).
  - **Shading**: Cook-Torrance (GGX, Smith-Schlick, Schlick); a clear coat
    (Filament's lobe) and a sheen (Charlie, Neubelt's visibility); point
    and spot lights windowed to their range; a sun with up to four
    cascades, a spot with a shadow of its own, each read through a
    comparison sampler with nine bilinear taps; a hemisphere plus an
    environment (GGX-prefiltered specular chain, irradiance cube) weighted
    by a BRDF lookup texture made once per instance; ambient occlusion
    from the map and from the view; base color, normal, ORM and emission
    maps, cut-outs, a uv transform; linear or sRGB out.
  - **What a compile does for a scene**, at the top of the list: makes the
    BRDF lookup texture once, clears an environment never filled to
    black, and culls the scene's lights for each 16-texel tile of its
    view's target with a compute pass into a buffer set the surface shader
    reads (`aprend_cmd_prepare_lit_scenes`). Empty slots hold placeholders
    a block bit says not to read: nothing needs filling, no lit call needs
    a queue.
  - **Fixes below Aprend** it brought, in SpudGPU: comparison samplers
    (`compare_enable`, `compare_op`) on all three backends; D3D12 begins a
    depth-only pass; Metal binds a set only to the stages whose function
    reads it (from Metal's pipeline reflection), and emits every declared
    resource into a shader's argument buffer struct, so a shader that uses
    some of a set's bindings reads the right ones (spudlib/CLAUDE.md has
    what is still a gap). Vulkan's and D3D12's sides have run, in the
    D3D12 and Vulkan runs below. On 2026-10-17 Trellis_Avalonia also built
    and ran over them on Windows, D3D12 and Vulkan, and Trellis_macOS over
    the Metal changes (reported); Trellis uses no lit surface, comparison
    sampler or depth-only pass, so that shows only that they break nothing
    there.
  - **What has run.** `../apricottests`, on an Apple M5 Pro (2026-10-17),
    Debug under the Metal validation settings with warnings fatal and
    Release, no leak: `test_lit_surface`, `test_lit_mesh_shadow`,
    `test_tone_map` (nine curves and encodings, AgX's against the same
    formula in C), `test_lit_environment`, `test_lit_cascades` (the fit
    on the CPU: splits, framing, texel snapping, refusals; on the GPU:
    the cascade chosen by depth, and past the last), `test_lit_spot_and_
    cutout_shadows` (a spot's shadow, 0 against 231 predicted lit; a
    cut-out's shadow there or not by its cutoff), `test_lit_coat_and_sheen`
    (against the lobes' formulas in C), `test_lit_tiles` (culled per tile,
    texel for texel the same as unculled), `test_lit_sky` (the zenith
    read 161, 255, 255, as the model gives), `test_auto_exposure` (46, 6,
    46, 11 as worked out), `test_lit_ambient_occlusion` (a step occluded
    beside it, 155 of 255; open elsewhere; the scene darkened by it
    there alone). Found on the way and fixed: Metal's argument buffer
    struct for a shader using some of a set's bindings (black sky), a set
    bound to a stage that doesn't read it, push blocks Metal rounds to 8.
    **Passed on D3D12 and Vulkan on 2026-10-17** (Windows 11, Intel Iris
    Xe and NVIDIA RTX A2000, Debug, at 7b18da8 with spudlib 69e6c43; D3D12
    under cdb, Vulkan with the validation layer, both clean), the first
    D3D12 run of a depth-only pass included, after one fix here: the
    clear of an environment never filled was to opaque black, and every
    texture is made with SpudGPU's clear value zeroed, so D3D12's debug
    layer warned of a mismatched clear value at each of the 30 faces
    (`CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE`). The faces are now
    cleared to 0 in every channel; only the cubes' color is read.
- **Uniforms found once, written 2026-10-17; run on Metal** (from "Next,
  from the review", its first step). `aprend_material_find_uniform`
  turns a member's name into an `aprend_material_uniform` once, against
  a binding layout, and `aprend_material_set_uniform_value` and
  `aprend_material_get_uniform_value` write and read it with no string
  read. `aprend_material_set_uniform` is now a find and a set.
  - The handle is a value of the layout, its binding's place and the
    member's place in its block, not a pointer into a material: one find
    serves every material of the layout, which is how a caller with one
    material per look and one layout for all of them uses it. It is
    checked at every set against the material's layout (another layout,
    even one of the same bindings, is refused) and against what the
    material owns (a block named external is refused).
  - What it was measured against. Hazel's `Material::Set(name, value)`
    hashes the name into its shader buffer's map at every call and has
    no handle. Unreal resolves a parameter once and sets it by index
    (`UMaterialInstanceDynamic::InitializeScalarParameterAndGetIndex`,
    `SetScalarParameterByIndex`), and its shader parameters are bound to
    offsets once at compile; GL's uniform locations, bgfx's uniform
    handles and Unity's property IDs are the same idea. Aprend's is that
    shape in C: a plain struct handed back by value, no allocation, no
    lifetime of its own beyond the layout's.
  - One rule changed with it: a bare name is looked for in every block a
    material of the layout could own, a block some material leaves
    external included, so a name means the same member in every material
    of the layout. Before, it was looked for in the blocks the material
    at hand owned.
  - `../apricottests`' `test_material` finds `color` and `strength` and
    checks their type and offset, finds `layers[1].add` by its binding's
    name and its block's and gets the same handle, sets by handle between
    two submissions, reads values back, and is refused a zeroed handle,
    one of another layout made from the same shaders, and one into a
    block the material leaves external. Passed on Metal (Debug under
    validation, Release, no leak), 2026-10-17, and the same day on D3D12
    and Vulkan (Intel Iris Xe and NVIDIA RTX A2000, Debug, debug and
    validation layers clean).
- **Exact staged ranges and buffers written once, written 2026-10-18;
  run on Metal, where neither is staged** (item 13, step 2 of "Suggested
  order"). A staged store's one dirty range is now a list of uploads
  (`aprend_buffer_upload` in `aprend_internal.hpp`), each copied on its own
  by `aprend_command_list_submit`, and `APREND_BUFFER_WRITES` is a new
  required argument of `aprend_vertex_buffer_create`,
  `aprend_index_buffer_create`, `aprend_storage_buffer_create` and
  `aprend_storage_buffer_set_create` (0 is refused: there is no default).
  - `APREND_BUFFER_WRITES_MANY`: the staging buffer of the buffer's size,
    for life, as before. A write merges with the ranges it overlaps or
    touches and no others, so two writes far apart are two copies and
    nothing between them crosses the bus.
  - `APREND_BUFFER_WRITES_ONCE`: written at creation only; the update calls
    refuse it. No staging buffer for life: each write is staged in an
    `UPLOAD` buffer of its own size, copied by the next submission that
    uses the buffer, and retired then, so it is released once that
    submission has run. Created with no data, the buffer is never staged.
  - A pool's blocks are `ONCE` buffers written by the pool, a write a
    range: each range is staged on its own and copied exactly, and a block
    keeps no staging copy. The update calls on a block now refuse, as the
    pool's header already said they are not for the caller.
  - The lit scene's light tiles and its placeholder, written by a shader
    only, are `ONCE` with no data: on a device with memory of its own they
    had a staging copy each that was never written.
  - Callers changed in the same change: trellislib's four vertex buffers
    (`src/trellisscene.cpp`) and latran's four mesh buffers
    (`core/src/latranrender.cpp`) are `ONCE`; none is ever updated.
    `../apricottests` passes `MANY` for the one buffer it updates and
    `ONCE` for the rest.
  - `../apricottests`' `test_buffer_writes`: a `MANY` buffer written in two
    ranges with a gap and then the gap; a `ONCE` buffer drawn in two frames
    and refused an update; a create with `writes` 0 refused; two pool
    ranges of one block drawn from where the pool put them, and the block
    refusing an update. Passed on Metal (Apple M5 Pro, Debug), 2026-10-18,
    with every other test, and trellislib's and latran's tests pass over
    it. That device is unified, so the buffers were written directly and
    none of the new staged code ran there. It ran first on the RTX A2000
    (memory of its own): every test passed on Windows on 2026-10-18, D3D12 under its debug layer and Vulkan under
    the validation layer with synchronization validation, on the Iris Xe
    and the RTX A2000 (reported; step 2 and 3 of "Suggested order" have
    what that run found). Not built on Linux.
- **Stale lists refused, and storage barriers from what shaders write,
  written 2026-10-18; run on Metal** (item 32, step 3 of "Suggested order").
  - The instance keeps every command list compiled and not yet submitted
    (`compiled_lists`, under `release_mutex`). Destroying a texture, a
    vertex, index or storage buffer, a material or a GPU timer, from any
    thread, and resizing a texture call `aprend_instance_forget`, which
    marks each such list that uses it stale; `aprend_command_list_submit`
    refuses a stale list before it reads anything of it. Before, a destroy
    was a use after free at submit, and a resize was refused only when the
    tracked layout happened to differ. A list records every buffer store it
    uses (`used_stores`), staged or not, so the refusal is the same on every
    device.
  - A pipeline records the storage bindings its shaders write
    (`storage_writes`: declared without `readonly`, used by the entry
    point), and a binding set the binding of each storage image and buffer
    it holds. The list keeps, for each subresource and each storage buffer,
    whether a shader may have used and written it since its last barrier
    (`layout_use::storage_accessed` / `storage_written`, `storage_hazards`;
    both start true, for the submission before). A use needs the ordering
    barrier if it was written since, or if this use writes and it was used
    since. Before, every storage image and buffer of a pass or dispatch got
    one. The lit light cull's dispatch goes through the same preparation
    now, in place of its own barrier, so the tiles it writes are known
    written.
  - The compile walk is inside a `try`: a vector growing out of memory in
    it could throw out of `aprend_command_list_compile`, a C entry point.
    The loop is indented one more tab for it; nothing else in it changed.
  - SpudGPU, found by the new test: Metal set the compute pipeline state on
    the encoder at every bind, and its validation refuses setting the same
    state twice. It sets it only when it differs now (spudlib).
  - `../apricottests`: `test_storage_ordering` (add, copy, copy, add, copy
    over r32f storage images: a write, two reads with none between, a write
    after reads, a read; the values checked) and `test_stale_lists` (a
    target resized, a target destroyed, a vertex buffer destroyed, each
    after a compile and refused at submit, and a list that goes). Every
    test passed on Metal (Apple M5 Pro), Debug under Metal API and shader
    validation with warnings fatal, and Release under `leaks` (none).
    trellislib's and latran's tests pass over it. On Metal the barriers do
    nothing; every test passed on Windows on 2026-10-18, D3D12 under its debug layer and Vulkan under
    the validation layer with synchronization validation, on the Iris Xe
    and the RTX A2000 (reported; step 2 and 3 of "Suggested order" have
    what that run found). Not built on Linux. Under sync validation `test_storage_ordering` had
    no hazard, so the barrier left out between two reads is not needed.
- **`aprend_material_copy`, and no default resources by decision,
  written 2026-10-18; run on Metal** (from "Next, from the review", its
  first step).
  - `aprend_material_copy(source)` makes a material of the same layout and
    owned blocks, its own arena ranges holding the source's values, and
    the same resource handles (shared, not duplicated). The two are
    independent after; either may be destroyed first. No binding set and
    no lit scene are copied; the copy's blocks go across at its first
    submission. Create and copy share the owned block's setup
    (`aprend_material_own_block`).
  - Compared: Hazel's `Material::Copy` copies the uniform bytes and the
    slot assignments, sharing the resources, and leaves the flags to
    `Init`; Unreal's material instances read what they don't override
    from a parent, live, and its `CopyParameterOverrides` copies values
    once. Aprend's is Hazel's shape (a material carries no flags here),
    and not Unreal's inheritance (item 19).
  - Default resources for empty slots are decided against, not left
    undone. Hazel fills every sampled 2D slot with one white texture,
    every cube with a black one and every sampler with one sampler
    (`DescriptorSetManager.cpp`, with its flag forced on); Unreal has no
    empty slot, since every parameter has a default its author gave it.
    What an empty slot should read depends on what it is for (white for
    a base color, black for emission, a flat normal), which Aprend
    doesn't know, and a placeholder would have to match each slot's
    shape and scalar type to pass the compile's view checks. So the
    default is the author's, as in Unreal, and the author is the caller:
    a base material with its own choices, copied. Aprend's lit material
    is the one place Aprend is the author, and it fills its maps.
  - A compile that finds an empty binding names it, its element and what
    is missing (a texture view, a buffer or a sampler), and points at the
    copy. `aprend_material_is_complete` is the same search.
  - `aprendmaterial.h`, reviewed for the copy: a "Copying" section at the
    top; `aprend_material_destroy`'s warning was stale since the stale-list
    check (a list compiled with a destroyed material is refused at submit
    now); the two uniform-buffer setters take a block name as well as a
    binding name, which the code did and the docs didn't say; a view or
    sampler outlives every material that holds it. Nothing renamed or
    removed: the calls were consistent with each other and with the rest
    of Aprend.
  - `../apricottests`' `test_material_copy`: a complete base copied, the
    copy's values read back, both drawn, each changed alone and drawn,
    the base destroyed and the copy drawn, a copy of NULL refused, and a
    copy of a lit material with its values. Passed on Metal (Apple M5
    Pro), Debug under Metal API and shader validation with warnings
    fatal and Release under `leaks` (none), with every other test.
    latran's 26 suites pass over it, and trellislib's 10. Of trellislib's,
    `trellislib_model_tests` failed once, in the first run after the
    relink, with its output lost; nine runs since passed, and it calls
    no material code. Every test passed on Windows on 2026-10-18, D3D12 under its debug layer and Vulkan under
    the validation layer with synchronization validation, on the Iris Xe
    and the RTX A2000 (reported; step 2 and 3 of "Suggested order" have
    what that run found). Not built on Linux.
- **A uniform buffer's uniforms found once, written 2026-10-18; run on
  Metal** (from "Next, from the review", its first step).
  - `aprend_uniform_layout_get_uniform_index(layout, name)` finds a uniform
    of an `aprend_uniform_layout` by name, once, and
    `aprend_uniform_buffer_update_uniform(buffer, index, data)` and
    `aprend_uniform_buffer_set_update_uniform(set, index, data)` write it
    with no search. `aprend_uniform_buffer_update_by_name` is the lookup on
    the buffer's copy of its layout and then the write, as before (the
    first uniform of the name).
  - The handle is the uniform's index in its layout, not a struct as the
    material's `aprend_material_uniform` is. A uniform buffer's layout is a
    plain struct the caller writes or reads from shaders and passes at
    create, not a handle, so the index is what names a uniform of it: it
    serves every buffer and buffer set of the layout, a caller that wrote
    the layout knows it already, and it has the shape of
    `aprend_buffer_layout_get_element_index` for a vertex layout. It is
    checked against the buffer's layout's count at each write.
  - Every data argument of `aprendbuffers.h` is `const void *` now (the
    create and update calls of every buffer and buffer set); each was
    only read. The same symbol and the same ABI, and source-compatible.
  - `../apricottests`' `test_uniform_buffer_index`: a uniform buffer and a
    buffer set made from the material shader's uniform layout, written by
    index, bound through two materials' external blocks, drawn green and
    blue, then swapped (the buffer by name, the set by index); unknown
    names find `UINT32_MAX`, an index past the layout and NULL data are
    refused. Passed on Metal (Apple M5 Pro), Debug under Metal API and
    shader validation with warnings fatal and Release under `leaks`
    (none), with every other test; latran's and trellislib's tests pass
    over it. Every test passed on Windows on 2026-10-18, D3D12 under its debug layer and Vulkan under
    the validation layer with synchronization validation, on the Iris Xe
    and the RTX A2000 (reported; step 2 and 3 of "Suggested order" have
    what that run found). Not built on Linux.
- **The uniform buffer cleaned up, written 2026-10-18; run on Metal.**
  From a review of `aprend_uniform_buffer_t`:
  - `aprend_uniform_buffer_create` checks a layout's `uniforms` against its
    `count` (it read through NULL), refuses a uniform whose type is no
    `APREND_UNIFORM_TYPE` (accepted before, and every write of it refused
    later) and a layout of no uniforms and no size, copies nothing for a
    layout of no uniforms (a `malloc(0)` that may return NULL failed the
    call), and prints each failure on its own line with what it was (it
    printed the result string bare, "success" for a failed `malloc`).
    Checks one to a line; the newer conventions throughout the section.
  - A buffer in the device's memory (the material arena's, which only the
    GPU's copies write) is the same struct with no mapping; every update
    call refuses one now, where it would have written through NULL. None
    can reach a caller, so this guards Aprend's own code.
  - `aprend_uniform_buffer_get_layout` and `aprend_uniform_buffer_get_size`,
    as a vertex buffer and a storage buffer have: the layout is what
    `aprend_uniform_layout_get_uniform_index` reads, for a caller that
    holds only the buffer.
  - Removed: `aprend_uniform_buffer_get_spudgpu_buffer_view` and the view
    behind it (made for every buffer and every copy of a set, read by
    nothing: binding sets bind the buffer with an offset and range; no
    caller in the workspace called it), and the stored
    `spudgpu_buffer_view_desc`, whose offset was always 0 and was added
    into every write. The struct is the buffer, its size, its layout and
    its mapping.
  - `aprendbuffers.h` says for uniform buffers what it said for the others:
    a write lands where the GPU reads, so not while a submission that reads
    the buffer is unfinished; a block rewritten every frame belongs in a
    buffer set.
  - `../apricottests`' `test_uniform_buffer_index` checks the getters (the
    buffer's own layout finds the same indices), the three refused
    creates, and a block of no uniforms made with a size and written as
    bytes. Every test passed on Metal (Apple M5 Pro), Debug under Metal
    API and shader validation with warnings fatal and Release under
    `leaks` (none); latran's and trellislib's tests pass over it. Not
    built on Windows or Linux.
- **Buffer offsets follow the device's alignment, written 2026-10-18; run
  on Metal** (the rest of the uniform buffer review).
  - SpudGPU reports `uniform_buffer_offset_alignment` and
    `storage_buffer_offset_alignment` in `SPUDGPU_DEVICE_PROPERTIES`:
    Vulkan's `minUniformBufferOffsetAlignment` and
    `minStorageBufferOffsetAlignment`; D3D12's 256 and 16 (a constant buffer
    view's placement, a raw buffer view's first byte); 256 for both on
    Metal, which has no query. The Vulkan and D3D12 halves were written on
    macOS without either API's headers, and first compiled and run on
    Windows on 2026-10-18 (spudlib b7ac7e6), with every test passing on
    both devices.
  - `aprend_binding_set_create` (and `_update`) refuses a uniform or storage
    buffer entry whose `_offset` is not a multiple of the device's
    alignment for its type, naming the entry and the alignment. Before,
    any offset inside the buffer was taken, and one that wasn't aligned
    was outside Vulkan's and D3D12's rules. Every caller bound at 0.
  - The material arena places blocks at the device's uniform alignment
    (16 at the least), not at 256 everywhere (item 19).
  - The uniform buffer update calls, and the set's create and update calls,
    print each refusal, as create does and as the material's calls do;
    they returned false silently.
  - `aprend_uniform::size` is `array_count`: it was an element count named
    a size. Source-compatible for a caller that fills the struct by
    position (trellislib does); one that names the field changes the name.
  - `../apricottests`' `test_uniform_buffer_index`: the alignments SpudGPU
    reports are powers of two up to 256, an entry at the device's own
    alignment is taken and one at 4 refused (both, on Metal's 256). Every
    test passed on Metal (Apple M5 Pro), Debug under Metal API and shader
    validation with warnings fatal and Release under `leaks` (none);
    latran's and trellislib's tests pass over it. Every test passed on Windows on 2026-10-18, D3D12 under its debug layer and Vulkan under
    the validation layer with synchronization validation, on the Iris Xe
    and the RTX A2000 (reported; step 2 and 3 of "Suggested order" have
    what that run found). Not built on Linux.
- **Vertex buffers are bytes and vertex layouts the pipeline's, written
  2026-10-18; run on Metal.** As Unreal (`FVertexBuffer` and the vertex
  declaration), Hazel 2025 (`VertexBuffer` is a size and its bytes; the
  layouts are in `PipelineSpecification`) and all three APIs have it.
  - `aprend_vertex_buffer_create(instance, size, writes, pData)`,
    `aprend_vertex_buffer_update(buffer, offset, size, pData)` and
    `aprend_vertex_buffer_get_size`, in bytes, as the storage buffer's.
    The buffer holds no layout, no stride and no names: the copied layout's
    names pointed at the caller's strings (a use after free for a caller
    that freed them), and that is gone with it.
    `aprend_vertex_buffer_get_layout` and
    `aprend_vertex_buffer_get_spudgpu_buffer_view` are removed; nothing in
    the workspace called either.
  - `aprend_vertex_layout` (in `aprendpipeline.h`, was `aprend_buffer_layout`
    in `aprendbuffers.h`): `count`, `elements` and a required `stride`.
    `aprend_vertex_element` is `name`, `type` and `offset`; its size follows
    from the type (the `size` field, which nothing kept in step with the
    type, is gone). `APREND_VERTEX_ELEMENT_TYPE` (was
    `APREND_BUFFER_ELEMENT_TYPE`) gains `UINT` to `UINT4`, appended, so a
    `uint` vertex input can be fed. `aprend_vertex_element_type_get_size`
    and `aprend_vertex_layout_get_element_index` are the renamed calls;
    `aprend_buffer_layout_get_total_size` is gone with the computed stride.
    An explicit stride lets a vertex have padding after its last element
    and a pipeline read part of a larger vertex.
  - A graphics pipeline checks each layout (a stride not 0 and a multiple of
    4, each element's type valid, offset a multiple of 4, and ending inside
    the stride) and copies it, names included, so
    `aprend_graphics_pipeline_get_desc` hands back the pipeline's copy, not
    the caller's array.
  - The command list binds vertex buffers at each draw, at the bound
    pipeline's binding strides: a SpudGPU vertex view carries its stride,
    as D3D12's does, so each buffer keeps a view for each stride a pipeline
    has read it at (usually one), and a span's view is made at the first
    draw that reads it. A slot is bound again only where its view changes;
    the views survive a pipeline change, as vertex bindings do on every
    API, and are taken as unbound at each pass.
  - The stride check is replaced by a bounds check: a draw's vertices, an
    instanced draw's instances and the one instance any other draw reads
    must end inside the buffer or span, at the binding's stride and its
    furthest element's end. `SET_TRANSIENT_VERTEX_BUFFER` has no `_stride`;
    the span is read at the pipeline's.
  - A vertex pool's desc takes `vertex_stride` in place of a layout.
    Aprend's lit layouts have their strides (`sizeof(aprend_lit_vertex)`,
    `sizeof(aprend_lit_instance)`).
  - Callers in the same change: trellislib (its four layouts,
    `src/trellisrenderresources.cpp`; four vertex buffers sized in bytes,
    `src/trellisscene.cpp`; the solid pool's stride, `src/trellissolids.cpp`),
    latran (`core/src/latranrender.cpp`) and `../apricottests`.
  - `../apricottests`' `test_vertex_layouts`: six refused pipelines (stride
    0, stride 10, an offset of 2, an element past the stride, an unknown
    type, a `uint` input fed by an `INT` element); the pipeline's copy of a
    layout whose caller overwrote its array and names; a padded,
    interleaved vertex (32 bytes, elements to 24) read for its positions
    alone; a `uint` input that draws for 7 and not for 3; two draws past
    the end of a 6-vertex buffer refused. Every test passed on Metal (Apple
    M5 Pro), Debug under Metal API and shader validation with warnings
    fatal and Release under `leaks` (none); latran's and trellislib's tests
    pass over it. Every test passed on Windows on 2026-10-18, D3D12 under its debug layer and Vulkan under
    the validation layer with synchronization validation, on the Iris Xe
    and the RTX A2000 (reported; step 2 and 3 of "Suggested order" have
    what that run found). Not built on Linux. That includes D3D12, whose
    vertex view's stride is what the per-stride views are for.
    Trellis_Avalonia and latran ran over it on Windows the same day
    (reported).
- **Shader reflection, written 2026-10-11 and compiled on all three
  backends** (item 18; closes item 1). Type-checked with GCC 11 as the
  entry above before that. Part of it has run without a device: see the
  last bullet.
  - `aprendshader.h` (new): the shader calls moved out of
    `aprendpipeline.h`, and `aprend_shader_reflection`. A shader reflects
    its SPIR-V at creation through SPIRV-Cross's core
    (`aprendreflect.cpp`, `aprendreflect_internal.hpp`): each descriptor
    binding (set, binding, type, count, a block's size and members, an
    image's view shape, scalar type and depth compare, storage read-only),
    the push constant block, a vertex shader's inputs, a fragment shader's
    color output count and a compute or mesh shader's workgroup size, each
    marked used if the stage's one entry point reaches it. Block members
    are listed flat with their real offsets, array and matrix strides and
    majority, structs and arrays of structs as `lights[2].color`. Creation
    fails for SPIR-V that can't be read, has no entry point for the stage,
    or uses a texel buffer, input attachment, acceleration structure,
    unsized descriptor array, multisampled or 1D image.
  - Pipelines are checked against their shaders: entry point, every used
    binding declared by the layout at its set with its type, count and
    stage, push constant bytes covered by a range of the stage, and every
    used vertex input fed at each location by an element of its kind of
    number. `_vertex_inputs_by_name` (new, last field of the graphics
    pipeline desc) gives elements their locations from the inputs' names
    (item 11's positional locations).
  - `aprend_binding_layout_create_from_shaders`, and `_name` (new, last
    field) on `aprend_binding_desc`; `aprend_binding_layout_find`,
    `_get_binding`, `_get_binding_count`, `_get_block`,
    `_get_uniform_layout`. A layout made from shaders keeps its bindings'
    names and blocks.
  - A command list checks the texture views of each set it binds for a
    pipeline against the shape the shader declares, and a stencil read as
    other than unsigned or a depth as other than float.
  - `APREND_UNIFORM_TYPE_MAT2` and `_MAT3`; `aprend_uniform_layout::size`
    (new, last field). `aprend_uniform_buffer_create` sizes from it, and
    std140's matrix columns of 16 are kept in the footprint and by
    `aprend_uniform_buffer_update_by_name` (item 1).
  - The material (item 19) takes its names and blocks from the layout: its
    desc is a layout and the names of uniform blocks it shouldn't own. A
    member is set by its path, or its binding's name and its path.
  - CMake: SPIRV-Cross's core is linked PRIVATE, the target SpudLib already
    made on Metal and D3D12, built here from `../.deps/SPIRV-Cross` on
    Vulkan. Configured and built that way on all three as of 2026-10-12.
  - **What has run**, on Linux in a scratch build, not through the repo's
    CMake: `tests/aprendreflect_tests.cpp` (new) against SPIR-V from
    glslang 16.6.0, under AddressSanitizer and UBSan: passes. And a
    scratch harness (not in the repo) that linked all of `src/render`
    against a SpudGPU of stubs that create nothing and return success:
    layouts from shaders, every pipeline check passing and failing as
    meant, a material set by name and completed, a render pass, a draw
    list, a compile refused for a wrong view shape and passing once fixed,
    submits writing the material's frame copies (since replaced by the
    material arena, below) with the "read in this
    turn" rule, three frames, and teardown through the release queue,
    with no sanitizer error. The same harness made trellislib's four
    pipelines (geometry, line, solid, overlay) the way
    `trellisrenderresources.cpp` does, from its GLSL compiled with glslang,
    and none was refused by the new checks. That exercises Aprend's logic
    and none of SpudGPU, so nothing is known about any backend from it.
  - Push constants are checked per member the stage uses (SPIR-V's active
    ranges), so two stages can share one block and each take a range for
    its own part.
- **Layouts of the same bindings are one layout, written 2026-10-11 and
  compiled on all three backends** (was step 1 of "Next, from the review").
  Run in the stub harness, not on a device.
  - `aprend_binding_layout_core` (`aprend_internal.hpp`): a layout's
    bindings without names (sorted by number), its SpudGPU layout and its
    descriptor pools. Both create calls look for the instance's core of
    the same bindings (number, type, count, stages) and make one only if
    there is none, so two layouts of the same bindings share one SpudGPU
    layout. Each handle keeps its own names and blocks.
  - A core is held by each layout handle and each binding set made through
    one, and freed with its last holder. Every holder lets go on the thread
    using the instance (a release function, or a failed create), so the
    count takes no lock and destroy calls stay safe from any thread. A set
    holds the core itself, so it no longer reads the handle it was made
    through after creation.
  - The compile, a dispatch, and a render or compute pass compare a set's
    core with the pipeline's layout's (`aprend_binding_set_fits`) where
    they compared the handles. A material made for one layout is drawn with
    a pipeline that declares another of the same bindings.
  - `aprendpipeline.h`, "Layouts of the same bindings": what counts (names
    don't, stages do), and that a layout made from shaders is made from
    every shader that reads the set and given to each pipeline that binds
    it.
  - Run in the stub harness: two layouts made separately from the same
    shaders, and one by hand with other names, share a core; another stage
    mask doesn't; a material made for one layout compiled into a pass
    whose pipeline declares the other; handles destroyed before the sets
    made through them; every core gone once the last layout and set are.
- **The material arena, written 2026-10-11 and compiled on all three
  backends** (was step 1 of "Next, from the review" after layouts were
  shared). Run in the stub harness, not on a device.
  - A material's owned uniform block is a fixed range of the instance's
    material arena (`aprend_material_arena`, `aprend_internal.hpp`) for
    the material's life, in place of a uniform buffer set: 64 KiB uniform
    buffers in the device's memory (`aprend_uniform_buffer_create_device`,
    new, internal), ranges 256-byte aligned and handed out with the pools'
    range list. A block larger than 64 KiB gets a buffer of its own, which
    goes with it. One copy of a block on the GPU, not one per frame, and
    one descriptor set for a material that holds no caller's buffer set.
  - `aprend_material_set_uniform` and `_bytes` mark the block dirty. At the
    submit of a list that binds the material, each dirty block is written
    to a span of the frame's transient memory and a copy from it into the
    block's range is recorded in the list's upload list, the arena
    buffer moved COMMON to COPY_DEST before and COPY_DEST to the uniform
    state after, once per buffer. The flag is cleared once the submission
    has gone in. A value set is seen by every later submission, of one
    frame or the next; the "read in this turn" rule and
    `aprend_instance_t::frame_count` are gone.
  - A material that owns a block needs an instance with transient memory
    and is refused without it. `aprend_material_destroy` puts the whole
    material on the release queue, so its ranges are handed out again only
    once the GPU has finished with what was submitted before it. The
    arena's buffers go with the instance.
  - `aprend_uniform_buffer_t`'s destructor unmaps, and destroys a view or
    buffer, only where there is one.
  - Run in the stub harness: the first submission copies the zeroed and
    set block (bytes read back from the transient chunk), an unchanged
    material copies nothing, a change between two submissions of one frame
    reaches the second, the next frame copies nothing, no frame-locked
    sets; three materials share an arena buffer at different ranges, a
    destroyed one's range comes back after the queue and is handed out
    again; a material is refused on an instance without transient memory
    unless its block is external; a block over 64 KiB gets a buffer of its
    own, gone after the material. Never on a GPU: the barrier that orders a
    copy after earlier submissions' reads is SpudGPU's, unverified on every
    backend, as for the staged buffers (item 13).
- **No tests in this repo of anything that needs a device.** There is no
  `tests/aprend_tests.*`: the repo's tests are headless with no GPU. The two
  parts of Aprend that run without a device each have a test, both
  registered with ctest behind `APRICOTFIELDS_BUILD_TESTS`:
  `tests/aprendranges_tests.cpp` (the pools' range list; whether it has
  been built or run is not recorded) and `tests/aprendreflect_tests.cpp`
  (reflection; passed in a scratch build on Linux, not recorded as run
  through this repo's CMake). The tests that need a device are
  `../apricottests` (`aprend_gpu_tests`), run on Metal (macOS) and on D3D12
  and Vulkan (Windows).
- **Reviewed by reading on 2026-10-13; nothing compiled or run for it.**
  Every file of `src/render` except `aprendpools.cpp`, `aprendreflect.cpp`
  and most of `aprendpipeline.cpp` was read against this file, and this
  file was brought into line with the code. What the review found wrong
  is items 37 and 38 and the additions to items 8, 12, 13 and 20; what it
  found already fixed was taken out of items 8, 12, 14 and 20. No code was
  changed.
- **Multiple render targets, depth bias, debug labels and GPU timers,
  written 2026-10-15; compiled on macOS over Metal through Trellis_macOS, which ran (reported 2026-10-16)** (items 25, 26, 29):
  - The Trellis run used one color target, no bias, no labels and no
    timer, so it shows the arrays and the new pipeline checks build and
    pass for what Trellis draws, and no more.
  - **Run on Windows on 2026-10-16 by `../apricottests`, all passed:** D3D12
    on an NVIDIA RTX A2000 Laptop (memory of its own) and an Intel Iris Xe
    (unified), debug layer clean; Vulkan on both, validation clean.
    `test_two_targets`, `test_pipeline_must_match_pass`,
    `test_depth_bias`, `test_labels` and `test_gpu_timer` (a time above 0
    and below a second) each passed there. The Vulkan run found that a
    descriptor's image layout and a binding's shader stages reached Vulkan
    untranslated, which turned every sampled texture black; fixed in
    SpudGPU.
  - **Run on macOS on 2026-10-16 by `../apricottests`, all passed:** Apple
    M5 Pro, Debug under Metal API and shader validation with warnings made
    fatal, and Release; no leak at exit. The timer read 0.08-0.11 ms for
    its pass (above 0 and below a second; not compared with a known cost).
    The run found five things in SpudGPU's Metal backend, all fixed there:
    samplers made without `supportArgumentBuffers` though every set is an
    argument buffer; every set bound to the vertex and fragment stages
    whatever its bindings' stages (a validation error for a stage whose
    function has no argument there); a depth-stencil state and the
    rasterizer state set again when the encoder already had them, which
    two pipelines with equal depth state do; the timestamp counter set
    used after its autorelease pool had freed it, which crashed creating a
    timer; and the array of devices from `spudgpu_enumerate_devices` never
    freed (Vulkan too; it is the instance's now, as on D3D12). Labels have
    been compiled and submitted, not looked at in RenderDoc, PIX or Xcode.
  - SpudGPU: `spudgpu_shader_pipeline_desc` has `color_attachment_formats`,
    `blend_attachments` and `color_attachment_count` in place of the one
    format and blend, and `depth_bias_constant`, `_slope`, `_clamp`. The
    bundle inheritance desc has the formats too. New device properties
    `depth_bias_clamp`, `independent_blend` and `pass_timestamps`, the
    first two Vulkan features enabled where reported. The checks every
    backend makes on these are written once (`src/gpu/spudgpushared.c`).
    Vulkan's blend state now translates the caller's factors and ops: it
    had been fixed to straight alpha whatever was asked.
  - SpudGPU debug labels (`spudgpu_cmd_begin_debug_label`, `_end_`):
    `VK_EXT_debug_utils`, enabled on the instance wherever offered; PIX's
    ANSI events on D3D12; debug groups on Metal, on the render encoder in a
    pass and the command buffer outside one.
  - SpudGPU timestamp sets (`spudgpu_create_timestamp_set`, reset, resolve,
    `spudgpu_get_timestamps` in nanoseconds), written by a pass at its start
    and end through `spudgpu_rendering_begin_desc::timestamp_set`. A query
    pool on Vulkan, a query heap and readback buffer on D3D12, a counter
    sample buffer sampled at stage boundaries on Metal. A new `query` file
    in each backend.
  - Aprend: the pipeline desc's `color_attachment_formats` and
    `color_attachment_count` (0 for depth only), `_blends`, and the depth
    bias; a render pass takes up to `APREND_MAX_COLOR_TARGETS` targets,
    each checked against the pipeline's format at its index. The compile
    now refuses a pipeline set inside a pass whose targets it doesn't
    match (count, formats, depth), which nothing checked before.
  - Aprend labels (`APREND_COMMAND_BEGIN_LABEL`, `END_LABEL`), checked at
    compile for nesting and for not crossing a pass's boundary, and closed
    in the SpudGPU list on a failed compile so it stays balanced.
  - `aprend_gpu_timer` (`aprendtimer.h`): slots a pass is timed into,
    through `BEGIN_RENDERING`'s `_timer` and `_timer_slot` or a render
    pass's, kept once for each frame in flight and read a frame late.
  - Every caller in the workspace follows: spudgpusamples, libdemo,
    trellislib, Latran and `../apricottests` set the formats as arrays.
  - What to check first: a pipeline with two targets on each backend; a
    shadow pass's bias; labels in RenderDoc, PIX and Xcode; a timer's
    reading against a known cost, on Metal above all.
- **Command memory for each frame and the upload batch, written 2026-10-15;
  compiled on macOS over Metal through Trellis_macOS, which ran (reported 2026-10-16)** (items 37 and 6):
  - On Metal there is no allocator memory to leak, so the run shows the
    per-frame lists and the batch work there, not that the D3D12 leak is
    gone. Whether Trellis updated or read back a texture in the run is not
    recorded.
  - An instance has a SpudGPU command allocator for each frame index, and
    the SpudGPU lists recorded from it. `aprend_instance_next_frame` resets
    the index's allocator once its wait has succeeded. A list is handed out
    for one recording and not again until then, so none is reopened while
    it may still run (Vulkan can't reset a pending command buffer).
  - An `aprend_command_list` owns no SpudGPU list. A compile takes one from
    the frame, or records over its own if it was compiled in this frame and
    not submitted since; the upload list of its staged buffers is taken at
    submit. Every list is refused at submit in any frame but the one it was
    compiled in, counted by a frame number, which replaces the two checks by
    frame index (per-frame sets, transient spans). Those compared indices
    only, so a list `frames_in_flight` frames old passed them.
  - Texture updates and readbacks and framebuffer clears are recorded into
    one batch for the frame, from the frame's lists, which the next
    submission carries ahead of its own lists. `aprend_instance_next_frame`
    and `aprend_instance_wait_idle` submit it if nothing has; a readback
    submits it and waits for that serial. No more allocator, list and
    submission per call. What is destroyed while a batch waits is retired a
    serial later, for the submission that will carry it.
  - A texture update is staged in the frame's transient memory where the
    instance has some and the copy's offset rule is a power of two, and in a
    buffer of its own (as before) otherwise. The offset rule is new in
    SpudGPU: `spudgpu_get_image_buffer_copy_offset_alignment` (512 on D3D12;
    the texel block size, and of 4 on Vulkan; the block size on Metal).
    `spudgpu.h` also now says when an allocator may be reset and that a list
    is not reopened while its last recording may run.
  - The texture update and readback calls refuse a format with no whole
    number of bytes to a texel, where they divided by zero (item 38).
  - **The batch has an allocator of its own, fixed 2026-10-16; run on
    D3D12 (NVIDIA RTX A2000 Laptop) by `aprend_gpu_tests`, all passed.**
    The batch was taken from the frame's one allocator and stays open
    while other lists of that allocator are opened, which D3D12 refuses
    (`COMMAND_ALLOCATOR_CONTENTION`): the compile's list stayed closed
    and `BeginRenderPass` on it faulted in the driver. Each frame index
    now has two pools, `recordings` and `batches`, each an allocator and
    its lists, both reset by `aprend_instance_next_frame`. `spudgpu.h`
    now says that one list of an allocator is open at a time. Run on Vulkan
    on Windows the same day (both devices, validation clean), and on Metal
    by `aprend_gpu_tests` (validation clean).
  - **The batch and the per-frame memory, run on Windows on 2026-10-16 by `../apricottests`, all passed:** D3D12
    on an NVIDIA RTX A2000 Laptop (memory of its own) and an Intel Iris Xe
    (unified), debug layer clean; Vulkan on both, validation clean.
    `test_updates_batched` passed there: two updates in order ahead of a
    pass, staged in transient memory (so D3D12's 512-byte offset rule
    inside a chunk held), and an update carried across a frame turn by
    `aprend_instance_next_frame`. What the tests don't show is the leak of
    item 37 gone: they turn the frame twice. That needs a host running for
    minutes on D3D12 with memory watched.
  - **Run on macOS on 2026-10-16 by `../apricottests`, all passed**, with
    `test_updates_batched`, under Metal validation (see the entry above).
  - **Memory flat on D3D12, reported 2026-10-16:** Trellis_Avalonia on
    Windows over D3D12 ran idle for 20 minutes with no growth in memory,
    the check item 37 was waiting on. Whether its viewport drew frames
    throughout while idle, and on which device, isn't recorded.
    **It showed less than it seemed** (found 2026-10-17): trellislib never
    called `aprend_instance_next_frame` - it waits for the GPU after every
    render and kept one frame in flight, so it saw no need - and an idle
    window doesn't render. So the per-frame allocators were never reset
    under a host, and every compile took a new SpudGPU list. It came to
    light when a viewport glow's bloom took a new pooled chain every frame
    and ran out of memory. trellislib now turns the frame after each render
    call's wait. Memory over minutes of rendering, not idling, is still to
    be watched.
- **Bloom and post-processing, written 2026-10-16; run on Metal the same
  day**
  (item 36): `APREND_COMMAND_BLOOM` (source, target, threshold, knee,
  radius, intensity, levels), outside a pass: four fragment passes in
  `shaders/aprend_bloom_*.frag` over a float chain taken from the
  instance's new render target pool (`aprendrendertargets.cpp`), added
  onto the target. No bloom handle and no header of its own: the command's
  doc in `aprendcommands.h` describes bloom, `aprendcontext.h` ("Textures a
  command uses in between") the pool, and
  `aprend_get_fullscreen_vertex_shader` (`aprendshader.h`) hands the
  built-in vertex shader to a caller's own pass over a texture.
  Built-in pipelines per stage and target format (`aprendbuiltin.cpp`), a
  shared `aprend_record_fullscreen_pass` (mip generation records through
  it too), each view keeping the set that reads it, and a command list
  keeping what its compile took from the pool until it is submitted.
  `../apricottests`' `test_bloom` **passed on Metal on 2026-10-16** (Apple
  M5 Pro) on its first build: Debug under Metal API and shader validation
  with warnings fatal, and Release; no leak at exit. The white square read
  255 at its centre, 34 ten texels out on both sides (white, alpha the
  same), 2 in the corner. On 2026-10-17 it passed on D3D12 and Vulkan on
  Windows, on an Intel Iris Xe and an NVIDIA RTX A2000 (reported).
- **Mip generation in a command list, written 2026-10-16; run on Metal
  the same day** (item 31): `APREND_COMMAND_GENERATE_MIPS`, outside a pass,
  checked as it is sent (texture, instance, level, filter, usage, format)
  and again at compile (a resize since), with the base level's contents
  taken from the list's own layout tracking. Its views and sets, and those
  of `aprend_texture2d_generate_mips`, are now kept by the texture
  (`aprend_mip_chain` in `aprendimages_internal.hpp`) instead of made and
  retired each call; both forms record through one
  `aprend_record_mip_pass`. `../apricottests`' `test_generate_mips_command`
  renders mip 0, generates, and samples mip 3 in one list, twice, and
  checks three compile failures. **Passed on Metal on 2026-10-16** (Apple
  M5 Pro) on its first build: Debug under Metal API and shader validation
  with warnings fatal, and Release; no leak at exit. **Passed on D3D12 and
  Vulkan the same evening** (Windows 11, Intel Iris Xe and NVIDIA RTX
  A2000, Debug; D3D12 under cdb with the debug layer clean, Vulkan with
  the validation layer clean), on the first run, with every other check.
- **Aprend ships shaders, and generates mips with them; written
  2026-10-16; run on Metal the same day** (items 22, 31, 7):
  - GLSL in `shaders/` is compiled to SPIR-V by glslc at build time and
    embedded in the library as C arrays (`-mfmt=c`), on every platform, as
    trellislib and `../apricottests` already do: nothing to deploy or load
    at run time. glslc is now needed on Apple too. The block that compiled
    `apricot_default.vert`/`.frag` to `.spv` files on Windows and Linux
    only, and linked Vulkan for it, is gone with those shaders (item 7);
    nothing loaded them.
  - `src/render/aprendbuiltin.cpp` (`aprendbuiltin_internal.hpp`): the
    instance's built-in objects, made through Aprend's own create calls
    when first needed and destroyed through the release queue with the
    instance. So far a full-target vertex shader (`aprend_fullscreen.vert`,
    three vertices, no buffer), a downsample shader
    (`aprend_downsample.frag`, a separate texture and sampler since
    SpudGPU's Metal backend has no combined one), its layout, a clamped
    sampler for each filter, and a pipeline for each format used.
  - `aprend_texture2d_generate_mips` (item 31's immediate form): every
    level below a base level of every layer, a pass each, sampling the
    level above through a one-mip, one-layer 2D array view (the shader
    declares a `texture2DArray`: D3D12 has no 2D view of a layer past the
    first, and a 2D shader texture given an array view is undefined
    there); recorded into the upload batch,
    each level left in the layout it was found in. Refused, printed: a
    texture without render target and shader resource usage, a format
    outside the eleven every backend renders to and filters, a base level
    some layer has never had written.
  - `../apricottests`' `test_generate_mips` checks a two-layer 8x8 chain
    level by level and the refusals. **Passed on Metal on 2026-10-16** (Apple
    M5 Pro, unified memory), on its first run: Debug under Metal API and
    shader validation with warnings fatal, and Release; no leak at exit.
    **Passed on D3D12 and Vulkan on 2026-10-16** (Windows 11, Intel Iris Xe
    and NVIDIA RTX A2000; D3D12 under cdb with the debug layer clean, Vulkan
    with the validation layer clean), after a fix in SpudGPU's D3D12 backend:
    every 2D view there saw layer 0, so layer 1 was rendered into and
    sampled from layer 0. The array-view source came after, and has run on
    D3D12 and Vulkan, and then on Metal (M5 Pro, 2026-10-16: Debug under
    validation with warnings fatal, and Release; no leak at exit).
- **Struct bases, 2026-10-20.** Aprend's 24 descs and lit scene settings
  start with an `APRI_STRUCT_BASE` (`apricore.h`; `aprendcontext.h`, "Struct
  bases"), and every other public struct is marked frozen.
  `APREND_COMMAND`'s `_params` is padded to 120 bytes (the command to 128).
  `aprend_graphics_pipeline_create()` and `aprend_compute_pipeline_create()`
  take their desc by pointer, as every other create does. **Run on Metal on
  2026-10-20** (M5 Pro): `../apricottests` passes, Debug under Metal API and
  shader validation with warnings fatal, with a new `test_struct_bases`
  (a zeroed base, another struct's type and a chained struct refused; the
  getter's base). Not compiled for D3D12 or Vulkan: the backends are
  untouched, but every caller's desc changed, so a Windows run is due.

## Wrong things

### 1. Uniform layouts are declared by hand

Closed 2026-10-11 by item 18, compiled and not run on a device: `aprend_binding_layout_get_uniform_layout`
gives a uniform buffer's layout as the shader has it, `mat2` and `mat3` have
types, structs and arrays of structs are listed member by member, and every
offset is the SPIR-V's. A layout written by hand is still the caller's to
get right, and `aprend_uniform_buffer_update_by_name` assumes std140 (a
uniform block in `layout(std430)` or `scalar` through an extension would be
written at the wrong strides; the material, which reads the strides, is
not).

### 2. A host-visible storage buffer on D3D12

Closed 2026-10-10, compiled on all three backends. A storage buffer written directly is
`SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE`, which a device has when it is
created with `SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY` (since 2026-10-21;
before, `SPUDGPU_DEVICE_PROPERTIES::device_mappable_memory`, item 39) and which D3D12 backs
with a custom heap, not an upload heap. A device without it takes the staged
path for storage buffers alone. The D3D12 heap has never run: an integrated
GPU under D3D12 is the case to test first.

### 3. Nothing fills a texture's mips but the caller

A texture created with `mip_levels` 0 gets every level, and each starts
undefined. `aprend_texture*_update` writes any one level, so a caller with its
own mip data can fill the chain. A 2D texture without it can now have the
chain made from its first level (`aprend_texture2d_generate_mips`, item 31;
written, not run). A 3D texture still can't: such a texture wants
`mip_levels` 1, or a sampler whose `max_lod` is 0.

### 6. Immediate operations: what is left

Texture updates and readbacks and framebuffer clears are recorded into one
batch for the frame, carried by the next submission, and an update is
staged in the frame's transient memory (see "State of the code",
2026-10-15). Left over:

- **A readback blocks**, and has to: the caller reads the result. One that
  hands the result over later (a pending read the caller polls) is the only
  way it wouldn't, and nothing asks for it. It also submits the whole batch
  early, with whatever updates the frame has recorded so far.
- **An update can't be placed between two commands of a command list.** The
  batch goes ahead of the next submission's lists, so an update between two
  passes of one list (write, draw, write again, draw) is two lists. A
  command that records a copy from a transient span inside the list is the
  way, and nothing asks for it yet.
- **The staging falls back to a buffer of its own** on an instance with no
  transient memory, past `transient_memory_limit`, and for a format whose
  copy offset rule is not a power of two (a 12-byte texel on Vulkan). Each
  of those still makes and retires a buffer per call.
- **The batch's layouts are written when it is recorded.** If the
  submission that carries it fails, and `aprend_instance_next_frame` then
  drops it, the tracked layouts say what the dropped barriers would have
  done. A failed submission is a lost device in practice.

### 7. The default shaders are unused

Closed 2026-10-16 with item 22: `shaders/apricot_default.vert` and `.frag`,
loaded by nothing, are gone, and with them the block of `CMakeLists.txt`
that compiled them on Windows and Linux only.

### 8. Older-generation habits still to convert

Per `CLAUDE.md`, only when the function is already being changed: joined
argument checks (`if (!instance || !desc)`, 14 of them in `src/render` as of
2026-10-13), failure reported by `printf` with no result code, and
`std::vector` growth inside C entry points with nothing catching
`std::bad_alloc`. The two `malloc`s this item named as unchecked (the layout
copy in `aprend_vertex_buffer_create`, the file buffer in
`aprend_shader_read_from_file_spirv`) are checked now.

Where the vector growth matters most: `aprend_send_command` (every array
it copies), and `aprend_framebuffer_create` and `_resize`.
`aprend_command_list_compile` catches it over its whole walk since
2026-10-18, and closes the SpudGPU list on the way out.

In the command list: vertex buffers, the index buffer, viewports and
scissor rects sent before a pass are passed to SpudGPU where they stand,
outside the pass, and whether they are still set inside it is the backend's.
(`SET_INDEX_BUFFER` with a NULL buffer is refused at compile; this item
used to say it was dereferenced.)

Smaller things the review of 2026-10-13 found, none of them a wrong result
today:

- **`APREND_DESTRUCT__T` is two statements**, not wrapped. Under an `if`
  with no braces only the destructor is guarded:
  `aprend_swap_chain_destroy` is the one such use, and is saved by
  `free(NULL)` being allowed.
- **Two comments name a function of a caller**
  (`create_offscreen_color_target`, in `aprendframes.cpp` and
  `aprendimages_internal.hpp`), which `CLAUDE.md` rules out. They mean "the
  caller's array may be a local".
- **`aprend_framebuffer_texture_spec::use_for_gui`** does exactly what
  `shader_resource` does and names what a caller does with the texture.
  Removing it changes the header and every caller that sets it.
- **`APREND_TEXTURE_VIEW_TYPE_RENDER_TAGET`** is misspelt in the public
  enum. Renaming it changes every caller.
- **`aprend_destroy_texture_view`** is the one destroy call with the verb
  first; the rest are `aprend_<object>_destroy`.

### 37. The instance's command allocator is never reset

Closed 2026-10-15; run in Trellis_macOS on Metal, and by `../apricottests` on Metal, and on D3D12 and Vulkan on Windows (2026-10-16) (see "State of the code"). Each frame index
has its own allocator, reset by `aprend_instance_next_frame` after its wait,
and a recording takes a list of the current frame's; a list compiled in
one frame is refused at submit in a later one. SpudGPU needed no new call
for it: a list stays tied to the allocator it was created on, which is what
Vulkan requires, and Aprend keeps lists for each frame. The D3D12 leak it
fixes was found by reading and never measured before the fix; after it,
Trellis_Avalonia ran idle on D3D12 for 20 minutes with memory flat
(reported 2026-10-16) - which showed little, as trellislib didn't turn the
frame until 2026-10-17 (see "State of the code"). Left: memory watched
over minutes of a host rendering.

### 38. Unchecked inputs and stale lists, from the review of 2026-10-13

Numbered out of order, as item 31. Each was found by reading and none has
been reproduced.

- **Fixed 2026-10-15, not compiled: `aprend_texture2d_update` and
  `_get_data` (and the 3D pair) divided by the format's bytes per pixel**,
  0 for a block-compressed format and for one with no known pixel size.
  They now refuse such a format. A compressed texture still has no upload
  path: rows of texels are not what it is made of.
- **`aprend_shader_read_from_file_spirv` leaks the file's bytes** when the
  read or the file's release fails: both go to the failure label, which
  frees the handle and not `data`.
- **A list that presents can outlive the swap chain it was compiled
  for.** Submit checks that the swap chain's acquired image is still the
  index the list was compiled against. After `aprend_swap_chain_resize`
  and a new acquire the index is often the same one, of a new swap chain:
  a list compiled before the resize then passes the check and copies into
  a back buffer that was destroyed. A count on the swap chain that a
  resize moves on, compared at submit, would refuse it. Item 32 has the
  same shape for textures.
- **`PRESENT_TEXTURE` checks neither size nor format.** A source smaller
  than the back buffer is copied into its corner and the rest of the back
  buffer is left undefined (it is moved from `UNDEFINED`); one of another
  format is handed to SpudGPU's blit as it is.
- **`aprend_swap_chain_acquire` keeps whatever index SpudGPU returns**,
  and SpudGPU's acquire has no way to say it failed.
- **`aprend_texture2d_resize` leaves the texture's views pointing at the
  old image.** The header says views are destroyed before a resize;
  nothing checks, and the old image is released once the GPU has passed
  it, so a view kept across a resize is a view of a destroyed image.

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
  Two layouts of the same bindings share one SpudGPU layout (see "State of
  the code"), so a debug name would be a handle's, not the SpudGPU
  object's.
- A layout is checked against the SPIR-V when a pipeline is made from it
  (item 18); a binding set against its layout, and its views' shapes
  against the shader when it is bound.
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
- **Locations are positional by default**: element N across the bindings
  is location N. `_vertex_inputs_by_name` (item 18) gives them from the
  shader's input names instead, and a shader that skips a location can be
  fed that way.
- **Only 32-bit element types.** No normalized 8- or 16-bit formats (a
  color in four bytes, a packed normal), no half floats: vertices are two to
  four times larger than they need be. Appended to
  `APREND_VERTEX_ELEMENT_TYPE` when they come, with the pipeline's format
  map, scalar-type check and the multiple-of-4 rule (a 16-bit format reads
  from 2-byte offsets on every API) changed with them.
- **What a draw reads is checked only where the compile can see it**
  (2026-10-18): a draw's vertices, an instanced draw's instances, and the
  one instance a draw that isn't instanced reads. An indexed draw's
  vertices and an indirect draw's anything are read where the indices or
  arguments say.
- **Nothing checks how the bytes were packed.** A vertex buffer is bytes and
  the pipeline says what they are, so a buffer packed at one stride and
  read at another draws garbage inside its bounds, as on every API. A draw
  list's `_instance_stride` is the stride it packs instances at, and is not
  compared with the pipeline's binding (the transient command had a
  `_stride` that was, until 2026-10-18).
- **A compile that fails inside a pass after a PUSH_CONSTANTS** leaves a
  Metal encoder with push constants no draw used, and Metal's validation,
  run with warnings fatal, stops at that encoder's end even though the list
  is never submitted. SpudGPU's Metal backend sends render push constants
  as they are pushed; sending them at the next draw (as its compute path
  does at the dispatch) would end it, and is SpudGPU's to change.

### 12. Frames in flight: what is left

Buffer sets give each frame in flight its own copy of a uniform or storage
buffer (see "State of the code"), so the frame being written doesn't share a
buffer with one the GPU is reading. Left over:

- **Pacing counts only what goes through Aprend**, on one queue. Every
  submission is made by `aprend_instance_submit`: command lists, and the
  immediate operations, of which only a readback then waits for its own
  serial (item 6). Each signals
  the instance's fence to the next serial, and `aprend_instance_next_frame`
  waits for the last serial of the frame that used the index before. Work a
  caller submits to SpudGPU itself is outside it. An instance submits on one
  queue, the queue of its first submission, and a submission on another is
  refused (written 2026-10-10): the serials are an order of
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
  - Command memory for each thread, so different lists can be compiled
    from different threads as they can already be recorded. Each frame
    index has an allocator of its own now (item 37); recording in parallel
    needs one for each thread and frame index, with the frame's lists kept
    apart the same way. Not before something records in parallel.
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
- **Some destroy calls don't go through the queue**, each for its own
  reason. The swap chain waits for its queue, as above. A uniform or
  storage buffer set, a framebuffer, a pass and a draw list are freed at
  once: what they own on the GPU (a set's buffers, a framebuffer's views
  and textures) is retired, and the handle itself holds nothing the GPU
  reads. So a binding set that still names a destroyed buffer set reads
  freed memory at its next `aprend_binding_set_update`, and such a destroy
  is not one of the calls that is safe from any thread while the handle is
  in use.
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
- **A buffer written once gives its staging back; one written many times
  doesn't** (written 2026-10-18, see "State of the code"; the staged path
  has not run). An `APREND_BUFFER_WRITES_ONCE` buffer, and every pool
  block, stages each write in a buffer of its own and retires it after
  the submission that copies it. An `APREND_BUFFER_WRITES_MANY` buffer
  keeps a staging buffer of its own size for life, so it costs its size
  twice on a device with its own memory. That is the right price for one
  rewritten often; one rewritten rarely would be cheaper staged a write at
  a time, as `ONCE` is, and nothing chooses that.
- **Each `ONCE` write and each pool range makes a SpudGPU buffer of its
  own.** Many small ranges allocated together (a scene's meshes loaded at
  once) are as many buffer creations. A shared staging buffer, written
  through by many small writes and retired whole once the last copy from
  it has run, would make them one. Not built.
- **A write must not be made while submitted work that uses the buffer is
  unfinished.** On a unified device the write lands under the GPU; on a staged
  one the copy that hasn't run yet reads the new bytes. The same rule either
  way. Buffer sets lift it for uniform and storage buffers (item 12); the
  release queue doesn't, since nothing is destroyed.
- **Every staged write is its own copy command.** A `MANY` buffer's
  ranges merge only where they overlap or touch, so a thousand scattered
  small writes are a thousand copies in the upload list. Nothing merges
  ranges whose gap is small, which would copy a little more for fewer
  commands; that threshold would be a choice nothing has measured.
- **A pool is never compacted and frees nothing by itself.** Ranges don't
  move. A pool's table of allocations only grows: an entry is used again,
  never removed, so a pool that once held a million ranges keeps a million
  entries (24 bytes each) until it is destroyed.
- **Every submission flushes all of the frame's transient memory handed
  out so far**, from the start of each chunk, not what was written since
  the last submission. Correct, and more than is needed for a frame of
  many submissions on a device whose upload memory needs the flush.
- **Uniform buffers stay host-visible on every device**, on purpose: they are
  small and rewritten every frame, and a staged copy per buffer per frame
  costs more than the bus read it saves. A large uniform buffer written once
  would be better staged; nothing asks for it.
- **A texture upload is staged in transient memory** and recorded into the
  frame's batch (item 6); where it falls back to a buffer of its own is
  item 6's.
- **A compiled list holds a pointer to each buffer it uses**, as it does
  to each texture. Destroying the buffer and then submitting the list is
  refused since 2026-10-18 (item 32).
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
  needed. Reflection (item 18) is written and says which storage bindings
  are read-only (`aprend_shader_resource::read_only`); nothing carries that
  to a pipeline or a command list yet, so the barriers are as they were.
- **A buffer a shader writes has no way back to the CPU**: there is no
  storage buffer readback.
- **A compute pipeline lasts until the next pass or present**, and is set
  again after either. That is the rule that holds on every backend; on some
  it would have survived.
- **Dispatches are direct only.** No indirect dispatch.
- **The compute pass object is `aprend_compute_pass`** (item 20), and a
  material is bound for a dispatch with `SET_MATERIAL`, since
  `SET_BINDING_SET` serves both. What Hazel's `ComputePass` has beyond it
  is inputs set by name, which is item 20's. Its resource states are
  tracked by NVRHI; Aprend's own tracking does that job.
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
  - A device created without `SPUDGPU_FEATURE_LAYERED_RENDERING` can't
    make a 2D array view to render to, and Aprend has no fallback: the
    caller renders a layer at a time through 2D views there. Aprend has no
    query of its own for it; the caller asks for the feature when it
    creates the device (item 39).
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
- **Storage barriers follow what the shaders write** (written 2026-10-18,
  see "State of the code"). A storage image or buffer no pipeline has
  written since its last barrier in the list, used by one that only reads
  it, gets no barrier. Left: the first use in each list always gets one,
  since the list doesn't know what the submission before it did; a texture
  could carry "written since the last barrier" from one submission to the
  next, as it carries its layout, and the submit check would compare it.
- **A list's other handles are not checked at submit.** A destroyed or
  resized texture, a destroyed buffer, material or GPU timer is refused
  (written 2026-10-18). A view, sampler, pipeline or binding set destroyed
  before submitting a list compiled against it is not: only the recorded
  commands name them, and the GPU would use what is gone. The same
  aprend_instance_forget would cover them, if each recorded which list
  names it.
- **A list leaves a sampled texture in `SHADER_READ_ONLY` and a storage image
  in `GENERAL`.** Nothing puts it back. Right for most uses; written down
  because the immediate operations do put a texture back.

### 18. Shader reflection: what is left

Written (see "State of the code"). Hazel:
`Platform/Vulkan/ShaderCompiler/VulkanShaderCompiler.h` (`Reflect`),
`Shader.h` (`ShaderBuffer`, `ShaderUniform`), `DescriptorSetManager::Validate`.
Left over:

- **One entry point per stage is assumed** to tell used from declared.
  SPIR-V with several entry points for a stage counts everything as used,
  so a pipeline over it needs layouts for all of them.
- **Reflection is redone for every shader created**, with no cache. Fine
  for a handful of shaders; a cache belongs with item 22.
- **Specialization constants**: a workgroup size or array size set by one
  reads as 0, and a descriptor array sized by one is refused.
- **Formats aren't checked**: a storage image's declared format against
  its view's, a vertex element's component count against the input's, a
  color output's type against the target's format. The fragment shader's
  color output count is reflected and nothing compares it yet.
- **The bindings of one set must agree across stages** in type; different
  array sizes take the largest, and different block declarations the
  largest block's members, without saying so.
- **SPIRV-Cross on D3D12 builds is SpudLib's git tag**, a different
  revision from the vendored one; the core API used here is old and
  stable, but the two aren't pinned together.

### 19. Material: what is left

`aprend_material` is written (see "State of the code"). Hazel: `Material.h`.
Left over:

- **Names come from the layout**, which for owned blocks has to be made
  from shaders (item 18). A hand-written layout gives names to textures
  and buffers the caller sets, and no owned block.
- **A changed resource rebuilds the whole binding set**, at the next
  compile (item 9's first point). Fine for a texture swapped now and then;
  a material whose textures change every frame wants item 33's bindless.
- **A list compiled before a resource changed and submitted after it**
  binds what the material held at its compile, as with any updated set;
  nothing checks.
- **One material is one set slot.** A shader that reads per-material data
  from two sets needs two materials.
- **No owned block in an array binding**, and a buffer the caller gives
  is always bound whole: no `_offset` or `_range` through the material.
- **No push constants in a material.** Hazel's materials can be push
  constant blocks; here a material is a binding set.
- **A copy is a copy, not an instance of a parent** (2026-10-18). Unreal's
  material instances read every parameter they don't override from their
  parent, live; `aprend_material_copy` takes the values once. A caller
  that wants a parent's later changes to reach its children propagates
  them itself: inheritance needs change tracking and ties the parent's
  life to its children's, an asset system's concern, not a renderer's.
  Nor is there a call that copies values into a material that exists
  (Unreal's `CopyParameterOverrides`); nothing asks for one.
- **The arena keeps one empty buffer, and no more** (2026-10-11;
  run in the stub harness only). When a material's range comes back
  and leaves its buffer empty, the buffer goes if another is empty
  already, and is kept as the spare if not. This happens in the
  material's release function, after the GPU has finished with it, so the
  buffer is destroyed there and then. Left: a new block goes to the first
  buffer with room, not the fullest, so a buffer with one long-lived
  material in it may never empty; nothing releases the spare
  (`aprend_vertex_pool_trim` has no counterpart for the arena); nothing
  compacts, by design, since moving a block means a copy and a rebuilt
  descriptor set. Item 35 may replace the range list underneath.
- **An owned block takes at least the device's uniform buffer offset
  alignment** (`SPUDGPU_ADAPTER_PROPERTIES::uniform_buffer_offset_alignment`,
  16 at the least) since 2026-10-18: 256 on D3D12 and Metal, often 64 on
  Vulkan. Before, 256 everywhere, since SpudGPU didn't report it.
- **A changed block is copied whole**, not the changed bytes.
- **A material of a frequently changing block** (one written every frame)
  pays a copy and a barrier on its arena buffer each submission; a block
  that changes every frame is a buffer set's job, as an external block.

### 20. Render and compute passes: what is left

`aprend_render_pass` and `aprend_compute_pass` are written, and a render pass
renders to an `aprend_framebuffer` and checks it against the pipeline (see
"State of the code"). Hazel: `RenderPass.h`, `ComputePass.h`,
`Renderer::BeginRenderPass`. Left over:

- **One color target**, until a pipeline can describe more (item 25).
  `APREND_RENDER_PASS_MAX_COLOR_TARGETS` is 1 for that reason.
- **The render area is always the whole target**, and the viewport and
  scissor cover it. A caller wanting less sends its own SET_VIEWPORTS
  after the begin, or BEGIN_RENDERING by hand.
- **Ops are fixed per pass.** A pass that clears on its first use in a
  frame and loads on its second is two passes.
- **Inputs are by slot, not by name** (Hazel's `SetInput("u_...")`).
  Reflection (item 18) is written and a layout made from shaders has its
  bindings' names; a pass doesn't use them. A material does, so a pass's
  named inputs today are a material at one of its slots.
- **A depth-only pass doesn't render on D3D12.** Aprend accepts a pass
  with no color target, and SpudGPU's D3D12 `spudgpu_cmd_begin_rendering`
  returns early without one (`../spudlib/CLAUDE.md`, "Known gaps").
  Shadow maps (item 26) need it. SpudLib's to fix; Aprend says nothing
  when it happens.
- **A pass over a framebuffer with a borrowed texture reports the wrong
  size after a resize.** `aprend_framebuffer_resize` doesn't resize an
  `existing_texture`, and `aprend_render_pass_get_size` returns the
  framebuffer's size, not the attachment's.
- **A pipeline set inside the pass** with SET_SHADER_PIPELINE is not
  checked against the targets' formats, as BEGIN_RENDERING never was.
- **The swap chain is not a target.** Presenting is still
  `PRESENT_TEXTURE`'s copy from a texture the pass rendered to.
- **A compute pass only sets state**: there is no begin and end, and
  nothing groups its dispatches.

### 21. Draw list: what is left

`aprend_draw_list` is written (see "State of the code"). Hazel:
`SceneRenderer` (`MeshKey`, `DrawCommand`, `m_SBSInstanceTransforms`),
`Renderer::RenderMesh`. Left over:

- **Instance bytes are a per-instance vertex binding, not a storage
  buffer** as this item first asked. A vertex binding needs no binding
  layout of the list's own, works the same on every backend, and takes its
  memory from the frame's transient arena, so it needs no buffer set. What
  it costs: a 4x4 matrix is four VEC4 elements at four locations (item 11),
  and a shader can't index another instance's data. A storage buffer path
  would be a second mode of the list, for a shader that wants one.
- **No culling and no GPU-driven path.** The list draws what it is given;
  culling is the caller's, and an indirect path built by a compute shader
  waits on item 34.
- **Batched order sorts by handle value.** Draws of one material are
  together and each material is bound once, but which material comes first
  follows its address, which changes from run to run. Nothing sorts front
  to back for early depth rejection: that needs a depth the caller would
  hand over.
- **No pipeline per draw.** Draws that need different pipelines are in
  different lists, each replayed after its own SET_SHADER_PIPELINE.
- **A second build in one frame takes transient memory again**; the first
  span is not reused until the frame comes round.

### 22. Shaders are precompiled SPIR-V only

Half settled on 2026-10-16. Aprend ships shaders of its own: GLSL in
`shaders/`, compiled to SPIR-V by glslc when the library is built and
embedded in it (see "State of the code"), and the objects made of them kept
by the instance (`aprendbuiltin_internal.hpp`). A caller's shaders are
still SPIR-V it hands over: item 18 has Aprend reflect them, so SPIR-V is
all it needs. Written 2026-10-16 and run on all three backends by `../apricottests` (mip generation and bloom draw with them; D3D12 and Vulkan by 2026-10-17). Left:

- **No runtime compilation, includes, macros, cache or reload**, for the
  caller's shaders or Aprend's. Aprend's shaders share nothing yet; when two
  do, glslc's `-I` and `#include` (`GL_GOOGLE_include_directive`) at build
  time are enough, with no compiler at run time. Hazel:
  `VulkanShaderCompiler`, `ShaderPreprocessor`, `VulkanShaderCache`,
  `ShaderPack`.
- **Variants.** One pipeline per target format is made on demand. A
  shader that needs a variant of itself (an integer format, a different
  sample count) is a second shader, or a specialization constant SpudGPU
  doesn't expose.
- **Built-ins are made on first use**, so the first call that needs one
  pays for a pipeline. A call to make them ahead of time is for when that
  shows.

### 23. No 2D, line, text or debug drawing

Hazel: `Renderer2D` (batched quads, lines, circles, MSDF text), `DebugRenderer`.
Needs items 9 and 12.

The memory for it is there: transient spans (`aprendtransient.h`, see "State
of the code") are what a batch of quads or glyphs is written into each
frame. Not there: the batcher, the shaders, a font atlas, and uniform data
from transient memory (a binding set reads a buffer, not a span).

#### Text: the design (decided 2026-10-15, not started)

Text is split in two. Everything about turning a string into glyphs is CPU
work in a new module, ApText (`APTEXT_TODO.md`); Aprend only draws what
ApText hands it. Aprend never sees a string, a font or a codepoint.

**The method: MTSDF.** A multi-channel signed distance field (MSDF) in RGB
with a true single-channel SDF in alpha, generated with msdfgen. One atlas
draws a glyph at any size, scale, rotation and under perspective, so text
placed in a 3D scene and zoomed stays sharp; the alpha channel's true
distance is what an outline, halo or soft shadow is computed from (MSDF's
RGB is only exact near the edge). The cost is softer small text than a
hinted bitmap at its exact size. Small fixed-size text is the case bitmap
atlases win, and is not what this is for; a bitmap mode can be added later
as a second atlas kind if a caller needs it, as Unreal has both.

**What Aprend holds:**

- **The atlas is an ordinary 2D texture** the caller creates and updates:
  RGBA8 unorm, linear filtering, clamp, one mip. ApText reports which
  rectangles changed since the last upload, and the caller updates those
  with `aprend_texture2d_update`. Nothing in Aprend knows it is a font atlas.
  It grows by layers (a 2D array) or by a bigger texture and a re-pack;
  which is ApText's choice, and Aprend needs nothing new for either.
- **The glyph batch** is part of the 2D batcher above: per glyph one
  instance (position or 3D transform row, size, atlas rectangle, layer,
  color, outline color and width), written into a transient span, drawn as
  instanced quads. Screen-space and world-space glyphs are one batch kind
  with the transform in the vertex shader; the caller chooses which per
  batch.
- **The shader:** median of RGB, `screenPxRange` from `fwidth` of the atlas
  coordinate and the atlas's distance range, coverage clamped to [0, 1];
  outline and halo from the alpha channel. The distance range (pxRange) is
  data from the atlas, passed in, never a constant in the shader: Hazel
  hard-codes 2.0 in both places and they must agree by hand. Depth test,
  depth write and blending are the pipeline's, set by the caller.
- **No mips.** MSDF doesn't sample mips, so this doesn't wait on item 31.
  At strong minification it aliases; the shader fades coverage once
  `screenPxRange` drops below 1 rather than relying on a mip chain.

**Needs:** items 9 and 12 as above, and uniform data from transient memory.
Item 22 has answered how Aprend ships a shader (embedded SPIR-V, built-ins
kept by the instance), so the glyph shader is one more in `shaders/`.

**What not to copy from Hazel** (`Renderer/UI/Font.cpp`,
`Renderer2D::DrawString`, `Renderer2D_Text.glsl`):

- Layout inside the renderer. `DrawString` decodes UTF-8, wraps lines,
  applies kerning and writes vertices in one function. Here layout is
  ApText's and the draw takes positioned glyphs.
- No shaping: per-codepoint lookup with kerning pairs only, so no
  ligatures and no complex scripts. ApText shapes with HarfBuzz.
- A fixed charset baked at load (Latin, Latin-1, Cyrillic), and a missing
  glyph silently skipped. ApText adds glyphs on demand and has fallback
  faces.
- An RGBA32F atlas, four times RGBA8's memory for no visible difference.
- The atlas cached to a disk path from inside the font class, and default
  fonts in static globals. Aprend and ApText do no file I/O and hold no
  global state.

**Reference: Unreal.** Slate shapes with HarfBuzz, breaks lines and
reorders bidirectional text with ICU, and caches glyphs on demand in a
dynamic atlas with fallback faces per Unicode range (composite fonts).
Since UE 5.5 a font face chooses Bitmap, MSDF, SDF or an approximated SDF
(`EFontRasterizationMode`, with a pixels-per-em setting for the distance
field and per-platform overrides). In-world text is `TextRenderComponent`
over a font texture, or the Text3D plugin's extruded meshes.

**Not this:** single-stroke (plotter-style) fonts are lines, not filled
glyphs, and go through the line batch; ApText gives their strokes as
polylines (`APTEXT_TODO.md`). Exact curve rendering with no atlas (Slug) is
the quality ceiling and is commercial; MTSDF is the choice until something
shows it isn't enough.

### 24. Lighting: what is left

Written 2026-10-17 and run on Metal (see "State of the code"). Against the
chain Hazel's `SceneRenderer::FlushDrawList` runs - directional and spot
shadow maps, pre-depth, tiled light culling, skybox, PBR geometry, GTAO,
SSR, bloom, composite - Aprend has every stage but screen-space
reflections: cascaded sun shadows and spot shadows, tiled culling (as a
compute pass a compile records), a sky and its background, the PBR
surface with Hazel's model, a horizon-based ambient occlusion, bloom
(item 36) and a composite with four tone curves and adapting exposure.
Against Unreal's forward path: its material inputs (base color,
metallic, specular, roughness, emissive, opacity, opacity mask), clear
coat, a cloth-like sheen, split-sum image-based light from a lookup
texture, stable cascades, eye adaptation. Unreal's deferred path, virtual
shadow maps, Lumen and Nanite are past what a forward renderer is for.

**The shape, decided 2026-10-17.** Aprend ships the shading, never the
geometry: the surface fragment shader has a documented interface and the
caller's vertex shader decides the vertex format, the transforms and the
space. That is Unreal's split - a vertex factory per kind of geometry, a
shading model per kind of surface, a material that only hands over values
- not Hazel's, whose one PBR shader is tied to its one vertex layout and
keeps its material values in push constants. Aprend's own vertex shaders
are there for a caller with no geometry of its own; trellislib keeps its
own. Set 0 for the view and set 1 for the material is the usual order of
binding frequency. There is no scene graph: what is drawn, in what order
and into which shadow map is the caller's, and Aprend gives it the
matrices (`aprend_lit_fit_sun_cascades`,
`aprend_lit_spot_shadow_view_projection`) as functions with no GPU in
them, so a caller that frames its own keeps doing so.

Left:

- [x] **Run on D3D12 and Vulkan.** `../apricottests` passed on both, on
      an Intel Iris Xe and an NVIDIA RTX A2000, on 2026-10-17: its shadow
      tests the first use there of SpudGPU's comparison samplers and of
      D3D12's depth-only passes. One fix came of it, the clear value of an
      environment never filled (see "What has run" under "State of the
      code").
- [ ] **Screen-space reflections.** A forward pass has no normal or
      roughness target to reflect from: it needs the surface to write a
      second target (a variant of the surface shader, and a pipeline of
      two targets, item 25), then a ray march over depth and a resolve.
      Hazel does it deferred-style over its own G-buffer outputs.
- [ ] **Shadows from point lights** (a cube map each, six passes).
- [ ] **Depth bounds for the tiles**: without a depth pre-pass a tile's
      frustum runs the whole depth of the view (Hazel's forward+ uses its
      pre-depth for each tile's range), so a light behind a wall is kept.
      A pass that reduces a depth buffer per tile, and culling against it.
- [ ] **Transmission** (glTF's KHR_materials_transmission): refraction of
      what is behind, needing a copy of the opaque picture.
- [ ] **A sun disc** in the sky shader (left out of the environment on
      purpose, where it would only blur), and Hosek-Wilkie's sky for a
      better horizon than Preetham's.
- [ ] **Cascade blending**: a fragment takes one cascade; a blend over the
      last stretch of each would hide the seam between them (only the
      last fades out now).
- [ ] **GTAO proper** (Jimenez et al.: cosine-weighted horizons and a
      bent normal) in place of the horizon-based estimate, and at half
      resolution with an upsample, when it is measured.
- [ ] **A histogram for exposure** (Unreal's), which can leave the
      darkest and brightest few percent out, in place of the log
      average.
- [ ] **Per-map uv transforms** (KHR_texture_transform's), one for all
      maps now.
- [ ] **Variants**: branches on the encoding, the map bits, the shadows
      and the tiles are at run time; a shader per case, or specialization
      constants (item 22), when a measurement shows them.

### 36. Post-processing, and bloom first

**Bloom written 2026-10-16, run on Metal the same day** (see "State of
the code"). What was decided:

- **No module of its own.** In Hazel bloom is a step of `SceneRenderer`
  (`BloomSettings` beside `DOFSettings`, its textures members of the scene
  renderer), and in Unreal one stage of `AddPostProcessingPasses`, its
  settings fields of `FPostProcessSettings` and its textures taken from the
  render graph's pool for the frame. In both, each effect is a building
  block that records its passes into the frame, and the order and settings
  of the effects are the scene renderer's (`SceneRenderer`,
  `AddPostProcessingPasses`). Aprend's building blocks are commands; the
  order and settings belong to whatever owns the caller's frame
  (trellislib's renderer), not to Aprend. The textures an effect needs in
  between come from a pool the instance keeps (as Unreal's), not from a
  handle the caller makes. No header of its own: the command's doc
  describes bloom, `aprendcontext.h` the pool.
- **A temporal effect will need a handle** (TAA, auto-exposure, motion
  blur): history that lasts from one frame to the next, which a pool that
  lends textures within a frame can't hold. Unreal keeps it per view
  (`FSceneViewState`), Hazel in `SceneRenderer`. Not before one is wanted.
- **A pass over a texture** needed one thing Aprend didn't hand out: a
  full-target vertex shader. `aprend_get_fullscreen_vertex_shader`
  (`aprendshader.h`) gives the instance's own; with it a caller's pass
  over a texture is the commands that exist already (a pass on the
  destination, a pipeline with no vertex input and its fragment shader, a
  binding set of the source, a draw of 3). No further command.
- **What glows is the caller's.** `APREND_COMMAND_BLOOM` blooms whatever
  source it is given onto a target; a threshold and knee of 0 make every
  texel glow, which is what a caller drawing only what should glow into a
  source of its own wants. The composite adds onto the target with an
  additive blend, so the source is what glows and the target is the
  picture: no third texture.
- **Fragment passes, not compute**, so the chain renders through the
  machinery mip generation already runs, and a source needs no storage
  usage.
- **One mipped R16G16B16A16_FLOAT texture** of `_levels` levels from the
  instance's render target pool (`aprendrendertargets_internal.hpp`), level
  0 half the source's first mip: taken at compile, free again once the
  frame it was taken in has finished (aprend_instance_next_frame's wait),
  never shared by two takes of a frame, given back by a compile that is
  not submitted, retired after 120 frames untaken. Prefilter (13 taps and the soft threshold),
  downsample (13 taps, Jimenez), upsample (a tent `_radius` texels wide,
  added into the level above), composite (the same tent, times
  `_intensity`, added onto the target). Places from `gl_FragCoord` with
  the target size pushed, so odd sizes and a source of another size read
  right.
- **Alpha**: the glow carries the source's coverage, weighted with its
  color, and is added to the target's alpha.
- **Sets live on views.** The set that reads a view (a level of a pooled
  chain, or the caller's source) is kept by the view
  (`aprend_texture_view_t::bloom_set`) and released with it. A set kept by
  the pool or the list could outlive the view, or be taken for a new view
  made at the same address; and updating a set at compile retires
  descriptors a compiled, unsubmitted list may still bind.

Left:

- [x] **Run on D3D12 and Vulkan.** `../apricottests`' `test_bloom` passed
      on both, on an Intel Iris Xe and an NVIDIA RTX A2000, on
      2026-10-17, after Metal the day before.
- [ ] **Exposure and a tone curve** belong with an HDR pipeline (item 24),
      in the composite or after it.
- [ ] **A lens dirt texture** (Hazel's) and a mask texture weighting the
      glow are not there.
- [ ] **Cost** is two passes a level and two more, every time the command
      is compiled in. Fewer levels is the caller's setting. Nothing is
      skipped for a source that has nothing in it.
- [ ] **The first bloom makes four pipelines**, the first onto a target
      format another, and the first of a size a pooled texture, inside a
      compile.
- [ ] **A view a bloom read keeps its set until it is destroyed.**
- [ ] **The pool's memory**: a pooled texture for every size and level
      count taken in the last 120 frames, and one per bloom compiled in a
      frame, times the frames in flight. Nothing reports it or lets a
      caller trim it. The idle count is a constant.
- [ ] **The pool is bloom's alone so far.** A blur or an outline that
      needs a texture in between takes one the same way
      (aprend_render_target_take).

Hazel: `SceneRenderer::BloomCompute`, `Resources/Shaders/PostProcessing/Bloom.glsl`,
`SceneComposite.glsl`.

After bloom, in the order a caller is likeliest to want them: a separable
blur on its own (it falls out of the chain), an outline from a mask, a
tone curve, then the lit pipeline's stages under item 24.

## Needs something SpudGPU doesn't expose yet

What SpudGPU exposes follows from the GPU APIs it translates and is decided
there, not by this list. These are what Aprend can't do until it does.

### 25. Multiple render targets per pipeline

Closed 2026-10-15; run in Trellis_macOS on Metal, and by `../apricottests` on Metal, and on D3D12 and Vulkan on Windows (2026-10-16) (see "State of the code"). SpudGPU's
pipeline desc has a format and a blend state for each color attachment
(`color_attachment_formats`, `blend_attachments`, `color_attachment_count`)
and Aprend's mirrors it (`color_attachment_formats`, `_blends`). A render
pass takes up to `APREND_MAX_COLOR_TARGETS` targets. Left: nothing checks
that a fragment shader writes as many outputs as the pipeline has targets
(item 18's reflection could).

### 26. Depth bias

Closed 2026-10-15; run in Trellis_macOS on Metal, and by `../apricottests` on Metal, and on D3D12 and Vulkan on Windows (2026-10-16). `_depth_bias_constant`,
`_depth_bias_slope` and `_depth_bias_clamp` on the pipeline desc, through
SpudGPU on all three backends; a clamp needs a device created with
`SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP` (item 39).

### 27. MSAA and resolve

No sample count on images or pipelines and no resolve attachment.
`aprend_texture2d_create` refuses `sample_count > 1` and `BEGIN_RENDERING`
refuses a `resolve_view` for this reason. Hazel doesn't use MSAA either, so this
is not part of the bar.

### 28. Stencil state

Stencil load and store ops exist on the depth target, but there is no stencil
test or op state on the pipeline. Hazel leaves stencil off as well.

### 29. GPU timing and debug markers

Closed 2026-10-15; run in Trellis_macOS on Metal, and by `../apricottests` on Metal, and on D3D12 and Vulkan on Windows (2026-10-16) (see "State of the code"). Debug labels are
`APREND_COMMAND_BEGIN_LABEL` and `END_LABEL`; GPU time is an
`aprend_gpu_timer` (`aprendtimer.h`) whose slots a pass is timed into, read
`frames_in_flight` frames later. Left:

- **Only whole passes are timed.** SpudGPU writes timestamps at a pass's
  start and end, the one place Apple GPUs sample them, so compute
  dispatches and time between passes are in no slot. Timing compute needs
  a compute pass in SpudGPU (Metal's compute pass descriptor has the same
  sample buffer attachments), which it doesn't have.
- **No pipeline statistics.** Vulkan and D3D12 have them and Metal's
  counters differ; nothing asks.
- **The Metal conversion is checked for sense only.** GPU ticks are set
  against `sampleTimestamps:` taken at the set's creation and at each read.
  On an M5 Pro it gives 0.08-0.11 ms for `aprend_gpu_tests`' pass, inside
  the test's bounds; it hasn't been held against a known cost, nor
  checked on another Apple GPU or over a long span.
- Hazel: `RenderCommandBuffer` (`RT_BeginTimerQuery`, `RT_BeginMarker`).

### 30. Hardware without dynamic rendering

`CLAUDE.md`, "Aprend": SpudGPU's plan is a caller-owned `spudgpu_framebuffer`,
designed and not built. When it exists, `aprend_framebuffer` holds and caches
it.

### 31. Mip generation

Numbered out of order so the other items keep their numbers. The immediate
form is written (2026-10-16; `test_generate_mips` passed on Metal, D3D12 and Vulkan):
`aprend_texture2d_generate_mips` renders each level below a base level of
every layer, a pass per level sampling the one above through Aprend's own
downsample shader (item 22). SpudGPU's blit resamples only on Vulkan, so a
blit-based form would have done nothing on Metal and D3D12. The command
form, `APREND_COMMAND_GENERATE_MIPS`, is written (2026-10-16;
`test_generate_mips_command` passed on Metal, D3D12 and Vulkan): the same passes recorded into a command list outside any pass,
through the list's layout tracking, for a texture rendered earlier in the
same list; every level it fills, and the base, ends in
`SHADER_READ_ONLY` for a later pass. The views and sets both forms render
through are kept by the texture (`aprend_mip_chain`), one chain per base
level and filter, made by the first generation and released with the
texture, or retired with its image by a resize; a list compiled every
frame makes nothing new. Left:
- **3D textures**: their depth halves too, which a 2D pass can't do; a
  compute shader or a pass per slice of the target pair.
- **Formats**: eleven every backend renders to and filters. 32-bit float
  and snorm formats depend on the device, and SpudGPU has no query for
  what a format supports; integer formats can't be filtered, and would
  need a shader that reads texels and averages them itself.
- **Odd sizes** are filtered with two taps a side, so a level's last texel
  of an odd side weighs less. A three-tap filter for odd sides is the
  usual fix, in the shader.
- **The first generation in a format makes a pipeline**, and a texture's
  first of a base level and filter its views and sets, inside whatever
  call or compile it is.
- **A chain is kept for the texture's life**: a texture generated from
  several base levels or with both filters keeps one for each. Nothing
  drops one sooner.

Hazel: `Texture2D::GenerateMips`.

### 33. Bundles and bindless: not on every backend

Numbered out of order, as item 31. SpudGPU has both on Vulkan and D3D12 and
compiles both out on Metal (`SPUDGPU_EXT_BUNDLES`,
`SPUDGPU_EXT_BINDLESS`), so Aprend can't offer either as
the same thing everywhere, and offers neither.

- **Bundles: not planned.** A bundle is a second kind of command list,
  recorded ahead and replayed inside a pass. Metal has no such thing, so an
  Aprend API for it would be compiled out on one backend and a caller would
  have two ways to draw the same thing. What a bundle is for, recording
  draws once and replaying them, an Aprend command list already does: it is
  compiled once and submitted as often as its textures and buffers allow.
  `APREND_RENDERING_FLAG_EXECUTES_BUNDLES` and the `_flags` of
  `BEGIN_RENDERING` were removed on 2026-10-10: the
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

### 35. One device allocation for every buffer and image

Numbered out of order, as item 31. Every `spudgpu_create_buffer` and
`spudgpu_create_image` is a device allocation of its own: a
`vkAllocateMemory` on Vulkan (where many drivers allow about 4096 in all,
`maxMemoryAllocationCount`), a committed resource on D3D12 (64 KiB at the
least, whatever the buffer's size), a `newBufferWithLength` on Metal. Aprend
works around it twice, with the vertex and index pools (item 13) and the
material arena (item 19), each suballocating ranges of a few large buffers;
every other buffer and every texture is still an allocation each.

Decided 2026-10-11: **SpudGPU exposes heaps and resources placed in them,
and Aprend decides placement.** That is translation, and keeps the
decision with SpudLib's caller as `../CLAUDE.md` asks: a heap of a memory
kind, and a buffer or image created at an offset of one, which are
`vkAllocateMemory` and `vkBind*Memory` on Vulkan, `ID3D12Heap` and
`CreatePlacedResource` on D3D12, and `MTLHeap` on Metal, with each
resource's size and alignment requirement reported so the caller can place
it. The alternative, Vulkan Memory Allocator and D3D12 Memory Allocator
inside SpudGPU's backends, would be SpudLib deciding where memory goes, and
Metal would need one of its own.

- **SpudLib first**: done 2026-10-11, written and not run on a GPU.
  `spudgpu_create_heap` (one memory kind, one resource class: buffers,
  images, attachment images), `spudgpu_get_buffer/image_placement_requirements`
  (asked of the heap), `spudgpu_create_placed_buffer/image` (ordinary
  handles, destroyed with the ordinary calls). Vulkan and D3D12
  type-checked on Linux, the Vulkan side run against a mock; Metal parsed
  against a stub only. See `../spudlib/CLAUDE.md` for what was settled.
- **Then Aprend**: a device allocator over the heaps for every buffer and
  texture, with the placement policy (block sizes, which block a resource
  goes in, when an empty block is released). Its bookkeeping could be VMA's
  virtual allocator (`VmaVirtualBlock`, TLSF, which touches no Vulkan
  object and serves every backend) in place of `aprend_range_list`, under
  the pools and the arena as well.
- The pools and the arena stay: they place many ranges in one buffer,
  which is what one view, one barrier per buffer and a material table
  (item 21, item 33) need, and which a device allocator doesn't do.

### 39. A device has only the features its caller asked for

Numbered out of order, as item 31: written as item 37 on 2026-10-21, a
number already taken by the command allocator. Closed 2026-10-21, on
SpudGPU's adapter and device split (`../spudlib/SPUDGPU_DEVICE_TODO.md`).
Builds on macOS over Metal with no warnings and the repo's headless tests
pass. Run on Metal (Apple M5 Pro) on 2026-10-21 by `../apricottests`,
converted the same day, which runs every test twice: on a device with
these optional features and the tests' (0x7f of the adapter's 0xeff), and
on one with none, the first run of the staged storage buffers and of an
indirect draw split a call a draw on Metal; its `test_feature_checks` saw
each group 2 call made with its feature and refused without. All passed,
Debug under Metal validation and `leaks`, and Release. Built and run on
Windows 11 on 2026-10-22 (Debug, D3D12 and Vulkan, Intel Iris Xe and NVIDIA
RTX A2000): every check passed on both devices of every run, the first
devices 0x7f of 0x1dff (Iris Xe) and 0x3f of 0x1fbf (RTX A2000), with no
debug layer or validation layer message, none under Vulkan's
synchronization validation either. Not built on Linux.

A `spudgpu_device` has exactly the `SPUDGPU_FEATURE_*` bits it was created
with, whatever its adapter supports. The device stays the caller's: Aprend
is handed it and never creates it (the caller keeps the queue and feature
choices for its own needs, and two instances may share one device,
`aprendcontext.h`). So Aprend says what it can use, in two groups:

| Feature | Use | Group |
|---|---|---|
| `MULTI_DRAW_INDIRECT` | an indirect draw command is one call, not split to the adapter's `max_draw_indirect_count` | 1 |
| `DEVICE_MAPPABLE_MEMORY` | storage buffers written directly, else staged | 1 |
| `PRESENT` | `aprend_swap_chain_create` | 2 |
| `PASS_TIMESTAMPS` | `aprend_gpu_timer_create` | 2 |
| `MESH_SHADING` | a pipeline with `mesh_shader` | 2 |
| `LAYERED_RENDERING` | 2D array views rendered to | 2 |
| `CUBE_ARRAY_VIEWS` | `APREND_TEXTURE_VIEW_SHAPE_CUBE_ARRAY` | 2 |
| `DEPTH_BIAS_CLAMP`, `INDEPENDENT_BLEND` | a pipeline's `_depth_bias_clamp`, differing `_blends` | 2 |

What was done:

- **Group 1, the features only Aprend's own code uses:**
  `aprend_get_optional_gpu_features()` (`aprendcontext.h`) returns them, a
  function so the value is the linked library's. The caller asks for
  `aprend_get_optional_gpu_features() & supported_features` when it
  creates the device; without them Aprend takes the other path.
  `aprendfeatures_tests` (C, headless) checks the set.
- **Group 2, features one of Aprend's calls needs:** each call names its
  feature in its header comment and checks the device itself, refusing in
  Aprend's way (NULL after printing why) before SpudGPU sees the call:
  the swap chain, the GPU timer, a mesh pipeline, a depth bias clamp,
  differing blends (by SpudGPU's rule: entries that don't blend are equal),
  a 2D array view rendered to, a cube array view. The caller asks for each
  of the calls it makes.
- **Aprend reads the device, not the adapter, for features.**
  `aprend_instance_create` keeps `spudgpu_get_device_features(device)` in
  the instance (`aprend_instance_t::features`), and every check reads
  that. Properties and limits come from
  `spudgpu_get_adapter_properties(spudgpu_get_device_adapter(device))`:
  `unified_memory`, the two buffer offset alignments, and
  `max_draw_indirect_count`, which counts only on a device with
  `MULTI_DRAW_INDIRECT` and is 1 without. `layered_rendering`, the
  instance's copy of the old property, is gone.
- **Confirmed: no internal pass needs a group 2 feature.** Aprend's own
  views (mip generation, the lit placeholders, bloom and post) render to
  2D views and read 2D, 2D array and cube views: no layered pass and no
  cube array view, so group 1 stays the two features. No Aprend shader
  writes `gl_Layer` or uses `nonuniformEXT` (the lit shaders' unsized
  `tile_lights[]` is a storage buffer's last member, core SPIR-V), so none
  needs a feature switched on behind one of these bits either.
- The instance's command allocators are direct, so the device needs a
  direct queue; `aprendcontext.h` says so, and SpudGPU refuses the
  allocators otherwise.

Left: `../apricottests` on D3D12 and Vulkan, where the bare device is
also the first run of each fallback off Metal (and on D3D12 of a device
without device-mappable memory); mesh shading and the swap chain's
refusals, which no test covers.

## Suggested order

Each step is usable on its own, and later ones lean on earlier ones. Items 4
and 5 are closed and their numbers are not reused.

1. Run what is written. It builds on all three backends as of 2026-10-12
   (see "State of the code"); left of this step:
   - Run more of it: one application ran on Metal and on D3D12 on
     2026-10-11 and on Vulkan (Linux) by 2026-10-12, covering only what it
     uses (see "State of the code"). No push constant set, staging,
     compute, indirect draw, partial view or layered pass has run on any
     of the three. `../apricottests` (`aprend_gpu_tests`)
     is written for this: push constants, a buffer update (staged on a
     device with its own memory), sampled views of one mip, rendering to
     one mip and sampling it in a later pass, each checked by reading the
     pixels back. Ran on Metal on 2026-10-11 (Apple M5 Pro, unified memory,
     so not the staged path) and passed, after two fixes: a SpudGPU Metal
     descriptor set kept a rewritten descriptor's old resource and crashed
     on it once destroyed; and a compiled list could not in fact be
     submitted twice (a SpudGPU command list is submitted once per
     recording), so `aprend_command_list_submit()` now takes one compile
     once and the header says so. On 2026-10-16 it ran on Windows, D3D12
     and Vulkan, on an RTX A2000 (memory of its own, so the staged path
     ran for the first time) and an Iris Xe, and passed after fixes in
     Aprend and SpudGPU (`../apricottests/README.md` lists them). The
     same day every test, the newer ones too, passed on Metal under its
     validation layers, after five fixes in SpudGPU's Metal backend. Not
     run on Linux.
   - Vulkan: builds and runs on Linux as of 2026-10-12 (see "State of the
     code"); the Wayland driver issue no longer holds that machine. Write
     its side of each change alongside Metal and D3D12 and build it there.
     Built and run on Windows by `../apricottests` (2026-10-16).
   - Run it on a device with its own memory as well, for the staged path:
     done on D3D12 and Vulkan (RTX A2000), 2026-10-16.
2. Freeing staging copies (item 13): written 2026-10-18, with exact
   staged ranges and `APREND_BUFFER_WRITES`, and run on Metal only, where
   nothing is staged. `test_buffer_writes` then passed on D3D12 and Vulkan
   on the RTX A2000 (memory of its own) and the Iris Xe, 2026-10-18, with
   every other test, Vulkan with the validation layer, both clean (at
   1f75d84 plus the fix below, spudlib b7ac7e6). That run found and fixed
   one thing here: an upload to a texture with no layout yet always left
   it in SHADER_READ_ONLY, which Vulkan forbids for an image without
   sampled usage (VUID-VkImageMemoryBarrier-oldLayout-01211, a
   storage-only texture). It now rests in a layout its usage allows
   (`aprend_texture_upload_rest_layout`): SHADER_READ_ONLY if sampled,
   GENERAL if storage only, else TRANSFER_DST. A crash on Vulkan in the
   same runs was the test's own: `test_stale_lists` resized a texture with
   a view of it still alive, against aprend_texture2d_resize()'s contract.
   A Debug build now refuses that resize (2D and 3D): each texture counts
   its views not destroyed (`live_views`, Debug only), and a 2D resize
   leaves out its own mip chain's views, which it releases itself.
   `test_resize_with_views` (2D) and `test_resize_3d_with_views` (3D, the
   first test of a 3D texture and a 3D view) cover it; both passed on
   D3D12 and Vulkan, both GPUs, 2026-10-18. The same evening the fixes of
   that run (and spudlib's Vulkan barrier and D3D12 message changes) were
   built on macOS and every test passed on Metal (Apple M5 Pro), Debug
   under Metal API and shader validation with warnings fatal, both resize
   tests refusing as they should, and Release under `leaks` (none);
   latran's and trellislib's tests pass over them.
   The shared upload path for
   textures is written (item 6), and staged buffers don't use it: a
   transient span is taken back at the next frame, and a buffer may be
   created a frame or more before a list that uses it is submitted.
   Rewriting a binding set in place (item 9) is written.
3. The rest of item 32: written 2026-10-18 and run on Metal (see "State
   of the code"), except reading a depth texture while it is the pass's
   depth target, which needs a read-only depth layout SpudGPU doesn't
   have and a test of what Metal allows; nothing asks for it yet.
   `test_storage_ordering` then ran on 2026-10-18 under Vulkan's
   synchronization validation (`VK_LAYER_VALIDATE_SYNC=1`) and under D3D12's
   debug layer (under cdb: SpudGPU registered no message callback then,
   and since the same day prints the layer's corruptions, errors and
   warnings to stdout itself in Debug builds), on the Iris Xe and the RTX
   A2000: no hazard on any dispatch or storage image, so the barrier left
   out between two reads is not needed. Sync validation did report 15
   hazards per device elsewhere, all from SpudGPU's Vulkan barriers
   inferring access and stages from a layout (`layout_to_access`,
   `layout_to_stage`, spudgpuvulkanrenderpass.c), not from Aprend:
   COLOR_ATTACHMENT_OPTIMAL gives write access only, so a pass that loads
   its target reads it unordered after the transition (6, READ_AFTER_WRITE
   at vkCmdBeginRendering); DEPTH_STENCIL_ATTACHMENT_OPTIMAL gives write
   only and the early fragment tests only, while depth is written at the
   late tests, so the transition out after a depth pass is unordered (9,
   WRITE_AFTER_WRITE, the lit tests' depth targets). Fixed in spudlib the
   same day (attachments read and written, depth at both test stages, and
   the same access in spudgpu_cmd_pipeline_barrier's resource states);
   rerun, every test passed under sync validation on both devices with no
   message.
4. Item 18, which settles item 1 and unblocks 19. Written, and run on
   all three backends through the materials and passes made from it
   (`../apricottests`, 2026-10-17). What is left is item 18's own list.
5. Items 19, 20, 21: material, pass and draw list. Written, compiled, and
   run on Metal, D3D12 and Vulkan by `../apricottests` (2026-10-17, see
   "State of the code"). What is left of each item is in its item, and
   the first of it is "Next, from the review" below.
6. Items 25, 26 and 29 are written and have run on D3D12, Vulkan and
   Metal (`../apricottests`, 2026-10-16). Item 22 (Aprend ships shaders)
   and item 31 (mip generation, both forms) are written and have run on
   all three. Bloom (item 36) is written and `test_bloom` has passed on
   all three (2026-10-17). Then 23 and 24, each a shader Aprend
   ships, as item 22 allows (24 is written and has run on Metal,
   2026-10-17; what is left of it is in the item); and trellislib's half of the selective glow
   (`../trellislib/TODO.md`).

Item 39 (a device has only its features) is written and has run on
Metal through `../apricottests`; D3D12 and Vulkan are left.

Item 37 (the command allocator) is written and has run on all three
backends. The 20-minute D3D12 check (2026-10-16) was of an idle host that
never turned Aprend's frame; trellislib turns it since 2026-10-17, and
memory over minutes of rendering is still to be watched. Item 38's points are small
and independent; fix each when its function is next touched (the division
by zero is fixed).

Item 2 is closed and its D3D12 heap has never run: it belongs to step 1's
"run it on a device with its own memory", on an integrated GPU under
D3D12.

Items 7, 22, 27, 28, 30, 33 and 34 wait until something needs them, decides
them or SpudGPU gains them. Item 35's SpudLib half is written (heaps and
placed resources, 2026-10-11, not run on a GPU); its Aprend half, the
device allocator, is not started and waits on those heaps having run.

## Next, from the review of items 18 and 19

From the review of the reflection and the material against Hazel on
2026-10-11. Most important first. Remove a step here when it is fixed,
and record what was done under "State of the code" and in the item it
belongs to.

1. **Reflection limits**: the list is item 18's "what is left", and is
   kept there only.
2. **The arena's remaining policy** (item 19): place a new block in the
   fullest buffer that has room, so lightly used buffers drain, and a trim
   that releases the spare buffer. Held until item 35 decides what the
   arena's bookkeeping is.
