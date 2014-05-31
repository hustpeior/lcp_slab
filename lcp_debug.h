#ifndef _LCP_DEBUG_H_
#define _LCP_DEBUG_H_

extern int lcp_debug;

#define lcp_dbg(fmt, args...)                                   \
    do {                                                        \
        if (lcp_debug)                                          \
        printf("(%s:%d) "fmt, __FUNCTION__, __LINE__, ##args);  \
    }while(0)

#define lcp_info(fmt, args...)                                  \
    do {                                                        \
        if (lcp_debug > 1)                                      \
        printf(fmt, ##args);  \
    }while(0)

#define lcp_err(fmt, args...)                                   \
    do {                                                        \
        printf("(%s:%d) "fmt, __FUNCTION__, __LINE__, ##args);  \
    }while(0)

#endif
