#ifndef APAUDIO_STREAM_READER_H
#define APAUDIO_STREAM_READER_H

/*
 * Internal header - never included outside ApricotFields/src/audio/.
 *
 * What a streaming decoder reads a caller's source through (apaudio.h,
 * APAUDIO_SOURCE; "Streamed sounds"): a position, and the first read that
 * failed. Written in C because stb_vorbis's changed copy reads through it
 * too: its FILE * path is this reader (CMakeLists.txt, "ApAudio's
 * decoders"), with fgetc, fread, fseek and ftell the calls below, so a
 * Vorbis stream decodes with stb_vorbis's own pull decoder and seeks exactly.
 * The reader holds the source's three fields itself, so this compiles with
 * no header of ApricotFields'.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct apaudio_stream_reader
{
    /* The caller's source: APAUDIO_SOURCE's fields. */
    void *user;
    uint64_t size;
    bool (*read)(void *user, uint64_t offset, void *buffer, uint64_t count);
    /* The next byte a read takes. */
    uint64_t position;
    /* A byte-at-a-time reader's room (stb_vorbis's): [buffer_count] bytes of
     * the source from [buffer_offset]. NULL: every read goes to the source. */
    unsigned char *buffer;
    uint64_t buffer_offset;
    uint32_t buffer_capacity;
    uint32_t buffer_count;
    /* The first read of the source that failed: where, and how many bytes. */
    bool failed;
    uint64_t failed_offset;
    uint64_t failed_count;
} apaudio_stream_reader;

/* Copies up to [count] bytes from the reader's position into [out] and moves
 * on past them: how many, fewer only at the source's end. Never asks the
 * source for a byte past its size. 0, with the reader failed, if the source's
 * read fails; once failed, every read is 0. */
uint64_t apaudio_stream_reader_read(apaudio_stream_reader *reader, void *out, uint64_t count);

/* Moves the position to [offset]: false, unmoved, past the source's end. */
bool apaudio_stream_reader_seek(apaudio_stream_reader *reader, uint64_t offset);

/* stb_vorbis's stdio calls, over a reader ([file] is one). */
int apaudio_vorbis_fgetc(apaudio_stream_reader *file);
size_t apaudio_vorbis_fread(void *data, size_t size, size_t count, apaudio_stream_reader *file);
int apaudio_vorbis_fseek(apaudio_stream_reader *file, long offset, int origin);
long apaudio_vorbis_ftell(apaudio_stream_reader *file);
int apaudio_vorbis_fclose(apaudio_stream_reader *file);
/* No file is ever opened by name: these refuse. */
apaudio_stream_reader *apaudio_vorbis_fopen(const char *name, const char *mode);
int apaudio_vorbis_fopen_s(apaudio_stream_reader **file, const char *name, const char *mode);

#ifdef __cplusplus
}
#endif

#endif /* APAUDIO_STREAM_READER_H */
