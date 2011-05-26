/* Simple server for archive data.
 *
 */

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

#include "error.h"
#include "sniffer.h"
#include "archiver.h"
#include "mask.h"
#include "buffer.h"
#include "reader.h"
#include "parse.h"
#include "disk.h"
#include "transform.h"
#include "decimate.h"

#include "socket_server.h"


/* String used to report protocol version in response to CV command. */
#define PROTOCOL_VERSION    "1.0"


/* Block buffer for full resolution FA data. */
static struct buffer *fa_block_buffer;
/* Block buffer for decimated FA data. */
static struct buffer *decimated_buffer;


static bool __attribute__((format(printf, 2, 3)))
    write_string(int sock, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *buffer = NULL;
    bool ok =
        TEST_IO(vasprintf(&buffer, format, args))  &&
        TEST_write_(
            sock, buffer, strlen(buffer), "Unable to write response");
    free(buffer);
    return ok;
}


static double get_mean_frame_rate(void)
{
    const struct disk_header *header = get_header();
    return (1e6 * header->major_sample_count) / header->last_duration;
}


/* The C command prefix is followed by a sequence of one letter commands, and
 * each letter receives a one line response.  The following commands are
 * supported:
 *
 *  Q   Closes archive server
 *  H   Halts data capture (only sensible for debug use)
 *  R   Resumes halted data capture
 *
 *  F   Returns current sample frequency
 *  d   Returns first decimation
 *  D   Returns second decimation
 *  T   Returns earliest available timestamp
 *  V   Returns protocol identification string
 *  M   Returns configured capture mask
 *  C   Returns live decimation factor if available
 */
static bool process_command(int scon, const char *buf)
{
    const struct disk_header *header = get_header();
    bool ok = true;
    for (buf ++; ok  &&  *buf != '\0'; buf ++)
    {
        switch (*buf)
        {
            case 'Q':
                log_message("Shutdown command received");
                ok = write_string(scon, "Shutdown\n");
                shutdown_archiver();
                break;
            case 'H':
                log_message("Temporary halt command received");
                ok = write_string(scon, "Halted\n");
                enable_buffer_write(fa_block_buffer, false);
                break;
            case 'R':
                log_message("Resume command received");
                enable_buffer_write(fa_block_buffer, true);
                break;

            case 'F':
                ok = write_string(scon, "%f\n", get_mean_frame_rate());
                break;
            case 'd':
                ok = write_string(scon,
                     "%"PRIu32"\n", 1 << header->first_decimation_log2);
                break;
            case 'D':
                ok = write_string(scon,
                     "%"PRIu32"\n", 1 << header->second_decimation_log2);
                break;
            case 'T':
                {
                    uint64_t start = get_earliest_timestamp();
                    ok = write_string(scon, "%"PRIu64".%06"PRIu64"\n",
                        start / 1000000, start % 1000000);
                }
                break;
            case 'V':
                ok = write_string(scon, PROTOCOL_VERSION "\n");
                break;
            case 'M':
                {
                    char string[RAW_MASK_BYTES + 1];
                    format_raw_mask(&get_header()->archive_mask, string);
                    ok = write_string(scon, "%s\n", string);
                }
                break;

            case 'C':
                ok = write_string(scon, "%d\n", get_decimation_factor());
                break;

            default:
                ok = write_string(scon, "Unknown command '%c'\n", *buf);
                break;
        }
    }
    return ok;
}


/* A subscribe request is a filter mask followed by options:
 *
 *  subscription = "S" filter-mask options
 *  options = [ "T" ] [ "Z" ] [ "D" ]
 *
 * The options have the following meanings:
 *
 *  T   Start subscription stream with timestamp
 *  Z   Start subscription stream with t0
 *  D   Want decimated data stream
 *
 * If both T and Z are specified then the timestamp is sent first before T0.
 */
static bool parse_subscription(
    const char **string, struct filter_mask *mask,
    struct buffer **buffer, bool *want_timestamp, bool *want_t0)
{
    return
        parse_char(string, 'S')  &&
        parse_mask(string, mask)  &&
        DO_(
            *want_timestamp = read_char(string, 'T');
            *want_t0 = read_char(string, 'Z'))  &&
        IF_(read_char(string, 'D'),
            TEST_NULL_(decimated_buffer, "Decimated data not available")  &&
            DO_(*buffer = decimated_buffer));
}


bool report_socket_error(int scon, bool ok)
{
    bool write_ok;
    if (ok)
    {
        /* If all is well write a single null to let the caller know to expect a
         * normal response to follow. */
        pop_error_handling(false);
        char nul = '\0';
        write_ok = TEST_write(scon, &nul, 1);
    }
    else
    {
        /* If an error is encountered write the error message to the socket. */
        char *error_message = pop_error_handling(true);
        write_ok = write_string(scon, "%s\n", error_message);
        free(error_message);
    }
    return write_ok;
}


static bool send_subscription(
    int scon, struct reader_state *reader, uint64_t timestamp,
    bool want_timestamp, bool want_t0,
    struct filter_mask *mask, const void **block)
{
    /* The transmitted block optionally begins with the timestamp and T0 values,
     * in that order, if requested. */
    bool ok =
        IF_(want_timestamp,
            TEST_write(scon, &timestamp, sizeof(uint64_t)))  &&
        IF_(want_t0,
            TEST_write(scon, *block, sizeof(uint32_t)));

    size_t block_size = reader_block_size(reader);
    while (ok)
    {
        ok = FINALLY(
            write_frames(scon, mask, *block, block_size / FA_FRAME_SIZE),
            // Always do this, even if write_frames fails.
            TEST_OK_(release_read_block(reader),
                "Write underrun to client"));
        if (ok)
            ok = TEST_NULL_(
                *block = get_read_block(reader, NULL, NULL),
                "Gap in subscribed data");
        else
            *block = NULL;
    }
    return ok;
}


/* A subscription is a command of the form S<mask> where <mask> is a mask
 * specification as described in mask.h.  The default mask is empty. */
static bool process_subscribe(int scon, const char *buf)
{
    push_error_handling();

    /* Parse the incoming request. */
    struct filter_mask mask;
    bool want_timestamp = false, want_t0 = false;
    struct buffer *buffer = fa_block_buffer;
    if (!DO_PARSE(
            "subscription", parse_subscription, buf, &mask,
            &buffer, &want_timestamp, &want_t0))
        return report_socket_error(scon, false);

    /* See if we can start the subscription, report the final status to the
     * caller. */
    struct reader_state *reader = open_reader(buffer, false);
    uint64_t timestamp;
    const void *block = get_read_block(reader, NULL, &timestamp);
    bool start_ok = TEST_NULL_(block, "No data currently available");
    bool ok = report_socket_error(scon, start_ok);

    /* Send the requested subscription if all is well. */
    if (start_ok  &&  ok)
        ok = send_subscription(
            scon, reader, timestamp, want_timestamp, want_t0, &mask, &block);

    /* Clean up resources.  Rather important to get this right, as this can
     * happen many times. */
    if (block != NULL)
        release_read_block(reader);
    close_reader(reader);

    return ok;
}


static bool process_error(int scon, const char *buf)
{
    return write_string(scon, "Invalid command\n");
}


struct command_table {
    char id;            // Identification character
    bool (*process)(int scon, const char *buf);
} command_table[] = {
    { 'C', process_command },
    { 'R', process_read },
    { 'S', process_subscribe },
    { 0,   process_error }
};


/* Converts connected socket to a printable identification string. */
static void get_client_name(int scon, char *client_name)
{
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    if (TEST_IO(getpeername(scon, (struct sockaddr *) &name, &namelen)))
    {
        uint8_t * ip = (uint8_t *) &name.sin_addr.s_addr;
        sprintf(client_name, "%u.%u.%u.%u:%u",
            ip[0], ip[1], ip[2], ip[3], ntohs(name.sin_port));
    }
    else
        sprintf(client_name, "unknown");
}


/* Sets socket receive timeout.  Used so we don't have threads hanging waiting
 * for users to complete sending their commands.  (Also so I can remember how to
 * do this!)  See socket(7) for documentatino of SO_RCVTIMEO option. */
static bool set_socket_timeout(int sock, int secs, int usecs)
{
    struct timeval timeout = { .tv_sec = secs, .tv_usec = usecs };
    return TEST_IO(setsockopt(
        sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
}


/* Reads from the given socket until one of the following is encountered: a
 * newline (the preferred case), end of input, end of buffer or an error.  The
 * newline and anything following is discarded. */
static bool read_line(int sock, char *buf, size_t buflen)
{
    ssize_t rx;
    while (
        TEST_OK_(buflen > 0, "Read buffer exhausted")  &&
        TEST_IO_(rx = read(sock, buf, buflen), "Socket read failed")  &&
        TEST_OK_(rx > 0, "End of file on input"))
    {
        char *newline = memchr(buf, '\n', rx);
        if (newline)
        {
            *newline = '\0';
            return true;
        }

        buflen -= rx;
        buf += rx;
    }
    return false;
}


/* Command successfully read, dispatch it to the appropriate handler. */
static bool dispatch_command(
    int scon, const char *client_name, const char *buf)
{
    log_message("Client %s: \"%s\"", client_name, buf);
    struct command_table *command = command_table;
    while (command->id  &&  command->id != buf[0])
        command += 1;
    return command->process(scon, buf);
}


static void * process_connection(void *context)
{
    int scon = (intptr_t) context;

    /* Retrieve client address so we can log all messages associated with this
     * client with the appropriate address. */
    char client_name[64];
    get_client_name(scon, client_name);

    /* Read the command, required to be one line terminated by \n, and dispatch
     * to the appropriate handler.  Any errors are handled locally and are
     * reported below. */
    char buf[4096];
    push_error_handling();
    bool ok = FINALLY(
        set_socket_timeout(scon, 1, 0)  &&      // 1 second rx timeout
        read_line(scon, buf, sizeof(buf))  &&
        dispatch_command(scon, client_name, buf),

        // Always close the socket when done
        TEST_IO(close(scon)));

    /* Report any errors. */
    char *error_message = pop_error_handling(!ok);
    if (!ok)
        log_message("Client %s: %s", client_name, error_message);
    free(error_message);

    return NULL;
}


static void * run_server(void *context)
{
    int sock = (int)(intptr_t) context;
    /* Note that we need to create the spawned threads with DETACHED attribute,
     * otherwise we accumlate internal joinable state information and eventually
     * run out of resources. */
    pthread_attr_t attr;
    ASSERT_0(pthread_attr_init(&attr));
    ASSERT_0(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));

    int scon;
    pthread_t thread;
    while (TEST_IO(scon = accept(sock, NULL, NULL)))
        IGNORE(TEST_0(pthread_create(&thread, &attr,
            process_connection, (void *)(intptr_t) scon)));
    return NULL;
}


static pthread_t server_thread;
static int server_socket;

bool initialise_server(
    struct buffer *fa_buffer, struct buffer *decimated, int port)
{
    fa_block_buffer = fa_buffer;
    decimated_buffer = decimated;

    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };
    return
        TEST_IO(server_socket = socket(AF_INET, SOCK_STREAM, 0))  &&
        TEST_IO(bind(server_socket, (struct sockaddr *) &sin, sizeof(sin)))  &&
        TEST_IO(listen(server_socket, 5))  &&
        DO_(log_message("Server listening on port %d", port));
}

bool start_server(void)
{
    return
        TEST_0(pthread_create(
            &server_thread, NULL, run_server,
            (void *) (intptr_t) server_socket));
}


void terminate_server(void)
{
    IGNORE(TEST_0(pthread_cancel(server_thread)));
    /* We probably need to kill all the client threads. */
}
