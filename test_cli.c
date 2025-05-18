#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>

#include "normpath.h"

int parse_int(const char *optarg) {
    char *endptr;
    errno = 0;
    long val = strtol(optarg, &endptr, 10);
    if (errno != 0 || *endptr != '\0') {
        fprintf(stderr, "Invalid number: '%s'\n", optarg);
        exit(EXIT_FAILURE);
    }
    if (val < INT_MIN || val > INT_MAX) {
        fprintf(stderr, "Out of int range: '%s'\n", optarg);
        exit(EXIT_FAILURE);
    }
    return (int)val;
}

int main(int argc, char *argv[]) {
    int want_absolute = 0;
    int logical = 0;
    const char *existing = NULL;
    const char *soft = NULL;
    int dirfd = AT_FDCWD;

    int opt;
    while ((opt = getopt(argc, argv, "lae:d:")) != -1) {
        switch (opt) {
            case 'l':
                logical = 1;
                break;
            case 'a':
                want_absolute = 1;
                break;
            case 'e':
                existing = optarg;
                break;
            case 'd':
                dirfd = parse_int(optarg);
                break;
            default:
                fprintf(stderr, "Usage: %s [-l] [-a] [-e existing] [soft...]\n", argv[0]);
                return 1;
        }
    }

    if (optind < argc) {
        size_t len = 0;
        for (int i = optind; i < argc; ++i) {
            len += strlen(argv[i]) + 1;
        }
        char *soft_buf = malloc(len);
        if (!soft_buf) {
            perror("malloc");
            return 1;
        }
        soft_buf[0] = '\0';
        for (int i = optind; i < argc; ++i) {
            strcat(soft_buf, argv[i]);
            if (i < argc - 1) strcat(soft_buf, " ");
        }
        soft = soft_buf;
    }

    char dst[PATH_MAX];
    ssize_t result;
    if (logical) {
        result = logical_normpath(dirfd, existing, soft, want_absolute, dst, sizeof(dst));
    } else {
        result = physical_normpath(dirfd, existing, soft, want_absolute, dst, sizeof(dst));
    }

    printf("Arguments: %s-e \"%s\" \"%s\"  => ", (want_absolute ? (logical ? "-la " : "-a ") : (logical ? "-l " : "")), existing, soft);
    if (result < 0) {
        printf("%s (%d)\n", strerror(errno), errno);
        return (int)result;
    }

    printf("Result: \"%s\" (%zd)\n", dst, result);
    return 0;
}
