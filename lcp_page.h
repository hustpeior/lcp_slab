/*
 * not consider the mutiprocesses condition, 
 * that's impossible in this project                                    
 */
#ifndef _LCP_PAGE_H_
#define _LCP_PAGE_H_

#include "list.h"
#include "lcp.h"
#include "bitops.h"
#include <stdio.h>
#define  MAX_ORDER				10
#define  L1_CACHE_BYTES        16 

#define LCP_PAGE_SHIFT  12
#define LCP_PAGE_SIZE (1 << LCP_PAGE_SHIFT)
#define LCP_ENTRY_TBSIZE       (128*4)

#define lock_t  int

typedef struct lcp_free_area {
    struct list_head	free_list;
    unsigned int *map;
}lcp_free_area_t;

typedef struct lcp_page {		
    struct list_head list;		
    unsigned int vaddr;
}lcp_mem_map_t;

typedef struct lcp_zone {	
    lock_t          lock;
    unsigned int    free_pages;				//free pages in the zone
    lcp_free_area_t		free_area[MAX_ORDER];
    struct lcp_page		*zone_mem_map;
    unsigned int        *zone_bitmap;
    void *zone_start_ptr;
    unsigned int zone_start_mapnr;       //the index of first page
    unsigned int size;
}lcp_zone_t;

/*
 * @brief - increment pointer
 * @return - return the new pointer to the new positon
 */
static inline void * lcp_inc_ptr(void *start, unsigned int offset)
{
	return (void *)((char *)((char *)start + offset));
}

/**
*内存管理初始化和析构函数,伙伴管理机制的建立
*/

/*
 *	unlike the system call, it return the page's offset , instead of the virtual 
 *  address, because many processes will share the memory area, virtuall address
 *  is useless. 
 *  the first parameter, where tell the allocator get the page from which zone
 */	
struct lcp_page * lcp_alloc_pages(int order);
unsigned int lcp_get_free_pages(int order);

void lcp_dealloc_pages(struct lcp_page * page, int order);
void lcp_free_pages(int offset, int order);
#endif
