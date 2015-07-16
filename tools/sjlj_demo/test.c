#include "exceptions.h"

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

static void
test_cil_table (void)
{
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
}

int main(void)
{
	/*fprintf (stderr, "Throwing test:\n");*/
	/*test_simple (TRUE);*/
	/*fprintf (stderr, "Non-throwing test:\n");*/
	/*test_simple (FALSE);*/

	fprintf (stderr, "CIL-throwing test:\n");
	test_cil_table ();
}
