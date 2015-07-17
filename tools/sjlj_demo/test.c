#include "exceptions.h"

caught_exception_func test_caught_exception_func;
unhandled_exception_func test_unhandled_exception_func;

static void
test_simple (gboolean have_exception) {
	MonoTryStack *stack = NULL;
	gboolean no_exception = TRUE;

	/*volatile*/
	MonoJumpBuffer value;
	mono_push_try_handler (&stack, &value, NULL, 0);
	int status = setjmp (value.buf);

	fprintf (stderr, "Returned from try entry with %d as status\n", status);
	switch (status) {
		case 0:
			fprintf (stderr, "Nonexceptional branch\n");
			break;
		default:
			no_exception = FALSE;
			fprintf (stderr, "Exceptional branch value %d\n", status);
	}

	if (have_exception && no_exception) {
		fprintf (stderr, "Throwing\n");
		mono_try_stack_rethrow (&stack, (MonoException *)100);
	}

	if (no_exception)
		mono_try_stack_exit (&stack);

	return;
}

#define NUM_TEST_EXCEPTIONS 100

void __attribute__((noreturn)) 
disjoint_exception_caught_func (MonoException *exc, MonoExceptionClause *clause)
{
	MonoJumpBuffer *jmp = (MonoJumpBuffer *)exc;
	jmp->ref_count = 999;
	longjmp (jmp->buf, MonoJumpLand);
}

static void
test_disjoint_exceptions (void)
{
	test_caught_exception_func = &disjoint_exception_caught_func;

	MonoMethodHeader fixture;
	gpointer expected [NUM_TEST_EXCEPTIONS];
	MonoExceptionClause clauses [NUM_TEST_EXCEPTIONS];
	fixture.clauses = clauses;
	fixture.num_clauses = NUM_TEST_EXCEPTIONS;

	intptr_t try_start = 0;

	fprintf (stderr, "Setting up data\n");
	for (intptr_t i=0; i < fixture.num_clauses; i++)
	{
		fixture.clauses[i].try_offset = try_start;
		fixture.clauses[i].try_len = 5;
		fixture.clauses[i].handler_offset = try_start + 5;
		fixture.clauses[i].handler_len = 5;

		// Because we can't throw NULL/0 and have it be
		// recognized as failure
		expected [i] = g_malloc0 (sizeof(MonoJumpBuffer));
		fixture.clauses[i].data.catch_class = (MonoClass *)expected [i];

		try_start += 10;
	}

	g_assert (fixture.clauses);

	fprintf (stderr, "Executing throws\n");
	for (int i=0; i < fixture.num_clauses; i++) {
		fprintf (stderr, "SetJmp first before\n");
		intptr_t loop_state = setjmp (((MonoJumpBuffer *)expected[i])->buf);
		fprintf (stderr, "SetJmp first done\n");

		if (loop_state != MonoJumpReturnDirect) {
			g_assert (((MonoJumpBuffer **)expected) [i]->ref_count == 999);
			g_free (expected [i]);
			continue;
		}

		fprintf (stderr, "Enter try %p\n", (MonoException *)expected [i]);
		mono_enter_try (NULL, fixture.clauses[i].try_offset, &fixture);
		fprintf (stderr, "Throwing exception %p\n", (MonoException *)expected [i]);
		mono_throw ((MonoException *)expected [i]);
	}


	test_caught_exception_func = 0;
}

// Make exceptions that nest
// throw
// check that all frames visited in right order

typedef struct TracedException {
	GList *seen_clauses;
	MonoJumpBuffer jmp;
} TracedException;

void __attribute__((noreturn)) 
nested_exception_caught_func (MonoException *exc, MonoExceptionClause *clause)
{
	g_assert_not_reached ();
}

void __attribute__((noreturn)) 
return_on_unhandled (MonoException *exc)
{
	fprintf (stderr, "Unhandled exc handler jump\n");
	TracedException *te = (TracedException *)exc;
	longjmp (te->jmp.buf, MonoJumpLand);
}

#define NUM_OUTER_TEST_EXCEPTIONS 10
#define NUM_INNER_TEST_EXCEPTIONS 4

static void
test_nested_exceptions (void)
{
	const size_t handler_size = 5;
	const size_t inner_region_size = 5;

	test_caught_exception_func = &nested_exception_caught_func;
	test_unhandled_exception_func = &return_on_unhandled;

	MonoMethodHeader fixture;
	MonoExceptionClause clauses [NUM_TEST_EXCEPTIONS];
	fixture.clauses = clauses;
	fixture.num_clauses = NUM_OUTER_TEST_EXCEPTIONS * NUM_INNER_TEST_EXCEPTIONS;

	size_t try_start = 0;
	size_t bottom = 0;

	fprintf (stderr, "Setting up data\n");
	while (bottom < fixture.num_clauses) {
		fixture.clauses[bottom].try_offset = try_start;

		size_t outer = bottom;
		bottom++;

		size_t accum = 0;

		for (size_t i=0; i < NUM_INNER_TEST_EXCEPTIONS; i++) {
			fixture.clauses [bottom].try_offset = fixture.clauses[outer].try_offset + accum;
			fixture.clauses [bottom].try_len = inner_region_size;
			fixture.clauses [bottom].handler_offset = fixture.clauses [bottom].try_offset + fixture.clauses [bottom].try_len;
			fixture.clauses [bottom].handler_len = handler_size;

			// UNUSED
			fixture.clauses[bottom].data.catch_class = NULL;

			accum += handler_size + inner_region_size;

			bottom++;
		}

		fixture.clauses[outer].try_len = accum;
		fixture.clauses[outer].handler_offset = fixture.clauses[outer].try_offset + fixture.clauses[outer].try_len; 
		fixture.clauses[outer].handler_len = handler_size;
		// UNUSED
		fixture.clauses[outer].data.catch_class = NULL;

		try_start += accum + handler_size;
	}

	for (int i = 0; i < fixture.num_clauses; i++) {
		MonoExceptionClause *clause = &fixture.clauses[i];
		fprintf (stderr, "Clause %d: try start/end(%d to %d) and handler start/end(%d to %d)\n",
			i, clause->try_offset, clause->try_offset + clause->try_len, clause->handler_offset, clause->handler_offset + clause->handler_len
		);
	}
	g_assert (fixture.clauses);

	fprintf (stderr, "Executing throws\n");
	for (int i=0; i < fixture.num_clauses; i++) {
		TracedException log;

		fprintf (stderr, "SetJmp first before\n");
		intptr_t loop_state = setjmp (log.jmp.buf);
		fprintf (stderr, "SetJmp first done\n");

		if (loop_state != MonoJumpReturnDirect) {
			fprintf (stderr, "Validating\n");
			/*int i=0;*/
			/*MonoExceptionClause *prev = NULL;*/

			// This won't work with a sane control flow, I'll just inspect print output.
			/*for (GList *curr = log.seen_clauses; curr; curr = curr->next) {*/
				/*MonoExceptionClause *clause = (MonoExceptionClause *)curr->data;*/
				/*fprintf (stderr, "Unwind step %d: Visited handler with try start/end(%d:%d) and handler start/end(%d:%d)\n",*/
					/*i++, clause->try_offset, clause->try_offset + clause->try_len, clause->handler_offset, clause->handler_offset + clause->handler_len*/
				/*);*/

				/*if (prev) {*/
					/*g_assert (prev->try_offset == clause->try_offset);*/
					/*g_assert ((prev->handler_offset + prev->handler_offset) < (clause->handler_offset + clause->handler_offset));*/
				/*}*/
				/*prev = clause;*/
			/*}*/
			continue;
		}

		fprintf (stderr, "Entering try\n");
		mono_enter_try (NULL, fixture.clauses[i].try_offset, &fixture);
		fprintf (stderr, "Throwing exception %p\n", &log);
		mono_throw ((MonoException *)&log);
	}

	test_caught_exception_func = NULL;
	test_unhandled_exception_func = NULL;
}

#define NUM_MUTUAL_CASES 30

static void
test_mutual_exceptions (void)
{
	test_unhandled_exception_func = &return_on_unhandled;

	MonoMethodHeader fixture;
	MonoExceptionClause clauses [NUM_TEST_EXCEPTIONS];
	fixture.clauses = clauses;
	fixture.num_clauses = NUM_MUTUAL_CASES;

	size_t try_offset = 0;
	size_t try_len = 10;

	size_t bottom = try_offset + try_len;

	for (intptr_t i=0; i < fixture.num_clauses; i++)
	{
		fixture.clauses[i].try_offset = try_offset;
		fixture.clauses[i].try_len = try_len;
		fixture.clauses[i].handler_offset = bottom;
		fixture.clauses[i].handler_len = 5;
		fixture.clauses[i].data.catch_class = NULL;
		bottom += 5;
	}

	for (int i = 0; i < fixture.num_clauses; i++) {
		MonoExceptionClause *clause = &fixture.clauses[i];
		fprintf (stderr, "Clause %d: try start/end(%d to %d) and handler start/end(%d to %d)\n",
			i, clause->try_offset, clause->try_offset + clause->try_len, clause->handler_offset, clause->handler_offset + clause->handler_len
		);
	}
	g_assert (fixture.clauses);

	fprintf (stderr, "Executing throws\n");
	for (int i=0; i < fixture.num_clauses; i++) {
		TracedException log;

		fprintf (stderr, "SetJmp first before\n");
		intptr_t loop_state = setjmp (log.jmp.buf);
		fprintf (stderr, "SetJmp first done\n");

		if (loop_state != MonoJumpReturnDirect) {
			// Since all share same base.
			break;
		}

		fprintf (stderr, "Entering try\n");
		mono_enter_try (NULL, fixture.clauses[i].try_offset, &fixture);
		fprintf (stderr, "Throwing exception %p\n", &log);
		mono_throw ((MonoExceptionClause *)&log);
	}

	test_caught_exception_func = NULL;
	test_unhandled_exception_func = NULL;
}

int main(void)
{
	/*fprintf (stderr, "Throwing test:\n");*/
	/*test_simple (TRUE);*/
	/*fprintf (stderr, "Non-throwing test:\n");*/
	/*test_simple (FALSE);*/

	/*fprintf (stderr, "disjoint test:\n");*/
	/*test_disjoint_exceptions ();*/
	/*fprintf (stderr, "disjoint test done \n");*/

	/*fprintf (stderr, "nested test:\n\n\n");*/
	/*test_nested_exceptions ();*/
	/*fprintf (stderr, "nested test done \n\n\n");*/

	fprintf (stderr, "mutual test:\n\n\n");
	test_mutual_exceptions ();
	fprintf (stderr, "mutual test done \n\n\n");
}

