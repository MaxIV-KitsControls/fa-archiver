/* Helper macros and declarations to simplify error handling. */

/* The following error handling macros are defined here:
 *
 *  TEST_OK     TEST_OK_    ASSERT_OK       Fail if expression is false
 *  TEST_IO     TEST_IO_    ASSERT_IO       Fail if expression is -1
 *  TEST_NULL   TEST_NULL_  ASSERT_NULL     Fail if expression equals NULL
 *  TEST_0      TEST_0_     ASSERT_0        Fail if expression is not 0
 *
 * There are also macros for handling file I/O in a similar form (but with
 * slightly different argument lists):
 *
 *  TEST_read   TEST_read_  ASSERT_read     Fail if read not of expected size
 *  TEST_write  TEST_write_ ASSERT_write    Fail if write not of expected size
 *
 * The three patterns behave thus:
 *
 *  TEST_xx(expr)
 *      If the test fails a canned error message (defined by the macro
 *      ERROR_MESSAGE) is generated and the macro evaluates to False, otherwise
 *      evaluates to True.
 *
 *  TEST_xx_(expr, message...)
 *      If the test fails then the given error message (with sprintf formatting)
 *      is generated and the macro evaluates to False, otherwise True.
 *
 *  ASSERT_xx(expr)
 *      If the test fails then panic_error() is called and execution does not
 *      continue from this point.
 *
 * Note that the _0 macros have the extra side effect of assigning any non-zero
 * expression to errno: these are designed to be used with the pthread functions
 * where this behaviour is appropriate.
 *
 * These macros are designed to be used as chained conjunctions of the form
 *
 *  TEST_xx(...)  &&  TEST_xx(...)  &&  ...
 *
 * To facilitate this three further macros are provided:
 *
 *  DO_(statements)             Performs statements and evaluates to True
 *  IF_(test, iftrue)                   Only checks iftrue if test succeeds
 *  IF_ELSE(test, iftrue, iffalse)      Alternative spelling of (?:)
 */


/* Hint to compiler that x is likely to be 0. */
#define unlikely(x)   __builtin_expect((x), 0)

/* Controls whether to emit log_message() output. */
void verbose_logging(bool verbose);
/* Controls whether to print timestamps on logged message output. */
void timestamp_logging(bool timestamps);

/* Before this is called all messages are sent to stderr, afterwards all are
 * sent to syslog with the given identification mark.*/
void start_logging(const char *ident);

/* Routines to write informative message or error to stderr or syslog. */
void log_message(const char * message, ...)
    __attribute__((format(printf, 1, 2)));
void log_error(const char * message, ...)
    __attribute__((format(printf, 1, 2)));
void vlog_message(int priority, const char *format, va_list args);

/* Functions to support redirection of error messages generated by print_error
 * below.  Once push_error_handling() has been called any subsequent calls to
 * print_error() in the same thread will cause the error message to be stashed.
 * Previous error handling is restored by calling pop_error_handling() which
 * returns the stored error message if return_message is set (in which case the
 * caller is responsible for freeing the error message).  Error handlers can be
 * nested like exception handlers. */
void push_error_handling(void);
char * pop_error_handling(bool return_message);


/* Internal routines called by error handling macros below. */
void print_error(const char * message, ...)
    __attribute__((format(printf, 1, 2)));
void panic_error(const char * filename, int line)
    __attribute__((__noreturn__));


/* Generic TEST macro: computes a boolean from expr using COND (should be a
 * macro), and prints the given error message if the boolean is false.  The
 * boolean result is the value of the entire expression. */
#define TEST_(COND, expr, message...) \
    ( { \
        bool __ok__ = COND(expr); \
        if (unlikely(!__ok__)) \
            print_error(message); \
        __ok__; \
    } )

/* An assert for tests that really really should not fail!  This exits
 * immediately. */
#define ASSERT_(COND, expr)  \
    do { \
        if (unlikely(!COND(expr))) \
            panic_error(__FILE__, __LINE__); \
    } while (0)


/* Default error message for unexpected errors. */
#define ERROR_MESSAGE       "Unexpected error at %s:%d", __FILE__, __LINE__

/* Tests system calls: -1 => error. */
#define _COND_IO(expr)                  ((intptr_t) (expr) != -1)
#define TEST_IO_(expr, message...)      TEST_(_COND_IO, expr, message)
#define TEST_IO(expr)                   TEST_IO_(expr, ERROR_MESSAGE)
#define ASSERT_IO(expr)                 ASSERT_(_COND_IO, expr)

/* Tests pointers: NULL => error. */
#define _COND_NULL(expr)                ((expr) != NULL)
#define TEST_NULL_(expr, message...)    TEST_(_COND_NULL, expr, message)
#define TEST_NULL(expr)                 TEST_NULL_(expr, ERROR_MESSAGE)
#define ASSERT_NULL(expr)               ASSERT_(_COND_NULL, expr)

/* Tests an ordinary boolean: false => error. */
#define _COND_OK(expr)                  ((bool) (expr))
#define TEST_OK_(expr, message...)      TEST_(_COND_OK, expr, message)
#define TEST_OK(expr)                   TEST_OK_(expr, ERROR_MESSAGE)
#define ASSERT_OK(expr)                 ASSERT_(_COND_OK, expr)

/* Tests the return from a pthread_ call: a non zero return is the error
 * code!  We just assign this to errno. */
#define _COND_0(expr) \
    ( { \
        int __rc__ = (expr); \
        if (unlikely(__rc__ != 0)) \
            errno = __rc__; \
        __rc__ == 0; \
    } )
#define TEST_0_(expr, message...)       TEST_(_COND_0, expr, message)
#define TEST_0(expr)                    TEST_0_(expr, ERROR_MESSAGE)
#define ASSERT_0(expr)                  ASSERT_(_COND_0, expr)

/* For failing immediately.  Same as TEST_OK_(false, message...) */
#define FAIL_(message...)  ( { print_error(message); false; } )


/* These two macros facilitate using the macros above by creating if
 * expressions that are slightly more sensible looking than ?: in context. */
#define DO_(action)                     ({action; true;})
#define IF_(test, iftrue)               ((test) ? (iftrue) : true)
#define IF_ELSE(test, iftrue, iffalse)  ((test) ? (iftrue) : (iffalse))

/* Used to ensure that the finally action always occurs, even if action fails.
 * Returns combined success of both actions. */
#define FINALLY(action, finally) \
    ( { \
        bool __oK__ = (action); \
        (finally)  &&  __oK__; \
    } )

/* If action fails perform on_fail as a cleanup action.  Returns status of
 * action. */
#define UNLESS(action, on_fail) \
    ( { \
        bool __oK__ = (action); \
        if (!__oK__) { on_fail; } \
        __oK__; \
    } )


/* Testing read and write happens often enough to be annoying, so some
 * special case macros here. */
#define _COND_rw(rw, fd, buf, count) \
    (rw(fd, buf, count) == (ssize_t) (count))
#define TEST_read(fd, buf, count)   TEST_OK(_COND_rw(read, fd, buf, count))
#define TEST_write(fd, buf, count)  TEST_OK(_COND_rw(write, fd, buf, count))
#define TEST_read_(fd, buf, count, message...) \
    TEST_OK_(_COND_rw(read, fd, buf, count), message)
#define TEST_write_(fd, buf, count, message...) \
    TEST_OK_(_COND_rw(write, fd, buf, count), message)
#define ASSERT_read(fd, buf, count)  ASSERT_OK(_COND_rw(read, fd, buf, count))
#define ASSERT_write(fd, buf, count) ASSERT_OK(_COND_rw(write, fd, buf, count))


/* A tricksy compile time bug checking macro modified from the kernel. */
#define COMPILE_ASSERT(e)           ((void) sizeof(struct { int:-!(e); }))

/* A rather randomly placed helper routine.  This and its equivalents are
 * defined all over the place, but there doesn't appear to be a definitive
 * definition anywhere. */
#define ARRAY_SIZE(a)   (sizeof(a)/sizeof((a)[0]))


/* For ignoring return values even when warn_unused_result is in force. */
#define IGNORE(e)       do if(e) {} while (0)


/* Debug utility for dumping binary data in ASCII format. */
void dump_binary(FILE *out, const void *buffer, size_t length);
