// so OpenMPI borrowed gasnet's platform-detection code and didn't change
//  the define names - work around it by undef'ing anything set via mpi.h
//  before we include gasnet.h
#undef __PLATFORM_COMPILER_GNU_VERSION_STR

#ifndef GASNET_PAR
#define GASNET_PAR
#endif
#include <gasnet.h>

#ifndef GASNETT_THREAD_SAFE
#define GASNETT_THREAD_SAFE
#endif
#include <gasnet_tools.h>

// eliminate GASNet warnings for unused static functions
static const void *ignore_gasnet_warning1 __attribute__((unused)) = (void *)_gasneti_threadkey_init;
static const void *ignore_gasnet_warning2 __attribute__((unused)) = (void *)_gasnett_trace_printf_noop;

#define GASNETHSL_IMPL     gasnet_hsl_t mutex
#define GASNETCONDVAR_IMPL gasnett_cond_t condvar

