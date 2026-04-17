// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "index.h"
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

typedef struct TreeNode TreeNode;
typedef struct {
    char name[256];
    uint32_t mode;
    ObjectID hash;
} FileItem;

struct TreeNode {
    char name[256];
    FileItem *files;
    int file_count;
    int file_cap;
    TreeNode **children;
    int child_count;
    int child_cap;
};

static TreeNode *new_node(const char *name) {
    TreeNode *n = calloc(1, sizeof(TreeNode));
    if (!n) return NULL;
    if (name) snprintf(n->name, sizeof(n->name), "%s", name);
    return n;
}

static TreeNode *find_child(TreeNode *parent, const char *name) {
    for (int i = 0; i < parent->child_count; i++) {
        if (strcmp(parent->children[i]->name, name) == 0) return parent->children[i];
    }
    return NULL;
}

static int add_child(TreeNode *parent, TreeNode *child) {
    if (parent->child_count == parent->child_cap) {
        int next_cap = parent->child_cap == 0 ? 4 : parent->child_cap * 2;
        TreeNode **next = realloc(parent->children, (size_t)next_cap * sizeof(TreeNode *));
        if (!next) return -1;
        parent->children = next;
        parent->child_cap = next_cap;
    }
    parent->children[parent->child_count++] = child;
    return 0;
}

static int add_file(TreeNode *node, const char *name, uint32_t mode, const ObjectID *hash) {
    if (strlen(name) >= sizeof(node->files[0].name)) return -1;
    if (node->file_count == node->file_cap) {
        int next_cap = node->file_cap == 0 ? 8 : node->file_cap * 2;
        FileItem *next = realloc(node->files, (size_t)next_cap * sizeof(FileItem));
        if (!next) return -1;
        node->files = next;
        node->file_cap = next_cap;
    }
    FileItem *it = &node->files[node->file_count++];
    snprintf(it->name, sizeof(it->name), "%s", name);
    it->mode = mode;
    it->hash = *hash;
    return 0;
}

static void free_tree(TreeNode *node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) free_tree(node->children[i]);
    free(node->children);
    free(node->files);
    free(node);
}

static int write_node(TreeNode *node, ObjectID *out) {
    Tree tree = {0};

    for (int i = 0; i < node->file_count; i++) {
        if (tree.count >= MAX_TREE_ENTRIES) return -1;
        TreeEntry *e = &tree.entries[tree.count++];
        e->mode = node->files[i].mode;
        e->hash = node->files[i].hash;
        snprintf(e->name, sizeof(e->name), "%s", node->files[i].name);
    }

    for (int i = 0; i < node->child_count; i++) {
        ObjectID child_id;
        if (write_node(node->children[i], &child_id) != 0) return -1;
        if (tree.count >= MAX_TREE_ENTRIES) return -1;
        TreeEntry *e = &tree.entries[tree.count++];
        e->mode = MODE_DIR;
        e->hash = child_id;
        snprintf(e->name, sizeof(e->name), "%s", node->children[i]->name);
    }

    void *serialized = NULL;
    size_t serialized_len = 0;
    if (tree_serialize(&tree, &serialized, &serialized_len) != 0) return -1;
    int rc = object_write(OBJ_TREE, serialized, serialized_len, out);
    free(serialized);
    return rc;
}

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    Index index = {0};
    FILE *f = fopen(INDEX_FILE, "r");
    if (f) {
        char mode_str[16];
        char hex[HASH_HEX_SIZE + 1];
        unsigned long long mtime = 0;
        unsigned int size = 0;
        char path[512];
        while (fscanf(f, "%15s %64s %llu %u %511[^\n]\n",
                      mode_str, hex, &mtime, &size, path) == 5) {
            if (index.count >= MAX_INDEX_ENTRIES) {
                fclose(f);
                return -1;
            }
            IndexEntry *e = &index.entries[index.count++];
            e->mode = (uint32_t)strtoul(mode_str, NULL, 8);
            if (hex_to_hash(hex, &e->hash) != 0) {
                fclose(f);
                return -1;
            }
            e->mtime_sec = (uint64_t)mtime;
            e->size = size;
            snprintf(e->path, sizeof(e->path), "%s", path);
        }
        if (!feof(f)) {
            fclose(f);
            return -1;
        }
        fclose(f);
    } else if (access(INDEX_FILE, F_OK) == 0) {
        return -1;
    }

    TreeNode *root = new_node("");
    if (!root) return -1;

    for (int i = 0; i < index.count; i++) {
        const IndexEntry *ie = &index.entries[i];
        char path_buf[512];
        snprintf(path_buf, sizeof(path_buf), "%s", ie->path);

        TreeNode *cur = root;
        char *segment = path_buf;
        char *slash = strchr(segment, '/');

        while (slash) {
            *slash = '\0';
            if (segment[0] == '\0') {
                free_tree(root);
                return -1;
            }
            TreeNode *child = find_child(cur, segment);
            if (!child) {
                child = new_node(segment);
                if (!child || add_child(cur, child) != 0) {
                    free_tree(root);
                    return -1;
                }
            }
            cur = child;
            segment = slash + 1;
            slash = strchr(segment, '/');
        }

        if (segment[0] == '\0' || add_file(cur, segment, ie->mode, &ie->hash) != 0) {
            free_tree(root);
            return -1;
        }
    }

    int rc = write_node(root, id_out);
    free_tree(root);
    return rc;
}