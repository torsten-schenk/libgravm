#pragma once

#include <CUnit/Basic.h>

#define ADD_TEST(NAME, FN) \
	do {\
		test = CU_add_test(suite, NAME, FN); \
		if(test == NULL) \
			return CU_get_error(); \
	} while(false)

#define BEGIN_SUITE(NAME, INIT, CLEANUP) \
	do { \
		suite = CU_add_suite(NAME, INIT, CLEANUP); \
		if(suite == NULL) \
			return CU_get_error(); \
	} while(false)

#define END_SUITE

