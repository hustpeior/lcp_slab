#ifndef _LCP_H_
#define _LCP_H_

void lcp_free(void *ptr);
void *lcp_malloc(unsigned int size);
void lcp_mem_init(void *base, unsigned int size);
void lcp_mem_destroy();
void show_zone_info();

#define pfree(ptr) lcp_free(ptr)
#define pmalloc(size) lcp_malloc(size)
#endif
