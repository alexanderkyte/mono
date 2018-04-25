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
#include "mono-stack-unwinding.h"
#include "mono-context.h"
#include <mono/metadata/threads-types.h>

#define MONO_NATIVE_STATE_PROTOCOL_VERSION "0.0.1"

/**
 * 
 */
char *
mono_summarize_native_state (MonoContext *ctx);

#endif // MONO_UTILS_NATIVE_STATE
