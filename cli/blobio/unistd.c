/*
 *  Synopsis:
 *	Wrapper layer around common posix/unixish routines.
 */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/*
 *  Interruptable close() of a file descriptor.
 *  Caller can depend on correct value of errno.
 */
int
uni_close(int fd)
{
again:
	if (close(fd) == 0)
		return 0;
	if (errno == EINTR || errno == EAGAIN)
		goto again;
	return -1;
}

/*
 *  Interruptable write() of a file descriptor.
 *  Caller can depend on correct value of errno.
 */
ssize_t
uni_write(int fd, const void *buf, size_t count)
{
	ssize_t nwrite;
again:
	nwrite = write(fd, buf, count);
	if (nwrite >= 0)
		return nwrite;
	if (errno == EINTR || errno == EAGAIN)
		goto again;
	return -1;
}

/*
 *  Write a full buffer or return error.
 *  Caller can depend on correct value of errno.
 */
int
uni_write_buf(int fd, const void *buf, size_t count)
{
	size_t nwrite = 0;

	while (nwrite < count) {

		ssize_t nw;

		nw = uni_write(fd, buf + nwrite, count - nwrite);
		if (nw == -1)
			return -1;
		nwrite += nw;
	}
	return 0;
}

/*
 *  Interruptable read() of a file descriptor.
 *  Caller can depend on correct value of errno.
 */
ssize_t
uni_read(int fd, void *buf, size_t count)
{
	ssize_t nread;
again:
	nread = read(fd, buf, count);
	if (nread >= 0)
		return nread;
	if (errno == EINTR || errno == EAGAIN)
		goto again;
	return -1;
}

int
uni_open(const char *pathname, int flags)
{
	int fd;

again:
	fd = open(pathname, flags);
	if (fd >= 0)
		return fd;
	if (errno == EINTR || errno == EAGAIN)
		goto again;
	return -1;
}

int
uni_open_mode(const char *pathname, int flags, int mode)
{
	int fd;

again:
	fd = open(pathname, flags, mode);
	if (fd >= 0)
		return fd;
	if (errno == EINTR || errno == EAGAIN)
		goto again;
	return -1;
}