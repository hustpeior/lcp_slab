#include "lcp.h"
#include <stdio.h>
#include <stdlib.h>
struct test {
    int a;
    int b;
    int c;
};
#define MEM_SIZE 8*0x100000
int main(void)
{
	void *ptr1,*ptr2,*ptr3,*ptr4;
    int i = 0;
    struct test *t = NULL;
    void *mem = malloc(MEM_SIZE);
    printf("mem = %p\n", mem);
	lcp_mem_init(mem, MEM_SIZE);
	show_zone_info();

    for(i=0; i < 4; i++) {
        ptr1=lcp_malloc(4000);
        printf("ptr1 = %p\n", ptr1);
	    show_zone_info();
        lcp_free(ptr1);
    }
    ptr2 = pmalloc(45);
    printf("ptr2 = %p\n", ptr2);
	lcp_mem_destroy();
    free(mem);
}
