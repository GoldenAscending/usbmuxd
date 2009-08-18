/*
	usbmuxd - iPhone/iPod Touch USB multiplex server daemon

Copyright (C) 2009	Hector Martin "marcan" <hector@marcansoft.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 or version 3.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#define _BSD_SOURCE

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <getopt.h>
#include <pwd.h>
#include <grp.h>

#include "log.h"
#include "usb.h"
#include "device.h"
#include "client.h"

static const char *socket_path = "/var/run/usbmuxd";
static const char *lockfile = "/var/run/usbmuxd.lock";

int should_exit;

static int verbose = 0;
static int foreground = 0;
static int drop_privileges = 0;
static const char *drop_user = "usbmux";
static int opt_udev = 0;
static int opt_exit = 0;
static int exit_signal = 0;

int create_socket(void) {
	struct sockaddr_un bind_addr;
	int listenfd;

	if(unlink(socket_path) == -1 && errno != ENOENT) {
		usbmuxd_log(LL_FATAL, "unlink(%s) failed: %s", socket_path, strerror(errno));
		return -1;
	}

	listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listenfd == -1) {
		usbmuxd_log(LL_FATAL, "socket() failed: %s", strerror(errno));
		return -1;
	}

	bzero(&bind_addr, sizeof(bind_addr));
	bind_addr.sun_family = AF_UNIX;
	strcpy(bind_addr.sun_path, socket_path);
	if (bind(listenfd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
		usbmuxd_log(LL_FATAL, "bind() failed: %s", strerror(errno));
		return -1;
	}

	// Start listening
	if (listen(listenfd, 5) != 0) {
		usbmuxd_log(LL_FATAL, "listen() failed: %s", strerror(errno));
		return -1;
	}

	chmod(socket_path, 0666);

	return listenfd;
}

void handle_signal(int sig)
{
	if (sig != SIGUSR1) {
		usbmuxd_log(LL_NOTICE,"Caught signal %d, exiting", sig);
		should_exit = 1;
	} else {
		if(opt_udev) {
			usbmuxd_log(LL_INFO, "Caught SIGUSR1, checking if we can terminate (no more devices attached)...");
			if (device_get_count() > 0) {
				// we can't quit, there are still devices attached.
				usbmuxd_log(LL_NOTICE, "Refusing to terminate, there are still devices attached. Kill me with signal 15 (TERM) to force quit.");
			} else {
				// it's safe to quit
				should_exit = 1;
			}
		} else {
			usbmuxd_log(LL_INFO, "Caught SIGUSR1 but we weren't started in --udev mode, ignoring");
		}
	}
}

void set_signal_handlers(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = handle_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
}

int main_loop(int listenfd)
{
	int to, cnt, i, dto;
	struct fdlist pollfds;
	
	while(!should_exit) {
		usbmuxd_log(LL_FLOOD, "main_loop iteration");
		to = usb_get_timeout();
		usbmuxd_log(LL_FLOOD, "USB timeout is %d ms", to);
		dto = device_get_timeout();
		usbmuxd_log(LL_FLOOD, "Device timeout is %d ms", to);
		if(dto < to)
			to = dto;
		
		fdlist_create(&pollfds);
		fdlist_add(&pollfds, FD_LISTEN, listenfd, POLLIN);
		usb_get_fds(&pollfds);
		client_get_fds(&pollfds);
		usbmuxd_log(LL_FLOOD, "fd count is %d", pollfds.count);
		
		cnt = poll(pollfds.fds, pollfds.count, to);
		usbmuxd_log(LL_FLOOD, "poll() returned %d", cnt);
		
		if(cnt == -1) {
			if(errno == EINTR && should_exit) {
				usbmuxd_log(LL_INFO, "event processing interrupted");
				fdlist_free(&pollfds);
				return 0;
			}
		} else if(cnt == 0) {
			if(usb_process() < 0) {
				usbmuxd_log(LL_FATAL, "usb_process() failed");
				fdlist_free(&pollfds);
				return -1;
			}
			device_check_timeouts();
		} else {
			int done_usb = 0;
			for(i=0; i<pollfds.count; i++) {
				if(pollfds.fds[i].revents) {
					if(!done_usb && pollfds.owners[i] == FD_USB) {
						if(usb_process() < 0) {
							usbmuxd_log(LL_FATAL, "usb_process() failed");
							fdlist_free(&pollfds);
							return -1;
						}
						done_usb = 1;
					}
					if(pollfds.owners[i] == FD_LISTEN) {
						if(client_accept(listenfd) < 0) {
							usbmuxd_log(LL_FATAL, "client_accept() failed");
							fdlist_free(&pollfds);
							return -1;
						}
					}
					if(pollfds.owners[i] == FD_CLIENT) {
						client_process(pollfds.fds[i].fd, pollfds.fds[i].revents);
					}
				}
			}
		}
		fdlist_free(&pollfds);
	}
	return 0;
}

/**
 * make this program run detached from the current console
 */
static int daemonize()
{
	pid_t pid;
	pid_t sid;

	// already a daemon
	if (getppid() == 1)
		return 0;

	pid = fork();
	if (pid < 0) {
		exit(EXIT_FAILURE);
	}

	if (pid > 0) {
		// exit parent process
		exit(EXIT_SUCCESS);
	}
	// At this point we are executing as the child process

	// Change the file mode mask
	umask(0);

	// Create a new SID for the child process
	sid = setsid();
	if (sid < 0) {
		return -1;
	}
	// Change the current working directory.
	if ((chdir("/")) < 0) {
		return -2;
	}
	// Redirect standard files to /dev/null
	if (!freopen("/dev/null", "r", stdin)) {
		usbmuxd_log(LL_ERROR, "ERROR: redirection of stdin failed.");
	}
	if (!freopen("/dev/null", "w", stdout)) {
		usbmuxd_log(LL_ERROR, "ERROR: redirection of stdout failed.");
	}
	if (!freopen("/dev/null", "w", stderr)) {
		usbmuxd_log(LL_ERROR, "ERROR: redirection of stderr failed.");
	}

	return 0;
}

static void usage()
{
	printf("usage: usbmuxd [options]\n");
	printf("\t-h|--help                 Print this message.\n");
	printf("\t-v|--verbose              Be verbose (use twice or more to increase).\n");
	printf("\t-f|--foreground           Do not daemonize (implies one -v).\n");
	printf("\t-u|--user[=USER]          Change to this user after startup (needs usb privileges).\n");
	printf("\t                          If USER is not specified, defaults to usbmux.\n");
	printf("\t-d|--udev                 Run in udev operation mode.\n");
	printf("\t-x|--exit                 Tell a running instance to exit if there are no devices\n");
	printf("\t                          connected (must be in udev mode).\n");
	printf("\t-X|--force-exit           Tell a running instance to exit, even if there are still\n");
	printf("\t                          devices connected (always works).\n");
	printf("\n");
}

static void parse_opts(int argc, char **argv)
{
	static struct option longopts[] = {
		{"help", 0, NULL, 'h'},
		{"foreground", 0, NULL, 'f'},
		{"verbose", 0, NULL, 'v'},
		{"user", 2, NULL, 'u'},
		{"udev", 0, NULL, 'd'},
		{"exit", 0, NULL, 'x'},
		{"force-exit", 0, NULL, 'X'},
		{NULL, 0, NULL, 0}
	};
	int c;

	while (1) {
		c = getopt_long(argc, argv, "hfvdu::xX", longopts, (int *) 0);
		if (c == -1) {
			break;
		}

		switch (c) {
		case 'h':
			usage();
			exit(0);
		case 'f':
			foreground = 1;
			break;
		case 'v':
			++verbose;
			break;
		case 'u':
			drop_privileges = 1;
			if(optarg)
				drop_user = optarg;
			break;
		case 'd':
			opt_udev = 1;
			break;
		case 'x':
			opt_exit = 1;
			exit_signal = SIGUSR1;
			break;
		case 'X':
			opt_exit = 1;
			exit_signal = SIGTERM;
			break;
		default:
			usage();
			exit(2);
		}
	}
}

int main(int argc, char *argv[])
{
	int listenfd;
	int res = 0;
	FILE *lfd = NULL;
	struct flock lock;

	parse_opts(argc, argv);

	argc -= optind;
	argv += optind;

	if (!foreground) {
		verbose += LL_WARNING;
		log_enable_syslog();
	} else {
		verbose += LL_NOTICE;
	}

	/* set log level to specified verbosity */
	log_level = verbose;

	usbmuxd_log(LL_NOTICE, "usbmux v0.1 starting up");
	should_exit = 0;

	set_signal_handlers();

	lfd = fopen(lockfile, "r");
	if (lfd) {
		lock.l_type = 0;
		lock.l_whence = SEEK_SET;
		lock.l_start = 0;
		lock.l_len = 0;
		fcntl(fileno(lfd), F_GETLK, &lock);
		fclose(lfd);
		if (lock.l_type != F_UNLCK) {
			if (opt_exit) {
				if (lock.l_pid && !kill(lock.l_pid, 0)) {
					usbmuxd_log(LL_NOTICE, "sending signal %d to instance with pid %d", exit_signal, lock.l_pid);
					if (kill(lock.l_pid, exit_signal) < 0) {
						usbmuxd_log(LL_FATAL, "Error: could not deliver signal %d to pid %d", exit_signal, lock.l_pid);
					}
					res = 0;
					goto terminate;
				} else {
					usbmuxd_log(LL_ERROR, "Could not determine pid of the other running instance!");
					res = -1;
					goto terminate;
				}
			} else {
				if (!opt_udev) {
					usbmuxd_log(LL_ERROR, "Another instance is already running (pid %d). exiting.", lock.l_pid);
					res = -1;
				} else {
					usbmuxd_log(LL_NOTICE, "Another instance is already running (pid %d). exiting.", lock.l_pid);
					res = 0;
				}
				goto terminate;
			}
		}
	}

	if (opt_exit) {
		usbmuxd_log(LL_NOTICE, "No running instance found, none killed. exiting.");
		goto terminate;
	}

	usbmuxd_log(LL_INFO, "Creating socket");
	listenfd = create_socket();
	if(listenfd < 0)
		return 1;

	client_init();
	device_init();
	usbmuxd_log(LL_INFO, "Initializing USB");
	if((res = usb_init()) < 0)
		return 2;
	usbmuxd_log(LL_INFO, "%d device%s detected", res, (res==1)?"":"s");
	
	usbmuxd_log(LL_NOTICE, "Initialization complete");

	if (!foreground) {
		if (daemonize() < 0) {
			fprintf(stderr, "usbmuxd: FATAL: Could not daemonize!\n");
			usbmuxd_log(LL_ERROR, "FATAL: Could not daemonize!");
			log_disable_syslog();
			exit(EXIT_FAILURE);
		}
	}

	// now open the lockfile and place the lock
	lfd = fopen(lockfile, "w");
	if (lfd) {
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		lock.l_start = 0;
		lock.l_len = 0;
		if (fcntl(fileno(lfd), F_SETLK, &lock) == -1) {
			usbmuxd_log(LL_FATAL, "Lockfile locking failed!");
			log_disable_syslog();
			exit(EXIT_FAILURE);
		}
	}

	// drop elevated privileges
	if (drop_privileges && (getuid() == 0 || geteuid() == 0)) {
		struct passwd *pw = getpwnam(drop_user);
		if (!pw) {
			usbmuxd_log(LL_FATAL, "Dropping privileges failed, check if user '%s' exists!", drop_user);
			res = 1;
			goto terminate;
		}

		if ((res = initgroups(drop_user, pw->pw_gid)) < 0) {
			usbmuxd_log(LL_FATAL, "Failed to drop privileges (cannot set supplementary groups)");
			goto terminate;
		}
		if ((res = setgid(pw->pw_gid)) < 0) {
			usbmuxd_log(LL_FATAL, "Failed to drop privileges (cannot set group ID to %d)", pw->pw_gid);
			goto terminate;
		}
		if ((res = setuid(pw->pw_uid)) < 0) {
			usbmuxd_log(LL_FATAL, "Failed to drop privileges (cannot set user ID to %d)", pw->pw_uid);
			goto terminate;
		}

		// security check
		if (setuid(0) != -1) {
			usbmuxd_log(LL_FATAL, "Failed to drop privileges properly!");
			res = 1;
			goto terminate;
		}
		if (getuid() != pw->pw_uid || getgid() != pw->pw_gid) {
			usbmuxd_log(LL_FATAL, "Failed to drop privileges properly!");
			res = 1;
			goto terminate;
		}
		usbmuxd_log(LL_NOTICE, "Successfully dropped privileges to '%s'", drop_user);
	}

	res = main_loop(listenfd);
	if(res < 0)
		usbmuxd_log(LL_FATAL, "main_loop failed");

	usbmuxd_log(LL_NOTICE, "usbmux shutting down");
	device_kill_connections();
	usb_shutdown();
	device_shutdown();
	client_shutdown();
	usbmuxd_log(LL_NOTICE, "Shutdown complete");

terminate:
	log_disable_syslog();

	if(res < 0)
		return -res;
	return 0;
}