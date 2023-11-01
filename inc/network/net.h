#ifndef _SGL_NET_H_
#define _SGL_NET_H_

#include <network/packet.h>
#include <stddef.h>

#define NET_DONTWAIT 0x40

/*
 * opaque; contents do not need to be seen or accessed
 *
 * this is for includes sake, we only need to include network
 * stuff in the net.c file, instead of scattered around the
 * project
 */
struct net_context;

int net_generate_signature();

#ifndef _WIN32
char *net_get_ip();
#endif

char *net_init_server(struct net_context **ctx, int port);
char *net_init_client(struct net_context **ctx, char *hostname, int port);

long net_recvfrom(struct net_context *ctx, void *__restrict __buf, size_t __n, int __flags);
long net_sendto(struct net_context *ctx, const void *__buf, size_t __n, int __flags);

#endif