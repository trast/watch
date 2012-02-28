#define _GNU_SOURCE

#include <errno.h>
#include <sys/inotify.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

void die(const char *msg)
{
	fprintf(stderr, "error: %s\n", msg);
	exit(1);
}

/* --- from git, GPLv2 --- */

void *xrealloc(void *ptr, size_t size)
{
	void *ret = realloc(ptr, size);
	if (!ret && !size)
		ret = realloc(ptr, 1);
	if (!ret) {
		ret = realloc(ptr, size);
		if (!ret && !size)
			ret = realloc(ptr, 1);
		if (!ret)
			die("Out of memory, realloc failed");
	}
	return ret;
}

#define alloc_nr(x) (((x)+16)*3/2)

/*
 * Realloc the buffer pointed at by variable 'x' so that it can hold
 * at least 'nr' entries; the number of entries currently allocated
 * is 'alloc', using the standard growing factor alloc_nr() macro.
 *
 * DO NOT USE any expression with side-effect for 'x', 'nr', or 'alloc'.
 */
#define ALLOC_GROW(x, nr, alloc) \
	do { \
		if ((nr) > alloc) { \
			if (alloc_nr(alloc) < (nr)) \
				alloc = (nr); \
			else \
				alloc = alloc_nr(alloc); \
			x = xrealloc((x), alloc * sizeof(*(x))); \
		} \
	} while (0)

/* --- end from git --- */

char **ignore_patterns = NULL;
int ignore_alloc = 0;
#define IGNORE_FILE_NAME "~/.watch-ignore"

char **wdpaths = NULL;
int wd_alloc = 0;

#define HIST 5
const char *lru[HIST] = {0};

void set_dirpath(int wd, const char *path)
{
	int old_alloc = wd_alloc;
	ALLOC_GROW(wdpaths, wd+1, wd_alloc);
	if (old_alloc < wd_alloc)
		memset(wdpaths+old_alloc, 0,
		       (wd_alloc-old_alloc)*sizeof(const char *));
	wdpaths[wd] = strdup(path);
}

void die_errno(const char *msg)
{
	perror(msg);
	exit(1);
}

int ifd = -1;

int xfnmatch(const char *pat, const char *str, int flags)
{
	int ret = fnmatch(pat, str, flags);
	if (!ret || ret == FNM_NOMATCH)
		return ret;
	die_errno("fnmatch");
	return -1;
}

// returns either 'path' or a statically allocated buffer!
char *expanduser(char *path)
{
	static char buf[PATH_MAX+1];

	if (path[0] != '~')
		return path;

	if (strlen(path)+strlen(home)+1 >= PATH_MAX)
		die("I cannot handle paths longer than PATH_MAX");

	home = getenv("HOME");
	if (!home)
		die("HOME is not set");
	strcpy(buf, home);
	strcat(buf, path+1);
	return buf;
}

void read_ignore_file()
{
	FILE *fp;
	int i = 0;
	char *line = NULL;
	size_t len;
	int ret;
	char buf[PATH_MAX+1];
	char *home;

	/* ensure at least NULL is in the list */
	ALLOC_GROW(ignore_patterns, 1, ignore_alloc);
	ignore_patterns[0] = NULL;

	if (!(fp = fopen(expanduser(IGNORE_FILE_NAME), "r"))) {
		if (errno == ENOENT)
			return;
		die_errno("fopen");
	}

	while ((ret = getline(&line, &len, fp)) > 0) {
		i++;
		ALLOC_GROW(ignore_patterns, i+1, ignore_alloc);
		if (ret != strlen(line))
			die("\\0 in ignore file");
		if (line[ret-1] == '\n')
			line[ret-1] = '\0';
		ignore_patterns[i-1] = line;
		line = NULL;
	}

	ignore_patterns[i] = NULL;

	if (fclose(fp) != 0)
		die_errno("fclose");
}

int is_ignored(const char *path)
{
	char **pp = ignore_patterns;
	while (*pp) {
		if (!fnmatch(*pp, path, 0))
			return 1;
		pp++;
	}
	return 0;
}

struct dirent *xreaddir(DIR *dirfd)
{
	struct dirent *ent;
	errno = 0;
	ent = readdir(dirfd);
	if (!ent && errno)
		die_errno("readdir");
	return ent;
}

int isdir(const char *path)
{
	struct stat st;
	if (lstat(path, &st) < 0)
		die_errno("stat");
	return S_ISDIR(st.st_mode);
}

const int MASK =
	IN_ACCESS
	| IN_ATTRIB
	| IN_CREATE
	| IN_MODIFY
	| IN_MOVE_SELF
	| IN_MOVED_TO
	| IN_OPEN
	| IN_DONT_FOLLOW
	| IN_EXCL_UNLINK;

const char *event_msg(int mask)
{
	if (mask & IN_ACCESS)
		return "access";
	else if (mask & IN_ATTRIB)
		return "attrib";
	else if (mask & IN_CREATE)
		return "create";
	else if (mask & IN_MODIFY)
		return "modify";
	else if (mask & IN_MOVE_SELF)
		return "move_self";
	else if (mask & IN_MOVED_TO)
		return "moved_to";
	else if (mask & IN_OPEN)
		return "open";
	else if (mask & IN_Q_OVERFLOW)
		return "<overflow>";
	return "<huh?>";
}

void setup_watches (const char *dir)
{
	DIR *dirfd = opendir(dir);
	char fullent[PATH_MAX];
	int dirlen = strlen(dir);
	memcpy(fullent, dir, dirlen);
	fullent[dirlen] = '/';
	struct dirent *ent;
	if (!dirfd) {
		if (errno == ENOENT || errno == EACCES)
			return;
		die_errno("opendir");
	}
	while ((ent = xreaddir(dirfd))) {
		int entlen = strlen(ent->d_name);
		int wd;
		if (!strcmp(ent->d_name, ".")
		    || !strcmp(ent->d_name, ".."))
			continue;
		memcpy(fullent+dirlen+1, ent->d_name, entlen);
		fullent[dirlen+1+entlen] = '\0';
		if (is_ignored(fullent))
			continue;
		if (!isdir(fullent))
			continue;
		wd = inotify_add_watch(ifd, fullent, MASK);
		if (wd < 0) {
			if (errno == EACCES || errno == ENOENT)
				continue;
			die_errno("inotify_add_watch");
		}
		set_dirpath(wd, fullent);
		fprintf(stderr, "%d: %s\n", wd, fullent);
		setup_watches(fullent);
	}
	if (closedir(dirfd))
		die_errno("closedir");
}

int should_exit = 0;

void make_me_exit (int sig)
{
	should_exit = 1;
}

void xsignal(int sig, sighandler_t handler)
{
	if (signal(sig, handler) == SIG_ERR)
		die_errno("signal");
}

void handle_event(struct inotify_event *ev)
{
	char *wdp;
	int i, j;
	if (ev->wd < 0)
		return;
	wdp = wdpaths[ev->wd];
	if (!wdp)
		return;
	/* inotify shows directory events in the parent too; ignore them there */
	if (ev->len && ev->mask & IN_ISDIR)
		return;
	if (ev->mask & IN_IGNORED) {
		wdpaths[ev->wd] = NULL;
		for (i = 0; i < HIST; i++) {
			if (!lru[i])
				break;
			if (strcmp(wdp, lru[i]))
				continue;
			for (j = i; j < HIST-1; j++)
				lru[j] = lru[j+1];
			lru[HIST-1] = NULL;
			break;
		}
		free(wdp);
		return;
	}
	if (ev->mask != IN_ACCESS)
		fprintf(stdout, "%08x %s %s %s\n", ev->mask, event_msg(ev->mask), wdp,
			ev->len ? ev->name : "(none)");
	if (ev->mask & IN_ISDIR && ev->mask & IN_CREATE) {
		char buf[PATH_MAX];
		strcpy(buf, wdp);
		strcat(buf, "/");
		strcat(buf, ev->name);
		setup_watches(buf);
	}
	for (i = 0; i < HIST; i++) {
		if (!lru[i])
			break;
		if (strcmp(wdp, lru[i]))
			continue;
		if (i == 0)
			return;
		for (j = i; j > 0; j--)
			lru[j] = lru[j-1];
		lru[0] = wdp;
		return;
	}
	for (j = HIST-1; j > 0; j--)
		lru[j] = lru[j-1];
	lru[0] = wdp;
}

static inline int event_len(struct inotify_event *ev)
{
	return sizeof(struct inotify_event) + ev->len;
}

void handle_inotify ()
{
	char buf[4096+PATH_MAX];
	ssize_t ret = read(ifd, &buf, sizeof(buf));
	int handled = 0;
	if (ret == -1) {
		if (errno == EINTR)
			return;
		die_errno("read");
	}
	while (handled < ret) {
		char *p = buf + handled;
		struct inotify_event *ev = (struct inotify_event *) p;
		handle_event(ev);
		handled += event_len(ev);
	}
}

const char SOCKNAME[] = "/home/thomas/.watchsock";

void send_lru(int conn)
{
	char buf[5*PATH_MAX+10];
	char *p = buf;
	int i;

	for (i = 0; i < HIST; i++) {
		int len;
		if (!lru[i])
			break;
		len = strlen(lru[i]);
		memcpy(p, lru[i], len);
		p += len;
		*p++ = '\n';
	}

	write(conn, buf, p-buf); /* errors ignored */
}

int main (int argc, char *argv[])
{
	int sock, conn;
	struct sockaddr_un addr, peer;
	socklen_t peer_sz;
	fd_set rfds;
	int ret;
	int maxfd;

	read_ignore_file();

	ifd = inotify_init();
	if (ifd < 0)
		die_errno("inotify_init");
	setup_watches("/home/thomas");
	setup_watches("/media");

	unlink(SOCKNAME); /* errors deliberately ignored */
	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0)
		die_errno("socket");
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCKNAME, sizeof(addr.sun_path)-1);
	if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)))
		die_errno("bind");
	if (listen(sock, 10))
		die_errno("listen");

	maxfd = sock;
	if (ifd > maxfd)
		maxfd = ifd;
	maxfd++;

	while (1) {
		FD_ZERO(&rfds);
		FD_SET(ifd, &rfds);
		FD_SET(sock, &rfds);
		ret = select(maxfd, &rfds, NULL, NULL, NULL);
		if (ret == -1)
			die_errno("select");
		if (!ret)
			continue;
		if (FD_ISSET(ifd, &rfds))
			handle_inotify();
		if (FD_ISSET(sock, &rfds)) {
			conn = accept(sock, (struct sockaddr *) &peer, &peer_sz);
			if (conn < 0)
				die_errno("accept");
			send_lru(conn);
			close(conn); /* errors ignored */
		}
	}

	/* we never get here, but meh */
	if (close(sock) < 0)
		die_errno("close");

	return 0;
}
