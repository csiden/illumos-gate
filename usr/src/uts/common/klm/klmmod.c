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
 * Copyright 2010 Nexenta Systems, Inc.  All rights reserved.
 */

/*
 * NFS Lock Manager, server-side and common.
 *
 * This file contains all the external entry points of klmmod.
 * Basically, this is the "glue" to the BSD nlm code.
 */

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/modctl.h>
#include <sys/flock.h>

#include <nfs/nfs.h>
#include <nfs/nfssys.h>
#include <nfs/lm.h>
#include <rpcsvc/nlm_prot.h>
#include "nlm_impl.h"


static struct modlmisc modlmisc = {
	&mod_miscops, "lock mgr common module"
};

static struct modlinkage modlinkage = {
	MODREV_1, &modlmisc, NULL
};

kmutex_t		lm_lck;
zone_key_t		nlm_zone_key;

/*
 * ****************************************************************
 * Stubs called in _init/_fini
 */

/*
 * Init/fini our collection of "sysid" mappings, which are
 * local numeric short-hand identifiers for remote systems
 * for which we're tracking locks.  See lm_get_sysid().
 */
void
lm_sysid_init()
{
}

void
lm_sysid_fini()
{
}

/*
 * Init/fini per-zone stuff for klm
 */
void *
lm_zone_init(zoneid_t zoneid)
{
	struct nlm_globals *g;

	g = kmem_zalloc(sizeof (*g), KM_SLEEP);

	/* XXX - todo... */

	return (g);
}

void
lm_zone_fini(zoneid_t zoneid, void *data)
{
	struct nlm_globals *g = data;

	kmem_free(g, sizeof (*g));
}



/*
 * ****************************************************************
 * module init, fini, info
 */
int
_init()
{
	int retval;

	mutex_init(&lm_lck, NULL, MUTEX_DEFAULT, NULL);
	zone_key_create(&nlm_zone_key, lm_zone_init, NULL, lm_zone_fini);
	/* Per-zone lockmgr data.  See: os/flock.c */
	zone_key_create(&flock_zone_key, flk_zone_init, NULL, flk_zone_fini);
	lm_sysid_init();

	retval = mod_install(&modlinkage);
	if (retval == 0)
		return (0);

	/*
	 * mod_install failed! undo above, reverse order
	 */

	lm_sysid_fini();
	(void) zone_key_delete(flock_zone_key);
	flock_zone_key = ZONE_KEY_UNINITIALIZED;
	(void) zone_key_delete(nlm_zone_key);
	mutex_destroy(&lm_lck);

	return (retval);
}

int
_fini()
{
	/* Don't unload. */
	return (EBUSY);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}



/*
 * ****************************************************************
 * Stubs listed in modstubs.s
 */

/*
 * klm system calls.  Start service on some endpoint.
 * Called by nfssys() LM_SVC, from lockd.
 */
int
lm_svc(struct lm_svc_args *args)
{
	struct knetconfig knc;
	char *netid;
	struct nlm_globals *g;
	struct file *fp = NULL;
	int err = 0;

	/* Get our "globals" */
	g = zone_getspecific(nlm_zone_key, curzone);

	/*
	 * Check version of lockd calling.
	 */
	if (args->version != LM_SVC_CUR_VERS) {
		cmn_err(CE_WARN, "lm_svc: version mismatch");
		return (EINVAL);
	}

	/*
	 * Build knetconfig, checking arg values.
	 * Also come up with the "netid" string.
	 * (With some knowledge of /etc/netconfig)
	 * XXX: Later, should just put all of this in
	 * the lm_svc_args and bump the version...
	 */
	bzero(&knc, sizeof (knc));

	/*
	 * NB: User-level encodes nc_semantics in n_proto
	 * as LM_TCP, LM_UDP. (for loopback too!)
	 */
	switch (args->n_proto) {
	case LM_TCP:
		knc.knc_semantics = NC_TPI_COTS_ORD;
		knc.knc_proto = NC_TCP;
		break;
	case LM_UDP:
		knc.knc_semantics = NC_TPI_CLTS;
		knc.knc_proto = NC_UDP;
		break;
	default:
		return (EINVAL);
	}
	switch (args->n_fmly) {
	case LM_INET:
		knc.knc_protofmly = NC_INET;
		netid = (args->n_proto == LM_TCP) ?
		    "tcp" : "udp";
		break;
	case LM_INET6:
		knc.knc_protofmly = NC_INET6;
		netid = (args->n_proto == LM_TCP) ?
		    "tcp6" : "udp6";
		break;
	case LM_LOOPBACK:
		knc.knc_protofmly = NC_LOOPBACK;
		/* Override what we set above. */
		knc.knc_proto = NC_NOPROTO;
		netid = (args->n_proto == LM_TCP) ?
		    "ticotsord" : "ticlts";
		break;
	default:
		return (EINVAL);
	}
	knc.knc_rdev = args->n_rdev;

	/*
	 * Setup service on the passed transport.
	 * NB: must releasef(fp) after this.
	 */
	if ((fp = getf(args->fd)) == NULL) {
		return (EBADF);
	}

	mutex_enter(&g->lock);

	/*
	 * Don't try to start while still shutting down,
	 * or lots of things will fail...
	 */
	if (g->run_status == NLM_ST_STOPPING) {
		mutex_exit(&g->lock);
		err = EAGAIN;
		goto out;
	}

	/*
	 * First caller does initialization.  The state change is
	 * just for observability, while nlm_svc_starting runs.
	 */
	if (g->run_status == NLM_ST_DOWN) {
		g->run_status = NLM_ST_STARTING;
		g->lockd_pid = curproc->p_pid;

		/* Save the options. */
		g->debug_level  = args->debug;
		g->cn_idle_tmo  = args->timout;
		g->grace_period = args->grace;
		g->retrans_tmo  = args->retransmittimeout;

		/* See nfs_sys.c (not yet per-zone) */
		if (INGLOBALZONE(curproc)) {
			rfs4_grace_period = args->grace;
			rfs4_lease_time   = args->grace;
		}

		mutex_exit(&g->lock);

		err = nlm_svc_starting(g, netid, &knc);
		if (err == 0)
			flk_set_lockmgr_status(FLK_LOCKMGR_UP);

		mutex_enter(&g->lock);

		g->run_status = (err == 0) ?
		    NLM_ST_UP : NLM_ST_DOWN;
	}

	mutex_exit(&g->lock);

	if (err == 0)
		err = nlm_svc_add_ep(g, fp, netid, &knc);

out:
	if (fp != NULL)
		releasef(args->fd);
	return (err);
}

/*
 * klm system calls.  Kill the lock manager.
 * Called by nfssys() KILL_LOCKMGR,
 * liblm:lm_shutdown() <- unused?
 */
int
lm_shutdown(void)
{
	struct nlm_globals *g;
	proc_t *p;
	pid_t pid;

	/* Get our "globals" */
	g = zone_getspecific(nlm_zone_key, curzone);

	pid = g->lockd_pid;
	if (pid == 0)
		return (ESRCH);

	mutex_enter(&pidlock);
	p = prfind(pid);
	if (p != NULL)
		psignal(p, SIGTERM);
	mutex_exit(&pidlock);

	return (0);
}

/*
 * Cleanup remote locks on FS un-export.
 * See nfs_export.c:unexport()
 */
void
lm_unexport(struct exportinfo *ei)
{
	/* XXX - todo... */
}

/*
 * CPR suspend/resume hooks.
 * See:cpr_suspend, cpr_resume
 *
 * Before suspend, get current state from "statd" on
 * all remote systems for which we have locks.
 *
 * After resume, check with those systems again,
 * and either reclaim locks, or do SIGLOST.
 */
void
lm_cprsuspend(void)
{
}

void
lm_cprresume(void)
{
}

/*
 * Called by nfs_frlock to check lock constraints.
 * Return non-zero if the lock request is "safe", i.e.
 * the range is not mapped, not MANDLOCK, etc.
 */
int
lm_safelock(vnode_t *vp, const struct flock64 *fl, cred_t *cr)
{
	/* XXX - todo... */
	return (0);
}

/*
 * Called by nfs_lockcompletion to check whether it's "safe"
 * to map the file (and cache it's data).  Walks the list of
 * file locks looking for any that are not "whole file".
 */
int
lm_safemap(const vnode_t *vp)
{
	/* XXX - todo... */
	return (0);
}

/*
 * Called by nfs_map() for the MANDLOCK case.
 * Return non-zero if the file has any locks with a
 * blocked request (sleep).
 */
int
lm_has_sleep(const vnode_t *vp)
{
	/* XXX - todo... */
	return (0);
}

/*
 * Called by NFS unmount.  Free the klm netconfig.
 * XXX: Only needed if we store something there.
 */
void
lm_free_config(struct knetconfig *knc)
{
}

/*
 * Called by NFS4 delegation code to check if there are any
 * NFSv2/v3 locks for the file, so it should not delegate.
 */
int
lm_vp_active(const vnode_t *vp)
{
	/* XXX - todo... */
	return (0);
}

/*
 * Find or create a "sysid" for given knc+addr.
 * name is optional.  Sets nc_changed if the
 * found knc_proto is different from passed.
 * Increments the reference count.
 *
 * Called internally, and in nfs4_find_sysid()
 *
 * XXX: struct lm_sysid is like our struct nlm_host.
 */
struct lm_sysid *
lm_get_sysid(struct knetconfig *knc, struct netbuf *addr,
		char *name, bool_t *nc_changed)
{
	/* XXX - todo... */
	return (0);
}

/*
 * Release a reference on a "sysid".
 */
void
lm_rel_sysid(struct lm_sysid *sysid)
{
	/* XXX - todo... */
}

/*
 * Alloc/free a sysid_t (number).
 * Used by NFSv4 rfs4_op_lockt and smbsrv/smb_fsop_frlock,
 * both to represent non-local locks outside of klm.
 */
sysid_t
lm_alloc_sysidt()
{
	return (-1);
}

void
lm_free_sysidt(sysid_t s)
{
	/* XXX - todo... */
}

/* Access private member lms->sysid */
sysid_t
lm_sysidt(struct lm_sysid *lms)
{
	/* XXX - todo... */
	return (-1);
}


/*
 * ****************************************************************
 * Stuff needed by klmops?
 */
