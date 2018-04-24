/**
 * \file
 * Support for verbose unmanaged crash dumps
 *
 * Author:
 *   Alexander Kyte (alkyte@microsoft.com)
 *
 * (C) 2018 Microsoft, Inc.
 *
 */
#include <config.h>
#include <glib.h>
#include <mono/utils/json.h>
#include "mono-state.h"
#include <mono/metadata/object-internals.h>

gchar *
mono_native_state_emit (MonoNativeState *state)
{
	JsonWriter writer;
	mono_json_writer_init (&writer);

	mono_json_writer_object_begin(&writer);

	if (state->ctx) {
		// Context
		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "ctx");
		mono_json_writer_indent (&writer);
		mono_json_writer_object_begin(&writer);

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "IP");
		mono_json_writer_printf (&writer, "\"%p\",\n", (gpointer) MONO_CONTEXT_GET_IP (state->ctx));

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "SP");
		mono_json_writer_printf (&writer, "\"%p\",\n", (gpointer) MONO_CONTEXT_GET_SP (state->ctx));

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "BP");
		mono_json_writer_printf (&writer, "\"%p\",\n", (gpointer) MONO_CONTEXT_GET_BP (state->ctx));

		mono_json_writer_object_end (&writer);
		mono_json_writer_printf (&writer, ",\n");
	}

	// Threads
	mono_json_writer_indent (&writer);
	mono_json_writer_object_key(&writer, "threads");
	mono_json_writer_array_begin (&writer);

	for (int i = 0; i < state->nthreads; ++i) {
		MonoThreadSummary *thread = &state->threads [i]; 

		mono_json_writer_indent (&writer);
		mono_json_writer_object_begin(&writer);

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "is_managed");
		mono_json_writer_printf (&writer, "%s,\n", thread->is_managed ? "true" : "false");

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "managed_thread_ptr");
		mono_json_writer_printf (&writer, "\"%p\",\n", (gpointer) thread->managed_thread_ptr);

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "thread_info_addr");
		mono_json_writer_printf (&writer, "\"%p\",\n", (gpointer) thread->info_addr);

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "native_thread_id");
		mono_json_writer_printf (&writer, "\"%p\",\n", (gpointer) thread->native_thread_id);

		// Stack frames for thread
		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "frames");

		mono_json_writer_array_begin (&writer);
		for (int i = 0; i < thread->nframes; ++i) {
			MonoStackFrameInfo *frame = &thread->frames [i];

			if (frame->type == FRAME_TYPE_MANAGED) {
			} else {
				mono_json_writer_indent (&writer);
				mono_json_writer_object_key(&writer, "method_start");
				mono_json_writer_printf (&writer, "\"%p\",\n", (gpointer) 0x0);

				/*mono_json_writer_indent (&writer);*/
				/*mono_json_writer_object_key(&writer, "native_offset");*/
				/*mono_json_writer_printf (&writer, "\"%p\",\n", (gpointer) frame->ji->code_start);*/
			}
		}

		mono_json_writer_indent_pop (&writer);
		mono_json_writer_indent (&writer);
		mono_json_writer_array_end (&writer);
		mono_json_writer_printf (&writer, ",\n");


		mono_json_writer_indent_pop (&writer);
		mono_json_writer_indent (&writer);
		mono_json_writer_object_end (&writer);
		mono_json_writer_printf (&writer, ",\n");
	}

	mono_json_writer_indent_pop (&writer);
	mono_json_writer_indent (&writer);
	mono_json_writer_array_end (&writer);
	mono_json_writer_printf (&writer, ",\n");

	char *out = g_strdup (writer.text->str);
	mono_json_writer_destroy (&writer);

	fprintf (stderr, "output is: \n||%s||\n", out);

	return out;
}

MonoNativeState *
mono_native_state_new_with_ctx (MonoContext *ctx)
{
	MonoNativeState *state = g_malloc0(sizeof (MonoNativeState));
	state->ctx = ctx;
	state->protocol_version = MONO_NATIVE_STATE_PROTOCOL_VERSION;
	state->nthreads = mono_threads_summarize_all (&state->threads);

	return state;
}
