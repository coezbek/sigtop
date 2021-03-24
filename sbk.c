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

#define JSMN_STATIC
#define JSMN_STRICT

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqlite3.h"

#include "jsmn.h"
#include "sigtop.h"

struct sbk_recipient_entry {
	char		*id;
	struct sbk_recipient recipient;
	RB_ENTRY(sbk_recipient_entry) entries;
};

RB_HEAD(sbk_recipient_tree, sbk_recipient_entry);

struct sbk_ctx {
	sqlite3		*db;
	int		 db_version;
	char		*error;
	struct sbk_recipient_tree recipients;
};

static int sbk_cmp_recipient_entries(struct sbk_recipient_entry *,
    struct sbk_recipient_entry *);

RB_GENERATE_STATIC(sbk_recipient_tree, sbk_recipient_entry, entries,
    sbk_cmp_recipient_entries)

static void
sbk_error_clear(struct sbk_ctx *ctx)
{
	free(ctx->error);
	ctx->error = NULL;
}

static void
sbk_error_set(struct sbk_ctx *ctx, const char *fmt, ...)
{
	va_list	 ap;
	char	*errmsg, *msg;
	int	 saved_errno;

	va_start(ap, fmt);
	saved_errno = errno;
	sbk_error_clear(ctx);
	errmsg = strerror(saved_errno);

	if (fmt == NULL || vasprintf(&msg, fmt, ap) == -1)
		ctx->error = strdup(errmsg);
	else if (asprintf(&ctx->error, "%s: %s", msg, errmsg) == -1)
		ctx->error = msg;
	else
		free(msg);

	errno = saved_errno;
	va_end(ap);
}

static void
sbk_error_setx(struct sbk_ctx *ctx, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	sbk_error_clear(ctx);

	if (fmt == NULL || vasprintf(&ctx->error, fmt, ap) == -1)
		ctx->error = NULL;

	va_end(ap);
}

static void
sbk_error_sqlite_vsetd(struct sbk_ctx *ctx, sqlite3 *db, const char *fmt,
    va_list ap)
{
	const char	*errmsg;
	char		*msg;

	sbk_error_clear(ctx);
	errmsg = sqlite3_errmsg(db);

	if (fmt == NULL || vasprintf(&msg, fmt, ap) == -1)
		ctx->error = strdup(errmsg);
	else if (asprintf(&ctx->error, "%s: %s", msg, errmsg) == -1)
		ctx->error = msg;
	else
		free(msg);
}

static void
sbk_error_sqlite_setd(struct sbk_ctx *ctx, sqlite3 *db, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	sbk_error_sqlite_vsetd(ctx, db, fmt, ap);
	va_end(ap);
}

static void
sbk_error_sqlite_set(struct sbk_ctx *ctx, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	sbk_error_sqlite_vsetd(ctx, ctx->db, fmt, ap);
	va_end(ap);
}

static int
sbk_sqlite_bind_text(struct sbk_ctx *ctx, sqlite3 *db, sqlite3_stmt *stm,
    int idx, const char *val)
{
	if (sqlite3_bind_text(stm, idx, val, -1, SQLITE_STATIC) != SQLITE_OK) {
		sbk_error_sqlite_setd(ctx, db, "Cannot bind SQL parameter");
		return -1;
	}

	return 0;
}

static int
sbk_sqlite_column_text_copy(struct sbk_ctx *ctx, char **buf, sqlite3_stmt *stm,
    int idx)
{
#ifdef notyet
	const unsigned char	*txt;
	int			 len;

	*buf = NULL;

	if (sqlite3_column_type(stm, idx) == SQLITE_NULL)
		return 0;

	if ((txt = sqlite3_column_text(stm, idx)) == NULL) {
		sbk_error_sqlite_set(ctx, "Cannot get column text");
		return -1;
	}

	if ((len = sqlite3_column_bytes(stm, idx)) < 0) {
		sbk_error_sqlite_set(ctx, "Cannot get column size");
		return -1;
	}

	if ((*buf = malloc((size_t)len + 1)) == NULL) {
		sbk_error_set(ctx, NULL);
		return -1;
	}

	memcpy(*buf, txt, (size_t)len + 1);
	return len;
#else
	const unsigned char *txt;

	*buf = NULL;

	if (sqlite3_column_type(stm, idx) == SQLITE_NULL)
		return 0;

	if ((txt = sqlite3_column_text(stm, idx)) == NULL) {
		sbk_error_sqlite_set(ctx, "Cannot get column text");
		return -1;
	}

	if ((*buf = strdup(txt)) == NULL) {
		sbk_error_set(ctx, NULL);
		return -1;
	}

	return 0;
#endif
}

static int
sbk_sqlite_open(struct sbk_ctx *ctx, sqlite3 **db, const char *path, int flags)
{
	if (sqlite3_open_v2(path, db, flags, NULL) != SQLITE_OK) {
		sbk_error_sqlite_setd(ctx, *db, "Cannot open database");
		return -1;
	}

	return 0;
}

static int
sbk_sqlite_prepare(struct sbk_ctx *ctx, sqlite3 *db, sqlite3_stmt **stm,
    const char *query)
{
	if (sqlite3_prepare_v2(db, query, -1, stm, NULL) != SQLITE_OK) {
		sbk_error_sqlite_setd(ctx, db, "Cannot prepare SQL statement");
		return -1;
	}

	return 0;
}

static int
sbk_sqlite_step(struct sbk_ctx *ctx, sqlite3 *db, sqlite3_stmt *stm)
{
	int ret;

	ret = sqlite3_step(stm);
	if (ret != SQLITE_ROW && ret != SQLITE_DONE)
		sbk_error_sqlite_setd(ctx, db, "Cannot execute SQL statement");

	return ret;
}

static int
sbk_sqlite_exec(struct sbk_ctx *ctx, sqlite3 *db, const char *sql)
{
	char *errmsg;

	if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
		sbk_error_setx(ctx, "Cannot execute SQL statement: %s",
		    errmsg);
		sqlite3_free(errmsg);
		return -1;
	}

	return 0;
}

static int
sbk_sqlite_key(struct sbk_ctx *ctx, sqlite3 *db, const char *key)
{
	if (sqlite3_key(db, key, strlen(key)) == -1) {
		sbk_error_sqlite_setd(ctx, db, "Cannot set key");
		return -1;
	}

	return 0;
}

static int
sbk_get_database_version(struct sbk_ctx *ctx)
{
	sqlite3_stmt	*stm;
	int		 version;

	if (sbk_sqlite_prepare(ctx, ctx->db, &stm, "PRAGMA user_version") ==
	    -1)
		return -1;

	if (sbk_sqlite_step(ctx, ctx->db, stm) != SQLITE_ROW) {
		sqlite3_finalize(stm);
		return -1;
	}

	if ((version = sqlite3_column_int(stm, 0)) < 0) {
		sbk_error_setx(ctx, "Negative database version");
		sqlite3_finalize(stm);
		return -1;
	}

	sqlite3_finalize(stm);
	return version;
}

static int
sbk_set_database_version(struct sbk_ctx *ctx, sqlite3 *db, const char *schema,
    int version)
{
	char	*sql;
	int	 ret;

	if (asprintf(&sql, "PRAGMA %s.user_version = %d", schema, version) ==
	    -1) {
		sbk_error_setx(ctx, "asprintf() failed");
		return -1;
	}

	ret = sbk_sqlite_exec(ctx, db, sql);
	free(sql);
	return ret;
}

int
sbk_write_database(struct sbk_ctx *ctx, const char *path)
{
	sqlite3		*db;
	sqlite3_backup	*bak;
	sqlite3_stmt	*stm;

	if (sbk_sqlite_open(ctx, &db, ":memory:",
	    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) == -1)
		goto error;

	/* Set a dummy key to enable encryption */
	if (sbk_sqlite_key(ctx, db, "x") == -1)
		goto error;

	if ((bak = sqlite3_backup_init(db, "main", ctx->db, "main")) == NULL) {
		sbk_error_sqlite_setd(ctx, db, "Cannot write database");
		goto error;
	}

	if (sqlite3_backup_step(bak, -1) != SQLITE_DONE) {
		sbk_error_sqlite_setd(ctx, db, "Cannot write database");
		sqlite3_backup_finish(bak);
		goto error;
	}

	sqlite3_backup_finish(bak);

	/* Attaching with an empty key will disable encryption */
	if (sbk_sqlite_prepare(ctx, db, &stm,
	    "ATTACH DATABASE ? AS plaintext KEY ''") == -1)
		goto error;

	if (sbk_sqlite_bind_text(ctx, db, stm, 1, path) == -1)
		goto error;

	if (sbk_sqlite_step(ctx, db, stm) != SQLITE_DONE)
		goto error;

	sqlite3_finalize(stm);

	if (sbk_sqlite_exec(ctx, db, "BEGIN TRANSACTION") == -1)
		goto error;

	if (sbk_sqlite_exec(ctx, db, "SELECT sqlcipher_export('plaintext')") ==
	    -1)
		goto error;

	if (sbk_set_database_version(ctx, db, "plaintext", ctx->db_version) ==
	    -1)
		goto error;

	if (sbk_sqlite_exec(ctx, db, "END TRANSACTION") == -1)
		goto error;

	if (sbk_sqlite_exec(ctx, db, "DETACH DATABASE plaintext") == -1)
		goto error;

	if (sqlite3_close(db) != SQLITE_OK) {
		sbk_error_sqlite_setd(ctx, db, "Cannot close database");
		return -1;
	}

	return 0;

error:
	sqlite3_close(db);
	return -1;
}

static int
sbk_jsmn_parse(const char *json, size_t jsonlen, jsmntok_t *tokens,
    size_t ntokens)
{
	jsmn_parser	parser;
	int		len;

	jsmn_init(&parser);
	len = jsmn_parse(&parser, json, jsonlen, tokens, ntokens);
	if (len <= 0 || tokens[0].type != JSMN_OBJECT)
		len = -1;
	return len;
}

static int
sbk_jsmn_is_valid_key(const jsmntok_t *token)
{
	return token->type == JSMN_STRING && token->size == 1;
}

static int
sbk_jsmn_token_equals(const char *json, const jsmntok_t *token,
    const char *str)
{
	size_t len;

	len = strlen(str);
	if (len != (unsigned int)(token->end - token->start))
		return 0;
	else
		return memcmp(json + token->start, str, len) == 0;
}

static int
sbk_jsmn_get_total_token_size(const jsmntok_t *tokens)
{
	int i, idx, size;

	idx = 1;
	switch (tokens[0].type) {
	case JSMN_OBJECT:
		for (i = 0; i < tokens[0].size; i++) {
			if (!sbk_jsmn_is_valid_key(&tokens[idx]))
				return -1;
			size = sbk_jsmn_get_total_token_size(&tokens[++idx]);
			if (size == -1)
				return -1;
			idx += size;
		}
		break;
	case JSMN_ARRAY:
		for (i = 0; i < tokens[0].size; i++) {
			size = sbk_jsmn_get_total_token_size(&tokens[idx]);
			if (size == -1)
				return -1;
			idx += size;
		}
		break;
	case JSMN_STRING:
	case JSMN_PRIMITIVE:
		if (tokens[0].size != 0)
			return -1;
		break;
	case JSMN_UNDEFINED:
		return -1;
	}

	return idx;
}

static int
sbk_jsmn_find_key(const char *json, const jsmntok_t *tokens, const char *key)
{
	int i, idx, size;

	if (tokens[0].type != JSMN_OBJECT)
		return -1;

	idx = 1;
	for (i = 0; i < tokens[0].size; i++) {
		if (!sbk_jsmn_is_valid_key(&tokens[idx]))
			return -1;
		if (sbk_jsmn_token_equals(json, &tokens[idx], key))
			return idx;
		/* Skip value */
		size = sbk_jsmn_get_total_token_size(&tokens[++idx]);
		if (size == -1)
			return -1;
		idx += size;
	}

	/* Not found */
	return -1;
}

static int
sbk_jsmn_get_value(const char *json, const jsmntok_t *tokens, const char *key,
    jsmntype_t type)
{
	int idx;

	idx = sbk_jsmn_find_key(json, tokens, key);
	if (idx == -1)
		return -1;
	if (tokens[++idx].type != type)
		return -1;
	return idx;
}

static int
sbk_jsmn_get_string(const char *json, const jsmntok_t *tokens, const char *key)
{
	return sbk_jsmn_get_value(json, tokens, key, JSMN_STRING);
}

/* Read the database encryption key from a JSON file */
static int
sbk_get_key(struct sbk_ctx *ctx, char *buf, size_t bufsize, const char *path)
{
	jsmntok_t	tokens[64];
	ssize_t		jsonlen;
	int		fd, idx, keylen, len;
	char		json[2048], *key;

	if ((fd = open(path, O_RDONLY)) == -1) {
		sbk_error_set(ctx, "%s", path);
		return -1;
	}

	if ((jsonlen = read(fd, json, sizeof json - 1)) == -1) {
		sbk_error_set(ctx, "%s", path);
		close(fd);
		goto error;
	}

	json[jsonlen] = '\0';
	close(fd);

	if (sbk_jsmn_parse(json, jsonlen, tokens, nitems(tokens)) == -1) {
		sbk_error_setx(ctx, "%s: Cannot parse JSON data", path);
		goto error;
	}

	idx = sbk_jsmn_get_string(json, tokens, "key");
	if (idx == -1) {
		sbk_error_setx(ctx, "%s: Cannot find key", path);
		goto error;
	}

	key = json + tokens[idx].start;
	keylen = tokens[idx].end - tokens[idx].start;

	/* Write the key as an SQLite blob literal */
	len = snprintf(buf, bufsize, "x'%.*s'", keylen, key);
	if (len < 0 || (unsigned int)len >= bufsize) {
		sbk_error_setx(ctx, "%s: Cannot get key", path);
		goto error;
	}

	explicit_bzero(json, sizeof json);
	return 0;

error:
	explicit_bzero(json, sizeof json);
	explicit_bzero(buf, bufsize);
	return -1;
}

static int
sbk_cmp_recipient_entries(struct sbk_recipient_entry *e,
    struct sbk_recipient_entry *f)
{
	return strcmp(e->id, f->id);
}

static void
sbk_free_recipient_entry(struct sbk_recipient_entry *ent)
{
	if (ent == NULL)
		return;

	switch (ent->recipient.type) {
	case SBK_CONTACT:
		free(ent->recipient.contact->name);
		free(ent->recipient.contact->profile_name);
		free(ent->recipient.contact->profile_family_name);
		free(ent->recipient.contact->profile_joined_name);
		free(ent->recipient.contact);
		break;
	case SBK_GROUP:
		free(ent->recipient.group->name);
		free(ent->recipient.group);
		break;
	}

	free(ent->id);
	free(ent);
}

static void
sbk_free_recipient_tree(struct sbk_ctx *ctx)
{
	struct sbk_recipient_entry *ent;

	while ((ent = RB_ROOT(&ctx->recipients)) != NULL) {
		RB_REMOVE(sbk_recipient_tree, &ctx->recipients, ent);
		sbk_free_recipient_entry(ent);
	}
}

/* For database versions >= 19 */
#define SBK_RECIPIENTS_QUERY_19						\
	"SELECT "							\
	"id, "								\
	"type, "							\
	"name, "							\
	"profileName, "							\
	"profileFamilyName, "						\
	"profileFullName "						\
	"FROM conversations"

static struct sbk_recipient_entry *
sbk_get_recipient_entry(struct sbk_ctx *ctx, sqlite3_stmt *stm)
{
	struct sbk_recipient_entry	*ent;
	struct sbk_contact		*con;
	struct sbk_group		*grp;
	const unsigned char		*type;

	if ((ent = calloc(1, sizeof *ent)) == NULL) {
		sbk_error_set(ctx, NULL);
		return NULL;
	}

	if (sbk_sqlite_column_text_copy(ctx, &ent->id, stm, 0) == -1)
		goto error;

	if ((type = sqlite3_column_text(stm, 1)) == NULL) {
		sbk_error_sqlite_set(ctx, "Cannot get column text");
		goto error;
	}

	if (strcmp(type, "private") == 0)
		ent->recipient.type = SBK_CONTACT;
	else if (strcmp(type, "group") == 0)
		ent->recipient.type = SBK_GROUP;
	else {
		sbk_error_setx(ctx, "Unknown recipient type");
		goto error;
	}

	switch (ent->recipient.type) {
	case SBK_CONTACT:
		con = ent->recipient.contact = calloc(1, sizeof *con);
		if (con == NULL) {
			sbk_error_set(ctx, NULL);
			goto error;
		}

		if (sbk_sqlite_column_text_copy(ctx, &con->name,
		    stm, 2) == -1)
			goto error;

		if (sbk_sqlite_column_text_copy(ctx, &con->profile_name,
		    stm, 3) == -1)
			goto error;

		if (sbk_sqlite_column_text_copy(ctx, &con->profile_family_name,
		    stm, 4) == -1)
			goto error;

		if (sbk_sqlite_column_text_copy(ctx, &con->profile_joined_name,
		    stm, 5) == -1)
			goto error;

		break;

	case SBK_GROUP:
		grp = ent->recipient.group = calloc(1, sizeof *grp);
		if (grp == NULL) {
			sbk_error_set(ctx, NULL);
			goto error;
		}

		if (sbk_sqlite_column_text_copy(ctx, &grp->name,
		    stm, 2) == -1)
			goto error;
	}

	return ent;

error:
	sbk_free_recipient_entry(ent);
	return NULL;
}

static int
sbk_build_recipient_tree(struct sbk_ctx *ctx)
{
	struct sbk_recipient_entry	*ent;
	sqlite3_stmt			*stm;
	int				 ret;

	if (!RB_EMPTY(&ctx->recipients))
		return 0;

	if (sbk_sqlite_prepare(ctx, ctx->db, &stm, SBK_RECIPIENTS_QUERY_19) ==
	    -1)
		return -1;

	while ((ret = sbk_sqlite_step(ctx, ctx->db, stm)) == SQLITE_ROW) {
		if ((ent = sbk_get_recipient_entry(ctx, stm)) == NULL)
			goto error;
		RB_INSERT(sbk_recipient_tree, &ctx->recipients, ent);
	}

	if (ret != SQLITE_DONE)
		goto error;

	sqlite3_finalize(stm);
	return 0;

error:
	sbk_free_recipient_tree(ctx);
	sqlite3_finalize(stm);
	return -1;
}

static struct sbk_recipient *
sbk_get_recipient_from_conversation_id(struct sbk_ctx *ctx, const char *id)
{
	struct sbk_recipient_entry find, *result;

	if (sbk_build_recipient_tree(ctx) == -1)
		return NULL;

	find.id = (char *)id;
	result = RB_FIND(sbk_recipient_tree, &ctx->recipients, &find);

	if (result == NULL) {
		sbk_error_setx(ctx, "Cannot find recipient");
		return NULL;
	}

	return &result->recipient;
}

const char *
sbk_get_recipient_display_name(const struct sbk_recipient *rcp)
{
	switch (rcp->type) {
	case SBK_CONTACT:
		if (rcp->contact->name != NULL)
			return rcp->contact->name;
		if (rcp->contact->profile_joined_name != NULL)
			return rcp->contact->profile_joined_name;
		if (rcp->contact->profile_name != NULL)
			return rcp->contact->profile_name;
		break;
	case SBK_GROUP:
		if (rcp->group->name != NULL)
			return rcp->group->name;
		break;
	}

	return "Unknown";
}

int
sbk_is_outgoing_message(const struct sbk_message *msg)
{
	return strcmp(msg->type, "outgoing") == 0;
}

static void
sbk_free_message(struct sbk_message *msg)
{
	free(msg->type);
	free(msg->text);
	free(msg->json);
	free(msg);
}

void
sbk_free_message_list(struct sbk_message_list *lst)
{
	struct sbk_message *msg;

	if (lst != NULL) {
		while ((msg = SIMPLEQ_FIRST(lst)) != NULL) {
			SIMPLEQ_REMOVE_HEAD(lst, entries);
			sbk_free_message(msg);
		}
		free(lst);
	}
}

/* For database versions 8 to 19 */
#define SBK_MESSAGES_QUERY_8						\
	"SELECT "							\
	"conversationId, "						\
	"source, "							\
	"type, "							\
	"body, "							\
	"json, "							\
	"sent_at, "							\
	"received_at "							\
	"FROM messages "						\
	"ORDER BY received_at"

/* For database versions >= 20 */
#define SBK_MESSAGES_QUERY_20						\
	"SELECT "							\
	"m.conversationId, "						\
	"c.id, "							\
	"m.type, "							\
	"m.body, "							\
	"m.json, "							\
	"m.sent_at, "							\
	"m.received_at "						\
	"FROM messages AS m "						\
	"LEFT JOIN conversations AS c "					\
	"ON m.sourceUuid = c.uuid "					\
	"ORDER BY m.received_at"

static struct sbk_message *
sbk_get_message(struct sbk_ctx *ctx, sqlite3_stmt *stm)
{
	struct sbk_message	*msg;
	unsigned const char	*id;

	if ((msg = calloc(1, sizeof *msg)) == NULL) {
		sbk_error_set(ctx, NULL);
		return NULL;
	}

	if ((id = sqlite3_column_text(stm, 0)) == NULL) {
		/* Likely message with error */
		msg->conversation = NULL;
	} else {
		msg->conversation = sbk_get_recipient_from_conversation_id(ctx,
		    id);
		if (msg->conversation == NULL)
			goto error;
	}

	if ((id = sqlite3_column_text(stm, 1)) == NULL) {
		msg->source = NULL;
	} else {
		msg->source = sbk_get_recipient_from_conversation_id(ctx, id);
		if (msg->source == NULL)
			goto error;
	}

	if (sbk_sqlite_column_text_copy(ctx, &msg->type, stm, 2) == -1)
		goto error;

	if (sbk_sqlite_column_text_copy(ctx, &msg->text, stm, 3) == -1)
		goto error;

	if (sbk_sqlite_column_text_copy(ctx, &msg->json, stm, 4) == -1)
		goto error;

	msg->time_sent = sqlite3_column_int64(stm, 5);
	msg->time_recv = sqlite3_column_int64(stm, 6);
	return msg;

error:
	sbk_free_message(msg);
	return NULL;
}

static struct sbk_message_list *
sbk_get_messages(struct sbk_ctx *ctx, sqlite3_stmt *stm)
{
	struct sbk_message_list	*lst;
	struct sbk_message	*msg;
	int			 ret;

	if ((lst = malloc(sizeof *lst)) == NULL) {
		sbk_error_set(ctx, NULL);
		goto error;
	}

	SIMPLEQ_INIT(lst);

	while ((ret = sbk_sqlite_step(ctx, ctx->db, stm)) == SQLITE_ROW) {
		if ((msg = sbk_get_message(ctx, stm)) == NULL)
			goto error;
		if (msg->conversation == NULL) {
			/* Likely message with error; skip it */
			sbk_free_message(msg);
		} else {
			SIMPLEQ_INSERT_TAIL(lst, msg, entries);
		}
	}

	if (ret != SQLITE_DONE)
		goto error;

	sqlite3_finalize(stm);
	return lst;

error:
	sbk_free_message_list(lst);
	sqlite3_finalize(stm);
	return NULL;
}

struct sbk_message_list *
sbk_get_all_messages(struct sbk_ctx *ctx)
{
	sqlite3_stmt	*stm;
	const char	*query;

	if (ctx->db_version < 20)
		query = SBK_MESSAGES_QUERY_8;
	else
		query = SBK_MESSAGES_QUERY_20;

	if (sbk_sqlite_prepare(ctx, ctx->db, &stm, query) == -1)
		return NULL;

	return sbk_get_messages(ctx, stm);
}

int
sbk_open(struct sbk_ctx **ctx, const char *dir)
{
	char	*dbfile, *keyfile;
	int	 ret;
	char	 key[128];

	dbfile = NULL;
	keyfile = NULL;
	ret = -1;

	if ((*ctx = malloc(sizeof **ctx)) == NULL)
		goto out;

	(*ctx)->error = NULL;
	RB_INIT(&(*ctx)->recipients);

	if (asprintf(&dbfile, "%s/sql/db.sqlite", dir) == -1) {
		sbk_error_setx(*ctx, "asprintf() failed");
		dbfile = NULL;
		goto out;
	}

	if (asprintf(&keyfile, "%s/config.json", dir) == -1) {
		sbk_error_setx(*ctx, "asprintf() failed");
		keyfile = NULL;
		goto out;
	}

	if (access(dbfile, F_OK) == -1) {
		sbk_error_set(*ctx, "%s", dbfile);
		goto out;
	}

	if (sbk_sqlite_open(*ctx, &(*ctx)->db, dbfile, SQLITE_OPEN_READONLY) ==
	    -1)
		goto out;

	if (sbk_get_key(*ctx, key, sizeof key, keyfile) == -1)
		goto out;

	if (sbk_sqlite_key(*ctx, (*ctx)->db, key) == -1) {
		explicit_bzero(key, sizeof key);
		goto out;
	}

	explicit_bzero(key, sizeof key);

	/* Verify key */
	if (sqlite3_exec((*ctx)->db, "SELECT count(*) FROM sqlite_master",
	    NULL, NULL, NULL) != SQLITE_OK) {
		sbk_error_setx(*ctx, "Incorrect key");
		goto out;
	}

	if (((*ctx)->db_version = sbk_get_database_version(*ctx)) == -1)
		goto out;

	if ((*ctx)->db_version < 19) {
		sbk_error_setx(*ctx, "Database version not supported (yet)");
		goto out;
	}

	ret = 0;

out:
	free(dbfile);
	free(keyfile);
	return ret;
}

void
sbk_close(struct sbk_ctx *ctx)
{
	if (ctx != NULL) {
		sqlite3_close(ctx->db);
		sbk_free_recipient_tree(ctx);
		sbk_error_clear(ctx);
	}
}

const char *
sbk_error(struct sbk_ctx *ctx)
{
	if (ctx == NULL)
		return strerror(ENOMEM);
	else
		return (ctx->error != NULL) ? ctx->error : "Unknown error";
}
