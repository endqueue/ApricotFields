
// Writing and reading a block member where a shader's reflection lays it
// out (rbtrendreflect_internal.hpp). The reflection itself is made at build
// time (tools/rbtrend_reflect) and read from the package's table
// (rbtrendreflect_table.cpp). Nothing here calls Carrottus GPU.

#include "rbtrendreflect_internal.hpp"

#include <cstring>

uint32_t rbtrend_uniform_type_columns(RBTREND_UNIFORM_TYPE type) {
	switch (type) {
	case RBTREND_UNIFORM_TYPE_MAT2:
		return 2;
	case RBTREND_UNIFORM_TYPE_MAT3:
		return 3;
	case RBTREND_UNIFORM_TYPE_MAT4:
		return 4;
	default:
		return 1;
	}
}

uint32_t rbtrend_uniform_type_column_size(RBTREND_UNIFORM_TYPE type) {
	const uint32_t size = rbtrend_uniform_type_get_size(type);
	return size / rbtrend_uniform_type_columns(type);
}

void rbtrend_write_block_member(
    const RBTREND_SHADER_MEMBER &member,
    uint8_t *block,
    const void *data) {
	const uint8_t *source        = (const uint8_t *)data;
	const uint32_t columns       = rbtrend_uniform_type_columns(member.type);
	const uint32_t column_size   = rbtrend_uniform_type_column_size(member.type);
	const uint32_t element_size  = columns * column_size;
	for (uint32_t element = 0; element < member.array_count; ++element) {
		uint8_t *destination = block + member.offset + (size_t)element * member.array_stride;
		if (columns == 1) {
			memcpy(destination, source, element_size);
		} else if (!member.row_major) {
			// Column after column, each at the matrix stride: a mat3's
			// columns are 12 bytes apart in the caller's data and 16 in a
			// std140 block.
			for (uint32_t column = 0; column < columns; ++column)
				memcpy(destination + (size_t)column * member.matrix_stride, source + (size_t)column * column_size, column_size);
		} else {
			// Stored a row at a time: the caller's column c, row r is
			// row r, column c of the block.
			const uint32_t rows = column_size / 4;
			for (uint32_t column = 0; column < columns; ++column)
				for (uint32_t row = 0; row < rows; ++row)
					memcpy(destination + (size_t)row * member.matrix_stride + (size_t)column * 4, source + (size_t)column * column_size + (size_t)row * 4, 4);
		}
		source += element_size;
	}
}

void rbtrend_read_block_member(
    const RBTREND_SHADER_MEMBER &member,
    const uint8_t *block,
    void *out_data) {
	uint8_t *destination         = (uint8_t *)out_data;
	const uint32_t columns       = rbtrend_uniform_type_columns(member.type);
	const uint32_t column_size   = rbtrend_uniform_type_column_size(member.type);
	const uint32_t element_size  = columns * column_size;
	for (uint32_t element = 0; element < member.array_count; ++element) {
		const uint8_t *source = block + member.offset + (size_t)element * member.array_stride;
		// rbtrend_write_block_member, the other way.
		if (columns == 1) {
			memcpy(destination, source, element_size);
		} else if (!member.row_major) {
			for (uint32_t column = 0; column < columns; ++column)
				memcpy(destination + (size_t)column * column_size, source + (size_t)column * member.matrix_stride, column_size);
		} else {
			const uint32_t rows = column_size / 4;
			for (uint32_t column = 0; column < columns; ++column)
				for (uint32_t row = 0; row < rows; ++row)
					memcpy(destination + (size_t)column * column_size + (size_t)row * 4, source + (size_t)row * member.matrix_stride + (size_t)column * 4, 4);
		}
		destination += element_size;
	}
}
