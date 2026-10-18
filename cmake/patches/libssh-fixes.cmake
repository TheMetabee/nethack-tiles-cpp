# Fixes three defects in libssh 0.12.2 that this build hits on Windows.
#
# Runs as the FetchContent PATCH_COMMAND, in the libssh source directory.
# Each fix changes nothing when it is already there, so a second run is safe.
# When libssh is updated, a fix that no longer matches stops the configure
# with a message: check whether the new version has it, and drop it here.

# Replaces `broken` with `fixed` in `file`, once.
function(fix name file broken fixed)
  file(READ ${file} text)
  string(FIND "${text}" "${fixed}" already)
  if(already GREATER -1)
    return()
  endif()
  string(FIND "${text}" "${broken}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "libssh fix '${name}': the code to change is not in ${file}.")
  endif()
  string(REPLACE "${broken}" "${fixed}" text "${text}")
  file(WRITE ${file} "${text}")
endfunction()

# 1. A missing return.
#
# With MBEDTLS_THREADING_ALT, which this build uses on Windows (see
# cmake/mbedtls/nh_mbedtls_config.h), crypto_thread_init() sets the locks and
# then falls off the end of the function. ssh_init() returns that value, so
# the library's start-up result is whatever the return register held. MSVC
# reports it as C4715.
fix(threading-alt-return src/threads/mbedtls.c
  "                                  user_callbacks->mutex_unlock);\n    }\n#elif defined MBEDTLS_THREADING_PTHREAD"
  "                                  user_callbacks->mutex_unlock);\n    }\n    return SSH_OK;\n#elif defined MBEDTLS_THREADING_PTHREAD")

# 2. A refused connection waits for the whole timeout on Windows.
#
# Winsock reports a failed non-blocking connect() only in select()'s
# exceptfds. libssh's poll emulation puts a socket in exceptfds only when
# urgent data is asked for, so "connection refused" never wakes it, and the
# connect fails 20 seconds later as a timeout.
#
# The fix watches exceptfds as well while a socket waits to become writable,
# which is how libssh waits for a connect, and reports POLLERR. libssh then
# reads SO_ERROR for the real reason. The fix must not read SO_ERROR itself:
# on Winsock that read clears it. SSH never sends TCP urgent data, so on
# these sockets exceptfds means a failed connect.
fix(windows-connect-error-set src/poll.c
  "        if (fds[i].events & (POLLPRI | POLLRDBAND)) {\n            FD_SET(fds[i].fd, &exceptfds);\n        }\n"
  "        if (fds[i].events & (POLLPRI | POLLRDBAND)) {\n            FD_SET(fds[i].fd, &exceptfds);\n        }\n#ifdef _WIN32\n        /* Winsock reports a failed connect() only in exceptfds. */\n        if (fds[i].events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {\n            FD_SET(fds[i].fd, &exceptfds);\n        }\n#endif\n")
fix(windows-connect-error-report src/poll.c
  "            if (FD_ISSET(fds[i].fd, &exceptfds)) {\n                fds[i].revents |= fds[i].events & (POLLPRI | POLLRDBAND);\n            }\n"
  "            if (FD_ISSET(fds[i].fd, &exceptfds)) {\n                fds[i].revents |= fds[i].events & (POLLPRI | POLLRDBAND);\n#ifdef _WIN32\n                if (fds[i].events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {\n                    fds[i].revents |= POLLERR;\n                }\n#endif\n            }\n")

# 3. Every socket error reads "Unknown error" on Windows.
#
# ssh_strerror() uses strerror_s(), which knows only the C library's errno
# values. Winsock errors are numbers from WSABASEERR (10000) up, so a refused
# connection reads "could not reach host:22: Unknown error". The fix asks
# the system for the text of those numbers.
fix(windows-socket-error-text src/misc.c
  "#if defined(_WIN32)\n    rv = strerror_s(buf, buflen, err_num);\n#else"
  "#if defined(_WIN32)\n    if (err_num >= WSABASEERR && buflen > 0 &&\n        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,\n                       NULL, (DWORD)err_num, 0, buf, (DWORD)buflen, NULL) > 0) {\n        /* The system text ends with a full stop and CR LF (13, 10). */\n        size_t end = strlen(buf);\n        while (end > 0 && (buf[end - 1] == 13 || buf[end - 1] == 10 ||\n                           buf[end - 1] == ' ' || buf[end - 1] == '.')) {\n            buf[--end] = 0;\n        }\n        return buf;\n    }\n    rv = strerror_s(buf, buflen, err_num);\n#else")
