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

#include <mono/utils/mono-debugger-types.h>
#include <mono/utils/mono-threads-debug.h>
#include <mono/utils/json.h>

// So we have layers of abstractions in the debugger 
//
// Thread Stack Model - A thread can be resumed or suspended
// Thread Model - A thread can be resumed or suspended
// BP Model - There is a global store of breakpoints we can modify and read
// Thread Pending 
//
// Thread state layer - resume command, suspend command, bp add, bp remove
//
// Debugger-controller layer - add/remove bp, add invoke, finsh invoke, hit bp, resume, suspend, step(which becomes series of breakpoints)
// Protocol layer - command received, command reply sent, event sent
// Connection Layer - Received Packet, Sent Packet, Connected, Disconnected
//
// Client State Model - Client has log of events, is in connected, waiting(for response to command), disconnected

void
mono_debugger_log_init (void);

void
mono_debugger_log_free (void);

void
mono_debugger_log_command (const char *command_set, const char *command, guint8 *buf, int len);

void
mono_debugger_log_event (DebuggerTlsData *tls, const char *event, guint8 *buf, int len);

void
mono_debugger_log_exit (int exit_code);

void
mono_debugger_log_add_bp (MonoMethod *method, long il_offset);

void
mono_debugger_log_remove_bp (MonoMethod *method, long il_offset);

void
mono_debugger_log_bp_hit (DebuggerTlsData *tls, MonoMethod *method, long il_offset);

void
mono_debugger_log_resume (DebuggerTlsData *tls);

void
mono_debugger_log_suspend (DebuggerTlsData *tls);

#if 0
#define DEBUGGER_STATE_MACHINE_DEBUG(level, ...)
#else
#define DEBUGGER_STATE_MACHINE_DEBUG(level, ...) MOSTLY_ASYNC_SAFE_PRINTF(__VA_ARGS__)
#endif

void
mono_debugger_state (JsonWriter *writer);

#endif // __MONO_UTILS_DEBUGGER_STATE_MACHINE__ 
