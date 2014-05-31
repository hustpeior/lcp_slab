lcp_slab
========

malloc implementaton based on slab 

基于简单的伙伴分配算法以及slab算法实现的一种简单的 malloc
malloc 管理的内存是从系统的堆中分配的，按照指定的页大小进行
管理，主要是为了理解一下基本的 伙伴分配器以及 slab工作原理
