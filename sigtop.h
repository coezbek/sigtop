/*
 * Copyright (c) 2018 Tim van der Molen <tim@kariliq.nl>
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

#ifndef SIGTOP_H
#define SIGTOP_H

#include <sys/types.h>

#include <stdint.h>

#include "compat.h"

#ifndef nitems
#define nitems(a) (sizeof (a) / sizeof (a)[0])
#endif

enum cmd_status {
	CMD_OK,
	CMD_ERROR,
	CMD_USAGE
};

struct cmd_entry {
	const char	*name;
	const char	*alias;
	const char	*usage;
	const char	*oldname;
	enum cmd_status	 (*exec)(int, char **);
};

struct sbk_ctx;

struct sbk_contact {
	char		*name;
	char		*profile_name;
	char		*profile_family_name;
	char		*profile_joined_name;
	char		*phone;
};

struct sbk_group {
	char		*name;
};

struct sbk_recipient {
	enum {
		SBK_CONTACT,
		SBK_GROUP
	} type;
	struct sbk_contact	*contact;
	struct sbk_group	*group;
};

struct sbk_attachment {
	char		*path;
	char		*filename;
	char		*content_type;
	uint64_t	 size;
	uint64_t	 time_sent;
	uint64_t	 time_recv;
	TAILQ_ENTRY(sbk_attachment) entries;
};

TAILQ_HEAD(sbk_attachment_list, sbk_attachment);

struct sbk_reaction {
	struct sbk_recipient *recipient;
	uint64_t	 time_sent;
	uint64_t	 time_recv;
	char		*emoji;
	SIMPLEQ_ENTRY(sbk_reaction) entries;
};

SIMPLEQ_HEAD(sbk_reaction_list, sbk_reaction);

struct sbk_message {
	struct sbk_recipient *conversation;
	struct sbk_recipient *source;
	uint64_t	 time_sent;
	uint64_t	 time_recv;
	char		*type;
	char		*text;
	char		*json;
	struct sbk_attachment_list *attachments;
	struct sbk_reaction_list *reactions;
	SIMPLEQ_ENTRY(sbk_message) entries;
};

SIMPLEQ_HEAD(sbk_message_list, sbk_message);

int		 sbk_open(struct sbk_ctx **, const char *);
void		 sbk_close(struct sbk_ctx *);
const char	*sbk_error(struct sbk_ctx *);

int		 sbk_check(struct sbk_ctx *, char ***);

struct sbk_attachment_list *sbk_get_all_attachments(struct sbk_ctx *);
struct sbk_attachment_list *sbk_get_attachments_sent_after(struct sbk_ctx *,
		    time_t);
struct sbk_attachment_list *sbk_get_attachments_sent_before(struct sbk_ctx *,
		    time_t);
struct sbk_attachment_list *sbk_get_attachments_sent_between(struct sbk_ctx *,
		    time_t, time_t);
void		 sbk_free_attachment_list(struct sbk_attachment_list *);
char		*sbk_get_attachment_path(struct sbk_ctx *,
		    struct sbk_attachment *);

struct sbk_message_list *sbk_get_all_messages(struct sbk_ctx *);
struct sbk_message_list *sbk_get_messages_sent_after(struct sbk_ctx *, time_t);
struct sbk_message_list *sbk_get_messages_sent_before(struct sbk_ctx *,
		    time_t);
struct sbk_message_list *sbk_get_messages_sent_between(struct sbk_ctx *,
		    time_t, time_t);
void		 sbk_free_message_list(struct sbk_message_list *);
int		 sbk_is_outgoing_message(const struct sbk_message *);

const char	*sbk_get_recipient_display_name(const struct sbk_recipient *);

int		 sbk_write_database(struct sbk_ctx *, const char *);

const char	*mime_get_extension(const char *);

size_t		 utf8_encode(uint8_t [4], uint32_t);
int		 utf16_is_high_surrogate(uint16_t);
int		 utf16_is_low_surrogate(uint16_t);
uint32_t	 utf16_decode_surrogate_pair(uint16_t, uint16_t);

char		*get_signal_dir(void);
int		 unveil_dirname(const char *, const char *);
int		 unveil_signal_dir(const char *);
int		 parse_time_interval(char *, time_t *, time_t *);

#endif
