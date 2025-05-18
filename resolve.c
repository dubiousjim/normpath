#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>

#include <assert.h>
#include <stdio.h>

#define SYMLOOP_MAX 40

#if defined(__APPLE__) && defined(__MACH__)
static char *strchrnul(const char *s, int c) {
    c = (unsigned char)c;
    if (!c) return (char *)s + strlen(s);
    for (; *s && *(unsigned char *)s != c; s++);
    return (char *)s;
}
#endif

extern ssize_t getdirpath(int dirfd, char *dst, size_t dst_size);
extern ssize_t normal(const char *src, int force_slash, char *dst, size_t dst_size);

static size_t slash_len(const char *s) {
    const char *s0 = s;
    while (*s == '/') s++;
    return (size_t)(s - s0);
}

/* based on musl's implemtation of realpath */

ssize_t resolve(int dirfd, const char *restrict src, int force_slash, int *can_soft, int want_absolute, char *restrict dst, size_t q, size_t dst_size) {
    char stack[PATH_MAX + 1];
    size_t p, len, len0, symlink_cnt = 0, nup = 0;
    int check_dir = 0;

    if (!src) {
        errno = EINVAL;
        return -1;
    }
    len = strnlen(src, sizeof(stack));
    if (!len) {
        errno = ENOENT;
        return -1;
    }
    if (q + len >= dst_size) goto toolong;
    p = sizeof(stack) - len - 1;
    memcpy(stack + p, src, len + 1);

    /* Main loop. Each iteration pops the next part from stack of
     * remaining path components and consumes any slashes that follow.
     * If not a link, it's moved to dst; if a link, contents are
     * pushed to the stack. */
restart:
    for (; ; p += slash_len(stack + p)) {
        /* If stack starts with /, the whole component is /
         * and the output state must be reset. */
        if (stack[p] == '/') {
            check_dir = 0;
            nup = 0;
            q = 0;
            dst[q++] = '/';
            p++;
            continue;
        }

        char *z = strchrnul(stack + p, '/');
        len0 = len = (size_t)(z - (stack + p));

        if (!len && !check_dir) break;

        /* Skip any . component but preserve check_dir status. */
        if (len == 1 && stack[p] == '.') {
            p += len;
            continue;
        }

        /* Copy next component onto dst at least temporarily, to
         * call readlink, but wait to advance dst position until
         * determining it's not a link. */
        if (q && dst[q - 1] != '/') {
            if (!p) goto toolong;
            stack[--p] = '/';
            len++;
        }
        if (q + len >= dst_size) goto toolong;
        memcpy(dst + q, stack + p, len);
        dst[q + len] = 0;

        p += len;
        assert(stack + p == z);

        int up = 0;
        if (len0 == 2 && stack[p - 2] == '.' && stack[p - 1] == '.') {
            up = 1;
            /* Any non-.. path components we could cancel start
             * after nup repetitions of the 3-byte string "../";
             * if there are none, accumulate .. components to
             * later apply to cwd, if needed. */
            if (q <= 3*nup) {
                nup++;
                q += len;
                continue;
            }
            /* When previous components are already known to be
             * directories, processing .. can skip readlink. */
            if (!check_dir) goto skip_readlink;
        }
        ssize_t k = readlinkat(dirfd, dst, stack, p);
        if (k == (ssize_t)p) goto toolong;
        if (!k) {
            errno = ENOENT;
            return -1;
        }
        if (k < 0) {
            if (can_soft && errno == ENOENT) {
                ssize_t norm_len = normal(stack + p - len, 0, dst + q, dst_size - q);
                if (norm_len < 0) return -1;
                q += (size_t) norm_len;
                assert(q + 1 < dst_size);
                *can_soft = 1;
                break;
            } else if (errno != EINVAL) return -1;
skip_readlink:
            int was_check_dir = check_dir;
            check_dir = 0;
            if (up) {
                assert(q <= 1 || dst[q - 1] != '/');
                while(q && dst[q - 1] != '/') q--;
                if (q > 1 && (q > 2 || dst[0] != '/')) q--;
                continue;
            }
            if (len0) {
                q += len;
            } else {
                assert(len == 1);
                assert(was_check_dir);
                assert(dst[q] == '/');
            }
            if (stack[p]) {
                check_dir = 1;
            } else if (force_slash) {
                check_dir = 1;
                force_slash = 0;
            }
            continue;
        }
        if (++symlink_cnt == SYMLOOP_MAX) {
            errno = ELOOP;
            return -1;
        }

        /* If link contents end in /, strip any slashes already on
         * stack to avoid /->// or spurious toolong. */
        if (stack[k - 1] == '/') while (stack[p] == '/') p++;
        p -= (size_t)k;
        memmove(stack + p, stack, (size_t)k);

        /* Skip the stack advancement in case we have a new
         * absolute base path. */
        goto restart;
    } /* for */

    dst[q] = 0;

    if (dst[0] != '/' && want_absolute) {
        len = (size_t)getdirpath(dirfd, stack, sizeof(stack));
        if ((ssize_t)len < 0) return -1;
        /* Cancel any initial .. components. */
        p = 0;
        while (nup--) {
            while(len > 1 && stack[len - 1] != '/') len--;
            if (len > 1) len--;
            p += 2;
            if (p < q) p++;
        }
        if (q - p && stack[len - 1] != '/') stack[len++] = '/';
        if (len + (q - p) + 1 >= dst_size) goto toolong;
        memmove(dst + len, dst + p, q - p + 1);
        memcpy(dst, stack, len);
        q = len + q - p;
    }

    return (ssize_t)q;

toolong:
    errno = ENAMETOOLONG;
    return -1;
}
