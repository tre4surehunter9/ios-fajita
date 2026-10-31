/*
 * The few IOKit declarations phone/ code needs: the iPhoneOS SDK ships IOKit.framework
 * (the functions) but not its headers.
 */
#pragma once
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef mach_port_t io_object_t;
typedef io_object_t io_registry_entry_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
kern_return_t IORegistryEntryGetRegistryEntryID(io_registry_entry_t entry, uint64_t *entryID);
kern_return_t IOObjectRelease(io_object_t object);
#ifdef __cplusplus
}
#endif
