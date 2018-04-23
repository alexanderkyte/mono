/**
 * \file
 * Support for interop with the Microsoft Error Reporting tool (header)
 *
 * Author:
 *   Alexander Kyte (alkyte@microsoft.com)
 *
 * (C) 2018 Microsoft, Inc.
 *
 */
#ifndef __MONO_UTILS_NATIVE_STATE__
#define __MONO_UTILS_NATIVE_STATE__

#include <config.h>
#include <glib.h>

#define MONO_NATIVE_STATE_PROTOCOL_VERSION "0.0.1"

typedef struct {
	gboolean is_managed;

	intptr_t managed_thread_ptr;
	intptr_t info_addr;
	intptr_t native_thread_id;

	int nframes;
	MonoStackFrameInfo *frames;
} MonoThreadState;

typedef struct {
	char *protocol_version;

	int nthreads;
	MonoThreadState *threads;

	MonoContext *ctx;
} MonoNativeState;

/**
 * Emit the MonoNativeState state to output_path in a chosen json format
 * 
 * \arg state the state to serialize and write
 * \arg output_path the location
 */
void
mono_native_state_write (MonoNativeState *state, char *output_path);

/**
 * Emit the MonoNativeState state to output_path in a chosen json format
 * 
 * \arg state the state to serialize and write
 * \arg output_path the location
 */
gchar *
mono_native_state_emit (MonoNativeState *state);

/**
 * Create the VMState from the memory of the calling process
 */
MonoNativeState *
mono_native_state_new (void);

#endif // MONO_UTILS_NATIVE_STATE
