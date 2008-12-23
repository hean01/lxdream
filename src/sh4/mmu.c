/**
 * $Id$
 *
 * MMU implementation
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
#define MODULE sh4_module

#include <stdio.h>
#include <assert.h>
#include "sh4/sh4mmio.h"
#include "sh4/sh4core.h"
#include "sh4/sh4trans.h"
#include "mem.h"
#include "mmu.h"

#ifdef HAVE_FRAME_ADDRESS
#define RETURN_VIA(exc) do{ *(((void **)__builtin_frame_address(0))+1) = exc; return; } while(0)
#else
#define RETURN_VIA(exc) return MMU_VMA_ERROR
#endif

/* The MMU (practically unique in the system) is allowed to raise exceptions
 * directly, with a return code indicating that one was raised and the caller
 * had better behave appropriately.
 */
#define RAISE_TLB_ERROR(code, vpn) \
    MMIO_WRITE(MMU, TEA, vpn); \
    MMIO_WRITE(MMU, PTEH, ((MMIO_READ(MMU, PTEH) & 0x000003FF) | (vpn&0xFFFFFC00))); \
    sh4_raise_tlb_exception(code);

#define RAISE_MEM_ERROR(code, vpn) \
    MMIO_WRITE(MMU, TEA, vpn); \
    MMIO_WRITE(MMU, PTEH, ((MMIO_READ(MMU, PTEH) & 0x000003FF) | (vpn&0xFFFFFC00))); \
    sh4_raise_exception(code);

#define RAISE_OTHER_ERROR(code) \
    sh4_raise_exception(code);
/**
 * Abort with a non-MMU address error. Caused by user-mode code attempting
 * to access privileged regions, or alignment faults.
 */
#define MMU_READ_ADDR_ERROR() RAISE_OTHER_ERROR(EXC_DATA_ADDR_READ)
#define MMU_WRITE_ADDR_ERROR() RAISE_OTHER_ERROR(EXC_DATA_ADDR_WRITE)

#define MMU_TLB_READ_MISS_ERROR(vpn) RAISE_TLB_ERROR(EXC_TLB_MISS_READ, vpn)
#define MMU_TLB_WRITE_MISS_ERROR(vpn) RAISE_TLB_ERROR(EXC_TLB_MISS_WRITE, vpn)
#define MMU_TLB_INITIAL_WRITE_ERROR(vpn) RAISE_MEM_ERROR(EXC_INIT_PAGE_WRITE, vpn)
#define MMU_TLB_READ_PROT_ERROR(vpn) RAISE_MEM_ERROR(EXC_TLB_PROT_READ, vpn)
#define MMU_TLB_WRITE_PROT_ERROR(vpn) RAISE_MEM_ERROR(EXC_TLB_PROT_WRITE, vpn)
#define MMU_TLB_MULTI_HIT_ERROR(vpn) sh4_raise_reset(EXC_TLB_MULTI_HIT); \
    MMIO_WRITE(MMU, TEA, vpn); \
    MMIO_WRITE(MMU, PTEH, ((MMIO_READ(MMU, PTEH) & 0x000003FF) | (vpn&0xFFFFFC00)));


#define OCRAM_START (0x1C000000>>LXDREAM_PAGE_BITS)
#define OCRAM_END   (0x20000000>>LXDREAM_PAGE_BITS)


static struct itlb_entry mmu_itlb[ITLB_ENTRY_COUNT];
static struct utlb_entry mmu_utlb[UTLB_ENTRY_COUNT];
static uint32_t mmu_urc;
static uint32_t mmu_urb;
static uint32_t mmu_lrui;
static uint32_t mmu_asid; // current asid

static struct utlb_sort_entry mmu_utlb_sorted[UTLB_ENTRY_COUNT];
static uint32_t mmu_utlb_entries; // Number of entries in mmu_utlb_sorted. 

static sh4ptr_t cache = NULL;

static void mmu_invalidate_tlb();
static void mmu_utlb_sorted_reset();
static void mmu_utlb_sorted_reload();

static uint32_t get_mask_for_flags( uint32_t flags )
{
    switch( flags & TLB_SIZE_MASK ) {
    case TLB_SIZE_1K: return MASK_1K;
    case TLB_SIZE_4K: return MASK_4K;
    case TLB_SIZE_64K: return MASK_64K;
    case TLB_SIZE_1M: return MASK_1M;
    default: return 0; /* Unreachable */
    }
}

MMIO_REGION_READ_FN( MMU, reg )
{
    reg &= 0xFFF;
    switch( reg ) {
    case MMUCR:
        return MMIO_READ( MMU, MMUCR) | (mmu_urc<<10) | (mmu_urb<<18) | (mmu_lrui<<26);
    default:
        return MMIO_READ( MMU, reg );
    }
}

MMIO_REGION_WRITE_FN( MMU, reg, val )
{
    uint32_t tmp;
    reg &= 0xFFF;
    switch(reg) {
    case SH4VER:
        return;
    case PTEH:
        val &= 0xFFFFFCFF;
        if( (val & 0xFF) != mmu_asid ) {
            mmu_asid = val&0xFF;
            sh4_icache.page_vma = -1; // invalidate icache as asid has changed
        }
        break;
    case PTEL:
        val &= 0x1FFFFDFF;
        break;
    case PTEA:
        val &= 0x0000000F;
        break;
    case TRA:
    	val &= 0x000003FC;
    	break;
    case EXPEVT:
    case INTEVT:
    	val &= 0x00000FFF;
    	break;
    case MMUCR:
        if( val & MMUCR_TI ) {
            mmu_invalidate_tlb();
        }
        mmu_urc = (val >> 10) & 0x3F;
        mmu_urb = (val >> 18) & 0x3F;
        mmu_lrui = (val >> 26) & 0x3F;
        val &= 0x00000301;
        tmp = MMIO_READ( MMU, MMUCR );
        if( (val ^ tmp) & (MMUCR_AT|MMUCR_SV) ) {
            // AT flag has changed state - flush the xlt cache as all bets
            // are off now. We also need to force an immediate exit from the
            // current block
            MMIO_WRITE( MMU, MMUCR, val );
            sh4_flush_icache();
        }
        break;
    case CCR:
        CCN_set_cache_control( val );
        val &= 0x81A7;
        break;
    case MMUUNK1:
    	/* Note that if the high bit is set, this appears to reset the machine.
    	 * Not emulating this behaviour yet until we know why...
    	 */
    	val &= 0x00010007;
    	break;
    case QACR0:
    case QACR1:
    	val &= 0x0000001C;
    	break;
    case PMCR1:
        PMM_write_control(0, val);
        val &= 0x0000C13F;
        break;
    case PMCR2:
        PMM_write_control(1, val);
        val &= 0x0000C13F;
        break;
    default:
        break;
    }
    MMIO_WRITE( MMU, reg, val );
}


void MMU_init()
{
}

void MMU_reset()
{
    mmio_region_MMU_write( CCR, 0 );
    mmio_region_MMU_write( MMUCR, 0 );
    mmu_utlb_sorted_reload();
}

void MMU_save_state( FILE *f )
{
    fwrite( &mmu_itlb, sizeof(mmu_itlb), 1, f );
    fwrite( &mmu_utlb, sizeof(mmu_utlb), 1, f );
    fwrite( &mmu_urc, sizeof(mmu_urc), 1, f );
    fwrite( &mmu_urb, sizeof(mmu_urb), 1, f );
    fwrite( &mmu_lrui, sizeof(mmu_lrui), 1, f );
    fwrite( &mmu_asid, sizeof(mmu_asid), 1, f );
}

int MMU_load_state( FILE *f )
{
    if( fread( &mmu_itlb, sizeof(mmu_itlb), 1, f ) != 1 ) {
        return 1;
    }
    if( fread( &mmu_utlb, sizeof(mmu_utlb), 1, f ) != 1 ) {
        return 1;
    }
    if( fread( &mmu_urc, sizeof(mmu_urc), 1, f ) != 1 ) {
        return 1;
    }
    if( fread( &mmu_urc, sizeof(mmu_urb), 1, f ) != 1 ) {
        return 1;
    }
    if( fread( &mmu_lrui, sizeof(mmu_lrui), 1, f ) != 1 ) {
        return 1;
    }
    if( fread( &mmu_asid, sizeof(mmu_asid), 1, f ) != 1 ) {
        return 1;
    }
    mmu_utlb_sorted_reload();
    return 0;
}


/******************* Sorted TLB data structure ****************/
/*
 * mmu_utlb_sorted maintains a list of all active (valid) entries,
 * sorted by masked VPN and then ASID. Multi-hit entries are resolved 
 * ahead of time, and have -1 recorded as the corresponding PPN.
 * 
 * FIXME: Multi-hit detection doesn't pick up cases where two pages 
 * overlap due to different sizes (and don't share the same base
 * address). 
 */ 
static void mmu_utlb_sorted_reset() 
{
    mmu_utlb_entries = 0;
}

/**
 * Find an entry in the sorted table (VPN+ASID check). 
 */
static inline int mmu_utlb_sorted_find( sh4addr_t vma )
{
    int low = 0;
    int high = mmu_utlb_entries;
    uint32_t lookup = (vma & 0xFFFFFC00) + mmu_asid;

    mmu_urc++;
    if( mmu_urc == mmu_urb || mmu_urc == 0x40 ) {
        mmu_urc = 0;
    }

    while( low != high ) {
        int posn = (high+low)>>1;
        int masked = lookup & mmu_utlb_sorted[posn].mask;
        if( mmu_utlb_sorted[posn].key < masked ) {
            low = posn+1;
        } else if( mmu_utlb_sorted[posn].key > masked ) {
            high = posn;
        } else {
            return mmu_utlb_sorted[posn].entryNo;
        }
    }
    return -1;

}

static void mmu_utlb_insert_entry( int entry )
{
    int low = 0;
    int high = mmu_utlb_entries;
    uint32_t key = (mmu_utlb[entry].vpn & mmu_utlb[entry].mask) + mmu_utlb[entry].asid;

    assert( mmu_utlb_entries < UTLB_ENTRY_COUNT );
    /* Find the insertion point */
    while( low != high ) {
        int posn = (high+low)>>1;
        if( mmu_utlb_sorted[posn].key < key ) {
            low = posn+1;
        } else if( mmu_utlb_sorted[posn].key > key ) {
            high = posn;
        } else {
            /* Exact match - multi-hit */
            mmu_utlb_sorted[posn].entryNo = -2;
            return;
        }
    } /* 0 2 4 6 */
    memmove( &mmu_utlb_sorted[low+1], &mmu_utlb_sorted[low], 
             (mmu_utlb_entries - low) * sizeof(struct utlb_sort_entry) );
    mmu_utlb_sorted[low].key = key;
    mmu_utlb_sorted[low].mask = mmu_utlb[entry].mask | 0x000000FF;
    mmu_utlb_sorted[low].entryNo = entry;
    mmu_utlb_entries++;
}

static void mmu_utlb_remove_entry( int entry )
{
    int low = 0;
    int high = mmu_utlb_entries;
    uint32_t key = (mmu_utlb[entry].vpn & mmu_utlb[entry].mask) + mmu_utlb[entry].asid;
    while( low != high ) {
        int posn = (high+low)>>1;
        if( mmu_utlb_sorted[posn].key < key ) {
            low = posn+1;
        } else if( mmu_utlb_sorted[posn].key > key ) {
            high = posn;
        } else {
            if( mmu_utlb_sorted[posn].entryNo == -2 ) {
                /* Multiple-entry recorded - rebuild the whole table minus entry */
                int i;
                mmu_utlb_entries = 0;
                for( i=0; i< UTLB_ENTRY_COUNT; i++ ) {
                    if( i != entry && (mmu_utlb[i].flags & TLB_VALID)  ) {
                        mmu_utlb_insert_entry(i);
                    }
                }
            } else {
                mmu_utlb_entries--;
                memmove( &mmu_utlb_sorted[posn], &mmu_utlb_sorted[posn+1],
                         (mmu_utlb_entries - posn)*sizeof(struct utlb_sort_entry) );
            }
            return;
        }
    }
    assert( 0 && "UTLB key not found!" );
}

static void mmu_utlb_sorted_reload()
{
    int i;
    mmu_utlb_entries = 0;
    for( i=0; i<UTLB_ENTRY_COUNT; i++ ) {
        if( mmu_utlb[i].flags & TLB_VALID ) 
            mmu_utlb_insert_entry( i );
    }
}

/* TLB maintanence */

/**
 * LDTLB instruction implementation. Copies PTEH, PTEL and PTEA into the UTLB
 * entry identified by MMUCR.URC. Does not modify MMUCR or the ITLB.
 */
void MMU_ldtlb()
{
    if( mmu_utlb[mmu_urc].flags & TLB_VALID )
        mmu_utlb_remove_entry( mmu_urc );
    mmu_utlb[mmu_urc].vpn = MMIO_READ(MMU, PTEH) & 0xFFFFFC00;
    mmu_utlb[mmu_urc].asid = MMIO_READ(MMU, PTEH) & 0x000000FF;
    mmu_utlb[mmu_urc].ppn = MMIO_READ(MMU, PTEL) & 0x1FFFFC00;
    mmu_utlb[mmu_urc].flags = MMIO_READ(MMU, PTEL) & 0x00001FF;
    mmu_utlb[mmu_urc].pcmcia = MMIO_READ(MMU, PTEA);
    mmu_utlb[mmu_urc].mask = get_mask_for_flags(mmu_utlb[mmu_urc].flags);
    if( mmu_utlb[mmu_urc].ppn >= 0x1C000000 )
        mmu_utlb[mmu_urc].ppn |= 0xE0000000;
    if( mmu_utlb[mmu_urc].flags & TLB_VALID )
        mmu_utlb_insert_entry( mmu_urc );
}

static void mmu_invalidate_tlb()
{
    int i;
    for( i=0; i<ITLB_ENTRY_COUNT; i++ ) {
        mmu_itlb[i].flags &= (~TLB_VALID);
    }
    for( i=0; i<UTLB_ENTRY_COUNT; i++ ) {
        mmu_utlb[i].flags &= (~TLB_VALID);
    }
    mmu_utlb_entries = 0;
}

#define ITLB_ENTRY(addr) ((addr>>7)&0x03)

int32_t FASTCALL mmu_itlb_addr_read( sh4addr_t addr )
{
    struct itlb_entry *ent = &mmu_itlb[ITLB_ENTRY(addr)];
    return ent->vpn | ent->asid | (ent->flags & TLB_VALID);
}
int32_t FASTCALL mmu_itlb_data_read( sh4addr_t addr )
{
    struct itlb_entry *ent = &mmu_itlb[ITLB_ENTRY(addr)];
    return (ent->ppn & 0x1FFFFC00) | ent->flags;
}

void FASTCALL mmu_itlb_addr_write( sh4addr_t addr, uint32_t val )
{
    struct itlb_entry *ent = &mmu_itlb[ITLB_ENTRY(addr)];
    ent->vpn = val & 0xFFFFFC00;
    ent->asid = val & 0x000000FF;
    ent->flags = (ent->flags & ~(TLB_VALID)) | (val&TLB_VALID);
}

void FASTCALL mmu_itlb_data_write( sh4addr_t addr, uint32_t val )
{
    struct itlb_entry *ent = &mmu_itlb[ITLB_ENTRY(addr)];
    ent->ppn = val & 0x1FFFFC00;
    ent->flags = val & 0x00001DA;
    ent->mask = get_mask_for_flags(val);
    if( ent->ppn >= 0x1C000000 )
        ent->ppn |= 0xE0000000;
}

#define UTLB_ENTRY(addr) ((addr>>8)&0x3F)
#define UTLB_ASSOC(addr) (addr&0x80)
#define UTLB_DATA2(addr) (addr&0x00800000)

int32_t FASTCALL mmu_utlb_addr_read( sh4addr_t addr )
{
    struct utlb_entry *ent = &mmu_utlb[UTLB_ENTRY(addr)];
    return ent->vpn | ent->asid | (ent->flags & TLB_VALID) |
    ((ent->flags & TLB_DIRTY)<<7);
}
int32_t FASTCALL mmu_utlb_data_read( sh4addr_t addr )
{
    struct utlb_entry *ent = &mmu_utlb[UTLB_ENTRY(addr)];
    if( UTLB_DATA2(addr) ) {
        return ent->pcmcia;
    } else {
        return (ent->ppn&0x1FFFFC00) | ent->flags;
    }
}

/**
 * Find a UTLB entry for the associative TLB write - same as the normal
 * lookup but ignores the valid bit.
 */
static inline int mmu_utlb_lookup_assoc( uint32_t vpn, uint32_t asid )
{
    int result = -1;
    unsigned int i;
    for( i = 0; i < UTLB_ENTRY_COUNT; i++ ) {
        if( (mmu_utlb[i].flags & TLB_VALID) &&
                ((mmu_utlb[i].flags & TLB_SHARE) || asid == mmu_utlb[i].asid) &&
                ((mmu_utlb[i].vpn ^ vpn) & mmu_utlb[i].mask) == 0 ) {
            if( result != -1 ) {
                fprintf( stderr, "TLB Multi hit: %d %d\n", result, i );
                return -2;
            }
            result = i;
        }
    }
    return result;
}

/**
 * Find a ITLB entry for the associative TLB write - same as the normal
 * lookup but ignores the valid bit.
 */
static inline int mmu_itlb_lookup_assoc( uint32_t vpn, uint32_t asid )
{
    int result = -1;
    unsigned int i;
    for( i = 0; i < ITLB_ENTRY_COUNT; i++ ) {
        if( (mmu_itlb[i].flags & TLB_VALID) &&
                ((mmu_itlb[i].flags & TLB_SHARE) || asid == mmu_itlb[i].asid) &&
                ((mmu_itlb[i].vpn ^ vpn) & mmu_itlb[i].mask) == 0 ) {
            if( result != -1 ) {
                return -2;
            }
            result = i;
        }
    }
    return result;
}

void FASTCALL mmu_utlb_addr_write( sh4addr_t addr, uint32_t val )
{
    if( UTLB_ASSOC(addr) ) {
        int utlb = mmu_utlb_lookup_assoc( val, mmu_asid );
        if( utlb >= 0 ) {
            struct utlb_entry *ent = &mmu_utlb[utlb];
            uint32_t old_flags = ent->flags;
            ent->flags = ent->flags & ~(TLB_DIRTY|TLB_VALID);
            ent->flags |= (val & TLB_VALID);
            ent->flags |= ((val & 0x200)>>7);
            if( (old_flags & TLB_VALID) && !(ent->flags&TLB_VALID) ) {
                mmu_utlb_remove_entry( utlb );
            } else if( !(old_flags & TLB_VALID) && (ent->flags&TLB_VALID) ) {
                mmu_utlb_insert_entry( utlb );
            }
        }

        int itlb = mmu_itlb_lookup_assoc( val, mmu_asid );
        if( itlb >= 0 ) {
            struct itlb_entry *ent = &mmu_itlb[itlb];
            ent->flags = (ent->flags & (~TLB_VALID)) | (val & TLB_VALID);
        }

        if( itlb == -2 || utlb == -2 ) {
            MMU_TLB_MULTI_HIT_ERROR(addr);
            return;
        }
    } else {
        struct utlb_entry *ent = &mmu_utlb[UTLB_ENTRY(addr)];
        if( ent->flags & TLB_VALID ) 
            mmu_utlb_remove_entry( UTLB_ENTRY(addr) );
        ent->vpn = (val & 0xFFFFFC00);
        ent->asid = (val & 0xFF);
        ent->flags = (ent->flags & ~(TLB_DIRTY|TLB_VALID));
        ent->flags |= (val & TLB_VALID);
        ent->flags |= ((val & 0x200)>>7);
        if( ent->flags & TLB_VALID ) 
            mmu_utlb_insert_entry( UTLB_ENTRY(addr) );
    }
}

void FASTCALL mmu_utlb_data_write( sh4addr_t addr, uint32_t val )
{
    struct utlb_entry *ent = &mmu_utlb[UTLB_ENTRY(addr)];
    if( UTLB_DATA2(addr) ) {
        ent->pcmcia = val & 0x0000000F;
    } else {
        if( ent->flags & TLB_VALID ) 
            mmu_utlb_remove_entry( UTLB_ENTRY(addr) );
        ent->ppn = (val & 0x1FFFFC00);
        ent->flags = (val & 0x000001FF);
        ent->mask = get_mask_for_flags(val);
        if( mmu_utlb[mmu_urc].ppn >= 0x1C000000 )
            mmu_utlb[mmu_urc].ppn |= 0xE0000000;
        if( ent->flags & TLB_VALID ) 
            mmu_utlb_insert_entry( UTLB_ENTRY(addr) );
    }
}

/* Cache access - not implemented */

int32_t FASTCALL mmu_icache_addr_read( sh4addr_t addr )
{
    return 0; // not implemented
}
int32_t FASTCALL mmu_icache_data_read( sh4addr_t addr )
{
    return 0; // not implemented
}
int32_t FASTCALL mmu_ocache_addr_read( sh4addr_t addr )
{
    return 0; // not implemented
}
int32_t FASTCALL mmu_ocache_data_read( sh4addr_t addr )
{
    return 0; // not implemented
}

void FASTCALL mmu_icache_addr_write( sh4addr_t addr, uint32_t val )
{
}

void FASTCALL mmu_icache_data_write( sh4addr_t addr, uint32_t val )
{
}

void FASTCALL mmu_ocache_addr_write( sh4addr_t addr, uint32_t val )
{
}

void FASTCALL mmu_ocache_data_write( sh4addr_t addr, uint32_t val )
{
}

/******************************************************************************/
/*                        MMU TLB address translation                         */
/******************************************************************************/

/**
 * The translations are excessively complicated, but unfortunately it's a
 * complicated system. TODO: make this not be painfully slow.
 */

/**
 * Perform the actual utlb lookup w/ asid matching.
 * Possible utcomes are:
 *   0..63 Single match - good, return entry found
 *   -1 No match - raise a tlb data miss exception
 *   -2 Multiple matches - raise a multi-hit exception (reset)
 * @param vpn virtual address to resolve
 * @return the resultant UTLB entry, or an error.
 */
static inline int mmu_utlb_lookup_vpn_asid( uint32_t vpn )
{
    int result = -1;
    unsigned int i;

    mmu_urc++;
    if( mmu_urc == mmu_urb || mmu_urc == 0x40 ) {
        mmu_urc = 0;
    }

    for( i = 0; i < UTLB_ENTRY_COUNT; i++ ) {
        if( (mmu_utlb[i].flags & TLB_VALID) &&
                ((mmu_utlb[i].flags & TLB_SHARE) || mmu_asid == mmu_utlb[i].asid) &&
                ((mmu_utlb[i].vpn ^ vpn) & mmu_utlb[i].mask) == 0 ) {
            if( result != -1 ) {
                return -2;
            }
            result = i;
        }
    }
    return result;
}

/**
 * Perform the actual utlb lookup matching on vpn only
 * Possible utcomes are:
 *   0..63 Single match - good, return entry found
 *   -1 No match - raise a tlb data miss exception
 *   -2 Multiple matches - raise a multi-hit exception (reset)
 * @param vpn virtual address to resolve
 * @return the resultant UTLB entry, or an error.
 */
static inline int mmu_utlb_lookup_vpn( uint32_t vpn )
{
    int result = -1;
    unsigned int i;

    mmu_urc++;
    if( mmu_urc == mmu_urb || mmu_urc == 0x40 ) {
        mmu_urc = 0;
    }

    for( i = 0; i < UTLB_ENTRY_COUNT; i++ ) {
        if( (mmu_utlb[i].flags & TLB_VALID) &&
                ((mmu_utlb[i].vpn ^ vpn) & mmu_utlb[i].mask) == 0 ) {
            if( result != -1 ) {
                return -2;
            }
            result = i;
        }
    }

    return result;
}

/**
 * Update the ITLB by replacing the LRU entry with the specified UTLB entry.
 * @return the number (0-3) of the replaced entry.
 */
static int inline mmu_itlb_update_from_utlb( int entryNo )
{
    int replace;
    /* Determine entry to replace based on lrui */
    if( (mmu_lrui & 0x38) == 0x38 ) {
        replace = 0;
        mmu_lrui = mmu_lrui & 0x07;
    } else if( (mmu_lrui & 0x26) == 0x06 ) {
        replace = 1;
        mmu_lrui = (mmu_lrui & 0x19) | 0x20;
    } else if( (mmu_lrui & 0x15) == 0x01 ) {
        replace = 2;
        mmu_lrui = (mmu_lrui & 0x3E) | 0x14;
    } else { // Note - gets invalid entries too
        replace = 3;
        mmu_lrui = (mmu_lrui | 0x0B);
    }

    mmu_itlb[replace].vpn = mmu_utlb[entryNo].vpn;
    mmu_itlb[replace].mask = mmu_utlb[entryNo].mask;
    mmu_itlb[replace].ppn = mmu_utlb[entryNo].ppn;
    mmu_itlb[replace].asid = mmu_utlb[entryNo].asid;
    mmu_itlb[replace].flags = mmu_utlb[entryNo].flags & 0x01DA;
    return replace;
}

/**
 * Perform the actual itlb lookup w/ asid protection
 * Possible utcomes are:
 *   0..63 Single match - good, return entry found
 *   -1 No match - raise a tlb data miss exception
 *   -2 Multiple matches - raise a multi-hit exception (reset)
 * @param vpn virtual address to resolve
 * @return the resultant ITLB entry, or an error.
 */
static inline int mmu_itlb_lookup_vpn_asid( uint32_t vpn )
{
    int result = -1;
    unsigned int i;

    for( i = 0; i < ITLB_ENTRY_COUNT; i++ ) {
        if( (mmu_itlb[i].flags & TLB_VALID) &&
                ((mmu_itlb[i].flags & TLB_SHARE) || mmu_asid == mmu_itlb[i].asid) &&
                ((mmu_itlb[i].vpn ^ vpn) & mmu_itlb[i].mask) == 0 ) {
            if( result != -1 ) {
                return -2;
            }
            result = i;
        }
    }

    if( result == -1 ) {
        int utlbEntry = mmu_utlb_sorted_find( vpn );
        if( utlbEntry < 0 ) {
            return utlbEntry;
        } else {
            return mmu_itlb_update_from_utlb( utlbEntry );
        }
    }

    switch( result ) {
    case 0: mmu_lrui = (mmu_lrui & 0x07); break;
    case 1: mmu_lrui = (mmu_lrui & 0x19) | 0x20; break;
    case 2: mmu_lrui = (mmu_lrui & 0x3E) | 0x14; break;
    case 3: mmu_lrui = (mmu_lrui | 0x0B); break;
    }

    return result;
}

/**
 * Perform the actual itlb lookup on vpn only
 * Possible utcomes are:
 *   0..63 Single match - good, return entry found
 *   -1 No match - raise a tlb data miss exception
 *   -2 Multiple matches - raise a multi-hit exception (reset)
 * @param vpn virtual address to resolve
 * @return the resultant ITLB entry, or an error.
 */
static inline int mmu_itlb_lookup_vpn( uint32_t vpn )
{
    int result = -1;
    unsigned int i;

    for( i = 0; i < ITLB_ENTRY_COUNT; i++ ) {
        if( (mmu_itlb[i].flags & TLB_VALID) &&
                ((mmu_itlb[i].vpn ^ vpn) & mmu_itlb[i].mask) == 0 ) {
            if( result != -1 ) {
                return -2;
            }
            result = i;
        }
    }

    if( result == -1 ) {
        int utlbEntry = mmu_utlb_lookup_vpn( vpn );
        if( utlbEntry < 0 ) {
            return utlbEntry;
        } else {
            return mmu_itlb_update_from_utlb( utlbEntry );
        }
    }

    switch( result ) {
    case 0: mmu_lrui = (mmu_lrui & 0x07); break;
    case 1: mmu_lrui = (mmu_lrui & 0x19) | 0x20; break;
    case 2: mmu_lrui = (mmu_lrui & 0x3E) | 0x14; break;
    case 3: mmu_lrui = (mmu_lrui | 0x0B); break;
    }

    return result;
}

#ifdef HAVE_FRAME_ADDRESS
sh4addr_t FASTCALL mmu_vma_to_phys_read( sh4vma_t addr, void *exc )
#else
sh4addr_t FASTCALL mmu_vma_to_phys_read( sh4vma_t addr )
#endif
{
    uint32_t mmucr = MMIO_READ(MMU,MMUCR);
    if( addr & 0x80000000 ) {
        if( IS_SH4_PRIVMODE() ) {
            if( addr >= 0xE0000000 ) {
                return addr; /* P4 - passthrough */
            } else if( addr < 0xC0000000 ) {
                /* P1, P2 regions are pass-through (no translation) */
                return VMA_TO_EXT_ADDR(addr);
            }
        } else {
            if( addr >= 0xE0000000 && addr < 0xE4000000 &&
                    ((mmucr&MMUCR_SQMD) == 0) ) {
                /* Conditional user-mode access to the store-queue (no translation) */
                return addr;
            }
            MMU_READ_ADDR_ERROR();
            RETURN_VIA(exc);
        }
    }

    if( (mmucr & MMUCR_AT) == 0 ) {
        return VMA_TO_EXT_ADDR(addr);
    }

    /* If we get this far, translation is required */
    int entryNo;
    if( ((mmucr & MMUCR_SV) == 0) || !IS_SH4_PRIVMODE() ) {
        entryNo = mmu_utlb_sorted_find( addr );
    } else {
        entryNo = mmu_utlb_lookup_vpn( addr );
    }

    switch(entryNo) {
    case -1:
    MMU_TLB_READ_MISS_ERROR(addr);
    RETURN_VIA(exc);
    case -2:
    MMU_TLB_MULTI_HIT_ERROR(addr);
    RETURN_VIA(exc);
    default:
        if( (mmu_utlb[entryNo].flags & TLB_USERMODE) == 0 &&
                !IS_SH4_PRIVMODE() ) {
            /* protection violation */
            MMU_TLB_READ_PROT_ERROR(addr);
            RETURN_VIA(exc);
        }

        /* finally generate the target address */
        return (mmu_utlb[entryNo].ppn & mmu_utlb[entryNo].mask) |
        	(addr & (~mmu_utlb[entryNo].mask));
    }
}

#ifdef HAVE_FRAME_ADDRESS
sh4addr_t FASTCALL mmu_vma_to_phys_write( sh4vma_t addr, void *exc )
#else
sh4addr_t FASTCALL mmu_vma_to_phys_write( sh4vma_t addr )
#endif
{
    uint32_t mmucr = MMIO_READ(MMU,MMUCR);
    if( addr & 0x80000000 ) {
        if( IS_SH4_PRIVMODE() ) {
            if( addr >= 0xE0000000 ) {
                return addr; /* P4 - passthrough */
            } else if( addr < 0xC0000000 ) {
                /* P1, P2 regions are pass-through (no translation) */
                return VMA_TO_EXT_ADDR(addr);
            }
        } else {
            if( addr >= 0xE0000000 && addr < 0xE4000000 &&
                    ((mmucr&MMUCR_SQMD) == 0) ) {
                /* Conditional user-mode access to the store-queue (no translation) */
                return addr;
            }
            MMU_WRITE_ADDR_ERROR();
            RETURN_VIA(exc);
        }
    }

    if( (mmucr & MMUCR_AT) == 0 ) {
        return VMA_TO_EXT_ADDR(addr);
    }

    /* If we get this far, translation is required */
    int entryNo;
    if( ((mmucr & MMUCR_SV) == 0) || !IS_SH4_PRIVMODE() ) {
        entryNo = mmu_utlb_sorted_find( addr );
    } else {
        entryNo = mmu_utlb_lookup_vpn( addr );
    }

    switch(entryNo) {
    case -1:
    MMU_TLB_WRITE_MISS_ERROR(addr);
    RETURN_VIA(exc);
    case -2:
    MMU_TLB_MULTI_HIT_ERROR(addr);
    RETURN_VIA(exc);
    default:
        if( IS_SH4_PRIVMODE() ? ((mmu_utlb[entryNo].flags & TLB_WRITABLE) == 0)
                : ((mmu_utlb[entryNo].flags & TLB_USERWRITABLE) != TLB_USERWRITABLE) ) {
            /* protection violation */
            MMU_TLB_WRITE_PROT_ERROR(addr);
            RETURN_VIA(exc);
        }

        if( (mmu_utlb[entryNo].flags & TLB_DIRTY) == 0 ) {
            MMU_TLB_INITIAL_WRITE_ERROR(addr);
            RETURN_VIA(exc);
        }

        /* finally generate the target address */
        sh4addr_t pma = (mmu_utlb[entryNo].ppn & mmu_utlb[entryNo].mask) |
        	(addr & (~mmu_utlb[entryNo].mask));
        return pma;
    }
}

/**
 * Update the icache for an untranslated address
 */
static inline void mmu_update_icache_phys( sh4addr_t addr )
{
    if( (addr & 0x1C000000) == 0x0C000000 ) {
        /* Main ram */
        sh4_icache.page_vma = addr & 0xFF000000;
        sh4_icache.page_ppa = 0x0C000000;
        sh4_icache.mask = 0xFF000000;
        sh4_icache.page = sh4_main_ram;
    } else if( (addr & 0x1FE00000) == 0 ) {
        /* BIOS ROM */
        sh4_icache.page_vma = addr & 0xFFE00000;
        sh4_icache.page_ppa = 0;
        sh4_icache.mask = 0xFFE00000;
        sh4_icache.page = mem_get_region(0);
    } else {
        /* not supported */
        sh4_icache.page_vma = -1;
    }
}

/**
 * Update the sh4_icache structure to describe the page(s) containing the
 * given vma. If the address does not reference a RAM/ROM region, the icache
 * will be invalidated instead.
 * If AT is on, this method will raise TLB exceptions normally
 * (hence this method should only be used immediately prior to execution of
 * code), and otherwise will set the icache according to the matching TLB entry.
 * If AT is off, this method will set the entire referenced RAM/ROM region in
 * the icache.
 * @return TRUE if the update completed (successfully or otherwise), FALSE
 * if an exception was raised.
 */
gboolean FASTCALL mmu_update_icache( sh4vma_t addr )
{
    int entryNo;
    if( IS_SH4_PRIVMODE()  ) {
        if( addr & 0x80000000 ) {
            if( addr < 0xC0000000 ) {
                /* P1, P2 and P4 regions are pass-through (no translation) */
                mmu_update_icache_phys(addr);
                return TRUE;
            } else if( addr >= 0xE0000000 && addr < 0xFFFFFF00 ) {
                MMU_READ_ADDR_ERROR();
                return FALSE;
            }
        }

        uint32_t mmucr = MMIO_READ(MMU,MMUCR);
        if( (mmucr & MMUCR_AT) == 0 ) {
            mmu_update_icache_phys(addr);
            return TRUE;
        }

        if( (mmucr & MMUCR_SV) == 0 )
        	entryNo = mmu_itlb_lookup_vpn_asid( addr );
        else
        	entryNo = mmu_itlb_lookup_vpn( addr );
    } else {
        if( addr & 0x80000000 ) {
            MMU_READ_ADDR_ERROR();
            return FALSE;
        }

        uint32_t mmucr = MMIO_READ(MMU,MMUCR);
        if( (mmucr & MMUCR_AT) == 0 ) {
            mmu_update_icache_phys(addr);
            return TRUE;
        }

        entryNo = mmu_itlb_lookup_vpn_asid( addr );

        if( entryNo != -1 && (mmu_itlb[entryNo].flags & TLB_USERMODE) == 0 ) {
            MMU_TLB_READ_PROT_ERROR(addr);
            return FALSE;
        }
    }

    switch(entryNo) {
    case -1:
    MMU_TLB_READ_MISS_ERROR(addr);
    return FALSE;
    case -2:
    MMU_TLB_MULTI_HIT_ERROR(addr);
    return FALSE;
    default:
        sh4_icache.page_ppa = mmu_itlb[entryNo].ppn & mmu_itlb[entryNo].mask;
        sh4_icache.page = mem_get_region( sh4_icache.page_ppa );
        if( sh4_icache.page == NULL ) {
            sh4_icache.page_vma = -1;
        } else {
            sh4_icache.page_vma = mmu_itlb[entryNo].vpn & mmu_itlb[entryNo].mask;
            sh4_icache.mask = mmu_itlb[entryNo].mask;
        }
        return TRUE;
    }
}

/**
 * Translate address for disassembly purposes (ie performs an instruction
 * lookup) - does not raise exceptions or modify any state, and ignores
 * protection bits. Returns the translated address, or MMU_VMA_ERROR
 * on translation failure.
 */
sh4addr_t FASTCALL mmu_vma_to_phys_disasm( sh4vma_t vma )
{
    if( vma & 0x80000000 ) {
        if( vma < 0xC0000000 ) {
            /* P1, P2 and P4 regions are pass-through (no translation) */
            return VMA_TO_EXT_ADDR(vma);
        } else if( vma >= 0xE0000000 && vma < 0xFFFFFF00 ) {
            /* Not translatable */
            return MMU_VMA_ERROR;
        }
    }

    uint32_t mmucr = MMIO_READ(MMU,MMUCR);
    if( (mmucr & MMUCR_AT) == 0 ) {
        return VMA_TO_EXT_ADDR(vma);
    }

    int entryNo = mmu_itlb_lookup_vpn( vma );
    if( entryNo == -2 ) {
        entryNo = mmu_itlb_lookup_vpn_asid( vma );
    }
    if( entryNo < 0 ) {
        return MMU_VMA_ERROR;
    } else {
        return (mmu_itlb[entryNo].ppn & mmu_itlb[entryNo].mask) |
        (vma & (~mmu_itlb[entryNo].mask));
    }
}

void FASTCALL sh4_flush_store_queue( sh4addr_t addr )
{
    int queue = (addr&0x20)>>2;
    uint32_t hi = MMIO_READ( MMU, QACR0 + (queue>>1)) << 24;
    sh4ptr_t src = (sh4ptr_t)&sh4r.store_queue[queue];
    sh4addr_t target = (addr&0x03FFFFE0) | hi;
    ext_address_space[target>>12]->write_burst( target, src );
//    mem_copy_to_sh4( target, src, 32 );
} 

gboolean FASTCALL sh4_flush_store_queue_mmu( sh4addr_t addr )
{
    uint32_t mmucr = MMIO_READ(MMU,MMUCR);
    int queue = (addr&0x20)>>2;
    sh4ptr_t src = (sh4ptr_t)&sh4r.store_queue[queue];
    sh4addr_t target;
    /* Store queue operation */

    int entryNo;
    if( ((mmucr & MMUCR_SV) == 0) || !IS_SH4_PRIVMODE() ) {
    	entryNo = mmu_utlb_lookup_vpn_asid( addr );
    } else {
    	entryNo = mmu_utlb_lookup_vpn( addr );
    }
    switch(entryNo) {
    case -1:
    MMU_TLB_WRITE_MISS_ERROR(addr);
    return FALSE;
    case -2:
    MMU_TLB_MULTI_HIT_ERROR(addr);
    return FALSE;
    default:
    	if( IS_SH4_PRIVMODE() ? ((mmu_utlb[entryNo].flags & TLB_WRITABLE) == 0)
    			: ((mmu_utlb[entryNo].flags & TLB_USERWRITABLE) != TLB_USERWRITABLE) ) {
    		/* protection violation */
    		MMU_TLB_WRITE_PROT_ERROR(addr);
    		return FALSE;
    	}

    	if( (mmu_utlb[entryNo].flags & TLB_DIRTY) == 0 ) {
    		MMU_TLB_INITIAL_WRITE_ERROR(addr);
    		return FALSE;
    	}

    	/* finally generate the target address */
    	target = ((mmu_utlb[entryNo].ppn & mmu_utlb[entryNo].mask) |
    			(addr & (~mmu_utlb[entryNo].mask))) & 0xFFFFFFE0;
    }

    ext_address_space[target>>12]->write_burst( target, src );
    // mem_copy_to_sh4( target, src, 32 );
    return TRUE;
}

