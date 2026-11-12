/* Haku /dev header.
 *
 * The built-in entries (null, zero, urandom, console, tty) live in the
 * backend itself. Loadable modules add their own entries through
 * devfs_register_device; each one remembers how many descriptors are open so
 * a module cannot be unloaded while a device it owns is still in use. */
#ifndef HAKU_DEVFS_H
#define HAKU_DEVFS_H

#include "cpu.h"

/* One device backend. read/write return 1 or a negative -VFS_E* errno and
 * report the transferred count through out_count; a device that has no
 * meaningful read or write side can return 1 with a zero count. The name is
 * copied into the table, so it may live in a loadable module. */
typedef struct {
    const char *name;
    int32_t (*read)(void *buffer, uint32_t count, uint32_t *out_count);
    int32_t (*write)(const void *buffer, uint32_t count,
                     uint32_t *out_count);
} devfs_device_t;

/* Register the backend. The caller mounts it with
 * vfs_mount("dev", "devfs", 0). */
int devfs_init(void);

/* Add a device to /dev. Returns 1, or a negative -VFS_E* errno (-EEXIST for
 * a duplicate name, -ENOSPC when the module table is full). */
int32_t devfs_register_device(const devfs_device_t *device);

/* Remove a device added by devfs_register_device. Returns 1, or -EBUSY when
 * a descriptor is still open on it, -ENOENT for an unknown name. */
int32_t devfs_unregister_device(const char *name);

#endif
