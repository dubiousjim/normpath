// for header
#include <sys/types.h>
#include <stddef.h> // or <unistd.h>


#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>


static ssize_t getpath(int fd, char *dst, size_t dst_size, mode_t *out_mode);

ssize_t getdirpath(int dirfd, char *dst, size_t dst_size) {
    if (dirfd == AT_FDCWD) {
        if (!getcwd(dst, dst_size)) return -1;
        return (ssize_t)strlen(dst);
    }
#ifdef __linux__
    mode_t mode = 0;
    ssize_t path_len = getpath(dirfd, dst, dst_size, &mode);
    if (path_len < 0) return -1;
    if (!S_ISDIR(mode)) { errno = ENOTDIR; return -1; }
    return path_len;
#elif defined(__APPLE__) && defined(__MACH__)
    assert(dst_size >= PATH_MAX);
    struct stat fd_stat;
    if (fstat(dirfd, &fd_stat) != 0) return -1;
    if (!S_ISDIR(fd_stat.st_mode)) { errno = ENOTDIR; return -1; }
    if (fcntl(dirfd, F_GETPATH, dst) == -1) return -1;
    return (ssize_t)strlen(dst);
#else
    errno = ENOTSUP;
    return -1;
#endif
}

/*
 * May fail with:
 *   EINVAL: if dst is NULL or dst_size is 0
 *   EOPNOTSUPP: if getpath is called with a fd from opening a symlink with O_PATH|O_NOFOLLOW
 *   EIO
 *   EBADF
 *   ESTALE: if fd's target is occupied by a different inode
 *   ENOENT: if fd's target moved or was unlinked
 *   EACCES
 *   ELOOP
 *   ENAMETOOLONG
 *   ENOTDIR
 *
 * If the fd is to a symlink opened with O_GETPATH|O_NOFOLLOW,
 * getpath will fail.
 */


static
ssize_t getpath(int fd, char *dst, size_t dst_size, mode_t *out_mode) {
    if (!dst || dst_size == 0) {
        errno = EINVAL;
        return -1;
    }

    struct stat fd_stat;
    if (fstat(fd, &fd_stat) != 0)
        return -1;

    if (S_ISLNK(fd_stat.st_mode)) {
        errno = EOPNOTSUPP; // or EINVAL
        return -1;
    }

    char link[25];
    int n = snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    assert(0 < n && (size_t)n < sizeof(link));

    char res[PATH_MAX];
    if (!realpath(link, res))
        return -1;

    struct stat res_stat;
    if (lstat(res, &res_stat) != 0)
        return -1;

    if (fd_stat.st_dev != res_stat.st_dev || fd_stat.st_ino != res_stat.st_ino) {
        errno = ESTALE;
        return -1;
    }

    if (out_mode)
        *out_mode = res_stat.st_mode;

    size_t res_len = strlen(res);
    if (res_len >= dst_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(dst, res, res_len + 1);
    // returns the (aspirational and here, actual) strlen, like snprintf, strlcpy, strlcat
    // unlike read, write, which include any terminal NUL
    return (ssize_t)res_len;
}
