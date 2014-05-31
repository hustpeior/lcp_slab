all:	
	gcc -g -o lcp_slab memtest.c buddy.c lcp_slab.c
clean:
	rm -rf lcp_slab
