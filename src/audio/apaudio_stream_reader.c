#include "apaudio_stream_reader.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* A streaming decoder's reads of a caller's source (apaudio_stream_reader.h).
 * Compiled into stb_vorbis's library, whose changed copy calls the stdio
 * half; the decoders in apaudio_decode.cpp call the rest. No read is ever
 * asked of the source past its size: a request past it is the decoder's end
 * of data, not a read. */

/* Reads [count] bytes of the source at [offset], which the caller has kept
 * within its size. False, with the reader failed, if the source's read
 * fails. */
static bool source_read(apaudio_stream_reader *reader, uint64_t offset, void *out, uint64_t count)
{
    if (reader->failed)
        return false;
    if (count == 0)
        return true;
    if (reader->read(reader->user, offset, out, count))
        return true;
    reader->failed = true;
    reader->failed_offset = offset;
    reader->failed_count = count;
    return false;
}

/* The bytes left from [position] to the source's end. */
static uint64_t left(const apaudio_stream_reader *reader)
{
    return reader->position < reader->size ? reader->size - reader->position : 0;
}

uint64_t apaudio_stream_reader_read(apaudio_stream_reader *reader, void *out, uint64_t count)
{
    uint64_t available = left(reader);
    if (count > available)
        count = available;
    if (count == 0 || reader->failed)
        return 0;
    if (!reader->buffer)
    {
        if (!source_read(reader, reader->position, out, count))
            return 0;
        reader->position += count;
        return count;
    }
    /* Buffered: the buffer's bytes first, then a whole buffer at a time, or
     * the rest straight in when it is at least a buffer's worth. */
    unsigned char *to = (unsigned char *)out;
    uint64_t done = 0;
    while (done < count)
    {
        uint64_t end = reader->buffer_offset + reader->buffer_count;
        if (reader->position >= reader->buffer_offset && reader->position < end)
        {
            uint64_t take = end - reader->position;
            if (take > count - done)
                take = count - done;
            memcpy(to + done, reader->buffer + (reader->position - reader->buffer_offset), (size_t)take);
            reader->position += take;
            done += take;
            continue;
        }
        uint64_t rest = count - done;
        if (rest >= reader->buffer_capacity)
        {
            if (!source_read(reader, reader->position, to + done, rest))
                return 0;
            reader->position += rest;
            done += rest;
            continue;
        }
        uint64_t fill = left(reader);
        if (fill > reader->buffer_capacity)
            fill = reader->buffer_capacity;
        if (!source_read(reader, reader->position, reader->buffer, fill))
        {
            reader->buffer_count = 0;
            return 0;
        }
        reader->buffer_offset = reader->position;
        reader->buffer_count = (uint32_t)fill;
    }
    return done;
}

bool apaudio_stream_reader_seek(apaudio_stream_reader *reader, uint64_t offset)
{
    if (offset > reader->size)
        return false;
    reader->position = offset;
    return true;
}

/* ---- stb_vorbis's stdio calls ------------------------------------------- */

int apaudio_vorbis_fgetc(apaudio_stream_reader *file)
{
    unsigned char byte;
    if (apaudio_stream_reader_read(file, &byte, 1) != 1)
        return EOF;
    return byte;
}

size_t apaudio_vorbis_fread(void *data, size_t size, size_t count, apaudio_stream_reader *file)
{
    if (size == 0 || count == 0)
        return 0;
    /* stb_vorbis asks for one item of n bytes: all of it, or none. */
    if (count > SIZE_MAX / size)
        return 0;
    uint64_t bytes = (uint64_t)size * count;
    uint64_t got = apaudio_stream_reader_read(file, data, bytes);
    return (size_t)(got / size);
}

int apaudio_vorbis_fseek(apaudio_stream_reader *file, long offset, int origin)
{
    int64_t base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? (int64_t)file->position : (int64_t)file->size;
    int64_t to = base + (int64_t)offset;
    if (to < 0 || !apaudio_stream_reader_seek(file, (uint64_t)to))
        return -1;
    return 0;
}

long apaudio_vorbis_ftell(apaudio_stream_reader *file)
{
    /* A Vorbis source is under 2 GiB (apaudio_sound_create() refuses one at
     * or past it), so its every position fits. */
    return file->position > (uint64_t)LONG_MAX ? -1L : (long)file->position;
}

int apaudio_vorbis_fclose(apaudio_stream_reader *file)
{
    (void)file;
    return 0;
}

apaudio_stream_reader *apaudio_vorbis_fopen(const char *name, const char *mode)
{
    (void)name;
    (void)mode;
    return NULL;
}

int apaudio_vorbis_fopen_s(apaudio_stream_reader **file, const char *name, const char *mode)
{
    (void)name;
    (void)mode;
    *file = NULL;
    return 1;
}
