#include "exceptions.h"

caught_exception_func test_caught_exception_func;

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
	TracedException *te = (TracedException *)exc;
	GList **seen = &(te->seen_clauses);
	*seen = g_list_prepend(*seen, clause);
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

	MonoMethodHeader fixture;
	gpointer expected [NUM_TEST_EXCEPTIONS];
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
			fixture.clauses [bottom].try_offset = try_start + accum;
			fixture.clauses [bottom].try_len = inner_region_size;
			fixture.clauses [bottom].handler_offset = fixture.clauses [bottom].try_offset + fixture.clauses [bottom].try_len;
			fixture.clauses [bottom].handler_len = handler_size;

			accum += handler_size + inner_region_size;

			bottom++;
		}

		fixture.clauses[outer].try_len = accum;
		fixture.clauses[outer].handler_offset = fixture.clauses[outer].try_offset + fixture.clauses[outer].try_len; 
		fixture.clauses[outer].handler_len = handler_size;

		try_start += accum + handler_size;
	}

	g_assert (fixture.clauses);

	fprintf (stderr, "Executing throws\n");
	for (int i=0; i < fixture.num_clauses; i++) {
		TracedException log;

		fprintf (stderr, "SetJmp first before\n");
		intptr_t loop_state = setjmp (log.jmp.buf);
		fprintf (stderr, "SetJmp first done\n");

		if (loop_state != MonoJumpReturnDirect) {
			int i=0;
			MonoExceptionClause *prev = NULL;

			for (GList *curr = log.seen_clauses; curr; curr = curr->next) {
				MonoExceptionClause *clause = (MonoExceptionClause *)curr->data;
				fprintf (stderr, "Unwind step %d: Visited handler with try start/end(%d:%d) and handler start/end(%d:%d)",
					i++, clause->try_offset, clause->try_len, clause->handler_offset, clause->handler_len
				);

				if (prev) {
					g_assert (prev->try_offset == clause->try_offset);
					g_assert ((prev->handler_offset + prev->handler_offset) < (clause->handler_offset + clause->handler_offset));
				}
				prev = clause;
			}
			continue;
		}

		fprintf (stderr, "Enter try %p\n", (MonoException *)expected [i]);
		mono_enter_try (NULL, fixture.clauses[i].try_offset, &fixture);
		fprintf (stderr, "Throwing exception %p\n", &log);
		mono_throw ((MonoException *)&log);
	}

	test_caught_exception_func = NULL;
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

	fprintf (stderr, "nested test:\n\n\n");
	test_nested_exceptions ();
	fprintf (stderr, "nested test done \n\n\n");
}


