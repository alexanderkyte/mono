#include "exceptions.h"

static void
test_simple (gboolean have_exception) {
	MonoMethodHeader fixture;
	// fill in fixture
	MonoTryStack *stack = NULL;
	gboolean no_exception = TRUE;

	/*volatile*/
	MonoJumpBuffer value;
	int status = setjmp (value.buf);
	fprintf (stderr, "Returned from try entry with %d as status\n", status);
	switch (status) {
		case 0:
			mono_try_stack_push (&stack, value);
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

	for (intptr_t i=0; i < fixture.num_clauses; i++)
	{
		fixture.clauses[i].try_offset = try_start;
		fixture.clauses[i].try_len = 5;
		fixture.clauses[i].handler_offset = try_start + 5;
		fixture.clauses[i].handler_len = 5;

		expected [i] = (gpointer) i;
		fixture.clauses[i].data.catch_class = (MonoClass *)expected [i];

		try_start += 10;
	}

	g_assert (fixture.clauses);

	for (int i=0; i < fixture.num_clauses; i++) {
		mono_emit_try_enter (NULL, i, &fixture);
		mono_throw ((MonoException *)expected [i]);
	}
}

int main(void)
{
	fprintf (stderr, "Throwing test:\n");
	test_simple (TRUE);
	fprintf (stderr, "Non-throwing test:\n");
	test_simple (FALSE);

	fprintf (stderr, "CIL-throwing test:\n");
	test_cil_table ();
}
