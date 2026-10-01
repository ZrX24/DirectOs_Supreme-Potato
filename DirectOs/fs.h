#ifndef FS_H
#define FS_H

#define FS_NAME_MAX 24
#define FS_PATH_MAX 96
#define FS_DATA_MAX 512
#define FS_NODE_MAX 32

typedef struct {
    char name[FS_NAME_MAX];
    unsigned int size;
    unsigned int node_id;
    unsigned char is_dir;
    unsigned char permissions;
} FsInfo;

void fs_init(void);
int fs_resolve(const char *path);
int fs_chdir(const char *path);
void fs_get_cwd(char *output, unsigned int capacity);
int fs_create(const char *path, int is_dir);
int fs_write(const char *path, const char *data, unsigned int size);
const char *fs_read(const char *path, unsigned int *size);
int fs_remove(const char *path, int is_dir);
int fs_move(const char *source, const char *destination);
int fs_child_count(int directory);
int fs_child_at(int directory, int index);
void fs_get_info(int node, FsInfo *info);
int fs_chmod(const char *path, unsigned char permissions);
void fs_format(void);
void fs_stats(unsigned int *used_nodes, unsigned int *free_nodes, unsigned int *used_bytes);
int fs_check(void);

#endif
