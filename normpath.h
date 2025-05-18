
extern ssize_t logical_normpath(int dirfd, const char *existing, const char *soft, int want_absolute, char *dst, size_t dst_size);
extern ssize_t physical_normpath(int dirfd, const char *existing, const char *soft, int want_absolute, char *dst, size_t dst_size);

