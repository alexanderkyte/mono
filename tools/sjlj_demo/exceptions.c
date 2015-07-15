#include "exceptions.h"

void
mono_try_stack_pop (MonoTryStack **stack)
{
	g_assert (*stack);
	if ((*stack)->bottom == 0) {
		MonoTryStack *old = *stack;
		*stack = (*stack)->next;
		g_free (old);
	} else {
		(*stack)->bottom--;
	}
}

MonoJumpBuffer *
mono_try_stack_peek (MonoTryStack **stack)
{
	g_assert (*stack);
	if ((*stack)->bottom == 0) {
		if ((*stack)->next)
			return &(*stack)->next->buffers [TRY_STACKLET_SIZE - 1];
		else
			return NULL;
	} else {
		return &(*stack)->buffers [(*stack)->bottom - 1];
	}
}

void 
mono_try_stack_push (MonoTryStack **stack, MonoJumpBuffer value)
{
	if (!(*stack) || (*stack)->bottom == TRY_STACKLET_SIZE) {
		MonoTryStack *old = *stack;
		MonoTryStack *top = g_malloc0 (sizeof(MonoTryStack));
		top->buffers [0] = value;
		top->bottom = 1;

		top->next = old;
		*stack = top;
	} else {
		(*stack)->buffers [(*stack)->bottom] = value;
		(*stack)->bottom = (*stack)->bottom + 1;
	}
}

void __attribute__((noreturn))
mono_try_stack_rethrow (MonoTryStack **stack, MonoException *exc)
{
	g_assert (*stack);
	MonoJumpBuffer *container = mono_try_stack_peek (stack);
	g_assert (container);

	intptr_t throw_data = (intptr_t)exc;
	longjmp (container->buf, throw_data);
}

void 
mono_try_stack_exit (MonoTryStack **stack)
{
	g_assert (*stack);
	mono_try_stack_pop (stack);
}

// Bury stack in jit tls info later
static MonoTryStack *global_stack = NULL;

void
mono_throw (MonoException *exc)
{
	mono_try_stack_rethrow (&global_stack, exc);
}

void
mono_jump_try_handler (MonoExceptionClause **exceptions, int count)
{
	MonoJumpBuffer *value = g_malloc0 (sizeof (MonoJumpBuffer));
	value->num_clauses = count;
	value->clauses = exceptions;

	intptr_t exc = setjmp (value->buf);

	if (exc == 0) {
		mono_try_stack_push (&global_stack, *value);
	} else {
		// Nothing allocated on the stack above this can be accessed below
		// That includes arguments
		// To statically enforce this I'm making a new function call
		mono_handle_exception_jump ((MonoException *)exc);
	}
}

void 
mono_handle_exception_jump (MonoException *exc)
{
	MonoJumpBuffer *curr = mono_try_stack_peek (&global_stack);

	gboolean matched = FALSE;

	for (int i=0; i < curr->num_clauses; i++) {
		MonoExceptionClause *clause = curr->clauses [i];
		fprintf (stderr, "For exception %zu, offset %d len %d considered\n", (intptr_t)exc, clause->handler_offset, clause->handler_len);
		
		// FIXME: Temp for test. Use actual classes
		if (clause->data.catch_class == (MonoClass *)exc) {
			fprintf (stderr, "Exception caught");
			matched = TRUE;
		}
	}

	// Frees curr
	mono_try_stack_pop (&global_stack);

	if (!matched)
		mono_try_stack_rethrow (&global_stack, (MonoException *)exc);
}

gint 
sort_ending_offset_desc (gconstpointer _a, gconstpointer _b)
{
	MonoExceptionClause *a = (MonoExceptionClause *)_a;
	MonoExceptionClause *b = (MonoExceptionClause *)_b;
	return (a->handler_offset + a->handler_len) - (b->handler_offset + b->handler_len);
}

void
mono_emit_try_enter (MonoCompile *cfg, intptr_t offset, MonoMethodHeader *header)
{
	if (!header->num_clauses)
		return;

	// As per ECMA-335 I.12.4.2.7, all clauses must either be
	// disjoint -- No sections in common
	// mutually protective -- Same protected region, disjoint handler
	// nested -- All regions of one fits cleanly inside the other
	
	GPtrArray *clauses = g_ptr_array_new ();

	// Make collection that has same starting offset 
	for (int i=0; i < header->num_clauses; i++) {
		MonoExceptionClause *curr = &header->clauses [i];
		if (offset == curr->try_offset)
			g_ptr_array_add (clauses, curr);
	}

	// Sort by ending offset
	g_ptr_array_sort (clauses, sort_ending_offset_desc);

	GArray *curr_shared = g_array_new (TRUE, TRUE, sizeof (MonoExceptionClause *));
	MonoExceptionClause *prev = (MonoExceptionClause *)clauses->pdata [0];
	g_array_append_val (curr_shared, prev);

	for (int i=1; i < clauses->len; i++) {
		// We're using i here rather than the clause because the order in the collection
		// determines matching order
		if (i == 0)
			break;

		MonoExceptionClause *curr = (MonoExceptionClause *)clauses->pdata [i];

		if (curr->handler_offset + curr->handler_len <= prev->try_len + prev->try_offset) {
			// Nesting level += 1

			// FIXME: YOU CAN"T DO THIS. 
			// You need to send the offsets instead
			// And they need to be a monoinst
			/*MonoInst *args[2];*/
			/*args[0] = (MonoInst *)curr_shared->data;*/
			/*args[1] = (MonoInst *)curr_shared->len;*/
			/*mono_emit_jit_icall (cfg, mono_try_enter_impl, args);*/
			
			mono_jump_try_handler ((MonoExceptionClause **)curr_shared->data, curr_shared->len);

			// Don't free the element data
			// Will be freed when frame is popped
			g_array_free (curr_shared, FALSE);

			g_array_append_val (curr_shared, curr);

		} else if (curr->try_offset == prev->try_offset && curr->try_len == prev->try_len) {
			g_array_append_val (curr_shared, curr);

		} else {
			// Disjoint means shouldn't share try_offset
			g_assert_not_reached ();
		}

		prev = curr;
	}
	if (curr_shared->len > 0)
		mono_jump_try_handler ((MonoExceptionClause **)curr_shared->data, curr_shared->len);

	g_array_free (curr_shared, curr_shared->len == 0);

	g_ptr_array_free (clauses, TRUE);
}

