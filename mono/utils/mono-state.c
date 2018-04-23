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

/**
 * Create the VMState from the memory of the calling process
 */
MonoNativeState *
mono_native_state_new_with_ctx (MonoContext *ctx)
{
	MonoInternalThread *thread_array [128];

	MonoNativeState *state = g_new0 (sizeof (MonoNativeState));
	state->ctx = ctx;
	state->protocol_version = MONO_NATIVE_STATE_PROTOCOL_VERSION;
	state->nthreads = collect_threads (thread_array, 128);
	state->threads = g_new0 (sizeof (gpointer) * state->nthreads);

	for (int tindex = 0; tindex < nthreads; ++tindex) {
		MonoInternalThread *thread = thread_array [tindex];
		state->threads [i] = summarize_thread (thread);
	}

	return state;
}

static MonoThreadState *
summarize_thread (MonoInternalThread *thread)
{
	MonoDomain *obj_domain = thread->obj->vtable->domain;

	MonoThreadState *state = g_new0 (sizeof (MonoThreadState));
	state->managed_thread_ptr = (intptr_t) get_current_thread_ptr_for_domain (domain, thread);
	state->info_addr = (intptr_t) thread->thread_info;
	state->native_thread_id = (intptr_t) thread_get_tid (thread);

	// Caller owns memory for state->frames
	state->nframes = mono_threads_get_thread_stacktrace (thread, state->frames);

	return state;
}

gchar *
mono_native_state_emit (MonoNativeState *state)
{
	FILE *dumpfile;
	char * dumpname;

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
		MonoThreadState *thread;

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
		mono_json_writer_printf (&writer, "\"%p\",\n", (gpointer) thread->info_addr)

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "native_thread_id");
		mono_json_writer_printf (&writer, "\"%p\",\n", (gpointer) thread->native_thread_id);

		// Stack frames for thread
		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "frames");

		mono_json_writer_array_begin (&writer);
		for (int i = 0; i < thread->nframes; ++i) {
			MonoStackFrameInfo *frame = thread->frames [i];

			if (frame->type == FRAME_TYPE_MANAGED) {
			} else {
				mono_json_writer_indent (&writer);
				mono_json_writer_object_key(&writer, "method_start");
				mono_json_writer_printf (&writer, "\"%p\",\n", (gpointer) frame->ji->code_start);

				mono_json_writer_indent (&writer);
				mono_json_writer_object_key(&writer, "native_offset");
				mono_json_writer_printf (&writer, "\"%p\",\n", (gpointer) frame->ji->code_start);
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

	dumpname = g_strdup_printf ("%s.json", g_path_get_basename (acfg->image->name));
	dumpfile = fopen (dumpname, "w+");
	g_free (dumpname);

	fprintf (dumpfile, "%s", writer.text->str);
	fclose (dumpfile);

	mono_json_writer_destroy (&writer);
}


