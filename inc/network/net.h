#ifndef _SGL_NET_H_
#define _SGL_NET_H_

#include <network/packet.h>
#include <stddef.h>

#define NET_DONTWAIT 0x40

/*
 * opaque; so we don't include network headers everywhere
 */
struct net_context;

#define NET_SOCKET_SERVER -1
#define NET_SOCKET_NONE -2
#define NET_SOCKET_FIRST_FD 2
typedef int net_socket;

enum net_poll_reason {
    NET_POLL_FAILED                 = 0,
    NET_POLL_INCOMING_CONNECTION    = (1 << 0),
    NET_POLL_INCOMING_UDP           = (1 << 1),
    NET_POLL_INCOMING_TCP           = (1 << 2)
};

#ifndef _WIN32
char *net_get_ip();
#endif

char *net_init_server(struct net_context **ctx, int port);
char *net_init_client(struct net_context **ctx, char *hostname, int port);

/*
 * SERVER-specific
 * hide from windows, need WSAPoll support first
 */
#ifndef _WIN32
enum net_poll_reason net_poll(struct net_context *ctx);
net_socket net_accept(struct net_context *ctx);
int net_fd_count(struct net_context *ctx);
bool net_did_event_happen_here(struct net_context *ctx, int fd);
#endif

// SERVER
void net_close(struct net_context *ctx, int fd);

// CLIENT
// void net_goodbye(struct net_context *ctx);

// UDP
long net_recv_udp(struct net_context *ctx, void *__restrict __buf, size_t __n, int __flags);
long net_send_udp(struct net_context *ctx, const void *__buf, size_t __n, int __flags);
long net_recv_udp_timeout(struct net_context *ctx, void *__restrict __buf, size_t __n, int __flags, size_t timeout_ms);

// TCP
bool net_recv_tcp(struct net_context *ctx, int fd, void *__restrict __buf, size_t __n);
bool net_send_tcp(struct net_context *ctx, int fd, const void *__buf, size_t __n);
bool net_recv_tcp_timeout(struct net_context *ctx, int fd, void *__restrict __buf, size_t __n, size_t timeout_ms);

#endif