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
 *
 * $FreeBSD$
 */

/*
 * NFS Lock Manager (NLM) private declarations, etc.
 *
 * Source code derived from FreeBSD nlm.h
 */

#ifndef	_NLM_NLM_H_
#define	_NLM_NLM_H_

#include <sys/cmn_err.h>
#include <sys/queue.h>

#define	RPC_MSGOUT(args...)	cmn_err(CE_NOTE, args)

#ifndef	SEEK_SET
#define	SEEK_SET	0
#endif
#ifndef	SEEK_CUR
#define	SEEK_CUR	1
#endif
#ifndef	SEEK_END
#define	SEEK_END	2
#endif

/*
 * This value is added to host system IDs when recording NFS client
 * locks in the local lock manager.
 */
#define	NLM_SYSID_CLIENT	0x1000000

struct nlm_host;
struct vnode;

/*
 * Callback functions for nlm_do_lock()
 *
 * Calls to nlm_do_lock are unusual, because it needs to handle
 * the reply itself, instead of letting it happen the normal way.
 * It also needs to make an RPC call _back_ to the client when a
 * blocked lock request completes.
 *
 * We pass three callback functions to nlm_do_lock:
 *  nlm_reply_cb: send a normal RPC reply
 *    nlm_res_cb: do a _res (message style) RPC (call)
 *  nlm_grant_cb: do a "granted" RPC call (after blocking)
 * Only one of the 1st or 2nd is used.
 * The 3rd is used only for blocking
 */
typedef bool_t (*nlm_reply_cb)(SVCXPRT *, nlm4_res *);
typedef enum clnt_stat (*nlm_lkres_cb)(nlm4_res *, void *, CLIENT *);
typedef enum clnt_stat (*nlm_grant_cb)(nlm4_testargs *, void *, CLIENT *);

/*
 * Function pointer for sending a reply via RPC callback
 * from any of the other nlm_xxx_msg functions.
 */
typedef enum clnt_stat (*nlm_res_cb)(void *, void *, CLIENT *);

/*
 * Could use flock.h flk_nlm_status_t instead, but
 * prefer our own enum with initial zero...
 */
typedef enum {
	NLM_ST_DOWN = 0,
	NLM_ST_STOPPING,
	NLM_ST_UP,
	NLM_ST_STARTING
} nlm_run_status_t;

/*
 * Data structures
 */

struct nlm_globals {
	kmutex_t lock;
	clock_t grace_threshold;
	clock_t next_idle_check;
	CLIENT *nlm_nsm;
	AUTH *nlm_auth;
	pid_t lockd_pid;
	int nsm_state;
	nlm_run_status_t run_status;
	kcondvar_t status_cv;
	/* options from lockd */
	int debug_level;
	int cn_idle_tmo;
	int grace_period;
	int retrans_tmo;
};

/* XXX: temporary... */
extern zone_key_t nlm_zone_key;
extern clock_t nlm_grace_threshold;


/*
 * Debug level passed in from userland.
 */
#if !defined(NDEBUG) || defined(lint)
#define	NLM_DEBUG(_level, args...)			\
	do {						\
		struct nlm_globals *g;			\
		g = zone_getspecific(nlm_zone_key, curzone); \
		if (g->debug_level >= (_level))	\
			cmn_err(CE_CONT, args);	\
	} while (__lintzero)
#else	/* NDEBUG */
#define	NLM_DEBUG(_level, args...)	((void)0)
#endif	/* NDEBUG */
#define	NLM_ERR(args...)	cmn_err(CE_NOTE, args)

/*
 * Locks:
 * (l)		locked by nh_lock
 * (s)		only accessed via server RPC which is single threaded XXX
 * (g)		locked by nlm_global_lock
 * (c)		const until freeing
 * (a)		modified using atomic ops
 */

/*
 * List of vnodes in use by some client.  We (the server) keep
 * these active (with VN_HOLD) on behalf of the client so the
 * locks won't be destroyed by VOP_INACTIVE.
 *
 * The _refs member is or active functions calls, incremented
 * in nlm_vnode_findcreate and decremented nlm_vnode_release.
 * The _locks member is incremented when a lock is granted,
 * decremented when a lock is unlocked, and _set_to_zero_
 * when we're clearing all locks for a crashed client.
 * (See nlm_destroy_client_locks)
 */
struct nlm_vnode {
	TAILQ_ENTRY(nlm_vnode) nv_link;	/* (l) hosts list of vnodes */
	vnode_t *nv_vp;			/* (c) the held vnode */
	int nv_refs;			/* (l) references */
	int nv_locks;			/* (l) locks cnt? XXX */
};
TAILQ_HEAD(nlm_vnode_list, nlm_vnode);

enum nlm_wait_state {
	NLM_WS_UNKNOWN = 0,
	NLM_WS_BLOCKED,
	NLM_WS_GRANTED,
	NLM_WS_CANCELLED
};

/*
 * A pending client-side lock request (we are the client)
 * stored on the nh_waiting list of the NLM host.
 */
struct nlm_waiting_lock {
	TAILQ_ENTRY(nlm_waiting_lock) nw_link;	/* (l) */
	enum nlm_wait_state	nw_state;	/* (l) */
	kcondvar_t	nw_cond;		/* (l) */
	nlm4_lock	nw_lock;		/* (c) */
	struct netobj	nw_fh;			/* (c) */
	int32_t		nw_sysid;		/* (c) */
	struct nlm_host *nw_host;		/* (c) */
	struct vnode	*nw_vp;			/* (c) */
};
TAILQ_HEAD(nlm_waiting_lock_list, nlm_waiting_lock);


/*
 * A pending server-side asynchronous lock request, stored on the
 * nh_pending list of the NLM host.
 */
struct nlm_async_lock {
	TAILQ_ENTRY(nlm_async_lock) af_link; /* (l) host's list of locks */
	struct nlm_host *af_host;	/* (c) host which is locking */
	struct vnode	*af_vp;		/* (l) vnode to lock */
	struct flock64	af_fl;		/* (c) lock details */
	int		af_flags;	/* (c) FREAD, etc. */
};
TAILQ_HEAD(nlm_async_lock_list, nlm_async_lock);

/*
 * NLM host.
 */
enum nlm_host_state {
	NLM_UNMONITORED,
	NLM_MONITORED,
	NLM_MONITOR_FAILED,
	NLM_RECOVERING
};

struct nlm_rpc {
	CLIENT		*nr_client;    /* (l) RPC client handle */
	time_t		nr_create_time; /* (l) when client was created */
};

struct nlm_host {
	kmutex_t	nh_lock;
	volatile uint_t	nh_refs;	/* (a) reference count */
	TAILQ_ENTRY(nlm_host) nh_link; /* (g) global list of hosts */
	char		*nh_name;	/* (c) printable name of host */
	char		*nh_netid;	/* TLI binding name */
	struct netbuf	nh_addr;	/* (c) remote address of host */
	int32_t		nh_sysid;	/* (c) our allocaed system ID */
	struct nlm_rpc	nh_srvrpc;	/* (l) RPC for server replies */
	struct nlm_rpc	nh_clntrpc;	/* (l) RPC for client requests */
	int		nh_state;	/* (s) last seen NSM state of host */
	enum nlm_host_state nh_monstate; /* (l) local NSM monitoring state */
	time_t		nh_idle_timeout; /* (s) Time at which host is idle */
	struct nlm_waiting_lock_list nh_waiting; /* (l) client-side waits */
	struct nlm_vnode_list nh_vnodes;	/* (l) active vnodes */
	struct nlm_async_lock_list nh_pending; /* (l) server-side waits */
};
TAILQ_HEAD(nlm_host_list, nlm_host);

/*
 * This is what we pass as the "owner handle" for NLM_LOCK.
 * This lets us find the blocked lock in NLM_GRANTED.
 * It also exposes on the wire what we're using as the
 * sysid for any server, which can be very helpful for
 * problem diagnosis.  (Observability is good).
 */
struct nlm_owner_handle {
	int oh_sysid;		/* of remote host */
};

/* nlm_client.c */
int nlm_frlock(struct vnode *vp, int cmd, struct flock64 *flk,
	int flag, u_offset_t offset, struct cred *cr,
	struct netobj *fh, struct flk_callback *flcb, int vers);
int nlm_shrlock(struct vnode *vp, int cmd, struct shrlock *shr,
	int flag, struct netobj *fh, int vers);

/* nlm_rpc_clnt.c */
enum clnt_stat
nlm_test_rpc(nlm4_testargs *args, nlm4_testres *res,
    CLIENT *client, rpcvers_t vers);
enum clnt_stat
nlm_lock_rpc(nlm4_lockargs *args, nlm4_res *res,
    CLIENT *client, rpcvers_t vers);
enum clnt_stat
nlm_cancel_rpc(nlm4_cancargs *args, nlm4_res *res,
    CLIENT *client, rpcvers_t vers);
enum clnt_stat
nlm_unlock_rpc(nlm4_unlockargs *args, nlm4_res *res,
    CLIENT *client, rpcvers_t vers);
enum clnt_stat
nlm_share_rpc(nlm4_shareargs *args, nlm4_shareres *res,
    CLIENT *client, rpcvers_t vers);
enum clnt_stat
nlm_unshare_rpc(nlm4_shareargs *args, nlm4_shareres *res,
    CLIENT *client, rpcvers_t vers);


/*
 * RPC service functions.
 * nlm_dispatch.c
 */
void nlm_prog_2(struct svc_req *rqstp, SVCXPRT *transp);
void nlm_prog_3(struct svc_req *rqstp, SVCXPRT *transp);
void nlm_prog_4(struct svc_req *rqstp, SVCXPRT *transp);



/* New lockd process starting in this zone. */
int nlm_svc_starting(struct nlm_globals *,
	const char *, struct knetconfig *);
void nlm_svc_stopping(struct nlm_globals *);

/* Start NLM service on the given endpoint. */
int nlm_svc_add_ep(struct nlm_globals *, struct file *,
	char *, struct knetconfig *);

/* XXX: Not sure yet what this needs to do... */
extern void CLNT_RELEASE(CLIENT *);

/*
 * Copy a struct netobj.
 */
extern void nlm_copy_netobj(struct netobj *dst, struct netobj *src);

const char *nlm_netid_from_knetconfig(struct knetconfig *);

/*
 * Search for an existing NLM host that matches the given name
 * (typically the caller_name element of an nlm4_lock).  If none is
 * found, create a new host. If 'addr' is non-NULL, record the remote
 * address of the host so that we can call it back for async
 * responses. If 'vers' is greater than zero then record the NLM
 * program version to use to communicate with this client. The host
 * reference count is incremented - the caller must call
 * nlm_host_release when it has finished using it.
 */
extern struct nlm_host *nlm_find_host_by_name(const char *name,
    struct netbuf *addr, rpcvers_t vers);

/*
 * Search for an existing NLM host that matches the given remote
 * address. If none is found, create a new host with the requested
 * address and remember 'vers' as the NLM protocol version to use for
 * that host. The host reference count is incremented - the caller
 * must call nlm_host_release when it has finished using it.
 */
extern struct nlm_host *nlm_find_host_by_addr(struct netbuf *addr,
    int vers);

struct nlm_host *nlm_host_findcreate(char *name,
    const char *netid, struct netbuf *addr);
struct nlm_host *nlm_host_find_by_sysid(int sysid);

/*
 * Register this NLM host with the local NSM so that we can be
 * notified if it reboots.
 */
void nlm_host_monitor(struct nlm_host *host, int state);
void nlm_host_unmonitor(struct nlm_host *host);

/*
 * Decrement the host reference count, freeing resources if the
 * reference count reaches zero.
 */
void nlm_host_release(struct nlm_host *host);

void nlm_host_notify_server(struct nlm_host *host, int newstate);
void nlm_host_notify_client(struct nlm_host *host);

/*
 * Return an RPC client handle that can be used to talk to the NLM
 * running on the given host.
 */
extern CLIENT *nlm_host_get_rpc(struct nlm_host *, int vers, bool_t issrv);

/*
 * Return the system ID for a host.
 */
extern int nlm_host_get_sysid(struct nlm_host *host);

/*
 * Return the remote NSM state value for a host.
 */
extern int nlm_host_get_state(struct nlm_host *host);

struct nlm_vnode *
nlm_vnode_findcreate(struct nlm_host *host, struct netobj *n);
void nlm_vnode_release(struct nlm_host *host, struct nlm_vnode *nv);


/*
 * When sending a blocking lock request, we need to track the request
 * in our waiting lock list. We add an entry to the waiting list
 * before we send the lock RPC so that we can cope with a granted
 * message arriving at any time. Call this function before sending the
 * lock rpc. If the lock succeeds, call nlm_deregister_wait_lock with
 * the handle this function returns, otherwise nlm_wait_lock. Both
 * will remove the entry from the waiting list.
 */
extern void *nlm_register_wait_lock(struct nlm_host *host,
    struct nlm4_lock *lock, struct vnode *vp);

/*
 * Deregister a blocking lock request. Call this if the lock succeeded
 * without blocking.
 */
extern void nlm_deregister_wait_lock(struct nlm_host *host, void *handle);

/*
 * Wait for a granted callback for a blocked lock request, waiting at
 * most timo ticks. If no granted message is received within the
 * timeout, return EWOULDBLOCK. If a signal interrupted the wait,
 * return EINTR - the caller must arrange to send a cancellation to
 * the server. In both cases, the request is removed from the waiting
 * list.
 */
extern int nlm_wait_lock(void *handle, int timo);

int nlm_cancel_async_lock(struct nlm_async_lock *af);
void nlm_free_async_lock(struct nlm_async_lock *af);

/*
 * Called when a host restarts.
 */
void nlm_do_notify1(nlm_sm_status *, void *, struct svc_req *);
void nlm_do_notify2(nlm_sm_status *, void *, struct svc_req *);

/*
 * Implementation for lock testing RPCs. If the request was handled
 * successfully and rpcp is non-NULL, *rpcp is set to an RPC client
 * handle which can be used to send an async rpc reply. Returns zero
 * if the request was handled, or a suitable unix error code
 * otherwise.
 */
void nlm_do_test(nlm4_testargs *, nlm4_testres *,
    struct svc_req *, nlm_res_cb);

/*
 * Implementation for lock setting RPCs.
 * See above for callback typedefs.
 */
void nlm_do_lock(nlm4_lockargs *, nlm4_res *, struct svc_req *,
    nlm_reply_cb, nlm_lkres_cb, nlm_grant_cb);

/*
 * Implementation for cancelling a pending lock request. If the
 * request was handled successfully and rpcp is non-NULL, *rpcp is set
 * to an RPC client handle which can be used to send an async rpc
 * reply. Returns zero if the request was handled, or a suitable unix
 * error code otherwise.
 */
void nlm_do_cancel(nlm4_cancargs *, nlm4_res *,
    struct svc_req *, nlm_res_cb);

/*
 * Implementation for unlocking RPCs. If the request was handled
 * successfully and rpcp is non-NULL, *rpcp is set to an RPC client
 * handle which can be used to send an async rpc reply. Returns zero
 * if the request was handled, or a suitable unix error code
 * otherwise.
 */
void nlm_do_unlock(nlm4_unlockargs *, nlm4_res *,
    struct svc_req *, nlm_res_cb);

/*
 * Implementation for granted RPCs. If the request was handled
 * successfully and rpcp is non-NULL, *rpcp is set to an RPC client
 * handle which can be used to send an async rpc reply. Returns zero
 * if the request was handled, or a suitable unix error code
 * otherwise.
 */
void nlm_do_granted(nlm4_testargs *, nlm4_res *,
    struct svc_req *, nlm_res_cb);

/*
 * Implementation for share/unshare RPCs.
 */
void nlm_do_share(nlm4_shareargs *, nlm4_shareres *, struct svc_req *);
void nlm_do_unshare(nlm4_shareargs *, nlm4_shareres *, struct svc_req *);

/*
 * Free all locks associated with the hostname argp->name.
 */
void nlm_do_free_all(nlm4_notify *, void *, struct svc_req *);

/*
 * Recover client lock state after a server reboot.
 */
typedef void (*recovery_cb)(struct nlm_host *);
extern void nlm_client_recovery(struct nlm_host *);
void nlm_set_recovery_cb(recovery_cb);

/*
 * Interface from NFS client code to the NLM.
 */
struct vop_advlock_args;
struct vop_reclaim_args;
extern int nlm_advlock(struct vop_advlock_args *ap);
extern int nlm_reclaim(struct vop_reclaim_args *ap);

/*
 * Acquire the next sysid for remote locks not handled by the NLM.
 */
extern uint32_t nlm_acquire_next_sysid(void);

#endif	/* _NLM_NLM_H_ */
