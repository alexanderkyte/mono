#include "exceptions.h"

// Bury state in jit tls info later
// Can use in tests for now
MonoTryState global_state = { 0, 0 };

void
mono_try_stack_pop (MonoTryStack **stack)
{
	g_assert (*stack);

	MonoJumpBuffer *old_buf = mono_try_stack_peek (stack)->buffer;
	old_buf->ref_count--;

	if (old_buf->ref_count == 0)
		g_free (old_buf);

	if ((*stack)->bottom == 0) {
		MonoTryStack *old = *stack;
		*stack = (*stack)->next;
		g_free (old);
	} else {
		(*stack)->bottom--;
	}

}

MonoTryFrame*
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

MonoTryFrame *
mono_try_stack_reserve (MonoTryStack **stack)
{
	gpointer addr = NULL;

	if (!(*stack) || (*stack)->bottom == TRY_STACKLET_SIZE) {
		MonoTryStack *old = *stack;
		MonoTryStack *top = g_malloc0 (sizeof(MonoTryStack));
		addr = &(top->buffers [0]);
		top->bottom = 1;

		top->next = old;
		*stack = top;
	} else {
		addr = &((*stack)->buffers [(*stack)->bottom]);
		(*stack)->bottom = (*stack)->bottom + 1;
	}

	return addr;
}

void __attribute__((noreturn))
mono_try_stack_rethrow (MonoTryStack **stack, MonoException *exc)
{
	global_state.current_exc = exc;

	g_assert (*stack);
	MonoTryFrame *container = mono_try_stack_peek (stack);

	if (!container) {
		if (test_unhandled_exception_func)
			test_unhandled_exception_func (exc);
		else
			g_error ("Unhandled exception\n");
	}

	g_assert (container->buffer);
	longjmp (container->buffer->buf, MonoJumpLand);
}

void 
mono_try_stack_exit (MonoTryStack **stack)
{
	g_assert (*stack);
	mono_try_stack_pop (stack);
}

void
mono_throw (MonoException *exc)
{
	mono_try_stack_rethrow (&global_state.stack, exc);
}

void
mono_push_try_handler (MonoTryStack **stack, MonoJumpBuffer *jbuf, MonoExceptionClause **exceptions, int count)
{
	fprintf (stderr, "Start push group\n");
	for (int i=0; i < count; i++) {
		MonoExceptionClause *clause = exceptions [i];
		fprintf (stderr, "Clause %d: try start/end(%d to %d) and handler start/end(%d to %d)\n",
			i, clause->try_offset, clause->try_offset + clause->try_len, clause->handler_offset, clause->handler_offset + clause->handler_len
		);
	}
	fprintf (stderr, "End push group\n");
	MonoTryFrame *frame = mono_try_stack_reserve (stack);
	frame->num_clauses = count;
	frame->clauses = exceptions;
	frame->buffer = jbuf;
}

void __attribute__((noreturn))
mono_handle_exception_jump (MonoException *exc)
{
	fprintf (stderr, "Before peek\n");
	MonoTryFrame *curr = mono_try_stack_peek (&global_state.stack);
	fprintf (stderr, "After peek\n");
	fprintf (stderr, "Peeking %p from stack\n", curr->clauses[0]->data.catch_class);

	for (int i=0; i < curr->num_clauses; i++) {
		MonoExceptionClause *clause = curr->clauses [i];
		fprintf (stderr, "For exception %p, try %d to %d handler %d to %d considered, catch class %p\n",
			exc,
			clause->try_offset, clause->try_offset + clause->try_len,
			clause->handler_offset, clause->handler_offset + clause->handler_len,
			clause->data.catch_class);

		// FIXME: Temp for test. Use actual classes
		if (clause->data.catch_class == (MonoClass *)exc) {
			fprintf (stderr, "Exception caught!\n");

			if (test_caught_exception_func)
				(*test_caught_exception_func) (exc, clause);
		}
	}

	// Frees curr
	mono_try_stack_pop (&global_state.stack);
	fprintf (stderr, "Popping %p from stack\n", curr->clauses[0]->data.catch_class);

	mono_try_stack_rethrow (&global_state.stack, (MonoException *)exc);
}

gint 
sort_ending_offset_desc (gconstpointer _a, gconstpointer _b)
{
	MonoExceptionClause *first = *(MonoExceptionClause **)_a;
	MonoExceptionClause *second = *(MonoExceptionClause **)_b;
	size_t first_end = (first->handler_offset + first->handler_len);
	size_t second_end = (second->handler_offset + second->handler_len);

	return second_end - first_end;
}

MonoJumpBuffer *
mono_push_try_handlers (MonoCompile *cfg, intptr_t offset, MonoMethodHeader *header)
{
	if (!header->num_clauses)
		return NULL;

	fprintf (stderr, "Processing offset %ld\n", offset);

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

	if (clauses->len == 0) {
		g_ptr_array_free (clauses, TRUE);
		return NULL;
	}

	MonoJumpBuffer *jbuf = g_malloc0 (sizeof(MonoJumpBuffer));

	// Sort by ending offset
	g_ptr_array_sort (clauses, sort_ending_offset_desc);

	GArray *curr_shared = g_array_new (TRUE, TRUE, sizeof (MonoExceptionClause *));
	MonoExceptionClause *prev = (MonoExceptionClause *)clauses->pdata [0];
	g_array_append_val (curr_shared, prev);

	for (int i=1; i < clauses->len; i++) {
		MonoExceptionClause *curr = (MonoExceptionClause *)clauses->pdata [i];

		g_assert (curr->try_offset == prev->try_offset);

		if (curr->try_len == prev->try_len) {
			// Mutual protection of same region by different handlers
			// We know this to be true because if the protected regions
			// are the same, then one entire try+handler block cannot fit
			// inside another's try block
			g_array_append_val (curr_shared, curr);

		} else if (prev->handler_offset >= curr->handler_len + curr->handler_offset) {
			// Nesting level += 1

			// FIXME: YOU CAN"T DO THIS in mini. 
			// You need to send the offsets instead
			// And they need to be a monoinst
			/*MonoInst *args[2];*/
			/*args[0] = (MonoInst *)curr_shared->data;*/
			/*args[1] = (MonoInst *)curr_shared->len;*/
			/*mono_emit_jit_icall (cfg, mono_try_enter_impl, args);*/
			
			mono_push_try_handler (&global_state.stack, jbuf, (MonoExceptionClause **)curr_shared->data, curr_shared->len);
			jbuf->ref_count++;

			// Don't free the element data
			// Will be freed when frame is popped
			g_array_free (curr_shared, FALSE);
			curr_shared = g_array_new (TRUE, TRUE, sizeof (MonoExceptionClause *));

			g_array_append_val (curr_shared, curr);

		} else {
			g_assert_not_reached ();
		}

		prev = curr;
	}

	if (curr_shared->len > 0) {
		mono_push_try_handler (&global_state.stack, jbuf, (MonoExceptionClause **)curr_shared->data, curr_shared->len);
		jbuf->ref_count++;
	}

	// Free the array, leave behind the array if it's got any elements
	g_array_free (curr_shared, curr_shared->len == 0);

	g_ptr_array_free (clauses, TRUE);

	return jbuf;
}

void
__attribute__((always_inline))
mono_enter_try (MonoCompile *cfg, intptr_t offset, MonoMethodHeader *header)
{
	MonoJumpBuffer *curr = mono_push_try_handlers (cfg, offset, header);
	intptr_t exc = (intptr_t) setjmp (curr->buf);
	if (exc == MonoJumpReturnDirect)
		return;

	// Can't return from here.
	// Must jump to handler
	mono_handle_exception_jump (global_state.current_exc);
}



