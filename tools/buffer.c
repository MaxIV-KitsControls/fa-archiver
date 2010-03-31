/* FA archiver memory buffer.
 *
 * Handles the central memory buffer. */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>

#include "list.h"
#include "error.h"

#include "buffer.h"


size_t fa_block_size;               // Bytes in one buffer block

static size_t block_count;          // Number of blocks in buffer
static void * frame_buffer;         // In RAM buffer of captured FA frames
static bool * gap_buffer;           // Records gaps in frame buffer

static size_t buffer_index_in = 0;  // Write pointer, in blocks


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Miscellaneous support routines.                                           */


pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t in_signal = PTHREAD_COND_INITIALIZER;

static void lock(void)     { ASSERT_0(pthread_mutex_lock(&buffer_mutex)); }
static void unlock(void*_) { ASSERT_0(pthread_mutex_unlock(&buffer_mutex)); }

/* When using deferred thread cancellation we need to use pthread_cleanup_
 * calls for locking to ensure that when a thread is cancelled we don't end
 * up with the buffer_mutex in an inconsistent state. */
#define LOCK() \
    lock(); \
    pthread_cleanup_push(unlock, NULL)
#define UNLOCK() \
    pthread_cleanup_pop(true)


static void signal(pthread_cond_t *cond)
{
    ASSERT_0(pthread_cond_broadcast(cond));
}
static void wait(pthread_cond_t *cond)
{
    ASSERT_0(pthread_cond_wait(cond, &buffer_mutex));
}


static void advance_index(size_t *index)
{
    *index += 1;
    if (*index >= block_count)
        *index -= block_count;
}

static void * get_buffer(size_t index)
{
    return (char *) frame_buffer + index * fa_block_size;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Reader routines.                                                          */

struct reader_state
{
    size_t index_out;               // Next block to read
    bool underflowed;               // Set if buffer overrun for this reader
    bool running;                   // Used to halt reader
    int backlog;                    // Gap between read and write pointer
    struct list_head list_entry;    // Links all active readers together
    struct list_head reserved_entry;    // and all reserved readers
};


LIST_HEAD(all_readers);
LIST_HEAD(reserved_readers);

/* Iterators for these two lists. */
#define for_all_readers(reader) \
    list_for_each_entry( \
        struct reader_state, list_entry, reader, &all_readers)
#define for_reserved_readers(reader) \
    list_for_each_entry( \
        struct reader_state, reserved_entry, reader, &reserved_readers)


/* Updates the backlog count.  This is computed as the maximum number of
 * unread frames from the write pointer to our read pointer.  As we're only
 * interested in the maximum value, this only needs to be updated when frames
 * are written. */
static void update_backlog(struct reader_state *reader)
{
    int backlog = buffer_index_in - reader->index_out;
    if (backlog < 0)
        backlog += block_count;
    
    if (backlog > reader->backlog)
        reader->backlog = backlog;
}


struct reader_state * open_reader(bool reserved_reader)
{
    struct reader_state *reader = malloc(sizeof(struct reader_state));
    reader->index_out = buffer_index_in;
    reader->underflowed = false;
    reader->backlog = 0;
    reader->running = true;
    INIT_LIST_HEAD(&reader->reserved_entry);
    
    LOCK();
    list_add_tail(&reader->list_entry, &all_readers);
    if (reserved_reader)
        list_add_tail(&reader->reserved_entry, &reserved_readers);
    UNLOCK();
    
    return reader;
}


void close_reader(struct reader_state *reader)
{
    LOCK();
    list_del(&reader->list_entry);
    list_del(&reader->reserved_entry);
    UNLOCK();
    
    free(reader);
}


void * get_read_block(struct reader_state *reader, int *backlog)
{
    void *buffer;
    LOCK();
    if (reader->underflowed)
    {
        /* If we were underflowed then perform a complete reset of the read
         * stream.  Discard everything in the buffer and start again.  This
         * helps the writer which can rely on this.  We'll also start by
         * reporting a synthetic gap. */
        reader->index_out = buffer_index_in;
        reader->underflowed = false;
        buffer = NULL;
    }
    else
    {

        /* If we're on the tail of the writer then we have to wait for a new
         * entry in the buffer. */
        while (reader->running  &&  reader->index_out == buffer_index_in)
            wait(&in_signal);
        if (!reader->running)
            buffer = NULL;
        else if (gap_buffer[reader->index_out])
        {
            /* Nothing to actually read at this point, just return gap
             * indicator instead. */
            buffer = NULL;
            advance_index(&reader->index_out);
        }
        else
            buffer = get_buffer(reader->index_out);
    }
    
    if (backlog)
    {
        *backlog = reader->backlog * fa_block_size;
        reader->backlog = 0;
    }
    UNLOCK();
    return buffer;
}


void stop_reader(struct reader_state *reader)
{
    LOCK();
    reader->running = false;
    signal(&in_signal);
    UNLOCK();
}


bool release_read_block(struct reader_state *reader)
{
    bool underflow;
    LOCK();
    advance_index(&reader->index_out);
    underflow = reader->underflowed;
    UNLOCK();
    return !underflow;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Writer routines.                                                          */

static bool in_gap = false;         // Used to coalesce repeated gaps

/* Checks for the presence of a blocking reserved reader. */
static bool blocking_readers(void)
{
    for_reserved_readers(reader)
        if (buffer_index_in == reader->index_out  &&  reader->underflowed)
            return true;
    return false;
}


void * get_write_block(void)
{
    void *buffer;
    LOCK();
    if (blocking_readers())
        /* There's a reserved reader not finished with the next block yet.
         * Bail and try again later. */
        buffer = NULL;
    else
        /* Normal case, just write into the current in pointer. */
        buffer = get_buffer(buffer_index_in);
    UNLOCK();
    return buffer;
}


void release_write_block(bool gap)
{
    if (gap  &&  in_gap)
        /* Ignore repeated reports of the same gap. */
        return;
    in_gap = gap;
    
    LOCK();
    gap_buffer[buffer_index_in] = gap;
    advance_index(&buffer_index_in);

    /* Let all readers know if they've suffered an underflow. */
    for_all_readers(reader)
    {
        if (buffer_index_in == reader->index_out)
            /* Whoops.  We've collided with a reader.  Mark the reader as
             * underflowed. */
            reader->underflowed = true;
        else
            update_backlog(reader);
    }
    signal(&in_signal);
    UNLOCK();
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */



bool initialise_buffer(size_t _block_size, size_t _block_count)
{
    fa_block_size = _block_size;
    block_count = _block_count;
    return
        /* The frame buffer must be page aligned, because we're going to write
         * to disk with direct I/O. */
        TEST_NULL(frame_buffer = valloc(block_count * fa_block_size))  &&
        TEST_NULL(gap_buffer   = malloc(block_count * sizeof(bool)));
}