#ifndef __SJLJ_EXCEPTIONS_
#define __SJLJ_EXCEPTIONS_

#include <glib.h>
#include <mono/mini/mini.h>

#include <setjmp.h>

#define TRY_STACKLET_SIZE 10

// A way to get around being unable to pass stack
// arrays (jmp_buf is int[]) around
typedef struct MonoJumpBuffer {
	jmp_buf buf;
} MonoJumpBuffer;

typedef struct MonoTryStack {
	MonoJumpBuffer buffers [TRY_STACKLET_SIZE];
	size_t bottom;
	struct MonoTryStack *next;
} MonoTryStack;

MonoJumpBuffer *
mono_try_stack_peek (MonoTryStack **stack);

void
mono_try_stack_pop (MonoTryStack **stack);

void 
mono_try_stack_push (MonoTryStack **stack, MonoJumpBuffer value);

void __attribute__((noreturn))
mono_try_stack_throw (MonoTryStack **stack, intptr_t throw_data);

void 
mono_try_stack_exit (MonoTryStack **stack);

intptr_t
mono_try_stack_enter (MonoTryStack **stack, GSList *handlers);

void
mono_emit_try_enter (MonoCompile *cfg, intptr_t offset, MonoMethodHeader *header);

void
mono_try_enter_impl (MonoExceptionClause *exceptions, int count);

gint 
sort_ending_offset_desc (gconstpointer _a, gconstpointer _b);

#endif
