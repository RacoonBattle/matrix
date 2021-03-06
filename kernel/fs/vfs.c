#include <stddef.h>
#include <string.h>
#include <errno.h>
#include "matrix/matrix.h"
#include "mm/malloc.h"
#include "mm/slab.h"
#include "mutex.h"
#include "proc/process.h"
#include "rtl/fsrtl.h"
#include "fs.h"
#include "kstrdup.h"
#include "debug.h"

/* List of registered File Systems */
static struct list _fs_list = {
	.prev = &_fs_list,
	.next = &_fs_list
};
static struct mutex _fs_list_lock;

/* List of all mounts */
static struct list _mount_list = {
	.prev = &_mount_list,
	.next = &_mount_list
};
static struct mutex _mount_list_lock;

/* Cache of Virtual File System node structure */
static slab_cache_t _vfs_node_cache;

/* Mount at the root of the File System */
struct vfs_mount *_root_mount = NULL;

struct vfs_node *vfs_node_alloc(struct vfs_mount *mnt, uint32_t type,
				struct vfs_node_ops *ops, void *data)
{
	struct vfs_node *n;

	n = (struct vfs_node *)slab_cache_alloc(&_vfs_node_cache);
	if (n) {
		memset(n, 0, sizeof(struct vfs_node));
		n->ref_count = 0;
		n->type = type;
		n->ops = ops;
		n->data = data;
		n->mounted = NULL;
		n->mount = mnt;
	}

	return n;
}

void vfs_node_free(struct vfs_node *node)
{
	ASSERT(node->ref_count == 0);

	DEBUG(DL_DBG, ("node(%s) mount(%p).\n", node->name, node->mount));

	/* If the node has a mount, remove it from the node cache */
	if (node->mount) {
		avl_tree_remove(&node->mount->nodes, node->ino);
	}
	
	slab_cache_free(&_vfs_node_cache, node);
}

int vfs_node_refer(struct vfs_node *node)
{
	int ref_count;

	ref_count = node->ref_count;
	if (ref_count < 0) {
		DEBUG(DL_ERR, ("node(%s:%d) %p corrupted.\n",
			       node->name, node->ino, node));
		PANIC("vfs_node_refer: ref_count is corrupted!");
	}
	node->ref_count++;
	
	return ref_count;
}

int vfs_node_deref(struct vfs_node *node)
{
	int ref_count;

	ref_count = node->ref_count;
	if (ref_count <= 0) {
		DEBUG(DL_ERR, ("node(%s:%d) %p corrupted.\n",
			       node->name, node->ino, node));
		PANIC("vfs_node_deref: ref_count is corrupted!");
	}
	node->ref_count--;
	if (!node->ref_count) {
		vfs_node_free(node);
	}

	return ref_count;
}

struct vfs_node *vfs_node_clone(struct vfs_node *src)
{
	struct vfs_node *n;

	n = NULL;

	if (!src) {
		goto out;
	}

	/* VFS node should never be allocated by kmalloc */
	n = (struct vfs_node *)slab_cache_alloc(&_vfs_node_cache);
	if (n) {
		memcpy(n, src, sizeof(struct vfs_node));
	}

 out:
	return n;
}

int vfs_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{
	int rc = -1;

	if (!node || !buffer) {
		goto out;
	}

	if (!node->ops) {
		rc = EGENERIC;
		DEBUG(DL_INF, ("no ops on node %s.\n", node->name));
		goto out;
	}

	if (node->ops->read != NULL) {
		rc = node->ops->read(node, offset, size, buffer);
	} else {
		DEBUG(DL_DBG, ("read node failed, operation not support.\n"));
	}

 out:
	return rc;
}

int vfs_write(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{
	int rc = -1;

	if (!node || !buffer) {
		goto out;
	}

	if (!node->ops) {
		rc = EGENERIC;
		DEBUG(DL_INF, ("no ops on node %s.\n", node->name));
		goto out;
	}

	if (node->ops->write != NULL) {
		rc = node->ops->write(node, offset, size, buffer);
	} else {
		rc = 0;
	}

 out:
	return rc;
}

int vfs_create(const char *path, uint32_t type, struct vfs_node **np)
{
	int rc = -1;
	char *dir, *name;
	struct vfs_node *parent, *n;

	parent = NULL;
	n = NULL;
	dir = NULL;
	name = NULL;

	/* Split the path into directory and file name */
	rc = split_path(path, &dir, &name, 0);
	if (rc != 0) {
		goto out;
	}
	
	/* Check whether file name is valid */
	if ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0)) {
		goto out;
	}

	/* Lookup the parent node */
	parent = vfs_lookup(dir, VFS_DIRECTORY);
	if (!parent) {
		DEBUG(DL_DBG, ("parent not exist.\n"));
		goto out;
	}

	if (!parent->ops->create) {
		DEBUG(DL_DBG, ("create not supported by file system.\n"));
		goto out;
	}

	/* Create file node */
	rc = parent->ops->create(parent, name, type, &n);
	if (rc != 0) {
		goto out;
	} else {
		vfs_node_refer(n);
	}

	ASSERT(n != NULL);
	DEBUG(DL_DBG, ("create(%s:%d) node(%p) ref_count(%d).\n",
		       path, n->ino, n, n->ref_count));

	if (np) {
		*np = n;
		n = NULL;
	}

 out:
	if (parent) {
		vfs_node_deref(parent);
	}
	if (n) {
		vfs_node_deref(n);
	}
	if (dir) {
		kfree(dir);
	}
	if (name) {
		kfree(name);
	}
	
	return rc;
}

int vfs_close(struct vfs_node *node)
{
	int rc = -1;

	if (!node) {
		goto out;
	}
	
	if (node->ops->close != NULL) {
		rc = node->ops->close(node);
		if (rc != 0) {
			DEBUG(DL_DBG, ("close (%s) failed.\n", node->name));
		}
	}

 out:
	return rc;
}

int vfs_readdir(struct vfs_node *node, uint32_t index, struct dirent **dentry)
{
	int rc = -1;

	if (!node) {
		goto out;
	}

	if (node->type != VFS_DIRECTORY) {
		DEBUG(DL_DBG, ("node(%s:%x) is not directory.\n",
			       node->name, node->type));
		goto out;
	}

	if (node->ops->readdir != NULL) {
		rc = node->ops->readdir(node, index, dentry);
	} else {
		DEBUG(DL_INF, ("node(%s:%x) readdir not support.\n",
			       node->name, node->type));
	}

 out:
	return rc;
}

int vfs_finddir(struct vfs_node *node, const char *name, ino_t *id)
{
	int rc = -1;

	if (!node || !name) {
		goto out;
	}

	if (node->type != VFS_DIRECTORY) {
		DEBUG(DL_DBG, ("node(%s:%x) is not directory.\n",
			       node->name, node->type));
		goto out;
	}

	if (node->ops->finddir != NULL) {
		rc = node->ops->finddir(node, name, id);
	} else {
		DEBUG(DL_INF, ("node(%s:%x) finddir not support.\n",
			       node->name, node->type));
	}

 out:
	return rc;
}

static struct vfs_node *vfs_lookup_internal(struct vfs_node *n, char *path)
{
	int rc = -1;
	char *tok = NULL;
	ino_t ino = 0;
	struct vfs_node *v;
	struct vfs_mount *m;

	/* Check whether the path is absolute path */
	if (path[0] == '/') {
		/* Drop the node we were provided, if any */
		if (n) {
			vfs_node_deref(n);
		}

		/* Stripe off all '/' characters at the start of the path. */
		while (path[0] == '/') {
			path++;
		}

		/* Get the root node of the current process. */
		ASSERT(CURR_PROC->ioctx.rd);
		n = CURR_PROC->ioctx.rd;
		vfs_node_refer(n);

		ASSERT(n->type == VFS_DIRECTORY);

		/* Return the root node if the end of the path has been reached */
		if (!path[0]) {
			return n;
		}
	} else {
		ASSERT((n != NULL) && (n->type == VFS_DIRECTORY));
	}

	/* Loop through each element of the path string */
	while (TRUE) {

		tok = path;
		if (path) {
			path = strchr(path, '/');
			if (path) {
				path[0] = '\0';
				path++;
			}
		}

		if (!tok) {
			/* The last token was the last element of the path string,
			 * return the current node we're currently on.
			 */
			DEBUG(DL_DBG, ("returned %s:%d\n", n->name, n->ino));
			return n;
		} else if (n->type != VFS_DIRECTORY) {
			/* The previous token was not a directory, this means the
			 * path string is trying to treat a non-directory as a
			 * directory. Reject this.
			 */
			DEBUG(DL_DBG, ("component(%s) type(0x%x)\n",
				       n->name, n->type));
			vfs_node_deref(n);
			return NULL;
		} else if (!tok[0]) {
			/* Zero-length path component, do nothing */
			continue;
		}

		/* Look up this name within the directory */
		rc = vfs_finddir(n, tok, &ino);
		if (rc != 0) {
			DEBUG(DL_DBG, ("vfs_finddir(%s) failed, err(%x).\n", tok, rc));
			vfs_node_deref(n);
			return NULL;
		}

		m = n->mount;
		mutex_acquire(&m->lock);
		v = n;

		DEBUG(DL_DBG, ("looking for (%s) in node(%s) ino(%d).\n", tok, n->name, ino));
		
		/* Lookup the node in cached tree first */
		n = avl_tree_lookup(&m->nodes, ino);
		if (n) {
			ASSERT((n->mount == m) && (n->ino == ino));
			DEBUG(DL_DBG, ("VFS node cache name(%s) tok(%s).\n",
				       n->name, tok));
			if (n->mounted) {
				DEBUG(DL_DBG, ("node(%s) is mountpoint, root(%s).\n",
					       n->name, n->mounted->root->name));
				n = n->mounted->root;
				ASSERT(n->type == VFS_DIRECTORY);
				vfs_node_refer(n);
			} else {
				vfs_node_refer(n);
			}
		} else {
			/* The node is not in the cache tree. Load it from the
			 * file system
			 */
			if (!m->ops->read_node) {
				DEBUG(DL_DBG, ("no read_node on mount(%s).\n",
					       m->type->name));
				mutex_release(&m->lock);
				vfs_node_deref(v);
				return NULL;
			}

			rc = m->ops->read_node(m, ino, &n);
			if (rc != 0) {
				DEBUG(DL_INF, ("read_node failed, mount(%s).\n", m->type->name));
				mutex_release(&m->lock);
				vfs_node_deref(v);
				return NULL;
			}

			ASSERT(n && n->ops != NULL);
			/* Insert the node into the node cache */
			avl_tree_insert(&m->nodes, ino, n);
			vfs_node_refer(n);

			DEBUG(DL_DBG, ("vfs node(%s:%d) miss, ref_count(%d).\n",
				       n->name, n->ino, n->ref_count));
		}

		mutex_release(&m->lock);
		ASSERT(v != NULL);
		vfs_node_deref(v);
	}
}

struct vfs_node *vfs_lookup(const char *path, int type)
{
	char *dup = NULL;
	struct vfs_node *n = NULL;
	struct vfs_node *c = NULL;
	
	if (!_root_mount || !path || !path[0]) {
		goto out;
	}

	/* Start from the current directory if the path is relative */
	if (path[0] != '/'); {
		ASSERT(CURR_PROC->ioctx.rd);
		c = CURR_PROC->ioctx.rd;
		vfs_node_refer(c);
	}

	/* Duplicate path so that vfs_lookup_internal can modify it */
	dup = kstrdup(path, 0);
	if (!dup) {
		goto out;
	}

	/* Look up the path string */
	n = vfs_lookup_internal(c, dup);
	if (n) {
		if ((type >= 0) && (n->type != type)) {
			DEBUG(DL_DBG, ("node(%s) type mismatch, n->type(%d), type(%d).\n",
				       n->name, n->type, type));
			vfs_node_deref(n);
			n = NULL;
		} else {
			DEBUG(DL_DBG, ("node(%s) ref_count(%d).\n", n->name, n->ref_count));
		}
	} else {
		DEBUG(DL_DBG, ("current node(%s), path(%s) not found.\n",
			       c->name, dup));
	}

out:
	if (dup) {
		kfree(dup);
	}
	return n;
}

static struct vfs_type *vfs_type_lookup_internal(const char *name)
{
	struct list *l;
	struct vfs_type *type;
	
	LIST_FOR_EACH(l, &_fs_list) {
		type = LIST_ENTRY(l, struct vfs_type, link);
		if (strcmp(type->name, name) == 0) {
			return type;
		}
	}

	return NULL;
}

static struct vfs_type *vfs_type_lookup(const char *name)
{
	struct vfs_type *type;

	mutex_acquire(&_fs_list_lock);

	type = vfs_type_lookup_internal(name);
	if (type) {
		atomic_inc(&type->ref_count);
	}

	mutex_release(&_fs_list_lock);

	return type;
}

int vfs_type_register(struct vfs_type *type)
{
	int rc = -1;
	
	/* Check whether the structure is valid */
	if (!type || !type->name || !type->desc) {
		return rc;
	}

	mutex_acquire(&_fs_list_lock);

	/* Check whether this File System has been registered */
	if (NULL != vfs_type_lookup_internal(type->name)) {
		DEBUG(DL_DBG, ("File system(%s) already registered.\n", type->name));
		goto out;
	}
	
	type->ref_count = 0;
	list_add_tail(&type->link, &_fs_list);

	rc = 0;
	
	DEBUG(DL_DBG, ("registered file system(%s).\n", type->name));

 out:
	mutex_release(&_fs_list_lock);

	return rc;
}

int vfs_type_unregister(struct vfs_type *type)
{
	int rc = -1;
	
	mutex_acquire(&_fs_list_lock);

	if (vfs_type_lookup_internal(type->name) != type) {
		;
	} else if (type->ref_count > 0) {
		;
	} else {
		ASSERT(type->ref_count == 0);
		list_del(&type->link);
		rc = 0;
	}

	mutex_release(&_fs_list_lock);

	return rc;
}

int vfs_mount(const char *dev, const char *path, const char *type, const void *data)
{
	int rc = -1, flags = 0;
	struct vfs_node *n = NULL;
	struct vfs_mount *mnt = NULL;

	if (!path || (!dev && !type)) {
		rc = EINVAL;
		return rc;
	}

	mutex_acquire(&_mount_list_lock);

	/* If the root File System is not mounted yet, the only place we can
	 * mount is root
	 */
	if (!_root_mount) {
		ASSERT(CURR_PROC == _kernel_proc);
		if (strcmp(path, "/") != 0) {
			PANIC("Non-root mount before root FS mounted");
		}
	} else {
		/* Look up the destination directory */
		n = vfs_lookup(path, VFS_DIRECTORY);
		if (!n) {
			rc = ENOENT;
			DEBUG(DL_DBG, ("vfs_lookup(%s) not found.\n", path));
			goto out;
		}
		ASSERT(n->type == VFS_DIRECTORY);

		/* Check whether it is being used as a mount point already */
		if (n->mount->root == n) {
			rc = -1;
			DEBUG(DL_DBG, ("%s is already a mount point"));
			goto out;
		}
	}

	/* Initialize the mount structure */
	mnt = kmalloc(sizeof(struct vfs_mount), 0);
	if (!mnt) {
		rc = ENOMEM;
		goto out;
	}
	LIST_INIT(&mnt->link);
	mutex_init(&mnt->lock, "fs-mnt-mutex", 0);
	avl_tree_init(&mnt->nodes);
	mnt->flags = flags;
	mnt->mnt_point = n;
	mnt->root = NULL;
	mnt->type = NULL;
	mnt->data = NULL;

	/* If a type is specified, look up it */
	if (type) {
		mnt->type = vfs_type_lookup(type);
		if (!mnt->type) {
			rc = EINVAL;
			DEBUG(DL_DBG, ("vfs_type_lookup(%s) not found.\n", type));
			goto out;
		}
	}

	/* Call the File System's mount function to do the mount, if mount
	 * function was successfully called, mnt->root will point to the
	 * new root
	 */
	ASSERT(mnt->type->mount != NULL);
	rc = mnt->type->mount(mnt, flags, data);
	if (rc != 0) {
		DEBUG(DL_DBG, ("mount failed, err(%x).\n", rc));
		goto out;
	} else if (!mnt->root) {
		PANIC("Mount with root not set");
	}

	ASSERT(mnt->root->ref_count >= 1);

	/* Make the mnt_point point to the new mount */
	if (mnt->mnt_point) {
		mnt->mnt_point->mounted = mnt;
	}

	/* Append the mount to the mount list */
	list_add_tail(&mnt->link, &_mount_list);
	if (!_root_mount) {
		/* The first mount is the root mount */
		_root_mount = mnt;
		vfs_node_refer(_root_mount->root);
	}

	DEBUG(DL_DBG, ("mounted (%s) on (%s), FS type(%s).\n",
		       mnt->root->name, mnt->mnt_point->name, type));

 out:
	if (rc != 0) {
		if (mnt) {
			if (mnt->type) {
				atomic_dec(&mnt->type->ref_count);
			}
			kfree(mnt);
		}
		if (n) {
			vfs_node_deref(n);
		}
	}
	mutex_release(&_mount_list_lock);

	return rc;
}

static int vfs_umount_internal(struct vfs_mount *mnt, struct vfs_node *n)
{
	int rc = -1;

	if (n) {
		if (n != mnt->root) {
			return rc;
		} else if (!mnt->mnt_point) {
			return rc;
		}
	}

	return rc;
}

/* Unmount a file system */
int vfs_umount(const char *path)
{
	int rc = -1;
	struct vfs_node *n;

	if (!path) {
		goto out;
	}

	/* Acquire the mount lock first */
	mutex_acquire(&_mount_list_lock);

	n = vfs_lookup(path, VFS_DIRECTORY);
	if (n) {
		rc = vfs_umount_internal(n->mount, n);
	} else {
		DEBUG(DL_DBG, ("vfs_lookup(%s) not found.\n", path));
	}

	/* Release the mount lock */
	mutex_release(&_mount_list_lock);
	
 out:
	return rc;
}

/* Initialize the File System layer */
void init_fs()
{
	/* Initialize the fs list lock and mount list lock */
	mutex_init(&_fs_list_lock, "fs-mutex", 0);
	mutex_init(&_mount_list_lock, "mnt-mutex", 0);

	/* Initialize the vfs node cache */
	slab_cache_init(&_vfs_node_cache, "vfs-cache", sizeof(struct vfs_node),
			NULL, NULL, 0);
}
