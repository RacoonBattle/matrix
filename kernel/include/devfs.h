#ifndef __DEVFS_H__
#define __DEVFS_H__

typedef void * devfs_handle_t;

#define INVALID_DEVFS_HANDLE	(devfs_handle_t)(-1)

extern int devfs_register(devfs_handle_t dir, const char *name, int flags,
			  void *ops, dev_t dev_id);
extern int devfs_unregister(devfs_handle_t handle);

#endif	/* __DEVFS_H__ */
