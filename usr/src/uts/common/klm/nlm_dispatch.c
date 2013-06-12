/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy is of the CDDL is also available via the Internet
 * at http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2013 Nexenta Systems, Inc.  All rights reserved.
 */

/*
 * NFS Lock Manager, server-side dispatch tables and
 * dispatch programs: nlm_prog_3, nlm_prog4
 *
 * These are called by RPC framework after the RPC service
 * endpoints setup done in nlm_impl.c: nlm_svc_add_ep().
 *
 * Originally from rpcgen, then reduced.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sdt.h>
#include <rpcsvc/nlm_prot.h>
#include "nlm_impl.h"

/*
 * Entries in the dispatch tables below.
 */
struct dispatch_entry {
	/* de_func args: argp, resp, svcreq */
	bool_t		(*de_func)();
	xdrproc_t	de_xargs;
	xdrproc_t	de_xres;
	uint_t		de_flags;
	/* Flag bits in de_flags */

#define	NLM_DISP_NOREPLY	1	/* Skip svc_sendreply */
#define	NLM_DISP_NOREMOTE	2	/* Local calls only */
};

/*
 * Cast macro for dispatch table function pointers.
 * Intentionally does not declare arg types.
 */
#define	RPCGEN_ACTION(func)	(bool_t (*)())func

/* ARGSUSED */
static bool_t
nlm_null_svc(void *args, void *resp, struct svc_req *sr)
{
	return (TRUE);
}

/*
 * The common NLM service dispatch function, used by
 * both: nlm_prog_3, nlm_prog_4
 */
void
nlm_dispatch(
	struct svc_req *rqstp,
	SVCXPRT *transp,
	const struct dispatch_entry *de)
{
	union {
		/* All the arg types */
		nlm_cancargs	au_cancargs;
		nlm_lockargs	au_lockargs;
		nlm_notify	au_notify;
		nlm_res		au_res;
		nlm_shareargs	au_shareargs;
		nlm_sm_status	au_sm_status;
		nlm_testargs	au_testargs;
		nlm_testres	au_testres;
		nlm_unlockargs	au_unlockargs;
		nlm4_cancargs	au_cancargs4;
		nlm4_lockargs	au_lockargs4;
		nlm4_notify	au_notify4;
		nlm4_res	au_res4;
		nlm4_shareargs	au_shareargs4;
		nlm4_testargs	au_testargs4;
		nlm4_testres	au_testres4;
		nlm4_unlockargs	au_unlockargs4;
	} argu;
	union {
		/* All the ret types */
		int		ru_int;
		nlm_res		ru_res;
		nlm_shareres	ru_shareres;
		nlm_testres	ru_testres;
		nlm4_res	ru_res4;
		nlm4_shareres	ru_shareres4;
		nlm4_testres	ru_testres4;

	} resu;
	bool_t (*func)(char *, void *, struct svc_req *);
	bool_t do_reply;

	if ((func = de->de_func) == NULL) {
		svcerr_noproc(transp);
		return;
	}


	if ((de->de_flags & NLM_DISP_NOREMOTE) &&
	    !nlm_caller_is_local(transp)) {
		svcerr_noproc(transp);
		return;
	}

	/*
	 * This section from rpcgen, and then modified slightly.
	 *
	 * Dispatch entries that should _never_ send a response
	 * (i.e. all the _MSG and _RES entries) put NULL in the
	 * de_xres field to indicate that.  For such entries, we
	 * will NOT call svc_sendreply nor xdr_free().  Normal
	 * dispatch entries skip svc_sendreply if the dispatch
	 * function returns zero, but always call xdr_free().
	 *
	 * There are more complex cases where some dispatch
	 * functions need to send their own reply.  We chose
	 * to indicate those using a flag (NLM_DISP_NOREPLY).
	 * XXX: In retrospect a NULL de_xres field might have
	 * worked just as well.  That service function would
	 * simply need to call xdr_free itself.  (would only
	 * affect nlm_do_lock and it's callers)
	 */
	bzero((char *)&argu, sizeof (argu));
	if (!SVC_GETARGS(transp, de->de_xargs, (caddr_t)&argu)) {
		svcerr_decode(transp);
		return;
	}

	bzero((char *)&resu, sizeof (resu));
	do_reply = (*func)((char *)&argu, (void *)&resu, rqstp);

	if (do_reply && !(de->de_flags & NLM_DISP_NOREPLY)) {
		ASSERT(de->de_xres != (xdrproc_t)0);
		DTRACE_PROBE3(sendreply, struct svc_req *, rqstp,
		    SVCXPRT *, transp, struct dispatch_entry *, de);

		if (!svc_sendreply(transp, de->de_xres, (char *)&resu)) {
			svcerr_systemerr(transp);
			NLM_ERR("nlm_dispatch(): svc_sendreply() failed!\n");
		}
	}

	if (!SVC_FREEARGS(transp, de->de_xargs, (caddr_t)&argu))
		NLM_WARN("nlm_dispatch(): unable to free arguments");

	if (de->de_xres != (xdrproc_t)0)
		xdr_free(de->de_xres, (caddr_t)&resu);
}

/*
 * Dispatch tables for each program version.
 *
 * The tables here were all originally from rpcgen,
 * but then arg/resp sizes removed, flags added.
 */

/*
 * Dispatch table for versions 1, 2, 3
 * (NLM_VERS, NLM_SM, NLM_VERSX)
 */
static const struct dispatch_entry
nlm_prog_3_dtable[] = {

	/*
	 * Version 1 (NLM_VERS) entries.
	 */

	{ /* 0: NULLPROC */
	RPCGEN_ACTION(nlm_null_svc),
	(xdrproc_t)xdr_void,
	(xdrproc_t)xdr_void,
	0 },

	{ /* 1: NLM_TEST */
	RPCGEN_ACTION(nlm_test_1_svc),
	(xdrproc_t)xdr_nlm_testargs,
	(xdrproc_t)xdr_nlm_testres,
	0 },

	{ /* 2: NLM_LOCK */
	RPCGEN_ACTION(nlm_lock_1_svc),
	(xdrproc_t)xdr_nlm_lockargs,
	(xdrproc_t)xdr_nlm_res,
	NLM_DISP_NOREPLY },	/* Does it's own reply. */

	{ /* 3: NLM_CANCEL */
	RPCGEN_ACTION(nlm_cancel_1_svc),
	(xdrproc_t)xdr_nlm_cancargs,
	(xdrproc_t)xdr_nlm_res,
	0 },

	{ /* 4: NLM_UNLOCK */
	RPCGEN_ACTION(nlm_unlock_1_svc),
	(xdrproc_t)xdr_nlm_unlockargs,
	(xdrproc_t)xdr_nlm_res,
	0 },

	{ /* 5: NLM_GRANTED */
	RPCGEN_ACTION(nlm_granted_1_svc),
	(xdrproc_t)xdr_nlm_testargs,
	(xdrproc_t)xdr_nlm_res,
	0 },

	/*
	 * All the _MSG and _RES entries are "one way" calls that
	 * skip the usual RPC reply.  We give them a null xdr_res
	 * function so the dispatcher will not send a reply.
	 */

	{ /* 6: NLM_TEST_MSG */
	RPCGEN_ACTION(nlm_test_msg_1_svc),
	(xdrproc_t)xdr_nlm_testargs,
	(xdrproc_t)0,
	0 },

	{ /* 7: NLM_LOCK_MSG */
	RPCGEN_ACTION(nlm_lock_msg_1_svc),
	(xdrproc_t)xdr_nlm_lockargs,
	(xdrproc_t)0,
	0 },

	{ /* 8: NLM_CANCEL_MSG */
	RPCGEN_ACTION(nlm_cancel_msg_1_svc),
	(xdrproc_t)xdr_nlm_cancargs,
	(xdrproc_t)0,
	0 },

	{ /* 9: NLM_UNLOCK_MSG */
	RPCGEN_ACTION(nlm_unlock_msg_1_svc),
	(xdrproc_t)xdr_nlm_unlockargs,
	(xdrproc_t)0,
	0 },

	{ /* 10: NLM_GRANTED_MSG */
	RPCGEN_ACTION(nlm_granted_msg_1_svc),
	(xdrproc_t)xdr_nlm_testargs,
	(xdrproc_t)0,
	0 },

	{ /* 11: NLM_TEST_RES */
	RPCGEN_ACTION(nlm_test_res_1_svc),
	(xdrproc_t)xdr_nlm_testres,
	(xdrproc_t)0,
	0 },

	{ /* 12: NLM_LOCK_RES */
	RPCGEN_ACTION(nlm_lock_res_1_svc),
	(xdrproc_t)xdr_nlm_res,
	(xdrproc_t)0,
	0 },

	{ /* 13: NLM_CANCEL_RES */
	RPCGEN_ACTION(nlm_cancel_res_1_svc),
	(xdrproc_t)xdr_nlm_res,
	(xdrproc_t)0,
	0 },

	{ /* 14: NLM_UNLOCK_RES */
	RPCGEN_ACTION(nlm_unlock_res_1_svc),
	(xdrproc_t)xdr_nlm_res,
	(xdrproc_t)0,
	0 },

	{ /* 15: NLM_GRANTED_RES */
	RPCGEN_ACTION(nlm_granted_res_1_svc),
	(xdrproc_t)xdr_nlm_res,
	(xdrproc_t)0,
	0 },

	{ /* 16: not used */
	RPCGEN_ACTION(0),
	(xdrproc_t)0,
	(xdrproc_t)0,
	0 },

	{ /* 17: NLM_SM_NOTIFY1 */
	RPCGEN_ACTION(nlm_sm_notify1_2_svc),
	(xdrproc_t)xdr_nlm_sm_status,
	(xdrproc_t)xdr_void,
	NLM_DISP_NOREMOTE },

	{ /* 18: NLM_SM_NOTIFY2 */
	RPCGEN_ACTION(nlm_sm_notify2_2_svc),
	(xdrproc_t)xdr_nlm_sm_status,
	(xdrproc_t)xdr_void,
	NLM_DISP_NOREMOTE },

	/*
	 * Version 3 (NLM_VERSX) entries.
	 */

	{ /* 19: not used */
	RPCGEN_ACTION(0),
	(xdrproc_t)0,
	(xdrproc_t)0,
	0 },

	{ /* 20: NLM_SHARE */
	RPCGEN_ACTION(nlm_share_3_svc),
	(xdrproc_t)xdr_nlm_shareargs,
	(xdrproc_t)xdr_nlm_shareres,
	0 },

	{ /* 21: NLM_UNSHARE */
	RPCGEN_ACTION(nlm_unshare_3_svc),
	(xdrproc_t)xdr_nlm_shareargs,
	(xdrproc_t)xdr_nlm_shareres,
	0 },

	{ /* 22: NLM_NM_LOCK */
	RPCGEN_ACTION(nlm_nm_lock_3_svc),
	(xdrproc_t)xdr_nlm_lockargs,
	(xdrproc_t)xdr_nlm_res,
	NLM_DISP_NOREPLY },	/* Does it's own reply. */

	{ /* 23: NLM_FREE_ALL */
	RPCGEN_ACTION(nlm_free_all_3_svc),
	(xdrproc_t)xdr_nlm_notify,
	(xdrproc_t)xdr_void,
	0 },
};
static int nlm_prog_3_dtsize =
	sizeof (nlm_prog_3_dtable) /
	sizeof (nlm_prog_3_dtable[0]);

/*
 * RPC dispatch function for nlm_prot versions: 1,2,3
 */
void
nlm_prog_3(struct svc_req *rqstp, register SVCXPRT *transp)
{
	const struct dispatch_entry *de;
	rpcproc_t max_proc;

	switch (rqstp->rq_vers) {
	case NLM_VERS:
		max_proc = NLM_GRANTED_RES;
		break;
	case NLM_SM:
		max_proc = NLM_SM_NOTIFY2;
		break;
	case NLM_VERSX:
		max_proc = NLM_FREE_ALL;
		break;
	default:
		/* Our svc registration should prevent this. */
		ASSERT(0); /* paranoid */
		svcerr_noprog(transp);
		return;
	}
	ASSERT(max_proc < nlm_prog_3_dtsize);

	if (rqstp->rq_proc > max_proc) {
		svcerr_noproc(transp);
		return;
	}

	de = &nlm_prog_3_dtable[rqstp->rq_proc];

	nlm_dispatch(rqstp, transp, de);
}

/*
 * Dispatch table for version 4 (NLM4_VERS)
 */
static const struct dispatch_entry
nlm_prog_4_dtable[] = {

	{ /* 0: NULLPROC */
	RPCGEN_ACTION(nlm_null_svc),
	(xdrproc_t)xdr_void,
	(xdrproc_t)xdr_void,
	0 },

	{ /* 1: NLM4_TEST */
	RPCGEN_ACTION(nlm4_test_4_svc),
	(xdrproc_t)xdr_nlm4_testargs,
	(xdrproc_t)xdr_nlm4_testres,
	0 },

	{ /* 2: NLM4_LOCK */
	RPCGEN_ACTION(nlm4_lock_4_svc),
	(xdrproc_t)xdr_nlm4_lockargs,
	(xdrproc_t)xdr_nlm4_res,
	NLM_DISP_NOREPLY },	/* Does it's own reply. */

	{ /* 3: NLM4_CANCEL */
	RPCGEN_ACTION(nlm4_cancel_4_svc),
	(xdrproc_t)xdr_nlm4_cancargs,
	(xdrproc_t)xdr_nlm4_res,
	0 },

	{ /* 4: NLM4_UNLOCK */
	RPCGEN_ACTION(nlm4_unlock_4_svc),
	(xdrproc_t)xdr_nlm4_unlockargs,
	(xdrproc_t)xdr_nlm4_res,
	0 },

	{ /* 5: NLM4_GRANTED */
	RPCGEN_ACTION(nlm4_granted_4_svc),
	(xdrproc_t)xdr_nlm4_testargs,
	(xdrproc_t)xdr_nlm4_res,
	0 },

	/*
	 * All the _MSG and _RES entries are "one way" calls that
	 * skip the usual RPC reply.  We give them a null xdr_res
	 * function so the dispatcher will not send a reply.
	 */

	{ /* 6: NLM4_TEST_MSG */
	RPCGEN_ACTION(nlm4_test_msg_4_svc),
	(xdrproc_t)xdr_nlm4_testargs,
	(xdrproc_t)0,
	0 },

	{ /* 7: NLM4_LOCK_MSG */
	RPCGEN_ACTION(nlm4_lock_msg_4_svc),
	(xdrproc_t)xdr_nlm4_lockargs,
	(xdrproc_t)0,
	0 },

	{ /* 8: NLM4_CANCEL_MSG */
	RPCGEN_ACTION(nlm4_cancel_msg_4_svc),
	(xdrproc_t)xdr_nlm4_cancargs,
	(xdrproc_t)0,
	0 },

	{ /* 9: NLM4_UNLOCK_MSG */
	RPCGEN_ACTION(nlm4_unlock_msg_4_svc),
	(xdrproc_t)xdr_nlm4_unlockargs,
	(xdrproc_t)0,
	0 },

	{ /* 10: NLM4_GRANTED_MSG */
	RPCGEN_ACTION(nlm4_granted_msg_4_svc),
	(xdrproc_t)xdr_nlm4_testargs,
	(xdrproc_t)0,
	0 },

	{ /* 11: NLM4_TEST_RES */
	RPCGEN_ACTION(nlm4_test_res_4_svc),
	(xdrproc_t)xdr_nlm4_testres,
	(xdrproc_t)0,
	0 },

	{ /* 12: NLM4_LOCK_RES */
	RPCGEN_ACTION(nlm4_lock_res_4_svc),
	(xdrproc_t)xdr_nlm4_res,
	(xdrproc_t)0,
	0 },

	{ /* 13: NLM4_CANCEL_RES */
	RPCGEN_ACTION(nlm4_cancel_res_4_svc),
	(xdrproc_t)xdr_nlm4_res,
	(xdrproc_t)0,
	0 },

	{ /* 14: NLM4_UNLOCK_RES */
	RPCGEN_ACTION(nlm4_unlock_res_4_svc),
	(xdrproc_t)xdr_nlm4_res,
	(xdrproc_t)0,
	0 },

	{ /* 15: NLM4_GRANTED_RES */
	RPCGEN_ACTION(nlm4_granted_res_4_svc),
	(xdrproc_t)xdr_nlm4_res,
	(xdrproc_t)0,
	0 },

	{ /* 16: not used */
	RPCGEN_ACTION(0),
	(xdrproc_t)0,
	(xdrproc_t)0,
	0 },

	{ /* 17: NLM_SM_NOTIFY1 (not in v4) */
	RPCGEN_ACTION(0),
	(xdrproc_t)0,
	(xdrproc_t)0,
	0 },

	{ /* 18: NLM_SM_NOTIFY2 (not in v4) */
	RPCGEN_ACTION(0),
	(xdrproc_t)0,
	(xdrproc_t)0,
	0 },

	{ /* 19: not used */
	RPCGEN_ACTION(0),
	(xdrproc_t)0,
	(xdrproc_t)0,
	0 },

	{ /* 20: NLM4_SHARE */
	RPCGEN_ACTION(nlm4_share_4_svc),
	(xdrproc_t)xdr_nlm4_shareargs,
	(xdrproc_t)xdr_nlm4_shareres,
	0 },

	{ /* 21: NLM4_UNSHARE */
	RPCGEN_ACTION(nlm4_unshare_4_svc),
	(xdrproc_t)xdr_nlm4_shareargs,
	(xdrproc_t)xdr_nlm4_shareres,
	0 },

	{ /* 22: NLM4_NM_LOCK */
	RPCGEN_ACTION(nlm4_nm_lock_4_svc),
	(xdrproc_t)xdr_nlm4_lockargs,
	(xdrproc_t)xdr_nlm4_res,
	NLM_DISP_NOREPLY },	/* Does it's own reply. */

	{ /* 23: NLM4_FREE_ALL */
	RPCGEN_ACTION(nlm4_free_all_4_svc),
	(xdrproc_t)xdr_nlm4_notify,
	(xdrproc_t)xdr_void,
	0 },
};
static int nlm_prog_4_dtsize =
	sizeof (nlm_prog_4_dtable) /
	sizeof (nlm_prog_4_dtable[0]);

/*
 * RPC dispatch function for nlm_prot version 4.
 */
void
nlm_prog_4(struct svc_req *rqstp, register SVCXPRT *transp)
{
	const struct dispatch_entry *de;

	if (rqstp->rq_vers != NLM4_VERS) {
		/* Our svc registration should prevent this. */
		ASSERT(0); /* paranoid */
		svcerr_noprog(transp);
		return;
	}

	if (rqstp->rq_proc >= nlm_prog_4_dtsize) {
		svcerr_noproc(transp);
		return;
	}

	de = &nlm_prog_4_dtable[rqstp->rq_proc];

	nlm_dispatch(rqstp, transp, de);
}
