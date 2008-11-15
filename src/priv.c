/*
 * Copyright (c) 2008 Vincent Bernat <bernat@luffy.cx>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* This file contains code for privilege separation. When an error arises in
 * monitor (which is running as root), it just stops instead of trying to
 * recover. This module also contains proxies to privileged operations. In this
 * case, error can be non fatal. */

#include "lldpd.h"

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <regex.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <linux/sockios.h>
#include <netpacket/packet.h>

enum {
	PRIV_FORK,
	PRIV_CREATE_CTL_SOCKET,
	PRIV_DELETE_CTL_SOCKET,
	PRIV_GET_HOSTNAME,
	PRIV_OPEN,
	PRIV_ETHTOOL,
	PRIV_IFACE_INIT,
	PRIV_IFACE_MULTICAST,
};

static int may_read(int, void *, size_t);
static void must_read(int, void *, size_t);
static void must_write(int, void *, size_t);

int remote;			/* Other side */
int monitored = -1;		/* Child */
int sock = -1;

/* Proxies */

/* Proxy for fork */
void
priv_fork()
{
	int cmd, rc;
	cmd = PRIV_FORK;
	must_write(remote, &cmd, sizeof(int));
	must_read(remote, &rc, sizeof(int));
}

/* Proxy for ctl_create, no argument since this is the monitor that decides the
 * location of the socket */
int
priv_ctl_create()
{
	int cmd, rc;
	cmd = PRIV_CREATE_CTL_SOCKET;
	must_write(remote, &cmd, sizeof(int));
	must_read(remote, &rc, sizeof(int));
	if (rc == -1)
		return -1;
	return receive_fd(remote);
}

/* Proxy for ctl_cleanup */
void
priv_ctl_cleanup()
{
	int cmd, rc;
	cmd = PRIV_DELETE_CTL_SOCKET;
	must_write(remote, &cmd, sizeof(int));
	must_read(remote, &rc, sizeof(int));
}

/* Proxy for gethostbyname */
char *
priv_gethostbyname()
{
	int cmd, rc;
	static char *buf = NULL;
	cmd = PRIV_GET_HOSTNAME;
	must_write(remote, &cmd, sizeof(int));
	must_read(remote, &rc, sizeof(int));
	if ((buf = (char*)realloc(buf, rc+1)) == NULL)
		fatal(NULL);
	must_read(remote, buf, rc+1);
	return buf;
}

/* Proxy for open */
int
priv_open(char *file)
{
	int cmd, len, rc;
	cmd = PRIV_OPEN;
	must_write(remote, &cmd, sizeof(int));
	len = strlen(file);
	must_write(remote, &len, sizeof(int));
	must_write(remote, file, len + 1);
	must_read(remote, &rc, sizeof(int));
	if (rc == -1)
		return rc;
	return receive_fd(remote);
}

/* Proxy for ethtool ioctl */
int
priv_ethtool(char *ifname, struct ethtool_cmd *ethc)
{
	int cmd, rc, len;
	cmd = PRIV_ETHTOOL;
	must_write(remote, &cmd, sizeof(int));
	len = strlen(ifname);
	must_write(remote, &len, sizeof(int));
	must_write(remote, ifname, len + 1);
	must_read(remote, &rc, sizeof(int));
	if (rc != 0)
		return rc;
	must_read(remote, ethc, sizeof(struct ethtool_cmd));
	return rc;
}

int
priv_iface_init(struct lldpd_hardware *hardware, int master)
{
	int cmd, rc;
	cmd = PRIV_IFACE_INIT;
	must_write(remote, &cmd, sizeof(int));
	must_write(remote, &master, sizeof(int));
	must_write(remote, hardware->h_ifname, IFNAMSIZ);
	must_read(remote, &rc, sizeof(int));
	if (rc != 0)
		return rc;	/* It's errno */
	hardware->h_raw = receive_fd(remote);
	return 0;
}

int
priv_iface_multicast(char *name, u_int8_t *mac, int add)
{
	int cmd, rc;
	cmd = PRIV_IFACE_MULTICAST;
	must_write(remote, &cmd, sizeof(cmd));
	must_write(remote, name, IFNAMSIZ);
	must_write(remote, mac, ETH_ALEN);
	must_write(remote, &add, sizeof(int));
	must_read(remote, &rc, sizeof(int));
	return rc;
}

void
asroot_fork()
{
	int pid;
	char *spid;
	if (daemon(0, 0) != 0)
		fatal("[priv]: failed to detach daemon");
	if ((pid = open(LLDPD_PID_FILE,
		    O_TRUNC | O_CREAT | O_WRONLY)) == -1)
		fatal("[priv]: unable to open pid file " LLDPD_PID_FILE);
	if (asprintf(&spid, "%d\n", getpid()) == -1)
		fatal("[priv]: unable to create pid file " LLDPD_PID_FILE);
	if (write(pid, spid, strlen(spid)) == -1)
		fatal("[priv]: unable to write pid file " LLDPD_PID_FILE);
	free(spid);
	close(pid);

	/* Ack */
	must_write(remote, &pid, sizeof(int));
}

void
asroot_ctl_create()
{
	int rc;
	if ((rc = ctl_create(LLDPD_CTL_SOCKET)) == -1) {
		LLOG_WARN("[priv]: unable to create control socket");
		must_write(remote, &rc, sizeof(int));
		return;
	}
	must_write(remote, &rc, sizeof(int));
	send_fd(remote, rc);
	close(rc);
}

void
asroot_ctl_cleanup()
{
	int rc = 0;
	ctl_cleanup(LLDPD_CTL_SOCKET);

	/* Ack */
	must_write(remote, &rc, sizeof(int));
}

void
asroot_gethostbyname()
{
	struct utsname un;
	struct hostent *hp;
	int len;
	if (uname(&un) != 0)
		fatal("[priv]: failed to get system information");
	if ((hp = gethostbyname(un.nodename)) == NULL)
		fatal("[priv]: failed to get system name");
	len = strlen(hp->h_name);
	must_write(remote, &len, sizeof(int));
	must_write(remote, hp->h_name, strlen(hp->h_name) + 1);
}

void
asroot_open()
{
	const char* authorized[] = {
		"/proc/sys/net/ipv4/ip_forward",
		"/sys/class/net/[^.][^/]*/brforward",
		"/sys/class/net/[^.][^/]*/brport",
		NULL
	};
	char **f;
	char *file;
	int fd, len, rc;
	regex_t preg;

	must_read(remote, &len, sizeof(len));
	if ((file = (char *)malloc(len + 1)) == NULL)
		fatal(NULL);
	must_read(remote, file, len);
	file[len] = '\0';

	for (f=authorized; *f != NULL; f++) {
		if (regcomp(&preg, *f, REG_NOSUB) != 0)
			/* Should not happen */
			fatal("unable to compile a regex");
		if (regexec(&preg, file, 0, NULL, 0) == 0) {
			regfree(&preg);
			break;
		}
		regfree(&preg);
	}
	if (*f == NULL) {
		LLOG_WARNX("[priv]: not authorized to open %s", file);
		rc = -1;
		must_write(remote, &rc, sizeof(int));
		free(file);
		return;
	}
	if ((fd = open(file, 0)) == -1) {
		rc = -1;
		must_write(remote, &rc, sizeof(int));
		free(file);
		return;
	}
	free(file);
	must_write(remote, &fd, sizeof(int));
	send_fd(remote, fd);
	close(fd);
}

void
asroot_ethtool()
{
	struct ifreq ifr;
	struct ethtool_cmd ethc;
	int len, rc;
	char *ifname;

	memset(&ifr, 0, sizeof(ifr));
	memset(&ethc, 0, sizeof(ethc));
	must_read(remote, &len, sizeof(int));
	if ((ifname = (char*)malloc(len + 1)) == NULL)
		fatal(NULL);
	must_read(remote, ifname, len);
	ifname[len] = '\0';
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	ifr.ifr_data = (caddr_t)&ethc;
	ethc.cmd = ETHTOOL_GSET;
	if ((rc = ioctl(sock, SIOCETHTOOL, &ifr)) != 0) {
		LLOG_DEBUG("[priv]: unable to ioctl ETHTOOL for %s", ifname);
		must_write(remote, &rc, sizeof(int));
		free(ifname);
		close(sock);
		return;
	}
	must_write(remote, &rc, sizeof(int));
	must_write(remote, &ethc, sizeof(struct ethtool_cmd));
}

void
asroot_iface_init()
{
	struct sockaddr_ll sa;
	int un = 1;
	int s, master;
	char ifname[IFNAMSIZ];

	must_read(remote, &master, sizeof(int));
	must_read(remote, ifname, IFNAMSIZ);
	ifname[IFNAMSIZ-1] = '\0';

	/* Open listening socket to receive/send frames */
	if ((s = socket(PF_PACKET, SOCK_RAW,
		    htons(ETH_P_ALL))) < 0) {
		must_write(remote, &errno, sizeof(errno));
		return;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sll_family = AF_PACKET;
	sa.sll_protocol = 0;
	if (master == -1)
		sa.sll_ifindex = if_nametoindex(ifname);
	else
		sa.sll_ifindex = master;
	if (bind(s, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
		must_write(remote, &errno, sizeof(errno));
		close(s);
		return;
	}

	if (master != -1) {
		/* With bonding, we need to listen to bond device. We use
		 * setsockopt() PACKET_ORIGDEV to get physical device instead of
		 * bond device */
		if (setsockopt(s, SOL_PACKET,
			PACKET_ORIGDEV, &un, sizeof(un)) == -1) {
			LLOG_WARN("[priv]: unable to setsockopt for master bonding device of %s. "
                            "You will get inaccurate results",
			    ifname);
                }
	}
	errno = 0;
	must_write(remote, &errno, sizeof(errno));
	send_fd(remote, s);
	close(s);
}

void
asroot_iface_multicast()
{
	int add, rc = 0;
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	must_read(remote, ifr.ifr_name, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ-1] = '\0';
	must_read(remote, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
	must_read(remote, &add, sizeof(int));

	if (ioctl(sock, (add)?SIOCADDMULTI:SIOCDELMULTI,
		&ifr) < 0) {
		must_write(remote, &errno, sizeof(errno));
		return;
		}
	
	must_write(remote, &rc, sizeof(rc));
}

struct dispatch_actions {
	int				msg;
	void(*function)(void);
};

struct dispatch_actions actions[] = {
	{PRIV_FORK, asroot_fork},
	{PRIV_CREATE_CTL_SOCKET, asroot_ctl_create},
	{PRIV_DELETE_CTL_SOCKET, asroot_ctl_cleanup},
	{PRIV_GET_HOSTNAME, asroot_gethostbyname},
	{PRIV_OPEN, asroot_open},
	{PRIV_ETHTOOL, asroot_ethtool},
	{PRIV_IFACE_INIT, asroot_iface_init},
	{PRIV_IFACE_MULTICAST, asroot_iface_multicast},
	{-1, NULL}
};

/* Main loop, run as root */
void
priv_loop()
{
	int cmd;
	struct dispatch_actions *a;

	while (!may_read(remote, &cmd, sizeof(int))) {
		for (a = actions; a->function != NULL; a++) {
			if (cmd == a->msg) {
				a->function();
				break;
			}
		}
		if (a->function == NULL)
			fatal("[priv]: bogus message received");
	}
	/* Should never be there */
}

void
priv_exit()
{
	int status;
	int rc;
	if ((rc = waitpid(monitored, &status, WNOHANG)) == 0) {
		LLOG_DEBUG("[priv]: killing child");
		kill(monitored, SIGTERM);
	}
	if ((rc = waitpid(monitored, &status, WNOHANG)) == -1)
		_exit(0);
	LLOG_DEBUG("[priv]: waiting for child %d to terminate", monitored);
}

/* If priv parent gets a TERM or HUP, pass it through to child instead */
static void
sig_pass_to_chld(int sig)
{
	int oerrno = errno;
	if (monitored != -1)
		kill(monitored, sig);
	errno = oerrno;
}

/* if parent gets a SIGCHLD, it will exit */
static void
sig_chld(int sig)
{
	LLOG_DEBUG("[priv]: received signal %d, exiting", sig);
	priv_exit();
}

/* Initialization */
void
priv_init(char *chrootdir)
{
	int pair[2];
	struct passwd *user;
	uid_t uid;
	struct group *group;
	gid_t gid;
	gid_t gidset[1];

	/* Create socket pair */
	if (socketpair(AF_LOCAL, SOCK_DGRAM, PF_UNSPEC, pair) < 0)
		fatal("[priv]: unable to create socket pair for privilege separation");

	/* Get users */
	if ((user = getpwnam(PRIVSEP_USER)) == NULL)
		fatal("[priv]: no " PRIVSEP_USER " user for privilege separation");
	uid = user->pw_uid;
	if ((group = getgrnam(PRIVSEP_GROUP)) == NULL)
		fatal("[priv]: no " PRIVSEP_GROUP " group for privilege separation");
	gid = group->gr_gid;

	/* Spawn off monitor */
	if ((monitored = fork()) < 0)
		fatal("[priv]: unable to fork monitor");
	switch (monitored) {
	case 0:
		/* We are in the children, drop privileges */
		if (chroot(chrootdir) == -1)
			fatal("[priv]: unable to chroot");
		if (chdir("/") != 0)
			fatal("[priv]: unable to chdir");
		gidset[0] = gid;
		if (setresgid(gid, gid, gid) == -1)
			fatal("[priv]: setresgid() failed");
		if (setgroups(1, gidset) == -1)
			fatal("[priv]: setgroups() failed");
		if (setresuid(uid, uid, uid) == -1)
			fatal("[priv]: setresuid() failed");
		remote = pair[0];
		close(pair[1]);
		break;
	default:
		/* We are in the monitor */
		remote = pair[1];
		close(pair[0]);
		if (atexit(priv_exit) != 0)
			fatal("[priv]: unable to set exit function");
		if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
			fatal("[priv]: unable to get a socket");
		}

		signal(SIGALRM, sig_pass_to_chld);
		signal(SIGTERM, sig_pass_to_chld);
		signal(SIGHUP, sig_pass_to_chld);
		signal(SIGINT, sig_pass_to_chld);
		signal(SIGQUIT, sig_pass_to_chld);
		signal(SIGCHLD, sig_chld);
		priv_loop();
		exit(0);
	}
	
}

/* Stolen from sbin/pflogd/privsep.c from OpenBSD */
/*
 * Copyright (c) 2003 Can Erkin Acar
 * Copyright (c) 2003 Anil Madhavapeddy <anil@recoil.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* Read all data or return 1 for error.  */
static int
may_read(int fd, void *buf, size_t n)
{
	char *s = buf;
	ssize_t res, pos = 0;

	while (n > pos) {
		res = read(fd, s + pos, n - pos);
		switch (res) {
		case -1:
			if (errno == EINTR || errno == EAGAIN)
				continue;
		case 0:
			return (1);
		default:
			pos += res;
		}
	}
	return (0);
}

/* Read data with the assertion that it all must come through, or
 * else abort the process.  Based on atomicio() from openssh. */
static void
must_read(int fd, void *buf, size_t n)
{
	char *s = buf;
	ssize_t res, pos = 0;

	while (n > pos) {
		res = read(fd, s + pos, n - pos);
		switch (res) {
		case -1:
			if (errno == EINTR || errno == EAGAIN)
				continue;
		case 0:
			_exit(0);
		default:
			pos += res;
		}
	}
}

/* Write data with the assertion that it all has to be written, or
 * else abort the process.  Based on atomicio() from openssh. */
static void
must_write(int fd, void *buf, size_t n)
{
	char *s = buf;
	ssize_t res, pos = 0;

	while (n > pos) {
		res = write(fd, s + pos, n - pos);
		switch (res) {
		case -1:
			if (errno == EINTR || errno == EAGAIN)
				continue;
		case 0:
			_exit(0);
		default:
			pos += res;
		}
	}
}