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
