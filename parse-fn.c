/* $Id$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicm@users.sourceforge.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <ctype.h>
#include <pwd.h>
#include <netdb.h>
#include <fnmatch.h>
#include <string.h>

#include "fdm.h"
#include "fetch.h"
#include "match.h"

void
free_strings(struct strings *sp)
{
	u_int	i;

	for (i = 0; i < ARRAY_LENGTH(sp); i++) {
		xfree(ARRAY_ITEM(sp, i));
	}
	ARRAY_FREE(sp);
}

struct strings *
weed_strings(struct strings *sp)
{
	u_int	 i, j;
	char	*s;

	if (ARRAY_LENGTH(sp) == 0)
		return (sp);

	for (i = 0; i < ARRAY_LENGTH(sp) - 1; i++) {
		s = ARRAY_ITEM(sp, i);
		if (s == NULL)
			continue;

		for (j = i + 1; j < ARRAY_LENGTH(sp); j++) {
			if (ARRAY_ITEM(sp, j) == NULL)
				continue;

			if (strcmp(s, ARRAY_ITEM(sp, j)) == 0) {
				xfree(ARRAY_ITEM(sp, j));
				ARRAY_ITEM(sp, j) = NULL;
			}
		}
	}

	i = 0;
	while (i < ARRAY_LENGTH(sp)) {
		if (ARRAY_ITEM(sp, i) == NULL)
			ARRAY_REMOVE(sp, i);
		else
			i++;
	}

	return (sp);
}

struct users *
weed_users(struct users *up)
{
	u_int	i, j;
	uid_t	uid;

	if (ARRAY_LENGTH(up) == 0)
		return (up);

	for (i = 0; i < ARRAY_LENGTH(up) - 1; i++) {
		uid = ARRAY_ITEM(up, i);
		if (uid == NOUSR)
			continue;

		for (j = i + 1; j < ARRAY_LENGTH(up); j++) {
			if (ARRAY_ITEM(up, j) == uid)
				ARRAY_ITEM(up, j) = NOUSR;
		}
	}

	i = 0;
	while (i < ARRAY_LENGTH(up)) {
		if (ARRAY_ITEM(up, i) == NOUSR)
			ARRAY_REMOVE(up, i);
		else
			i++;
	}

	return (up);
}

char *
fmt_replstrs(const char *prefix, struct replstrs *rsp)
{
	return (fmt_strings(prefix, (struct strings *) rsp)); /* XXX */
}

void
free_replstrs(struct replstrs *rsp)
{
	return (free_strings((struct strings *) rsp)); /* XXX */
}

char *
fmt_strings(const char *prefix, struct strings *sp)
{
	char	*buf, *s;
	size_t	 slen, len;
	ssize_t	 off;
	u_int	 i;

	if (ARRAY_LENGTH(sp) == 0) {
		if (prefix != NULL)
			return (xstrdup(prefix));
		return (xstrdup(""));
	}
	if (ARRAY_LENGTH(sp) == 1) {
		s = ARRAY_FIRST(sp);
		if (prefix != NULL)
			xasprintf(&buf, "%s\"%s\"", prefix, s);
		else
			xasprintf(&buf, "\"%s\"", s);
		return (buf);
	}

	len = BUFSIZ;
	buf = xmalloc(len);
	off = 0;
	if (prefix != NULL) {
		ENSURE_SIZE(buf, len, strlen(prefix));
		off = strlcpy(buf, prefix, len);
	}

	for (i = 0; i < ARRAY_LENGTH(sp); i++) {
		s = ARRAY_ITEM(sp, i);
		slen = strlen(s);

		ENSURE_FOR(buf, len, off, slen + 4);
		xsnprintf(buf + off, len - off, "\"%s\" ", s);
		off += slen + 3;
	}

	buf[off - 1] = '\0';

	return (buf);

}

char *
fmt_users(const char *prefix, struct users *up)
{
	char	*buf;
	size_t	 uidlen, len;
	ssize_t	 off;
	u_int	 i;
	uid_t	 uid;

	if (ARRAY_LENGTH(up) == 0) {
		if (prefix != NULL)
			return (xstrdup(prefix));
		return (xstrdup(""));
	}
	if (ARRAY_LENGTH(up) == 1) {
		uid = ARRAY_FIRST(up);
		if (prefix != NULL)
			xasprintf(&buf, "%s%lu", prefix, (u_long) uid);
		else
			xasprintf(&buf, "%lu", (u_long) uid);
		return (buf);
	}

	len = BUFSIZ;
	buf = xmalloc(len);
	off = 0;
	if (prefix != NULL) {
		ENSURE_SIZE(buf, len, strlen(prefix));
		off = strlcpy(buf, prefix, len);
	}

	for (i = 0; i < ARRAY_LENGTH(up); i++) {
		uid = ARRAY_ITEM(up, i);
		uidlen = xsnprintf(NULL, 0, "%lu", (u_long) uid);

		ENSURE_FOR(buf, len, off, uidlen + 2);
		xsnprintf(buf + off, len - off, "%lu ", (u_long) uid);
		off += uidlen + 1;
	}

	buf[off - 1] = '\0';

	return (buf);

}

struct account *
find_account(char *name)
{
	struct account	*a;

	TAILQ_FOREACH(a, &conf.accounts, entry) {
		if (strcmp(a->name, name) == 0)
			return (a);
	}
	return (NULL);
}

int
have_accounts(char *name)
{
	struct account	*a;

	TAILQ_FOREACH(a, &conf.accounts, entry) {
		if (name_match(name, a->name))
			return (1);
	}

	return (0);
}

struct action *
find_action(char *name)
{
	struct action	*t;

	TAILQ_FOREACH(t, &conf.actions, entry) {
		if (strcmp(t->name, name) == 0)
			return (t);
	}
	return (NULL);
}

struct actions *
match_actions(const char *name)
{
	struct action	*t;
	struct actions	*ta;

	ta = xmalloc(sizeof *ta);
	ARRAY_INIT(ta);

	TAILQ_FOREACH(t, &conf.actions, entry) {
		if (name_match(name, t->name))
			ARRAY_ADD(ta, t);
	}

	return (ta);
}

struct macro *
find_macro(const char *name)
{
	struct macro	*macro;

	TAILQ_FOREACH(macro, &parse_macros, entry) {
		if (strcmp(macro->name, name) == 0)
			return (macro);
	}

	return (NULL);
}

void
print_rule(struct rule *r)
{
	struct expritem	*ei;
	char		 s[BUFSIZ], *sa, *su, *ss, desc[DESCBUFSIZE];

	if (r->expr == NULL)
		strlcpy(s, "all ", sizeof s);
	else {
		*s = '\0';
		TAILQ_FOREACH(ei, r->expr, entry) {
			switch (ei->op) {
			case OP_AND:
				strlcat(s, "and ", sizeof s);
				break;
			case OP_OR:
				strlcat(s, "or ", sizeof s);
				break;
			case OP_NONE:
				break;
			}
			if (ei->inverted)
				strlcat(s, "not ", sizeof s);
			ei->match->desc(ei, desc, sizeof desc);
			strlcat(s, desc, sizeof s);
			strlcat(s, " ", sizeof s);
		}
	}
	if (r->accounts != NULL)
		sa = fmt_strings(" accounts=", r->accounts);
	else
		sa = xstrdup("");
	if (r->users != NULL)
		su = fmt_users(" users=", r->users);
	else
		su = xstrdup("");
	if (r->lambda != NULL) {
		make_actlist(r->lambda->list, desc, sizeof desc);
		log_debug2("added rule %u:%s%s matches=%slambda=%s", r->idx,
		    sa, su, s, desc);
	} else if (r->actions != NULL) {
		ss = fmt_replstrs(NULL, r->actions);
		log_debug2("added rule %u:%s%s matches=%sactions=%s", r->idx,
		    sa, su, s, ss);
		xfree(ss);
	} else
		log_debug2("added rule %u:%s matches=%snested", r->idx, sa, s);
	xfree(sa);
	xfree(su);
}

void
print_action(struct action *t)
{
	char		 s[BUFSIZ], *su;
	size_t		 off;

	if (t->users != NULL)
		su = fmt_users(" users=", t->users);
	else
		su = xstrdup("");
	off = xsnprintf(s, sizeof s, "added action \"%s\":%s deliver=",
	    t->name, su);
	xfree(su);

	make_actlist(t->list, s + off, (sizeof s) - off);
	log_debug2("%s", s);
}

void
make_actlist(struct actlist *tl, char *buf, size_t len)
{
	struct actitem			*ti;
	struct deliver_action_data	*data;
	char				 desc[DESCBUFSIZE], *s;
	size_t		 		 off;

	off = 0;
	TAILQ_FOREACH(ti, tl, entry) {
		if (ti->deliver != NULL)
			ti->deliver->desc(ti, desc, sizeof desc);
		else {
			data = ti->data;
			s = fmt_replstrs(NULL, data->actions);
			xsnprintf(desc, sizeof desc, "action %s", s);
			xfree(s);
		}
		off += xsnprintf(buf + off, len - off, "%u:%s ", ti->idx, desc);
		if (off >= len)
			break;
	}
}

void
free_action(struct action *t)
{
	struct actitem	*ti;

	if (t->users != NULL)
		ARRAY_FREEALL(t->users);

	while (!TAILQ_EMPTY(t->list)) {
		ti = TAILQ_FIRST(t->list);
		TAILQ_REMOVE(t->list, ti, entry);

		free_actitem(ti);
	}
	xfree(t->list);

	xfree(t);
}

void
free_actitem(struct actitem *ti)
{
	if (ti->deliver == &deliver_pipe || ti->deliver == &deliver_exec) {
		struct deliver_pipe_data		*data = ti->data;
		xfree(data->cmd.str);
	} else if (ti->deliver == &deliver_rewrite) {
		struct deliver_rewrite_data		*data = ti->data;
		xfree(data->cmd.str);
	} else if (ti->deliver == &deliver_write ||
	    ti->deliver == &deliver_append) {
		struct deliver_write_data		*data = ti->data;
		xfree(data->path.str);
	} else if (ti->deliver == &deliver_maildir) {
		struct deliver_maildir_data		*data = ti->data;
		xfree(data->path.str);
	} else if (ti->deliver == &deliver_remove_header) {
		struct deliver_remove_header_data	*data = ti->data;
		xfree(data->hdr.str);
	} else if (ti->deliver == &deliver_add_header) {
		struct deliver_add_header_data		*data = ti->data;
		xfree(data->hdr.str);
		xfree(data->value.str);
	} else if (ti->deliver == &deliver_mbox) {
		struct deliver_mbox_data		*data = ti->data;
		xfree(data->path.str);
	} else if (ti->deliver == &deliver_tag) {
		struct deliver_tag_data			*data = ti->data;
		xfree(data->key.str);
		if (data->value.str != NULL)
			xfree(data->value.str);
	} else if (ti->deliver == &deliver_to_cache) {
		struct deliver_to_cache_data		*data = ti->data;
		xfree(data->key.str);
		xfree(data->path);
	} else if (ti->deliver == &deliver_smtp) {
		struct deliver_smtp_data		*data = ti->data;
		xfree(data->to.str);
		xfree(data->server.host);
		xfree(data->server.port);
		if (data->server.ai != NULL)
			freeaddrinfo(data->server.ai);
	} else if (ti->deliver == NULL) {
		struct deliver_action_data		*data = ti->data;
		free_replstrs(data->actions);
		ARRAY_FREEALL(data->actions);
	}
	if (ti->data != NULL)
		xfree(ti->data);

	xfree(ti);
}

void
free_rule(struct rule *r)
{
	struct rule	*rr;
	struct expritem	*ei;

	if (r->accounts != NULL) {
		free_strings(r->accounts);
		ARRAY_FREEALL(r->accounts);
	}
	if (r->users != NULL)
		ARRAY_FREEALL(r->users);
	if (r->actions != NULL) {
		free_replstrs(r->actions);
		ARRAY_FREEALL(r->actions);
	}

	if (r->lambda != NULL)
		free_action(r->lambda);

	while (!TAILQ_EMPTY(&r->rules)) {
		rr = TAILQ_FIRST(&r->rules);
		TAILQ_REMOVE(&r->rules, rr, entry);
		free_rule(rr);
	}
	if (r->expr == NULL) {
		xfree(r);
		return;
	}

	while (!TAILQ_EMPTY(r->expr)) {
		ei = TAILQ_FIRST(r->expr);
		TAILQ_REMOVE(r->expr, ei, entry);

		if (ei->match == &match_regexp) {
			struct match_regexp_data	*data = ei->data;
			re_free(&data->re);
		} else if (ei->match == &match_command) {
			struct match_command_data	*data = ei->data;
			xfree(data->cmd.str);
			if (data->re.str != NULL)
				re_free(&data->re);
		} else if (ei->match == &match_tagged) {
			struct match_tagged_data	*data = ei->data;
			xfree(data->tag.str);
		} else if (ei->match == &match_string) {
			struct match_string_data	*data = ei->data;
			xfree(data->str.str);
			re_free(&data->re);
		} else if (ei->match == &match_in_cache) {
			struct match_in_cache_data	*data = ei->data;
			xfree(data->key.str);
			xfree(data->path);
		} else if (ei->match == &match_attachment) {
			struct match_attachment_data	*data = ei->data;
			if (data->op == ATTACHOP_ANYTYPE ||
			    data->op == ATTACHOP_ANYNAME)
				xfree(data->value.str.str);
		}
		if (ei->data != NULL)
			xfree(ei->data);

		xfree(ei);
	}
	xfree(r->expr);

	xfree(r);
}

void
free_cache(struct cache *cache)
{
	xfree(cache->path);
	xfree(cache);
}

void
free_account(struct account *a)
{
	if (a->users != NULL)
		ARRAY_FREEALL(a->users);

	if (a->fetch == &fetch_pop3) {
		struct fetch_pop3_data		*data = a->data;
		xfree(data->user);
		xfree(data->pass);
		xfree(data->server.host);
		xfree(data->server.port);
		if (data->server.ai != NULL)
			freeaddrinfo(data->server.ai);
	} else if (a->fetch == &fetch_imap) {
		struct fetch_imap_data		*data = a->data;
		xfree(data->user);
		xfree(data->pass);
		xfree(data->folder);
		xfree(data->server.host);
		xfree(data->server.port);
		if (data->server.ai != NULL)
			freeaddrinfo(data->server.ai);
	} else if (a->fetch == &fetch_imappipe) {
		struct fetch_imap_data		*data = a->data;
		if (data->user != NULL)
			xfree(data->user);
		if (data->pass != NULL)
			xfree(data->pass);
		xfree(data->folder);
		xfree(data->pipecmd);
	} else if (a->fetch == &fetch_maildir) {
		struct fetch_maildir_data	*data = a->data;
		free_strings(data->maildirs);
		ARRAY_FREEALL(data->maildirs);
	} else if (a->fetch == &fetch_nntp) {
		struct fetch_nntp_data		*data = a->data;
		free_strings(data->names);
		ARRAY_FREEALL(data->names);
		xfree(data->path);
		xfree(data->server.host);
		xfree(data->server.port);
		if (data->server.ai != NULL)
			freeaddrinfo(data->server.ai);
	}
	if (a->data != NULL)
		xfree(a->data);

	xfree(a);
}

char *
expand_path(const char *path)
{
	const char	*src;
	char		*ptr;
	struct passwd	*pw;

	src = path;
	while (isspace((u_char) *src))
		src++;
	if (src[0] != '~')
		return (NULL);

	/* ~ */
	if (src[1] == '\0')
		return (xstrdup(conf.info.home));

	/* ~/ */
	if (src[1] == '/') {
		xasprintf(&ptr, "%s/%s", conf.info.home, src + 2);
		return (ptr);
	}

	/* ~user or ~user/ */
	ptr = strchr(src + 1, '/');
	if (ptr != NULL)
		*ptr = '\0';

	pw = getpwnam(src + 1);
	if (pw == NULL || pw->pw_dir == NULL || *pw->pw_dir == '\0') {
		endpwent();
		return (NULL);
	}

	if (ptr == NULL)
		ptr = xstrdup(pw->pw_dir);
	else
		xasprintf(&ptr, "%s/%s", pw->pw_dir, ptr + 1);

	endpwent();

	return (ptr);
}

void
find_netrc(const char *host, char **user, char **pass)
{
	FILE	*f;
	char	*cause;

	if ((f = netrc_open(conf.info.home, &cause)) == NULL)
		yyerror("%s", cause);

	if (netrc_lookup(f, host, user, pass) != 0)
		yyerror("error reading .netrc");

	if (user != NULL) {
		if (*user == NULL)
			yyerror("can't find user for \"%s\" in .netrc", host);
		if (**user == '\0')
			yyerror("invalid user");
	}
	if (pass != NULL) {
		if (*pass == NULL)
			yyerror("can't find pass for \"%s\" in .netrc", host);
		if (**pass == '\0')
			yyerror("invalid pass");
	}

	netrc_close(f);
}

char *
run_command(const char *s, const char *file)
{
	struct cmd	*cmd;
	char		*lbuf, *sbuf;
	size_t		 llen, slen;
	char		*cause, *out, *err;
	int		 status;

	if (*s == '\0')
		yyerror("empty command");

	log_debug3("running command: %s", s);
	if ((cmd = cmd_start(s, CMD_OUT, DEFTIMEOUT, NULL, 0, &cause)) == NULL)
		yyerror("%s: %s", s, cause);

	llen = IO_LINESIZE;
	lbuf = xmalloc(llen);

	slen = 1;
	sbuf = xmalloc(slen);

	*sbuf = '\0';
	do {
		status = cmd_poll(cmd, &out, &err, &lbuf, &llen, &cause);
		if (status == -1) {
			cmd_free(cmd);
			yyerror("%s: %s", s, cause);
		}
		if (status == 0) {
			if (err != NULL) {
				log_warnx("%s: %s: %s", file, s, err);
			}				
			if (out != NULL) {
				slen += strlen(out) + 1;
				sbuf = xrealloc(sbuf, 1, slen);
				strlcat(sbuf, out, slen);
				strlcat(sbuf, "\n", slen);
			}
		}
	} while (status == 0);
	status--;

	if (status != 0) {
		cmd_free(cmd);
		yyerror("%s: command returned %d", s, status);
	}

	cmd_free(cmd);

	slen--;
	while (slen > 0 && sbuf[slen - 1] == '\n') {
		sbuf[slen - 1] = '\0';
		slen--;
	}
	return (sbuf);
}