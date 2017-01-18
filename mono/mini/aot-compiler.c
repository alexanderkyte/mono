/*
 * aot-compiler.c: mono Ahead of Time compiler
 *
 * Author:
 *   Dietmar Maurer (dietmar@ximian.com)
 *   Zoltan Varga (vargaz@gmail.com)
 *   Johan Lorensson (lateralusx.github@gmail.com)
 *
 * (C) 2002 Ximian, Inc.
 * Copyright 2003-2011 Novell, Inc 
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "config.h"
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#ifndef HOST_WIN32
#include <sys/time.h>
#else
#include <winsock2.h>
#include <windows.h>
#endif

#include <errno.h>
#include <sys/stat.h>

#include <mono/metadata/abi-details.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/class.h>
#include <mono/metadata/object.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/reflection-internals.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/gc-internals.h>
#include <mono/metadata/mempool-internals.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/threads-types.h>
#include <mono/utils/mono-logger-internals.h>
#include <mono/utils/mono-compiler.h>
#include <mono/utils/mono-time.h>
#include <mono/utils/mono-mmap.h>
#include <mono/utils/mono-rand.h>
#include <mono/utils/json.h>
#include <mono/utils/mono-threads-coop.h>
#include <mono/profiler/mono-profiler-aot.h>
#include <mono/utils/w32api.h>

#include "aot-compiler.h"
#include "seq-points.h"
#include "image-writer.h"
#include "dwarfwriter.h"
#include "mini-gc.h"
#include "mini-llvm.h"

#if !defined(DISABLE_AOT) && !defined(DISABLE_JIT)

// Use MSVC toolchain, Clang for MSVC using MSVC codegen and linker, when compiling for AMD64
// targeting WIN32 platforms running AOT compiler on WIN32 platform with VS installation.
#if defined(TARGET_AMD64) && defined(TARGET_WIN32) && defined(HOST_WIN32) && defined(_MSC_VER)
#define TARGET_X86_64_WIN32_MSVC
#endif

#if defined(TARGET_X86_64_WIN32_MSVC)
#define TARGET_WIN32_MSVC
#endif

#if defined(__linux__) || defined(__native_client_codegen__)
#define RODATA_SECT ".rodata"
#elif defined(TARGET_MACH)
#define RODATA_SECT ".section __TEXT, __const"
#elif defined(TARGET_WIN32_MSVC)
#define RODATA_SECT ".rdata"
#else
#define RODATA_SECT ".text"
#endif

#define TV_DECLARE(name) gint64 name
#define TV_GETTIME(tv) tv = mono_100ns_ticks ()
#define TV_ELAPSED(start,end) (((end) - (start)) / 10)

#define ALIGN_TO(val,align) ((((guint64)val) + ((align) - 1)) & ~((align) - 1))
#define ALIGN_PTR_TO(ptr,align) (gpointer)((((gssize)(ptr)) + (align - 1)) & (~(align - 1)))
#define ROUND_DOWN(VALUE,SIZE)	((VALUE) & ~((SIZE) - 1))

typedef struct {
	char *name;
	MonoImage *image;
} ImageProfileData;

typedef struct ClassProfileData ClassProfileData;

typedef struct {
	int argc;
	ClassProfileData **argv;
	MonoGenericInst *inst;
} GInstProfileData;

struct ClassProfileData {
	ImageProfileData *image;
	char *ns, *name;
	GInstProfileData *inst;
	MonoClass *klass;
};

typedef struct {
	ClassProfileData *klass;
	int id;
	char *name;
	int param_count;
	char *signature;
	GInstProfileData *inst;
	MonoMethod *method;
} MethodProfileData;

typedef struct {
	GHashTable *images, *classes, *ginsts, *methods;
} ProfileData;

/* predefined values for static readonly fields without needed to run the .cctor */
typedef struct _ReadOnlyValue ReadOnlyValue;
struct _ReadOnlyValue {
	ReadOnlyValue *next;
	char *name;
	int type; /* to be used later for typechecking to prevent user errors */
	union {
		guint8 i1;
		guint16 i2;
		guint32 i4;
		guint64 i8;
		gpointer ptr;
	} value;
};
static ReadOnlyValue *readonly_values;

typedef struct MonoAotOptions {
	char *outfile;
	char *llvm_outfile;
	char *data_outfile;
	GList *profile_files;
	gboolean save_temps;
	gboolean write_symbols;
	gboolean metadata_only;
	gboolean bind_to_runtime_version;
	MonoAotMode mode;
	gboolean no_dlsym;
	gboolean static_link;
	gboolean asm_only;
	gboolean asm_writer;
	gboolean nodebug;
	gboolean dwarf_debug;
	gboolean soft_debug;
	gboolean log_generics;
	gboolean log_instances;
	gboolean gen_msym_dir;
	char *gen_msym_dir_path;
	gboolean direct_pinvoke;
	gboolean direct_icalls;
	gboolean no_direct_calls;
	gboolean use_trampolines_page;
	gboolean no_instances;
	gboolean gnu_asm;
	gboolean llvm;
	gboolean llvm_only;
	int nthreads;
	int ntrampolines;
	int nrgctx_trampolines;
	int nimt_trampolines;
	int ngsharedvt_arg_trampolines;
	int nrgctx_fetch_trampolines;
	gboolean print_skipped_methods;
	gboolean stats;
	gboolean verbose;
	char *tool_prefix;
	char *ld_flags;
	char *mtriple;
	char *llvm_path;
	char *temp_path;
	char *instances_logfile_path;
	char *logfile;
	gboolean dump_json;
	gboolean profile_only;
} MonoAotOptions;

typedef enum {
	METHOD_CAT_NORMAL,
	METHOD_CAT_GSHAREDVT,
	METHOD_CAT_INST,
	METHOD_CAT_WRAPPER,
	METHOD_CAT_NUM
} MethodCategory;

typedef struct MonoAotStats {
	int ccount, mcount, lmfcount, abscount, gcount, ocount, genericcount;
	gint64 code_size, info_size, ex_info_size, unwind_info_size, got_size, class_info_size, got_info_size, plt_size;
	int methods_without_got_slots, direct_calls, all_calls, llvm_count;
	int got_slots, offsets_size;
	int method_categories [METHOD_CAT_NUM];
	int got_slot_types [MONO_PATCH_INFO_NUM];
	int got_slot_info_sizes [MONO_PATCH_INFO_NUM];
	int jit_time, gen_time, link_time;
} MonoAotStats;

typedef struct GotInfo {
	GHashTable *patch_to_got_offset;
	GHashTable **patch_to_got_offset_by_type;
	GPtrArray *got_patches;
} GotInfo;

typedef struct MonoAotCompile {
	MonoImage *image;
	GPtrArray *methods;
	GHashTable *method_indexes;
	GHashTable *method_depth;
	MonoCompile **cfgs;
	int cfgs_size;
	GHashTable **patch_to_plt_entry;
	GHashTable *plt_offset_to_entry;
	//GHashTable *patch_to_got_offset;
	//GHashTable **patch_to_got_offset_by_type;
	//GPtrArray *got_patches;
	GotInfo got_info, llvm_got_info;
	GHashTable *image_hash;
	GHashTable *method_to_cfg;
	GHashTable *token_info_hash;
	GHashTable *method_to_pinvoke_import;
	GPtrArray *extra_methods;
	GPtrArray *image_table;
	GPtrArray *globals;
	GPtrArray *method_order;
	GHashTable *export_names;
	/* Maps MonoClass* -> blob offset */
	GHashTable *klass_blob_hash;
	/* Maps MonoMethod* -> blob offset */
	GHashTable *method_blob_hash;
	GHashTable *gsharedvt_in_signatures;
	GHashTable *gsharedvt_out_signatures;
	guint32 *plt_got_info_offsets;
	guint32 got_offset, llvm_got_offset, plt_offset, plt_got_offset_base, nshared_got_entries;
	/* Number of GOT entries reserved for trampolines */
	guint32 num_trampoline_got_entries;
	guint32 tramp_page_size;

	guint32 table_offsets [MONO_AOT_TABLE_NUM];
	guint32 num_trampolines [MONO_AOT_TRAMP_NUM];
	guint32 trampoline_got_offset_base [MONO_AOT_TRAMP_NUM];
	guint32 trampoline_size [MONO_AOT_TRAMP_NUM];
	guint32 tramp_page_code_offsets [MONO_AOT_TRAMP_NUM];

	MonoAotOptions aot_opts;
	guint32 nmethods;
	guint32 opts;
	guint32 simd_opts;
	MonoMemPool *mempool;
	MonoAotStats stats;
	int method_index;
	char *static_linking_symbol;
	mono_mutex_t mutex;
	gboolean gas_line_numbers;
	/* Whenever to emit an object file directly from llc */
	gboolean llvm_owriter;
	gboolean llvm_owriter_supported;
	MonoImageWriter *w;
	MonoDwarfWriter *dwarf;
	FILE *fp;
	char *tmpbasename;
	char *tmpfname;
	char *llvm_sfile;
	char *llvm_ofile;
	GSList *cie_program;
	GHashTable *unwind_info_offsets;
	GPtrArray *unwind_ops;
	guint32 unwind_info_offset;
	char *global_prefix;
	char *got_symbol;
	char *llvm_got_symbol;
	char *plt_symbol;
	char *llvm_eh_frame_symbol;
	GHashTable *method_label_hash;
	const char *temp_prefix;
	const char *user_symbol_prefix;
	const char *llvm_label_prefix;
	const char *inst_directive;
	int align_pad_value;
	guint32 label_generator;
	gboolean llvm;
	gboolean has_jitted_code;
	MonoAotFileFlags flags;
	MonoDynamicStream blob;
	gboolean blob_closed;
	GHashTable *typespec_classes;
	GString *llc_args;
	GString *as_args;
	char *assembly_name_sym;
	GHashTable *plt_entry_debug_sym_cache;
	gboolean thumb_mixed, need_no_dead_strip, need_pt_gnu_stack;
	GHashTable *ginst_hash;
	GHashTable *dwarf_ln_filenames;
	gboolean global_symbols;
	int objc_selector_index, objc_selector_index_2;
	GPtrArray *objc_selectors;
	GHashTable *objc_selector_to_index;
	GList *profile_data;
	GHashTable *profile_methods;
	FILE *logfile;
	FILE *instances_logfile;
	FILE *data_outfile;
	int datafile_offset;
	int gc_name_offset;
} MonoAotCompile;

typedef struct {
	int plt_offset;
	char *symbol, *llvm_symbol, *debug_sym;
	MonoJumpInfo *ji;
	gboolean jit_used, llvm_used;
} MonoPltEntry;

#define mono_acfg_lock(acfg) mono_os_mutex_lock (&((acfg)->mutex))
#define mono_acfg_unlock(acfg) mono_os_mutex_unlock (&((acfg)->mutex))

/* This points to the current acfg in LLVM mode */
static MonoAotCompile *llvm_acfg;

#ifdef HAVE_ARRAY_ELEM_INIT
#define MSGSTRFIELD(line) MSGSTRFIELD1(line)
#define MSGSTRFIELD1(line) str##line
static const struct msgstr_t {
#define PATCH_INFO(a,b) char MSGSTRFIELD(__LINE__) [sizeof (b)];
#include "patch-info.h"
#undef PATCH_INFO
} opstr = {
#define PATCH_INFO(a,b) b,
#include "patch-info.h"
#undef PATCH_INFO
};
static const gint16 opidx [] = {
#define PATCH_INFO(a,b) [MONO_PATCH_INFO_ ## a] = offsetof (struct msgstr_t, MSGSTRFIELD(__LINE__)),
#include "patch-info.h"
#undef PATCH_INFO
};

static G_GNUC_UNUSED const char*
get_patch_name (int info)
{
	return (const char*)&opstr + opidx [info];
}

#else
#define PATCH_INFO(a,b) b,
static const char* const
patch_types [MONO_PATCH_INFO_NUM + 1] = {
#include "patch-info.h"
	NULL
};

static G_GNUC_UNUSED const char*
get_patch_name (int info)
{
	return patch_types [info];
}

#endif

static guint32
get_unwind_info_offset (MonoAotCompile *acfg, guint8 *encoded, guint32 encoded_len);

static char*
get_plt_entry_debug_sym (MonoAotCompile *acfg, MonoJumpInfo *ji, GHashTable *cache);

static void
add_gsharedvt_wrappers (MonoAotCompile *acfg, MonoMethodSignature *sig, gboolean gsharedvt_in, gboolean gsharedvt_out);

static void
add_profile_instances (MonoAotCompile *acfg, ProfileData *data);

static void
aot_printf (MonoAotCompile *acfg, const gchar *format, ...)
{
	FILE *output;
	va_list args;

	if (acfg->logfile)
		output = acfg->logfile;
	else
		output = stdout;

	va_start (args, format);
	vfprintf (output, format, args);
	va_end (args);
}

static void
aot_printerrf (MonoAotCompile *acfg, const gchar *format, ...)
{
	FILE *output;
	va_list args;

	if (acfg->logfile)
		output = acfg->logfile;
	else
		output = stderr;

	va_start (args, format);
	vfprintf (output, format, args);
	va_end (args);
}

static void
report_loader_error (MonoAotCompile *acfg, MonoError *error, const char *format, ...)
{
	FILE *output;
	va_list args;

	if (mono_error_ok (error))
		return;

	if (acfg->logfile)
		output = acfg->logfile;
	else
		output = stderr;

	va_start (args, format);
	vfprintf (output, format, args);
	va_end (args);
	mono_error_cleanup (error);

	g_error ("FullAOT cannot continue if there are loader errors");
}

/* Wrappers around the image writer functions */

#define MAX_SYMBOL_SIZE 256

static inline const char *
mangle_symbol (const char * symbol, char * mangled_symbol, gsize length)
{
	gsize needed_size = length;

	g_assert (NULL != symbol);
	g_assert (NULL != mangled_symbol);
	g_assert (0 != length);

#if defined(TARGET_WIN32) && defined(TARGET_X86)
	if (symbol && '_' != symbol [0]) {
		needed_size = g_snprintf (mangled_symbol, length, "_%s", symbol);
	} else {
		needed_size = g_snprintf (mangled_symbol, length, "%s", symbol);
	}
#else
	needed_size = g_snprintf (mangled_symbol, length, "%s", symbol);
#endif

	g_assert (0 <= needed_size && needed_size < length);
	return mangled_symbol;
}

static inline char *
mangle_symbol_alloc (const char * symbol)
{
	g_assert (NULL != symbol);

#if defined(TARGET_WIN32) && defined(TARGET_X86)
	if (symbol && '_' != symbol [0]) {
		return g_strdup_printf ("_%s", symbol);
	}
	else {
		return g_strdup_printf ("%s", symbol);
	}
#else
	return g_strdup_printf ("%s", symbol);
#endif
}

static inline void
emit_section_change (MonoAotCompile *acfg, const char *section_name, int subsection_index)
{
	mono_img_writer_emit_section_change (acfg->w, section_name, subsection_index);
}

#if defined(TARGET_WIN32) && defined(TARGET_X86)

static inline void
emit_local_symbol (MonoAotCompile *acfg, const char *name, const char *end_label, gboolean func)
{
	const char * mangled_symbol_name = name;
	char * mangled_symbol_name_alloc = NULL;

	if (TRUE == func) {
		mangled_symbol_name_alloc = mangle_symbol_alloc (name);
		mangled_symbol_name = mangled_symbol_name_alloc;
	}

	if (name != mangled_symbol_name && 0 != g_strcasecmp (name, mangled_symbol_name)) {
		mono_img_writer_emit_label (acfg->w, mangled_symbol_name);
	}
	mono_img_writer_emit_local_symbol (acfg->w, mangled_symbol_name, end_label, func);

	if (NULL != mangled_symbol_name_alloc) {
		g_free (mangled_symbol_name_alloc);
	}
}

#else

static inline void
emit_local_symbol (MonoAotCompile *acfg, const char *name, const char *end_label, gboolean func) 
{ 
	mono_img_writer_emit_local_symbol (acfg->w, name, end_label, func);
}

#endif

static inline void
emit_label (MonoAotCompile *acfg, const char *name) 
{ 
	mono_img_writer_emit_label (acfg->w, name); 
}

static inline void
emit_bytes (MonoAotCompile *acfg, const guint8* buf, int size) 
{ 
	mono_img_writer_emit_bytes (acfg->w, buf, size); 
}

static inline void
emit_string (MonoAotCompile *acfg, const char *value) 
{ 
	mono_img_writer_emit_string (acfg->w, value); 
}

static inline void
emit_line (MonoAotCompile *acfg) 
{ 
	mono_img_writer_emit_line (acfg->w); 
}

static inline void
emit_alignment (MonoAotCompile *acfg, int size)
{ 
	mono_img_writer_emit_alignment (acfg->w, size);
}

static inline void
emit_alignment_code (MonoAotCompile *acfg, int size)
{
	if (acfg->align_pad_value)
		mono_img_writer_emit_alignment_fill (acfg->w, size, acfg->align_pad_value);
	else
		mono_img_writer_emit_alignment (acfg->w, size);
}

static inline void
emit_padding (MonoAotCompile *acfg, int size)
{
	int i;
	guint8 buf [16];

	if (acfg->align_pad_value) {
		for (i = 0; i < 16; ++i)
			buf [i] = acfg->align_pad_value;
	} else {
		memset (buf, 0, sizeof (buf));
	}

	for (i = 0; i < size; i += 16) {
		if (size - i < 16)
			emit_bytes (acfg, buf, size - i);
		else
			emit_bytes (acfg, buf, 16);
	}
}

static inline void
emit_pointer (MonoAotCompile *acfg, const char *target) 
{ 
	mono_img_writer_emit_pointer (acfg->w, target); 
}

static inline void
emit_pointer_2 (MonoAotCompile *acfg, const char *prefix, const char *target) 
{ 
	if (prefix [0] != '\0') {
		char *s = g_strdup_printf ("%s%s", prefix, target);
		mono_img_writer_emit_pointer (acfg->w, s);
		g_free (s);
	} else {
		mono_img_writer_emit_pointer (acfg->w, target);
	}
}

static inline void
emit_int16 (MonoAotCompile *acfg, int value) 
{ 
	mono_img_writer_emit_int16 (acfg->w, value); 
}

static inline void
emit_int32 (MonoAotCompile *acfg, int value) 
{ 
	mono_img_writer_emit_int32 (acfg->w, value); 
}

static inline void
emit_symbol_diff (MonoAotCompile *acfg, const char *end, const char* start, int offset) 
{ 
	mono_img_writer_emit_symbol_diff (acfg->w, end, start, offset); 
}

static inline void
emit_zero_bytes (MonoAotCompile *acfg, int num) 
{ 
	mono_img_writer_emit_zero_bytes (acfg->w, num); 
}

static inline void
emit_byte (MonoAotCompile *acfg, guint8 val) 
{ 
	mono_img_writer_emit_byte (acfg->w, val); 
}

#if defined(TARGET_WIN32) && defined(TARGET_X86)

static G_GNUC_UNUSED void
emit_global_inner (MonoAotCompile *acfg, const char *name, gboolean func)
{
	const char * mangled_symbol_name = name;
	char * mangled_symbol_name_alloc = NULL;

	mangled_symbol_name_alloc = mangle_symbol_alloc (name);
	mangled_symbol_name = mangled_symbol_name_alloc;
	
	if (0 != g_strcasecmp (name, mangled_symbol_name)) {
		mono_img_writer_emit_label (acfg->w, mangled_symbol_name);
	}
	mono_img_writer_emit_global (acfg->w, mangled_symbol_name, func);

	if (NULL != mangled_symbol_name_alloc) {
		g_free (mangled_symbol_name_alloc);
	}
}

#else

static G_GNUC_UNUSED void
emit_global_inner (MonoAotCompile *acfg, const char *name, gboolean func)
{
	mono_img_writer_emit_global (acfg->w, name, func);
}

#endif

static inline gboolean
link_shared_library (MonoAotCompile *acfg)
{
	return !acfg->aot_opts.static_link && !acfg->aot_opts.asm_only;
}

static inline gboolean
add_to_global_symbol_table (MonoAotCompile *acfg)
{
#ifdef TARGET_WIN32_MSVC
	return acfg->aot_opts.no_dlsym || link_shared_library (acfg);
#else
	return acfg->aot_opts.no_dlsym;
#endif
}

static void
emit_global (MonoAotCompile *acfg, const char *name, gboolean func)
{
	if (add_to_global_symbol_table (acfg))
		g_ptr_array_add (acfg->globals, g_strdup (name));

	if (acfg->aot_opts.no_dlsym) {
		mono_img_writer_emit_local_symbol (acfg->w, name, NULL, func);
	} else {
		emit_global_inner (acfg, name, func);
	}
}

static void
emit_symbol_size (MonoAotCompile *acfg, const char *name, const char *end_label)
{
	mono_img_writer_emit_symbol_size (acfg->w, name, end_label);
}

/* Emit a symbol which is referenced by the MonoAotFileInfo structure */
static void
emit_info_symbol (MonoAotCompile *acfg, const char *name)
{
	char symbol [MAX_SYMBOL_SIZE];

	if (acfg->llvm) {
		emit_label (acfg, name);
		/* LLVM generated code references this */
		sprintf (symbol, "%s%s%s", acfg->user_symbol_prefix, acfg->global_prefix, name);
		emit_label (acfg, symbol);
		emit_global_inner (acfg, symbol, FALSE);
	} else {
		emit_label (acfg, name);
	}
}

static void
emit_string_symbol (MonoAotCompile *acfg, const char *name, const char *value)
{
	if (acfg->llvm) {
		mono_llvm_emit_aot_data (name, (guint8*)value, strlen (value) + 1);
		return;
	}

	mono_img_writer_emit_section_change (acfg->w, RODATA_SECT, 1);
#ifdef TARGET_MACH
	/* On apple, all symbols need to be aligned to avoid warnings from ld */
	emit_alignment (acfg, 4);
#endif
	mono_img_writer_emit_label (acfg->w, name);
	mono_img_writer_emit_string (acfg->w, value);
}

static G_GNUC_UNUSED void
emit_uleb128 (MonoAotCompile *acfg, guint32 value)
{
	do {
		guint8 b = value & 0x7f;
		value >>= 7;
		if (value != 0) /* more bytes to come */
			b |= 0x80;
		emit_byte (acfg, b);
	} while (value);
}

static G_GNUC_UNUSED void
emit_sleb128 (MonoAotCompile *acfg, gint64 value)
{
	gboolean more = 1;
	gboolean negative = (value < 0);
	guint32 size = 64;
	guint8 byte;

	while (more) {
		byte = value & 0x7f;
		value >>= 7;
		/* the following is unnecessary if the
		 * implementation of >>= uses an arithmetic rather
		 * than logical shift for a signed left operand
		 */
		if (negative)
			/* sign extend */
			value |= - ((gint64)1 <<(size - 7));
		/* sign bit of byte is second high order bit (0x40) */
		if ((value == 0 && !(byte & 0x40)) ||
			(value == -1 && (byte & 0x40)))
			more = 0;
		else
			byte |= 0x80;
		emit_byte (acfg, byte);
	}
}

static G_GNUC_UNUSED void
encode_uleb128 (guint32 value, guint8 *buf, guint8 **endbuf)
{
	guint8 *p = buf;

	do {
		guint8 b = value & 0x7f;
		value >>= 7;
		if (value != 0) /* more bytes to come */
			b |= 0x80;
		*p ++ = b;
	} while (value);

	*endbuf = p;
}

static G_GNUC_UNUSED void
encode_sleb128 (gint32 value, guint8 *buf, guint8 **endbuf)
{
	gboolean more = 1;
	gboolean negative = (value < 0);
	guint32 size = 32;
	guint8 byte;
	guint8 *p = buf;

	while (more) {
		byte = value & 0x7f;
		value >>= 7;
		/* the following is unnecessary if the
		 * implementation of >>= uses an arithmetic rather
		 * than logical shift for a signed left operand
		 */
		if (negative)
			/* sign extend */
			value |= - (1 <<(size - 7));
		/* sign bit of byte is second high order bit (0x40) */
		if ((value == 0 && !(byte & 0x40)) ||
			(value == -1 && (byte & 0x40)))
			more = 0;
		else
			byte |= 0x80;
		*p ++= byte;
	}

	*endbuf = p;
}

static void
encode_int (gint32 val, guint8 *buf, guint8 **endbuf)
{
	// FIXME: Big-endian
	buf [0] = (val >> 0) & 0xff;
	buf [1] = (val >> 8) & 0xff;
	buf [2] = (val >> 16) & 0xff;
	buf [3] = (val >> 24) & 0xff;

	*endbuf = buf + 4;
}

static void
encode_int16 (guint16 val, guint8 *buf, guint8 **endbuf)
{
	buf [0] = (val >> 0) & 0xff;
	buf [1] = (val >> 8) & 0xff;

	*endbuf = buf + 2;
}

static void
encode_string (const char *s, guint8 *buf, guint8 **endbuf)
{
	int len = strlen (s);

	memcpy (buf, s, len + 1);
	*endbuf = buf + len + 1;
}

static void
emit_unset_mode (MonoAotCompile *acfg)
{
	mono_img_writer_emit_unset_mode (acfg->w);
}

static G_GNUC_UNUSED void
emit_set_thumb_mode (MonoAotCompile *acfg)
{
	emit_unset_mode (acfg);
	fprintf (acfg->fp, ".code 16\n");
}

static G_GNUC_UNUSED void
emit_set_arm_mode (MonoAotCompile *acfg)
{
	emit_unset_mode (acfg);
	fprintf (acfg->fp, ".code 32\n");
}

static inline void
emit_code_bytes (MonoAotCompile *acfg, const guint8* buf, int size)
{
#ifdef TARGET_ARM64
	int i;

	g_assert (size % 4 == 0);
	emit_unset_mode (acfg);
	for (i = 0; i < size; i += 4)
		fprintf (acfg->fp, "%s 0x%x\n", acfg->inst_directive, *(guint32*)(buf + i));
#else
	emit_bytes (acfg, buf, size);
#endif
}

/* ARCHITECTURE SPECIFIC CODE */

#if defined(TARGET_X86) || defined(TARGET_AMD64) || defined(TARGET_ARM) || defined(TARGET_POWERPC) || defined(TARGET_ARM64)
#define EMIT_DWARF_INFO 1
#endif

#ifdef TARGET_WIN32_MSVC
#undef EMIT_DWARF_INFO
#endif

#if defined(TARGET_ARM)
#define AOT_FUNC_ALIGNMENT 4
#else
#define AOT_FUNC_ALIGNMENT 16
#endif
 
#if defined(TARGET_POWERPC64) && !defined(__mono_ilp32__)
#define PPC_LD_OP "ld"
#define PPC_LDX_OP "ldx"
#else
#define PPC_LD_OP "lwz"
#define PPC_LDX_OP "lwzx"
#endif

#ifdef TARGET_X86_64_WIN32_MSVC
#define AOT_TARGET_STR "AMD64 (WIN32) (MSVC codegen)"
#elif TARGET_AMD64
#define AOT_TARGET_STR "AMD64"
#endif

#ifdef TARGET_ARM
#ifdef TARGET_MACH
#define AOT_TARGET_STR "ARM (MACH)"
#else
#define AOT_TARGET_STR "ARM (!MACH)"
#endif
#endif

#ifdef TARGET_ARM64
#ifdef TARGET_MACH
#define AOT_TARGET_STR "ARM64 (MACH)"
#else
#define AOT_TARGET_STR "ARM64 (!MACH)"
#endif
#endif

#ifdef TARGET_POWERPC64
#ifdef __mono_ilp32__
#define AOT_TARGET_STR "POWERPC64 (mono ilp32)"
#else
#define AOT_TARGET_STR "POWERPC64 (!mono ilp32)"
#endif
#else
#ifdef TARGET_POWERPC
#ifdef __mono_ilp32__
#define AOT_TARGET_STR "POWERPC (mono ilp32)"
#else
#define AOT_TARGET_STR "POWERPC (!mono ilp32)"
#endif
#endif
#endif

#ifdef TARGET_X86
#ifdef TARGET_WIN32
#define AOT_TARGET_STR "X86 (WIN32)"
#elif defined(__native_client_codegen__)
#define AOT_TARGET_STR "X86 (native client codegen)"
#else
#define AOT_TARGET_STR "X86 (!native client codegen)"
#endif
#endif

#ifndef AOT_TARGET_STR
#define AOT_TARGET_STR ""
#endif

static void
arch_init (MonoAotCompile *acfg)
{
	acfg->llc_args = g_string_new ("");
	acfg->as_args = g_string_new ("");
	acfg->llvm_owriter_supported = TRUE;

	/*
	 * The prefix LLVM likes to put in front of symbol names on darwin.
	 * The mach-os specs require this for globals, but LLVM puts them in front of all
	 * symbols. We need to handle this, since we need to refer to LLVM generated
	 * symbols.
	 */
	acfg->llvm_label_prefix = "";
	acfg->user_symbol_prefix = "";

#if defined(TARGET_X86)
	g_string_append (acfg->llc_args, " -march=x86 -mattr=sse4.1");
#endif

#if defined(TARGET_AMD64)
	g_string_append (acfg->llc_args, " -march=x86-64 -mattr=sse4.1");
	/* NOP */
	acfg->align_pad_value = 0x90;
#endif

#ifdef TARGET_ARM
	if (acfg->aot_opts.mtriple && strstr (acfg->aot_opts.mtriple, "darwin")) {
		g_string_append (acfg->llc_args, "-mattr=+v6");
	} else {
#if defined(ARM_FPU_VFP_HARD)
		g_string_append (acfg->llc_args, " -mattr=+vfp2,-neon,+d16 -float-abi=hard");
		g_string_append (acfg->as_args, " -mfpu=vfp3");
#elif defined(ARM_FPU_VFP)
		g_string_append (acfg->llc_args, " -mattr=+vfp2,-neon,+d16");
		g_string_append (acfg->as_args, " -mfpu=vfp3");
#else
		g_string_append (acfg->llc_args, " -soft-float");
#endif
	}
	if (acfg->aot_opts.mtriple && strstr (acfg->aot_opts.mtriple, "thumb"))
		acfg->thumb_mixed = TRUE;

	if (acfg->aot_opts.mtriple)
		mono_arch_set_target (acfg->aot_opts.mtriple);
#endif

#ifdef TARGET_ARM64
	acfg->inst_directive = ".inst";
	if (acfg->aot_opts.mtriple)
		mono_arch_set_target (acfg->aot_opts.mtriple);
#endif

#ifdef TARGET_MACH
	acfg->user_symbol_prefix = "_";
	acfg->llvm_label_prefix = "_";
	acfg->inst_directive = ".word";
	acfg->need_no_dead_strip = TRUE;
	acfg->aot_opts.gnu_asm = TRUE;
#endif

#if defined(__linux__) && !defined(TARGET_ARM)
	acfg->need_pt_gnu_stack = TRUE;
#endif

#ifdef MONOTOUCH
	acfg->global_symbols = TRUE;
#endif

#ifdef TARGET_ANDROID
	acfg->llvm_owriter_supported = FALSE;
#endif
}

#ifdef TARGET_ARM64


/* Load the contents of GOT_SLOT into dreg, clobbering ip0 */
static void
arm64_emit_load_got_slot (MonoAotCompile *acfg, int dreg, int got_slot)
{
	int offset;

	g_assert (acfg->fp);
	emit_unset_mode (acfg);
	/* r16==ip0 */
	offset = (int)(got_slot * sizeof (gpointer));
#ifdef TARGET_MACH
	/* clang's integrated assembler */
	fprintf (acfg->fp, "adrp x16, %s@PAGE+%d\n", acfg->got_symbol, offset & 0xfffff000);
	fprintf (acfg->fp, "add x16, x16, %s@PAGEOFF\n", acfg->got_symbol);
	fprintf (acfg->fp, "ldr x%d, [x16, #%d]\n", dreg, offset & 0xfff);
#else
	/* Linux GAS */
	fprintf (acfg->fp, "adrp x16, %s+%d\n", acfg->got_symbol, offset & 0xfffff000);
	fprintf (acfg->fp, "add x16, x16, :lo12:%s\n", acfg->got_symbol);
	fprintf (acfg->fp, "ldr x%d, [x16, %d]\n", dreg, offset & 0xfff);
#endif
}

static void
arm64_emit_objc_selector_ref (MonoAotCompile *acfg, guint8 *code, int index, int *code_size)
{
	int reg;

	g_assert (acfg->fp);
	emit_unset_mode (acfg);

	/* ldr rt, target */
	reg = arm_get_ldr_lit_reg (code);

	fprintf (acfg->fp, "adrp x%d, L_OBJC_SELECTOR_REFERENCES_%d@PAGE\n", reg, index);
	fprintf (acfg->fp, "add x%d, x%d, L_OBJC_SELECTOR_REFERENCES_%d@PAGEOFF\n", reg, reg, index);
	fprintf (acfg->fp, "ldr x%d, [x%d]\n", reg, reg);

	*code_size = 12;
}

static void
arm64_emit_direct_call (MonoAotCompile *acfg, const char *target, gboolean external, gboolean thumb, MonoJumpInfo *ji, int *call_size)
{
	g_assert (acfg->fp);
	emit_unset_mode (acfg);
	if (ji && ji->relocation == MONO_R_ARM64_B) {
		fprintf (acfg->fp, "b %s\n", target);
	} else {
		if (ji)
			g_assert (ji->relocation == MONO_R_ARM64_BL);
		fprintf (acfg->fp, "bl %s\n", target);
	}
	*call_size = 4;
}

static void
arm64_emit_got_access (MonoAotCompile *acfg, guint8 *code, int got_slot, int *code_size)
{
	int reg;

	/* ldr rt, target */
	reg = arm_get_ldr_lit_reg (code);
	arm64_emit_load_got_slot (acfg, reg, got_slot);
	*code_size = 12;
}

static void
arm64_emit_plt_entry (MonoAotCompile *acfg, const char *got_symbol, int offset, int info_offset)
{
	arm64_emit_load_got_slot (acfg, ARMREG_R16, offset / sizeof (gpointer));
	fprintf (acfg->fp, "br x16\n");
	/* Used by mono_aot_get_plt_info_offset () */
	fprintf (acfg->fp, "%s %d\n", acfg->inst_directive, info_offset);
}

static void
arm64_emit_tramp_page_common_code (MonoAotCompile *acfg, int pagesize, int arg_reg, int *size)
{
	guint8 buf [256];
	guint8 *code;
	int imm;

	/* The common code */
	code = buf;
	imm = pagesize;
	/* The trampoline address is in IP0 */
	arm_movzx (code, ARMREG_IP1, imm & 0xffff, 0);
	arm_movkx (code, ARMREG_IP1, (imm >> 16) & 0xffff, 16);
	/* Compute the data slot address */
	arm_subx (code, ARMREG_IP0, ARMREG_IP0, ARMREG_IP1);
	/* Trampoline argument */
	arm_ldrx (code, arg_reg, ARMREG_IP0, 0);
	/* Address */
	arm_ldrx (code, ARMREG_IP0, ARMREG_IP0, 8);
	arm_brx (code, ARMREG_IP0);

	/* Emit it */
	emit_code_bytes (acfg, buf, code - buf);

	*size = code - buf;
}

static void
arm64_emit_tramp_page_specific_code (MonoAotCompile *acfg, int pagesize, int common_tramp_size, int specific_tramp_size)
{
	guint8 buf [256];
	guint8 *code;
	int i, count;

	count = (pagesize - common_tramp_size) / specific_tramp_size;
	for (i = 0; i < count; ++i) {
		code = buf;
		arm_adrx (code, ARMREG_IP0, code);
		/* Branch to the generic code */
		arm_b (code, code - 4 - (i * specific_tramp_size) - common_tramp_size);
		/* This has to be 2 pointers long */
		arm_nop (code);
		arm_nop (code);
		g_assert (code - buf == specific_tramp_size);
		emit_code_bytes (acfg, buf, code - buf);
	}
}

static void
arm64_emit_specific_trampoline_pages (MonoAotCompile *acfg)
{
	guint8 buf [128];
	guint8 *code;
	guint8 *labels [16];
	int common_tramp_size;
	int specific_tramp_size = 2 * 8;
	int imm, pagesize;
	char symbol [128];

	if (!acfg->aot_opts.use_trampolines_page)
		return;

#ifdef TARGET_MACH
	/* Have to match the target pagesize */
	pagesize = 16384;
#else
	pagesize = mono_pagesize ();
#endif
	acfg->tramp_page_size = pagesize;

	/* The specific trampolines */
	sprintf (symbol, "%sspecific_trampolines_page", acfg->user_symbol_prefix);
	emit_alignment (acfg, pagesize);
	emit_global (acfg, symbol, TRUE);
	emit_label (acfg, symbol);

	/* The common code */
	arm64_emit_tramp_page_common_code (acfg, pagesize, ARMREG_IP1, &common_tramp_size);
	acfg->tramp_page_code_offsets [MONO_AOT_TRAMP_SPECIFIC] = common_tramp_size;

	arm64_emit_tramp_page_specific_code (acfg, pagesize, common_tramp_size, specific_tramp_size);

	/* The rgctx trampolines */
	/* These are the same as the specific trampolines, but they load the argument into MONO_ARCH_RGCTX_REG */
	sprintf (symbol, "%srgctx_trampolines_page", acfg->user_symbol_prefix);
	emit_alignment (acfg, pagesize);
	emit_global (acfg, symbol, TRUE);
	emit_label (acfg, symbol);

	/* The common code */
	arm64_emit_tramp_page_common_code (acfg, pagesize, MONO_ARCH_RGCTX_REG, &common_tramp_size);
	acfg->tramp_page_code_offsets [MONO_AOT_TRAMP_STATIC_RGCTX] = common_tramp_size;

	arm64_emit_tramp_page_specific_code (acfg, pagesize, common_tramp_size, specific_tramp_size);

	/* The gsharedvt arg trampolines */
	/* These are the same as the specific trampolines */
	sprintf (symbol, "%sgsharedvt_arg_trampolines_page", acfg->user_symbol_prefix);
	emit_alignment (acfg, pagesize);
	emit_global (acfg, symbol, TRUE);
	emit_label (acfg, symbol);

	arm64_emit_tramp_page_common_code (acfg, pagesize, ARMREG_IP1, &common_tramp_size);
	acfg->tramp_page_code_offsets [MONO_AOT_TRAMP_GSHAREDVT_ARG] = common_tramp_size;

	arm64_emit_tramp_page_specific_code (acfg, pagesize, common_tramp_size, specific_tramp_size);

	/* The IMT trampolines */
	sprintf (symbol, "%simt_trampolines_page", acfg->user_symbol_prefix);
	emit_alignment (acfg, pagesize);
	emit_global (acfg, symbol, TRUE);
	emit_label (acfg, symbol);

	code = buf;
	imm = pagesize;
	/* The trampoline address is in IP0 */
	arm_movzx (code, ARMREG_IP1, imm & 0xffff, 0);
	arm_movkx (code, ARMREG_IP1, (imm >> 16) & 0xffff, 16);
	/* Compute the data slot address */
	arm_subx (code, ARMREG_IP0, ARMREG_IP0, ARMREG_IP1);
	/* Trampoline argument */
	arm_ldrx (code, ARMREG_IP1, ARMREG_IP0, 0);

	/* Same as arch_emit_imt_trampoline () */
	labels [0] = code;
	arm_ldrx (code, ARMREG_IP0, ARMREG_IP1, 0);
	arm_cmpx (code, ARMREG_IP0, MONO_ARCH_RGCTX_REG);
	labels [1] = code;
	arm_bcc (code, ARMCOND_EQ, 0);

	/* End-of-loop check */
	labels [2] = code;
	arm_cbzx (code, ARMREG_IP0, 0);

	/* Loop footer */
	arm_addx_imm (code, ARMREG_IP1, ARMREG_IP1, 2 * 8);
	arm_b (code, labels [0]);

	/* Match */
	mono_arm_patch (labels [1], code, MONO_R_ARM64_BCC);
	/* Load vtable slot addr */
	arm_ldrx (code, ARMREG_IP0, ARMREG_IP1, 8);
	/* Load vtable slot */
	arm_ldrx (code, ARMREG_IP0, ARMREG_IP0, 0);
	arm_brx (code, ARMREG_IP0);

	/* No match */
	mono_arm_patch (labels [2], code, MONO_R_ARM64_CBZ);
	/* Load fail addr */
	arm_ldrx (code, ARMREG_IP0, ARMREG_IP1, 8);
	arm_brx (code, ARMREG_IP0);

	emit_code_bytes (acfg, buf, code - buf);

	common_tramp_size = code - buf;
	acfg->tramp_page_code_offsets [MONO_AOT_TRAMP_IMT] = common_tramp_size;

	arm64_emit_tramp_page_specific_code (acfg, pagesize, common_tramp_size, specific_tramp_size);
}

static void
arm64_emit_specific_trampoline (MonoAotCompile *acfg, int offset, int *tramp_size)
{
	/* Load argument from second GOT slot */
	arm64_emit_load_got_slot (acfg, ARMREG_R17, offset + 1);
	/* Load generic trampoline address from first GOT slot */
	arm64_emit_load_got_slot (acfg, ARMREG_R16, offset);
	fprintf (acfg->fp, "br x16\n");
	*tramp_size = 7 * 4;
}

static void
arm64_emit_unbox_trampoline (MonoAotCompile *acfg, MonoCompile *cfg, MonoMethod *method, const char *call_target)
{
	emit_unset_mode (acfg);
	fprintf (acfg->fp, "add x0, x0, %d\n", (int)(sizeof (MonoObject)));
	fprintf (acfg->fp, "b %s\n", call_target);
}

static void
arm64_emit_static_rgctx_trampoline (MonoAotCompile *acfg, int offset, int *tramp_size)
{
	/* Similar to the specific trampolines, but use the rgctx reg instead of ip1 */

	/* Load argument from first GOT slot */
	arm64_emit_load_got_slot (acfg, MONO_ARCH_RGCTX_REG, offset);
	/* Load generic trampoline address from second GOT slot */
	arm64_emit_load_got_slot (acfg, ARMREG_R16, offset + 1);
	fprintf (acfg->fp, "br x16\n");
	*tramp_size = 7 * 4;
}

static void
arm64_emit_imt_trampoline (MonoAotCompile *acfg, int offset, int *tramp_size)
{
	guint8 buf [128];
	guint8 *code, *labels [16];

	/* Load parameter from GOT slot into ip1 */
	arm64_emit_load_got_slot (acfg, ARMREG_R17, offset);

	code = buf;
	labels [0] = code;
	arm_ldrx (code, ARMREG_IP0, ARMREG_IP1, 0);
	arm_cmpx (code, ARMREG_IP0, MONO_ARCH_RGCTX_REG);
	labels [1] = code;
	arm_bcc (code, ARMCOND_EQ, 0);

	/* End-of-loop check */
	labels [2] = code;
	arm_cbzx (code, ARMREG_IP0, 0);

	/* Loop footer */
	arm_addx_imm (code, ARMREG_IP1, ARMREG_IP1, 2 * 8);
	arm_b (code, labels [0]);

	/* Match */
	mono_arm_patch (labels [1], code, MONO_R_ARM64_BCC);
	/* Load vtable slot addr */
	arm_ldrx (code, ARMREG_IP0, ARMREG_IP1, 8);
	/* Load vtable slot */
	arm_ldrx (code, ARMREG_IP0, ARMREG_IP0, 0);
	arm_brx (code, ARMREG_IP0);

	/* No match */
	mono_arm_patch (labels [2], code, MONO_R_ARM64_CBZ);
	/* Load fail addr */
	arm_ldrx (code, ARMREG_IP0, ARMREG_IP1, 8);
	arm_brx (code, ARMREG_IP0);

	emit_code_bytes (acfg, buf, code - buf);

	*tramp_size = code - buf + (3 * 4);
}

static void
arm64_emit_gsharedvt_arg_trampoline (MonoAotCompile *acfg, int offset, int *tramp_size)
{
	/* Similar to the specific trampolines, but the address is in the second slot */
	/* Load argument from first GOT slot */
	arm64_emit_load_got_slot (acfg, ARMREG_R17, offset);
	/* Load generic trampoline address from second GOT slot */
	arm64_emit_load_got_slot (acfg, ARMREG_R16, offset + 1);
	fprintf (acfg->fp, "br x16\n");
	*tramp_size = 7 * 4;
}


#endif

#ifdef MONO_ARCH_AOT_SUPPORTED
/*
 * arch_emit_direct_call:
 *
 *   Emit a direct call to the symbol TARGET. CALL_SIZE is set to the size of the
 * calling code.
 */
static void
arch_emit_direct_call (MonoAotCompile *acfg, const char *target, gboolean external, gboolean thumb, MonoJumpInfo *ji, int *call_size)
{
#if defined(TARGET_X86) || defined(TARGET_AMD64)
	/* Need to make sure this is exactly 5 bytes long */
	emit_unset_mode (acfg);
	fprintf (acfg->fp, "call %s\n", target);
	*call_size = 5;
#elif defined(TARGET_ARM)
	emit_unset_mode (acfg);
	if (thumb)
		fprintf (acfg->fp, "blx %s\n", target);
	else
		fprintf (acfg->fp, "bl %s\n", target);
	*call_size = 4;
#elif defined(TARGET_ARM64)
	arm64_emit_direct_call (acfg, target, external, thumb, ji, call_size);
#elif defined(TARGET_POWERPC)
	emit_unset_mode (acfg);
	fprintf (acfg->fp, "bl %s\n", target);
	*call_size = 4;
#else
	g_assert_not_reached ();
#endif
}
#endif

/*
 * PPC32 design:
 * - we use an approach similar to the x86 abi: reserve a register (r30) to hold 
 *   the GOT pointer.
 * - The full-aot trampolines need access to the GOT of mscorlib, so we store
 *   in in the 2. slot of every GOT, and require every method to place the GOT
 *   address in r30, even when it doesn't access the GOT otherwise. This way,
 *   the trampolines can compute the mscorlib GOT address by loading 4(r30).
 */

/*
 * PPC64 design:
 * PPC64 uses function descriptors which greatly complicate all code, since
 * these are used very inconsistently in the runtime. Some functions like 
 * mono_compile_method () return ftn descriptors, while others like the
 * trampoline creation functions do not.
 * We assume that all GOT slots contain function descriptors, and create 
 * descriptors in aot-runtime.c when needed.
 * The ppc64 abi uses r2 to hold the address of the TOC/GOT, which is loaded
 * from function descriptors, we could do the same, but it would require 
 * rewriting all the ppc/aot code to handle function descriptors properly.
 * So instead, we use the same approach as on PPC32.
 * This is a horrible mess, but fixing it would probably lead to an even bigger
 * one.
 */

/*
 * X86 design:
 * - similar to the PPC32 design, we reserve EBX to hold the GOT pointer.
 */

#ifdef MONO_ARCH_AOT_SUPPORTED
/*
 * arch_emit_got_offset:
 *
 *   The memory pointed to by CODE should hold native code for computing the GOT
 * address (OP_LOAD_GOTADDR). Emit this code while patching it with the offset
 * between code and the GOT. CODE_SIZE is set to the number of bytes emitted.
 */
static void
arch_emit_got_offset (MonoAotCompile *acfg, guint8 *code, int *code_size)
{
#if defined(TARGET_POWERPC64)
	emit_unset_mode (acfg);
	/* 
	 * The ppc32 code doesn't seem to work on ppc64, the assembler complains about
	 * unsupported relocations. So we store the got address into the .Lgot_addr
	 * symbol which is in the text segment, compute its address, and load it.
	 */
	fprintf (acfg->fp, ".L%d:\n", acfg->label_generator);
	fprintf (acfg->fp, "lis 0, (.Lgot_addr + 4 - .L%d)@h\n", acfg->label_generator);
	fprintf (acfg->fp, "ori 0, 0, (.Lgot_addr + 4 - .L%d)@l\n", acfg->label_generator);
	fprintf (acfg->fp, "add 30, 30, 0\n");
	fprintf (acfg->fp, "%s 30, 0(30)\n", PPC_LD_OP);
	acfg->label_generator ++;
	*code_size = 16;
#elif defined(TARGET_POWERPC)
	emit_unset_mode (acfg);
	fprintf (acfg->fp, ".L%d:\n", acfg->label_generator);
	fprintf (acfg->fp, "lis 0, (%s + 4 - .L%d)@h\n", acfg->got_symbol, acfg->label_generator);
	fprintf (acfg->fp, "ori 0, 0, (%s + 4 - .L%d)@l\n", acfg->got_symbol, acfg->label_generator);
	acfg->label_generator ++;
	*code_size = 8;
#else
	guint32 offset = mono_arch_get_patch_offset (code);
	emit_bytes (acfg, code, offset);
	emit_symbol_diff (acfg, acfg->got_symbol, ".", offset);

	*code_size = offset + 4;
#endif
}

/*
 * arch_emit_got_access:
 *
 *   The memory pointed to by CODE should hold native code for loading a GOT
 * slot (OP_AOTCONST/OP_GOT_ENTRY). Emit this code while patching it so it accesses the
 * GOT slot GOT_SLOT. CODE_SIZE is set to the number of bytes emitted.
 */
static void
arch_emit_got_access (MonoAotCompile *acfg, const char *got_symbol, guint8 *code, int got_slot, int *code_size)
{
#ifdef TARGET_AMD64
	/* mov reg, got+offset(%rip) */
	if (acfg->llvm) {
		/* The GOT symbol is in the LLVM module, the clang assembler has problems emitting symbol diffs for it */
		int dreg;
		int rex_r;

		/* Decode reg, see amd64_mov_reg_membase () */
		rex_r = code [0] & AMD64_REX_R;
		g_assert (code [0] == 0x49 + rex_r);
		g_assert (code [1] == 0x8b);
		dreg = ((code [2] >> 3) & 0x7) + (rex_r ? 8 : 0);

		emit_unset_mode (acfg);
		fprintf (acfg->fp, "mov %s+%d(%%rip), %s\n", got_symbol, (unsigned int) ((got_slot * sizeof (gpointer))), mono_arch_regname (dreg));
		*code_size = 7;
	} else {
		emit_bytes (acfg, code, mono_arch_get_patch_offset (code));
		emit_symbol_diff (acfg, got_symbol, ".", (unsigned int) ((got_slot * sizeof (gpointer)) - 4));
		*code_size = mono_arch_get_patch_offset (code) + 4;
	}
#elif defined(TARGET_X86)
	emit_bytes (acfg, code, mono_arch_get_patch_offset (code));
	emit_int32 (acfg, (unsigned int) ((got_slot * sizeof (gpointer))));
	*code_size = mono_arch_get_patch_offset (code) + 4;
#elif defined(TARGET_ARM)
	emit_bytes (acfg, code, mono_arch_get_patch_offset (code));
	emit_symbol_diff (acfg, got_symbol, ".", (unsigned int) ((got_slot * sizeof (gpointer))) - 12);
	*code_size = mono_arch_get_patch_offset (code) + 4;
#elif defined(TARGET_ARM64)
	emit_bytes (acfg, code, mono_arch_get_patch_offset (code));
	arm64_emit_got_access (acfg, code, got_slot, code_size);
#elif defined(TARGET_POWERPC)
	{
		guint8 buf [32];

		emit_bytes (acfg, code, mono_arch_get_patch_offset (code));
		code = buf;
		ppc_load32 (code, ppc_r0, got_slot * sizeof (gpointer));
		g_assert (code - buf == 8);
		emit_bytes (acfg, buf, code - buf);
		*code_size = code - buf;
	}
#else
	g_assert_not_reached ();
#endif
}

#endif

#ifdef MONO_ARCH_AOT_SUPPORTED
/*
 * arch_emit_objc_selector_ref:
 *
 *   Emit the implementation of OP_OBJC_GET_SELECTOR, which itself implements @selector(foo:) in objective-c.
 */
static void
arch_emit_objc_selector_ref (MonoAotCompile *acfg, guint8 *code, int index, int *code_size)
{
#if defined(TARGET_ARM)
	char symbol1 [MAX_SYMBOL_SIZE];
	char symbol2 [MAX_SYMBOL_SIZE];
	int lindex = acfg->objc_selector_index_2 ++;

	/* Emit ldr.imm/b */
	emit_bytes (acfg, code, 8);

	sprintf (symbol1, "L_OBJC_SELECTOR_%d", lindex);
	sprintf (symbol2, "L_OBJC_SELECTOR_REFERENCES_%d", index);

	emit_label (acfg, symbol1);
	mono_img_writer_emit_unset_mode (acfg->w);
	fprintf (acfg->fp, ".long %s-(%s+12)", symbol2, symbol1);

	*code_size = 12;
#elif defined(TARGET_ARM64)
	arm64_emit_objc_selector_ref (acfg, code, index, code_size);
#else
	g_assert_not_reached ();
#endif
}
#endif

/*
 * arch_emit_plt_entry:
 *
 *   Emit code for the PLT entry.
 * The plt entry should look like this:
 * <indirect jump to GOT_SYMBOL + OFFSET>
 * <INFO_OFFSET embedded into the instruction stream>
 */
static void
arch_emit_plt_entry (MonoAotCompile *acfg, const char *got_symbol, int offset, int info_offset)
{
#if defined(TARGET_X86)
		/* jmp *<offset>(%ebx) */
		emit_byte (acfg, 0xff);
		emit_byte (acfg, 0xa3);
		emit_int32 (acfg, offset);
		/* Used by mono_aot_get_plt_info_offset */
		emit_int32 (acfg, info_offset);
#elif defined(TARGET_AMD64)
		emit_unset_mode (acfg);
		fprintf (acfg->fp, "jmp *%s+%d(%%rip)\n", got_symbol, offset);
		/* Used by mono_aot_get_plt_info_offset */
		emit_int32 (acfg, info_offset);
		acfg->stats.plt_size += 10;
#elif defined(TARGET_ARM)
		guint8 buf [256];
		guint8 *code;

		code = buf;
		ARM_LDR_IMM (code, ARMREG_IP, ARMREG_PC, 0);
		ARM_LDR_REG_REG (code, ARMREG_PC, ARMREG_PC, ARMREG_IP);
		emit_bytes (acfg, buf, code - buf);
		emit_symbol_diff (acfg, got_symbol, ".", offset - 4);
		/* Used by mono_aot_get_plt_info_offset */
		emit_int32 (acfg, info_offset);
#elif defined(TARGET_ARM64)
		arm64_emit_plt_entry (acfg, got_symbol, offset, info_offset);
#elif defined(TARGET_POWERPC)
		/* The GOT address is guaranteed to be in r30 by OP_LOAD_GOTADDR */
		emit_unset_mode (acfg);
		fprintf (acfg->fp, "lis 11, %d@h\n", offset);
		fprintf (acfg->fp, "ori 11, 11, %d@l\n", offset);
		fprintf (acfg->fp, "add 11, 11, 30\n");
		fprintf (acfg->fp, "%s 11, 0(11)\n", PPC_LD_OP);
#ifdef PPC_USES_FUNCTION_DESCRIPTOR
		fprintf (acfg->fp, "%s 2, %d(11)\n", PPC_LD_OP, (int)sizeof (gpointer));
		fprintf (acfg->fp, "%s 11, 0(11)\n", PPC_LD_OP);
#endif
		fprintf (acfg->fp, "mtctr 11\n");
		fprintf (acfg->fp, "bctr\n");
		emit_int32 (acfg, info_offset);
#else
		g_assert_not_reached ();
#endif
}

/*
 * arch_emit_llvm_plt_entry:
 *
 *   Same as arch_emit_plt_entry, but handles calls from LLVM generated code.
 * This is only needed on arm to handle thumb interop.
 */
static void
arch_emit_llvm_plt_entry (MonoAotCompile *acfg, const char *got_symbol, int offset, int info_offset)
{
#if defined(TARGET_ARM)
	/* LLVM calls the PLT entries using bl, so these have to be thumb2 */
	/* The caller already transitioned to thumb */
	/* The code below should be 12 bytes long */
	/* clang has trouble encoding these instructions, so emit the binary */
#if 0
	fprintf (acfg->fp, "ldr ip, [pc, #8]\n");
	/* thumb can't encode ld pc, [pc, ip] */
	fprintf (acfg->fp, "add ip, pc, ip\n");
	fprintf (acfg->fp, "ldr ip, [ip, #0]\n");
	fprintf (acfg->fp, "bx ip\n");
#endif
	emit_set_thumb_mode (acfg);
	fprintf (acfg->fp, ".4byte 0xc008f8df\n");
	fprintf (acfg->fp, ".2byte 0x44fc\n");
	fprintf (acfg->fp, ".4byte 0xc000f8dc\n");
	fprintf (acfg->fp, ".2byte 0x4760\n");
	emit_symbol_diff (acfg, got_symbol, ".", offset + 4);
	emit_int32 (acfg, info_offset);
	emit_unset_mode (acfg);
	emit_set_arm_mode (acfg);
#else
	g_assert_not_reached ();
#endif
}

/* Save unwind_info in the module and emit the offset to the information at symbol */
static void save_unwind_info (MonoAotCompile *acfg, char *symbol, GSList *unwind_ops)
{
	guint32 uw_offset, encoded_len;
	guint8 *encoded;

	emit_section_change (acfg, RODATA_SECT, 0);
	emit_global (acfg, symbol, FALSE);
	emit_label (acfg, symbol);

	encoded = mono_unwind_ops_encode (unwind_ops, &encoded_len);
	uw_offset = get_unwind_info_offset (acfg, encoded, encoded_len);
	g_free (encoded);
	emit_int32 (acfg, uw_offset);
}

/*
 * arch_emit_specific_trampoline_pages:
 *
 * Emits a page full of trampolines: each trampoline uses its own address to
 * lookup both the generic trampoline code and the data argument.
 * This page can be remapped in process multiple times so we can get an
 * unlimited number of trampolines.
 * Specifically this implementation uses the following trick: two memory pages
 * are allocated, with the first containing the data and the second containing the trampolines.
 * To reduce trampoline size, each trampoline jumps at the start of the page where a common
 * implementation does all the lifting.
 * Note that the ARM single trampoline size is 8 bytes, exactly like the data that needs to be stored
 * on the arm 32 bit system.
 */
static void
arch_emit_specific_trampoline_pages (MonoAotCompile *acfg)
{
#if defined(TARGET_ARM)
	guint8 buf [128];
	guint8 *code;
	guint8 *loop_start, *loop_branch_back, *loop_end_check, *imt_found_check;
	int i;
	int pagesize = MONO_AOT_TRAMP_PAGE_SIZE;
	GSList *unwind_ops = NULL;
#define COMMON_TRAMP_SIZE 16
	int count = (pagesize - COMMON_TRAMP_SIZE) / 8;
	int imm8, rot_amount;
	char symbol [128];

	if (!acfg->aot_opts.use_trampolines_page)
		return;

	acfg->tramp_page_size = pagesize;

	sprintf (symbol, "%sspecific_trampolines_page", acfg->user_symbol_prefix);
	emit_alignment (acfg, pagesize);
	emit_global (acfg, symbol, TRUE);
	emit_label (acfg, symbol);

	/* emit the generic code first, the trampoline address + 8 is in the lr register */
	code = buf;
	imm8 = mono_arm_is_rotated_imm8 (pagesize, &rot_amount);
	ARM_SUB_REG_IMM (code, ARMREG_LR, ARMREG_LR, imm8, rot_amount);
	ARM_LDR_IMM (code, ARMREG_R1, ARMREG_LR, -8);
	ARM_LDR_IMM (code, ARMREG_PC, ARMREG_LR, -4);
	ARM_NOP (code);
	g_assert (code - buf == COMMON_TRAMP_SIZE);

	/* Emit it */
	emit_bytes (acfg, buf, code - buf);

	for (i = 0; i < count; ++i) {
		code = buf;
		ARM_PUSH (code, 0x5fff);
		ARM_BL (code, 0);
		arm_patch (code - 4, code - COMMON_TRAMP_SIZE - 8 * (i + 1));
		g_assert (code - buf == 8);
		emit_bytes (acfg, buf, code - buf);
	}

	/* now the rgctx trampolines: each specific trampolines puts in the ip register
	 * the instruction pointer address, so the generic trampoline at the start of the page
	 * subtracts 4096 to get to the data page and loads the values
	 * We again fit the generic trampiline in 16 bytes.
	 */
	sprintf (symbol, "%srgctx_trampolines_page", acfg->user_symbol_prefix);
	emit_global (acfg, symbol, TRUE);
	emit_label (acfg, symbol);
	code = buf;
	imm8 = mono_arm_is_rotated_imm8 (pagesize, &rot_amount);
	ARM_SUB_REG_IMM (code, ARMREG_IP, ARMREG_IP, imm8, rot_amount);
	ARM_LDR_IMM (code, MONO_ARCH_RGCTX_REG, ARMREG_IP, -8);
	ARM_LDR_IMM (code, ARMREG_PC, ARMREG_IP, -4);
	ARM_NOP (code);
	g_assert (code - buf == COMMON_TRAMP_SIZE);

	/* Emit it */
	emit_bytes (acfg, buf, code - buf);

	for (i = 0; i < count; ++i) {
		code = buf;
		ARM_MOV_REG_REG (code, ARMREG_IP, ARMREG_PC);
		ARM_B (code, 0);
		arm_patch (code - 4, code - COMMON_TRAMP_SIZE - 8 * (i + 1));
		g_assert (code - buf == 8);
		emit_bytes (acfg, buf, code - buf);
	}

	/*
	 * gsharedvt arg trampolines: see arch_emit_gsharedvt_arg_trampoline ()
	 */
	sprintf (symbol, "%sgsharedvt_arg_trampolines_page", acfg->user_symbol_prefix);
	emit_global (acfg, symbol, TRUE);
	emit_label (acfg, symbol);
	code = buf;
	ARM_PUSH (code, (1 << ARMREG_R0) | (1 << ARMREG_R1) | (1 << ARMREG_R2) | (1 << ARMREG_R3));
	imm8 = mono_arm_is_rotated_imm8 (pagesize, &rot_amount);
	ARM_SUB_REG_IMM (code, ARMREG_IP, ARMREG_IP, imm8, rot_amount);
	ARM_LDR_IMM (code, ARMREG_R0, ARMREG_IP, -8);
	ARM_LDR_IMM (code, ARMREG_PC, ARMREG_IP, -4);
	g_assert (code - buf == COMMON_TRAMP_SIZE);
	/* Emit it */
	emit_bytes (acfg, buf, code - buf);

	for (i = 0; i < count; ++i) {
		code = buf;
		ARM_MOV_REG_REG (code, ARMREG_IP, ARMREG_PC);
		ARM_B (code, 0);
		arm_patch (code - 4, code - COMMON_TRAMP_SIZE - 8 * (i + 1));
		g_assert (code - buf == 8);
		emit_bytes (acfg, buf, code - buf);
	}

	/* now the imt trampolines: each specific trampolines puts in the ip register
	 * the instruction pointer address, so the generic trampoline at the start of the page
	 * subtracts 4096 to get to the data page and loads the values
	 */
#define IMT_TRAMP_SIZE 72
	sprintf (symbol, "%simt_trampolines_page", acfg->user_symbol_prefix);
	emit_global (acfg, symbol, TRUE);
	emit_label (acfg, symbol);
	code = buf;
	/* Need at least two free registers, plus a slot for storing the pc */
	ARM_PUSH (code, (1 << ARMREG_R0)|(1 << ARMREG_R1)|(1 << ARMREG_R2));

	imm8 = mono_arm_is_rotated_imm8 (pagesize, &rot_amount);
	ARM_SUB_REG_IMM (code, ARMREG_IP, ARMREG_IP, imm8, rot_amount);
	ARM_LDR_IMM (code, ARMREG_R0, ARMREG_IP, -8);

	/* The IMT method is in v5, r0 has the imt array address */

	loop_start = code;
	ARM_LDR_IMM (code, ARMREG_R1, ARMREG_R0, 0);
	ARM_CMP_REG_REG (code, ARMREG_R1, ARMREG_V5);
	imt_found_check = code;
	ARM_B_COND (code, ARMCOND_EQ, 0);

	/* End-of-loop check */
	ARM_CMP_REG_IMM (code, ARMREG_R1, 0, 0);
	loop_end_check = code;
	ARM_B_COND (code, ARMCOND_EQ, 0);

	/* Loop footer */
	ARM_ADD_REG_IMM8 (code, ARMREG_R0, ARMREG_R0, sizeof (gpointer) * 2);
	loop_branch_back = code;
	ARM_B (code, 0);
	arm_patch (loop_branch_back, loop_start);

	/* Match */
	arm_patch (imt_found_check, code);
	ARM_LDR_IMM (code, ARMREG_R0, ARMREG_R0, 4);
	ARM_LDR_IMM (code, ARMREG_R0, ARMREG_R0, 0);
	/* Save it to the third stack slot */
	ARM_STR_IMM (code, ARMREG_R0, ARMREG_SP, 8);
	/* Restore the registers and branch */
	ARM_POP (code, (1 << ARMREG_R0)|(1 << ARMREG_R1)|(1 << ARMREG_PC));

	/* No match */
	arm_patch (loop_end_check, code);
	ARM_LDR_IMM (code, ARMREG_R0, ARMREG_R0, 4);
	ARM_STR_IMM (code, ARMREG_R0, ARMREG_SP, 8);
	ARM_POP (code, (1 << ARMREG_R0)|(1 << ARMREG_R1)|(1 << ARMREG_PC));
	ARM_NOP (code);

	/* Emit it */
	g_assert (code - buf == IMT_TRAMP_SIZE);
	emit_bytes (acfg, buf, code - buf);

	for (i = 0; i < count; ++i) {
		code = buf;
		ARM_MOV_REG_REG (code, ARMREG_IP, ARMREG_PC);
		ARM_B (code, 0);
		arm_patch (code - 4, code - IMT_TRAMP_SIZE - 8 * (i + 1));
		g_assert (code - buf == 8);
		emit_bytes (acfg, buf, code - buf);
	}

	acfg->tramp_page_code_offsets [MONO_AOT_TRAMP_SPECIFIC] = 16;
	acfg->tramp_page_code_offsets [MONO_AOT_TRAMP_STATIC_RGCTX] = 16;
	acfg->tramp_page_code_offsets [MONO_AOT_TRAMP_IMT] = 72;
	acfg->tramp_page_code_offsets [MONO_AOT_TRAMP_GSHAREDVT_ARG] = 16;

	/* Unwind info for specifc trampolines */
	sprintf (symbol, "%sspecific_trampolines_page_gen_p", acfg->user_symbol_prefix);
	/* We unwind to the original caller, from the stack, since lr is clobbered */
	mono_add_unwind_op_def_cfa (unwind_ops, 0, 0, ARMREG_SP, 14 * sizeof (mgreg_t));
	mono_add_unwind_op_offset (unwind_ops, 0, 0, ARMREG_LR, -4);
	save_unwind_info (acfg, symbol, unwind_ops);
	mono_free_unwind_info (unwind_ops);

	sprintf (symbol, "%sspecific_trampolines_page_sp_p", acfg->user_symbol_prefix);
	mono_add_unwind_op_def_cfa (unwind_ops, 0, 0, ARMREG_SP, 0);
	mono_add_unwind_op_def_cfa_offset (unwind_ops, 4, 0, 14 * sizeof (mgreg_t));
	save_unwind_info (acfg, symbol, unwind_ops);
	mono_free_unwind_info (unwind_ops);

	/* Unwind info for rgctx trampolines */
	sprintf (symbol, "%srgctx_trampolines_page_gen_p", acfg->user_symbol_prefix);
	mono_add_unwind_op_def_cfa (unwind_ops, 0, 0, ARMREG_SP, 0);
	save_unwind_info (acfg, symbol, unwind_ops);

	sprintf (symbol, "%srgctx_trampolines_page_sp_p", acfg->user_symbol_prefix);
	save_unwind_info (acfg, symbol, unwind_ops);
	mono_free_unwind_info (unwind_ops);

	/* Unwind info for gsharedvt trampolines */
	sprintf (symbol, "%sgsharedvt_trampolines_page_gen_p", acfg->user_symbol_prefix);
	mono_add_unwind_op_def_cfa (unwind_ops, 0, 0, ARMREG_SP, 0);
	mono_add_unwind_op_def_cfa_offset (unwind_ops, 4, 0, 4 * sizeof (mgreg_t));
	save_unwind_info (acfg, symbol, unwind_ops);
	mono_free_unwind_info (unwind_ops);

	sprintf (symbol, "%sgsharedvt_trampolines_page_sp_p", acfg->user_symbol_prefix);
	mono_add_unwind_op_def_cfa (unwind_ops, 0, 0, ARMREG_SP, 0);
	save_unwind_info (acfg, symbol, unwind_ops);
	mono_free_unwind_info (unwind_ops);

	/* Unwind info for imt trampolines */
	sprintf (symbol, "%simt_trampolines_page_gen_p", acfg->user_symbol_prefix);
	mono_add_unwind_op_def_cfa (unwind_ops, 0, 0, ARMREG_SP, 0);
	mono_add_unwind_op_def_cfa_offset (unwind_ops, 4, 0, 3 * sizeof (mgreg_t));
	save_unwind_info (acfg, symbol, unwind_ops);
	mono_free_unwind_info (unwind_ops);

	sprintf (symbol, "%simt_trampolines_page_sp_p", acfg->user_symbol_prefix);
	mono_add_unwind_op_def_cfa (unwind_ops, 0, 0, ARMREG_SP, 0);
	save_unwind_info (acfg, symbol, unwind_ops);
	mono_free_unwind_info (unwind_ops);
#elif defined(TARGET_ARM64)
	arm64_emit_specific_trampoline_pages (acfg);
#endif
}

/*
 * arch_emit_specific_trampoline:
 *
 *   Emit code for a specific trampoline. OFFSET is the offset of the first of
 * two GOT slots which contain the generic trampoline address and the trampoline
 * argument. TRAMP_SIZE is set to the size of the emitted trampoline.
 */
static void
arch_emit_specific_trampoline (MonoAotCompile *acfg, int offset, int *tramp_size)
{
	/*
	 * The trampolines created here are variations of the specific 
	 * trampolines created in mono_arch_create_specific_trampoline (). The 
	 * differences are:
	 * - the generic trampoline address is taken from a got slot.
	 * - the offset of the got slot where the trampoline argument is stored
	 *   is embedded in the instruction stream, and the generic trampoline
	 *   can load the argument by loading the offset, adding it to the
	 *   address of the trampoline to get the address of the got slot, and
	 *   loading the argument from there.
	 * - all the trampolines should be of the same length.
	 */
#if defined(TARGET_AMD64)
	/* This should be exactly 8 bytes long */
	*tramp_size = 8;
	/* call *<offset>(%rip) */
	if (acfg->llvm) {
		emit_unset_mode (acfg);
		fprintf (acfg->fp, "call *%s+%d(%%rip)\n", acfg->got_symbol, (int)(offset * sizeof (gpointer)));
		emit_zero_bytes (acfg, 2);
	} else {
		emit_byte (acfg, '\x41');
		emit_byte (acfg, '\xff');
		emit_byte (acfg, '\x15');
		emit_symbol_diff (acfg, acfg->got_symbol, ".", (offset * sizeof (gpointer)) - 4);
		emit_zero_bytes (acfg, 1);
	}
#elif defined(TARGET_ARM)
	guint8 buf [128];
	guint8 *code;

	/* This should be exactly 20 bytes long */
	*tramp_size = 20;
	code = buf;
	ARM_PUSH (code, 0x5fff);
	ARM_LDR_IMM (code, ARMREG_R1, ARMREG_PC, 4);
	/* Load the value from the GOT */
	ARM_LDR_REG_REG (code, ARMREG_R1, ARMREG_PC, ARMREG_R1);
	/* Branch to it */
	ARM_BLX_REG (code, ARMREG_R1);

	g_assert (code - buf == 16);

	/* Emit it */
	emit_bytes (acfg, buf, code - buf);
	/* 
	 * Only one offset is needed, since the second one would be equal to the
	 * first one.
	 */
	emit_symbol_diff (acfg, acfg->got_symbol, ".", (offset * sizeof (gpointer)) - 4 + 4);
	//emit_symbol_diff (acfg, acfg->got_symbol, ".", ((offset + 1) * sizeof (gpointer)) - 4 + 8);
#elif defined(TARGET_ARM64)
	arm64_emit_specific_trampoline (acfg, offset, tramp_size);
#elif defined(TARGET_POWERPC)
	guint8 buf [128];
	guint8 *code;

	*tramp_size = 4;
	code = buf;

	/*
	 * PPC has no ip relative addressing, so we need to compute the address
	 * of the mscorlib got. That is slow and complex, so instead, we store it
	 * in the second got slot of every aot image. The caller already computed
	 * the address of its got and placed it into r30.
	 */
	emit_unset_mode (acfg);
	/* Load mscorlib got address */
	fprintf (acfg->fp, "%s 0, %d(30)\n", PPC_LD_OP, (int)sizeof (gpointer));
	/* Load generic trampoline address */
	fprintf (acfg->fp, "lis 11, %d@h\n", (int)(offset * sizeof (gpointer)));
	fprintf (acfg->fp, "ori 11, 11, %d@l\n", (int)(offset * sizeof (gpointer)));
	fprintf (acfg->fp, "%s 11, 11, 0\n", PPC_LDX_OP);
#ifdef PPC_USES_FUNCTION_DESCRIPTOR
	fprintf (acfg->fp, "%s 11, 0(11)\n", PPC_LD_OP);
#endif
	fprintf (acfg->fp, "mtctr 11\n");
	/* Load trampoline argument */
	/* On ppc, we pass it normally to the generic trampoline */
	fprintf (acfg->fp, "lis 11, %d@h\n", (int)((offset + 1) * sizeof (gpointer)));
	fprintf (acfg->fp, "ori 11, 11, %d@l\n", (int)((offset + 1) * sizeof (gpointer)));
	fprintf (acfg->fp, "%s 0, 11, 0\n", PPC_LDX_OP);
	/* Branch to generic trampoline */
	fprintf (acfg->fp, "bctr\n");

#ifdef PPC_USES_FUNCTION_DESCRIPTOR
	*tramp_size = 10 * 4;
#else
	*tramp_size = 9 * 4;
#endif
#elif defined(TARGET_X86)
	guint8 buf [128];
	guint8 *code;

	/* Similar to the PPC code above */

	/* FIXME: Could this clobber the register needed by get_vcall_slot () ? */

	code = buf;
	/* Load mscorlib got address */
	x86_mov_reg_membase (code, X86_ECX, MONO_ARCH_GOT_REG, sizeof (gpointer), 4);
	/* Push trampoline argument */
	x86_push_membase (code, X86_ECX, (offset + 1) * sizeof (gpointer));
	/* Load generic trampoline address */
	x86_mov_reg_membase (code, X86_ECX, X86_ECX, offset * sizeof (gpointer), 4);
	/* Branch to generic trampoline */
	x86_jump_reg (code, X86_ECX);

	emit_bytes (acfg, buf, code - buf);

	*tramp_size = 17;
	g_assert (code - buf == *tramp_size);
#else
	g_assert_not_reached ();
#endif
}

/*
 * arch_emit_unbox_trampoline:
 *
 *   Emit code for the unbox trampoline for METHOD used in the full-aot case.
 * CALL_TARGET is the symbol pointing to the native code of METHOD.
 */
static void
arch_emit_unbox_trampoline (MonoAotCompile *acfg, MonoCompile *cfg, MonoMethod *method, const char *call_target)
{
#if defined(TARGET_AMD64)
	guint8 buf [32];
	guint8 *code;
	int this_reg;

	this_reg = mono_arch_get_this_arg_reg (NULL);
	code = buf;
	amd64_alu_reg_imm (code, X86_ADD, this_reg, sizeof (MonoObject));

	emit_bytes (acfg, buf, code - buf);
	/* jump <method> */
	if (acfg->llvm) {
		emit_unset_mode (acfg);
		fprintf (acfg->fp, "jmp %s\n", call_target);
	} else {
		emit_byte (acfg, '\xe9');
		emit_symbol_diff (acfg, call_target, ".", -4);
	}
#elif defined(TARGET_X86)
	guint8 buf [32];
	guint8 *code;
	int this_pos = 4;

	code = buf;

	x86_alu_membase_imm (code, X86_ADD, X86_ESP, this_pos, sizeof (MonoObject));

	emit_bytes (acfg, buf, code - buf);

	/* jump <method> */
	emit_byte (acfg, '\xe9');
	emit_symbol_diff (acfg, call_target, ".", -4);
#elif defined(TARGET_ARM)
	guint8 buf [128];
	guint8 *code;

	if (acfg->thumb_mixed && cfg->compile_llvm) {
		fprintf (acfg->fp, "add r0, r0, #%d\n", (int)sizeof (MonoObject));
		fprintf (acfg->fp, "b %s\n", call_target);
		fprintf (acfg->fp, ".arm\n");
		fprintf (acfg->fp, ".align 2\n");
		return;
	}

	code = buf;

	ARM_ADD_REG_IMM8 (code, ARMREG_R0, ARMREG_R0, sizeof (MonoObject));

	emit_bytes (acfg, buf, code - buf);
	/* jump to method */
	if (acfg->thumb_mixed && cfg->compile_llvm)
		fprintf (acfg->fp, "\n\tbx %s\n", call_target);
	else
		fprintf (acfg->fp, "\n\tb %s\n", call_target);
#elif defined(TARGET_ARM64)
	arm64_emit_unbox_trampoline (acfg, cfg, method, call_target);
#elif defined(TARGET_POWERPC)
	int this_pos = 3;

	fprintf (acfg->fp, "\n\taddi %d, %d, %d\n", this_pos, this_pos, (int)sizeof (MonoObject));
	fprintf (acfg->fp, "\n\tb %s\n", call_target);
#else
	g_assert_not_reached ();
#endif
}

/*
 * arch_emit_static_rgctx_trampoline:
 *
 *   Emit code for a static rgctx trampoline. OFFSET is the offset of the first of
 * two GOT slots which contain the rgctx argument, and the method to jump to.
 * TRAMP_SIZE is set to the size of the emitted trampoline.
 * These kinds of trampolines cannot be enumerated statically, since there could
 * be one trampoline per method instantiation, so we emit the same code for all
 * trampolines, and parameterize them using two GOT slots.
 */
static void
arch_emit_static_rgctx_trampoline (MonoAotCompile *acfg, int offset, int *tramp_size)
{
#if defined(TARGET_AMD64)
	/* This should be exactly 13 bytes long */
	*tramp_size = 13;

	if (acfg->llvm) {
		emit_unset_mode (acfg);
		fprintf (acfg->fp, "mov %s+%d(%%rip), %%r10\n", acfg->got_symbol, (int)(offset * sizeof (gpointer)));
		fprintf (acfg->fp, "jmp *%s+%d(%%rip)\n", acfg->got_symbol, (int)((offset + 1) * sizeof (gpointer)));
	} else {
		/* mov <OFFSET>(%rip), %r10 */
		emit_byte (acfg, '\x4d');
		emit_byte (acfg, '\x8b');
		emit_byte (acfg, '\x15');
		emit_symbol_diff (acfg, acfg->got_symbol, ".", (offset * sizeof (gpointer)) - 4);

		/* jmp *<offset>(%rip) */
		emit_byte (acfg, '\xff');
		emit_byte (acfg, '\x25');
		emit_symbol_diff (acfg, acfg->got_symbol, ".", ((offset + 1) * sizeof (gpointer)) - 4);
	}
#elif defined(TARGET_ARM)
	guint8 buf [128];
	guint8 *code;

	/* This should be exactly 24 bytes long */
	*tramp_size = 24;
	code = buf;
	/* Load rgctx value */
	ARM_LDR_IMM (code, ARMREG_IP, ARMREG_PC, 8);
	ARM_LDR_REG_REG (code, MONO_ARCH_RGCTX_REG, ARMREG_PC, ARMREG_IP);
	/* Load branch addr + branch */
	ARM_LDR_IMM (code, ARMREG_IP, ARMREG_PC, 4);
	ARM_LDR_REG_REG (code, ARMREG_PC, ARMREG_PC, ARMREG_IP);

	g_assert (code - buf == 16);

	/* Emit it */
	emit_bytes (acfg, buf, code - buf);
	emit_symbol_diff (acfg, acfg->got_symbol, ".", (offset * sizeof (gpointer)) - 4 + 8);
	emit_symbol_diff (acfg, acfg->got_symbol, ".", ((offset + 1) * sizeof (gpointer)) - 4 + 4);
#elif defined(TARGET_ARM64)
	arm64_emit_static_rgctx_trampoline (acfg, offset, tramp_size);
#elif defined(TARGET_POWERPC)
	guint8 buf [128];
	guint8 *code;

	*tramp_size = 4;
	code = buf;

	/*
	 * PPC has no ip relative addressing, so we need to compute the address
	 * of the mscorlib got. That is slow and complex, so instead, we store it
	 * in the second got slot of every aot image. The caller already computed
	 * the address of its got and placed it into r30.
	 */
	emit_unset_mode (acfg);
	/* Load mscorlib got address */
	fprintf (acfg->fp, "%s 0, %d(30)\n", PPC_LD_OP, (int)sizeof (gpointer));
	/* Load rgctx */
	fprintf (acfg->fp, "lis 11, %d@h\n", (int)(offset * sizeof (gpointer)));
	fprintf (acfg->fp, "ori 11, 11, %d@l\n", (int)(offset * sizeof (gpointer)));
	fprintf (acfg->fp, "%s %d, 11, 0\n", PPC_LDX_OP, MONO_ARCH_RGCTX_REG);
	/* Load target address */
	fprintf (acfg->fp, "lis 11, %d@h\n", (int)((offset + 1) * sizeof (gpointer)));
	fprintf (acfg->fp, "ori 11, 11, %d@l\n", (int)((offset + 1) * sizeof (gpointer)));
	fprintf (acfg->fp, "%s 11, 11, 0\n", PPC_LDX_OP);
#ifdef PPC_USES_FUNCTION_DESCRIPTOR
	fprintf (acfg->fp, "%s 2, %d(11)\n", PPC_LD_OP, (int)sizeof (gpointer));
	fprintf (acfg->fp, "%s 11, 0(11)\n", PPC_LD_OP);
#endif
	fprintf (acfg->fp, "mtctr 11\n");
	/* Branch to the target address */
	fprintf (acfg->fp, "bctr\n");

#ifdef PPC_USES_FUNCTION_DESCRIPTOR
	*tramp_size = 11 * 4;
#else
	*tramp_size = 9 * 4;
#endif

#elif defined(TARGET_X86)
	guint8 buf [128];
	guint8 *code;

	/* Similar to the PPC code above */

	g_assert (MONO_ARCH_RGCTX_REG != X86_ECX);

	code = buf;
	/* Load mscorlib got address */
	x86_mov_reg_membase (code, X86_ECX, MONO_ARCH_GOT_REG, sizeof (gpointer), 4);
	/* Load arg */
	x86_mov_reg_membase (code, MONO_ARCH_RGCTX_REG, X86_ECX, offset * sizeof (gpointer), 4);
	/* Branch to the target address */
	x86_jump_membase (code, X86_ECX, (offset + 1) * sizeof (gpointer));

	emit_bytes (acfg, buf, code - buf);

	*tramp_size = 15;
	g_assert (code - buf == *tramp_size);
#else
	g_assert_not_reached ();
#endif
}	

/*
 * arch_emit_imt_trampoline:
 *
 *   Emit an IMT trampoline usable in full-aot mode. The trampoline uses 1 got slot which
 * points to an array of pointer pairs. The pairs of the form [key, ptr], where
 * key is the IMT key, and ptr holds the address of a memory location holding
 * the address to branch to if the IMT arg matches the key. The array is 
 * terminated by a pair whose key is NULL, and whose ptr is the address of the 
 * fail_tramp.
 * TRAMP_SIZE is set to the size of the emitted trampoline.
 */
static void
arch_emit_imt_trampoline (MonoAotCompile *acfg, int offset, int *tramp_size)
{
#if defined(TARGET_AMD64)
	guint8 *buf, *code;
	guint8 *labels [16];
	guint8 mov_buf[3];
	guint8 *mov_buf_ptr = mov_buf;

	const int kSizeOfMove = 7;

	code = buf = (guint8 *)g_malloc (256);

	/* FIXME: Optimize this, i.e. use binary search etc. */
	/* Maybe move the body into a separate function (slower, but much smaller) */

	/* MONO_ARCH_IMT_SCRATCH_REG is a free register */

	if (acfg->llvm) {
		emit_unset_mode (acfg);
		fprintf (acfg->fp, "mov %s+%d(%%rip), %s\n", acfg->got_symbol, (int)(offset * sizeof (gpointer)), mono_arch_regname (MONO_ARCH_IMT_SCRATCH_REG));
	}

	labels [0] = code;
	amd64_alu_membase_imm (code, X86_CMP, MONO_ARCH_IMT_SCRATCH_REG, 0, 0);
	labels [1] = code;
	amd64_branch8 (code, X86_CC_Z, 0, FALSE);

	/* Check key */
	amd64_alu_membase_reg_size (code, X86_CMP, MONO_ARCH_IMT_SCRATCH_REG, 0, MONO_ARCH_IMT_REG, sizeof (gpointer));
	labels [2] = code;
	amd64_branch8 (code, X86_CC_Z, 0, FALSE);

	/* Loop footer */
	amd64_alu_reg_imm (code, X86_ADD, MONO_ARCH_IMT_SCRATCH_REG, 2 * sizeof (gpointer));
	amd64_jump_code (code, labels [0]);

	/* Match */
	mono_amd64_patch (labels [2], code);
	amd64_mov_reg_membase (code, MONO_ARCH_IMT_SCRATCH_REG, MONO_ARCH_IMT_SCRATCH_REG, sizeof (gpointer), sizeof (gpointer));
	amd64_jump_membase (code, MONO_ARCH_IMT_SCRATCH_REG, 0);

	/* No match */
	mono_amd64_patch (labels [1], code);
	/* Load fail tramp */
	amd64_alu_reg_imm (code, X86_ADD, MONO_ARCH_IMT_SCRATCH_REG, sizeof (gpointer));
	/* Check if there is a fail tramp */
	amd64_alu_membase_imm (code, X86_CMP, MONO_ARCH_IMT_SCRATCH_REG, 0, 0);
	labels [3] = code;
	amd64_branch8 (code, X86_CC_Z, 0, FALSE);
	/* Jump to fail tramp */
	amd64_jump_membase (code, MONO_ARCH_IMT_SCRATCH_REG, 0);

	/* Fail */
	mono_amd64_patch (labels [3], code);
	x86_breakpoint (code);

	if (!acfg->llvm) {
		/* mov <OFFSET>(%rip), MONO_ARCH_IMT_SCRATCH_REG */
		amd64_emit_rex (mov_buf_ptr, sizeof(gpointer), MONO_ARCH_IMT_SCRATCH_REG, 0, AMD64_RIP);
		*(mov_buf_ptr)++ = (unsigned char)0x8b; /* mov opcode */
		x86_address_byte (mov_buf_ptr, 0, MONO_ARCH_IMT_SCRATCH_REG & 0x7, 5);
		emit_bytes (acfg, mov_buf, mov_buf_ptr - mov_buf);
		emit_symbol_diff (acfg, acfg->got_symbol, ".", (offset * sizeof (gpointer)) - 4);
	}
	emit_bytes (acfg, buf, code - buf);

	*tramp_size = code - buf + kSizeOfMove;

	g_free (buf);

#elif defined(TARGET_X86)
	guint8 *buf, *code;
	guint8 *labels [16];

	code = buf = g_malloc (256);

	/* Allocate a temporary stack slot */
	x86_push_reg (code, X86_EAX);
	/* Save EAX */
	x86_push_reg (code, X86_EAX);

	/* Load mscorlib got address */
	x86_mov_reg_membase (code, X86_EAX, MONO_ARCH_GOT_REG, sizeof (gpointer), 4);
	/* Load arg */
	x86_mov_reg_membase (code, X86_EAX, X86_EAX, offset * sizeof (gpointer), 4);

	labels [0] = code;
	x86_alu_membase_imm (code, X86_CMP, X86_EAX, 0, 0);
	labels [1] = code;
	x86_branch8 (code, X86_CC_Z, FALSE, 0);

	/* Check key */
	x86_alu_membase_reg (code, X86_CMP, X86_EAX, 0, MONO_ARCH_IMT_REG);
	labels [2] = code;
	x86_branch8 (code, X86_CC_Z, FALSE, 0);

	/* Loop footer */
	x86_alu_reg_imm (code, X86_ADD, X86_EAX, 2 * sizeof (gpointer));
	x86_jump_code (code, labels [0]);

	/* Match */
	mono_x86_patch (labels [2], code);
	x86_mov_reg_membase (code, X86_EAX, X86_EAX, sizeof (gpointer), 4);
	x86_mov_reg_membase (code, X86_EAX, X86_EAX, 0, 4);
	/* Save the target address to the temporary stack location */
	x86_mov_membase_reg (code, X86_ESP, 4, X86_EAX, 4);
	/* Restore EAX */
	x86_pop_reg (code, X86_EAX);
	/* Jump to the target address */
	x86_ret (code);

	/* No match */
	mono_x86_patch (labels [1], code);
	/* Load fail tramp */
	x86_mov_reg_membase (code, X86_EAX, X86_EAX, sizeof (gpointer), 4);
	x86_alu_membase_imm (code, X86_CMP, X86_EAX, 0, 0);
	labels [3] = code;
	x86_branch8 (code, X86_CC_Z, FALSE, 0);
	/* Jump to fail tramp */
	x86_mov_membase_reg (code, X86_ESP, 4, X86_EAX, 4);
	x86_pop_reg (code, X86_EAX);
	x86_ret (code);

	/* Fail */
	mono_x86_patch (labels [3], code);
	x86_breakpoint (code);

	emit_bytes (acfg, buf, code - buf);
	
	*tramp_size = code - buf;

	g_free (buf);

#elif defined(TARGET_ARM)
	guint8 buf [128];
	guint8 *code, *code2, *labels [16];

	code = buf;

	/* The IMT method is in v5 */

	/* Need at least two free registers, plus a slot for storing the pc */
	ARM_PUSH (code, (1 << ARMREG_R0)|(1 << ARMREG_R1)|(1 << ARMREG_R2));
	labels [0] = code;
	/* Load the parameter from the GOT */
	ARM_LDR_IMM (code, ARMREG_R0, ARMREG_PC, 0);
	ARM_LDR_REG_REG (code, ARMREG_R0, ARMREG_PC, ARMREG_R0);

	labels [1] = code;
	ARM_LDR_IMM (code, ARMREG_R1, ARMREG_R0, 0);
	ARM_CMP_REG_REG (code, ARMREG_R1, ARMREG_V5);
	labels [2] = code;
	ARM_B_COND (code, ARMCOND_EQ, 0);

	/* End-of-loop check */
	ARM_CMP_REG_IMM (code, ARMREG_R1, 0, 0);
	labels [3] = code;
	ARM_B_COND (code, ARMCOND_EQ, 0);

	/* Loop footer */
	ARM_ADD_REG_IMM8 (code, ARMREG_R0, ARMREG_R0, sizeof (gpointer) * 2);
	labels [4] = code;
	ARM_B (code, 0);
	arm_patch (labels [4], labels [1]);

	/* Match */
	arm_patch (labels [2], code);
	ARM_LDR_IMM (code, ARMREG_R0, ARMREG_R0, 4);
	ARM_LDR_IMM (code, ARMREG_R0, ARMREG_R0, 0);
	/* Save it to the third stack slot */
	ARM_STR_IMM (code, ARMREG_R0, ARMREG_SP, 8);
	/* Restore the registers and branch */
	ARM_POP (code, (1 << ARMREG_R0)|(1 << ARMREG_R1)|(1 << ARMREG_PC));

	/* No match */
	arm_patch (labels [3], code);
	ARM_LDR_IMM (code, ARMREG_R0, ARMREG_R0, 4);
	ARM_STR_IMM (code, ARMREG_R0, ARMREG_SP, 8);
	ARM_POP (code, (1 << ARMREG_R0)|(1 << ARMREG_R1)|(1 << ARMREG_PC));

	/* Fixup offset */
	code2 = labels [0];
	ARM_LDR_IMM (code2, ARMREG_R0, ARMREG_PC, (code - (labels [0] + 8)));

	emit_bytes (acfg, buf, code - buf);
	emit_symbol_diff (acfg, acfg->got_symbol, ".", (offset * sizeof (gpointer)) + (code - (labels [0] + 8)) - 4);

	*tramp_size = code - buf + 4;
#elif defined(TARGET_ARM64)
	arm64_emit_imt_trampoline (acfg, offset, tramp_size);
#elif defined(TARGET_POWERPC)
	guint8 buf [128];
	guint8 *code, *labels [16];

	code = buf;

	/* Load the mscorlib got address */
	ppc_ldptr (code, ppc_r12, sizeof (gpointer), ppc_r30);
	/* Load the parameter from the GOT */
	ppc_load (code, ppc_r0, offset * sizeof (gpointer));
	ppc_ldptr_indexed (code, ppc_r12, ppc_r12, ppc_r0);

	/* Load and check key */
	labels [1] = code;
	ppc_ldptr (code, ppc_r0, 0, ppc_r12);
	ppc_cmp (code, 0, sizeof (gpointer) == 8 ? 1 : 0, ppc_r0, MONO_ARCH_IMT_REG);
	labels [2] = code;
	ppc_bc (code, PPC_BR_TRUE, PPC_BR_EQ, 0);

	/* End-of-loop check */
	ppc_cmpi (code, 0, sizeof (gpointer) == 8 ? 1 : 0, ppc_r0, 0);
	labels [3] = code;
	ppc_bc (code, PPC_BR_TRUE, PPC_BR_EQ, 0);

	/* Loop footer */
	ppc_addi (code, ppc_r12, ppc_r12, 2 * sizeof (gpointer));
	labels [4] = code;
	ppc_b (code, 0);
	mono_ppc_patch (labels [4], labels [1]);

	/* Match */
	mono_ppc_patch (labels [2], code);
	ppc_ldptr (code, ppc_r12, sizeof (gpointer), ppc_r12);
	/* r12 now contains the value of the vtable slot */
	/* this is not a function descriptor on ppc64 */
	ppc_ldptr (code, ppc_r12, 0, ppc_r12);
	ppc_mtctr (code, ppc_r12);
	ppc_bcctr (code, PPC_BR_ALWAYS, 0);

	/* Fail */
	mono_ppc_patch (labels [3], code);
	/* FIXME: */
	ppc_break (code);

	*tramp_size = code - buf;

	emit_bytes (acfg, buf, code - buf);
#else
	g_assert_not_reached ();
#endif
}


#if defined (TARGET_AMD64)

static void
amd64_emit_load_got_slot (MonoAotCompile *acfg, int dreg, int got_slot)
{

	g_assert (acfg->fp);
	emit_unset_mode (acfg);

	fprintf (acfg->fp, "mov %s+%d(%%rip), %s\n", acfg->got_symbol, (unsigned int) ((got_slot * sizeof (gpointer))), mono_arch_regname (dreg));
}

#endif


/*
 * arch_emit_gsharedvt_arg_trampoline:
 *
 *   Emit code for a gsharedvt arg trampoline. OFFSET is the offset of the first of
 * two GOT slots which contain the argument, and the code to jump to.
 * TRAMP_SIZE is set to the size of the emitted trampoline.
 * These kinds of trampolines cannot be enumerated statically, since there could
 * be one trampoline per method instantiation, so we emit the same code for all
 * trampolines, and parameterize them using two GOT slots.
 */
static void
arch_emit_gsharedvt_arg_trampoline (MonoAotCompile *acfg, int offset, int *tramp_size)
{
#if defined(TARGET_X86)
	guint8 buf [128];
	guint8 *code;

	/* Similar to the PPC code above */

	g_assert (MONO_ARCH_RGCTX_REG != X86_ECX);

	code = buf;
	/* Load mscorlib got address */
	x86_mov_reg_membase (code, X86_ECX, MONO_ARCH_GOT_REG, sizeof (gpointer), 4);
	/* Load arg */
	x86_mov_reg_membase (code, X86_EAX, X86_ECX, offset * sizeof (gpointer), 4);
	/* Branch to the target address */
	x86_jump_membase (code, X86_ECX, (offset + 1) * sizeof (gpointer));

	emit_bytes (acfg, buf, code - buf);

	*tramp_size = 15;
	g_assert (code - buf == *tramp_size);
#elif defined(TARGET_ARM)
	guint8 buf [128];
	guint8 *code;

	/* The same as mono_arch_get_gsharedvt_arg_trampoline (), but for AOT */
	/* Similar to arch_emit_specific_trampoline () */
	*tramp_size = 24;
	code = buf;
	ARM_PUSH (code, (1 << ARMREG_R0) | (1 << ARMREG_R1) | (1 << ARMREG_R2) | (1 << ARMREG_R3));
	ARM_LDR_IMM (code, ARMREG_R1, ARMREG_PC, 8);
	/* Load the arg value from the GOT */
	ARM_LDR_REG_REG (code, ARMREG_R0, ARMREG_PC, ARMREG_R1);
	/* Load the addr from the GOT */
	ARM_LDR_REG_REG (code, ARMREG_R1, ARMREG_PC, ARMREG_R1);
	/* Branch to it */
	ARM_BX (code, ARMREG_R1);

	g_assert (code - buf == 20);

	/* Emit it */
	emit_bytes (acfg, buf, code - buf);
	emit_symbol_diff (acfg, acfg->got_symbol, ".", (offset * sizeof (gpointer)) + 4);
#elif defined(TARGET_ARM64)
	arm64_emit_gsharedvt_arg_trampoline (acfg, offset, tramp_size);
#elif defined (TARGET_AMD64)

	amd64_emit_load_got_slot (acfg, AMD64_RAX, offset);
	amd64_emit_load_got_slot (acfg, MONO_ARCH_IMT_SCRATCH_REG, offset + 1);
	g_assert (AMD64_R11 == MONO_ARCH_IMT_SCRATCH_REG);
	fprintf (acfg->fp, "jmp *%%r11\n");

	*tramp_size = 0x11;
#else
	g_assert_not_reached ();
#endif
}	

/* END OF ARCH SPECIFIC CODE */

static guint32
mono_get_field_token (MonoClassField *field) 
{
	MonoClass *klass = field->parent;
	int i;

	int fcount = mono_class_get_field_count (klass);
	for (i = 0; i < fcount; ++i) {
		if (field == &klass->fields [i])
			return MONO_TOKEN_FIELD_DEF | (mono_class_get_first_field_idx (klass) + 1 + i);
	}

	g_assert_not_reached ();
	return 0;
}

static inline void
encode_value (gint32 value, guint8 *buf, guint8 **endbuf)
{
	guint8 *p = buf;

	//printf ("ENCODE: %d 0x%x.\n", value, value);

	/* 
	 * Same encoding as the one used in the metadata, extended to handle values
	 * greater than 0x1fffffff.
	 */
	if ((value >= 0) && (value <= 127))
		*p++ = value;
	else if ((value >= 0) && (value <= 16383)) {
		p [0] = 0x80 | (value >> 8);
		p [1] = value & 0xff;
		p += 2;
	} else if ((value >= 0) && (value <= 0x1fffffff)) {
		p [0] = (value >> 24) | 0xc0;
		p [1] = (value >> 16) & 0xff;
		p [2] = (value >> 8) & 0xff;
		p [3] = value & 0xff;
		p += 4;
	}
	else {
		p [0] = 0xff;
		p [1] = (value >> 24) & 0xff;
		p [2] = (value >> 16) & 0xff;
		p [3] = (value >> 8) & 0xff;
		p [4] = value & 0xff;
		p += 5;
	}
	if (endbuf)
		*endbuf = p;
}

static void
stream_init (MonoDynamicStream *sh)
{
	sh->index = 0;
	sh->alloc_size = 4096;
	sh->data = (char *)g_malloc (4096);

	/* So offsets are > 0 */
	sh->data [0] = 0;
	sh->index ++;
}

static void
make_room_in_stream (MonoDynamicStream *stream, int size)
{
	if (size <= stream->alloc_size)
		return;
	
	while (stream->alloc_size <= size) {
		if (stream->alloc_size < 4096)
			stream->alloc_size = 4096;
		else
			stream->alloc_size *= 2;
	}
	
	stream->data = (char *)g_realloc (stream->data, stream->alloc_size);
}

static guint32
add_stream_data (MonoDynamicStream *stream, const char *data, guint32 len)
{
	guint32 idx;
	
	make_room_in_stream (stream, stream->index + len);
	memcpy (stream->data + stream->index, data, len);
	idx = stream->index;
	stream->index += len;
	return idx;
}

/*
 * add_to_blob:
 *
 *   Add data to the binary blob inside the aot image. Returns the offset inside the
 * blob where the data was stored.
 */
static guint32
add_to_blob (MonoAotCompile *acfg, const guint8 *data, guint32 data_len)
{
	g_assert (!acfg->blob_closed);

	if (acfg->blob.alloc_size == 0)
		stream_init (&acfg->blob);

	return add_stream_data (&acfg->blob, (char*)data, data_len);
}

static guint32
add_to_blob_aligned (MonoAotCompile *acfg, const guint8 *data, guint32 data_len, guint32 align)
{
	char buf [4] = {0};
	guint32 count;

	if (acfg->blob.alloc_size == 0)
		stream_init (&acfg->blob);

	count = acfg->blob.index % align;

	/* we assume the stream data will be aligned */
	if (count)
		add_stream_data (&acfg->blob, buf, 4 - count);

	return add_stream_data (&acfg->blob, (char*)data, data_len);
}

/* Emit a table of data into the aot image */
static void
emit_aot_data (MonoAotCompile *acfg, MonoAotFileTable table, const char *symbol, guint8 *data, int size)
{
	if (acfg->data_outfile) {
		acfg->table_offsets [(int)table] = acfg->datafile_offset;
		fwrite (data,1, size, acfg->data_outfile);
		acfg->datafile_offset += size;
		// align the data to 8 bytes. Put zeros in the file (so that every build results in consistent output).
		int align = 8 - size % 8;
		acfg->datafile_offset += align;
		guint8 align_buf [16];
		memset (&align_buf, 0, sizeof (align_buf));
		fwrite (align_buf, align, 1, acfg->data_outfile);
	} else if (acfg->llvm) {
		mono_llvm_emit_aot_data (symbol, data, size);
	} else {
		emit_section_change (acfg, RODATA_SECT, 0);
		emit_alignment (acfg, 8);
		emit_label (acfg, symbol);
		emit_bytes (acfg, data, size);
	}
}

/*
 * emit_offset_table:
 *
 *   Emit a table of increasing offsets in a compact form using differential encoding.
 * There is an index entry for each GROUP_SIZE number of entries. The greater the
 * group size, the more compact the table becomes, but the slower it becomes to compute
 * a given entry. Returns the size of the table.
 */
static guint32
emit_offset_table (MonoAotCompile *acfg, const char *symbol, MonoAotFileTable table, int noffsets, int group_size, gint32 *offsets)
{
	gint32 current_offset;
	int i, buf_size, ngroups, index_entry_size;
	guint8 *p, *buf;
	guint8 *data_p, *data_buf;
	guint32 *index_offsets;

	ngroups = (noffsets + (group_size - 1)) / group_size;

	index_offsets = g_new0 (guint32, ngroups);

	buf_size = noffsets * 4;
	p = buf = (guint8 *)g_malloc0 (buf_size);

	current_offset = 0;
	for (i = 0; i < noffsets; ++i) {
		//printf ("D: %d -> %d\n", i, offsets [i]);
		if ((i % group_size) == 0) {
			index_offsets [i / group_size] = p - buf;
			/* Emit the full value for these entries */
			encode_value (offsets [i], p, &p);
		} else {
			/* The offsets are allowed to be non-increasing */
			//g_assert (offsets [i] >= current_offset);
			encode_value (offsets [i] - current_offset, p, &p);
		}
		current_offset = offsets [i];
	}
	data_buf = buf;
	data_p = p;

	if (ngroups && index_offsets [ngroups - 1] < 65000)
		index_entry_size = 2;
	else
		index_entry_size = 4;

	buf_size = (data_p - data_buf) + (ngroups * 4) + 16;
	p = buf = (guint8 *)g_malloc0 (buf_size);

	/* Emit the header */
	encode_int (noffsets, p, &p);
	encode_int (group_size, p, &p);
	encode_int (ngroups, p, &p);
	encode_int (index_entry_size, p, &p);

	/* Emit the index */
	for (i = 0; i < ngroups; ++i) {
		if (index_entry_size == 2)
			encode_int16 (index_offsets [i], p, &p);
		else
			encode_int (index_offsets [i], p, &p);
	}
	/* Emit the data */
	memcpy (p, data_buf, data_p - data_buf);
	p += data_p - data_buf;

	g_assert (p - buf <= buf_size);

	emit_aot_data (acfg, table, symbol, buf, p - buf);

	g_free (buf);
	g_free (data_buf);

    return (int)(p - buf);
}

static guint32
get_image_index (MonoAotCompile *cfg, MonoImage *image)
{
	guint32 index;

	index = GPOINTER_TO_UINT (g_hash_table_lookup (cfg->image_hash, image));
	if (index)
		return index - 1;
	else {
		index = g_hash_table_size (cfg->image_hash);
		g_hash_table_insert (cfg->image_hash, image, GUINT_TO_POINTER (index + 1));
		g_ptr_array_add (cfg->image_table, image);
		return index;
	}
}

static guint32
find_typespec_for_class (MonoAotCompile *acfg, MonoClass *klass)
{
	int i;
	int len = acfg->image->tables [MONO_TABLE_TYPESPEC].rows;

	/* FIXME: Search referenced images as well */
	if (!acfg->typespec_classes) {
		acfg->typespec_classes = g_hash_table_new (NULL, NULL);
		for (i = 0; i < len; i++) {
			MonoError error;
			int typespec = MONO_TOKEN_TYPE_SPEC | (i + 1);
			MonoClass *klass_key = mono_class_get_and_inflate_typespec_checked (acfg->image, typespec, NULL, &error);
			if (!is_ok (&error)) {
				mono_error_cleanup (&error);
				continue;
			}
			g_hash_table_insert (acfg->typespec_classes, klass_key, GINT_TO_POINTER (typespec));
		}
	}
	return GPOINTER_TO_INT (g_hash_table_lookup (acfg->typespec_classes, klass));
}

static void
encode_method_ref (MonoAotCompile *acfg, MonoMethod *method, guint8 *buf, guint8 **endbuf);

static void
encode_klass_ref (MonoAotCompile *acfg, MonoClass *klass, guint8 *buf, guint8 **endbuf);

static void
encode_ginst (MonoAotCompile *acfg, MonoGenericInst *inst, guint8 *buf, guint8 **endbuf);

static void
encode_type (MonoAotCompile *acfg, MonoType *t, guint8 *buf, guint8 **endbuf);

static void
encode_klass_ref_inner (MonoAotCompile *acfg, MonoClass *klass, guint8 *buf, guint8 **endbuf)
{
	guint8 *p = buf;

	/*
	 * The encoding begins with one of the MONO_AOT_TYPEREF values, followed by additional
	 * information.
	 */

	if (mono_class_is_ginst (klass)) {
		guint32 token;
		g_assert (klass->type_token);

		/* Find a typespec for a class if possible */
		token = find_typespec_for_class (acfg, klass);
		if (token) {
			encode_value (MONO_AOT_TYPEREF_TYPESPEC_TOKEN, p, &p);
			encode_value (token, p, &p);
		} else {
			MonoClass *gclass = mono_class_get_generic_class (klass)->container_class;
			MonoGenericInst *inst = mono_class_get_generic_class (klass)->context.class_inst;
			static int count = 0;
			guint8 *p1 = p;

			encode_value (MONO_AOT_TYPEREF_GINST, p, &p);
			encode_klass_ref (acfg, gclass, p, &p);
			encode_ginst (acfg, inst, p, &p);

			count += p - p1;
		}
	} else if (klass->type_token) {
		int iindex = get_image_index (acfg, klass->image);

		g_assert (mono_metadata_token_code (klass->type_token) == MONO_TOKEN_TYPE_DEF);
		if (iindex == 0) {
			encode_value (MONO_AOT_TYPEREF_TYPEDEF_INDEX, p, &p);
			encode_value (klass->type_token - MONO_TOKEN_TYPE_DEF, p, &p);
		} else {
			encode_value (MONO_AOT_TYPEREF_TYPEDEF_INDEX_IMAGE, p, &p);
			encode_value (klass->type_token - MONO_TOKEN_TYPE_DEF, p, &p);
			encode_value (get_image_index (acfg, klass->image), p, &p);
		}
	} else if ((klass->byval_arg.type == MONO_TYPE_VAR) || (klass->byval_arg.type == MONO_TYPE_MVAR)) {
		MonoGenericContainer *container = mono_type_get_generic_param_owner (&klass->byval_arg);
		MonoGenericParam *par = klass->byval_arg.data.generic_param;

		encode_value (MONO_AOT_TYPEREF_VAR, p, &p);

		encode_value (par->gshared_constraint ? 1 : 0, p, &p);
		if (par->gshared_constraint) {
			MonoGSharedGenericParam *gpar = (MonoGSharedGenericParam*)par;
			encode_type (acfg, par->gshared_constraint, p, &p);
			encode_klass_ref (acfg, mono_class_from_generic_parameter (gpar->parent, NULL, klass->byval_arg.type == MONO_TYPE_MVAR), p, &p);
		} else {
			encode_value (klass->byval_arg.type, p, &p);
			encode_value (mono_type_get_generic_param_num (&klass->byval_arg), p, &p);

			encode_value (container->is_anonymous ? 0 : 1, p, &p);

			if (!container->is_anonymous) {
				encode_value (container->is_method, p, &p);
				if (container->is_method)
					encode_method_ref (acfg, container->owner.method, p, &p);
				else
					encode_klass_ref (acfg, container->owner.klass, p, &p);
			}
		}
	} else if (klass->byval_arg.type == MONO_TYPE_PTR) {
		encode_value (MONO_AOT_TYPEREF_PTR, p, &p);
		encode_type (acfg, &klass->byval_arg, p, &p);
	} else {
		/* Array class */
		g_assert (klass->rank > 0);
		encode_value (MONO_AOT_TYPEREF_ARRAY, p, &p);
		encode_value (klass->rank, p, &p);
		encode_klass_ref (acfg, klass->element_class, p, &p);
	}
	*endbuf = p;
}

/*
 * encode_klass_ref:
 *
 *   Encode a reference to KLASS. We use our home-grown encoding instead of the
 * standard metadata encoding.
 */
static void
encode_klass_ref (MonoAotCompile *acfg, MonoClass *klass, guint8 *buf, guint8 **endbuf)
{
	gboolean shared = FALSE;

	/* 
	 * The encoding of generic instances is large so emit them only once.
	 */
	if (mono_class_is_ginst (klass)) {
		guint32 token;
		g_assert (klass->type_token);

		/* Find a typespec for a class if possible */
		token = find_typespec_for_class (acfg, klass);
		if (!token)
			shared = TRUE;
	} else if ((klass->byval_arg.type == MONO_TYPE_VAR) || (klass->byval_arg.type == MONO_TYPE_MVAR)) {
		shared = TRUE;
	}

	if (shared) {
		guint offset = GPOINTER_TO_UINT (g_hash_table_lookup (acfg->klass_blob_hash, klass));
		guint8 *buf2, *p;

		if (!offset) {
			buf2 = (guint8 *)g_malloc (1024);
			p = buf2;

			encode_klass_ref_inner (acfg, klass, p, &p);
			g_assert (p - buf2 < 1024);

			offset = add_to_blob (acfg, buf2, p - buf2);
			g_free (buf2);

			g_hash_table_insert (acfg->klass_blob_hash, klass, GUINT_TO_POINTER (offset + 1));
		} else {
			offset --;
		}

		p = buf;
		encode_value (MONO_AOT_TYPEREF_BLOB_INDEX, p, &p);
		encode_value (offset, p, &p);
		*endbuf = p;
		return;
	}

	encode_klass_ref_inner (acfg, klass, buf, endbuf);
}

static void
encode_field_info (MonoAotCompile *cfg, MonoClassField *field, guint8 *buf, guint8 **endbuf)
{
	guint32 token = mono_get_field_token (field);
	guint8 *p = buf;

	encode_klass_ref (cfg, field->parent, p, &p);
	g_assert (mono_metadata_token_code (token) == MONO_TOKEN_FIELD_DEF);
	encode_value (token - MONO_TOKEN_FIELD_DEF, p, &p);
	*endbuf = p;
}

static void
encode_ginst (MonoAotCompile *acfg, MonoGenericInst *inst, guint8 *buf, guint8 **endbuf)
{
	guint8 *p = buf;
	int i;

	encode_value (inst->type_argc, p, &p);
	for (i = 0; i < inst->type_argc; ++i)
		encode_klass_ref (acfg, mono_class_from_mono_type (inst->type_argv [i]), p, &p);
	*endbuf = p;
}

static void
encode_generic_context (MonoAotCompile *acfg, MonoGenericContext *context, guint8 *buf, guint8 **endbuf)
{
	guint8 *p = buf;
	MonoGenericInst *inst;

	inst = context->class_inst;
	if (inst) {
		g_assert (inst->type_argc);
		encode_ginst (acfg, inst, p, &p);
	} else {
		encode_value (0, p, &p);
	}
	inst = context->method_inst;
	if (inst) {
		g_assert (inst->type_argc);
		encode_ginst (acfg, inst, p, &p);
	} else {
		encode_value (0, p, &p);
	}
	*endbuf = p;
}

static void
encode_type (MonoAotCompile *acfg, MonoType *t, guint8 *buf, guint8 **endbuf)
{
	guint8 *p = buf;

	g_assert (t->num_mods == 0);
	/* t->attrs can be ignored */
	//g_assert (t->attrs == 0);

	if (t->pinned) {
		*p = MONO_TYPE_PINNED;
		++p;
	}
	if (t->byref) {
		*p = MONO_TYPE_BYREF;
		++p;
	}

	*p = t->type;
	p ++;

	switch (t->type) {
	case MONO_TYPE_VOID:
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_STRING:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_TYPEDBYREF:
		break;
	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_CLASS:
		encode_klass_ref (acfg, mono_class_from_mono_type (t), p, &p);
		break;
	case MONO_TYPE_SZARRAY:
		encode_klass_ref (acfg, t->data.klass, p, &p);
		break;
	case MONO_TYPE_PTR:
		encode_type (acfg, t->data.type, p, &p);
		break;
	case MONO_TYPE_GENERICINST: {
		MonoClass *gclass = t->data.generic_class->container_class;
		MonoGenericInst *inst = t->data.generic_class->context.class_inst;

		encode_klass_ref (acfg, gclass, p, &p);
		encode_ginst (acfg, inst, p, &p);
		break;
	}
	case MONO_TYPE_ARRAY: {
		MonoArrayType *array = t->data.array;
		int i;

		encode_klass_ref (acfg, array->eklass, p, &p);
		encode_value (array->rank, p, &p);
		encode_value (array->numsizes, p, &p);
		for (i = 0; i < array->numsizes; ++i)
			encode_value (array->sizes [i], p, &p);
		encode_value (array->numlobounds, p, &p);
		for (i = 0; i < array->numlobounds; ++i)
			encode_value (array->lobounds [i], p, &p);
		break;
	}
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		encode_klass_ref (acfg, mono_class_from_mono_type (t), p, &p);
		break;
	default:
		g_assert_not_reached ();
	}

	*endbuf = p;
}

static void
encode_signature (MonoAotCompile *acfg, MonoMethodSignature *sig, guint8 *buf, guint8 **endbuf)
{
	guint8 *p = buf;
	guint32 flags = 0;
	int i;

	/* Similar to the metadata encoding */
	if (sig->generic_param_count)
		flags |= 0x10;
	if (sig->hasthis)
		flags |= 0x20;
	if (sig->explicit_this)
		flags |= 0x40;
	flags |= (sig->call_convention & 0x0F);

	*p = flags;
	++p;
	if (sig->generic_param_count)
		encode_value (sig->generic_param_count, p, &p);
	encode_value (sig->param_count, p, &p);

	encode_type (acfg, sig->ret, p, &p);
	for (i = 0; i < sig->param_count; ++i) {
		if (sig->sentinelpos == i) {
			*p = MONO_TYPE_SENTINEL;
			++p;
		}
		encode_type (acfg, sig->params [i], p, &p);
	}

	*endbuf = p;
}

#define MAX_IMAGE_INDEX 250

static void
encode_method_ref (MonoAotCompile *acfg, MonoMethod *method, guint8 *buf, guint8 **endbuf)
{
	guint32 image_index = get_image_index (acfg, method->klass->image);
	guint32 token = method->token;
	MonoJumpInfoToken *ji;
	guint8 *p = buf;

	/*
	 * The encoding for most methods is as follows:
	 * - image index encoded as a leb128
	 * - token index encoded as a leb128
	 * Values of image index >= MONO_AOT_METHODREF_MIN are used to mark additional
	 * types of method encodings.
	 */

	/* Mark methods which can't use aot trampolines because they need the further 
	 * processing in mono_magic_trampoline () which requires a MonoMethod*.
	 */
	if ((method->is_generic && (method->flags & METHOD_ATTRIBUTE_VIRTUAL)) ||
		(method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED))
		encode_value ((MONO_AOT_METHODREF_NO_AOT_TRAMPOLINE << 24), p, &p);

	if (method->wrapper_type) {
		WrapperInfo *info = mono_marshal_get_wrapper_info (method);

		encode_value ((MONO_AOT_METHODREF_WRAPPER << 24), p, &p);

		encode_value (method->wrapper_type, p, &p);

		switch (method->wrapper_type) {
		case MONO_WRAPPER_REMOTING_INVOKE:
		case MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK:
		case MONO_WRAPPER_XDOMAIN_INVOKE: {
			MonoMethod *m;

			m = mono_marshal_method_from_wrapper (method);
			g_assert (m);
			encode_method_ref (acfg, m, p, &p);
			break;
		}
		case MONO_WRAPPER_PROXY_ISINST:
		case MONO_WRAPPER_LDFLD:
		case MONO_WRAPPER_LDFLDA:
		case MONO_WRAPPER_STFLD: {
			g_assert (info);
			encode_klass_ref (acfg, info->d.proxy.klass, p, &p);
			break;
		}
		case MONO_WRAPPER_ALLOC: {
			/* The GC name is saved once in MonoAotFileInfo */
			g_assert (info->d.alloc.alloc_type != -1);
			encode_value (info->d.alloc.alloc_type, p, &p);
			break;
		}
		case MONO_WRAPPER_WRITE_BARRIER: {
			g_assert (info);
			break;
		}
		case MONO_WRAPPER_STELEMREF: {
			g_assert (info);
			encode_value (info->subtype, p, &p);
			if (info->subtype == WRAPPER_SUBTYPE_VIRTUAL_STELEMREF)
				encode_value (info->d.virtual_stelemref.kind, p, &p);
			break;
		}
		case MONO_WRAPPER_UNKNOWN: {
			g_assert (info);
			encode_value (info->subtype, p, &p);
			if (info->subtype == WRAPPER_SUBTYPE_PTR_TO_STRUCTURE ||
				info->subtype == WRAPPER_SUBTYPE_STRUCTURE_TO_PTR)
				encode_klass_ref (acfg, method->klass, p, &p);
			else if (info->subtype == WRAPPER_SUBTYPE_SYNCHRONIZED_INNER)
				encode_method_ref (acfg, info->d.synchronized_inner.method, p, &p);
			else if (info->subtype == WRAPPER_SUBTYPE_ARRAY_ACCESSOR)
				encode_method_ref (acfg, info->d.array_accessor.method, p, &p);
			else if (info->subtype == WRAPPER_SUBTYPE_GSHAREDVT_IN_SIG)
				encode_signature (acfg, info->d.gsharedvt.sig, p, &p);
			else if (info->subtype == WRAPPER_SUBTYPE_GSHAREDVT_OUT_SIG)
				encode_signature (acfg, info->d.gsharedvt.sig, p, &p);
			break;
		}
		case MONO_WRAPPER_MANAGED_TO_NATIVE: {
			g_assert (info);
			encode_value (info->subtype, p, &p);
			if (info->subtype == WRAPPER_SUBTYPE_ICALL_WRAPPER) {
				strcpy ((char*)p, method->name);
				p += strlen (method->name) + 1;
			} else if (info->subtype == WRAPPER_SUBTYPE_NATIVE_FUNC_AOT) {
				encode_method_ref (acfg, info->d.managed_to_native.method, p, &p);
			} else {
				g_assert (info->subtype == WRAPPER_SUBTYPE_NONE || info->subtype == WRAPPER_SUBTYPE_PINVOKE);
				encode_method_ref (acfg, info->d.managed_to_native.method, p, &p);
			}
			break;
		}
		case MONO_WRAPPER_SYNCHRONIZED: {
			MonoMethod *m;

			m = mono_marshal_method_from_wrapper (method);
			g_assert (m);
			g_assert (m != method);
			encode_method_ref (acfg, m, p, &p);
			break;
		}
		case MONO_WRAPPER_MANAGED_TO_MANAGED: {
			g_assert (info);
			encode_value (info->subtype, p, &p);

			if (info->subtype == WRAPPER_SUBTYPE_ELEMENT_ADDR) {
				encode_value (info->d.element_addr.rank, p, &p);
				encode_value (info->d.element_addr.elem_size, p, &p);
			} else if (info->subtype == WRAPPER_SUBTYPE_STRING_CTOR) {
				encode_method_ref (acfg, info->d.string_ctor.method, p, &p);
			} else {
				g_assert_not_reached ();
			}
			break;
		}
		case MONO_WRAPPER_CASTCLASS: {
			g_assert (info);
			encode_value (info->subtype, p, &p);
			break;
		}
		case MONO_WRAPPER_RUNTIME_INVOKE: {
			g_assert (info);
			encode_value (info->subtype, p, &p);
			if (info->subtype == WRAPPER_SUBTYPE_RUNTIME_INVOKE_DIRECT || info->subtype == WRAPPER_SUBTYPE_RUNTIME_INVOKE_VIRTUAL)
				encode_method_ref (acfg, info->d.runtime_invoke.method, p, &p);
			else if (info->subtype == WRAPPER_SUBTYPE_RUNTIME_INVOKE_NORMAL)
				encode_signature (acfg, info->d.runtime_invoke.sig, p, &p);
			break;
		}
		case MONO_WRAPPER_DELEGATE_INVOKE:
		case MONO_WRAPPER_DELEGATE_BEGIN_INVOKE:
		case MONO_WRAPPER_DELEGATE_END_INVOKE: {
			if (method->is_inflated) {
				/* These wrappers are identified by their class */
				encode_value (1, p, &p);
				encode_klass_ref (acfg, method->klass, p, &p);
			} else {
				MonoMethodSignature *sig = mono_method_signature (method);
				WrapperInfo *info = mono_marshal_get_wrapper_info (method);

				encode_value (0, p, &p);
				if (method->wrapper_type == MONO_WRAPPER_DELEGATE_INVOKE)
					encode_value (info ? info->subtype : 0, p, &p);
				encode_signature (acfg, sig, p, &p);
			}
			break;
		}
		case MONO_WRAPPER_NATIVE_TO_MANAGED: {
			g_assert (info);
			encode_method_ref (acfg, info->d.native_to_managed.method, p, &p);
			encode_klass_ref (acfg, info->d.native_to_managed.klass, p, &p);
			break;
		}
		default:
			g_assert_not_reached ();
		}
	} else if (mono_method_signature (method)->is_inflated) {
		/* 
		 * This is a generic method, find the original token which referenced it and
		 * encode that.
		 * Obtain the token from information recorded by the JIT.
		 */
		ji = (MonoJumpInfoToken *)g_hash_table_lookup (acfg->token_info_hash, method);
		if (ji) {
			image_index = get_image_index (acfg, ji->image);
			g_assert (image_index < MAX_IMAGE_INDEX);
			token = ji->token;

			encode_value ((MONO_AOT_METHODREF_METHODSPEC << 24), p, &p);
			encode_value (image_index, p, &p);
			encode_value (token, p, &p);
		} else {
			MonoMethod *declaring;
			MonoGenericContext *context = mono_method_get_context (method);

			g_assert (method->is_inflated);
			declaring = ((MonoMethodInflated*)method)->declaring;

			/*
			 * This might be a non-generic method of a generic instance, which 
			 * doesn't have a token since the reference is generated by the JIT 
			 * like Nullable:Box/Unbox, or by generic sharing.
			 */
			encode_value ((MONO_AOT_METHODREF_GINST << 24), p, &p);
			/* Encode the klass */
			encode_klass_ref (acfg, method->klass, p, &p);
			/* Encode the method */
			image_index = get_image_index (acfg, method->klass->image);
			g_assert (image_index < MAX_IMAGE_INDEX);
			g_assert (declaring->token);
			token = declaring->token;
			g_assert (mono_metadata_token_table (token) == MONO_TABLE_METHOD);
			encode_value (image_index, p, &p);
			encode_value (token, p, &p);
			encode_generic_context (acfg, context, p, &p);
		}
	} else if (token == 0) {
		/* This might be a method of a constructed type like int[,].Set */
		/* Obtain the token from information recorded by the JIT */
		ji = (MonoJumpInfoToken *)g_hash_table_lookup (acfg->token_info_hash, method);
		if (ji) {
			image_index = get_image_index (acfg, ji->image);
			g_assert (image_index < MAX_IMAGE_INDEX);
			token = ji->token;

			encode_value ((MONO_AOT_METHODREF_METHODSPEC << 24), p, &p);
			encode_value (image_index, p, &p);
			encode_value (token, p, &p);
		} else {
			/* Array methods */
			g_assert (method->klass->rank);

			/* Encode directly */
			encode_value ((MONO_AOT_METHODREF_ARRAY << 24), p, &p);
			encode_klass_ref (acfg, method->klass, p, &p);
			if (!strcmp (method->name, ".ctor") && mono_method_signature (method)->param_count == method->klass->rank)
				encode_value (0, p, &p);
			else if (!strcmp (method->name, ".ctor") && mono_method_signature (method)->param_count == method->klass->rank * 2)
				encode_value (1, p, &p);
			else if (!strcmp (method->name, "Get"))
				encode_value (2, p, &p);
			else if (!strcmp (method->name, "Address"))
				encode_value (3, p, &p);
			else if (!strcmp (method->name, "Set"))
				encode_value (4, p, &p);
			else
				g_assert_not_reached ();
		}
	} else {
		g_assert (mono_metadata_token_table (token) == MONO_TABLE_METHOD);

		if (image_index >= MONO_AOT_METHODREF_MIN) {
			encode_value ((MONO_AOT_METHODREF_LARGE_IMAGE_INDEX << 24), p, &p);
			encode_value (image_index, p, &p);
			encode_value (mono_metadata_token_index (token), p, &p);
		} else {
			encode_value ((image_index << 24) | mono_metadata_token_index (token), p, &p);
		}
	}
	*endbuf = p;
}

static gint
compare_patches (gconstpointer a, gconstpointer b)
{
	int i, j;

	i = (*(MonoJumpInfo**)a)->ip.i;
	j = (*(MonoJumpInfo**)b)->ip.i;

	if (i < j)
		return -1;
	else
		if (i > j)
			return 1;
	else
		return 0;
}

static G_GNUC_UNUSED char*
patch_to_string (MonoJumpInfo *patch_info)
{
	GString *str;

	str = g_string_new ("");

	g_string_append_printf (str, "%s(", get_patch_name (patch_info->type));

	switch (patch_info->type) {
	case MONO_PATCH_INFO_VTABLE:
		mono_type_get_desc (str, &patch_info->data.klass->byval_arg, TRUE);
		break;
	default:
		break;
	}
	g_string_append_printf (str, ")");
	return g_string_free (str, FALSE);
}

/*
 * is_plt_patch:
 *
 *   Return whenever PATCH_INFO refers to a direct call, and thus requires a
 * PLT entry.
 */
static inline gboolean
is_plt_patch (MonoJumpInfo *patch_info)
{
	switch (patch_info->type) {
	case MONO_PATCH_INFO_METHOD:
	case MONO_PATCH_INFO_INTERNAL_METHOD:
	case MONO_PATCH_INFO_JIT_ICALL_ADDR:
	case MONO_PATCH_INFO_ICALL_ADDR_CALL:
	case MONO_PATCH_INFO_RGCTX_FETCH:
		return TRUE;
	default:
		return FALSE;
	}
}

/*
 * get_plt_symbol:
 *
 *   Return the symbol identifying the plt entry PLT_OFFSET.
 */
static char*
get_plt_symbol (MonoAotCompile *acfg, int plt_offset, MonoJumpInfo *patch_info)
{
#ifdef TARGET_MACH
	/* 
	 * The Apple linker reorganizes object files, so it doesn't like branches to local
	 * labels, since those have no relocations.
	 */
	return g_strdup_printf ("%sp_%d", acfg->llvm_label_prefix, plt_offset);
#else
	return g_strdup_printf ("%sp_%d", acfg->temp_prefix, plt_offset);
#endif
}

/*
 * get_plt_entry:
 *
 *   Return a PLT entry which belongs to the method identified by PATCH_INFO.
 */
static MonoPltEntry*
get_plt_entry (MonoAotCompile *acfg, MonoJumpInfo *patch_info)
{
	MonoPltEntry *res;
	gboolean synchronized = FALSE;
	static int synchronized_symbol_idx;

	if (!is_plt_patch (patch_info))
		return NULL;

	if (!acfg->patch_to_plt_entry [patch_info->type])
		acfg->patch_to_plt_entry [patch_info->type] = g_hash_table_new (mono_patch_info_hash, mono_patch_info_equal);
	res = (MonoPltEntry *)g_hash_table_lookup (acfg->patch_to_plt_entry [patch_info->type], patch_info);

	if (!acfg->llvm && patch_info->type == MONO_PATCH_INFO_METHOD && (patch_info->data.method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)) {
		/* 
		 * Allocate a separate PLT slot for each such patch, since some plt
		 * entries will refer to the method itself, and some will refer to the
		 * wrapper.
		 */
		res = NULL;
		synchronized = TRUE;
	}

	if (!res) {
		MonoJumpInfo *new_ji;

		new_ji = mono_patch_info_dup_mp (acfg->mempool, patch_info);

		res = (MonoPltEntry *)mono_mempool_alloc0 (acfg->mempool, sizeof (MonoPltEntry));
		res->plt_offset = acfg->plt_offset;
		res->ji = new_ji;
		res->symbol = get_plt_symbol (acfg, res->plt_offset, patch_info);
		if (acfg->aot_opts.write_symbols)
			res->debug_sym = get_plt_entry_debug_sym (acfg, res->ji, acfg->plt_entry_debug_sym_cache);
		if (synchronized) {
			/* Avoid duplicate symbols because we don't cache */
			res->symbol = g_strdup_printf ("%s_%d", res->symbol, synchronized_symbol_idx);
			if (res->debug_sym)
				res->debug_sym = g_strdup_printf ("%s_%d", res->debug_sym, synchronized_symbol_idx);
			synchronized_symbol_idx ++;
		}
		if (res->debug_sym)
			res->llvm_symbol = g_strdup_printf ("%s_%s_llvm", res->symbol, res->debug_sym);
		else
			res->llvm_symbol = g_strdup_printf ("%s_llvm", res->symbol);

		g_hash_table_insert (acfg->patch_to_plt_entry [new_ji->type], new_ji, res);

		g_hash_table_insert (acfg->plt_offset_to_entry, GUINT_TO_POINTER (res->plt_offset), res);

		//g_assert (mono_patch_info_equal (patch_info, new_ji));
		//mono_print_ji (patch_info); printf ("\n");
		//g_hash_table_print_stats (acfg->patch_to_plt_entry);

		acfg->plt_offset ++;
	}

	return res;
}

/**
 * get_got_offset:
 *
 *   Returns the offset of the GOT slot where the runtime object resulting from resolving
 * JI could be found if it exists, otherwise allocates a new one.
 */
static guint32
get_got_offset (MonoAotCompile *acfg, gboolean llvm, MonoJumpInfo *ji)
{
	guint32 got_offset;
	GotInfo *info = llvm ? &acfg->llvm_got_info : &acfg->got_info;

	got_offset = GPOINTER_TO_UINT (g_hash_table_lookup (info->patch_to_got_offset_by_type [ji->type], ji));
	if (got_offset)
		return got_offset - 1;

	if (llvm) {
		got_offset = acfg->llvm_got_offset;
		acfg->llvm_got_offset ++;
	} else {
		got_offset = acfg->got_offset;
		acfg->got_offset ++;
	}

	acfg->stats.got_slots ++;
	acfg->stats.got_slot_types [ji->type] ++;

	g_hash_table_insert (info->patch_to_got_offset, ji, GUINT_TO_POINTER (got_offset + 1));
	g_hash_table_insert (info->patch_to_got_offset_by_type [ji->type], ji, GUINT_TO_POINTER (got_offset + 1));
	g_ptr_array_add (info->got_patches, ji);

	return got_offset;
}

/* Add a method to the list of methods which need to be emitted */
static void
add_method_with_index (MonoAotCompile *acfg, MonoMethod *method, int index, gboolean extra)
{
	g_assert (method);
	if (!g_hash_table_lookup (acfg->method_indexes, method)) {
		g_ptr_array_add (acfg->methods, method);
		g_hash_table_insert (acfg->method_indexes, method, GUINT_TO_POINTER (index + 1));
		acfg->nmethods = acfg->methods->len + 1;
	}

	if (method->wrapper_type || extra)
		g_ptr_array_add (acfg->extra_methods, method);
}

static gboolean
prefer_gsharedvt_method (MonoAotCompile *acfg, MonoMethod *method)
{
	/* One instantiation with valuetypes is generated for each async method */
	if (method->klass->image == mono_defaults.corlib && (!strcmp (method->klass->name, "AsyncMethodBuilderCore") || !strcmp (method->klass->name, "AsyncVoidMethodBuilder")))
		return TRUE;
	else
		return FALSE;
}

static guint32
get_method_index (MonoAotCompile *acfg, MonoMethod *method)
{
	int index = GPOINTER_TO_UINT (g_hash_table_lookup (acfg->method_indexes, method));
	
	g_assert (index);

	return index - 1;
}

static int
add_method_full (MonoAotCompile *acfg, MonoMethod *method, gboolean extra, int depth)
{
	int index;

	index = GPOINTER_TO_UINT (g_hash_table_lookup (acfg->method_indexes, method));
	if (index)
		return index - 1;

	index = acfg->method_index;
	add_method_with_index (acfg, method, index, extra);

	g_ptr_array_add (acfg->method_order, GUINT_TO_POINTER (index));

	g_hash_table_insert (acfg->method_depth, method, GUINT_TO_POINTER (depth));

	acfg->method_index ++;

	return index;
}

static int
add_method (MonoAotCompile *acfg, MonoMethod *method)
{
	return add_method_full (acfg, method, FALSE, 0);
}

static void
add_extra_method_with_depth (MonoAotCompile *acfg, MonoMethod *method, int depth)
{
	if (mono_method_is_generic_sharable_full (method, TRUE, TRUE, FALSE))
		method = mini_get_shared_method (method);
	else if ((acfg->opts & MONO_OPT_GSHAREDVT) && prefer_gsharedvt_method (acfg, method) && mono_method_is_generic_sharable_full (method, FALSE, FALSE, TRUE))
		/* Use the gsharedvt version */
		method = mini_get_shared_method_full (method, TRUE, TRUE);

	if (acfg->aot_opts.log_generics)
		aot_printf (acfg, "%*sAdding method %s.\n", depth, "", mono_method_get_full_name (method));

	add_method_full (acfg, method, TRUE, depth);
}

static void
add_extra_method (MonoAotCompile *acfg, MonoMethod *method)
{
	add_extra_method_with_depth (acfg, method, 0);
}

static void
add_jit_icall_wrapper (gpointer key, gpointer value, gpointer user_data)
{
	MonoAotCompile *acfg = (MonoAotCompile *)user_data;
	MonoJitICallInfo *callinfo = (MonoJitICallInfo *)value;
	MonoMethod *wrapper;
	char *name;

	if (!callinfo->sig)
		return;

	name = g_strdup_printf ("__icall_wrapper_%s", callinfo->name);
	wrapper = mono_marshal_get_icall_wrapper (callinfo->sig, name, callinfo->func, TRUE);
	g_free (name);

	add_method (acfg, wrapper);
}

static MonoMethod*
get_runtime_invoke_sig (MonoMethodSignature *sig)
{
	MonoMethodBuilder *mb;
	MonoMethod *m;

	mb = mono_mb_new (mono_defaults.object_class, "FOO", MONO_WRAPPER_NONE);
	m = mono_mb_create_method (mb, sig, 16);
	MonoMethod *invoke = mono_marshal_get_runtime_invoke (m, FALSE);
	mono_mb_free (mb);
	return invoke;
}

static MonoMethod*
get_runtime_invoke (MonoAotCompile *acfg, MonoMethod *method, gboolean virtual_)
{
	return mono_marshal_get_runtime_invoke (method, virtual_);
}

static gboolean
can_marshal_struct (MonoClass *klass)
{
	MonoClassField *field;
	gboolean can_marshal = TRUE;
	gpointer iter = NULL;
	MonoMarshalType *info;
	int i;

	if (mono_class_is_auto_layout (klass))
		return FALSE;

	info = mono_marshal_load_type_info (klass);

	/* Only allow a few field types to avoid asserts in the marshalling code */
	while ((field = mono_class_get_fields (klass, &iter))) {
		if ((field->type->attrs & FIELD_ATTRIBUTE_STATIC))
			continue;

		switch (field->type->type) {
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
		case MONO_TYPE_CHAR:
		case MONO_TYPE_I8:
		case MONO_TYPE_U8:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_PTR:
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
		case MONO_TYPE_STRING:
			break;
		case MONO_TYPE_VALUETYPE:
			if (!mono_class_from_mono_type (field->type)->enumtype && !can_marshal_struct (mono_class_from_mono_type (field->type)))
				can_marshal = FALSE;
			break;
		case MONO_TYPE_SZARRAY: {
			gboolean has_mspec = FALSE;

			if (info) {
				for (i = 0; i < info->num_fields; ++i) {
					if (info->fields [i].field == field && info->fields [i].mspec)
						has_mspec = TRUE;
				}
			}
			if (!has_mspec)
				can_marshal = FALSE;
			break;
		}
		default:
			can_marshal = FALSE;
			break;
		}
	}

	/* Special cases */
	/* Its hard to compute whenever these can be marshalled or not */
	if (!strcmp (klass->name_space, "System.Net.NetworkInformation.MacOsStructs") && strcmp (klass->name, "sockaddr_dl"))
		return TRUE;

	return can_marshal;
}

static void
create_gsharedvt_inst (MonoAotCompile *acfg, MonoMethod *method, MonoGenericContext *ctx)
{
	/* Create a vtype instantiation */
	MonoGenericContext shared_context;
	MonoType **args;
	MonoGenericInst *inst;
	MonoGenericContainer *container;
	MonoClass **constraints;
	int i;

	memset (ctx, 0, sizeof (MonoGenericContext));

	if (mono_class_is_gtd (method->klass)) {
		shared_context = mono_class_get_generic_container (method->klass)->context;
		inst = shared_context.class_inst;

		args = g_new0 (MonoType*, inst->type_argc);
		for (i = 0; i < inst->type_argc; ++i) {
			args [i] = &mono_defaults.int_class->byval_arg;
		}
		ctx->class_inst = mono_metadata_get_generic_inst (inst->type_argc, args);
	}
	if (method->is_generic) {
		container = mono_method_get_generic_container (method);
		shared_context = container->context;
		inst = shared_context.method_inst;

		args = g_new0 (MonoType*, inst->type_argc);
		for (i = 0; i < container->type_argc; ++i) {
			MonoGenericParamInfo *info = &container->type_params [i].info;
			gboolean ref_only = FALSE;

			if (info && info->constraints) {
				constraints = info->constraints;

				while (*constraints) {
					MonoClass *cklass = *constraints;
					if (!(cklass == mono_defaults.object_class || (cklass->image == mono_defaults.corlib && !strcmp (cklass->name, "ValueType"))))
						/* Inflaring the method with our vtype would not be valid */
						ref_only = TRUE;
					constraints ++;
				}
			}

			if (ref_only)
				args [i] = &mono_defaults.object_class->byval_arg;
			else
				args [i] = &mono_defaults.int_class->byval_arg;
		}
		ctx->method_inst = mono_metadata_get_generic_inst (inst->type_argc, args);
	}
}

static void
add_wrappers (MonoAotCompile *acfg)
{
	MonoMethod *method, *m;
	int i, j;
	MonoMethodSignature *sig, *csig;
	guint32 token;

	/* 
	 * FIXME: Instead of AOTing all the wrappers, it might be better to redesign them
	 * so there is only one wrapper of a given type, or inlining their contents into their
	 * callers.
	 */
	for (i = 0; i < acfg->image->tables [MONO_TABLE_METHOD].rows; ++i) {
		MonoError error;
		MonoMethod *method;
		guint32 token = MONO_TOKEN_METHOD_DEF | (i + 1);
		gboolean skip = FALSE;

		method = mono_get_method_checked (acfg->image, token, NULL, NULL, &error);
		report_loader_error (acfg, &error, "Failed to load method token 0x%x due to %s\n", i, mono_error_get_message (&error));

		if ((method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
			(method->iflags & METHOD_IMPL_ATTRIBUTE_RUNTIME) ||
			(method->flags & METHOD_ATTRIBUTE_ABSTRACT))
			skip = TRUE;

		/* Skip methods which can not be handled by get_runtime_invoke () */
		sig = mono_method_signature (method);
		if (!sig)
			continue;
		if ((sig->ret->type == MONO_TYPE_PTR) ||
			(sig->ret->type == MONO_TYPE_TYPEDBYREF))
			skip = TRUE;
		if (mono_class_is_open_constructed_type (sig->ret))
			skip = TRUE;

		for (j = 0; j < sig->param_count; j++) {
			if (sig->params [j]->type == MONO_TYPE_TYPEDBYREF)
				skip = TRUE;
			if (mono_class_is_open_constructed_type (sig->params [j]))
				skip = TRUE;
		}

#ifdef MONO_ARCH_DYN_CALL_SUPPORTED
		if (!mono_class_is_contextbound (method->klass)) {
			MonoDynCallInfo *info = mono_arch_dyn_call_prepare (sig);
			gboolean has_nullable = FALSE;

			for (j = 0; j < sig->param_count; j++) {
				if (sig->params [j]->type == MONO_TYPE_GENERICINST && mono_class_is_nullable (mono_class_from_mono_type (sig->params [j])))
					has_nullable = TRUE;
			}

			if (info && !has_nullable && !acfg->aot_opts.llvm_only) {
				/* Supported by the dynamic runtime-invoke wrapper */
				skip = TRUE;
			}
			if (info)
				mono_arch_dyn_call_free (info);
		}
#endif

		if (acfg->aot_opts.llvm_only)
			/* Supported by the gsharedvt based runtime-invoke wrapper */
			skip = TRUE;

		if (!skip) {
			//printf ("%s\n", mono_method_full_name (method, TRUE));
			add_method (acfg, get_runtime_invoke (acfg, method, FALSE));
		}
 	}

	if (strcmp (acfg->image->assembly->aname.name, "mscorlib") == 0) {
		int nallocators;

		/* Runtime invoke wrappers */

		/* void runtime-invoke () [.cctor] */
		csig = mono_metadata_signature_alloc (mono_defaults.corlib, 0);
		csig->ret = &mono_defaults.void_class->byval_arg;
		add_method (acfg, get_runtime_invoke_sig (csig));

		/* void runtime-invoke () [Finalize] */
		csig = mono_metadata_signature_alloc (mono_defaults.corlib, 0);
		csig->hasthis = 1;
		csig->ret = &mono_defaults.void_class->byval_arg;
		add_method (acfg, get_runtime_invoke_sig (csig));

		/* void runtime-invoke (string) [exception ctor] */
		csig = mono_metadata_signature_alloc (mono_defaults.corlib, 1);
		csig->hasthis = 1;
		csig->ret = &mono_defaults.void_class->byval_arg;
		csig->params [0] = &mono_defaults.string_class->byval_arg;
		add_method (acfg, get_runtime_invoke_sig (csig));

		/* void runtime-invoke (string, string) [exception ctor] */
		csig = mono_metadata_signature_alloc (mono_defaults.corlib, 2);
		csig->hasthis = 1;
		csig->ret = &mono_defaults.void_class->byval_arg;
		csig->params [0] = &mono_defaults.string_class->byval_arg;
		csig->params [1] = &mono_defaults.string_class->byval_arg;
		add_method (acfg, get_runtime_invoke_sig (csig));

		/* string runtime-invoke () [Exception.ToString ()] */
		csig = mono_metadata_signature_alloc (mono_defaults.corlib, 0);
		csig->hasthis = 1;
		csig->ret = &mono_defaults.string_class->byval_arg;
		add_method (acfg, get_runtime_invoke_sig (csig));

		/* void runtime-invoke (string, Exception) [exception ctor] */
		csig = mono_metadata_signature_alloc (mono_defaults.corlib, 2);
		csig->hasthis = 1;
		csig->ret = &mono_defaults.void_class->byval_arg;
		csig->params [0] = &mono_defaults.string_class->byval_arg;
		csig->params [1] = &mono_defaults.exception_class->byval_arg;
		add_method (acfg, get_runtime_invoke_sig (csig));

		/* Assembly runtime-invoke (string, Assembly, bool) [DoAssemblyResolve] */
		csig = mono_metadata_signature_alloc (mono_defaults.corlib, 3);
		csig->hasthis = 1;
		csig->ret = &(mono_class_load_from_name (
											mono_defaults.corlib, "System.Reflection", "Assembly"))->byval_arg;
		csig->params [0] = &mono_defaults.string_class->byval_arg;
		csig->params [1] = &(mono_class_load_from_name (mono_defaults.corlib, "System.Reflection", "Assembly"))->byval_arg;
		csig->params [2] = &mono_defaults.boolean_class->byval_arg;
		add_method (acfg, get_runtime_invoke_sig (csig));

		/* runtime-invoke used by finalizers */
		add_method (acfg, get_runtime_invoke (acfg, mono_class_get_method_from_name_flags (mono_defaults.object_class, "Finalize", 0, 0), TRUE));

		/* This is used by mono_runtime_capture_context () */
		method = mono_get_context_capture_method ();
		if (method)
			add_method (acfg, get_runtime_invoke (acfg, method, FALSE));

#ifdef MONO_ARCH_DYN_CALL_SUPPORTED
		if (!acfg->aot_opts.llvm_only)
			add_method (acfg, mono_marshal_get_runtime_invoke_dynamic ());
#endif

		/* These are used by mono_jit_runtime_invoke () to calls gsharedvt out wrappers */
		if (acfg->aot_opts.llvm_only) {
			int variants;

			/* Create simplified signatures which match the signature used by the gsharedvt out wrappers */
			for (variants = 0; variants < 4; ++variants) {
				for (i = 0; i < 16; ++i) {
					sig = mini_get_gsharedvt_out_sig_wrapper_signature ((variants & 1) > 0, (variants & 2) > 0, i);
					add_extra_method (acfg, mono_marshal_get_runtime_invoke_for_sig (sig));

					g_free (sig);
				}
			}
		}

		/* stelemref */
		add_method (acfg, mono_marshal_get_stelemref ());

		/* Managed Allocators */
		nallocators = mono_gc_get_managed_allocator_types ();
		for (i = 0; i < nallocators; ++i) {
			if ((m = mono_gc_get_managed_allocator_by_type (i, MANAGED_ALLOCATOR_REGULAR)))
				add_method (acfg, m);
			if ((m = mono_gc_get_managed_allocator_by_type (i, MANAGED_ALLOCATOR_SLOW_PATH)))
				add_method (acfg, m);
		}

		/* write barriers */
		if (mono_gc_is_moving ()) {
			add_method (acfg, mono_gc_get_specific_write_barrier (FALSE));
			add_method (acfg, mono_gc_get_specific_write_barrier (TRUE));
		}

		/* Stelemref wrappers */
		{
			MonoMethod **wrappers;
			int nwrappers;

			wrappers = mono_marshal_get_virtual_stelemref_wrappers (&nwrappers);
			for (i = 0; i < nwrappers; ++i)
				add_method (acfg, wrappers [i]);
			g_free (wrappers);
		}

		/* castclass_with_check wrapper */
		add_method (acfg, mono_marshal_get_castclass_with_cache ());
		/* isinst_with_check wrapper */
		add_method (acfg, mono_marshal_get_isinst_with_cache ());

		/* JIT icall wrappers */
		/* FIXME: locking - this is "safe" as full-AOT threads don't mutate the icall hash*/
		g_hash_table_foreach (mono_get_jit_icall_info (), add_jit_icall_wrapper, acfg);
	}

	/* 
	 * remoting-invoke-with-check wrappers are very frequent, so avoid emitting them,
	 * we use the original method instead at runtime.
	 * Since full-aot doesn't support remoting, this is not a problem.
	 */
#if 0
	/* remoting-invoke wrappers */
	for (i = 0; i < acfg->image->tables [MONO_TABLE_METHOD].rows; ++i) {
		MonoError error;
		MonoMethodSignature *sig;
		
		token = MONO_TOKEN_METHOD_DEF | (i + 1);
		method = mono_get_method_checked (acfg->image, token, NULL, NULL, &error);
		g_assert (mono_error_ok (&error)); /* FIXME don't swallow the error */

		sig = mono_method_signature (method);

		if (sig->hasthis && (method->klass->marshalbyref || method->klass == mono_defaults.object_class)) {
			m = mono_marshal_get_remoting_invoke_with_check (method);

			add_method (acfg, m);
		}
	}
#endif

	/* delegate-invoke wrappers */
	for (i = 0; i < acfg->image->tables [MONO_TABLE_TYPEDEF].rows; ++i) {
		MonoError error;
		MonoClass *klass;
		MonoCustomAttrInfo *cattr;
		
		token = MONO_TOKEN_TYPE_DEF | (i + 1);
		klass = mono_class_get_checked (acfg->image, token, &error);

		if (!klass) {
			mono_error_cleanup (&error);
			continue;
		}

		if (!klass->delegate || klass == mono_defaults.delegate_class || klass == mono_defaults.multicastdelegate_class)
			continue;

		if (!mono_class_is_gtd (klass)) {
			method = mono_get_delegate_invoke (klass);

			m = mono_marshal_get_delegate_invoke (method, NULL);

			add_method (acfg, m);

			method = mono_class_get_method_from_name_flags (klass, "BeginInvoke", -1, 0);
			if (method)
				add_method (acfg, mono_marshal_get_delegate_begin_invoke (method));

			method = mono_class_get_method_from_name_flags (klass, "EndInvoke", -1, 0);
			if (method)
				add_method (acfg, mono_marshal_get_delegate_end_invoke (method));

			cattr = mono_custom_attrs_from_class_checked (klass, &error);
			if (!is_ok (&error)) {
				mono_error_cleanup (&error);
				continue;
			}

			if (cattr) {
				int j;

				for (j = 0; j < cattr->num_attrs; ++j)
					if (cattr->attrs [j].ctor && (!strcmp (cattr->attrs [j].ctor->klass->name, "MonoNativeFunctionWrapperAttribute") || !strcmp (cattr->attrs [j].ctor->klass->name, "UnmanagedFunctionPointerAttribute")))
						break;
				if (j < cattr->num_attrs) {
					MonoMethod *invoke;
					MonoMethod *wrapper;
					MonoMethod *del_invoke;

					/* Add wrappers needed by mono_ftnptr_to_delegate () */
					invoke = mono_get_delegate_invoke (klass);
					wrapper = mono_marshal_get_native_func_wrapper_aot (klass);
					del_invoke = mono_marshal_get_delegate_invoke_internal (invoke, FALSE, TRUE, wrapper);
					add_method (acfg, wrapper);
					add_method (acfg, del_invoke);
				}
			}
		} else if ((acfg->opts & MONO_OPT_GSHAREDVT) && mono_class_is_gtd (klass)) {
			MonoError error;
			MonoGenericContext ctx;
			MonoMethod *inst, *gshared;

			/*
			 * Emit gsharedvt versions of the generic delegate-invoke wrappers
			 */
			/* Invoke */
			method = mono_get_delegate_invoke (klass);
			create_gsharedvt_inst (acfg, method, &ctx);

			inst = mono_class_inflate_generic_method_checked (method, &ctx, &error);
			g_assert (mono_error_ok (&error)); /* FIXME don't swallow the error */

			m = mono_marshal_get_delegate_invoke (inst, NULL);
			g_assert (m->is_inflated);

			gshared = mini_get_shared_method_full (m, FALSE, TRUE);
			add_extra_method (acfg, gshared);

			/* begin-invoke */
			method = mono_get_delegate_begin_invoke (klass);
			if (method) {
				create_gsharedvt_inst (acfg, method, &ctx);

				inst = mono_class_inflate_generic_method_checked (method, &ctx, &error);
				g_assert (mono_error_ok (&error)); /* FIXME don't swallow the error */

				m = mono_marshal_get_delegate_begin_invoke (inst);
				g_assert (m->is_inflated);

				gshared = mini_get_shared_method_full (m, FALSE, TRUE);
				add_extra_method (acfg, gshared);
			}

			/* end-invoke */
			method = mono_get_delegate_end_invoke (klass);
			if (method) {
				create_gsharedvt_inst (acfg, method, &ctx);

				inst = mono_class_inflate_generic_method_checked (method, &ctx, &error);
				g_assert (mono_error_ok (&error)); /* FIXME don't swallow the error */

				m = mono_marshal_get_delegate_end_invoke (inst);
				g_assert (m->is_inflated);

				gshared = mini_get_shared_method_full (m, FALSE, TRUE);
				add_extra_method (acfg, gshared);
			}
		}
	}

	/* array access wrappers */
	for (i = 0; i < acfg->image->tables [MONO_TABLE_TYPESPEC].rows; ++i) {
		MonoError error;
		MonoClass *klass;
		
		token = MONO_TOKEN_TYPE_SPEC | (i + 1);
		klass = mono_class_get_checked (acfg->image, token, &error);

		if (!klass) {
			mono_error_cleanup (&error);
			continue;
		}

		if (klass->rank && MONO_TYPE_IS_PRIMITIVE (&klass->element_class->byval_arg)) {
			MonoMethod *m, *wrapper;

			/* Add runtime-invoke wrappers too */

			m = mono_class_get_method_from_name (klass, "Get", -1);
			g_assert (m);
			wrapper = mono_marshal_get_array_accessor_wrapper (m);
			add_extra_method (acfg, wrapper);
			if (!acfg->aot_opts.llvm_only)
				add_extra_method (acfg, get_runtime_invoke (acfg, wrapper, FALSE));

			m = mono_class_get_method_from_name (klass, "Set", -1);
			g_assert (m);
			wrapper = mono_marshal_get_array_accessor_wrapper (m);
			add_extra_method (acfg, wrapper);
			if (!acfg->aot_opts.llvm_only)
				add_extra_method (acfg, get_runtime_invoke (acfg, wrapper, FALSE));
		}
	}

	/* Synchronized wrappers */
	for (i = 0; i < acfg->image->tables [MONO_TABLE_METHOD].rows; ++i) {
		MonoError error;
		token = MONO_TOKEN_METHOD_DEF | (i + 1);
		method = mono_get_method_checked (acfg->image, token, NULL, NULL, &error);
		report_loader_error (acfg, &error, "Failed to load method token 0x%x due to %s\n", i, mono_error_get_message (&error));

		if (method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED) {
			if (method->is_generic) {
				// FIXME:
			} else if ((acfg->opts & MONO_OPT_GSHAREDVT) && mono_class_is_gtd (method->klass)) {
				MonoError error;
				MonoGenericContext ctx;
				MonoMethod *inst, *gshared, *m;

				/*
				 * Create a generic wrapper for a generic instance, and AOT that.
				 */
				create_gsharedvt_inst (acfg, method, &ctx);
				inst = mono_class_inflate_generic_method_checked (method, &ctx, &error);
				g_assert (mono_error_ok (&error)); /* FIXME don't swallow the error */
				m = mono_marshal_get_synchronized_wrapper (inst);
				g_assert (m->is_inflated);
				gshared = mini_get_shared_method_full (m, FALSE, TRUE);
				add_method (acfg, gshared);
			} else {
				add_method (acfg, mono_marshal_get_synchronized_wrapper (method));
			}
		}
	}

	/* pinvoke wrappers */
	for (i = 0; i < acfg->image->tables [MONO_TABLE_METHOD].rows; ++i) {
		MonoError error;
		MonoMethod *method;
		guint32 token = MONO_TOKEN_METHOD_DEF | (i + 1);

		method = mono_get_method_checked (acfg->image, token, NULL, NULL, &error);
		report_loader_error (acfg, &error, "Failed to load method token 0x%x due to %s\n", i, mono_error_get_message (&error));

		if ((method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
			(method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)) {
			add_method (acfg, mono_marshal_get_native_wrapper (method, TRUE, TRUE));
		}

		if (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) {
			if (acfg->aot_opts.llvm_only) {
				/* The wrappers have a different signature (hasthis is not set) so need to add this too */
				add_gsharedvt_wrappers (acfg, mono_method_signature (method), FALSE, TRUE);
			}
		}
	}
 
	/* native-to-managed wrappers */
	for (i = 0; i < acfg->image->tables [MONO_TABLE_METHOD].rows; ++i) {
		MonoError error;
		MonoMethod *method;
		guint32 token = MONO_TOKEN_METHOD_DEF | (i + 1);
		MonoCustomAttrInfo *cattr;
		int j;

		method = mono_get_method_checked (acfg->image, token, NULL, NULL, &error);
		report_loader_error (acfg, &error, "Failed to load method token 0x%x due to %s\n", i, mono_error_get_message (&error));

		/* 
		 * Only generate native-to-managed wrappers for methods which have an
		 * attribute named MonoPInvokeCallbackAttribute. We search for the attribute by
		 * name to avoid defining a new assembly to contain it.
		 */
		cattr = mono_custom_attrs_from_method_checked (method, &error);
		if (!is_ok (&error)) {
			char *name = mono_method_get_full_name (method);
			report_loader_error (acfg, &error, "Failed to load custom attributes from method %s due to %s\n", name, mono_error_get_message (&error));
			g_free (name);
		}

		if (cattr) {
			for (j = 0; j < cattr->num_attrs; ++j)
				if (cattr->attrs [j].ctor && !strcmp (cattr->attrs [j].ctor->klass->name, "MonoPInvokeCallbackAttribute"))
					break;
			if (j < cattr->num_attrs) {
				MonoCustomAttrEntry *e = &cattr->attrs [j];
				MonoMethodSignature *sig = mono_method_signature (e->ctor);
				const char *p = (const char*)e->data;
				const char *named;
				int slen, num_named, named_type;
				char *n;
				MonoType *t;
				MonoClass *klass;
				char *export_name = NULL;
				MonoMethod *wrapper;

				/* this cannot be enforced by the C# compiler so we must give the user some warning before aborting */
				if (!(method->flags & METHOD_ATTRIBUTE_STATIC)) {
					g_warning ("AOT restriction: Method '%s' must be static since it is decorated with [MonoPInvokeCallback]. See http://ios.xamarin.com/Documentation/Limitations#Reverse_Callbacks", 
						mono_method_full_name (method, TRUE));
					exit (1);
				}

				g_assert (sig->param_count == 1);
				g_assert (sig->params [0]->type == MONO_TYPE_CLASS && !strcmp (mono_class_from_mono_type (sig->params [0])->name, "Type"));

				/* 
				 * Decode the cattr manually since we can't create objects
				 * during aot compilation.
				 */
					
				/* Skip prolog */
				p += 2;

				/* From load_cattr_value () in reflection.c */
				slen = mono_metadata_decode_value (p, &p);
				n = (char *)g_memdup (p, slen + 1);
				n [slen] = 0;
				t = mono_reflection_type_from_name_checked (n, acfg->image, &error);
				g_assert (t);
				mono_error_assert_ok (&error);
				g_free (n);

				klass = mono_class_from_mono_type (t);
				g_assert (klass->parent == mono_defaults.multicastdelegate_class);

				p += slen;

				num_named = read16 (p);
				p += 2;

				g_assert (num_named < 2);
				if (num_named == 1) {
					int name_len;
					char *name;

					/* parse ExportSymbol attribute */
					named = p;
					named_type = *named;
					named += 1;
					/* data_type = *named; */
					named += 1;

					name_len = mono_metadata_decode_blob_size (named, &named);
					name = (char *)g_malloc (name_len + 1);
					memcpy (name, named, name_len);
					name [name_len] = 0;
					named += name_len;

					g_assert (named_type == 0x54);
					g_assert (!strcmp (name, "ExportSymbol"));

					/* load_cattr_value (), string case */
					g_assert (*named != (char)0xff);
					slen = mono_metadata_decode_value (named, &named);
					export_name = (char *)g_malloc (slen + 1);
					memcpy (export_name, named, slen);
					export_name [slen] = 0;
					named += slen;
				}

				wrapper = mono_marshal_get_managed_wrapper (method, klass, 0);
				add_method (acfg, wrapper);
				if (export_name)
					g_hash_table_insert (acfg->export_names, wrapper, export_name);
			}
			g_free (cattr);
		}

		if ((method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
			(method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)) {
			add_method (acfg, mono_marshal_get_native_wrapper (method, TRUE, TRUE));
		}
	}

	/* StructureToPtr/PtrToStructure wrappers */
	for (i = 0; i < acfg->image->tables [MONO_TABLE_TYPEDEF].rows; ++i) {
		MonoError error;
		MonoClass *klass;
		
		token = MONO_TOKEN_TYPE_DEF | (i + 1);
		klass = mono_class_get_checked (acfg->image, token, &error);

		if (!klass) {
			mono_error_cleanup (&error);
			continue;
		}

		if (klass->valuetype && !mono_class_is_gtd (klass) && can_marshal_struct (klass) &&
			!(klass->nested_in && strstr (klass->nested_in->name, "<PrivateImplementationDetails>") == klass->nested_in->name)) {
			add_method (acfg, mono_marshal_get_struct_to_ptr (klass));
			add_method (acfg, mono_marshal_get_ptr_to_struct (klass));
		}
	}
}

static gboolean
has_type_vars (MonoClass *klass)
{
	if ((klass->byval_arg.type == MONO_TYPE_VAR) || (klass->byval_arg.type == MONO_TYPE_MVAR))
		return TRUE;
	if (klass->rank)
		return has_type_vars (klass->element_class);
	if (mono_class_is_ginst (klass)) {
		MonoGenericContext *context = &mono_class_get_generic_class (klass)->context;
		if (context->class_inst) {
			int i;

			for (i = 0; i < context->class_inst->type_argc; ++i)
				if (has_type_vars (mono_class_from_mono_type (context->class_inst->type_argv [i])))
					return TRUE;
		}
	}
	if (mono_class_is_gtd (klass))
		return TRUE;
	return FALSE;
}

static gboolean
is_vt_inst (MonoGenericInst *inst)
{
	int i;

	for (i = 0; i < inst->type_argc; ++i) {
		MonoType *t = inst->type_argv [i];
		if (MONO_TYPE_ISSTRUCT (t) || t->type == MONO_TYPE_VALUETYPE)
			return TRUE;
	}
	return FALSE;
}

static gboolean
method_has_type_vars (MonoMethod *method)
{
	if (has_type_vars (method->klass))
		return TRUE;

	if (method->is_inflated) {
		MonoGenericContext *context = mono_method_get_context (method);
		if (context->method_inst) {
			int i;

			for (i = 0; i < context->method_inst->type_argc; ++i)
				if (has_type_vars (mono_class_from_mono_type (context->method_inst->type_argv [i])))
					return TRUE;
		}
	}
	return FALSE;
}

static
gboolean mono_aot_mode_is_full (MonoAotOptions *opts)
{
	return opts->mode == MONO_AOT_MODE_FULL;
}

static
gboolean mono_aot_mode_is_hybrid (MonoAotOptions *opts)
{
	return opts->mode == MONO_AOT_MODE_HYBRID;
}

static void add_generic_class_with_depth (MonoAotCompile *acfg, MonoClass *klass, int depth, const char *ref);

static void
add_generic_class (MonoAotCompile *acfg, MonoClass *klass, gboolean force, const char *ref)
{
	/* This might lead to a huge code blowup so only do it if neccesary */
	if (!mono_aot_mode_is_full (&acfg->aot_opts) && !mono_aot_mode_is_hybrid (&acfg->aot_opts) && !force)
		return;

	add_generic_class_with_depth (acfg, klass, 0, ref);
}

static gboolean
check_type_depth (MonoType *t, int depth)
{
	int i;

	if (depth > 8)
		return TRUE;

	switch (t->type) {
	case MONO_TYPE_GENERICINST: {
		MonoGenericClass *gklass = t->data.generic_class;
		MonoGenericInst *ginst = gklass->context.class_inst;

		if (ginst) {
			for (i = 0; i < ginst->type_argc; ++i) {
				if (check_type_depth (ginst->type_argv [i], depth + 1))
					return TRUE;
			}
		}
		break;
	}
	default:
		break;
	}

	return FALSE;
}

static void
add_types_from_method_header (MonoAotCompile *acfg, MonoMethod *method);

/*
 * add_generic_class:
 *
 *   Add all methods of a generic class.
 */
static void
add_generic_class_with_depth (MonoAotCompile *acfg, MonoClass *klass, int depth, const char *ref)
{
	MonoMethod *method;
	MonoClassField *field;
	gpointer iter;
	gboolean use_gsharedvt = FALSE;

	if (!acfg->ginst_hash)
		acfg->ginst_hash = g_hash_table_new (NULL, NULL);

	mono_class_init (klass);

	if (mono_class_is_ginst (klass) && mono_class_get_generic_class (klass)->context.class_inst->is_open)
		return;

	if (has_type_vars (klass))
		return;

	if (!mono_class_is_ginst (klass) && !klass->rank)
		return;

	if (mono_class_has_failure (klass))
		return;

	if (!acfg->ginst_hash)
		acfg->ginst_hash = g_hash_table_new (NULL, NULL);

	if (g_hash_table_lookup (acfg->ginst_hash, klass))
		return;

	if (check_type_depth (&klass->byval_arg, 0))
		return;

	if (acfg->aot_opts.log_generics)
		aot_printf (acfg, "%*sAdding generic instance %s [%s].\n", depth, "", mono_type_full_name (&klass->byval_arg), ref);

	g_hash_table_insert (acfg->ginst_hash, klass, klass);

	/*
	 * Use gsharedvt for generic collections with vtype arguments to avoid code blowup.
	 * Enable this only for some classes since gsharedvt might not support all methods.
	 */
	if ((acfg->opts & MONO_OPT_GSHAREDVT) && klass->image == mono_defaults.corlib && mono_class_is_ginst (klass) && mono_class_get_generic_class (klass)->context.class_inst && is_vt_inst (mono_class_get_generic_class (klass)->context.class_inst) &&
		(!strcmp (klass->name, "Dictionary`2") || !strcmp (klass->name, "List`1") || !strcmp (klass->name, "ReadOnlyCollection`1")))
		use_gsharedvt = TRUE;

	iter = NULL;
	while ((method = mono_class_get_methods (klass, &iter))) {
		if ((acfg->opts & MONO_OPT_GSHAREDVT) && method->is_inflated && mono_method_get_context (method)->method_inst) {
			/*
			 * This is partial sharing, and we can't handle it yet
			 */
			continue;
		}
		
		if (mono_method_is_generic_sharable_full (method, FALSE, FALSE, use_gsharedvt)) {
			/* Already added */
			add_types_from_method_header (acfg, method);
			continue;
		}

		if (method->is_generic)
			/* FIXME: */
			continue;

		/*
		 * FIXME: Instances which are referenced by these methods are not added,
		 * for example Array.Resize<int> for List<int>.Add ().
		 */
		add_extra_method_with_depth (acfg, method, depth + 1);
	}

	iter = NULL;
	while ((field = mono_class_get_fields (klass, &iter))) {
		if (field->type->type == MONO_TYPE_GENERICINST)
			add_generic_class_with_depth (acfg, mono_class_from_mono_type (field->type), depth + 1, "field");
	}

	if (klass->delegate) {
		method = mono_get_delegate_invoke (klass);

		method = mono_marshal_get_delegate_invoke (method, NULL);

		if (acfg->aot_opts.log_generics)
			aot_printf (acfg, "%*sAdding method %s.\n", depth, "", mono_method_get_full_name (method));

		add_method (acfg, method);
	}

	/* Add superclasses */
	if (klass->parent)
		add_generic_class_with_depth (acfg, klass->parent, depth, "parent");

	/* 
	 * For ICollection<T>, add instances of the helper methods
	 * in Array, since a T[] could be cast to ICollection<T>.
	 */
	if (klass->image == mono_defaults.corlib && !strcmp (klass->name_space, "System.Collections.Generic") &&
		(!strcmp(klass->name, "ICollection`1") || !strcmp (klass->name, "IEnumerable`1") || !strcmp (klass->name, "IList`1") || !strcmp (klass->name, "IEnumerator`1") || !strcmp (klass->name, "IReadOnlyList`1"))) {
		MonoClass *tclass = mono_class_from_mono_type (mono_class_get_generic_class (klass)->context.class_inst->type_argv [0]);
		MonoClass *array_class = mono_bounded_array_class_get (tclass, 1, FALSE);
		gpointer iter;
		char *name_prefix;

		if (!strcmp (klass->name, "IEnumerator`1"))
			name_prefix = g_strdup_printf ("%s.%s", klass->name_space, "IEnumerable`1");
		else
			name_prefix = g_strdup_printf ("%s.%s", klass->name_space, klass->name);

		/* Add the T[]/InternalEnumerator class */
		if (!strcmp (klass->name, "IEnumerable`1") || !strcmp (klass->name, "IEnumerator`1")) {
			MonoError error;
			MonoClass *nclass;

			iter = NULL;
			while ((nclass = mono_class_get_nested_types (array_class->parent, &iter))) {
				if (!strcmp (nclass->name, "InternalEnumerator`1"))
					break;
			}
			g_assert (nclass);
			nclass = mono_class_inflate_generic_class_checked (nclass, mono_generic_class_get_context (mono_class_get_generic_class (klass)), &error);
			mono_error_assert_ok (&error); /* FIXME don't swallow the error */
			add_generic_class (acfg, nclass, FALSE, "ICollection<T>");
		}

		iter = NULL;
		while ((method = mono_class_get_methods (array_class, &iter))) {
			if (strstr (method->name, name_prefix)) {
				MonoMethod *m = mono_aot_get_array_helper_from_wrapper (method);

				add_extra_method_with_depth (acfg, m, depth);
			}
		}

		g_free (name_prefix);
	}

	/* Add an instance of GenericComparer<T> which is created dynamically by Comparer<T> */
	if (klass->image == mono_defaults.corlib && !strcmp (klass->name_space, "System.Collections.Generic") && !strcmp (klass->name, "Comparer`1")) {
		MonoError error;
		MonoClass *tclass = mono_class_from_mono_type (mono_class_get_generic_class (klass)->context.class_inst->type_argv [0]);
		MonoClass *icomparable, *gcomparer, *icomparable_inst;
		MonoGenericContext ctx;
		MonoType *args [16];

		memset (&ctx, 0, sizeof (ctx));

		icomparable = mono_class_load_from_name (mono_defaults.corlib, "System", "IComparable`1");

		args [0] = &tclass->byval_arg;
		ctx.class_inst = mono_metadata_get_generic_inst (1, args);

		icomparable_inst = mono_class_inflate_generic_class_checked (icomparable, &ctx, &error);
		mono_error_assert_ok (&error); /* FIXME don't swallow the error */

		if (mono_class_is_assignable_from (icomparable_inst, tclass)) {
			MonoClass *gcomparer_inst;
			gcomparer = mono_class_load_from_name (mono_defaults.corlib, "System.Collections.Generic", "GenericComparer`1");
			gcomparer_inst = mono_class_inflate_generic_class_checked (gcomparer, &ctx, &error);
			mono_error_assert_ok (&error); /* FIXME don't swallow the error */

			add_generic_class (acfg, gcomparer_inst, FALSE, "Comparer<T>");
		}
	}

	/* Add an instance of GenericEqualityComparer<T> which is created dynamically by EqualityComparer<T> */
	if (klass->image == mono_defaults.corlib && !strcmp (klass->name_space, "System.Collections.Generic") && !strcmp (klass->name, "EqualityComparer`1")) {
		MonoError error;
		MonoClass *tclass = mono_class_from_mono_type (mono_class_get_generic_class (klass)->context.class_inst->type_argv [0]);
		MonoClass *iface, *gcomparer, *iface_inst;
		MonoGenericContext ctx;
		MonoType *args [16];

		memset (&ctx, 0, sizeof (ctx));

		iface = mono_class_load_from_name (mono_defaults.corlib, "System", "IEquatable`1");
		g_assert (iface);
		args [0] = &tclass->byval_arg;
		ctx.class_inst = mono_metadata_get_generic_inst (1, args);

		iface_inst = mono_class_inflate_generic_class_checked (iface, &ctx, &error);
		mono_error_assert_ok (&error); /* FIXME don't swallow the error */

		if (mono_class_is_assignable_from (iface_inst, tclass)) {
			MonoClass *gcomparer_inst;
			MonoError error;

			gcomparer = mono_class_load_from_name (mono_defaults.corlib, "System.Collections.Generic", "GenericEqualityComparer`1");
			gcomparer_inst = mono_class_inflate_generic_class_checked (gcomparer, &ctx, &error);
			mono_error_assert_ok (&error); /* FIXME don't swallow the error */
			add_generic_class (acfg, gcomparer_inst, FALSE, "EqualityComparer<T>");
		}
	}

	/* Add an instance of EnumComparer<T> which is created dynamically by EqualityComparer<T> for enums */
	if (klass->image == mono_defaults.corlib && !strcmp (klass->name_space, "System.Collections.Generic") && !strcmp (klass->name, "EqualityComparer`1")) {
		MonoClass *enum_comparer;
		MonoClass *tclass = mono_class_from_mono_type (mono_class_get_generic_class (klass)->context.class_inst->type_argv [0]);
		MonoGenericContext ctx;
		MonoType *args [16];

		if (mono_class_is_enum (tclass)) {
			MonoClass *enum_comparer_inst;
			MonoError error;

			memset (&ctx, 0, sizeof (ctx));
			args [0] = &tclass->byval_arg;
			ctx.class_inst = mono_metadata_get_generic_inst (1, args);

			enum_comparer = mono_class_load_from_name (mono_defaults.corlib, "System.Collections.Generic", "EnumEqualityComparer`1");
			enum_comparer_inst = mono_class_inflate_generic_class_checked (enum_comparer, &ctx, &error);
			mono_error_assert_ok (&error); /* FIXME don't swallow the error */
			add_generic_class (acfg, enum_comparer_inst, FALSE, "EqualityComparer<T>");
		}
	}

	/* Add an instance of ObjectComparer<T> which is created dynamically by Comparer<T> for enums */
	if (klass->image == mono_defaults.corlib && !strcmp (klass->name_space, "System.Collections.Generic") && !strcmp (klass->name, "Comparer`1")) {
		MonoClass *comparer;
		MonoClass *tclass = mono_class_from_mono_type (mono_class_get_generic_class (klass)->context.class_inst->type_argv [0]);
		MonoGenericContext ctx;
		MonoType *args [16];

		if (mono_class_is_enum (tclass)) {
			MonoClass *comparer_inst;
			MonoError error;

			memset (&ctx, 0, sizeof (ctx));
			args [0] = &tclass->byval_arg;
			ctx.class_inst = mono_metadata_get_generic_inst (1, args);

			comparer = mono_class_load_from_name (mono_defaults.corlib, "System.Collections.Generic", "ObjectComparer`1");
			comparer_inst = mono_class_inflate_generic_class_checked (comparer, &ctx, &error);
			mono_error_assert_ok (&error); /* FIXME don't swallow the error */
			add_generic_class (acfg, comparer_inst, FALSE, "Comparer<T>");
		}
	}
}

static void
add_instances_of (MonoAotCompile *acfg, MonoClass *klass, MonoType **insts, int ninsts, gboolean force)
{
	int i;
	MonoGenericContext ctx;
	MonoType *args [16];

	if (acfg->aot_opts.no_instances)
		return;

	memset (&ctx, 0, sizeof (ctx));

	for (i = 0; i < ninsts; ++i) {
		MonoError error;
		MonoClass *generic_inst;
		args [0] = insts [i];
		ctx.class_inst = mono_metadata_get_generic_inst (1, args);
		generic_inst = mono_class_inflate_generic_class_checked (klass, &ctx, &error);
		mono_error_assert_ok (&error); /* FIXME don't swallow the error */
		add_generic_class (acfg, generic_inst, force, "");
	}
}

static void
add_types_from_method_header (MonoAotCompile *acfg, MonoMethod *method)
{
	MonoError error;
	MonoMethodHeader *header;
	MonoMethodSignature *sig;
	int j, depth;

	depth = GPOINTER_TO_UINT (g_hash_table_lookup (acfg->method_depth, method));

	sig = mono_method_signature (method);

	if (sig) {
		for (j = 0; j < sig->param_count; ++j)
			if (sig->params [j]->type == MONO_TYPE_GENERICINST)
				add_generic_class_with_depth (acfg, mono_class_from_mono_type (sig->params [j]), depth + 1, "arg");
	}

	header = mono_method_get_header_checked (method, &error);

	if (header) {
		for (j = 0; j < header->num_locals; ++j)
			if (header->locals [j]->type == MONO_TYPE_GENERICINST)
				add_generic_class_with_depth (acfg, mono_class_from_mono_type (header->locals [j]), depth + 1, "local");
		mono_metadata_free_mh (header);
	} else {
		mono_error_cleanup (&error); /* FIXME report the error */
	}

}

/*
 * add_generic_instances:
 *
 *   Add instances referenced by the METHODSPEC/TYPESPEC table.
 */
static void
add_generic_instances (MonoAotCompile *acfg)
{
	int i;
	guint32 token;
	MonoMethod *method;
	MonoGenericContext *context;

	if (acfg->aot_opts.no_instances)
		return;

	for (i = 0; i < acfg->image->tables [MONO_TABLE_METHODSPEC].rows; ++i) {
		MonoError error;
		token = MONO_TOKEN_METHOD_SPEC | (i + 1);
		method = mono_get_method_checked (acfg->image, token, NULL, NULL, &error);

		if (!method) {
			aot_printerrf (acfg, "Failed to load methodspec 0x%x due to %s.\n", token, mono_error_get_message (&error));
			aot_printerrf (acfg, "Run with MONO_LOG_LEVEL=debug for more information.\n");
			mono_error_cleanup (&error);
			continue;
		}

		if (method->klass->image != acfg->image)
			continue;

		context = mono_method_get_context (method);

		if (context && ((context->class_inst && context->class_inst->is_open)))
			continue;

		/*
		 * For open methods, create an instantiation which can be passed to the JIT.
		 * FIXME: Handle class_inst as well.
		 */
		if (context && context->method_inst && context->method_inst->is_open) {
			MonoError error;
			MonoGenericContext shared_context;
			MonoGenericInst *inst;
			MonoType **type_argv;
			int i;
			MonoMethod *declaring_method;
			gboolean supported = TRUE;

			/* Check that the context doesn't contain open constructed types */
			if (context->class_inst) {
				inst = context->class_inst;
				for (i = 0; i < inst->type_argc; ++i) {
					if (MONO_TYPE_IS_REFERENCE (inst->type_argv [i]) || inst->type_argv [i]->type == MONO_TYPE_VAR || inst->type_argv [i]->type == MONO_TYPE_MVAR)
						continue;
					if (mono_class_is_open_constructed_type (inst->type_argv [i]))
						supported = FALSE;
				}
			}
			if (context->method_inst) {
				inst = context->method_inst;
				for (i = 0; i < inst->type_argc; ++i) {
					if (MONO_TYPE_IS_REFERENCE (inst->type_argv [i]) || inst->type_argv [i]->type == MONO_TYPE_VAR || inst->type_argv [i]->type == MONO_TYPE_MVAR)
						continue;
					if (mono_class_is_open_constructed_type (inst->type_argv [i]))
						supported = FALSE;
				}
			}

			if (!supported)
				continue;

			memset (&shared_context, 0, sizeof (MonoGenericContext));

			inst = context->class_inst;
			if (inst) {
				type_argv = g_new0 (MonoType*, inst->type_argc);
				for (i = 0; i < inst->type_argc; ++i) {
					if (MONO_TYPE_IS_REFERENCE (inst->type_argv [i]) || inst->type_argv [i]->type == MONO_TYPE_VAR || inst->type_argv [i]->type == MONO_TYPE_MVAR)
						type_argv [i] = &mono_defaults.object_class->byval_arg;
					else
						type_argv [i] = inst->type_argv [i];
				}
				
				shared_context.class_inst = mono_metadata_get_generic_inst (inst->type_argc, type_argv);
				g_free (type_argv);
			}

			inst = context->method_inst;
			if (inst) {
				type_argv = g_new0 (MonoType*, inst->type_argc);
				for (i = 0; i < inst->type_argc; ++i) {
					if (MONO_TYPE_IS_REFERENCE (inst->type_argv [i]) || inst->type_argv [i]->type == MONO_TYPE_VAR || inst->type_argv [i]->type == MONO_TYPE_MVAR)
						type_argv [i] = &mono_defaults.object_class->byval_arg;
					else
						type_argv [i] = inst->type_argv [i];
				}

				shared_context.method_inst = mono_metadata_get_generic_inst (inst->type_argc, type_argv);
				g_free (type_argv);
			}

			if (method->is_generic || mono_class_is_gtd (method->klass))
				declaring_method = method;
			else
				declaring_method = mono_method_get_declaring_generic_method (method);

			method = mono_class_inflate_generic_method_checked (declaring_method, &shared_context, &error);
			g_assert (mono_error_ok (&error)); /* FIXME don't swallow the error */
		}

		/* 
		 * If the method is fully sharable, it was already added in place of its
		 * generic definition.
		 */
		if (mono_method_is_generic_sharable_full (method, FALSE, FALSE, FALSE))
			continue;

		/*
		 * FIXME: Partially shared methods are not shared here, so we end up with
		 * many identical methods.
		 */
		add_extra_method (acfg, method);
	}

	for (i = 0; i < acfg->image->tables [MONO_TABLE_TYPESPEC].rows; ++i) {
		MonoError error;
		MonoClass *klass;

		token = MONO_TOKEN_TYPE_SPEC | (i + 1);

		klass = mono_class_get_checked (acfg->image, token, &error);
		if (!klass || klass->rank) {
			mono_error_cleanup (&error);
			continue;
		}

		add_generic_class (acfg, klass, FALSE, "typespec");
	}

	/* Add types of args/locals */
	for (i = 0; i < acfg->methods->len; ++i) {
		method = (MonoMethod *)g_ptr_array_index (acfg->methods, i);
		add_types_from_method_header (acfg, method);
	}

	if (acfg->image == mono_defaults.corlib) {
		MonoClass *klass;
		MonoType *insts [256];
		int ninsts = 0;

		insts [ninsts ++] = &mono_defaults.byte_class->byval_arg;
		insts [ninsts ++] = &mono_defaults.sbyte_class->byval_arg;
		insts [ninsts ++] = &mono_defaults.int16_class->byval_arg;
		insts [ninsts ++] = &mono_defaults.uint16_class->byval_arg;
		insts [ninsts ++] = &mono_defaults.int32_class->byval_arg;
		insts [ninsts ++] = &mono_defaults.uint32_class->byval_arg;
		insts [ninsts ++] = &mono_defaults.int64_class->byval_arg;
		insts [ninsts ++] = &mono_defaults.uint64_class->byval_arg;
		insts [ninsts ++] = &mono_defaults.single_class->byval_arg;
		insts [ninsts ++] = &mono_defaults.double_class->byval_arg;
		insts [ninsts ++] = &mono_defaults.char_class->byval_arg;
		insts [ninsts ++] = &mono_defaults.boolean_class->byval_arg;

		/* Add GenericComparer<T> instances for primitive types for Enum.ToString () */
		klass = mono_class_try_load_from_name (acfg->image, "System.Collections.Generic", "GenericComparer`1");
		if (klass)
			add_instances_of (acfg, klass, insts, ninsts, TRUE);
		klass = mono_class_try_load_from_name (acfg->image, "System.Collections.Generic", "GenericEqualityComparer`1");
		if (klass)
			add_instances_of (acfg, klass, insts, ninsts, TRUE);

		/* Add instances of EnumEqualityComparer which are created by EqualityComparer<T> for enums */
		{
			MonoClass *enum_comparer;
			MonoType *insts [16];
			int ninsts;

			ninsts = 0;
			insts [ninsts ++] = &mono_defaults.int32_class->byval_arg;
			insts [ninsts ++] = &mono_defaults.uint32_class->byval_arg;
			insts [ninsts ++] = &mono_defaults.uint16_class->byval_arg;
			insts [ninsts ++] = &mono_defaults.byte_class->byval_arg;
			enum_comparer = mono_class_load_from_name (mono_defaults.corlib, "System.Collections.Generic", "EnumEqualityComparer`1");
			add_instances_of (acfg, enum_comparer, insts, ninsts, FALSE);

			ninsts = 0;
			insts [ninsts ++] = &mono_defaults.int16_class->byval_arg;
			enum_comparer = mono_class_load_from_name (mono_defaults.corlib, "System.Collections.Generic", "ShortEnumEqualityComparer`1");
			add_instances_of (acfg, enum_comparer, insts, ninsts, FALSE);

			ninsts = 0;
			insts [ninsts ++] = &mono_defaults.sbyte_class->byval_arg;
			enum_comparer = mono_class_load_from_name (mono_defaults.corlib, "System.Collections.Generic", "SByteEnumEqualityComparer`1");
			add_instances_of (acfg, enum_comparer, insts, ninsts, FALSE);

			enum_comparer = mono_class_load_from_name (mono_defaults.corlib, "System.Collections.Generic", "LongEnumEqualityComparer`1");
			ninsts = 0;
			insts [ninsts ++] = &mono_defaults.int64_class->byval_arg;
			insts [ninsts ++] = &mono_defaults.uint64_class->byval_arg;
			add_instances_of (acfg, enum_comparer, insts, ninsts, FALSE);
		}

		/* Add instances of the array generic interfaces for primitive types */
		/* This will add instances of the InternalArray_ helper methods in Array too */
		klass = mono_class_try_load_from_name (acfg->image, "System.Collections.Generic", "ICollection`1");
		if (klass)
			add_instances_of (acfg, klass, insts, ninsts, TRUE);

		klass = mono_class_try_load_from_name (acfg->image, "System.Collections.Generic", "IList`1");
		if (klass)
			add_instances_of (acfg, klass, insts, ninsts, TRUE);

		klass = mono_class_try_load_from_name (acfg->image, "System.Collections.Generic", "IEnumerable`1");
		if (klass)
			add_instances_of (acfg, klass, insts, ninsts, TRUE);

		/* 
		 * Add a managed-to-native wrapper of Array.GetGenericValueImpl<object>, which is
		 * used for all instances of GetGenericValueImpl by the AOT runtime.
		 */
		{
			MonoGenericContext ctx;
			MonoType *args [16];
			MonoMethod *get_method;
			MonoClass *array_klass = mono_array_class_get (mono_defaults.object_class, 1)->parent;

			get_method = mono_class_get_method_from_name (array_klass, "GetGenericValueImpl", 2);

			if (get_method) {
				MonoError error;
				memset (&ctx, 0, sizeof (ctx));
				args [0] = &mono_defaults.object_class->byval_arg;
				ctx.method_inst = mono_metadata_get_generic_inst (1, args);
				add_extra_method (acfg, mono_marshal_get_native_wrapper (mono_class_inflate_generic_method_checked (get_method, &ctx, &error), TRUE, TRUE));
				g_assert (mono_error_ok (&error)); /* FIXME don't swallow the error */
			}
		}

		/* Same for CompareExchange<T>/Exchange<T> */
		{
			MonoGenericContext ctx;
			MonoType *args [16];
			MonoMethod *m;
			MonoClass *interlocked_klass = mono_class_load_from_name (mono_defaults.corlib, "System.Threading", "Interlocked");
			gpointer iter = NULL;

			while ((m = mono_class_get_methods (interlocked_klass, &iter))) {
				if ((!strcmp (m->name, "CompareExchange") || !strcmp (m->name, "Exchange")) && m->is_generic) {
					MonoError error;
					memset (&ctx, 0, sizeof (ctx));
					args [0] = &mono_defaults.object_class->byval_arg;
					ctx.method_inst = mono_metadata_get_generic_inst (1, args);
					add_extra_method (acfg, mono_marshal_get_native_wrapper (mono_class_inflate_generic_method_checked (m, &ctx, &error), TRUE, TRUE));
					g_assert (mono_error_ok (&error)); /* FIXME don't swallow the error */
				}
			}
		}

		/* Same for Volatile.Read/Write<T> */
		{
			MonoGenericContext ctx;
			MonoType *args [16];
			MonoMethod *m;
			MonoClass *volatile_klass = mono_class_try_load_from_name (mono_defaults.corlib, "System.Threading", "Volatile");
			gpointer iter = NULL;

			if (volatile_klass) {
				while ((m = mono_class_get_methods (volatile_klass, &iter))) {
					if ((!strcmp (m->name, "Read") || !strcmp (m->name, "Write")) && m->is_generic) {
						MonoError error;
						memset (&ctx, 0, sizeof (ctx));
						args [0] = &mono_defaults.object_class->byval_arg;
						ctx.method_inst = mono_metadata_get_generic_inst (1, args);
						add_extra_method (acfg, mono_marshal_get_native_wrapper (mono_class_inflate_generic_method_checked (m, &ctx, &error), TRUE, TRUE));
						g_assert (mono_error_ok (&error)); /* FIXME don't swallow the error */
					}
				}
			}
		}

		/* object[] accessor wrappers. */
		for (i = 1; i < 4; ++i) {
			MonoClass *obj_array_class = mono_array_class_get (mono_defaults.object_class, i);
			MonoMethod *m;

			m = mono_class_get_method_from_name (obj_array_class, "Get", i);
			g_assert (m);

			m = mono_marshal_get_array_accessor_wrapper (m);
			add_extra_method (acfg, m);

			m = mono_class_get_method_from_name (obj_array_class, "Address", i);
			g_assert (m);

			m = mono_marshal_get_array_accessor_wrapper (m);
			add_extra_method (acfg, m);

			m = mono_class_get_method_from_name (obj_array_class, "Set", i + 1);
			g_assert (m);

			m = mono_marshal_get_array_accessor_wrapper (m);
			add_extra_method (acfg, m);
		}
	}
}

/*
 * is_direct_callable:
 *
 *   Return whenever the method identified by JI is directly callable without 
 * going through the PLT.
 */
static gboolean
is_direct_callable (MonoAotCompile *acfg, MonoMethod *method, MonoJumpInfo *patch_info)
{
	if ((patch_info->type == MONO_PATCH_INFO_METHOD) && (patch_info->data.method->klass->image == acfg->image)) {
		MonoCompile *callee_cfg = (MonoCompile *)g_hash_table_lookup (acfg->method_to_cfg, patch_info->data.method);
		if (callee_cfg) {
			gboolean direct_callable = TRUE;

			if (direct_callable && !(!callee_cfg->has_got_slots && mono_class_is_before_field_init (callee_cfg->method->klass)))
				direct_callable = FALSE;
			if ((callee_cfg->method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED) && (!method || method->wrapper_type != MONO_WRAPPER_SYNCHRONIZED))
				// FIXME: Maybe call the wrapper directly ?
				direct_callable = FALSE;

			if (acfg->aot_opts.soft_debug || acfg->aot_opts.no_direct_calls) {
				/* Disable this so all calls go through load_method (), see the
				 * mini_get_debug_options ()->load_aot_jit_info_eagerly = TRUE; line in
				 * mono_debugger_agent_init ().
				 */
				direct_callable = FALSE;
			}

			if (callee_cfg->method->wrapper_type == MONO_WRAPPER_ALLOC)
				/* sgen does some initialization when the allocator method is created */
				direct_callable = FALSE;
			if (callee_cfg->method->wrapper_type == MONO_WRAPPER_WRITE_BARRIER)
				/* we don't know at compile time whether sgen is concurrent or not */
				direct_callable = FALSE;

			if (direct_callable)
				return TRUE;
		}
	} else if ((patch_info->type == MONO_PATCH_INFO_ICALL_ADDR_CALL && patch_info->data.method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)) {
		if (acfg->aot_opts.direct_pinvoke)
			return TRUE;
	} else if (patch_info->type == MONO_PATCH_INFO_ICALL_ADDR_CALL) {
		if (acfg->aot_opts.direct_icalls)
			return TRUE;
		return FALSE;
	}

	return FALSE;
}

#ifdef MONO_ARCH_AOT_SUPPORTED
static const char *
get_pinvoke_import (MonoAotCompile *acfg, MonoMethod *method)
{
	MonoImage *image = method->klass->image;
	MonoMethodPInvoke *piinfo = (MonoMethodPInvoke *) method;
	MonoTableInfo *tables = image->tables;
	MonoTableInfo *im = &tables [MONO_TABLE_IMPLMAP];
	MonoTableInfo *mr = &tables [MONO_TABLE_MODULEREF];
	guint32 im_cols [MONO_IMPLMAP_SIZE];
	char *import;

	import = (char *)g_hash_table_lookup (acfg->method_to_pinvoke_import, method);
	if (import != NULL)
		return import;

	if (!piinfo->implmap_idx || piinfo->implmap_idx > im->rows)
		return NULL;

	mono_metadata_decode_row (im, piinfo->implmap_idx - 1, im_cols, MONO_IMPLMAP_SIZE);

	if (!im_cols [MONO_IMPLMAP_SCOPE] || im_cols [MONO_IMPLMAP_SCOPE] > mr->rows)
		return NULL;

	import = g_strdup_printf ("%s", mono_metadata_string_heap (image, im_cols [MONO_IMPLMAP_NAME]));

	g_hash_table_insert (acfg->method_to_pinvoke_import, method, import);
	
	return import;
}
#else
static const char *
get_pinvoke_import (MonoAotCompile *acfg, MonoMethod *method)
{
	return NULL;
}
#endif

static gint
compare_lne (MonoDebugLineNumberEntry *a, MonoDebugLineNumberEntry *b)
{
	if (a->native_offset == b->native_offset)
		return a->il_offset - b->il_offset;
	else
		return a->native_offset - b->native_offset;
}

/*
 * compute_line_numbers:
 *
 * Returns a sparse array of size CODE_SIZE containing MonoDebugSourceLocation* entries for the native offsets which have a corresponding line number
 * entry.
 */
static MonoDebugSourceLocation**
compute_line_numbers (MonoMethod *method, int code_size, MonoDebugMethodJitInfo *debug_info)
{
	MonoDebugMethodInfo *minfo;
	MonoDebugLineNumberEntry *ln_array;
	MonoDebugSourceLocation *loc;
	int i, prev_line, prev_il_offset;
	int *native_to_il_offset = NULL;
	MonoDebugSourceLocation **res;
	gboolean first;

	minfo = mono_debug_lookup_method (method);
	if (!minfo)
		return NULL;
	// FIXME: This seems to happen when two methods have the same cfg->method_to_register
	if (debug_info->code_size != code_size)
		return NULL;

	g_assert (code_size);

	/* Compute the native->IL offset mapping */

	ln_array = g_new0 (MonoDebugLineNumberEntry, debug_info->num_line_numbers);
	memcpy (ln_array, debug_info->line_numbers, debug_info->num_line_numbers * sizeof (MonoDebugLineNumberEntry));

	qsort (ln_array, debug_info->num_line_numbers, sizeof (MonoDebugLineNumberEntry), (int (*)(const void *, const void *))compare_lne);

	native_to_il_offset = g_new0 (int, code_size + 1);

	for (i = 0; i < debug_info->num_line_numbers; ++i) {
		int j;
		MonoDebugLineNumberEntry *lne = &ln_array [i];

		if (i == 0) {
			for (j = 0; j < lne->native_offset; ++j)
				native_to_il_offset [j] = -1;
		}

		if (i < debug_info->num_line_numbers - 1) {
			MonoDebugLineNumberEntry *lne_next = &ln_array [i + 1];

			for (j = lne->native_offset; j < lne_next->native_offset; ++j)
				native_to_il_offset [j] = lne->il_offset;
		} else {
			for (j = lne->native_offset; j < code_size; ++j)
				native_to_il_offset [j] = lne->il_offset;
		}
	}
	g_free (ln_array);

	/* Compute the native->line number mapping */
	res = g_new0 (MonoDebugSourceLocation*, code_size);
	prev_il_offset = -1;
	prev_line = -1;
	first = TRUE;
	for (i = 0; i < code_size; ++i) {
		int il_offset = native_to_il_offset [i];

		if (il_offset == -1 || il_offset == prev_il_offset)
			continue;
		prev_il_offset = il_offset;
		loc = mono_debug_symfile_lookup_location (minfo, il_offset);
		if (!(loc && loc->source_file))
			continue;
		if (loc->row == prev_line) {
			mono_debug_symfile_free_location (loc);
			continue;
		}
		prev_line = loc->row;
		//printf ("D: %s:%d il=%x native=%x\n", loc->source_file, loc->row, il_offset, i);
		if (first)
			/* This will cover the prolog too */
			res [0] = loc;
		else
			res [i] = loc;
		first = FALSE;
	}
	return res;
}

static int
get_file_index (MonoAotCompile *acfg, const char *source_file)
{
	int findex;

	// FIXME: Free these
	if (!acfg->dwarf_ln_filenames)
		acfg->dwarf_ln_filenames = g_hash_table_new (g_str_hash, g_str_equal);
	findex = GPOINTER_TO_INT (g_hash_table_lookup (acfg->dwarf_ln_filenames, source_file));
	if (!findex) {
		findex = g_hash_table_size (acfg->dwarf_ln_filenames) + 1;
		g_hash_table_insert (acfg->dwarf_ln_filenames, g_strdup (source_file), GINT_TO_POINTER (findex));
		emit_unset_mode (acfg);
		fprintf (acfg->fp, ".file %d \"%s\"\n", findex, mono_dwarf_escape_path (source_file));
	}
	return findex;
}

#ifdef TARGET_ARM64
#define INST_LEN 4
#else
#define INST_LEN 1
#endif

/*
 * emit_and_reloc_code:
 *
 *   Emit the native code in CODE, handling relocations along the way. If GOT_ONLY
 * is true, calls are made through the GOT too. This is used for emitting trampolines
 * in full-aot mode, since calls made from trampolines couldn't go through the PLT,
 * since trampolines are needed to make PTL work.
 */
static void
emit_and_reloc_code (MonoAotCompile *acfg, MonoMethod *method, guint8 *code, guint32 code_len, MonoJumpInfo *relocs, gboolean got_only, MonoDebugMethodJitInfo *debug_info)
{
	int i, pindex, start_index;
	GPtrArray *patches;
	MonoJumpInfo *patch_info;
	MonoDebugSourceLocation **locs = NULL;
	gboolean skip, prologue_end = FALSE;
#ifdef MONO_ARCH_AOT_SUPPORTED
	gboolean direct_call, external_call;
	guint32 got_slot;
	const char *direct_call_target = 0;
	const char *direct_pinvoke;
#endif

	if (acfg->gas_line_numbers && method && debug_info) {
		locs = compute_line_numbers (method, code_len, debug_info);
		if (!locs) {
			int findex = get_file_index (acfg, "<unknown>");
			emit_unset_mode (acfg);
			fprintf (acfg->fp, ".loc %d %d 0\n", findex, 1);
		}
	}

	/* Collect and sort relocations */
	patches = g_ptr_array_new ();
	for (patch_info = relocs; patch_info; patch_info = patch_info->next)
		g_ptr_array_add (patches, patch_info);
	g_ptr_array_sort (patches, compare_patches);

	start_index = 0;
	for (i = 0; i < code_len; i += INST_LEN) {
		patch_info = NULL;
		for (pindex = start_index; pindex < patches->len; ++pindex) {
			patch_info = (MonoJumpInfo *)g_ptr_array_index (patches, pindex);
			if (patch_info->ip.i >= i)
				break;
		}

		if (locs && locs [i]) {
			MonoDebugSourceLocation *loc = locs [i];
			int findex;
			const char *options;

			findex = get_file_index (acfg, loc->source_file);
			emit_unset_mode (acfg);
			if (!prologue_end)
				options = " prologue_end";
			else
				options = "";
			prologue_end = TRUE;
			fprintf (acfg->fp, ".loc %d %d 0%s\n", findex, loc->row, options);
			mono_debug_symfile_free_location (loc);
		}

		skip = FALSE;
#ifdef MONO_ARCH_AOT_SUPPORTED
		if (patch_info && (patch_info->ip.i == i) && (pindex < patches->len)) {
			start_index = pindex;

			switch (patch_info->type) {
			case MONO_PATCH_INFO_NONE:
				break;
			case MONO_PATCH_INFO_GOT_OFFSET: {
				int code_size;
 
				arch_emit_got_offset (acfg, code + i, &code_size);
				i += code_size - INST_LEN;
				skip = TRUE;
				patch_info->type = MONO_PATCH_INFO_NONE;
				break;
			}
			case MONO_PATCH_INFO_OBJC_SELECTOR_REF: {
				int code_size, index;
				char *selector = (char *)patch_info->data.target;

				if (!acfg->objc_selector_to_index)
					acfg->objc_selector_to_index = g_hash_table_new (g_str_hash, g_str_equal);
				if (!acfg->objc_selectors)
					acfg->objc_selectors = g_ptr_array_new ();
				index = GPOINTER_TO_UINT (g_hash_table_lookup (acfg->objc_selector_to_index, selector));
				if (index)
					index --;
				else {
					index = acfg->objc_selector_index;
					g_ptr_array_add (acfg->objc_selectors, (void*)patch_info->data.target);
					g_hash_table_insert (acfg->objc_selector_to_index, selector, GUINT_TO_POINTER (index + 1));
					acfg->objc_selector_index ++;
				}

				arch_emit_objc_selector_ref (acfg, code + i, index, &code_size);
				i += code_size - INST_LEN;
				skip = TRUE;
				patch_info->type = MONO_PATCH_INFO_NONE;
				break;
			}
			default: {
				/*
				 * If this patch is a call, try emitting a direct call instead of
				 * through a PLT entry. This is possible if the called method is in
				 * the same assembly and requires no initialization.
				 */
				direct_call = FALSE;
				external_call = FALSE;
				if ((patch_info->type == MONO_PATCH_INFO_METHOD) && (patch_info->data.method->klass->image == acfg->image)) {
					if (!got_only && is_direct_callable (acfg, method, patch_info)) {
						MonoCompile *callee_cfg = (MonoCompile *)g_hash_table_lookup (acfg->method_to_cfg, patch_info->data.method);
						//printf ("DIRECT: %s %s\n", method ? mono_method_full_name (method, TRUE) : "", mono_method_full_name (callee_cfg->method, TRUE));
						direct_call = TRUE;
						direct_call_target = callee_cfg->asm_symbol;
						patch_info->type = MONO_PATCH_INFO_NONE;
						acfg->stats.direct_calls ++;
					}

					acfg->stats.all_calls ++;
				} else if (patch_info->type == MONO_PATCH_INFO_ICALL_ADDR_CALL) {
					if (!got_only && is_direct_callable (acfg, method, patch_info)) {
						if (!(patch_info->data.method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL))
							direct_pinvoke = mono_lookup_icall_symbol (patch_info->data.method);
						else
							direct_pinvoke = get_pinvoke_import (acfg, patch_info->data.method);
						if (direct_pinvoke) {
							direct_call = TRUE;
							g_assert (strlen (direct_pinvoke) < 1000);
							direct_call_target = g_strdup_printf ("%s%s", acfg->user_symbol_prefix, direct_pinvoke);
						}
					}
				} else if (patch_info->type == MONO_PATCH_INFO_JIT_ICALL_ADDR) {
					const char *sym = mono_lookup_jit_icall_symbol (patch_info->data.name);
					if (!got_only && sym && acfg->aot_opts.direct_icalls) {
						/* Call to a C function implementing a jit icall */
						direct_call = TRUE;
						external_call = TRUE;
						g_assert (strlen (sym) < 1000);
						direct_call_target = g_strdup_printf ("%s%s", acfg->user_symbol_prefix, sym);
					}
				} else if (patch_info->type == MONO_PATCH_INFO_INTERNAL_METHOD) {
					MonoJitICallInfo *info = mono_find_jit_icall_by_name (patch_info->data.name);
					const char *sym = mono_lookup_jit_icall_symbol (patch_info->data.name);
					if (!got_only && sym && acfg->aot_opts.direct_icalls && info->func == info->wrapper) {
						/* Call to a jit icall without a wrapper */
						direct_call = TRUE;
						external_call = TRUE;
						g_assert (strlen (sym) < 1000);
						direct_call_target = g_strdup_printf ("%s%s", acfg->user_symbol_prefix, sym);
					}
				}

				if (direct_call) {
					patch_info->type = MONO_PATCH_INFO_NONE;
					acfg->stats.direct_calls ++;
				}

				if (!got_only && !direct_call) {
					MonoPltEntry *plt_entry = get_plt_entry (acfg, patch_info);
					if (plt_entry) {
						/* This patch has a PLT entry, so we must emit a call to the PLT entry */
						direct_call = TRUE;
						direct_call_target = plt_entry->symbol;
		
						/* Nullify the patch */
						patch_info->type = MONO_PATCH_INFO_NONE;
						plt_entry->jit_used = TRUE;
					}
				}

				if (direct_call) {
					int call_size;

					arch_emit_direct_call (acfg, direct_call_target, external_call, FALSE, patch_info, &call_size);
					i += call_size - INST_LEN;
				} else {
					int code_size;

					got_slot = get_got_offset (acfg, FALSE, patch_info);

					arch_emit_got_access (acfg, acfg->got_symbol, code + i, got_slot, &code_size);
					i += code_size - INST_LEN;
				}
				skip = TRUE;
			}
			}
		}
#endif /* MONO_ARCH_AOT_SUPPORTED */

		if (!skip) {
			/* Find next patch */
			patch_info = NULL;
			for (pindex = start_index; pindex < patches->len; ++pindex) {
				patch_info = (MonoJumpInfo *)g_ptr_array_index (patches, pindex);
				if (patch_info->ip.i >= i)
					break;
			}

			/* Try to emit multiple bytes at once */
			if (pindex < patches->len && patch_info->ip.i > i) {
				int limit;

				for (limit = i + INST_LEN; limit < patch_info->ip.i; limit += INST_LEN) {
					if (locs && locs [limit])
						break;
				}

				emit_code_bytes (acfg, code + i, limit - i);
				i = limit - INST_LEN;
			} else {
				emit_code_bytes (acfg, code + i, INST_LEN);
			}
		}
	}

	g_ptr_array_free (patches, TRUE);
	g_free (locs);
}

/*
 * sanitize_symbol:
 *
 *   Return a modified version of S which only includes characters permissible in symbols.
 */
static char*
sanitize_symbol (MonoAotCompile *acfg, char *s)
{
	gboolean process = FALSE;
	int i, len;
	GString *gs;
	char *res;

	if (!s)
		return s;

	len = strlen (s);
	for (i = 0; i < len; ++i)
		if (!(s [i] <= 0x7f && (isalnum (s [i]) || s [i] == '_')))
			process = TRUE;
	if (!process)
		return s;

	gs = g_string_sized_new (len);
	for (i = 0; i < len; ++i) {
		guint8 c = s [i];
		if (c <= 0x7f && (isalnum (c) || c == '_')) {
			g_string_append_c (gs, c);
		} else if (c > 0x7f) {
			/* multi-byte utf8 */
			g_string_append_printf (gs, "_0x%x", c);
			i ++;
			c = s [i];
			while (c >> 6 == 0x2) {
				g_string_append_printf (gs, "%x", c);
				i ++;
				c = s [i];
			}
			g_string_append_printf (gs, "_");
			i --;
		} else {
			g_string_append_c (gs, '_');
		}
	}

	res = mono_mempool_strdup (acfg->mempool, gs->str);
	g_string_free (gs, TRUE);
	return res;
}

static char*
get_debug_sym (MonoMethod *method, const char *prefix, GHashTable *cache)
{
	char *name1, *name2, *cached;
	int i, j, len, count;
	MonoMethod *cached_method;

	name1 = mono_method_full_name (method, TRUE);

#ifdef TARGET_MACH
	// This is so that we don't accidentally create a local symbol (which starts with 'L')
	if ((!prefix || !*prefix) && name1 [0] == 'L')
		prefix = "_";
#endif

#if defined(TARGET_WIN32) && defined(TARGET_X86)
	char adjustedPrefix [MAX_SYMBOL_SIZE];
	prefix = mangle_symbol (prefix, adjustedPrefix, G_N_ELEMENTS (adjustedPrefix));
#endif

	len = strlen (name1);
	name2 = (char *)malloc (strlen (prefix) + len + 16);
	memcpy (name2, prefix, strlen (prefix));
	j = strlen (prefix);
	for (i = 0; i < len; ++i) {
		if (i == 0 && name1 [0] >= '0' && name1 [0] <= '9') {
			name2 [j ++] = '_';
		} else if (isalnum (name1 [i])) {
			name2 [j ++] = name1 [i];
		} else if (name1 [i] == ' ' && name1 [i + 1] == '(' && name1 [i + 2] == ')') {
			i += 2;
		} else if (name1 [i] == ',' && name1 [i + 1] == ' ') {
			name2 [j ++] = '_';
			i++;
		} else if (name1 [i] == '(' || name1 [i] == ')' || name1 [i] == '>') {
		} else
			name2 [j ++] = '_';
	}
	name2 [j] = '\0';

	g_free (name1);

	count = 0;
	while (TRUE) {
		cached_method = (MonoMethod *)g_hash_table_lookup (cache, name2);
		if (!(cached_method && cached_method != method))
			break;
		sprintf (name2 + j, "_%d", count);
		count ++;
	}

	cached = g_strdup (name2);
	g_hash_table_insert (cache, cached, method);

	return name2;
}

static void
emit_method_code (MonoAotCompile *acfg, MonoCompile *cfg)
{
	MonoMethod *method;
	int method_index;
	guint8 *code;
	char *debug_sym = NULL;
	char *symbol = NULL;
	int func_alignment = AOT_FUNC_ALIGNMENT;
	char *export_name;

	method = cfg->orig_method;
	code = cfg->native_code;

	method_index = get_method_index (acfg, method);
	symbol = g_strdup_printf ("%sme_%x", acfg->temp_prefix, method_index);

	/* Make the labels local */
	emit_section_change (acfg, ".text", 0);
	emit_alignment_code (acfg, func_alignment);
	
	if (acfg->global_symbols && acfg->need_no_dead_strip)
		fprintf (acfg->fp, "	.no_dead_strip %s\n", cfg->asm_symbol);
	
	emit_label (acfg, cfg->asm_symbol);

	if (acfg->aot_opts.write_symbols && !acfg->global_symbols && !acfg->llvm) {
		/* 
		 * Write a C style symbol for every method, this has two uses:
		 * - it works on platforms where the dwarf debugging info is not
		 *   yet supported.
		 * - it allows the setting of breakpoints of aot-ed methods.
		 */
		debug_sym = get_debug_sym (method, "", acfg->method_label_hash);
		cfg->asm_debug_symbol = g_strdup (debug_sym);

		if (acfg->need_no_dead_strip)
			fprintf (acfg->fp, "	.no_dead_strip %s\n", debug_sym);
		emit_local_symbol (acfg, debug_sym, symbol, TRUE);
		emit_label (acfg, debug_sym);
	}

	export_name = (char *)g_hash_table_lookup (acfg->export_names, method);
	if (export_name) {
		/* Emit a global symbol for the method */
		emit_global_inner (acfg, export_name, TRUE);
		emit_label (acfg, export_name);
	}

	if (cfg->verbose_level > 0)
		g_print ("Method %s emitted as %s\n", mono_method_get_full_name (method), cfg->asm_symbol);

	acfg->stats.code_size += cfg->code_len;

	acfg->cfgs [method_index]->got_offset = acfg->got_offset;

	emit_and_reloc_code (acfg, method, code, cfg->code_len, cfg->patch_info, FALSE, mono_debug_find_method (cfg->jit_info->d.method, mono_domain_get ()));

	emit_line (acfg);

	if (acfg->aot_opts.write_symbols) {
		if (debug_sym)
			emit_symbol_size (acfg, debug_sym, ".");
		else
			emit_symbol_size (acfg, cfg->asm_symbol, ".");
		g_free (debug_sym);
	}

	emit_label (acfg, symbol);
	g_free (symbol);
}

/**
 * encode_patch:
 *
 *  Encode PATCH_INFO into its disk representation.
 */
static void
encode_patch (MonoAotCompile *acfg, MonoJumpInfo *patch_info, guint8 *buf, guint8 **endbuf)
{
	guint8 *p = buf;

	switch (patch_info->type) {
	case MONO_PATCH_INFO_NONE:
		break;
	case MONO_PATCH_INFO_IMAGE:
		encode_value (get_image_index (acfg, patch_info->data.image), p, &p);
		break;
	case MONO_PATCH_INFO_MSCORLIB_GOT_ADDR:
	case MONO_PATCH_INFO_GC_CARD_TABLE_ADDR:
	case MONO_PATCH_INFO_GC_NURSERY_START:
	case MONO_PATCH_INFO_GC_NURSERY_BITS:
		break;
	case MONO_PATCH_INFO_CASTCLASS_CACHE:
		encode_value (patch_info->data.index, p, &p);
		break;
	case MONO_PATCH_INFO_METHOD_REL:
		encode_value ((gint)patch_info->data.offset, p, &p);
		break;
	case MONO_PATCH_INFO_SWITCH: {
		gpointer *table = (gpointer *)patch_info->data.table->table;
		int k;

		encode_value (patch_info->data.table->table_size, p, &p);
		for (k = 0; k < patch_info->data.table->table_size; k++)
			encode_value ((int)(gssize)table [k], p, &p);
		break;
	}
	case MONO_PATCH_INFO_METHODCONST:
	case MONO_PATCH_INFO_METHOD:
	case MONO_PATCH_INFO_METHOD_JUMP:
	case MONO_PATCH_INFO_ICALL_ADDR:
	case MONO_PATCH_INFO_ICALL_ADDR_CALL:
	case MONO_PATCH_INFO_METHOD_RGCTX:
	case MONO_PATCH_INFO_METHOD_CODE_SLOT:
		encode_method_ref (acfg, patch_info->data.method, p, &p);
		break;
	case MONO_PATCH_INFO_AOT_JIT_INFO:
	case MONO_PATCH_INFO_GET_TLS_TRAMP:
	case MONO_PATCH_INFO_SET_TLS_TRAMP:
		encode_value (patch_info->data.index, p, &p);
		break;
	case MONO_PATCH_INFO_INTERNAL_METHOD:
	case MONO_PATCH_INFO_JIT_ICALL_ADDR: {
		guint32 len = strlen (patch_info->data.name);

		encode_value (len, p, &p);

		memcpy (p, patch_info->data.name, len);
		p += len;
		*p++ = '\0';
		break;
	}
	case MONO_PATCH_INFO_LDSTR: {
		guint32 image_index = get_image_index (acfg, patch_info->data.token->image);
		guint32 token = patch_info->data.token->token;
		g_assert (mono_metadata_token_code (token) == MONO_TOKEN_STRING);
		encode_value (image_index, p, &p);
		encode_value (patch_info->data.token->token - MONO_TOKEN_STRING, p, &p);
		break;
	}
	case MONO_PATCH_INFO_RVA:
	case MONO_PATCH_INFO_DECLSEC:
	case MONO_PATCH_INFO_LDTOKEN:
	case MONO_PATCH_INFO_TYPE_FROM_HANDLE:
		encode_value (get_image_index (acfg, patch_info->data.token->image), p, &p);
		encode_value (patch_info->data.token->token, p, &p);
		encode_value (patch_info->data.token->has_context, p, &p);
		if (patch_info->data.token->has_context)
			encode_generic_context (acfg, &patch_info->data.token->context, p, &p);
		break;
	case MONO_PATCH_INFO_EXC_NAME: {
		MonoClass *ex_class;

		ex_class =
			mono_class_load_from_name (mono_defaults.exception_class->image,
								  "System", (const char *)patch_info->data.target);
		encode_klass_ref (acfg, ex_class, p, &p);
		break;
	}
	case MONO_PATCH_INFO_R4:
		encode_value (*((guint32 *)patch_info->data.target), p, &p);
		break;
	case MONO_PATCH_INFO_R8:
		encode_value (((guint32 *)patch_info->data.target) [MINI_LS_WORD_IDX], p, &p);
		encode_value (((guint32 *)patch_info->data.target) [MINI_MS_WORD_IDX], p, &p);
		break;
	case MONO_PATCH_INFO_VTABLE:
	case MONO_PATCH_INFO_CLASS:
	case MONO_PATCH_INFO_IID:
	case MONO_PATCH_INFO_ADJUSTED_IID:
		encode_klass_ref (acfg, patch_info->data.klass, p, &p);
		break;
	case MONO_PATCH_INFO_DELEGATE_TRAMPOLINE:
		encode_klass_ref (acfg, patch_info->data.del_tramp->klass, p, &p);
		if (patch_info->data.del_tramp->method) {
			encode_value (1, p, &p);
			encode_method_ref (acfg, patch_info->data.del_tramp->method, p, &p);
		} else {
			encode_value (0, p, &p);
		}
		encode_value (patch_info->data.del_tramp->is_virtual, p, &p);
		break;
	case MONO_PATCH_INFO_FIELD:
	case MONO_PATCH_INFO_SFLDA:
		encode_field_info (acfg, patch_info->data.field, p, &p);
		break;
	case MONO_PATCH_INFO_INTERRUPTION_REQUEST_FLAG:
		break;
	case MONO_PATCH_INFO_RGCTX_FETCH:
	case MONO_PATCH_INFO_RGCTX_SLOT_INDEX: {
		MonoJumpInfoRgctxEntry *entry = patch_info->data.rgctx_entry;
		guint32 offset;
		guint8 *buf2, *p2;

		/* 
		 * entry->method has a lenghtly encoding and multiple rgctx_fetch entries
		 * reference the same method, so encode the method only once.
		 */
		offset = GPOINTER_TO_UINT (g_hash_table_lookup (acfg->method_blob_hash, entry->method));
		if (!offset) {
			buf2 = (guint8 *)g_malloc (1024);
			p2 = buf2;

			encode_method_ref (acfg, entry->method, p2, &p2);
			g_assert (p2 - buf2 < 1024);

			offset = add_to_blob (acfg, buf2, p2 - buf2);
			g_free (buf2);

			g_hash_table_insert (acfg->method_blob_hash, entry->method, GUINT_TO_POINTER (offset + 1));
		} else {
			offset --;
		}

		encode_value (offset, p, &p);
		g_assert ((int)entry->info_type < 256);
		g_assert (entry->data->type < 256);
		encode_value ((entry->in_mrgctx ? 1 : 0) | (entry->info_type << 1) | (entry->data->type << 9), p, &p);
		encode_patch (acfg, entry->data, p, &p);
		break;
	}
	case MONO_PATCH_INFO_SEQ_POINT_INFO:
	case MONO_PATCH_INFO_AOT_MODULE:
		break;
	case MONO_PATCH_INFO_SIGNATURE:
	case MONO_PATCH_INFO_GSHAREDVT_IN_WRAPPER:
		encode_signature (acfg, (MonoMethodSignature*)patch_info->data.target, p, &p);
		break;
	case MONO_PATCH_INFO_GSHAREDVT_CALL:
		encode_signature (acfg, (MonoMethodSignature*)patch_info->data.gsharedvt->sig, p, &p);
		encode_method_ref (acfg, patch_info->data.gsharedvt->method, p, &p);
		break;
	case MONO_PATCH_INFO_GSHAREDVT_METHOD: {
		MonoGSharedVtMethodInfo *info = patch_info->data.gsharedvt_method;
		int i;

		encode_method_ref (acfg, info->method, p, &p);
		encode_value (info->num_entries, p, &p);
		for (i = 0; i < info->num_entries; ++i) {
			MonoRuntimeGenericContextInfoTemplate *template_ = &info->entries [i];

			encode_value (template_->info_type, p, &p);
			switch (mini_rgctx_info_type_to_patch_info_type (template_->info_type)) {
			case MONO_PATCH_INFO_CLASS:
				encode_klass_ref (acfg, mono_class_from_mono_type ((MonoType *)template_->data), p, &p);
				break;
			case MONO_PATCH_INFO_FIELD:
				encode_field_info (acfg, (MonoClassField *)template_->data, p, &p);
				break;
			default:
				g_assert_not_reached ();
				break;
			}
		}
		break;
	}
	case MONO_PATCH_INFO_LDSTR_LIT: {
		const char *s = (const char *)patch_info->data.target;
		int len = strlen (s);

		encode_value (len, p, &p);
		memcpy (p, s, len + 1);
		p += len + 1;
		break;
	}
	case MONO_PATCH_INFO_VIRT_METHOD:
		encode_klass_ref (acfg, patch_info->data.virt_method->klass, p, &p);
		encode_method_ref (acfg, patch_info->data.virt_method->method, p, &p);
		break;
	case MONO_PATCH_INFO_GC_SAFE_POINT_FLAG:
	case MONO_PATCH_INFO_JIT_THREAD_ATTACH:
		break;
	default:
		g_warning ("unable to handle jump info %d", patch_info->type);
		g_assert_not_reached ();
	}

	*endbuf = p;
}

static void
encode_patch_list (MonoAotCompile *acfg, GPtrArray *patches, int n_patches, gboolean llvm, int first_got_offset, guint8 *buf, guint8 **endbuf)
{
	guint8 *p = buf;
	guint32 pindex, offset;
	MonoJumpInfo *patch_info;

	encode_value (n_patches, p, &p);

	for (pindex = 0; pindex < patches->len; ++pindex) {
		patch_info = (MonoJumpInfo *)g_ptr_array_index (patches, pindex);

		if (patch_info->type == MONO_PATCH_INFO_NONE || patch_info->type == MONO_PATCH_INFO_BB)
			/* Nothing to do */
			continue;

		offset = get_got_offset (acfg, llvm, patch_info);
		encode_value (offset, p, &p);
	}

	*endbuf = p;
}

static void
emit_method_info (MonoAotCompile *acfg, MonoCompile *cfg)
{
	MonoMethod *method;
	int pindex, buf_size, n_patches;
	GPtrArray *patches;
	MonoJumpInfo *patch_info;
	guint32 method_index;
	guint8 *p, *buf;
	guint32 first_got_offset;

	method = cfg->orig_method;

	method_index = get_method_index (acfg, method);

	/* Sort relocations */
	patches = g_ptr_array_new ();
	for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next)
		g_ptr_array_add (patches, patch_info);
	g_ptr_array_sort (patches, compare_patches);

	first_got_offset = acfg->cfgs [method_index]->got_offset;

	/**********************/
	/* Encode method info */
	/**********************/

	buf_size = (patches->len < 1000) ? 40960 : 40960 + (patches->len * 64);
	p = buf = (guint8 *)g_malloc (buf_size);

	if (mono_class_get_cctor (method->klass)) {
		encode_value (1, p, &p);
		encode_klass_ref (acfg, method->klass, p, &p);
	} else {
		/* Not needed when loading the method */
		encode_value (0, p, &p);
	}

	g_assert (!(cfg->opt & MONO_OPT_SHARED));

	n_patches = 0;
	for (pindex = 0; pindex < patches->len; ++pindex) {
		patch_info = (MonoJumpInfo *)g_ptr_array_index (patches, pindex);
		
		if ((patch_info->type == MONO_PATCH_INFO_GOT_OFFSET) ||
			(patch_info->type == MONO_PATCH_INFO_NONE)) {
			patch_info->type = MONO_PATCH_INFO_NONE;
			/* Nothing to do */
			continue;
		}

		if ((patch_info->type == MONO_PATCH_INFO_IMAGE) && (patch_info->data.image == acfg->image)) {
			/* Stored in a GOT slot initialized at module load time */
			patch_info->type = MONO_PATCH_INFO_NONE;
			continue;
		}

		if (patch_info->type == MONO_PATCH_INFO_GC_CARD_TABLE_ADDR ||
			patch_info->type == MONO_PATCH_INFO_GC_NURSERY_START ||
			patch_info->type == MONO_PATCH_INFO_GC_NURSERY_BITS ||
			patch_info->type == MONO_PATCH_INFO_AOT_MODULE) {
			/* Stored in a GOT slot initialized at module load time */
			patch_info->type = MONO_PATCH_INFO_NONE;
			continue;
		}

		if (is_plt_patch (patch_info) && !(cfg->compile_llvm && acfg->aot_opts.llvm_only)) {
			/* Calls are made through the PLT */
			patch_info->type = MONO_PATCH_INFO_NONE;
			continue;
		}

		n_patches ++;
	}

	if (n_patches)
		g_assert (cfg->has_got_slots);

	encode_patch_list (acfg, patches, n_patches, cfg->compile_llvm, first_got_offset, p, &p);

	g_ptr_array_free (patches, TRUE);

	acfg->stats.info_size += p - buf;

	g_assert (p - buf < buf_size);

	cfg->method_info_offset = add_to_blob (acfg, buf, p - buf);
	g_free (buf);
}

static guint32
get_unwind_info_offset (MonoAotCompile *acfg, guint8 *encoded, guint32 encoded_len)
{
	guint32 cache_index;
	guint32 offset;

	/* Reuse the unwind module to canonize and store unwind info entries */
	cache_index = mono_cache_unwind_info (encoded, encoded_len);

	/* Use +/- 1 to distinguish 0s from missing entries */
	offset = GPOINTER_TO_UINT (g_hash_table_lookup (acfg->unwind_info_offsets, GUINT_TO_POINTER (cache_index + 1)));
	if (offset)
		return offset - 1;
	else {
		guint8 buf [16];
		guint8 *p;

		/* 
		 * It would be easier to use assembler symbols, but the caller needs an
		 * offset now.
		 */
		offset = acfg->unwind_info_offset;
		g_hash_table_insert (acfg->unwind_info_offsets, GUINT_TO_POINTER (cache_index + 1), GUINT_TO_POINTER (offset + 1));
		g_ptr_array_add (acfg->unwind_ops, GUINT_TO_POINTER (cache_index));

		p = buf;
		encode_value (encoded_len, p, &p);

		acfg->unwind_info_offset += encoded_len + (p - buf);
		return offset;
	}
}

static void
emit_exception_debug_info (MonoAotCompile *acfg, MonoCompile *cfg, gboolean store_seq_points)
{
	int i, k, buf_size;
	guint32 debug_info_size, seq_points_size;
	guint8 *code;
	MonoMethodHeader *header;
	guint8 *p, *buf, *debug_info;
	MonoJitInfo *jinfo = cfg->jit_info;
	guint32 flags;
	gboolean use_unwind_ops = FALSE;
	MonoSeqPointInfo *seq_points;

	code = cfg->native_code;
	header = cfg->header;

	if (!acfg->aot_opts.nodebug) {
		mono_debug_serialize_debug_info (cfg, &debug_info, &debug_info_size);
	} else {
		debug_info = NULL;
		debug_info_size = 0;
	}

	seq_points = cfg->seq_point_info;
	seq_points_size = (store_seq_points)? mono_seq_point_info_get_write_size (seq_points) : 0;

	buf_size = header->num_clauses * 256 + debug_info_size + 2048 + seq_points_size + cfg->gc_map_size;

	p = buf = (guint8 *)g_malloc (buf_size);

	use_unwind_ops = cfg->unwind_ops != NULL;

	flags = (jinfo->has_generic_jit_info ? 1 : 0) | (use_unwind_ops ? 2 : 0) | (header->num_clauses ? 4 : 0) | (seq_points_size ? 8 : 0) | (cfg->compile_llvm ? 16 : 0) | (jinfo->has_try_block_holes ? 32 : 0) | (cfg->gc_map ? 64 : 0) | (jinfo->has_arch_eh_info ? 128 : 0);

	encode_value (flags, p, &p);

	if (use_unwind_ops) {
		guint32 encoded_len;
		guint8 *encoded;
		guint32 unwind_desc;

		encoded = mono_unwind_ops_encode (cfg->unwind_ops, &encoded_len);

		unwind_desc = get_unwind_info_offset (acfg, encoded, encoded_len);
		encode_value (unwind_desc, p, &p);

		g_free (encoded);
	} else {
		encode_value (jinfo->unwind_info, p, &p);
	}

	/*Encode the number of holes before the number of clauses to make decoding easier*/
	if (jinfo->has_try_block_holes) {
		MonoTryBlockHoleTableJitInfo *table = mono_jit_info_get_try_block_hole_table_info (jinfo);
		encode_value (table->num_holes, p, &p);
	}

	if (jinfo->has_arch_eh_info) {
		/*
		 * In AOT mode, the code length is calculated from the address of the previous method,
		 * which could include alignment padding, so calculating the start of the epilog as
		 * code_len - epilog_size is correct any more. Save the real code len as a workaround.
		 */
		encode_value (jinfo->code_size, p, &p);
	}

	/* Exception table */
	if (cfg->compile_llvm) {
		/*
		 * When using LLVM, we can't emit some data, like pc offsets, this reg/offset etc.,
		 * since the information is only available to llc. Instead, we let llc save the data
		 * into the LSDA, and read it from there at runtime.
		 */
		/* The assembly might be CIL stripped so emit the data ourselves */
		if (header->num_clauses)
			encode_value (header->num_clauses, p, &p);

		for (k = 0; k < header->num_clauses; ++k) {
			MonoExceptionClause *clause;

			clause = &header->clauses [k];

			encode_value (clause->flags, p, &p);
			if (clause->data.catch_class) {
				encode_value (1, p, &p);
				encode_klass_ref (acfg, clause->data.catch_class, p, &p);
			} else {
				encode_value (0, p, &p);
			}

			/* Emit the IL ranges too, since they might not be available at runtime */
			encode_value (clause->try_offset, p, &p);
			encode_value (clause->try_len, p, &p);
			encode_value (clause->handler_offset, p, &p);
			encode_value (clause->handler_len, p, &p);

			/* Emit a list of nesting clauses */
			for (i = 0; i < header->num_clauses; ++i) {
				gint32 cindex1 = k;
				MonoExceptionClause *clause1 = &header->clauses [cindex1];
				gint32 cindex2 = i;
				MonoExceptionClause *clause2 = &header->clauses [cindex2];

				if (cindex1 != cindex2 && clause1->try_offset >= clause2->try_offset && clause1->handler_offset <= clause2->handler_offset)
					encode_value (i, p, &p);
			}
			encode_value (-1, p, &p);
		}
	} else {
		if (jinfo->num_clauses)
			encode_value (jinfo->num_clauses, p, &p);

		for (k = 0; k < jinfo->num_clauses; ++k) {
			MonoJitExceptionInfo *ei = &jinfo->clauses [k];

			encode_value (ei->flags, p, &p);
#ifdef MONO_CONTEXT_SET_LLVM_EXC_REG
			/* Not used for catch clauses */
			if (ei->flags != MONO_EXCEPTION_CLAUSE_NONE)
				encode_value (ei->exvar_offset, p, &p);
#else
			encode_value (ei->exvar_offset, p, &p);
#endif

			if (ei->flags == MONO_EXCEPTION_CLAUSE_FILTER || ei->flags == MONO_EXCEPTION_CLAUSE_FINALLY)
				encode_value ((gint)((guint8*)ei->data.filter - code), p, &p);
			else {
				if (ei->data.catch_class) {
					guint8 *buf2, *p2;
					int len;

					buf2 = (guint8 *)g_malloc (4096);
					p2 = buf2;
					encode_klass_ref (acfg, ei->data.catch_class, p2, &p2);
					len = p2 - buf2;
					g_assert (len < 4096);
					encode_value (len, p, &p);
					memcpy (p, buf2, len);
					p += p2 - buf2;
					g_free (buf2);
				} else {
					encode_value (0, p, &p);
				}
			}

			encode_value ((gint)((guint8*)ei->try_start - code), p, &p);
			encode_value ((gint)((guint8*)ei->try_end - code), p, &p);
			encode_value ((gint)((guint8*)ei->handler_start - code), p, &p);
		}
	}

	if (jinfo->has_try_block_holes) {
		MonoTryBlockHoleTableJitInfo *table = mono_jit_info_get_try_block_hole_table_info (jinfo);
		for (i = 0; i < table->num_holes; ++i) {
			MonoTryBlockHoleJitInfo *hole = &table->holes [i];
			encode_value (hole->clause, p, &p);
			encode_value (hole->length, p, &p);
			encode_value (hole->offset, p, &p);
		}
	}

	if (jinfo->has_arch_eh_info) {
		MonoArchEHJitInfo *eh_info;

		eh_info = mono_jit_info_get_arch_eh_info (jinfo);
		encode_value (eh_info->stack_size, p, &p);
		encode_value (eh_info->epilog_size, p, &p);
	}

	if (jinfo->has_generic_jit_info) {
		MonoGenericJitInfo *gi = mono_jit_info_get_generic_jit_info (jinfo);
		MonoGenericSharingContext* gsctx = gi->generic_sharing_context;
		guint8 *buf2, *p2;
		int len;

		encode_value (gi->nlocs, p, &p);
		if (gi->nlocs) {
			for (i = 0; i < gi->nlocs; ++i) {
				MonoDwarfLocListEntry *entry = &gi->locations [i];

				encode_value (entry->is_reg ? 1 : 0, p, &p);
				encode_value (entry->reg, p, &p);
				if (!entry->is_reg)
					encode_value (entry->offset, p, &p);
				if (i == 0)
					g_assert (entry->from == 0);
				else
					encode_value (entry->from, p, &p);
				encode_value (entry->to, p, &p);
			}
		} else {
			if (!cfg->compile_llvm) {
				encode_value (gi->has_this ? 1 : 0, p, &p);
				encode_value (gi->this_reg, p, &p);
				encode_value (gi->this_offset, p, &p);
			}
		}

		/* 
		 * Need to encode jinfo->method too, since it is not equal to 'method'
		 * when using generic sharing.
		 */
		buf2 = (guint8 *)g_malloc (4096);
		p2 = buf2;
		encode_method_ref (acfg, jinfo->d.method, p2, &p2);
		len = p2 - buf2;
		g_assert (len < 4096);
		encode_value (len, p, &p);
		memcpy (p, buf2, len);
		p += p2 - buf2;
		g_free (buf2);

		if (gsctx && gsctx->is_gsharedvt) {
			encode_value (1, p, &p);
		} else {
			encode_value (0, p, &p);
		}
	}

	if (seq_points_size)
		p += mono_seq_point_info_write (seq_points, p);

	g_assert (debug_info_size < buf_size);

	encode_value (debug_info_size, p, &p);
	if (debug_info_size) {
		memcpy (p, debug_info, debug_info_size);
		p += debug_info_size;
		g_free (debug_info);
	}

	/* GC Map */
	if (cfg->gc_map) {
		encode_value (cfg->gc_map_size, p, &p);
		/* The GC map requires 4 bytes of alignment */
		while ((gsize)p % 4)
			p ++;
		memcpy (p, cfg->gc_map, cfg->gc_map_size);
		p += cfg->gc_map_size;
	}

	acfg->stats.ex_info_size += p - buf;

	g_assert (p - buf < buf_size);

	/* Emit info */
	/* The GC Map requires 4 byte alignment */
	cfg->ex_info_offset = add_to_blob_aligned (acfg, buf, p - buf, cfg->gc_map ? 4 : 1);
	g_free (buf);
}

static guint32
emit_klass_info (MonoAotCompile *acfg, guint32 token)
{
	MonoError error;
	MonoClass *klass = mono_class_get_checked (acfg->image, token, &error);
	guint8 *p, *buf;
	int i, buf_size, res;
	gboolean no_special_static, cant_encode;
	gpointer iter = NULL;

	if (!klass) {
		mono_error_cleanup (&error);

		buf_size = 16;

		p = buf = (guint8 *)g_malloc (buf_size);

		/* Mark as unusable */
		encode_value (-1, p, &p);

		res = add_to_blob (acfg, buf, p - buf);
		g_free (buf);

		return res;
	}
		
	buf_size = 10240 + (klass->vtable_size * 16);
	p = buf = (guint8 *)g_malloc (buf_size);

	g_assert (klass);

	mono_class_init (klass);

	mono_class_get_nested_types (klass, &iter);
	g_assert (klass->nested_classes_inited);

	mono_class_setup_vtable (klass);

	/* 
	 * Emit all the information which is required for creating vtables so
	 * the runtime does not need to create the MonoMethod structures which
	 * take up a lot of space.
	 */

	no_special_static = !mono_class_has_special_static_fields (klass);

	/* Check whenever we have enough info to encode the vtable */
	cant_encode = FALSE;
	for (i = 0; i < klass->vtable_size; ++i) {
		MonoMethod *cm = klass->vtable [i];

		if (cm && mono_method_signature (cm)->is_inflated && !g_hash_table_lookup (acfg->token_info_hash, cm))
			cant_encode = TRUE;
	}

	mono_class_has_finalizer (klass);
	if (mono_class_has_failure (klass))
		cant_encode = TRUE;

	if (mono_class_is_gtd (klass) || cant_encode) {
		encode_value (-1, p, &p);
	} else {
		gboolean has_nested = mono_class_get_nested_classes_property (klass) != NULL;
		encode_value (klass->vtable_size, p, &p);
		encode_value ((mono_class_is_gtd (klass) ? (1 << 8) : 0) | (no_special_static << 7) | (klass->has_static_refs << 6) | (klass->has_references << 5) | ((klass->blittable << 4) | (has_nested ? 1 : 0) << 3) | (klass->has_cctor << 2) | (klass->has_finalize << 1) | klass->ghcimpl, p, &p);
		if (klass->has_cctor)
			encode_method_ref (acfg, mono_class_get_cctor (klass), p, &p);
		if (klass->has_finalize)
			encode_method_ref (acfg, mono_class_get_finalizer (klass), p, &p);
 
		encode_value (klass->instance_size, p, &p);
		encode_value (mono_class_data_size (klass), p, &p);
		encode_value (klass->packing_size, p, &p);
		encode_value (klass->min_align, p, &p);

		for (i = 0; i < klass->vtable_size; ++i) {
			MonoMethod *cm = klass->vtable [i];

			if (cm)
				encode_method_ref (acfg, cm, p, &p);
			else
				encode_value (0, p, &p);
		}
	}

	acfg->stats.class_info_size += p - buf;

	g_assert (p - buf < buf_size);
	res = add_to_blob (acfg, buf, p - buf);
	g_free (buf);

	return res;
}

static char*
get_plt_entry_debug_sym (MonoAotCompile *acfg, MonoJumpInfo *ji, GHashTable *cache)
{
	char *debug_sym = NULL;
	char *prefix;

	if (acfg->llvm && llvm_acfg->aot_opts.static_link) {
		/* Need to add a prefix to create unique symbols */
		prefix = g_strdup_printf ("plt_%s_", acfg->assembly_name_sym);
	} else {
#if defined(TARGET_WIN32) && defined(TARGET_X86)
		prefix = mangle_symbol_alloc ("plt_");
#else
		prefix = g_strdup ("plt_");
#endif
	}

	switch (ji->type) {
	case MONO_PATCH_INFO_METHOD:
		debug_sym = get_debug_sym (ji->data.method, prefix, cache);
		break;
	case MONO_PATCH_INFO_INTERNAL_METHOD:
		debug_sym = g_strdup_printf ("%s_jit_icall_%s", prefix, ji->data.name);
		break;
	case MONO_PATCH_INFO_RGCTX_FETCH:
		debug_sym = g_strdup_printf ("%s_rgctx_fetch_%d", prefix, acfg->label_generator ++);
		break;
	case MONO_PATCH_INFO_ICALL_ADDR:
	case MONO_PATCH_INFO_ICALL_ADDR_CALL: {
		char *s = get_debug_sym (ji->data.method, "", cache);
		
		debug_sym = g_strdup_printf ("%s_icall_native_%s", prefix, s);
		g_free (s);
		break;
	}
	case MONO_PATCH_INFO_JIT_ICALL_ADDR:
		debug_sym = g_strdup_printf ("%s_jit_icall_native_%s", prefix, ji->data.name);
		break;
	default:
		break;
	}

	g_free (prefix);

	return sanitize_symbol (acfg, debug_sym);
}

/*
 * Calls made from AOTed code are routed through a table of jumps similar to the
 * ELF PLT (Program Linkage Table). Initially the PLT entries jump to code which transfers
 * control to the AOT runtime through a trampoline.
 */
static void
emit_plt (MonoAotCompile *acfg)
{
	int i;

	if (acfg->aot_opts.llvm_only) {
		g_assert (acfg->plt_offset == 1);
		return;
	}

	emit_line (acfg);

	emit_section_change (acfg, ".text", 0);
	emit_alignment_code (acfg, 16);
	emit_info_symbol (acfg, "plt");
	emit_label (acfg, acfg->plt_symbol);

	for (i = 0; i < acfg->plt_offset; ++i) {
		char *debug_sym = NULL;
		MonoPltEntry *plt_entry = NULL;

		if (i == 0)
			/* 
			 * The first plt entry is unused.
			 */
			continue;

		plt_entry = (MonoPltEntry *)g_hash_table_lookup (acfg->plt_offset_to_entry, GUINT_TO_POINTER (i));

		debug_sym = plt_entry->debug_sym;

		if (acfg->thumb_mixed && !plt_entry->jit_used)
			/* Emit only a thumb version */
			continue;

		/* Skip plt entries not actually called */
		if (!plt_entry->jit_used && !plt_entry->llvm_used)
			continue;

		if (acfg->llvm && !acfg->thumb_mixed) {
			emit_label (acfg, plt_entry->llvm_symbol);
			if (acfg->llvm) {
				emit_global_inner (acfg, plt_entry->llvm_symbol, TRUE);
#if defined(TARGET_MACH)
				fprintf (acfg->fp, ".private_extern %s\n", plt_entry->llvm_symbol);
#endif
			}
		}

		if (debug_sym) {
			if (acfg->need_no_dead_strip) {
				emit_unset_mode (acfg);
				fprintf (acfg->fp, "	.no_dead_strip %s\n", debug_sym);
			}
			emit_local_symbol (acfg, debug_sym, NULL, TRUE);
			emit_label (acfg, debug_sym);
		}

		emit_label (acfg, plt_entry->symbol);

		arch_emit_plt_entry (acfg, acfg->got_symbol, (acfg->plt_got_offset_base + i) * sizeof (gpointer), acfg->plt_got_info_offsets [i]);

		if (debug_sym)
			emit_symbol_size (acfg, debug_sym, ".");
	}

	if (acfg->thumb_mixed) {
		/* Make sure the ARM symbols don't alias the thumb ones */
		emit_zero_bytes (acfg, 16);

		/* 
		 * Emit a separate set of PLT entries using thumb2 which is called by LLVM generated
		 * code.
		 */
		for (i = 0; i < acfg->plt_offset; ++i) {
			char *debug_sym = NULL;
			MonoPltEntry *plt_entry = NULL;

			if (i == 0)
				continue;

			plt_entry = (MonoPltEntry *)g_hash_table_lookup (acfg->plt_offset_to_entry, GUINT_TO_POINTER (i));

			/* Skip plt entries not actually called by LLVM code */
			if (!plt_entry->llvm_used)
				continue;

			if (acfg->aot_opts.write_symbols) {
				if (plt_entry->debug_sym)
					debug_sym = g_strdup_printf ("%s_thumb", plt_entry->debug_sym);
			}

			if (debug_sym) {
#if defined(TARGET_MACH)
				fprintf (acfg->fp, "	.thumb_func %s\n", debug_sym);
				fprintf (acfg->fp, "	.no_dead_strip %s\n", debug_sym);
#endif
				emit_local_symbol (acfg, debug_sym, NULL, TRUE);
				emit_label (acfg, debug_sym);
			}
			fprintf (acfg->fp, "\n.thumb_func\n");

			emit_label (acfg, plt_entry->llvm_symbol);

			if (acfg->llvm)
				emit_global_inner (acfg, plt_entry->llvm_symbol, TRUE);

			arch_emit_llvm_plt_entry (acfg, acfg->got_symbol, (acfg->plt_got_offset_base + i) * sizeof (gpointer), acfg->plt_got_info_offsets [i]);

			if (debug_sym) {
				emit_symbol_size (acfg, debug_sym, ".");
				g_free (debug_sym);
			}
		}
	}

	emit_symbol_size (acfg, acfg->plt_symbol, ".");

	emit_info_symbol (acfg, "plt_end");
}

/*
 * emit_trampoline_full:
 *
 *   If EMIT_TINFO is TRUE, emit additional information which can be used to create a MonoJitInfo for this trampoline by
 * create_jit_info_for_trampoline ().
 */
static G_GNUC_UNUSED void
emit_trampoline_full (MonoAotCompile *acfg, int got_offset, MonoTrampInfo *info, gboolean emit_tinfo)
{
	char start_symbol [MAX_SYMBOL_SIZE];
	char end_symbol [MAX_SYMBOL_SIZE];
	char symbol [MAX_SYMBOL_SIZE];
	guint32 buf_size, info_offset;
	MonoJumpInfo *patch_info;
	guint8 *buf, *p;
	GPtrArray *patches;
	char *name;
	guint8 *code;
	guint32 code_size;
	MonoJumpInfo *ji;
	GSList *unwind_ops;

	g_assert (info);

	name = info->name;
	code = info->code;
	code_size = info->code_size;
	ji = info->ji;
	unwind_ops = info->unwind_ops;

	/* Emit code */

	sprintf (start_symbol, "%s%s", acfg->user_symbol_prefix, name);

	emit_section_change (acfg, ".text", 0);
	emit_global (acfg, start_symbol, TRUE);
	emit_alignment_code (acfg, AOT_FUNC_ALIGNMENT);
	emit_label (acfg, start_symbol);

	sprintf (symbol, "%snamed_%s", acfg->temp_prefix, name);
	emit_label (acfg, symbol);

	/* 
	 * The code should access everything through the GOT, so we pass
	 * TRUE here.
	 */
	emit_and_reloc_code (acfg, NULL, code, code_size, ji, TRUE, NULL);

	emit_symbol_size (acfg, start_symbol, ".");

	if (emit_tinfo) {
		sprintf (end_symbol, "%snamede_%s", acfg->temp_prefix, name);
		emit_label (acfg, end_symbol);
	}

	/* Emit info */

	/* Sort relocations */
	patches = g_ptr_array_new ();
	for (patch_info = ji; patch_info; patch_info = patch_info->next)
		if (patch_info->type != MONO_PATCH_INFO_NONE)
			g_ptr_array_add (patches, patch_info);
	g_ptr_array_sort (patches, compare_patches);

	buf_size = patches->len * 128 + 128;
	buf = (guint8 *)g_malloc (buf_size);
	p = buf;

	encode_patch_list (acfg, patches, patches->len, FALSE, got_offset, p, &p);
	g_assert (p - buf < buf_size);
	g_ptr_array_free (patches, TRUE);

	sprintf (symbol, "%s%s_p", acfg->user_symbol_prefix, name);

	info_offset = add_to_blob (acfg, buf, p - buf);

	emit_section_change (acfg, RODATA_SECT, 0);
	emit_global (acfg, symbol, FALSE);
	emit_label (acfg, symbol);

	emit_int32 (acfg, info_offset);

	if (emit_tinfo) {
		guint8 *encoded;
		guint32 encoded_len;
		guint32 uw_offset;

		/*
		 * Emit additional information which can be used to reconstruct a partial MonoTrampInfo.
		 */
		encoded = mono_unwind_ops_encode (info->unwind_ops, &encoded_len);
		uw_offset = get_unwind_info_offset (acfg, encoded, encoded_len);
		g_free (encoded);

		emit_symbol_diff (acfg, end_symbol, start_symbol, 0);
		emit_int32 (acfg, uw_offset);
	}

	/* Emit debug info */
	if (unwind_ops) {
		char symbol2 [MAX_SYMBOL_SIZE];

		sprintf (symbol, "%s", name);
		sprintf (symbol2, "%snamed_%s", acfg->temp_prefix, name);

		if (acfg->dwarf)
			mono_dwarf_writer_emit_trampoline (acfg->dwarf, symbol, symbol2, NULL, NULL, code_size, unwind_ops);
	}

	g_free (buf);
}

static G_GNUC_UNUSED void
emit_trampoline (MonoAotCompile *acfg, int got_offset, MonoTrampInfo *info)
{
	emit_trampoline_full (acfg, got_offset, info, TRUE);
}

static void
emit_trampolines (MonoAotCompile *acfg)
{
	char symbol [MAX_SYMBOL_SIZE];
	char end_symbol [MAX_SYMBOL_SIZE];
	int i, tramp_got_offset;
	int ntype;
#ifdef MONO_ARCH_HAVE_FULL_AOT_TRAMPOLINES
	int tramp_type;
#endif

	if (!mono_aot_mode_is_full (&acfg->aot_opts) || acfg->aot_opts.llvm_only)
		return;
	
	g_assert (acfg->image->assembly);

	/* Currently, we emit most trampolines into the mscorlib AOT image. */
	if (strcmp (acfg->image->assembly->aname.name, "mscorlib") == 0) {
#ifdef MONO_ARCH_HAVE_FULL_AOT_TRAMPOLINES
		MonoTrampInfo *info;

		/*
		 * Emit the generic trampolines.
		 *
		 * We could save some code by treating the generic trampolines as a wrapper
		 * method, but that approach has its own complexities, so we choose the simpler
		 * method.
		 */
		for (tramp_type = 0; tramp_type < MONO_TRAMPOLINE_NUM; ++tramp_type) {
			/* we overload the boolean here to indicate the slightly different trampoline needed, see mono_arch_create_generic_trampoline() */
#ifdef DISABLE_REMOTING
			if (tramp_type == MONO_TRAMPOLINE_GENERIC_VIRTUAL_REMOTING)
				continue;
#endif
#ifndef MONO_ARCH_HAVE_HANDLER_BLOCK_GUARD
			if (tramp_type == MONO_TRAMPOLINE_HANDLER_BLOCK_GUARD)
				continue;
#endif
			mono_arch_create_generic_trampoline ((MonoTrampolineType)tramp_type, &info, acfg->aot_opts.use_trampolines_page? 2: TRUE);
			emit_trampoline (acfg, acfg->got_offset, info);
		}

		/* Emit the exception related code pieces */
		mono_arch_get_restore_context (&info, TRUE);
		emit_trampoline (acfg, acfg->got_offset, info);
		mono_arch_get_call_filter (&info, TRUE);
		emit_trampoline (acfg, acfg->got_offset, info);
		mono_arch_get_throw_exception (&info, TRUE);
		emit_trampoline (acfg, acfg->got_offset, info);
		mono_arch_get_rethrow_exception (&info, TRUE);
		emit_trampoline (acfg, acfg->got_offset, info);
		mono_arch_get_throw_corlib_exception (&info, TRUE);
		emit_trampoline (acfg, acfg->got_offset, info);

#ifdef MONO_ARCH_HAVE_SDB_TRAMPOLINES
		mono_arch_create_sdb_trampoline (TRUE, &info, TRUE);
		emit_trampoline (acfg, acfg->got_offset, info);
		mono_arch_create_sdb_trampoline (FALSE, &info, TRUE);
		emit_trampoline (acfg, acfg->got_offset, info);
#endif

#ifdef MONO_ARCH_GSHAREDVT_SUPPORTED
		mono_arch_get_gsharedvt_trampoline (&info, TRUE);
		if (info) {
			emit_trampoline_full (acfg, acfg->got_offset, info, TRUE);

			/* Create a separate out trampoline for more information in stack traces */
			info->name = g_strdup ("gsharedvt_out_trampoline");
			emit_trampoline_full (acfg, acfg->got_offset, info, TRUE);
		}
#endif

#if defined(MONO_ARCH_HAVE_GET_TRAMPOLINES)
		{
			GSList *l = mono_arch_get_trampolines (TRUE);

			while (l) {
				MonoTrampInfo *info = (MonoTrampInfo *)l->data;

				emit_trampoline (acfg, acfg->got_offset, info);
				l = l->next;
			}
		}
#endif

		for (i = 0; i < acfg->aot_opts.nrgctx_fetch_trampolines; ++i) {
			int offset;

			offset = MONO_RGCTX_SLOT_MAKE_RGCTX (i);
			mono_arch_create_rgctx_lazy_fetch_trampoline (offset, &info, TRUE);
			emit_trampoline (acfg, acfg->got_offset, info);
			g_free (info);

			offset = MONO_RGCTX_SLOT_MAKE_MRGCTX (i);
			mono_arch_create_rgctx_lazy_fetch_trampoline (offset, &info, TRUE);
			emit_trampoline (acfg, acfg->got_offset, info);
			g_free (info);
		}

#ifdef MONO_ARCH_HAVE_GENERAL_RGCTX_LAZY_FETCH_TRAMPOLINE
		mono_arch_create_general_rgctx_lazy_fetch_trampoline (&info, TRUE);
		emit_trampoline (acfg, acfg->got_offset, info);
#endif

		{
			GSList *l;

			/* delegate_invoke_impl trampolines */
			l = mono_arch_get_delegate_invoke_impls ();
			while (l) {
				MonoTrampInfo *info = (MonoTrampInfo *)l->data;

				emit_trampoline (acfg, acfg->got_offset, info);
				l = l->next;
			}
		}

#ifdef MONO_ARCH_HAVE_HANDLER_BLOCK_GUARD_AOT
		mono_arch_create_handler_block_trampoline (&info, TRUE);
		emit_trampoline (acfg, acfg->got_offset, info);
#endif

#endif /* #ifdef MONO_ARCH_HAVE_FULL_AOT_TRAMPOLINES */

		/* Emit trampolines which are numerous */

		/*
		 * These include the following:
		 * - specific trampolines
		 * - static rgctx invoke trampolines
		 * - imt trampolines
		 * These trampolines have the same code, they are parameterized by GOT 
		 * slots. 
		 * They are defined in this file, in the arch_... routines instead of
		 * in tramp-<ARCH>.c, since it is easier to do it this way.
		 */

		/*
		 * When running in aot-only mode, we can't create specific trampolines at 
		 * runtime, so we create a few, and save them in the AOT file. 
		 * Normal trampolines embed their argument as a literal inside the 
		 * trampoline code, we can't do that here, so instead we embed an offset
		 * which needs to be added to the trampoline address to get the address of
		 * the GOT slot which contains the argument value.
		 * The generated trampolines jump to the generic trampolines using another
		 * GOT slot, which will be setup by the AOT loader to point to the 
		 * generic trampoline code of the given type.
		 */

		/*
		 * FIXME: Maybe we should use more specific trampolines (i.e. one class init for
		 * each class).
		 */

		emit_section_change (acfg, ".text", 0);

		tramp_got_offset = acfg->got_offset;

		for (ntype = 0; ntype < MONO_AOT_TRAMP_NUM; ++ntype) {
			switch (ntype) {
			case MONO_AOT_TRAMP_SPECIFIC:
				sprintf (symbol, "specific_trampolines");
				break;
			case MONO_AOT_TRAMP_STATIC_RGCTX:
				sprintf (symbol, "static_rgctx_trampolines");
				break;
			case MONO_AOT_TRAMP_IMT:
				sprintf (symbol, "imt_trampolines");
				break;
			case MONO_AOT_TRAMP_GSHAREDVT_ARG:
				sprintf (symbol, "gsharedvt_arg_trampolines");
				break;
			default:
				g_assert_not_reached ();
			}

			sprintf (end_symbol, "%s_e", symbol);

			if (acfg->aot_opts.write_symbols)
				emit_local_symbol (acfg, symbol, end_symbol, TRUE);

			emit_alignment_code (acfg, AOT_FUNC_ALIGNMENT);
			emit_info_symbol (acfg, symbol);

			acfg->trampoline_got_offset_base [ntype] = tramp_got_offset;

			for (i = 0; i < acfg->num_trampolines [ntype]; ++i) {
				int tramp_size = 0;

				switch (ntype) {
				case MONO_AOT_TRAMP_SPECIFIC:
					arch_emit_specific_trampoline (acfg, tramp_got_offset, &tramp_size);
					tramp_got_offset += 2;
				break;
				case MONO_AOT_TRAMP_STATIC_RGCTX:
					arch_emit_static_rgctx_trampoline (acfg, tramp_got_offset, &tramp_size);				
					tramp_got_offset += 2;
					break;
				case MONO_AOT_TRAMP_IMT:
					arch_emit_imt_trampoline (acfg, tramp_got_offset, &tramp_size);
					tramp_got_offset += 1;
					break;
				case MONO_AOT_TRAMP_GSHAREDVT_ARG:
					arch_emit_gsharedvt_arg_trampoline (acfg, tramp_got_offset, &tramp_size);				
					tramp_got_offset += 2;
					break;
				default:
					g_assert_not_reached ();
				}
				if (!acfg->trampoline_size [ntype]) {
					g_assert (tramp_size);
					acfg->trampoline_size [ntype] = tramp_size;
				}
			}

			emit_label (acfg, end_symbol);
			emit_int32 (acfg, 0);
		}

		arch_emit_specific_trampoline_pages (acfg);

		/* Reserve some entries at the end of the GOT for our use */
		acfg->num_trampoline_got_entries = tramp_got_offset - acfg->got_offset;
	}

	acfg->got_offset += acfg->num_trampoline_got_entries;
}

static gboolean
str_begins_with (const char *str1, const char *str2)
{
	int len = strlen (str2);
	return strncmp (str1, str2, len) == 0;
}

void*
mono_aot_readonly_field_override (MonoClassField *field)
{
	ReadOnlyValue *rdv;
	for (rdv = readonly_values; rdv; rdv = rdv->next) {
		char *p = rdv->name;
		int len;
		len = strlen (field->parent->name_space);
		if (strncmp (p, field->parent->name_space, len))
			continue;
		p += len;
		if (*p++ != '.')
			continue;
		len = strlen (field->parent->name);
		if (strncmp (p, field->parent->name, len))
			continue;
		p += len;
		if (*p++ != '.')
			continue;
		if (strcmp (p, field->name))
			continue;
		switch (rdv->type) {
		case MONO_TYPE_I1:
			return &rdv->value.i1;
		case MONO_TYPE_I2:
			return &rdv->value.i2;
		case MONO_TYPE_I4:
			return &rdv->value.i4;
		default:
			break;
		}
	}
	return NULL;
}

static void
add_readonly_value (MonoAotOptions *opts, const char *val)
{
	ReadOnlyValue *rdv;
	const char *fval;
	const char *tval;
	/* the format of val is:
	 * namespace.typename.fieldname=type/value
	 * type can be i1 for uint8/int8/boolean, i2 for uint16/int16/char, i4 for uint32/int32
	 */
	fval = strrchr (val, '/');
	if (!fval) {
		fprintf (stderr, "AOT : invalid format for readonly field '%s', missing /.\n", val);
		exit (1);
	}
	tval = strrchr (val, '=');
	if (!tval) {
		fprintf (stderr, "AOT : invalid format for readonly field '%s', missing =.\n", val);
		exit (1);
	}
	rdv = g_new0 (ReadOnlyValue, 1);
	rdv->name = (char *)g_malloc0 (tval - val + 1);
	memcpy (rdv->name, val, tval - val);
	tval++;
	fval++;
	if (strncmp (tval, "i1", 2) == 0) {
		rdv->value.i1 = atoi (fval);
		rdv->type = MONO_TYPE_I1;
	} else if (strncmp (tval, "i2", 2) == 0) {
		rdv->value.i2 = atoi (fval);
		rdv->type = MONO_TYPE_I2;
	} else if (strncmp (tval, "i4", 2) == 0) {
		rdv->value.i4 = atoi (fval);
		rdv->type = MONO_TYPE_I4;
	} else {
		fprintf (stderr, "AOT : unsupported type for readonly field '%s'.\n", tval);
		exit (1);
	}
	rdv->next = readonly_values;
	readonly_values = rdv;
}

static gchar *
clean_path (gchar * path)
{
	if (!path)
		return NULL;

	if (g_str_has_suffix (path, G_DIR_SEPARATOR_S))
		return path;

	gchar *clean = g_strconcat (path, G_DIR_SEPARATOR_S, NULL);
	g_free (path);

	return clean;
}

static gchar *
wrap_path (gchar * path)
{
	int len;
	if (!path)
		return NULL;

	// If the string contains no spaces, just return the original string.
	if (strstr (path, " ") == NULL)
		return path;

	// If the string is already wrapped in quotes, return it.
	len = strlen (path);
	if (len >= 2 && path[0] == '\"' && path[len-1] == '\"')
		return path;

	// If the string contains spaces, then wrap it in quotes.
	gchar *clean = g_strdup_printf ("\"%s\"", path);

	return clean;
}

// Duplicate a char range and add it to a ptrarray, but only if it is nonempty
static void
ptr_array_add_range_if_nonempty(GPtrArray *args, gchar const *start, gchar const *end)
{
	ptrdiff_t len = end-start;
	if (len > 0)
		g_ptr_array_add (args, g_strndup (start, len));
}

static GPtrArray *
mono_aot_split_options (const char *aot_options)
{
	enum MonoAotOptionState {
		MONO_AOT_OPTION_STATE_DEFAULT,
		MONO_AOT_OPTION_STATE_STRING,
		MONO_AOT_OPTION_STATE_ESCAPE,
	};

	GPtrArray *args = g_ptr_array_new ();
	enum MonoAotOptionState state = MONO_AOT_OPTION_STATE_DEFAULT;
	gchar const *opt_start = aot_options;
	gboolean end_of_string = FALSE;
	gchar cur;

	g_return_val_if_fail (aot_options != NULL, NULL);

	while ((cur = *aot_options) != '\0') {
		if (state == MONO_AOT_OPTION_STATE_ESCAPE)
			goto next;

		switch (cur) {
		case '"':
			// If we find a quote, then if we're in the default case then
			// it means we've found the start of a string, if not then it
			// means we've found the end of the string and should switch
			// back to the default case.		
			switch (state) {
			case MONO_AOT_OPTION_STATE_DEFAULT:
				state = MONO_AOT_OPTION_STATE_STRING;
				break;
			case MONO_AOT_OPTION_STATE_STRING:
				state = MONO_AOT_OPTION_STATE_DEFAULT;
				break;
			case MONO_AOT_OPTION_STATE_ESCAPE:
				g_assert_not_reached ();
				break;
			}
			break;
		case '\\':
			// If we've found an escaping operator, then this means we
			// should not process the next character if inside a string.		
			if (state == MONO_AOT_OPTION_STATE_STRING) 
				state = MONO_AOT_OPTION_STATE_ESCAPE;
			break;
		case ',':
			// If we're in the default state then this means we've found
			// an option, store it for later processing.
			if (state == MONO_AOT_OPTION_STATE_DEFAULT)
				goto new_opt;
			break;
		}

	next:
		aot_options++;
	restart:
		// If the next character is end of string, then process the last option.
		if (*(aot_options) == '\0') {
			end_of_string = TRUE;
			goto new_opt;
		}
		continue;

	new_opt:
		ptr_array_add_range_if_nonempty (args, opt_start, aot_options);
		opt_start = ++aot_options;
		if (end_of_string)
			break;
		goto restart; // Check for null and continue loop
	}

	return args;
}

static void
mono_aot_parse_options (const char *aot_options, MonoAotOptions *opts)
{
	GPtrArray* args;

	args = mono_aot_split_options (aot_options ? aot_options : "");
	for (int i = 0; i < args->len; ++i) {
		const char *arg = (const char *)g_ptr_array_index (args, i);

		if (str_begins_with (arg, "outfile=")) {
			opts->outfile = g_strdup (arg + strlen ("outfile="));
		} else if (str_begins_with (arg, "llvm-outfile=")) {
			opts->llvm_outfile = g_strdup (arg + strlen ("llvm-outfile="));
		} else if (str_begins_with (arg, "temp-path=")) {
			opts->temp_path = clean_path (g_strdup (arg + strlen ("temp-path=")));
		} else if (str_begins_with (arg, "save-temps")) {
			opts->save_temps = TRUE;
		} else if (str_begins_with (arg, "keep-temps")) {
			opts->save_temps = TRUE;
		} else if (str_begins_with (arg, "write-symbols")) {
			opts->write_symbols = TRUE;
		} else if (str_begins_with (arg, "no-write-symbols")) {
			opts->write_symbols = FALSE;
		} else if (str_begins_with (arg, "metadata-only")) {
			opts->metadata_only = TRUE;
		} else if (str_begins_with (arg, "bind-to-runtime-version")) {
			opts->bind_to_runtime_version = TRUE;
		} else if (str_begins_with (arg, "full")) {
			opts->mode = MONO_AOT_MODE_FULL;
		} else if (str_begins_with (arg, "hybrid")) {
			opts->mode = MONO_AOT_MODE_HYBRID;			
		} else if (str_begins_with (arg, "threads=")) {
			opts->nthreads = atoi (arg + strlen ("threads="));
		} else if (str_begins_with (arg, "static")) {
			opts->static_link = TRUE;
			opts->no_dlsym = TRUE;
		} else if (str_begins_with (arg, "asmonly")) {
			opts->asm_only = TRUE;
		} else if (str_begins_with (arg, "asmwriter")) {
			opts->asm_writer = TRUE;
		} else if (str_begins_with (arg, "nodebug")) {
			opts->nodebug = TRUE;
		} else if (str_begins_with (arg, "dwarfdebug")) {
			opts->dwarf_debug = TRUE;
		} else if (str_begins_with (arg, "nopagetrampolines")) {
			opts->use_trampolines_page = FALSE;
		} else if (str_begins_with (arg, "ntrampolines=")) {
			opts->ntrampolines = atoi (arg + strlen ("ntrampolines="));
		} else if (str_begins_with (arg, "nrgctx-trampolines=")) {
			opts->nrgctx_trampolines = atoi (arg + strlen ("nrgctx-trampolines="));
		} else if (str_begins_with (arg, "nrgctx-fetch-trampolines=")) {
			opts->nrgctx_fetch_trampolines = atoi (arg + strlen ("nrgctx-fetch-trampolines="));
		} else if (str_begins_with (arg, "nimt-trampolines=")) {
			opts->nimt_trampolines = atoi (arg + strlen ("nimt-trampolines="));
		} else if (str_begins_with (arg, "ngsharedvt-trampolines=")) {
			opts->ngsharedvt_arg_trampolines = atoi (arg + strlen ("ngsharedvt-trampolines="));
		} else if (str_begins_with (arg, "tool-prefix=")) {
			opts->tool_prefix = g_strdup (arg + strlen ("tool-prefix="));
		} else if (str_begins_with (arg, "ld-flags=")) {
			opts->ld_flags = g_strdup (arg + strlen ("ld-flags="));			
		} else if (str_begins_with (arg, "soft-debug")) {
			opts->soft_debug = TRUE;
		} else if (str_begins_with (arg, "gen-seq-points-file=")) {
			fprintf (stderr, "Mono Warning: aot option gen-seq-points-file= is deprecated.\n");
		} else if (str_begins_with (arg, "gen-seq-points-file")) {
			fprintf (stderr, "Mono Warning: aot option gen-seq-points-file is deprecated.\n");
		} else if (str_begins_with (arg, "msym-dir=")) {
			debug_options.no_seq_points_compact_data = FALSE;
			opts->gen_msym_dir = TRUE;
			opts->gen_msym_dir_path = g_strdup (arg + strlen ("msym_dir="));;
		} else if (str_begins_with (arg, "direct-pinvoke")) {
			opts->direct_pinvoke = TRUE;
		} else if (str_begins_with (arg, "direct-icalls")) {
			opts->direct_icalls = TRUE;
		} else if (str_begins_with (arg, "no-direct-calls")) {
			opts->no_direct_calls = TRUE;
		} else if (str_begins_with (arg, "print-skipped")) {
			opts->print_skipped_methods = TRUE;
		} else if (str_begins_with (arg, "stats")) {
			opts->stats = TRUE;
		} else if (str_begins_with (arg, "no-instances")) {
			opts->no_instances = TRUE;
		} else if (str_begins_with (arg, "log-generics")) {
			opts->log_generics = TRUE;
		} else if (str_begins_with (arg, "log-instances=")) {
			opts->log_instances = TRUE;
			opts->instances_logfile_path = g_strdup (arg + strlen ("log-instances="));
		} else if (str_begins_with (arg, "log-instances")) {
			opts->log_instances = TRUE;
		} else if (str_begins_with (arg, "internal-logfile=")) {
			opts->logfile = g_strdup (arg + strlen ("internal-logfile="));
		} else if (str_begins_with (arg, "mtriple=")) {
			opts->mtriple = g_strdup (arg + strlen ("mtriple="));
		} else if (str_begins_with (arg, "llvm-path=")) {
			opts->llvm_path = clean_path (g_strdup (arg + strlen ("llvm-path=")));
		} else if (!strcmp (arg, "llvm")) {
			opts->llvm = TRUE;
		} else if (str_begins_with (arg, "readonly-value=")) {
			add_readonly_value (opts, arg + strlen ("readonly-value="));
		} else if (str_begins_with (arg, "info")) {
			printf ("AOT target setup: %s.\n", AOT_TARGET_STR);
			exit (0);
		} else if (str_begins_with (arg, "gc-maps")) {
			mini_gc_enable_gc_maps_for_aot ();
		} else if (str_begins_with (arg, "dump")) {
			opts->dump_json = TRUE;
		} else if (str_begins_with (arg, "llvmonly")) {
			opts->mode = MONO_AOT_MODE_FULL;
			opts->llvm = TRUE;
			opts->llvm_only = TRUE;
		} else if (str_begins_with (arg, "data-outfile=")) {
			opts->data_outfile = g_strdup (arg + strlen ("data-outfile="));
		} else if (str_begins_with (arg, "profile=")) {
			opts->profile_files = g_list_append (opts->profile_files, g_strdup (arg + strlen ("profile=")));
		} else if (!strcmp (arg, "profile-only")) {
			opts->profile_only = TRUE;
		} else if (!strcmp (arg, "verbose")) {
			opts->verbose = TRUE;
		} else if (str_begins_with (arg, "help") || str_begins_with (arg, "?")) {
			printf ("Supported options for --aot:\n");
			printf ("    outfile=\n");
			printf ("    llvm-outfile=\n");
			printf ("    llvm-path=\n");
			printf ("    temp-path=\n");
			printf ("    save-temps\n");
			printf ("    keep-temps\n");
			printf ("    write-symbols\n");
			printf ("    metadata-only\n");
			printf ("    bind-to-runtime-version\n");
			printf ("    full\n");
			printf ("    threads=\n");
			printf ("    static\n");
			printf ("    asmonly\n");
			printf ("    asmwriter\n");
			printf ("    nodebug\n");
			printf ("    dwarfdebug\n");
			printf ("    ntrampolines=\n");
			printf ("    nrgctx-trampolines=\n");
			printf ("    nimt-trampolines=\n");
			printf ("    ngsharedvt-trampolines=\n");
			printf ("    tool-prefix=\n");
			printf ("    readonly-value=\n");
			printf ("    soft-debug\n");
			printf ("    msym-dir=\n");
			printf ("    gc-maps\n");
			printf ("    print-skipped\n");
			printf ("    no-instances\n");
			printf ("    stats\n");
			printf ("    dump\n");
			printf ("    info\n");
			printf ("    verbose\n");
			printf ("    help/?\n");
			exit (0);
		} else {
			fprintf (stderr, "AOT : Unknown argument '%s'.\n", arg);
			exit (1);
		}

		g_free ((gpointer) arg);
	}

	if (opts->use_trampolines_page) {
		opts->ntrampolines = 0;
		opts->nrgctx_trampolines = 0;
		opts->nimt_trampolines = 0;
		opts->ngsharedvt_arg_trampolines = 0;
	}

	g_ptr_array_free (args, /*free_seg=*/TRUE);
}

static void
add_token_info_hash (gpointer key, gpointer value, gpointer user_data)
{
	MonoMethod *method = (MonoMethod*)key;
	MonoJumpInfoToken *ji = (MonoJumpInfoToken*)value;
	MonoAotCompile *acfg = (MonoAotCompile *)user_data;
	MonoJumpInfoToken *new_ji;

	new_ji = (MonoJumpInfoToken *)mono_mempool_alloc0 (acfg->mempool, sizeof (MonoJumpInfoToken));
	new_ji->image = ji->image;
	new_ji->token = ji->token;
	g_hash_table_insert (acfg->token_info_hash, method, new_ji);
}

static gboolean
can_encode_class (MonoAotCompile *acfg, MonoClass *klass)
{
	if (klass->type_token)
		return TRUE;
	if ((klass->byval_arg.type == MONO_TYPE_VAR) || (klass->byval_arg.type == MONO_TYPE_MVAR) || (klass->byval_arg.type == MONO_TYPE_PTR))
		return TRUE;
	if (klass->rank)
		return can_encode_class (acfg, klass->element_class);
	return FALSE;
}

static gboolean
can_encode_method (MonoAotCompile *acfg, MonoMethod *method)
{
		if (method->wrapper_type) {
			switch (method->wrapper_type) {
			case MONO_WRAPPER_NONE:
			case MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK:
			case MONO_WRAPPER_XDOMAIN_INVOKE:
			case MONO_WRAPPER_STFLD:
			case MONO_WRAPPER_LDFLD:
			case MONO_WRAPPER_LDFLDA:
			case MONO_WRAPPER_STELEMREF:
			case MONO_WRAPPER_PROXY_ISINST:
			case MONO_WRAPPER_ALLOC:
			case MONO_WRAPPER_REMOTING_INVOKE:
			case MONO_WRAPPER_UNKNOWN:
			case MONO_WRAPPER_WRITE_BARRIER:
			case MONO_WRAPPER_DELEGATE_INVOKE:
			case MONO_WRAPPER_DELEGATE_BEGIN_INVOKE:
			case MONO_WRAPPER_DELEGATE_END_INVOKE:
			case MONO_WRAPPER_SYNCHRONIZED:
				break;
			case MONO_WRAPPER_MANAGED_TO_MANAGED:
			case MONO_WRAPPER_CASTCLASS: {
				WrapperInfo *info = mono_marshal_get_wrapper_info (method);

				if (info)
					return TRUE;
				else
					return FALSE;
				break;
			}
			default:
				//printf ("Skip (wrapper call): %d -> %s\n", patch_info->type, mono_method_full_name (patch_info->data.method, TRUE));
				return FALSE;
			}
		} else {
			if (!method->token) {
				/* The method is part of a constructed type like Int[,].Set (). */
				if (!g_hash_table_lookup (acfg->token_info_hash, method)) {
					if (method->klass->rank)
						return TRUE;
					return FALSE;
				}
			}
		}
		return TRUE;
}

static gboolean
can_encode_patch (MonoAotCompile *acfg, MonoJumpInfo *patch_info)
{
	switch (patch_info->type) {
	case MONO_PATCH_INFO_METHOD:
	case MONO_PATCH_INFO_METHODCONST:
	case MONO_PATCH_INFO_METHOD_CODE_SLOT: {
		MonoMethod *method = patch_info->data.method;

		return can_encode_method (acfg, method);
	}
	case MONO_PATCH_INFO_VTABLE:
	case MONO_PATCH_INFO_CLASS:
	case MONO_PATCH_INFO_IID:
	case MONO_PATCH_INFO_ADJUSTED_IID:
		if (!can_encode_class (acfg, patch_info->data.klass)) {
			//printf ("Skip: %s\n", mono_type_full_name (&patch_info->data.klass->byval_arg));
			return FALSE;
		}
		break;
	case MONO_PATCH_INFO_DELEGATE_TRAMPOLINE: {
		if (!can_encode_class (acfg, patch_info->data.del_tramp->klass)) {
			//printf ("Skip: %s\n", mono_type_full_name (&patch_info->data.klass->byval_arg));
			return FALSE;
		}
		break;
	}
	case MONO_PATCH_INFO_RGCTX_FETCH:
	case MONO_PATCH_INFO_RGCTX_SLOT_INDEX: {
		MonoJumpInfoRgctxEntry *entry = patch_info->data.rgctx_entry;

		if (!can_encode_method (acfg, entry->method))
			return FALSE;
		if (!can_encode_patch (acfg, entry->data))
			return FALSE;
		break;
	}
	default:
		break;
	}

	return TRUE;
}

static gboolean
is_concrete_type (MonoType *t)
{
	MonoClass *klass;
	int i;

	if (t->type == MONO_TYPE_VAR || t->type == MONO_TYPE_MVAR)
		return FALSE;
	if (t->type == MONO_TYPE_GENERICINST) {
		MonoGenericContext *orig_ctx;
		MonoGenericInst *inst;
		MonoType *arg;

		if (!MONO_TYPE_ISSTRUCT (t))
			return TRUE;
		klass = mono_class_from_mono_type (t);
		orig_ctx = &mono_class_get_generic_class (klass)->context;

		inst = orig_ctx->class_inst;
		if (inst) {
			for (i = 0; i < inst->type_argc; ++i) {
				arg = mini_get_underlying_type (inst->type_argv [i]);
				if (!is_concrete_type (arg))
					return FALSE;
			}
		}
		inst = orig_ctx->method_inst;
		if (inst) {
			for (i = 0; i < inst->type_argc; ++i) {
				arg = mini_get_underlying_type (inst->type_argv [i]);
				if (!is_concrete_type (arg))
					return FALSE;
			}
		}
	}
	return TRUE;
}

/* LOCKING: Assumes the loader lock is held */
static void
add_gsharedvt_wrappers (MonoAotCompile *acfg, MonoMethodSignature *sig, gboolean gsharedvt_in, gboolean gsharedvt_out)
{
	MonoMethod *wrapper;
	gboolean concrete = TRUE;
	gboolean add_in = gsharedvt_in;
	gboolean add_out = gsharedvt_out;

	if (gsharedvt_in && g_hash_table_lookup (acfg->gsharedvt_in_signatures, sig))
		add_in = FALSE;
	if (gsharedvt_out && g_hash_table_lookup (acfg->gsharedvt_out_signatures, sig))
		add_out = FALSE;

	if (!add_in && !add_out)
		return;

	if (mini_is_gsharedvt_variable_signature (sig))
		return;

	if (add_in)
		g_hash_table_insert (acfg->gsharedvt_in_signatures, sig, sig);
	if (add_out)
		g_hash_table_insert (acfg->gsharedvt_out_signatures, sig, sig);

	if (!sig->has_type_parameters) {
		//printf ("%s\n", mono_signature_full_name (sig));

		if (gsharedvt_in) {
			wrapper = mini_get_gsharedvt_in_sig_wrapper (sig);
			add_extra_method (acfg, wrapper);
		}
		if (gsharedvt_out) {
			wrapper = mini_get_gsharedvt_out_sig_wrapper (sig);
			add_extra_method (acfg, wrapper);
		}
	} else {
		/* For signatures creared during generic sharing, convert them to a concrete signature if possible */
		MonoMethodSignature *copy = mono_metadata_signature_dup (sig);
		int i;

		//printf ("%s\n", mono_signature_full_name (sig));

		copy->ret = mini_get_underlying_type (sig->ret);
		if (!is_concrete_type (copy->ret))
			concrete = FALSE;
		for (i = 0; i < sig->param_count; ++i) {
			copy->params [i] = mini_get_underlying_type (sig->params [i]);
			if (!is_concrete_type (copy->params [i]))
				concrete = FALSE;
		}
		if (concrete) {
			copy->has_type_parameters = 0;

			if (gsharedvt_in) {
				wrapper = mini_get_gsharedvt_in_sig_wrapper (copy);
				add_extra_method (acfg, wrapper);
			}

			if (gsharedvt_out) {
				wrapper = mini_get_gsharedvt_out_sig_wrapper (copy);
				add_extra_method (acfg, wrapper);
			}

			//printf ("%s\n", mono_method_full_name (wrapper, 1));
		}
	}
}

/*
 * compile_method:
 *
 *   AOT compile a given method.
 * This function might be called by multiple threads, so it must be thread-safe.
 */
static void
compile_method (MonoAotCompile *acfg, MonoMethod *method)
{
	MonoCompile *cfg;
	MonoJumpInfo *patch_info;
	gboolean skip;
	int index, depth;
	MonoMethod *wrapped;
	GTimer *jit_timer;
	JitFlags flags;

	if (acfg->aot_opts.metadata_only)
		return;

	mono_acfg_lock (acfg);
	index = get_method_index (acfg, method);
	mono_acfg_unlock (acfg);

	/* fixme: maybe we can also precompile wrapper methods */
	if ((method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
		(method->iflags & METHOD_IMPL_ATTRIBUTE_RUNTIME) ||
		(method->flags & METHOD_ATTRIBUTE_ABSTRACT)) {
		//printf ("Skip (impossible): %s\n", mono_method_full_name (method, TRUE));
		return;
	}

	if (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
		return;

	wrapped = mono_marshal_method_from_wrapper (method);
	if (wrapped && (wrapped->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) && wrapped->is_generic)
		// FIXME: The wrapper should be generic too, but it is not
		return;

	if (method->wrapper_type == MONO_WRAPPER_COMINTEROP)
		return;

	if (acfg->aot_opts.profile_only && !method->is_inflated && !g_hash_table_lookup (acfg->profile_methods, method))
		return;

	InterlockedIncrement (&acfg->stats.mcount);

#if 0
	if (method->is_generic || mono_class_is_gtd (method->klass)) {
		InterlockedIncrement (&acfg->stats.genericcount);
		return;
	}
#endif

	//acfg->aot_opts.print_skipped_methods = TRUE;

	/*
	 * Since these methods are the only ones which are compiled with
	 * AOT support, and they are not used by runtime startup/shutdown code,
	 * the runtime will not see AOT methods during AOT compilation,so it
	 * does not need to support them by creating a fake GOT etc.
	 */
	flags = JIT_FLAG_AOT;
	if (mono_aot_mode_is_full (&acfg->aot_opts))
		flags = (JitFlags)(flags | JIT_FLAG_FULL_AOT);
	if (acfg->llvm)
		flags = (JitFlags)(flags | JIT_FLAG_LLVM);
	if (acfg->aot_opts.llvm_only)
		flags = (JitFlags)(flags | JIT_FLAG_LLVM_ONLY | JIT_FLAG_EXPLICIT_NULL_CHECKS);
	if (acfg->aot_opts.no_direct_calls)
		flags = (JitFlags)(flags | JIT_FLAG_NO_DIRECT_ICALLS);
	if (acfg->aot_opts.direct_pinvoke)
		flags = (JitFlags)(flags | JIT_FLAG_DIRECT_PINVOKE);

	jit_timer = mono_time_track_start ();
	cfg = mini_method_compile (method, acfg->opts, mono_get_root_domain (), flags, 0, index);
	mono_time_track_end (&mono_jit_stats.jit_time, jit_timer);

	if (cfg->exception_type == MONO_EXCEPTION_GENERIC_SHARING_FAILED) {
		if (acfg->aot_opts.print_skipped_methods)
			printf ("Skip (gshared failure): %s (%s)\n", mono_method_get_full_name (method), cfg->exception_message);
		InterlockedIncrement (&acfg->stats.genericcount);
		return;
	}
	if (cfg->exception_type != MONO_EXCEPTION_NONE) {
		if (acfg->aot_opts.print_skipped_methods)
			printf ("Skip (JIT failure): %s\n", mono_method_get_full_name (method));
		/* Let the exception happen at runtime */
		return;
	}

	if (cfg->disable_aot) {
		if (acfg->aot_opts.print_skipped_methods)
			printf ("Skip (disabled): %s\n", mono_method_get_full_name (method));
		InterlockedIncrement (&acfg->stats.ocount);
		return;
	}
	cfg->method_index = index;

	/* Nullify patches which need no aot processing */
	for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
		switch (patch_info->type) {
		case MONO_PATCH_INFO_LABEL:
		case MONO_PATCH_INFO_BB:
			patch_info->type = MONO_PATCH_INFO_NONE;
			break;
		default:
			break;
		}
	}

	/* Collect method->token associations from the cfg */
	mono_acfg_lock (acfg);
	g_hash_table_foreach (cfg->token_info_hash, add_token_info_hash, acfg);
	mono_acfg_unlock (acfg);
	g_hash_table_destroy (cfg->token_info_hash);
	cfg->token_info_hash = NULL;

	/*
	 * Check for absolute addresses.
	 */
	skip = FALSE;
	for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
		switch (patch_info->type) {
		case MONO_PATCH_INFO_ABS:
			/* unable to handle this */
			skip = TRUE;	
			break;
		default:
			break;
		}
	}

	if (skip) {
		if (acfg->aot_opts.print_skipped_methods)
			printf ("Skip (abs call): %s\n", mono_method_get_full_name (method));
		InterlockedIncrement (&acfg->stats.abscount);
		return;
	}

	/* Lock for the rest of the code */
	mono_acfg_lock (acfg);

	if (cfg->gsharedvt)
		acfg->stats.method_categories [METHOD_CAT_GSHAREDVT] ++;
	else if (cfg->gshared)
		acfg->stats.method_categories [METHOD_CAT_INST] ++;
	else if (cfg->method->wrapper_type)
		acfg->stats.method_categories [METHOD_CAT_WRAPPER] ++;
	else
		acfg->stats.method_categories [METHOD_CAT_NORMAL] ++;

	/*
	 * Check for methods/klasses we can't encode.
	 */
	skip = FALSE;
	for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
		if (!can_encode_patch (acfg, patch_info))
			skip = TRUE;
	}

	if (skip) {
		if (acfg->aot_opts.print_skipped_methods)
			printf ("Skip (patches): %s\n", mono_method_get_full_name (method));
		acfg->stats.ocount++;
		mono_acfg_unlock (acfg);
		return;
	}

	if (!cfg->compile_llvm)
		acfg->has_jitted_code = TRUE;

	if (method->is_inflated && acfg->aot_opts.log_instances) {
		if (acfg->instances_logfile)
			fprintf (acfg->instances_logfile, "%s ### %d\n", mono_method_get_full_name (method), cfg->code_size);
		else
			printf ("%s ### %d\n", mono_method_get_full_name (method), cfg->code_size);
	}

	/* Adds generic instances referenced by this method */
	/* 
	 * The depth is used to avoid infinite loops when generic virtual recursion is 
	 * encountered.
	 */
	depth = GPOINTER_TO_UINT (g_hash_table_lookup (acfg->method_depth, method));
	if (!acfg->aot_opts.no_instances && depth < 32 && mono_aot_mode_is_full (&acfg->aot_opts)) {
		for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
			switch (patch_info->type) {
			case MONO_PATCH_INFO_RGCTX_FETCH:
			case MONO_PATCH_INFO_RGCTX_SLOT_INDEX:
			case MONO_PATCH_INFO_METHOD: {
				MonoMethod *m = NULL;

				if (patch_info->type == MONO_PATCH_INFO_RGCTX_FETCH || patch_info->type == MONO_PATCH_INFO_RGCTX_SLOT_INDEX) {
					MonoJumpInfoRgctxEntry *e = patch_info->data.rgctx_entry;

					if (e->info_type == MONO_RGCTX_INFO_GENERIC_METHOD_CODE)
						m = e->data->data.method;
				} else {
					m = patch_info->data.method;
				}

				if (!m)
					break;
				if (m->is_inflated && mono_aot_mode_is_full (&acfg->aot_opts)) {
					if (!(mono_class_generic_sharing_enabled (m->klass) &&
						  mono_method_is_generic_sharable_full (m, FALSE, FALSE, FALSE)) &&
						(!method_has_type_vars (m) || mono_method_is_generic_sharable_full (m, TRUE, TRUE, FALSE))) {
						if (m->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) {
							if (mono_aot_mode_is_full (&acfg->aot_opts) && !method_has_type_vars (m))
								add_extra_method_with_depth (acfg, mono_marshal_get_native_wrapper (m, TRUE, TRUE), depth + 1);
						} else {
							add_extra_method_with_depth (acfg, m, depth + 1);
							add_types_from_method_header (acfg, m);
						}
					}
					add_generic_class_with_depth (acfg, m->klass, depth + 5, "method");
				}
				if (m->wrapper_type == MONO_WRAPPER_MANAGED_TO_MANAGED) {
					WrapperInfo *info = mono_marshal_get_wrapper_info (m);

					if (info && info->subtype == WRAPPER_SUBTYPE_ELEMENT_ADDR)
						add_extra_method_with_depth (acfg, m, depth + 1);
				}
				break;
			}
			case MONO_PATCH_INFO_VTABLE: {
				MonoClass *klass = patch_info->data.klass;

				if (mono_class_is_ginst (klass) && !mini_class_is_generic_sharable (klass))
					add_generic_class_with_depth (acfg, klass, depth + 5, "vtable");
				break;
			}
			case MONO_PATCH_INFO_SFLDA: {
				MonoClass *klass = patch_info->data.field->parent;

				/* The .cctor needs to run at runtime. */
				if (mono_class_is_ginst (klass) && !mono_generic_context_is_sharable_full (&mono_class_get_generic_class (klass)->context, FALSE, FALSE) && mono_class_get_cctor (klass))
					add_extra_method_with_depth (acfg, mono_class_get_cctor (klass), depth + 1);
				break;
			}
			default:
				break;
			}
		}
	}

	/* Determine whenever the method has GOT slots */
	for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
		switch (patch_info->type) {
		case MONO_PATCH_INFO_GOT_OFFSET:
		case MONO_PATCH_INFO_NONE:
		case MONO_PATCH_INFO_GC_CARD_TABLE_ADDR:
		case MONO_PATCH_INFO_GC_NURSERY_START:
		case MONO_PATCH_INFO_GC_NURSERY_BITS:
			break;
		case MONO_PATCH_INFO_IMAGE:
			/* The assembly is stored in GOT slot 0 */
			if (patch_info->data.image != acfg->image)
				cfg->has_got_slots = TRUE;
			break;
		default:
			if (!is_plt_patch (patch_info) || (cfg->compile_llvm && acfg->aot_opts.llvm_only))
				cfg->has_got_slots = TRUE;
			break;
		}
	}

	if (!cfg->has_got_slots)
		InterlockedIncrement (&acfg->stats.methods_without_got_slots);

	/* Add gsharedvt wrappers for signatures used by the method */
	if (acfg->aot_opts.llvm_only) {
		GSList *l;

		if (!cfg->method->wrapper_type || cfg->method->wrapper_type == MONO_WRAPPER_DELEGATE_INVOKE)
			/* These only need out wrappers */
			add_gsharedvt_wrappers (acfg, mono_method_signature (cfg->method), FALSE, TRUE);

		for (l = cfg->signatures; l; l = l->next) {
			MonoMethodSignature *sig = mono_metadata_signature_dup ((MonoMethodSignature*)l->data);

			/* These only need in wrappers */
			add_gsharedvt_wrappers (acfg, sig, TRUE, FALSE);
		}
	}

	/* 
	 * FIXME: Instead of this mess, allocate the patches from the aot mempool.
	 */
	/* Make a copy of the patch info which is in the mempool */
	{
		MonoJumpInfo *patches = NULL, *patches_end = NULL;

		for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
			MonoJumpInfo *new_patch_info = mono_patch_info_dup_mp (acfg->mempool, patch_info);

			if (!patches)
				patches = new_patch_info;
			else
				patches_end->next = new_patch_info;
			patches_end = new_patch_info;
		}
		cfg->patch_info = patches;
	}
	/* Make a copy of the unwind info */
	{
		GSList *l, *unwind_ops;
		MonoUnwindOp *op;

		unwind_ops = NULL;
		for (l = cfg->unwind_ops; l; l = l->next) {
			op = (MonoUnwindOp *)mono_mempool_alloc (acfg->mempool, sizeof (MonoUnwindOp));
			memcpy (op, l->data, sizeof (MonoUnwindOp));
			unwind_ops = g_slist_prepend_mempool (acfg->mempool, unwind_ops, op);
		}
		cfg->unwind_ops = g_slist_reverse (unwind_ops);
	}
	/* Make a copy of the argument/local info */
	{
		MonoError error;
		MonoInst **args, **locals;
		MonoMethodSignature *sig;
		MonoMethodHeader *header;
		int i;
		
		sig = mono_method_signature (method);
		args = (MonoInst **)mono_mempool_alloc (acfg->mempool, sizeof (MonoInst*) * (sig->param_count + sig->hasthis));
		for (i = 0; i < sig->param_count + sig->hasthis; ++i) {
			args [i] = (MonoInst *)mono_mempool_alloc (acfg->mempool, sizeof (MonoInst));
			memcpy (args [i], cfg->args [i], sizeof (MonoInst));
		}
		cfg->args = args;

		header = mono_method_get_header_checked (method, &error);
		mono_error_assert_ok (&error); /* FIXME don't swallow the error */
		locals = (MonoInst **)mono_mempool_alloc (acfg->mempool, sizeof (MonoInst*) * header->num_locals);
		for (i = 0; i < header->num_locals; ++i) {
			locals [i] = (MonoInst *)mono_mempool_alloc (acfg->mempool, sizeof (MonoInst));
			memcpy (locals [i], cfg->locals [i], sizeof (MonoInst));
		}
		mono_metadata_free_mh (header);
		cfg->locals = locals;
	}

	/* Free some fields used by cfg to conserve memory */
	mono_empty_compile (cfg);

	//printf ("Compile:           %s\n", mono_method_full_name (method, TRUE));

	while (index >= acfg->cfgs_size) {
		MonoCompile **new_cfgs;
		int new_size;

		new_size = acfg->cfgs_size * 2;
		new_cfgs = g_new0 (MonoCompile*, new_size);
		memcpy (new_cfgs, acfg->cfgs, sizeof (MonoCompile*) * acfg->cfgs_size);
		g_free (acfg->cfgs);
		acfg->cfgs = new_cfgs;
		acfg->cfgs_size = new_size;
	}
	acfg->cfgs [index] = cfg;

	g_hash_table_insert (acfg->method_to_cfg, cfg->orig_method, cfg);

	mono_update_jit_stats (cfg);

	/*
	if (cfg->orig_method->wrapper_type)
		g_ptr_array_add (acfg->extra_methods, cfg->orig_method);
	*/

	mono_acfg_unlock (acfg);

	InterlockedIncrement (&acfg->stats.ccount);
}
 
static mono_thread_start_return_t WINAPI
compile_thread_main (gpointer user_data)
{
	MonoDomain *domain = ((MonoDomain **)user_data) [0];
	MonoAotCompile *acfg = ((MonoAotCompile **)user_data) [1];
	GPtrArray *methods = ((GPtrArray **)user_data) [2];
	int i;

	MonoError error;
	MonoThread *thread = mono_thread_attach (domain);
	mono_thread_set_name_internal (thread->internal_thread, mono_string_new (mono_get_root_domain (), "AOT compiler"), TRUE, &error);
	mono_error_assert_ok (&error);

	for (i = 0; i < methods->len; ++i)
		compile_method (acfg, (MonoMethod *)g_ptr_array_index (methods, i));

	return 0;
}
 
/* Used by the LLVM backend */
guint32
mono_aot_get_got_offset (MonoJumpInfo *ji)
{
	return get_got_offset (llvm_acfg, TRUE, ji);
}

/*
 * mono_aot_is_shared_got_offset:
 *
 *   Return whenever OFFSET refers to a GOT slot which is preinitialized
 * when the AOT image is loaded.
 */
gboolean
mono_aot_is_shared_got_offset (int offset)
{
	return offset < llvm_acfg->nshared_got_entries;
}

char*
mono_aot_get_method_name (MonoCompile *cfg)
{
	if (llvm_acfg->aot_opts.static_link)
		/* Include the assembly name too to avoid duplicate symbol errors */
		return g_strdup_printf ("%s_%s", llvm_acfg->assembly_name_sym, get_debug_sym (cfg->orig_method, "", llvm_acfg->method_label_hash));
	else
		return get_debug_sym (cfg->orig_method, "", llvm_acfg->method_label_hash);
}

/*
 * mono_aot_is_linkonce_method:
 *
 *   Return whenever METHOD should be emitted with linkonce linkage,
 * eliminating duplicate copies when compiling in static mode.
 */
gboolean
mono_aot_is_linkonce_method (MonoMethod *method)
{
	return FALSE;
#if 0
	WrapperInfo *info;

	// FIXME: Add more cases
	if (method->wrapper_type != MONO_WRAPPER_UNKNOWN)
		return FALSE;
	info = mono_marshal_get_wrapper_info (method);
	if ((info && (info->subtype == WRAPPER_SUBTYPE_GSHAREDVT_IN_SIG || info->subtype == WRAPPER_SUBTYPE_GSHAREDVT_OUT_SIG)))
		return TRUE;
	return FALSE;
#endif
}

static gboolean
append_mangled_type (GString *s, MonoType *t)
{
	if (t->byref)
		g_string_append_printf (s, "b");
	switch (t->type) {
	case MONO_TYPE_VOID:
		g_string_append_printf (s, "void_");
		break;
	case MONO_TYPE_I1:
		g_string_append_printf (s, "i1");
		break;
	case MONO_TYPE_U1:
		g_string_append_printf (s, "u1");
		break;
	case MONO_TYPE_I2:
		g_string_append_printf (s, "i2");
		break;
	case MONO_TYPE_U2:
		g_string_append_printf (s, "u2");
		break;
	case MONO_TYPE_I4:
		g_string_append_printf (s, "i4");
		break;
	case MONO_TYPE_U4:
		g_string_append_printf (s, "u4");
		break;
	case MONO_TYPE_I8:
		g_string_append_printf (s, "i8");
		break;
	case MONO_TYPE_U8:
		g_string_append_printf (s, "u8");
		break;
	case MONO_TYPE_I:
		g_string_append_printf (s, "ii");
		break;
	case MONO_TYPE_U:
		g_string_append_printf (s, "ui");
		break;
	case MONO_TYPE_R4:
		g_string_append_printf (s, "fl");
		break;
	case MONO_TYPE_R8:
		g_string_append_printf (s, "do");
		break;
	default: {
		char *fullname = mono_type_full_name (t);
		GString *temp;
		char *temps;
		int i, len;

		/*
		 * Have to create a mangled name which is:
		 * - a valid symbol
		 * - unique
		 */
		temp = g_string_new ("");
		len = strlen (fullname);
		for (i = 0; i < len; ++i) {
			char c = fullname [i];
			if (isalnum (c)) {
				g_string_append_c (temp, c);
			} else if (c == '_') {
				g_string_append_c (temp, '_');
				g_string_append_c (temp, '_');
			} else {
				g_string_append_c (temp, '_');
				g_string_append_printf (temp, "%x", (int)c);
			}
		}
		temps = g_string_free (temp, FALSE);
		/* Include the length to avoid different length type names aliasing each other */
		g_string_append_printf (s, "cl%x_%s_", strlen (temps), temps);
		g_free (temps);
		return TRUE;
	}
	}
	return TRUE;
}

static gboolean
append_mangled_signature (GString *s, MonoMethodSignature *sig)
{
	int i;
	gboolean supported;

	supported = append_mangled_type (s, sig->ret);
	if (!supported)
		return FALSE;
	if (sig->hasthis)
		g_string_append_printf (s, "this_");
	for (i = 0; i < sig->param_count; ++i) {
		supported = append_mangled_type (s, sig->params [i]);
		if (!supported)
			return FALSE;
	}

	return TRUE;
}

static void
append_mangled_wrapper_type (GString *s, guint32 wrapper_type) 
{
	const char *label;

	switch (wrapper_type) {
	case MONO_WRAPPER_REMOTING_INVOKE:
		label = "remoting_invoke";
		break;
	case MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK:
		label = "remoting_invoke_check";
		break;
	case MONO_WRAPPER_XDOMAIN_INVOKE:
		label = "remoting_invoke_xdomain";
		break;
	case MONO_WRAPPER_PROXY_ISINST:
		label = "proxy_isinst";
		break;
	case MONO_WRAPPER_LDFLD:
		label = "ldfld";
		break;
	case MONO_WRAPPER_LDFLDA:
		label = "ldflda";
		break;
	case MONO_WRAPPER_STFLD: 
		label = "stfld";
		break;
	case MONO_WRAPPER_ALLOC:
		label = "alloc";
		break;
	case MONO_WRAPPER_WRITE_BARRIER:
		label = "write_barrier";
		break;
	case MONO_WRAPPER_STELEMREF:
		label = "stelemref";
		break;
	case MONO_WRAPPER_UNKNOWN:
		label = "unknown";
		break;
	case MONO_WRAPPER_MANAGED_TO_NATIVE:
		label = "man2native";
		break;
	case MONO_WRAPPER_SYNCHRONIZED:
		label = "synch";
		break;
	case MONO_WRAPPER_MANAGED_TO_MANAGED:
		label = "man2man";
		break;
	case MONO_WRAPPER_CASTCLASS:
		label = "castclass";
		break;
	case MONO_WRAPPER_RUNTIME_INVOKE:
		label = "run_invoke";
		break;
	case MONO_WRAPPER_DELEGATE_INVOKE:
		label = "del_inv";
		break;
	case MONO_WRAPPER_DELEGATE_BEGIN_INVOKE:
		label = "del_beg_inv";
		break;
	case MONO_WRAPPER_DELEGATE_END_INVOKE:
		label = "del_end_inv";
		break;
	case MONO_WRAPPER_NATIVE_TO_MANAGED:
		label = "native2man";
		break;
	default:
		g_assert_not_reached ();
	}

	g_string_append_printf (s, "%s_", label);
}

static void
append_mangled_wrapper_subtype (GString *s, WrapperSubtype subtype) 
{
	const char *label;

	switch (subtype) 
	{
	case WRAPPER_SUBTYPE_NONE:
		return;
	case WRAPPER_SUBTYPE_ELEMENT_ADDR:
		label = "elem_addr";
		break;
	case WRAPPER_SUBTYPE_STRING_CTOR:
		label = "str_ctor";
		break;
	case WRAPPER_SUBTYPE_VIRTUAL_STELEMREF:
		label = "virt_stelem";
		break;
	case WRAPPER_SUBTYPE_FAST_MONITOR_ENTER:
		label = "fast_mon_enter";
		break;
	case WRAPPER_SUBTYPE_FAST_MONITOR_ENTER_V4:
		label = "fast_mon_enter_4";
		break;
	case WRAPPER_SUBTYPE_FAST_MONITOR_EXIT:
		label = "fast_monitor_exit";
		break;
	case WRAPPER_SUBTYPE_PTR_TO_STRUCTURE:
		label = "ptr2struct";
		break;
	case WRAPPER_SUBTYPE_STRUCTURE_TO_PTR:
		label = "struct2ptr";
		break;
	case WRAPPER_SUBTYPE_CASTCLASS_WITH_CACHE:
		label = "castclass_w_cache";
		break;
	case WRAPPER_SUBTYPE_ISINST_WITH_CACHE:
		label = "isinst_w_cache";
		break;
	case WRAPPER_SUBTYPE_RUNTIME_INVOKE_NORMAL:
		label = "run_inv_norm";
		break;
	case WRAPPER_SUBTYPE_RUNTIME_INVOKE_DYNAMIC:
		label = "run_inv_dyn";
		break;
	case WRAPPER_SUBTYPE_RUNTIME_INVOKE_DIRECT:
		label = "run_inv_dir";
		break;
	case WRAPPER_SUBTYPE_RUNTIME_INVOKE_VIRTUAL:
		label = "run_inv_vir";
		break;
	case WRAPPER_SUBTYPE_ICALL_WRAPPER:
		label = "icall";
		break;
	case WRAPPER_SUBTYPE_NATIVE_FUNC_AOT:
		label = "native_func_aot";
		break;
	case WRAPPER_SUBTYPE_PINVOKE:
		label = "pinvoke";
		break;
	case WRAPPER_SUBTYPE_SYNCHRONIZED_INNER:
		label = "synch_inner";
		break;
	case WRAPPER_SUBTYPE_GSHAREDVT_IN:
		label = "gshared_in";
		break;
	case WRAPPER_SUBTYPE_GSHAREDVT_OUT:
		label = "gshared_out";
		break;
	case WRAPPER_SUBTYPE_ARRAY_ACCESSOR:
		label = "array_acc";
		break;
	case WRAPPER_SUBTYPE_GENERIC_ARRAY_HELPER:
		label = "generic_arry_help";
		break;
	case WRAPPER_SUBTYPE_DELEGATE_INVOKE_VIRTUAL:
		label = "del_inv_virt";
		break;
	case WRAPPER_SUBTYPE_DELEGATE_INVOKE_BOUND:
		label = "del_inv_bound";
		break;
	case WRAPPER_SUBTYPE_GSHAREDVT_IN_SIG:
		label = "gsharedvt_in_sig";
		break;
	case WRAPPER_SUBTYPE_GSHAREDVT_OUT_SIG:
		label = "gsharedvt_out_sig";
		break;
	default:
		g_assert_not_reached ();
	}

	g_string_append_printf (s, "%s_", label);
}

static gboolean
append_mangled_klass (GString *s, MonoClass *klass)
{
	char *klass_desc = mono_class_full_name (klass);
	g_string_append_printf (s, "_%s_%s_", klass->name_space, klass_desc);
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
	g_string_append_printf (s, "aot_wrapper_");

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
			MonoMethodSignature *sig = mono_method_signature (method);
			WrapperInfo *info = mono_marshal_get_wrapper_info (method);

			g_string_append_printf (s, "u_");
			if (method->wrapper_type == MONO_WRAPPER_DELEGATE_INVOKE)
				append_mangled_wrapper_subtype (s, info->subtype);
			g_string_append_printf (s, "u_sigstart");
			success = success && append_mangled_signature (s, sig);
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
	return success;
}

static gboolean
append_mangled_method (GString *s, MonoMethod *method)
{
	if (method->wrapper_type) {
		return append_mangled_wrapper (s, method);
	} else if (method->is_inflated) {
		g_string_append_printf (s, "aot_inflated_");
		MonoMethodInflated *imethod = (MonoMethodInflated*) method;
		append_mangled_method (s, imethod->declaring);

		if (imethod->context.class_inst != NULL || imethod->context.method_inst != NULL) {
			g_string_append_printf (s, "_gens_");
			g_string_append_printf (s, mono_context_get_desc (&imethod->context));
		}
	} else if (method->is_generic) {
		g_string_append_printf (s, "aot_generic_");
		append_mangled_klass (s, method->klass);

		MonoGenericContainer *container = mono_method_get_generic_container (method);
		g_string_append_printf (s, "_%s_gens_%s", method->name, mono_context_get_desc (&container->context));

		return append_mangled_signature (s, method->signature);
	} else {
		g_string_append_printf (s, "aot_");
		append_mangled_klass (s, method->klass);
		g_string_append_printf (s, "_%s", method->name);
		if (!append_mangled_signature (s, method->signature)) {
			g_string_free (s, TRUE);
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * mono_aot_get_mangled_method_name:
 *
 *   Return a unique mangled name for METHOD, or NULL.
 */
char*
mono_aot_get_mangled_method_name (MonoMethod *method)
{
	GString *s = g_string_new ("");
	if (!append_mangled_method (s, method)) {
		g_string_free (s, TRUE);
		return NULL;
	} else {
		return g_string_free (s, FALSE);
	}
}

gboolean
mono_aot_is_direct_callable (MonoJumpInfo *patch_info)
{
	return is_direct_callable (llvm_acfg, NULL, patch_info);
}

void
mono_aot_mark_unused_llvm_plt_entry (MonoJumpInfo *patch_info)
{
	MonoPltEntry *plt_entry;

	plt_entry = get_plt_entry (llvm_acfg, patch_info);
	plt_entry->llvm_used = FALSE;
}

char*
mono_aot_get_direct_call_symbol (MonoJumpInfoType type, gconstpointer data)
{
	const char *sym = NULL;

	if (llvm_acfg->aot_opts.direct_icalls) {
		if (type == MONO_PATCH_INFO_JIT_ICALL_ADDR) {
			/* Call to a C function implementing a jit icall */
			sym = mono_lookup_jit_icall_symbol ((const char *)data);
		} else if (type == MONO_PATCH_INFO_ICALL_ADDR_CALL) {
			MonoMethod *method = (MonoMethod *)data;
			if (!(method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL))
				sym = mono_lookup_icall_symbol (method);
			else if (llvm_acfg->aot_opts.direct_pinvoke)
				sym = get_pinvoke_import (llvm_acfg, method);
		}
		if (sym)
			return g_strdup (sym);
	}
	return NULL;
}

char*
mono_aot_get_plt_symbol (MonoJumpInfoType type, gconstpointer data)
{
	MonoJumpInfo *ji = (MonoJumpInfo *)mono_mempool_alloc (llvm_acfg->mempool, sizeof (MonoJumpInfo));
	MonoPltEntry *plt_entry;
	const char *sym = NULL;

	ji->type = type;
	ji->data.target = data;

	if (!can_encode_patch (llvm_acfg, ji))
		return NULL;

	if (llvm_acfg->aot_opts.direct_icalls) {
		if (type == MONO_PATCH_INFO_JIT_ICALL_ADDR) {
			/* Call to a C function implementing a jit icall */
			sym = mono_lookup_jit_icall_symbol ((const char *)data);
		} else if (type == MONO_PATCH_INFO_ICALL_ADDR_CALL) {
			MonoMethod *method = (MonoMethod *)data;
			if (!(method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL))
				sym = mono_lookup_icall_symbol (method);
		}
		if (sym)
			return g_strdup (sym);
	}

	plt_entry = get_plt_entry (llvm_acfg, ji);
	plt_entry->llvm_used = TRUE;

#if defined(TARGET_MACH)
	return g_strdup_printf (plt_entry->llvm_symbol + strlen (llvm_acfg->llvm_label_prefix));
#else
	return g_strdup_printf (plt_entry->llvm_symbol);
#endif
}

int
mono_aot_get_method_index (MonoMethod *method)
{
	g_assert (llvm_acfg);
	return get_method_index (llvm_acfg, method);
}

MonoJumpInfo*
mono_aot_patch_info_dup (MonoJumpInfo* ji)
{
	MonoJumpInfo *res;

	mono_acfg_lock (llvm_acfg);
	res = mono_patch_info_dup_mp (llvm_acfg->mempool, ji);
	mono_acfg_unlock (llvm_acfg);

	return res;
}

static int
execute_system (const char * command)
{
	int status = 0;

#if HOST_WIN32
	// We need an extra set of quotes around the whole command to properly handle commands 
	// with spaces since internally the command is called through "cmd /c.
	char * quoted_command = g_strdup_printf ("\"%s\"", command);

	int size =  MultiByteToWideChar (CP_UTF8, 0 , quoted_command , -1, NULL , 0);
	wchar_t* wstr = g_malloc (sizeof (wchar_t) * size);
	MultiByteToWideChar (CP_UTF8, 0, quoted_command, -1, wstr , size);
	status = _wsystem (wstr);
	g_free (wstr);

	g_free (quoted_command);
#elif defined (HAVE_SYSTEM)
	status = system (command);
#else
	g_assert_not_reached ();
#endif

	return status;
}

#ifdef ENABLE_LLVM

/*
 * emit_llvm_file:
 *
 *   Emit the LLVM code into an LLVM bytecode file, and compile it using the LLVM
 * tools.
 */
static gboolean
emit_llvm_file (MonoAotCompile *acfg)
{
	char *command, *opts, *tempbc, *optbc, *output_fname;

	if (acfg->aot_opts.llvm_only && acfg->aot_opts.asm_only) {
		tempbc = g_strdup_printf ("%s.bc", acfg->tmpbasename);
		optbc = g_strdup (acfg->aot_opts.llvm_outfile);
	} else {
		tempbc = g_strdup_printf ("%s.bc", acfg->tmpbasename);
		optbc = g_strdup_printf ("%s.opt.bc", acfg->tmpbasename);
	}

	mono_llvm_emit_aot_module (tempbc, g_path_get_basename (acfg->image->name));

	/*
	 * FIXME: Experiment with adding optimizations, the -std-compile-opts set takes
	 * a lot of time, and doesn't seem to save much space.
	 * The following optimizations cannot be enabled:
	 * - 'tailcallelim'
	 * - 'jump-threading' changes our blockaddress references to int constants.
	 * - 'basiccg' fails because it contains:
	 * if (CS && !isa<IntrinsicInst>(II)) {
	 * and isa<IntrinsicInst> is false for invokes to intrinsics (iltests.exe).
	 * - 'prune-eh' and 'functionattrs' depend on 'basiccg'.
	 * The opt list below was produced by taking the output of:
	 * llvm-as < /dev/null | opt -O2 -disable-output -debug-pass=Arguments
	 * then removing tailcallelim + the global opts.
	 * strip-dead-prototypes deletes unused intrinsics definitions.
	 */
	/* The dse pass is disabled because of #13734 and #17616 */
	/*
	 * The dse bug is in DeadStoreElimination.cpp:isOverwrite ():
	 * // If we have no DataLayout information around, then the size of the store
	 *  // is inferrable from the pointee type.  If they are the same type, then
	 * // we know that the store is safe.
	 * if (AA.getDataLayout() == 0 &&
	 * Later.Ptr->getType() == Earlier.Ptr->getType()) {
	 * return OverwriteComplete;
	 * Here, if 'Earlier' refers to a memset, and Later has no size info, it mistakenly thinks the memset is redundant.
	 */
	if (acfg->aot_opts.llvm_only)
		// FIXME: This doesn't work yet
		opts = g_strdup ("");
	else
#if LLVM_API_VERSION > 100
		opts = g_strdup ("-O2 -disable-tail-calls");
#else
		opts = g_strdup ("-targetlibinfo -no-aa -basicaa -notti -instcombine -simplifycfg -inline-cost -inline -sroa -domtree -early-cse -lazy-value-info -correlated-propagation -simplifycfg -instcombine -simplifycfg -reassociate -domtree -loops -loop-simplify -lcssa -loop-rotate -licm -lcssa -loop-unswitch -instcombine -scalar-evolution -loop-simplify -lcssa -indvars -loop-idiom -loop-deletion -loop-unroll -memdep -gvn -memdep -memcpyopt -sccp -instcombine -lazy-value-info -correlated-propagation -domtree -memdep -adce -simplifycfg -instcombine -strip-dead-prototypes -domtree -verify");
#endif
	command = g_strdup_printf ("\"%sopt\" -f %s -o \"%s\" \"%s\"", acfg->aot_opts.llvm_path, opts, optbc, tempbc);
	aot_printf (acfg, "Executing opt: %s\n", command);
	if (execute_system (command) != 0)
		return FALSE;
	g_free (opts);

	if (acfg->aot_opts.llvm_only && acfg->aot_opts.asm_only)
		/* Nothing else to do */
		return TRUE;

	if (acfg->aot_opts.llvm_only) {
		/* Use the stock clang from xcode */
		// FIXME: arch
		command = g_strdup_printf ("clang++ -fexceptions -march=x86-64 -fpic -msse -msse2 -msse3 -msse4 -O2 -fno-optimize-sibling-calls -Wno-override-module -c -o \"%s\" \"%s.opt.bc\"", acfg->llvm_ofile, acfg->tmpbasename);

		aot_printf (acfg, "Executing clang: %s\n", command);
		if (execute_system (command) != 0)
			return FALSE;
		return TRUE;
	}

	if (!acfg->llc_args)
		acfg->llc_args = g_string_new ("");

	/* Verbose asm slows down llc greatly */
	g_string_append (acfg->llc_args, " -asm-verbose=false");

	if (acfg->aot_opts.mtriple)
		g_string_append_printf (acfg->llc_args, " -mtriple=%s", acfg->aot_opts.mtriple);

	g_string_append (acfg->llc_args, " -disable-gnu-eh-frame -enable-mono-eh-frame");

	g_string_append_printf (acfg->llc_args, " -mono-eh-frame-symbol=%s%s", acfg->user_symbol_prefix, acfg->llvm_eh_frame_symbol);

#if LLVM_API_VERSION > 100
	g_string_append_printf (acfg->llc_args, " -disable-tail-calls");
#endif

#if defined(TARGET_MACH) && defined(TARGET_ARM)
	/* ios requires PIC code now */
	g_string_append_printf (acfg->llc_args, " -relocation-model=pic");
#else
	if (llvm_acfg->aot_opts.static_link)
		g_string_append_printf (acfg->llc_args, " -relocation-model=static");
	else
		g_string_append_printf (acfg->llc_args, " -relocation-model=pic");
#endif

	if (acfg->llvm_owriter) {
		/* Emit an object file directly */
		output_fname = g_strdup_printf ("%s", acfg->llvm_ofile);
		g_string_append_printf (acfg->llc_args, " -filetype=obj");
	} else {
		output_fname = g_strdup_printf ("%s", acfg->llvm_sfile);
	}
	command = g_strdup_printf ("\"%sllc\" %s -o \"%s\" \"%s.opt.bc\"", acfg->aot_opts.llvm_path, acfg->llc_args->str, output_fname, acfg->tmpbasename);
	g_free (output_fname);

	aot_printf (acfg, "Executing llc: %s\n", command);

	if (execute_system (command) != 0)
		return FALSE;
	return TRUE;
}
#endif

static void
emit_code (MonoAotCompile *acfg)
{
	int oindex, i, prev_index;
	gboolean saved_unbox_info = FALSE;
	char symbol [MAX_SYMBOL_SIZE];

	if (acfg->aot_opts.llvm_only)
		return;

#if defined(TARGET_POWERPC64)
	sprintf (symbol, ".Lgot_addr");
	emit_section_change (acfg, ".text", 0);
	emit_alignment (acfg, 8);
	emit_label (acfg, symbol);
	emit_pointer (acfg, acfg->got_symbol);
#endif

	/* 
	 * This global symbol is used to compute the address of each method using the
	 * code_offsets array. It is also used to compute the memory ranges occupied by
	 * AOT code, so it must be equal to the address of the first emitted method.
	 */
	emit_section_change (acfg, ".text", 0);
	emit_alignment_code (acfg, 8);
	emit_info_symbol (acfg, "jit_code_start");

	/* 
	 * Emit some padding so the local symbol for the first method doesn't have the
	 * same address as 'methods'.
	 */
	emit_padding (acfg, 16);

	for (oindex = 0; oindex < acfg->method_order->len; ++oindex) {
		MonoCompile *cfg;
		MonoMethod *method;

		i = GPOINTER_TO_UINT (g_ptr_array_index (acfg->method_order, oindex));

		cfg = acfg->cfgs [i];

		if (!cfg)
			continue;

		method = cfg->orig_method;

		/* Emit unbox trampoline */
		if (mono_aot_mode_is_full (&acfg->aot_opts) && cfg->orig_method->klass->valuetype) {
			sprintf (symbol, "ut_%d", get_method_index (acfg, method));

			emit_section_change (acfg, ".text", 0);

			if (acfg->thumb_mixed && cfg->compile_llvm) {
				emit_set_thumb_mode (acfg);
				fprintf (acfg->fp, "\n.thumb_func\n");
			}

			emit_label (acfg, symbol);

			arch_emit_unbox_trampoline (acfg, cfg, cfg->orig_method, cfg->asm_symbol);

			if (acfg->thumb_mixed && cfg->compile_llvm)
				emit_set_arm_mode (acfg);

			if (!saved_unbox_info) {
				char user_symbol [128];
				GSList *unwind_ops;
				sprintf (user_symbol, "%sunbox_trampoline_p", acfg->user_symbol_prefix);

				emit_label (acfg, "ut_end");

				unwind_ops = mono_unwind_get_cie_program ();
				save_unwind_info (acfg, user_symbol, unwind_ops);
				mono_free_unwind_info (unwind_ops);

				/* Save the unbox trampoline size */
				emit_symbol_diff (acfg, "ut_end", symbol, 0);

				saved_unbox_info = TRUE;
			}
		}

		if (cfg->compile_llvm)
			acfg->stats.llvm_count ++;
		else
			emit_method_code (acfg, cfg);
	}

	emit_section_change (acfg, ".text", 0);
	emit_alignment_code (acfg, 8);
	emit_info_symbol (acfg, "jit_code_end");

	/* To distinguish it from the next symbol */
	emit_padding (acfg, 4);

	/* 
	 * Add .no_dead_strip directives for all LLVM methods to prevent the OSX linker
	 * from optimizing them away, since it doesn't see that code_offsets references them.
	 * JITted methods don't need this since they are referenced using assembler local
	 * symbols.
	 * FIXME: This is why write-symbols doesn't work on OSX ?
	 */
	if (acfg->llvm && acfg->need_no_dead_strip) {
		fprintf (acfg->fp, "\n");
		for (i = 0; i < acfg->nmethods; ++i) {
			if (acfg->cfgs [i] && acfg->cfgs [i]->compile_llvm)
				fprintf (acfg->fp, ".no_dead_strip %s\n", acfg->cfgs [i]->asm_symbol);
		}
	}

	/*
	 * To work around linker issues, we emit a table of branches, and disassemble them at runtime.
	 * This is PIE code, and the linker can update it if needed.
	 */
	
	sprintf (symbol, "method_addresses");
	emit_section_change (acfg, ".text", 1);
	emit_alignment_code (acfg, 8);
	emit_info_symbol (acfg, symbol);
	if (acfg->aot_opts.write_symbols)
		emit_local_symbol (acfg, symbol, "method_addresses_end", TRUE);
	emit_unset_mode (acfg);
	if (acfg->need_no_dead_strip)
		fprintf (acfg->fp, "	.no_dead_strip %s\n", symbol);

	for (i = 0; i < acfg->nmethods; ++i) {
#ifdef MONO_ARCH_AOT_SUPPORTED
		int call_size;

		if (acfg->cfgs [i]) {
			arch_emit_direct_call (acfg, acfg->cfgs [i]->asm_symbol, FALSE, acfg->thumb_mixed && acfg->cfgs [i]->compile_llvm, NULL, &call_size);
		} else {
			arch_emit_direct_call (acfg, symbol, FALSE, FALSE, NULL, &call_size);
		}
#endif
	}

	sprintf (symbol, "method_addresses_end");
	emit_label (acfg, symbol);
	emit_line (acfg);

	/* Emit a sorted table mapping methods to the index of their unbox trampolines */
	sprintf (symbol, "unbox_trampolines");
	emit_section_change (acfg, RODATA_SECT, 0);
	emit_alignment (acfg, 8);
	emit_info_symbol (acfg, symbol);

	prev_index = -1;
	for (i = 0; i < acfg->nmethods; ++i) {
		MonoCompile *cfg;
		MonoMethod *method;
		int index;

		cfg = acfg->cfgs [i];
		if (!cfg)
			continue;

		method = cfg->orig_method;

		if (mono_aot_mode_is_full (&acfg->aot_opts) && cfg->orig_method->klass->valuetype) {
			index = get_method_index (acfg, method);

			emit_int32 (acfg, index);
			/* Make sure the table is sorted by index */
			g_assert (index > prev_index);
			prev_index = index;
		}
	}
	sprintf (symbol, "unbox_trampolines_end");
	emit_info_symbol (acfg, symbol);
	emit_int32 (acfg, 0);

	/* Emit a separate table with the trampoline addresses/offsets */
	sprintf (symbol, "unbox_trampoline_addresses");
	emit_section_change (acfg, ".text", 0);
	emit_alignment_code (acfg, 8);
	emit_info_symbol (acfg, symbol);

	for (i = 0; i < acfg->nmethods; ++i) {
		MonoCompile *cfg;
		MonoMethod *method;
		int index;

		cfg = acfg->cfgs [i];
		if (!cfg)
			continue;

		method = cfg->orig_method;

		if (mono_aot_mode_is_full (&acfg->aot_opts) && cfg->orig_method->klass->valuetype) {
#ifdef MONO_ARCH_AOT_SUPPORTED
			int call_size;

			index = get_method_index (acfg, method);
			sprintf (symbol, "ut_%d", index);

			arch_emit_direct_call (acfg, symbol, FALSE, acfg->thumb_mixed && cfg->compile_llvm, NULL, &call_size);
#endif
		}
	}
	emit_int32 (acfg, 0);
}

static void
emit_info (MonoAotCompile *acfg)
{
	int oindex, i;
	gint32 *offsets;

	offsets = g_new0 (gint32, acfg->nmethods);

	for (oindex = 0; oindex < acfg->method_order->len; ++oindex) {
		i = GPOINTER_TO_UINT (g_ptr_array_index (acfg->method_order, oindex));

		if (acfg->cfgs [i]) {
			emit_method_info (acfg, acfg->cfgs [i]);
			offsets [i] = acfg->cfgs [i]->method_info_offset;
		} else {
			offsets [i] = 0;
		}
	}

	acfg->stats.offsets_size += emit_offset_table (acfg, "method_info_offsets", MONO_AOT_TABLE_METHOD_INFO_OFFSETS, acfg->nmethods, 10, offsets);

	g_free (offsets);
}

#endif /* #if !defined(DISABLE_AOT) && !defined(DISABLE_JIT) */

#define rot(x,k) (((x)<<(k)) | ((x)>>(32-(k))))
#define mix(a,b,c) { \
	a -= c;  a ^= rot(c, 4);  c += b; \
	b -= a;  b ^= rot(a, 6);  a += c; \
	c -= b;  c ^= rot(b, 8);  b += a; \
	a -= c;  a ^= rot(c,16);  c += b; \
	b -= a;  b ^= rot(a,19);  a += c; \
	c -= b;  c ^= rot(b, 4);  b += a; \
}
#define final(a,b,c) { \
	c ^= b; c -= rot(b,14); \
	a ^= c; a -= rot(c,11); \
	b ^= a; b -= rot(a,25); \
	c ^= b; c -= rot(b,16); \
	a ^= c; a -= rot(c,4);  \
	b ^= a; b -= rot(a,14); \
	c ^= b; c -= rot(b,24); \
}

static guint
mono_aot_type_hash (MonoType *t1)
{
	guint hash = t1->type;

	hash |= t1->byref << 6; /* do not collide with t1->type values */
	switch (t1->type) {
	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_SZARRAY:
		/* check if the distribution is good enough */
		return ((hash << 5) - hash) ^ mono_metadata_str_hash (t1->data.klass->name);
	case MONO_TYPE_PTR:
		return ((hash << 5) - hash) ^ mono_metadata_type_hash (t1->data.type);
	case MONO_TYPE_ARRAY:
		return ((hash << 5) - hash) ^ mono_metadata_type_hash (&t1->data.array->eklass->byval_arg);
	case MONO_TYPE_GENERICINST:
		return ((hash << 5) - hash) ^ 0;
	default:
		return hash;
	}
}

/*
 * mono_aot_method_hash:
 *
 *   Return a hash code for methods which only depends on metadata.
 */
guint32
mono_aot_method_hash (MonoMethod *method)
{
	MonoMethodSignature *sig;
	MonoClass *klass;
	int i, hindex;
	int hashes_count;
	guint32 *hashes_start, *hashes;
	guint32 a, b, c;
	MonoGenericInst *class_ginst = NULL;
	MonoGenericInst *ginst = NULL;

	/* Similar to the hash in mono_method_get_imt_slot () */

	sig = mono_method_signature (method);

	if (mono_class_is_ginst (method->klass))
		class_ginst = mono_class_get_generic_class (method->klass)->context.class_inst;
	if (method->is_inflated)
		ginst = ((MonoMethodInflated*)method)->context.method_inst;

	hashes_count = sig->param_count + 5 + (class_ginst ? class_ginst->type_argc : 0) + (ginst ? ginst->type_argc : 0);
	hashes_start = (guint32 *)g_malloc0 (hashes_count * sizeof (guint32));
	hashes = hashes_start;

	/* Some wrappers are assigned to random classes */
	if (!method->wrapper_type || method->wrapper_type == MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK)
		klass = method->klass;
	else
		klass = mono_defaults.object_class;

	if (!method->wrapper_type) {
		char *full_name;

		if (mono_class_is_ginst (klass))
			full_name = mono_type_full_name (&mono_class_get_generic_class (klass)->container_class->byval_arg);
		else
			full_name = mono_type_full_name (&klass->byval_arg);

		hashes [0] = mono_metadata_str_hash (full_name);
		hashes [1] = 0;
		g_free (full_name);
	} else {
		hashes [0] = mono_metadata_str_hash (klass->name);
		hashes [1] = mono_metadata_str_hash (klass->name_space);
	}
	if (method->wrapper_type == MONO_WRAPPER_STFLD || method->wrapper_type == MONO_WRAPPER_LDFLD || method->wrapper_type == MONO_WRAPPER_LDFLDA)
		/* The method name includes a stringified pointer */
		hashes [2] = 0;
	else
		hashes [2] = mono_metadata_str_hash (method->name);
	hashes [3] = method->wrapper_type;
	hashes [4] = mono_aot_type_hash (sig->ret);
	hindex = 5;
	for (i = 0; i < sig->param_count; i++) {
		hashes [hindex ++] = mono_aot_type_hash (sig->params [i]);
	}
	if (class_ginst) {
		for (i = 0; i < class_ginst->type_argc; ++i)
			hashes [hindex ++] = mono_aot_type_hash (class_ginst->type_argv [i]);
	}
	if (ginst) {
		for (i = 0; i < ginst->type_argc; ++i)
			hashes [hindex ++] = mono_aot_type_hash (ginst->type_argv [i]);
	}		
	g_assert (hindex == hashes_count);

	/* Setup internal state */
	a = b = c = 0xdeadbeef + (((guint32)hashes_count)<<2);

	/* Handle most of the hashes */
	while (hashes_count > 3) {
		a += hashes [0];
		b += hashes [1];
		c += hashes [2];
		mix (a,b,c);
		hashes_count -= 3;
		hashes += 3;
	}

	/* Handle the last 3 hashes (all the case statements fall through) */
	switch (hashes_count) { 
	case 3 : c += hashes [2];
	case 2 : b += hashes [1];
	case 1 : a += hashes [0];
		final (a,b,c);
	case 0: /* nothing left to add */
		break;
	}
	
	g_free (hashes_start);
	
	return c;
}
#undef rot
#undef mix
#undef final

/*
 * mono_aot_get_array_helper_from_wrapper;
 *
 * Get the helper method in Array called by an array wrapper method.
 */
MonoMethod*
mono_aot_get_array_helper_from_wrapper (MonoMethod *method)
{
	MonoMethod *m;
	const char *prefix;
	MonoGenericContext ctx;
	MonoType *args [16];
	char *mname, *iname, *s, *s2, *helper_name = NULL;

	prefix = "System.Collections.Generic";
	s = g_strdup_printf ("%s", method->name + strlen (prefix) + 1);
	s2 = strstr (s, "`1.");
	g_assert (s2);
	s2 [0] = '\0';
	iname = s;
	mname = s2 + 3;

	//printf ("X: %s %s\n", iname, mname);

	if (!strcmp (iname, "IList"))
		helper_name = g_strdup_printf ("InternalArray__%s", mname);
	else
		helper_name = g_strdup_printf ("InternalArray__%s_%s", iname, mname);
	m = mono_class_get_method_from_name (mono_defaults.array_class, helper_name, mono_method_signature (method)->param_count);
	g_assert (m);
	g_free (helper_name);
	g_free (s);

	if (m->is_generic) {
		MonoError error;
		memset (&ctx, 0, sizeof (ctx));
		args [0] = &method->klass->element_class->byval_arg;
		ctx.method_inst = mono_metadata_get_generic_inst (1, args);
		m = mono_class_inflate_generic_method_checked (m, &ctx, &error);
		g_assert (mono_error_ok (&error)); /* FIXME don't swallow the error */
	}

	return m;
}

#if !defined(DISABLE_AOT) && !defined(DISABLE_JIT)

typedef struct HashEntry {
    guint32 key, value, index;
	struct HashEntry *next;
} HashEntry;

/*
 * emit_extra_methods:
 *
 * Emit methods which are not in the METHOD table, like wrappers.
 */
static void
emit_extra_methods (MonoAotCompile *acfg)
{
	int i, table_size, buf_size;
	guint8 *p, *buf;
	guint32 *info_offsets;
	guint32 hash;
	GPtrArray *table;
	HashEntry *entry, *new_entry;
	int nmethods, max_chain_length;
	int *chain_lengths;

	info_offsets = g_new0 (guint32, acfg->extra_methods->len);

	/* Emit method info */
	nmethods = 0;
	for (i = 0; i < acfg->extra_methods->len; ++i) {
		MonoMethod *method = (MonoMethod *)g_ptr_array_index (acfg->extra_methods, i);
		MonoCompile *cfg = (MonoCompile *)g_hash_table_lookup (acfg->method_to_cfg, method);

		if (!cfg)
			continue;

		buf_size = 10240;
		p = buf = (guint8 *)g_malloc (buf_size);

		nmethods ++;

		method = cfg->method_to_register;

		encode_method_ref (acfg, method, p, &p);

		g_assert ((p - buf) < buf_size);

		info_offsets [i] = add_to_blob (acfg, buf, p - buf);
		g_free (buf);
	}

	/*
	 * Construct a chained hash table for mapping indexes in extra_method_info to
	 * method indexes.
	 */
	table_size = g_spaced_primes_closest ((int)(nmethods * 1.5));
	table = g_ptr_array_sized_new (table_size);
	for (i = 0; i < table_size; ++i)
		g_ptr_array_add (table, NULL);
	chain_lengths = g_new0 (int, table_size);
	max_chain_length = 0;
	for (i = 0; i < acfg->extra_methods->len; ++i) {
		MonoMethod *method = (MonoMethod *)g_ptr_array_index (acfg->extra_methods, i);
		MonoCompile *cfg = (MonoCompile *)g_hash_table_lookup (acfg->method_to_cfg, method);
		guint32 key, value;

		if (!cfg)
			continue;

		key = info_offsets [i];
		value = get_method_index (acfg, method);

		hash = mono_aot_method_hash (method) % table_size;
		//printf ("X: %s %x\n", mono_method_get_full_name (method), mono_aot_method_hash (method));

		chain_lengths [hash] ++;
		max_chain_length = MAX (max_chain_length, chain_lengths [hash]);

		new_entry = (HashEntry *)mono_mempool_alloc0 (acfg->mempool, sizeof (HashEntry));
		new_entry->key = key;
		new_entry->value = value;

		entry = (HashEntry *)g_ptr_array_index (table, hash);
		if (entry == NULL) {
			new_entry->index = hash;
			g_ptr_array_index (table, hash) = new_entry;
		} else {
			while (entry->next)
				entry = entry->next;
			
			entry->next = new_entry;
			new_entry->index = table->len;
			g_ptr_array_add (table, new_entry);
		}
	}
	g_free (chain_lengths);

	//printf ("MAX: %d\n", max_chain_length);

	buf_size = table->len * 12 + 4;
	p = buf = (guint8 *)g_malloc (buf_size);
	encode_int (table_size, p, &p);

	for (i = 0; i < table->len; ++i) {
		HashEntry *entry = (HashEntry *)g_ptr_array_index (table, i);

		if (entry == NULL) {
			encode_int (0, p, &p);
			encode_int (0, p, &p);
			encode_int (0, p, &p);
		} else {
			//g_assert (entry->key > 0);
			encode_int (entry->key, p, &p);
			encode_int (entry->value, p, &p);
			if (entry->next)
				encode_int (entry->next->index, p, &p);
			else
				encode_int (0, p, &p);
		}
	}
	g_assert (p - buf <= buf_size);

	/* Emit the table */
	emit_aot_data (acfg, MONO_AOT_TABLE_EXTRA_METHOD_TABLE, "extra_method_table", buf, p - buf);

	g_free (buf);

	/* 
	 * Emit a table reverse mapping method indexes to their index in extra_method_info.
	 * This is used by mono_aot_find_jit_info ().
	 */
	buf_size = acfg->extra_methods->len * 8 + 4;
	p = buf = (guint8 *)g_malloc (buf_size);
	encode_int (acfg->extra_methods->len, p, &p);
	for (i = 0; i < acfg->extra_methods->len; ++i) {
		MonoMethod *method = (MonoMethod *)g_ptr_array_index (acfg->extra_methods, i);

		encode_int (get_method_index (acfg, method), p, &p);
		encode_int (info_offsets [i], p, &p);
	}
	emit_aot_data (acfg, MONO_AOT_TABLE_EXTRA_METHOD_INFO_OFFSETS, "extra_method_info_offsets", buf, p - buf);

	g_free (buf);
	g_free (info_offsets);
	g_ptr_array_free (table, TRUE);
}	

static void
generate_aotid (guint8* aotid)
{
	gpointer rand_handle;
	MonoError error;

	mono_rand_open ();
	rand_handle = mono_rand_init (NULL, 0);

	mono_rand_try_get_bytes (&rand_handle, aotid, 16, &error);
	mono_error_assert_ok (&error);

	mono_rand_close (rand_handle);
}

static void
emit_exception_info (MonoAotCompile *acfg)
{
	int i;
	gint32 *offsets;
	SeqPointData sp_data;
	gboolean seq_points_to_file = FALSE;

	offsets = g_new0 (gint32, acfg->nmethods);
	for (i = 0; i < acfg->nmethods; ++i) {
		if (acfg->cfgs [i]) {
			MonoCompile *cfg = acfg->cfgs [i];

			// By design aot-runtime decode_exception_debug_info is not able to load sequence point debug data from a file.
			// As it is not possible to load debug data from a file its is also not possible to store it in a file.
			gboolean method_seq_points_to_file = acfg->aot_opts.gen_msym_dir &&
				cfg->gen_seq_points && !cfg->gen_sdb_seq_points;
			gboolean method_seq_points_to_binary = cfg->gen_seq_points && !method_seq_points_to_file;
			
			emit_exception_debug_info (acfg, cfg, method_seq_points_to_binary);
			offsets [i] = cfg->ex_info_offset;

			if (method_seq_points_to_file) {
				if (!seq_points_to_file) {
					mono_seq_point_data_init (&sp_data, acfg->nmethods);
					seq_points_to_file = TRUE;
				}
				mono_seq_point_data_add (&sp_data, cfg->method->token, cfg->method_index, cfg->seq_point_info);
			}
		} else {
			offsets [i] = 0;
		}
	}

	if (seq_points_to_file) {
		char *aotid = mono_guid_to_string_minimal (acfg->image->aotid);
		char *dir = g_build_filename (acfg->aot_opts.gen_msym_dir_path, aotid, NULL);
		char *image_basename = g_path_get_basename (acfg->image->name);
		char *aot_file = g_strdup_printf("%s%s", image_basename, SEQ_POINT_AOT_EXT);
		char *aot_file_path = g_build_filename (dir, aot_file, NULL);

		if (g_ensure_directory_exists (aot_file_path) == FALSE) {
			fprintf (stderr, "AOT : failed to create msym directory: %s\n", aot_file_path);
			exit (1);
		}

		mono_seq_point_data_write (&sp_data, aot_file_path);
		mono_seq_point_data_free (&sp_data);

		g_free (aotid);
		g_free (dir);
		g_free (image_basename);
		g_free (aot_file);
		g_free (aot_file_path);
	}

	acfg->stats.offsets_size += emit_offset_table (acfg, "ex_info_offsets", MONO_AOT_TABLE_EX_INFO_OFFSETS, acfg->nmethods, 10, offsets);
	g_free (offsets);
}

static void
emit_unwind_info (MonoAotCompile *acfg)
{
	int i;
	char symbol [128];

	if (acfg->aot_opts.llvm_only) {
		g_assert (acfg->unwind_ops->len == 0);
		return;
	}

	/* 
	 * The unwind info contains a lot of duplicates so we emit each unique
	 * entry once, and only store the offset from the start of the table in the
	 * exception info.
	 */

	sprintf (symbol, "unwind_info");
	emit_section_change (acfg, RODATA_SECT, 1);
	emit_alignment (acfg, 8);
	emit_info_symbol (acfg, symbol);

	for (i = 0; i < acfg->unwind_ops->len; ++i) {
		guint32 index = GPOINTER_TO_UINT (g_ptr_array_index (acfg->unwind_ops, i));
		guint8 *unwind_info;
		guint32 unwind_info_len;
		guint8 buf [16];
		guint8 *p;

		unwind_info = mono_get_cached_unwind_info (index, &unwind_info_len);

		p = buf;
		encode_value (unwind_info_len, p, &p);
		emit_bytes (acfg, buf, p - buf);
		emit_bytes (acfg, unwind_info, unwind_info_len);

		acfg->stats.unwind_info_size += (p - buf) + unwind_info_len;
	}
}

static void
emit_class_info (MonoAotCompile *acfg)
{
	int i;
	gint32 *offsets;

	offsets = g_new0 (gint32, acfg->image->tables [MONO_TABLE_TYPEDEF].rows);
	for (i = 0; i < acfg->image->tables [MONO_TABLE_TYPEDEF].rows; ++i)
		offsets [i] = emit_klass_info (acfg, MONO_TOKEN_TYPE_DEF | (i + 1));

	acfg->stats.offsets_size += emit_offset_table (acfg, "class_info_offsets", MONO_AOT_TABLE_CLASS_INFO_OFFSETS, acfg->image->tables [MONO_TABLE_TYPEDEF].rows, 10, offsets);
	g_free (offsets);
}

typedef struct ClassNameTableEntry {
	guint32 token, index;
	struct ClassNameTableEntry *next;
} ClassNameTableEntry;

static void
emit_class_name_table (MonoAotCompile *acfg)
{
	int i, table_size, buf_size;
	guint32 token, hash;
	MonoClass *klass;
	GPtrArray *table;
	char *full_name;
	guint8 *buf, *p;
	ClassNameTableEntry *entry, *new_entry;

	/*
	 * Construct a chained hash table for mapping class names to typedef tokens.
	 */
	table_size = g_spaced_primes_closest ((int)(acfg->image->tables [MONO_TABLE_TYPEDEF].rows * 1.5));
	table = g_ptr_array_sized_new (table_size);
	for (i = 0; i < table_size; ++i)
		g_ptr_array_add (table, NULL);
	for (i = 0; i < acfg->image->tables [MONO_TABLE_TYPEDEF].rows; ++i) {
		MonoError error;
		token = MONO_TOKEN_TYPE_DEF | (i + 1);
		klass = mono_class_get_checked (acfg->image, token, &error);
		if (!klass) {
			mono_error_cleanup (&error);
			continue;
		}
		full_name = mono_type_get_name_full (mono_class_get_type (klass), MONO_TYPE_NAME_FORMAT_FULL_NAME);
		hash = mono_metadata_str_hash (full_name) % table_size;
		g_free (full_name);

		/* FIXME: Allocate from the mempool */
		new_entry = g_new0 (ClassNameTableEntry, 1);
		new_entry->token = token;

		entry = (ClassNameTableEntry *)g_ptr_array_index (table, hash);
		if (entry == NULL) {
			new_entry->index = hash;
			g_ptr_array_index (table, hash) = new_entry;
		} else {
			while (entry->next)
				entry = entry->next;
			
			entry->next = new_entry;
			new_entry->index = table->len;
			g_ptr_array_add (table, new_entry);
		}
	}

	/* Emit the table */
	buf_size = table->len * 4 + 4;
	p = buf = (guint8 *)g_malloc0 (buf_size);

	/* FIXME: Optimize memory usage */
	g_assert (table_size < 65000);
	encode_int16 (table_size, p, &p);
	g_assert (table->len < 65000);
	for (i = 0; i < table->len; ++i) {
		ClassNameTableEntry *entry = (ClassNameTableEntry *)g_ptr_array_index (table, i);

		if (entry == NULL) {
			encode_int16 (0, p, &p);
			encode_int16 (0, p, &p);
		} else {
			encode_int16 (mono_metadata_token_index (entry->token), p, &p);
			if (entry->next)
				encode_int16 (entry->next->index, p, &p);
			else
				encode_int16 (0, p, &p);
		}
		g_free (entry);
	}
	g_assert (p - buf <= buf_size);
	g_ptr_array_free (table, TRUE);

	emit_aot_data (acfg, MONO_AOT_TABLE_CLASS_NAME, "class_name_table", buf, p - buf);

	g_free (buf);
}

static void
emit_image_table (MonoAotCompile *acfg)
{
	int i, buf_size;
	guint8 *buf, *p;

	/*
	 * The image table is small but referenced in a lot of places.
	 * So we emit it at once, and reference its elements by an index.
	 */
	buf_size = acfg->image_table->len * 28 + 4;
	for (i = 0; i < acfg->image_table->len; i++) {
		MonoImage *image = (MonoImage*)g_ptr_array_index (acfg->image_table, i);
		MonoAssemblyName *aname = &image->assembly->aname;

		buf_size += strlen (image->assembly_name) + strlen (image->guid) + (aname->culture ? strlen (aname->culture) : 1) + strlen ((char*)aname->public_key_token) + 4;
	}

	buf = p = (guint8 *)g_malloc0 (buf_size);
	encode_int (acfg->image_table->len, p, &p);
	for (i = 0; i < acfg->image_table->len; i++) {
		MonoImage *image = (MonoImage*)g_ptr_array_index (acfg->image_table, i);
		MonoAssemblyName *aname = &image->assembly->aname;

		/* FIXME: Support multi-module assemblies */
		g_assert (image->assembly->image == image);

		encode_string (image->assembly_name, p, &p);
		encode_string (image->guid, p, &p);
		encode_string (aname->culture ? aname->culture : "", p, &p);
		encode_string ((const char*)aname->public_key_token, p, &p);

		while (GPOINTER_TO_UINT (p) % 8 != 0)
			p ++;

		encode_int (aname->flags, p, &p);
		encode_int (aname->major, p, &p);
		encode_int (aname->minor, p, &p);
		encode_int (aname->build, p, &p);
		encode_int (aname->revision, p, &p);
	}
	g_assert (p - buf <= buf_size);

	emit_aot_data (acfg, MONO_AOT_TABLE_IMAGE_TABLE, "image_table", buf, p - buf);

	g_free (buf);
}

static void
emit_got_info (MonoAotCompile *acfg, gboolean llvm)
{
	int i, first_plt_got_patch = 0, buf_size;
	guint8 *p, *buf;
	guint32 *got_info_offsets;
	GotInfo *info = llvm ? &acfg->llvm_got_info : &acfg->got_info;

	/* Add the patches needed by the PLT to the GOT */
	if (!llvm) {
		acfg->plt_got_offset_base = acfg->got_offset;
		first_plt_got_patch = info->got_patches->len;
		for (i = 1; i < acfg->plt_offset; ++i) {
			MonoPltEntry *plt_entry = (MonoPltEntry *)g_hash_table_lookup (acfg->plt_offset_to_entry, GUINT_TO_POINTER (i));

			g_ptr_array_add (info->got_patches, plt_entry->ji);

			acfg->stats.got_slot_types [plt_entry->ji->type] ++;
		}

		acfg->got_offset += acfg->plt_offset;
	}

	/**
	 * FIXME: 
	 * - optimize offsets table.
	 * - reduce number of exported symbols.
	 * - emit info for a klass only once.
	 * - determine when a method uses a GOT slot which is guaranteed to be already 
	 *   initialized.
	 * - clean up and document the code.
	 * - use String.Empty in class libs.
	 */

	/* Encode info required to decode shared GOT entries */
	buf_size = info->got_patches->len * 128;
	p = buf = (guint8 *)mono_mempool_alloc (acfg->mempool, buf_size);
	got_info_offsets = (guint32 *)mono_mempool_alloc (acfg->mempool, info->got_patches->len * sizeof (guint32));
	if (!llvm) {
		acfg->plt_got_info_offsets = (guint32 *)mono_mempool_alloc (acfg->mempool, acfg->plt_offset * sizeof (guint32));
		/* Unused */
		if (acfg->plt_offset)
			acfg->plt_got_info_offsets [0] = 0;
	}
	for (i = 0; i < info->got_patches->len; ++i) {
		MonoJumpInfo *ji = (MonoJumpInfo *)g_ptr_array_index (info->got_patches, i);
		guint8 *p2;

		p = buf;

		encode_value (ji->type, p, &p);
		p2 = p;
		encode_patch (acfg, ji, p, &p);
		acfg->stats.got_slot_info_sizes [ji->type] += p - p2;
		g_assert (p - buf <= buf_size);
		got_info_offsets [i] = add_to_blob (acfg, buf, p - buf);

		if (!llvm && i >= first_plt_got_patch)
			acfg->plt_got_info_offsets [i - first_plt_got_patch + 1] = got_info_offsets [i];
		acfg->stats.got_info_size += p - buf;
	}

	/* Emit got_info_offsets table */

	/* No need to emit offsets for the got plt entries, the plt embeds them directly */
	acfg->stats.offsets_size += emit_offset_table (acfg, llvm ? "llvm_got_info_offsets" : "got_info_offsets", llvm ? MONO_AOT_TABLE_LLVM_GOT_INFO_OFFSETS : MONO_AOT_TABLE_GOT_INFO_OFFSETS, llvm ? acfg->llvm_got_offset : first_plt_got_patch, 10, (gint32*)got_info_offsets);
}

static void
emit_got (MonoAotCompile *acfg)
{
	char symbol [MAX_SYMBOL_SIZE];

	if (acfg->aot_opts.llvm_only)
		return;

	/* Don't make GOT global so accesses to it don't need relocations */
	sprintf (symbol, "%s", acfg->got_symbol);

#ifdef TARGET_MACH
	emit_unset_mode (acfg);
	fprintf (acfg->fp, ".section __DATA, __bss\n");
	emit_alignment (acfg, 8);
	if (acfg->llvm)
		emit_info_symbol (acfg, "jit_got");
	fprintf (acfg->fp, ".lcomm %s, %d\n", acfg->got_symbol, (int)(acfg->got_offset * sizeof (gpointer)));
#else
	emit_section_change (acfg, ".bss", 0);
	emit_alignment (acfg, 8);
	if (acfg->aot_opts.write_symbols)
		emit_local_symbol (acfg, symbol, "got_end", FALSE);
	emit_label (acfg, symbol);
	if (acfg->llvm)
		emit_info_symbol (acfg, "jit_got");
	if (acfg->got_offset > 0)
		emit_zero_bytes (acfg, (int)(acfg->got_offset * sizeof (gpointer)));
#endif

	sprintf (symbol, "got_end");
	emit_label (acfg, symbol);
}

typedef struct GlobalsTableEntry {
	guint32 value, index;
	struct GlobalsTableEntry *next;
} GlobalsTableEntry;

#ifdef TARGET_WIN32_MSVC
#define DLL_ENTRY_POINT "DllMain"

static void
emit_library_info (MonoAotCompile *acfg)
{
	// Only include for shared libraries linked directly from generated object.
	if (link_shared_library (acfg)) {
		char	*name = NULL;
		char	symbol [MAX_SYMBOL_SIZE];

		// Ask linker to export all global symbols.
		emit_section_change (acfg, ".drectve", 0);
		for (guint i = 0; i < acfg->globals->len; ++i) {
			name = (char *)g_ptr_array_index (acfg->globals, i);
			g_assert (name != NULL);
			sprintf_s (symbol, MAX_SYMBOL_SIZE, " /EXPORT:%s", name);
			emit_string (acfg, symbol);
		}

		// Emit DLLMain function, needed by MSVC linker for DLL's.
		// NOTE, DllMain should not go into exports above.
		emit_section_change (acfg, ".text", 0);
		emit_global (acfg, DLL_ENTRY_POINT, TRUE);
		emit_label (acfg, DLL_ENTRY_POINT);

		// Simple implementation of DLLMain, just returning TRUE.
		// For more information about DLLMain: https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583(v=vs.85).aspx
		fprintf (acfg->fp, "movl $1, %%eax\n");
		fprintf (acfg->fp, "ret\n");

		// Inform linker about our dll entry function.
		emit_section_change (acfg, ".drectve", 0);
		emit_string (acfg, "/ENTRY:" DLL_ENTRY_POINT);
		return;
	}
}

#else

static inline void
emit_library_info (MonoAotCompile *acfg)
{
	return;
}
#endif

static void
emit_globals (MonoAotCompile *acfg)
{
	int i, table_size;
	guint32 hash;
	GPtrArray *table;
	char symbol [1024];
	GlobalsTableEntry *entry, *new_entry;

	if (!acfg->aot_opts.static_link)
		return;

	if (acfg->aot_opts.llvm_only) {
		g_assert (acfg->globals->len == 0);
		return;
	}

	/* 
	 * When static linking, we emit a table containing our globals.
	 */

	/*
	 * Construct a chained hash table for mapping global names to their index in
	 * the globals table.
	 */
	table_size = g_spaced_primes_closest ((int)(acfg->globals->len * 1.5));
	table = g_ptr_array_sized_new (table_size);
	for (i = 0; i < table_size; ++i)
		g_ptr_array_add (table, NULL);
	for (i = 0; i < acfg->globals->len; ++i) {
		char *name = (char *)g_ptr_array_index (acfg->globals, i);

		hash = mono_metadata_str_hash (name) % table_size;

		/* FIXME: Allocate from the mempool */
		new_entry = g_new0 (GlobalsTableEntry, 1);
		new_entry->value = i;

		entry = (GlobalsTableEntry *)g_ptr_array_index (table, hash);
		if (entry == NULL) {
			new_entry->index = hash;
			g_ptr_array_index (table, hash) = new_entry;
		} else {
			while (entry->next)
				entry = entry->next;
			
			entry->next = new_entry;
			new_entry->index = table->len;
			g_ptr_array_add (table, new_entry);
		}
	}

	/* Emit the table */
	sprintf (symbol, ".Lglobals_hash");
	emit_section_change (acfg, RODATA_SECT, 0);
	emit_alignment (acfg, 8);
	emit_label (acfg, symbol);

	/* FIXME: Optimize memory usage */
	g_assert (table_size < 65000);
	emit_int16 (acfg, table_size);
	for (i = 0; i < table->len; ++i) {
		GlobalsTableEntry *entry = (GlobalsTableEntry *)g_ptr_array_index (table, i);

		if (entry == NULL) {
			emit_int16 (acfg, 0);
			emit_int16 (acfg, 0);
		} else {
			emit_int16 (acfg, entry->value + 1);
			if (entry->next)
				emit_int16 (acfg, entry->next->index);
			else
				emit_int16 (acfg, 0);
		}
	}

	/* Emit the names */
	for (i = 0; i < acfg->globals->len; ++i) {
		char *name = (char *)g_ptr_array_index (acfg->globals, i);

		sprintf (symbol, "name_%d", i);
		emit_section_change (acfg, RODATA_SECT, 1);
#ifdef TARGET_MACH
		emit_alignment (acfg, 4);
#endif
		emit_label (acfg, symbol);
		emit_string (acfg, name);
	}

	/* Emit the globals table */
	sprintf (symbol, "globals");
	emit_section_change (acfg, ".data", 0);
	/* This is not a global, since it is accessed by the init function */
	emit_alignment (acfg, 8);
	emit_info_symbol (acfg, symbol);

	sprintf (symbol, "%sglobals_hash", acfg->temp_prefix);
	emit_pointer (acfg, symbol);

	for (i = 0; i < acfg->globals->len; ++i) {
		char *name = (char *)g_ptr_array_index (acfg->globals, i);

		sprintf (symbol, "name_%d", i);
		emit_pointer (acfg, symbol);

		g_assert (strlen (name) < sizeof (symbol));
		sprintf (symbol, "%s", name);
		emit_pointer (acfg, symbol);
	}
	/* Null terminate the table */
	emit_int32 (acfg, 0);
	emit_int32 (acfg, 0);
}

static void
emit_mem_end (MonoAotCompile *acfg)
{
	char symbol [128];

	if (acfg->aot_opts.llvm_only)
		return;

	sprintf (symbol, "mem_end");
	emit_section_change (acfg, ".text", 1);
	emit_alignment_code (acfg, 8);
	emit_label (acfg, symbol);
}

static void
init_aot_file_info (MonoAotCompile *acfg, MonoAotFileInfo *info)
{
	int i;

	info->version = MONO_AOT_FILE_VERSION;
	info->plt_got_offset_base = acfg->plt_got_offset_base;
	info->got_size = acfg->got_offset * sizeof (gpointer);
	info->plt_size = acfg->plt_offset;
	info->nmethods = acfg->nmethods;
	info->flags = acfg->flags;
	info->opts = acfg->opts;
	info->simd_opts = acfg->simd_opts;
	info->gc_name_index = acfg->gc_name_offset;
	info->datafile_size = acfg->datafile_offset;
	for (i = 0; i < MONO_AOT_TABLE_NUM; ++i)
		info->table_offsets [i] = acfg->table_offsets [i];
	for (i = 0; i < MONO_AOT_TRAMP_NUM; ++i)
		info->num_trampolines [i] = acfg->num_trampolines [i];
	for (i = 0; i < MONO_AOT_TRAMP_NUM; ++i)
		info->trampoline_got_offset_base [i] = acfg->trampoline_got_offset_base [i];
	for (i = 0; i < MONO_AOT_TRAMP_NUM; ++i)
		info->trampoline_size [i] = acfg->trampoline_size [i];
	info->num_rgctx_fetch_trampolines = acfg->aot_opts.nrgctx_fetch_trampolines;

	info->double_align = MONO_ABI_ALIGNOF (double);
	info->long_align = MONO_ABI_ALIGNOF (gint64);
	info->generic_tramp_num = MONO_TRAMPOLINE_NUM;
	info->tramp_page_size = acfg->tramp_page_size;
	info->nshared_got_entries = acfg->nshared_got_entries;
	for (i = 0; i < MONO_AOT_TRAMP_NUM; ++i)
		info->tramp_page_code_offsets [i] = acfg->tramp_page_code_offsets [i];

	memcpy(&info->aotid, acfg->image->aotid, 16);
}

static void
emit_aot_file_info (MonoAotCompile *acfg, MonoAotFileInfo *info)
{
	char symbol [MAX_SYMBOL_SIZE];
	int i, sindex;
	const char **symbols;

	symbols = g_new0 (const char *, MONO_AOT_FILE_INFO_NUM_SYMBOLS);
	sindex = 0;
	symbols [sindex ++] = acfg->got_symbol;
	if (acfg->llvm) {
		symbols [sindex ++] = g_strdup_printf ("%s%s", acfg->user_symbol_prefix, acfg->llvm_got_symbol);
		symbols [sindex ++] = acfg->llvm_eh_frame_symbol;
	} else {
		symbols [sindex ++] = NULL;
		symbols [sindex ++] = NULL;
	}
	/* llvm_get_method */
	symbols [sindex ++] = NULL;
	/* llvm_get_unbox_tramp */
	symbols [sindex ++] = NULL;
	if (!acfg->aot_opts.llvm_only) {
		symbols [sindex ++] = "jit_code_start";
		symbols [sindex ++] = "jit_code_end";
		symbols [sindex ++] = "method_addresses";
	} else {
		symbols [sindex ++] = NULL;
		symbols [sindex ++] = NULL;
		symbols [sindex ++] = NULL;
	}
	if (acfg->data_outfile) {
		for (i = 0; i < MONO_AOT_TABLE_NUM; ++i)
			symbols [sindex ++] = NULL;
	} else {
		symbols [sindex ++] = "blob";
		symbols [sindex ++] = "class_name_table";
		symbols [sindex ++] = "class_info_offsets";
		symbols [sindex ++] = "method_info_offsets";
		symbols [sindex ++] = "ex_info_offsets";
		symbols [sindex ++] = "extra_method_info_offsets";
		symbols [sindex ++] = "extra_method_table";
		symbols [sindex ++] = "got_info_offsets";
		if (acfg->llvm)
			symbols [sindex ++] = "llvm_got_info_offsets";
		else
			symbols [sindex ++] = NULL;
		symbols [sindex ++] = "image_table";
	}
	symbols [sindex ++] = "mem_end";
	symbols [sindex ++] = "assembly_guid";
	symbols [sindex ++] = "runtime_version";
	if (acfg->num_trampoline_got_entries) {
		symbols [sindex ++] = "specific_trampolines";
		symbols [sindex ++] = "static_rgctx_trampolines";
		symbols [sindex ++] = "imt_trampolines";
		symbols [sindex ++] = "gsharedvt_arg_trampolines";
	} else {
		symbols [sindex ++] = NULL;
		symbols [sindex ++] = NULL;
		symbols [sindex ++] = NULL;
		symbols [sindex ++] = NULL;
	}
	if (acfg->aot_opts.static_link) {
		symbols [sindex ++] = "globals";
	} else {
		symbols [sindex ++] = NULL;
	}
	symbols [sindex ++] = "assembly_name";
	symbols [sindex ++] = "plt";
	symbols [sindex ++] = "plt_end";
	symbols [sindex ++] = "unwind_info";
	if (!acfg->aot_opts.llvm_only) {
		symbols [sindex ++] = "unbox_trampolines";
		symbols [sindex ++] = "unbox_trampolines_end";
		symbols [sindex ++] = "unbox_trampoline_addresses";
	} else {
		symbols [sindex ++] = NULL;
		symbols [sindex ++] = NULL;
		symbols [sindex ++] = NULL;
	}

	g_assert (sindex == MONO_AOT_FILE_INFO_NUM_SYMBOLS);

	sprintf (symbol, "%smono_aot_file_info", acfg->user_symbol_prefix);
	emit_section_change (acfg, ".data", 0);
	emit_alignment (acfg, 8);
	emit_label (acfg, symbol);
	if (!acfg->aot_opts.static_link)
		emit_global (acfg, symbol, FALSE);

	/* The data emitted here must match MonoAotFileInfo. */

	emit_int32 (acfg, info->version);
	emit_int32 (acfg, info->dummy);

	/* 
	 * We emit pointers to our data structures instead of emitting global symbols which
	 * point to them, to reduce the number of globals, and because using globals leads to
	 * various problems (i.e. arm/thumb).
	 */
	for (i = 0; i < MONO_AOT_FILE_INFO_NUM_SYMBOLS; ++i)
		emit_pointer (acfg, symbols [i]);

	emit_int32 (acfg, info->plt_got_offset_base);
	emit_int32 (acfg, info->got_size);
	emit_int32 (acfg, info->plt_size);
	emit_int32 (acfg, info->nmethods);
	emit_int32 (acfg, info->flags);
	emit_int32 (acfg, info->opts);
	emit_int32 (acfg, info->simd_opts);
	emit_int32 (acfg, info->gc_name_index);
	emit_int32 (acfg, info->num_rgctx_fetch_trampolines);
	emit_int32 (acfg, info->double_align);
	emit_int32 (acfg, info->long_align);
	emit_int32 (acfg, info->generic_tramp_num);
	emit_int32 (acfg, info->tramp_page_size);
	emit_int32 (acfg, info->nshared_got_entries);
	emit_int32 (acfg, info->datafile_size);

	for (i = 0; i < MONO_AOT_TABLE_NUM; ++i)
		emit_int32 (acfg, info->table_offsets [i]);
	for (i = 0; i < MONO_AOT_TRAMP_NUM; ++i)
		emit_int32 (acfg, info->num_trampolines [i]);
	for (i = 0; i < MONO_AOT_TRAMP_NUM; ++i)
		emit_int32 (acfg, info->trampoline_got_offset_base [i]);
	for (i = 0; i < MONO_AOT_TRAMP_NUM; ++i)
		emit_int32 (acfg, info->trampoline_size [i]);
	for (i = 0; i < MONO_AOT_TRAMP_NUM; ++i)
		emit_int32 (acfg, info->tramp_page_code_offsets [i]);

	emit_bytes (acfg, info->aotid, 16);

	if (acfg->aot_opts.static_link) {
		emit_global_inner (acfg, acfg->static_linking_symbol, FALSE);
		emit_alignment (acfg, sizeof (gpointer));
		emit_label (acfg, acfg->static_linking_symbol);
		emit_pointer_2 (acfg, acfg->user_symbol_prefix, "mono_aot_file_info");
	}
}

/*
 * Emit a structure containing all the information not stored elsewhere.
 */
static void
emit_file_info (MonoAotCompile *acfg)
{
	char *build_info;
	MonoAotFileInfo *info;

	if (acfg->aot_opts.bind_to_runtime_version) {
		build_info = mono_get_runtime_build_info ();
		emit_string_symbol (acfg, "runtime_version", build_info);
		g_free (build_info);
	} else {
		emit_string_symbol (acfg, "runtime_version", "");
	}

	emit_string_symbol (acfg, "assembly_guid" , acfg->image->guid);

	/* Emit a string holding the assembly name */
	emit_string_symbol (acfg, "assembly_name", acfg->image->assembly->aname.name);

	info = g_new0 (MonoAotFileInfo, 1);
	init_aot_file_info (acfg, info);

	if (acfg->aot_opts.static_link) {
		char symbol [MAX_SYMBOL_SIZE];
		char *p;

		/*
		 * Emit a global symbol which can be passed by an embedding app to
		 * mono_aot_register_module (). The symbol points to a pointer to the the file info
		 * structure.
		 */
		sprintf (symbol, "%smono_aot_module_%s_info", acfg->user_symbol_prefix, acfg->image->assembly->aname.name);

		/* Get rid of characters which cannot occur in symbols */
		p = symbol;
		for (p = symbol; *p; ++p) {
			if (!(isalnum (*p) || *p == '_'))
				*p = '_';
		}
		acfg->static_linking_symbol = g_strdup (symbol);
	}

	if (acfg->llvm)
		mono_llvm_emit_aot_file_info (info, acfg->has_jitted_code);
	else
		emit_aot_file_info (acfg, info);
}

static void
emit_blob (MonoAotCompile *acfg)
{
	acfg->blob_closed = TRUE;

	emit_aot_data (acfg, MONO_AOT_TABLE_BLOB, "blob", (guint8*)acfg->blob.data, acfg->blob.index);
}

static void
emit_objc_selectors (MonoAotCompile *acfg)
{
	int i;
	char symbol [128];

	if (!acfg->objc_selectors || acfg->objc_selectors->len == 0)
		return;

	/*
	 * From
	 * cat > foo.m << EOF
	 * void *ret ()
	 * {
	 * return @selector(print:);
	 * }
	 * EOF
	 */

	mono_img_writer_emit_unset_mode (acfg->w);
	g_assert (acfg->fp);
	fprintf (acfg->fp, ".section	__DATA,__objc_selrefs,literal_pointers,no_dead_strip\n");
	fprintf (acfg->fp, ".align	3\n");
	for (i = 0; i < acfg->objc_selectors->len; ++i) {
		sprintf (symbol, "L_OBJC_SELECTOR_REFERENCES_%d", i);
		emit_label (acfg, symbol);
		sprintf (symbol, "L_OBJC_METH_VAR_NAME_%d", i);
		emit_pointer (acfg, symbol);

	}
	fprintf (acfg->fp, ".section	__TEXT,__cstring,cstring_literals\n");
	for (i = 0; i < acfg->objc_selectors->len; ++i) {
		fprintf (acfg->fp, "L_OBJC_METH_VAR_NAME_%d:\n", i);
		fprintf (acfg->fp, ".asciz \"%s\"\n", (char*)g_ptr_array_index (acfg->objc_selectors, i));
	}

	fprintf (acfg->fp, ".section	__DATA,__objc_imageinfo,regular,no_dead_strip\n");
	fprintf (acfg->fp, ".align	3\n");
	fprintf (acfg->fp, "L_OBJC_IMAGE_INFO:\n");
	fprintf (acfg->fp, ".long	0\n");
	fprintf (acfg->fp, ".long	16\n");
}

static void
emit_dwarf_info (MonoAotCompile *acfg)
{
#ifdef EMIT_DWARF_INFO
	int i;
	char symbol2 [128];

	/* DIEs for methods */
	for (i = 0; i < acfg->nmethods; ++i) {
		MonoCompile *cfg = acfg->cfgs [i];

		if (!cfg)
			continue;

		// FIXME: LLVM doesn't define .Lme_...
		if (cfg->compile_llvm)
			continue;

		sprintf (symbol2, "%sme_%x", acfg->temp_prefix, i);

		mono_dwarf_writer_emit_method (acfg->dwarf, cfg, cfg->method, cfg->asm_symbol, symbol2, cfg->asm_debug_symbol, (guint8 *)cfg->jit_info->code_start, cfg->jit_info->code_size, cfg->args, cfg->locals, cfg->unwind_ops, mono_debug_find_method (cfg->jit_info->d.method, mono_domain_get ()));
	}
#endif
}

static gboolean
collect_methods (MonoAotCompile *acfg)
{
	int mindex, i;
	MonoImage *image = acfg->image;

	/* Collect methods */
	for (i = 0; i < image->tables [MONO_TABLE_METHOD].rows; ++i) {
		MonoError error;
		MonoMethod *method;
		guint32 token = MONO_TOKEN_METHOD_DEF | (i + 1);

		method = mono_get_method_checked (acfg->image, token, NULL, NULL, &error);

		if (!method) {
			aot_printerrf (acfg, "Failed to load method 0x%x from '%s' due to %s.\n", token, image->name, mono_error_get_message (&error));
			aot_printerrf (acfg, "Run with MONO_LOG_LEVEL=debug for more information.\n");
			mono_error_cleanup (&error);
			return FALSE;
		}
			
		/* Load all methods eagerly to skip the slower lazy loading code */
		mono_class_setup_methods (method->klass);

		if (mono_aot_mode_is_full (&acfg->aot_opts) && method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) {
			/* Compile the wrapper instead */
			/* We do this here instead of add_wrappers () because it is easy to do it here */
			MonoMethod *wrapper = mono_marshal_get_native_wrapper (method, TRUE, TRUE);
			method = wrapper;
		}

		/* FIXME: Some mscorlib methods don't have debug info */
		/*
		if (acfg->aot_opts.soft_debug && !method->wrapper_type) {
			if (!((method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
				  (method->iflags & METHOD_IMPL_ATTRIBUTE_RUNTIME) ||
				  (method->flags & METHOD_ATTRIBUTE_ABSTRACT) ||
				  (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL))) {
				if (!mono_debug_lookup_method (method)) {
					fprintf (stderr, "Method %s has no debug info, probably the .mdb file for the assembly is missing.\n", mono_method_get_full_name (method));
					exit (1);
				}
			}
		}
		*/

		if (method->is_generic || mono_class_is_gtd (method->klass))
			/* Compile the ref shared version instead */
			method = mini_get_shared_method (method);

		/* Since we add the normal methods first, their index will be equal to their zero based token index */
		add_method_with_index (acfg, method, i, FALSE);
		acfg->method_index ++;
	}

	/* gsharedvt methods */
	for (mindex = 0; mindex < image->tables [MONO_TABLE_METHOD].rows; ++mindex) {
		MonoError error;
		MonoMethod *method;
		guint32 token = MONO_TOKEN_METHOD_DEF | (mindex + 1);

		if (!(acfg->opts & MONO_OPT_GSHAREDVT))
			continue;

		method = mono_get_method_checked (acfg->image, token, NULL, NULL, &error);
		report_loader_error (acfg, &error, "Failed to load method token 0x%x due to %s\n", i, mono_error_get_message (&error));

		if (method->is_generic || mono_class_is_gtd (method->klass)) {
			MonoMethod *gshared;

			gshared = mini_get_shared_method_full (method, TRUE, TRUE);
			add_extra_method (acfg, gshared);
		}
	}

	if (mono_aot_mode_is_full (&acfg->aot_opts))
		add_generic_instances (acfg);

	if (mono_aot_mode_is_full (&acfg->aot_opts))
		add_wrappers (acfg);
	return TRUE;
}

static void
compile_methods (MonoAotCompile *acfg)
{
	int i, methods_len;

	if (acfg->aot_opts.nthreads > 0) {
		GPtrArray *frag;
		int len, j;
		GPtrArray *threads;
		MonoThreadHandle *thread_handle;
		gpointer *user_data;
		MonoMethod **methods;

		methods_len = acfg->methods->len;

		len = acfg->methods->len / acfg->aot_opts.nthreads;
		g_assert (len > 0);
		/* 
		 * Partition the list of methods into fragments, and hand it to threads to
		 * process.
		 */
		threads = g_ptr_array_new ();
		/* Make a copy since acfg->methods is modified by compile_method () */
		methods = g_new0 (MonoMethod*, methods_len);
		//memcpy (methods, g_ptr_array_index (acfg->methods, 0), sizeof (MonoMethod*) * methods_len);
		for (i = 0; i < methods_len; ++i)
			methods [i] = (MonoMethod *)g_ptr_array_index (acfg->methods, i);
		i = 0;
		while (i < methods_len) {
			frag = g_ptr_array_new ();
			for (j = 0; j < len; ++j) {
				if (i < methods_len) {
					g_ptr_array_add (frag, methods [i]);
					i ++;
				}
			}

			user_data = g_new0 (gpointer, 3);
			user_data [0] = mono_domain_get ();
			user_data [1] = acfg;
			user_data [2] = frag;
			
			thread_handle = mono_threads_create_thread (compile_thread_main, (gpointer) user_data, NULL, NULL);
			g_ptr_array_add (threads, thread_handle);
		}
		g_free (methods);

		for (i = 0; i < threads->len; ++i) {
			mono_thread_info_wait_one_handle (g_ptr_array_index (threads, i), MONO_INFINITE_WAIT, FALSE);
			mono_threads_close_thread_handle (g_ptr_array_index (threads, i));
		}
	} else {
		methods_len = 0;
	}

	/* Compile methods added by compile_method () or all methods if nthreads == 0 */
	for (i = methods_len; i < acfg->methods->len; ++i) {
		/* This can add new methods to acfg->methods */
		compile_method (acfg, (MonoMethod *)g_ptr_array_index (acfg->methods, i));
	}
}

static int
compile_asm (MonoAotCompile *acfg)
{
	char *command, *objfile;
	char *outfile_name, *tmp_outfile_name, *llvm_ofile;
	const char *tool_prefix = acfg->aot_opts.tool_prefix ? acfg->aot_opts.tool_prefix : "";
	char *ld_flags = acfg->aot_opts.ld_flags ? acfg->aot_opts.ld_flags : g_strdup("");

#ifdef TARGET_WIN32_MSVC
#define AS_OPTIONS "-c -x assembler"
#elif defined(TARGET_AMD64) && !defined(TARGET_MACH)
#define AS_OPTIONS "--64"
#elif defined(TARGET_POWERPC64)
#define AS_OPTIONS "-a64 -mppc64"
#elif defined(sparc) && SIZEOF_VOID_P == 8
#define AS_OPTIONS "-xarch=v9"
#elif defined(TARGET_X86) && defined(TARGET_MACH) && !defined(__native_client_codegen__)
#define AS_OPTIONS "-arch i386"
#else
#define AS_OPTIONS ""
#endif

#ifdef __native_client_codegen__
#if defined(TARGET_AMD64)
#define AS_NAME "nacl64-as"
#else
#define AS_NAME "nacl-as"
#endif
#elif defined(TARGET_OSX)
#define AS_NAME "clang"
#elif defined(TARGET_WIN32_MSVC)
#define AS_NAME "clang.exe"
#else
#define AS_NAME "as"
#endif

#ifdef TARGET_WIN32_MSVC
#define AS_OBJECT_FILE_SUFFIX "obj"
#else
#define AS_OBJECT_FILE_SUFFIX "o"
#endif

#if defined(sparc)
#define LD_NAME "ld"
#define LD_OPTIONS "-shared -G"
#elif defined(__ppc__) && defined(TARGET_MACH)
#define LD_NAME "gcc"
#define LD_OPTIONS "-dynamiclib"
#elif defined(TARGET_AMD64) && defined(TARGET_MACH)
#define LD_NAME "clang"
#define LD_OPTIONS "--shared"
#elif defined(TARGET_WIN32_MSVC)
#define LD_NAME "link.exe"
#define LD_OPTIONS "/DLL /MACHINE:X64 /NOLOGO"
#elif defined(TARGET_WIN32) && !defined(TARGET_ANDROID)
#define LD_NAME "gcc"
#define LD_OPTIONS "-shared"
#elif defined(TARGET_X86) && defined(TARGET_MACH) && !defined(__native_client_codegen__)
#define LD_NAME "clang"
#define LD_OPTIONS "-m32 -dynamiclib"
#elif defined(TARGET_ARM) && !defined(TARGET_ANDROID)
#define LD_NAME "gcc"
#define LD_OPTIONS "--shared"
#elif defined(TARGET_POWERPC64)
#define LD_OPTIONS "-m elf64ppc"
#endif

#ifndef LD_OPTIONS
#define LD_OPTIONS ""
#endif

	if (acfg->aot_opts.asm_only) {
		aot_printf (acfg, "Output file: '%s'.\n", acfg->tmpfname);
		if (acfg->aot_opts.static_link)
			aot_printf (acfg, "Linking symbol: '%s'.\n", acfg->static_linking_symbol);
		if (acfg->llvm)
			aot_printf (acfg, "LLVM output file: '%s'.\n", acfg->llvm_sfile);
		return 0;
	}

	if (acfg->aot_opts.static_link) {
		if (acfg->aot_opts.outfile)
			objfile = g_strdup_printf ("%s", acfg->aot_opts.outfile);
		else
			objfile = g_strdup_printf ("%s." AS_OBJECT_FILE_SUFFIX, acfg->image->name);
	} else {
		objfile = g_strdup_printf ("%s." AS_OBJECT_FILE_SUFFIX, acfg->tmpfname);
	}

#ifdef TARGET_OSX
	g_string_append (acfg->as_args, "-c -x assembler");
#endif

	command = g_strdup_printf ("\"%s%s\" %s %s -o %s %s", tool_prefix, AS_NAME, AS_OPTIONS,
			acfg->as_args ? acfg->as_args->str : "", 
			wrap_path (objfile), wrap_path (acfg->tmpfname));
	aot_printf (acfg, "Executing the native assembler: %s\n", command);
	if (execute_system (command) != 0) {
		g_free (command);
		g_free (objfile);
		return 1;
	}

	if (acfg->llvm && !acfg->llvm_owriter) {
		command = g_strdup_printf ("\"%s%s\" %s %s -o %s %s", tool_prefix, AS_NAME, AS_OPTIONS,
			acfg->as_args ? acfg->as_args->str : "",
			wrap_path (acfg->llvm_ofile), wrap_path (acfg->llvm_sfile));
		aot_printf (acfg, "Executing the native assembler: %s\n", command);
		if (execute_system (command) != 0) {
			g_free (command);
			g_free (objfile);
			return 1;
		}
	}

	g_free (command);

	if (acfg->aot_opts.static_link) {
		aot_printf (acfg, "Output file: '%s'.\n", objfile);
		aot_printf (acfg, "Linking symbol: '%s'.\n", acfg->static_linking_symbol);
		g_free (objfile);
		return 0;
	}

	if (acfg->aot_opts.outfile)
		outfile_name = g_strdup_printf ("%s", acfg->aot_opts.outfile);
	else
		outfile_name = g_strdup_printf ("%s%s", acfg->image->name, MONO_SOLIB_EXT);

	tmp_outfile_name = g_strdup_printf ("%s.tmp", outfile_name);

	if (acfg->llvm) {
		llvm_ofile = g_strdup_printf ("\"%s\"", acfg->llvm_ofile);
	} else {
		llvm_ofile = g_strdup ("");
	}

	/* replace the ; flags separators with spaces */
	g_strdelimit (ld_flags, ";", ' ');

	if (acfg->aot_opts.llvm_only)
		ld_flags = g_strdup_printf ("%s %s", ld_flags, "-lstdc++");

#ifdef TARGET_WIN32_MSVC
	g_assert (tmp_outfile_name != NULL);
	g_assert (objfile != NULL);
	command = g_strdup_printf ("\"%s%s\" %s %s /OUT:\"%s\" \"%s\"", tool_prefix, LD_NAME, LD_OPTIONS,
		ld_flags, tmp_outfile_name, objfile);
#elif defined(LD_NAME)
	command = g_strdup_printf ("%s%s %s -o %s %s %s %s", tool_prefix, LD_NAME, LD_OPTIONS,
		wrap_path (tmp_outfile_name), wrap_path (llvm_ofile),
		wrap_path (g_strdup_printf ("%s." AS_OBJECT_FILE_SUFFIX, acfg->tmpfname)), ld_flags);
#else
	// Default (linux)
	if (acfg->aot_opts.tool_prefix) {
		/* Cross compiling */
		command = g_strdup_printf ("\"%sld\" %s -shared -o %s %s %s %s", tool_prefix, LD_OPTIONS,
								   wrap_path (tmp_outfile_name), wrap_path (llvm_ofile),
								   wrap_path (g_strdup_printf ("%s." AS_OBJECT_FILE_SUFFIX, acfg->tmpfname)), ld_flags);
	} else {
		char *args = g_strdup_printf ("%s -shared -o %s %s %s %s", LD_OPTIONS,
									  wrap_path (tmp_outfile_name), wrap_path (llvm_ofile),
									  wrap_path (g_strdup_printf ("%s." AS_OBJECT_FILE_SUFFIX, acfg->tmpfname)), ld_flags);

		if (acfg->aot_opts.llvm_only) {
			command = g_strdup_printf ("clang++ %s", args);
		} else {
			command = g_strdup_printf ("\"%sld\" %s", tool_prefix, args);
		}
		g_free (args);
	}
#endif
	aot_printf (acfg, "Executing the native linker: %s\n", command);
	if (execute_system (command) != 0) {
		g_free (tmp_outfile_name);
		g_free (outfile_name);
		g_free (command);
		g_free (objfile);
		g_free (ld_flags);
		return 1;
	}

	g_free (command);

	/*com = g_strdup_printf ("strip --strip-unneeded %s%s", acfg->image->name, MONO_SOLIB_EXT);
	printf ("Stripping the binary: %s\n", com);
	execute_system (com);
	g_free (com);*/

#if defined(TARGET_ARM) && !defined(TARGET_MACH)
	/* 
	 * gas generates 'mapping symbols' each time code and data is mixed, which 
	 * happens a lot in emit_and_reloc_code (), so we need to get rid of them.
	 */
	command = g_strdup_printf ("\"%sstrip\" --strip-symbol=\\$a --strip-symbol=\\$d %s", wrap_path(tool_prefix), wrap_path(tmp_outfile_name));
	aot_printf (acfg, "Stripping the binary: %s\n", command);
	if (execute_system (command) != 0) {
		g_free (tmp_outfile_name);
		g_free (outfile_name);
		g_free (command);
		g_free (objfile);
		return 1;
	}
#endif

	if (0 != rename (tmp_outfile_name, outfile_name)) {
		if (G_FILE_ERROR_EXIST == g_file_error_from_errno (errno)) {
			/* Since we are rebuilding the module we need to be able to replace any old copies. Remove old file and retry rename operation. */
			unlink (outfile_name);
			rename (tmp_outfile_name, outfile_name);
		}
	}

#if defined(TARGET_MACH)
	command = g_strdup_printf ("dsymutil \"%s\"", outfile_name);
	aot_printf (acfg, "Executing dsymutil: %s\n", command);
	if (execute_system (command) != 0) {
		return 1;
	}
#endif

	if (!acfg->aot_opts.save_temps)
		unlink (objfile);

	g_free (tmp_outfile_name);
	g_free (outfile_name);
	g_free (objfile);

	if (acfg->aot_opts.save_temps)
		aot_printf (acfg, "Retained input file.\n");
	else
		unlink (acfg->tmpfname);

	return 0;
}

static guint8
profread_byte (FILE *infile)
{
	guint8 i;
	int res;

	res = fread (&i, 1, 1, infile);
	g_assert (res == 1);
	return i;
}

static int
profread_int (FILE *infile)
{
	int i, res;

	res = fread (&i, 4, 1, infile);
	g_assert (res == 1);
	return i;
}

static char*
profread_string (FILE *infile)
{
	int len, res;
	char buf [1024];
	char *pbuf;

	len = profread_int (infile);
	if (len + 1 > 1024)
		pbuf = g_malloc (len + 1);
	else
		pbuf = buf;
	res = fread (pbuf, 1, len, infile);
	g_assert (res == len);
	pbuf [len] = '\0';
	if (pbuf == buf)
		return g_strdup (buf);
	else
		return pbuf;
}

static void
load_profile_file (MonoAotCompile *acfg, char *filename)
{
	FILE *infile;
	char buf [1024];
	int res, len, version;
	char magic [32];

	infile = fopen (filename, "r");
	if (!infile) {
		fprintf (stderr, "Unable to open file '%s': %s.\n", filename, strerror (errno));
		exit (1);
	}

	printf ("Using profile data file '%s'\n", filename);

	sprintf (magic, AOT_PROFILER_MAGIC);
	len = strlen (magic);
	res = fread (buf, 1, len, infile);
	magic [len] = '\0';
	buf [len] = '\0';
	if ((res != len) || strcmp (buf, magic) != 0) {
		printf ("Profile file has wrong header: '%s'.\n", buf);
		fclose (infile);
		exit (1);
	}
	guint32 expected_version = (AOT_PROFILER_MAJOR_VERSION << 16) | AOT_PROFILER_MINOR_VERSION;
	version = profread_int (infile);
	if (version != expected_version) {
		printf ("Profile file has wrong version 0x%4x, expected 0x%4x.\n", version, expected_version);
		fclose (infile);
		exit (1);
	}

	ProfileData *data = g_new0 (ProfileData, 1);
	data->images = g_hash_table_new (NULL, NULL);
	data->classes = g_hash_table_new (NULL, NULL);
	data->ginsts = g_hash_table_new (NULL, NULL);
	data->methods = g_hash_table_new (NULL, NULL);

	while (TRUE) {
		int type = profread_byte (infile);
		int id = profread_int (infile);

		if (type == AOTPROF_RECORD_NONE)
			break;

		switch (type) {
		case AOTPROF_RECORD_IMAGE: {
			ImageProfileData *idata = g_new0 (ImageProfileData, 1);
			idata->name = profread_string (infile);
			char *mvid = profread_string (infile);
			g_free (mvid);
			g_hash_table_insert (data->images, GINT_TO_POINTER (id), idata);
			break;
		}
		case AOTPROF_RECORD_GINST: {
			int i;
			int len = profread_int (infile);

			GInstProfileData *gdata = g_new0 (GInstProfileData, 1);
			gdata->argc = len;
			gdata->argv = g_new0 (ClassProfileData*, len);

			for (i = 0; i < len; ++i) {
				int class_id = profread_int (infile);

				gdata->argv [i] = g_hash_table_lookup (data->classes, GINT_TO_POINTER (class_id));
				g_assert (gdata->argv [i]);
			}
			g_hash_table_insert (data->ginsts, GINT_TO_POINTER (id), gdata);
			break;
		}
		case AOTPROF_RECORD_TYPE: {
			int type = profread_byte (infile);

			switch (type) {
			case MONO_TYPE_CLASS: {
				int image_id = profread_int (infile);
				int ginst_id = profread_int (infile);
				char *class_name = profread_string (infile);

				ImageProfileData *image = g_hash_table_lookup (data->images, GINT_TO_POINTER (image_id));
				g_assert (image);

				char *p = strrchr (class_name, '.');
				g_assert (p);
				*p = '\0';

				ClassProfileData *cdata = g_new0 (ClassProfileData, 1);
				cdata->image = image;
				cdata->ns = g_strdup (class_name);
				cdata->name = g_strdup (p + 1);

				if (ginst_id != -1) {
					cdata->inst = g_hash_table_lookup (data->ginsts, GINT_TO_POINTER (ginst_id));
					g_assert (cdata->inst);
				}
				g_free (class_name);

				g_hash_table_insert (data->classes, GINT_TO_POINTER (id), cdata);
				break;
			}
#if 0
			case MONO_TYPE_SZARRAY: {
				int elem_id = profread_int (infile);
				// FIXME:
				break;
			}
#endif
			default:
				g_assert_not_reached ();
				break;
			}
			break;
		}
		case AOTPROF_RECORD_METHOD: {
			int class_id = profread_int (infile);
			int ginst_id = profread_int (infile);
			int param_count = profread_int (infile);
			char *method_name = profread_string (infile);
			char *sig = profread_string (infile);

			ClassProfileData *klass = g_hash_table_lookup (data->classes, GINT_TO_POINTER (class_id));
			g_assert (klass);

			MethodProfileData *mdata = g_new0 (MethodProfileData, 1);
			mdata->id = id;
			mdata->klass = klass;
			mdata->name = method_name;
			mdata->signature = sig;
			mdata->param_count = param_count;

			if (ginst_id != -1) {
				mdata->inst = g_hash_table_lookup (data->ginsts, GINT_TO_POINTER (ginst_id));
				g_assert (mdata->inst);
			}
			g_hash_table_insert (data->methods, GINT_TO_POINTER (id), mdata);
			break;
		}
		default:
			printf ("%d\n", type);
			g_assert_not_reached ();
			break;
		}
	}

	fclose (infile);
	acfg->profile_data = g_list_append (acfg->profile_data, data);
}

static void
resolve_class (ClassProfileData *cdata);

static void
resolve_ginst (GInstProfileData *inst_data)
{
	int i;

	if (inst_data->inst)
		return;

	for (i = 0; i < inst_data->argc; ++i) {
		resolve_class (inst_data->argv [i]);
		if (!inst_data->argv [i]->klass)
			return;
	}
	MonoType **args = g_new0 (MonoType*, inst_data->argc);
	for (i = 0; i < inst_data->argc; ++i)
		args [i] = &inst_data->argv [i]->klass->byval_arg;

	inst_data->inst = mono_metadata_get_generic_inst (inst_data->argc, args);
}

static void
resolve_class (ClassProfileData *cdata)
{
	MonoError error;
	MonoClass *klass;

	if (!cdata->image->image)
		return;

	klass = mono_class_from_name_checked (cdata->image->image, cdata->ns, cdata->name, &error);
	if (!klass) {
		//printf ("[%s] %s.%s\n", cdata->image->name, cdata->ns, cdata->name);
		return;
	}
	if (cdata->inst) {
		resolve_ginst (cdata->inst);
		if (!cdata->inst->inst)
			return;
		MonoGenericContext ctx;

		memset (&ctx, 0, sizeof (ctx));
		ctx.class_inst = cdata->inst->inst;
		cdata->klass = mono_class_inflate_generic_class_checked (klass, &ctx, &error);
	} else {
		cdata->klass = klass;
	}
}

/*
 * Resolve the profile data to the corresponding loaded classes/methods etc. if possible.
 */
static void
resolve_profile_data (MonoAotCompile *acfg, ProfileData *data)
{
	GHashTableIter iter;
	gpointer key, value;
	int i;

	if (!data)
		return;

	/* Images */
	GPtrArray *assemblies = mono_domain_get_assemblies (mono_get_root_domain (), FALSE);
	g_hash_table_iter_init (&iter, data->images);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		ImageProfileData *idata = (ImageProfileData*)value;

		for (i = 0; i < assemblies->len; ++i) {
			MonoAssembly *ass = g_ptr_array_index (assemblies, i);

			if (!strcmp (ass->aname.name, idata->name)) {
				idata->image = ass->image;
				break;
			}
		}
	}
	g_ptr_array_free (assemblies, TRUE);

	/* Classes */
	g_hash_table_iter_init (&iter, data->classes);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		ClassProfileData *cdata = (ClassProfileData*)value;

		if (!cdata->image->image) {
			if (acfg->aot_opts.verbose)
				printf ("Unable to load class '%s.%s' because its image '%s' is not loaded.\n", cdata->ns, cdata->name, cdata->image->name);
			continue;
		}

		resolve_class (cdata);
		/*
		if (cdata->klass)
			printf ("%s %s %s\n", cdata->ns, cdata->name, mono_class_full_name (cdata->klass));
		*/
	}

	/* Methods */
	g_hash_table_iter_init (&iter, data->methods);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		MethodProfileData *mdata = (MethodProfileData*)value;
		MonoClass *klass;
		MonoMethod *m;
		gpointer miter;

		resolve_class (mdata->klass);
		klass = mdata->klass->klass;
		if (!klass) {
			if (acfg->aot_opts.verbose)
				printf ("Unable to load method '%s' because its class '%s.%s' is not loaded.\n", mdata->name, mdata->klass->ns, mdata->klass->name);
			continue;
		}
		miter = NULL;
		while ((m = mono_class_get_methods (klass, &miter))) {
			MonoError error;

			if (strcmp (m->name, mdata->name))
				continue;
			MonoMethodSignature *sig = mono_method_signature (m);
			if (!sig)
				continue;
			if (sig->param_count != mdata->param_count)
				continue;
			if (mdata->inst) {
				resolve_ginst (mdata->inst);
				if (!mdata->inst->inst)
					continue;
				MonoGenericContext ctx;

				memset (&ctx, 0, sizeof (ctx));
				ctx.method_inst = mdata->inst->inst;

				m = mono_class_inflate_generic_method_checked (m, &ctx, &error);
				if (!m)
					continue;
				sig = mono_method_signature_checked (m, &error);
				if (!is_ok (&error)) {
					mono_error_cleanup (&error);
					continue;
				}
			}
			char *sig_str = mono_signature_full_name (sig);
			gboolean match = !strcmp (sig_str, mdata->signature);
			g_free (sig_str);
			if (!match)

				continue;
			//printf ("%s\n", mono_method_full_name (m, 1));
			mdata->method = m;
			break;
		}
		if (!mdata->method) {
			if (acfg->aot_opts.verbose)
				printf ("Unable to load method '%s' from class '%s', not found.\n", mdata->name, mono_class_full_name (klass));
		}
	}
}

static gboolean
inst_references_image (MonoGenericInst *inst, MonoImage *image)
{
	int i;

	for (i = 0; i < inst->type_argc; ++i) {
		MonoClass *k = mono_class_from_mono_type (inst->type_argv [i]);
		if (k->image == image)
			return TRUE;
		if (mono_class_is_ginst (k)) {
			MonoGenericInst *kinst = mono_class_get_context (k)->class_inst;
			if (inst_references_image (kinst, image))
				return TRUE;
		}
	}
	return FALSE;
}

static gboolean
is_local_inst (MonoGenericInst *inst, MonoImage *image)
{
	int i;

	for (i = 0; i < inst->type_argc; ++i) {
		MonoClass *k = mono_class_from_mono_type (inst->type_argv [i]);
		if (!MONO_TYPE_IS_PRIMITIVE (inst->type_argv [i]) && k->image != image)
			return FALSE;
	}
	return TRUE;
}

static void
add_profile_instances (MonoAotCompile *acfg, ProfileData *data)
{
	GHashTableIter iter;
	gpointer key, value;
	int count = 0;

	if (!data)
		return;

	if (acfg->aot_opts.profile_only) {
		/* Add methods referenced by the profile */
		g_hash_table_iter_init (&iter, data->methods);
		while (g_hash_table_iter_next (&iter, &key, &value)) {
			MethodProfileData *mdata = (MethodProfileData*)value;
			MonoMethod *m = mdata->method;

			if (!m)
				continue;
			if (m->is_inflated)
				continue;
			add_extra_method (acfg, m);
			g_hash_table_insert (acfg->profile_methods, m, m);
			count ++;
		}
	}

	/*
	 * Add method instances 'related' to this assembly to the AOT image.
	 */
	g_hash_table_iter_init (&iter, data->methods);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		MethodProfileData *mdata = (MethodProfileData*)value;
		MonoMethod *m = mdata->method;
		MonoGenericContext *ctx;

		if (!m)
			continue;
		if (!m->is_inflated)
			continue;

		ctx = mono_method_get_context (m);
		/* For simplicity, add instances which reference the assembly we are compiling */
		if (((ctx->class_inst && inst_references_image (ctx->class_inst, acfg->image)) ||
			 (ctx->method_inst && inst_references_image (ctx->method_inst, acfg->image))) &&
			!mono_method_is_generic_sharable_full (m, FALSE, FALSE, FALSE)) {
			//printf ("%s\n", mono_method_full_name (m, TRUE));
			add_extra_method (acfg, m);
			count ++;
		} else if (m->klass->image == acfg->image &&
			((ctx->class_inst && is_local_inst (ctx->class_inst, acfg->image)) ||
			 (ctx->method_inst && is_local_inst (ctx->method_inst, acfg->image))) &&
			!mono_method_is_generic_sharable_full (m, FALSE, FALSE, FALSE))  {
			/* Add instances where the gtd is in the assembly and its inflated with types from this assembly or corlib */
			//printf ("%s\n", mono_method_full_name (m, TRUE));
			add_extra_method (acfg, m);
			count ++;
		}
		/*
		 * FIXME: We might skip some instances, for example:
		 * Foo<Bar> won't be compiled when compiling Foo's assembly since it doesn't match the first case,
		 * and it won't be compiled when compiling Bar's assembly if Foo's assembly is not loaded.
		 */
	}

	printf ("Added %d methods from profile.\n", count);
}

static void
init_got_info (GotInfo *info)
{
	int i;

	info->patch_to_got_offset = g_hash_table_new (mono_patch_info_hash, mono_patch_info_equal);
	info->patch_to_got_offset_by_type = g_new0 (GHashTable*, MONO_PATCH_INFO_NUM);
	for (i = 0; i < MONO_PATCH_INFO_NUM; ++i)
		info->patch_to_got_offset_by_type [i] = g_hash_table_new (mono_patch_info_hash, mono_patch_info_equal);
	info->got_patches = g_ptr_array_new ();
}

static MonoAotCompile*
acfg_create (MonoAssembly *ass, guint32 opts)
{
	MonoImage *image = ass->image;
	MonoAotCompile *acfg;

	acfg = g_new0 (MonoAotCompile, 1);
	acfg->methods = g_ptr_array_new ();
	acfg->method_indexes = g_hash_table_new (NULL, NULL);
	acfg->method_depth = g_hash_table_new (NULL, NULL);
	acfg->plt_offset_to_entry = g_hash_table_new (NULL, NULL);
	acfg->patch_to_plt_entry = g_new0 (GHashTable*, MONO_PATCH_INFO_NUM);
	acfg->method_to_cfg = g_hash_table_new (NULL, NULL);
	acfg->token_info_hash = g_hash_table_new_full (NULL, NULL, NULL, NULL);
	acfg->method_to_pinvoke_import = g_hash_table_new_full (NULL, NULL, NULL, g_free);
	acfg->image_hash = g_hash_table_new (NULL, NULL);
	acfg->image_table = g_ptr_array_new ();
	acfg->globals = g_ptr_array_new ();
	acfg->image = image;
	acfg->opts = opts;
	/* TODO: Write out set of SIMD instructions used, rather than just those available */
	acfg->simd_opts = mono_arch_cpu_enumerate_simd_versions ();
	acfg->mempool = mono_mempool_new ();
	acfg->extra_methods = g_ptr_array_new ();
	acfg->unwind_info_offsets = g_hash_table_new (NULL, NULL);
	acfg->unwind_ops = g_ptr_array_new ();
	acfg->method_label_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	acfg->method_order = g_ptr_array_new ();
	acfg->export_names = g_hash_table_new (NULL, NULL);
	acfg->klass_blob_hash = g_hash_table_new (NULL, NULL);
	acfg->method_blob_hash = g_hash_table_new (NULL, NULL);
	acfg->plt_entry_debug_sym_cache = g_hash_table_new (g_str_hash, g_str_equal);
	acfg->gsharedvt_in_signatures = g_hash_table_new ((GHashFunc)mono_signature_hash, (GEqualFunc)mono_metadata_signature_equal);
	acfg->gsharedvt_out_signatures = g_hash_table_new ((GHashFunc)mono_signature_hash, (GEqualFunc)mono_metadata_signature_equal);
	acfg->profile_methods = g_hash_table_new (NULL, NULL);
	mono_os_mutex_init_recursive (&acfg->mutex);

	init_got_info (&acfg->got_info);
	init_got_info (&acfg->llvm_got_info);

	return acfg;
}

static void
got_info_free (GotInfo *info)
{
	int i;

	for (i = 0; i < MONO_PATCH_INFO_NUM; ++i)
		g_hash_table_destroy (info->patch_to_got_offset_by_type [i]);
	g_free (info->patch_to_got_offset_by_type);
	g_hash_table_destroy (info->patch_to_got_offset);
	g_ptr_array_free (info->got_patches, TRUE);
}

static void
acfg_free (MonoAotCompile *acfg)
{
	int i;

	mono_img_writer_destroy (acfg->w);
	for (i = 0; i < acfg->nmethods; ++i)
		if (acfg->cfgs [i])
			mono_destroy_compile (acfg->cfgs [i]);

	g_free (acfg->cfgs);

	g_free (acfg->static_linking_symbol);
	g_free (acfg->got_symbol);
	g_free (acfg->plt_symbol);
	g_ptr_array_free (acfg->methods, TRUE);
	g_ptr_array_free (acfg->image_table, TRUE);
	g_ptr_array_free (acfg->globals, TRUE);
	g_ptr_array_free (acfg->unwind_ops, TRUE);
	g_hash_table_destroy (acfg->method_indexes);
	g_hash_table_destroy (acfg->method_depth);
	g_hash_table_destroy (acfg->plt_offset_to_entry);
	for (i = 0; i < MONO_PATCH_INFO_NUM; ++i) {
		if (acfg->patch_to_plt_entry [i])
			g_hash_table_destroy (acfg->patch_to_plt_entry [i]);
	}
	g_free (acfg->patch_to_plt_entry);
	g_hash_table_destroy (acfg->method_to_cfg);
	g_hash_table_destroy (acfg->token_info_hash);
	g_hash_table_destroy (acfg->method_to_pinvoke_import);
	g_hash_table_destroy (acfg->image_hash);
	g_hash_table_destroy (acfg->unwind_info_offsets);
	g_hash_table_destroy (acfg->method_label_hash);
	if (acfg->typespec_classes)
		g_hash_table_destroy (acfg->typespec_classes);
	g_hash_table_destroy (acfg->export_names);
	g_hash_table_destroy (acfg->plt_entry_debug_sym_cache);
	g_hash_table_destroy (acfg->klass_blob_hash);
	g_hash_table_destroy (acfg->method_blob_hash);
	got_info_free (&acfg->got_info);
	got_info_free (&acfg->llvm_got_info);
	mono_mempool_destroy (acfg->mempool);
	g_free (acfg);
}

#define WRAPPER(e,n) n,
static const char* const
wrapper_type_names [MONO_WRAPPER_NUM + 1] = {
#include "mono/metadata/wrapper-types.h"
	NULL
};

static G_GNUC_UNUSED const char*
get_wrapper_type_name (int type)
{
	return wrapper_type_names [type];
}

//#define DUMP_PLT
//#define DUMP_GOT

static void aot_dump (MonoAotCompile *acfg)
{
	FILE *dumpfile;
	char * dumpname;

	JsonWriter writer;
	mono_json_writer_init (&writer);

	mono_json_writer_object_begin(&writer);

	// Methods
	mono_json_writer_indent (&writer);
	mono_json_writer_object_key(&writer, "methods");
	mono_json_writer_array_begin (&writer);

	int i;
	for (i = 0; i < acfg->nmethods; ++i) {
		MonoCompile *cfg;
		MonoMethod *method;
		MonoClass *klass;

		cfg = acfg->cfgs [i];
		if (!cfg)
			continue;

		method = cfg->orig_method;

		mono_json_writer_indent (&writer);
		mono_json_writer_object_begin(&writer);

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "name");
		mono_json_writer_printf (&writer, "\"%s\",\n", method->name);

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "signature");
		mono_json_writer_printf (&writer, "\"%s\",\n", mono_method_get_full_name (method));

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "code_size");
		mono_json_writer_printf (&writer, "\"%d\",\n", cfg->code_size);

		klass = method->klass;

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "class");
		mono_json_writer_printf (&writer, "\"%s\",\n", klass->name);

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "namespace");
		mono_json_writer_printf (&writer, "\"%s\",\n", klass->name_space);

		mono_json_writer_indent (&writer);
		mono_json_writer_object_key(&writer, "wrapper_type");
		mono_json_writer_printf (&writer, "\"%s\",\n", get_wrapper_type_name(method->wrapper_type));

		mono_json_writer_indent_pop (&writer);
		mono_json_writer_indent (&writer);
		mono_json_writer_object_end (&writer);
		mono_json_writer_printf (&writer, ",\n");
	}

	mono_json_writer_indent_pop (&writer);
	mono_json_writer_indent (&writer);
	mono_json_writer_array_end (&writer);
	mono_json_writer_printf (&writer, ",\n");

	// PLT entries
#ifdef DUMP_PLT
	mono_json_writer_indent_push (&writer);
	mono_json_writer_indent (&writer);
	mono_json_writer_object_key(&writer, "plt");
	mono_json_writer_array_begin (&writer);

	for (i = 0; i < acfg->plt_offset; ++i) {
		MonoPltEntry *plt_entry = NULL;
		MonoJumpInfo *ji;

		if (i == 0)
			/* 
			 * The first plt entry is unused.
			 */
			continue;

		plt_entry = g_hash_table_lookup (acfg->plt_offset_to_entry, GUINT_TO_POINTER (i));
		ji = plt_entry->ji;

		mono_json_writer_indent (&writer);
		mono_json_writer_printf (&writer, "{ ");
		mono_json_writer_object_key(&writer, "symbol");
		mono_json_writer_printf (&writer, "\"%s\" },\n", plt_entry->symbol);
	}

	mono_json_writer_indent_pop (&writer);
	mono_json_writer_indent (&writer);
	mono_json_writer_array_end (&writer);
	mono_json_writer_printf (&writer, ",\n");
#endif

	// GOT entries
#ifdef DUMP_GOT
	mono_json_writer_indent_push (&writer);
	mono_json_writer_indent (&writer);
	mono_json_writer_object_key(&writer, "got");
	mono_json_writer_array_begin (&writer);

	mono_json_writer_indent_push (&writer);
	for (i = 0; i < acfg->got_info.got_patches->len; ++i) {
		MonoJumpInfo *ji = g_ptr_array_index (acfg->got_info.got_patches, i);

		mono_json_writer_indent (&writer);
		mono_json_writer_printf (&writer, "{ ");
		mono_json_writer_object_key(&writer, "patch_name");
		mono_json_writer_printf (&writer, "\"%s\" },\n", get_patch_name (ji->type));
	}

	mono_json_writer_indent_pop (&writer);
	mono_json_writer_indent (&writer);
	mono_json_writer_array_end (&writer);
	mono_json_writer_printf (&writer, ",\n");
#endif

	mono_json_writer_indent_pop (&writer);
	mono_json_writer_indent (&writer);
	mono_json_writer_object_end (&writer);

	dumpname = g_strdup_printf ("%s.json", g_path_get_basename (acfg->image->name));
	dumpfile = fopen (dumpname, "w+");
	g_free (dumpname);

	fprintf (dumpfile, "%s", writer.text->str);
	fclose (dumpfile);

	mono_json_writer_destroy (&writer);
}

static const char *preinited_jit_icalls[] = {
	"mono_aot_init_llvm_method",
	"mono_aot_init_gshared_method_this",
	"mono_aot_init_gshared_method_mrgctx",
	"mono_aot_init_gshared_method_vtable",
	"mono_llvm_throw_corlib_exception",
	"mono_init_vtable_slot",
	"mono_helper_ldstr_mscorlib"
};

static void
add_preinit_got_slots (MonoAotCompile *acfg)
{
	MonoJumpInfo *ji;
	int i;

	/*
	 * Allocate the first few GOT entries to information which is needed frequently, or it is needed
	 * during method initialization etc.
	 */

	ji = (MonoJumpInfo *)mono_mempool_alloc0 (acfg->mempool, sizeof (MonoJumpInfo));
	ji->type = MONO_PATCH_INFO_IMAGE;
	ji->data.image = acfg->image;
	get_got_offset (acfg, FALSE, ji);
	get_got_offset (acfg, TRUE, ji);

	ji = (MonoJumpInfo *)mono_mempool_alloc0 (acfg->mempool, sizeof (MonoJumpInfo));
	ji->type = MONO_PATCH_INFO_MSCORLIB_GOT_ADDR;
	get_got_offset (acfg, FALSE, ji);
	get_got_offset (acfg, TRUE, ji);

	ji = (MonoJumpInfo *)mono_mempool_alloc0 (acfg->mempool, sizeof (MonoJumpInfo));
	ji->type = MONO_PATCH_INFO_GC_CARD_TABLE_ADDR;
	get_got_offset (acfg, FALSE, ji);
	get_got_offset (acfg, TRUE, ji);

	ji = (MonoJumpInfo *)mono_mempool_alloc0 (acfg->mempool, sizeof (MonoJumpInfo));
	ji->type = MONO_PATCH_INFO_GC_NURSERY_START;
	get_got_offset (acfg, FALSE, ji);
	get_got_offset (acfg, TRUE, ji);

	ji = (MonoJumpInfo *)mono_mempool_alloc0 (acfg->mempool, sizeof (MonoJumpInfo));
	ji->type = MONO_PATCH_INFO_AOT_MODULE;
	get_got_offset (acfg, FALSE, ji);
	get_got_offset (acfg, TRUE, ji);

	ji = (MonoJumpInfo *)mono_mempool_alloc0 (acfg->mempool, sizeof (MonoJumpInfo));
	ji->type = MONO_PATCH_INFO_GC_NURSERY_BITS;
	get_got_offset (acfg, FALSE, ji);
	get_got_offset (acfg, TRUE, ji);

	for (i = 0; i < TLS_KEY_NUM; i++) {
		ji = (MonoJumpInfo *)mono_mempool_alloc0 (acfg->mempool, sizeof (MonoJumpInfo));
		ji->type = MONO_PATCH_INFO_GET_TLS_TRAMP;
		ji->data.index = i;
		get_got_offset (acfg, FALSE, ji);
		get_got_offset (acfg, TRUE, ji);

		ji = (MonoJumpInfo *)mono_mempool_alloc0 (acfg->mempool, sizeof (MonoJumpInfo));
		ji->type = MONO_PATCH_INFO_SET_TLS_TRAMP;
		ji->data.index = i;
		get_got_offset (acfg, FALSE, ji);
		get_got_offset (acfg, TRUE, ji);
	}

	ji = (MonoJumpInfo *)mono_mempool_alloc0 (acfg->mempool, sizeof (MonoJumpInfo));
	ji->type = MONO_PATCH_INFO_JIT_THREAD_ATTACH;
	get_got_offset (acfg, FALSE, ji);
	get_got_offset (acfg, TRUE, ji);

	for (i = 0; i < sizeof (preinited_jit_icalls) / sizeof (char*); ++i) {
		ji = (MonoJumpInfo *)mono_mempool_alloc0 (acfg->mempool, sizeof (MonoAotCompile));
		ji->type = MONO_PATCH_INFO_INTERNAL_METHOD;
		ji->data.name = preinited_jit_icalls [i];
		get_got_offset (acfg, FALSE, ji);
		get_got_offset (acfg, TRUE, ji);
	}

	acfg->nshared_got_entries = acfg->got_offset;
}

int
mono_compile_assembly (MonoAssembly *ass, guint32 opts, const char *aot_options)
{
	MonoImage *image = ass->image;
	int i, res;
	gint64 all_sizes;
	MonoAotCompile *acfg;
	char *outfile_name, *tmp_outfile_name, *p;
	char llvm_stats_msg [256];
	TV_DECLARE (atv);
	TV_DECLARE (btv);

	acfg = acfg_create (ass, opts);

	memset (&acfg->aot_opts, 0, sizeof (acfg->aot_opts));
	acfg->aot_opts.write_symbols = TRUE;
	acfg->aot_opts.ntrampolines = 4096;
	acfg->aot_opts.nrgctx_trampolines = 4096;
	acfg->aot_opts.nimt_trampolines = 512;
	acfg->aot_opts.nrgctx_fetch_trampolines = 128;
	acfg->aot_opts.ngsharedvt_arg_trampolines = 512;
	acfg->aot_opts.llvm_path = g_strdup ("");
	acfg->aot_opts.temp_path = g_strdup ("");
#ifdef MONOTOUCH
	acfg->aot_opts.use_trampolines_page = TRUE;
#endif

	mono_aot_parse_options (aot_options, &acfg->aot_opts);

	if (acfg->aot_opts.logfile) {
		acfg->logfile = fopen (acfg->aot_opts.logfile, "a+");
	}

	if (acfg->aot_opts.data_outfile) {
		acfg->data_outfile = fopen (acfg->aot_opts.data_outfile, "w+");
		if (!acfg->data_outfile) {
			aot_printerrf (acfg, "Unable to create file '%s': %s\n", acfg->aot_opts.data_outfile, strerror (errno));
			return 1;
		}
		acfg->flags = (MonoAotFileFlags)(acfg->flags | MONO_AOT_FILE_FLAG_SEPARATE_DATA);
	}

	//acfg->aot_opts.print_skipped_methods = TRUE;

#if !defined(MONO_ARCH_GSHAREDVT_SUPPORTED)
	if (acfg->opts & MONO_OPT_GSHAREDVT) {
		aot_printerrf (acfg, "-O=gsharedvt not supported on this platform.\n");
		return 1;
	}
	if (acfg->aot_opts.llvm_only) {
		aot_printerrf (acfg, "--aot=llvmonly requires a runtime that supports gsharedvt.\n");
		return 1;
	}
#else
	if (acfg->aot_opts.llvm_only || mono_aot_mode_is_full (&acfg->aot_opts) || mono_aot_mode_is_hybrid (&acfg->aot_opts))
		acfg->opts |= MONO_OPT_GSHAREDVT;
#endif

#if !defined(ENABLE_LLVM)
	if (acfg->aot_opts.llvm_only) {
		aot_printerrf (acfg, "--aot=llvmonly requires a runtime compiled with llvm support.\n");
		return 1;
	}
#endif

	if (acfg->opts & MONO_OPT_GSHAREDVT)
		mono_set_generic_sharing_vt_supported (TRUE);

	aot_printf (acfg, "Mono Ahead of Time compiler - compiling assembly %s\n", image->name);

	generate_aotid ((guint8*) &acfg->image->aotid);

	char *aotid = mono_guid_to_string (acfg->image->aotid);
	aot_printf (acfg, "AOTID %s\n", aotid);
	g_free (aotid);

#ifndef MONO_ARCH_HAVE_FULL_AOT_TRAMPOLINES
	if (mono_aot_mode_is_full (&acfg->aot_opts)) {
		aot_printerrf (acfg, "--aot=full is not supported on this platform.\n");
		return 1;
	}
#endif

	if (acfg->aot_opts.direct_pinvoke && !acfg->aot_opts.static_link) {
		aot_printerrf (acfg, "The 'direct-pinvoke' AOT option also requires the 'static' AOT option.\n");
		return 1;
	}

	if (acfg->aot_opts.static_link)
		acfg->aot_opts.asm_writer = TRUE;

	if (acfg->aot_opts.soft_debug) {
		MonoDebugOptions *opt = mini_get_debug_options ();

		opt->mdb_optimizations = TRUE;
		opt->gen_sdb_seq_points = TRUE;

		if (!mono_debug_enabled ()) {
			aot_printerrf (acfg, "The soft-debug AOT option requires the --debug option.\n");
			return 1;
		}
		acfg->flags = (MonoAotFileFlags)(acfg->flags | MONO_AOT_FILE_FLAG_DEBUG);
	}

	if (mono_use_llvm || acfg->aot_opts.llvm) {
		acfg->llvm = TRUE;
		acfg->aot_opts.asm_writer = TRUE;
		acfg->flags = (MonoAotFileFlags)(acfg->flags | MONO_AOT_FILE_FLAG_WITH_LLVM);

		if (acfg->aot_opts.soft_debug) {
			aot_printerrf (acfg, "The 'soft-debug' option is not supported when compiling with LLVM.\n");
			return 1;
		}

		mini_llvm_init ();

		if (acfg->aot_opts.asm_only && !acfg->aot_opts.llvm_outfile) {
			aot_printerrf (acfg, "Compiling with LLVM and the asm-only option requires the llvm-outfile= option.\n");
			return 1;
		}
	}

	if (mono_aot_mode_is_full (&acfg->aot_opts))
		acfg->flags = (MonoAotFileFlags)(acfg->flags | MONO_AOT_FILE_FLAG_FULL_AOT);

	if (mono_threads_is_coop_enabled ())
		acfg->flags = (MonoAotFileFlags)(acfg->flags | MONO_AOT_FILE_FLAG_SAFEPOINTS);

	if (acfg->aot_opts.instances_logfile_path) {
		acfg->instances_logfile = fopen (acfg->aot_opts.instances_logfile_path, "w");
		if (!acfg->instances_logfile) {
			aot_printerrf (acfg, "Unable to create logfile: '%s'.\n", acfg->aot_opts.instances_logfile_path);
			return 1;
		}
	}

	if (acfg->aot_opts.profile_files) {
		GList *l;

		for (l = acfg->aot_opts.profile_files; l; l = l->next) {
			load_profile_file (acfg, (char*)l->data);
		}
	}

	{
		int method_index;

       for (method_index = 0; method_index < acfg->image->tables [MONO_TABLE_METHOD].rows; ++method_index) {
		   g_ptr_array_add (acfg->method_order,GUINT_TO_POINTER (method_index));
       }
	}

	acfg->num_trampolines [MONO_AOT_TRAMP_SPECIFIC] = mono_aot_mode_is_full (&acfg->aot_opts) ? acfg->aot_opts.ntrampolines : 0;
#ifdef MONO_ARCH_GSHARED_SUPPORTED
	acfg->num_trampolines [MONO_AOT_TRAMP_STATIC_RGCTX] = mono_aot_mode_is_full (&acfg->aot_opts) ? acfg->aot_opts.nrgctx_trampolines : 0;
#endif
	acfg->num_trampolines [MONO_AOT_TRAMP_IMT] = mono_aot_mode_is_full (&acfg->aot_opts) ? acfg->aot_opts.nimt_trampolines : 0;
#ifdef MONO_ARCH_GSHAREDVT_SUPPORTED
	if (acfg->opts & MONO_OPT_GSHAREDVT)
		acfg->num_trampolines [MONO_AOT_TRAMP_GSHAREDVT_ARG] = mono_aot_mode_is_full (&acfg->aot_opts) ? acfg->aot_opts.ngsharedvt_arg_trampolines : 0;
#endif

	acfg->temp_prefix = mono_img_writer_get_temp_label_prefix (NULL);

	arch_init (acfg);

	if (mono_use_llvm || acfg->aot_opts.llvm) {

		/*
		 * Emit all LLVM code into a separate assembly/object file and link with it
		 * normally.
		 */
		if (!acfg->aot_opts.asm_only && acfg->llvm_owriter_supported) {
			acfg->llvm_owriter = TRUE;
		} else if (acfg->aot_opts.llvm_outfile) {
			int len = strlen (acfg->aot_opts.llvm_outfile);

			if (len >= 2 && acfg->aot_opts.llvm_outfile [len - 2] == '.' && acfg->aot_opts.llvm_outfile [len - 1] == 'o')
				acfg->llvm_owriter = TRUE;
		}
	}

	if (acfg->llvm && acfg->thumb_mixed)
		acfg->flags = (MonoAotFileFlags)(acfg->flags | MONO_AOT_FILE_FLAG_LLVM_THUMB);
	if (acfg->aot_opts.llvm_only)
		acfg->flags = (MonoAotFileFlags)(acfg->flags | MONO_AOT_FILE_FLAG_LLVM_ONLY);

	acfg->assembly_name_sym = g_strdup (acfg->image->assembly->aname.name);
	/* Get rid of characters which cannot occur in symbols */
	for (p = acfg->assembly_name_sym; *p; ++p) {
		if (!(isalnum (*p) || *p == '_'))
			*p = '_';
	}

	acfg->global_prefix = g_strdup_printf ("mono_aot_%s", acfg->assembly_name_sym);
	acfg->plt_symbol = g_strdup_printf ("%s_plt", acfg->global_prefix);
	acfg->got_symbol = g_strdup_printf ("%s_got", acfg->global_prefix);
 	if (acfg->llvm) {
		acfg->llvm_got_symbol = g_strdup_printf ("%s_llvm_got", acfg->global_prefix);
		acfg->llvm_eh_frame_symbol = g_strdup_printf ("%s_eh_frame", acfg->global_prefix);
	}

	acfg->method_index = 1;

	if (mono_aot_mode_is_full (&acfg->aot_opts) || mono_aot_mode_is_hybrid (&acfg->aot_opts))
		mono_set_partial_sharing_supported (TRUE);

	res = collect_methods (acfg);
	if (!res)
		return 1;

	{
		GList *l;

		for (l = acfg->profile_data; l; l = l->next)
			resolve_profile_data (acfg, (ProfileData*)l->data);
		for (l = acfg->profile_data; l; l = l->next)
			add_profile_instances (acfg, (ProfileData*)l->data);
	}

	acfg->cfgs_size = acfg->methods->len + 32;
	acfg->cfgs = g_new0 (MonoCompile*, acfg->cfgs_size);

	/* PLT offset 0 is reserved for the PLT trampoline */
	acfg->plt_offset = 1;
	add_preinit_got_slots (acfg);

#ifdef ENABLE_LLVM
	if (acfg->llvm) {
		llvm_acfg = acfg;
		mono_llvm_create_aot_module (acfg->image->assembly, acfg->global_prefix, TRUE, acfg->aot_opts.static_link, acfg->aot_opts.llvm_only);
	}
#endif

	TV_GETTIME (atv);

	compile_methods (acfg);

	TV_GETTIME (btv);

	acfg->stats.jit_time = TV_ELAPSED (atv, btv);

	TV_GETTIME (atv);

#ifdef ENABLE_LLVM
	if (acfg->llvm) {
		if (acfg->aot_opts.asm_only) {
			if (acfg->aot_opts.outfile) {
				acfg->tmpfname = g_strdup_printf ("%s", acfg->aot_opts.outfile);
				acfg->tmpbasename = g_strdup (acfg->tmpfname);
			} else {
				acfg->tmpbasename = g_strdup_printf ("%s", acfg->image->name);
				acfg->tmpfname = g_strdup_printf ("%s.s", acfg->tmpbasename);
			}
			g_assert (acfg->aot_opts.llvm_outfile);
			acfg->llvm_sfile = g_strdup (acfg->aot_opts.llvm_outfile);
			if (acfg->llvm_owriter)
				acfg->llvm_ofile = g_strdup (acfg->aot_opts.llvm_outfile);
			else
				acfg->llvm_sfile = g_strdup (acfg->aot_opts.llvm_outfile);
		} else {
			acfg->tmpbasename = (strcmp (acfg->aot_opts.temp_path, "") == 0) ?
				g_strdup_printf ("%s", "temp") :
				g_build_filename (acfg->aot_opts.temp_path, "temp", NULL);
				
			acfg->tmpfname = g_strdup_printf ("%s.s", acfg->tmpbasename);
			acfg->llvm_sfile = g_strdup_printf ("%s-llvm.s", acfg->tmpbasename);
			acfg->llvm_ofile = g_strdup_printf ("%s-llvm.o", acfg->tmpbasename);
		}
	}
#endif

	if (acfg->aot_opts.asm_only && !acfg->aot_opts.llvm_only) {
		if (acfg->aot_opts.outfile)
			acfg->tmpfname = g_strdup_printf ("%s", acfg->aot_opts.outfile);
		else
			acfg->tmpfname = g_strdup_printf ("%s.s", acfg->image->name);
		acfg->fp = fopen (acfg->tmpfname, "w+");
	} else {
		int i = g_file_open_tmp ("mono_aot_XXXXXX", &acfg->tmpfname, NULL);
		acfg->fp = fdopen (i, "w+");
	}
	if (acfg->fp == 0 && !acfg->aot_opts.llvm_only) {
		aot_printerrf (acfg, "Unable to open file '%s': %s\n", acfg->tmpfname, strerror (errno));
		return 1;
	}
	if (acfg->fp)
		acfg->w = mono_img_writer_create (acfg->fp, FALSE);

	tmp_outfile_name = NULL;
	outfile_name = NULL;

	/* Compute symbols for methods */
	for (i = 0; i < acfg->nmethods; ++i) {
		if (acfg->cfgs [i]) {
			MonoCompile *cfg = acfg->cfgs [i];
			int method_index = get_method_index (acfg, cfg->orig_method);

			if (COMPILE_LLVM (cfg))
				cfg->asm_symbol = g_strdup_printf ("%s%s", acfg->llvm_label_prefix, cfg->llvm_method_name);
			else if (acfg->global_symbols || acfg->llvm)
				cfg->asm_symbol = get_debug_sym (cfg->orig_method, "", acfg->method_label_hash);
			else
				cfg->asm_symbol = g_strdup_printf ("%s%sm_%x", acfg->temp_prefix, acfg->llvm_label_prefix, method_index);
			cfg->asm_debug_symbol = cfg->asm_symbol;
		}
	}

	if (acfg->aot_opts.dwarf_debug && acfg->aot_opts.gnu_asm) {
		/*
		 * CLANG supports GAS .file/.loc directives, so emit line number information this way
		 */
		acfg->gas_line_numbers = TRUE;
	}

#ifdef EMIT_DWARF_INFO
	if ((!acfg->aot_opts.nodebug || acfg->aot_opts.dwarf_debug) && acfg->has_jitted_code) {
		if (acfg->aot_opts.dwarf_debug && !mono_debug_enabled ()) {
			aot_printerrf (acfg, "The dwarf AOT option requires the --debug option.\n");
			return 1;
		}
		acfg->dwarf = mono_dwarf_writer_create (acfg->w, NULL, 0, !acfg->gas_line_numbers);
	}
#endif /* EMIT_DWARF_INFO */

	if (acfg->w)
		mono_img_writer_emit_start (acfg->w);

	if (acfg->dwarf)
		mono_dwarf_writer_emit_base_info (acfg->dwarf, g_path_get_basename (acfg->image->name), mono_unwind_get_cie_program ());

	emit_code (acfg);

	emit_info (acfg);

	emit_extra_methods (acfg);

	emit_trampolines (acfg);

	emit_class_name_table (acfg);

	emit_got_info (acfg, FALSE);
	if (acfg->llvm)
		emit_got_info (acfg, TRUE);

	emit_exception_info (acfg);

	emit_unwind_info (acfg);

	emit_class_info (acfg);

	emit_plt (acfg);

	emit_image_table (acfg);

	emit_got (acfg);

	{
		/*
		 * The managed allocators are GC specific, so can't use an AOT image created by one GC
		 * in another.
		 */
		const char *gc_name = mono_gc_get_gc_name ();
		acfg->gc_name_offset = add_to_blob (acfg, (guint8*)gc_name, strlen (gc_name) + 1);
	}

	emit_blob (acfg);

	emit_objc_selectors (acfg);

	emit_globals (acfg);

	emit_file_info (acfg);

	emit_library_info (acfg);

	if (acfg->dwarf) {
		emit_dwarf_info (acfg);
		mono_dwarf_writer_close (acfg->dwarf);
	}

	emit_mem_end (acfg);

	if (acfg->need_pt_gnu_stack) {
		/* This is required so the .so doesn't have an executable stack */
		/* The bin writer already emits this */
		fprintf (acfg->fp, "\n.section	.note.GNU-stack,\"\",@progbits\n");
	}

	if (acfg->aot_opts.data_outfile)
		fclose (acfg->data_outfile);

#ifdef ENABLE_LLVM
	if (acfg->llvm) {
		gboolean res;

		res = emit_llvm_file (acfg);
		if (!res)
			return 1;
	}
#endif

	TV_GETTIME (btv);

	acfg->stats.gen_time = TV_ELAPSED (atv, btv);

	if (acfg->llvm)
		sprintf (llvm_stats_msg, ", LLVM: %d (%d%%)", acfg->stats.llvm_count, acfg->stats.mcount ? (acfg->stats.llvm_count * 100) / acfg->stats.mcount : 100);
	else
		strcpy (llvm_stats_msg, "");

	all_sizes = acfg->stats.code_size + acfg->stats.info_size + acfg->stats.ex_info_size + acfg->stats.unwind_info_size + acfg->stats.class_info_size + acfg->stats.got_info_size + acfg->stats.offsets_size + acfg->stats.plt_size;

	aot_printf (acfg, "Code: %d(%d%%) Info: %d(%d%%) Ex Info: %d(%d%%) Unwind Info: %d(%d%%) Class Info: %d(%d%%) PLT: %d(%d%%) GOT Info: %d(%d%%) Offsets: %d(%d%%) GOT: %d\n",
				(int)acfg->stats.code_size, (int)(acfg->stats.code_size * 100 / all_sizes),
				(int)acfg->stats.info_size, (int)(acfg->stats.info_size * 100 / all_sizes),
				(int)acfg->stats.ex_info_size, (int)(acfg->stats.ex_info_size * 100 / all_sizes),
				(int)acfg->stats.unwind_info_size, (int)(acfg->stats.unwind_info_size * 100 / all_sizes),
				(int)acfg->stats.class_info_size, (int)(acfg->stats.class_info_size * 100 / all_sizes),
				acfg->stats.plt_size ? (int)acfg->stats.plt_size : (int)acfg->plt_offset, acfg->stats.plt_size ? (int)(acfg->stats.plt_size * 100 / all_sizes) : 0,
				(int)acfg->stats.got_info_size, (int)(acfg->stats.got_info_size * 100 / all_sizes),
				(int)acfg->stats.offsets_size, (int)(acfg->stats.offsets_size * 100 / all_sizes),
			(int)(acfg->got_offset * sizeof (gpointer)));
	aot_printf (acfg, "Compiled: %d/%d (%d%%)%s, No GOT slots: %d (%d%%), Direct calls: %d (%d%%)\n", 
			acfg->stats.ccount, acfg->stats.mcount, acfg->stats.mcount ? (acfg->stats.ccount * 100) / acfg->stats.mcount : 100,
			llvm_stats_msg,
			acfg->stats.methods_without_got_slots, acfg->stats.mcount ? (acfg->stats.methods_without_got_slots * 100) / acfg->stats.mcount : 100,
			acfg->stats.direct_calls, acfg->stats.all_calls ? (acfg->stats.direct_calls * 100) / acfg->stats.all_calls : 100);
	if (acfg->stats.genericcount)
		aot_printf (acfg, "%d methods are generic (%d%%)\n", acfg->stats.genericcount, acfg->stats.mcount ? (acfg->stats.genericcount * 100) / acfg->stats.mcount : 100);
	if (acfg->stats.abscount)
		aot_printf (acfg, "%d methods contain absolute addresses (%d%%)\n", acfg->stats.abscount, acfg->stats.mcount ? (acfg->stats.abscount * 100) / acfg->stats.mcount : 100);
	if (acfg->stats.lmfcount)
		aot_printf (acfg, "%d methods contain lmf pointers (%d%%)\n", acfg->stats.lmfcount, acfg->stats.mcount ? (acfg->stats.lmfcount * 100) / acfg->stats.mcount : 100);
	if (acfg->stats.ocount)
		aot_printf (acfg, "%d methods have other problems (%d%%)\n", acfg->stats.ocount, acfg->stats.mcount ? (acfg->stats.ocount * 100) / acfg->stats.mcount : 100);

	TV_GETTIME (atv);
	if (acfg->w) {
		res = mono_img_writer_emit_writeout (acfg->w);
		if (res != 0) {
			acfg_free (acfg);
			return res;
		}
		res = compile_asm (acfg);
		if (res != 0) {
			acfg_free (acfg);
			return res;
		}
	}
	TV_GETTIME (btv);
	acfg->stats.link_time = TV_ELAPSED (atv, btv);

	if (acfg->aot_opts.stats) {
		int i;

		aot_printf (acfg, "GOT slot distribution:\n");
		for (i = 0; i < MONO_PATCH_INFO_NUM; ++i)
			if (acfg->stats.got_slot_types [i])
				aot_printf (acfg, "\t%s: %d (%d)\n", get_patch_name (i), acfg->stats.got_slot_types [i], acfg->stats.got_slot_info_sizes [i]);
		aot_printf (acfg, "\nMethod stats:\n");
		aot_printf (acfg, "\tNormal:    %d\n", acfg->stats.method_categories [METHOD_CAT_NORMAL]);
		aot_printf (acfg, "\tInstance:  %d\n", acfg->stats.method_categories [METHOD_CAT_INST]);
		aot_printf (acfg, "\tGSharedvt: %d\n", acfg->stats.method_categories [METHOD_CAT_GSHAREDVT]);
		aot_printf (acfg, "\tWrapper:   %d\n", acfg->stats.method_categories [METHOD_CAT_WRAPPER]);
	}

	aot_printf (acfg, "JIT time: %d ms, Generation time: %d ms, Assembly+Link time: %d ms.\n", acfg->stats.jit_time / 1000, acfg->stats.gen_time / 1000, acfg->stats.link_time / 1000);

	if (acfg->aot_opts.dump_json)
		aot_dump (acfg);

	acfg_free (acfg);
	
	return 0;
}

#else

/* AOT disabled */

void*
mono_aot_readonly_field_override (MonoClassField *field)
{
	return NULL;
}

int
mono_compile_assembly (MonoAssembly *ass, guint32 opts, const char *aot_options)
{
	return 0;
}

gboolean
mono_aot_is_shared_got_offset (int offset)
{
	return FALSE;
}

#endif
