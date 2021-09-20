/* -*- C -*- */
/*
 * Copyright (c) 2013-2020 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */


#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_BE
#include "lib/trace.h"

#include "be/op.h"

#include "lib/misc.h"    /* M0_BITS */
#include "fop/fom.h"     /* m0_fom_phase_outcome */
#include "motr/magic.h"  /* M0_BE_OP_SET_MAGIC */

/**
 * @addtogroup be
 *
 * @{
 */

M0_TL_DESCR_DEFINE(bos, "m0_be_op::bo_children", static,
		   struct m0_be_op, bo_set_link, bo_set_link_magic,
		   M0_BE_OP_SET_LINK_MAGIC, M0_BE_OP_SET_MAGIC);
M0_TL_DEFINE(bos, static, struct m0_be_op);

static struct m0_sm_state_descr op_states[] = {
	[M0_BOS_INIT] = {
		.sd_flags   = M0_SDF_INITIAL,
		.sd_name    = "M0_BOS_INIT",
		.sd_allowed = M0_BITS(M0_BOS_ACTIVE),
	},
	[M0_BOS_ACTIVE] = {
		.sd_flags   = 0,
		.sd_name    = "M0_BOS_ACTIVE",
		.sd_allowed = M0_BITS(M0_BOS_DONE),
	},
	[M0_BOS_DONE] = {
		.sd_flags   = M0_SDF_TERMINAL,
		.sd_name    = "M0_BOS_DONE",
		.sd_allowed = 0,
	},
};

static struct m0_sm_trans_descr op_trans[] = {
	{ "started",   M0_BOS_INIT,   M0_BOS_ACTIVE },
	{ "completed", M0_BOS_ACTIVE, M0_BOS_DONE   },
};

M0_INTERNAL struct m0_sm_conf op_states_conf = {
	.scf_name      = "m0_be_op::bo_sm",
	.scf_nr_states = ARRAY_SIZE(op_states),
	.scf_state     = op_states,
	.scf_trans_nr  = ARRAY_SIZE(op_trans),
	.scf_trans     = op_trans
};

static void be_op_sm_init(struct m0_be_op *op)
{
	m0_sm_init(&op->bo_sm, &op_states_conf, M0_BOS_INIT, &op->bo_sm_group);
	op->bo_sm.sm_invariant_chk_off = true;
	m0_sm_addb2_counter_init(&op->bo_sm);
}

static void be_op_sm_fini(struct m0_be_op *op)
{
	M0_PRE(M0_IN(op->bo_sm.sm_state, (M0_BOS_INIT, M0_BOS_DONE)));

	if (op->bo_sm.sm_state == M0_BOS_INIT) {
		m0_sm_state_set(&op->bo_sm, M0_BOS_ACTIVE);
		m0_sm_state_set(&op->bo_sm, M0_BOS_DONE);
	}
	m0_sm_fini(&op->bo_sm);
}

M0_INTERNAL void m0_be_op_init(struct m0_be_op *op)
{
	M0_PRE_EX(M0_IS0(op));
	m0_sm_group_init(&op->bo_sm_group);
	be_op_sm_init(op);
	bos_tlist_init(&op->bo_children);
	bos_tlink_init(op);
	op->bo_rc                    = 0;
	op->bo_rc_is_set             = false;
	op->bo_kind                  = M0_BE_OP_KIND_NORMAL;
	op->bo_parent                = NULL;
	op->bo_set_triggered_by      = NULL;
	op->bo_set_addition_finished = false;
	op->bo_generation            = 0;
}

M0_INTERNAL void m0_be_op_fini(struct m0_be_op *op)
{
	bos_tlink_fini(op);
	bos_tlist_fini(&op->bo_children);
	m0_be_op_lock(op);
	be_op_sm_fini(op);
	m0_be_op_unlock(op);
	m0_sm_group_fini(&op->bo_sm_group);
}

M0_INTERNAL void m0_be_op_lock(struct m0_be_op *op)
{
	m0_sm_group_lock(op->bo_sm.sm_grp);
}

M0_INTERNAL void m0_be_op_unlock(struct m0_be_op *op)
{
	m0_sm_group_unlock(op->bo_sm.sm_grp);
}

M0_INTERNAL bool m0_be_op_is_locked(const struct m0_be_op *op)
{
	return m0_sm_group_is_locked(op->bo_sm.sm_grp);
}

static void be_op_set_add(struct m0_be_op *parent, struct m0_be_op *child)
{
	M0_PRE(m0_be_op_is_locked(parent));
	M0_PRE(m0_be_op_is_locked(child));

	bos_tlist_add_tail(&parent->bo_children, child);
	child->bo_parent = parent;
}

static bool be_op_set_del(struct m0_be_op *parent, struct m0_be_op *child)
{
	M0_PRE(m0_be_op_is_locked(parent));
	M0_PRE(m0_be_op_is_locked(child));
	M0_PRE(child->bo_parent == parent);
	M0_PRE(bos_tlist_contains(&parent->bo_children, child));

	bos_tlist_del(child);
	child->bo_parent = NULL;

	return bos_tlist_is_empty(&parent->bo_children);
}

M0_INTERNAL void m0_be_op_reset(struct m0_be_op *op)
{
	M0_ENTRY("op=%p", op);

	m0_be_op_lock(op);
	be_op_sm_fini(op);
	m0_be_op_unlock(op);
	M0_ASSERT(op->bo_parent == NULL);
	M0_ASSERT(bos_tlist_is_empty(&op->bo_children));
	op->bo_rc                    = 0;
	op->bo_rc_is_set             = false;
	op->bo_kind                  = M0_BE_OP_KIND_NORMAL;
	op->bo_set_triggered_by      = NULL;
	op->bo_set_addition_finished = false;
	be_op_sm_init(op);
}

static void be_op_state_change(struct m0_be_op     *op,
                               enum m0_be_op_state  state,
                               long                 generation,
                               struct m0_be_op     *trigger)
{
	struct m0_be_op *child;
	struct m0_be_op *parent = NULL;
	m0_be_op_cb_t    cb_gc = NULL;
	void            *cb_gc_param;
	long             parent_generation = -1;
	bool             last_child;

	// XXX comment
	M0_ENTRY("op=%p state=%s bo_kind=%d generation=%ld", op,
		 m0_sm_state_name(&op->bo_sm, state), op->bo_kind, generation);

	M0_PRE(M0_IN(state, (M0_BOS_ACTIVE, M0_BOS_DONE)));

	m0_be_op_lock(op);
	if (generation != -1 && generation != op->bo_generation) {
		M0_LOG(M0_ALWAYS, "generation mismatch for op=%p: "
		       "generation=%ld != op->bo_generation=%ld",
		       op, generation, op->bo_generation);
		m0_be_op_unlock(op);
		return;
	}
	M0_ASSERT(ergo(M0_IN(op->bo_kind, (M0_BE_OP_KIND_SET_AND,
					   M0_BE_OP_KIND_SET_OR)),
	               _0C(equi(state == M0_BOS_DONE, generation != -1)) &&
	               _0C(op->bo_set_triggered_by != NULL) &&
	               _0C(trigger != NULL)));
	if (op->bo_set_triggered_by != trigger) {
		M0_LOG(M0_ALWAYS, "another trigger for op set: "
		       "op=%p op->bo_set_triggered_by=%p trigger=%p",
		       op, op->bo_set_triggered_by, trigger);
		m0_be_op_unlock(op);
		return;
	}
	M0_ASSERT(ergo(op->bo_kind == M0_BE_OP_KIND_SET_AND &&
	               state == M0_BOS_DONE,
	               bos_tlist_is_empty(&op->bo_children)));
	M0_ASSERT(ergo(M0_IN(op->bo_kind, (M0_BE_OP_KIND_SET_AND,
	                                   M0_BE_OP_KIND_SET_OR)),
	                     op->bo_set_addition_finished));
	if (state == M0_BOS_DONE && op->bo_kind == M0_BE_OP_KIND_SET_OR) {
		while ((child = bos_tlist_head(&op->bo_children)) != NULL) {
			m0_be_op_unlock(op);
			m0_be_op_lock(child);
			m0_be_op_lock(op);
			if (child->bo_parent != NULL) {
				M0_ASSERT(child->bo_parent == op);
				be_op_set_del(op, child);
			}
			m0_be_op_unlock(child);
		}
	}
	if (op->bo_parent != NULL && state == M0_BOS_DONE) {
		m0_be_op_lock(op->bo_parent);
		last_child = be_op_set_del(op->bo_parent, op);
		if (op->bo_parent->bo_set_addition_finished &&
		    (op->bo_parent->bo_kind == M0_BE_OP_KIND_SET_OR ||
		     (op->bo_parent->bo_kind == M0_BE_OP_KIND_SET_AND &&
		      last_child))) {
			parent = op->bo_parent;
			parent_generation = parent->bo_generation;
			parent->bo_set_triggered_by = op;
		}
		m0_be_op_unlock(parent);
	}

	// XXX comment
	M0_LOG(M0_DEBUG, "op=%p parent=%p %s -> %s", op, parent,
	       m0_sm_state_name(&op->bo_sm, op->bo_sm.sm_state),
	       m0_sm_state_name(&op->bo_sm, state));
	if (state == M0_BOS_ACTIVE && op->bo_cb_active != NULL)
		op->bo_cb_active(op, op->bo_cb_active_param);
	++op->bo_generation;
	m0_sm_state_set(&op->bo_sm, state);
	if (state == M0_BOS_DONE && op->bo_cb_done != NULL)
		op->bo_cb_done(op, op->bo_cb_done_param);
	if (state == M0_BOS_DONE) {
		cb_gc       = op->bo_cb_gc;
		cb_gc_param = op->bo_cb_gc_param;
	}
	m0_be_op_unlock(op);

	/* if someone set bo_cb_gc then it's safe to call GC function here */
	if (cb_gc != NULL)
		cb_gc(op, cb_gc_param);

	if (parent != NULL) {
		M0_LOG(M0_DEBUG, "parent=%p -> state=%d: "
		       "parent_generation=%ld op=%p", parent, state,
		       parent_generation, op);
		be_op_state_change(parent, state, parent_generation, op);
	}
}

M0_INTERNAL void m0_be_op_active(struct m0_be_op *op)
{
	M0_PRE(op->bo_kind == M0_BE_OP_KIND_NORMAL);

	be_op_state_change(op, M0_BOS_ACTIVE, -1, NULL);
}

M0_INTERNAL void m0_be_op_done(struct m0_be_op *op)
{
	M0_PRE(op->bo_kind == M0_BE_OP_KIND_NORMAL);

	be_op_state_change(op, M0_BOS_DONE, -1, NULL);
}

M0_INTERNAL bool m0_be_op_is_done(struct m0_be_op *op)
{
	return op->bo_sm.sm_state == M0_BOS_DONE;
}

M0_INTERNAL void m0_be_op_callback_set(struct m0_be_op     *op,
				       m0_be_op_cb_t        cb,
				       void                *param,
				       enum m0_be_op_state  state)
{
	M0_PRE(M0_IN(state, (M0_BOS_ACTIVE, M0_BOS_DONE, M0_BOS_GC)));

	switch (state) {
	case M0_BOS_ACTIVE:
		M0_ASSERT(op->bo_cb_active == NULL);
		op->bo_cb_active = cb;
		op->bo_cb_active_param = param;
		break;
	case M0_BOS_DONE:
		M0_ASSERT(op->bo_cb_done == NULL);
		op->bo_cb_done = cb;
		op->bo_cb_done_param = param;
		break;
	case M0_BOS_GC:
		M0_ASSERT(op->bo_cb_gc == NULL);
		op->bo_cb_gc = cb;
		op->bo_cb_gc_param = param;
		break;
	default:
		M0_IMPOSSIBLE("invalid state");
	}
}

M0_INTERNAL int m0_be_op_tick_ret(struct m0_be_op *op,
				  struct m0_fom   *fom,
				  int              next_state)
{
	enum m0_fom_phase_outcome ret = M0_FSO_AGAIN;

	m0_be_op_lock(op);
	M0_PRE(M0_IN(op->bo_sm.sm_state, (M0_BOS_ACTIVE, M0_BOS_DONE)));

	if (op->bo_sm.sm_state == M0_BOS_ACTIVE) {
		ret = M0_FSO_WAIT;
		m0_fom_wait_on(fom, &op->bo_sm.sm_chan, &fom->fo_cb);
	}
	m0_be_op_unlock(op);

	m0_fom_phase_set(fom, next_state);
	return ret;
}

M0_INTERNAL void m0_be_op_wait(struct m0_be_op *op)
{
	struct m0_sm *sm = &op->bo_sm;
	int           rc;

	m0_be_op_lock(op);
	rc = m0_sm_timedwait(sm, M0_BITS(M0_BOS_DONE), M0_TIME_NEVER);
	M0_ASSERT_INFO(rc == 0, "rc=%d", rc);
	m0_be_op_unlock(op);
}

static void be_op_set_make(struct m0_be_op    *op,
                           enum m0_be_op_kind  op_kind)
{
	M0_PRE(M0_IN(op_kind, (M0_BE_OP_KIND_SET_AND, M0_BE_OP_KIND_SET_OR)));
	M0_PRE(op->bo_kind == M0_BE_OP_KIND_NORMAL);
	m0_be_op_active(op);
	m0_be_op_lock(op);
	op->bo_kind = op_kind;
	m0_be_op_unlock(op);
}

M0_INTERNAL void m0_be_op_make_set_and(struct m0_be_op *op)
{
	M0_ENTRY("op=%p", op);
	be_op_set_make(op, M0_BE_OP_KIND_SET_AND);
	M0_LEAVE("op=%p", op);
}

M0_INTERNAL void m0_be_op_make_set_or(struct m0_be_op *op)
{
	M0_ENTRY("op=%p", op);
	be_op_set_make(op, M0_BE_OP_KIND_SET_OR);
	M0_LEAVE("op=%p", op);
}

M0_INTERNAL void m0_be_op_set_add(struct m0_be_op *parent,
				  struct m0_be_op *child)
{
	M0_ENTRY("parent=%p child=%p", parent, child);
	/*
	 * Lock order here, in be_op_state_change() and in
	 * m0_be_op_set_add_finish() should be the same.
	 */
	m0_be_op_lock(child);
	m0_be_op_lock(parent);

	M0_ASSERT(M0_IN(parent->bo_kind, (M0_BE_OP_KIND_SET_AND,
	                                  M0_BE_OP_KIND_SET_OR)));
	M0_ASSERT(parent->bo_sm.sm_state != M0_BOS_DONE);
	M0_ASSERT(!parent->bo_set_addition_finished);

	be_op_set_add(parent, child);

	m0_be_op_unlock(parent);
	m0_be_op_unlock(child);
	M0_LEAVE("parent=%p child=%p", parent, child);
}

M0_INTERNAL void m0_be_op_set_add_finish(struct m0_be_op *op)
{
	struct m0_be_op *child;
	struct m0_be_op *trigger;
	long             generation;

	M0_ENTRY("op=%p", op);
	M0_ASSERT(M0_IN(op->bo_kind, (M0_BE_OP_KIND_SET_AND,
	                              M0_BE_OP_KIND_SET_OR)));
	m0_be_op_lock(op);
	M0_ASSERT(!bos_tlist_is_empty(&op->bo_children));
	generation = op->bo_generation;
	m0_tl_for(bos, &op->bo_children, child) {
		m0_be_op_unlock(op);
		/**
		 * Lock order here should be the same as in m0_be_op_set_add()
		 * and in be_op_state_change().
		 */
		m0_be_op_lock(child);
		m0_be_op_lock(op);
		if (op->bo_generation == generation) {
			/* extra parentheses to satisfy -Werror=parentheses */
			if (m0_be_op_is_done(child) &&
			    (op->bo_kind == M0_BE_OP_KIND_SET_OR ||
			     ((op->bo_kind == M0_BE_OP_KIND_SET_AND) &&
			      be_op_set_del(op, child)))) {
				op->bo_set_triggered_by = child;
				trigger = child;
			}
		} else {
			M0_LEAVE("generation has changed: "
			         "generation=%lu %p->bo_generation=%lu",
			         generation, op, op->bo_generation);
			m0_be_op_unlock(op);
			m0_be_op_unlock(child);
			return;
		}
		m0_be_op_unlock(child);
	} m0_tl_endfor;
	op->bo_set_addition_finished = true;
	m0_be_op_unlock(op);

	if (trigger != NULL)
		be_op_state_change(op, M0_BOS_DONE, generation, trigger);
	M0_LEAVE("op=%p", op);
}

M0_INTERNAL struct m0_be_op *m0_be_op_set_triggered_by(struct m0_be_op *op)
{
	M0_PRE(m0_be_op_is_done(op));
	M0_PRE(M0_IN(op->bo_kind, (M0_BE_OP_KIND_SET_OR,
				   M0_BE_OP_KIND_SET_AND)));
	M0_PRE(op->bo_set_triggered_by != NULL);

	/*
	 * If this BE op is done then there is nothing there that could modify
	 * m0_be_op::bo_set_triggered_by concurrently.
	 */
	return op->bo_set_triggered_by;
}

M0_INTERNAL void m0_be_op_rc_set(struct m0_be_op *op, int rc)
{
	m0_be_op_lock(op);
	M0_PRE(op->bo_sm.sm_state == M0_BOS_ACTIVE);
	M0_PRE(!op->bo_rc_is_set);
	op->bo_rc        = rc;
	op->bo_rc_is_set = true;
	m0_be_op_unlock(op);
}

M0_INTERNAL int m0_be_op_rc(struct m0_be_op *op)
{
	int rc;

	m0_be_op_lock(op);
	M0_PRE(op->bo_sm.sm_state == M0_BOS_DONE);
	M0_PRE(op->bo_rc_is_set);
	rc = op->bo_rc;
	m0_be_op_unlock(op);
	return rc;
}

/** @} end of be group */
#undef M0_TRACE_SUBSYSTEM

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
/*
 * vim: tabstop=8 shiftwidth=8 noexpandtab textwidth=80 nowrap
 */
