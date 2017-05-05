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


/*
 * Mono Method Mangler:
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
 * Format:
 * WrapperTypeEncoding: Packed string tag found in encoding table
 * WrapperSubTypeEncoding: Packed string tag found in encoding table
 * TypeEncoding: Packed string tag found in encoding table
 * SignatureEncoding
 *
 * aot: "aot [ " <MethodEncoding> " ]"
 *
 * MethodEncoding: <NormalMethodEncoding> | <WrapperEncoding>
 *
 * WrapperEncoding: "wrapper " <WrapperTypeEncoding> " [ " <WrapperSpecificEncoding> " ] "
 * WrapperSpecificEncoding: Types, Method names, Mangled Method references,
 * 	and other wrapper attributes specific to the type. This may include a subtype encoding tag.
 * 	if so, it will prefix the other wrapper specific encoding attributes.
 *
 * NormalMethodEncoding:
 *
 * Below we have a bidirectional compiler between metadata descriptions and
 * mangled strings
 */

#include "config.h"
#include "aot-compiler.h"

#define MonoForwardMangle(FWD, BCK) \
	case FWD: \
		g_string_append_printf (s, "BCK"); \
		break; 

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
	X(WRAPPER_SUBTYPE_NONE, "") \
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

static gboolean
append_mangled_type (GString *s, MonoType *t)
{
	g_string_append_printf (s, "[ type ");
	if (t->byref)
		g_string_append_printf (s, "byref ");
	switch (t->type) {
#define X(FWD, BCK) MonoForwardMangle(FWD, BCK)
	MonoMangleTypeTable
#undef X
	default: {
		char *fullname = mono_type_full_name (t);
		g_string_append_printf (s, "other %x %s", strlen (fullname), fullname);
	}
	}
	g_string_append_printf (s, " ]");
	return TRUE;
}

static gboolean
append_mangled_signature (GString *s, MonoMethodSignature *sig)
{
	int i;
	gboolean supported;

	g_string_append_printf (s, "[ sig ");
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
	g_string_append_printf (s, " ]");

	return TRUE;
}

static void
append_mangled_wrapper_type (GString *s, guint32 wrapper_type) 
{
	switch (wrapper_type) {
#define X(FWD, BCK) MonoForwardMangle(FWD, BCK)
MonoMangleWrapperTable
#undef X
	default:
		g_assert_not_reached ();
	}

	g_string_append_printf (s, " ");
}

static void
append_mangled_wrapper_subtype (GString *s, WrapperSubtype subtype) 
{
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

#define MonoForwardSanitize(FWD, BCK) \
	case FWD: \
		g_string_append_printf (s, "_%s_", BCK); \
		break;

#define MonoMangleSanitizeTable \
	X('.', "dot") \
	X('_', "underscore") \
	X(' ', "space") \
	X('`', "bt") \
	X('<', "le") \
	X('>', "gt") \
	X('/', "sl") \
	X('[', "lbrack") \
	X(']', "rbrack") \
	X('(', "lparen") \
	X('-', "dash") \
	X(')', "rparen") \
	X(',', "comma")

// Keep in sync with above
#define num_sanitize_cases 13

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

static char *
desanitize_mangled_string (char *name) 
{
	static GHashTable *tbl = NULL;
	if (tbl == NULL) {
		GHashTable *tmp = g_hash_table_new (g_str_hash, g_str_equal);
		int i = 0;
		gchar *mem = g_malloc (sizeof (gchar) * num_sanitize_cases);
#define X(FWD, BCK) mem [i++] = FWD;
MonoMangleSanitizeTable
#undef X
		i = 0;
#define X(FWD, BCK) g_hash_table_insert (tmp, (gpointer) g_strdup(BCK), (gpointer) &mem [i++]);
MonoMangleSanitizeTable
#undef X
		tbl = tmp;
	}
	gchar **split = g_strsplit (name, "_", 1000);
	GString *accum = g_string_new ("");
	for (int i=0; split [i] != NULL; i++) {
		gchar *output = g_hash_table_lookup (tbl, split [i]);
		if (output) {
			g_string_append_c (accum, *output);
		} else {
			g_string_append (accum, split [i]);
		}
	}
	g_strfreev(split);
	return g_string_free (accum, FALSE);
}

static gboolean
append_mangled_klass (GString *s, MonoClass *klass)
{
	char *klass_desc = mono_class_full_name (klass);
	g_string_append_printf (s, "[ klass namespace: %s name: %s ]", klass->name_space, klass_desc);
	g_free (klass_desc);

	// Success
	return TRUE;
}

static gboolean
append_mangled_method (GString *s, MonoMethod *method);

static gboolean
append_mangled_wrapper (GString *s, MonoMethod *method) 
{
	gboolean success = TRUE;
	WrapperInfo *info = mono_marshal_get_wrapper_info (method);
	g_string_append_printf (s, "wrapper_");

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
		g_string_append_printf (s, "%d_", info->d.alloc.alloc_type);
		// SlowAlloc, etc
		g_string_append_printf (s, "%s_", method->name);
		break;
	}
	case MONO_WRAPPER_WRITE_BARRIER: {
		break;
	}
	case MONO_WRAPPER_STELEMREF: {
		append_mangled_wrapper_subtype (s, info->subtype);
		if (info->subtype == WRAPPER_SUBTYPE_VIRTUAL_STELEMREF)
			g_string_append_printf (s, "%d", info->d.virtual_stelemref.kind);
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
			g_string_append_printf (s, "%s", method->name);
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
			g_string_append_printf (s, "%d_", info->d.element_addr.rank);
			g_string_append_printf (s, "%d_", info->d.element_addr.elem_size);
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
			g_string_append_printf (s, "i_");
			success = success && append_mangled_klass (s, method->klass);
		} else {
			WrapperInfo *info = mono_marshal_get_wrapper_info (method);

			g_string_append_printf (s, "u_");
			if (method->wrapper_type == MONO_WRAPPER_DELEGATE_INVOKE)
				append_mangled_wrapper_subtype (s, info->subtype);
			g_string_append_printf (s, "u_sigstart");
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

static void
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
}	

// Tells you the length of the area inside of the parenthesis
// the array of strings contains
static int
extract_inside_parens (gchar **in, int len)
{
	// FIXME: Error handling
	if (len < 2 || strcmp (in [0], "[")) 
		g_assert_not_reached ();
	int i;

	int nesting = 0;
	for (i = 1; i < len; i++) {
		if (!strcmp (in [i], "]"))
			if (nesting == 0)
				break;
			else
				nesting--;
		else if (!strcmp (in [i], "["))
			nesting++;
	}
	g_assert (nesting == 0);

	if (i == len)
		g_assert_not_reached ();
	else
		i++; // Count closing bracket
	return i;
}


static int
describe_mangled_sig (GString *out, gchar **in, size_t len)
{
	return extract_inside_parens (in, len);
}

static int
describe_mangled_context (GString *out, gchar **in, size_t len)
{
	g_assert (!strcmp (in [0], "["));
	g_assert (!strcmp (in [1], "gens"));
	int i = 2;
	if (!strcmp (in [i], "class_inst")) {
		g_string_append_printf (out, "class_inst: \"%s\"", in [i+1]);
		i += 2;
	}
	if (!strcmp (in [i], "method_inst")) {
		g_string_append_printf (out, "method_inst: \"%s\"", in [i+1]);
		i += 2;
	}
	g_assert (!strcmp (in [i], "]"));
	return i + 1;
}

static int
describe_mangled_klass (GString *out, gchar **in, size_t len)
{
	int i = 0;
	g_assert (!strcmp (in [i++], "["));
	g_assert (!strcmp (in [i++], "klass"));
	g_assert (!strcmp (in [i++], "namespace:"));

	gchar *name = "";
	gchar *name_space = "";

	if (!strcmp (in [i], "name:")) {
		name = in [i+1];
		i += 2;
	} else {
		name_space = in [i+1];
		i += 1;
	}
	if (!strcmp (in [i], "name:")) {
		name = in [i++];
	}
	g_assert (!strcmp (in [i++], "]"));
	g_string_append_printf (out, "namespace: \"%s\", name: \"%s\"", name_space, name);
	return i;
}

/*
 * decribe_mangled_method: 
 *
 * s: Array of strings
 * len: length of array
 * Returns: number of tokens consumed
 * -1 on failure.
 */
static int
describe_mangled_method (GString *out, gchar **in, size_t len)
{
	if (len < 5)
		g_assert_not_reached ();

	// FIXME wrappers
	if (strcmp (in [0], "[") || strcmp (in [1], "method") || strcmp (in [2], "asm:"))
	    g_assert_not_reached ();

	gchar *aname = in [3];
	int i = 4;

	GString *mangled_context = g_string_new ("");
	GString *nested_mangled_method;

	if (!strcmp (in [i], "inflated:")) {
		i++;
		int cont_off = describe_mangled_context (mangled_context, &in [i], len - i);
		i += cont_off;

		if (strcmp (in [i++], "declaring:"))
			g_assert_not_reached ();

		nested_mangled_method = g_string_new ("");
		int declaring_off = describe_mangled_method (nested_mangled_method, &in [i], len - i);
		i += declaring_off;
	
		if (strcmp (in [i], "]"))
			g_assert_not_reached ();

		g_string_append_printf (out, "context: %s declaring: { \n\t%s} ", mangled_context->str, nested_mangled_method->str);

		return i;
	} else if (!strcmp (in [i], "gtd:")) {
		int cont_off = describe_mangled_context (mangled_context, &in [i], len - i);
		i += cont_off;
		g_string_append_printf (out, "context: %s ", cont_off);
	}

	// decode class
	GString *class_decoded = g_string_new ("");
	if (strcmp (in [i], "["))
		g_assert_not_reached ();
	int class_off = describe_mangled_klass (class_decoded, &in[i], len - i);
	i += class_off;
	// Ensure class terminated correctly
	if (strcmp (in [i-1], "]"))
		g_assert_not_reached ();

	gchar *method_name = in [i++];

	GString *signature = g_string_new ("");
	if (strcmp (in [i], "["))
		g_assert_not_reached ();
	int sig_off = describe_mangled_sig (signature, &in [i], len - i);
	i += sig_off;
	// Ensure signature terminated correctly
	if (strcmp (in [i-1], "]"))
		g_assert_not_reached ();

	// Ensure delimiters honored
	if (strcmp (in [i], "]"))
		g_assert_not_reached ();

	g_string_append_printf (out, "aname: %s name: %s, sig: %s, class: %s\n", aname, method_name, signature->str, class_decoded->str);

	return i;
}

// Todo: add as run option for mono, step that only decodes mangled

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
		g_string_append_printf (s, "gtd:");

		MonoGenericContainer *container = mono_method_get_generic_container (method);
		append_mangled_context (s, &container->context);
	}

	success = success && append_mangled_klass (s, method->klass);
	g_string_append_printf (s, " %s ", method->name);
	success = success && append_mangled_signature (s, mono_method_signature (method));

	g_string_append_printf (s, " ] ");

	return success;
}

static char *
mono_get_mangled_method (char *name)
{
	gchar *str = desanitize_mangled_string (name);

	gchar **tokens = g_strsplit (str, " ", strlen (name));
	int len;
	for (len=0; tokens [len] != NULL; len++);

	GString *json = g_string_new ("");
	g_assert (!strcmp (tokens [0], "mono_aot"));
	g_assert (describe_mangled_method (json, &tokens [1], len) > 0);
	char *tmp = g_string_free (json, FALSE);
	g_strfreev (tokens);
	g_free (str);

	return tmp;
}

void
dump_mangled_method (char *name)
{
	char *tmp = mono_get_mangled_method (name);
	fprintf (stderr, "Mangled Method Encodes: %s", tmp);
	g_free (tmp);
}

/*
 * mono_aot_get_mangled_method_name:
 *
 *   Return a unique mangled name for METHOD, or NULL.
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
		char *reverse = desanitize_mangled_string (cleaned);

		dump_mangled_method (cleaned);
		fprintf (stderr, "Input (%s) output (%s) bidirectional (%s)\n", out, cleaned, reverse);
		g_assert_not_reached ();
		g_free (out);
		return cleaned;
	}
}


