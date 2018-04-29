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
	mono_json_writer_object_begin(writer);

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "IP");
	mono_json_writer_printf (writer, "\"%p\",\n", (gpointer) MONO_CONTEXT_GET_IP (ctx));

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "SP");
	mono_json_writer_printf (writer, "\"%p\",\n", (gpointer) MONO_CONTEXT_GET_SP (ctx));

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "BP");
	mono_json_writer_printf (writer, "\"%p\"\n", (gpointer) MONO_CONTEXT_GET_BP (ctx));

	mono_json_writer_indent_pop (writer);
	mono_json_writer_indent (writer);
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
		mono_json_writer_indent (writer);
		mono_json_writer_object_key(writer, "assembly");
		mono_json_writer_printf (writer, "\"%s\",\n", frame->str_descr);

		mono_json_writer_indent (writer);
		mono_json_writer_object_key(writer, "token");
		mono_json_writer_printf (writer, "\"0x%05x\",\n", frame->managed_data.token);

		mono_json_writer_indent (writer);
		mono_json_writer_object_key(writer, "native_offset");
		mono_json_writer_printf (writer, "\"0x%x\",\n", frame->managed_data.native_offset);

		mono_json_writer_indent (writer);
		mono_json_writer_object_key(writer, "il_offset");
		mono_json_writer_printf (writer, "\"0x%05x\"\n", frame->managed_data.il_offset);

	} else {
		mono_json_writer_indent (writer);
		mono_json_writer_object_key(writer, "native_address");
		mono_json_writer_printf (writer, "\"0x%x\",\n", (intptr_t) frame->unmanaged_data.ip);

		mono_json_writer_indent (writer);
		mono_json_writer_object_key(writer, "is_trampoline");
		mono_json_writer_printf (writer, "\"%s\"", frame->unmanaged_data.is_trampoline ? "true" : "false");

		if (frame->unmanaged_data.has_name) {
			mono_json_writer_printf (writer, ",\n");
			mono_json_writer_indent (writer);
			mono_json_writer_object_key(writer, "unmanaged_name");
			mono_json_writer_printf (writer, "\"%s\"\n", frame->str_descr);
		} else {
			mono_json_writer_printf (writer, "\n");
		}
	}

	mono_json_writer_indent_pop (writer);
	mono_json_writer_indent (writer);
	mono_json_writer_object_end (writer);
}

static void
mono_native_state_add_frames (JsonWriter *writer, int num_frames, MonoFrameSummary *frames, const char *label)
{
	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, label);

	mono_json_writer_array_begin (writer);

	mono_native_state_add_frame (writer, &frames [0]);
	for (int i = 1; i < num_frames; ++i) {
		mono_json_writer_printf (writer, ",\n");
		mono_native_state_add_frame (writer, &frames [i]);
	}
	mono_json_writer_printf (writer, "\n");

	mono_json_writer_indent_pop (writer);
	mono_json_writer_indent (writer);
	mono_json_writer_array_end (writer);
}


static void
mono_native_state_add_thread (JsonWriter *writer, MonoThreadSummary *thread, MonoContext *ctx)
{
	static gboolean not_first_thread = FALSE;

	if (not_first_thread) {
		mono_json_writer_printf (writer, ",\n");
	} else {
		not_first_thread = TRUE;
	}

	mono_json_writer_indent (writer);
	mono_json_writer_object_begin(writer);

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "is_managed");
	mono_json_writer_printf (writer, "%s,\n", thread->is_managed ? "true" : "false");

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "managed_thread_ptr");
	mono_json_writer_printf (writer, "\"0x%x\",\n", (gpointer) thread->managed_thread_ptr);

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "thread_info_addr");
	mono_json_writer_printf (writer, "\"0x%x\",\n", (gpointer) thread->info_addr);

	if (thread->name) {
		mono_json_writer_indent (writer);
		mono_json_writer_object_key(writer, "thread_name");
		mono_json_writer_printf (writer, "\"%s\",\n", thread->name);
	}

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "native_thread_id");
	mono_json_writer_printf (writer, "\"0x%x\",\n", (gpointer) thread->native_thread_id);

	mono_native_state_add_ctx (writer, ctx);

	if (thread->num_managed_frames > 0) {
		mono_native_state_add_frames (writer, thread->num_managed_frames, thread->managed_frames, "managed_frames");
	}
	if (thread->num_unmanaged_frames > 0) {
		if (thread->num_managed_frames > 0)
			mono_json_writer_printf (writer, ",\n");
		mono_native_state_add_frames (writer, thread->num_unmanaged_frames, thread->unmanaged_frames, "unmanaged_frames");
	}
	mono_json_writer_printf (writer, "\n");

	mono_json_writer_indent (writer);
	mono_json_writer_object_end (writer);
}

static void
mono_native_state_add_prologue (JsonWriter *writer)
{
	mono_json_writer_init (writer);
	mono_json_writer_object_begin(writer);

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "protocol_version");
	mono_json_writer_printf (writer, "\"%s\",\n", MONO_NATIVE_STATE_PROTOCOL_VERSION);

	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "threads");

	mono_arch_memory_info (&merp->memRes, &merp->memVirt);

	// Start threads array
	mono_json_writer_indent (writer);
	mono_json_writer_object_key(writer, "threads");
	mono_json_writer_array_begin (writer);
}

static void
mono_native_state_add_epilogue (JsonWriter *writer)
{
	mono_json_writer_indent_pop (writer);
	mono_json_writer_printf (writer, "\n");
	mono_json_writer_indent (writer);
	mono_json_writer_array_end (writer);

	mono_json_writer_indent_pop (writer);
	mono_json_writer_indent (writer);
	mono_json_writer_object_end (writer);
	mono_json_writer_printf (writer, "\n");
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

