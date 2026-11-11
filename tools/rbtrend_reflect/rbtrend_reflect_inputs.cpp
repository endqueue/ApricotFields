// A vertex shader's declared inputs, from Slang's reflection of it (card
// A51): Slang's SPIR-V keeps only the inputs the entry point reads, and its
// DXIL keeps every declared input in the signature, which D3D12 requires an
// input layout to feed. The SPIR-V's reflection
// (rbtrend_reflect_spirv.cpp) is completed here from the JSON Slang writes
// beside it (-reflection-json), each input it dropped added, not used.

#include "rbtrend_reflect_tool.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

// One JSON value, as much as this file reads: a tree of plain values.
struct json_value {
	enum kind_t : uint8_t { null_value, boolean, number, string, array, object };
	kind_t kind = null_value;
	double number_value = 0.0;
	std::string text;
	std::vector<json_value> items;   // an array's, or an object's values
	std::vector<std::string> keys;   // an object's keys, beside its items

	const json_value *get(const char *key) const {
		if (kind != object)
			return nullptr;
		for (size_t i = 0; i < keys.size(); ++i)
			if (keys[i] == key)
				return &items[i];
		return nullptr;
	}
	const char *get_string(const char *key) const {
		const json_value *value = get(key);
		return value && value->kind == string ? value->text.c_str() : nullptr;
	}
	bool get_uint(const char *key, uint32_t *out) const {
		const json_value *value = get(key);
		if (!value || value->kind != number || value->number_value < 0.0 || value->number_value > 4294967295.0 ||
		    value->number_value != (double)(uint32_t)value->number_value)
			return false;
		*out = (uint32_t)value->number_value;
		return true;
	}
};

// Nesting past this is refused: Slang's reflection is a few levels deep.
constexpr int max_json_depth = 64;

// A bounded reader of the JSON grammar over [size] bytes; never reads past
// them. Throws std::bad_alloc.
struct json_reader {
	const char *at;
	const char *end;

	void skip_space() {
		while (at < end && (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r'))
			++at;
	}
	bool literal(const char *word) {
		const size_t length = strlen(word);
		if ((size_t)(end - at) < length || memcmp(at, word, length) != 0)
			return false;
		at += length;
		return true;
	}
	bool read_string(std::string *out) {
		if (at >= end || *at != '"')
			return false;
		++at;
		while (at < end && *at != '"') {
			char c = *at++;
			if ((unsigned char)c < 0x20)
				return false;
			if (c == '\\') {
				if (at >= end)
					return false;
				c = *at++;
				switch (c) {
				case '"': case '\\': case '/': break;
				case 'b': c = '\b'; break;
				case 'f': c = '\f'; break;
				case 'n': c = '\n'; break;
				case 'r': c = '\r'; break;
				case 't': c = '\t'; break;
				case 'u':
					// A name read here is ASCII; another code point is kept as '?'.
					if (end - at < 4)
						return false;
					for (int i = 0; i < 4; ++i)
						if (!strchr("0123456789abcdefABCDEF", at[i]) || !at[i])
							return false;
					at += 4;
					c = '?';
					break;
				default:
					return false;
				}
			}
			out->push_back(c);
		}
		if (at >= end)
			return false;
		++at;
		return true;
	}
	bool read_number(double *out) {
		const char *start = at;
		while (at < end && strchr("+-0123456789.eE", *at) && *at)
			++at;
		if (at == start || at - start > 64)
			return false;
		const std::string digits(start, at);
		char *stop = nullptr;
		*out = strtod(digits.c_str(), &stop);
		return stop == digits.c_str() + digits.size();
	}
	bool read(json_value *out, int depth) {
		if (depth > max_json_depth)
			return false;
		skip_space();
		if (at >= end)
			return false;
		if (*at == '{') {
			++at;
			out->kind = json_value::object;
			skip_space();
			if (at < end && *at == '}') {
				++at;
				return true;
			}
			for (;;) {
				skip_space();
				out->keys.emplace_back();
				if (!read_string(&out->keys.back()))
					return false;
				skip_space();
				if (at >= end || *at != ':')
					return false;
				++at;
				out->items.emplace_back();
				if (!read(&out->items.back(), depth + 1))
					return false;
				skip_space();
				if (at < end && *at == ',') {
					++at;
					continue;
				}
				if (at < end && *at == '}') {
					++at;
					return true;
				}
				return false;
			}
		}
		if (*at == '[') {
			++at;
			out->kind = json_value::array;
			skip_space();
			if (at < end && *at == ']') {
				++at;
				return true;
			}
			for (;;) {
				out->items.emplace_back();
				if (!read(&out->items.back(), depth + 1))
					return false;
				skip_space();
				if (at < end && *at == ',') {
					++at;
					continue;
				}
				if (at < end && *at == ']') {
					++at;
					return true;
				}
				return false;
			}
		}
		if (*at == '"') {
			out->kind = json_value::string;
			return read_string(&out->text);
		}
		if (literal("true")) {
			out->kind = json_value::boolean;
			return true;
		}
		if (literal("false")) {
			out->kind = json_value::boolean;
			return true;
		}
		if (literal("null"))
			return true;
		out->kind = json_value::number;
		return read_number(&out->number_value);
	}
};

// One input the shader declares: a field of a struct parameter, or a
// parameter, bound as a varying input.
struct declared_input {
	std::string name;
	uint32_t location;
	const json_value *type;
};

RBTREND_SHADER_SCALAR_TYPE scalar_type_of(const char *slang_name) {
	if (!slang_name)
		return RBTREND_SHADER_SCALAR_TYPE_OTHER;
	if (strcmp(slang_name, "float32") == 0)
		return RBTREND_SHADER_SCALAR_TYPE_FLOAT;
	if (strcmp(slang_name, "int32") == 0)
		return RBTREND_SHADER_SCALAR_TYPE_INT;
	if (strcmp(slang_name, "uint32") == 0)
		return RBTREND_SHADER_SCALAR_TYPE_UINT;
	if (strcmp(slang_name, "bool") == 0)
		return RBTREND_SHADER_SCALAR_TYPE_BOOL;
	return RBTREND_SHADER_SCALAR_TYPE_OTHER;
}

// [value]'s binding, if it is a varying input of its own (a system value
// such as SV_VertexID has none, or another kind).
bool varying_input_location(const json_value &value, uint32_t *location) {
	const json_value *binding = value.get("binding");
	if (!binding)
		return false;
	const char *kind = binding->get_string("kind");
	if (!kind || strcmp(kind, "varyingInput") != 0)
		return false;
	const char *semantic = value.get_string("semanticName");
	if (semantic && strncmp(semantic, "SV_", 3) == 0)
		return false;
	return binding->get_uint("index", location);
}

bool complete(const json_value &root, rbtrend_reflection_store &store, std::string &error) {
	const json_value *entry_points = root.get("entryPoints");
	if (!entry_points || entry_points->kind != json_value::array) {
		error = "the reflection JSON has no entryPoints";
		return false;
	}
	const json_value *entry = nullptr;
	for (const json_value &candidate : entry_points->items) {
		const char *name = candidate.get_string("name");
		if (name && strcmp(name, store.view.entry_point) == 0)
			entry = &candidate;
	}
	if (!entry) {
		error = std::string("the reflection JSON has no entry point ") + store.view.entry_point;
		return false;
	}
	std::vector<declared_input> declared;
	if (const json_value *parameters = entry->get("parameters")) {
		for (const json_value &parameter : parameters->items) {
			const json_value *type = parameter.get("type");
			const char *type_kind = type ? type->get_string("kind") : nullptr;
			if (type_kind && strcmp(type_kind, "struct") == 0) {
				const json_value *fields = type->get("fields");
				if (!fields)
					continue;
				for (const json_value &field : fields->items) {
					uint32_t location;
					const char *name = field.get_string("name");
					if (name && field.get("type") && varying_input_location(field, &location))
						declared.push_back({name, location, field.get("type")});
				}
				continue;
			}
			uint32_t location;
			const char *name = parameter.get_string("name");
			if (name && type && varying_input_location(parameter, &location))
				declared.push_back({name, location, type});
		}
	}

	// Every input the SPIR-V has is declared, by name and location.
	for (const RBTREND_SHADER_INPUT &input : store.inputs) {
		bool found = false;
		for (const declared_input &d : declared)
			found = found || (d.location == input.location && d.name == input.name);
		if (!found) {
			error = std::string("the SPIR-V's input ") + input.name + " isn't one the reflection JSON declares at its location";
			return false;
		}
	}
	// Each declared input the SPIR-V dropped, not used.
	for (const declared_input &d : declared) {
		const RBTREND_SHADER_INPUT *present = nullptr;
		for (const RBTREND_SHADER_INPUT &input : store.inputs)
			if (input.location == d.location)
				present = &input;
		if (present && d.name != present->name) {
			error = "the reflection JSON declares " + d.name + " where the SPIR-V's input " + present->name + " is";
			return false;
		}
		if (present)
			continue;
		const char *kind = d.type->get_string("kind");
		RBTREND_SHADER_INPUT input{};
		if (kind && strcmp(kind, "scalar") == 0) {
			input.components  = 1;
			input.scalar_type = scalar_type_of(d.type->get_string("scalarType"));
		} else if (kind && strcmp(kind, "vector") == 0) {
			const json_value *element = d.type->get("elementType");
			if (!d.type->get_uint("elementCount", &input.components) || input.components < 1 || input.components > 4 || !element) {
				error = "the reflection JSON's input " + d.name + " has no vector size";
				return false;
			}
			input.scalar_type = scalar_type_of(element->get_string("scalarType"));
		} else {
			// A matrix's or an array's locations aren't read from the JSON:
			// declare it read, or not at all.
			error = "the input " + d.name + ", declared and not read, is a matrix or an array, whose locations aren't known here";
			return false;
		}
		store.strings.push_back(d.name);
		input.name           = store.strings.back().c_str();
		input.location       = d.location;
		input.location_count = 1;
		input.used           = false;
		store.inputs.push_back(input);
	}
	std::sort(store.inputs.begin(), store.inputs.end(),
	          [](const RBTREND_SHADER_INPUT &a, const RBTREND_SHADER_INPUT &b) { return a.location < b.location; });
	for (size_t i = 1; i < store.inputs.size(); ++i) {
		const RBTREND_SHADER_INPUT &previous = store.inputs[i - 1];
		if (store.inputs[i].location < previous.location + previous.location_count) {
			error = std::string("the inputs ") + previous.name + " and " + store.inputs[i].name + " overlap";
			return false;
		}
	}
	store.view.inputs      = store.inputs.data();
	store.view.input_count = (uint32_t)store.inputs.size();
	return true;
}

} // namespace

bool rbtrend_reflect_declared_inputs(const char *json, size_t size, rbtrend_reflection_store *store, std::string *error) noexcept {
	try {
		// A shader whose SPIR-V has more than one vertex entry point counts
		// everything declared as used already (rbtrendshader.h, "used").
		if (store->view.stage != CRGPU_SHADER_STAGE_VERTEX || !store->view.entry_point)
			return true;
		json_value root;
		json_reader reader{json, json + size};
		if (!reader.read(&root, 0)) {
			*error = "the reflection JSON can't be read";
			return false;
		}
		reader.skip_space();
		if (reader.at != reader.end) {
			*error = "the reflection JSON has bytes after its value";
			return false;
		}
		return complete(root, *store, *error);
	} catch (const std::bad_alloc &) {
		try {
			*error = "out of memory";
		} catch (...) {
		}
		return false;
	}
}
