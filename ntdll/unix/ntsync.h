/*
 * ntsync - userspace synchronization primitives
 * Header for Wine userspace ntsync backend (macOS-compatible)
 *
 * Originally inspired by the Linux ntsync backend for Wine.
 * This implementation is designed to be used as a replacement for Wine's unix/sync.c
 * and provides userspace versions of NT synchronization objects on macOS.
 */

#ifndef __WINE_NTDLL_UNIX_NTSYNC_H
#define __WINE_NTDLL_UNIX_NTSYNC_H

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"

int do_ntsync(void);
void ntsync_init(void);
BOOL ntsync_is_handle(HANDLE handle);

NTSTATUS ntsync_close(HANDLE handle);
NTSTATUS ntsync_duplicate(HANDLE source_process, HANDLE source, HANDLE dest_process, HANDLE *dest,
                          ACCESS_MASK access, ULONG attributes, ULONG options);

NTSTATUS ntsync_create_event(HANDLE *handle, ACCESS_MASK access,
                             const OBJECT_ATTRIBUTES *attr, EVENT_TYPE type, BOOLEAN initial);
NTSTATUS ntsync_open_event(HANDLE *handle, ACCESS_MASK access,
                           const OBJECT_ATTRIBUTES *attr);
NTSTATUS ntsync_set_event(HANDLE handle, LONG *prev_state);
NTSTATUS ntsync_reset_event(HANDLE handle, LONG *prev_state);
NTSTATUS ntsync_pulse_event(HANDLE handle, LONG *prev_state);
NTSTATUS ntsync_query_event(HANDLE handle, EVENT_BASIC_INFORMATION *info);

NTSTATUS ntsync_create_mutex(HANDLE *handle, ACCESS_MASK access,
                             const OBJECT_ATTRIBUTES *attr, BOOLEAN initial);
NTSTATUS ntsync_open_mutex(HANDLE *handle, ACCESS_MASK access,
                           const OBJECT_ATTRIBUTES *attr);
NTSTATUS ntsync_release_mutex(HANDLE handle, LONG *prev_count);
NTSTATUS ntsync_query_mutex(HANDLE handle, MUTANT_BASIC_INFORMATION *info);

NTSTATUS ntsync_create_semaphore(HANDLE *handle, ACCESS_MASK access,
                                 const OBJECT_ATTRIBUTES *attr, LONG initial, LONG max);
NTSTATUS ntsync_open_semaphore(HANDLE *handle, ACCESS_MASK access,
                               const OBJECT_ATTRIBUTES *attr);
NTSTATUS ntsync_release_semaphore(HANDLE handle, ULONG count, ULONG *prev_count);
NTSTATUS ntsync_query_semaphore(HANDLE handle, SEMAPHORE_BASIC_INFORMATION *info);

NTSTATUS ntsync_wait_objects(DWORD count, const HANDLE *handles, BOOLEAN wait_any,
                             BOOLEAN alertable, const LARGE_INTEGER *timeout);
NTSTATUS ntsync_signal_and_wait(HANDLE signal, HANDLE wait, BOOLEAN alertable,
                                const LARGE_INTEGER *timeout);

#endif
