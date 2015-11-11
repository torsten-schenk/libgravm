#include <stdbool.h>
#include <CUnit/Basic.h>

#include <gravm/runstack.h>

int gravmtest_runstack();

static int sbcb_init(
		void *data)
{
	return 1;
}

static int sbcb_structure(
		void *data,
		int edge,
		gravm_runstack_edgedef_t *def)
{
	def->source = GRAVM_RS_ROOT;
	def->target = 0;
	def->priority = 0;
	return 0;
}

static int sbcb_edge_next(
		void *data,
		int iteration,
		int id,
		void *frame)
{
	return iteration < 1000000;
}

static int simplebench()
{
	gravm_runstack_callback_t cb;
	gravm_runstack_t *rs;
	int ret;

	memset(&cb, 0, sizeof(cb));
	cb.init = sbcb_init;
	cb.structure = sbcb_structure;
	cb.edge_next = sbcb_edge_next;

	rs = gravm_runstack_new(&cb, -1, 0);
	if(rs == NULL) {
		printf("Error creating new runstack instance\n");
		return -1;
	}
	ret = gravm_runstack_prepare(rs, NULL);
	if(ret != 0) {
		printf("Error prepating runstack\n");
		return -1;
	}
	for(;;) {
		ret = gravm_runstack_step(rs);
		if(ret == GRAVM_RS_FALSE)
			return 0;
		else if(ret != GRAVM_RS_TRUE) {
			printf("Error execution runstack\n");
			return -1;
		}
	}
}

int main(
		int argn,
		const char *const *argv)
{
	int ret;
	int i;
	bool run_bench = false;

	for(i = 1; i < argn; i++) {
		if(strcmp(argv[i], "-b") == 0)
			run_bench = true;
		else {
			printf("invalid argument %d: '%s'\n", i, argv[i]);
			return -1;
		}
	}
	
	if(run_bench)
		ret = simplebench();
	else {
		ret = CU_initialize_registry();
		if(ret != CUE_SUCCESS) {
			printf("Error initializing CUnit.\n");
			CU_cleanup_registry();
			return ret;
		}

		ret = gravmtest_runstack();
		if(ret != 0) {
			CU_cleanup_registry();
			return ret;
		}

		CU_basic_set_mode(CU_BRM_VERBOSE);
		CU_basic_run_tests();
		ret = CU_get_error();
		CU_cleanup_registry();
	}
	return ret;
}

