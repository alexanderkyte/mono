#include "exceptions.h"


int main(void)
{
	MonoMethodHeader fixture;
	// fill in fixture
	MonoTryStack *stack = NULL;

	int status =  mono_try_stack_enter (&stack, NULL);
	switch (status) {
		case 0:
			fprintf (stderr, "Nonexceptional branch\n");
		default:
			fprintf (stderr, "Exceptional branch\n");
	}
	mono_try_stack_exit (&stack);
	
}
