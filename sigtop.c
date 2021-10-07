/*
 * Copyright (c) 2021 Tim van der Molen <tim@kariliq.nl>
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

#include "config.h"

#include <sys/types.h>

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sigtop.h"

void
usage(const char *cmd, const char *args)
{
	fprintf(stderr, "usage: %s %s %s\n", getprogname(), cmd, args);
	exit(1);
}

int
unveil_dirname(const char *path, const char *perms)
{
	char *dir, *tmp;

	if ((tmp = strdup(path)) == NULL) {
		warn(NULL);
		return -1;
	}

	if ((dir = dirname(tmp)) == NULL) {
		warnx("dirname() failed");
		free(tmp);
		return -1;
	}

	if (unveil(dir, perms) == -1) {
		warn("unveil: %s", dir);
		free(tmp);
		return -1;
	}

	free(tmp);
	return 0;
}

int
unveil_signal_dir(const char *dir)
{
	char *dbdir;

	if (unveil(dir, "r") == -1) {
		warn("unveil: %s", dir);
		return -1;
	}

	/*
	 * SQLCipher needs to create the sql/db.sqlite-{shm,wal} files if they
	 * don't exist already
	 */

	if (asprintf(&dbdir, "%s/sql", dir) == -1) {
		warnx("asprintf() failed");
		return -1;
	}

	if (unveil(dbdir, "rwc") == -1) {
		warn("unveil: %s", dbdir);
		free(dbdir);
		return -1;
	}

	free(dbdir);
	return 0;
}

static int
parse_time(const char *str, time_t *tt)
{
	struct tm	 tm;
	char		*c;

	if (*str == '\0') {
		*tt = (time_t)-1;
		return 0;
	}

	memset(&tm, 0, sizeof tm);
	c = strptime(str, "%Y-%m-%dT%H:%M:%S", &tm);

	if (c == NULL || *c != '\0') {
		warnx("%s: Invalid time specification", str);
		return -1;
	}

	tm.tm_isdst = -1;

	if ((*tt = mktime(&tm)) < 0) {
		warnx("mktime() failed");
		return -1;
	}

	return 0;
}

int
parse_time_interval(char *str, time_t *min, time_t *max)
{
	char *maxstr, *minstr, *sep;

	if ((sep = strchr(str, ',')) == NULL) {
		warnx("%s: Missing separator in time interval", str);
		return -1;
	}

	*sep = '\0';
	minstr = str;
	maxstr = sep + 1;

	if (parse_time(minstr, min) == -1 || parse_time(maxstr, max) == -1)
		return -1;

	if (*max != (time_t)-1 && *min > *max) {
		warnx("%s is later than %s", minstr, maxstr);
		return -1;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	setprogname(argv[0]);

	if (argc < 2)
		usage("command", "[argument ...]");

	argc--;
	argv++;

	if (strcmp(argv[0], "attachments") == 0)
		return cmd_attachments(argc, argv);
	if (strcmp(argv[0], "check") == 0)
		return cmd_check(argc, argv);
	if (strcmp(argv[0], "messages") == 0)
		return cmd_messages(argc, argv);
	if (strcmp(argv[0], "sqlite") == 0)
		return cmd_sqlite(argc, argv);

	errx(1, "%s: Invalid command", argv[0]);
}
