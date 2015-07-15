#include "exceptions.h"

MonoJumpBuffer
mono_try_stack_pop (MonoTryStack **stack)
{
	MonoJumpBuffer value;
	g_assert (*stack);

	if ((*stack)->bottom == 0) {
		MonoTryStack *old = *stack;
		value = old->buffers [0];

		*stack = (*stack)->next;
		g_free (old);
	} else {
		value = (*stack)->buffers [(*stack)->bottom];
		(*stack)->bottom--;
	}

	return value;
}

void 
mono_try_stack_push (MonoTryStack **stack, MonoJumpBuffer value)
{
	if (!(*stack) || (*stack)->bottom == TRY_STACKLET_SIZE) {
		MonoTryStack *old = *stack;
		MonoTryStack *top = g_malloc0 (sizeof(MonoTryStack));
		top->buffers [0] = value;

		top->next = old;
		*stack = old;
	} else {
		(*stack)->buffers [(*stack)->bottom] = value;
		(*stack)->bottom++;
	}
}

void 
mono_try_stack_throw (MonoTryStack **stack, intptr_t throw_data)
{
	longjmp (mono_try_stack_pop (stack).buf, throw_data);
}

void 
mono_try_stack_exit (MonoTryStack **stack)
{
	mono_try_stack_pop (stack);
}

intptr_t
mono_try_stack_enter (MonoTryStack **stack, GSList *handlers)
{
	MonoJumpBuffer value;
	intptr_t result = setjmp (value.buf);
	if (result == 0)
		mono_try_stack_push (stack, value);
	return result;
}

void
mono_try_enter_impl (MonoExceptionClause *exceptions, int count)
{
/*MonoJitTlsData *jit_tls = mono_native_tls_get_value (mono_jit_tls_id);*/
// At this point, either success and we discard
// or exception and we have our handlers to try in order
// and we can get monojitinfo info
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

	GArray *curr_shared = NULL;

	MonoExceptionClause *prev = NULL;

	for (int i=0; i < clauses->len; i++) {
		// We're using i here rather than the clause because the order in the collection
		// determines matching order
		if (i == 0)
			prev = (MonoExceptionClause *)clauses->pdata [i];
			break;

		MonoExceptionClause *curr = (MonoExceptionClause *)clauses->pdata [i];

		if (curr->handler_offset + curr->handler_len <= prev->try_len + prev->try_offset) {
			// Nesting level += 1
			if(curr_shared) {
				g_array_append_val (curr_shared, curr);

				// YOU CAN"T DO THIS. BREAKS AOT.
				// You need to send the offsets instead
				// And they need to be a monoinst
				MonoInst *args[2];
				args[0] = (MonoInst *)curr_shared->data;
				args[1] = (MonoInst *)curr_shared->len;
				mono_emit_jit_icall (cfg, mono_try_enter_impl, args);

				// Don't free the element data
				// Will be freed when frame is popped
				g_array_free (curr_shared, FALSE);
			}
			GArray *curr_shared = g_array_new (TRUE, TRUE, sizeof (MonoExceptionClause *));
			g_array_append_val (curr_shared, curr);

		} else if (curr->try_offset == prev->try_offset && curr->try_len == prev->try_len) {
			g_array_append_val (curr_shared, curr);

		} else {
			// Disjoint means shouldn't share try_offset
			g_assert_not_reached ();
		}

		prev = curr;
	}

	g_ptr_array_free (clauses, TRUE);
}

