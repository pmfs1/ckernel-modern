//
// Kernel heap allocator
//
#ifndef KMALLOC_H
#define KMALLOC_H

struct bucket {
    void *mem;            // List of chunks of memory
    unsigned long elems;  // # chunks available in this bucket
    unsigned long pages;  // # pages used for this bucket size
    unsigned long size;   // Size of this kind of chunk
};

extern struct bucket buckets[PAGESHIFT];

krnlapi void *kmalloc_tag(int size, unsigned long tag);

krnlapi void *krealloc_tag(void *addr, int newsize, unsigned long tag);

krnlapi void *kmalloc(int size);

krnlapi void *krealloc(void *addr, int newsize);

krnlapi void kfree(void *addr);

void init_malloc();

int kheapstat_proc(struct proc_file *pf, void *arg);

void dump_malloc();

void *malloc(size_t size);

void free(void *addr);

#endif
