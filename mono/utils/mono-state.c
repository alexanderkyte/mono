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
#include <mono/utils/mono-state.h>
#include <mono/metadata/object-internals.h>

#define MONO_MAX_SUMMARY_LEN 900
static JsonWriter writer;
static GString static_gstr;
static char output_dump_str [MONO_MAX_SUMMARY_LEN];

static void mono_json_writer_init_static (void) {
	static_gstr.len = 0;
	static_gstr.allocated_len = MONO_MAX_SUMMARY_LEN;
	static_gstr.str = output_dump_str;
	memset (output_dump_str, 0, sizeof (output_dump_str));

	writer.indent = 0;
	writer.text = &static_gstr;
}

static void
mono_native_state_add_ctx (JsonWriter *writer, MonoContext *ctx)
{
	// Context
	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "ctx");
	mono_json_writer_indent (writer);
	mono_json_writer_object_begin(writer);

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "IP");
	mono_json_writer_printf (writer, "\"%p\",\n", (gpointer) MONO_CONTEXT_GET_IP (ctx));

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "SP");
	mono_json_writer_printf (writer, "\"%p\",\n", (gpointer) MONO_CONTEXT_GET_SP (ctx));

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "BP");
	mono_json_writer_printf (writer, "\"%p\",\n", (gpointer) MONO_CONTEXT_GET_BP (ctx));

	mono_json_writer_object_end (writer);
	mono_json_writer_printf (writer, ",\n");
}

static void
mono_native_state_add_frame (JsonWriter *writer, MonoFrameSummary *frame)
{
	mono_json_writer_indent (writer);
	mono_json_writer_object_begin(writer);

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "is_managed");
	mono_json_writer_printf (writer, "\"%s\",\n", frame->is_managed ? "true" : "false");

	if (frame->is_managed) {
		/*mono_json_writer_indent (writer);*/
		/*mono_json_writer_object_key(writer, "assembly");*/
		/*mono_json_writer_printf (writer, "\"%s\",\n", frame->managed_data.assembly);*/

		/*mono_json_writer_indent (writer);*/
		/*mono_json_writer_object_key(writer, "token");*/
		/*mono_json_writer_printf (writer, "\"%d\",\n", frame->managed_data.token);*/

		/*mono_json_writer_indent (writer);*/
		/*mono_json_writer_object_key(writer, "native_offset");*/
		/*mono_json_writer_printf (writer, "\"%d\",\n", frame->managed_data.native_offset);*/

		/*mono_json_writer_indent (writer);*/
		/*mono_json_writer_object_key(writer, "il_offset");*/
		/*mono_json_writer_printf (writer, "\"%d\",\n", frame->managed_data.il_offset);*/

	} else {
		mono_json_writer_indent (writer);
		mono_json_writer_object_key(writer, "native_address");
		mono_json_writer_printf (writer, "\"%lx\",\n", (intptr_t) frame->unmanaged_data.ip);

		/*mono_json_writer_indent (writer);*/
		/*mono_json_writer_object_key(writer, "is_trampoline");*/
		/*mono_json_writer_printf (writer, "\"%s\",\n", frame->unmanaged_data.is_trampoline ? "true" : "false");*/
	}

	mono_json_writer_indent_pop (writer);
	mono_json_writer_indent (writer);
	mono_json_writer_object_end (writer);
	mono_json_writer_printf (writer, ",\n");
}

static void
mono_native_state_add_thread (JsonWriter *writer, MonoThreadSummary *thread, MonoContext *ctx)
{
	mono_json_writer_indent (writer);
	mono_json_writer_object_begin(writer);

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "protocol_version");
	mono_json_writer_printf (writer, "%s,\n", MONO_NATIVE_STATE_PROTOCOL_VERSION);

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "is_managed");
	mono_json_writer_printf (writer, "%s,\n", thread->is_managed ? "true" : "false");

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "managed_thread_ptr");
	mono_json_writer_printf (writer, "\"%x\",\n", (gpointer) thread->managed_thread_ptr);

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "thread_info_addr");
	mono_json_writer_printf (writer, "\"%x\",\n", (gpointer) thread->info_addr);

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "native_thread_id");
	mono_json_writer_printf (writer, "\"%x\",\n", (gpointer) thread->native_thread_id);

	mono_native_state_add_ctx (writer, ctx);

	// Stack frames for thread
	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "frames");

	fprintf (stderr, "frames: %d\n", thread->num_frames);

	mono_json_writer_array_begin (writer);
	for (int i = 0; i < thread->num_frames; ++i)
		mono_native_state_add_frame (writer, &thread->frames [i]);

	mono_json_writer_indent_pop (writer);
	mono_json_writer_indent (writer);
	mono_json_writer_array_end (writer);
	mono_json_writer_printf (writer, ",\n");

	mono_json_writer_indent_pop (writer);
	mono_json_writer_indent (writer);
	mono_json_writer_object_end (writer);
	mono_json_writer_printf (writer, ",\n");
}

static void
mono_native_state_add_prologue (JsonWriter *writer)
{
	mono_json_writer_init (writer);
	mono_json_writer_object_begin(writer);

	// Start threads array
	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "threads");
	mono_json_writer_array_begin (writer);
}

static void
mono_native_state_add_epilogue (JsonWriter *writer)
{
	mono_json_writer_indent_pop (writer);
	mono_json_writer_indent (writer);
	mono_json_writer_array_end (writer);
	mono_json_writer_printf (writer, ",\n");
}

void
mono_summarize_native_state_begin (void)
{
	mono_json_writer_init_static ();
	fprintf (stderr, "%s %d : writer: %s\n", __FILE__, __LINE__, writer.text->str);
	mono_native_state_add_prologue (&writer);
	fprintf (stderr, "%s %d : writer: %s\n", __FILE__, __LINE__, writer.text->str);
}

char *
mono_summarize_native_state_end (void)
{
	mono_native_state_add_epilogue (&writer);
	fprintf (stderr, "%s %d : writer: %s\n", __FILE__, __LINE__, writer.text->str);
	return writer.text->str;
}

void
mono_summarize_native_state_add_thread (MonoThreadSummary *thread, MonoContext *ctx)
{
	fprintf (stderr, "%s %d : writer: %s\n", __FILE__, __LINE__, writer.text->str);
	mono_native_state_add_thread (&writer, thread, ctx);
	fprintf (stderr, "%s %d : writer: %s\n", __FILE__, __LINE__, writer.text->str);
}

