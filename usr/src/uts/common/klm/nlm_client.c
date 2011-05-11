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
 * Client-side support for (NFS) VOP_FRLOCK, VOP_SHRLOCK.
 * (called via klmops.c: lm_frlock, lm4_frlock)
 *
 * Source code derived from FreeBSD nlm_advlock.c
 */

#include <sys/param.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/flock.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/share.h>
#include <sys/syslog.h>
#include <sys/systm.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <sys/queue.h>

#include <fs/fs_subr.h>
#include <rpcsvc/nlm_prot.h>

#include <nfs/nfs.h>
#include <nfs/nfs_clnt.h>
#include <nfs/export.h>
#include <nfs/rnode.h>

#include "nlm_impl.h"

#define	PID_MAX	MAX_MAXPID

/* Extra flags for nlm_call_lock() - xflags */
#define	NLM_X_RECLAIM	1
#define	NLM_X_BLOCKING	2

kmutex_t nlm_svid_lock;
static struct unrhdr *nlm_svid_allocator;
static volatile uint32_t nlm_xid = 1;

static int nlm_map_status(nlm4_stats stat);

static int nlm_init_lock(struct nlm4_lock *lock,
	const struct flock64 *fl, struct netobj *fh,
	struct nlm_owner_handle *oh);

static int nlm_call_lock(vnode_t *vp, struct flock64 *flk,
	struct nlm_host *host, struct netobj *fh,
	struct flk_callback *flcb, int vers, int xflags);
static int nlm_call_unlock(vnode_t *vp, struct flock64 *flk,
	struct nlm_host *host, struct netobj *fh, int vers);
static int nlm_call_test(vnode_t *vp, struct flock64 *flk,
	struct nlm_host *host, struct netobj *fh, int vers);
static int nlm_call_cancel(struct nlm4_lockargs *largs,
	struct nlm_host *host, int vers);

static int nlm_local_getlk(vnode_t *vp, struct flock64 *fl, int flags);
static int nlm_local_setlk(vnode_t *vp, struct flock64 *fl, int flags);

static void nlm_init_share(struct nlm4_share *share,
	const struct shrlock *sl, struct netobj *fh,
	struct nlm_owner_handle *oh);

static int
nlm_call_share(vnode_t *vp, struct shrlock *shr,
	struct nlm_host *host, struct netobj *fh,
	int vers, int reclaim);
static int
nlm_call_unshare(struct vnode *vp, struct shrlock *shr,
	struct nlm_host *host, struct netobj *fh, int vers);

static int
nlm_local_shrlock(vnode_t *vp, struct shrlock *sl, int cmd, int flags);

/* XXX - call this somewhere... */
static void
nlm_client_init(void *dummy)
{
	int i;

	mutex_init(&nlm_svid_lock, "NLM svid lock", MUTEX_DEFAULT, NULL);
#if 0	/* XXX */
	nlm_svid_allocator = new_unrhdr(PID_MAX + 2, INT_MAX, &nlm_svid_lock);
#endif
}

static int
nlm_msg(kthread_t *td, const char *server, const char *msg, int error)
{

	if (error) {
		uprintf("nfs server %s: %s, error %d\n", server,
		    msg, error);
	} else {
		uprintf("nfs server %s: %s\n", server, msg);
	}
	return (0);
}

struct nlm_feedback_arg {
	bool_t	nf_printed;
	mntinfo_t *nf_nmp;
};

static void
nlm_down(struct nlm_feedback_arg *nf, kthread_t *td,
    const char *msg, int error)
{
	mntinfo_t *nmp = nf->nf_nmp;

	if (nmp == NULL)
		return;
#if 0	/* XXX */
	mutex_enter(&nmp->mi_lock);
	if (!(nmp->nm_state & NFSSTA_LOCKTIMEO)) {
		nmp->nm_state |= NFSSTA_LOCKTIMEO;
		mutex_exit(&nmp->mi_lock);
		vfs_event_signal(&nmp->nm_mountp->mnt_stat.f_fsid,
		    VQ_NOTRESPLOCK, 0);
	} else {
		mutex_exit(&nmp->mi_lock);
	}
#endif	/* XXX */

	nf->nf_printed = TRUE;
	nlm_msg(td, nmp->mi_curr_serv->sv_hostname, msg, error);
}

static void
nlm_up(struct nlm_feedback_arg *nf, kthread_t *td,
    const char *msg)
{
	mntinfo_t *nmp = nf->nf_nmp;

	if (!nf->nf_printed)
		return;

	nlm_msg(td, nmp->mi_curr_serv->sv_hostname, msg, 0);

#if 0	/* XXX not yet */
	mutex_enter(&nmp->mi_lock);
	if (nmp->nm_state & NFSSTA_LOCKTIMEO) {
		nmp->nm_state &= ~NFSSTA_LOCKTIMEO;
		mutex_exit(&nmp->mi_lock);
		vfs_event_signal(&nmp->nm_mountp->mnt_stat.f_fsid,
		    VQ_NOTRESPLOCK, 1);
	} else {
		mutex_exit(&nmp->mi_lock);
	}
#endif	/* XXX not yet */
}

#if 0	/* XXX not yet */
static void
nlm_feedback(int type, int proc, void *arg)
{
	kthread_t *td = curthread;
	struct nlm_feedback_arg *nf = (struct nlm_feedback_arg *)arg;

	switch (type) {
	case FEEDBACK_REXMIT2:
	case FEEDBACK_RECONNECT:
		nlm_down(nf, td, "lockd not responding", 0);
		break;

	case FEEDBACK_OK:
		nlm_up(nf, td, "lockd is alive again");
		break;
	}
}
#endif	/* XXX not yet */

/* **************************************************************** */

/*
 * nlm_frlock --
 *      NFS advisory byte-range locks.
 *	Called in klmops.c
 *
 * Note that the local locking code (os/flock.c) is used to
 * keep track of remote locks granted by some server, so we
 * can reclaim those locks after a server restarts.  We can
 * also sometimes use this as a cache of lock information.
 *
 * Was: nlm_advlock()
 */
int
nlm_frlock(struct vnode *vp, int cmd, struct flock64 *flk,
	int flags, u_offset_t offset, struct cred *cr,
	struct netobj *fh, struct flk_callback *flcb, int vers)
{
	struct flock64 flk0, tflk;
	mntinfo_t *mi;
	servinfo_t *sv;
	const char *netid;
	struct nlm_host *host = NULL;
	int error, xflags;

	mi = VTOMI(vp);
	sv = mi->mi_curr_serv;

	netid = nlm_netid_from_knetconfig(sv->sv_knconf);
	if (netid == NULL) {
		cmn_err(CE_NOTE, "nlm_frlock: unknown NFS netid");
		error = ENOSYS;
		goto out;
	}

	host = nlm_host_findcreate(sv->sv_hostname, netid, &sv->sv_addr);

	/*
	 * BSD: Push dirty pages to the server and flush our cache
	 * so that if we are contending with another machine for a
	 * file, we get whatever they wrote and vice-versa.
	 * (The NFS code calling here has already done that).
	 */

	/*
	 * Convert the lock offset from "whence" base to zero based,
	 * first making a local copy so the caller's data will not be
	 * modified.  If the passed lock "whence" is EOF, make sure
	 * that when convoff() does VOP_GETATTR it will get the
	 * latest data (purge cached attributes).
	 */
	flk0 = *flk;
	if (flk->l_whence == SEEK_END) {
		/* Purge NFS attr. cache */
		PURGE_ATTRCACHE(vp);
	}
	error = convoff(vp, &flk0, 0, (offset_t)offset);
	if (error)
		goto out;
	/* Now flk0 is the zero-based lock request. */

	if (cmd == F_GETLK) {
		/*
		 * Check local (cached) locks first.
		 * If we find one, no need for RPC.
		 */
		tflk = flk0;
		error = nlm_local_getlk(vp, &tflk, flags);
		if (error == 0 && tflk.l_type != F_UNLCK) {
			/* Found local F_RDLK or F_WRLCK */
			goto getlk_found;
		}
		/* Not found locally.  Try remote. */
		tflk = flk0;
		error = nlm_call_test(vp, &tflk, host, fh, vers);
		if (error != 0)
			goto out;
		/*
		 * Update the caller's *flk with information
		 * on the conflicting lock (or lack thereof).
		 * Note: This is the only place where we
		 * modify the caller's *flk data.
		 */
		if (tflk.l_type == F_UNLCK) {
			/* No conflicting lock. */
			flk->l_type = F_UNLCK;
		} else {
			/*
			 * Found a conflicting lock.  Set the
			 * caller's *flk with the info, first
			 * converting to the caller's whence.
			 */
		getlk_found:
			(void) convoff(vp, &tflk, flk->l_whence,
			    (offset_t)offset);
			*flk = tflk;
		}
		error = 0;
		goto out;
	}

	/*
	 * cmd: F_SETLK, F_SETLKW
	 * (We're modifying.)
	 *
	 * Fill in l_sysid for the local locking calls.
	 * Also, let's not trust the caller's l_pid.
	 */
	flk0.l_sysid = NLM_SYSID_CLIENT | nlm_host_get_sysid(host);
	flk0.l_pid = curproc->p_pid;

	if (flk0.l_type == F_UNLCK) {
		/*
		 * Purge local (cached) lock information first,
		 * then clear the remote lock.
		 */
		(void) nlm_local_setlk(vp, &flk0, flags);
		error = nlm_call_unlock(vp, &flk0, host, fh, vers);
		goto out;
	}

	/*
	 * l_type: F_RDLCK, F_WRLCK
	 * (Requesting a lock.)
	 */

	if (cmd == F_SETLK) {
		/*
		 * This is a non-blocking "set" request,
		 * so we can check locally first, and
		 * sometimes avoid an RPC call.
		 */
		tflk = flk0;
		error = nlm_local_getlk(vp, &tflk, flags);
		if (error == 0 && tflk.l_type != F_UNLCK) {
			/* Found a conflicting lock. */
			error = EAGAIN;
			goto out;
		}
		xflags = 0;
	} else
		xflags = NLM_X_BLOCKING;

	/*
	 * Blocking lock, or no conflicts found locally.
	 * Do the NLM_LOCK RPC.
	 * XXX: Check flags & (FREAD | FWRITE) ?
	 */

	error = nlm_call_lock(vp, &flk0, host, fh, flcb, vers, xflags);
	if (error != 0)
		goto out;

	/*
	 * Save the lock locally.  This should not fail,
	 * because the server is authoritative about locks
	 * and it just told us we have the lock!
	 */
	error = nlm_local_setlk(vp, &flk0, flags);
	if (error != 0) {
		/*
		 * Oh oh, we really don't expect an error here.
		 * XXX: release the remote lock?  Or what?
		 * Ignore the local error for now...
		 */
		cmn_err(CE_NOTE, "NLM: set locally, err %d", error);
		error = 0;
	}
	/* Start monitoring this host. */

	nlm_host_monitor(host, 0);

out:
	if (host)
		nlm_host_release(host);

	return (error);
}


/*
 * The BSD code had functions here to "reclaim" (destroy)
 * remote locks when a vnode is being forcibly destroyed.
 * We just keep vnodes around until statd tells us the
 * client has gone away.
 */

/*
 * Just to complicate the terminology, "reclaim" is also
 * something we do during "recovery", where we learn that
 * a server has restarted (and is in it's "grace period")
 * so we need to "reclaim" (retransmit) all our locks.
 */

struct nlm_recovery_context {
	struct nlm_host	*nr_host;	/* host we are recovering */
	int		nr_state;	/* remote NSM state for recovery */
};

static int
nlm_client_recover_lock(struct vnode *vp, struct flock64 *fl, void *arg)
{
	struct nlm_recovery_context *nr = arg;
	mntinfo_t *mi = VTOMI(vp);
	struct netobj lm_fh;
	int error, state, vers, xflags;

#if 0	/* XXX: don't think we need to bother with this. */
	/*
	 * If the remote NSM state changes during recovery, the host
	 * must have rebooted a second time. In that case, we must
	 * restart the recovery.
	 */
	state = nlm_host_get_state(nr->nr_host);
	if (nr->nr_state != state)
		return (ERESTART);
#endif

	/*
	 * Too bad the NFS code doesn't just carry the FH
	 * in a netobj or a netbuf.
	 */
	switch (mi->mi_vers) {
	case NFS_V3:
		/* See nfs3_frlock() */
		vers = NLM4_VERS;
		lm_fh.n_len = VTOFH3(vp)->fh3_length;
		lm_fh.n_bytes = (char *)&(VTOFH3(vp)->fh3_u.data);
		break;
	case NFS_VERSION:
		/* See nfs_frlock() */
		vers = NLM_VERS;
		lm_fh.n_len = sizeof (fhandle_t);
		lm_fh.n_bytes = (char *)VTOFH(vp);
		break;
	default:
		return (ENOSYS);
	}

	xflags = NLM_X_RECLAIM;
	error = nlm_call_lock(vp, fl, nr->nr_host, &lm_fh,
	    NULL, vers, xflags);

	/*
	 * If we could not reclaim the lock, send SIGLOST
	 * to the process that thinks it holds the lock.
	 */
	if (error != 0) {
		proc_t  *p;

		mutex_enter(&pidlock);
		p = prfind(fl->l_pid);
		if (p)
			psignal(p, SIGLOST);
		mutex_exit(&pidlock);
	}

	return (error);
}

/*
 * See nlm_impl.c: nlm_host_notify()
 *
 * XXX: Need to set a callback function pointer for this,
 * because of klmops -> klmmod one-way dependency.
 * XXX: Do that in klmops.c:_init().
 */
void
nlm_client_recovery(struct nlm_host *host)
{
	struct nlm_recovery_context nr;
	int sysid, error;
	locklist_t *llp_head, *llp;

	sysid = NLM_SYSID_CLIENT | nlm_host_get_sysid(host);

	nr.nr_host = host;
	nr.nr_state = nlm_host_get_state(host);

	llp_head = flk_get_active_locks(sysid, NOPID);
	for (llp = llp_head; llp; llp = llp->ll_next) {
		nlm_client_recover_lock(llp->ll_vp, &llp->ll_flock, &nr);
	}
	flk_free_locklist(llp_head);
	/* XXX: Deal with ERESTART? (see above) */
}

/*
 * Moved these to nlm_client.c:
 * nlm_test_rpc
 * nlm_lock_rpc
 * nlm_cancel_rpc
 * nlm_unlock_rpc
 */


/*
 * Get local lock information for some NFS server.
 *
 * This gets (checks for) a local conflicting lock.
 * Note: Modifies passed flock, if a conflict is found,
 * but the caller expects that.
 */
static int
nlm_local_getlk(vnode_t *vp, struct flock64 *fl, int flags)
{
	int err, cmd = 0; /* get */

	ASSERT(fl->l_whence == SEEK_SET);

	err = reclock(vp, fl, cmd, flags, 0, NULL);

	return (err);
}

/*
 * Set local lock information for some NFS server.
 *
 * Called after a lock request (set or clear) succeeded. We record the
 * details in the local lock manager. Note that since the remote
 * server has granted the lock, we can be sure that it doesn't
 * conflict with any other locks we have in the local lock manager.
 *
 * Since it is possible that host may also make NLM client requests to
 * our NLM server, we use a different sysid value to record our own
 * client locks.
 *
 * Note that since it is possible for us to receive replies from the
 * server in a different order than the locks were granted (e.g. if
 * many local threads are contending for the same lock), we must use a
 * blocking operation when registering with the local lock manager.
 * We expect that any actual wait will be rare and short hence we
 * ignore signals for this.  (XXX not yet signals)
 *
 * XXX: was nlm_record_lock()
 */
static int
nlm_local_setlk(vnode_t *vp, struct flock64 *fl, int flags)
{
	int err, cmd = SETFLCK | SLPFLCK;

	ASSERT(fl->l_whence == SEEK_SET);

	err = reclock(vp, fl, cmd, flags, 0, NULL);

	return (err);
}

/*
 * Do NLM_LOCK call.
 * Was: nlm_setlock()
 */
static int
nlm_call_lock(vnode_t *vp, struct flock64 *fl,
	struct nlm_host *host, struct netobj *fh,
	struct flk_callback *flcb, int vers, int xflags)
{
	struct nlm4_lockargs args;
	struct nlm4_res res;
	struct nlm_owner_handle oh;
	uint32_t xid;
	CLIENT *client;
	mntinfo_t *mi = VTOMI(vp);
	struct nlm_globals *g;
	void *wait_handle = NULL;
	enum clnt_stat stat;
	clock_t retry;
	int block, exclusive;
	int error;

	int retries = 3;	/* XXX */

	bzero(&args, sizeof (args));
	bzero(&res, sizeof (res));

	block = (xflags & NLM_X_BLOCKING) ? TRUE : FALSE;
	exclusive = (fl->l_type == F_WRLCK);

	g = zone_getspecific(nlm_zone_key, curzone);
	error = nlm_init_lock(&args.alock, fl, fh, &oh);
	if (error)
		return (error);
	args.block = block;
	args.exclusive = exclusive;
	args.reclaim = xflags & NLM_X_RECLAIM;
	args.state = g->nsm_state;

	/* Update OH */
	oh.oh_sysid = nlm_host_get_sysid(host);

	retry = SEC_TO_TICK(5);
	for (;;) {
		ASSERT(wait_handle == NULL);

		client = nlm_host_get_rpc(host, vers, FALSE);
		if (!client)
			return (ENOLCK); /* XXX retry? */

		if (block)
			wait_handle = nlm_register_wait_lock(
			    host, &args.alock, vp);

		xid = atomic_inc_32_nv(&nlm_xid);
		args.cookie.n_len = sizeof (xid);
		args.cookie.n_bytes = (char *)&xid;

		stat = nlm_lock_rpc(&args, &res, client, vers);

		CLNT_RELEASE(client);

		if (stat != RPC_SUCCESS) {
			if (block) {
				nlm_deregister_wait_lock(
				    host, wait_handle);
				wait_handle = NULL;
			}
			if (retries) {
				retries--;
				continue;
			}
			return (EINVAL);
		}

		/*
		 * Free res.cookie.
		 */
		xdr_free((xdrproc_t)xdr_nlm4_res, (void *)&res);

		if (block && res.stat.stat != nlm4_blocked) {
			nlm_deregister_wait_lock(
			    host, wait_handle);
			wait_handle = NULL;
		}
		if (res.stat.stat == nlm4_denied_grace_period) {
			/*
			 * The server has recently rebooted and is
			 * giving old clients a change to reclaim
			 * their locks. Wait for a few seconds and try
			 * again.
			 */
			error = delay_sig(retry);
			if (error)
				return (error);
			retry = 2 * retry;
			if (retry > SEC_TO_TICK(30))
				retry = SEC_TO_TICK(30);
			continue;
		}

		if (block && res.stat.stat == nlm4_blocked) {
			/*
			 * The server should call us back with a
			 * granted message when the lock succeeds.
			 * In order to deal with broken servers,
			 * lost granted messages, or server reboots,
			 * we will also re-try every few seconds.
			 *
			 * Note: We're supposed to call these
			 * flk_invoke_callbacks when blocking.
			 */

			flk_invoke_callbacks(flcb, FLK_BEFORE_SLEEP);
			error = nlm_wait_lock(wait_handle, retry);
			flk_invoke_callbacks(flcb, FLK_AFTER_SLEEP);

			/* nlm_wait_lock destroys wait_handle */
			wait_handle = NULL;

			if (error == ETIME) {
				retry = 2 * retry;
				if (retry > SEC_TO_TICK(30))
					retry = SEC_TO_TICK(30);
				continue;
			}
			if (error) {
				/*
				 * We need to call the server to
				 * cancel our lock request.
				 * Note: intr may be pending,
				 * so block them here.
				 */
				k_sigset_t oldmask, newmask;
				sigfillset(&newmask);
				sigreplace(&newmask, &oldmask);
				nlm_call_cancel(&args, host, vers);
				sigreplace(&oldmask, (k_sigset_t *)NULL);
			}
			break;
		}
		error = nlm_map_status(res.stat.stat);
		break;
	}
	ASSERT(wait_handle == NULL);
	return (error);
}

/*
 * Do NLM_CANCEL call.
 * Helper for nlm_call_lock() error recovery.
 */
static int
nlm_call_cancel(struct nlm4_lockargs *largs,
	struct nlm_host *host, int vers)
{
	nlm4_cancargs cargs;
	struct nlm4_res res;
	uint32_t xid;
	CLIENT *client;
	enum clnt_stat stat;
	int error;

	bzero(&cargs, sizeof (cargs));

	xid = atomic_inc_32_nv(&nlm_xid);
	/* XXX: Use largs->cookie here? (same xid) */
	cargs.cookie.n_len = sizeof (xid);
	cargs.cookie.n_bytes = (char *)&xid;
	cargs.block	= largs->block;
	cargs.exclusive	= largs->exclusive;
	cargs.alock	= largs->alock;

	do {
		client = nlm_host_get_rpc(host, vers, FALSE);
		if (!client)
			/* XXX retry? */
			return (ENOLCK);

		stat = nlm_cancel_rpc(&cargs, &res, client, vers);
		CLNT_RELEASE(client);

		if (stat != RPC_SUCCESS) {
			/*
			 * We need to cope with temporary network partitions
			 * as well as server reboots. This means we have to
			 * keep trying to cancel until the server is back.
			 */
			delay(SEC_TO_TICK(10));
		}
	} while (stat != RPC_SUCCESS);

	/*
	 * Free res.cookie.
	 */
	xdr_free((xdrproc_t)xdr_nlm4_res,
	    (void *)&res);

	switch (res.stat.stat) {
	case nlm_denied:
		/*
		 * There was nothing to cancel. We are going to go ahead
		 * and assume we got the lock.
		 */
		error = 0;
		break;

	case nlm4_denied_grace_period:
		/*
		 * The server has recently rebooted.  Treat this as a
		 * successful cancellation.
		 */
		error = 0;
		break;

	case nlm4_granted:
		/*
		 * We managed to cancel.
		 */
		error = 0;
		break;

	default:
		/*
		 * Broken server implementation.  Can't really do
		 * anything here.
		 */
		error = EIO;
		break;
	}
	return (error);
}

/*
 * Do NLM_UNLOCK call.
 * Was: nlm_clearlock
 */
static int
nlm_call_unlock(struct vnode *vp, struct flock64 *fl,
	struct nlm_host *host, struct netobj *fh, int vers)
{
	struct nlm4_unlockargs args;
	struct nlm4_res res;
	struct nlm_owner_handle oh;
	uint32_t xid;
	CLIENT *client;
	mntinfo_t *mi = VTOMI(vp);
	enum clnt_stat stat;
	int error;

	int retries = 3;	/* XXX */

	bzero(&args, sizeof (args));
	bzero(&res, sizeof (res));

	error = nlm_init_lock(&args.alock, fl, fh, &oh);
	if (error)
		return (error);

	/* Update OH */
	oh.oh_sysid = nlm_host_get_sysid(host);

	for (;;) {
		client = nlm_host_get_rpc(host, vers, FALSE);
		if (!client)
			return (ENOLCK); /* XXX retry? */

		xid = atomic_inc_32_nv(&nlm_xid);
		args.cookie.n_len = sizeof (xid);
		args.cookie.n_bytes = (char *)&xid;

		stat = nlm_unlock_rpc(&args, &res, client, vers);
		CLNT_RELEASE(client);

		if (stat != RPC_SUCCESS) {
			if (retries) {
				retries--;
				continue;
			}
			return (EINVAL);
		}

		/*
		 * Free res.cookie.
		 */
		xdr_free((xdrproc_t)xdr_nlm4_res, (void *)&res);

		if (res.stat.stat == nlm4_denied_grace_period) {
			/*
			 * The server has recently rebooted and is
			 * giving old clients a change to reclaim
			 * their locks. Wait for a few seconds and try
			 * again.
			 */
			error = delay_sig(SEC_TO_TICK(5));
			if (error)
				return (error);
			continue;
		}
		break;
	}

	/* special cases */
	switch (res.stat.stat) {
	case nlm4_denied:
		error = EINVAL;
		break;
	default:
		error = nlm_map_status(res.stat.stat);
		break;
	}

	return (error);
}

/*
 * Do NLM_TEST call.
 * Was: nlm_getlock()
 */
static int
nlm_call_test(struct vnode *vp, struct flock64 *fl,
	struct nlm_host *host, struct netobj *fh, int vers)
{
	struct nlm4_testargs args;
	struct nlm4_testres res;
	struct nlm4_holder *h;
	struct nlm_owner_handle oh;
	uint32_t xid;
	CLIENT *client;
	mntinfo_t *mi = VTOMI(vp);
	enum clnt_stat stat;
	int exclusive;
	int error;

	int retries = 3;	/* XXX */

	bzero(&args, sizeof (args));
	bzero(&res, sizeof (res));

	exclusive = (fl->l_type == F_WRLCK);

	error = nlm_init_lock(&args.alock, fl, fh, &oh);
	if (error)
		return (error);
	args.exclusive = exclusive;

	/* Update OH */
	oh.oh_sysid = nlm_host_get_sysid(host);

	for (;;) {
		client = nlm_host_get_rpc(host, vers, FALSE);
		if (!client)
			return (ENOLCK); /* XXX retry? */

		xid = atomic_inc_32_nv(&nlm_xid);
		args.cookie.n_len = sizeof (xid);
		args.cookie.n_bytes = (char *)&xid;

		stat = nlm_test_rpc(&args, &res, client, vers);
		CLNT_RELEASE(client);

		if (stat != RPC_SUCCESS) {
			if (retries) {
				retries--;
				continue;
			}
			return (EINVAL);
		}

		if (res.stat.stat == nlm4_denied_grace_period) {
			/*
			 * The server has recently rebooted and is
			 * giving old clients a change to reclaim
			 * their locks. Wait for a few seconds and try
			 * again.
			 */
			xdr_free((xdrproc_t)xdr_nlm4_testres, (void *)&res);
			error = delay_sig(SEC_TO_TICK(5));
			if (error)
				return (error);
			continue;
		}

		switch (res.stat.stat) {
		case nlm4_granted:
			fl->l_type = F_UNLCK;
			error = 0;
			break;

		case nlm4_denied:
			h = &res.stat.nlm4_testrply_u.holder;
			fl->l_start = h->l_offset;
			fl->l_len = h->l_len;
			fl->l_pid = h->svid;
			fl->l_type = (h->exclusive) ? F_WRLCK : F_RDLCK;
			fl->l_whence = SEEK_SET;
			fl->l_sysid = 0;
			error = 0;
			break;

		default:
			error = nlm_map_status(res.stat.stat);
			break;

		}

		xdr_free((xdrproc_t)xdr_nlm4_testres, (void *)&res);
		break;

	}
	return (error);
}


static int
nlm_init_lock(struct nlm4_lock *lock,
	const struct flock64 *fl, struct netobj *fh,
	struct nlm_owner_handle *oh)
{

	/* Caller converts to zero-base. */
	ASSERT(fl->l_whence == SEEK_SET);

#if 0 /* XXX handled in our stubs */
	if (vers == NLM_VERS) {
		/*
		 * Enforce range limits on V1 locks
		 */
		if (fl->l_start > 0xffffffffLL || fl->l_len > 0xffffffffLL)
			return (EOVERFLOW);
	}
#endif /* XXX */

	bzero(lock, sizeof (*lock));
	bzero(oh, sizeof (*oh));

	lock->caller_name = uts_nodename();
	lock->fh.n_len = fh->n_len;
	lock->fh.n_bytes = fh->n_bytes;
	lock->oh.n_len = sizeof (*oh);
	lock->oh.n_bytes = (void *)oh;
	lock->svid = curproc->p_pid;
	lock->l_offset = fl->l_start;
	lock->l_len = fl->l_len;

	return (0);
}

/* ************************************************************** */

int
nlm_shrlock(struct vnode *vp, int cmd, struct shrlock *shr,
	int flags, struct netobj *fh, int vers)
{
	struct shrlock shlk;
	mntinfo_t *mi;
	servinfo_t *sv;
	const char *netid;
	struct nlm_host *host = NULL;
	int error, xflags;

	mi = VTOMI(vp);
	sv = mi->mi_curr_serv;

	netid = nlm_netid_from_knetconfig(sv->sv_knconf);
	if (netid == NULL) {
		cmn_err(CE_NOTE, "nlm_frlock: unknown NFS netid");
		error = ENOSYS;
		goto out;
	}

	host = nlm_host_findcreate(sv->sv_hostname, netid, &sv->sv_addr);

	/*
	 * Fill in s_sysid for the local locking calls.
	 * Also, let's not trust the caller's l_pid.
	 */
	shlk = *shr;
	shlk.s_sysid = NLM_SYSID_CLIENT | nlm_host_get_sysid(host);
	shlk.s_pid = curproc->p_pid;

	if (cmd == F_UNSHARE) {
		/*
		 * Purge local (cached) share information first,
		 * then clear the remote share.
		 */
		(void) nlm_local_shrlock(vp, &shlk, cmd, flags);
		error = nlm_call_unshare(vp, &shlk, host, fh, vers);
		goto out;
	}

	/*
	 * Do the NLM_SHARE RPC.
	 * XXX: Check flags & (FREAD | FWRITE) ?
	 */

	error = nlm_call_share(vp, &shlk, host, fh, vers, FALSE);
	if (error != 0)
		goto out;

	/*
	 * Save the share locally.  This should not fail,
	 * because the server is authoritative about shares
	 * and it just told us we have the share reservation!
	 */
	error = nlm_local_shrlock(vp, shr, cmd, flags);
	if (error != 0) {
		/*
		 * Oh oh, we really don't expect an error here.
		 * XXX: release the remote lock?  Or what?
		 * Ignore the local error for now...
		 */
		cmn_err(CE_NOTE, "NLM: set locally, err %d", error);
		error = 0;
	}

	/* Start monitoring this host. */
	nlm_host_monitor(host, 0);

out:
	if (host)
		nlm_host_release(host);

	return (error);
}

/*
 * XXX: share recovery stuff?
 */

/*
 * Moved these to nlm_client.c:
 * nlm_share_rpc
 * nlm_unshare_rpc
 */

/*
 * Set local share information for some NFS server.
 *
 * Called after a share request (set or clear) succeeded. We record
 * the details in the local lock manager. Note that since the remote
 * server has granted the share, we can be sure that it doesn't
 * conflict with any other shares we have in the local lock manager.
 *
 * Since it is possible that host may also make NLM client requests to
 * our NLM server, we use a different sysid value to record our own
 * client shares.
 */
int
nlm_local_shrlock(vnode_t *vp, struct shrlock *shr, int cmd, int flags)
{
	int err;

	err = fs_shrlock(vp, cmd, shr, flags, CRED(), NULL);

	return (err);
}

/*
 * Do NLM_SHARE call.
 * Was: nlm_setshare()
 */
static int
nlm_call_share(vnode_t *vp, struct shrlock *shr,
	struct nlm_host *host, struct netobj *fh,
	int vers, int reclaim)
{
	struct nlm4_shareargs args;
	struct nlm4_shareres res;
	struct nlm_owner_handle oh;
	uint32_t xid;
	CLIENT *client;
	mntinfo_t *mi = VTOMI(vp);
	struct nlm_globals *g;
	enum clnt_stat stat;
	clock_t retry;
	int error;

	int retries = 3;	/* XXX */

	bzero(&args, sizeof (args));
	bzero(&res, sizeof (res));

	g = zone_getspecific(nlm_zone_key, curzone);

	nlm_init_share(&args.share, shr, fh, &oh);
	args.reclaim = reclaim;

	/* Update what args.oh points to. */
	oh.oh_sysid = nlm_host_get_sysid(host);

	retry = SEC_TO_TICK(5);
	for (;;) {

		client = nlm_host_get_rpc(host, vers, FALSE);
		if (!client)
			return (ENOLCK); /* XXX retry? */


		/* XXX: Get XID from RPC handle? */
		xid = atomic_inc_32_nv(&nlm_xid);
		args.cookie.n_len = sizeof (xid);
		args.cookie.n_bytes = (char *)&xid;

		stat = nlm_share_rpc(&args, &res, client, vers);
		CLNT_RELEASE(client);

		if (stat != RPC_SUCCESS) {
			if (retries) {
				retries--;
				continue;
			}
			return (EINVAL);
		}

		/*
		 * Free res.cookie.
		 */
		xdr_free((xdrproc_t)xdr_nlm4_shareres, (void *)&res);

		if (res.stat == nlm4_denied_grace_period) {
			/*
			 * The server has recently rebooted and is
			 * giving old clients a change to reclaim
			 * their shares. Wait for a few seconds and try
			 * again.
			 */
			error = delay_sig(retry);
			if (error)
				return (error);
			retry = 2 * retry;
			if (retry > SEC_TO_TICK(30))
				retry = SEC_TO_TICK(30);
			continue;
		}

		break;
	}

	switch (res.stat) {
	case nlm4_granted:
		error = 0;
		break;
	case nlm4_blocked:
	case nlm4_denied:
		error = EAGAIN;
		break;
	case nlm4_denied_nolocks:
	case nlm4_deadlck:
		error = ENOLCK;
		break;
	default:
		error = EINVAL;
		break;
	}

	return (error);
}

/*
 * Do NLM_UNSHARE call.
 */
static int
nlm_call_unshare(struct vnode *vp, struct shrlock *shr,
	struct nlm_host *host, struct netobj *fh, int vers)
{
	struct nlm4_shareargs args;
	struct nlm4_shareres res;
	struct nlm_owner_handle oh;
	uint32_t xid;
	CLIENT *client;
	mntinfo_t *mi = VTOMI(vp);
	enum clnt_stat stat;
	int error;

	int retries = 3;	/* XXX */

	bzero(&args, sizeof (args));
	bzero(&res, sizeof (res));

	nlm_init_share(&args.share, shr, fh, &oh);

	/* Update what args.oh points to. */
	oh.oh_sysid = nlm_host_get_sysid(host);

	for (;;) {
		client = nlm_host_get_rpc(host, vers, FALSE);
		if (!client)
			return (ENOLCK); /* XXX retry? */

		xid = atomic_inc_32_nv(&nlm_xid);
		args.cookie.n_len = sizeof (xid);
		args.cookie.n_bytes = (char *)&xid;

		stat = nlm_unshare_rpc(&args, &res, client, vers);

		CLNT_RELEASE(client);

		if (stat != RPC_SUCCESS) {
			if (retries) {
				retries--;
				continue;
			}
			return (EINVAL);
		}

		/*
		 * Free res.cookie.
		 */
		xdr_free((xdrproc_t)xdr_nlm4_res, (void *)&res);

		if (res.stat == nlm4_denied_grace_period) {
			/*
			 * The server has recently rebooted and is
			 * giving old clients a change to reclaim
			 * their shares. Wait for a few seconds and try
			 * again.
			 */
			error = delay_sig(SEC_TO_TICK(5));
			if (error)
				return (error);
			continue;
		}

		break;
	}

	switch (res.stat) {
	case nlm4_granted:
		error = 0;
		break;
	case nlm4_denied:
		error = EAGAIN;
		break;
	case nlm4_denied_nolocks:
		error = ENOLCK;
		break;
	default:
		error = EINVAL;
		break;
	}

	return (error);
}

static void
nlm_init_share(struct nlm4_share *args,
	const struct shrlock *shr, struct netobj *fh,
	struct nlm_owner_handle *oh)
{

	bzero(args, sizeof (*args));
	bzero(oh, sizeof (*oh));

	args->caller_name = uts_nodename();
	args->fh.n_len = fh->n_len;
	args->fh.n_bytes = fh->n_bytes;
	args->oh.n_len = sizeof (*oh);
	args->oh.n_bytes = (void *)oh;

	switch (shr->s_deny) {
	default:
	case F_NODNY:
		args->mode = fsm_DN;
		break;
	case F_RDDNY:
		args->mode = fsm_DR;
		break;
	case F_WRDNY:
		args->mode = fsm_DW;
		break;
	case F_RWDNY:
		args->mode = fsm_DRW;
		break;
	}

	switch (shr->s_access) {
	default:
	case 0:	/* seen with F_UNSHARE */
		args->access = fsa_NONE;
		break;
	case F_RDACC:
		args->access = fsa_R;
		break;
	case F_WRACC:
		args->access = fsa_W;
		break;
	case F_RWACC:
		args->access = fsa_RW;
		break;
	}
}

static int
nlm_map_status(nlm4_stats stat)
{
	switch (stat) {
	case nlm4_granted:
		return (0);

	case nlm4_denied:
		return (EAGAIN);

	case nlm4_denied_nolocks:
		return (ENOLCK);

	case nlm4_blocked:
		return (EAGAIN);

	case nlm4_denied_grace_period:
		return (EAGAIN);

	case nlm4_deadlck:
		return (EDEADLK);

	case nlm4_rofs:
		return (EROFS);

	case nlm4_stale_fh:
		return (ESTALE);

	case nlm4_fbig:
		return (EFBIG);

	case nlm4_failed:
		return (EACCES);

	default:
		return (EINVAL);
	}
}
