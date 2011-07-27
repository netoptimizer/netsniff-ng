/*
 * curvetun - the cipherspace wormhole creator
 * Part of the netsniff-ng project
 * By Daniel Borkmann <daniel@netsniff-ng.org>
 * Copyright 2011 Daniel Borkmann <dborkma@tik.ee.ethz.ch>,
 * Subject to the GPL.
 *
 * This is a lightweight multiuser IP tunnel based on Daniel J.
 * Bernsteins Networking and Cryptography library (NaCl). The tunnel
 * acts fully in non-blocking I/O and uses epoll(2) for event
 * notification. Network traffic is being compressed and encrypted
 * for secure communication. The tunnel supports IPv4 via UDP, IPv4
 * via TCP, IPv6 via UDP and IPv6 via TCP. Flows are scheduled for
 * processing in a CPU-local manner.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <errno.h>
#include <stdbool.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "xmalloc.h"
#include "netdev.h"
#include "version.h"
#include "stun.h"
#include "die.h"
#include "strlcpy.h"
#include "signals.h"
#include "curvetun.h"
#include "curve.h"
#include "deflate.h"
#include "usermgmt.h"
#include "servmgmt.h"
#include "write_or_die.h"
#include "crypto_verify_32.h"
#include "crypto_box_curve25519xsalsa20poly1305.h"
#include "crypto_scalarmult_curve25519.h"
#include "crypto_auth_hmacsha512256.h"

void *memset (void *__s, int __c, size_t __n) __attribute__ ((__noinline__));

#define CURVETUN_ENTROPY_SOURCE	"/dev/random"

enum working_mode {
	MODE_UNKNOW,
	MODE_KEYGEN,
	MODE_EXPORT,
	MODE_TOKEN,
	MODE_DUMPC,
	MODE_DUMPS,
	MODE_CLIENT,
	MODE_SERVER,
};

sig_atomic_t sigint = 0;

static const char *short_options = "kxc::svhp:t:d:uCS46HDA";

static struct option long_options[] = {
	{"client", optional_argument, 0, 'c'},
	{"dev", required_argument, 0, 'd'},
	{"port", required_argument, 0, 'p'},
	{"stun", required_argument, 0, 't'},
	{"keygen", no_argument, 0, 'k'},
	{"export", no_argument, 0, 'x'},
	{"auth-token", no_argument, 0, 'A'},
	{"dumpc", no_argument, 0, 'C'},
	{"dumps", no_argument, 0, 'S'},
	{"server", no_argument, 0, 's'},
	{"udp", no_argument, 0, 'u'},
	{"ipv4", no_argument, 0, '4'},
	{"ipv6", no_argument, 0, '6'},
	{"nofork", no_argument, 0, 'D'},
	{"version", no_argument, 0, 'v'},
	{"help", no_argument, 0, 'h'},
	{0, 0, 0, 0}
};

static void signal_handler(int number)
{
	switch (number) {
	case SIGINT:
		sigint = 1;
		break;
	case SIGHUP:
		break;
	default:
		break;
	}
}

static void header(void)
{
	printf("%s%s%s\n", colorize_start(bold), "curvetun "
	       VERSION_STRING, colorize_end());
}

static void help(void)
{
	printf("\ncurvetun %s, lightweight curve25519-based multiuser IP tunnel\n",
	       VERSION_STRING);
	printf("http://www.netsniff-ng.org\n\n");
	printf("Usage: curvetun [options]\n");
	printf("Options:\n");
	printf("  -k|--keygen             Generate public/private keypair\n");
	printf("  -x|--export             Export your public data for remote servers\n");
	printf("  -A|--auth-token         Export your shared auth_token for remote clients\n");
	printf("  -C|--dumpc              Dump parsed clients\n");
	printf("  -S|--dumps              Dump parsed servers\n");
	printf("  -D|--nofork             Do not daemonize\n");
	printf("  -d|--dev <tun>          Networking tunnel device, e.g. tun0\n");
	printf(" Client settings:\n");
	printf("  -c|--client[=alias]     Client mode, server alias optional\n");
	printf(" Server settings:\n");
	printf("  -s|--server             Server mode\n");
	printf("  -p|--port <num>         Port number (mandatory)\n");
	printf("  -t|--stun <server>      Show public IP/Port mapping via STUN\n");
	printf("  -u|--udp                Use UDP as carrier instead of TCP\n");
	printf("  -4|--ipv4               Tunnel devices are IPv4\n");
	printf("  -6|--ipv6               Tunnel devices are IPv6\n");
	printf("                          (default: same as carrier protocol)\n");
	printf(" Misc:\n");
	printf("  -v|--version            Print version\n");
	printf("  -h|--help               Print this help\n");
	printf("\n");
	printf("Example:\n");
	printf("  A. Keygen example:\n");
	printf("      1. curvetun --keygen\n");
	printf("      2. Now the following files are done setting up:\n");
	printf("           ~/.curvetun/priv.key   - Your private key\n");
	printf("           ~/.curvetun/pub.key    - Your public key\n");
	printf("           ~/.curvetun/username   - Your username\n");
	printf("           ~/.curvetun/auth_token - Your server auth token\n");
	printf("      3. To export your key for remote servers, use:\n");
	printf("           curvetun --export\n");
	printf("  B. Server:\n");
	printf("      1. curvetun --server -4 --port 6666 --stun stunserver.org\n");
	printf("      2. ifconfig curves0 up\n");
	printf("      2. ifconfig curves0 10.0.0.1/24\n");
	printf("      3. (setup route)\n");
	printf("  C. Client:\n");
	printf("      1. curvetun --client\n");
	printf("      2. ifconfig curvec0 up\n");
	printf("      2. ifconfig curvec0 10.0.0.2/24\n");
	printf("      3. (setup route)\n");
	printf("  Where both participants have the following files specified ...\n");
	printf("   ~/.curvetun/clients - Participants the server accepts\n");
	printf("        line-format:   username;pubkey\n");
	printf("   ~/.curvetun/servers - Possible servers the client can connect to\n");
	printf("        line-format:   alias;serverip|servername;port;udp|tcp;pubkey;auth_token\n");
	printf("  ... and are synced to an ntpd!\n");
	printf("\n");
	printf("Note:\n");
	printf("  There is no default port specified, so that you are forced\n");
	printf("  to select your own! For status messages see syslog!\n");
	printf("\n");
	printf("Secret ingredient: 7647-14-5\n");
	printf("\n");
	printf("Please report bugs to <bugs@netsniff-ng.org>\n");
	printf("Copyright (C) 2011 Daniel Borkmann <dborkma@tik.ee.ethz.ch>,\n");
	printf("License: GNU GPL version 2\n");
	printf("This is free software: you are free to change and redistribute it.\n");
	printf("There is NO WARRANTY, to the extent permitted by law.\n\n");

	die();
}

static void version(void)
{
	printf("\ncurvetun %s, lightweight curve25519-based multiuser IP tunnel\n",
               VERSION_STRING);
	printf("Build: %s\n", BUILD_STRING);
	printf("zLib: %s\n", z_get_version());
	printf("http://www.netsniff-ng.org\n\n");
	printf("Please report bugs to <bugs@netsniff-ng.org>\n");
	printf("Copyright (C) 2011 Daniel Borkmann <dborkma@tik.ee.ethz.ch>,\n");
	printf("License: GNU GPL version 2\n");
	printf("This is free software: you are free to change and redistribute it.\n");
	printf("There is NO WARRANTY, to the extent permitted by law.\n\n");

	die();
}

static void towel(void)
{
	printf("                \\:.   /                 _.-----._\n");
	printf("                 `---'          \\)|)_ ,'         `. _))|)\n");
	printf("        |                        );-'/             \\`-:(\n");
	printf("      -(o)-            .        //  :               :  \\\\   .\n");
	printf("    .   |                      //_,'; ,.         ,. |___\\\\\n");
	printf("           .                   `---':(  `-.___.-'  );----'\n");
	printf("                                     \\`. `'-'-'' ,'/\n");
	printf("                                      `.`-.,-.-.','\n");
	printf("   DON'T FORGET TO APPEND               ``---\\` :\n");
	printf("    YOUR TOWEL INTO THE         *             `.'       *\n");
	printf("          PAYLOAD                         .        .\n\n");
	printf("                                      (by Sebastian Stoecker)\n");

	die();
}

static void check_file_or_die(char *home, char *file, int maybeempty)
{
	char path[PATH_MAX];
	struct stat st;

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s/%s", home, file);
	path[sizeof(path) - 1] = 0;

	if (stat(path, &st))
		panic("No such file %s! Type --help for further information\n",
		      path);

	if (!S_ISREG(st.st_mode))
		panic("%s is not a regular file!\n", path);

	if ((st.st_mode & ~S_IFREG) != (S_IRUSR | S_IWUSR))
		panic("You have set too many permissions on %s (%o)!\n",
		      path, st.st_mode);

	if (maybeempty == 0 && st.st_size == 0)
		panic("%s is empty!\n", path);
}

static void check_config_exists_or_die(char *home)
{
	if (!home)
		panic("No home dir specified!\n");
	check_file_or_die(home, FILE_CLIENTS, 1);
	check_file_or_die(home, FILE_SERVERS, 1);
	check_file_or_die(home, FILE_PRIVKEY, 0);
	check_file_or_die(home, FILE_PUBKEY, 0);
	check_file_or_die(home, FILE_USERNAM, 0);
	check_file_or_die(home, FILE_TOKEN, 0);
}

static char *fetch_home_dir(void)
{
	char *home = getenv("HOME");
	if (!home)
		panic("No HOME defined!\n");
	return home;
}

static void write_username(char *home)
{
	int fd, ret;
	char path[PATH_MAX], *eof;
	char user[512];

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s/%s", home, FILE_USERNAM);
	path[sizeof(path) - 1] = 0;

	printf("Username: [%s] ", getenv("USER"));
	fflush(stdout);

	memset(user, 0, sizeof(user));
	eof = fgets(user, sizeof(user), stdin);
	user[sizeof(user) - 1] = 0;
	user[strlen(user) - 1] = 0; /* omit last \n */

	if (strlen(user) == 0)
		strlcpy(user, getenv("USER"), sizeof(user));

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd < 0)
		panic("Cannot open your username file!\n");
	ret = write(fd, user, strlen(user));
	if (ret != strlen(user))
		panic("Could not write username!\n");
	close(fd);

	info("Username written to %s!\n", path);
}

static void create_curvedir(char *home)
{
	int ret, fd;
	char path[PATH_MAX];

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s/%s", home, ".curvetun/");
	path[sizeof(path) - 1] = 0;

	errno = 0;
	ret = mkdir(path, S_IRWXU);
	if (ret < 0 && errno != EEXIST)
		panic("Cannot create curvetun dir!\n");

	info("curvetun directory %s created!\n", path);

	/* We also create empty files for clients and servers! */
	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s/%s", home, FILE_CLIENTS);
	path[sizeof(path) - 1] = 0;

	fd = open(path, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd < 0)
		panic("Cannot open clients file!\n");
	close(fd);

	info("Empty client file written to %s!\n", path);

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s/%s", home, FILE_SERVERS);
	path[sizeof(path) - 1] = 0;

	fd = open(path, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd < 0)
		panic("Cannot open servers file!\n");
	close(fd);

	info("Empty server file written to %s!\n", path);
}

static void create_keypair(char *home)
{
	int fd;
	ssize_t ret;
	unsigned char publickey[crypto_box_curve25519xsalsa20poly1305_PUBLICKEYBYTES] = { 0 };
	unsigned char secretkey[crypto_box_curve25519xsalsa20poly1305_SECRETKEYBYTES] = { 0 };
	char path[PATH_MAX];
	const char * errstr = NULL;
	int err = 0;

	info("Reading from %s (this may take a while) ...\n", CURVETUN_ENTROPY_SOURCE);

	fd = open_or_die(CURVETUN_ENTROPY_SOURCE, O_RDONLY);
	ret = read_exact(fd, secretkey, sizeof(secretkey), 0);

	if (ret != sizeof(secretkey)) {
		err = EIO;
		errstr = "Cannot read from "CURVETUN_ENTROPY_SOURCE"!\n";
		goto out;
	}

	close(fd);

	crypto_scalarmult_curve25519_base(publickey, secretkey);

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s/%s", home, FILE_PUBKEY);
	path[sizeof(path) - 1] = 0;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);

	if (fd < 0) {
		err = EIO;
		errstr = "Cannot open pubkey file!\n";
		goto out;
	}

	ret = write(fd, publickey, sizeof(publickey));

	if (ret != sizeof(publickey)) {
		err = EIO;
		errstr = "Cannot write public key!\n";
		goto out;
	}

	close(fd);

	info("Public key written to %s!\n", path);

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s/%s", home, FILE_PRIVKEY);
	path[sizeof(path) - 1] = 0;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);

	if (fd < 0) {
		err = EIO;
		errstr = "Cannot open privkey file!\n";
		goto out;
	}

	ret = write(fd, secretkey, sizeof(secretkey));

	if (ret != sizeof(secretkey)) {
		err = EIO;
		errstr = "Cannot write private key!\n";
		goto out;
	}

out:
	close(fd);

	memset(publickey, 0, sizeof(publickey));
	memset(secretkey, 0, sizeof(secretkey));

	if (err)
		panic("%s: %s", errstr, strerror(errno));
	else
		info("Private key written to %s!\n", path);
}

static void create_token(char *home)
{
	int fd;
	ssize_t ret;
	unsigned char token[crypto_auth_hmacsha512256_KEYBYTES];
	char path[PATH_MAX];
	int err = 0;
	const char * errstr = NULL;

	info("Reading from %s (this may take a while) ...\n", CURVETUN_ENTROPY_SOURCE);

	fd = open_or_die(CURVETUN_ENTROPY_SOURCE, O_RDONLY);
	ret = read_exact(fd, token, sizeof(token), 0);

	if (ret != sizeof(token)) {
		err = EIO;
		errstr = "Cannot read from "CURVETUN_ENTROPY_SOURCE"!\n";
		goto out;
	}

	close(fd);

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s/%s", home, FILE_TOKEN);
	path[sizeof(path) - 1] = 0;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);

	if (fd < 0) {
		err = EIO;
		errstr = "Cannot open pubkey file!\n";
		goto out;
	}

	ret = write(fd, token, sizeof(token));

	if (ret != sizeof(token)) {
		err = EIO;
		errstr = "Cannot write auth token!\n";
		goto out;
	}

out:
	close(fd);

	memset(token, 0, sizeof(token));

	if(err)
		panic("%s: %s", errstr, strerror(errno));
	else
		info("Auth token written to %s!\n", path);
}

static void check_config_keypair_or_die(char *home)
{
	int fd;
	ssize_t ret;
	int err;
	const char * errstr = NULL;
	unsigned char publickey[crypto_box_curve25519xsalsa20poly1305_PUBLICKEYBYTES];
	unsigned char publicres[crypto_box_curve25519xsalsa20poly1305_PUBLICKEYBYTES];
	unsigned char secretkey[crypto_box_curve25519xsalsa20poly1305_SECRETKEYBYTES];
	char path[PATH_MAX];

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s/%s", home, FILE_PRIVKEY);
	path[sizeof(path) - 1] = 0;

	fd = open(path, O_RDONLY);

	if (fd < 0) {
		err = EIO;
		errstr = "Cannot open privkey file!\n";
		goto out;
	}

	ret = read(fd, secretkey, sizeof(secretkey));

	if (ret != sizeof(secretkey)) {
		err = EIO;
		errstr = "Cannot read private key!\n";
		goto out;
	}

	close(fd);

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s/%s", home, FILE_PUBKEY);
	path[sizeof(path) - 1] = 0;

	fd = open(path, O_RDONLY);

	if (fd < 0) {
		err = EIO;
		errstr = "Cannot open pubkey file!\n";
		goto out;
	}

	ret = read(fd, publickey, sizeof(publickey));

	if (ret != sizeof(publickey)) {
		err = EIO;
		errstr = "Cannot read public key!\n";
		goto out;
	}

	crypto_scalarmult_curve25519_base(publicres, secretkey);

	err = crypto_verify_32(publicres, publickey);

	if (err) {
		err = EINVAL;
		errstr = "WARNING: your keypair is corrupted!!! You need to "
			 "generate new keys!!!\n";
		goto out;
	}

out:
	close(fd);

	memset(publickey, 0, sizeof(publickey));
	memset(publicres, 0, sizeof(publicres));
	memset(secretkey, 0, sizeof(secretkey));

	if (err)
		panic("%s: %s\n", errstr, strerror(errno));
}

static int main_keygen(char *home)
{
	create_curvedir(home);
	write_username(home);
	create_keypair(home);
	create_token(home);
	check_config_keypair_or_die(home);
	return 0;
}

static int main_token(char *home)
{
	int fd, i;
	ssize_t ret;
	char path[PATH_MAX], tmp[crypto_auth_hmacsha512256_KEYBYTES];

	check_config_exists_or_die(home);

	printf("Your auth token for clients:\n\n");

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s/%s", home, FILE_TOKEN);
	path[sizeof(path) - 1] = 0;

	fd = open_or_die(path, O_RDONLY);
	ret = read(fd, tmp, sizeof(tmp));
	if (ret != crypto_auth_hmacsha512256_KEYBYTES)
		panic("Cannot read auth token!\n");
	for (i = 0; i < ret; ++i)
		if (i == ret - 1)
			printf("%02x\n\n", (unsigned char) tmp[i]);
		else
			printf("%02x:", (unsigned char) tmp[i]);
	close(fd);
	fflush(stdout);

	return 0;
}

static int main_export(char *home)
{
	int fd, i;
	ssize_t ret;
	char path[PATH_MAX], tmp[64];

	check_config_exists_or_die(home);
	check_config_keypair_or_die(home);

	printf("Your exported public information:\n\n");

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s/%s", home, FILE_USERNAM);
	path[sizeof(path) - 1] = 0;

	fd = open_or_die(path, O_RDONLY);
	while ((ret = read(fd, tmp, sizeof(tmp))) > 0) {
		ret = write(STDOUT_FILENO, tmp, ret);
	}
	close(fd);

	printf(";");

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s/%s", home, FILE_PUBKEY);
	path[sizeof(path) - 1] = 0;

	fd = open_or_die(path, O_RDONLY);
	ret = read(fd, tmp, sizeof(tmp));
	if (ret != crypto_box_curve25519xsalsa20poly1305_PUBLICKEYBYTES)
		panic("Cannot read public key!\n");
	for (i = 0; i < ret; ++i)
		if (i == ret - 1)
			printf("%02x\n\n", (unsigned char) tmp[i]);
		else
			printf("%02x:", (unsigned char) tmp[i]);
	close(fd);
	fflush(stdout);

	return 0;
}

static int main_dumpc(char *home)
{
	check_config_exists_or_die(home);
	check_config_keypair_or_die(home);

	printf("Your clients:\n\n");

	parse_userfile_and_generate_user_store_or_die(home);
	dump_user_store();
	destroy_user_store();

	printf("\n");
	die();
	return 0;
}

static int main_dumps(char *home)
{
	check_config_exists_or_die(home);
	check_config_keypair_or_die(home);

	printf("Your servers:\n\n");

	parse_userfile_and_generate_serv_store_or_die(home);
	dump_serv_store();
	destroy_serv_store();

	printf("\n");
	die();
	return 0;
}

static void daemonize(const char *lockfile)
{
	char pidstr[8];
	mode_t lperm = S_IRWXU | S_IRGRP | S_IXGRP; /* 0750 */
	int lfp;

	if (getppid() == 1)
		return;

	if (daemon(0, 0))
		panic("Cannot daemonize: %s", strerror(errno));

	umask(lperm);

	if (lockfile) {
		lfp = open(lockfile, O_RDWR | O_CREAT | O_EXCL, 0640);
		if (lfp < 0)
			panic("Cannot create lockfile at %s! "
			      "curvetun server already running?\n",
			      lockfile);

		snprintf(pidstr, sizeof(pidstr), "%u", getpid());
		pidstr[sizeof(pidstr) - 1] = 0;

		if (write(lfp, pidstr, strlen(pidstr)) <= 0)
			panic("Could not write pid to pidfile %s", lockfile);

		close(lfp);
	}
}

static int main_client(char *home, char *dev, char *alias, int daemon)
{
	int ret, udp;
	char *host, *port;

	check_config_exists_or_die(home);
	check_config_keypair_or_die(home);

	parse_userfile_and_generate_serv_store_or_die(home);
	get_serv_store_entry_by_alias(alias, alias ? strlen(alias) + 1 : 0,
				      &host, &port, &udp);
	if (!host || !port || udp < 0)
		panic("Did not find alias/entry in configuration!\n");
	printf("Using [%s] -> %s:%s via %s as endpoint!\n",
	       alias ? : "default", host, port, udp ? "udp" : "tcp");

	if (daemon)
		daemonize(NULL);
	ret = client_main(home, dev, host, port, udp);
	destroy_serv_store();

	return ret;
}

static int main_server(char *home, char *dev, char *port, int udp,
		       int ipv4, int daemon)
{
	int ret;

	check_config_exists_or_die(home);
	check_config_keypair_or_die(home);
	if (daemon)
		daemonize(LOCKFILE);
	ret = server_main(home, dev, port, udp, ipv4);
	unlink(LOCKFILE);

	return ret;
}

int main(int argc, char **argv)
{
	int ret = 0, c, opt_index, udp = 0, ipv4 = -1, daemon = 1;
	char *port = NULL, *stun = NULL, *dev = NULL, *home = NULL, *alias=NULL;
	enum working_mode wmode = MODE_UNKNOW;

	if (getuid() != geteuid())
		seteuid(getuid());
	if (getenv("LD_PRELOAD"))
		panic("curvetun cannot be preloaded!\n");

	home = fetch_home_dir();

	while ((c = getopt_long(argc, argv, short_options, long_options,
	       &opt_index)) != EOF) {
		switch (c) {
		case 'h':
			help();
			break;
		case 'H':
			towel();
			break;
		case 'v':
			version();
			break;
		case 'D':
			daemon = 0;
			break;
		case 'C':
			wmode = MODE_DUMPC;
			break;
		case 'S':
			wmode = MODE_DUMPS;
			break;
		case 'A':
			wmode = MODE_TOKEN;
			break;
		case 'c':
			wmode = MODE_CLIENT;
			if (optarg) {
				if (*optarg == '=')
					optarg++;
				alias = xstrdup(optarg);
			}
			break;
		case 'd':
			dev = xstrdup(optarg);
			break;
		case 'k':
			wmode = MODE_KEYGEN;
			break;
		case '4':
			ipv4 = 1;
			break;
		case '6':
			ipv4 = 0;
			break;
		case 'x':
			wmode = MODE_EXPORT;
			break;
		case 's':
			wmode = MODE_SERVER;
			break;
		case 'u':
			udp = 1;
			break;
		case 't':
			stun = xstrdup(optarg);
			break;
		case 'p':
			port = xstrdup(optarg);
			break;
		case '?':
			switch (optopt) {
			case 't':
			case 'd':
			case 'u':
			case 'p':
				panic("Option -%c requires an argument!\n",
				      optopt);
			default:
				if (isprint(optopt))
					whine("Unknown option character "
					      "`0x%X\'!\n", optopt);
				die();
			}
		default:
			break;
		}
	}

	if (argc < 2)
		help();

	register_signal(SIGINT, signal_handler);
	register_signal(SIGHUP, signal_handler);

	header();
	curve25519_selftest();

	switch (wmode) {
	case MODE_KEYGEN:
		ret = main_keygen(home);
		break;
	case MODE_EXPORT:
		ret = main_export(home);
		break;
	case MODE_TOKEN:
		ret = main_token(home);
		break;
	case MODE_DUMPC:
		ret = main_dumpc(home);
		break;
	case MODE_DUMPS:
		ret = main_dumps(home);
		break;
	case MODE_CLIENT:
		ret = main_client(home, dev, alias, daemon);
		break;
	case MODE_SERVER:
		if (!port)
			panic("No port specified!\n");
		if (stun)
			print_stun_probe(stun, 3478, strtoul(port, NULL, 10));
		ret = main_server(home, dev, port, udp, ipv4, daemon);
		break;
	default:
		die();
	}

	if (dev)
		xfree(dev);
	if (stun)
		xfree(stun);
	if (port)
		xfree(port);
	if (alias)
		xfree(alias);
	return ret;
}

