/*
 * Userspace ntsync backend for Wine.
 *
 * Optimized for low-contention waits in game workloads:
 * - table lock only for handle table mutations
 * - per-object lock for hot-path operations
 * - hybrid wait (atomic poll + short spin + condvar sleep)
 * - generation-tagged handles on 64-bit to avoid ABA reuse
 */

#if 0
#pragma makedep unix
#endif

#ifdef __WINE_PE_BUILD__
#error "ntsync.c is for Unix only"
#endif

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"

#include "ntsync.h"

WINE_DEFAULT_DEBUG_CHANNEL(ntsync);

#define MAX_NTSYNC_OBJECTS 32768
#define TICKS_PER_SEC 10000000ULL
#define NSECS_PER_SEC 1000000000ULL
#define SECS_1601_TO_1970 11644473600ULL
#define NTSYNC_TIMEOUT_INFINITE ((LONGLONG)0x8000000000000000ULL)
#define NTSYNC_SPIN_USEC 120

enum ntsync_type
{
    NTSYNC_TYPE_EVENT,
    NTSYNC_TYPE_MUTEX,
    NTSYNC_TYPE_SEMAPHORE,
};

struct ntsync_object
{
    enum ntsync_type type;
    volatile LONG refcount; /* handle refs + transient operation refs */
    pthread_mutex_t lock;
    volatile ULONG seq;

    int signaled;
    BOOL manual_reset;

    DWORD owner_tid;
    LONG recursion;
    BOOL abandoned;

    LONG sem_count;
    LONG sem_max;
};

struct ntsync_slot
{
    struct ntsync_object *obj;
#ifdef _WIN64
    USHORT gen;
#endif
};

static struct ntsync_slot object_table[MAX_NTSYNC_OBJECTS];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static volatile ULONG wait_seq = 1;
static pthread_once_t ntsync_once = PTHREAD_ONCE_INIT;
static int do_ntsync_cached = -1;

#ifdef _WIN64
#define NTSYNC_HANDLE_MASK  ((UINT_PTR)0xffff000000000003ULL)
#define NTSYNC_HANDLE_MAGIC ((UINT_PTR)0x6f77000000000001ULL)
#define NTSYNC_INDEX_BITS   15
#define NTSYNC_INDEX_MASK   ((1u << NTSYNC_INDEX_BITS) - 1u)
#define NTSYNC_GEN_BITS     16
#define NTSYNC_GEN_MASK     ((1u << NTSYNC_GEN_BITS) - 1u)
#else
#define NTSYNC_HANDLE_MASK  ((UINT_PTR)0xffff0003U)
#define NTSYNC_HANDLE_MAGIC ((UINT_PTR)0x6f770001U)
#endif

enum timeout_mode
{
    TIMEOUT_MODE_INFINITE,
    TIMEOUT_MODE_IMMEDIATE,
    TIMEOUT_MODE_TIMED,
};

static void ntsync_lazy_init(void)
{
    if (do_ntsync()) TRACE("WINENTSYNC enabled; userspace backend active\n");
}

void ntsync_init(void)
{
    pthread_once(&ntsync_once, ntsync_lazy_init);
}

int do_ntsync(void)
{
    if (do_ntsync_cached == -1)
    {
        const char *env = getenv("WINENTSYNC");
        do_ntsync_cached = (env && atoi(env)) ? 1 : 0;
        TRACE("WINENTSYNC=%s -> %d\n", env ? env : "(null)", do_ntsync_cached);
    }
    return do_ntsync_cached;
}

static BOOL attr_is_named(const OBJECT_ATTRIBUTES *attr)
{
    return attr && ((attr->RootDirectory && attr->RootDirectory != 0) ||
                    (attr->ObjectName && attr->ObjectName->Buffer && attr->ObjectName->Length));
}

static inline ULONG atomic_load_ulong(volatile ULONG *ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

static inline void atomic_inc_ulong(volatile ULONG *ptr)
{
    __atomic_add_fetch(ptr, 1, __ATOMIC_ACQ_REL);
}

static inline struct ntsync_object *atomic_load_object(struct ntsync_object *volatile *ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

static void object_release(struct ntsync_object *obj)
{
    if (!obj) return;

    if (__atomic_sub_fetch(&obj->refcount, 1, __ATOMIC_ACQ_REL) == 0)
    {
        pthread_mutex_destroy(&obj->lock);
        free(obj);
    }
}

static void object_retain(struct ntsync_object *obj)
{
    __atomic_add_fetch(&obj->refcount, 1, __ATOMIC_ACQ_REL);
}

static void notify_waiters(struct ntsync_object *obj)
{
    if (obj) atomic_inc_ulong(&obj->seq);
    atomic_inc_ulong(&wait_seq);
    pthread_cond_broadcast(&wait_cond);
}

#ifdef _WIN64
static HANDLE make_handle(unsigned int index, unsigned int gen)
{
    UINT_PTR payload;

    payload = (((UINT_PTR)(gen & NTSYNC_GEN_MASK) << NTSYNC_INDEX_BITS) |
               (UINT_PTR)(index & NTSYNC_INDEX_MASK)) + 1;
    return (HANDLE)(NTSYNC_HANDLE_MAGIC | (payload << 2));
}

static int parse_handle(HANDLE handle, unsigned int *index, unsigned int *gen)
{
    UINT_PTR value = (UINT_PTR)handle;
    UINT_PTR payload, encoded;

    if ((value & NTSYNC_HANDLE_MASK) != NTSYNC_HANDLE_MAGIC)
        return 0;

    encoded = (value & ~NTSYNC_HANDLE_MASK) >> 2;
    if (!encoded)
        return -1;

    payload = encoded - 1;
    *index = (unsigned int)(payload & NTSYNC_INDEX_MASK);
    *gen = (unsigned int)((payload >> NTSYNC_INDEX_BITS) & NTSYNC_GEN_MASK);

    if (*index >= MAX_NTSYNC_OBJECTS)
        return -1;

    return 1;
}

static void bump_slot_gen(struct ntsync_slot *slot)
{
    unsigned int next = (slot->gen + 1) & NTSYNC_GEN_MASK;
    if (!next) next = 1;
    slot->gen = (USHORT)next;
}
#else
static HANDLE make_handle(unsigned int index, unsigned int gen)
{
    (void)gen;
    return (HANDLE)(NTSYNC_HANDLE_MAGIC | ((UINT_PTR)(index + 1) << 2));
}

static int parse_handle(HANDLE handle, unsigned int *index, unsigned int *gen)
{
    UINT_PTR value = (UINT_PTR)handle;
    UINT_PTR encoded;

    (void)gen;

    if ((value & NTSYNC_HANDLE_MASK) != NTSYNC_HANDLE_MAGIC)
        return 0;

    encoded = (value & ~NTSYNC_HANDLE_MASK) >> 2;
    if (!encoded || encoded > MAX_NTSYNC_OBJECTS)
        return -1;

    *index = (unsigned int)encoded - 1;
    return 1;
}

static void bump_slot_gen(struct ntsync_slot *slot)
{
    (void)slot;
}
#endif

static NTSTATUS alloc_slot_locked(struct ntsync_object *obj, HANDLE *handle)
{
    unsigned int i;

    for (i = 0; i < MAX_NTSYNC_OBJECTS; ++i)
    {
        if (!object_table[i].obj)
        {
#ifdef _WIN64
            if (!object_table[i].gen) object_table[i].gen = 1;
            *handle = make_handle(i, object_table[i].gen);
#else
            *handle = make_handle(i, 0);
#endif
            __atomic_store_n(&object_table[i].obj, obj, __ATOMIC_RELEASE);
            return STATUS_SUCCESS;
        }
    }

    return STATUS_TOO_MANY_OPENED_FILES;
}

static NTSTATUS get_object_ref(HANDLE handle, int expected_type, struct ntsync_object **out)
{
    struct ntsync_slot *slot;
    struct ntsync_object *obj;
    unsigned int index;
    unsigned int gen = 0;
    int parsed;

    parsed = parse_handle(handle, &index, &gen);
    if (parsed == 0) return STATUS_NOT_IMPLEMENTED;
    if (parsed < 0) return STATUS_INVALID_HANDLE;

    slot = &object_table[index];
    obj = atomic_load_object((struct ntsync_object *volatile *)&slot->obj);
    if (!obj) return STATUS_INVALID_HANDLE;
#ifdef _WIN64
    if (slot->gen != gen) return STATUS_INVALID_HANDLE;
#endif

    object_retain(obj);

    if (atomic_load_object((struct ntsync_object *volatile *)&slot->obj) != obj)
    {
        object_release(obj);
        return STATUS_INVALID_HANDLE;
    }
#ifdef _WIN64
    if (slot->gen != gen)
    {
        object_release(obj);
        return STATUS_INVALID_HANDLE;
    }
#endif

    if (expected_type >= 0 && obj->type != expected_type)
    {
        object_release(obj);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    *out = obj;
    return STATUS_SUCCESS;
}

BOOL ntsync_is_handle(HANDLE handle)
{
    struct ntsync_slot *slot;
    unsigned int index;
    unsigned int gen = 0;
    int parsed;

    if (!do_ntsync()) return FALSE;

    parsed = parse_handle(handle, &index, &gen);
    if (parsed <= 0) return FALSE;

    slot = &object_table[index];
    if (!atomic_load_object((struct ntsync_object *volatile *)&slot->obj)) return FALSE;
#ifdef _WIN64
    if (slot->gen != gen) return FALSE;
#endif
    return TRUE;
}

static struct ntsync_object *alloc_object(enum ntsync_type type)
{
    struct ntsync_object *obj = calloc(1, sizeof(*obj));
    if (!obj) return NULL;

    obj->type = type;
    obj->refcount = 1;
    obj->seq = 1;
    pthread_mutex_init(&obj->lock, NULL);
    return obj;
}

NTSTATUS ntsync_close(HANDLE handle)
{
    struct ntsync_slot *slot;
    struct ntsync_object *obj;
    unsigned int index;
    unsigned int gen = 0;
    int parsed;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;

    parsed = parse_handle(handle, &index, &gen);
    if (parsed == 0) return STATUS_NOT_IMPLEMENTED;
    if (parsed < 0) return STATUS_INVALID_HANDLE;

    pthread_mutex_lock(&table_lock);

    slot = &object_table[index];
    obj = slot->obj;
    if (!obj)
    {
        pthread_mutex_unlock(&table_lock);
        return STATUS_INVALID_HANDLE;
    }
#ifdef _WIN64
    if (slot->gen != gen)
    {
        pthread_mutex_unlock(&table_lock);
        return STATUS_INVALID_HANDLE;
    }
#endif

    __atomic_store_n(&slot->obj, NULL, __ATOMIC_RELEASE);
    bump_slot_gen(slot);
    pthread_mutex_unlock(&table_lock);

    object_release(obj);
    return STATUS_SUCCESS;
}

NTSTATUS ntsync_duplicate(HANDLE source_process, HANDLE source, HANDLE dest_process, HANDLE *dest,
                          ACCESS_MASK access, ULONG attributes, ULONG options)
{
    const ULONG allowed_options = DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS | DUPLICATE_SAME_ATTRIBUTES;
    struct ntsync_slot *src_slot;
    struct ntsync_object *obj;
    NTSTATUS status = STATUS_SUCCESS;
    HANDLE out = 0;
    unsigned int index;
    unsigned int gen = 0;
    int parsed;

    (void)access;
    (void)attributes;

    if (dest) *dest = 0;
    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;

    parsed = parse_handle(source, &index, &gen);
    if (parsed == 0) return STATUS_NOT_IMPLEMENTED;
    if (parsed < 0) return STATUS_INVALID_HANDLE;

    if (options & ~allowed_options) return STATUS_INVALID_PARAMETER;
    if (!dest && !(options & DUPLICATE_CLOSE_SOURCE)) return STATUS_INVALID_PARAMETER;
    if (source_process != NtCurrentProcess()) return STATUS_NOT_SUPPORTED;
    if (dest && dest_process != NtCurrentProcess()) return STATUS_NOT_SUPPORTED;

    pthread_mutex_lock(&table_lock);

    src_slot = &object_table[index];
    obj = src_slot->obj;
    if (!obj)
    {
        status = STATUS_INVALID_HANDLE;
        goto out;
    }
#ifdef _WIN64
    if (src_slot->gen != gen)
    {
        status = STATUS_INVALID_HANDLE;
        goto out;
    }
#endif

    if (dest)
    {
        object_retain(obj);
        status = alloc_slot_locked(obj, &out);
        if (status)
        {
            object_release(obj);
            goto out;
        }
    }

    if (options & DUPLICATE_CLOSE_SOURCE)
    {
        __atomic_store_n(&src_slot->obj, NULL, __ATOMIC_RELEASE);
        bump_slot_gen(src_slot);
        object_release(obj);
    }

out:
    pthread_mutex_unlock(&table_lock);

    if (!status && dest) *dest = out;
    return status;
}

NTSTATUS ntsync_create_event(HANDLE *handle, ACCESS_MASK access,
                             const OBJECT_ATTRIBUTES *attr, EVENT_TYPE type, BOOLEAN initial)
{
    struct ntsync_object *obj;
    NTSTATUS status;

    (void)access;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;
    ntsync_init();

    if (!handle) return STATUS_INVALID_PARAMETER;
    *handle = 0;

    if (attr_is_named(attr)) return STATUS_NOT_IMPLEMENTED;
    if (type != NotificationEvent && type != SynchronizationEvent) return STATUS_INVALID_PARAMETER;

    obj = alloc_object(NTSYNC_TYPE_EVENT);
    if (!obj) return STATUS_NO_MEMORY;

    obj->signaled = initial ? 1 : 0;
    obj->manual_reset = (type == NotificationEvent);

    pthread_mutex_lock(&table_lock);
    status = alloc_slot_locked(obj, handle);
    pthread_mutex_unlock(&table_lock);

    if (status) object_release(obj);
    return status;
}

NTSTATUS ntsync_open_event(HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr)
{
    (void)access;
    (void)attr;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;
    if (handle) *handle = 0;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS ntsync_set_event(HANDLE handle, LONG *prev_state)
{
    struct ntsync_object *obj;
    NTSTATUS status;
    LONG prev;
    BOOL changed = FALSE;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;

    status = get_object_ref(handle, NTSYNC_TYPE_EVENT, &obj);
    if (status) return status;

    pthread_mutex_lock(&obj->lock);
    prev = obj->signaled;
    obj->signaled = 1;
    changed = !prev;
    pthread_mutex_unlock(&obj->lock);

    if (prev_state) *prev_state = prev;
    if (changed) notify_waiters(obj);
    object_release(obj);
    return STATUS_SUCCESS;
}

NTSTATUS ntsync_reset_event(HANDLE handle, LONG *prev_state)
{
    struct ntsync_object *obj;
    NTSTATUS status;
    LONG prev;
    BOOL changed = FALSE;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;

    status = get_object_ref(handle, NTSYNC_TYPE_EVENT, &obj);
    if (status) return status;

    pthread_mutex_lock(&obj->lock);
    prev = obj->signaled;
    obj->signaled = 0;
    changed = !!prev;
    pthread_mutex_unlock(&obj->lock);

    if (prev_state) *prev_state = prev;
    if (changed) notify_waiters(obj);
    object_release(obj);
    return STATUS_SUCCESS;
}

NTSTATUS ntsync_pulse_event(HANDLE handle, LONG *prev_state)
{
    struct ntsync_object *obj;
    NTSTATUS status;
    LONG prev;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;

    status = get_object_ref(handle, NTSYNC_TYPE_EVENT, &obj);
    if (status) return status;

    pthread_mutex_lock(&obj->lock);
    prev = obj->signaled;
    obj->signaled = 1;
    obj->signaled = 0;
    pthread_mutex_unlock(&obj->lock);

    if (prev_state) *prev_state = prev;
    notify_waiters(obj);
    object_release(obj);
    return STATUS_SUCCESS;
}

NTSTATUS ntsync_query_event(HANDLE handle, EVENT_BASIC_INFORMATION *info)
{
    struct ntsync_object *obj;
    NTSTATUS status;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;
    if (!info) return STATUS_INVALID_PARAMETER;

    status = get_object_ref(handle, NTSYNC_TYPE_EVENT, &obj);
    if (status) return status;

    pthread_mutex_lock(&obj->lock);
    info->EventType = obj->manual_reset ? NotificationEvent : SynchronizationEvent;
    info->EventState = obj->signaled;
    pthread_mutex_unlock(&obj->lock);

    object_release(obj);
    return STATUS_SUCCESS;
}

NTSTATUS ntsync_create_mutex(HANDLE *handle, ACCESS_MASK access,
                             const OBJECT_ATTRIBUTES *attr, BOOLEAN initial)
{
    struct ntsync_object *obj;
    NTSTATUS status;

    (void)access;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;
    ntsync_init();

    if (!handle) return STATUS_INVALID_PARAMETER;
    *handle = 0;

    if (attr_is_named(attr)) return STATUS_NOT_IMPLEMENTED;

    obj = alloc_object(NTSYNC_TYPE_MUTEX);
    if (!obj) return STATUS_NO_MEMORY;

    if (initial)
    {
        obj->owner_tid = GetCurrentThreadId();
        obj->recursion = 1;
    }

    pthread_mutex_lock(&table_lock);
    status = alloc_slot_locked(obj, handle);
    pthread_mutex_unlock(&table_lock);

    if (status) object_release(obj);
    return status;
}

NTSTATUS ntsync_open_mutex(HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr)
{
    (void)access;
    (void)attr;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;
    if (handle) *handle = 0;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS ntsync_release_mutex(HANDLE handle, LONG *prev_count)
{
    struct ntsync_object *obj;
    NTSTATUS status;
    DWORD tid = GetCurrentThreadId();
    BOOL changed = FALSE;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;

    status = get_object_ref(handle, NTSYNC_TYPE_MUTEX, &obj);
    if (status) return status;

    pthread_mutex_lock(&obj->lock);

    if (obj->owner_tid != tid)
    {
        pthread_mutex_unlock(&obj->lock);
        object_release(obj);
        return STATUS_MUTANT_NOT_OWNED;
    }

    if (prev_count) *prev_count = obj->recursion;

    if (--obj->recursion <= 0)
    {
        obj->owner_tid = 0;
        obj->recursion = 0;
        changed = TRUE;
    }

    pthread_mutex_unlock(&obj->lock);

    if (changed) notify_waiters(obj);
    object_release(obj);
    return STATUS_SUCCESS;
}

NTSTATUS ntsync_query_mutex(HANDLE handle, MUTANT_BASIC_INFORMATION *info)
{
    struct ntsync_object *obj;
    NTSTATUS status;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;
    if (!info) return STATUS_INVALID_PARAMETER;

    status = get_object_ref(handle, NTSYNC_TYPE_MUTEX, &obj);
    if (status) return status;

    pthread_mutex_lock(&obj->lock);
    info->CurrentCount = obj->owner_tid ? 0 : 1;
    info->OwnedByCaller = (obj->owner_tid == GetCurrentThreadId());
    info->AbandonedState = obj->abandoned;
    pthread_mutex_unlock(&obj->lock);

    object_release(obj);
    return STATUS_SUCCESS;
}

NTSTATUS ntsync_create_semaphore(HANDLE *handle, ACCESS_MASK access,
                                 const OBJECT_ATTRIBUTES *attr, LONG initial, LONG max)
{
    struct ntsync_object *obj;
    NTSTATUS status;

    (void)access;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;
    ntsync_init();

    if (!handle) return STATUS_INVALID_PARAMETER;
    *handle = 0;

    if (max <= 0 || initial < 0 || initial > max) return STATUS_INVALID_PARAMETER;
    if (attr_is_named(attr)) return STATUS_NOT_IMPLEMENTED;

    obj = alloc_object(NTSYNC_TYPE_SEMAPHORE);
    if (!obj) return STATUS_NO_MEMORY;

    obj->sem_count = initial;
    obj->sem_max = max;

    pthread_mutex_lock(&table_lock);
    status = alloc_slot_locked(obj, handle);
    pthread_mutex_unlock(&table_lock);

    if (status) object_release(obj);
    return status;
}

NTSTATUS ntsync_open_semaphore(HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr)
{
    (void)access;
    (void)attr;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;
    if (handle) *handle = 0;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS ntsync_release_semaphore(HANDLE handle, ULONG count, ULONG *prev_count)
{
    struct ntsync_object *obj;
    NTSTATUS status;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;
    if (!count) return STATUS_INVALID_PARAMETER;

    status = get_object_ref(handle, NTSYNC_TYPE_SEMAPHORE, &obj);
    if (status) return status;

    pthread_mutex_lock(&obj->lock);
    if (obj->sem_count + (LONG)count > obj->sem_max)
    {
        pthread_mutex_unlock(&obj->lock);
        object_release(obj);
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }

    if (prev_count) *prev_count = obj->sem_count;
    obj->sem_count += count;
    pthread_mutex_unlock(&obj->lock);

    notify_waiters(obj);
    object_release(obj);
    return STATUS_SUCCESS;
}

NTSTATUS ntsync_query_semaphore(HANDLE handle, SEMAPHORE_BASIC_INFORMATION *info)
{
    struct ntsync_object *obj;
    NTSTATUS status;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;
    if (!info) return STATUS_INVALID_PARAMETER;

    status = get_object_ref(handle, NTSYNC_TYPE_SEMAPHORE, &obj);
    if (status) return status;

    pthread_mutex_lock(&obj->lock);
    info->CurrentCount = obj->sem_count;
    info->MaximumCount = obj->sem_max;
    pthread_mutex_unlock(&obj->lock);

    object_release(obj);
    return STATUS_SUCCESS;
}

static BOOL object_ready_nolock(struct ntsync_object *obj, DWORD tid, BOOL *abandoned)
{
    *abandoned = FALSE;

    switch (obj->type)
    {
    case NTSYNC_TYPE_EVENT:
        return obj->signaled;

    case NTSYNC_TYPE_SEMAPHORE:
        return obj->sem_count > 0;

    case NTSYNC_TYPE_MUTEX:
        if (obj->owner_tid == tid) return TRUE;
        if (!obj->owner_tid)
        {
            if (obj->abandoned) *abandoned = TRUE;
            return TRUE;
        }
        return FALSE;
    }

    return FALSE;
}

static BOOL object_consume_nolock(struct ntsync_object *obj, DWORD tid, BOOL *abandoned)
{
    BOOL was_abandoned = FALSE;

    switch (obj->type)
    {
    case NTSYNC_TYPE_EVENT:
        if (!obj->signaled) return FALSE;
        if (!obj->manual_reset) obj->signaled = 0;
        return TRUE;

    case NTSYNC_TYPE_SEMAPHORE:
        if (obj->sem_count <= 0) return FALSE;
        obj->sem_count--;
        return TRUE;

    case NTSYNC_TYPE_MUTEX:
        if (obj->owner_tid == tid)
        {
            obj->recursion++;
            return TRUE;
        }
        if (obj->owner_tid) return FALSE;

        was_abandoned = obj->abandoned;
        obj->owner_tid = tid;
        obj->recursion = 1;
        obj->abandoned = FALSE;
        if (abandoned) *abandoned = was_abandoned;
        return TRUE;
    }

    return FALSE;
}

static NTSTATUS try_wait_any(DWORD count, struct ntsync_object **objs, DWORD tid)
{
    DWORD i;

    for (i = 0; i < count; ++i)
    {
        BOOL abandoned = FALSE;
        BOOL consumed = FALSE;

        pthread_mutex_lock(&objs[i]->lock);
        if (object_ready_nolock(objs[i], tid, &abandoned))
            consumed = object_consume_nolock(objs[i], tid, &abandoned);
        pthread_mutex_unlock(&objs[i]->lock);

        if (consumed)
            return abandoned ? (STATUS_ABANDONED_WAIT_0 + i) : (STATUS_WAIT_0 + i);
    }

    return STATUS_PENDING;
}

static NTSTATUS try_wait_all(DWORD count, struct ntsync_object **objs, DWORD tid)
{
    struct ntsync_object *sorted[MAXIMUM_WAIT_OBJECTS];
    DWORD order[MAXIMUM_WAIT_OBJECTS];
    BOOL any_abandoned = FALSE;
    DWORD i, j;
    NTSTATUS status = STATUS_PENDING;

    for (i = 0; i < count; ++i) order[i] = i;

    for (i = 0; i < count; ++i)
    {
        for (j = i + 1; j < count; ++j)
        {
            if (objs[order[j]] < objs[order[i]])
            {
                DWORD tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    for (i = 0; i < count; ++i) sorted[i] = objs[order[i]];

    for (i = 0; i < count; ++i) pthread_mutex_lock(&sorted[i]->lock);

    for (i = 0; i < count; ++i)
    {
        BOOL abandoned = FALSE;
        if (!object_ready_nolock(objs[i], tid, &abandoned)) goto out;
        any_abandoned |= abandoned;
    }

    for (i = 0; i < count; ++i)
    {
        BOOL abandoned = FALSE;
        if (!object_consume_nolock(objs[i], tid, &abandoned)) goto out;
        any_abandoned |= abandoned;
    }

    status = any_abandoned ? STATUS_ABANDONED_WAIT_0 : STATUS_SUCCESS;

out:
    for (i = count; i > 0; --i) pthread_mutex_unlock(&sorted[i - 1]->lock);
    return status;
}

static ULONGLONG monotonic_now_ns(void)
{
#ifdef CLOCK_MONOTONIC
    struct timespec ts;
    if (!clock_gettime(CLOCK_MONOTONIC, &ts))
        return (ULONGLONG)ts.tv_sec * NSECS_PER_SEC + (ULONGLONG)ts.tv_nsec;
#endif
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (ULONGLONG)tv.tv_sec * NSECS_PER_SEC + (ULONGLONG)tv.tv_usec * 1000ULL;
    }
}

static void get_realtime(struct timespec *ts)
{
#ifdef CLOCK_REALTIME
    if (!clock_gettime(CLOCK_REALTIME, ts)) return;
#endif
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        ts->tv_sec = tv.tv_sec;
        ts->tv_nsec = (long)tv.tv_usec * 1000L;
    }
}

static void add_100ns_to_timespec(const struct timespec *base, ULONGLONG ticks_100ns, struct timespec *out)
{
    ULONGLONG add_sec = ticks_100ns / TICKS_PER_SEC;
    ULONGLONG add_nsec = (ticks_100ns % TICKS_PER_SEC) * 100ULL;
    ULONGLONG sec = (ULONGLONG)base->tv_sec;
    ULONGLONG nsec = (ULONGLONG)base->tv_nsec + add_nsec;

    if (nsec >= NSECS_PER_SEC)
    {
        sec += nsec / NSECS_PER_SEC;
        nsec %= NSECS_PER_SEC;
    }

    if (ULLONG_MAX - sec < add_sec)
        sec = ULLONG_MAX;
    else
        sec += add_sec;

    if (sec > (ULONGLONG)LLONG_MAX) sec = (ULONGLONG)LLONG_MAX;

    out->tv_sec = (time_t)sec;
    out->tv_nsec = (long)nsec;
}

static enum timeout_mode timeout_to_abs_timespec(const LARGE_INTEGER *timeout, struct timespec *abs)
{
    LONGLONG q;

    if (!timeout) return TIMEOUT_MODE_INFINITE;

    q = timeout->QuadPart;
    if (q == NTSYNC_TIMEOUT_INFINITE) return TIMEOUT_MODE_INFINITE;
    if (q == 0) return TIMEOUT_MODE_IMMEDIATE;

    if (q < 0)
    {
        struct timespec now;
        ULONGLONG rel_100ns = (ULONGLONG)(-q);

        get_realtime(&now);
        add_100ns_to_timespec(&now, rel_100ns, abs);
        return TIMEOUT_MODE_TIMED;
    }

    {
        ULONGLONG epoch_diff_100ns = SECS_1601_TO_1970 * TICKS_PER_SEC;
        ULONGLONG abs_100ns = (ULONGLONG)q;
        ULONGLONG unix_100ns;

        if (abs_100ns <= epoch_diff_100ns) return TIMEOUT_MODE_IMMEDIATE;

        unix_100ns = abs_100ns - epoch_diff_100ns;
        abs->tv_sec = (time_t)(unix_100ns / TICKS_PER_SEC);
        abs->tv_nsec = (long)((unix_100ns % TICKS_PER_SEC) * 100ULL);
        return TIMEOUT_MODE_TIMED;
    }
}

static BOOL observed_change(DWORD count, struct ntsync_object **objs, ULONG *obj_seq, ULONG *last_wait_seq)
{
    BOOL changed = FALSE;
    ULONG wseq;
    DWORD i;

    wseq = atomic_load_ulong(&wait_seq);
    if (wseq != *last_wait_seq)
    {
        *last_wait_seq = wseq;
        changed = TRUE;
    }

    for (i = 0; i < count; ++i)
    {
        ULONG seq = atomic_load_ulong(&objs[i]->seq);
        if (seq != obj_seq[i])
        {
            obj_seq[i] = seq;
            changed = TRUE;
        }
    }

    return changed;
}

static void spin_pause(unsigned int iter)
{
    if (!(iter & 63)) sched_yield();
    else __asm__ __volatile__("" ::: "memory");
}

NTSTATUS ntsync_wait_objects(DWORD count, const HANDLE *handles, BOOLEAN wait_any,
                             BOOLEAN alertable, const LARGE_INTEGER *timeout)
{
    struct ntsync_object *objs[MAXIMUM_WAIT_OBJECTS];
    ULONG obj_seq[MAXIMUM_WAIT_OBJECTS];
    struct timespec abs_timeout;
    enum timeout_mode mode;
    ULONGLONG spin_deadline;
    ULONG last_wait_seq;
    DWORD tid;
    DWORD i;
    unsigned int spin_iter = 0;
    NTSTATUS status;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;
    if (alertable) return STATUS_NOT_IMPLEMENTED;

    if (!count || count > MAXIMUM_WAIT_OBJECTS) return STATUS_INVALID_PARAMETER_1;

    if (!wait_any)
    {
        for (i = 0; i < count; ++i)
        {
            DWORD j;
            for (j = i + 1; j < count; ++j)
                if (handles[i] == handles[j]) return STATUS_INVALID_PARAMETER_MIX;
        }
    }

    for (i = 0; i < count; ++i)
    {
        status = get_object_ref(handles[i], -1, &objs[i]);
        if (status == STATUS_NOT_IMPLEMENTED)
        {
            DWORD k;
            for (k = 0; k < i; ++k) object_release(objs[k]);
            return i ? STATUS_INVALID_HANDLE : STATUS_NOT_IMPLEMENTED;
        }
        if (status)
        {
            DWORD k;
            for (k = 0; k < i; ++k) object_release(objs[k]);
            return status;
        }

        obj_seq[i] = atomic_load_ulong(&objs[i]->seq);
    }

    mode = timeout_to_abs_timespec(timeout, &abs_timeout);
    tid = GetCurrentThreadId();
    last_wait_seq = atomic_load_ulong(&wait_seq);
    spin_deadline = monotonic_now_ns() + (ULONGLONG)NTSYNC_SPIN_USEC * 1000ULL;

    for (;;)
    {
        status = wait_any ? try_wait_any(count, objs, tid) : try_wait_all(count, objs, tid);
        if (status != STATUS_PENDING) break;

        if (mode == TIMEOUT_MODE_IMMEDIATE)
        {
            status = STATUS_TIMEOUT;
            break;
        }

        if (monotonic_now_ns() < spin_deadline)
        {
            spin_pause(spin_iter++);
            continue;
        }

        if (observed_change(count, objs, obj_seq, &last_wait_seq))
        {
            spin_deadline = monotonic_now_ns() + (ULONGLONG)NTSYNC_SPIN_USEC * 1000ULL;
            spin_iter = 0;
            continue;
        }

        pthread_mutex_lock(&wait_lock);

        if (!observed_change(count, objs, obj_seq, &last_wait_seq))
        {
            if (mode == TIMEOUT_MODE_TIMED)
            {
                int ret = pthread_cond_timedwait(&wait_cond, &wait_lock, &abs_timeout);
                if (ret == ETIMEDOUT)
                {
                    pthread_mutex_unlock(&wait_lock);
                    status = STATUS_TIMEOUT;
                    break;
                }
            }
            else
            {
                pthread_cond_wait(&wait_cond, &wait_lock);
            }
        }

        pthread_mutex_unlock(&wait_lock);

        spin_deadline = monotonic_now_ns() + (ULONGLONG)NTSYNC_SPIN_USEC * 1000ULL;
        spin_iter = 0;
    }

    for (i = 0; i < count; ++i) object_release(objs[i]);
    return status;
}

NTSTATUS ntsync_signal_and_wait(HANDLE signal, HANDLE wait, BOOLEAN alertable,
                                const LARGE_INTEGER *timeout)
{
    BOOL signal_ntsync;
    BOOL wait_ntsync;
    NTSTATUS status;

    if (!do_ntsync()) return STATUS_NOT_IMPLEMENTED;
    if (alertable) return STATUS_NOT_IMPLEMENTED;

    signal_ntsync = ntsync_is_handle(signal);
    wait_ntsync = ntsync_is_handle(wait);

    if (!signal_ntsync && !wait_ntsync) return STATUS_NOT_IMPLEMENTED;
    if (!signal_ntsync || !wait_ntsync) return STATUS_INVALID_HANDLE;

    status = ntsync_set_event(signal, NULL);
    if (status == STATUS_OBJECT_TYPE_MISMATCH)
    {
        status = ntsync_release_mutex(signal, NULL);
        if (status == STATUS_OBJECT_TYPE_MISMATCH)
            status = ntsync_release_semaphore(signal, 1, NULL);
    }

    if (status) return status;
    return ntsync_wait_objects(1, &wait, TRUE, alertable, timeout);
}
