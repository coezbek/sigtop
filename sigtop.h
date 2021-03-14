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

#include <sys/queue.h>

#ifndef nitems
#define nitems(a) (sizeof (a) / sizeof (a)[0])
#endif

struct sbk_ctx;

struct sbk_contact {
	char		*name;
	char		*profile_name;
	char		*profile_family_name;
	char		*profile_joined_name;
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

struct sbk_message {
	struct sbk_recipient *conversation;
	struct sbk_recipient *source;
	uint64_t	 time_sent;
	uint64_t	 time_recv;
	char		*type;
	char		*text;
	SIMPLEQ_ENTRY(sbk_message) entries;
};

SIMPLEQ_HEAD(sbk_message_list, sbk_message);

int		 sbk_open(struct sbk_ctx **, const char *, const char *);
void		 sbk_close(struct sbk_ctx *);
const char	*sbk_error(struct sbk_ctx *);

struct sbk_message_list *sbk_get_all_messages(struct sbk_ctx *);
void		 sbk_free_message_list(struct sbk_message_list *);
int		 sbk_is_outgoing_message(const struct sbk_message *);

const char	*sbk_get_recipient_display_name(const struct sbk_recipient *);

#endif
