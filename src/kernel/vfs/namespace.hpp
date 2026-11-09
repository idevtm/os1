#pragma once
#include "vfs/initrd.hpp"
// Nonblocking admission, never a spinlock held across filesystem I/O.
// All published namespace mutations use this guard before backend locks.
struct NamespaceGuard
{
    bool held;
    NamespaceGuard();
    ~NamespaceGuard();
};
long vfs_context_open(DescriptorTable&,
                      int root,
                      const os1_context_open_request&,
                      const char* path);
long vfs_describe_path(DescriptorTable&, int root, int object, char* path, size_t capacity);
long vfs_move_at(DescriptorTable&, int source, const os1_move_request&);
long vfs_same_object(DescriptorTable&, int left, int right);

// NamespaceGuard must be held. Current backends cap tree depth at 32.
inline bool vfs_contains(VfsNode* tree, VfsNode* target)
{
    if(tree == target)
        return true;
    for(auto* child = tree->children; child; child = child->next)
        if(vfs_contains(child, target))
            return true;
    return false;
}
inline unsigned vfs_height(VfsNode* tree)
{
    unsigned height = 0;
    for(auto* child = tree->children; child; child = child->next)
    {
        unsigned h = 1 + vfs_height(child);
        if(h > height)
            height = h;
    }
    return height;
}
inline void vfs_detach(VfsNode* parent, VfsNode* node)
{
    auto** link = &parent->children;
    while(*link != node)
        link = &(*link)->next;
    *link = node->next;
    node->next = nullptr;
}

uint64_t vfs_identity();
uint64_t vfs_generation();  // NamespaceGuard required.
void vfs_changed(VfsNode*,
                 VfsNode* = nullptr,
                 VfsNode* = nullptr,
                 VfsNode* = nullptr);        // NamespaceGuard required, after publication.
long vfs_metadata(VfsNode*, os1_metadata&);  // NamespaceGuard required.
long vfs_space(VfsNode*, os1_space&);        // NamespaceGuard required.
long vfs_tree_step(DescriptorTable&, int, os1_tree_step&);

// Caller holds NamespaceGuard. Current-time mutation accepts access/modify bits only.
long vfs_times(VfsNode*, os1_file_times&);
long vfs_set_times(VfsNode*, const os1_set_times&);
