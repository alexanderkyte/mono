#ifndef __MONO_METADATA_LOCK_TRACER_H__
#define __MONO_METADATA_LOCK_TRACER_H__

/*This is a private header*/
#include <glib.h>

#include "mono/utils/mono-os-mutex.h"
#include "mono/utils/mono-coop-mutex.h"

G_BEGIN_DECLS

#define LOCK_TRACER_STATIC
#define LOCK_TRACER

typedef enum {

#define MONO_LOCK_PROCESS(name) name ,

#include "lock-tracer-locks.h"

#undef MONO_LOCK_PROCESS

} RuntimeLocks;


#if defined(LOCK_TRACER) && defined(LOCK_TRACER_STATIC)

#define MONO_LOCK_PROCESS(name) \
void mono_locks_lock_acquired_## name (RuntimeLocks kind, gpointer lock); \
void mono_locks_lock_released_## name (RuntimeLocks kind, gpointer lock);

#include "lock-tracer-locks.h"

#undef MONO_LOCK_PROCESS

#define mono_locks_tracer_init() do {} while (0)

#define mono_locks_lock_acquired(kind, lock) do { \
	mono_locks_lock_acquired_## kind (kind, lock); \
} while (0)

#define mono_locks_lock_released(kind, lock) do { \
	mono_locks_lock_released_## kind (kind, lock); \
} while (0)

#elif defined(LOCK_TRACER)

// void mono_locks_tracer_init (void);
// 
// void mono_locks_lock_acquired (RuntimeLocks kind, gpointer lock);
// void mono_locks_lock_released (RuntimeLocks kind, gpointer lock);

#else

// #define mono_locks_tracer_init() do {} while (0)
// 
// #define mono_locks_lock_acquired(__UNUSED0, __UNUSED1) do {} while (0)
// #define mono_locks_lock_released(__UNUSED0, __UNUSED1) do {} while (0)

#endif // LOCK_TRACER

#define mono_locks_os_acquire(LOCK,NAME)	\
	do {	\
		mono_os_mutex_lock (LOCK);	\
		mono_locks_lock_acquired (NAME, LOCK);	\
	} while (0)

#define mono_locks_os_release(LOCK,NAME)	\
	do {	\
		mono_locks_lock_released (NAME, LOCK);	\
		mono_os_mutex_unlock (LOCK);	\
	} while (0)

#define mono_locks_coop_acquire(LOCK,NAME)	\
	do {	\
		mono_coop_mutex_lock (LOCK);	\
		mono_locks_lock_acquired (NAME, LOCK);	\
	} while (0)

#define mono_locks_coop_release(LOCK,NAME)	\
	do {	\
		mono_locks_lock_released (NAME, LOCK);	\
		mono_coop_mutex_unlock (LOCK);	\
	} while (0)

G_END_DECLS

#endif /* __MONO_METADATA_LOCK_TRACER_H__ */
