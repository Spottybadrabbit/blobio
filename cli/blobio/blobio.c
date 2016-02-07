/*
 *  Synopsis:
 *	Client to get/put/give/take/eat/wrap/roll blobs via a blobio service.
 *  Exit Status:
 *  	0	request succeed - ok
 *  	1	request denied. blob may not exist or is not empty -
 *  	2	
 *	2	missing or invalid command line argument
 *	16	unexpected hash digest error
 *	17	unexpected blobio service error
 *	18	unexpected error on unix system call
 *
 *	Under OSX 10.9, an exit status 141 in various shells can indicate a
 *	SIGPIPE interupted the execution.  blobio does not exit 141.
 *  Options:
 *	--service name:end_point
 *	--udig algorithm:digest
 *	--algorithm name
 *	--input-path <path/to/file>
 *	--output-path <path/to/file>
 *  Note:
 *	A shared library service might be interesting.
 *
 *	Add the exit status codes to the usage output (dork).
 *
 *	Also, the take&give exit statuses ought to reflect the various ok/no
 *	chat histories or perhaps the exit status ought to also store the
 *	verb.  See exit status for child process requests in the blobio
 *	server.
 *
 *	--output-path /dev/null fails, which is problematic.
 *
 *	Would be nice to eliminate stdio dependencies.  Currently stdio is
 *	required by only trace.c.
 *
 *	--algorithm <name> needs a way to prepend the algorithm to the written
 *	digest, so that a full udig is written instead of the just the digest.
 *	perhaps adding a plus to the end of algorithm name.
 *
 *		--algorithm sha+
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "blobio.h"

#define EXIT_OK		0	//  request satisfied
#define EXIT_NO		1	//  request not satisfied
#define EXIT_BAD_ARG	2	//  missing or invalid command line argument
#define EXIT_BAD_DIG	16	//  unexpetced error in digest
#define EXIT_BAD_SER	17	//  unexpected error in blob service
#define EXIT_BAD_UNI	18	//  unexpected error in unix system call

static char	*progname = "blobio";

extern int	tracing;

/*
 *  The global request.
 */
char	*verb;
char	algorithm[9] = {};
char	digest[129] = {};
char	*output_path = 0;
char	*input_path = 0;
char	end_point[129] = {};

int	input_fd = 0;
int	output_fd = 1;

extern struct digest		*digests[];
extern struct service		*services[];
extern int			h_errno;
extern int			errno;

static struct service		*service = 0;

static char		usage[] =
   "usage: blobio [help | get|put|give|take|eat|wrap|roll|empty] [options]\n";


/*
 * Synopsis:
 *  	Fast, safe and simple string concatenator
 *  Usage:
 *  	buf[0] = 0
 *  	bufcat(buf, sizeof buf, "hello, world");
 *  	bufcat(buf, sizeof buf, ": ");
 *  	bufcat(buf, sizeof buf, "good bye, cruel world");
 */
void
bufcat(char *tgt, int tgtsize, const char *src)
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

void
ecat(char *buf, int size, char *msg)
{
	bufcat(buf, size, progname);
	if (verb) {
		bufcat(buf, size, ": ");
		bufcat(buf, size, verb);
	}
	if (service) {
		bufcat(buf, size, ": ");
		bufcat(buf, size, service->name);
	}
	bufcat(buf, size, ": ERROR: ");
	bufcat(buf, size, msg);
}

static void
leave(int status)
{
	if (service) {
		service->close();
		service = (struct service *)0;
	}

	uni_close(input_fd);

	//  Note: what about closing the digest?

	//  unlink() file created by --output-path, grumbling if unlink() fails.
	if (status && output_path != NULL && unlink(output_path))
		if (errno != ENOENT) {
			static char panic[] =
				"PANIC: unlink(output_path) failed: ";
			char buf[PIPE_MAX];
			char *err;

			err = strerror(errno);

			//  assemble error message about failed unlink()

			buf[0] = 0;
			ecat(buf, sizeof buf, panic);
			bufcat(buf, sizeof buf, err);
			bufcat(buf, sizeof buf, "\n");

			uni_write(2, buf, strlen(buf));
		}
	_exit(status);
}

static void
help()
{
	static char blurb[] = "Usage:\n\
	blobio [get|put|give|take|eat|wrap|roll|empty | help] [options]\n\
Options:\n\
	--service       request blob from service <name:end_point>\n\
	--input-path    put/give/eat source file <default stdin>\n\
	--output-path   get/take target file <default stdout>\n\
	--udig          algorithm:digest for get/put/give/take/eat/empty/roll\n\
	--algorithm     algorithm name for local eat request\n\
Exit Status:\n\
	0	request succeed\n\
  	1	request denied.  blob may not exist or is not empty\n\
	2	missing or invalid command line argument\n\
	16	unexpected bio4:// service error\n\
	17	unexpected file:// system service error\n\
Examples:\n\
	S=bio4:localhost:1797\n\
	UD=sha:a0bc76c479b55b5af2e3c7c4a4d2ccf44e6c4e71\n\
\n\
	blobio get --service $S --udig $UD --output-path tmp.blob\n\
\n\
	UDIG=$(blobio eat --algorithm sha --input-path resume.pdf)\n\
	blobio put --udig $UD --input-path resume.pdf --service $S\n\
";
	write(1, blurb, strlen(blurb));
	leave(0);
}

/*
 *  Format an error message like
 *
 * 	blobio: ERROR: <verb>: <message>\n
 *
 *  and write to standard error and exit.
 */
void
die(int status, char *msg)
{
	char buf[PIPE_MAX];

	buf[0] = 0;
	if (msg)
		ecat(buf, sizeof buf, msg);
	else
		ecat(buf, sizeof buf, "<null error message>");
	bufcat(buf, sizeof buf, "\n");
	uni_write(2, buf, strlen(buf));
	leave(status);
}

static void
die2(int status, char *msg1, char *msg2)
{
	char buf[PIPE_MAX];

	buf[0] = 0;
	if (msg1)
		bufcat(buf, sizeof buf, msg1);
	if (msg2) {
		if (buf[0])
			bufcat(buf, sizeof buf, ": ");
		bufcat(buf, sizeof buf, msg2);
	}
	die(status, buf);
}

void
die3(int status, char *msg1, char *msg2, char *msg3)
{
	char buf[PIPE_MAX];

	buf[0] = 0;
	if (msg1 && msg1[0]) {
		bufcat(buf, sizeof buf, msg1);
		
		if (msg2 && msg2[0]) {
			bufcat(buf, sizeof buf, ": ");
			bufcat(buf, sizeof buf, msg2);
		}
	} else if (msg2 && msg2[0])
		bufcat(buf, sizeof buf, msg2);
	die2(status, buf, msg3);
}

/*
 *  Synopsis:
 *	Parse out the algorithm and digest from a text uniform digest (udig).
 *  Args:
 *	udig:		null terminated udig string to parse
 *	algorithm:	at least 9 bytes for extracted algorithm. 
 *	digest:		at least 129 bytes for extracted digest
 *  Returns:
 *	(char *)0	upon success
 *	error string	upon failure
 */
static char *
parse_udig(char *udig)
{
	char *u, *a, *d;
	int in_algorithm = 1;

	if (!udig[0])
		return "empty udig";

	a = algorithm;
	d = digest;

	for (u = udig;  *u;  u++) {
		char c = *u;

		if (!isascii(c) || !isgraph(c))
			return "non ascii or graph char";
		/*
		 *  Scanning algorithm.
		 */
		if (in_algorithm) {
			if (c == ':') {
				if (a == algorithm)
					return "missing <algorithm>";
				*a = 0;
				in_algorithm = 0;
			}
			else {
				if (a - algorithm >= 8)
					return "algorithm > 8 chars";
				*a++ = c;
			}
		} else {
			/*
			 *  Scanning digest.
			 */
			if (d - digest > 128)
				return "digest > 128 chars";
			*d++ = c;
		}
	}
	*d = 0;
	if (d - digest < 32)
		return "digest < 32 characters";
	return (char *)0;
}

static void
eopt(const char *option, char *why)
{
	char buf[PIPE_MAX];

	buf[0] = 0;

	bufcat(buf, sizeof buf, "option --");
	bufcat(buf, sizeof buf, option);

	if (why) {
		bufcat(buf, sizeof buf, ": ");
		bufcat(buf, sizeof buf, why);
	} else
		bufcat(buf, sizeof buf, "<null why message>");
	die(EXIT_BAD_ARG, buf);
}

static void
eopt2(char *option, char *why1, char *why2)
{
	char buf[PIPE_MAX];

	buf[0] = 0;
	if (why1)
		bufcat(buf, sizeof buf, why1);
	if (why2) {
		if (buf[0])
			bufcat(buf, sizeof buf, ": ");
		bufcat(buf, sizeof buf, why2);
	}
	eopt(option, buf);
}

static void
eservice(char *why1, char *why2)
{
	if (why2 && why2[0])
		eopt2("service", why1, why2);
	else
		eopt("service", why1);
}

static void
no_opt(char *option)
{
	char buf[PIPE_MAX];

	buf[0] = 0;
	bufcat(buf, sizeof buf, "missing required option: --");
	bufcat(buf, sizeof buf, option);

	die(EXIT_BAD_ARG, buf);
}

static void
emany(char *option)
{
	char buf[PIPE_MAX];

	buf[0] = 0;
	bufcat(buf, sizeof buf, "option given more than once: --");
	bufcat(buf, sizeof buf, option);
	die(EXIT_BAD_ARG, buf);
}

static void
enot(char *option)
{
	eopt(option, "not needed");
}

int
main(int argc, char **argv)
{
	int i;
	struct digest *dig = 0;
	char *err;
	int exit_status = -1, ok_no;

	if (argc == 1) {
		uni_write(2, usage, strlen(usage));
		die(EXIT_BAD_ARG, "no command line arguments");
	}
	if (*argv[1] == 'h' && strcmp("help", argv[1]) == 0)
		help();

	//  first argument is always the request verb

	if (strcmp("get",   argv[1]) != 0 &&
	    strcmp("put",   argv[1]) != 0 &&
	    strcmp("eat",   argv[1]) != 0 &&
	    strcmp("give",  argv[1]) != 0 &&
	    strcmp("take",  argv[1]) != 0 &&
	    strcmp("wrap",  argv[1]) != 0 &&
	    strcmp("roll",  argv[1]) != 0 &&
	    strcmp("empty", argv[1]) != 0)
	    	die2(EXIT_BAD_ARG, "unknown verb", argv[1]);

	verb = argv[1];

	//  parse command line arguments, pass 1

	for (i = 2;  i < argc;  i++) {
		char *a = argv[i];

		//  all options start with --

		if (*a++ != '-' || *a++ != '-')
			die2(EXIT_BAD_ARG, "unknown option", argv[i]);

		/*
		 *  Options:
		 *	--service name:end_point
		 *	--udig algorithm:digest
		 *	--algorithm name
		 *	--input-path <path/to/file>
		 *	--output-path <path/to/file>
		 */
		if (strcmp("udig", a) == 0) {
			char *ud;
			int j;
			struct digest *d;

			if (digest[0])
				emany("udig");

			// both --udig and --algorithm can't be defined at once

			if (algorithm[0])
				eopt("udig", "option --algorithm conflicts");
			if (++i == argc)
				eopt("udig", "missing <algorithm:digest>");
			ud = argv[i];

			err = parse_udig(ud);
			if (err)
				eopt2("udig", err, ud);

			//  find the digest module for algorithm.

			for (j = 0;  digests[j];  j++)
				if (strcmp(algorithm,
				      digests[j]->algorithm) == 0)
					break;
			d = digests[j];
			if (!d)
				eopt2("udig", "unknown algorithm", algorithm);

			//  verify the digest is syntactically well formed

			if (digest[0] && d->syntax() == 0)
				eopt2("udig", "bad digest syntax", digest);
			dig = d;
		} else if (strcmp("algorithm", a) == 0) {
			int j;
			char *n;

			if (algorithm[0])
				emany("algorithm");
			if (digest[0])
				eopt("algorithm", "option --udig conflicts");
			if (++i == argc)
				eopt("algorithm", "missing <name>");

			//  find the digest module for udig algorithm
			n = argv[i];
			if (!*n)
				eopt("algorithm", "empty name");
			for (j = 0;  digests[j];  j++)
				if (strcmp(n, digests[j]->algorithm)==0)
					break;
			if (!digests[j])
				eopt2("algorithm", "unknown algorithm", n);
			strcpy(algorithm, digests[j]->algorithm);
			dig = digests[j];
		} else if (strcmp("input-path", a) == 0) {
			if (input_path)
				emany("input-path");

			if (++i == argc)
				eopt("input-path", "requires file path");
			if (!*argv[i])
				eopt("input-path", "empty file path");
			input_path = argv[i];

		} else if (strcmp("output-path", a) == 0) {
			if (output_path)
				emany("output-path");

			if (++i == argc)
				eopt("output-path", "missing <file path>");
			output_path = argv[i];
		} else if (strcmp("service", a) == 0) {
			char *s, *p;
			char name[9], *endp;
			struct service *sp = 0;
			int j;

			if (service)
				emany("service");

			if (++i == argc)
				eservice("missing <name:end_point>", "");
			s = argv[i];

			//  extract the ascii service name.

			p = strchr(s, ':');
			if (!p)
				eservice("no colon in <name:end_point>", s);
			if (p - s > 8)
				eservice("name > 8 chars", s);
			if (p - s == 0)
				eservice("empty name", s);

			//  find the service structure

			memcpy(name, s, p - s);
			name[p - s] = 0;
			for (j = 0;  services[j] && sp == NULL;  j++)
				if (strcmp(services[j]->name, name) == 0)
					sp = services[j];
			if (!sp)
				eservice("unknown service", name);

			//  extract the end point of the service.

			p++;
			if (strlen(p) > 255)
				eservice("end point > 255 characters", s);
			if (!*p)
				eservice("missing <end point>", s);
			endp = p;

			//  validate the syntax of the end point

			err = sp->end_point_syntax(endp);
			if (err)
				eservice(err, endp);
			service = sp;
			strcpy(end_point, endp);
		} else if (strcmp("trace", a) == 0) {
			if (tracing)
				emany("trace");
			tracing = 1;
			TRACE("trace enabled");
		} else
			die2(EXIT_BAD_ARG, "unknown option", argv[i]);
	}

	if (service) {
		TRACE2("service name", service->name);
		TRACE2("end point", end_point);
	}

#ifndef COMPILE_TRACE
	tracing = 0;
#endif

	/*
	 *  verb: get/give/put/take/roll
	 *
	 *  	--service required
	 *  	--udig required
	 *  	no --{input,output}-path for verb roll
	 *  	no --output-path for verb give
	 *  	no --input-path for verb take
	 *
	 *  verb: eat
	 *  	no --output-path
	 *	--service requires --udig and 
	 *		implies no --algorithm
	 *  verb: wrap
	 *	no --{input,output}-path, --udig
	 *	--algorithm required
	 *  Note:
	 *	Convert if/else if/ tests to switch.
	 */
	if (*verb == 'g' || *verb == 'p' || *verb == 't' || *verb == 'r') {
		if (!service)
			no_opt("service");
		if (!digest[0])
			no_opt("udig");
		if (*verb == 'r') {
			if (input_path)
				enot("input-path");
			else if (output_path)
				enot("output-path");
		} else if (verb[1] == 'i') {
			if (output_path)
				enot("output-path");
		} else if (verb[1] == 'a') {
			if (input_path)
				enot("input-path");
		} else if (*verb == 'p') {
			if (output_path)
				enot("output-path");
		}
	} else if (*verb == 'e') {
		if (service) {
			if (output_path)
				enot("output-path");
			if (algorithm[0]) {
				if (!digest[0])
					enot("algorithm");
			} else
				enot("udig");
		} else if (digest[0])
			no_opt("service");
		else if (!algorithm[0])
			no_opt("algorithm");
	} else if (*verb == 'w') {
		if (!service)
			no_opt("service");
		if (input_path)
			enot("input-path");
		if (output_path)
			enot("output-path");
		if (digest[0])
			enot("udig");
		if (algorithm[0])
			enot("algorithm");
	}

	//  the input path must exist

	if (input_path) {
		struct stat st;

		if (stat(input_path, &st) != 0) {
			if (errno == ENOENT)
				eopt2("input-path", "no file", input_path);
			eopt2("input-path", strerror(errno), input_path);
		}
	}

	//  the output path must not exist

	if (output_path) {
		struct stat st;

		if (stat(output_path, &st) == 0)
			eopt2("output-path", "file exists", output_path);
		if (errno != ENOENT)
			eopt2("output-path", strerror(errno), output_path);
	}

	if (service)
		service->digest = dig;

	//  initialize the digest

	if (dig && (err = dig->init()))
		die(EXIT_BAD_DIG, err);

	//  open the blob service

	if (service && (err = service->open())) {
		char buf[PIPE_MAX];


		buf[0] = 0;
		bufcat(buf, sizeof buf, "service open(");
		bufcat(buf, sizeof buf, end_point);
		bufcat(buf, sizeof buf, ") failed");
		die2(EXIT_BAD_SER, buf, err);
	}

	//  open the input path

	if (input_path) {
		if (service) {
			err = service->open_input();
			if (err)
				die3(EXIT_BAD_SER, "service open(input) failed",
							err, input_path);
		} else {
			int fd;

			fd = uni_open(input_path, O_RDONLY);
			if (fd == -1)
				die3(EXIT_BAD_SER, "open(input) failed",
							err, strerror(errno));
			input_fd = fd;
		}
	}

	//  open the output path

	if (output_path) {
		if (service) {
			err = service->open_output();
			if (err)
				die3(EXIT_BAD_SER,
					"service open(output) failed",
					err,
					output_path
				);
		} else {
			int fd;

			fd = uni_open_mode(
				output_path,
				O_WRONLY | O_EXCL | O_CREAT,
				S_IRUSR | S_IRGRP
			);
			if (fd == -1)
				die3(EXIT_BAD_SER,
					"open(output) failed",
					strerror(errno),
					output_path
				);
			output_fd = fd;
		}
	}


	//  close unneeded std{in,out}.  files are a precious resource

	if (input_fd != 0 && uni_close(0))
		die2(EXIT_BAD_UNI, "close(stdin) failed", strerror(errno));
	if (output_fd != 1 && uni_close(1))
		die2(EXIT_BAD_UNI, "close(stdout) failed", strerror(errno));


	//  invoke the service callback
	//
	//  Note: Convert if/else if/ tests to switch.

	if (*verb == 'g') {

		//  "get" or "give" a blob

		if (verb[1] == 'e') {
			if ((err = service->get(&ok_no)))
				die2(EXIT_BAD_SER, "get failed", err);
			exit_status = ok_no;
		} else {
			if ((err = service->give(&ok_no)))
				die2(EXIT_BAD_SER, "give failed", err);

			//  remove input path upon successful "give"

			if (ok_no == 0 && input_path) {
				int status = unlink(input_path);

				if (status == -1 && errno != ENOENT)
					die2(EXIT_BAD_UNI,
						"unlink(input) failed",
						strerror(errno)
					);
			}
			exit_status = ok_no;
		}
	} else if (*verb == 'e') {
		if (service) {
			if ((err = service->eat(&ok_no)))
				die2(EXIT_BAD_SER, "eat(service) failed", err);
			exit_status = ok_no;
		} else {
			char buf[128 + 1];

			if ((err = dig->eat_input()))
				die2(EXIT_BAD_SER, "eat(input) failed", err);

			//  write the ascii digest

			buf[0] = 0;
			bufcat(buf, sizeof buf, digest);
			bufcat(buf, sizeof buf, "\n");
			if (uni_write_buf(output_fd, buf, strlen(buf)))
				die2(EXIT_BAD_UNI, "write(digest) failed",
							strerror(errno));
			exit_status = 0;
		}
	} else if (*verb == 'p') {
		if ((err = service->put(&ok_no)))
			die2(EXIT_BAD_SER, "put() failed", err);
		exit_status = ok_no;
	} else if (*verb == 't') {
		if ((err = service->take(&ok_no)))
			die2(EXIT_BAD_SER, "take() failed", err);
		exit_status = ok_no;
	} else if (*verb == 'w') {
		if ((err = service->wrap(&ok_no)))
			die2(EXIT_BAD_SER, "wrap() failed", err);
		exit_status = ok_no;
	} else if (*verb == 'r') {
		if ((err = service->roll(&ok_no)))
			die2(EXIT_BAD_SER, "roll() failed", err);
		exit_status = ok_no;
	}

	//  close input file

	if (input_fd > 0 && uni_close(input_fd))
		die2(EXIT_BAD_UNI, "close(in) failed", strerror(errno));

	//  close output file

	if (output_fd > 1 && uni_close(output_fd))
		die2(EXIT_BAD_UNI, "close(out) failed", strerror(errno));

	leave(exit_status);

	/*NOTREACHED*/
	exit(0);
}