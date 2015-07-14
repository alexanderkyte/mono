
#ifndef __MINI_ARM_CCONV_TEST
#define __MINI_ARM_CCONV_TEST

#include <mono/mini/mini-arm.h>
#include <glib.h>

// Start Types
typedef int MonoRegister;

typedef enum {
	C_TYPE_CHAR = 0,
	C_TYPE_INT,
	C_TYPE_SHORT,
	C_TYPE_LONG,
	C_TYPE_POINTER,
	C_TYPE_FLOAT,
	C_TYPE_DOUBLE,

	C_TYPE_COUNT,
} CTypeName;

static MonoTypeEnum mapNameToType[] = {
	MONO_TYPE_I1,
	MONO_TYPE_I2,
	MONO_TYPE_I4,
	MONO_TYPE_I8,
	MONO_TYPE_PTR,
	MONO_TYPE_R4,
	MONO_TYPE_R8,
};

static const size_t type_count = C_TYPE_COUNT - C_TYPE_CHAR;

typedef enum {
	ArgumentRegisters, ReturnRegisters
} RegisterUsageSource;

typedef struct {
	RegisterUsageSource source;
	int num_registers;
	MonoRegister *regs;
} MonoRegisterUsage;

// End Types

// Start Decls
extern CallInfo *
get_call_info (MonoGenericSharingContext *gsctx, MonoMemPool *mp, MonoMethodSignature *sig);

MonoType *
nameToType (CTypeName typ);

void
cconv_in (CTypeName *type, int count, MonoRegister **regs, int *reg_cout);

void
cconv_out (CTypeName type, MonoRegister **regs, int *reg_count);
// END DECLS

MonoType *
nameToType (CTypeName typ)
{
	static MonoType *types [type_count];

	if (types [typ] != NULL)
		return types [typ];

	types [typ] = g_malloc0 (sizeof(MonoType));
	types[typ]->type = mapNameToType[typ];

	return types[typ];
}

void
cconv_in (CTypeName *type, int count, MonoRegister **regs, int *reg_count)
{
	MonoMethodSignature *sig = g_malloc0 (sizeof(MonoMethodSignature) + sizeof(MonoType) * count);
	sig->ret = NULL;
	sig->param_count = count;
	sig->generic_param_count = 0;
	sig->hasthis = 0;
	sig->is_inflated = 0;
	for (int i = 0; i < count; i++){
		sig->params [i] = nameToType (type[i]);
	}

	CallInfo *res = get_call_info (NULL, NULL, sig);
	g_free (sig);
	g_assert (res->nargs == 0);

	*reg_count = 0;
}

void
cconv_out (CTypeName type, MonoRegister **regs, int *reg_count)
{
	MonoMethodSignature sig;
	sig.ret = nameToType (type);
	sig.param_count = 0;
	sig.generic_param_count = 0;
	sig.hasthis = 0;
	sig.is_inflated = 0;

	CallInfo *res = get_call_info (NULL, NULL, &sig);
	g_assert (res->nargs == 0);

	switch (res->ret.storage) {
		case RegTypeNone:
			*reg_count = 0;
			*regs = NULL;
		case RegTypeGeneral:
			*reg_count = 0;
			*regs = malloc(sizeof(MonoRegister) * *reg_count);
			*regs[0] = res->ret.reg;
		case RegTypeIRegPair:
			*reg_count = 0;
			*regs = malloc(sizeof(MonoRegister) * *reg_count);
			*regs[0] = res->ret.reg;
			*regs[1] = res->ret.reg + 1;
		case RegTypeBase:
			*reg_count = 1;
			*regs = malloc(sizeof(MonoRegister) * *reg_count);
			// Uses stack
			*regs[0] = 13;
		case RegTypeBaseGen:
			*reg_count = 1;
			*regs = malloc(sizeof(MonoRegister) * *reg_count);
			// Uses stack
			*regs[0] = 13;
			*regs[1] = 3;
		case RegTypeFP:
			*reg_count = 1;
			*regs = malloc(sizeof(MonoRegister) * *reg_count);
			*regs[0] = res->ret.reg;
		case RegTypeStructByAddr:
			*reg_count = 0;
			*regs = malloc(sizeof(MonoRegister) * 1);
			*regs[0] = res->ret.reg;
		case RegTypeStructByVal:
		case RegTypeGSharedVtInReg:
		case RegTypeGSharedVtOnStack:
		case RegTypeHFA:
			*reg_count = 0;
			*regs = malloc(sizeof(MonoRegister) * 1);
			*regs[0] = 13;
	}
	g_free (res);
}

#endif
