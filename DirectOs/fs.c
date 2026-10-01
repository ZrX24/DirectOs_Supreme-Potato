#include "fs.h"

typedef struct {
    char name[FS_NAME_MAX];
    char data[FS_DATA_MAX];
    unsigned int size;
    int parent;
    unsigned char used;
    unsigned char is_dir;
    unsigned char permissions;
} FsNode;

static FsNode nodes[FS_NODE_MAX];
static int current_directory;

static int text_equal(const char *left, const char *right)
{
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return *left == *right;
}

static unsigned int text_length(const char *text)
{
    unsigned int length = 0;
    while (text[length]) length++;
    return length;
}

static void text_copy(char *target, const char *source, unsigned int capacity)
{
    unsigned int i = 0;
    if (!capacity) return;
    while (i + 1 < capacity && source[i]) {
        target[i] = source[i];
        i++;
    }
    target[i] = 0;
}

static int find_child(int parent, const char *name)
{
    int i;
    for (i = 1; i < FS_NODE_MAX; i++) {
        if (nodes[i].used && nodes[i].parent == parent && text_equal(nodes[i].name, name)) return i;
    }
    return -1;
}

static int resolve_from(const char *path, int starting_node)
{
    char component[FS_NAME_MAX];
    int node = starting_node;
    unsigned int position = 0;

    if (!path || !*path) return node;
    while (path[position]) {
        unsigned int length = 0;
        while (path[position] == '/') position++;
        if (!path[position]) break;
        while (path[position] && path[position] != '/') {
            if (length + 1 >= FS_NAME_MAX) return -1;
            component[length++] = path[position++];
        }
        component[length] = 0;
        if (text_equal(component, ".")) continue;
        if (text_equal(component, "..")) {
            node = nodes[node].parent >= 0 ? nodes[node].parent : 0;
            continue;
        }
        if (!nodes[node].is_dir) return -1;
        node = find_child(node, component);
        if (node < 0) return -1;
    }
    return node;
}

int fs_resolve(const char *path)
{
    if (!path || !*path) return current_directory;
    return resolve_from(path, path[0] == '/' ? 0 : current_directory);
}

static int resolve_parent(const char *path, int *parent, char *name)
{
    char parent_path[FS_PATH_MAX];
    unsigned int length;
    unsigned int split;
    unsigned int i;
    int node;

    if (!path || !*path) return 0;
    length = text_length(path);
    if (length >= FS_PATH_MAX) return 0;
    while (length && path[length - 1] == '/') length--;
    if (!length) return 0;
    split = length;
    while (split && path[split - 1] != '/') split--;
    if (length - split == 0 || length - split >= FS_NAME_MAX) return 0;
    for (i = 0; i < length - split; i++) name[i] = path[split + i];
    name[length - split] = 0;
    if (text_equal(name, ".") || text_equal(name, "..")) return 0;
    if (!split) {
        node = current_directory;
    } else if (split == 1) {
        node = 0;
    } else {
        for (i = 0; i < split - 1; i++) parent_path[i] = path[i];
        parent_path[split - 1] = 0;
        node = fs_resolve(parent_path);
    }
    if (node < 0 || !nodes[node].is_dir) return 0;
    *parent = node;
    return 1;
}

void fs_init(void)
{
    int i;
    for (i = 0; i < FS_NODE_MAX; i++) {
        nodes[i].used = 0;
        nodes[i].size = 0;
        nodes[i].parent = -1;
    }
    nodes[0].used = 1;
    nodes[0].is_dir = 1;
    nodes[0].permissions = 7;
    nodes[0].name[0] = '/';
    nodes[0].name[1] = 0;
    current_directory = 0;
    fs_create("/readme.txt", 0);
    {
        const char *welcome = "DirectOS RAM file system is ready.\nFiles are volatile and reset on reboot.\n";
        fs_write("/readme.txt", welcome, text_length(welcome));
    }
}

int fs_chdir(const char *path)
{
    int node = fs_resolve(path);
    if (node < 0 || !nodes[node].is_dir) return 0;
    current_directory = node;
    return 1;
}

void fs_get_cwd(char *output, unsigned int capacity)
{
    int chain[FS_NODE_MAX];
    int count = 0;
    int node = current_directory;
    unsigned int used = 0;
    int i;

    if (!capacity) return;
    if (!node) {
        text_copy(output, "/", capacity);
        return;
    }
    while (node > 0 && count < FS_NODE_MAX) {
        chain[count++] = node;
        node = nodes[node].parent;
    }
    output[used++] = '/';
    for (i = count - 1; i >= 0; i--) {
        unsigned int j = 0;
        while (nodes[chain[i]].name[j] && used + 1 < capacity) output[used++] = nodes[chain[i]].name[j++];
        if (i && used + 1 < capacity) output[used++] = '/';
    }
    output[used] = 0;
}

int fs_create(const char *path, int is_dir)
{
    char name[FS_NAME_MAX];
    int parent;
    int i;
    if (!resolve_parent(path, &parent, name) || find_child(parent, name) >= 0) return 0;
    for (i = 1; i < FS_NODE_MAX; i++) {
        if (!nodes[i].used) {
            nodes[i].used = 1;
            nodes[i].is_dir = is_dir ? 1 : 0;
            nodes[i].permissions = 6;
            nodes[i].parent = parent;
            nodes[i].size = 0;
            text_copy(nodes[i].name, name, FS_NAME_MAX);
            nodes[i].data[0] = 0;
            return 1;
        }
    }
    return 0;
}

int fs_write(const char *path, const char *data, unsigned int size)
{
    int node = fs_resolve(path);
    unsigned int i;
    if (node < 0) {
        if (!fs_create(path, 0)) return 0;
        node = fs_resolve(path);
    }
    if (node < 0 || nodes[node].is_dir || size >= FS_DATA_MAX) return 0;
    for (i = 0; i < size; i++) nodes[node].data[i] = data[i];
    nodes[node].data[size] = 0;
    nodes[node].size = size;
    return 1;
}

const char *fs_read(const char *path, unsigned int *size)
{
    int node = fs_resolve(path);
    if (node < 0 || nodes[node].is_dir) return 0;
    if (size) *size = nodes[node].size;
    return nodes[node].data;
}

int fs_remove(const char *path, int is_dir)
{
    int node = fs_resolve(path);
    int i;
    if (node <= 0 || node == current_directory || nodes[node].is_dir != (is_dir ? 1 : 0)) return 0;
    if (is_dir) {
        for (i = 1; i < FS_NODE_MAX; i++) {
            if (nodes[i].used && nodes[i].parent == node) return 0;
        }
    }
    nodes[node].used = 0;
    nodes[node].size = 0;
    return 1;
}

int fs_move(const char *source, const char *destination)
{
    char name[FS_NAME_MAX];
    int node = fs_resolve(source);
    int parent;
    int target;
    int ancestor;
    if (node <= 0) return 0;
    target = fs_resolve(destination);
    if (target >= 0) {
        if (!nodes[target].is_dir) return 0;
        parent = target;
        text_copy(name, nodes[node].name, FS_NAME_MAX);
    } else if (!resolve_parent(destination, &parent, name)) return 0;
    target = find_child(parent, name);
    if (target == node) return 1;
    if (target >= 0) return 0;
    if (nodes[node].is_dir) {
        ancestor = parent;
        while (ancestor > 0) {
            if (ancestor == node) return 0;
            ancestor = nodes[ancestor].parent;
        }
    }
    nodes[node].parent = parent;
    text_copy(nodes[node].name, name, FS_NAME_MAX);
    return 1;
}

int fs_child_count(int directory)
{
    int i;
    int count = 0;
    for (i = 1; i < FS_NODE_MAX; i++) {
        if (nodes[i].used && nodes[i].parent == directory) count++;
    }
    return count;
}

int fs_child_at(int directory, int index)
{
    int i;
    int count = 0;
    for (i = 1; i < FS_NODE_MAX; i++) {
        if (nodes[i].used && nodes[i].parent == directory) {
            if (count++ == index) return i;
        }
    }
    return -1;
}

void fs_get_info(int node, FsInfo *info)
{
    unsigned int i;
    if (!info) return;
    info->name[0] = 0;
    info->size = 0;
    info->node_id = 0;
    info->is_dir = 0;
    info->permissions = 0;
    if (node < 0 || node >= FS_NODE_MAX || !nodes[node].used) return;
    for (i = 0; i < FS_NAME_MAX; i++) info->name[i] = nodes[node].name[i];
    info->size = nodes[node].size;
    info->node_id = (unsigned int)node;
    info->is_dir = nodes[node].is_dir;
    info->permissions = nodes[node].permissions;
}

int fs_chmod(const char *path, unsigned char permissions)
{
    int node = fs_resolve(path);
    if (node < 0 || permissions > 7) return 0;
    nodes[node].permissions = permissions;
    return 1;
}

void fs_format(void)
{
    fs_init();
}

void fs_stats(unsigned int *used_nodes, unsigned int *free_nodes, unsigned int *used_bytes)
{
    unsigned int used = 0;
    unsigned int bytes = 0;
    int i;
    for (i = 0; i < FS_NODE_MAX; i++) {
        if (nodes[i].used) {
            used++;
            bytes += nodes[i].size;
        }
    }
    if (used_nodes) *used_nodes = used;
    if (free_nodes) *free_nodes = FS_NODE_MAX - used;
    if (used_bytes) *used_bytes = bytes;
}

int fs_check(void)
{
    int i;
    int j;
    if (!nodes[0].used || !nodes[0].is_dir || nodes[0].parent != -1) return 0;
    for (i = 1; i < FS_NODE_MAX; i++) {
        if (!nodes[i].used) continue;
        if (nodes[i].parent < 0 || nodes[i].parent >= FS_NODE_MAX || !nodes[nodes[i].parent].used || !nodes[nodes[i].parent].is_dir) return 0;
        if (nodes[i].size >= FS_DATA_MAX) return 0;
        for (j = i + 1; j < FS_NODE_MAX; j++) {
            if (nodes[j].used && nodes[i].parent == nodes[j].parent && text_equal(nodes[i].name, nodes[j].name)) return 0;
        }
    }
    return 1;
}
