/**
 * \file
 * Types for the debugger state machine and wire protocol
 *
 * Author:
 *   Alexander Kyte (alkyte@microsoft.com)
 *
 * (C) 2018 Microsoft, Inc.
 *
 */
#ifndef __MONO_UTILS_DEBUGGER_STATE_MACHINE__
#define __MONO_UTILS_DEBUGGER_STATE_MACHINE__

#include <config.h>
#include <glib.h>
#include <mono/metadata/metadata.h>
#include <mono/utils/mono-threads-debug.h>
#include <mono/utils/json.h>

typedef enum {
	MONO_DEBUGGER_STARTED = 0,
	MONO_DEBUGGER_RESUMED = 1,
	MONO_DEBUGGER_SUSPENDED = 2,
	MONO_DEBUGGER_TERMINATED = 3,
} MonoDebuggerThreadState;

void
mono_debugger_log_init (void);

void
mono_debugger_log_free (void);

void
mono_debugger_log_exit (int exit_code);

void
mono_debugger_log_add_bp (MonoMethod *method, long il_offset);

void
mono_debugger_log_remove_bp (MonoMethod *method, long il_offset);

void
mono_debugger_log_command (const char *command_set, const char *command, guint8 *buf, int len);

void
mono_debugger_log_event (gpointer tls, const char *event, guint8 *buf, int len);

void
mono_debugger_log_bp_hit (gpointer tls, MonoMethod *method, long il_offset);

void
mono_debugger_log_resume (gpointer tls);

void
mono_debugger_log_suspend (gpointer tls);

#if 0
#define DEBUGGER_STATE_MACHINE_DEBUG(level, ...)
#else
#define DEBUGGER_STATE_MACHINE_DEBUG(level, ...) MOSTLY_ASYNC_SAFE_PRINTF(__VA_ARGS__)
#endif

void
mono_debugger_state (JsonWriter *writer);

#endif // __MONO_UTILS_DEBUGGER_STATE_MACHINE__ 
