#include <string.h>
#include <time.h>

#include "types.h"
#include "vfs.h"
#include "dfs.h"

int dfs_format(vfs_devno_t devno, char *opts);

int dfs_mount(struct fs *fs, char *opts);

int dfs_unmount(struct fs *fs);

int dfs_open(struct file *filp, char *name, int mode);

int dfs_close(struct file *filp);

int dfs_flush(struct file *filp);

int dfs_read(struct file *filp, void *data, size_t size);

int dfs_write(struct file *filp, void *data, size_t size);

vfs_loff_t dfs_tell(struct file *filp);

vfs_loff_t dfs_lseek(struct file *filp, vfs_loff_t offset, int origin);

int dfs_chsize(struct file *filp, vfs_loff_t size);

int dfs_futime(struct file *filp, struct vfs_utimbuf *times);

int dfs_fstat(struct file *filp, struct vfs_stat *buffer);

int dfs_stat(struct fs *fs, char *name, struct vfs_stat *buffer);

int dfs_mkdir(struct fs *fs, char *name, int mode);

int dfs_rmdir(struct fs *fs, char *name);

int dfs_rename(struct fs *fs, char *oldname, char *newname);

int dfs_link(struct fs *fs, char *oldname, char *newname);

int dfs_unlink(struct fs *fs, char *name);

int dfs_opendir(struct file *filp, char *name);

int dfs_readdir(struct file *filp, struct vfs_dirent *dirp, int count);

struct fsops dfsops =
    {
        dfs_format,
        dfs_mount,
        dfs_unmount,

        dfs_open,
        dfs_close,
        dfs_flush,

        dfs_read,
        dfs_write,

        dfs_tell,
        dfs_lseek,
        dfs_chsize,

        dfs_futime,

        dfs_fstat,
        dfs_stat,

        dfs_mkdir,
        dfs_rmdir,

        dfs_rename,
        dfs_link,
        dfs_unlink,

        dfs_opendir,
        dfs_readdir};

void dfs_init()
{
    register_filesystem("dfs", &dfsops);
}

int dfs_stat(struct fs *fs, char *name, struct vfs_stat *buffer)
{
    vfs_ino_t ino;
    struct inode *inode;

    ino = lookup_name((struct filsys *)fs->data, DFS_INODE_ROOT, name, strlen(name));
    if (ino == -1)
        return -1;

    inode = get_inode((struct filsys *)fs->data, ino);
    if (!inode)
        return -1;

    buffer->mode = inode->desc->mode;
    buffer->ino = ino;
    buffer->atime = time(NULL);
    buffer->mtime = inode->desc->mtime;
    buffer->ctime = inode->desc->ctime;

    buffer->quad.size_low = (int)inode->desc->size;
    buffer->quad.size_high = 0;

    release_inode(inode);

    return 0;
}

int dfs_mkdir(struct fs *fs, char *name, int mode)
{
    struct inode *parent;
    struct inode *dir;
    int len;

    len = strlen(name);
    parent = parse_name((struct filsys *)fs->data, &name, &len);
    if (!parent)
        return -1;

    if (find_dir_entry(parent, name, len) != -1)
    {
        release_inode(parent);
        return -1;
    }

    dir = alloc_inode(parent, VFS_S_IFDIR | mode);
    if (!dir)
    {
        release_inode(parent);
        return -1;
    }

    dir->desc->linkcount++;
    mark_inode_dirty(dir);

    if (add_dir_entry(parent, name, len, dir->ino) < 0)
    {
        unlink_inode(dir);
        release_inode(dir);
        release_inode(parent);
        return -1;
    }

    release_inode(dir);
    release_inode(parent);
    return 0;
}

int dfs_rmdir(struct fs *fs, char *name)
{
    struct inode *parent;
    struct inode *dir;
    vfs_ino_t ino;
    int len;

    len = strlen(name);
    parent = parse_name((struct filsys *)fs->data, &name, &len);
    if (!parent)
        return -1;

    ino = find_dir_entry(parent, name, len);
    if (ino == -1 || ino == DFS_INODE_ROOT)
    {
        release_inode(parent);
        return -1;
    }

    dir = get_inode(parent->fs, ino);
    if (!dir)
    {
        release_inode(parent);
        return -1;
    }

    if (!VFS_S_ISDIR(dir->desc->mode) || dir->desc->size > 0)
    {
        release_inode(dir);
        release_inode(parent);
        return -1;
    }

    if (delete_dir_entry(parent, name, len) < 0)
    {
        release_inode(dir);
        release_inode(parent);
        return -1;
    }

    if (unlink_inode(dir) < 0)
    {
        release_inode(dir);
        release_inode(parent);
        return -1;
    }

    release_inode(dir);
    release_inode(parent);
    return 0;
}

int dfs_rename(struct fs *fs, char *oldname, char *newname)
{
    struct inode *inode;
    struct inode *oldparent;
    struct inode *newparent;
    int oldlen;
    int newlen;
    vfs_ino_t ino;

    oldlen = strlen(oldname);
    oldparent = parse_name((struct filsys *)fs->data, &oldname, &oldlen);
    if (!oldparent)
        return -1;

    ino = find_dir_entry(oldparent, oldname, oldlen);
    if (ino == -1)
    {
        release_inode(oldparent);
        return -1;
    }

    inode = get_inode(oldparent->fs, ino);
    if (!inode)
    {
        release_inode(oldparent);
        return -1;
    }

    newlen = strlen(newname);
    newparent = parse_name((struct filsys *)fs->data, &newname, &newlen);
    if (!newparent)
    {
        release_inode(inode);
        release_inode(oldparent);
        return -1;
    }

    if (add_dir_entry(newparent, newname, newlen, inode->ino) < 0)
    {
        release_inode(inode);
        release_inode(oldparent);
        release_inode(newparent);
        return -1;
    }

    if (delete_dir_entry(oldparent, oldname, oldlen) < 0)
    {
        release_inode(inode);
        release_inode(oldparent);
        release_inode(newparent);
        return -1;
    }

    release_inode(inode);
    release_inode(oldparent);
    release_inode(newparent);
    return 0;
}

int dfs_link(struct fs *fs, char *oldname, char *newname)
{
    struct inode *inode;
    struct inode *parent;
    vfs_ino_t ino;
    int len;

    ino = lookup_name((struct filsys *)fs->data, DFS_INODE_ROOT, oldname, strlen(oldname));
    if (ino == -1)
        return -1;

    inode = get_inode((struct filsys *)fs->data, ino);
    if (!inode)
        return -1;

    len = strlen(newname);
    parent = parse_name((struct filsys *)fs->data, &newname, &len);
    if (!parent)
    {
        release_inode(inode);
        return -1;
    }

    if (find_dir_entry(parent, newname, len) != -1)
    {
        release_inode(inode);
        release_inode(parent);
        return -1;
    }

    inode->desc->linkcount++;
    mark_inode_dirty(inode);

    if (add_dir_entry(parent, newname, len, inode->ino) < 0)
    {
        unlink_inode(inode);
        release_inode(inode);
        release_inode(parent);
        return -1;
    }

    release_inode(inode);
    release_inode(parent);
    return 0;
}

int dfs_unlink(struct fs *fs, char *name)
{
    struct inode *dir;
    struct inode *inode;
    vfs_ino_t ino;
    int len;

    len = strlen(name);
    dir = parse_name((struct filsys *)fs->data, &name, &len);
    if (!dir)
        return -1;

    ino = find_dir_entry(dir, name, len);
    if (ino == -1)
    {
        release_inode(dir);
        return -1;
    }

    inode = get_inode(dir->fs, ino);
    if (!inode)
    {
        release_inode(dir);
        return -1;
    }

    if (VFS_S_ISDIR(inode->desc->mode))
    {
        release_inode(inode);
        release_inode(dir);
        return -1;
    }

    if (delete_dir_entry(dir, name, len) < 0)
    {
        release_inode(inode);
        release_inode(dir);
        return -1;
    }

    if (unlink_inode(inode) < 0)
    {
        release_inode(inode);
        release_inode(dir);
        return -1;
    }

    release_inode(inode);
    release_inode(dir);
    return 0;
}
