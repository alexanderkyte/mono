
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
} MonoTypeName;

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
nameToType (MonoTypeName typ);

MonoRegisterUsage cconv_in (MonoTypeName *type, int count);

MonoRegisterUsage cconv_out (MonoTypeName type);
// END DECLS

MonoType *
nameToType (MonoTypeName typ)
{
	static MonoType *types [type_count];

	if (types [typ] != NULL)
		return types [typ];

	types [typ] = g_malloc0 (sizeof(MonoType));
	types[typ]->type = mapNameToType[typ];

	return types[typ];
}

MonoRegisterUsage
cconv_in (MonoTypeName *type, int count)
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

	MonoRegisterUsage ret;
	ret.source = ArgumentRegisters;

	g_free (res);

	return ret;
}

MonoRegisterUsage
cconv_out (MonoTypeName type)
{
	MonoMethodSignature sig;
	sig.ret = nameToType (type);
	sig.param_count = 0;
	sig.generic_param_count = 0;
	sig.hasthis = 0;
	sig.is_inflated = 0;

	CallInfo *res = get_call_info (NULL, NULL, &sig);
	g_assert (res->nargs == 0);

	MonoRegisterUsage ret;
	ret.source = ReturnRegisters;

	switch (res->ret.storage) {
		case RegTypeNone:
			ret.num_registers = 0;
			ret.regs = NULL;
		case RegTypeGeneral:
			ret.num_registers = 0;
			ret.regs = malloc(sizeof(MonoRegister) * ret.num_registers);
			ret.regs[0] = res->ret.reg;
		case RegTypeIRegPair:
			ret.num_registers = 0;
			ret.regs = malloc(sizeof(MonoRegister) * ret.num_registers);
			ret.regs[0] = res->ret.reg;
			ret.regs[1] = res->ret.reg + 1;
		case RegTypeBase:
			ret.num_registers = 1;
			ret.regs = malloc(sizeof(MonoRegister) * ret.num_registers);
			// Uses stack
			ret.regs[0] = 13;
		case RegTypeBaseGen:
			ret.num_registers = 1;
			ret.regs = malloc(sizeof(MonoRegister) * ret.num_registers);
			// Uses stack
			ret.regs[0] = 13;
			ret.regs[1] = 3;
		case RegTypeFP:
			ret.num_registers = 1;
			ret.regs = malloc(sizeof(MonoRegister) * ret.num_registers);
			ret.regs[0] = res->ret.reg;
		case RegTypeStructByAddr:
			ret.num_registers = 0;
			ret.regs = malloc(sizeof(MonoRegister) * 1);
			ret.regs[0] = res->ret.reg;
		case RegTypeStructByVal:
		case RegTypeGSharedVtInReg:
		case RegTypeGSharedVtOnStack:
		case RegTypeHFA:
			ret.num_registers = 0;
			ret.regs = malloc(sizeof(MonoRegister) * 1);
			ret.regs[0] = 13;
	}
	g_free (res);

	return ret;
}

#endif
