#ifndef RBTAUDIO_STREAM_READER_H
#define RBTAUDIO_STREAM_READER_H

/*
 * Internal header - never included outside rabbittus/src/audio/.
 *
 * What a streaming decoder reads a caller's source through (rbtaudio.h,
 * RBTAUDIO_SOURCE; "Streamed sounds"): a position, and the first read that
 * failed. Written in C because stb_vorbis's changed copy reads through it
 * too: its FILE * path is this reader (CMakeLists.txt, "Rabbittus Audio's
 * decoders"), with fgetc, fread, fseek and ftell the calls below, so a
 * Vorbis stream decodes with stb_vorbis's own pull decoder and seeks exactly.
 * The reader holds the source's three fields itself, so this compiles with
 * no header of Rabbittus'.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rbtaudio_stream_reader
{
    /* The caller's source: RBTAUDIO_SOURCE's fields. */
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
} rbtaudio_stream_reader;

/* Copies up to [count] bytes from the reader's position into [out] and moves
 * on past them: how many, fewer only at the source's end. Never asks the
 * source for a byte past its size. 0, with the reader failed, if the source's
 * read fails; once failed, every read is 0. */
uint64_t rbtaudio_stream_reader_read(rbtaudio_stream_reader *reader, void *out, uint64_t count);

/* Moves the position to [offset]: false, unmoved, past the source's end. */
bool rbtaudio_stream_reader_seek(rbtaudio_stream_reader *reader, uint64_t offset);

/* stb_vorbis's stdio calls, over a reader ([file] is one). */
int rbtaudio_vorbis_fgetc(rbtaudio_stream_reader *file);
size_t rbtaudio_vorbis_fread(void *data, size_t size, size_t count, rbtaudio_stream_reader *file);
int rbtaudio_vorbis_fseek(rbtaudio_stream_reader *file, long offset, int origin);
long rbtaudio_vorbis_ftell(rbtaudio_stream_reader *file);
int rbtaudio_vorbis_fclose(rbtaudio_stream_reader *file);
/* No file is ever opened by name: these refuse. */
rbtaudio_stream_reader *rbtaudio_vorbis_fopen(const char *name, const char *mode);
int rbtaudio_vorbis_fopen_s(rbtaudio_stream_reader **file, const char *name, const char *mode);

#ifdef __cplusplus
}
#endif

#endif /* RBTAUDIO_STREAM_READER_H */
