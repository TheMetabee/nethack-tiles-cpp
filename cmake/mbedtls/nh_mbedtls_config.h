/* Added to mbedTLS's default configuration through MBEDTLS_USER_CONFIG_FILE.
 *
 * libssh refuses an mbedTLS without threads (src/threads/mbedtls.c). On
 * Windows there are no pthreads, so mbedTLS takes its locks from libssh at
 * run time: libssh's winlocks.c gives them, and threading_alt.h beside this
 * file says what a lock is. */
#define MBEDTLS_THREADING_C
#if defined(_WIN32)
#define MBEDTLS_THREADING_ALT
#else
#define MBEDTLS_THREADING_PTHREAD
#endif
