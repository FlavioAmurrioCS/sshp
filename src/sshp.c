/*
 * Parallel ssh
 *
 * Author: Dave Eddy <dave@daveeddy.com>
 * Date: March 26, 2021
 * License: MIT
 */

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// app detauls
#define PROG_NAME	"sshp"
#define PROG_VERSION	"v0.0.0"
#define PROG_FULL_NAME	"Parallel SSH Manager"
#define PROG_SOURCE	"https://github.com/bahamas10/sshp"
#define PROG_LICENSE	"MIT License"

// epoll options
#define EPOLL_MAX_EVENTS	50
#define EPOLL_WAIT_TIMEOUT	-1

// maximum number of arguments for a child process
#define MAX_ARGS	256

// max characters to process in line-by-line and join mode respectively
#define DEFAULT_MAX_LINE_LENGTH		(1 * 1024) // 1k
#define DEFAULT_MAX_OUTPUT_LENGTH	(8 * 1024) // 8k

// pipe ends
#define READ_END	0
#define WRITE_END	1

// ANSI color codes
#define COLOR_BLACK	"\033[0;30m"
#define COLOR_RED	"\033[0;31m"
#define COLOR_GREEN	"\033[0;32m"
#define COLOR_YELLOW	"\033[0;33m"
#define COLOR_BLUE	"\033[0;34m"
#define COLOR_MAGENTA	"\033[0;35m"
#define COLOR_CYAN	"\033[0;36m"
#define COLOR_WHITE	"\033[0;37m"
#define COLOR_RESET	"\033[0m"

// printf-like function that runs if "debug" mode is enabled
#define DEBUG(...) { \
	if (opts.debug) { \
		printf("[%s%s%s] ", colors.cyan, PROG_NAME, colors.reset); \
		printf(__VA_ARGS__); \
	} \
}

/*
 * Program modes of execution.
 */
enum ProgMode {
	MODE_LINE_BY_LINE = 0,	// line-by-line mode, default
	MODE_GROUP,		// group mode, `-g` or `--group`
	MODE_JOIN		// join mode, `-j` or `--join`
};

/*
 * Pipe types.
 */
enum PipeType {
	PIPE_STDOUT = 1,	// stdout pipe
	PIPE_STDERR,		// stderr pipe
	PIPE_STDIO		// both stdout and stderr (used in join mode)
};

/*
 * A struct that represents a single child process.
 *
 * - stdout_fd and stderr_fd are used in group and line-by-line mode.
 * - stdio_fd represents both output streams and is used in join mode, as well
 *   as the buffer object to store the output.
 */
typedef struct child_process {
	pid_t pid;		// child pid, -1 = hasn't started
	int stdout_fd;		// stdout fd, -1 = hasn't started, -2 = closed
	int stderr_fd;		// stderr fd, -1 = hasn't started, -2 = closed
	int stdio_fd;		// stdio fd,  -1 = hasn't started, -2 = closed
	char *output;		// output buffer (used by join mode)
	int output_idx;		// output index (used by join mode)
	int exit_code;		// exit code, -1 = hasn't exited
	long started_time;	// monotonic time (in ms) when child forked
	long finished_time;	// monotonic time (in ms) when child reaped
} ChildProcess;

/*
 * A struct that represents a single host (as a linked-list).
 */
typedef struct host {
	char *name;		// host name
	ChildProcess *cp;	// child process
	struct host *next;	// next Host in the list
} Host;

/*
 * Wrapper struct for use when an fd sees an event.
 */
typedef struct fd_event {
	Host *host;		// related Host struct
	int fd;			// fd number
	char *buffer;		// buffer used by line-by-line and join mode
	int offset;		// buffer offset used as noted above
	enum PipeType type;	// type of fd this event represents
} FdEvent;

// Linked-list of Hosts
static Host *hosts = NULL;

// Command to execute
static char **remote_command = {NULL};

// Base SSH Command
static char *base_ssh_command[MAX_ARGS] = {NULL};

// Epoll instance
static int epoll_fd;

// If a newline was printed (used for group mode only)
static bool newline_printed = true;

// If stdout is a tty
static bool stdout_isatty;

// CLI options for getopt_long
static char *short_options = "ac:def:ghi:jl:m:nNo:p:qstv";
static struct option long_options[] = {
	{"max-line-length", required_argument, NULL, 1000},
	{"max-output-length", required_argument, NULL, 1001},
	{"anonymous", no_argument, NULL, 'a'},
	{"color", required_argument, NULL, 'c'},
	{"debug", no_argument, NULL, 'd'},
	{"exit-codes", no_argument, NULL, 'e'},
	{"file", required_argument, NULL, 'f'},
	{"group", no_argument, NULL, 'g'},
	{"help", no_argument, NULL, 'h'},
	{"identity", required_argument, NULL, 'i'},
	{"join", no_argument, NULL, 'j'},
	{"login", required_argument, NULL, 'l'},
	{"max-jobs", required_argument, NULL, 'm'},
	{"dry-run", no_argument, NULL, 'n'},
	{"no-strict", no_argument, NULL, 'N'},
	{"option", required_argument, NULL, 'o'},
	{"port", required_argument, NULL, 'p'},
	{"quiet", no_argument, NULL, 'q'},
	{"silent", no_argument, NULL, 's'},
	{"trim", no_argument, NULL, 't'},
	{"version", no_argument, NULL, 'v'},
	{NULL, 0, NULL, 0}
};

// options set via CLI opts
static struct opts {
	// user options (program)
	bool anonymous;		// -a, --anonymous
	char *color;		// -c, --color <on|off|auto>
	bool debug;		// -d, --debug
	bool exit_codes;	// -e, --exit-codes
	char *file;		// -f, --file <file>
	bool group;		// -g, --group
	bool join;		// -j, --join
	int max_jobs;		// -m, --max-jobs <num>
	bool dry_run;		// -n, --dry-run
	char *port;		// -p, --port <port>
	bool silent;		// -s, --silent
	bool trim;		// -t, --trim
	int max_line_length;	// --max-line-length <num>
	int max_output_length;	// --max-output-length <num>

	// user options (passed directly to ssh)
	char *identity;		// -i, --ident <file>
	char *login;		// -l, --login <name>
	bool no_strict;		// -N, --no-strict
	bool quiet;		// -q, --quiet

	// derived options
	enum ProgMode mode;	// set by program based on `-j` or `-g`
} opts;

// colors to use when printing if coloring is enabled
static struct colors {
	char *black;
	char *red;
	char *green;
	char *yellow;
	char *blue;
	char *magenta;
	char *cyan;
	char *white;
	char *reset;
} colors;

/*
 * Print the usage message to the given filestream.
 */
static void
print_usage(FILE *s)
{
	// print banner
	fprintf(s, "%s        _         %s\n", colors.magenta, colors.reset);
	fprintf(s, "%s  _____| |_  _ __ %s   ", colors.magenta, colors.reset);
	fprintf(s, "%s %s (%s)%s\n", colors.green, PROG_FULL_NAME, PROG_VERSION, colors.reset);
	fprintf(s, "%s (_-<_-< ' \\| '_ \\%s   ", colors.magenta, colors.reset);
	fprintf(s, "%s Source: %s%s\n", colors.green, PROG_SOURCE, colors.reset);
	fprintf(s, "%s /__/__/_||_| .__/%s   ", colors.magenta, colors.reset);
	fprintf(s, "%s %s%s\n", colors.green, PROG_LICENSE, colors.reset);
	fprintf(s, "%s            |_|   %s   \n", colors.magenta, colors.reset);
	fprintf(s, "\n");
	fprintf(s, "Parallel ssh with streaming output\n");
	fprintf(s, "\n");
	// usage
	fprintf(s, "%sUSAGE:%s\n", colors.yellow, colors.reset);
	fprintf(s, "%s    %s [-m maxjobs] [-f file] command ...%s\n",
	    colors.green, PROG_NAME, colors.reset);
	fprintf(s, "\n");
	// examples
	fprintf(s, "%sEXAMPLES:%s\n", colors.yellow, colors.reset);
	fprintf(s, "    ssh into a list of hosts passed via stdin and get the output of `uname -v`\n");
	fprintf(s, "\n");
	fprintf(s, "%s      %s uname -v < hosts%s\n",
	    colors.green, PROG_NAME, colors.reset);
	fprintf(s, "\n");
	fprintf(s, "    ssh into a list of hosts passed on the command line, limit max parallel\n");
	fprintf(s, "    connections to 3, and grab the output of pgrep\n");
	fprintf(s, "\n");
	fprintf(s, "%s      %s -m 3 -f hosts.txt pgrep -fl process%s\n",
	    colors.green, PROG_NAME, colors.reset);
	fprintf(s, "\n");
	// options
	fprintf(s, "%sOPTIONS:%s\n", colors.yellow, colors.reset);
	fprintf(s, "%s  -a, --anonymous            %s", colors.green, colors.reset);
	fprintf(s, "hide hostname prefix, defaults to false\n");
	fprintf(s, "%s  -c, --color <on|off|auto>  %s", colors.green, colors.reset);
	fprintf(s, "enable or disable color output, defaults to auto\n");
	fprintf(s, "%s  -d, --debug                %s", colors.green, colors.reset);
	fprintf(s, "turn on debugging information, defaults to false\n");
	fprintf(s, "%s  -e, --exit-codes           %s", colors.green, colors.reset);
	fprintf(s, "print the exit code of the remote processes, defaults to false\n");
	fprintf(s, "%s  -f, --file <file>          %s", colors.green, colors.reset);
	fprintf(s, "a file of hosts separated by newlines, defaults to stdin\n");
	fprintf(s, "%s  -g, --group                %s", colors.green, colors.reset);
	fprintf(s, "group the output together as it comes in by hostname, not line-by-line\n");
	fprintf(s, "%s  -h, --help                 %s", colors.green, colors.reset);
	fprintf(s, "print this message and exit\n");
	fprintf(s, "%s  -j, --join                 %s", colors.green, colors.reset);
	fprintf(s, "join hosts together by unique output (aggregation mode)\n");
	fprintf(s, "%s  -m, --max-jobs <num>       %s", colors.green, colors.reset);
	fprintf(s, "the maximum number of jobs to run concurrently, defaults to 300\n");
	fprintf(s, "%s  -n, --dry-run              %s", colors.green, colors.reset);
	fprintf(s, "print debug information without actually running any commands\n");
	fprintf(s, "%s  -N, --no-strict            %s", colors.green, colors.reset);
	fprintf(s, "disable strict host key checking for ssh, defaults to false\n");
	fprintf(s, "%s  -s, --silent               %s", colors.green, colors.reset);
	fprintf(s, "silence all stdout and stderr from remote hosts, defaults to false\n");
	fprintf(s, "%s  -t, --trim                 %s", colors.green, colors.reset);
	fprintf(s, "trim hostnames (remove domain) for output only, defaults to false\n");
	fprintf(s, "%s  -v, --version              %s", colors.green, colors.reset);
	fprintf(s, "print the version number and exit\n");
	fprintf(s, "%s  --max-line-length <num>    %s", colors.green, colors.reset);
	fprintf(s, "maximum line length (in line-by-line mode only), defaults to %d\n",
	    DEFAULT_MAX_LINE_LENGTH);
	fprintf(s, "%s  --max-output-length <num>  %s", colors.green, colors.reset);
	fprintf(s, "maximum output length (in join mode only), defaults to %d\n",
	    DEFAULT_MAX_OUTPUT_LENGTH);
	fprintf(s, "\n");
	// ssh options
	fprintf(s, "%sSSH OPTIONS:%s (passed directly to ssh)\n", colors.yellow, colors.reset);
	fprintf(s, "%s  -i, --identity <ident>     %s", colors.green, colors.reset);
	fprintf(s, "ssh identity file to use\n");
	fprintf(s, "%s  -l, --login <name>         %s", colors.green, colors.reset);
	fprintf(s, "the username to login as\n");
	fprintf(s, "%s  -q, --quiet                %s", colors.green, colors.reset);
	fprintf(s, "run ssh in quiet mode\n");
	fprintf(s, "%s  -p, --port <port>          %s", colors.green, colors.reset);
	fprintf(s, "the ssh port\n");
}

/*
 * Return an "s" if the number of items (given as an int) should be plural.
 */
static const char *
pluralize(int num)
{
	return num == 1 ? "" : "s";
}

/*
 * Convert the given mode to a string.
 */
static const char *
prog_mode_to_string(enum ProgMode mode)
{
	switch (mode) {
	case MODE_LINE_BY_LINE: return "line-by-line";
	case MODE_GROUP: return "group";
	case MODE_JOIN: return "join";
	default: errx(3, "unknown ProgMode: %d", mode);
	}
}

/*
 * Wrapper for malloc that takes an error message as the second argument and
 * exits on failure.
 */
static void *
safe_malloc(size_t size, const char *msg)
{
	void *ptr;

	assert(size > 0);
	assert(msg != NULL);

	ptr = malloc(size);

	if (ptr == NULL) {
		err(3, "malloc %s", msg);
	}

	return ptr;
}

/*
 * Create a ChildProcess object.
 */
static ChildProcess *
child_process_create()
{
	ChildProcess *cp = safe_malloc(sizeof (ChildProcess),
	    "child_process_create");

	cp->stdout_fd = -1;
	cp->stderr_fd = -1;
	cp->stdio_fd = -1;
	cp->pid = -1;
	cp->exit_code = -1;
	cp->started_time = -1;
	cp->finished_time = -1;
	cp->output = NULL;
	cp->output_idx = -1;

	return cp;
}

/*
 * Check if the given Host object has had both of its stdio pipes closed.
 */
static bool
child_process_stdio_done(ChildProcess *cp)
{
	assert(cp != NULL);

	return (cp->stdout_fd == -2 && cp->stderr_fd == -2) ||
	    cp->stdio_fd == -2;
}

/*
 * Free a ChildProcess object (and the optionally created output buffer).
 */
static void
child_process_destroy(ChildProcess *cp)
{
	if (cp == NULL) {
		return;
	}

	free(cp->output);
	free(cp);
}

/*
 * Allocate and create a new Host object given its hostname.  The hostname will
 * be copied from the given argument.
 */
static Host *
host_create(const char *name)
{
	assert(name != NULL);

	Host *host = safe_malloc(sizeof (Host), "host_create");
	char *name_dup = strdup(name);

	if (name_dup == NULL) {
		err(3, "strdup hostname %s", name);
	}

	host->name = name_dup;
	host->cp = NULL;
	host->next = NULL;

	return host;
}

/*
 * Free an allocated Host object.
 */
static void
host_destroy(Host *host)
{
	if (host == NULL) {
		return;
	}

	free(host->name);
	host->name = NULL;

	child_process_destroy(host->cp);

	free(host);
}

/*
 * Create and FdEvent object given a host pointer and pipetype.
 */
static FdEvent *
fdev_create(Host *host, enum PipeType type)
{
	assert(host != NULL);
	assert(host->cp != NULL);

	FdEvent *fdev = safe_malloc(sizeof (FdEvent), "FdEvent");

	fdev->host = host;
	fdev->type = type;
	fdev->offset = 0;
	fdev->buffer = NULL;

	// initailize stdio buffers
	switch (opts.mode) {
	case MODE_LINE_BY_LINE:
		fdev->buffer = safe_malloc(opts.max_line_length + 2,
		    "fdev->buffer");
		break;
	case MODE_JOIN:
		fdev->buffer = safe_malloc(opts.max_output_length + 1,
		    "fdev->buffer");
		break;
	case MODE_GROUP:
		// stdio is not buffered in group mode
		break;
	}

	// get fd
	switch (type) {
	case PIPE_STDOUT: fdev->fd = host->cp->stdout_fd; break;
	case PIPE_STDERR: fdev->fd = host->cp->stderr_fd; break;
	case PIPE_STDIO:  fdev->fd = host->cp->stdio_fd;  break;
	}
	assert(fdev->fd >= 0);

	return fdev;
}

/*
 * Given an FdEvent pointer return the event relevant color.
 */
static char *
fdev_get_color(FdEvent *fdev)
{
	assert(fdev != NULL);

	switch (fdev->type) {
	case PIPE_STDOUT: return colors.green;
	case PIPE_STDERR: return colors.red;
	case PIPE_STDIO: return "";
	default: errx(3, "fdev_get_color unknown fdev->type '%d'", fdev->type);
	}
}

/*
 * Free an allocated FdEvent object.
 */
static void
fdev_destroy(FdEvent *fdev)
{
	if (fdev != NULL) {
		free(fdev->buffer);
		fdev->buffer = NULL;
	}
	free(fdev);
}

/*
 * Create a pipe with both ends set to non-blocking and cloexec.
 */
static void
make_pipe(int *fd)
{
	assert(fd != NULL);

	if (pipe(fd) == -1) {
		err(3, "pipe");
	}
	if (fcntl(fd[READ_END], F_SETFL, O_NONBLOCK) == -1) {
		err(3, "set read end nonblocking");
	}
	if (fcntl(fd[WRITE_END], F_SETFL, O_NONBLOCK) == -1) {
		err(3, "set write end nonblocking");
	}
	if (fcntl(fd[READ_END], F_SETFD, FD_CLOEXEC) == -1) {
		err(3, "set read end cloexec");
	}
	if (fcntl(fd[WRITE_END], F_SETFD, FD_CLOEXEC) == -1) {
		err(3, "set write end cloexec");
	}
}

/*
 * Push an argument to the ssh base command and bounds check it.
 * The strings passed to this function need to be allocated or contstantly
 * defined.
 */
static void
push_arguments(char *s, ...)
{
	assert(s != NULL);

	static int idx = 0;
	va_list args;

	va_start(args, s);
	while (s != NULL) {
		if (idx >= MAX_ARGS - 2) {
			errx(2, "too many command arguments");
		}
		base_ssh_command[idx] = s;
		idx++;
		s = va_arg(args, char *);
	}
	va_end(args);
}

/*
 * Replace the first occurence of char c with '\0' in a string.
 * Returns true if a replacement was made and false otherwise.
 */
static bool
lsplit_str(char *s, char c)
{
	assert(s != NULL);

	for (int i = 0; s[i] != '\0'; i++) {
		if (s[i] == c) {
			s[i] = '\0';
			return true;
		}
	}
	return false;
}

/*
 * Given a null terminated stream return whether it ends in a newline
 * character.
 */
static bool
ends_in_newline(const char *s)
{
	assert(s != NULL);
	int idx = strlen(s);

	// empty strings don't end in a newline technically
	if (idx == 0) {
		return false;
	}

	return s[idx - 1] == '\n';
}

/*
 * Get the current monotonic time in ms.
 */
static long
monotonic_time_ms()
{
	struct timespec t;

	if (clock_gettime(CLOCK_MONOTONIC, &t) == -1) {
		err(3, "clock_gettime");
	}

	return (t.tv_sec * 1e3) + (t.tv_nsec / 1e6);
}

/*
 * Print the header for a given host.
 */
static void
print_host_header(Host *host)
{
	assert(host != NULL);

	printf("[%s%s%s]", colors.cyan,
	    host->name, colors.reset);
}

/*
 * Given a Host object and a buffer of a suitable size, fill the buffer with
 * the required arguments to exec a child process.
 */
static void
build_ssh_command(Host *host, char **command, int size)
{
	assert(host != NULL);
	assert(command != NULL);
	assert(size > 0);

	int idx = 0;
	char *name_array[] = {host->name, NULL};

	/*
	 * construct SSH command like:
	 * base_ssh_command + host name + remote_command
	 * as a null terminated array called "command"
	 */
	char **items_arr[] = {
		base_ssh_command,
		name_array,
		remote_command,
		NULL
	};
	char ***items = items_arr;
	while (*items != NULL) {
		char **item = *items;
		while (*item != NULL) {
			char *arg = *item;
			command[idx++] = arg;
			if (idx >= size) {
				errx(2, "too many arguments (<= %d)", size);
			}

			item++;
		}

		items++;
	}

	assert(idx < size);
	assert(command[size - 1] == NULL);
}

/*
 * Fork and exec a subprocess.  This function is responsible for creating and
 * initializing the stdio pipes and attaching them to the given Host object.
 */
static void
spawn_child_process(Host *host)
{
	assert(host != NULL);
	assert(host->name != NULL);
	assert(host->cp == NULL);

	char *command[MAX_ARGS] = {NULL};
	pid_t pid;
	int stdout_fd[2];
	int stderr_fd[2];
	int stdio_fd[2];

	// build the ssh command
	build_ssh_command(host, command, MAX_ARGS);

	// TODO change
	command[0] = "ls";
	command[1] = "-lha";
	command[2] = "/proc/self/fd";
	command[3] = NULL;

	command[0] = "./prog";
	command[1] = NULL;

	// create the stdio pipes
	switch (opts.mode) {
	case MODE_JOIN:
		// join mode uses a sharded stdout/stderr pipe
		make_pipe(stdio_fd);
		break;
	default:
		// all other modes use a pipe per stream
		make_pipe(stdout_fd);
		make_pipe(stderr_fd);
	}

	// fork the process
	pid = fork();
	if (pid == -1) {
		err(3, "fork");
	}

	// in child
	if (pid == 0) {
		int *out_fd = opts.mode == MODE_JOIN ? stdio_fd : stdout_fd;
		int *err_fd = opts.mode == MODE_JOIN ? stdio_fd : stderr_fd;

		if (dup2(out_fd[WRITE_END], STDOUT_FILENO) == -1) {
			err(3, "dup2 stdout");
		}
		if (dup2(err_fd[WRITE_END], STDERR_FILENO) == -1) {
			err(3, "dup2 stderr");
		}

		execvp(command[0], command);
		err(3, "exec");
	}

	// in parent
	host->cp = child_process_create();

	// close write ends and save read ends
	switch (opts.mode) {
	case MODE_JOIN:
		close(stdio_fd[WRITE_END]);
		host->cp->stdio_fd = stdio_fd[READ_END];
		break;
	default:
		close(stdout_fd[WRITE_END]);
		close(stderr_fd[WRITE_END]);
		host->cp->stdout_fd = stdout_fd[READ_END];
		host->cp->stderr_fd = stderr_fd[READ_END];
		break;
	}

	// save data
	host->cp->pid = pid;
	host->cp->started_time = monotonic_time_ms();
}

/*
 * Register a specific fd to epoll.
 */
static void
register_child_process_fd(Host *host, enum PipeType type)
{
	// create an epoll event
	struct epoll_event ev;
	FdEvent *fdev = fdev_create(host, type);

	ev.events = EPOLLIN;
	ev.data.ptr = fdev;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fdev->fd, &ev) == -1) {
		err(3, "epoll_ctl add");
	}
}

/*
 * Given a Host object that has had its child process spawned add both of its
 * pipes fds to the epoll watcher for events.
 */
static void
register_child_process_fds(Host *host)
{
	assert(host != NULL);

	switch (opts.mode) {
	case MODE_JOIN:
		register_child_process_fd(host, PIPE_STDIO);
		break;
	default:
		register_child_process_fd(host, PIPE_STDOUT);
		register_child_process_fd(host, PIPE_STDERR);
		break;
	}
}

/*
 * Call waitpid on the subprocess associated with the given Host object.  This
 * function will reap the process, set the exit code and remove the pid from
 * the Host object, and optionally print the exited message if opts.exit_codes
 * or opts.debug is set.
 */
static void
wait_for_child(Host *host)
{
	assert(host != NULL);

	int status;
	pid_t pid;
	ChildProcess *cp = host->cp;

	// reap the child
	pid = waitpid(cp->pid, &status, 0);

	if (pid < 0) {
		err(3, "waitpid");
	}

	// set the host as closed
	cp->exit_code = WEXITSTATUS(status);
	cp->pid = -2;
	cp->finished_time = monotonic_time_ms();

	// print the exit message
	if (opts.exit_codes || opts.debug) {
		long delta = cp->finished_time - cp->started_time;
		char *code_color = cp->exit_code == 0 ?
		    colors.green : colors.red;

		// check if a newline is needed
		if (!newline_printed) {
			printf("\n");
			newline_printed = true;
		}

		// print the exit status
		printf("[%s%s%s] exited: %s%d%s (%s%ld%s ms)\n",
		    colors.cyan, host->name, colors.reset,
		    code_color, cp->exit_code, colors.reset,
		    colors.magenta, delta, colors.reset);
	}
}

/*
 * Prints the given linebuf with the given color as well as the host header.
 *
 * (used for line-by-line mode).
 */
static void
print_line_buffer(FdEvent *fdev)
{
	assert(fdev != NULL);
	assert(fdev->host != NULL);
	assert(fdev->buffer != NULL);

	char *color = fdev_get_color(fdev);

	if (!opts.anonymous) {
		print_host_header(fdev->host);
		printf(" ");
	}

	printf("%s%s%s", color, fdev->buffer, colors.reset);
}

/*
 * Called by read_active_fd when processing read bytes in line-by-line mode.
 */
static void
process_data_line_by_line(FdEvent *fdev, char *buf, int bytes)
{
	assert(fdev != NULL);
	assert(fdev->host != NULL);
	assert(buf != NULL);
	assert(bytes > 0);

	// loop data character-by-character
	for (int i = 0; i < bytes; i++) {
		char c = buf[i];

		if (fdev->offset < opts.max_line_length) {
			// buffer has room for character
			fdev->buffer[fdev->offset] = c;
			fdev->offset++;
		} else if (fdev->offset == opts.max_line_length) {
			// no more room, call it a newline
			fdev->buffer[fdev->offset] = '\n';
			fdev->offset++;
		}

		// got a newline! print it
		if (c == '\n') {
			assert(fdev->offset > 0);
			assert(fdev->offset < opts.max_line_length + 2);

			fdev->buffer[fdev->offset] = '\0';
			print_line_buffer(fdev);
			fdev->offset = 0;
		}
	}
}

/*
 * Called by read_active_fd when processing read bytes in group mode.
 */
static void
process_data_group(FdEvent *fdev, char *buf, int bytes)
{
	assert(fdev != NULL);
	assert(fdev->host != NULL);
	assert(buf != NULL);
	assert(bytes > 0);

	static Host *last_host = NULL;

	// processing a new host from last time
	if (last_host != fdev->host) {
		// print a newline if needed
		if (!newline_printed) {
			printf("\n");
		}

		// print the host name
		if (!opts.anonymous) {
			print_host_header(fdev->host);
			printf("\n");
		}
	}

	// write the fd data to stdout
	printf("%s", fdev_get_color(fdev));
	fflush(stdout);
	if (write(STDOUT_FILENO, buf, bytes) < bytes) {
		err(3, "write failed");
	}
	printf("%s", colors.reset);

	// check if a newline was printed, save the last host
	newline_printed = buf[bytes - 1] == '\n';
	last_host = fdev->host;
}

/*
 * Called by read_active_fd when processing read bytes in join mode.
 */
static void
process_data_join(FdEvent *fdev, char *buf, int bytes)
{
	assert(fdev != NULL);
	assert(fdev->host != NULL);
	assert(buf != NULL);
	assert(bytes > 0);

	// loop data character-by-character
	for (int i = 0; i < bytes; i++) {
		char c = buf[i];

		// line is too long
		if (fdev->offset < opts.max_output_length) {
			// room for the character
			fdev->buffer[fdev->offset] = c;
			fdev->offset++;
		} else if (fdev->offset == opts.max_line_length) {
			// no more room, pad it with a nul byte
			fdev->buffer[fdev->offset] = '\0';
			fdev->offset++;
		} else {
			// we are overbook, just break
			break;
		}
	}
}

/*
 * Read data from FdEvent until end or would-block
 */
static bool
read_active_fd(FdEvent *fdev)
{
	char buf[BUFSIZ];
	int *fd;
	int bytes;
	Host *host;

	assert(fdev != NULL);
	assert(fdev->host != NULL);

	host = fdev->host;

	switch (fdev->type) {
	case PIPE_STDOUT:
		fd = &host->cp->stdout_fd;
		break;
	case PIPE_STDERR:
		fd = &host->cp->stderr_fd;
		break;
	case PIPE_STDIO:
		fd = &host->cp->stdio_fd;
		break;
	default:
		errx(3, "unknown type %d", fdev->type);
	}

	// loop while bytes available
	while ((bytes = read(*fd, buf, BUFSIZ)) > -1) {
		// done reading!
		if (bytes == 0) {
			epoll_ctl(epoll_fd, EPOLL_CTL_DEL, *fd, NULL);
			close(*fd);
			*fd = -2;

			switch (opts.mode) {
			case MODE_LINE_BY_LINE:
				// print any remaining data in line-by-line mode
				if (fdev->offset == 0) {
					break;
				}

				// data remaining! put a newline if it didn't have one
				if (fdev->buffer[fdev->offset - 1] != '\n') {
					fdev->buffer[fdev->offset] = '\n';
					fdev->offset++;
				}
				assert(fdev->offset < opts.max_line_length + 2);

				fdev->buffer[fdev->offset] = '\0';
				print_line_buffer(fdev);
				fdev->offset = 0;
				break;
			case MODE_GROUP:
				// nothing to do
				break;
			case MODE_JOIN:
				// copy fdev buffer to host object for later analysis
				if (fdev->offset <= opts.max_output_length) {
					fdev->buffer[fdev->offset] = '\0';
					fdev->offset++;
				}
				host->cp->output = fdev->buffer;
				fdev->buffer = NULL;
				break;
			default:
				errx(3, "unknown mode: %d", opts.mode);
			}

			fdev_destroy(fdev);

			return true;
		}

		// do nothing if in silent mode
		if (opts.silent) {
			continue;
		}

		// handle bytes in different modes
		switch (opts.mode) {
		case MODE_JOIN:
			assert(fdev->buffer != NULL);

			process_data_join(fdev, buf, bytes);
			break;
		case MODE_LINE_BY_LINE:
			assert(fdev->buffer != NULL);

			process_data_line_by_line(fdev, buf, bytes);
			break;
		case MODE_GROUP: {
			process_data_group(fdev, buf, bytes);
			break;
		}
		default:
			errx(3, "unknown mode: %d", opts.mode);
			break;
		}
	}

	assert(bytes < 0);

	// handle read error
	if (errno == EWOULDBLOCK) {
		return false;
	}

	err(3, "read failed");
}

/*
 * Finish analysis for join mode.
 */
static void
join_mode_finish(int num_hosts)
{
	int idx = 0;
	int *count = safe_malloc(sizeof (int) * num_hosts, "join_mode_finish");

	printf("\n");

	// loop the hosts to check their output
	for (Host *h1 = hosts; h1 != NULL; h1 = h1->next) {
		int num_same = 1;

		// this host already processed
		if (h1->cp->output_idx >= 0) {
			continue;
		}

		h1->cp->output_idx = idx;

		for (Host *h2 = h1->next; h2 != NULL; h2 = h2->next) {
			// skip already processed host
			if (h2->cp->output_idx >= 0) {
				continue;
			}

			// check if output is the same
			if (strcmp(h1->cp->output, h2->cp->output) == 0) {
				h2->cp->output_idx = idx;
				num_same++;
			}

		}

		count[idx] = num_same;
		idx++;
	}

	printf("finished with %s%d%s unique result%s\n\n",
	    colors.magenta, idx, colors.reset, pluralize(idx));

	for (int i = 0; i < idx; i++) {
		printf("hosts (%s%d%s/%s%d%s):%s",
		    colors.magenta, count[i], colors.reset,
		    colors.magenta, num_hosts, colors.reset,
		    colors.cyan);

		char *output = NULL;
		for (Host *h = hosts; h != NULL; h = h->next) {
			if (h->cp->output_idx != i) {
				continue;
			}

			output = h->cp->output;
			printf(" %s", h->name);
		}
		assert(output != NULL);

		printf("%s\n%s", colors.reset, output);
		if (!ends_in_newline(output)) {
			printf("\n");
		}
		printf("\n");
	}
}

/*
 * Print the progress line as hosts finish in join mode.
 */
static void
print_progress_line(int done, int num_hosts)
{
	printf("[%s%s%s] finished %s%d%s/%s%d%s\r",
	    colors.cyan, PROG_NAME, colors.reset,
	    colors.magenta, done, colors.reset,
	    colors.magenta, num_hosts, colors.reset);
	fflush(stdout);
}

/*
 * The main program loop that should be called from main().
 */
static void
main_loop(int num_hosts)
{
	Host *cur_host = hosts;
	int outstanding = 0;
	int done = 0;
	struct epoll_event events[EPOLL_MAX_EVENTS];

	if (opts.mode == MODE_JOIN && stdout_isatty) {
		print_progress_line(done, num_hosts);
	}

	// loop while there are still child processes
	while (cur_host != NULL || outstanding > 0) {
		assert(outstanding <= opts.max_jobs);

		int num_events;

		// create child processes
		while (cur_host != NULL && outstanding < opts.max_jobs) {
			spawn_child_process(cur_host);

			// chop off the domain portion of the name if -t
			if (opts.trim) {
				lsplit_str(cur_host->name, '.');
			}

			register_child_process_fds(cur_host);

			outstanding++;
			cur_host = cur_host->next;
		}

		// wait for fd events
		num_events = epoll_wait(epoll_fd, events, EPOLL_MAX_EVENTS,
		    EPOLL_WAIT_TIMEOUT);
		if (num_events == -1) {
			err(3, "epoll_wait");
		}

		// loop fd events
		for (int i = 0; i < num_events; i++) {
			struct epoll_event ev = events[i];
			FdEvent *fdev = ev.data.ptr;
			Host *host = fdev->host;

			assert(host != NULL);

			// read the active fd until it would block or is done
			bool fd_closed = read_active_fd(fdev);

			// check if the childs stdio is done and reap it
			if (fd_closed && child_process_stdio_done(host->cp)) {
				wait_for_child(host);
				outstanding--;
				done++;
				if (opts.mode == MODE_JOIN) {
					print_progress_line(done, num_hosts);
					if (done == num_hosts) {
						printf("\n");
					}
				}
			}
		}
	}
}

/*
 * Parse the hosts file and create the Host structs
 */
static int
parse_hosts(FILE *f)
{
	Host *tail = NULL;
	char hostname[HOST_NAME_MAX];
	int lineno = 1;
	int num_hosts = 0;

	assert(f != NULL);

	while (fgets(hostname, HOST_NAME_MAX, f) != NULL) {
		Host *host;
		char prefix = hostname[0];

		// skip comments and blank lines
		switch (prefix) {
		case '#':
		case ' ':
		case '\n':
		case '\0':
			goto next;
		}

		/*
		 * remove the ending newline - if a newline is not present the
		 * line is too long
		 */
		if (!lsplit_str(hostname, '\n')) {
			errx(2, "hosts file line %d too long (>= %d chars)\n%s",
			    lineno, HOST_NAME_MAX, hostname);
		}

		// create Host
		host = host_create(hostname);

		// set head of list
		if (hosts == NULL) {
			hosts = host;
		}

		// set tail of list
		if (tail != NULL) {
			tail->next = host;
		}

		tail = host;
		num_hosts++;

next:
		lineno++;
	}

	if (ferror(f)) {
		errx(2, "failed to read hosts file");
	}
	assert(feof(f));

	return num_hosts;
}

/*
 * Parse command line arguments
 */
static void
parse_arguments(int argc, char **argv)
{
	int opt;
	bool help_option = false;
	bool unknown_option = false;

	// get options
	while ((opt = getopt_long(argc, argv, short_options, long_options,
	    NULL)) != -1) {

		switch (opt) {
		case 1000: opts.max_line_length = atoi(optarg); break;
		case 1001: opts.max_output_length = atoi(optarg); break;
		case 'a': opts.anonymous = true; break;
		case 'c': opts.color = optarg; break;
		case 'd': opts.debug = true; break;
		case 'e': opts.exit_codes = true; break;
		case 'f': opts.file = optarg; break;
		case 'g': opts.group = true; break;
		case 'h': help_option = true; break;
		case 'i': opts.identity = optarg; break;
		case 'j': opts.join = true; break;
		case 'l': opts.login = optarg; break;
		case 'm': opts.max_jobs = atoi(optarg); break;
		case 'n': opts.dry_run = true; break;
		case 'N': opts.no_strict = true; break;
		case 'o': push_arguments("-o", optarg, NULL); break;
		case 'p': opts.port = optarg; break;
		case 'q': opts.quiet = true; break;
		case 's': opts.silent = true; break;
		case 't': opts.trim = true; break;
		case 'v': printf("%s\n", PROG_VERSION); exit(0);
		default: unknown_option = true; break;
		}
	}
	argc -= optind;
	argv += optind;

	// sanity check options
	if (opts.max_jobs < 1) {
		errx(2, "invalid value for `-m`: '%d'", opts.max_jobs);
	}
	if (opts.join && opts.group) {
		errx(2, "`-j` and `-g` are mutually exclusive");
	}
	if (opts.join && opts.silent) {
		errx(2, "`-j` and `-s` are mutually exclusive");
	}
	if (opts.join && opts.anonymous) {
		errx(2, "`-j` and `-a` are mutually exclusive");
	}
	if (opts.max_line_length <= 0) {
		errx(2, "invalid value for `--max-line-length`: %d",
		    opts.max_line_length);
	}
	if (opts.max_output_length <= 0) {
		errx(2, "invalid value for `--max-output-length`: %d",
		    opts.max_output_length);
	}

	// set current sshp mode
	assert(!(opts.join && opts.group));
	if (opts.join) {
		opts.mode = MODE_JOIN;
	} else if (opts.group) {
		opts.mode = MODE_GROUP;
	}

	// check if colorized output should be enabled
	if (opts.color == NULL || strcmp(opts.color, "auto") == 0) {
		opts.color = stdout_isatty ? "on" : "off";
	}
	if (strcmp(opts.color, "on") == 0) {
		colors.black = COLOR_BLACK;
		colors.red = COLOR_RED;
		colors.green = COLOR_GREEN;
		colors.yellow = COLOR_YELLOW;
		colors.blue = COLOR_BLUE;
		colors.magenta = COLOR_MAGENTA;
		colors.cyan = COLOR_CYAN;
		colors.white = COLOR_WHITE;
		colors.reset = COLOR_RESET;
	} else if (strcmp(opts.color, "off") == 0) {
		// pass, this is default
	} else {
		errx(2, "invalid value for '-c': '%s'", opts.color);
	}

	// -h or unknown option
	if (unknown_option) {
		print_usage(stderr);
		exit(2);
	} else if (help_option) {
		print_usage(stdout);
		exit(0);
	}

	if (argc < 1) {
		errx(2, "no command specified");
	}

	// add options to command
	if (opts.quiet) {
		push_arguments("-q", NULL);
	}
	if (opts.identity != NULL) {
		push_arguments("-i", opts.identity, NULL);
	}
	if (opts.login != NULL) {
		push_arguments("-l", opts.login, NULL);
	}
	if (opts.port != NULL) {
		push_arguments("-p", opts.port, NULL);
	}

	// save the remaining arguments as the command
	remote_command = argv;
}

/*
 * Main method
 */
int
main(int argc, char **argv)
{
	FILE *hosts_file = stdin;
	long delta;
	long end_time;
	long start_time;
	int num_hosts;
	Host *host;

	// record start time
	start_time = monotonic_time_ms();

	// check stdout tty
	stdout_isatty = isatty(STDOUT_FILENO) == 1;

	// initalize options
	opts.max_line_length = DEFAULT_MAX_LINE_LENGTH;
	opts.max_output_length = DEFAULT_MAX_OUTPUT_LENGTH;
	opts.anonymous = false;
	opts.color = NULL;
	opts.debug = false;
	opts.exit_codes = false;
	opts.file = NULL;
	opts.group = false;
	opts.identity = NULL;
	opts.join = false;
	opts.login = NULL;
	opts.max_jobs = 50;
	opts.mode = MODE_LINE_BY_LINE;
	opts.dry_run = false;
	opts.no_strict = false;
	opts.port = NULL;
	opts.quiet = false;
	opts.silent = false;
	opts.trim = false;

	// initialize colors
	colors.black = "";
	colors.red = "";
	colors.green = "";
	colors.yellow = "";
	colors.blue = "";
	colors.magenta = "";
	colors.cyan = "";
	colors.white = "";
	colors.reset = "";

	// initalized the base ssh command
	push_arguments("echo", "ssh", NULL);

	// handle CLI options
	parse_arguments(argc, argv);

	// figure out where to read hosts from (stdin or a file)
	if (opts.file != NULL && strcmp(opts.file, "-") != 0) {
		hosts_file = fopen(opts.file, "r");
		if (hosts_file == NULL) {
			err(2, "open %s", opts.file);
		}
	}
	assert(hosts_file != NULL);

	// read in hosts and create structure for each one
	num_hosts = parse_hosts(hosts_file);
	fclose(hosts_file);

	// ensure at least 1 host is specified
	if (num_hosts < 1) {
		errx(2, "no hosts specified");
	}

	// create shared epoll instance
	epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (epoll_fd == -1) {
		err(3, "epoll_create1");
	}

	// print debug output
	if (opts.debug) {
		// print base command
		DEBUG("ssh command: [ ");
		for (char **arg = base_ssh_command; *arg != NULL; arg++) {
			printf("%s'%s'%s ",
			    colors.green, *arg, colors.reset);
		}
		printf("]\n");

		// print hosts
		DEBUG("hosts (%s%d%s): [ ",
		    colors.magenta, num_hosts, colors.reset);
		for (Host *h = hosts; h != NULL; h = h->next) {
			printf("%s'%s'%s ",
			    colors.green, h->name, colors.reset);
		}
		printf("]\n");

		// print command
		DEBUG("remote command: [ ");
		for (char **arg = remote_command; *arg != NULL; arg++) {
			printf("%s'%s'%s ",
			    colors.green, *arg, colors.reset);
		}
		printf("]\n");

		// print mode
		DEBUG("mode: %s%s%s\n",
		    colors.green, prog_mode_to_string(opts.mode), colors.reset);

		// print max jobs
		DEBUG("max-jobs: %s%d%s\n",
		    colors.green, opts.max_jobs, colors.reset);
	}

	// start the main loop!
	main_loop(num_hosts);

	// tidy up
	close(epoll_fd);

	// handle join mode if applicable
	if (opts.mode == MODE_JOIN) {
		join_mode_finish(num_hosts);
	}

	// free memory
	host = hosts;
	while (host != NULL) {
		Host *temp = host->next;
		host_destroy(host);
		host = temp;
	}

	// get end time and calculate time taken
	end_time = monotonic_time_ms();
	delta = end_time - start_time;
	DEBUG("finished (%s%ld%s ms)\n",
	    colors.magenta, delta, colors.reset);

	return 0;
}
