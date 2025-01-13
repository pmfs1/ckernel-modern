//
// Kernel memory page allocator
//
#include <os/krnl.h>

#define OSVMAP_PAGES 1
#define OSVMAP_ENTRIES (OSVMAP_PAGES * PAGESIZE / sizeof(struct rmap))

#define KMODMAP_PAGES 1
#define KMODMAP_ENTRIES (KMODMAP_PAGES * PAGESIZE / sizeof(struct rmap))

struct rmap *osvmap = (struct rmap *) OSVMAP_ADDRESS;
struct rmap *kmodmap = (struct rmap *) KMODMAP_ADDRESS;

void *alloc_pages(int pages, unsigned long tag) {
    char *vaddr;
    int i;
    unsigned long pfn;

    if (tag == 0) tag = 'KMEM';
    vaddr = (char *) PTOB(rmap_alloc(osvmap, pages));
    for (i = 0; i < pages; i++) {
        pfn = alloc_pageframe(tag);
        map_page(vaddr + PTOB(i), pfn, PT_WRITABLE | PT_PRESENT);
    }

    //kprintf("alloc kmem %dK @ %p (%d KB free)\n", pages * (PAGESIZE / K), vaddr, freemem * (PAGESIZE / K));
    return vaddr;
}

void *alloc_pages_align(int pages, int align, unsigned long tag) {
    char *vaddr;
    int i;
    unsigned long pfn;

    if (tag == 0) tag = 'KMEM';
    vaddr = (char *) PTOB(rmap_alloc_align(osvmap, pages, align));
    for (i = 0; i < pages; i++) {
        pfn = alloc_pageframe(tag);
        map_page(vaddr + PTOB(i), pfn, PT_WRITABLE | PT_PRESENT);
    }

    //kprintf("alloc kmem %dK @ %p (align %dK, %d KB free)\n", pages * (PAGESIZE / K), vaddr, align * (PAGESIZE / K), freemem * (PAGESIZE / K));
    return vaddr;
}

void *alloc_pages_linear(int pages, unsigned long tag) {
    char *vaddr;
    int i;
    unsigned long pfn;

    if (tag == 0) tag = 'KMEM';
    pfn = alloc_linear_pageframes(pages, tag);
    if (pfn == 0xFFFFFFFF) return 0;
    vaddr = (char *) PTOB(rmap_alloc(osvmap, pages));
    for (i = 0; i < pages; i++) {
        map_page(vaddr + PTOB(i), pfn, PT_WRITABLE | PT_PRESENT);
        pfn++;
    }

    //kprintf("alloc kmem linear %dK @ %p (%d KB free)\n", pages * (PAGESIZE / K), vaddr, freemem * (PAGESIZE / K));

    return vaddr;
}

void free_pages(void *addr, int pages) {
    int i;
    unsigned long pfn;

    //kprintf("free kmem %dK @ %p\n", pages * PAGESIZE / K, addr);

    for (i = 0; i < pages; i++) {
        pfn = BTOP(virt2phys((char *) addr + PTOB(i)));
        free_pageframe(pfn);
        unmap_page((char *) addr + PTOB(i));
    }

    rmap_free(osvmap, BTOP(addr), pages);
}

void *iomap(unsigned long addr, int size) {
    char *vaddr;
    int i;
    int pages = PAGES(size);

    vaddr = (char *) PTOB(rmap_alloc(osvmap, pages));
    for (i = 0; i < pages; i++) {
        map_page(vaddr + PTOB(i), BTOP(addr) + i, PT_WRITABLE | PT_PRESENT);
    }

    return vaddr;
}

void iounmap(void *addr, int size) {
    int i;
    int pages = PAGES(size);

    for (i = 0; i < pages; i++) unmap_page((char *) addr + PTOB(i));
    rmap_free(osvmap, BTOP(addr), pages);
}

void *alloc_module_mem(int pages) {
    char *vaddr;
    int i;
    unsigned long pfn;

    vaddr = (char *) PTOB(rmap_alloc(kmodmap, pages));
    for (i = 0; i < pages; i++) {
        pfn = alloc_pageframe('KMOD');
        map_page(vaddr + PTOB(i), pfn, PT_WRITABLE | PT_PRESENT);
        memset(vaddr + PTOB(i), 0, PAGESIZE);
    }

    //kprintf("alloc mod mem %dK @ %p\n", pages * PAGESIZE / K, vaddr);

    return vaddr;
}

void free_module_mem(void *addr, int pages) {
    int i;
    unsigned long pfn;

    //kprintf("free mod mem %dK @ %p\n", pages * PAGESIZE / K, addr);

    for (i = 0; i < pages; i++) {
        pfn = BTOP(virt2phys((char *) addr + PTOB(i)));
        free_pageframe(pfn);
        unmap_page((char *) addr + PTOB(i));
    }

    rmap_free(kmodmap, BTOP(addr), pages);
}

void init_kmem() {
    int pfn;
    struct image_header *imghdr;

    // Allocate page frame for kernel heap resource map and map into syspages
    pfn = alloc_pageframe('SYS');
    map_page(osvmap, pfn, PT_WRITABLE | PT_PRESENT);

    // Initialize resource map for kernel heap
    rmap_init(osvmap, OSVMAP_ENTRIES);

    // Add kernel heap address space to osvmap
    rmap_free(osvmap, BTOP(KHEAPBASE), BTOP(KHEAPSIZE));

    // Allocate page frame for kernel module map and map into syspages
    pfn = alloc_pageframe('SYS');
    map_page(kmodmap, pfn, PT_WRITABLE | PT_PRESENT);

    // Initialize resource map for kernel module area
    rmap_init(kmodmap, KMODMAP_ENTRIES);

    // Add kernel heap address space to kmodmap
    imghdr = get_image_header((hmodule_t) OSBASE);
    rmap_free(kmodmap, BTOP(OSBASE), BTOP(KMODSIZE));
    rmap_reserve(kmodmap, BTOP(OSBASE), BTOP(imghdr->optional.size_of_image));
}

int list_memmap(struct proc_file *pf, struct rmap *rmap, unsigned int startpos) {
    struct rmap *r;
    struct rmap *rlim;
    unsigned int pos = startpos;
    unsigned int total = 0;
    struct pdirstat stat;

    pprintf(pf, "   start      end      size committed  readonly       gap\n");
    pprintf(pf, "-------- -------- --------- --------- --------- ---------\n");

    rlim = &rmap[rmap->offset];

    for (r = &rmap[1]; r <= rlim; r++) {
        unsigned int size = r->offset - pos;

        if (size > 0) {
            pdir_stat((void *) (pos * PAGESIZE), size * PAGESIZE, &stat);
            pprintf(pf, "%08X %08X %8dK %8dK %8dK %8dK\n",
                    pos * PAGESIZE,
                    r->offset * PAGESIZE - 1,
                    size * (PAGESIZE / 1024),
                    stat.present * (PAGESIZE / 1024),
                    stat.readonly * (PAGESIZE / 1024),
                    r->size * (PAGESIZE / 1024));

            total += size;
        }
        pos = r->offset + r->size;
    }
    pprintf(pf, "Total: %dK\n", total * PAGESIZE / 1024);
    return 0;
}

int kmem_proc(struct proc_file *pf, void *arg) {
    return list_memmap(pf, osvmap, BTOP(KHEAPBASE));
}

int kmodmem_proc(struct proc_file *pf, void *arg) {
    return list_memmap(pf, kmodmap, BTOP(OSBASE));
}
