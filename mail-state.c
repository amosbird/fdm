/* $Id$ */

/*
 * Copyright (c) 2006 Nicholas Marriott <nicm@users.sourceforge.net>
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

#include <fnmatch.h>
#include <string.h>
#include <unistd.h>

#include "fdm.h"
#include "fetch.h"
#include "match.h"

struct strings *find_delivery_users(struct mail_ctx *, struct action *, int *);
int		fill_delivery_queue(struct mail_ctx *, struct rule *);

int		start_action(struct mail_ctx *, struct deliver_ctx *);
int		finish_action(struct deliver_ctx *, struct msg *,
		    struct msgbuf *);

#define ACTION_DONE 0
#define ACTION_ERROR 1
#define ACTION_PARENT 2

int
mail_match(struct mail_ctx *mctx, struct msg *msg, struct msgbuf *msgbuf)
{
	struct account	*a = mctx->account;
	struct mail	*m = mctx->mail;
	struct strings	*aa;
	struct expritem	*ei;
	u_int		 i;
	int		 error = MAIL_CONTINUE;
	char		*an, *tkey, *tvalue;

	set_wrapped(m, ' ');

	/*
	 * If blocked, check for msgs from parent.
	 */
	if (mctx->msgid != 0) {
		if (msg == NULL || msg->id != mctx->msgid)
			return (MAIL_BLOCKED);
		mctx->msgid = 0;

		if (msg->type != MSG_DONE)
			fatalx("child: unexpected message");
		if (msgbuf->buf == NULL || msgbuf->len == 0)
			fatalx("child: bad tags");
		strb_destroy(&m->tags);
		m->tags = msgbuf->buf;

		ei = mctx->expritem;
		switch (msg->data.error) {
		case MATCH_ERROR:
			return (MAIL_ERROR);
		case MATCH_TRUE:
			if (ei->op == OP_NONE || ei->op == OP_OR)
				mctx->result = 1;
			break;
		case MATCH_FALSE:
			if (ei->op == OP_AND)
				mctx->result = 0;
			break;
		default:
			fatalx("child: unexpected response");
		}

		goto next_expritem;
	}

	/*
	 * Check for completion and end of ruleset.
	 */
	if (mctx->done)
		return (MAIL_DONE);
	if (mctx->rule == NULL) {
		switch (conf.impl_act) {
		case DECISION_NONE:
			log_warnx("%s: reached end of ruleset. no "
			    "unmatched-mail option; keeping mail",  a->name);
			m->decision = DECISION_KEEP;
			break;
		case DECISION_KEEP:
			log_debug2("%s: reached end of ruleset. keeping mail",
			    a->name);
			m->decision = DECISION_KEEP;
			break;
		case DECISION_DROP:
			log_debug2("%s: reached end of ruleset. dropping mail",
			    a->name);
			m->decision = DECISION_DROP;
			break;
		}
		return (MAIL_DONE);
	}

	/*
	 * Expression not started. Start it.
	 */
	if (mctx->expritem == NULL) {
		/*
		 * Check rule account list. 
		 */
		aa = mctx->rule->accounts;
		if (aa != NULL && !ARRAY_EMPTY(aa)) {
			for (i = 0; i < ARRAY_LENGTH(aa); i++) {
				an = ARRAY_ITEM(aa, i, char *);
				if (name_match(an, a->name))
					break;
			}
			if (i == ARRAY_LENGTH(aa)) {
				mctx->result = 0;
				goto skip;
			}
		}

		/*
		 * No expression. Must be an "all" rule, treat it as always
		 * true.
		 */
		if (mctx->rule->expr == NULL || TAILQ_EMPTY(mctx->rule->expr)) {
			mctx->result = 1;
			goto skip;
		}
		
		/* 
		 * Start the expression.
		 */
		mctx->result = 0;
		mctx->expritem = TAILQ_FIRST(mctx->rule->expr);
	}

	/*
	 * Check this expression item and adjust the result.
	 */
	ei = mctx->expritem;
	switch (ei->match->match(mctx, ei)) {
	case MATCH_ERROR:
		return (MAIL_ERROR);
	case MATCH_PARENT:
		return (MAIL_BLOCKED);
	case MATCH_TRUE:
		if (ei->op == OP_NONE || ei->op == OP_OR)
			mctx->result = 1;
		break;
	case MATCH_FALSE:
		if (ei->op == OP_AND)
			mctx->result = 0;
		break;
	}

next_expritem:
	/*
	 * Move to the next item. If there is one, then return.
	 */
	mctx->expritem = TAILQ_NEXT(mctx->expritem, entry); 
	if (mctx->expritem != NULL)
		return (MAIL_CONTINUE);

skip:
	/*
	 * If the result was false, skip to find the next rule.
	 */
	if (!mctx->result)
		goto next_rule;
	mctx->matched = 1;
	log_debug2("%s: matched to rule %u", a->name, mctx->rule->idx);

	/* 
	 * If this rule is stop, mark the context so when we get back after
	 * delivery we know to stop.
	 */
	if (mctx->rule->stop)
		mctx->done = 1;

	/*
	 * Handle nested rules.
	 */
	if (!TAILQ_EMPTY(&mctx->rule->rules)) {
		log_debug2("%s: entering nested rules", a->name);

		/* 
		 * Stack the current rule (we are at the end of it so the
		 * the expritem must be NULL already).
		 */
		ARRAY_ADD(&mctx->stack, mctx->rule, struct rule *);

		/* 
		 * Continue with the first rule of the nested list.
		 */
		mctx->rule = TAILQ_FIRST(&mctx->rule->rules);
		return (MAIL_CONTINUE);
	}

	/* 
	 * Tag mail if necessary.
	 */
	if (mctx->rule->key.str != NULL) {
		tkey = replacestr(&mctx->rule->key, m->tags, m, &m->rml);
		tvalue = replacestr(&mctx->rule->value, m->tags, m, &m->rml);
		
		if (tkey != NULL && *tkey != '\0' && tvalue != NULL) {
			log_debug2("%s: tagging message: %s (%s)", a->name,
			    tkey, tvalue);
			add_tag(&m->tags, tkey, "%s", tvalue);
		}
		
		if (tkey != NULL)
			xfree(tkey);
		if (tvalue != NULL)
			xfree(tvalue);
	}

	/* 
	 * Fill the delivery action queue.
	 */  
	if (!ARRAY_EMPTY(mctx->rule->actions)) { 
		if (fill_delivery_queue(mctx, mctx->rule) != 0)
			return (MAIL_ERROR);
		error = MAIL_DELIVER;
	}

next_rule:
	/* 
	 * Move to the next rule.
	 */
	mctx->rule = TAILQ_NEXT(mctx->rule, entry);

	/*
	 * If no more rules, try to move up the stack. 
	 */
	while (mctx->rule == NULL) {
		if (ARRAY_EMPTY(&mctx->stack))
			break;
		mctx->rule = ARRAY_LAST(&mctx->stack, struct rule *);
		mctx->rule = TAILQ_NEXT(mctx->rule, entry);
		ARRAY_TRUNC(&mctx->stack, 1, struct rule *);
	}

	return (error);
}

int
mail_deliver(struct mail_ctx *mctx, struct msg *msg, struct msgbuf *msgbuf)
{
	struct account		*a = mctx->account;
	struct mail		*m = mctx->mail;
	struct deliver_ctx	*dctx;

	set_wrapped(m, '\n');

	/*
	 * If blocked, check for msgs from parent.
	 */
	if (mctx->msgid != 0) {
		if (msg == NULL || msg->id != mctx->msgid)
			return (MAIL_BLOCKED);
		mctx->msgid = 0;

		/* 
		 * Got message. Finish delivery.
		 */
		dctx = TAILQ_FIRST(&mctx->dqueue);
		if (finish_action(dctx, msg, msgbuf) == ACTION_ERROR)
			return (MAIL_ERROR);

		/*
		 * Move on to dequeue this delivery action.
		 */
		goto done;
	}

	/*
	 * Check if delivery is complete.
	 */
	if (TAILQ_EMPTY(&mctx->dqueue))
		return (MAIL_MATCH);
		
	/*
	 * Get the first delivery action and start it.
	 */
	dctx = TAILQ_FIRST(&mctx->dqueue);
	switch (start_action(mctx, dctx)) {
	case ACTION_ERROR:
		return (MAIL_ERROR);
	case ACTION_PARENT:
		return (MAIL_BLOCKED);
	}

done:
	/*
	 * Remove completed action from queue.
	 */
	TAILQ_REMOVE(&mctx->dqueue, dctx, entry);
	log_debug("%s: message %u delivered (rule %u, %s) after %.3f seconds", 
	    a->name, m->idx, dctx->rule->idx, 
	    dctx->action->deliver->name, get_time() - dctx->tim);
	xfree(dctx);
	return (MAIL_CONTINUE);
}

struct strings *
find_delivery_users(struct mail_ctx *mctx, struct action *t, int *should_free)
{
	struct account	*a = mctx->account;
	struct mail	*m = mctx->mail;
	struct rule	*r = mctx->rule;
	struct strings	*users;

	*should_free = 0;
	users = NULL;
	if (r->find_uid) {		/* rule comes first */
		*should_free = 1;
		users = find_users(m);
	} else if (r->users != NULL) {
		*should_free = 0;
		users = r->users;
	} else if (t->find_uid) {	/* then action */
		*should_free = 1;
		users = find_users(m);
	} else if (t->users != NULL) {
		*should_free = 0;
		users = t->users;
	} else if (a->find_uid) {	/* then account */
		*should_free = 1;
		users = find_users(m);
	} else if (a->users != NULL) {
		*should_free = 0;
		users = a->users;
	}
	if (users == NULL) {
		*should_free = 1;
		users = xmalloc(sizeof *users);
		ARRAY_INIT(users);
		ARRAY_ADD(users, conf.def_user, uid_t);
	}

	return (users);
}

int
fill_delivery_queue(struct mail_ctx *mctx, struct rule *r)
{
	struct account		*a = mctx->account;
	struct mail		*m = mctx->mail;
	struct action		*t;
	struct actions		*ta;
	u_int		 	 i, j, k;
	char			*s;
	struct replstr		*rs;
	struct deliver_ctx	*dctx;
	struct strings		*users;
	int			 should_free;

	for (i = 0; i < ARRAY_LENGTH(r->actions); i++) {
		rs = &ARRAY_ITEM(r->actions, i, struct replstr);
		s = replacestr(rs, m->tags, m, &m->rml);

		log_debug2("%s: looking for actions matching: %s", a->name, s);
		ta = match_actions(s);
		if (ARRAY_EMPTY(ta))
			goto empty;
		xfree(s);

		log_debug2("%s: found %u actions", a->name, ARRAY_LENGTH(ta));
		for (j = 0; j < ARRAY_LENGTH(ta); j++) {
			t = ARRAY_ITEM(ta, j, struct action *);
			users = find_delivery_users(mctx, t, &should_free);

			for (k = 0; k < ARRAY_LENGTH(users); k++) {
				dctx = xcalloc(1, sizeof *dctx);
				dctx->action = t;
				dctx->account = a;
				dctx->rule = r;
				dctx->mail = m;
				dctx->uid = ARRAY_ITEM(users, k, uid_t);

				log_debug3("%s: action %s, uid %lu", a->name,
				    t->name, (u_long) dctx->uid);
				TAILQ_INSERT_TAIL(&mctx->dqueue, dctx, entry);
			}

			if (should_free)
				ARRAY_FREEALL(users);
		}

		ARRAY_FREEALL(ta);
	}

	return (0);

empty:
	xfree(s);
	ARRAY_FREEALL(ta);
	log_warnx("%s: no actions matching: %s (%s)", a->name, s, rs->str);
	return (1);

}

int
start_action(struct mail_ctx *mctx, struct deliver_ctx *dctx)
{
	struct account	*a = dctx->account;
	struct action	*t = dctx->action;
	struct mail	*m = dctx->mail;
	struct mail	*md = &dctx->wr_mail;
	struct msg	 msg;
	struct msgbuf	 msgbuf;
	u_int		 lines;	

	dctx->tim = get_time();
 	if (t->deliver->deliver == NULL)
		return (0);

	log_debug2("%s: message %u, running action %s as user %lu",
	    a->name, m->idx, t->name, (u_long) dctx->uid);
	add_tag(&m->tags, "action", "%s", t->name);

	/* just deliver now for in-child delivery */
	if (t->deliver->type == DELIVER_INCHILD) {
		if (t->deliver->deliver(dctx, t) != DELIVER_SUCCESS)
			return (ACTION_ERROR);
		return (ACTION_DONE);
	}

#if 0
	/* if the current user is the same as the deliver user, don't bother
	   passing up either */
	if (t->deliver->type == DELIVER_ASUSER && dctx->uid == geteuid()) {
		if (t->deliver->deliver(dctx, t) != DELIVER_SUCCESS)
			return (ACTION_ERROR);
		return (ACTION_DONE);
	}
	if (t->deliver->type == DELIVER_WRBACK && dctx->uid == geteuid()) {
		mail_open(md, IO_BLOCKSIZE);
		md->decision = m->decision;
		
		if (t->deliver->deliver(dctx, t) != DELIVER_SUCCESS) {
			mail_destroy(md);
			return (ACTION_ERROR);
		}

		memcpy(&msg.data.mail, md, sizeof msg.data.mail);
		cleanup_deregister(md->shm.name);
		strb_destroy(&md->tags);

		mail_receive(m, &msg);
		log_debug2("%s: received modified mail: size %zu, body %zd",
		    a->name, m->size, m->body);

		/* trim from line */
		trim_from(m);
	
		/* and recreate the wrapped array */
		lines = fill_wrapped(m);
		log_debug2("%s: found %u wrapped lines", a->name, lines);
		
		return (ACTION_DONE);
	}
#endif

	memset(&msg, 0, sizeof msg);
	msg.type = MSG_ACTION;
	msg.id = m->idx;

	msg.data.account = a;
	msg.data.action = t;
	msg.data.uid = dctx->uid;

	msgbuf.buf = m->tags;
	msgbuf.len = STRB_SIZE(m->tags);

	mail_send(m, &msg);

	log_debug3("%s: sending action to parent", a->name);
	if (privsep_send(mctx->io, &msg, &msgbuf) != 0) 
		fatalx("child: privsep_send error");

	mctx->msgid = msg.id;
	return (ACTION_PARENT);
}

int
finish_action(struct deliver_ctx *dctx, struct msg *msg, struct msgbuf *msgbuf)
{	
	struct account	*a = dctx->account;
	struct action	*t = dctx->action;
	struct mail	*m = dctx->mail;
	u_int		 lines;
	
	if (msgbuf->buf == NULL || msgbuf->len == 0)
		fatalx("child: bad tags");
	strb_destroy(&m->tags);
	m->tags = msgbuf->buf;
	update_tags(&m->tags);
	
	if (msg->data.error != 0)
		return (ACTION_ERROR);
	
	if (t->deliver->type != DELIVER_WRBACK)
		return (ACTION_DONE);

	mail_receive(m, msg);
	log_debug2("%s: message %u, received modified mail: size %zu, body %zd",
	    a->name, m->idx, m->size, m->body);

	/* trim from line */
	trim_from(m);
	
	/* and recreate the wrapped array */
	lines = fill_wrapped(m);
	log_debug2("%s: found %u wrapped lines", a->name, lines);

	return (ACTION_DONE);
}

/* -------------------------------------------------------------------------- */
#if 0
 int
run_match(struct account *a, const char **cause)
{
	switch (fetch_rule(mctx, cause)) {
	case FETCH_ERROR:
		return (1);
	case FETCH_AGAIN:
		/* delivering mail, queue for delivery */
		log_debug3("%s: %u, adding to deliver queue", a->name, m->idx);
		TAILQ_REMOVE(&matchq, mctx, entry);
		TAILQ_INSERT_TAIL(&deliverq, mctx, entry);
		break;
	case FETCH_COMPLETE:
		/* finished with mail, queue on done queue */
		log_debug3("%s: %u, adding to done queue", a->name, m->idx);
		TAILQ_REMOVE(&matchq, mctx, entry);
		TAILQ_INSERT_TAIL(&doneq, mctx, entry);

		/*
		 * Destroy mail data now it is finished, just keep the mail
		 * structure.
		 */
		shm_destroy(&mctx->mail->shm);
		break;
	}

	return (0);
}

int
run_done(struct account *a, const char **cause)
{
	struct match_ctx	*mctx;
	struct mail		*m;
	int			 error = 0;
	const char		*type;

	if (TAILQ_EMPTY(&doneq))
		return (0);
	
	mctx = TAILQ_FIRST(&doneq);
	m = mctx->mail;
	log_debug3("%s: running done queue", a->name);

	TAILQ_REMOVE(&doneq, mctx, entry);
	ARRAY_FREE(&mctx->stack);
	log_debug("%s: message %u done after %.3f seconds", a->name, m->idx,
	    get_time() - mctx->tim);
	xfree(mctx);

	if (a->fetch->done != NULL) {
		switch (m->decision) {
		case DECISION_DROP:
			type = "deleting";
			dropped++;
			break;
		case DECISION_KEEP:
			type = "keeping";
			kept++;
			break;
		default:
			fatalx("invalid decision");
		}
		log_debug("%s: %s message %u", a->name, type, m->idx);
		
		if (a->fetch->done(a, m) != FETCH_SUCCESS) {
			*cause = type;
			error = 1;
		}
	}

	mail_destroy(m);
	xfree(m);

	return (error);
}

void
flush_queue(struct match_queue *mq)
{
	struct match_ctx	*mctx;
	struct deliver_ctx	*dctx;
	struct mail		*m;

	while (!TAILQ_EMPTY(mq)) {
		mctx = TAILQ_FIRST(mq);
		m = mctx->mail;

		TAILQ_REMOVE(mq, mctx, entry);
		while (!TAILQ_EMPTY(&mctx->dqueue)) {
			dctx = TAILQ_FIRST(&mctx->dqueue);
			TAILQ_REMOVE(&mctx->dqueue, dctx, entry);
			xfree(dctx);
		}
		ARRAY_FREE(&mctx->stack);
		xfree(mctx);
		
		mail_destroy(m);
		xfree(m);
	}
}

int
run_deliver(struct account *a, const char **cause)
{
	struct match_ctx	*mctx;
	struct mail		*m;
	struct deliver_ctx	*dctx;

	if (TAILQ_EMPTY(&deliverq))
		return (0);
	
	mctx = TAILQ_FIRST(&deliverq);
	m = mctx->mail;

	if (TAILQ_EMPTY(&mctx->dqueue)) {
		/* delivery done. return to match queue */
		log_debug3("%s: %u, returning to match queue", a->name, m->idx);
		TAILQ_REMOVE(&deliverq, mctx, entry);
		TAILQ_INSERT_HEAD(&matchq, mctx, entry);
		return (0);
	}

	/* start the first action */
	log_debug3("%s: running deliver queue", a->name);
	dctx = TAILQ_FIRST(&mctx->dqueue);

	switch (start_action(mctx, dctx)) {
	case ACTION_ERROR:
		*cause = "delivery";
		return (1);
	case ACTION_PARENT:
		log_debug3("%s: %u, adding to blocked queue", a->name, m->idx);
		TAILQ_REMOVE(&deliverq, mctx, entry);
		TAILQ_INSERT_HEAD(&blockedq, mctx, entry);
		return (0);
	}

	TAILQ_REMOVE(&mctx->dqueue, dctx, entry);
	log_debug("%s: message %u delivered (rule %u, %s) after %.3f seconds", 
	    a->name, mctx->mail->idx, dctx->rule->idx, 
	    dctx->action->deliver->name,  get_time() - dctx->tim);
	xfree(dctx);
	return (0);
}

int
recvd_deliver(struct msg *msg, struct msgbuf *msgbuf, void *data, 
    const char **cause)
{
	struct match_ctx	*mctx = data;
	struct account		*a = mctx->account;
	struct mail		*m = mctx->mail;
	struct deliver_ctx	*dctx;
	
	if (msg->type != MSG_DONE)
		fatalx("child: unexpected message");

	log_debug3("%s: %u, returning to deliver queue", a->name, m->idx);
	TAILQ_REMOVE(&blockedq, mctx, entry);
	TAILQ_INSERT_HEAD(&deliverq, mctx, entry);

	dctx = TAILQ_FIRST(&mctx->dqueue);
	if (finish_action(dctx, msg, msgbuf) != ACTION_DONE) {
		*cause = "delivery";
		return (1);
	}

	TAILQ_REMOVE(&mctx->dqueue, dctx, entry);
	log_debug("%s: message %u delivered (rule %u, %s) after %.3f seconds", 
	    a->name, mctx->mail->idx, dctx->rule->idx, 
	    dctx->action->deliver->name,  get_time() - dctx->tim);
	xfree(dctx);
	return (0);
}

int
fetch_rule(struct match_ctx *mctx, const char **cause)
{
	struct account		*a = mctx->account;
	struct strings		*aa;
	struct mail		*m = mctx->mail;
	struct rule		*r = mctx->rule;
	u_int		 	 i;
	int		 	 error;
	char			*tkey, *tvalue;

	/* matching finished */
	if (m->done) {
		if (conf.keep_all || a->keep)
			m->decision = DECISION_KEEP;
		return (FETCH_COMPLETE);
	}

	/* end of ruleset reached */
	if (r == NULL) {
		switch (conf.impl_act) {
		case DECISION_NONE:
			log_warnx("%s: reached end of ruleset. no "
			    "unmatched-mail option; keeping mail",  a->name);
			m->decision = DECISION_KEEP;
			break;
		case DECISION_KEEP:
			log_debug2("%s: reached end of ruleset. keeping mail",
			    a->name);
			m->decision = DECISION_KEEP;
			break;
		case DECISION_DROP:
			log_debug2("%s: reached end of ruleset. dropping mail",
			    a->name);
			m->decision = DECISION_DROP;
			break;
		}
		m->done = 1;
		return (FETCH_SUCCESS);
	}

	mctx->rule = TAILQ_NEXT(mctx->rule, entry);
	while (mctx->rule == NULL) {
		if (ARRAY_EMPTY(&mctx->stack))
			break;
		mctx->rule = ARRAY_LAST(&mctx->stack, struct rule *);
		mctx->rule = TAILQ_NEXT(mctx->rule, entry);
		ARRAY_TRUNC(&mctx->stack, 1, struct rule *);
	}

 	aa = r->accounts;
	if (!ARRAY_EMPTY(aa)) {
		for (i = 0; i < ARRAY_LENGTH(aa); i++) {
			if (name_match(ARRAY_ITEM(aa, i, char *), a->name))
				break;
		}
		if (i == ARRAY_LENGTH(aa))
			return (FETCH_SUCCESS);
	}

	/* match all the regexps */
	switch (r->type) {
	case RULE_EXPRESSION:
		/* combine wrapped lines */
		set_wrapped(m, ' ');
		
		/* perform the expression */
		if ((error = do_expr(r, mctx)) == -1) {
			*cause = "matching";
			return (FETCH_ERROR);
		}
		
		/* continue if no match */
		if (!error)
			return (FETCH_SUCCESS);
		break;
	case RULE_ALL:
		break;
	}

	/* reset wrapped lines */
	set_wrapped(m, '\n');
		
	/* report rule number */
	if (TAILQ_EMPTY(&r->rules))
		log_debug2("%s: matched to rule %u", a->name, r->idx);
	else
		log_debug2("%s: matched to rule %u (nested)", a->name, r->idx);

	/* deal with nested rules */
	if (!TAILQ_EMPTY(&r->rules)) {
		log_debug2("%s: entering nested rules", a->name);
		ARRAY_ADD(&mctx->stack, r, struct rule *);
		mctx->rule = TAILQ_FIRST(&r->rules);
		return (FETCH_SUCCESS);
	}
	
	/* tag mail if needed */
	if (r->key.str != NULL) {
		tkey = replacestr(&r->key, m->tags, m, &m->rml);
		tvalue = replacestr(&r->value, m->tags, m, &m->rml);
		
		if (tkey != NULL && *tkey != '\0' && tvalue != NULL) {
			log_debug2("%s: tagging message: %s (%s)", 
			    a->name, tkey, tvalue);
			add_tag(&m->tags, tkey, "%s", tvalue);
		}
		
		if (tkey != NULL)
			xfree(tkey);
		if (tvalue != NULL)
			xfree(tvalue);
	}
1	/* if this rule is marked as stop, mark the mail as done */
	if (r->stop)
		m->done = 1;

	/* handle delivery */
	if (r->actions != NULL) {
		log_debug2("%s: delivering message", a->name);
		mctx->matched = 1;
		if (do_deliver(r, mctx) != 0) {
			*cause = "delivery";
			return (FETCH_ERROR);
		}
		return (FETCH_AGAIN);
	}

	return (FETCH_SUCCESS);
}

int
do_expr(struct rule *r, struct match_ctx *mctx)
{
	int		 fres, cres;
	struct expritem	*ei;
	char		 desc[DESCBUFSIZE];

	fres = 0;
	TAILQ_FOREACH(ei, r->expr, entry) {
		cres = ei->match->match(mctx, ei);
		if (cres == MATCH_ERROR)
			return (-1);
		cres = cres == MATCH_TRUE;
		if (ei->inverted)
			cres = !cres;
		switch (ei->op) {
		case OP_NONE:
		case OP_OR:
			fres = fres || cres;
			break;
		case OP_AND:
			fres = fres && cres;
			break;
		}

		ei->match->desc(ei, desc, sizeof desc);
		log_debug2("%s: tried %s%s, got %d", mctx->account->name,
		    ei->inverted ? "not " : "", desc, cres);
	}

	return (fres);
}

int
do_deliver(struct rule *r, struct match_ctx *mctx)
{
	struct account		*a = mctx->account;
	struct mail		*m = mctx->mail;
	struct action		*t;
	struct actions		*ta;
	u_int		 	 i, j, k;
	char			*s;
	struct replstr		*rs;
	struct deliver_ctx	*dctx;
	struct strings		*users;
	int			 should_free;

	for (i = 0; i < ARRAY_LENGTH(r->actions); i++) {
		rs = &ARRAY_ITEM(r->actions, i, struct replstr);
		s = replacestr(rs, m->tags, m, &m->rml);

		log_debug2("%s: looking for actions matching: %s", a->name, s);
		ta = match_actions(s);
		if (ARRAY_EMPTY(ta))
			goto empty;
		xfree(s);

		log_debug2("%s: found %u actions", a->name, ARRAY_LENGTH(ta));
		for (j = 0; j < ARRAY_LENGTH(ta); j++) {
			t = ARRAY_ITEM(ta, j, struct action *);
			users = get_users(mctx, r, t, &should_free);

			for (k = 0; k < ARRAY_LENGTH(users); k++) {
				dctx = xmalloc(sizeof *dctx);
				dctx->action = t;
				dctx->account = a;
				dctx->rule = r;
				dctx->mail = m;
				dctx->uid = ARRAY_ITEM(users, k, uid_t);

				TAILQ_INSERT_TAIL(&mctx->dqueue, dctx, entry);
			}

			if (should_free)
				ARRAY_FREEALL(users);
		}

		ARRAY_FREEALL(ta);
	}

	return (0);

empty:
	xfree(s);
	ARRAY_FREEALL(ta);
	log_warnx("%s: no actions matching: %s (%s)", a->name, s, rs->str);
	return (1);
}
#endif
