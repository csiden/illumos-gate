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
 * NFS Lock Manager, start/stop, support functions, etc.
 * Most of the interesting code is here.
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

/*
 * If a host is inactive (and holds no locks) for this amount of
 * seconds, we consider it idle and stop tracking it.
 */
#define	NLM_IDLE_TIMEOUT	30

/*
 * We check the host list for idle every few seconds.
 */
#define	NLM_IDLE_PERIOD		5

/*
 * Grace period handling. The value of nlm_grace_threshold is the
 * value of ddi_get_lbolt() after which we are serving requests normally.
 */
clock_t nlm_grace_threshold;

/*
 * We check for idle hosts if ddi_get_lbolt() is greater than
 * nlm_next_idle_check,
 */
static clock_t nlm_next_idle_check;

/*
 * An RPC client handle that can be used to communicate with the local
 * NSM.
 */
static CLIENT *nlm_nsm;

/*
 * An AUTH handle for the server's creds.
 */
static AUTH *nlm_auth;

/*
 * A zero timeval for sending async RPC messages.
 */
struct timeval nlm_zero_tv = { 0, 0 };

/*
 * The local NSM state number
 */
int nlm_nsm_state;


/*
 * A lock to protect the host list and waiting lock list.
 */
static kmutex_t nlm_global_lock;

static struct nlm_host_list nlm_hosts;	/* (g) */
static int32_t nlm_next_sysid = 1;	/* (g) */

static recovery_cb nlm_recovery_func = NULL;	/* (c) */

void nlm_cancel_wait_locks(struct nlm_host *);

/* ******************************************************************* */

/*
 * Initialise NLM globals.
 */
#if 0	/* XXX */
static void
nlm_init(void *dummy)
{
	int error;

	mutex_init(&nlm_global_lock, "nlm_global_lock", MUTEX_DEFAULT, NULL);
	TAILQ_INIT(&nlm_hosts);

	error = syscall_register(&nlm_syscall_offset, &nlm_syscall_sysent,
	    &nlm_syscall_prev_sysent);
	if (error)
		NLM_ERR("Can't register NLM syscall\n");
	else
		nlm_syscall_registered = TRUE;
}
#endif	/* XXX */

#if 0	/* XXX */
static void
nlm_uninit(void *dummy)
{

	if (nlm_syscall_registered)
		syscall_deregister(&nlm_syscall_offset,
		    &nlm_syscall_prev_sysent);
}
#endif	/* XXX */

/*
 * The in-kernel RPC (kRPC) subsystem uses TLI/XTI, which needs
 * both a knetconfig and an address when creating endpoints.
 * These functions keep track of the bindings give to us by
 * the user-level lockd, allowing fetch by "netid".
 */

static void
nlm_nc_clear(struct nlm_globals *g)
{
	/* XXX todo */
}

static void
nlm_nc_add(struct nlm_globals *g, const char *netid, struct knetconfig *knc)
{
	/* XXX todo */
}

/*
 * Lookup (and hold) a knetconfig from one of our
 * service bindings.
 */
static struct knetconfig *
nlm_knetconfig_from_netid(const char *netid)
{
	/* XXX todo */
	return (NULL);
}

void
nlm_knetconfig_rele(struct knetconfig *knc)
{
	/* XXX todo */
}


/*
 * Figure out what "netid" we want, given a knetconfig.
 * This "knows" a bit about what bindings lockd will
 * normally register, but oh well.  This is called with
 * NFS knetconfigs, which may have types we don't know.
 */
const char *
nlm_netid_from_knetconfig(struct knetconfig *knc)
{
	bool_t co;

	switch (knc->knc_semantics) {
	case NC_TPI_CLTS:
		co = FALSE;
		break;
	case NC_TPI_COTS_ORD:
		co = TRUE;
		break;
	default:
		return (NULL);
	}

	if (0 == strcmp(knc->knc_protofmly, NC_INET))
		return ((co) ? "tcp" : "udp");
	if (0 == strcmp(knc->knc_protofmly, NC_INET6))
		return ((co) ? "tcp6" : "udp6");
	if (0 == strcmp(knc->knc_protofmly, NC_LOOPBACK))
		return ((co) ? "ticotsord" : "ticlts");

	return (NULL);
}


/*
 * Copy a struct netbuf.  (see tiuser.h)
 */
void
nlm_copy_netbuf(struct netbuf *dst, struct netbuf *src)
{

	ASSERT(src->len <= src->maxlen);

	dst->maxlen = src->maxlen;
	dst->len = src->len;
	dst->buf = kmem_zalloc(src->maxlen, KM_SLEEP);
	bcopy(src->buf, dst->buf, src->len);
}

/*
 * Copy a struct netobj.  (see xdr.h)
 */
void
nlm_copy_netobj(struct netobj *dst, struct netobj *src)
{

	dst->n_len = src->n_len;
	dst->n_bytes = kmem_alloc(src->n_len, KM_SLEEP);
	bcopy(src->n_bytes, dst->n_bytes, src->n_len);
}

/*
 * Create an RPC client handle for the given (prog, vers)
 * Note: modifies *addr (inserts the port number)
 *
 * The in-kernel RPC (kRPC) subsystem uses TLI/XTI, and
 * therfore needs _both_ a knetconfig and an address when
 * establishing a network endpoint.  We keep a collection
 * of the knetconfig structs for all our RPC bindings and
 * find one when needed using the "netid".
 */
static CLIENT *
nlm_get_rpc(const char *netid, struct netbuf *addr,
	rpcprog_t prog, rpcvers_t vers)
{
	struct knetconfig *knc = NULL;
	CLIENT *clnt = NULL;
	enum clnt_stat stat;
	int error;

	knc = nlm_knetconfig_from_netid(netid);
	if (knc == NULL)
		goto out;

	/*
	 * Contact the remote RPCBIND service to find the
	 * port for this prog+service. NB: modifies *addr
	 * XXX: Use a copy of the addr. here?
	 */
	stat = rpcbind_getaddr(knc, prog, vers, addr);
	if (stat != RPC_SUCCESS)
		goto out;

	error = clnt_tli_kcreate(knc, addr, prog, vers,
	    0, 0, CRED(), &clnt);
	if (error != 0)
		clnt = NULL;

out:
	if (knc != NULL)
		nlm_knetconfig_rele(knc);
	return (clnt);
}


/*
 * Find or create an nlm_vnode.
 * See comments at struct nlm_vnode def.
 */
struct nlm_vnode *
nlm_vnode_findcreate(struct nlm_host *host, struct netobj *n)
{
	fhandle_t *fhp;
	vnode_t *vp;
	struct nlm_vnode *nv, *new_nv;

	/*
	 * Get a vnode pointer for the given NFS file handle.
	 * Note that it could be an NFSv2 for NFSv3 handle,
	 * which means the size might vary.  (don't copy)
	 */
	if (n->n_len < sizeof (*fhp))
		return (NULL);
	/* We know this is aligned (mem_alloc) */
	/* LINTED: alignment */
	fhp = (fhandle_t *)n->n_bytes;
	vp = lm_fhtovp(fhp);
	if (vp == NULL)
		return (NULL);
	/* Note: VN_HOLD from fhtovp */

	/* XXX: maybe use a kmem_cache? */
	new_nv = kmem_zalloc(sizeof (*new_nv), KM_SLEEP);

	mutex_enter(&host->nh_lock);

	TAILQ_FOREACH(nv, &host->nh_vnodes, nv_link) {
		if (nv->nv_vp == vp)
			break;
	}
	if (nv != NULL) {
		/*
		 * Found existing entry.  We already did a VN_HOLD
		 * when we created this entry. Just bump refs.
		 */
		kmem_free(new_nv, sizeof (*new_nv));
		nv->nv_refs++;
		mutex_exit(&host->nh_lock);
		/* Give up the hold from fhtovp */
		VN_RELE(vp);
		return (nv);
	}
	/* Add an entry. */
	nv = new_nv;
	nv->nv_refs = 1;
	nv->nv_vp = vp;		/* Keep hold from fhtovp */
	TAILQ_INSERT_TAIL(&host->nh_vnodes, nv, nv_link);

	mutex_exit(&host->nh_lock);

	return (nv);
}

void
nlm_vnode_release(struct nlm_host *host, struct nlm_vnode *nv)
{
	/* XXX	struct nlm_vnode *dead_nv = NULL; */

	if (nv == NULL)
		return;

	mutex_enter(&host->nh_lock);

	nv->nv_refs--;

	/*
	 * We don't have an inexpensive way to find out if
	 * this client has locks on this vnode, but need to
	 * keep it active while this client has any locks.
	 * (Counting locks and unlocks won't work because
	 * those are not required to balance.)
	 *
	 * Current solution:  Just keep vnodes held until
	 * this host struct reaches its idle timeout.
	 */
#if 0	/* XXX */
	if (nv->nv_refs == 0) {
		TAILQ_REMOVE(&host->nh_vnodes, nv, nv_link);
		dead_nv = nv;
	}
#endif	/* XXX */

	mutex_exit(&host->nh_lock);

#if 0	/* XXX */
	if (dead_nv != NULL) {
		VN_RELE(old_nv->nv_vp);
		kmem_free(old_nv, sizeof (*old_nv));
	}
#endif	/* XXX */
}

/*
 * In the BSD code, this was called in the context of the
 * F_UNLCK caller that allowed our F_SETLKW to succeed.
 * Here, F_SETLKW is done from the RPC service thread in a way
 * that allows it to simply blocks in the F_SETLK call.
 * XXX: We probably don't need this function.
 */
#if 0 /* XXX */
static void
nlm_lock_callback(flk_cb_when_t when, void *arg)
{
	struct nlm_async_lock *af = (struct nlm_async_lock *)arg;

}
#endif

/*
 * Free an async lock request. The request must have been removed from
 * any list.
 */
void
nlm_free_async_lock(struct nlm_async_lock *af)
{
	/*
	 * Free an async lock.
	 */
#if 0
	if (af->af_rpc)
		CLNT_RELEASE(af->af_rpc);
	xdr_free((xdrproc_t)xdr_nlm4_testargs, (void *)&af->af_granted);
#endif
	if (af->af_vp)
		VN_RELE(af->af_vp);
	kmem_free(af, sizeof (*af));
}

/*
 * Cancel our async request - this must be called with
 * af->nh_host->nh_lock held.
 */
int
nlm_cancel_async_lock(struct nlm_async_lock *af)
{
	struct flock64 fl;
	struct nlm_host *host;
	vnode_t *vp;
	int error, flags;

	/* Save these while locked. */
	host = af->af_host;
	vp = af->af_vp;
	fl = af->af_fl;		/* struct! */

	ASSERT(MUTEX_HELD(&host->nh_lock));

	mutex_exit(&host->nh_lock);

	/*
	 * BSD: VOP_ADVLOCKASYNC(af->af_vp, NULL, F_CANCEL, &af->af_fl,
	 * F_REMOTE, NULL, &af->af_cookie);
	 */

	/*
	 * Interesting os/flock.c feature:
	 * Blocked F_SETLKW calls can be cancelled by an
	 * F_UNLCK of exactly the same flock information.
	 */
	fl.l_type = F_UNLCK;
	flags = F_REMOTELOCK | FREAD | FWRITE;
	error = VOP_FRLOCK(vp, F_SETLK, &fl, flags,
	    (u_offset_t)0, NULL, CRED(), NULL);

	mutex_enter(&host->nh_lock);

	/*
	 * This af is removed from the list by the
	 * blocking thread (see nlm_block).
	 */
	return (error);
}

/*
 * Cancel pending blocked locks for this client.
 */
static void
nlm_destroy_client_pending(struct nlm_host *host)
{
	struct nlm_async_lock *af, *next_af;

	ASSERT(MUTEX_HELD(&host->nh_lock));

	/*
	 * Cancel all blocked lock requests.
	 * The blocked threads will cleanup.
	 */
	af = TAILQ_FIRST(&host->nh_pending);
	while (af != NULL) {
		next_af = TAILQ_NEXT(af, af_link);
		(void) nlm_cancel_async_lock(af);
		af = next_af;
	}
}

/*
 * Destroy any locks the client holds.
 * Do F_UNLKSYS on all it's vnodes.
 */
static void
nlm_destroy_client_locks(struct nlm_host *host)
{
	struct nlm_vnode *nv;
	struct flock64 fl;
	int flags;

	ASSERT(MUTEX_HELD(&host->nh_lock));

	bzero(&fl, sizeof (fl));
	fl.l_type = F_UNLKSYS;
	fl.l_sysid = host->nh_sysid;
	flags = F_REMOTELOCK | FREAD | FWRITE;

	TAILQ_FOREACH(nv, &host->nh_vnodes, nv_link) {
		(void) VOP_FRLOCK(nv->nv_vp, F_SETLK, &fl,
		    flags, 0, NULL, CRED(), NULL);
	}
}


/*
 * Free resources used by a host. This is called after the reference
 * count has reached zero so it doesn't need to worry about locks.
 */
static void
nlm_host_destroy(struct nlm_host *host)
{

	if (host->nh_srvrpc.nr_client)
		CLNT_RELEASE(host->nh_srvrpc.nr_client);
	if (host->nh_clntrpc.nr_client)
		CLNT_RELEASE(host->nh_clntrpc.nr_client);

	if (host->nh_name)
		strfree(host->nh_name);
	if (host->nh_netid)
		strfree(host->nh_netid);
	if (host->nh_addr.buf != NULL)
		kmem_free(host->nh_addr.buf, host->nh_addr.maxlen);

	mutex_destroy(&host->nh_lock);
	/* sysctl_ctx_free(&host->nh_sysctl); XXX */
	kmem_free(host, sizeof (*host));
}

void
nlm_set_recovery_cb(recovery_cb func)
{
	nlm_recovery_func = func;
}


/*
 * Thread start callback for client lock recovery
 */
static void
nlm_client_recovery_start(void *arg)
{
	struct nlm_host *host = (struct nlm_host *)arg;

	NLM_DEBUG(1, "NLM: client lock recovery for %s started\n",
	    host->nh_name);

	/* nlm_client_recovery(host); */
	if (nlm_recovery_func != NULL)
		(*nlm_recovery_func)(host);

	NLM_DEBUG(1, "NLM: client lock recovery for %s completed\n",
	    host->nh_name);

	host->nh_monstate = NLM_MONITORED;

	/* Note: refcnt was incremented before this thread started. */
	nlm_host_release(host);

	/* XXX kthread_exit(); */
}

/*
 * Cleanup SERVER-side state after a client restarts,
 * or becomes unresponsive, or whatever.
 *
 * This is called by the local NFS statd when we receive a
 * host state change notification.  (also nlm_svc_stopping)
 * XXX: Does NFS statd call us only after host restart?
 *
 * We unlock any active locks owned by the host. When rpc.lockd is
 * shutting down, this function is called with newstate set to zero
 * which allows us to cancel any pending async locks and clear the
 * locking state.
 */
void
nlm_host_notify_server(struct nlm_host *host, int newstate)
{

	if (newstate) {
		NLM_DEBUG(1, "NLM: host %s (sysid %d) rebooted, new "
		    "state is %d\n", host->nh_name,
		    host->nh_sysid, newstate);
	}

	/*
	 * Cleanup for a "crashed" NFS client.
	 * (For whom we are the server.)
	 */

	mutex_enter(&host->nh_lock);

	nlm_destroy_client_pending(host);
	nlm_destroy_client_locks(host);

	host->nh_state = newstate;

	mutex_exit(&host->nh_lock);
}

/*
 * Cleanup CLIENT-side state after a server restarts,
 * or becomes unresponsive, or whatever.
 *
 * This is called by the local NFS statd when we receive a
 * host state change notification.  (also nlm_svc_stopping)
 *
 * Deal with a server restart.  If we are stopping the
 * NLM service, we'll have newstate == 0, and will just
 * cancel all our client-side lock requests.  Otherwise,
 * star the "recovery" process to reclaim any locks
 * we hold on this server.
 */

void
nlm_host_notify_client(struct nlm_host *host)
{

	/*
	 * XXX: Walk list of locks...
	 *   where we own it, and on system H,
	 *      count++
	 *
	 * See: flk_get_active_locks(sysid, NOPID);

XXX More porting work.  We only want there to ever be one recovery
thread running for each host.  So, we can let the host object carry
state about the recovery thread running, if any, etc.

So, here:  (a) check if there's a recovery thread yet, and if not,
mark that one is starting.  (b) get the list of locks for this sysid.
if no locks, mark recovery complete now and don't bother starting the
recovery thread.  If there are some locks, store that list in the host
object and start the recovery thread.  mark recovery as 'running'.

	 */
	if (host->nh_monstate != NLM_RECOVERING &&
	    lf_countlocks(NLM_SYSID_CLIENT | host->nh_sysid) > 0) {

		/* XXX: Use a dynamic taskq here? */

		struct thread *td;
		host->nh_monstate = NLM_RECOVERING;

		/* rele this ref. i nlm_client_recovery_start */
		atomic_inc_uint(&host->nh_refs);

		kthread_add(nlm_client_recovery_start, host, curproc, &td, 0, 0,
		    "NFS lock recovery for %s", host->nh_name);
	}
}


/*
 * Create a new NLM host.
 */
static struct nlm_host *
nlm_create_host(char *name, const char *netid, struct netbuf *addr)
{
	struct nlm_host *host;

	ASSERT(MUTEX_HELD(&nlm_global_lock));

	host = kmem_zalloc(sizeof (*host), KM_SLEEP);

	mutex_init(&host->nh_lock, "nh_lock", MUTEX_DEFAULT, NULL);
	host->nh_refs = 1;

	host->nh_name = strdup(name);
	host->nh_netid = strdup(netid);
	nlm_copy_netbuf(&host->nh_addr, addr);

	host->nh_state = 0;
	host->nh_monstate = NLM_UNMONITORED;

	TAILQ_INIT(&host->nh_vnodes);
	TAILQ_INIT(&host->nh_pending);

	return (host);
}

/*
 * Acquire the next sysid for remote locks not handled by the NLM.
 */
uint32_t
nlm_acquire_next_sysid(void)
{
	uint32_t next_sysid;

	mutex_enter(&nlm_global_lock);
	next_sysid = nlm_next_sysid++;
	mutex_exit(&nlm_global_lock);
	return (next_sysid);
}


/*
 * Check for idle hosts and stop monitoring them. We could also free
 * the host structure here, possibly after a larger timeout but that
 * would require some care to avoid races with
 * e.g. nlm_host_lock_count_sysctl.
 */
static void
nlm_check_idle(void)
{
	struct nlm_host *host;
	clock_t time_uptime, new_timeout;

	ASSERT(MUTEX_HELD(&nlm_global_lock));

	time_uptime = ddi_get_lbolt();
	if (time_uptime <= nlm_next_idle_check)
		return;
	nlm_next_idle_check = time_uptime +
	    SEC_TO_TICK(NLM_IDLE_PERIOD);
	new_timeout = time_uptime +
	    SEC_TO_TICK(NLM_IDLE_TIMEOUT);

	TAILQ_FOREACH(host, &nlm_hosts, nh_link) {
		if (host->nh_monstate == NLM_MONITORED &&
		    time_uptime > host->nh_idle_timeout) {
			mutex_exit(&nlm_global_lock);
			if (lf_countlocks(host->nh_sysid) > 0 ||
			    lf_countlocks(NLM_SYSID_CLIENT +
			    host->nh_sysid)) {
				host->nh_idle_timeout = new_timeout;
				mutex_enter(&nlm_global_lock);
				continue;
			}
			nlm_host_unmonitor(host);
			mutex_enter(&nlm_global_lock);
		}
	}
}


/*
 * Find the host specified by...  (see below)
 * If found, increment the ref count.
 */
static struct nlm_host *
nlm_host_find_locked(char *name, const char *netid, struct netbuf *addr)
{
	struct nlm_host *host;

	ASSERT(MUTEX_HELD(&nlm_global_lock));

	TAILQ_FOREACH(host, &nlm_hosts, nh_link) {
		if (0 == strcmp(host->nh_name, name) &&
		    0 == strcmp(host->nh_netid, netid) &&
		    host->nh_addr.len == addr->len &&
		    0 == memcmp(host->nh_addr.buf,
		    addr->buf, addr->len)) {
			host->nh_refs++;
			break;
		}
	}
	return (host);
}


/*
 * Find or create an NLM host for the given name and address.
 *
 * The remote host is determined by all of: name, netidd, address.
 * Note that the netid is whatever nlm_svc_add_ep() gave to
 * svc_tli_kcreate() for the service binding.  If any of these
 * are different, allocate a new host (new sysid).
 */
struct nlm_host *
nlm_host_findcreate(char *name, const char *netid, struct netbuf *addr)
{
	struct nlm_host *host, *newhost;

	mutex_enter(&nlm_global_lock);
	host = nlm_host_find_locked(name, netid, addr);
	mutex_exit(&nlm_global_lock);
	if (host != NULL)
		goto done;

	/*
	 * Do allocations (etc.) outside of mutex,
	 * and then check again before inserting.
	 */
	newhost = nlm_create_host(name, netid, addr);

	mutex_enter(&nlm_global_lock);
	host = nlm_host_find_locked(name, netid, addr);
	if (host == NULL) {
		newhost->nh_sysid = nlm_next_sysid++;
		TAILQ_INSERT_TAIL(&nlm_hosts, newhost, nh_link);
	}
	mutex_exit(&nlm_global_lock);

	if (host != NULL) {
		nlm_host_destroy(newhost);
		newhost = NULL;
	} else {
		/* We inserted */
		host = newhost;
	}

	NLM_DEBUG(1, "NLM: new host %s (sysid %d)\n",
	    host->nh_name, host->nh_sysid);

done:
	host->nh_idle_timeout = ddi_get_lbolt() +
	    SEC_TO_TICK(NLM_IDLE_TIMEOUT);

	return (host);
}

/*
 * Find the NLM host that matches the value of 'sysid'.
 * If found, return it with a new ref,
 * else return NULL.
 */
struct nlm_host *
nlm_host_find_by_sysid(int sysid)
{
	struct nlm_host *host;

	mutex_enter(&nlm_global_lock);

	TAILQ_FOREACH(host, &nlm_hosts, nh_link) {
		if (host->nh_sysid == sysid) {
			atomic_inc_uint(&host->nh_refs);
			break;
		}
	}

	mutex_exit(&nlm_global_lock);

	return (host);
}

/*
 * Release a reference to some host.  If it has no references
 * arrange for its destruction ...eventually.
 *
 * XXX: This needs work.  We want these to stay in the list for
 * up to NLM_IDLE_TIMEOUT so they can be found and reused by the
 * same host during that time.  After the idle time expires,
 * hosts should be removed (and tell statd to stop monitoring).
 */
void
nlm_host_release(struct nlm_host *host)
{

	if (0 == atomic_dec_uint_nv(&host->nh_refs)) {
		/* Start idle timer */
		host->nh_idle_timeout = ddi_get_lbolt() +
		    SEC_TO_TICK(NLM_IDLE_TIMEOUT);
	}

	nlm_check_idle();	/* XXX */
}

/*
 * Unregister this NLM host (NFS client) with the local statd
 * due to idleness (no locks held for a while).
 */
void
nlm_host_unmonitor(struct nlm_host *host)
{
	mon_id args;
	sm_stat res;
	enum clnt_stat stat;

	NLM_DEBUG(1, "NLM: unmonitoring %s (sysid %d)\n",
	    host->nh_name, host->nh_sysid);

	/*
	 * We put our assigned system ID value in the priv field to
	 * make it simpler to find the host if we are notified of a
	 * host restart.
	 */
	args.mon_name = host->nh_name;
	args.my_id.my_name = uts_nodename();
	args.my_id.my_prog = NLM_PROG;
	args.my_id.my_vers = NLM_SM;
	args.my_id.my_proc = NLM_SM_NOTIFY1;

	/* Call SM_UNMON */
	stat = sm_unmon_1(&args, &res, nlm_nsm);
	if (stat != RPC_SUCCESS) {
		struct rpc_err err;
		CLNT_GETERR(nlm_nsm, &err);
		NLM_ERR("NLM: Failed to contact statd, "
		    "stat=%d error=%d\n",
		    stat, err.re_errno);
		return;
	}

	/* XXX: save res.state ? */
	host->nh_monstate = NLM_UNMONITORED;
}

/*
 * Ask the local NFS statd to begin monitoring this host.
 * It will call us back when that host restarts, using the
 * prog,vers,proc specified below, i.e. NLM_SM_NOTIFY1,
 * which is handled in nlm_do_notify1().
 */
void
nlm_host_monitor(struct nlm_host *host, int state)
{
	struct mon args;
	sm_stat_res res;
	enum clnt_stat stat;

	if (state && !host->nh_state) {
		/*
		 * This is the first time we have seen an NSM state
		 * value for this host. We record it here to help
		 * detect host reboots.
		 */
		host->nh_state = state;
		NLM_DEBUG(1, "NLM: host %s (sysid %d) has NSM state %d\n",
		    host->nh_name, host->nh_sysid, state);
	}

	mutex_enter(&host->nh_lock);
	if (host->nh_monstate != NLM_UNMONITORED) {
		mutex_exit(&host->nh_lock);
		return;
	}
	host->nh_monstate = NLM_MONITORED;
	mutex_exit(&host->nh_lock);

	NLM_DEBUG(1, "NLM: monitoring %s (sysid %d)\n",
	    host->nh_name, host->nh_sysid);

	/*
	 * Tell statd how to call us with status updates for
	 * this host.  Updates arrive via nlm_do_notify1().
	 *
	 * We put our assigned system ID value in the priv field to
	 * make it simpler to find the host if we are notified of a
	 * host restart.
	 */
	bzero(&args, sizeof (args));
	args.mon_id.mon_name = host->nh_name;
	args.mon_id.my_id.my_name = uts_nodename();
	args.mon_id.my_id.my_prog = NLM_PROG;
	args.mon_id.my_id.my_vers = NLM_SM;
	args.mon_id.my_id.my_proc = NLM_SM_NOTIFY1;
	bcopy(&host->nh_sysid, args.priv, sizeof (host->nh_sysid));

	/* Call SM_MON */
	stat = sm_mon_1(&args, &res, nlm_nsm);
	if (stat != RPC_SUCCESS) {
		NLM_ERR("Failed to contact local NSM - rpc error %d\n", stat);
		return;
	}
	if (res.res_stat == stat_fail) {
		NLM_ERR("Local NSM refuses to monitor %s\n",
		    host->nh_name);
		mutex_enter(&host->nh_lock);
		host->nh_monstate = NLM_MONITOR_FAILED;
		mutex_exit(&host->nh_lock);
		return;
	}

	host->nh_monstate = NLM_MONITORED;
}

/*
 * XXX porting work still todo XXX

The BSD kRPC service is apparently single-threaded,
or so it says some places in this code.  We're full MT!

I'm not sure we can share client handles (and cache) the way this
function currently does.  The one thing this _could_ cache is the
address we get from the rpcbind_getaddr (see nlm_get_rpc).  That's
the only thing that's a little expensive to get, due to OtW trips.
Further, there's just one of these per "host" object (not two).

XXX: Do we need to give each calling thread it's own RPC handle?
[ Actually, it looks like maybe RPC client handles are MT safe. ]
Or can the RPC client handles be used by multiple threads?
If not, we'll need to let each thread have its own RPC client.
(They're just a network endpoint and some memory.)

The algorithm for using such a cached rpcbind result should be:
Do the clnt_tli_kcreate, try the RPC call; if the call fails
with one of the errors suggesting "wrong port", then try the
rpcbind call (just once) and retry the call.  Minor possible
improvements to this algorithm would be to do a call to the
NULLPROC when using a cached bind addr that has not been used
in a while.  (server might change the port)

With that done, CLNT_RELEASE (not yet implemented) can
change to CLNT_DESTROY.

 * Return an RPC client handle that can be used to talk to the NLM
 * running on the given host.
 */
CLIENT *
nlm_host_get_rpc(struct nlm_host *host, int vers, bool_t isserver)
{
	struct nlm_rpc *rpc;
	CLIENT *client;
	clock_t time_uptime;

	mutex_enter(&host->nh_lock);

	if (isserver) {
		/*
		 * Some SVC function that needs to callback,
		 * i.e. for reply to a _msg_ call, or for a
		 * granted lock callback.
		 */
		rpc = &host->nh_srvrpc;
	} else {
		rpc = &host->nh_clntrpc;
	}

	/*
	 * We can't hold onto RPC handles for too long - the async
	 * call/reply protocol used by some NLM clients makes it hard
	 * to tell when they change port numbers (e.g. after a
	 * reboot). Note that if a client reboots while it isn't
	 * holding any locks, it won't bother to notify us. We
	 * expire the RPC handles after two minutes.
	 */
	time_uptime = ddi_get_lbolt();
	if (rpc->nr_client && time_uptime > rpc->nr_create_time + 2*60) {
		client = rpc->nr_client;
		rpc->nr_client = NULL;
		mutex_exit(&host->nh_lock);
		CLNT_RELEASE(client);
		mutex_enter(&host->nh_lock);
	}

	if (!rpc->nr_client) {
		mutex_exit(&host->nh_lock);
		client = nlm_get_rpc(host->nh_netid, &host->nh_addr,
		    NLM_PROG, vers);
		mutex_enter(&host->nh_lock);

		if (client) {
			if (rpc->nr_client) {
				mutex_exit(&host->nh_lock);
				CLNT_DESTROY(client);
				mutex_enter(&host->nh_lock);
			} else {
				rpc->nr_client = client;
				rpc->nr_create_time = time_uptime;
			}
		}
	}

	client = rpc->nr_client;
	if (client)
		CLNT_ACQUIRE(client);
	mutex_exit(&host->nh_lock);

	return (client);

}

int
nlm_host_get_sysid(struct nlm_host *host)
{

	return (host->nh_sysid);
}

int
nlm_host_get_state(struct nlm_host *host)
{

	return (host->nh_state);
}

/*
 * Our local client-side code calls this to block on a
 * remote lock.  (See nlm_call_lock).
 */
void *
nlm_register_wait_lock(
	struct nlm_host *host,
	struct nlm4_lock *lock,
	struct vnode *vp)
{
	struct nlm_owner_handle *oh;
	struct nlm_waiting_lock *nw;

	ASSERT(lock->oh.n_len == sizeof (*oh));
	oh = (void *) lock->oh.n_bytes;

	nw = kmem_zalloc(sizeof (*nw), KM_SLEEP);
	cv_init(&nw->nw_cond, NULL, CV_DEFAULT, NULL);
	nw->nw_lock = *lock;
	nlm_copy_netobj(&nw->nw_fh, &nw->nw_lock.fh);
	nw->nw_state = NLM_WS_BLOCKED;
	nw->nw_sysid = oh->oh_sysid;
	nw->nw_host = host;	/* no hold - caller has it */
	nw->nw_vp = vp;

	mutex_enter(&host->nh_lock);
	TAILQ_INSERT_TAIL(&host->nh_waiting, nw, nw_link);
	mutex_exit(&host->nh_lock);

	return (nw);
}

/*
 * Remove this lock from the wait list.
 */
void
nlm_deregister_wait_lock(struct nlm_host *host, void *handle)
{
	struct nlm_waiting_lock *nw = handle;

	mutex_enter(&host->nh_lock);
	TAILQ_REMOVE(&host->nh_waiting, nw, nw_link);
	mutex_exit(&host->nh_lock);

	kmem_free(nw->nw_fh.n_bytes, nw->nw_fh.n_len);
	cv_destroy(&nw->nw_cond);
	kmem_free(nw, sizeof (*nw));
}

/*
 * Wait for a lock, then remove from the wait list.
 */
int
nlm_wait_lock(void *handle, int timo)
{
	struct nlm_waiting_lock *nw = handle;
	struct nlm_host *host = nw->nw_host;
	clock_t when;
	int error, rc;

	/*
	 * If the granted message arrived before we got here,
	 * nw->nw_state will be GRANTED - in that case, don't sleep.
	 */
	mutex_enter(&host->nh_lock);
	error = 0;
	if (nw->nw_state == NLM_WS_BLOCKED) {
		when = ddi_get_lbolt() + SEC_TO_TICK(timo);
		rc = cv_timedwait_sig(&nw->nw_cond, &host->nh_lock, when);
	}
	TAILQ_REMOVE(&host->nh_waiting, nw, nw_link);
	if (rc <= 0) {
		/* Timeout or interrupt. */
		error = (rc == 0) ? EINTR : ETIME;
		/*
		 * The granted message may arrive after the
		 * interrupt/timeout but before we manage to lock the
		 * mutex. Detect this by examining nw_lock.
		 */
		if (nw->nw_state == NLM_WS_GRANTED)
			error = 0;
	} else {
		/* Got cv_signal. */
		error = 0;
		/*
		 * If nlm_cancel_wait is called, then error will be
		 * zero but nw_state will be NLM_WS_CANCELLED.
		 * We translate this into EINTR.
		 */
		if (nw->nw_state == NLM_WS_CANCELLED)
			error = EINTR;
	}
	mutex_exit(&host->nh_lock);

	kmem_free(nw, sizeof (*nw));

	return (error);
}

void
nlm_cancel_wait_locks(struct nlm_host *host)
{
	struct nlm_waiting_lock *nw;

	ASSERT(MUTEX_HELD(&host->nh_lock));

	TAILQ_FOREACH(nw, &host->nh_waiting, nw_link) {
		nw->nw_state = NLM_WS_CANCELLED;
		cv_broadcast(&nw->nw_cond);
	}
}


/* ******************************************************************* */

/*
 * Syscall interface with userland.
 * Bind RPC service endpoints.
 *
 * Local-only transports get a different set of programs than
 * network transports.  The local transport is used by statd
 * to call us back with host monitoring events using NLM_SM
 * (version==2) but for safety, don't let remote callers use
 * any calls in that program.
 */

/*
 * RPC service registrations for LOOPBACK,
 * allowed to call the real nlm_prog_2.
 * None of the others are used locally.
 */
static SVC_CALLOUT nlm_svcs_lo[] = {
	{ NLM_PROG, 2, 2, nlm_prog_2 }, /* NLM_SM */
};
static SVC_CALLOUT_TABLE nlm_sct_lo = {
	sizeof (nlm_svcs_lo) / sizeof (nlm_svcs_lo[0]),
	FALSE,	/* dynamically allocated? */
	nlm_svcs_lo	/* table above */
};

/*
 * RPC service registration for inet transports.
 * Note that the version 2 (NLM_SM) entries are
 * all NULL (noproc) in these dispatchers.
 */
static SVC_CALLOUT nlm_svcs_in[] = {
	{ NLM_PROG, 4, 4, nlm_prog_4 },	/* NLM4_VERS */
	{ NLM_PROG, 1, 3, nlm_prog_3 },	/* NLM_VERS - NLM_VERSX */
};
static SVC_CALLOUT_TABLE nlm_sct_in = {
	sizeof (nlm_svcs_in) / sizeof (nlm_svcs_in[0]),
	FALSE,	/* dynamically allocated? */
	nlm_svcs_in	/* table above */
};

static void nlm_xprtclose(const SVCMASTERXPRT *xprt);

/*
 * Called by klmmod.c when lockd adds a network endpoint
 * on which we should begin RPC services.
 */
int
nlm_svc_add_ep(struct nlm_globals *g, struct file *fp,
    char *netid, struct knetconfig *knc)
{
	int err;
	SVC_CALLOUT_TABLE *sct;
	SVCMASTERXPRT *xprt = NULL;

	if (0 == strcmp(knc->knc_protofmly, NC_LOOPBACK))
		sct = &nlm_sct_lo;
	else
		sct = &nlm_sct_in;

	err = svc_tli_kcreate(fp, 0, netid, NULL, &xprt,
	    sct, nlm_xprtclose, NLM_SVCPOOL_ID, FALSE);

	/*
	 * Keep a list of transports, and with each, the
	 * netconfig we used to create it.  This is used
	 * both for detecting close of the last transport,
	 * and to build outgoing transport handles when
	 * RPC service code need to build a CLIENT handle
	 * to call back to some host.  XXX - todo
	 */
	/* nlm_nc_add(XXX) */

	return (err);
}

/*
 * Called by klmmod.c before lockd starts up service on the
 * first endpoint.  Note: run_status == NLM_ST_STARTING
 *
 * The first transport passed in by the user-level lockd is
 * the loopback we'll use to talk to the statd.  Check that
 * we get what we're expecting, and setup the RPC client
 * handle we'll used to talk to statd.
 */
int
nlm_svc_starting(struct nlm_globals *g,
    const char *netid, struct knetconfig *knc)
{
	enum clnt_stat stat;
	clock_t time_uptime;
	char myaddr[SYS_NMLN + 2];
	struct netbuf nb;

	if (0 != strcmp(knc->knc_protofmly, NC_LOOPBACK)) {
		NLM_ERR("NLM: starting, wrong protofmly");
		return (EINVAL);
	}

	/*
	 * Initialize the list of netconfigs.
	 * Must do this before nlm_get_rpc().
	 */
	nlm_nc_clear(g);
	nlm_nc_add(g, netid, knc);

	/*
	 * Get an RPC client handle for the local statd.
	 *
	 * Create the "self" host address, which is like
	 * "nodename.service" where the service is empty,
	 * and nodename is the zone's node name.
	 */
	nb.buf = myaddr;
	nb.maxlen = sizeof (myaddr);
	nb.len = snprintf(nb.buf, nb.maxlen, "%s.", uts_nodename());

	nlm_nsm = nlm_get_rpc(netid, &nb, SM_PROG, SM_VERS);
	if (nlm_nsm == NULL) {
		NLM_ERR("NLM: internal error contacting NSM");
		return (EIO);
	}

	/*
	 * Inform statd that we're starting (or restarting)
	 * with call SM_SIMU_CRASH.
	 */
	stat = sm_simu_crash_1(NULL, NULL, nlm_nsm);
	if (stat != RPC_SUCCESS) {
		struct rpc_err err;

		CLNT_GETERR(nlm_nsm, &err);
		NLM_ERR("NLM: unexpected error contacting NSM, "
		    "stat=%d, errno=%d\n", stat, err.re_errno);
		return (EIO);
	}

	time_uptime = ddi_get_lbolt();
	g->grace_threshold = time_uptime +
	    SEC_TO_TICK(g->grace_period);
	g->next_idle_check = time_uptime +
	    SEC_TO_TICK(NLM_IDLE_PERIOD);

	return (0);
}

/*
 * Called by klmmod.c when lockd is going away.
 * Note: run_status == NLM_ST_STOPPING
 * XXX: figure out where to call this...
 */
void
nlm_svc_stopping(struct nlm_globals *g)
{
	struct nlm_host *host, *nhost;

	/*
	 * Trash all the existing state so that if the server
	 * restarts, it gets a clean slate. This is complicated by the
	 * possibility that there may be other threads trying to make
	 * client locking requests.

Set a flag to prevent creation of new host entries.
Walk existing host entries marking them "dead".
Wait for threads using them to leave...

	 * First we fake a client reboot notification which will
	 * cancel any pending async locks and purge remote lock state
	 * from the local lock manager. We release the reference from
	 * nlm_hosts to the host (which may remove it from the list
	 * and free it). After this phase, the only entries in the
	 * nlm_host list should be from other threads performing
	 * client lock requests. We arrange to defer closing the
	 * sockets until the last RPC client handle is released.
	 */
	mutex_enter(&nlm_global_lock);
	/* TAILQ_FOREACH_SAFE */
	host = TAILQ_FIRST(&nlm_hosts);
	while (host != NULL) {
		nhost = TAILQ_NEXT(host, nh_link);
		mutex_exit(&nlm_global_lock);
		nlm_host_notify_server(host, 0);
		nlm_cancel_wait_locks(host);
		/* nlm_host_release(host)	* XXX wrong... */
		mutex_enter(&nlm_global_lock);
		host = nhost;
	}
	/* TAILQ_FOREACH_SAFE */
	host = TAILQ_FIRST(&nlm_hosts);
	while (host != NULL) {
		nhost = TAILQ_NEXT(host, nh_link);
		mutex_enter(&host->nh_lock);

#if 0 /* XXX */
		if (host->nh_srvrpc.nr_client ||
		    host->nh_clntrpc.nr_client) {
			if (host->nh_addr.ss_family == AF_INET)
				v4_used++;
#ifdef INET6
			if (host->nh_addr.ss_family == AF_INET6)
				v6_used++;
#endif
			/*
			 * Note that the rpc over udp code copes
			 * correctly with the fact that a socket may
			 * be used by many rpc handles.
			 */
			if (host->nh_srvrpc.nr_client)
				CLNT_CONTROL(host->nh_srvrpc.nr_client,
				    CLSET_FD_CLOSE, 0);
			if (host->nh_clntrpc.nr_client)
				CLNT_CONTROL(host->nh_clntrpc.nr_client,
				    CLSET_FD_CLOSE, 0);
		}
#endif	/* XXX */

		mutex_exit(&host->nh_lock);
		host = nhost;
	}
	mutex_exit(&nlm_global_lock);

	AUTH_DESTROY(nlm_auth);

#if 0 /* XXX */
	if (!v4_used)
		soclose(nlm_socket);
	nlm_socket = NULL;
	if (!v6_used)
		soclose(nlm_socket6);
	nlm_socket6 = NULL;
#endif /* XXX */
}

/*
 * Called by the RPC code when it's done with this transport.
 */
static void nlm_xprtclose(const SVCMASTERXPRT *xprt)
{
	char *netid;

	netid = svc_getnetid(xprt);
	/* Destroy all hosts using this netid...  */
	(void) netid;

	/* XXX - todo */
}
