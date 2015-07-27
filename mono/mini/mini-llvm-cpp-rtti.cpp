
#include "mini-llvm-cpp.h"
#include <typeinfo>
#include <cxxabi.h>

class MonoExceptionSentinel: public std::exception { };

void
mono_llvm_cpp_throw_exception (void) 
{
	throw new MonoExceptionSentinel ();
}

void
mono_llvm_cpp_rethrow_exception (void) 
{
	mono_llvm_cpp_throw_exception ();
}
