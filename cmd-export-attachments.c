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

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sigtop.h"

enum export_mode {
	COPY,
	LINK,
	SYMLINK
};

enum mtime_mode {
	NONE,
	SENT,
	RECV
};

static enum cmd_status cmd_export_attachments(int, char **);

const struct cmd_entry cmd_export_attachments_entry = {
	.name = "export-attachments",
	.alias = "att",
	.usage = "[-LlMm] [-d signal-directory] [-s interval] [directory]",
	.exec = cmd_export_attachments
};

/*
 * Check if a file with the specified name exists. If so, replace the name with
 * a new, unique name. Given a name of the form "base[.ext]", the new name is
 * of the form "base-n[.ext]" where 1 < n < 1000.
 */
static int
get_unique_filename(int dfd, char **name)
{
	struct stat	 st;
	char		*newname;
	const char	*ext;
	size_t		 baselen, namelen, size;
	int		 i;

	if (fstatat(dfd, *name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
		if (errno != ENOENT) {
			warn("fstatat: %s", *name);
			return -1;
		}
		return 0;
	}

	namelen = strlen(*name);

	ext = strrchr(*name, '.');
	if (ext != NULL && ext != *name && ext[1] != '\0')
		baselen = ext - *name;
	else {
		baselen = namelen;
		ext = "";
	}

	if (namelen > SIZE_MAX - 5 || baselen > INT_MAX) {
		warnx("Attachment filename too long");
		return -1;
	}

	/* 4 for the "-n" affix and 1 for the NUL */
	size = namelen + 5;
	if ((newname = malloc(size)) == NULL) {
		warn(NULL);
		return -1;
	}

	for (i = 2; i < 1000; i++) {
		snprintf(newname, size, "%.*s-%d%s", (int)baselen, *name, i,
		    ext);
		if (fstatat(dfd, newname, &st, AT_SYMLINK_NOFOLLOW) == -1) {
			if (errno != ENOENT) {
				warn("fstatat: %s", newname);
				free(newname);
				return -1;
			}
			free(*name);
			*name = newname;
			return 0;
		}
	}

	warnx("%s: Cannot generate unique filename", *name);
	free(newname);
	return -1;
}

static char *
get_filename(int dfd, struct sbk_attachment *att)
{
	char		*name;
	const char	*ext;
	struct tm	*tm;
	time_t		 tt;
	char		 base[32];

	if (att->filename != NULL && *att->filename != '\0') {
		if ((name = strdup(att->filename)) == NULL) {
			warn(NULL);
			return NULL;
		}
		sanitise_filename(name);
	} else {
		tt = att->time_sent / 1000;
		if ((tm = localtime(&tt)) == NULL) {
			warnx("localtime() failed");
			return NULL;
		}
		snprintf(base, sizeof base,
		    "attachment-%d-%02d-%02d-%02d-%02d-%02d",
		    tm->tm_year + 1900,
		    tm->tm_mon + 1,
		    tm->tm_mday,
		    tm->tm_hour,
		    tm->tm_min,
		    tm->tm_sec);
		if (att->content_type == NULL)
			ext = NULL;
		else
			ext = mime_get_extension(att->content_type);
		if (ext == NULL) {
			if ((name = strdup(base)) == NULL) {
				warn(NULL);
				return NULL;
			}
		} else {
			if (asprintf(&name, "%s.%s", base, ext) == -1) {
				warnx("asprintf() failed");
				return NULL;
			}
		}
	}

	if (get_unique_filename(dfd, &name) == -1) {
		free(name);
		return NULL;
	}

	return name;
}

#define COPY_BUFSIZE (1024 * 1024)

static int
copy_attachment(const char *src, int dfd, const char *dst,
    struct timespec ts[2])
{
	char	*buf;
	ssize_t	 nr, nw, off;
	int	 ret, rfd, wfd;

	ret = rfd = wfd = -1;

	if ((buf = malloc(COPY_BUFSIZE)) == NULL) {
		warn(NULL);
		goto out;
	}
	if ((rfd = open(src, O_RDONLY)) == -1) {
		warn("open: %s", src);
		goto out;
	}
	if ((wfd = openat(dfd, dst, O_WRONLY | O_CREAT | O_EXCL, 0666)) ==
	    -1) {
		warn("openat: %s", dst);
		goto out;
	}
	while ((nr = read(rfd, buf, COPY_BUFSIZE)) > 0)
		for (off = 0; off < nr; off += nw)
			if ((nw = write(wfd, buf + off, nr - off)) == -1) {
				warn("write: %s", dst);
				goto out;
			}
	if (nr == -1) {
		warn("read: %s", src);
		goto out;
	}
	if (ts[1].tv_nsec != UTIME_OMIT)
		if (futimens(wfd, ts) == -1) {
			warn("futimens: %s", dst);
			goto out;
		}
	ret = 0;

out:
	if (rfd != -1)
		close(rfd);
	if (wfd != -1)
		close(wfd);
	free(buf);
	return ret;
}

static int
symlink_attachment(const char *src, int dfd, const char *dst,
    struct timespec ts[2])
{
	if (symlinkat(src, dfd, dst) == -1) {
		warn("symlinkat: %s", dst);
		return -1;
	}
	if (ts[1].tv_nsec != UTIME_OMIT)
		if (utimensat(dfd, dst, ts, AT_SYMLINK_NOFOLLOW) == -1) {
			warn("utimensat: %s", dst);
			return -1;
		}
	return 0;
}

static void
get_mtime(struct timespec ts[2], struct sbk_attachment *att,
    enum mtime_mode mtime_mode)
{
	uint64_t msec;

	switch (mtime_mode) {
	case NONE:
		ts[1].tv_nsec = UTIME_OMIT;
		return;
	case SENT:
		msec = att->time_sent;
		break;
	case RECV:
		msec = att->time_recv;
		break;
	}

	ts[0].tv_nsec = UTIME_OMIT;
	ts[1].tv_sec = msec / 1000;
	ts[1].tv_nsec = msec % 1000 * 1000 * 1000;
}

static int
export_attachment_list(struct sbk_ctx *ctx, struct sbk_attachment_list *lst,
    int dfd, enum export_mode export_mode, enum mtime_mode mtime_mode)
{
	struct sbk_attachment	*att;
	char			*dst, *src;
	struct timespec		 ts[2];
	int			 ret;

	ret = 0;

	TAILQ_FOREACH(att, lst, entries) {
		if (sbk_get_attachment_path(ctx, &src, att) == -1) {
			ret = -1;
			continue;
		}
		if (src == NULL) {
			warnx("Skipping attachment (sent at %" PRIu64 "); "
			    "possibly it was not downloaded by Signal",
			    att->time_sent);
			continue;
		}
		if (access(src, F_OK) == -1) {
			warn("access: %s", src);
			free(src);
			ret = -1;
			continue;
		}
		if ((dst = get_filename(dfd, att)) == NULL) {
			free(src);
			ret = -1;
			continue;
		}
		switch (export_mode) {
		case COPY:
			get_mtime(ts, att, mtime_mode);
			if (copy_attachment(src, dfd, dst, ts) == -1)
				ret = -1;
			break;
		case LINK:
			if (linkat(AT_FDCWD, src, dfd, dst, 0) == -1) {
				warn("linkat: %s", dst);
				ret = -1;
			}
			break;
		case SYMLINK:
			get_mtime(ts, att, mtime_mode);
			if (symlink_attachment(src, dfd, dst, ts) == -1)
				ret = -1;
			break;
		}
		free(src);
		free(dst);
	}

	return ret;
}

static int
get_conversation_directory(int dfd, struct sbk_conversation *cnv)
{
	char	*name;
	int	 cnv_dfd;

	if ((name = get_recipient_filename(cnv->recipient, NULL)) == NULL)
		return -1;

	if (mkdirat(dfd, name, 0777) == -1 && errno != EEXIST) {
		warn("%s", name);
		free(name);
		return -1;
	}

	if ((cnv_dfd = openat(dfd, name, O_RDONLY | O_DIRECTORY)) == -1) {
		warn("%s", name);
		free(name);
		return -1;
	}

	free(name);
	return cnv_dfd;
}

static int
export_conversation_attachments(struct sbk_ctx *ctx,
    struct sbk_conversation *cnv, int dfd, enum export_mode export_mode,
    enum mtime_mode mtime_mode, time_t min, time_t max)
{
	struct sbk_attachment_list	*lst;
	int				 cnv_dfd, ret;

	ret = -1;

	if (min == (time_t)-1 && max == (time_t)-1)
		lst = sbk_get_attachments(ctx, cnv);
	else if (min == (time_t)-1)
		lst = sbk_get_attachments_sent_before(ctx, cnv, max);
	else if (max == (time_t)-1)
		lst = sbk_get_attachments_sent_after(ctx, cnv, min);
	else
		lst = sbk_get_attachments_sent_between(ctx, cnv, min, max);

	if (lst == NULL)
		goto out;

	if (TAILQ_EMPTY(lst)) {
		ret = 0;
		goto out;
	}

	if ((cnv_dfd = get_conversation_directory(dfd, cnv)) == -1)
		goto out;

	ret = export_attachment_list(ctx, lst, cnv_dfd, export_mode,
	    mtime_mode);
	close(cnv_dfd);

out:
	sbk_free_attachment_list(lst);
	return ret;
}

static int
export_attachments(struct sbk_ctx *ctx, const char *dir,
    enum export_mode export_mode, enum mtime_mode mtime_mode, time_t min,
    time_t max)
{
	struct sbk_conversation_list	*lst;
	struct sbk_conversation		*cnv;
	int				 dfd, ret;

	if ((dfd = open(dir, O_RDONLY | O_DIRECTORY)) == -1) {
		warn("%s", dir);
		return -1;
	}

	if ((lst = sbk_get_conversations(ctx)) == NULL) {
		close(dfd);
		return -1;
	}

	ret = 0;
	SIMPLEQ_FOREACH(cnv, lst, entries)
		if (export_conversation_attachments(ctx, cnv, dfd, export_mode,
		    mtime_mode, min, max) == -1)
			ret = -1;

	sbk_free_conversation_list(lst);
	close(dfd);
	return ret;
}

static enum cmd_status
cmd_export_attachments(int argc, char **argv)
{
	struct sbk_ctx	*ctx;
	char		*signaldir;
	const char	*outdir;
	time_t		 max, min;
	int		 c;
	enum export_mode export_mode;
	enum mtime_mode  mtime_mode;
	enum cmd_status	 status;

	ctx = NULL;
	signaldir = NULL;
	export_mode = COPY;
	mtime_mode = NONE;
	min = max = (time_t)-1;

	while ((c = getopt(argc, argv, "d:LlMms:")) != -1)
		switch (c) {
		case 'd':
			free(signaldir);
			if ((signaldir = strdup(optarg)) == NULL) {
				warn(NULL);
				goto error;
			}
			break;
		case 'L':
			export_mode = LINK;
			break;
		case 'l':
			export_mode = SYMLINK;
			break;
		case 'M':
			mtime_mode = SENT;
			break;
		case 'm':
			mtime_mode = RECV;
			break;
		case 's':
			if (parse_time_interval(optarg, &min, &max) == -1)
				goto error;
			break;
		default:
			goto usage;
		}

	argc -= optind;
	argv += optind;

	switch (argc) {
	case 0:
		outdir = ".";
		break;
	case 1:
		outdir = argv[0];
		if (mkdir(outdir, 0777) == -1 && errno != EEXIST) {
			warn("mkdir: %s", outdir);
			goto error;
		}
		break;
	default:
		goto usage;
	}

	if (signaldir == NULL)
		if ((signaldir = get_signal_dir()) == NULL)
			goto error;

	if (unveil_signal_dir(signaldir) == -1)
		goto error;

	if (unveil(outdir, "rwc") == -1) {
		warn("unveil: %s", outdir);
		goto error;
	}

	/* For SQLite/SQLCipher */
	if (unveil("/dev/urandom", "r") == -1) {
		warn("unveil: /dev/urandom");
		goto error;
	}

	if (mtime_mode == NONE || export_mode == LINK) {
		if (pledge("stdio rpath wpath cpath flock", NULL) == -1) {
			warn("pledge");
			goto error;
		}
	} else {
		if (pledge("stdio rpath wpath cpath flock fattr", NULL) ==
		    -1) {
			warn("pledge");
			goto error;
		}
	}

	if (sbk_open(&ctx, signaldir) == -1)
		goto error;

	if (export_attachments(ctx, outdir, export_mode, mtime_mode, min, max)
	    == -1)
		goto error;

	status = CMD_OK;
	goto out;

error:
	status = CMD_ERROR;
	goto out;

usage:
	status = CMD_USAGE;

out:
	sbk_close(ctx);
	free(signaldir);
	return status;
}
