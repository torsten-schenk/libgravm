#include <string.h>

#include "common.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(X) (sizeof(X) / sizeof(*(X)))
#endif

#define CB_DONE \
	do { \
		if((call->flags & FCALL_ERRNO) != 0) \
			errno = call->err; \
		if((call->flags & FCALL_RET) != 0) \
			return call->ret; \
		else \
			return ret; \
	} while(false)

#define CB_BEGIN(ID, DEFAULT) \
	test_context_t *ctx = data; \
	const test_call_t *call = &ctx->steps[ctx->step].call; \
	int ret = DEFAULT; \
	if(ctx->skipping) \
		return ret; \
	ctx->cbresult.called = ID;

static const gravm_runstack_edgedef_t test_norootedges[] = {
	{ .source = 1, .target = 1, .priority = 0 }
};

static const gravm_runstack_edgedef_t test_roottarget[] = {
	{ .source = GRAVM_RS_ROOT, .target = GRAVM_RS_ROOT, .priority = 0 }
};

static const gravm_runstack_edgedef_t test_multiplesame[] = {
	{ .source = GRAVM_RS_ROOT, .target = 1, .priority = 0 },
	{ .source = GRAVM_RS_ROOT, .target = 1, .priority = 0 }
};

static const gravm_runstack_edgedef_t test_1edge[] = {
	{ .source = GRAVM_RS_ROOT, .target = 1, .priority = 0 }
};

static const gravm_runstack_edgedef_t test_1targetedge[] = {
	{ .source = GRAVM_RS_ROOT, .target = 1, .priority = 0 },
	{ .source = 1, .target = 2, .priority = 0 }
};

static const gravm_runstack_edgedef_t test_1pretargetedge[] = {
	{ .source = GRAVM_RS_ROOT, .target = 1, .priority = 0 },
	{ .source = 1, .target = 2, .priority = -1 }
};

static const gravm_runstack_edgedef_t test_3edge[] = {
	{ .source = GRAVM_RS_ROOT, .target = 3, .priority = 3 },
	{ .source = GRAVM_RS_ROOT, .target = 2, .priority = 2 },
	{ .source = GRAVM_RS_ROOT, .target = 1, .priority = 1 }
};

enum {
	CALL_NONE,
	CALL_BEGIN,
	CALL_DESCEND,
	CALL_EDGE_BEGIN,
	CALL_EDGE_NEXT,
	CALL_NODE_ENTER,
	CALL_EDGE_PREPARE,
	CALL_NODE_RUN,
	CALL_EDGE_UNPREPARE,
	CALL_NODE_LEAVE,
	CALL_NODE_CATCH,
	CALL_EDGE_END,
	CALL_EDGE_ABORT,
	CALL_EDGE_CATCH,
	CALL_ASCEND,
	CALL_END
};

static const char *test_call_names[] = {
	"none",
	"begin",
	"descend",
	"edge_begin",
	"edge_next",
	"node_enter",
	"edge_prepare",
	"node_run",
	"edge_unprepare",
	"node_leave",
	"node_catch",
	"edge_end",
	"edge_abort",
	"edge_catch",
	"ascend",
	"end"
};

enum {
	FRESULT_IP = 0x00000001,
	FRESULT_STATE = 0x00000002,
	FRESULT_ID = 0x00000004,
	FRESULT_FRAME = 0x00000008,
	FRESULT_STACK_SIZE = 0x00000010,
	FRESULT_RET = 0x00000020,
	FRESULT_CALL = 0x00000040,
	FRESULT_ITERATION = 0x00000080,
	FRESULT_ERRNO = 0x00000100,
	FRESULT_THROW_CODE = 0x00000200,
	FRESULT_DUMP = 0x80000000,
/*	FRESULT_NONE = 0
	FRESULT_NOCB = FRESULT_IP | FRESULT_STATE | FRESULT_STACK_SIZE | FRESULT_RET,
	FRESULT_ALL = FRESULT_IP | FRESULT_STATE | FRESULT_ID | FRESULT_FRAME | FRESULT_STACK_SIZE | FRESULT_RET | FRESULT_CALL*/
};

enum {
	FCALL_RET = 0x00000001,
	FCALL_ERRNO = 0x00000002,
/*	FCALL_NONE = 0,
	FCALL_ALL = FCALL_RET | FCALL_ERRNO | FCALL_COUNTER*/
};

typedef struct {

} test_frame_t;

typedef struct {
	int flags; /* which operations should be performed? possible:*/
	int ret;
	int err;
	int counter;
} test_call_t;

typedef struct {
	int flags; /* specify which fields to check */
	int ip;
	int state;
	int id;
	int stack_size;
	int ret;
	int called;
	int iteration;
	int err;
	int throw_code;
	test_frame_t frame;
} test_result_t;

typedef struct {
	int skip;
	test_call_t call;
	test_result_t result;
} test_step_t;

typedef struct {
	const gravm_runstack_edgedef_t *edges;
	int n_edges;
	const test_step_t *steps;
	int n_steps;

	int step;
	bool skipping;
	struct {
		int called;
		int id; /* node/edge id */
		int iteration;
		int throw_code;
		test_frame_t frame;
	} cbresult;
} test_context_t;

static gravm_runstack_t *test_self;
static gravm_runstack_callback_t test_cb;
static test_context_t test_ctx;

static void test_run_steps()
{
	int ret;
	int i;

	ret = gravm_runstack_prepare(test_self, &test_ctx);
	CU_ASSERT_EQUAL_FATAL(ret, 0);
	for(test_ctx.step = 0; test_ctx.step < test_ctx.n_steps; test_ctx.step++) {
		printf("%d ", test_ctx.step);
		test_ctx.skipping = true;
		for(i = 0; i < test_ctx.steps[test_ctx.step].skip; i++) {
			ret = gravm_runstack_step(test_self);
			CU_ASSERT_EQUAL_FATAL(ret, GRAVM_RS_TRUE); /* skip only allowed if further steps can be performed */
		}
		test_ctx.skipping = false;
		test_ctx.cbresult.called = CALL_NONE;
		ret = gravm_runstack_step(test_self);
		if((test_ctx.steps[test_ctx.step].result.flags & FRESULT_DUMP) != 0) {
			printf("STEP: ret=%d, called=%s\n", ret, test_call_names[test_ctx.cbresult.called]);
			gravm_runstack_dump(test_self, NULL, NULL);
		}
		if((test_ctx.steps[test_ctx.step].result.flags & FRESULT_RET) != 0) {
			CU_ASSERT_EQUAL_FATAL(ret, test_ctx.steps[test_ctx.step].result.ret);
		}
		else if(test_ctx.step < test_ctx.step - 1) {
			CU_ASSERT_EQUAL_FATAL(ret, GRAVM_RS_TRUE);
		}
		if((test_ctx.steps[test_ctx.step].result.flags & FRESULT_IP) != 0) {
			CU_ASSERT_EQUAL_FATAL(gravm_runstack_debug_ip(test_self), test_ctx.steps[test_ctx.step].result.ip);
		}
		if((test_ctx.steps[test_ctx.step].result.flags & FRESULT_STACK_SIZE) != 0) {
			CU_ASSERT_EQUAL_FATAL(gravm_runstack_debug_stack_size(test_self), test_ctx.steps[test_ctx.step].result.stack_size);
		}
		if((test_ctx.steps[test_ctx.step].result.flags & FRESULT_STATE) != 0) {
			CU_ASSERT_EQUAL_FATAL(gravm_runstack_debug_state(test_self), test_ctx.steps[test_ctx.step].result.state);
		}
		if((test_ctx.steps[test_ctx.step].result.flags & FRESULT_ID) != 0) {
			CU_ASSERT_EQUAL_FATAL(test_ctx.cbresult.id, test_ctx.steps[test_ctx.step].result.id);
		}
		if((test_ctx.steps[test_ctx.step].result.flags & FRESULT_FRAME) != 0) {
			CU_ASSERT_EQUAL_FATAL(memcmp(&test_ctx.cbresult.frame, &test_ctx.steps[test_ctx.step].result.frame, sizeof(test_frame_t)), 0);
		}
		if((test_ctx.steps[test_ctx.step].result.flags & FRESULT_CALL) != 0) {
			CU_ASSERT_EQUAL_FATAL(test_ctx.cbresult.called, test_ctx.steps[test_ctx.step].result.called);
		}
		if((test_ctx.steps[test_ctx.step].result.flags & FRESULT_ITERATION) != 0) {
			CU_ASSERT_EQUAL_FATAL(test_ctx.cbresult.iteration, test_ctx.steps[test_ctx.step].result.iteration);
		}
		if((test_ctx.steps[test_ctx.step].result.flags & FRESULT_ERRNO) != 0) {
			CU_ASSERT_EQUAL_FATAL(errno, test_ctx.steps[test_ctx.step].result.err);
		}
		if((test_ctx.steps[test_ctx.step].result.flags & FRESULT_THROW_CODE) != 0) {
			CU_ASSERT_EQUAL_FATAL(test_ctx.cbresult.throw_code, test_ctx.steps[test_ctx.step].result.throw_code);
		}
	}
}

/******************************** CALLBACKS **********************************/

static int cb_test_init(
		void *data)
{
	test_context_t *ctx = data;
	return ctx->n_edges;
}

static void cb_test_destroy(
		void *data)
{}

static int cb_test_structure(
		void *data,
		int edge,
		gravm_runstack_edgedef_t *def)
{
	test_context_t *ctx = data;
	*def = ctx->edges[edge];
	return 0;
}

static int cb_test_begin(
		void *data)
{
	CB_BEGIN(CALL_BEGIN, GRAVM_RS_TRUE)
	CB_DONE;
}

static int cb_test_descend(
		void *data,
		int edge,
		void *parent,
		void *child)
{
	CB_BEGIN(CALL_DESCEND, GRAVM_RS_TRUE)
	CB_DONE;
}

static int cb_test_edge_begin(
		void *data,
		int id,
		void *frame)
{
	CB_BEGIN(CALL_EDGE_BEGIN, GRAVM_RS_TRUE)
	memcpy(&ctx->cbresult.frame, frame, sizeof(test_frame_t));
	ctx->cbresult.id = id;
	CB_DONE;
}

static int cb_test_edge_next(
		void *data,
		int iteration,
		int id,
		void *frame)
{
	CB_BEGIN(CALL_EDGE_NEXT, GRAVM_RS_TRUE)
	memcpy(&ctx->cbresult.frame, frame, sizeof(test_frame_t));
	ctx->cbresult.id = id;
	ctx->cbresult.iteration = iteration;
	CB_DONE;
}

static int cb_test_edge_prepare(
		void *data,
		int id,
		void *frame)
{
	CB_BEGIN(CALL_EDGE_PREPARE, GRAVM_RS_SUCCESS)
	memcpy(&ctx->cbresult.frame, frame, sizeof(test_frame_t));
	ctx->cbresult.id = id;
	CB_DONE;
}

static int cb_test_node_enter(
		void *data,
		int id,
		void *frame)
{
	CB_BEGIN(CALL_NODE_ENTER, GRAVM_RS_TRUE)
	memcpy(&ctx->cbresult.frame, frame, sizeof(test_frame_t));
	ctx->cbresult.id = id;
	CB_DONE;
}

static int cb_test_node_run(
		void *data,
		int id,
		void *frame)
{
	CB_BEGIN(CALL_NODE_RUN, GRAVM_RS_TRUE)
	memcpy(&ctx->cbresult.frame, frame, sizeof(test_frame_t));
	ctx->cbresult.id = id;
	CB_DONE;
}

static int cb_test_node_leave(
		void *data,
		int id,
		void *frame)
{
	CB_BEGIN(CALL_NODE_LEAVE, GRAVM_RS_SUCCESS)
	memcpy(&ctx->cbresult.frame, frame, sizeof(test_frame_t));
	ctx->cbresult.id = id;
	CB_DONE;
}

static int cb_test_node_catch(
		void *data,
		int err,
		int id,
		void *frame)
{
	CB_BEGIN(CALL_NODE_CATCH, GRAVM_RS_FALSE)
	memcpy(&ctx->cbresult.frame, frame, sizeof(test_frame_t));
	ctx->cbresult.throw_code = err;
	ctx->cbresult.id = id;
	CB_DONE;
}

static int cb_test_edge_unprepare(
		void *data,
		int id,
		void *frame)
{
	CB_BEGIN(CALL_EDGE_UNPREPARE, GRAVM_RS_SUCCESS)
	memcpy(&ctx->cbresult.frame, frame, sizeof(test_frame_t));
	ctx->cbresult.id = id;
	CB_DONE;
}

static int cb_test_edge_end(
		void *data,
		int id,
		void *frame)
{
	CB_BEGIN(CALL_EDGE_END, GRAVM_RS_SUCCESS)
	memcpy(&ctx->cbresult.frame, frame, sizeof(test_frame_t));
	ctx->cbresult.id = id;
	CB_DONE;
}

static int cb_test_edge_abort(
		void *data,
		int err,
		int id,
		void *frame)
{
	CB_BEGIN(CALL_EDGE_ABORT, GRAVM_RS_FALSE)
	memcpy(&ctx->cbresult.frame, frame, sizeof(test_frame_t));
	ctx->cbresult.throw_code = err;
	ctx->cbresult.id = id;
	CB_DONE;
}

static int cb_test_edge_catch(
		void *data,
		int err,
		int id,
		void *frame)
{
	CB_BEGIN(CALL_EDGE_CATCH, GRAVM_RS_FALSE)
	memcpy(&ctx->cbresult.frame, frame, sizeof(test_frame_t));
	ctx->cbresult.throw_code = err;
	ctx->cbresult.id = id;
	CB_DONE;
}

static int cb_test_ascend(
		void *data,
		int edge,
		bool throwing,
		int err,
		void *parent,
		void *child)
{
	CB_BEGIN(CALL_ASCEND, GRAVM_RS_SUCCESS)
	if(throwing)
		ctx->cbresult.throw_code = err;
	CB_DONE;
}

static int cb_test_end(
		void *data)
{
	CB_BEGIN(CALL_END, GRAVM_RS_SUCCESS)
	CB_DONE;
}

/******************************** TEST 1 **********************************/

static int test1_init()
{
	test_self = gravm_runstack_new(&test_cb, -1, sizeof(test_frame_t));
	if(test_self == NULL)
		return -1;
	return 0;
}

static int test1_cleanup()
{
	gravm_runstack_destroy(test_self);
	return 0;
}

static void test1_no_edges()
{
	int ret;
	test_context_t ctx;

	ctx.edges = NULL;
	ctx.n_edges = 0;

	ret = gravm_runstack_prepare(test_self, &ctx);
	CU_ASSERT_EQUAL(ret, -ENOENT);
	CU_ASSERT_EQUAL(gravm_runstack_debug_state(test_self), GRAVM_RS_STATE_CREATED);
}

static void test1_no_root_edges()
{
	int ret;
	test_context_t ctx;

	ctx.edges = test_norootedges;
	ctx.n_edges = ARRAY_SIZE(test_norootedges);

	ret = gravm_runstack_prepare(test_self, &ctx);
	CU_ASSERT_EQUAL(ret, -ENOENT);
	CU_ASSERT_EQUAL(gravm_runstack_debug_state(test_self), GRAVM_RS_STATE_CREATED);
}

static void test1_multiple_same()
{
	int ret;
	test_context_t ctx;

	ctx.edges = test_multiplesame;
	ctx.n_edges = ARRAY_SIZE(test_multiplesame);

	ret = gravm_runstack_prepare(test_self, &ctx);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(gravm_runstack_debug_state(test_self), GRAVM_RS_STATE_PREPARED);
}

static void test1_root_target()
{
	int ret;
	test_context_t ctx;

	ctx.edges = test_roottarget;
	ctx.n_edges = ARRAY_SIZE(test_roottarget);

	ret = gravm_runstack_prepare(test_self, &ctx);
	CU_ASSERT_EQUAL(ret, -EINVAL);
	CU_ASSERT_EQUAL(gravm_runstack_debug_state(test_self), GRAVM_RS_STATE_CREATED);
}

/******************************** TEST 2 **********************************/

static int test2_init()
{
	test_ctx.edges = test_1edge;
	test_ctx.n_edges = ARRAY_SIZE(test_1edge);

	test_self = gravm_runstack_new(&test_cb, -1, sizeof(test_frame_t));
	if(test_self == NULL)
		return -1;
	return 0;
}

static int test2_cleanup()
{
	gravm_runstack_destroy(test_self);
	return 0;
}

static void test2_full_run_0()
{
	static const test_step_t steps[] = {
		{	.result = { /* call begin() */
				.flags = FRESULT_STATE | FRESULT_STACK_SIZE | FRESULT_CALL,
				.stack_size = 1,
				.called = CALL_BEGIN,
				.state = GRAVM_RS_STATE_EXECUTING } },

		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_begin() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_BEGIN } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL,
				.ret = GRAVM_RS_FALSE,
				.state = GRAVM_RS_STATE_EXECUTED,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_full_run_1()
{
	static const test_step_t steps[] = {
		{	.result = { /* call begin() */
				.flags = FRESULT_STATE | FRESULT_STACK_SIZE | FRESULT_CALL,
				.stack_size = 1,
				.called = CALL_BEGIN,
				.state = GRAVM_RS_STATE_EXECUTING } },

		{	.result = { /* call edge_begin() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_BEGIN } },

		{	.result = { /* call node_enter() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_ENTER } },

		{	.result = { /* call node_run() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ID,
				.id = 1,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_RUN } },

		{	.result = { /* call node_leave() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_LEAVE } },

		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_next() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ITERATION,
				.iteration = 1,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_NEXT } },

		{	.result = { /* call edge_end() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_END } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL,
				.ret = GRAVM_RS_FALSE,
				.state = GRAVM_RS_STATE_EXECUTED,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_full_run_3()
{
	static const test_step_t steps[] = {
		{	.skip = 6,
			.result = { /* call node_enter() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_ENTER } },

		{	.result = { /* call node_run() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ID,
				.id = 1,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_RUN } },

		{	.result = { /* call node_leave() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_LEAVE } },

		{	.result = { /* call edge_next() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ITERATION,
				.state = GRAVM_RS_STATE_EXECUTING,
				.iteration = 2,
				.called = CALL_EDGE_NEXT } },

		{	.result = { /* call node_enter() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_ENTER } },

		{	.result = { /* call node_run() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ID,
				.id = 1,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_RUN } },

		{	.result = { /* call node_leave() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_LEAVE } },

		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_next() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ITERATION,
				.state = GRAVM_RS_STATE_EXECUTING,
				.iteration = 3,
				.called = CALL_EDGE_NEXT } },

		{	.result = { /* call edge_end() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_END } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL,
				.ret = GRAVM_RS_FALSE,
				.state = GRAVM_RS_STATE_EXECUTED,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_veto_begin()
{
	static const test_step_t steps[] = {
		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = {
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_STACK_SIZE | FRESULT_CALL,
				.ret = GRAVM_RS_FALSE,
				.stack_size = 0,
				.state = GRAVM_RS_STATE_EXECUTED,
				.called = CALL_BEGIN } }
	};
	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_veto_edge_begin()
{
	static const test_step_t steps[] = {
		{	.skip = 1,
			.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_begin() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_BEGIN } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL,
				.ret = GRAVM_RS_FALSE,
				.state = GRAVM_RS_STATE_EXECUTED,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_veto_node_enter_1()
{
	static const test_step_t steps[] = {
		{	.skip = 2,
			.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call node_enter() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_ENTER } },

		{	.result = { /* call edge_next() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_NEXT } },
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_veto_node_enter_all_3()
{
	static const test_step_t steps[] = {
		{	.skip = 2,
			.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call node_enter() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_ENTER } },

		{	.result = { /* call edge_next() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_NEXT } },

		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call node_enter() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_ENTER } },

		{	.result = { /* call edge_next() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_NEXT } },

		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call node_enter() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_ENTER } },

		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_next() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_NEXT } },

		{	.result = { /* call edge_end() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_END } },
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_veto_node_run()
{
	static const test_step_t steps[] = {
		{	.skip = 3,
			.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call node_run() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ID,
				.id = 1,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_RUN } },

		{	.result = { /* call node_leave() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_LEAVE } },
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_throw_begin_error()
{
	static const test_step_t steps[] = {
		{	.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.ret = GRAVM_RS_THROW,
				.err = -123456 },
			.result = {
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL | FRESULT_ERRNO,
				.ret = GRAVM_RS_THROW,
				.err = -123456,
				.state = GRAVM_RS_STATE_EXECUTED_ERROR, /* difference to throw_value */
				.called = CALL_BEGIN } }
	};
	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_throw_begin_value()
{
	static const test_step_t steps[] = {
		{	.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.ret = GRAVM_RS_THROW,
				.err = 0 },
			.result = {
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL | FRESULT_ERRNO,
				.ret = GRAVM_RS_THROW,
				.err = 0,
				.state = GRAVM_RS_STATE_EXECUTED, /* difference to throw_error */
				.called = CALL_BEGIN } }
	};
	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_throw_edge_begin()
{
	static const test_step_t steps[] = {
		{	.skip = 1,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call edge_begin() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_BEGIN } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL,
				.ret = GRAVM_RS_THROW,
				.state = GRAVM_RS_STATE_EXECUTED_ERROR,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_throw_edge_next()
{
	static const test_step_t steps[] = {
		{ .skip = 2,
			.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call node_enter() */
				.flags = FRESULT_CALL,
				.called = CALL_NODE_ENTER } },
		{	.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call edge_next() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_NEXT } },

		{	.result = { /* call edge_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_CATCH } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL | FRESULT_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW,
				.state = GRAVM_RS_STATE_EXECUTED_ERROR,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_throw_node_enter()
{
	static const test_step_t steps[] = {
		{	.skip = 2,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call node_enter() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_ENTER } },

		{	.result = { /* call edge_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_CATCH } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL | FRESULT_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW,
				.state = GRAVM_RS_STATE_EXECUTED_ERROR,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_throw_node_run()
{
	static const test_step_t steps[] = {
		{	.skip = 3,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call node_run() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_RUN } },

		{	.result = { /* call node_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_CATCH } },

		{	.result = { /* call edge_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_CATCH } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL | FRESULT_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW,
				.state = GRAVM_RS_STATE_EXECUTED_ERROR,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_throw_node_leave()
{
	static const test_step_t steps[] = {
		{	.skip = 4,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call node_leave() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_LEAVE } },

		{	.result = { /* call edge_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_CATCH } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL | FRESULT_ERRNO,
				.ret = GRAVM_RS_THROW,
				.err = -123456,
				.state = GRAVM_RS_STATE_EXECUTED_ERROR,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_throw_edge_end()
{
	static const test_step_t steps[] = {
		{	.skip = 5,
			.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_next() */
				.flags = FRESULT_CALL,
				.called = CALL_EDGE_NEXT } },

		{	.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call edge_end() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_END } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL,
				.ret = GRAVM_RS_THROW,
				.state = GRAVM_RS_STATE_EXECUTED_ERROR,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_throw_end()
{
	static const test_step_t steps[] = {
		{	.skip = 5,
			.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_next() */
				.flags = FRESULT_CALL,
				.called = CALL_EDGE_NEXT } },

		{	.skip = 1,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL,
				.ret = GRAVM_RS_THROW,
				.state = GRAVM_RS_STATE_EXECUTED_ERROR,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_catch_edge()
{
	static const test_step_t steps[] = {
		{	.skip = 5,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call edge_next() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_NEXT } },

		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_TRUE },
			.result = { /* call edge_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_CATCH } },

		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_next() */
				.flags = FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.called = CALL_EDGE_NEXT } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_catch_node()
{
	static const test_step_t steps[] = {
		{	.skip = 3,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call node_run() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_RUN } },

		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_TRUE },
			.result = { /* call node_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_CATCH } },

		{	.result = { /* call edge_end() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_rethrow_edge()
{
	static const test_step_t steps[] = {
		{	.skip = 5,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call edge_next() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_NEXT } },

		{	.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -654321,
				.ret = GRAVM_RS_THROW },
			.result = { /* call edge_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_CATCH } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL | FRESULT_ERRNO,
				.ret = GRAVM_RS_THROW,
				.err = -654321,
				.state = GRAVM_RS_STATE_EXECUTED_ERROR,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test2_rethrow_node()
{
	static const test_step_t steps[] = {
		{	.skip = 3,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call node_run() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_RUN } },

		{	.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -654321,
				.ret = GRAVM_RS_THROW },
			.result = { /* call node_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_CATCH } },

		{	.result = { /* call edge_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE,
				.throw_code = -654321,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_CATCH } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

/******************************** TEST 3 **********************************/

static int test3_init()
{
	test_ctx.edges = test_3edge;
	test_ctx.n_edges = ARRAY_SIZE(test_3edge);

	test_self = gravm_runstack_new(&test_cb, -1, sizeof(test_frame_t));
	if(test_self == NULL)
		return -1;
	return 0;
}

static int test3_cleanup()
{
	gravm_runstack_destroy(test_self);
	return 0;
}

static void test3_full_run_0()
{
	static const test_step_t steps[] = {
		{	.result = { /* call begin() */
				.flags = FRESULT_STATE | FRESULT_STACK_SIZE | FRESULT_CALL,
				.stack_size = 1,
				.called = CALL_BEGIN,
				.state = GRAVM_RS_STATE_EXECUTING } },

		/* first edge */
		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_begin() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ID,
				.id = 2,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_BEGIN } },

		/* second edge */
		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_begin() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ID,
				.id = 1,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_BEGIN } },

		/* third edge */
		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_begin() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ID,
				.id = 0,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_BEGIN } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL,
				.ret = GRAVM_RS_FALSE,
				.state = GRAVM_RS_STATE_EXECUTED,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

/******************************** TEST 4 **********************************/

static int test4_init()
{
	test_ctx.edges = test_1targetedge;
	test_ctx.n_edges = ARRAY_SIZE(test_1targetedge);

	test_self = gravm_runstack_new(&test_cb, -1, sizeof(test_frame_t));
	if(test_self == NULL)
		return -1;
	return 0;
}

static int test4_cleanup()
{
	gravm_runstack_destroy(test_self);
	return 0;
}

static void test4_full_run_0()
{
	static const test_step_t steps[] = {
		{	.skip = 3,
			.result = { /* call edge_prepare() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_PREPARE } },

		{	.result = { /* call node_run() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_RUN } },

		{	.result = { /* call descend() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 2,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_DESCEND } },

		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_begin() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_BEGIN } },

		{	.result = { /* call ascend() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 2,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_ASCEND } },

		{	.result = { /* call edge_unprepare() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_UNPREPARE } },

		{	.result = { /* call node_leave() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_LEAVE } },

		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_next() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ITERATION,
				.iteration = 1,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_NEXT } },

		{	.result = { /* call edge_end() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_END } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL,
				.ret = GRAVM_RS_FALSE,
				.state = GRAVM_RS_STATE_EXECUTED,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_veto_node_run()
{
	static const test_step_t steps[] = {
		{	.skip = 4,
			.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call node_run() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_RUN } },

		{	.result = { /* call edge_unprepare() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_UNPREPARE } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_veto_descend()
{
	static const test_step_t steps[] = {
		{	.skip = 5,
			.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call descend() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 2,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_DESCEND } },

		{	.result = { /* call edge_unprepare() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_UNPREPARE } },
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_throw_node_enter_upper()
{
	static const test_step_t steps[] = {
		{	.skip = 2,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call node_enter() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_ENTER } },

		{	.result = { /* call edge_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE | FRESULT_ID,
				.id = 0,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_CATCH } },
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_throw_edge_prepare()
{
	static const test_step_t steps[] = {
		{	.skip = 3,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call edge_prepare() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_PREPARE } },

		{	.result = { /* call node_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE | FRESULT_ID,
				.id = 1,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_CATCH } },
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_throw_node_run_upper()
{
	static const test_step_t steps[] = {
		{	.skip = 4,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call node_run() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_RUN } },

		{	.result = { /* call edge_abort() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE | FRESULT_ID,
				.id = 1,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_ABORT } },

		{	.result = { /* call node_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_CATCH } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}


static void test4_throw_descend()
{
	static const test_step_t steps[] = {
		{	.skip = 5,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call descend() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 2,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_DESCEND } },

		{	.result = { /* call edge_abort() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE | FRESULT_ID | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.id = 1,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_ABORT } },

		{	.result = { /* call node_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_CATCH } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_throw_edge_begin()
{
	static const test_step_t steps[] = {
		{	.skip = 6,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call edge_begin() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ID | FRESULT_STACK_SIZE,
				.id = 1,
				.stack_size = 2,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_BEGIN } },

		{	.result = { /* call ascend() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_ASCEND } },

		{	.result = { /* call edge_abort() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE | FRESULT_ID | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.id = 1,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_ABORT } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_throw_edge_next()
{
	static const test_step_t steps[] = {
		{	.skip = 10,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call edge_next() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ID,
				.id = 1,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_NEXT } },

		{	.result = { /* call edge_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE | FRESULT_ID,
				.id = 1,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_CATCH } },

		{	.result = { /* call ascend() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_ASCEND } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_throw_node_enter()
{
	static const test_step_t steps[] = {
		{	.skip = 7,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call node_enter() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ID,
				.id = 2,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_ENTER } },

		{	.result = { /* call edge_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE | FRESULT_ID,
				.id = 1,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_CATCH } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_throw_node_run()
{
	static const test_step_t steps[] = {
		{	.skip = 8,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call node_run() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ID,
				.id = 2,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_RUN } },

		{	.result = { /* call node_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE | FRESULT_ID,
				.id = 2,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_CATCH } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_throw_node_leave()
{
	static const test_step_t steps[] = {
		{	.skip = 9,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call node_leave() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ID,
				.id = 2,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_LEAVE } },

		{	.result = { /* call edge_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE | FRESULT_ID,
				.id = 1,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_CATCH } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_throw_edge_end()
{
	static const test_step_t steps[] = {
		{	.skip = 10,
			.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_next()*/
				.flags = FRESULT_CALL,
				.called = CALL_EDGE_NEXT } },

		{	.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call edge_end() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ID,
				.id = 1,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_END } },

		{	.result = { /* call ascend() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_ASCEND } },

		{	.result = { /* call edge_abort() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE | FRESULT_ID,
				.id = 1,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_ABORT } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_throw_ascend()
{
	static const test_step_t steps[] = {
		{	.skip = 10,
			.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_next()*/
				.flags = FRESULT_CALL,
				.called = CALL_EDGE_NEXT } },

		{	.skip = 1,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call ascend() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_ASCEND } },

		{	.result = { /* call edge_abort() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE | FRESULT_ID,
				.id = 1,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_ABORT } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_throw_edge_unprepare()
{
	static const test_step_t steps[] = {
		{	.skip = 10,
			.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_next()*/
				.flags = FRESULT_CALL,
				.called = CALL_EDGE_NEXT } },

		{	.skip = 2,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call edge_unprepare() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_UNPREPARE } },

		{	.result = { /* call node_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE | FRESULT_ID,
				.id = 1,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_CATCH } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test4_throw_node_leave_upper()
{
	static const test_step_t steps[] = {
		{	.skip = 10,
			.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_next()*/
				.flags = FRESULT_CALL,
				.called = CALL_EDGE_NEXT } },

		{	.skip = 3,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.err = -123456,
				.ret = GRAVM_RS_THROW },
			.result = { /* call node_leave() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_NODE_LEAVE } },

		{	.result = { /* call edge_catch() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_THROW_CODE | FRESULT_ID,
				.id = 0,
				.throw_code = -123456,
				.state = GRAVM_RS_STATE_THROWING,
				.called = CALL_EDGE_CATCH } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

/******************************** TEST 5 **********************************/

static int test5_init()
{
	test_ctx.edges = test_1pretargetedge;
	test_ctx.n_edges = ARRAY_SIZE(test_1pretargetedge);

	test_self = gravm_runstack_new(&test_cb, -1, sizeof(test_frame_t));
	if(test_self == NULL)
		return -1;
	return 0;
}

static int test5_cleanup()
{
	gravm_runstack_destroy(test_self);
	return 0;
}

static void test5_full_run_0()
{
	static const test_step_t steps[] = {
		{	.skip = 3,
			.result = { /* call edge_prepare() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_PREPARE } },

		{	.result = { /* call descend() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 2,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_DESCEND } },

		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_begin() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_BEGIN } },

		{	.result = { /* call ascend() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 2,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_ASCEND } },

		{	.result = { /* call node_run() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_RUN } },

		{	.result = { /* call edge_unprepare() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_STACK_SIZE,
				.stack_size = 1,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_UNPREPARE } },

		{	.result = { /* call node_leave() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_NODE_LEAVE } },

		{	.call = {
				.flags = FCALL_RET,
				.ret = GRAVM_RS_FALSE },
			.result = { /* call edge_next() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_ITERATION,
				.iteration = 1,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_NEXT } },

		{	.result = { /* call edge_end() */
				.flags = FRESULT_STATE | FRESULT_CALL,
				.state = GRAVM_RS_STATE_EXECUTING,
				.called = CALL_EDGE_END } },

		{	.result = { /* call end() */
				.flags = FRESULT_RET | FRESULT_STATE | FRESULT_CALL,
				.ret = GRAVM_RS_FALSE,
				.state = GRAVM_RS_STATE_EXECUTED,
				.called = CALL_END } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

static void test5_fatal_errors()
{
	static const test_step_t steps[] = {
		{	.skip = 3,
			.call = {
				.flags = FCALL_RET | FCALL_ERRNO,
				.ret = GRAVM_RS_FATAL,
				.err = -123456 },
			.result = { /* call edge_prepare() */
				.flags = FRESULT_STATE | FRESULT_CALL | FRESULT_RET | FRESULT_ERRNO,
				.state = GRAVM_RS_STATE_EXECUTED_ERROR,
				.ret = GRAVM_RS_FATAL,
				.err = -123456,
				.called = CALL_EDGE_PREPARE } }
	};

	test_ctx.steps = steps;
	test_ctx.n_steps = ARRAY_SIZE(steps);
	test_run_steps();
}

/******************************** INITIALIZE **********************************/

int gravmtest_runstack()
{
	CU_pSuite suite;
	CU_pTest test;

	memset(&test_cb, 0, sizeof(test_cb));

	test_cb.init = cb_test_init;
	test_cb.destroy = cb_test_destroy;
	test_cb.structure = cb_test_structure;
	test_cb.begin = cb_test_begin;
	test_cb.end = cb_test_end;
	test_cb.descend = cb_test_descend;
	test_cb.ascend = cb_test_ascend;
	test_cb.edge_begin = cb_test_edge_begin;
	test_cb.edge_next = cb_test_edge_next;
	test_cb.edge_end = cb_test_edge_end;
	test_cb.edge_prepare = cb_test_edge_prepare;
	test_cb.edge_unprepare = cb_test_edge_unprepare;
	test_cb.edge_abort = cb_test_edge_abort;
	test_cb.edge_catch = cb_test_edge_catch;
	test_cb.node_enter = cb_test_node_enter;
	test_cb.node_run = cb_test_node_run;
	test_cb.node_leave = cb_test_node_leave;
	test_cb.node_catch = cb_test_node_catch;

	BEGIN_SUITE("RunStack Preparation", test1_init, test1_cleanup);
		ADD_TEST("no edges", test1_no_edges);
		ADD_TEST("no root edges", test1_no_root_edges);
		ADD_TEST("multiple same edges", test1_multiple_same);
		ADD_TEST("root node as target", test1_root_target);
	END_SUITE;
	BEGIN_SUITE("RunStack Single Root Edge", test2_init, test2_cleanup);
		ADD_TEST("full run for zero iterations", test2_full_run_0);
		ADD_TEST("full run for single iteration", test2_full_run_1);
		ADD_TEST("full run for three iterations", test2_full_run_3);
		ADD_TEST("veto: begin", test2_veto_begin);
		ADD_TEST("veto: edge begin", test2_veto_edge_begin);
		ADD_TEST("veto: node enter in single iteration", test2_veto_node_enter_1);
		ADD_TEST("veto: node enter in all of three iterations", test2_veto_node_enter_all_3);
		ADD_TEST("veto: node run", test2_veto_node_run);
		ADD_TEST("throw: begin (errno != 0)", test2_throw_begin_error);
		ADD_TEST("throw: begin (errno == 0)", test2_throw_begin_value);
		ADD_TEST("throw: edge begin", test2_throw_edge_begin);
		ADD_TEST("throw: edge next", test2_throw_edge_next);
		ADD_TEST("throw: node enter", test2_throw_node_enter);
		ADD_TEST("throw: node run", test2_throw_node_run);
		ADD_TEST("throw: node leave", test2_throw_node_leave);
		ADD_TEST("throw: edge end", test2_throw_edge_end);
		ADD_TEST("throw: end", test2_throw_end);
		ADD_TEST("catch: edge", test2_catch_edge);
		ADD_TEST("catch: node", test2_catch_node);
		ADD_TEST("rethrow: edge", test2_rethrow_edge);
		ADD_TEST("rethrow: node", test2_rethrow_node);
	END_SUITE;
	BEGIN_SUITE("RunStack Three Root Edges", test3_init, test3_cleanup);
		ADD_TEST("full run, zero iterations at each edge", test3_full_run_0);
	END_SUITE;
	BEGIN_SUITE("RunStack Single Node Edge (Post-Outgoing)", test4_init, test4_cleanup);
		ADD_TEST("full run, zero iterations", test4_full_run_0);
		ADD_TEST("veto: node run", test4_veto_node_run);
		ADD_TEST("veto: descend", test4_veto_descend);
		ADD_TEST("throw: node enter (first 'upper' node)", test4_throw_node_enter_upper);
		ADD_TEST("throw: edge prepare", test4_throw_edge_prepare);
		ADD_TEST("throw: node run (first 'upper' node)", test4_throw_node_run_upper);
		ADD_TEST("throw: descend", test4_throw_descend); /* NOTE: in comparison to test 2, the tests between descend() and ascend() run with stack_size == 2, i.e. not on the root edge but on the outgoing edge of the first node */
		ADD_TEST("throw: edge begin", test4_throw_edge_begin);
		ADD_TEST("throw: edge next", test4_throw_edge_next);
		ADD_TEST("throw: node enter", test4_throw_node_enter);
		ADD_TEST("throw: node run", test4_throw_node_run);
		ADD_TEST("throw: node leave", test4_throw_node_leave);
		ADD_TEST("throw: edge end", test4_throw_edge_end);
		ADD_TEST("throw: ascend", test4_throw_ascend);
		ADD_TEST("throw: edge unprepare", test4_throw_edge_unprepare);
		ADD_TEST("throw: node leave (first 'upper' node)", test4_throw_node_leave_upper);
	END_SUITE;
	BEGIN_SUITE("RunStack Single Node Edge (Pre-Outgoing)", test5_init, test5_cleanup);
		ADD_TEST("full run, zero iterations", test5_full_run_0);
		ADD_TEST("fatal errors", test5_fatal_errors);
	END_SUITE;

	return 0;
}

