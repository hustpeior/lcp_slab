#include <stdlib.h>
#include <string.h>
#include "lcp_slab.h"
#include "lcp_debug.h"

#define LCP_BUFCTL_END 0xffffFFFF
#define	LCP_SLAB_LIMIT 0xffffFFFE
#define	LCP_CFLGS_OFF_SLAB	0x010000UL	/* slab management in own cache */
#define	LCP_CFLGS_OPTIMIZE	0x020000UL	/* optimized slab lookup */
#define LCP_L1_CACHE_ALIGN(x) (((x)+(L1_CACHE_BYTES-1))&~(L1_CACHE_BYTES-1))
#define cache_chain (cache_cache.next)
#define	LCP_BREAK_GFP_ORDER_HI	2
#define	LCP_BREAK_GFP_ORDER_LO	1
#define	LCP_DFLGS_GROWN         0x000001UL
#define LCP_REAP_SCANLEN	100
#define LCP_REAP_PERFECT	0

#define LCP_MALLOC_SHIFT_HIGH  ((MAX_ORDER + LCP_PAGE_SHIFT - 1) <= 25 ? \
                        (MAX_ORDER + LCP_PAGE_SHIFT - 1) : 25)
#define LCP_MALLOC_MAX_SIZE (1UL<<LCP_MALLOC_SHIFT_HIGH)
#define	LCP_MAX_GFP_ORDER	(LCP_MALLOC_SHIFT_HIGH - LCP_PAGE_SHIFT)

#if LCP_DEBUG

#define	LCP_STATS_INC_ACTIVE(x)	((x)->num_active++)
#define	LCP_STATS_DEC_ACTIVE(x)	((x)->num_active--)
#define	LCP_STATS_INC_ALLOCED(x)	((x)->num_allocations++)
#define	LCP_STATS_INC_GROWN(x)	((x)->grown++)
#define	LCP_STATS_INC_REAPED(x)	((x)->reaped++)
#define	LCP_STATS_SET_HIGH(x)	do { if ((x)->num_active > (x)->high_mark) \
	(x)->high_mark = (x)->num_active; \
} while (0)
#define	LCP_STATS_INC_ERR(x)	((x)->errors++)
#else

#define	LCP_STATS_INC_ACTIVE(x)	do { } while (0)
#define	LCP_STATS_DEC_ACTIVE(x)	do { } while (0)
#define	LCP_STATS_INC_ALLOCED(x)	do { } while (0)
#define	LCP_STATS_INC_GROWN(x)	do { } while (0)
#define	LCP_STATS_INC_REAPED(x)	do { } while (0)
#define	LCP_STATS_SET_HIGH(x)	do { } while (0)
#define	LCP_STATS_INC_ERR(x)	do { } while (0)

#endif

#define	LCP_OFF_SLAB(x)	((x)->flags & LCP_CFLGS_OFF_SLAB)
#define	LCP_OPTIMIZE(x)	((x)->flags & LCP_CFLGS_OPTIMIZE)
#define	LCP_GROWN(x)	((x)->dlags & LCP_DFLGS_GROWN)

#define	LCP_SET_PAGE_CACHE(pg,x)  ((pg)->list.next = (struct list_head *)(x))
#define	LCP_GET_PAGE_CACHE(pg)    ((lcp_mem_cache_t *)(pg)->list.next)
#define	LCP_SET_PAGE_SLAB(pg,x)   ((pg)->list.prev = (struct list_head *)(x))
#define	LCP_GET_PAGE_SLAB(pg)     ((lcp_slab_t *)(pg)->list.prev)

#define CACHE(x) { (x), NULL},
	static lcp_cache_sizes_t cache_sizes[] = {
        CACHE(32)
        CACHE(64)
        CACHE(128)
        CACHE(256)
        CACHE(512)
        CACHE(1024)
        CACHE(2048)
        CACHE(4096)
        CACHE(8192)
        CACHE(16384)
        CACHE(32768)
        CACHE(65536)
#if LCP_MALLOC_MAX_SIZE >= 131072
        CACHE(131072)
#endif
#if LCP_MALLOC_MAX_SIZE >= 262144
        CACHE(262144)
#endif
#if LCP_MALLOC_MAX_SIZE >= 524288
        CACHE(524288)
#endif
#if LCP_MALLOC_MAX_SIZE >= 1048576
        CACHE(1048576)
#endif
		{     0,	NULL},
};

/**
*全局函数
*/

extern lcp_zone_t *mem_zone;
void * lcp_mem_cache_alloc (lcp_mem_cache_t *cachep);
void lcp_mem_cache_free (lcp_mem_cache_t *cachep, void *objp);
int lcp_mem_cache_destroy (lcp_mem_cache_t * cachep);

/**
*静态变量
*/
static int slab_break_gfp_order = LCP_BREAK_GFP_ORDER_LO;
static lcp_mem_cache_t *clock_searchp = &cache_cache;
static unsigned long offslab_limit;
/**
*静态函数
*/
static inline void * __lcp_mem_cache_alloc (lcp_mem_cache_t *cachep);
static inline void lcp_mem_cache_free_one(lcp_mem_cache_t *cachep, void *objp);
static void lcp_mem_slab_destroy (lcp_mem_cache_t *cachep, lcp_slab_t *slabp);
//static inline void lcp_mem_cache_free_one(lcp_mem_cache_t *cachep, void *objp);

#define BUG()                   \
    ({                          \
        printf("(%s:%d) Bug !!!\n", __FUNCTION__, __LINE__);    \
        exit(-1);               \
     })

static inline struct lcp_page  * lcp_virt_to_page(lcp_mem_cache_t *cachep, void * addr)
{
	lcp_zone_t *zone = mem_zone;
	return zone->zone_mem_map + (((char *)addr - (char *)zone->zone_start_ptr) >> LCP_PAGE_SHIFT);
	
}


static __inline__ void * soff_to_ptr(unsigned int offset)
{
	if(offset == 0)
		return NULL;
	return (void *)((char *)mem_zone->zone_start_ptr + offset);
}

static __inline__ unsigned int ptr_to_soff(void * addr)
{
	unsigned long offset = (char *)addr - (char *)mem_zone->zone_start_ptr;
    return offset;
}


/**
*slab分配器和伙伴分配器的接口，获取空闲物理页面的函数
*/

static inline void * lcp_mem_getpages (lcp_mem_cache_t *cachep)
{
	void	*addr;
	struct  lcp_page * page ;	
	lcp_zone_t * zone = mem_zone;
	page =  lcp_alloc_pages(cachep->gfporder);
#ifdef LCP_DEBUG
	LcpPrint("in get pages: %x %d\n",page,cachep->gfporder);
#endif	
	if(page)
		addr = (void *)lcp_inc_ptr(zone->zone_start_ptr ,(page - zone->zone_mem_map) << LCP_PAGE_SHIFT);
	else
		addr = NULL;
	return addr;
}

/**
*Interface to system's page release. 
*/
static inline void lcp_mem_freepages (lcp_mem_cache_t *cachep, void *addr)
{
	lcp_zone_t * zone = mem_zone;
	struct lcp_page * page = zone->zone_mem_map + (((char *)addr - (char *)zone->zone_start_ptr) >> LCP_PAGE_SHIFT);
	lcp_dealloc_pages(page,cachep->gfporder);
}

static void lcp_mem_cache_estimate(unsigned int gfp_order, size_t size, int flags, 
                                    size_t *left_over, unsigned int *num)
{
	int i;
	size_t wastage = (1 << LCP_PAGE_SHIFT) << gfp_order;
	size_t extra = 0;
	size_t base = 0;
	
	if (!(flags & LCP_CFLGS_OFF_SLAB)) {
		base = sizeof(lcp_slab_t);
		extra = sizeof(lcp_mem_bufctl_t);
	}

	i = 0;
	while (i*size + LCP_L1_CACHE_ALIGN(base+i*extra) <= wastage)
		i++;

	if (i > 0)
		i--;
	
	if (i > LCP_SLAB_LIMIT)
		i = LCP_SLAB_LIMIT;
	
	*num = i;
	wastage -= i*size;
	wastage -= LCP_L1_CACHE_ALIGN(base+i*extra);
	*left_over = wastage;
}

void lcp_mem_cache_init(void)
{
	size_t left_over;

#ifdef LCP_DEBUG
	char * fun_name = "void lcp_mem_cache_init(void)";
	ShowTrace(fun_name);
#endif
	
	INIT_LIST_HEAD(&cache_chain);
	
	lcp_mem_cache_estimate(0, cache_cache.objsize, 0,
		&left_over, &cache_cache.num);
	
	cache_cache.colour = left_over/cache_cache.colour_off;
	cache_cache.colour_next = 0;
}

/**
*通用缓冲区的初始化函数，初始化的缓冲区主要用于malloc函数使用
*/

void lcp_mem_cache_sizes_init(void)
{

	lcp_cache_sizes_t *sizes = cache_sizes;
	char name[20];
#ifdef LCP_DEBUG
	char * fun_name = "lcp_mem_cache_sizes_init";
	ShowTrace(fun_name);
#endif

	do {
		/* For performance, all the general caches are L1 aligned.
		 * This should be particularly beneficial on SMP boxes, as it
		 * eliminates "false sharing".
		 * Note for systems short on memory removing the alignment will
		 * allow tighter packing of the smaller caches. */

		sprintf(name,"size-%Zd",sizes->cs_size);
		if (!(sizes->cs_cachep =
			lcp_mem_cache_create(name, sizes->cs_size,
					0, LCP_SLAB_HWCACHE_ALIGN))) {
			BUG();
		}
		/* Inc off-slab bufctl limit until the ceiling is hit. */
		if (!(LCP_OFF_SLAB(sizes->cs_cachep))) {
			offslab_limit = sizes->cs_size-sizeof(lcp_slab_t);
			offslab_limit /= sizeof(lcp_mem_bufctl_t);
		}
		sizes++;
	} while (sizes->cs_size);
}



lcp_mem_cache_t * lcp_mem_find_general_cachep (size_t size)
{
	lcp_cache_sizes_t *csizep = cache_sizes;

	/* This function could be moved to the header file, and
	 * made inline so consumers can quickly determine what
	 * cache pointer they require.
	 */
	for ( ; csizep->cs_size; csizep++) {
		if (size > csizep->cs_size)
			continue;
		break;
	}
	return csizep->cs_cachep;
}

lcp_mem_cache_t *
lcp_mem_cache_create (const char *name, size_t size, size_t offset,
	unsigned int flags)
{
	size_t left_over, align, slab_size;
	lcp_mem_cache_t *cachep = NULL;
    lcp_dbg(" name = %-16s, size = %d\n", name, size);

	/* Get cache's description obj. */
	cachep = (lcp_mem_cache_t *) lcp_mem_cache_alloc(&cache_cache);
	if (!cachep)
		goto opps;
	memset(cachep, 0, sizeof(lcp_mem_cache_t));

    //align size 
	if (size & (BYTES_PER_WORD-1)) {
		size += (BYTES_PER_WORD-1);
		size &= ~(BYTES_PER_WORD-1);
	}
	
	align = BYTES_PER_WORD;
	if (flags & LCP_SLAB_HWCACHE_ALIGN)
		align = L1_CACHE_BYTES;

	/* Determine if the slab management is 'on' or 'off' slab. */
	if (size >= (LCP_PAGE_SIZE>>3))
		flags |= LCP_CFLGS_OFF_SLAB;

	do {
		unsigned int break_flag = 0;
cal_wastage:
		lcp_mem_cache_estimate(cachep->gfporder, size, flags,
						&left_over, &cachep->num);
		if (break_flag)
			break;
		if (cachep->gfporder >= LCP_MAX_GFP_ORDER)
			break;
		if (!cachep->num)
			goto next;
		if (flags & LCP_CFLGS_OFF_SLAB && cachep->num > offslab_limit) {
			/* Oops, this num of objs will cause problems. */
			cachep->gfporder--;
			break_flag++;
			goto cal_wastage;
		}

		/*
		 * Large num of objs is good, but v. large slabs are currently
		 * bad for the gfp()s.
		 */
		if (cachep->gfporder >= slab_break_gfp_order)
			break;

		if ((left_over*8) <= (LCP_PAGE_SIZE<<cachep->gfporder))
			break;	/* Acceptable internal fragmentation. */
next:
		cachep->gfporder++;
	} while (1);

	if (!cachep->num) {
		lcp_mem_cache_free(&cache_cache, cachep);
		cachep = NULL;
		goto opps;
	}
	slab_size = LCP_L1_CACHE_ALIGN(cachep->num*sizeof(lcp_mem_bufctl_t)+sizeof(lcp_slab_t));

	/*
	 * If the slab has been placed off-slab, and we have enough space then
	 * move it on-slab. This is at the expense of any extra colouring.
	 */
	if (flags & LCP_CFLGS_OFF_SLAB && left_over >= slab_size) {
		flags &= ~LCP_CFLGS_OFF_SLAB;
		left_over -= slab_size;
	}

	/* Offset must be a multiple of the alignment. */
	offset += (align-1);
	offset &= ~(align-1);
	if (!offset)
		offset = L1_CACHE_BYTES;
	cachep->colour_off = offset;
	cachep->colour = left_over/offset;

	/* init remaining fields */
	if (!cachep->gfporder && !(flags & LCP_CFLGS_OFF_SLAB))
		flags |= LCP_CFLGS_OPTIMIZE;

	cachep->flags = flags;

	cachep->objsize = size;
	INIT_LIST_HEAD(&cachep->slabs);
	cachep->firstnotfull = &cachep->slabs;

//	printf(  "   %x\n",cachep->firstnotfull);

	if (flags & LCP_CFLGS_OFF_SLAB)
		cachep->slabp_cache = lcp_mem_find_general_cachep(slab_size);
	/* Copy name over so we don't have problems with unloaded modules */
	strcpy(cachep->name, name);

	/* Need the semaphore to access the chain. */
	{
		struct list_head *p;

		list_for_each(p, &cache_chain) {
			lcp_mem_cache_t *pc = list_entry(p, lcp_mem_cache_t, next);

			/* The name field is constant - no lock needed. */
			if (!strcmp(pc->name, name))
				BUG();
		}
	}

	/* There is no reason to lock our new cache before we
	 * link it in - no one knows about it yet...
	 */
	list_add(&cachep->next, &cache_chain);

opps:
	return cachep;
}

static int __lcp_mem_cache_shrink(lcp_mem_cache_t *cachep)
{
	lcp_slab_t *slabp;
	int ret;
	
	/* If the cache is growing, stop shrinking. */
	while (!cachep->growing) {
		struct list_head *p;
		
		p = cachep->slabs.prev;
		if (p == &cachep->slabs)
			break;
		
		slabp = list_entry(cachep->slabs.prev, lcp_slab_t, list);
		if (slabp->inuse)
			break;
		
		list_del(&slabp->list);
		if (cachep->firstnotfull == &slabp->list)
			cachep->firstnotfull = &cachep->slabs;
		
		lcp_mem_slab_destroy(cachep, slabp);
	}
	ret = !list_empty(&cachep->slabs);
	return ret;
}



int lcp_mem_cache_destroy (lcp_mem_cache_t * cachep)
{
	if (!cachep || cachep->growing)
		BUG();
	
	/* Find the cache in the chain of caches. */
	/* the chain is never empty, cache_cache is never destroyed */
	if (clock_searchp == cachep)
		clock_searchp = list_entry(cachep->next.next,
		lcp_mem_cache_t, next);
	list_del(&cachep->next);
	
	if (__lcp_mem_cache_shrink(cachep)) {
		list_add(&cachep->next,&cache_chain);
		return 1;
	}
	lcp_mem_cache_free(&cache_cache, cachep);
	return 0;
}



static inline void lcp_mem_cache_init_objs (lcp_mem_cache_t * cachep,
			lcp_slab_t * slabp)
{
	int i;

	for (i = 0; i < cachep->num; i++) {
		void* objp = slabp->s_mem+cachep->objsize*i;
		/*
		 * Constructors are not allowed to allocate memory from
		 * the same cache which they are a constructor for.
		 * Otherwise, deadlock. They must also be threaded.
		 */
		lcp_slab_bufctl(slabp)[i] = i+1;
	}
	lcp_slab_bufctl(slabp)[i-1] = LCP_BUFCTL_END;
	slabp->free = 0;
}


/* Get the memory for a slab management obj. */
static inline lcp_slab_t * lcp_mem_cache_slabmgmt (lcp_mem_cache_t *cachep,
			void *objp, int colour_off)
{
	lcp_slab_t *slabp;
	
	if (LCP_OFF_SLAB(cachep)) {
		/* Slab management obj is off-slab. */
		slabp = lcp_mem_cache_alloc(cachep->slabp_cache);
		if (!slabp)
			return NULL;
	} else {
		/* FIXME: change to
			slabp = objp
		 * if you enable OPTIMIZE
		 */
		slabp = objp+colour_off;
		colour_off += LCP_L1_CACHE_ALIGN(cachep->num *
				sizeof(lcp_mem_bufctl_t) + sizeof(lcp_slab_t));
	}
	slabp->inuse = 0;
	slabp->colouroff = colour_off;
	slabp->s_mem = objp+colour_off;
	
	return slabp;
}


//not implement kmem_cache_alloc,kmem_cache_free,kmem_find_general_cachep

static int lcp_mem_cache_grow (lcp_mem_cache_t * cachep)
{
	lcp_slab_t	*slabp;
	struct lcp_page	*page;
	void		*objp;
	size_t		 offset;
	unsigned int i;
	unsigned int save_flags;

	/* About to mess with non-constant members - lock. */
    //spin_lock_irqsave(&cachep->spinlock, save_flags);

	/* Get colour for the slab, and cal the next value. */
	offset = cachep->colour_next;
	cachep->colour_next++;
	if (cachep->colour_next >= cachep->colour)
		cachep->colour_next = 0;
	offset *= cachep->colour_off;
	cachep->dflags |= LCP_DFLGS_GROWN;
	cachep->growing++;

//	spin_unlock_irqrestore(&cachep->spinlock, save_flags);
	/* Get mem for the objs. */
	if (!(objp = lcp_mem_getpages(cachep)))
		goto failed;

	/* Get slab management. */
	if (!(slabp = lcp_mem_cache_slabmgmt(cachep, objp, offset)))
		goto opps1;

	/* Nasty!!!!!! I hope this is OK. */

	i = 1 << cachep->gfporder;
	page = lcp_virt_to_page(cachep,objp);
	do {
		LCP_SET_PAGE_CACHE(page, cachep);
		LCP_SET_PAGE_SLAB(page, slabp);
		page++;
	} while (--i);

	lcp_mem_cache_init_objs(cachep, slabp);
	cachep->growing--;

	/* Make slab active. */
	list_add_tail(&slabp->list,&cachep->slabs);
	if (cachep->firstnotfull == &cachep->slabs)
		cachep->firstnotfull = &slabp->list;
	LCP_STATS_INC_GROWN(cachep);
	cachep->failures = 0;


	return 1;
opps1:
	lcp_mem_freepages(cachep, objp);
failed:
	cachep->growing--;
//	spin_unlock_irqrestore(&cachep->spinlock, save_flags);
	return 0;
}


void * lcp_mem_cache_alloc (lcp_mem_cache_t *cachep)
{
	return __lcp_mem_cache_alloc(cachep);
}


static inline void * lcp_mem_cache_alloc_one_tail(lcp_mem_cache_t *cachep,
												lcp_slab_t *slabp)
{
	void *objp;
	//LcpPrint("	enter lcp_mem_cache_alloc_one_tail\n");	
	if (!cachep) {
		return NULL;
	}
	LCP_STATS_INC_ALLOCED(cachep);
	LCP_STATS_INC_ACTIVE(cachep);
	LCP_STATS_SET_HIGH(cachep);
	/* get obj pointer */
	if (!slabp)
		return NULL;
	slabp->inuse++;
	objp = slabp->s_mem + slabp->free*cachep->objsize;
#ifdef LCP_DEBUG
	LcpPrint("	objp = slabp->s_mem + slabp->free*cachep->objsize objp = 0x%x;\n",objp);
	LcpPrint("	slabp = %x slabp->s_mem = 0x%x\n",slabp,slabp->s_mem);
	LcpPrint("	slabp->inuse = %d slabp->list = 0x%x\n",slabp->inuse,slabp->list);
	LcpPrint("	slabp->colouroff  = %d slabp->free = %d\n",slabp->colouroff ,slabp->free);
#endif
	slabp->free=lcp_slab_bufctl(slabp)[slabp->free];

	if (slabp->free == LCP_BUFCTL_END) {
		/* slab now full: move to next slab for next alloc */
		cachep->firstnotfull = slabp->list.next;
	}
	return objp;
}

#define lcp_mem_cache_alloc_one(cachep)				\
	({								\
	lcp_slab_t	*slabp;					\
	\
	/* Get slab alloc is to come from. */			\
{							\
	struct list_head* p = cachep->firstnotfull;	\
	if (p == &cachep->slabs)			\
{                                       \
	goto alloc_new_slab;			\
}                                       \
	slabp = list_entry(p,lcp_slab_t, list);	\
}							\
	lcp_mem_cache_alloc_one_tail(cachep, slabp);		\
})



static inline void * __lcp_mem_cache_alloc (lcp_mem_cache_t *cachep)
{
	void* objp;
try_again:
	objp = lcp_mem_cache_alloc_one(cachep);
	//LcpPrint("	objp = lcp_mem_cache_alloc_one(cachep);\n");
	return objp;
alloc_new_slab:
	if (lcp_mem_cache_grow(cachep)) {
//在这里要通知任何一个进程来转储自己的内容，即发出一个信号，然后等待若干时间，
//在进行try_again!
	//LcpPrint("	in alloc_new_slab:	if (lcp_mem_cache_grow(cachep, flags))\n");		
		goto try_again;
	}
	return NULL;
}


static inline void lcp_mem_cache_free_one(lcp_mem_cache_t *cachep, void *objp)
{
	lcp_slab_t* slabp;

	slabp = LCP_GET_PAGE_SLAB(lcp_virt_to_page(cachep,objp));
	{
		unsigned int objnr = (objp-slabp->s_mem)/cachep->objsize;

		lcp_slab_bufctl(slabp)[objnr] = slabp->free;
		slabp->free = objnr;
	}
	LCP_STATS_DEC_ACTIVE(cachep);
	
	/* fixup slab chain */
	if (slabp->inuse-- == cachep->num)
		goto moveslab_partial;
	if (!slabp->inuse)
		goto moveslab_free;
	return;

moveslab_partial:
    	/* was full.
	 * Even if the page is now empty, we can set c_firstnotfull to
	 * slabp: there are no partial slabs in this case
	 */
	{
		struct list_head *t = cachep->firstnotfull;

		cachep->firstnotfull = &slabp->list;
		if (slabp->list.next == t)
			return;
		list_del(&slabp->list);
		list_add_tail(&slabp->list, t);
		return;
	}
moveslab_free:
	/**
	 * was partial, now empty.
	 * c_firstnotfull might point to slabp
	 * FIXME: optimize
	 */
	{
		struct list_head *t = cachep->firstnotfull->prev;

		list_del(&slabp->list);
		list_add_tail(&slabp->list, &cachep->slabs);
		if (cachep->firstnotfull == &slabp->list)
			cachep->firstnotfull = t->next;
		return;
	}
}

static inline void __kmem_cache_free (lcp_mem_cache_t *cachep, void* objp)
{
	lcp_mem_cache_free_one(cachep, objp);
}



void lcp_mem_cache_free (lcp_mem_cache_t *cachep, void *objp)
{
	__kmem_cache_free(cachep, objp);
}

void lcp_mem_cache_reap()
{
	lcp_slab_t *slabp;
	lcp_mem_cache_t *searchp;
	lcp_mem_cache_t *best_cachep;
	unsigned int best_pages;
	unsigned int best_len;
	unsigned int scan;

	scan = LCP_REAP_SCANLEN;
	best_len = 0;
	best_pages = 0;
	best_cachep = NULL;
	searchp = clock_searchp;
	do {
		unsigned int pages;
		struct list_head* p;
		unsigned int full_free;

		/* It's safe to test this without holding the cache-lock. */
		if (searchp->flags & LCP_SLAB_NO_REAP)
			goto next;
		if (searchp->growing)
			goto next_unlock;
		if (searchp->dflags & LCP_DFLGS_GROWN) {
			searchp->dflags &= ~LCP_DFLGS_GROWN;
			goto next_unlock;
		}
		full_free = 0;
		p = searchp->slabs.prev;
		while (p != &searchp->slabs) {
			slabp = list_entry(p, lcp_slab_t, list);
			if (slabp->inuse)
				break;
			full_free++;
			p = p->prev;
		}

		/*
		 * Try to avoid slabs with constructors and/or
		 * more than one page per slab (as it can be difficult
		 * to get high orders from gfp()).
		 */
		pages = full_free * (1<<searchp->gfporder);
		if (searchp->gfporder)
			pages = (pages*4+1)/5;
		if (pages > best_pages) {
			best_cachep = searchp;
			best_len = full_free;
			best_pages = pages;
			if (full_free >= LCP_REAP_PERFECT) {
//				clock_searchp = list_entry(searchp->next.next,
//							lcp_mem_cache_t,next);
				goto perfect;
			}
		}
next_unlock:
next:
		searchp = list_entry(searchp->next.next,lcp_mem_cache_t,next);
	} while (--scan && searchp != clock_searchp);

	clock_searchp = searchp;

	if (!best_cachep)
		/* couldn't find anything to reap */
		goto out;

perfect:
	/* free only 80% of the free slabs */
//	best_len = (best_len*4 + 1)/5;
	for (scan = 0; scan < best_len; scan++) {
		struct list_head *p;
		if (best_cachep->growing)
			break;
		p = best_cachep->slabs.prev;
		if (p == &best_cachep->slabs)
			break;
		slabp = list_entry(p,lcp_slab_t,list);
		if (slabp->inuse)
			break;
		list_del(&slabp->list);
		if (best_cachep->firstnotfull == &slabp->list)
			best_cachep->firstnotfull = &best_cachep->slabs;
		LCP_STATS_INC_REAPED(best_cachep);

		/* Safe to drop the lock. The slab is no longer linked to the
		 * cache.
		 */
		lcp_mem_slab_destroy(best_cachep, slabp);
	}
out:
	return;
}




static void lcp_mem_slab_destroy (lcp_mem_cache_t *cachep, lcp_slab_t *slabp)
{
	lcp_mem_freepages(cachep, slabp->s_mem-slabp->colouroff);
	if (LCP_OFF_SLAB(cachep))
		lcp_mem_cache_free(cachep->slabp_cache, slabp);
}

unsigned long lcp_malloc_off (size_t size)
{
	lcp_cache_sizes_t *csizep = cache_sizes;
	void * objp;
	lcp_zone_t * zone;
	zone = mem_zone;
	for (; csizep->cs_size; csizep++) {
		if (size > csizep->cs_size)
			continue;
		objp =  __lcp_mem_cache_alloc(csizep->cs_cachep);
		if(objp != NULL)
			return (char*)objp - (char *)zone->zone_start_ptr;
	}
	return 0;
}

unsigned long lcp_malloc_off_order(int order)
{
	lcp_cache_sizes_t *csizep = cache_sizes;
	void * objp;
	lcp_zone_t * zone;
    zone = mem_zone;

	objp =  __lcp_mem_cache_alloc(csizep[order].cs_cachep);
	if(objp != NULL)
		return (char*)objp - (char *)zone->zone_start_ptr;
}

void lcp_free_off (unsigned int offset)
{
	lcp_mem_cache_t *c;
	void * objp;
	struct lcp_page * page;
	
	lcp_zone_t * zone = mem_zone;

	objp = (void *) lcp_inc_ptr(zone->zone_start_ptr, offset); 	
	page = zone->zone_mem_map + (((char *)objp - (char *)zone->zone_start_ptr) >> LCP_PAGE_SHIFT);

	c = LCP_GET_PAGE_CACHE(page);
	__kmem_cache_free(c, (void*)objp);
}

size_t lcp_mem_align(size_t size)
{
	int count = 0;
	lcp_cache_sizes_t *csizep = cache_sizes;
	for (; csizep->cs_size; csizep++,count++) {
		if (size > csizep->cs_size)
			continue;
		return count;
	}
	return -1;
}



void *lcp_malloc(unsigned int size) {
    unsigned int tmp;
    void * ptr;
    tmp = lcp_malloc_off(size);
    ptr = soff_to_ptr(tmp);
    return ptr;
}

void lcp_free(void *ptr)
{
    unsigned int tmp;
    if (!ptr) 
        return ;
    tmp = ptr_to_soff(ptr);
    lcp_free_off(tmp);
}
