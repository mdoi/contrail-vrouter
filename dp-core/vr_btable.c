/*
 * vr_btable.c -- Big tables. With (kernel)malloc, there is a limitation of
 * how much contiguous memory we will get (4M). So, for allocations more than
 * 4M, we need a way to manage the requests, and that's where big tables come
 * in. Basically, a two level table.
 *
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */
#include <vr_os.h>
#include "vr_btable.h"

/*
 * The aim of btable is to workaround kernel's limitation of 4M allocation
 * size by allocating multiple chunks of 4M for a huge allocation.
 *
 * In the linux world, while vmalloc can provide a huge chunk of memory,
 * kmalloc is preferred to vmalloc for the following reasons
 *
 * - lesser TLB misses
 * - vmalloc is restricted in 32 bit systems
 * - potential pagefaults
 *
 * Also, in 2.6, there are problems with mmap-ing k(mz)alloced memory (for
 * flow table). So, a page based allocation is what btable will follow.
 *
 * The basic oprations supported are alloc, free, and get. get is defined in
 * the header file as an inline function for performance reasons.
 */

/*
 * the discontiguous chunks of memory are seen as partitions, and hence the
 * nomenclature
 */
struct vr_btable_partition *
vr_btable_get_partition(struct vr_btable *table, unsigned int partition)
{
    if (partition >= VR_MAX_BTABLE_ENTRIES)
        return NULL;

    return &table->vb_table_info[partition];
}

/*
 * given an offset into the total memory managed by the btable (i.e memory
 * across all partitions), return the corresponding virtual address
 */
void *
vr_btable_get_address(struct vr_btable *table, unsigned int offset)
{
    unsigned int i;
    struct vr_btable_partition *partition;

    for (i = 0; i < table->vb_partitions; i++) {
        partition = vr_btable_get_partition(table, i);
        if (!partition)
            break;

        if (offset >= partition->vb_offset && 
                offset < partition->vb_offset + partition->vb_mem_size)
            return table->vb_mem[i] + (offset - partition->vb_offset);
    }

    return NULL;
}

void
vr_btable_free(struct vr_btable *table)
{
    unsigned int i;

    if (!table)
        return;

    for (i = 0; i < VR_MAX_BTABLE_ENTRIES; i++) {
        if (table->vb_mem[i])
            vr_page_free(table->vb_mem[i], table->vb_table_info[i].vb_mem_size);
    }

    vr_free(table);
    return;
}

struct vr_btable *
vr_btable_alloc(unsigned int num_entries, unsigned int entry_size)
{
    unsigned int i = 0, num_parts, remainder;
    uint64_t total_mem;
    struct vr_btable *table;
    unsigned int offset = 0;

    total_mem = num_entries * entry_size;
    /* need more testing. that's all */
    if (total_mem > VR_KNOWN_BIG_MEM_LIMIT)
        return NULL;

    table = vr_zalloc(sizeof(*table));
    if (!table)
        return NULL;

    num_parts = total_mem / VR_SINGLE_ALLOC_LIMIT;
    remainder = total_mem % VR_SINGLE_ALLOC_LIMIT;
    if (num_parts + !!remainder > VR_MAX_BTABLE_ENTRIES)
        return NULL;

    if (num_parts) {
        /*
         * the entry size has to be a factor of VR_SINGLE_ALLOC limit.
         * otherwise, we might access memory beyond the allocated chunk
         * while accessing the last entry
         */
        if (VR_SINGLE_ALLOC_LIMIT % entry_size)
            return NULL;
    }

    if (num_parts) {
        for (i = 0; i < num_parts; i++) {
            table->vb_mem[i] = vr_page_alloc(VR_SINGLE_ALLOC_LIMIT);
            if (!table->vb_mem[i])
                goto exit_alloc;
            table->vb_table_info[i].vb_mem_size = VR_SINGLE_ALLOC_LIMIT;
            table->vb_table_info[i].vb_offset = offset;
            offset += table->vb_table_info[i].vb_mem_size;
        }
        table->vb_partitions = num_parts;
    }

    if (remainder) {
        table->vb_mem[i] = vr_page_alloc(remainder);
        if (!table->vb_mem[i])
            goto exit_alloc;
        table->vb_table_info[i].vb_mem_size = remainder;
        table->vb_table_info[i].vb_offset = offset;
        table->vb_partitions++;
    }

    table->vb_entries = num_entries;
    table->vb_esize = entry_size;

    return table;

exit_alloc:
    vr_btable_free(table);
    return NULL;
}
