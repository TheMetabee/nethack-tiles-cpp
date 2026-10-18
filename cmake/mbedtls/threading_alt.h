/* The mutex type for MBEDTLS_THREADING_ALT.
 *
 * libssh's winlocks.c makes each lock with `int mutex_init(void **priv)`,
 * which allocates a CRITICAL_SECTION and stores its address. mbedTLS passes
 * a pointer to its mutex, so the mutex must be that one pointer. */
#ifndef NH_MBEDTLS_THREADING_ALT_H
#define NH_MBEDTLS_THREADING_ALT_H
typedef void *mbedtls_threading_mutex_t;
#endif
