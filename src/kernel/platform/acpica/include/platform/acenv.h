#ifndef __ACENV_H__
#define __ACENV_H__

#if defined(OS1_HOST_TEST)
#define ACPI_USE_SYSTEM_CLIBRARY
#define ACPI_USE_STANDARD_HEADERS
#endif

#include <stddef.h>
#include <stdint.h>

#if defined(ACPI_USE_STANDARD_HEADERS)
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#if defined(OS1_ACPICA_RENAME_UTPRINT)
#define snprintf os1_acpica_snprintf
#define sprintf os1_acpica_sprintf
#define vsnprintf os1_acpica_vsnprintf
#endif

#define ACPI_BINARY_SEMAPHORE 0
#define ACPI_OSL_MUTEX 1

#define DEBUGGER_SINGLE_THREADED 0
#define DEBUGGER_MULTI_THREADED 1

#define ACPI_MACHINE_WIDTH 64
#define ACPI_REDUCED_HARDWARE 1
#define ACPI_SINGLE_THREADED
#if !defined(OS1_ACPICA_ERROR_DIAGNOSTICS)
#define ACPI_NO_ERROR_MESSAGES
#endif
#define ACPI_USE_LOCAL_CACHE
#define ACPI_USE_BUILTIN_STDARG

#include "platform/acgcc.h"

#ifndef COMPILER_DEPENDENT_INT64
#define COMPILER_DEPENDENT_INT64 long long
#endif

#ifndef COMPILER_DEPENDENT_UINT64
#define COMPILER_DEPENDENT_UINT64 unsigned long long
#endif

#ifndef ACPI_MUTEX_TYPE
#define ACPI_MUTEX_TYPE ACPI_BINARY_SEMAPHORE
#endif

#ifndef ACPI_ACQUIRE_GLOBAL_LOCK
#define ACPI_ACQUIRE_GLOBAL_LOCK(GLptr, Acquired) Acquired = 1
#endif

#ifndef ACPI_RELEASE_GLOBAL_LOCK
#define ACPI_RELEASE_GLOBAL_LOCK(GLptr, Pending) Pending = 0
#endif

#ifndef ACPI_SEMAPHORE_NULL
#define ACPI_SEMAPHORE_NULL NULL
#endif

#ifndef ACPI_FLUSH_CPU_CACHE
#define ACPI_FLUSH_CPU_CACHE()
#endif

#ifndef ACPI_STRUCT_INIT
#define ACPI_STRUCT_INIT(field, value) value
#endif

#ifndef ACPI_SYSTEM_XFACE
#define ACPI_SYSTEM_XFACE
#endif

#ifndef ACPI_EXTERNAL_XFACE
#define ACPI_EXTERNAL_XFACE
#endif

#ifndef ACPI_INTERNAL_XFACE
#define ACPI_INTERNAL_XFACE
#endif

#ifndef ACPI_INTERNAL_VAR_XFACE
#define ACPI_INTERNAL_VAR_XFACE
#endif

#ifndef DEBUGGER_THREADING
#define DEBUGGER_THREADING DEBUGGER_SINGLE_THREADED
#endif

#ifndef ACPI_UINTPTR_T
#define ACPI_UINTPTR_T uintptr_t
#endif

#define ACPI_FILE void *
#define ACPI_FILE_OUT NULL
#define ACPI_FILE_ERR NULL

#ifndef ACPI_INIT_FUNCTION
#define ACPI_INIT_FUNCTION
#endif

#endif