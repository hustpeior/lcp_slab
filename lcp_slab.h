#ifndef LCPSLAB_H
#define LCPSLAB_H
#include "lcp.h"
#include "lcp_page.h"
#include "list.h"	

#define  L1_CACHE_BYTES       16

#define LCP_CACHE_NAMELEN       30
#define	LCP_SLAB_NO_REAP		0x00001000UL	/* never reap from the cache */
#define	LCP_SLAB_HWCACHE_ALIGN	0x00002000UL	/* align objs on a h/w cache lines */
#define	BYTES_PER_WORD          sizeof(unsigned int)

typedef unsigned int lcp_mem_bufctl_t;
	
typedef struct lcp_slab_s {
    struct list_head	list;
    unsigned int colouroff;
    void				*s_mem;		/* including colour offset */
    unsigned int		inuse;		/* num of objs active in slab */
    lcp_mem_bufctl_t free;
}lcp_slab_t;

#define lcp_slab_bufctl(slabp) \
    ((lcp_mem_bufctl_t *)(((lcp_slab_t*)slabp)+1))
	
typedef struct lcp_mem_cache_s lcp_mem_cache_t;
		
struct lcp_mem_cache_s {
    /* 1) each alloc & free */
    /* full, partial first, then free */
    struct list_head	slabs;
    struct list_head	*firstnotfull;
    unsigned int		objsize;
    unsigned int	 	flags;	/* constant flags */
    unsigned int		num;	/* # of objs per slab */
    lock_t				lock;

    /* 2) slab additions /removals */
    /* order of pgs per slab (2^n) */
    unsigned int		gfporder;

    size_t				colour;		/* cache colouring range */
    unsigned int		colour_off;	/* colour offset */
    unsigned int		colour_next;	/* cache colouring */
    lcp_mem_cache_t		*slabp_cache;
    unsigned int		growing;
    unsigned int		dflags;		/* dynamic flags */

    unsigned long		failures;

    /* 3) cache creation/removal */
    char		    	name[LCP_CACHE_NAMELEN];
    struct list_head	next;

#ifdef LCP_DEBUG
    unsigned long		num_active;
    unsigned long		num_allocations;
    unsigned long		high_mark;
    unsigned long		grown;
    unsigned long		reaped;
    unsigned long 		errors;
#endif
} ;

typedef struct lcp_cache_sizes {
    size_t		 cs_size;
    lcp_mem_cache_t	*cs_cachep;
} lcp_cache_sizes_t;
	

static lcp_mem_cache_t cache_cache = {
slabs:			LIST_HEAD_INIT(cache_cache.slabs),
                firstnotfull:	&cache_cache.slabs,
                objsize:		sizeof(lcp_mem_cache_t),
                flags:			LCP_SLAB_NO_REAP,
                colour_off:		L1_CACHE_BYTES,
                name:			"lcp_mem_cache",
};
unsigned long lcp_malloc_off (size_t size);
void lcp_free_off (unsigned int offset);


void lcp_mem_cache_sizes_init(void);
void lcp_mem_cache_init(void);
lcp_mem_cache_t *
lcp_mem_cache_create(const char *name, size_t size, size_t offset, unsigned int flags);

void * lcp_mem_cache_alloc (lcp_mem_cache_t *cachep);
void lcp_mem_cache_free (lcp_mem_cache_t *cachep, void *objp);
size_t lcp_mem_align(size_t size);
unsigned long lcp_malloc_off_order(int order);
void lcp_mem_cache_reap (void);
#endif
