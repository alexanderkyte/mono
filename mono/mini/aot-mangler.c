/**
 * \file
 * Aot Mangler Functions
 *
 * Authors:
 *   Alexander Kyte (alkyte@microsoft.com)
 *
 * Copyright 2017 Microsoft, Inc (http://www.microsoft.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include <mono/utils/json.h>
#include "config.h"
#include "aot-compiler.h"

/*
 * Mono Method Mangler Format:
 *
 * It is often useful to have a unique string associated with
 * each method. The complications with using this to describe so
 * rich a type system as C# is the issue of transitive type references.
 *
 * It is necessary to bake in information about the referenced types which 
 * becomes quite verbose when they nest. Clarity around the depth of the tree
 * at a given point in the string is important. We elect to use escaped brackets.
 *
 * Without using special characters, this is pretty difficult to do without
 * risking a clash with user-provided names. For this reason we encode the mangled
 * name in a format that is later scrubbed of these characters. This necessitates that we
 * make our emitted replacements unique, unable to clash with user-provided names. In our case
 * this is done by escaping the separator character we insert, the underscore.
 *
 * Mangler Grammar: (pseudo-BNF using parens for optional data, asterisk for "0 or more")
 * 	MANGLED_NAME: mono_aot METHOD_DEF
 * 	METHOD_DEF: "[" "method" "assembly_name" assembly_name METHOD_ATTRS
 * 	METHOD_ATTRS: "inflated" CONTEXT_DEF "declaring" METHOD_DEF "]" | "gtd" CONTEXT_DEF KLASS_DEF method_name SIG_DEF "]" | KLASS_DEF method_name SIG_DEF "]"
 * 	CONTEXT_DEF: "[" "gens" ("class_inst" class_inst) ("method_inst" method_inst) "]"
 * 	SIG_DEF: "[" "sig" TYPE_DEF ("hasthis") (TYPE_DEF)* "]"
 * 	KLASS_DEF: "[" "klass" "namespace:" namespace "name:" name ""]""
 * 	TYPE_DEF "[" "type" ("byref") type_str "]"
 * 	WRAPPER_DEF: "[" "wrapper" wrapper_type (wrapper_attribute)* "]"
 *
 * Wrapper attributes vary between all of the above DEFs, and added strings which are specific
 * to the wrapper type.
 *
 * This makes for long, difficult-to-read strings with lots of structural nesting.
 * 
 * This motivated the creation of a demangler that prints verbose information about the
 * mangled string in question.
 *
 * example mangled string:
 *     mono_underscore_aot_space__lbrack__space_method_space_asm:_space_mscorlib_space__lbrack__space_klass_space_namespace:_space_System_space_name:_space_byte_space__rbrack__space_System_dot_IConvertible_dot_ToUInt32_space__lbrack__space_sig_space__lbrack__space_type_space_u4_space__rbrack__space_hasthis_space__lbrack__space_type_space_other_space_System_dot_IFormatProvider_space__rbrack__space__rbrack__space__rbrack__space_
 *
 * example demangler output:
 *    {
 *      "category:" : "method",
 *      "assembly_name" : "mscorlib",
 *      "class" : {
 *        "category:" : "class",
 *        "namespace" : "System",
 *        "name" : "byte",
 *      },
 *      "method_name" : "System.IConvertible.ToUInt32",
 *      "signature" : {
 *        "category:" : "signature",
 *        "return_value" : {
 *          "category:" : "type",
 *          "type_name" : "MONO_TYPE_U4",
 *          "byref" : "False"
 *        },
 *        "has_this:" : "True",
 *        "parameters:" : [
 *          {
 *            "category:" : "type",
 *            "type_name" : "Other Type: System.IFormatProvider ",
 *            "byref" : "False"
 *          }
 *        ]
 *      }
 *    }
 *
 * Below are the mangler and demangler tables which are used to drive parsing and emitting.
 */

#define MonoMangleTypeTable \
	X(MONO_TYPE_VOID, "void") \
	X(MONO_TYPE_CHAR, "char") \
	X(MONO_TYPE_STRING, "str") \
	X(MONO_TYPE_I1, "i1") \
	X(MONO_TYPE_U1, "u1") \
	X(MONO_TYPE_I2, "i2") \
	X(MONO_TYPE_U2, "u2") \
	X(MONO_TYPE_I4, "i4") \
	X(MONO_TYPE_U4, "u4") \
	X(MONO_TYPE_I8, "i8") \
	X(MONO_TYPE_U8, "u8") \
	X(MONO_TYPE_I, "ii") \
	X(MONO_TYPE_U, "ui") \
	X(MONO_TYPE_R4, "fl") \
	X(MONO_TYPE_R8, "do")

#define MonoMangleWrapperTable \
	X(MONO_WRAPPER_REMOTING_INVOKE, "remoting_invoke") \
	X(MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK, "remoting_invoke_check") \
	X(MONO_WRAPPER_XDOMAIN_INVOKE, "remoting_invoke_xdomain") \
	X(MONO_WRAPPER_PROXY_ISINST, "proxy_isinst") \
	X(MONO_WRAPPER_LDFLD, "ldfld") \
	X(MONO_WRAPPER_LDFLDA, "ldflda") \
	X(MONO_WRAPPER_STFLD, "stfld") \
	X(MONO_WRAPPER_ALLOC, "alloc") \
	X(MONO_WRAPPER_WRITE_BARRIER, "write_barrier") \
	X(MONO_WRAPPER_STELEMREF, "stelemref") \
	X(MONO_WRAPPER_UNKNOWN, "unknown") \
	X(MONO_WRAPPER_MANAGED_TO_NATIVE, "man2native") \
	X(MONO_WRAPPER_SYNCHRONIZED, "synch") \
	X(MONO_WRAPPER_MANAGED_TO_MANAGED, "man2man") \
	X(MONO_WRAPPER_CASTCLASS, "castclass") \
	X(MONO_WRAPPER_RUNTIME_INVOKE, "run_invoke") \
	X(MONO_WRAPPER_DELEGATE_INVOKE, "del_inv") \
	X(MONO_WRAPPER_DELEGATE_BEGIN_INVOKE, "del_beg_inv") \
	X(MONO_WRAPPER_DELEGATE_END_INVOKE, "del_end_inv") \
	X(MONO_WRAPPER_NATIVE_TO_MANAGED, "native2man")

#define MonoMangleWrapperSubtypeTable \
	X(WRAPPER_SUBTYPE_NONE, "none") \
	X(WRAPPER_SUBTYPE_ELEMENT_ADDR, "elem_addr") \
	X(WRAPPER_SUBTYPE_STRING_CTOR, "str_ctor") \
	X(WRAPPER_SUBTYPE_VIRTUAL_STELEMREF, "virt_stelem") \
	X(WRAPPER_SUBTYPE_FAST_MONITOR_ENTER, "fast_mon_enter") \
	X(WRAPPER_SUBTYPE_FAST_MONITOR_ENTER_V4, "fast_mon_enter_4") \
	X(WRAPPER_SUBTYPE_FAST_MONITOR_EXIT, "fast_monitor_exit") \
	X(WRAPPER_SUBTYPE_PTR_TO_STRUCTURE, "ptr2struct") \
	X(WRAPPER_SUBTYPE_STRUCTURE_TO_PTR, "struct2ptr") \
	X(WRAPPER_SUBTYPE_CASTCLASS_WITH_CACHE, "castclass_w_cache") \
	X(WRAPPER_SUBTYPE_ISINST_WITH_CACHE, "isinst_w_cache") \
	X(WRAPPER_SUBTYPE_RUNTIME_INVOKE_NORMAL, "run_inv_norm") \
	X(WRAPPER_SUBTYPE_RUNTIME_INVOKE_DYNAMIC, "run_inv_dyn") \
	X(WRAPPER_SUBTYPE_RUNTIME_INVOKE_DIRECT, "run_inv_dir") \
	X(WRAPPER_SUBTYPE_RUNTIME_INVOKE_VIRTUAL, "run_inv_vir") \
	X(WRAPPER_SUBTYPE_ICALL_WRAPPER, "icall") \
	X(WRAPPER_SUBTYPE_NATIVE_FUNC_AOT, "native_func_aot") \
	X(WRAPPER_SUBTYPE_PINVOKE, "pinvoke") \
	X(WRAPPER_SUBTYPE_SYNCHRONIZED_INNER, "synch_inner") \
	X(WRAPPER_SUBTYPE_GSHAREDVT_IN, "gshared_in") \
	X(WRAPPER_SUBTYPE_GSHAREDVT_OUT, "gshared_out") \
	X(WRAPPER_SUBTYPE_ARRAY_ACCESSOR, "array_acc") \
	X(WRAPPER_SUBTYPE_GENERIC_ARRAY_HELPER, "generic_arry_help") \
	X(WRAPPER_SUBTYPE_DELEGATE_INVOKE_VIRTUAL, "del_inv_virt") \
	X(WRAPPER_SUBTYPE_DELEGATE_INVOKE_BOUND, "del_inv_bound") \
	X(WRAPPER_SUBTYPE_GSHAREDVT_IN_SIG, "gsharedvt_in_sig") \
	X(WRAPPER_SUBTYPE_GSHAREDVT_OUT_SIG, "gsharedvt_out_sig")

#define MonoMangleSanitizeTable \
	X(".", "dot") \
	X("_", "underscore") \
	X(" ", "space") \
	X("`", "bt") \
	X("<", "le") \
	X(">", "gt") \
	X("/", "sl") \
	X("[", "lbrack") \
	X("]", "rbrack") \
	X("(", "lparen") \
	X("-", "dash") \
	X(")", "rparen") \
	X(",", "comma")

#define MonoForwardMangle(FWD, BCK) \
	case FWD: \
		g_string_append_printf (s, "%s", BCK); \
		break; 

// Note the zero index, we do this to compare the
// char with the char we are switching on.
#define MonoForwardSanitize(FWD, BCK) \
	case FWD[0]: \
		g_string_append_printf (s, "_%s_", BCK); \
		break;

/*
 * Character mapping tables
 *
 * These static hash tables are lazily initialized when needed
 * and are freed when memory is freed by the runtime. (See mono_mangler_cleanup )
 */

static GHashTable *mangled_type_table = NULL;
static GHashTable *desanitize_table = NULL;

/*
 * Error Handling:
 *
 * We allow for mangling to encounter a hard failure (g_assert)
 * because we expect the mangler to be total. We should be able to
 * create a mangled string for any type, and will probably fail in a
 * place without recovery otherwise.
 *
 * Demangling is a debugging feature that we can't crash the runtime
 * for. We therefore have a delayed error handling process that saves information
 * to print about at the top-level and terminates further parsing.
 *
 * Since these can make debugging more difficult, change the "#if 0" line below
 * to cause demangling failures to become assertions.
 *
 */

// Set to cause prompt assertions during demangling errors

#if 0
#define demangle_assert(ERROR, COND) g_assert ((COND))
#else
#define demangle_assert(ERROR, COND) \
	{ \
		if (!(COND)) { \
			mono_error_set_execution_engine(ERROR, "Demangling assertion failure (%s) at (%s %d)\n", #COND, __FILE__, __LINE__); \
		} \
	}
#endif


/*
 * Mangling Functions
 *
 */

/*
 * append_mangled_type
 *
 *   Appends to the accumulating string the mangled representation of the type
 * \param s The string to append to
 * \param t The type to mangle
 *
 * \returns (boolean) Whether or not the mangling was successful
 */
static gboolean
append_mangled_type (GString *s, MonoType *t)
{
	g_string_append_printf (s, " [ type ");
	if (t->byref)
		g_string_append_printf (s, "byref ");
	switch (t->type) {
#define X(FWD, BCK) MonoForwardMangle(FWD, BCK)
	MonoMangleTypeTable
#undef X
	default: {
		char *fullname = mono_type_full_name (t);
		g_string_append_printf (s, "other %s", fullname);
		g_free (fullname);
	}
	}
	g_string_append_printf (s, " ] ");
	return TRUE;
}

/*
 * append_mangled_signature
 *
 *   Appends to the accumulating string the mangled representation of the signature
 * \param s The string to append to
 * \param sig The signature to mangle
 *
 * \returns (boolean) Whether or not the mangling was successful
 */
static gboolean
append_mangled_signature (GString *s, MonoMethodSignature *sig)
{
	int i;
	gboolean supported;

	g_string_append_printf (s, " [ sig ");
	supported = append_mangled_type (s, sig->ret);
	if (!supported)
		return FALSE;
	if (sig->hasthis)
		g_string_append_printf (s, " hasthis ");
	for (i = 0; i < sig->param_count; ++i) {
		supported = append_mangled_type (s, sig->params [i]);
		if (!supported)
			return FALSE;
	}
	g_string_append_printf (s, " ] ");

	return TRUE;
}

/*
 * append_mangled_wrapper_type
 *
 *   Appends to the accumulating string the mangled representation of the wrapper type
 * \param s The string to append to
 * \param wrapper_type The wrapper type to mangle
 *
 * \returns (boolean) Whether or not the mangling was successful
 */
static void
append_mangled_wrapper_type (GString *s, guint32 wrapper_type) 
{
	g_string_append_printf (s, "type:");
	switch (wrapper_type) {
#define X(FWD, BCK) MonoForwardMangle(FWD, BCK)
MonoMangleWrapperTable
#undef X
	default:
		g_assert_not_reached ();
	}

	g_string_append_printf (s, " ");
}

/*
 * append_mangled_wrapper_subtype
 *
 *   Appends to the accumulating string the mangled representation of the wrapper subtype
 * \param s The string to append to
 * \param subtype The wrapper subtype to mangle
 *
 * \returns (boolean) Whether or not the mangling was successful
 */
static void
append_mangled_wrapper_subtype (GString *s, WrapperSubtype subtype) 
{
	g_string_append_printf (s, "subtype:");
	switch (subtype) 
	{
#define X(FWD, BCK) MonoForwardMangle(FWD, BCK)
MonoMangleWrapperSubtypeTable
#undef X
	default:
		g_assert_not_reached ();
	}

	g_string_append_printf (s, " ");
}

static char *
sanitize_mangled_string (const char *input)
{
	GString *s = g_string_new ("");
	char prev_c = 'A'; // Not ' ', doesn't really matter what it is

	for (int i=0; input [i] != '\0'; i++) {
		char c = input [i];

		// Scrub out double spaces to make compact and consistent
		if (c == ' ' && prev_c == ' ')
			continue;
		prev_c = c;

		switch (c) {
#define X(FWD, BCK) MonoForwardSanitize(FWD, BCK)
MonoMangleSanitizeTable
#undef X
		default:
			g_string_append_c (s, c);
		}
	}

	return g_string_free (s, FALSE);
}

/*
 * append_mangled_klass
 *
 *   Appends to the accumulating string the mangled representation of the klass
 * \param s The string to append to
 * \param klass The klass to mangle
 *
 * \returns (boolean) Whether or not the mangling was successful
 */
static gboolean
append_mangled_klass (GString *s, MonoClass *klass)
{
	char *klass_desc = mono_class_full_name (klass);
	g_string_append_printf (s, " [ klass namespace: %s name: %s ] ", klass->name_space, klass_desc);
	g_free (klass_desc);

	// Success
	return TRUE;
}

static gboolean
append_mangled_method (GString *s, MonoMethod *method);

/*
 * append_mangled_wrapper
 *
 *   Appends to the accumulating string the mangled representation of the wrapper
 * \param s The string to append to
 * \param wrapper The wrapper to mangle
 *
 * \returns (boolean) Whether or not the mangling was successful
 */
static gboolean
append_mangled_wrapper (GString *s, MonoMethod *method) 
{
	gboolean success = TRUE;
	WrapperInfo *info = mono_marshal_get_wrapper_info (method);
	g_string_append_printf (s, "[ wrapper ");

	append_mangled_wrapper_type (s, method->wrapper_type);

	switch (method->wrapper_type) {
	case MONO_WRAPPER_REMOTING_INVOKE:
	case MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK:
	case MONO_WRAPPER_XDOMAIN_INVOKE: {
		MonoMethod *m = mono_marshal_method_from_wrapper (method);
		g_assert (m);
		success = success && append_mangled_method (s, m);
		break;
	}
	case MONO_WRAPPER_PROXY_ISINST:
	case MONO_WRAPPER_LDFLD:
	case MONO_WRAPPER_LDFLDA:
	case MONO_WRAPPER_STFLD: {
		g_assert (info);
		success = success && append_mangled_klass (s, info->d.proxy.klass);
		break;
	}
	case MONO_WRAPPER_ALLOC: {
		/* The GC name is saved once in MonoAotFileInfo */
		g_assert (info->d.alloc.alloc_type != -1);
		g_string_append_printf (s, " %d ", info->d.alloc.alloc_type);
		// SlowAlloc, etc
		g_string_append_printf (s, " %s ", method->name);
		break;
	}
	case MONO_WRAPPER_WRITE_BARRIER: {
		break;
	}
	case MONO_WRAPPER_STELEMREF: {
		append_mangled_wrapper_subtype (s, info->subtype);
		if (info->subtype == WRAPPER_SUBTYPE_VIRTUAL_STELEMREF)
			g_string_append_printf (s, " %d ", info->d.virtual_stelemref.kind);
		break;
	}
	case MONO_WRAPPER_UNKNOWN: {
		append_mangled_wrapper_subtype (s, info->subtype);
		if (info->subtype == WRAPPER_SUBTYPE_PTR_TO_STRUCTURE ||
			info->subtype == WRAPPER_SUBTYPE_STRUCTURE_TO_PTR)
			success = success && append_mangled_klass (s, method->klass);
		else if (info->subtype == WRAPPER_SUBTYPE_SYNCHRONIZED_INNER)
			success = success && append_mangled_method (s, info->d.synchronized_inner.method);
		else if (info->subtype == WRAPPER_SUBTYPE_ARRAY_ACCESSOR)
			success = success && append_mangled_method (s, info->d.array_accessor.method);
		else if (info->subtype == WRAPPER_SUBTYPE_GSHAREDVT_IN_SIG)
			append_mangled_signature (s, info->d.gsharedvt.sig);
		else if (info->subtype == WRAPPER_SUBTYPE_GSHAREDVT_OUT_SIG)
			append_mangled_signature (s, info->d.gsharedvt.sig);
		break;
	}
	case MONO_WRAPPER_MANAGED_TO_NATIVE: {
		append_mangled_wrapper_subtype (s, info->subtype);
		if (info->subtype == WRAPPER_SUBTYPE_ICALL_WRAPPER) {
			g_string_append_printf (s, " %s ", method->name);
		} else if (info->subtype == WRAPPER_SUBTYPE_NATIVE_FUNC_AOT) {
			success = success && append_mangled_method (s, info->d.managed_to_native.method);
		} else {
			g_assert (info->subtype == WRAPPER_SUBTYPE_NONE || info->subtype == WRAPPER_SUBTYPE_PINVOKE);
			success = success && append_mangled_method (s, info->d.managed_to_native.method);
		}
		break;
	}
	case MONO_WRAPPER_SYNCHRONIZED: {
		MonoMethod *m;

		m = mono_marshal_method_from_wrapper (method);
		g_assert (m);
		g_assert (m != method);
		success = success && append_mangled_method (s, m);
		break;
	}
	case MONO_WRAPPER_MANAGED_TO_MANAGED: {
		append_mangled_wrapper_subtype (s, info->subtype);

		if (info->subtype == WRAPPER_SUBTYPE_ELEMENT_ADDR) {
			g_string_append_printf (s, " %d ", info->d.element_addr.rank);
			g_string_append_printf (s, " %d ", info->d.element_addr.elem_size);
		} else if (info->subtype == WRAPPER_SUBTYPE_STRING_CTOR) {
			success = success && append_mangled_method (s, info->d.string_ctor.method);
		} else {
			g_assert_not_reached ();
		}
		break;
	}
	case MONO_WRAPPER_CASTCLASS: {
		append_mangled_wrapper_subtype (s, info->subtype);
		break;
	}
	case MONO_WRAPPER_RUNTIME_INVOKE: {
		append_mangled_wrapper_subtype (s, info->subtype);
		if (info->subtype == WRAPPER_SUBTYPE_RUNTIME_INVOKE_DIRECT || info->subtype == WRAPPER_SUBTYPE_RUNTIME_INVOKE_VIRTUAL)
			success = success && append_mangled_method (s, info->d.runtime_invoke.method);
		else if (info->subtype == WRAPPER_SUBTYPE_RUNTIME_INVOKE_NORMAL)
			success = success && append_mangled_signature (s, info->d.runtime_invoke.sig);
		break;
	}
	case MONO_WRAPPER_DELEGATE_INVOKE:
	case MONO_WRAPPER_DELEGATE_BEGIN_INVOKE:
	case MONO_WRAPPER_DELEGATE_END_INVOKE: {
		if (method->is_inflated) {
			/* These wrappers are identified by their class */
			g_string_append_printf (s, " i ");
			success = success && append_mangled_klass (s, method->klass);
		} else {
			WrapperInfo *info = mono_marshal_get_wrapper_info (method);

			g_string_append_printf (s, " u ");
			if (method->wrapper_type == MONO_WRAPPER_DELEGATE_INVOKE)
				append_mangled_wrapper_subtype (s, info->subtype);
			g_string_append_printf (s, " u sigstart ");
		}
		break;
	}
	case MONO_WRAPPER_NATIVE_TO_MANAGED: {
		g_assert (info);
		success = success && append_mangled_method (s, info->d.native_to_managed.method);
		success = success && append_mangled_klass (s, method->klass);
		break;
	}
	default:
		g_assert_not_reached ();
	}
	return success && append_mangled_signature (s, mono_method_signature (method));
}

/*
 * append_mangled_context
 *
 *   Appends to the accumulating string the mangled representation of the context
 * \param str The string to append to
 * \param context The context to mangle
 *
 * \returns (boolean) Whether or not the mangling was successful
 */
static gboolean
append_mangled_context (GString *str, MonoGenericContext *context)
{
	g_string_append_printf (str, "[ gens");

	gboolean good = context->class_inst && context->class_inst->type_argc > 0;
	good = good || (context->method_inst && context->method_inst->type_argc > 0);
	g_assert (good);

	if (context->class_inst) {
		g_string_append (str, " class_inst ");
		mono_ginst_get_desc (str, context->class_inst);
	}

	if (context->method_inst) {
		g_string_append (str, " method_inst ");
		mono_ginst_get_desc (str, context->method_inst);
	}
	g_string_append_printf (str, " ] ");

	return TRUE;
}	

/*
 * append_mangled_method
 *
 *   Appends to the accumulating string the mangled representation of the method
 * \param s The string to append to
 * \param method The method to mangle
 *
 * \returns (boolean) Whether or not the mangling was successful
 */
static gboolean
append_mangled_method (GString *s, MonoMethod *method)
{
	gboolean success = TRUE;

	if (method->wrapper_type)
		return append_mangled_wrapper (s, method);

	g_string_append_printf (s, "[ method asm: %s ", method->klass->image->assembly->aname.name);

	if (method->is_inflated) {
		g_string_append_printf (s, "inflated: ");
		MonoMethodInflated *imethod = (MonoMethodInflated*) method;
		g_assert (imethod->context.class_inst != NULL || imethod->context.method_inst != NULL);

		append_mangled_context (s, &imethod->context);
		g_string_append_printf (s, " declaring: ");
		success = success && append_mangled_method (s, imethod->declaring);
		g_string_append_printf (s, " ] ");

		return success;
	} else if (method->is_generic) {
		g_string_append_printf (s, "gtd: ");

		MonoGenericContainer *container = mono_method_get_generic_container (method);
		append_mangled_context (s, &container->context);
	}

	success = success && append_mangled_klass (s, method->klass);
	g_string_append_printf (s, " %s ", method->name);
	success = success && append_mangled_signature (s, mono_method_signature (method));

	g_string_append_printf (s, " ] ");

	return success;
}

/*
 * Demangling Functions
 *
 */

static int describe_mangled_context (JsonWriter *out, gchar **in, size_t len, MonoError *error);
static int describe_mangled_type (JsonWriter *out, gchar **in, size_t len, MonoError *error);
static int describe_mangled_klass (JsonWriter *out, gchar **in, size_t len, MonoError *error);
static int describe_mangled_signature (JsonWriter *out, gchar **in, size_t len, MonoError *error);
static int describe_mangled_method (JsonWriter *out, gchar **in, size_t len, MonoError *error);

/*
 * desanitize_mangled_string: 
 *   Identify characters which have been aliased when sanitizing and replace them
 *
 * \param name The sanitized, mangled string to desanitize
 *
 * \returns number of tokens consumed, -1 on failure.
 */
static char *
desanitize_mangled_string (const char *name) 
{
	if (desanitize_table == NULL) {
		GHashTable *tmp = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

#define X(FWD, BCK) g_hash_table_insert (tmp, (gpointer) g_strdup(BCK), (gpointer) g_strdup (FWD));
MonoMangleSanitizeTable
#undef X

		desanitize_table = tmp;
	}
	gchar **split = g_strsplit (name, "_", 1000);
	GString *accum = g_string_new ("");
	for (int i=0; split [i] != NULL; i++) {
		gchar *output = g_hash_table_lookup (desanitize_table, split [i]);
		if (output) {
			g_string_append_c (accum, *output);
		} else {
			g_string_append (accum, split [i]);
		}
	}
	g_strfreev(split);
	return g_string_free (accum, FALSE);
}

/*
 * decribe_mangled_wrapper: 
 *   Turn the tokens of a mangled wrapper method into a JSON object
 *
 * \param out JsonWriter that has been initialized. This emits a json Object, complete with enclosing braces
 * \param in Array of tokens of mangled string
 * \param len Length of array of tokens
 * \param error The error state
 *
 * \returns number of tokens consumed, -1 on failure.
 */
static int
describe_mangled_wrapper (JsonWriter *out, gchar **in, size_t len, MonoError *error)
{
	error_init (error);
	demangle_assert(error, !strcmp (in [0], "["));
	demangle_assert(error, !strcmp (in [1], "wrapper"));
	
	mono_json_writer_object_begin (out);

	mono_json_writer_indent (out);
	mono_json_writer_object_key (out, "category:");
	mono_json_writer_printf (out, "\"wrapper\",\n");

	mono_json_writer_indent (out);
	mono_json_writer_object_key (out, "wrapper_attributes:");
	mono_json_writer_array_begin (out);

	int i = 2;
	int depth = 0;
	while (i < len - 1) {
		if (!mono_error_ok (error))
			goto fail;

		if (!strcmp (in [i], "")) {
			i++;
			continue;
		}

		if (!strcmp (in [i], "[")) {
			int offset = 0;
			mono_json_writer_indent (out);
			if (!strcmp (in [i+1], "type"))
				offset = describe_mangled_type (out, &in [i], len - i, error);
			else if (!strcmp (in [i+1], "sig"))
				offset = describe_mangled_signature (out, &in [i], len - i, error);
			else if (!strcmp (in [i+1], "klass"))
				offset = describe_mangled_klass (out, &in [i], len - i, error);
			else if (!strcmp (in [i+1], "gens"))
				offset = describe_mangled_context (out, &in [i], len - i, error);
			else if (!strcmp (in [i+1], "method"))
				offset = describe_mangled_method (out, &in [i], len - i, error);
			else if (!strcmp (in [i+1], "wrapper"))
				offset = describe_mangled_wrapper (out, &in [i], len - i, error);
			else
				depth++;

			if (offset)
				mono_json_writer_printf (out, ",\n");

			i += offset;
			if (offset) {
				demangle_assert(error, !strcmp (in [i-1], "]"));
			} else {
				i++;
			}
		} else if (!strcmp (in [i], "]")) {
			mono_json_writer_indent (out);
			mono_json_writer_object_end (out);
			mono_json_writer_indent_pop (out);
			depth--;

			if (depth == 0)
				break;
			i++;
		} else {
			mono_json_writer_indent (out);
			mono_json_writer_printf (out, "\"%s\",\n", in [i]);
			i++;
		}
	}
	mono_json_writer_indent (out);
	mono_json_writer_array_end (out);
	mono_json_writer_printf (out, "\n");
	mono_json_writer_indent_pop (out);
	mono_json_writer_indent (out);
	mono_json_writer_object_end (out);

	return i + 1;

fail:
	return -1;
}


/*
 * decribe_mangled_type: 
 *   Turn the tokens of a mangled type into a JSON object
 *
 * \param out JsonWriter that has been initialized. This emits a json Object, complete with enclosing braces
 * \param in Array of tokens of mangled string
 * \param len Length of array of tokens
 * \param error The error state
 *
 * \returns number of tokens consumed, -1 on failure.
 */
static int
describe_mangled_type (JsonWriter *out, gchar **in, size_t len, MonoError *error)
{
	error_init (error);
	demangle_assert(error, !strcmp (in [0], "["));
	demangle_assert(error, !strcmp (in [1], "type"));
	if (!mono_error_ok (error))
		goto fail;
	int i = 2;

	gboolean byref = FALSE;
	if (!strcmp (in [i], "byref")) {
		byref = TRUE;
		i++;
	}

	if (mangled_type_table == NULL) {
		GHashTable *tmp = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
#define X(FWD, BCK) g_hash_table_insert (tmp, (gpointer) g_strdup(BCK), (gpointer) g_strdup (#FWD));
MonoMangleTypeTable
#undef X
		mangled_type_table = tmp;
	}

	GString *type_tag = g_string_new ("");
	if (!strcmp (in [i], "other")) {
		i++;
		g_string_append_printf (type_tag, "Other Type:");
		while (strcmp (in [i], "]"))
			g_string_append_printf (type_tag, " %s ", in [i++]);
	} else {
		gchar *name = g_hash_table_lookup (mangled_type_table, in [i]);
		demangle_assert(error, name);
		if (!mono_error_ok (error))
			goto fail;
		i += 1;
		g_string_append_printf (type_tag, "%s", name);
	}

	mono_json_writer_object_begin (out);

	mono_json_writer_indent (out);
	mono_json_writer_object_key (out, "category:");
	mono_json_writer_printf (out, "\"type\",\n");

	mono_json_writer_indent (out);
	mono_json_writer_object_key (out, "type_name");
	mono_json_writer_printf (out, "\"%s\",\n", type_tag->str);
	g_string_free (type_tag, TRUE);

	mono_json_writer_indent (out);
	mono_json_writer_object_key (out, "byref");
	mono_json_writer_printf (out, "\"%s\"\n", byref ? "True" : "False");

	mono_json_writer_indent_pop (out);
	mono_json_writer_indent (out);
	mono_json_writer_object_end (out);

	demangle_assert(error, !strcmp (in [i], "]"));
	if (!mono_error_ok (error))
		goto fail;

	return i + 1;

fail:
	return -1;
}

/*
 * decribe_mangled_signature: 
 *   Turn the tokens of a mangled signature into a JSON object
 *
 * \param out JsonWriter that has been initialized. This emits a json Object, complete with enclosing braces
 * \param in Array of tokens of mangled string
 * \param len Length of array of tokens
 * \param error The error state
 *
 * \returns number of tokens consumed, -1 on failure.
 */
static int
describe_mangled_signature (JsonWriter *out, gchar **in, size_t len, MonoError *error)
{
	error_init (error);
	demangle_assert(error, !strcmp (in [0], "["));
	demangle_assert(error, !strcmp (in [1], "sig"));
	if (!mono_error_ok (error))
		goto fail;

	int i = 2;

	mono_json_writer_object_begin (out);

	mono_json_writer_indent (out);
	mono_json_writer_object_key (out, "category:");
	mono_json_writer_printf (out, "\"signature\",\n");

	mono_json_writer_indent (out);
	mono_json_writer_object_key(out, "return_value");
	int ret_size = describe_mangled_type (out, &in[i], len - i, error);
	mono_json_writer_printf (out, ",\n");
	i += ret_size;

	mono_json_writer_indent (out);
	mono_json_writer_object_key(out, "has_this:");
	if (!strcmp (in [i], "hasthis")) {
		mono_json_writer_printf (out, "\"True\",\n");
		i++;
	} else {
		mono_json_writer_printf (out, "\"False\",\n");
	}

	mono_json_writer_indent (out);
	mono_json_writer_object_key(out, "parameters:");
	mono_json_writer_array_begin (out);
	gboolean first = TRUE;
	while (!strcmp (in [i], "[")) {
		if (!first)
			mono_json_writer_printf (out, ",");
		mono_json_writer_indent (out);
		first = FALSE;
		int param_size = describe_mangled_type (out, &in[i], len - i, error);
		i += param_size;
		demangle_assert(error, !strcmp (in[i-1], "]"));
	}
	mono_json_writer_printf (out, "\n");

	demangle_assert(error, !strcmp (in [i], "]"));
	if (!mono_error_ok (error))
		goto fail;

	mono_json_writer_indent_pop (out);
	mono_json_writer_indent (out);
	mono_json_writer_array_end (out);
	mono_json_writer_printf (out, "\n");

	mono_json_writer_indent (out);
	mono_json_writer_object_end (out);
	mono_json_writer_indent_pop (out);

	return i + 1;

fail:
	return -1;
}

/*
 * decribe_mangled_context: 
 *   Turn the tokens of a mangled generic context into a JSON object
 *
 * \param out JsonWriter that has been initialized. This emits a json Object, complete with enclosing braces
 * \param in Array of tokens of mangled string
 * \param len Length of array of tokens
 * \param error The error state
 *
 * \returns number of tokens consumed, -1 on failure.
 */
static int
describe_mangled_context (JsonWriter *out, gchar **in, size_t len, MonoError *error)
{
	error_init (error);
	demangle_assert(error, !strcmp (in [0], "["));
	demangle_assert(error, !strcmp (in [1], "gens"));
	if (!mono_error_ok (error))
		goto fail;

	mono_json_writer_object_begin (out);

	mono_json_writer_indent (out);
	mono_json_writer_object_key (out, "category:");
	mono_json_writer_printf (out, "\"context\",\n");

	int i = 2;
	if (!strcmp (in [i], "class_inst")) {
		mono_json_writer_indent (out);
		mono_json_writer_object_key(out, "class_inst");
		mono_json_writer_array_begin (out);
		i++;

		while (strcmp (in [i], "method_inst") && strcmp (in [i], "]")) {
			mono_json_writer_indent (out);
			mono_json_writer_printf (out, "\"%s\",\n", in [i++]);
		}
		mono_json_writer_indent_pop (out);
		mono_json_writer_indent (out);
		mono_json_writer_array_end (out);
		mono_json_writer_printf (out, "\n");
		mono_json_writer_indent (out);
	}
	if (!strcmp (in [i], "method_inst")) {
		mono_json_writer_indent (out);
		mono_json_writer_object_key(out, "method_inst");
		mono_json_writer_array_begin (out);
		i++;
		while (strcmp (in [i], "]")) {
			mono_json_writer_indent (out);
			mono_json_writer_printf (out, "\"%s\",\n", in [i++]);
		}
		mono_json_writer_indent (out);
		mono_json_writer_array_end (out);
		mono_json_writer_printf (out, "\n");
	}

	demangle_assert(error, !strcmp (in [i], "]"));
	if (!mono_error_ok (error))
		goto fail;

	mono_json_writer_indent (out);
	mono_json_writer_object_end (out);
	return i + 1;

fail:
	return -1;
}

/*
 * decribe_mangled_klass: 
 *   Turn the tokens of a mangled class into a JSON object
 *
 * \param out JsonWriter that has been initialized. This emits a json Object, complete with enclosing braces
 * \param in Array of tokens of mangled string
 * \param len Length of array of tokens
 * \param error The error state
 *
 * \returns number of tokens consumed, -1 on failure.
 */
static int
describe_mangled_klass (JsonWriter *out, gchar **in, size_t len, MonoError *error)
{
	int i = 0;
	error_init (error);
	demangle_assert(error, !strcmp (in [i++], "["));
	demangle_assert(error, !strcmp (in [i++], "klass"));
	demangle_assert(error, !strcmp (in [i++], "namespace:"));
	if (!mono_error_ok (error))
		goto fail;

	mono_json_writer_object_begin (out);

	mono_json_writer_indent (out);
	mono_json_writer_object_key (out, "category:");
	mono_json_writer_printf (out, "\"class\",\n");

	mono_json_writer_indent (out);
	mono_json_writer_object_key(out, "namespace");
	mono_json_writer_printf (out, "\"");

	while (strcmp (in [i], "name:"))
		mono_json_writer_printf (out, "%s", in [i++]);
	mono_json_writer_printf (out, "\",\n");

	demangle_assert(error, !strcmp (in [i], "name:"));
	if (!mono_error_ok (error))
		goto fail;

	mono_json_writer_indent (out);
	mono_json_writer_object_key(out, "name");
	mono_json_writer_printf (out, "\"");
	i++;

	while (strcmp (in [i], "]"))
		mono_json_writer_printf (out, "%s", in [i++]);
	mono_json_writer_printf (out, "\",\n");

	demangle_assert(error, !strcmp (in [i], "]"));
	if (!mono_error_ok (error))
		goto fail;

	mono_json_writer_indent_pop (out);
	mono_json_writer_indent (out);
	mono_json_writer_object_end (out);

	return i + 1;

fail:
	return -1;
}

/*
 * decribe_mangled_method: 
 *   Turn the tokens of a mangled method into a JSON object
 *
 * \param out JsonWriter that has been initialized. This emits a json Object, complete with enclosing braces
 * \param in Array of tokens of mangled string
 * \param len Length of array of tokens
 * \param error The error state
 *
 * \returns number of tokens consumed, -1 on failure.
 */
static int
describe_mangled_method (JsonWriter *out, gchar **in, size_t len, MonoError *error)
{
	error_init (error);
	if (len > 2 && !strcmp (in [0], "[") && !strcmp (in [1], "wrapper"))
		return describe_mangled_wrapper (out, in, len, error);

	demangle_assert(error, !(len < 3 || strcmp (in [0], "[") || strcmp (in [1], "method") || strcmp (in [2], "asm:")));
	if (!mono_error_ok (error))
		goto fail;

	gchar *aname = in [3];
	int i = 4;

	mono_json_writer_object_begin(out);

	mono_json_writer_indent (out);
	mono_json_writer_object_key (out, "category:");
	mono_json_writer_printf (out, "\"method\",\n");

	mono_json_writer_indent (out);
	mono_json_writer_object_key(out, "assembly_name");
	mono_json_writer_printf (out, "\"%s\",\n", aname);

	if (!strcmp (in [i], "inflated:")) {
		i++;
		mono_json_writer_indent (out);
		mono_json_writer_object_key(out, "context");
		int cont_off = describe_mangled_context (out, &in [i], len - i, error);
		mono_json_writer_printf (out, ",\n");
		i += cont_off;

		demangle_assert(error, !strcmp (in [i++], "declaring:"));
		if (!mono_error_ok (error))
			goto fail;

		mono_json_writer_indent (out);
		mono_json_writer_object_key(out, "declaring");
		int declaring_off = describe_mangled_method (out, &in [i], len - i, error);
		mono_json_writer_printf (out, ",\n");
		i += declaring_off;
	
		demangle_assert(error, !strcmp (in [i], "]"));
		if (!mono_error_ok (error))
			goto fail;

		mono_json_writer_indent (out);
		mono_json_writer_indent_pop (out);
		mono_json_writer_object_end(out);

		return i + 1;
	} else if (!strcmp (in [i], "gtd:")) {
		i++;
		mono_json_writer_indent (out);
		mono_json_writer_object_key(out, "context");
		int cont_off = describe_mangled_context (out, &in [i], len - i, error);
		mono_json_writer_printf (out, ",\n");
		i += cont_off;
	}

	// decode class
	demangle_assert(error, !strcmp (in [i], "["));
	if (!mono_error_ok (error))
		goto fail;

	mono_json_writer_indent (out);
	mono_json_writer_object_key(out, "class");
	int class_off = describe_mangled_klass (out, &in[i], len - i, error);
	mono_json_writer_printf (out, ",\n");
	i += class_off;
	// Ensure class terminated correctly
	demangle_assert(error, !strcmp (in [i-1], "]"));
	if (!mono_error_ok (error))
		goto fail;

	gchar *method_name = in [i++];
	mono_json_writer_indent (out);
	mono_json_writer_object_key(out, "method_name");
	mono_json_writer_printf (out, "\"%s\",\n", method_name);

	demangle_assert(error, !strcmp (in [i], "["));
	if (!mono_error_ok (error))
		goto fail;

	mono_json_writer_indent (out);
	mono_json_writer_object_key(out, "signature");
	int sig_off = describe_mangled_signature (out, &in [i], len - i, error);
	mono_json_writer_printf (out, "\n");
	i += sig_off;

	// Ensure signature terminated correctly
	demangle_assert(error, !strcmp (in [i-1], "]"));

	// Ensure delimiters honored
	demangle_assert(error, !strcmp (in [i], "]"));

	if (!mono_error_ok (error))
		goto fail;

	mono_json_writer_indent (out);
	mono_json_writer_object_end (out);

	return i + 1;

fail:
	return -1;
}

/*
 * demangle_method
 *
 *  Either fails, prints an error, and returns NULL, or 
 *  returns a json-ish summary of the mangled type
 *
 * \param verbosity The degree of verbosity to dump. 1 = s-expression, 2 = json
 * \param name The mangled name of the method to demangle
 *
 */
static char *
demangle_method (const char *name, int verbosity)
{
	gchar *str = desanitize_mangled_string (name);
	if (verbosity < 2)
		return str;

	gchar **tokens = g_strsplit (str, " ", strlen (name));
	int len;
	for (len=0; tokens [len] != NULL; len++);

	MonoError error;
	error_init (&error);
	demangle_assert(&error, !strcmp (tokens [0], "mono_aot"));

	JsonWriter json;
	mono_json_writer_init (&json);
	int length = describe_mangled_method (&json, &tokens [1], len, &error);

	gchar *ret = NULL;
	if (mono_error_ok (&error)) {
		ret = g_strdup (json.text->str);
		mono_json_writer_destroy (&json);
		g_assert (length > 0);
	} else {
		fprintf (stderr, "Unable to demangle method because %s. Mangled string: (%s) \n",
		    mono_error_get_message (&error), name);
	}

	mono_error_cleanup (&error);
	g_strfreev (tokens);
	g_free (str);

	return ret;
}

/*
 * dump_mangled_method
 *
 *  Print a json-ish summary of the mangled type
 *
 * \param verbosity The degree of verbosity to dump. 1 = s-expression, 2 = json
 * \param name The mangled name of the method to dump and print
 */
void
dump_mangled_method (const char *name, int verbosity)
{
	char *tmp = demangle_method (name, verbosity);
	if (tmp) {
		fprintf (stderr, "\tDemangled:\n%s\n\n", tmp);
		g_free (tmp);
	}
}

/*
 * mono_aot_get_mangled_method_name:
 *
 *   Return a unique mangled name for METHOD, or NULL.
 *
 * \param method The MonoMethod to mangle
 */
char*
mono_aot_get_mangled_method_name (MonoMethod *method)
{
	GString *s = g_string_new ("mono_aot ");
	if (!append_mangled_method (s, method)) {
		g_string_free (s, TRUE);
		return NULL;
	} else {
		char *out = g_string_free (s, FALSE);
		char *cleaned = sanitize_mangled_string (out);

		// To test demangling
		// dump_mangled_method (cleaned, 1); // s-expression
		// dump_mangled_method (cleaned, 2); // json 

		g_free (out);
		return cleaned;
	}
}

/*
 * mono_mangler_cleanup
 *
 *   Clean up the state associated with the demangler caches
 */
void
mono_mangler_cleanup (void)
{
	if (desanitize_table) {
		g_hash_table_destroy (desanitize_table);
		desanitize_table = NULL;
	}
	if (mangled_type_table) {
		g_hash_table_destroy (mangled_type_table);
		mangled_type_table = NULL;
	}
}


