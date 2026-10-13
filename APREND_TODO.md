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
  no new warnings. They are linked into the library now, and nothing of
  them has run. Doxygen has
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
  - Never run. No material has been bound, no pass begun and no draw list
    drawn on any backend. The first things to check: the per-instance
    binding read from `_start_instance_location` past 0 on Metal and D3D12,
    and a material's uniform values reaching the frame that set them.
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
  `../apricottests` (`aprend_gpu_tests`), run on Metal only.
- **Reviewed by reading on 2026-10-13; nothing compiled or run for it.**
  Every file of `src/render` except `aprendpools.cpp`, `aprendreflect.cpp`
  and most of `aprendpipeline.cpp` was read against this file, and this
  file was brought into line with the code. What the review found wrong
  is items 37 and 38 and the additions to items 8, 12, 13 and 20; what it
  found already fixed was taken out of items 8, 12, 14 and 20. No code was
  changed.

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
argument checks (`if (!instance || !desc)`, 14 of them in `src/render` as of
2026-10-13), failure reported by `printf` with no result code, and
`std::vector` growth inside C entry points with nothing catching
`std::bad_alloc`. The two `malloc`s this item named as unchecked (the layout
copy in `aprend_vertex_buffer_create`, the file buffer in
`aprend_shader_read_from_file_spirv`) are checked now.

Where the vector growth matters most: `aprend_send_command` (every array
it copies), `aprend_framebuffer_create` and `_resize`, and
`aprend_command_list_compile`, which guards its loop over materials and
nothing after it. A `std::bad_alloc` from the rest of the compile leaves
the function with the SpudGPU list still open.

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
- **`aprend_uniform_buffer_create` prints a bare result string** on
  failure, with no prefix and no newline, and prints the string for
  success when it was `malloc` or the size check that failed. Its
  `layout->uniforms` is not checked for NULL.
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

Numbered out of order, as item 31. Found by reading on 2026-10-13; not
measured on a device.

Every command list of an instance, and each list's upload list, records
into the instance's one `spudgpu_command_allocator`, and nothing in
`src/render` calls `spudgpu_reset_command_allocator`. What that costs
depends on the API underneath, which is why it has to be Aprend that
resets:

- On D3D12, opening a list (`spudgpu_begin_command_list`) resets the list
  and not its allocator. Each compile takes more of the allocator's memory
  and none comes back until the instance is destroyed: an application that
  compiles every frame grows for as long as it runs.
- On Vulkan the same call resets the command buffer, whose pool was made
  to allow that, so memory is reused.
- On Metal a command buffer is taken fresh each time and there is nothing
  to reset.

An allocator may be reset only when nothing recorded from it is still
running, and the instance already knows when that is: the frame fence.
What is wanted is an allocator for each frame index in place of the one,
reset in `aprend_instance_next_frame` after its wait, with a list opened
on the current frame's. That needs a list to be able to change allocator
between recordings, which `spudgpu_create_command_list` fixes at creation,
so it is a change on both sides: SpudGPU says which allocator a recording
uses, Aprend chooses. A list compiled in one frame and submitted in a
later one then has to be refused, as lists that read buffer sets and
transient spans already are. The immediate operations make and retire an
allocator each and are not affected (item 6).

Item 12's "a SpudGPU command allocator for each command list", wanted for
recording on several threads, is a different split of the same object;
decide the two together.

### 38. Unchecked inputs and stale lists, from the review of 2026-10-13

Numbered out of order, as item 31. Each was found by reading and none has
been reproduced.

- **`aprend_texture2d_update` and `_get_data` (and the 3D pair) divide by
  the format's bytes per pixel**, taken as `spudgpu_format_bit_count / 8`.
  That is 0 for a format of under 8 bits a pixel (the block-compressed
  ones report their average) and for one with no known pixel size, and
  the call then divides by zero where it should refuse. A compressed
  texture has no upload path at all: rows of texels are not what it is
  made of.
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
- **No unsigned element type**: a `uint` vertex input is refused at
  pipeline creation, since no `APREND_BUFFER_ELEMENT_TYPE` feeds it.

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
- **No copy.** Hazel's `Material::Copy` has no counterpart; a second
  material of the same desc is made from scratch and its values set again.
- **A material destroyed while a list that binds it is still compiled**
  is a use after free at that list's next submit, since the submit copies
  the material's blocks. The header says so; nothing checks.
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
- **Every owned block takes at least 256 bytes**, the largest alignment of
  the three APIs, since SpudGPU doesn't report the device's own. A
  device-reported alignment (Vulkan's is often 64) would pack small blocks
  four times tighter.
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

No runtime compilation, includes, macros, cache or reload; the build compiles two
GLSL files with glslc on Windows and Linux and nothing on Apple. Whether Aprend
should compile shaders at all, or only take SPIR-V and reflection data from its
caller, is undecided. Hazel: `VulkanShaderCompiler`, `ShaderPreprocessor`,
`VulkanShaderCache`, `ShaderPack`. Item 18 settled half of it: Aprend reflects
the SPIR-V it is given itself, so a caller hands over SPIR-V and nothing more.

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
Post-processing, with bloom first, is split out as item 36.

### 36. Post-processing, and bloom first

**A big one. Added 2026-10-12; nothing of it is written.** Aprend has no
post-processing: nothing takes a rendered texture and makes another from
it. A caller that wants a glow, a blur, a tone curve or an outline round
what it drew has no way to get one short of writing the passes and shaders
itself, and those are the same for every caller. Bloom is wanted first.

Hazel: `SceneRenderer::BloomCompute` and
`Resources/Shaders/PostProcessing/Bloom.glsl`, then `SceneComposite.glsl`.
One compute shader in four modes over the mips of two or three float
textures: prefilter (a threshold with a soft knee), downsample through the
mips, then upsample back up adding each level in. `BloomSettings` is
threshold, knee, upsample scale, intensity and a lens dirt texture's
intensity. The composite pass adds the result to the scene with exposure
and tone mapping. The same folder has the rest of what "post-processing"
means there: depth of field, edge detection, GTAO, SSR.

Two halves, the first of which the second and every later effect stand on:

- **A way to run a pass over a texture.** A source view, a destination
  view, a shader and its parameters: one call to record it into a command
  list, with a full-target triangle drawn (or a compute dispatch) and the
  layouts handled. Chains of these are what every post-process is. Nothing
  about it is bloom's.
- **Bloom on top of it.** A handle created for a size, holding its mip
  chain and pipelines, and a call that records "bloom this source into
  that destination" with its settings in a plain desc.

What bloom needs deciding or building:

- [ ] **What glows is the caller's.** Hazel blooms what is brighter than a
      threshold in an HDR scene. That is one policy. The source is
      whatever texture the caller gives: a caller that wants only some
      things to glow draws those into a texture of its own and blooms
      that, with the threshold at nothing. Aprend never decides what in a
      picture is worth glowing. The threshold and knee are settings, and
      0 means "all of the source".
- [ ] **Fragment shaders or compute.** Hazel's is compute. Compute here is
      written and compiled and has never been dispatched on any backend
      (item 14), so a bloom built on it debugs both at once. A chain of
      fragment passes (downsample, blur, upsample, add) runs on what has
      already drawn. Decide which first; the other can follow.
- [ ] **The mip chain.** One texture with a view per mip, each a render
      target in its turn and then sampled by the next pass. That leans on
      views of a range (item 16) and on layout tracking across a chain
      that reads one mip while writing another (items 17 and 32).
      Separate textures per level avoid both, at more memory.
- [ ] **Formats.** A float target (RGBA16F) for the chain, so the adds
      don't clip or band. Check it is renderable and filterable on all
      three backends before designing around it, and what an 8-bit source
      and destination do to the result.
- [ ] **Alpha.** A source and destination may carry premultiplied alpha
      for a caller that composites its picture over something else. A
      glow added to color alone then has no coverage where the picture
      was clear, and shows wrong or not at all once composited. Say what
      the composite writes to alpha - most likely the glow's own
      coverage, added - and test it over a clear destination.
- [ ] **The composite.** Add, with an intensity. Whether exposure and a
      tone curve belong in the same pass or a later one is a question for
      when there is an HDR pipeline to feed it (item 24).
- [ ] **Sizes changing.** The chain is rebuilt when the source's size
      does; say who notices - the caller, with a resize call, like a
      framebuffer.
- [ ] **Cost.** Nothing when not recorded. With it, a handful of passes
      at falling resolution every frame: how many levels is a setting.
- [ ] **Shaders.** SPIR-V, precompiled like every shader here (item 22),
      and reflected (item 18). They ship inside the library, which
      nothing does yet (item 7's default shaders are unused): decide how
      a built-in shader reaches a caller's binary.
- [ ] **A header and its contract** in the Doxygen style of
      `include/render/`, and the threads it is safe from.
- [ ] **Run on all three backends**, with a test in `../apricottests`
      that blooms a known source and reads the destination back.

After bloom, in the order a caller is likeliest to want them: a separable
blur on its own (it falls out of the chain), an outline from a mask, a
tone curve, then the lit pipeline's stages under item 24.

Leans on: render passes (item 20), binding sets (item 9), push constants
(item 10), views of a range (item 16) if one mipped texture is used.

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
     once and the header says so. Not built on D3D12 or Vulkan.
   - Vulkan: builds and runs on Linux as of 2026-10-12 (see "State of the
     code"); the Wayland driver issue no longer holds that machine. Write
     its side of each change alongside Metal and D3D12 and build it there.
     Not built on Windows.
   - Run it on a device with its own memory as well, for the staged path.
2. What the release queue made possible and isn't built: freeing staging
   copies and a shared upload path (items 13 and 6). Rewriting a binding
   set in place (item 9) is written.
3. The rest of item 32: what the command list still gets wrong about
   texture layouts.
4. Item 18, which settles item 1 and unblocks 19. Written and compiled;
   its reflection and the logic over it have run without a device only
   (see "State of the code").
5. Items 19, 20, 21: material, pass and draw list. Written and compiled,
   never run on a device (see "State of the code"); what is left of each
   is in its item.
6. Items 25, 26, 29 and 31 as SpudGPU gains them; then 23 and 24, and 36
   (post-processing, bloom first) once render passes have run.

Item 37 (the command allocator) is not in this order because it is not
new work to schedule: it is a leak on D3D12 today, and goes ahead of
steps 2 to 6. Item 38's points are small and independent; fix each when
its function is next touched, the division by zero first.

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

1. **`aprend_material_set_uniform` looks a name up by string on every
   call**, across every member of every owned block. A lookup once (a
   name to a handle) and a set by handle would make per-frame updates
   cheap.
2. **What Hazel's material has and Aprend's doesn't:** a copy
   (`Material::Copy`), reading a value back (`Get`), and default
   resources for slots left empty. Without defaults an incomplete
   material fails the compile, which is stricter than Hazel and arguably
   right, but every slot has to be set before the first draw.
3. **Reflection limits**: the list is item 18's "what is left", and is
   kept there only.
4. **The arena's remaining policy** (item 19): place a new block in the
   fullest buffer that has room, so lightly used buffers drain, and a trim
   that releases the spare buffer. Held until item 35 decides what the
   arena's bookkeeping is.
