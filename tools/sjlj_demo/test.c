#include "exceptions.h"

static void test_simple (gboolean have_exception) {
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
		mono_try_stack_throw (&stack, 100);
	}

	if (no_exception)
		mono_try_stack_exit (&stack);

	return;
}

int main(void)
{
	fprintf (stderr, "Throwing test:\n");
	test_simple (TRUE);
	fprintf (stderr, "Non-throwing test:\n");
	test_simple (FALSE);
}
