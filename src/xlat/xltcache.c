/**
 * $Id$
 * 
 * Translation cache management. This part is architecture independent.
 *
 * Copyright (c) 2005 Nathan Keynes.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <assert.h>

#include "dreamcast.h"
#include "sh4/sh4core.h"
#include "sh4/sh4trans.h"
#include "xlat/xltcache.h"

#define XLAT_LUT_PAGE_BITS 12
#define XLAT_LUT_TOTAL_BITS 28
#define XLAT_LUT_PAGE(addr) (((addr)>>13) & 0xFFFF)
#define XLAT_LUT_ENTRY(addr) (((addr)&0x1FFE) >> 1)

#define XLAT_LUT_PAGES (1<<(XLAT_LUT_TOTAL_BITS-XLAT_LUT_PAGE_BITS))
#define XLAT_LUT_PAGE_ENTRIES (1<<XLAT_LUT_PAGE_BITS)
#define XLAT_LUT_PAGE_SIZE (XLAT_LUT_PAGE_ENTRIES * sizeof(void *))

#define XLAT_LUT_ENTRY_EMPTY (void *)0
#define XLAT_LUT_ENTRY_USED  (void *)1

#define XLAT_ADDR_FROM_ENTRY(pagenum,entrynum) ((((pagenum)&0xFFFF)<<13)|(((entrynum)<<1)&0x1FFE))

#define NEXT(block) ( (xlat_cache_block_t)&((block)->code[(block)->size]))
#define IS_ENTRY_POINT(ent) (ent > XLAT_LUT_ENTRY_USED)
#define IS_ENTRY_USED(ent) (ent != XLAT_LUT_ENTRY_EMPTY)
#define IS_ENTRY_CONTINUATION(ent) (((uintptr_t)ent) & ((uintptr_t)XLAT_LUT_ENTRY_USED))
#define IS_FIRST_ENTRY_IN_PAGE(addr) (((addr)&0x1FFE) == 0)
#define XLAT_CODE_ADDR(ent) ((void *)(((uintptr_t)ent) & (~((uintptr_t)0x03))))
#define XLAT_BLOCK_FOR_LUT_ENTRY(ent) XLAT_BLOCK_FOR_CODE(XLAT_CODE_ADDR(ent))


#define MIN_BLOCK_SIZE 32
#define MIN_TOTAL_SIZE (sizeof(struct xlat_cache_block)+MIN_BLOCK_SIZE)

#define BLOCK_INACTIVE 0
#define BLOCK_ACTIVE 1
#define BLOCK_USED 2

xlat_cache_block_t xlat_new_cache;
xlat_cache_block_t xlat_new_cache_ptr;
xlat_cache_block_t xlat_new_create_ptr;

#ifdef XLAT_GENERATIONAL_CACHE
xlat_cache_block_t xlat_temp_cache;
xlat_cache_block_t xlat_temp_cache_ptr;
xlat_cache_block_t xlat_old_cache;
xlat_cache_block_t xlat_old_cache_ptr;
#endif

static void **xlat_lut[XLAT_LUT_PAGES];
static gboolean xlat_initialized = FALSE;
static xlat_target_fns_t xlat_target = NULL;

void xlat_cache_init(void) 
{
    if( !xlat_initialized ) {
        xlat_initialized = TRUE;
        xlat_new_cache = (xlat_cache_block_t)mmap( NULL, XLAT_NEW_CACHE_SIZE, PROT_EXEC|PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANON, -1, 0 );
        xlat_new_cache_ptr = xlat_new_cache;
        xlat_new_create_ptr = xlat_new_cache;
#ifdef XLAT_GENERATIONAL_CACHE
        xlat_temp_cache = mmap( NULL, XLAT_TEMP_CACHE_SIZE, PROT_EXEC|PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANON, -1, 0 );
        xlat_old_cache = mmap( NULL, XLAT_OLD_CACHE_SIZE, PROT_EXEC|PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANON, -1, 0 );
        xlat_temp_cache_ptr = xlat_temp_cache;
        xlat_old_cache_ptr = xlat_old_cache;
#endif
//        xlat_lut = mmap( NULL, XLAT_LUT_PAGES*sizeof(void *), PROT_READ|PROT_WRITE,
//                MAP_PRIVATE|MAP_ANON, -1, 0);
        memset( xlat_lut, 0, XLAT_LUT_PAGES*sizeof(void *) );
    }
    xlat_flush_cache();
}

void xlat_set_target_fns( xlat_target_fns_t target )
{
    xlat_target = target;
}

/**
 * Reset the cache structure to its default state
 */
void xlat_flush_cache() 
{
    xlat_cache_block_t tmp;
    int i;
    xlat_new_cache_ptr = xlat_new_cache;
    xlat_new_cache_ptr->active = 0;
    xlat_new_cache_ptr->size = XLAT_NEW_CACHE_SIZE - 2*sizeof(struct xlat_cache_block);
    tmp = NEXT(xlat_new_cache_ptr);
    tmp->active = 1;
    tmp->size = 0;
#ifdef XLAT_GENERATIONAL_CACHE
    xlat_temp_cache_ptr = xlat_temp_cache;
    xlat_temp_cache_ptr->active = 0;
    xlat_temp_cache_ptr->size = XLAT_TEMP_CACHE_SIZE - 2*sizeof(struct xlat_cache_block);
    tmp = NEXT(xlat_temp_cache_ptr);
    tmp->active = 1;
    tmp->size = 0;
    xlat_old_cache_ptr = xlat_old_cache;
    xlat_old_cache_ptr->active = 0;
    xlat_old_cache_ptr->size = XLAT_OLD_CACHE_SIZE - 2*sizeof(struct xlat_cache_block);
    tmp = NEXT(xlat_old_cache_ptr);
    tmp->active = 1;
    tmp->size = 0;
#endif
    for( i=0; i<XLAT_LUT_PAGES; i++ ) {
        if( xlat_lut[i] != NULL ) {
            memset( xlat_lut[i], 0, XLAT_LUT_PAGE_SIZE );
        }
    }
}

void xlat_delete_block( xlat_cache_block_t block )
{
    block->active = 0;
    *block->lut_entry = block->chain;
    if( block->use_list != NULL )
        xlat_target->unlink_block(block->use_list);
}

static void xlat_flush_page_by_lut( void **page )
{
    int i;
    for( i=0; i<XLAT_LUT_PAGE_ENTRIES; i++ ) {
        if( IS_ENTRY_POINT(page[i]) ) {
            void *p = XLAT_CODE_ADDR(page[i]);
            do {
                xlat_cache_block_t block = XLAT_BLOCK_FOR_CODE(p);
                xlat_delete_block(block);
                p = block->chain;
            } while( p != NULL );
        }
        page[i] = NULL;
    }
}

void FASTCALL xlat_invalidate_word( sh4addr_t addr )
{
    void **page = xlat_lut[XLAT_LUT_PAGE(addr)];
    if( page != NULL ) {
        int entry = XLAT_LUT_ENTRY(addr);
        if( entry == 0 && IS_ENTRY_CONTINUATION(page[entry]) ) {
            /* First entry may be a delay-slot for the previous page */
            xlat_flush_page_by_lut(xlat_lut[XLAT_LUT_PAGE(addr-2)]);
        }
        if( page[entry] != NULL ) {
            xlat_flush_page_by_lut(page);
        }
    }
}

void FASTCALL xlat_invalidate_long( sh4addr_t addr )
{
    void **page = xlat_lut[XLAT_LUT_PAGE(addr)];
    if( page != NULL ) {
        int entry = XLAT_LUT_ENTRY(addr);
        if( entry == 0 && IS_ENTRY_CONTINUATION(page[entry]) ) {
            /* First entry may be a delay-slot for the previous page */
            xlat_flush_page_by_lut(xlat_lut[XLAT_LUT_PAGE(addr-2)]);
        }
        if( *(uint64_t *)&page[entry] != 0 ) {
            xlat_flush_page_by_lut(page);
        }
    }
}

void FASTCALL xlat_invalidate_block( sh4addr_t address, size_t size )
{
    int i;
    int entry_count = size >> 1; // words;
    uint32_t page_no = XLAT_LUT_PAGE(address);
    int entry = XLAT_LUT_ENTRY(address);

    if( entry == 0 && xlat_lut[page_no] != NULL && IS_ENTRY_CONTINUATION(xlat_lut[page_no][entry])) {
        /* First entry may be a delay-slot for the previous page */
        xlat_flush_page_by_lut(xlat_lut[XLAT_LUT_PAGE(address-2)]);
    }
    do {
        void **page = xlat_lut[page_no];
        int page_entries = XLAT_LUT_PAGE_ENTRIES - entry;
        if( entry_count < page_entries ) {
            page_entries = entry_count;
        }
        if( page != NULL ) {
            if( page_entries == XLAT_LUT_PAGE_ENTRIES ) {
                /* Overwriting the entire page anyway */
                xlat_flush_page_by_lut(page);
            } else {
                for( i=entry; i<entry+page_entries; i++ ) {
                    if( page[i] != NULL ) {
                        xlat_flush_page_by_lut(page);
                        break;
                    }
                }
            }
            entry_count -= page_entries;
        }
        page_no ++;
        entry_count -= page_entries;
        entry = 0;
    } while( entry_count > 0 );
}

void FASTCALL xlat_flush_page( sh4addr_t address )
{
    void **page = xlat_lut[XLAT_LUT_PAGE(address)];
    if( page != NULL ) {
        xlat_flush_page_by_lut(page);
    }
}

void * FASTCALL xlat_get_code( sh4addr_t address )
{
    void *result = NULL;
    void **page = xlat_lut[XLAT_LUT_PAGE(address)];
    if( page != NULL ) {
        result = XLAT_CODE_ADDR(page[XLAT_LUT_ENTRY(address)]);
    }
    return result;
}

xlat_recovery_record_t xlat_get_pre_recovery( void *code, void *native_pc )
{
    if( code != NULL ) {
        uintptr_t pc_offset = ((uint8_t *)native_pc) - ((uint8_t *)code);
        xlat_cache_block_t block = XLAT_BLOCK_FOR_CODE(code);
        uint32_t count = block->recover_table_size;
        xlat_recovery_record_t records = (xlat_recovery_record_t)(&block->code[block->recover_table_offset]);
        uint32_t posn;
        for( posn = 1; posn < count; posn++ ) {
        	if( records[posn].xlat_offset >= pc_offset ) {
        		return &records[posn-1];
        	}
        }
        return &records[count-1];
    }
    return NULL;	
}

static void **xlat_get_lut_page( sh4addr_t address )
{
    void **page = xlat_lut[XLAT_LUT_PAGE(address)];

     /* Add the LUT entry for the block */
     if( page == NULL ) {
         xlat_lut[XLAT_LUT_PAGE(address)] = page =
             (void **)mmap( NULL, XLAT_LUT_PAGE_SIZE, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANON, -1, 0 );
         memset( page, 0, XLAT_LUT_PAGE_SIZE );
     }

     return page;
}

void ** FASTCALL xlat_get_lut_entry( sh4addr_t address )
{
    void **page = xlat_get_lut_page(address);
    return &page[XLAT_LUT_ENTRY(address)];
}



uint32_t FASTCALL xlat_get_block_size( void *block )
{
    xlat_cache_block_t xlt = (xlat_cache_block_t)(((char *)block)-sizeof(struct xlat_cache_block));
    return xlt->size;
}

uint32_t FASTCALL xlat_get_code_size( void *block )
{
    xlat_cache_block_t xlt = (xlat_cache_block_t)(((char *)block)-sizeof(struct xlat_cache_block));
    if( xlt->recover_table_offset == 0 ) {
        return xlt->size;
    } else {
        return xlt->recover_table_offset;
    }
}

/**
 * Cut the specified block so that it has the given size, with the remaining data
 * forming a new free block. If the free block would be less than the minimum size,
 * the cut is not performed.
 * @return the next block after the (possibly cut) block.
 */
static inline xlat_cache_block_t xlat_cut_block( xlat_cache_block_t block, int cutsize )
{
    cutsize = (cutsize + 3) & 0xFFFFFFFC; // force word alignment
    assert( cutsize <= block->size );
    if( block->size > cutsize + MIN_TOTAL_SIZE ) {
        int oldsize = block->size;
        block->size = cutsize;
        xlat_cache_block_t next = NEXT(block);
        next->active = 0;
        next->size = oldsize - cutsize - sizeof(struct xlat_cache_block);
        return next;
    } else {
        return NEXT(block);
    }
}

#ifdef XLAT_GENERATIONAL_CACHE
/**
 * Promote a block in temp space (or elsewhere for that matter) to old space.
 *
 * @param block to promote.
 */
static void xlat_promote_to_old_space( xlat_cache_block_t block )
{
    int allocation = (int)-sizeof(struct xlat_cache_block);
    int size = block->size;
    xlat_cache_block_t curr = xlat_old_cache_ptr;
    xlat_cache_block_t start_block = curr;
    do {
        allocation += curr->size + sizeof(struct xlat_cache_block);
        curr = NEXT(curr);
        if( allocation > size ) {
            break; /* done */
        }
        if( curr->size == 0 ) { /* End-of-cache Sentinel */
            /* Leave what we just released as free space and start again from the
             * top of the cache
             */
            start_block->active = 0;
            start_block->size = allocation;
            allocation = (int)-sizeof(struct xlat_cache_block);
            start_block = curr = xlat_old_cache;
        }
    } while(1);
    start_block->active = 1;
    start_block->size = allocation;
    start_block->lut_entry = block->lut_entry;
    start_block->chain = block->chain;
    start_block->fpscr_mask = block->fpscr_mask;
    start_block->fpscr = block->fpscr;
    start_block->recover_table_offset = block->recover_table_offset;
    start_block->recover_table_size = block->recover_table_size;
    *block->lut_entry = &start_block->code;
    memcpy( start_block->code, block->code, block->size );
    xlat_old_cache_ptr = xlat_cut_block(start_block, size );
    if( xlat_old_cache_ptr->size == 0 ) {
        xlat_old_cache_ptr = xlat_old_cache;
    }
}

/**
 * Similarly to the above method, promotes a block to temp space.
 * TODO: Try to combine these - they're nearly identical
 */
void xlat_promote_to_temp_space( xlat_cache_block_t block )
{
    int size = block->size;
    int allocation = (int)-sizeof(struct xlat_cache_block);
    xlat_cache_block_t curr = xlat_temp_cache_ptr;
    xlat_cache_block_t start_block = curr;
    do {
        if( curr->active == BLOCK_USED ) {
            xlat_promote_to_old_space( curr );
        } else if( curr->active == BLOCK_ACTIVE ) {
            // Active but not used, release block
            *((uintptr_t *)curr->lut_entry) &= ((uintptr_t)0x03);
        }
        allocation += curr->size + sizeof(struct xlat_cache_block);
        curr = NEXT(curr);
        if( allocation > size ) {
            break; /* done */
        }
        if( curr->size == 0 ) { /* End-of-cache Sentinel */
            /* Leave what we just released as free space and start again from the
             * top of the cache
             */
            start_block->active = 0;
            start_block->size = allocation;
            allocation = (int)-sizeof(struct xlat_cache_block);
            start_block = curr = xlat_temp_cache;
        }
    } while(1);
    start_block->active = 1;
    start_block->size = allocation;
    start_block->lut_entry = block->lut_entry;
    start_block->chain = block->chain;
    start_block->fpscr_mask = block->fpscr_mask;
    start_block->fpscr = block->fpscr;
    start_block->recover_table_offset = block->recover_table_offset;
    start_block->recover_table_size = block->recover_table_size;
    *block->lut_entry = &start_block->code;
    memcpy( start_block->code, block->code, block->size );
    xlat_temp_cache_ptr = xlat_cut_block(start_block, size );
    if( xlat_temp_cache_ptr->size == 0 ) {
        xlat_temp_cache_ptr = xlat_temp_cache;
    }

}
#else 
void xlat_promote_to_temp_space( xlat_cache_block_t block )
{
    *block->lut_entry = block->chain;
    xlat_delete_block(block);
}
#endif

/**
 * Returns the next block in the new cache list that can be written to by the
 * translator. If the next block is active, it is evicted first.
 */
xlat_cache_block_t xlat_start_block( sh4addr_t address )
{
    if( xlat_new_cache_ptr->size == 0 ) {
        xlat_new_cache_ptr = xlat_new_cache;
    }

    if( xlat_new_cache_ptr->active ) {
        xlat_promote_to_temp_space( xlat_new_cache_ptr );
    }
    xlat_new_create_ptr = xlat_new_cache_ptr;
    xlat_new_create_ptr->active = 1;
    xlat_new_cache_ptr = NEXT(xlat_new_cache_ptr);

    /* Add the LUT entry for the block */
    void **p = xlat_get_lut_entry(address);
    void *entry = *p;
    if( IS_ENTRY_POINT(entry) ) {
        xlat_cache_block_t oldblock = XLAT_BLOCK_FOR_LUT_ENTRY(entry);
        assert( oldblock->active );
        xlat_new_create_ptr->chain = XLAT_CODE_ADDR(entry);
    } else {
        xlat_new_create_ptr->chain = NULL;
    }
    xlat_new_create_ptr->use_list = NULL;

    *p = &xlat_new_create_ptr->code;
    if( IS_ENTRY_CONTINUATION(entry) ) {
        *((uintptr_t *)p) |= (uintptr_t)XLAT_LUT_ENTRY_USED;
    }
    xlat_new_create_ptr->lut_entry = p;

    return xlat_new_create_ptr;
}

xlat_cache_block_t xlat_extend_block( uint32_t newSize )
{
    assert( xlat_new_create_ptr->use_list == NULL );
    while( xlat_new_create_ptr->size < newSize ) {
        if( xlat_new_cache_ptr->size == 0 ) {
            /* Migrate to the front of the cache to keep it contiguous */
            xlat_new_create_ptr->active = 0;
            sh4ptr_t olddata = xlat_new_create_ptr->code;
            int oldsize = xlat_new_create_ptr->size;
            int size = oldsize + MIN_BLOCK_SIZE; /* minimum expansion */
            void **lut_entry = xlat_new_create_ptr->lut_entry;
            void *chain = xlat_new_create_ptr->chain;
            int allocation = (int)-sizeof(struct xlat_cache_block);
            xlat_new_cache_ptr = xlat_new_cache;
            do {
                if( xlat_new_cache_ptr->active ) {
                    xlat_promote_to_temp_space( xlat_new_cache_ptr );
                }
                allocation += xlat_new_cache_ptr->size + sizeof(struct xlat_cache_block);
                xlat_new_cache_ptr = NEXT(xlat_new_cache_ptr);
            } while( allocation < size );
            xlat_new_create_ptr = xlat_new_cache;
            xlat_new_create_ptr->active = 1;
            xlat_new_create_ptr->size = allocation;
            xlat_new_create_ptr->lut_entry = lut_entry;
            xlat_new_create_ptr->chain = chain;
            xlat_new_create_ptr->use_list = NULL;
            *lut_entry = &xlat_new_create_ptr->code;
            memmove( xlat_new_create_ptr->code, olddata, oldsize );
        } else {
            if( xlat_new_cache_ptr->active ) {
                xlat_promote_to_temp_space( xlat_new_cache_ptr );
            }
            xlat_new_create_ptr->size += xlat_new_cache_ptr->size + sizeof(struct xlat_cache_block);
            xlat_new_cache_ptr = NEXT(xlat_new_cache_ptr);
        }
    }
    return xlat_new_create_ptr;

}

void xlat_commit_block( uint32_t destsize, sh4addr_t startpc, sh4addr_t endpc )
{
    void **entry = xlat_get_lut_entry(startpc+2);
    /* assume main entry has already been set at this point */

    for( sh4addr_t pc = startpc+2; pc < endpc; pc += 2 ) {
        if( XLAT_LUT_ENTRY(pc) == 0 )
            entry = xlat_get_lut_entry(pc);
        *((uintptr_t *)entry) |= (uintptr_t)XLAT_LUT_ENTRY_USED;
        entry++;
    }

    xlat_new_cache_ptr = xlat_cut_block( xlat_new_create_ptr, destsize );
}

void xlat_check_cache_integrity( xlat_cache_block_t cache, xlat_cache_block_t ptr, int size )
{
    int foundptr = 0;
    xlat_cache_block_t tail = 
        (xlat_cache_block_t)(((char *)cache) + size - sizeof(struct xlat_cache_block));

    assert( tail->active == 1 );
    assert( tail->size == 0 ); 
    while( cache < tail ) {
        assert( cache->active >= 0 && cache->active <= 2 );
        assert( cache->size >= 0 && cache->size < size );
        if( cache == ptr ) {
            foundptr = 1;
        }
        cache = NEXT(cache);
    }
    assert( cache == tail );
    assert( foundptr == 1 || tail == ptr );
}

/**
 * Perform a reverse lookup to determine the SH4 address corresponding to
 * the start of the code block containing ptr. This is _slow_ - it does a
 * linear scan of the lookup table to find this.
 *
 * If the pointer cannot be found in any live block, returns -1 (as this
 * is not a legal PC)
 */
sh4addr_t xlat_get_address( unsigned char *ptr )
{
    int i,j;
    for( i=0; i<XLAT_LUT_PAGES; i++ ) {
        void **page = xlat_lut[i];
        if( page != NULL ) {
            for( j=0; j<XLAT_LUT_PAGE_ENTRIES; j++ ) {
                void *entry = page[j];
                if( ((uintptr_t)entry) > (uintptr_t)XLAT_LUT_ENTRY_USED ) {
                    xlat_cache_block_t block = XLAT_BLOCK_FOR_LUT_ENTRY(entry);
                    if( ptr >= block->code && ptr < block->code + block->size) {
                        /* Found it */
                        return (i<<13) | (j<<1);
                    }
                }
            }
        }
    }
    return -1;
}

/**
 * Sanity check that the given pointer is at least contained in one of cache
 * regions, and has a sane-ish size. We don't do a full region walk atm.
 */
gboolean xlat_is_code_pointer( void *p )
{
    char *region;
    uintptr_t region_size;

    xlat_cache_block_t block = XLAT_BLOCK_FOR_CODE(p);
    if( (((char *)block) - (char *)xlat_new_cache) < XLAT_NEW_CACHE_SIZE ) {
         /* Pointer is in new cache */
        region = (char *)xlat_new_cache;
        region_size = XLAT_NEW_CACHE_SIZE;
    }
#ifdef XLAT_GENERATIONAL_CACHE
    else if( (((char *)block) - (char *)xlat_temp_cache) < XLAT_TEMP_CACHE_SIZE ) {
         /* Pointer is in temp cache */
        region = (char *)xlat_temp_cache;
        region_size = XLAT_TEMP_CACHE_SIZE;
    } else if( (((char *)block) - (char *)xlat_odl_cache) < XLAT_OLD_CACHE_SIZE ) {
        /* Pointer is in old cache */
        region = (char *)xlat_old_cache;
        region_size = XLAT_OLD_CACHE_SIZE;
    }
#endif
    else {
        /* Not a valid cache pointer */
        return FALSE;
    }

    /* Make sure the whole block is in the region */
    if( (((char *)p) - region) >= region_size ||
        (((char *)(NEXT(block))) - region) >= region_size )
        return FALSE;
    return TRUE;
}

void xlat_check_integrity( )
{
    xlat_check_cache_integrity( xlat_new_cache, xlat_new_cache_ptr, XLAT_NEW_CACHE_SIZE );
#ifdef XLAT_GENERATIONAL_CACHE
    xlat_check_cache_integrity( xlat_temp_cache, xlat_temp_cache_ptr, XLAT_TEMP_CACHE_SIZE );
    xlat_check_cache_integrity( xlat_old_cache, xlat_old_cache_ptr, XLAT_OLD_CACHE_SIZE );
#endif
}

unsigned int xlat_get_active_block_count()
{
    unsigned int count = 0;
    xlat_cache_block_t ptr = xlat_new_cache;
    while( ptr->size != 0 ) {
        if( ptr->active != 0 ) {
            count++;
        }
        ptr = NEXT(ptr);
    }
    return count;
}

unsigned int xlat_get_active_blocks( struct xlat_block_ref *blocks, unsigned int size )
{
    unsigned int count = 0;
    xlat_cache_block_t ptr = xlat_new_cache;
    while( ptr->size != 0 ) {
        if( ptr->active != 0 ) {
            blocks[count].block = ptr;
            blocks[count].pc = 0;
            count++;
        }
        if( count >= size )
            break;
        ptr = NEXT(ptr);
    }
    return count;
}

static void xlat_get_block_pcs( struct xlat_block_ref *blocks, unsigned int size )
{
    unsigned i;
    for( i=0; i<XLAT_LUT_PAGES;i ++ ) {
        void **page = xlat_lut[i];
        if( page != NULL ) {
            for( unsigned j=0; j < XLAT_LUT_PAGE_ENTRIES; j++ ) {
                void *code = XLAT_CODE_ADDR(page[j]);
                if( code != NULL ) {
                    xlat_cache_block_t ptr = XLAT_BLOCK_FOR_CODE(code);
                    sh4addr_t pc = XLAT_ADDR_FROM_ENTRY(i,j);
                    for( unsigned k=0; k<size; k++ ) {
                        if( blocks[k].block == ptr ) {
                            blocks[k].pc = pc;
                            ptr = ptr->chain;
                            if( ptr == NULL )
                                break;
                            else {
                                ptr = XLAT_BLOCK_FOR_CODE(ptr);
                                k = 0;
                            }
                        }
                    }
                }
            }
        }
    }
}

static int xlat_compare_active_field( const void *a, const void *b )
{
    const struct xlat_block_ref *ptra = (const struct xlat_block_ref *)a;
    const struct xlat_block_ref *ptrb = (const struct xlat_block_ref *)b;
    return ptrb->block->active - ptra->block->active;
}

unsigned int xlat_get_cache_blocks_by_activity( xlat_block_ref_t outblocks, size_t topN )
{
    int i=0;
    int count = xlat_get_active_block_count();

    struct xlat_block_ref blocks[count];
    xlat_get_active_blocks(blocks, count);
    xlat_get_block_pcs(blocks,count);
    qsort(blocks, count, sizeof(struct xlat_block_ref), xlat_compare_active_field);

    if( topN > count )
        topN = count;
    memcpy(outblocks, blocks, topN*sizeof(struct xlat_block_ref));
    return topN;
}
