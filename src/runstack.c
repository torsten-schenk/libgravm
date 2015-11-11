#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <btree/memory.h>

#include "config.h"

#include <gravm/runstack.h>

#define EXEC_EXCEPTION_CASES \
	case GRAVM_RS_THROW: \
		self->throw_code = errno; \
		self->state = GRAVM_RS_STATE_THROWING; \
		return; \
	case GRAVM_RS_FATAL: \
		self->state = GRAVM_RS_STATE_EXECUTED_ERROR; \
		return; \
	default: \
		assert(false); \
		self->state = GRAVM_RS_STATE_EXECUTED_ERROR; \
		return;

#define THROW_EXCEPTION_CASES \
	case GRAVM_RS_THROW: \
	case GRAVM_RS_FATAL: \
		self->state = GRAVM_RS_STATE_EXECUTED_ERROR; \
		return; \
	default: \
		assert(false); \
		self->state = GRAVM_RS_STATE_EXECUTED_ERROR; \
		return;

#define THROW_FATAL_CASES \
	case GRAVM_RS_FATAL: \
		self->state = GRAVM_RS_STATE_EXECUTED_ERROR; \
		return; \
	default: \
		assert(false); \
		self->state = GRAVM_RS_STATE_EXECUTED_ERROR; \
		return;

typedef struct stackframe stackframe_t;

typedef struct {
	btree_it_t it;
	int lower;
	int upper;
} iterator_t;

typedef struct {
	int source; /* source node index as used in squirrel; -1: root */
	int priority;
	int target; /* target node index as used in squirrel */
	int id; /* edge index as used in squirrel */

	/* following fields are indices into btree. they specify outgoing edges on the 'target' node of this edge. -1: not available*/
	int out_lower;
	int out_boundary; /* boundary between pre-/post-outgoing edges (out_boundary = out_upper_pre = out_lower_post) */
	int out_upper;
} edge_entry_t;

struct stackframe {
	stackframe_t *prev;
	edge_entry_t *edge;
	int ip;
	int iteration; /* iteration couter for current edge */

	iterator_t out_it;
	edge_entry_t *out_cur; /* represents casted out_it.element; if NULL, iterator has reached its end */
	int out_upper; /* upper index in loops pre-/post outgoing edges */
	int out_nextip; /* next ip to jump to when iteration is finished */
	char user[1];
};

struct gravm_runstack {
	int state;
	bool suspended;

	stackframe_t *trash;
	stackframe_t *top;
	int stack_size;
	int max_stack_size;
	int framedata_size; /* userdata per stackframe */
	btree_t *edges;

	const gravm_runstack_callback_t *cb;

	int root_lower;
	int root_upper;

	iterator_t root_it;
	int throw_code;
	bool invoked; /* has a callback been invoked? */
	void *user;
};

static void exec_descend(
		gravm_runstack_t *self);
static void exec_edge_begin(
		gravm_runstack_t *self);
static void exec_edge_next(
		gravm_runstack_t *self);
static void exec_node_enter(
		gravm_runstack_t *self);
static void exec_begin_edge_prepare(
		gravm_runstack_t *self);
static void exec_loop_edge_prepare(
		gravm_runstack_t *self);
static void exec_begin_outgoing_pre(
		gravm_runstack_t *self);
static void exec_loop_outgoing_pre(
		gravm_runstack_t *self);
static void exec_node_run(
		gravm_runstack_t *self);
static void exec_begin_outgoing_post(
		gravm_runstack_t *self);
static void exec_loop_outgoing_post(
		gravm_runstack_t *self);
static void exec_begin_edge_unprepare(
		gravm_runstack_t *self);
static void exec_loop_edge_unprepare(
		gravm_runstack_t *self);
static void exec_node_leave(
		gravm_runstack_t *self);
static void exec_edge_end(
		gravm_runstack_t *self);
static void exec_ascend(
		gravm_runstack_t *self);
static void exec_pop(
		gravm_runstack_t *self);

static void throw_descend(
		gravm_runstack_t *self);
static void throw_edge_begin(
		gravm_runstack_t *self);
static void throw_edge_next(
		gravm_runstack_t *self);
static void throw_node_enter(
		gravm_runstack_t *self);
static void throw_loop_edge_prepare(
		gravm_runstack_t *self);
static void throw_loop_outgoing_pre(
		gravm_runstack_t *self);
static void throw_node_run(
		gravm_runstack_t *self);
static void throw_loop_outgoing_post(
		gravm_runstack_t *self);
static void throw_begin_edge_unprepare(
		gravm_runstack_t *self);
static void throw_loop_edge_unprepare(
		gravm_runstack_t *self);
static void throw_node_leave(
		gravm_runstack_t *self);
static void throw_edge_end(
		gravm_runstack_t *self);
static void throw_ascend(
		gravm_runstack_t *self);
static void throw_pop(
		gravm_runstack_t *self);

static const char *ip_names[] = {
	"descend",
	"edge_begin",
	"edge_next",
	"node_enter",
	"begin_edge_prepare",
	"loop_edge_prepare",
	"begin_outgoing_pre",
	"loop_outgoing_pre",
	"node_run",
	"begin_outgoing_post",
	"loop_outgoing_post",
	"begin_edge_unprepare",
	"loop_edge_unprepare",
	"node_leave",
	"edge_end",
	"ascend",
	"pop"
};

/* these functions return GRAVM_RS_THROWING, _FATAL and _SUCCESS. In case GRAVM_RS_FATAL is returned, errno is set */
static void (*step_exec[])(gravm_runstack_t*) = {
	exec_descend,
	exec_edge_begin,
	exec_edge_next,
	exec_node_enter,
	exec_begin_edge_prepare,
	exec_loop_edge_prepare,
	exec_begin_outgoing_pre,
	exec_loop_outgoing_pre,
	exec_node_run,
	exec_begin_outgoing_post,
	exec_loop_outgoing_post,
	exec_begin_edge_unprepare,
	exec_loop_edge_unprepare,
	exec_node_leave,
	exec_edge_end,
	exec_ascend,
	exec_pop
};

/* these functions return GRAVM_RS_THROWING, _FATAL, _SUCCESS and _TRUE.
 * throwing: immediate abortion, as we already have an unprocessed error
 * fatal: same as above
 * success: stay in throwing mode
 * true: the error has been catched, switch back to exec mode */
static void (*step_throw[])(gravm_runstack_t*) = {
	throw_descend,
	throw_edge_begin,
	throw_edge_next,
	throw_node_enter,
	NULL,
	throw_loop_edge_prepare,
	NULL,
	throw_loop_outgoing_pre,
	throw_node_run,
	NULL,
	throw_loop_outgoing_post,
	throw_begin_edge_unprepare,
	throw_loop_edge_unprepare,
	throw_node_leave,
	throw_edge_end,
	throw_ascend,
	throw_pop
};

static int cmp_full(
		const void *a_,
		const void *b_)
{
	const edge_entry_t *a = a_;
	const edge_entry_t *b = b_;
	if(a->source < b->source)
		return -1;
	else if(a->source > b->source)
		return 1;
	else if(a->priority < b->priority)
		return -1;
	else if(a->priority > b->priority)
		return 1;
	else if(a->target < b->target)
		return -1;
	else if(a->target > b->target)
		return 1;
	else if(a->id < b->id)
		return -1;
	else if(a->id > b->id)
		return 1;
	else
		return 0;
}

static int cmp_source(
		const void *a_,
		const void *b_)
{
	const edge_entry_t *a = a_;
	const edge_entry_t *b = b_;
	if(a->source < b->source)
		return -1;
	else if(a->source > b->source)
		return 1;
	else
		return 0;
}

static int cmp_priority(
		const void *a_,
		const void *b_)
{
	const edge_entry_t *a = a_;
	const edge_entry_t *b = b_;
	if(a->source < b->source)
		return -1;
	else if(a->source > b->source)
		return 1;
	else if(a->priority < 0 && b->priority >= 0)
		return -1;
	else if(a->priority >= 0 && b->priority < 0)
		return 1;
	else
		return 0;
}

static int cb_cmp(
		btree_t *btree,
		const void *a,
		const void *b,
		void *group)
{
	int (*cmp)(const void*, const void*) = group;
	return cmp(a, b);
}

static void pop(
		gravm_runstack_t *self)
{
	stackframe_t *top;

	assert(self->stack_size > 0);
	assert(self->top != NULL);

	top = self->top;
	self->top = self->top->prev;
	top->prev = self->trash;
	self->trash = top;
	self->stack_size--;
}

static int push(
		gravm_runstack_t *self,
		edge_entry_t *edge)
{
	stackframe_t *top;

	if(self->max_stack_size >= 0 && self->stack_size == self->max_stack_size)
		return -EOVERFLOW;
	if(self->trash == NULL) {
		self->trash = calloc(1, sizeof(stackframe_t) + self->framedata_size - 1);
		if(self->trash == NULL)
			return -ENOMEM;
	}
	top = self->top;
	self->top = self->trash;
	self->trash = self->top->prev;
	memset(self->top, 0, sizeof(stackframe_t) + self->framedata_size - 1);
	self->top->prev = top;
	self->top->iteration = -1;
	self->top->ip = GRAVM_RS_IP_DESCEND;
	self->top->edge = edge;
	self->stack_size++;
	return 0;
}

static bool it_begin(
		btree_t *btree,
		iterator_t *it,
		int lower,
		int upper)
{
#ifdef DEBUG
	int ret;
#endif

	if(lower < 0)
		return false;
	else if(lower >= upper)
		return false;
	it->lower = lower;
	it->upper = upper;

#ifndef DEBUG
	/* this must work if there is no bug since index must have been retrieved previously from same tree */
	btree_find_at(btree, lower, &it->it);
#else
	ret = btree_find_at(btree, lower, &it->it);
	assert(ret >= 0);
#endif
	return true;
}

static bool it_end(
		btree_t *btree,
		iterator_t *it,
		int upper,
		int lower)
{
#ifdef DEBUG
	int ret;
#endif

	if(upper <= 0)
		return false;
	else if(lower >= upper)
		return false;
	it->upper = upper;
	it->lower = lower;

#ifndef DEBUG
	/* this must work if there is no bug since index must have been retrieved previously from same tree */
	btree_find_at(btree, upper - 1, &it->it);
#else
	ret = btree_find_at(btree, upper - 1, &it->it);
	assert(ret >= 0);
#endif
	return true;
}

static bool it_next(
		iterator_t *it)
{
#ifdef DEBUG
	int ret;
#endif

#ifndef DEBUG
	btree_iterate_next(&it->it);
#else
	ret = btree_iterate_next(&it->it);
	assert(ret >= 0);
#endif
	if(it->it.index == it->upper)
		return false;

	return true;
}

static bool it_prev(
		iterator_t *it)
{
#ifdef DEBUG
	int ret;
#endif

	if(it->it.index == it->lower)
		return false;

#ifndef DEBUG
	btree_iterate_prev(&it->it);
#else
	ret = btree_iterate_prev(&it->it);
	assert(ret >= 0);
#endif
	return true;
}

static void exec_descend(
		gravm_runstack_t *self)
{
	int ret;
	
	if(self->top->prev != NULL && self->cb->descend != NULL) {
		ret = self->cb->descend(self->user, self->top->prev->user, self->top->user);
		self->invoked = true;
	}
	else
		ret = GRAVM_RS_TRUE;

	switch(ret) {
		case GRAVM_RS_TRUE:
			self->top->ip++;
			return;
		case GRAVM_RS_FALSE:
			self->top->ip = GRAVM_RS_IP_POP;
			return;
		EXEC_EXCEPTION_CASES
	}
}

static void exec_edge_begin(
		gravm_runstack_t *self)
{
	int ret;

	if(self->cb->edge_begin != NULL) {
		ret = self->cb->edge_begin(self->user, self->top->edge->id, self->top->user);
		self->invoked = true;
	}
	else
		ret = GRAVM_RS_TRUE;

	switch(ret) {
		case GRAVM_RS_TRUE:
			self->top->iteration = 0;
			self->top->ip = GRAVM_RS_IP_NODE_ENTER;
			return;
		case GRAVM_RS_FALSE:
			self->top->ip = GRAVM_RS_IP_ASCEND;
			return;
		EXEC_EXCEPTION_CASES
	}
}

static void exec_edge_next(
		gravm_runstack_t *self)
{
	int ret;

	self->top->iteration++;
	if(self->cb->edge_next != NULL) {
		ret = self->cb->edge_next(self->user, self->top->iteration, self->top->edge->id, self->top->user);
		self->invoked = true;
	}
	else if(self->top->iteration > 0)
		ret = GRAVM_RS_FALSE;
	else
		ret = GRAVM_RS_TRUE;

	switch(ret) {
		case GRAVM_RS_TRUE:
			self->top->ip++;
			return;
		case GRAVM_RS_FALSE:
			self->top->ip = GRAVM_RS_IP_EDGE_END;
			return;
		EXEC_EXCEPTION_CASES
	}
}

static void exec_node_enter(
		gravm_runstack_t *self)
{
	int ret;

	if(self->cb->node_enter != NULL) {
		ret = self->cb->node_enter(self->user, self->top->edge->target, self->top->user);
		self->invoked = true;
	}
	else
		ret = GRAVM_RS_TRUE;

	switch(ret) {
		case GRAVM_RS_TRUE:
			self->top->ip++;
			return;
		case GRAVM_RS_FALSE:
			self->top->ip = GRAVM_RS_IP_EDGE_NEXT;
			return;
		EXEC_EXCEPTION_CASES
	}
}

static void exec_begin_edge_prepare(
		gravm_runstack_t *self)
{
	if(self->cb->edge_prepare != NULL && it_begin(self->edges, &self->top->out_it, self->top->edge->out_lower, self->top->edge->out_upper)) {
		self->top->out_cur = self->top->out_it.it.element;
		self->top->ip++;
	}
	else
		self->top->ip = GRAVM_RS_IP_BEGIN_OUTGOING_PRE;
}

static void exec_loop_edge_prepare(
		gravm_runstack_t *self)
{
	int ret;

	assert(self->cb->edge_prepare != NULL);
	assert(self->top->out_cur != NULL);
	ret = self->cb->edge_prepare(self->user, self->top->out_cur->id, self->top->user);
	self->invoked = true;
	switch(ret) {
		case GRAVM_RS_SUCCESS:
			if(it_next(&self->top->out_it))
				self->top->out_cur = self->top->out_it.it.element;
			else
				self->top->ip++;
			return;
		EXEC_EXCEPTION_CASES
	}
}

static void exec_begin_outgoing_pre(
		gravm_runstack_t *self)
{
	if(it_begin(self->edges, &self->top->out_it, self->top->edge->out_lower, self->top->edge->out_boundary)) {
		self->top->out_cur = self->top->out_it.it.element;
		self->top->out_upper = self->top->edge->out_boundary;
		self->top->out_nextip = GRAVM_RS_IP_NODE_RUN;
		self->top->ip++;
	}
	else
		self->top->ip = GRAVM_RS_IP_NODE_RUN;
}

static void exec_loop_outgoing_pre(
		gravm_runstack_t *self)
{
	int ret;
	
	assert(self->top->out_cur != NULL);

	ret = push(self, self->top->out_cur);
	if(ret < 0) {
		errno = ret;
		self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
	}
}

static void exec_node_run(
		gravm_runstack_t *self)
{
	int ret;
	if(self->cb->node_run != NULL) {
		ret = self->cb->node_run(self->user, self->top->edge->target, self->top->user);
		self->invoked = true;
	}
	else
		ret = GRAVM_RS_TRUE;
	switch(ret) {
		case GRAVM_RS_TRUE:
			self->top->ip++;
			return;
		case GRAVM_RS_FALSE:
			self->top->ip = GRAVM_RS_IP_BEGIN_EDGE_UNPREPARE;
			return;
		EXEC_EXCEPTION_CASES
	}
}

static void exec_begin_outgoing_post(
		gravm_runstack_t *self)
{
	if(it_begin(self->edges, &self->top->out_it, self->top->edge->out_boundary, self->top->edge->out_upper)) {
		self->top->out_cur = self->top->out_it.it.element;
		self->top->out_upper = self->top->edge->out_upper;
		self->top->out_nextip = GRAVM_RS_IP_BEGIN_EDGE_UNPREPARE;
		self->top->ip++;
	}
	else
		self->top->ip = GRAVM_RS_IP_BEGIN_EDGE_UNPREPARE;
}

static void exec_loop_outgoing_post(
		gravm_runstack_t *self)
{
	int ret;
	
	assert(self->top->out_cur != NULL);

	ret = push(self, self->top->out_cur);
	if(ret < 0) {
		errno = ret;
		self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
	}
}

static void exec_begin_edge_unprepare(
		gravm_runstack_t *self)
{
	if(self->cb->edge_unprepare != NULL && it_end(self->edges, &self->top->out_it, self->top->edge->out_upper, self->top->edge->out_lower)) {
		self->top->out_cur = self->top->out_it.it.element;
		self->top->ip++;
	}
	else
		self->top->ip = GRAVM_RS_IP_NODE_LEAVE;
}

static void exec_loop_edge_unprepare(
		gravm_runstack_t *self)
{
	int ret;

	assert(self->cb->edge_unprepare != NULL);
	assert(self->top->out_cur != NULL);
	ret = self->cb->edge_unprepare(self->user, self->top->out_cur->id, self->top->user);
	self->invoked = true;
	switch(ret) {
		case GRAVM_RS_SUCCESS:
			if(it_prev(&self->top->out_it))
				self->top->out_cur = self->top->out_it.it.element;
			else
				self->top->ip++;
			return;
		EXEC_EXCEPTION_CASES
	}
}

static void exec_node_leave(
		gravm_runstack_t *self)
{
	int ret;

	if(self->cb->node_leave != NULL) {
		ret = self->cb->node_leave(self->user, self->top->edge->target, self->top->user);
		self->invoked = true;
	}
	else
		ret = GRAVM_RS_SUCCESS;
	switch(ret) {
		case GRAVM_RS_SUCCESS:
			self->top->ip = GRAVM_RS_IP_EDGE_NEXT;
			return;
		EXEC_EXCEPTION_CASES
	}
}

static void exec_edge_end(
		gravm_runstack_t *self)
{
	int ret;

	if(self->cb->edge_end != NULL) {
		ret = self->cb->edge_end(self->user, self->top->edge->id, self->top->user);
		self->invoked = true;
	}
	else
		ret = GRAVM_RS_SUCCESS;
	switch(ret) {
		case GRAVM_RS_SUCCESS:
			self->top->ip++;
			return;
		EXEC_EXCEPTION_CASES
	}
}

static void exec_ascend(
		gravm_runstack_t *self)
{
	int ret;

	if(self->top->prev != NULL && self->cb->ascend != NULL) {
		ret = self->cb->ascend(self->user, false, 0, self->top->prev->user, self->top->user);
		self->invoked = true;
	}
	else
		ret = GRAVM_RS_SUCCESS;
	switch(ret) {
		case GRAVM_RS_SUCCESS:
			self->top->ip++;
			return;
		EXEC_EXCEPTION_CASES
	}
}

static void exec_pop(
		gravm_runstack_t *self)
{
	pop(self);
	if(self->top != NULL) {
		if(it_next(&self->top->out_it))
			self->top->out_cur = self->top->out_it.it.element;
		else
			self->top->ip = self->top->out_nextip;
	}
}

static void throw_descend(
		gravm_runstack_t *self)
{
	pop(self);
}

static void throw_edge_begin(
		gravm_runstack_t *self)
{
	self->top->ip = GRAVM_RS_IP_EDGE_END;
}

static void throw_edge_next(
		gravm_runstack_t *self)
{
	self->top->ip = GRAVM_RS_IP_NODE_LEAVE;
}

static void throw_node_enter(
		gravm_runstack_t *self)
{
	self->top->ip = GRAVM_RS_IP_NODE_LEAVE;
}

static void throw_loop_edge_prepare(
		gravm_runstack_t *self)
{
	if(self->cb->edge_abort != NULL && it_prev(&self->top->out_it)) /* previously prepared edges for which abort() needs to be called? */
		self->top->out_cur = self->top->out_it.it.element;
	else
		self->top->out_cur = NULL;
	self->top->ip = GRAVM_RS_IP_BEGIN_EDGE_UNPREPARE;
}

static void throw_loop_outgoing_pre(
		gravm_runstack_t *self)
{
	self->top->ip = GRAVM_RS_IP_LOOP_OUTGOING_POST;
}

static void throw_node_run(
		gravm_runstack_t *self)
{
	self->top->ip = GRAVM_RS_IP_LOOP_OUTGOING_POST;
}

static void throw_loop_outgoing_post(
		gravm_runstack_t *self)
{
	if(self->cb->edge_abort != NULL && it_end(self->edges, &self->top->out_it, self->top->edge->out_upper, self->top->edge->out_lower))
		self->top->out_cur = self->top->out_it.it.element;
	else
		self->top->out_cur = NULL;
	self->top->ip = GRAVM_RS_IP_BEGIN_EDGE_UNPREPARE;
}

static void throw_begin_edge_unprepare(
		gravm_runstack_t *self)
{
	int ret;

	if(self->top->out_cur != NULL) { /* prepared edges remaining, abort them */
		assert(self->cb->edge_abort != NULL);
		ret = self->cb->edge_abort(self->user, self->throw_code, self->top->out_cur->id, self->top->user);
		self->invoked = true;
		if(it_prev(&self->top->out_it))
			self->top->out_cur = self->top->out_it.it.element;
		else
			self->top->out_cur = NULL;
		switch(ret) {
			case GRAVM_RS_SUCCESS:
				return;
			THROW_EXCEPTION_CASES
		}
	}
	else { /* no prepared edges remaining, call node_catch */
		if(self->cb->node_catch != NULL) {
			ret = self->cb->node_catch(self->user, self->throw_code, self->top->edge->target, self->top->user);
			self->invoked = true;
		}
		else
			ret = GRAVM_RS_FALSE;
		switch(ret) {
			case GRAVM_RS_FALSE:
				self->top->ip = GRAVM_RS_IP_NODE_LEAVE;
				return;
			case GRAVM_RS_TRUE:
				self->top->ip = GRAVM_RS_IP_EDGE_END;
				self->state = GRAVM_RS_STATE_EXECUTING;
				return;
			case GRAVM_RS_THROW:
				self->throw_code = errno;
				self->top->ip = GRAVM_RS_IP_NODE_LEAVE;
				return;
			THROW_FATAL_CASES
		}
	}
}

static void throw_loop_edge_unprepare(
		gravm_runstack_t *self)
{
	if(self->cb->edge_abort != NULL && it_prev(&self->top->out_it))
		self->top->out_cur = self->top->out_it.it.element;
	else
		self->top->out_cur = NULL;
	self->top->ip = GRAVM_RS_IP_BEGIN_EDGE_UNPREPARE;
}

static void throw_node_leave(
		gravm_runstack_t *self)
{
	int ret;

	if(self->cb->edge_catch != NULL) {
		ret = self->cb->edge_catch(self->user, self->throw_code, self->top->edge->id, self->top->user);
		self->invoked = true;
	}
	else
		ret = GRAVM_RS_FALSE;
	switch(ret) {
		case GRAVM_RS_FALSE:
			self->top->ip++;
			return;
		case GRAVM_RS_TRUE:
			self->top->ip = GRAVM_RS_IP_EDGE_NEXT;
			self->state = GRAVM_RS_STATE_EXECUTING;
			return;
		case GRAVM_RS_THROW:
			self->throw_code = errno;
			self->top->ip++;
			return;
		THROW_FATAL_CASES
	}
}

static void throw_edge_end(
		gravm_runstack_t *self)
{
	int ret;

	if(self->top->prev != NULL && self->cb->ascend != NULL) {
		ret = self->cb->ascend(self->user, true, self->throw_code, self->top->prev->user, self->top->user);
		self->invoked = true;
	}
	else
		ret = GRAVM_RS_SUCCESS;
	switch(ret) {
		case GRAVM_RS_SUCCESS:
			self->top->ip++;
			return;
		THROW_EXCEPTION_CASES
	}
	pop(self);
}

static void throw_ascend(
		gravm_runstack_t *self)
{
	pop(self);
}

static void throw_pop(
		gravm_runstack_t *self)
{
	pop(self);
}

gravm_runstack_t *gravm_runstack_new(
		const gravm_runstack_callback_t *cb,
		int max_stack_size,
		int framedata_size)
{
	gravm_runstack_t *rs;

	assert(cb->init != NULL);

	rs = calloc(1, sizeof(*rs));
	if(rs == NULL) {
		errno = -ENOMEM;
		return NULL;
	}

	rs->edges = btree_new(GRAVM_BTREE_ORDER, sizeof(edge_entry_t), cb_cmp, BTREE_OPT_DEFAULT);
	if(rs->edges == NULL) {
		free(rs);
		return NULL;
	}
	btree_set_group_default(rs->edges, cmp_full);

	rs->framedata_size = framedata_size;
	rs->cb = cb;
	rs->max_stack_size = max_stack_size;
	rs->state = GRAVM_RS_STATE_CREATED;

	return rs;
}

void gravm_runstack_destroy(
		gravm_runstack_t *self)
{
	stackframe_t *cur;
	stackframe_t *old;

	if(self->cb->destroy != NULL)
		self->cb->destroy(self->user);
	cur = self->trash;
	while(cur != NULL) {
		old = cur;
		cur = cur->prev;
		free(old);
	}
	cur = self->top;
	while(cur != NULL) {
		old = cur;
		cur = cur->prev;
		free(old);
	}
	btree_destroy(self->edges);
	free(self);
}

int gravm_runstack_prepare(
		gravm_runstack_t *self,
		void *user)
{
	int n;
	int i;
	int ret;
	edge_entry_t entry;
	btree_it_t it;
	edge_entry_t *cur;
	gravm_runstack_edgedef_t def;

	self->state = GRAVM_RS_STATE_CREATED;
	while(self->top != NULL)
		pop(self);
	btree_clear(self->edges);
	self->user = user;
	n = self->cb->init(self->user);
	if(n < 0)
		return n;

	for(i = 0; i < n; i++) {
		memset(&def, 0, sizeof(def));
		ret = self->cb->structure(self->user, i, &def);
		if(ret < 0)
			return ret;
		if(def.target == GRAVM_RS_ROOT)
			return -EINVAL;
		entry.id = i;
		entry.source = def.source;
		entry.priority = def.priority;
		entry.target = def.target;

		ret = btree_insert(self->edges, &entry);
		if(ret < 0)
			return ret;
	}

	entry.source = GRAVM_RS_ROOT;
	self->root_lower = btree_find_lower_group(self->edges, &entry, cmp_source, &it);
	self->root_upper = btree_find_upper_group(self->edges, &entry, cmp_source, NULL);
	assert(self->root_lower >= 0);
	assert(self->root_upper >= 0);
	if(self->root_lower == self->root_upper) /* missing root edges */
		return -ENOENT;

	for(/* it initialized above */; it.index < btree_size(self->edges); btree_iterate_next(&it)) {
		cur = it.element;

		/* boundaries of outgoing edges */
		entry.source = cur->target;
		cur->out_lower = btree_find_lower_group(self->edges, &entry, cmp_source, NULL);
		cur->out_upper = btree_find_upper_group(self->edges, &entry, cmp_source, NULL);
		assert(cur->out_upper >= cur->out_lower);

		/* boundaries of pre- and post-outgoing edges (priority < 0/>= 0) */
		entry.priority = -1;
		cur->out_boundary = btree_find_upper_group(self->edges, &entry, cmp_priority, NULL);
	}

	self->state = GRAVM_RS_STATE_PREPARED;

	return 0;
}

int gravm_runstack_suspend(
		gravm_runstack_t *self)
{
	self->suspended = true;
	return GRAVM_RS_SUCCESS;
}

int gravm_runstack_run(
		gravm_runstack_t *self)
{
	int ret;
	self->suspended = false;

	while(true) {
		ret = gravm_runstack_step(self);
		switch(ret) {
			case GRAVM_RS_TRUE:
				break;
			case GRAVM_RS_FALSE:
				return GRAVM_RS_SUCCESS;
			default:
				return ret;
		}
		if(self->suspended)
			return GRAVM_RS_SUSPENDED;
	}
}

int gravm_runstack_step(
		gravm_runstack_t *self)
{
	int ret;
	self->invoked = false;
	while(!self->invoked) {
		switch(self->state) {
			case GRAVM_RS_STATE_PREPARED:
				self->state = GRAVM_RS_STATE_EXECUTING;
				self->stack_size = 0;

				if(!it_begin(self->edges, &self->root_it, self->root_lower, self->root_upper)) {
					self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
					errno = -ENOENT;
					return GRAVM_RS_FATAL;
				}

				if(self->cb->begin != NULL) {
					ret = self->cb->begin(self->user);
					self->invoked = true;
				}
				else
					ret = GRAVM_RS_TRUE;
				switch(ret) {
					case GRAVM_RS_TRUE:
						ret = push(self, self->root_it.it.element);
						if(ret < 0) {
							self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
							return GRAVM_RS_FATAL;
						}
						break;
					case GRAVM_RS_FALSE:
						self->state = GRAVM_RS_STATE_EXECUTED;
						return GRAVM_RS_FALSE;
					case GRAVM_RS_FATAL:
						self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
						return GRAVM_RS_FATAL;
					case GRAVM_RS_THROW:
						if(errno == 0)
							self->state = GRAVM_RS_STATE_EXECUTED;
						else
							self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
						return GRAVM_RS_THROW;
					default:
						self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
						assert(false);
						return GRAVM_RS_FATAL;
				}
			case GRAVM_RS_STATE_EXECUTING:
				if(self->top == NULL) {
					if(it_next(&self->root_it)) {
						ret = push(self, self->root_it.it.element);
						if(ret < 0) {
							self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
							return GRAVM_RS_FATAL;
						}
					}
					else {
						if(self->cb->end != NULL)
							ret = self->cb->end(self->user);
						else
							ret = GRAVM_RS_SUCCESS;
						switch(ret) {
							case GRAVM_RS_SUCCESS:
								self->state = GRAVM_RS_STATE_EXECUTED;
								return GRAVM_RS_FALSE;
							case GRAVM_RS_FATAL:
								self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
								return GRAVM_RS_FATAL;
							case GRAVM_RS_THROW:
								if(errno == 0)
									self->state = GRAVM_RS_STATE_EXECUTED;
								else
									self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
								return GRAVM_RS_THROW;
							default:
								self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
								assert(false);
								return GRAVM_RS_FATAL;
						}
					}
				}
				step_exec[self->top->ip](self);
				if(self->state == GRAVM_RS_STATE_EXECUTED_ERROR)
					return GRAVM_RS_FATAL;
				break;
			case GRAVM_RS_STATE_THROWING:
				if(self->top == NULL) {
					if(it_next(&self->root_it)) {
						ret = push(self, self->root_it.it.element);
						if(ret < 0) {
							self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
							return GRAVM_RS_FATAL;
						}
					}
					else {
						if(self->cb->end != NULL)
							ret = self->cb->end(self->user);
						else
							ret = GRAVM_RS_SUCCESS;
						switch(ret) {
							case GRAVM_RS_SUCCESS:
								if(self->throw_code == 0)
									self->state = GRAVM_RS_STATE_EXECUTED;
								else
									self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
								return GRAVM_RS_THROW;
							case GRAVM_RS_FATAL:
							case GRAVM_RS_THROW:
								self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
								return GRAVM_RS_FATAL;
							default:
								self->state = GRAVM_RS_STATE_EXECUTED_ERROR;
								assert(false);
								return GRAVM_RS_FATAL;
						}
					}
				}
				step_throw[self->top->ip](self);
				if(self->state == GRAVM_RS_STATE_EXECUTED_ERROR)
					return GRAVM_RS_FATAL;
				break;
			case GRAVM_RS_STATE_EXECUTED:
				return GRAVM_RS_FALSE;
			case GRAVM_RS_STATE_EXECUTED_ERROR:
				return GRAVM_RS_FALSE;
			default:
				assert(false);
				errno = -EINVAL;
				return GRAVM_RS_FATAL;
		}
	}
	return GRAVM_RS_TRUE;
}

int gravm_runstack_debug_state(
		gravm_runstack_t *self)
{
	return self->state;
}

int gravm_runstack_debug_throw_code(
		gravm_runstack_t *self)
{
	return self->throw_code;
}

int gravm_runstack_debug_ip(
		gravm_runstack_t *self)
{
	if(self->top != NULL)
		return self->top->ip;
	else
		return -EINVAL;
}

int gravm_runstack_debug_stack_size(
		gravm_runstack_t *self)
{
	return self->stack_size;
}

int gravm_runstack_debug_stack(
		gravm_runstack_t *self,
		bool (*frame)(void *user, int edge, int ip, void *framedata))
{
	stackframe_t *cur = self->top;

	while(cur != NULL) {
		if(!frame(self->user, cur->edge->id, cur->ip, cur->user))
			return 0;
		cur = cur->prev;
	}
	return 0;
}

const char *gravm_runstack_ip_name(
		int ip)
{
	if(ip >= 0 && ip < sizeof(ip_names) / sizeof(*ip_names))
		return ip_names[ip];
	else
		return NULL;
}



void gravm_runstack_dump(
		gravm_runstack_t *self,
		void (*print_edge)(FILE *f, void *user, int id),
		void (*print_node)(FILE *f, void *user, int id))
{
	btree_it_t it;
	edge_entry_t *edge;
	int ret;

	printf("---------------------------- DUMP RUNSTACK ---------------------\n");
	printf("state: ");
	switch(self->state) {
		case GRAVM_RS_STATE_CREATED:
			printf("created\n");
			break;
		case GRAVM_RS_STATE_PREPARED:
			printf("prepared\n");
			break;
		case GRAVM_RS_STATE_EXECUTING:
			printf("executing\n");
			break;
		case GRAVM_RS_STATE_THROWING:
			printf("throwing %d\n", self->throw_code);
			break;
		case GRAVM_RS_STATE_EXECUTED_ERROR:
			printf("executed with error\n");
			break;
		case GRAVM_RS_STATE_EXECUTED:
			printf("executed without error\n");
			break;
		default:
			printf("(unknown)\n");
			break;
	}
	printf("\n");
	printf("edges:\n");
	ret = btree_find_at(self->edges, 0, &it);
	if(ret < 0)
		printf("  (error)\n");
	else do {
		edge = it.element;
		printf("  ");
		if(print_edge == NULL)
			printf("%d", edge->id);
		else
			print_edge(stdout, self->user, edge->id);
		printf(": source=");
		if(edge->source == GRAVM_RS_ROOT)
			printf("root");
		else if(print_node == NULL)
			printf("%d", edge->source);
		else
			print_node(stdout, self->user, edge->source);
		printf(", priority=%d, target=", edge->priority);
		if(print_node == NULL)
			printf("%d", edge->target);
		else
			print_node(stdout, self->user, edge->target);
		printf("\n");
	} while(btree_iterate_next(&it) < btree_size(self->edges));
	printf("\n");
	stackframe_t *cur = self->top;
	printf("stack (top to bottom):\n");
	while(cur) {
		printf("  instruction=%s, ", ip_names[cur->ip]);
		printf("edge=");
		if(print_edge == NULL)
			printf("%d", cur->edge->id);
		else
			print_edge(stdout, self->user, cur->edge->id);
		printf(", iteration=%d\n", cur->iteration);
		cur = cur->prev;
	}
	printf("----------------------------------------------------------------\n");
}

#ifdef TESTING
#include "../test/runstack.h"
#endif

