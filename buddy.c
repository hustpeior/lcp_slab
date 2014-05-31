#include "lcp_page.h"
#include "list.h"
#include "lcp_debug.h"
#include <stdlib.h>
#include <string.h>

int lcp_debug = 2;
lcp_zone_t *mem_zone;
void *cur_pos;

static void __lcp_dealloc_pages(struct lcp_page * page, unsigned int order);

void show_zone_info()
{
    lcp_zone_t *zone = mem_zone;
	int i = 0;
	int j;
	struct list_head  *pos;
	lcp_info("there are %d free pages in this zone\n",zone->free_pages);
	for (j = 0 ; j < MAX_ORDER; j++) {
		lcp_info("area %2d block size %4d : ", j,1<<j);
		i = 0;
		list_for_each(pos,&zone->free_area[j].free_list) {
			i++;
		}
		lcp_info("has %d blocks\n", i);
	}
} 


static struct lcp_page * create_pages(unsigned int size)
{	
    int i = 0;
	int page_nums = (size >> LCP_PAGE_SHIFT);
	struct lcp_page * ppages = cur_pos;

    cur_pos = lcp_inc_ptr(cur_pos, sizeof(struct lcp_page) * page_nums);
	for(i = 0; i < page_nums; i++)
	{
		INIT_LIST_HEAD(&ppages[i].list);
		ppages[i].vaddr = (i << LCP_PAGE_SHIFT); 
	}

	return ppages;
}


static unsigned int *create_bitmaps(unsigned int size)
{	
    /* mapsize = page_nums / 32 */
	unsigned int map_size = (size >> (LCP_PAGE_SHIFT + 5)) + 1;
	unsigned int total_size=0;
	int i;
	lcp_zone_t * zone = mem_zone;
	unsigned int * bitmaps;
	lcp_free_area_t tmp;

    /* caculate totoal bitmap sizes 
     * 1 page, 2 pages , 4 pages, .. MAX_ORDER pages bitmap 
     **/
	for(i = 0; i < MAX_ORDER ; i++) {
		total_size += map_size;
		map_size = (map_size >> 1) + 1;
		if (map_size == 0)
			map_size = 1;
		zone->free_area[i].map = (unsigned int*)total_size;
	}
	bitmaps = cur_pos; 
	cur_pos = lcp_inc_ptr(cur_pos, total_size * sizeof(unsigned int));
	memset(bitmaps, 0x0, total_size * sizeof(unsigned int));

	for(i = 0; i < MAX_ORDER ; i++)
	{
		zone->free_area[i].map = bitmaps + (unsigned int )zone->free_area[i].map ;
	}
	return bitmaps;
}

static void init_free_area(lcp_free_area_t * free_area)
{	
	int i;

	for (i = 0; i < MAX_ORDER; i++) {
		INIT_LIST_HEAD(&free_area[i].free_list);
	}
}


static void lcp_mem_create(void *base, unsigned int size)
{
	int i;
	lcp_zone_t * zone;
	int			first_free_page;
	void        * shm_start;
	
    cur_pos = base;
    mem_zone = malloc(sizeof(lcp_zone_t));
    zone = mem_zone;
    memset(cur_pos, 0, size);

	//first 64 bytes is reserved for entry points of the 
	cur_pos = lcp_inc_ptr(cur_pos, LCP_ENTRY_TBSIZE);

	zone->size = size >> LCP_PAGE_SHIFT; 
	zone->free_pages = 0;
	zone->zone_start_ptr = base ;
	zone->zone_start_mapnr = 0 ;
	
	zone->zone_mem_map = create_pages(size);
	zone->zone_bitmap = create_bitmaps(size);
	init_free_area(zone->free_area);
	first_free_page = (((unsigned int)cur_pos - (unsigned int)base) >> LCP_PAGE_SHIFT) + 1;

	for (i = first_free_page; i < zone->size; i++) {
		__lcp_dealloc_pages(&zone->zone_mem_map[i], 0);
	}

}

void lcp_mem_init(void *base, unsigned int size)
{
    lcp_mem_create(base, size);
	lcp_mem_cache_init();
	lcp_mem_cache_sizes_init();
}

void lcp_mem_destroy()
{
    printf("lcp_mem_destroy\n");
    //nothing to do
}

void lcp_dealloc_pages(struct lcp_page *page, int order)
{
	if(order >= MAX_ORDER)
		return;
    __lcp_dealloc_pages(page,order);
}


void lcp_free_pages(int offset, int order)
{
	lcp_zone_t *zone = mem_zone;
    lcp_dealloc_pages(zone->zone_mem_map + ((offset) >> LCP_PAGE_SHIFT), order);
}

static void __lcp_dealloc_pages(struct lcp_page * page, unsigned int order)	
{
	unsigned int index, page_idx;
    unsigned int mask, flags;
	lcp_free_area_t *area;
	struct lcp_page *base;
	lcp_zone_t      *zone = mem_zone;
	
	mask = (~0UL) << order;
	base = zone->zone_mem_map;
	page_idx = page - base;
	index = page_idx >> (1+order);
	area = zone->free_area + order;

	zone->free_pages -= mask;

	while (mask + (1<<(MAX_ORDER - 1))) {
		struct lcp_page * buddy1,*buddy2;

		if(!test_and_change_bit(index, area->map)) {
			break;
		}
		buddy1 = base + (page_idx ^ -mask);
		buddy2 = base + page_idx;
		list_del(&buddy1->list);
		mask <<= 1;
		area ++;
		index >>=1;
		page_idx &= mask;
	}

	list_add(&(base + page_idx)->list,&area->free_list);
}

unsigned int lcp_get_free_pages(int order)
{
	struct lcp_page *page;
	lcp_zone_t * zone = mem_zone;
	page = lcp_alloc_pages(order);
	if(page) 
		return page->vaddr;
	else 
        return -1;
}

static inline struct lcp_page * expand(lcp_zone_t * zone,struct lcp_page * page,
                                        unsigned int index,int low, int high,
                                        lcp_free_area_t *area)
{
	unsigned int size = 1<<high;
	while (high > low) {
		area--;
		high--;
		size>>=1;
		list_add(&(page)->list,&(area)->free_list);
		change_bit(index>>(1+high), area->map);
		index += size;
		page += size;
	}
	return page;
} 


static struct lcp_page * rmqueue(lcp_zone_t *zone, unsigned int order)
{
	lcp_free_area_t * area = zone->free_area + order;
	unsigned int curr_order = order;
	struct list_head *head, *curr;
	unsigned int flags;
	struct lcp_page *page;
	
	do {			
		head = &area->free_list;
		curr = head->next;
		if (curr != head) {
			unsigned int index;
			page = list_entry(curr,struct lcp_page,list);
			list_del(curr);

			index = page - zone->zone_mem_map;

			if (curr_order != MAX_ORDER-1)
				change_bit( index>>(1+curr_order) , area->map);
			zone->free_pages -= 1<<order;
			page = expand(zone,page,index,order,curr_order,area);
			return page;
		}
		curr_order ++;
		area++;
	} while(curr_order < MAX_ORDER);
	return NULL;
}

static struct lcp_page *__lcp_alloc_pages(int order)	
{
	lcp_zone_t      *zone = mem_zone;
	struct lcp_page * page = NULL ;
	int try_reap = 0;
	
	page = rmqueue(zone, order);
#if 1
	if (page == NULL) {
		while (page == NULL) {
			try_reap++;
			if (try_reap > 100) {
				//在这里启用转存机制
			}
			printf("no mem\n");
			lcp_mem_cache_reap();
			page = rmqueue(zone, order);
		}
	}
#endif
	return page;
}
struct lcp_page *lcp_alloc_pages(int order)
{
	if(order >= MAX_ORDER)
		return NULL;
	return __lcp_alloc_pages(order);
}	
