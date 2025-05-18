// #define _GNU_SOURCE
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <assert.h>

#include <stdio.h>

// when enabled, skips all validation of 'existing' when 'soft' starts with '/'
#define ABSOLUTE_SOFT_SHORT_CIRCUITS 0

extern ssize_t getdirpath(int dirfd, char *dst, size_t dst_size);
extern ssize_t resolve(int dirfd, const char *restrict src, int force_slash, int *can_soft, int want_absolute, char *restrict dst, size_t cursor, size_t dst_size);


/*
 * expects src ends with \0
 * accepts ""
 * does not write \0 to dst, but fails if there's no space for it
 * returns # bytes written >= 0
 */
extern ssize_t normal(const char *src, int force_slash, char *dst, size_t dst_size) {
    assert(src && dst && dst_size > 0 && dst_size <= SSIZE_MAX);
    // assert(dst_size <= PATH_MAX);

    const char *s = src;
    char *d = dst;
    size_t dst_left = dst_size;
    int first = 1;
    int trailing_slash = 0;

    // handle leading slashes (only one copied)
    while (*s == '/') s++;
    if (src[0] == '/') {
        if (1 >= dst_left) goto toolong;
        *d++ = '/';
        dst_left--;
    }

    while (*s) {
        // skip ./ segments
        if (s[0] == '.' && (s[1] == '/' || s[1] == '\0')) {
            s += (s[1] == '\0' ? 1 : 2);
        } else {
            // write '/' before components after the first
            if (!first) {
                if (1 >= dst_left) goto toolong;
                *d++ = '/';
                dst_left--;
            }
            first = 0;
            // scan component
            const char *comp_start = s;
            while (*s && *s != '/') s++;
            size_t comp_len = (size_t)(s - comp_start);
            assert(comp_len);
            trailing_slash = 0;

            if (comp_len >= dst_left) goto toolong;
            memcpy(d, comp_start, comp_len);
            d += comp_len;
            dst_left -= comp_len;
        }
        // skip any slashes
        if (*s == '/') {
            trailing_slash = 1;
            do { s++; } while (*s == '/');
        }
    }

    // handle final slash
    // cannot assume that trailing_slash implies !first, as in ".//"
    if (force_slash || (trailing_slash && !first)) {
        if (dst_left < dst_size) {
            if (*(d - 1) != '/') {
                if (1 >= dst_left) goto toolong;
                *d++ = '/';
                dst_left--;
            } else {
                assert(dst_size - dst_left == 1);
            }
        }
    }

    assert(dst_left >= 1);
    return (ssize_t)(dst_size - dst_left);

toolong:
    errno = ENAMETOOLONG; return -1;
}

/*
 * logical_normpath contract for existing path validation:
 * - Path existing must be lstat-able, i.e., it must name a valid filesystem object.
 * - The object may be a broken symlink.
 * - However, if 'existing' ends in a '/' (or if soft is non-NULL), we treat a final symlink in existing as "must refer to a directory."
 *   In this case, we resolve that final component of existing with stat.
 *   If that stat fails or points to a non-directory, we fail.
 */

ssize_t logical_normpath(int dirfd, const char *existing, const char *soft, int want_absolute, char *dst, size_t dst_size) {
    if (!dst || dst_size == 0 || dst_size > PATH_MAX || dst_size > SSIZE_MAX) { errno = EINVAL; return -1; }
    if (!existing && !soft) { errno = EINVAL; return -1; }
    // TODO consider whether next two checks can be relaxed
    if ((existing && existing[0] == '\0') || (soft && soft[0] == '\0')) { errno = EINVAL; return -1; }
    size_t cursor = 0;
    int soft_absolute = soft && soft[0] == '/';
    if (existing) {
#if ABSOLUTE_SOFT_SHORT_CIRCUITS
        if (!soft_absolute)
#endif
        {
            int force_slash = 0;
            size_t existing_len = strlen(existing);
            assert(existing_len > 0);
            struct stat existing_stat;
            if (fstatat(dirfd, existing, &existing_stat, AT_SYMLINK_NOFOLLOW) != 0) {
                // "link_loop/." or "link_dangling/[.]"
                return -1;
            }
            if (S_ISLNK(existing_stat.st_mode)) {
                if (fstatat(dirfd, existing, &existing_stat, 0) == 0) {
                    force_slash = S_ISDIR(existing_stat.st_mode);
                } else if (soft) {
                    // "link_loop" "." or "link_dangling" "."
                    // also "link_loop/" "." on Darwin
                    return -1;
                } else {
                    // else force_slash remains 0
#if defined(__APPLE__) && defined(__MACH__)
                    if (existing[existing_len - 1] == '/') {
                        // "link_loop/" on Darwin
                        assert(errno == ELOOP);
                        return -1;
                    } else
#endif
                    {
                        // dangling or looping symlink, but not trying to resolve it
                    }
                }
            } else {
                force_slash = S_ISDIR(existing_stat.st_mode);
#if defined(__APPLE__) && defined(__MACH__)
                if (!force_slash && existing[existing_len - 1] == '/') {
                    // "link_file/" on Darwin
                    errno = ENOTDIR; return -1;
                }
#endif
            }
            assert(existing[existing_len - 1] != '/' || force_slash == 1);
            if (soft && !force_slash) { errno = ENOTDIR; return -1; }
            if (!soft_absolute) {
                if (existing[0] != '/') {
                    if (want_absolute) {
                        ssize_t path_len = getdirpath(dirfd, dst, dst_size);
                        if (path_len < 0) return -1;
                        cursor = (size_t)path_len;
                        assert(0 < cursor && cursor < dst_size);
                        if (!(cursor == 1 && dst[0] == '/')) {
                            if (cursor + 1 >= dst_size) goto toolong;
                            dst[cursor++] = '/';
                        }
                    } else if (existing[0] == '-' || (existing[0] == '.' && existing[1] == '/')) {
                        if (2 >= dst_size) goto toolong;
                        dst[cursor++] = '.';
                        dst[cursor++] = '/';
                    }
                }
                ssize_t normal_len = normal(existing, force_slash, &dst[cursor], dst_size - cursor);
                if (normal_len < 0) return -1;
                cursor += (size_t) normal_len;
            }
        }
    } /* if (existing) */
    else if (soft[0] != '/') {
        if (want_absolute) {
            ssize_t path_len = getdirpath(dirfd, dst, dst_size);
            if (path_len < 0) return -1;
            cursor = (size_t)path_len;
            assert(0 < cursor && cursor < dst_size);
            if (!(cursor == 1 && dst[0] == '/')) {
                if (cursor + 1 >= dst_size) goto toolong;
                dst[cursor++] = '/';
            }
        }
    }

    if (!soft) {
        dst[cursor] = '\0';
    } else {
        if (cursor == 0) {
            if (soft[0] == '-' || (soft[0] == '.' && soft[1] == '/')) {
                if (2 >= dst_size) goto toolong;
                dst[cursor++] = '.';
                dst[cursor++] = '/';
            }
        }

        char *check = dst + cursor; // &dst[cursor]
        ssize_t soft_len = normal(soft, 0, check, dst_size - cursor);
        if (soft_len < 0) return -1;
        cursor += (size_t)soft_len;
        char *end = dst + cursor;
        *end = '\0';

        // validate all intermediate soft components used as directories
        int checking = 1;
        if (*check == '/') {
            assert(soft_absolute);
            check++;
        }
        while (checking) {
            while (check < end && *check != '/') check++;
            // skip checking final components that don't end with /
            if (check == end) break;
            check++;
            char sep = *check;
            *check = '\0';
            struct stat check_stat;
            if (fstatat(dirfd, dst, &check_stat, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno != ENOENT) return -1;
                checking = 0;
            }
#if defined(__APPLE__) && defined(__MACH__)
            else if (S_ISLNK(check_stat.st_mode)) {
                if (fstatat(dirfd, dst, &check_stat, 0) != 0 || !S_ISDIR(check_stat.st_mode)) {
                    // soft == "link_loop/" on Darwin
                    assert(errno == ELOOP);
                    return -1;
                }
            } else if (!S_ISDIR(check_stat.st_mode)) {
                // soft == "link_file/" at least on Darwin
                assert(errno == 0);
                errno = ENOTDIR; return -1;
            }
#endif
            *check = sep;
        } // while (checking)
    } // if (soft)

    // we include case where dst[2] == '\0'
    if (cursor >= 2 && dst[0] == '.' && dst[1] == '/' && dst[2] != '-') {
        memmove(dst, &dst[2], cursor - 1);
        cursor -= 2;
    }
    if (cursor == 0) {
        if (1 >= dst_size) goto toolong;
        dst[cursor++] = '.'; // TODO we render (any trailing) .. as ../, should we render (bare) . the same?
        dst[cursor] = '\0';
    }
    assert(cursor < dst_size);
    assert(dst[cursor] == '\0');
    return (ssize_t)cursor;

toolong:
    errno = ENAMETOOLONG; return -1;
}


ssize_t physical_normpath(int dirfd, const char *existing, const char *soft, int want_absolute, char *dst, size_t dst_size) {
    if (!dst || dst_size == 0 || dst_size > PATH_MAX || dst_size > SSIZE_MAX) { errno = EINVAL; return -1; }
    if (!existing && !soft) { errno = EINVAL; return -1; }
    // TODO consider whether next two checks can be relaxed
    if ((existing && existing[0] == '\0') || (soft && soft[0] == '\0')) { errno = EINVAL; return -1; }
    size_t cursor = 0;
    int force_slash = 0;
    int soft_absolute = soft && soft[0] == '/';
    if (existing) {
#if ABSOLUTE_SOFT_SHORT_CIRCUITS
        if (!soft_absolute)
#endif
        {
            size_t existing_len = strlen(existing);
            assert(existing_len > 0);
            struct stat existing_stat;
            if (fstatat(dirfd, existing, &existing_stat, 0) != 0) {
                return -1;
            }
            assert(!S_ISLNK(existing_stat.st_mode));
            force_slash = S_ISDIR(existing_stat.st_mode);
#if defined(__APPLE__) && defined(__MACH__)
            if (!force_slash && existing[existing_len - 1] == '/') {
                // "link_file/" on Darwin
                errno = ENOTDIR; return -1;
            }
#endif
            assert(existing[existing_len - 1] != '/' || force_slash == 1);
            if (soft && !force_slash) { errno = ENOTDIR; return -1; }
            if (!soft_absolute) {
                ssize_t resolve_len = resolve(dirfd, existing, force_slash, 0, want_absolute, dst, cursor, dst_size);
                if (resolve_len < 0) return -1;
                want_absolute = 0;
                cursor += (size_t) resolve_len;
            }
        }
    } /* if (existing) */

    if (!soft) {
        if (cursor <= 1) {
            force_slash = 0;
        } else {
            assert(dst[cursor - 1] != '/');
        }
    } else {
        int did_soft = 0;

        // FIXME set force_slash
        size_t soft_len = strlen(soft);
        assert(soft_len > 0);
        if (soft[soft_len - 1] == '/' || (soft_len >= 2 && soft[soft_len - 2] == '/' && soft[soft_len - 1] == '.') || (soft_len >= 3 && soft[soft_len - 3] == '/' && soft[soft_len - 2] == '.' && soft[soft_len - 1] == '.'))
            force_slash = 1;

        ssize_t resolve_len = resolve(dirfd, soft, force_slash, &did_soft, want_absolute, dst, cursor, dst_size);
        if (resolve_len < 0) return -1;
        cursor += (size_t)resolve_len;
        assert(did_soft || cursor <= 1 || dst[cursor - 1] != '/');
        if (did_soft || cursor <= 1) {
            force_slash = 0;
        } else if (!force_slash) {
            struct stat soft_stat;
            if (fstatat(dirfd, dst, &soft_stat, 0) != 0) {
                assert(0);
                return -1;
            }
            assert(!S_ISLNK(soft_stat.st_mode));
            force_slash = S_ISDIR(soft_stat.st_mode);
        }
    } // if (soft)

    if (force_slash) {
        if (cursor + 1 >= dst_size) goto toolong;
        dst[cursor++] = '/';
    }
    dst[cursor] = '\0';

    if (cursor == 0) {
        if (1 >= dst_size) goto toolong;
        dst[cursor++] = '.'; // TODO we render (any trailing) .. as ../, should we render (bare) . the same?
        dst[cursor] = '\0';
    } else if (dst[0] == '-') {
        if (cursor + 2 >= dst_size) goto toolong;
        memmove(dst + 2, dst, cursor + 1);
        dst[0] = '.';
        dst[1] = '/';
        cursor += 2;
    }
    assert(cursor < dst_size);
    assert(dst[cursor] == '\0');
    return (ssize_t)cursor;

toolong:
    errno = ENAMETOOLONG; return -1;
}
