#pragma once

#include <stdbool.h>
#include <stdio.h>

enum {
	GRAVM_RS_ROOT = -1
};

enum {
	GRAVM_RS_TRUE = 1,
	GRAVM_RS_FALSE = 0,
	GRAVM_RS_SUCCESS = 0,
	GRAVM_RS_THROW = -1, /* switch to throwing mode and ascend until catch is found */
	GRAVM_RS_FATAL = -2, /* immediately abort execution and return error code currently residing in errno */
	GRAVM_RS_SUSPENDED = -3,
	GRAVM_RS_UNKNOWN = -4
};

enum {
	GRAVM_RS_STATE_CREATED,
	GRAVM_RS_STATE_PREPARED,
	GRAVM_RS_STATE_EXECUTING,
	GRAVM_RS_STATE_THROWING,
	GRAVM_RS_STATE_EXECUTED_ERROR,
	GRAVM_RS_STATE_EXECUTED
};

enum {
	GRAVM_RS_IP_DESCEND,
	GRAVM_RS_IP_EDGE_BEGIN,
	GRAVM_RS_IP_EDGE_NEXT,
	GRAVM_RS_IP_NODE_ENTER,
	GRAVM_RS_IP_BEGIN_EDGE_PREPARE,
	GRAVM_RS_IP_LOOP_EDGE_PREPARE,
	GRAVM_RS_IP_BEGIN_OUTGOING_PRE,
	GRAVM_RS_IP_LOOP_OUTGOING_PRE,
	GRAVM_RS_IP_NODE_RUN,
	GRAVM_RS_IP_BEGIN_OUTGOING_POST,
	GRAVM_RS_IP_LOOP_OUTGOING_POST,
	GRAVM_RS_IP_BEGIN_EDGE_UNPREPARE,
	GRAVM_RS_IP_LOOP_EDGE_UNPREPARE,
	GRAVM_RS_IP_NODE_LEAVE,
	GRAVM_RS_IP_EDGE_END,
	GRAVM_RS_IP_ASCEND,
	GRAVM_RS_IP_POP
};

typedef struct gravm_runstack gravm_runstack_t;
typedef struct gravm_runstack_callback gravm_runstack_callback_t;

typedef struct {
	int source;
	int target;
	int priority;
} gravm_runstack_edgedef_t;

struct gravm_runstack_callback {
	int (*init)(void *user); /* returns: number of edges */
	void (*clear)(void *user);
	void (*destroy)(void *user);
	int (*structure)(void *user, int edge, gravm_runstack_edgedef_t *def);

	int (*begin)(void *user);
	int (*end)(void *user);
	/* descend/ascend: won't be called for root edges */
	int (*descend)(void *user, void *parent_framedata, void *child_framedata);
	int (*ascend)(void *user, bool throwing, int err, void *parent_framedata, void *child_framedata);

	/* difference abort/catch: abort is called if catch is not yet possible
	 * (i.e. the exception occured "above" the catch)
	 * --> abort is a passive notification, catch is an active way to manipulate the error
	 * in case of edge: catch is possible for next() method only,
	 * therefore abort() is called if an exception occurs
	 * after prepare() and before unprepare()
	 * and not after begin() and before end()
	 * NOTE: root edges won't be prepare()d, therefore abort() is never called on them */
	int (*edge_prepare)(void *user, int id, void *framedata);
	int (*edge_unprepare)(void *user, int id, void *framedata);
	int (*edge_begin)(void *user, int id, void *framedata);
	int (*edge_next)(void *user, int iteration, int id, void *framedata);
	int (*edge_end)(void *user, int id, void *framedata);
	int (*edge_abort)(void *user, int err, int id, void *framedata);
	int (*edge_catch)(void *user, int err, int id, void *framedata); /* only method that may return THROW during throwing; replaces the error code with the one stored in errno after call; other methods will produce a fatal error when returning THROW during throwing */

	int (*node_enter)(void *user, int id, void *framedata);
	int (*node_leave)(void *user, int id, void *framedata);
	int (*node_run)(void *user, int id, void *framedata);
	int (*node_catch)(void *user, int err, int id, void *framedata); /* only method that may return THROW during throwing; replaces the error code with the one stored in errno after call; other methods will produce a fatal error when returning THROW during throwing */
};

/* sets errno in case NULL is returned */
gravm_runstack_t *gravm_runstack_new(
		const gravm_runstack_callback_t *cb,
		int max_stack_size,
		int framedata_size);

void gravm_runstack_destroy(
		gravm_runstack_t *self);

void *gravm_runstack_data(
		gravm_runstack_t *self);

/* create the internal structure using the callback.structure callbacks */
int gravm_runstack_prepare(
		gravm_runstack_t *self,
		void *user);

/* call again after vm has been suspended */
int gravm_runstack_run(
		gravm_runstack_t *self);

int gravm_runstack_suspend(
		gravm_runstack_t *self);

/* returns GRAVM_RS_TRUE if more steps can be performed,
 * GRAVM_RS_FALSE if end has been reached,
 * GRAVM_RS_FATAL on error */
int gravm_runstack_step(
		gravm_runstack_t *self);

int gravm_runstack_debug_state(
		gravm_runstack_t *self);

int gravm_runstack_debug_throw_code(
		gravm_runstack_t *self);

int gravm_runstack_debug_ip(
		gravm_runstack_t *self);

int gravm_runstack_debug_stack_size(
		gravm_runstack_t *self);

int gravm_runstack_debug_stack(
		gravm_runstack_t *self,
		bool (*frame)(void *user, int edge, int ip, void *framedata));

const char *gravm_runstack_ip_name(
		int ip);

/* print functions may be NULL; in that case, the id is printed */
void gravm_runstack_dump(
		gravm_runstack_t *self,
		void (*print_edge)(FILE *f, void *user, int id),
		void (*print_node)(FILE *f, void *user, int id));

