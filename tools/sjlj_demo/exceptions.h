#ifndef __SJLJ_EXCEPTIONS_
#define __SJLJ_EXCEPTIONS_

#include <glib.h>
#include <mono/mini/mini.h>

#include <setjmp.h>

#define TRY_STACKLET_SIZE 10

#define ZERO_LENGTH_ARRAY 0

// A way to get around being unable to pass stack
// arrays (jmp_buf is int[]) around
typedef struct MonoJumpBuffer {
	jmp_buf buf;
	size_t ref_count;
} MonoJumpBuffer;

typedef struct MonoTryFrame {
	MonoJumpBuffer *buffer;
	int num_clauses;
	MonoExceptionClause **clauses;
} MonoTryFrame;

typedef struct MonoTryStack {
	MonoTryFrame buffers [TRY_STACKLET_SIZE];
	size_t bottom;
	struct MonoTryStack *next;
} MonoTryStack;


void
mono_try_stack_pop (MonoTryStack **stack);

MonoTryFrame*
mono_try_stack_peek (MonoTryStack **stack);

MonoTryFrame *
mono_try_stack_reserve (MonoTryStack **stack);

void __attribute__((noreturn))
mono_try_stack_rethrow (MonoTryStack **stack, MonoException *exc);

void 
mono_try_stack_exit (MonoTryStack **stack);

void
mono_throw (MonoException *exc);

void 
mono_handle_exception_jump (MonoException *exc);

gint 
sort_ending_offset_desc (gconstpointer _a, gconstpointer _b);

void
mono_push_try_handler (MonoTryStack **stack, MonoJumpBuffer *jbuf, MonoExceptionClause **exceptions, int count);

MonoJumpBuffer *
mono_push_try_handlers (MonoCompile *cfg, intptr_t offset, MonoMethodHeader *header);

void
mono_enter_try (MonoCompile *cfg, intptr_t offset, MonoMethodHeader *header);

#endif
