//
// Scatter/gather utility routines
//
#ifndef IOVEC_H
#define IOVEC_H

int check_iovec(struct iovec *iov, int iovlen, int modify);

size_t get_iovec_size(struct iovec *iov, int iovlen);

struct iovec *dup_iovec(struct iovec *iov, int iovlen);

int read_iovec(struct iovec *iov, int iovlen, char *buf, size_t count);

int write_iovec(struct iovec *iov, int iovlen, char *buf, size_t count);

#endif
