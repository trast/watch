#define _GNU_SOURCE

#include <errno.h>
#include <sys/inotify.h>
#include <pthread.h>
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

const char *ignore_patterns[] = {
    "/home/thomas/Mail",
    "/home/thomas/News",
    "/home/thomas/musik",
    "/home/thomas/filme",
    "*/.git",
    "*/.svn",
    "/home/thomas/.*",
    "*/tiles",
    "/home/thomas/dl/radar",
    "/home/thomas/g1/maps/*",
    "/home/thomas/eth/vc_simulation/*",
    "/home/thomas/logs",
    "*/.depend",
    "*/backup",
    NULL
};

char **wdpaths = NULL;
int wd_alloc = 0;

#define HIST 5
const char *lru[HIST] = {0};
pthread_mutex_t lru_lock = PTHREAD_MUTEX_INITIALIZER;

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

int is_ignored(const char *path)
{
	const char **pp = ignore_patterns;
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

void *watcher_thread (void *unused)
{
	char buf[4096+PATH_MAX];
	while (!should_exit) {
		ssize_t ret = read(ifd, &buf, sizeof(buf));
		int handled = 0;
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			die_errno("read");
		}
		pthread_mutex_lock(&lru_lock);
		while (handled < ret) {
			char *p = buf + handled;
			struct inotify_event *ev = (struct inotify_event *) p;
			handle_event(ev);
			handled += event_len(ev);
		}
		pthread_mutex_unlock(&lru_lock);
	}
	return NULL;
}

const char SOCKNAME[] = "/home/thomas/.watchsock";

void send_lru(int conn)
{
	char buf[5*PATH_MAX+10];
	char *p = buf;
	int i;

	pthread_mutex_lock(&lru_lock);
	for (i = 0; i < HIST; i++) {
		int len;
		if (!lru[i])
			break;
		len = strlen(lru[i]);
		memcpy(p, lru[i], len);
		p += len;
		*p++ = '\n';
	}
	pthread_mutex_unlock(&lru_lock);
	write(conn, buf, p-buf); /* errors ignored */
}

int main (int argc, char *argv[])
{
	pthread_t wt;
	int sock, conn;
	struct sockaddr_un addr, peer;
	socklen_t peer_sz;

	ifd = inotify_init();
	if (ifd < 0)
		die_errno("inotify_init");
	setup_watches("/home/thomas");
	setup_watches("/media");

	xsignal(SIGINT, make_me_exit);
	xsignal(SIGTERM, make_me_exit);

	unlink(SOCKNAME); /* errors deliberately ignored */
	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0)
		die_errno("socket");
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCKNAME, sizeof(addr.sun_path)-1);
	if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)))
		die_errno("bind");

	if (pthread_create(&wt, NULL, watcher_thread, NULL))
		die_errno("pthread_create");

	while (!should_exit) {
		if (listen(sock, 10))
			die_errno("listen");
		conn = accept(sock, (struct sockaddr *) &peer, &peer_sz);
		if (conn < 0)
			die_errno("accept");
		send_lru(conn);
		close(conn); /* errors ignored */
	}

	if (close(sock) < 0)
		die_errno("close");

	pthread_kill(wt, SIGINT);
	if (pthread_join(wt, NULL))
		die_errno("pthread_join");

	return 0;
}
