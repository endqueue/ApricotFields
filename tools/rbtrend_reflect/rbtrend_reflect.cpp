// rbtrend_reflect: a shader's SPIR-V in, the reflection table its package
// carries out (RBTREND_SHADER_PACKAGE::reflection, rbtrendreflect_table.hpp).
// A build tool for the shader step (cmake/rbtrend_slang.cmake), run on the
// build machine; never part of the library, which reads the table and no
// SPIR-V (card X18).
//
//     rbtrend_reflect --stage vertex|fragment|compute|mesh [--declared <in.reflection.json>]
//                    <in.spv> <out.reflection>
//
// --declared names Slang's reflection JSON of the same shader: a vertex
// shader's inputs it declares and its SPIR-V dropped are added from it, not
// used (card A51). Read for the vertex stage only.
//
// Exits 0, or 1 with the reason on stderr: among them a shader Rend can't
// bind (a texel buffer, an input attachment, an array of descriptors
// without a size...), so the build fails, not the run.

#include "rbtrend_reflect_tool.hpp"

#include <cstdio>
#include <cstring>
#include <new>

static int fail(const char *what, const char *detail) {
	fprintf(stderr, "rbtrend_reflect: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
	return 1;
}

int main(int argc, char **argv) {
	const char *stage_name = nullptr;
	const char *json_path  = nullptr;
	const char *in_path    = nullptr;
	const char *out_path   = nullptr;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--stage") == 0 && i + 1 < argc)
			stage_name = argv[++i];
		else if (strcmp(argv[i], "--declared") == 0 && i + 1 < argc)
			json_path = argv[++i];
		else if (!in_path)
			in_path = argv[i];
		else if (!out_path)
			out_path = argv[i];
		else
			return fail("unexpected argument", argv[i]);
	}
	if (!stage_name || !in_path || !out_path)
		return fail("usage: rbtrend_reflect --stage vertex|fragment|compute|mesh [--declared <in.reflection.json>] <in.spv> <out.reflection>", nullptr);
	CRGPU_SHADER_STAGE stage;
	if (strcmp(stage_name, "vertex") == 0)
		stage = CRGPU_SHADER_STAGE_VERTEX;
	else if (strcmp(stage_name, "fragment") == 0)
		stage = CRGPU_SHADER_STAGE_FRAGMENT;
	else if (strcmp(stage_name, "compute") == 0)
		stage = CRGPU_SHADER_STAGE_COMPUTE;
	else if (strcmp(stage_name, "mesh") == 0)
		stage = CRGPU_SHADER_STAGE_MESH;
	else
		return fail("--stage is none of vertex, fragment, compute and mesh", stage_name);
	// The table is little-endian, as every machine that builds shaders here is.
	const uint32_t one = 1;
	unsigned char first_byte;
	memcpy(&first_byte, &one, 1);
	if (first_byte != 1)
		return fail("this machine isn't little-endian, which the table is", nullptr);

	FILE *in = fopen(in_path, "rb");
	if (!in)
		return fail("can't open", in_path);
	std::vector<uint32_t> words;
	try {
		if (fseek(in, 0, SEEK_END) != 0) {
			fclose(in);
			return fail("can't read", in_path);
		}
		const long size = ftell(in);
		if (size <= 0 || size % 4 != 0 || fseek(in, 0, SEEK_SET) != 0) {
			fclose(in);
			return fail("not SPIR-V words", in_path);
		}
		words.resize((size_t)size / 4);
		const size_t read = fread(words.data(), 1, (size_t)size, in);
		fclose(in);
		if (read != (size_t)size)
			return fail("can't read", in_path);

		rbtrend_reflection_store store;
		std::string error;
		if (!rbtrend_reflect_spirv(words.data(), words.size(), stage, &store, &error))
			return fail(error.c_str(), in_path);
		if (json_path && stage == CRGPU_SHADER_STAGE_VERTEX) {
			FILE *json_file = fopen(json_path, "rb");
			if (!json_file)
				return fail("can't open", json_path);
			std::string json;
			char chunk[4096];
			size_t got;
			while ((got = fread(chunk, 1, sizeof(chunk), json_file)) > 0)
				json.append(chunk, got);
			const bool read_error = ferror(json_file) != 0;
			fclose(json_file);
			if (read_error)
				return fail("can't read", json_path);
			if (!rbtrend_reflect_declared_inputs(json.data(), json.size(), &store, &error))
				return fail(error.c_str(), json_path);
		}
		std::vector<uint8_t> table;
		if (!rbtrend_reflect_table_write(store, &table, &error))
			return fail(error.c_str(), in_path);
		FILE *out = fopen(out_path, "wb");
		if (!out)
			return fail("can't write", out_path);
		const bool written = fwrite(table.data(), 1, table.size(), out) == table.size();
		if (fclose(out) != 0 || !written)
			return fail("can't write", out_path);
	} catch (const std::bad_alloc &) {
		return fail("out of memory", nullptr);
	}
	return 0;
}
