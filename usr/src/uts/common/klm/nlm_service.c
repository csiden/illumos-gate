/*
 * Copyright 2010 Nexenta Systems, Inc.  All rights reserved.
 * Copyright (c) 2008 Isilon Inc http://www.isilon.com/
 * Authors: Doug Rabson <dfr@rabson.org>
 * Developed with Red Inc: Alfred Perlstein <alfred@freebsd.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * NFS Lock Manager service functions (nlm_do_...)
 * Called from nlm_rpc_svc.c wrappers.
 *
 * Source code derived from FreeBSD nlm_prot_impl.c
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/fcntl.h>
#include <sys/flock.h>
#include <sys/mount.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/share.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/syslog.h>
#include <sys/systm.h>
#include <sys/taskq.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <sys/vfs.h>
#include <sys/queue.h>

#include <rpc/rpc.h>
#include <rpc/xdr.h>
#include <rpc/pmap_prot.h>
#include <rpc/pmap_clnt.h>
/* #include <rpc/rpcb_clnt.h> ? */
#include <rpc/rpcb_prot.h>

/* #include <rpcsvc/nfs_proto.h> */
#include <rpcsvc/nlm_prot.h>
#include <rpcsvc/sm_inter.h>

#include <nfs/nfs.h>
#include <nfs/nfs_clnt.h>
#include <nfs/export.h>
#include <nfs/rnode.h>

#include "nlm_impl.h"

static void nlm_block(
	nlm4_lockargs *lockargs,
	struct nlm_host *host,
	struct nlm_vnode *nv,
	struct flock64 *fl,
	nlm_grant_cb grant_cb,
	CLIENT *clnt);

static void
nlm_init_flock(struct flock64 *fl, struct nlm4_lock *nl, int sysid)
{
	bzero(fl, sizeof (*fl));
	/* fl->l_type set by caller */
	fl->l_whence = SEEK_SET;
	fl->l_start = nl->l_offset;
	fl->l_len = nl->l_len;
	fl->l_sysid = sysid;
	fl->l_pid = nl->svid;
}

/* ******************************************************************* */

/*
 * NLM implementation details, called from the RPC svc code.
 */

/*
 * Call-back from NFS statd, used to notify that one of our
 * hosts had a status change.  (XXX always a restart?)
 *
 * The host may be either an NFS client, NFS server or both.
 *
 * In nlm_host_monitor(), we put the sysid in the private data
 * that statd carries in this callback, so we can easliy find
 * the host this call applies to.
 */
/* ARGSUSED */
void
nlm_do_notify1(nlm_sm_status *argp, void *res, struct svc_req *sr)
{
	uint32_t sysid;
	struct nlm_host *host;

	NLM_DEBUG(3, "nlm_do_notify1(): mon_name = %s\n", argp->mon_name);
	bcopy(&argp->priv, &sysid, sizeof (sysid));
	host = nlm_host_find_by_sysid(sysid);
	if (host) {
		nlm_host_notify_server(host, argp->state);
		nlm_host_notify_client(host);
		nlm_host_release(host);
	}
}

/*
 * Another available call-back for NFS statd.
 * Not currently used.
 */
/* ARGSUSED */
void
nlm_do_notify2(nlm_sm_status *argp, void *res, struct svc_req *sr)
{
}


/*
 * NLM_TEST, NLM_TEST_MSG,
 * NLM4_TEST, NLM4_TEST_MSG,
 * Client inquiry about locks, non-blocking.
 */
void
nlm_do_test(nlm4_testargs *argp, nlm4_testres *resp,
    struct svc_req *sr, nlm_res_cb cb)
{
	struct nlm_host *host;
	struct nlm_vnode *nv = NULL;
	struct netbuf *addr;
	char *netid;
	char *name;
	int error, sysid;
	struct flock64 fl;

	bzero(resp, sizeof (*resp));
	nlm_copy_netobj(&resp->cookie, &argp->cookie);

	name = argp->alock.caller_name;
	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	host = nlm_host_findcreate(name, netid, addr);
	if (host == NULL) {
		resp->stat.stat = nlm4_denied_nolocks;
		return;
	}
	sysid = host->nh_sysid;

	NLM_DEBUG(3, "nlm_do_test(): name = %s sysid = %d\n", name, sysid);

	nv = nlm_vnode_findcreate(host, &argp->alock.fh);
	if (nv == NULL) {
		resp->stat.stat = nlm4_stale_fh;
		goto out;
	}

	if (ddi_get_lbolt() < nlm_grace_threshold) {
		resp->stat.stat = nlm4_denied_grace_period;
		goto out;
	}

	nlm_init_flock(&fl, &argp->alock, sysid);
	fl.l_type = (argp->exclusive) ? F_WRLCK : F_RDLCK;

	/* BSD: VOP_ADVLOCK(nv->nv_vp, NULL, F_GETLK, &fl, F_REMOTE); */
	error = VOP_FRLOCK(nv->nv_vp, F_GETLK, &fl,
	    F_REMOTELOCK | FREAD | FWRITE,
	    (u_offset_t)0, NULL, CRED(), NULL);
	if (error) {
		resp->stat.stat = nlm4_failed;
		goto out;
	}

	if (fl.l_type == F_UNLCK) {
		resp->stat.stat = nlm4_granted;
	} else {
		struct nlm4_holder *lh;
		resp->stat.stat = nlm4_denied;
		lh = &resp->stat.nlm4_testrply_u.holder;
		lh->exclusive = (fl.l_type == F_WRLCK);
		lh->svid = fl.l_pid;
		/* Leave OH zero. XXX: sysid? */
		lh->l_offset = fl.l_start;
		lh->l_len = fl.l_len;
	}

out:
	/*
	 * If we have a callback funtion, use that to
	 * deliver the response via another RPC call.
	 */
	if (cb != NULL) {
		CLIENT *clnt;
		int stat;

		clnt = nlm_host_get_rpc(host, sr->rq_vers, TRUE);
		if (clnt != NULL) {
			/* i.e. nlm_test_res_4_cb */
			stat = (*cb)(resp, NULL, clnt);
			if (stat != RPC_SUCCESS) {
				struct rpc_err err;
				CLNT_GETERR(clnt, &err);
				NLM_ERR("NLM: do_test CB, stat=%d err=%d\n",
				    stat, err.re_errno);
			}
			CLNT_RELEASE(clnt);
		}
	}

	nlm_vnode_release(host, nv);
	nlm_host_release(host);
}

/*
 * NLM_LOCK, NLM_LOCK_MSG, NLM_NM_LOCK
 * NLM4_LOCK, NLM4_LOCK_MSG, NLM4_NM_LOCK
 *
 * Client request to set a lock, possibly blocking.
 *
 * If the lock needs to block, we return status blocked to
 * this RPC call, and then later call back the client with
 * a "granted" callback.  Tricky aspects of this include:
 * sending a reply before this function returns, and then
 * borrowing this thread from the RPC service pool for the
 * wait on the lock and doing the later granted callback.
 *
 * We also have to keep a list of locks (pending + granted)
 * both to handle retransmitted requests, and to keep the
 * vnodes for those locks active.
 */
void
nlm_do_lock(nlm4_lockargs *argp, nlm4_res *resp, struct svc_req *sr,
    nlm_reply_cb reply_cb, nlm_lkres_cb res_cb, nlm_grant_cb grant_cb)
{
	struct flock64 fl;
	struct nlm_host *host;
	struct nlm_vnode *nv = NULL;
	struct netbuf *addr;
	char *netid;
	char *name;
	CLIENT *clnt = NULL;
	int error, flags;
	bool_t do_blocking = FALSE;
	bool_t do_mon_req = FALSE;
	enum nlm4_stats status;

	bzero(resp, sizeof (*resp));
	nlm_copy_netobj(&resp->cookie, &argp->cookie);

	name = argp->alock.caller_name;
	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	host = nlm_host_findcreate(name, netid, addr);
	if (host == NULL) {
		status = nlm4_denied_nolocks;
		goto doreply;
	}

	NLM_DEBUG(3, "nlm_do_lock(): name = %s sysid = %d\n",
	    name, host->nh_sysid);

	nv = nlm_vnode_findcreate(host, &argp->alock.fh);
	if (nv == NULL) {
		resp->stat.stat = nlm4_stale_fh;
		goto doreply;
	}

	/*
	 * During the "grace period", only allow reclaim.
	 */
	if (argp->reclaim == 0 &&
	    ddi_get_lbolt() < nlm_grace_threshold) {
		status = nlm4_denied_grace_period;
		goto doreply;
	}

	/*
	 * If we may need to do RPC callback, get the
	 * RPC client handle now, so we know if we can
	 * bind to the NLM service on this client.
	 * The two cases where we need this are:
	 * 1: _msg_ call needing an RPC callback,
	 * 2: blocking call needing a later grant.
	 *
	 * Note: host object carries transport type.
	 * One client using multiple transports gets
	 * separate sysids for each of its transports.
	 */
	if (res_cb != NULL || grant_cb != NULL) {
		clnt = nlm_host_get_rpc(host, sr->rq_vers, TRUE);
		if (clnt == NULL) {
			status = nlm4_denied;
			goto doreply;
		}
	}

	/*
	 * Try to lock non-blocking first.  If we succeed
	 * getting the lock, we can reply with the granted
	 * status directly and avoid the complications of
	 * making the "granted" RPC callback later.
	 *
	 * This also let's us find out now about some
	 * possible errors like EROFS, etc.
	 */
	nlm_init_flock(&fl, &argp->alock, host->nh_sysid);
	fl.l_type = (argp->exclusive) ? F_WRLCK : F_RDLCK;

	flags = F_REMOTELOCK | FREAD | FWRITE;
	error = VOP_FRLOCK(nv->nv_vp, F_SETLK, &fl, flags,
	    (u_offset_t)0, NULL, CRED(), NULL);

	switch (error) {
	case 0:
		/* Got it without waiting! */
		status = nlm4_granted;
		do_mon_req = TRUE;
		break;

	/* EINPROGRESS too? */
	case EAGAIN:
		/* We did not get the lock. Should we block? */
		if (argp->block == FALSE || grant_cb == NULL) {
			status = nlm4_denied;
			break;
		}
		/*
		 * Should block.  Try to reserve this thread
		 * so we can use it to wait for the lock and
		 * later send the granted message.  If this
		 * reservation fails, say "no resources".
		 */
		if (!svc_reserve_thread(sr->rq_xprt)) {
			status = nlm4_denied_nolocks;
			break;
		}
		/*
		 * OK, can detach this thread, so this call
		 * will block below (after we reply).
		 */
		status = nlm4_blocked;
		do_blocking = TRUE;
		do_mon_req = TRUE;
		break;

	case ENOLCK:
		/* Failed for lack of resources. */
		status = nlm4_denied_nolocks;
		break;

	default:
		status = nlm4_denied;
		break;
	}

doreply:
	resp->stat.stat = status;

	/*
	 * We get one of two function pointers; one for a
	 * normal RPC reply, and another for doing an RPC
	 * "callback" _res reply for a _msg function.
	 * Use either of those to send the reply now.
	 *
	 * If sending this reply fails, just leave the
	 * lock in the list for retransmitted requests.
	 * Cleanup is via unlock or host rele (statmon).
	 */
	if (reply_cb != NULL) {
		/* i.e. nlm_lock_1_reply */
		if (0 == (*reply_cb)(sr->rq_xprt, resp)) {
			svcerr_systemerr(sr->rq_xprt);
		}
	}
	if (res_cb != NULL) {
		enum clnt_stat stat;
		/* i.e. nlm_lock_res_1_cb */
		stat = (*res_cb)(resp, NULL, clnt);
		if (stat != RPC_SUCCESS) {
			struct rpc_err err;
			CLNT_GETERR(clnt, &err);
			NLM_ERR("NLM: do_lock CB, stat=%d err=%d\n",
			    stat, err.re_errno);
		}
	}

	/*
	 * The reply has been sent to the client.
	 * Start monitoring this client (maybe).
	 *
	 * Note that the non-monitored (NM) calls pass grant_cb=NULL
	 * indicating that the client doesn't support RPC callbacks.
	 * No monitoring for these (lame) clients.
	 */
	if (do_mon_req && grant_cb != NULL)
		nlm_host_monitor(host, argp->state);

	if (do_blocking) {
		/*
		 * We need to block on this lock, and when that
		 * completes, do the granted RPC call. Note that
		 * we "reserved" this thread above, so we can now
		 * "detach" it from the RPC SVC pool, allowing it
		 * to block indefinitely if needed.
		 */
		(void) svc_detach_thread(sr->rq_xprt);
		nlm_block(argp, host, nv, &fl, grant_cb, clnt);
	}

	if (clnt != NULL) {
		CLNT_RELEASE(clnt);
	}

	nlm_vnode_release(host, nv);
	nlm_host_release(host);
}

/*
 * Helper for nlm_do_lock(), partly for observability,
 * (we'll see a call blocked in this function) and
 * because nlm_do_lock() was getting quite long.
 */
static void
nlm_block(
	nlm4_lockargs *lockargs,
	struct nlm_host *host,
	struct nlm_vnode *nv,
	struct flock64 *fl,
	nlm_grant_cb grant_cb,
	CLIENT *clnt)
{
	nlm4_testargs args;
	int error, flags;
	enum clnt_stat stat;

	struct nlm_async_lock *taf, *af = NULL;

	/*
	 * Now we're ready to block.
	 *
	 * XXX: Do we need to setup an flk_cb?
	 * flk_callback_t flk_cb;
	 * flk_init_callback(&flk_cb, ...);
	 */

	/*
	 * Keep a list of blocked locks on nh_pending, and use it
	 * to cancel these threads in nlm_destroy_client_pending.
	 *
	 * Check to see if this lock is already in the list
	 * and if not, add an entry for it.  Allocate first,
	 * then if we don't insert, free the new one.
	 * Caller already has vp held.
	 */
	af = kmem_zalloc(sizeof (*af), KM_SLEEP);
	af->af_host = host;
	af->af_vp = nv->nv_vp;
	af->af_fl = *fl;	/* struct */

	mutex_enter(&host->nh_lock);

	/*
	 * Not comparing l_sysid because this list is
	 * maintained per-host (all the same sysid).
	 */
	TAILQ_FOREACH(taf, &host->nh_pending, af_link) {
		if (taf->af_vp		== af->af_vp &&
		    taf->af_fl.l_start	== af->af_fl.l_start &&
		    taf->af_fl.l_len	== af->af_fl.l_len &&
		    taf->af_fl.l_pid	== af->af_fl.l_pid &&
		    taf->af_fl.l_type	== af->af_fl.l_type) {
			break;
		}
	}
	if (taf == NULL) {
		/* Not found. Insert our new entry. */
		TAILQ_INSERT_TAIL(&host->nh_pending, af, af_link);
	}

	mutex_exit(&host->nh_lock);

	if (taf != NULL) {
		/*
		 * Lock is already pending.  Let the other
		 * thread do the granted callback, etc.
		 */
		goto out;
	}

	/* BSD: VOP_ADVLOCK(vp, NULL, F_SETLK, fl, F_REMOTE); */
	flags = F_REMOTELOCK | FREAD | FWRITE;
	error = VOP_FRLOCK(af->af_vp, F_SETLKW, &af->af_fl,
	    flags, (u_offset_t)0, NULL, CRED(), NULL);

	/*
	 * Done waiting (no longer pending)
	 */
	mutex_enter(&host->nh_lock);
	TAILQ_REMOVE(&host->nh_pending, af, af_link);
	mutex_exit(&host->nh_lock);

	if (error != 0) {
		/*
		 * We failed getting the lock, but have no way to
		 * tell the client about that.  Let 'em time out.
		 */
		goto out;
	}

	/*
	 * Do the "granted" call-back to the client.
	 */
	args.cookie	= lockargs->cookie;
	args.exclusive	= lockargs->exclusive;
	args.alock	= lockargs->alock;
	stat = (*grant_cb)(&args, NULL, clnt);
	if (stat != RPC_SUCCESS) {
		struct rpc_err err;
		CLNT_GETERR(clnt, &err);
		NLM_ERR("NLM: grant CB, stat=%d err=%d\n",
		    stat, err.re_errno);
	}

out:
	nlm_free_async_lock(af);
}

/*
 * NLM_CANCEL, NLM_CANCEL_MSG,
 * NLM4_CANCEL, NLM4_CANCEL_MSG,
 * Client gives waiting for a blocking lock.
 */
void
nlm_do_cancel(nlm4_cancargs *argp, nlm4_res *resp,
    struct svc_req *sr, nlm_res_cb cb)
{
	struct nlm_host *host;
	struct nlm_vnode *nv = NULL;
	struct netbuf *addr;
	char *netid;
	char *name;
	int error, sysid;
	struct flock64 fl;
	struct nlm_async_lock *af;

	bzero(resp, sizeof (*resp));
	nlm_copy_netobj(&resp->cookie, &argp->cookie);

	name = argp->alock.caller_name;
	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	host = nlm_host_findcreate(name, netid, addr);
	if (host == NULL) {
		resp->stat.stat = nlm4_denied_nolocks;
		return;
	}
	sysid = host->nh_sysid;

	NLM_DEBUG(3, "nlm_do_cancel(): name = %s sysid = %d\n", name, sysid);

	nv = nlm_vnode_findcreate(host, &argp->alock.fh);
	if (nv == NULL) {
		resp->stat.stat = nlm4_stale_fh;
		goto out;
	}

	if (ddi_get_lbolt() < nlm_grace_threshold) {
		resp->stat.stat = nlm4_denied_grace_period;
		goto out;
	}

	nlm_init_flock(&fl, &argp->alock, sysid);
	fl.l_type = (argp->exclusive) ? F_WRLCK : F_RDLCK;

	/*
	 * First we need to try and find the async lock request - if
	 * there isn't one, we give up and return nlm4_denied.
	 */
	mutex_enter(&host->nh_lock);

	TAILQ_FOREACH(af, &host->nh_pending, af_link) {
		if (af->af_fl.l_start == fl.l_start &&
		    af->af_fl.l_len == fl.l_len &&
		    af->af_fl.l_pid == fl.l_pid &&
		    af->af_fl.l_type == fl.l_type) {
			break;
		}
	}

	if (!af) {
		mutex_exit(&host->nh_lock);
		resp->stat.stat = nlm4_denied;
		goto out;
	}

	error = nlm_cancel_async_lock(af);

	if (error) {
		resp->stat.stat = nlm4_denied;
	} else {
		resp->stat.stat = nlm4_granted;
	}

	mutex_exit(&host->nh_lock);

out:
	/*
	 * If we have a callback funtion, use that to
	 * deliver the response via another RPC call.
	 */
	if (cb != NULL) {
		CLIENT *clnt;
		int stat;

		clnt = nlm_host_get_rpc(host, sr->rq_vers, TRUE);
		if (clnt != NULL) {
			/* i.e. nlm_cancel_res_4_cb */
			stat = (*cb)(resp, NULL, clnt);
			if (stat != RPC_SUCCESS) {
				struct rpc_err err;
				CLNT_GETERR(clnt, &err);
				NLM_ERR("NLM: do_cancel CB, stat=%d err=%d\n",
				    stat, err.re_errno);
			}
			CLNT_RELEASE(clnt);
		}
	}

	nlm_vnode_release(host, nv);
	nlm_host_release(host);
}

/*
 * NLM_UNLOCK, NLM_UNLOCK_MSG,
 * NLM4_UNLOCK, NLM4_UNLOCK_MSG,
 * Client removes one of their locks.
 */
void
nlm_do_unlock(nlm4_unlockargs *argp, nlm4_res *resp,
    struct svc_req *sr, nlm_res_cb cb)
{
	struct nlm_host *host;
	struct nlm_vnode *nv = NULL;
	struct netbuf *addr;
	char *netid;
	char *name;
	int error, sysid;
	struct flock64 fl;

	bzero(resp, sizeof (*resp));
	nlm_copy_netobj(&resp->cookie, &argp->cookie);

	name = argp->alock.caller_name;
	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	host = nlm_host_findcreate(name, netid, addr);
	if (host == NULL) {
		resp->stat.stat = nlm4_denied_nolocks;
		return;
	}
	sysid = host->nh_sysid;

	NLM_DEBUG(3, "nlm_do_unlock(): name = %s sysid = %d\n", name, sysid);

	nv = nlm_vnode_findcreate(host, &argp->alock.fh);
	if (nv == NULL) {
		resp->stat.stat = nlm4_stale_fh;
		goto out;
	}

	if (ddi_get_lbolt() < nlm_grace_threshold) {
		resp->stat.stat = nlm4_denied_grace_period;
		goto out;
	}

	nlm_init_flock(&fl, &argp->alock, sysid);
	fl.l_type = F_UNLCK;

	/* BSD: VOP_ADVLOCK(nv->nv_vp, NULL, F_UNLCK, &fl, F_REMOTE); */
	error = VOP_FRLOCK(nv->nv_vp, F_UNLCK, &fl,
	    F_REMOTELOCK | FREAD | FWRITE,
	    (u_offset_t)0, NULL, CRED(), NULL);

	/*
	 * Ignore the error - there is no result code for failure,
	 * only for grace period.
	 */
	(void) error;
	resp->stat.stat = nlm4_granted;

out:
	/*
	 * If we have a callback funtion, use that to
	 * deliver the response via another RPC call.
	 */
	if (cb != NULL) {
		CLIENT *clnt;
		int stat;

		clnt = nlm_host_get_rpc(host, sr->rq_vers, TRUE);
		if (clnt != NULL) {
			/* i.e. nlm_unlock_res_4_cb */
			stat = (*cb)(resp, NULL, clnt);
			if (stat != RPC_SUCCESS) {
				struct rpc_err err;
				CLNT_GETERR(clnt, &err);
				NLM_ERR("NLM: do_unlock CB, stat=%d err=%d\n",
				    stat, err.re_errno);
			}
			CLNT_RELEASE(clnt);
		}
	}

	nlm_vnode_release(host, nv);
	nlm_host_release(host);
}

/*
 * NLM_GRANTED, NLM_GRANTED_MSG,
 * NLM4_GRANTED, NLM4_GRANTED_MSG,
 *
 * This service routine is special.  It's the only one that's
 * really part of our NLM _client_ support, used by _servers_
 * to "call back" when a blocking lock from this NLM client
 * is granted by the server.  In this case, we _know_ there is
 * already an nlm_host allocated and held by the client code.
 * We want to find that nlm_host here.
 *
 * Over in nlm_call_lock(), the client encoded the sysid for this
 * server in the "owner handle" netbuf sent with our lock request.
 * We can now use that to find the nlm_host object we used there.
 * (NB: The owner handle is opaque to the server.)
 */
void
nlm_do_granted(nlm4_testargs *argp, nlm4_res *resp,
    struct svc_req *sr, nlm_res_cb cb)
{
	struct nlm_owner_handle *oh;
	struct nlm_host *host;
	struct nlm_waiting_lock *nw;

	bzero(resp, sizeof (*resp));
	nlm_copy_netobj(&resp->cookie, &argp->cookie);

	oh = (void *) argp->alock.oh.n_bytes;
	host = nlm_host_find_by_sysid(oh->oh_sysid);
	if (host == NULL) {
		/* could not match alock */
		resp->stat.stat = nlm4_denied;
		return;
	}

	resp->stat.stat = nlm4_denied;

	mutex_enter(&host->nh_lock);
	TAILQ_FOREACH(nw, &host->nh_waiting, nw_link) {
		if (nw->nw_state != NLM_WS_BLOCKED)
			continue;
		if (oh->oh_sysid == nw->nw_sysid &&
		    argp->alock.svid == nw->nw_lock.svid &&
		    argp->alock.l_offset == nw->nw_lock.l_offset &&
		    argp->alock.l_len == nw->nw_lock.l_len &&
		    argp->alock.fh.n_len == nw->nw_lock.fh.n_len &&
		    !memcmp(argp->alock.fh.n_bytes, nw->nw_lock.fh.n_bytes,
		    nw->nw_lock.fh.n_len)) {
			nw->nw_state = NLM_WS_GRANTED;
			cv_broadcast(&nw->nw_cond);
			resp->stat.stat = nlm4_granted;
			break;
		}
	}
	mutex_exit(&host->nh_lock);

	/*
	 * If we have a callback funtion, use that to
	 * deliver the response via another RPC call.
	 */
	if (cb != NULL) {
		CLIENT *clnt;
		int stat;

		clnt = nlm_host_get_rpc(host, sr->rq_vers, TRUE);
		if (clnt != NULL) {
			/* i.e. nlm_granted_res_4_cb */
			stat = (*cb)(resp, NULL, clnt);
			if (stat != RPC_SUCCESS) {
				struct rpc_err err;
				CLNT_GETERR(clnt, &err);
				NLM_ERR("NLM: do_grantd CB, stat=%d err=%d\n",
				    stat, err.re_errno);
			}
			CLNT_RELEASE(clnt);
		}
	}

	nlm_host_release(host);
}

/*
 * NLM_FREE_ALL, NLM4_FREE_ALL
 *
 * Destroy all lock state for the calling host.
 */
void
nlm_do_free_all(nlm4_notify *argp, void *res, struct svc_req *sr)
{
	struct nlm_host *host;
	struct netbuf *addr;
	char *netid;
	char *name;

	name = argp->name;
	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	host = nlm_host_findcreate(name, netid, addr);
	if (host == NULL) {
		/* nothing to do */
		return;
	}

	/*
	 * Note that this does not do client-side cleanup.
	 * We want to do that ONLY if statd tells us the
	 * server has restarted.
	 */
	nlm_host_notify_server(host, argp->state);

	nlm_host_release(host);
	(void) res;
}

static void
nlm_init_shrlock(struct shrlock *shr,
	nlm4_share *nshare, struct nlm_host *host)
{

	switch (nshare->access) {
	default:
	case fsa_NONE:
		shr->s_access = 0;
		break;
	case fsa_R:
		shr->s_access = F_RDACC;
		break;
	case fsa_W:
		shr->s_access = F_WRACC;
		break;
	case fsa_RW:
		shr->s_access = F_RWACC;
		break;
	}

	switch (nshare->mode) {
	default:
	case fsm_DN:
		shr->s_deny = F_NODNY;
		break;
	case fsm_DR:
		shr->s_deny = F_RDDNY;
		break;
	case fsm_DW:
		shr->s_deny = F_WRDNY;
		break;
	case fsm_DRW:
		shr->s_deny = F_RWDNY;
		break;
	}

	shr->s_sysid = host->nh_sysid;
	shr->s_pid = 0;
	shr->s_own_len = nshare->oh.n_len;
	shr->s_owner   = nshare->oh.n_bytes;
}

/*
 * NLM_SHARE, NLM4_SHARE
 *
 * Request a DOS-style share reservation
 */
void
nlm_do_share(nlm4_shareargs *argp, nlm4_shareres *resp, struct svc_req *sr)
{
	struct nlm_host *host;
	struct nlm_vnode *nv = NULL;
	struct netbuf *addr;
	char *netid;
	char *name;
	int error, flags, sysid;
	struct shrlock shr;

	bzero(resp, sizeof (*resp));
	nlm_copy_netobj(&resp->cookie, &argp->cookie);

	name = argp->share.caller_name;
	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	host = nlm_host_findcreate(name, netid, addr);
	if (host == NULL) {
		resp->stat = nlm4_denied_nolocks;
		return;
	}
	sysid = host->nh_sysid;

	NLM_DEBUG(3, "nlm_do_share(): name = %s sysid = %d\n", name, sysid);

	if (argp->reclaim == 0 &&
	    ddi_get_lbolt() < nlm_grace_threshold) {
		resp->stat = nlm4_denied_grace_period;
		goto out;
	}

	nv = nlm_vnode_findcreate(host, &argp->share.fh);
	if (nv == NULL) {
		resp->stat = nlm4_stale_fh;
		goto out;
	}

	/* Convert to local form. */
	nlm_init_shrlock(&shr, &argp->share, host);

	flags = FREAD|FWRITE;
	error = VOP_SHRLOCK(nv->nv_vp, F_SHARE, &shr,
	    flags, CRED(), NULL);

	resp->stat = error ? nlm4_denied : nlm4_granted;

out:
	nlm_vnode_release(host, nv);
	nlm_host_release(host);
}

/*
 * NLM_UNSHARE, NLM4_UNSHARE
 *
 * Release a DOS-style share reservation
 */
void
nlm_do_unshare(nlm4_shareargs *argp, nlm4_shareres *resp, struct svc_req *sr)
{
	struct nlm_host *host;
	struct nlm_vnode *nv = NULL;
	struct netbuf *addr;
	char *netid;
	char *name;
	int error, flags, sysid;
	struct shrlock shr;

	bzero(resp, sizeof (*resp));
	nlm_copy_netobj(&resp->cookie, &argp->cookie);

	name = argp->share.caller_name;
	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	host = nlm_host_findcreate(name, netid, addr);
	if (host == NULL) {
		resp->stat = nlm4_denied_nolocks;
		return;
	}
	sysid = host->nh_sysid;

	NLM_DEBUG(3, "nlm_do_unshare(): name = %s sysid = %d\n", name, sysid);

	if (argp->reclaim == 0 &&
	    ddi_get_lbolt() < nlm_grace_threshold) {
		resp->stat = nlm4_denied_grace_period;
		goto out;
	}

	nv = nlm_vnode_findcreate(host, &argp->share.fh);
	if (nv == NULL) {
		resp->stat = nlm4_stale_fh;
		goto out;
	}

	/* Convert to local form. */
	nlm_init_shrlock(&shr, &argp->share, host);

	flags = FREAD|FWRITE;
	error = VOP_SHRLOCK(nv->nv_vp, F_UNSHARE, &shr,
	    flags, CRED(), NULL);

	(void) error;
	resp->stat = nlm4_granted;

out:
	nlm_vnode_release(host, nv);
	nlm_host_release(host);
}
