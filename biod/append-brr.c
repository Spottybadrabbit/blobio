/*
 *  Synopsis:
 *  	Atomic append of a blob request record to a fifo or file.
 *  Usage:
 *  	append-brr							\
 *		/path/to/file
 *  		start_request_time					\
 *		netflow							\
 *		verb							\
 *		algorithm:digest					\
 *		chat_history						\
 *		blob_size						\
 *		wall_duration						\
 *  Exit Status:
 *	0	ok
 *	1	wrong number of arguments
 *	2	a brr field is too big in the argument list
 *	3	the open() failed
 *	4	the write() failed
 *	5	the close() failed
 *  Note:
 *  	No syntax checking is done on the fields of the blob request record.
 *  	Only field sizes are checked.
 *
 *	O_CREAT must be an option on the file open, otherwise rolling the file
 *	is problemmatic.  For example, without O_CREAT, the following
 *	trivial roll will fail for each append()
 *
 *		mv pdf.log pdf-$(date +'%a').log
 *		#  window when an error in append-brr could occur
 *
 *	since pdf.log disappears briefly.
 *
 *	Consider replacing write() with writev().
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define EXIT_BAD_ARGC	1
#define EXIT_BAD_BRR	2
#define EXIT_BAD_OPEN	3
#define EXIT_BAD_WRITE	4
#define EXIT_BAD_CLOSE	5

static char	progname[] = "append-brr";

#define MAX_BRR		365

/*
 *  Synopsis:
 *	Common, interruptable system calls, die[123]() and _strcat().
 *  Usage:
 *	char *progname = "append-brr";
 *
 *	#define COMMON_NEED_READ
 *	#define COMMON_NEED_DIE3
 *	#include "../../common.c"
 *
 *	#define COMMON_NEED_READ
 *	#define EXIT_BAD_READ 4
 *	#include "../../common.c"
 *  See:
 *	flip-tail and c programs in schema directories.
 *  Note:
 *	Rename common.c to unistd.c?
 */

#ifndef PIPE_MAX
#define PIPE_MAX	512
#endif

/*
 * Synopsis:
 *  	Fast, safe and simple string concatenator
 *  Usage:
 *  	buf[0] = 0
 *  	_strcat(buf, sizeof buf, "hello, world");
 *  	_strcat(buf, sizeof buf, ": ");
 *  	_strcat(buf, sizeof buf, "good bye, cruel world");
 */
static void
_strcat(char *tgt, int tgtsize, char *src)
{
	//  find null terminated end of target buffer
	while (*tgt++)
		--tgtsize;
	--tgt;

	//  copy non-null src bytes, leaving room for trailing null
	while (--tgtsize > 0 && *src)
		*tgt++ = *src++;

	// target always null terminated
	*tgt = 0;
}

/*
 *  Write error message to standard error and exit process with status code.
 */
static void
die(int status, char *msg1)
{
	char msg[PIPE_MAX];
	static char ERROR[] = "ERROR: ";
	static char	colon[] = ": ";
	static char nl[] = "\n";

	msg[0] = 0;
	_strcat(msg, sizeof msg, progname);
	_strcat(msg, sizeof msg, colon);
	_strcat(msg, sizeof msg, ERROR);
	_strcat(msg, sizeof msg, msg1);
	_strcat(msg, sizeof msg, nl);

	write(2, msg, strlen(msg));

	_exit(status);
}

static void
die2(int status, char *msg1, char *msg2)
{
	static char colon[] = ": ";
	char msg[PIPE_MAX];

	msg[0] = 0;
	_strcat(msg, sizeof msg, msg1);
	_strcat(msg, sizeof msg, colon);
	_strcat(msg, sizeof msg, msg2);

	die(status, msg);
}

static void
die3(int status, char *msg1, char *msg2, char *msg3)
{
	static char colon[] = ": ";
	char msg[PIPE_MAX];

	msg[0] = 0;
	_strcat(msg, sizeof msg, msg1);
	_strcat(msg, sizeof msg, colon);
	_strcat(msg, sizeof msg, msg2);
	_strcat(msg, sizeof msg, colon);
	_strcat(msg, sizeof msg, msg3);

	die(status, msg);
}

/*
 *  write() exactly nbytes bytes, restarting on interrupt and dieing on error.
 */
static void
_write(int fd, void *p, ssize_t nbytes)
{
	int nb = 0;

	again:

	nb = write(fd, p + nb, nbytes);
	if (nb < 0) {
		if (errno == EINTR)
			goto again;
		die2(EXIT_BAD_WRITE, "write() failed", strerror(errno));
	}
	nbytes -= nb;
	if (nbytes > 0)
		goto again;
}

/*
 *  open() a file.
 */
static int
_open(char *path, int oflag, int mode)
{
	int fd;

	again:

	fd = open(path, oflag, mode);
	if (fd < 0) {
		if (errno == EINTR)
			goto again;
		die3(EXIT_BAD_OPEN, "open() failed", strerror(errno), path);
	}
	return fd;
}

/*
 *  close() a file descriptor.
 */
static void
_close(int fd)
{
	again:

	if (close(fd) < 0) {
		if (errno == EINTR)
			goto again;
		die2(EXIT_BAD_CLOSE, "close() failed", strerror(errno));
	}
}

/*
 *  Build the brr record from a command line argument.
 *  Update the pointer to the position in the buffer.
 *
 *  Note:
 *	Need to be more strict about correctness of brr fields.
 *	For example, a char set per arg would be easy to add.
 */
static void
arg2brr(char *name, char *arg, int size, char **brr)
{
	char *tgt, *src;
	int n;

	n = 0;
	src = arg;
	tgt = *brr;
	while (*src && n++ < size)
		*tgt++ = *src++;
	if (n == size && *src)
		die2(EXIT_BAD_BRR, "arg too big for brr field", name); 
	*tgt++ = '\t';
	*brr = tgt;
}

int
main(int argc, char **argv)
{
	// room for brr + newline + null
	char brr[MAX_BRR + 1 + 1], *b;
	char *path;
	int fd;

	if (argc != 9)
		die(EXIT_BAD_ARGC, "wrong number arguments");
	path = argv[1];

	/*
	 *  Build the blob request record from command line arguments
	 */
	b = brr;
	arg2brr("start request time", argv[2], 10+1+8+1+9+1+6, &b);
	arg2brr("netflow", argv[3], 8+1+128, &b);
	arg2brr("verb", argv[4], 8, &b);
	arg2brr("udig", argv[5], 8+1+128, &b);
	arg2brr("chat history", argv[6], 8, &b);
	arg2brr("blob size", argv[7], 20, &b);
	arg2brr("wall duration", argv[8], 10+1+9, &b);

	b[-1] = '\n';		//  zap trailing tab

	fd = _open(path,
		   O_WRONLY | O_APPEND | O_CREAT,
		   S_IRUSR | S_IWUSR | S_IRGRP
	);

	// atomically write exactly the number bytes in the blob request record
	_write(fd, brr, b - brr);
	_close(fd);
	_exit(0);
}
