#include <network/net.h>
#include <server/dynarr.h>

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#else
#include <winsock2.h>
#include <Ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")
static WSADATA wsaData;

#define MSG_NOSIGNAL 0

#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#ifdef _WIN32
typedef SOCKET sockfd_t;
typedef size_t sockret_t;
#define INVALID_RETURN_VALUE SOCKET_ERROR
#else
typedef int sockfd_t;
typedef ssize_t sockret_t;
#define INVALID_RETURN_VALUE -1
#endif

/*
 * context definition
 */
struct net_context {
    bool is_server;

    sockfd_t udp_socket;
    sockfd_t tcp_socket;

    struct sockaddr_in client;
    struct sockaddr_in server;

#ifndef _WIN32
    struct pollfd fds[SOMAXCONN + 2];
    bool in_use[SOMAXCONN + 2];
    int n_fds;
#endif
};

#define ERR_FAILED_TO_CREATE_SOCKET 0
#define ERR_FAILED_TO_BIND 1
#define ERR_WSA_STARTUP_FAILED 2
#define ERR_FAILED_TO_CONNECT 3
#define ERR_FAILED_TO_LISTEN 4

#define NET_PROTOCOL_TO_SOCKET(p) (p == NET_UDP ? SOCK_DGRAM : SOCK_STREAM)

char *error_messages[] = {
    "failed to create socket",
    "failed to bind to port",
    "wsa startup failed",
    "failed to connect",
    "failed to listen"
};

#ifndef _WIN32
char *net_get_ip()
{
    char host_buffer[256];
    char *ip_buffer;
    struct hostent *host_entry;
    int hostname;

    hostname = gethostname(host_buffer, sizeof(host_buffer));
    host_entry = gethostbyname(host_buffer);
    ip_buffer = inet_ntoa(*((struct in_addr*)
                        host_entry->h_addr_list[0]));
 
    return ip_buffer;
}
#endif

static void set_nonblocking(sockfd_t socket)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(socket, FIONBIO, &mode);
#else
    int flags = fcntl(socket, F_GETFL, 0);
    fcntl(socket, F_SETFL, flags | O_NONBLOCK);
#endif
}

static bool set_no_delay(sockfd_t socket)
{
    int flag = 1;
    // set TCP_NODELAY to disable Nagle's algorithm
    int ret = setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
    return ret == 0;
}

static bool set_reuse_addr(sockfd_t socket) 
{
    int flag = 1;
    int ret = setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(int));
    return ret == 0;
}

static char *net_create_sockets(struct net_context *nctx, int port)
{
    nctx->udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    nctx->tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (nctx->udp_socket < 0 || nctx->tcp_socket < 0)
        return error_messages[ERR_FAILED_TO_CREATE_SOCKET];

    memset(&nctx->server, 0, sizeof(nctx->server));
    nctx->server.sin_family      = AF_INET; 
    nctx->server.sin_port        = htons(port);

    return NULL;
}

#ifdef _WIN32
static int64_t timer_freq, timer_start;
void time_init() {
    LARGE_INTEGER t;
    QueryPerformanceFrequency(&t);
    timer_freq = t.QuadPart;

    QueryPerformanceCounter(&t);
    timer_start = t.QuadPart;
}

int64_t time_ms()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return ((t.QuadPart-timer_start) * 1000) / timer_freq;
}
#else
void time_init() {}

int64_t time_ms() 
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec*1000 + (int64_t)ts.tv_nsec/1000000;
}
#endif

char *net_init_server(struct net_context **ctx, int port)
{
    *ctx = malloc(sizeof(struct net_context));
    struct net_context *nctx = *ctx;

    nctx->is_server = true;

    /*
     * this will only be useful if the server gets windows compatibility
     */
#ifdef _WIN32
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != NO_ERROR)
        return error_messages[ERR_WSA_STARTUP_FAILED];
#endif

    char *status = net_create_sockets(nctx, port);
    if (status != NULL)
        return NULL;

    nctx->server.sin_addr.s_addr = INADDR_ANY;

    set_nonblocking(nctx->udp_socket);
    set_nonblocking(nctx->tcp_socket);
    set_no_delay(nctx->tcp_socket);
    set_no_delay(nctx->udp_socket);
    set_reuse_addr(nctx->tcp_socket);
    set_reuse_addr(nctx->udp_socket);

    if (bind(nctx->udp_socket, (struct sockaddr *)&nctx->server, sizeof(nctx->server)) < 0)
        return error_messages[ERR_FAILED_TO_BIND];

    if (bind(nctx->tcp_socket, (struct sockaddr *)&nctx->server, sizeof(nctx->server)) < 0)
        return error_messages[ERR_FAILED_TO_BIND];

    if (listen(nctx->tcp_socket, SOMAXCONN) < 0)
        return error_messages[ERR_FAILED_TO_LISTEN];

    /*
     * will cause errors on windows build
     */
#ifndef _WIN32
    memset(nctx->fds, 0, sizeof(nctx->fds));
    nctx->fds[0].fd = nctx->tcp_socket;
    nctx->fds[0].events = POLLIN;
    nctx->fds[1].fd = nctx->udp_socket;
    nctx->fds[1].events = POLLIN;
    nctx->n_fds = 2;
#endif

    /*
     * return null because no error message is to be returned
     */
    return NULL;
}

char *net_init_client(struct net_context **ctx, char *hostname, int port)
{
    *ctx = malloc(sizeof(struct net_context));
    struct net_context *nctx = *ctx;

    nctx->is_server = false;

#ifdef _WIN32
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != NO_ERROR)
        return error_messages[ERR_WSA_STARTUP_FAILED];
#endif

    char *status = net_create_sockets(nctx, port);
    if (status != NULL)
        return NULL;

    nctx->server.sin_addr.s_addr = inet_addr(hostname);

    set_nonblocking(nctx->udp_socket);
    set_nonblocking(nctx->tcp_socket);
    set_no_delay(nctx->tcp_socket);
    set_no_delay(nctx->udp_socket);

    if (connect(nctx->tcp_socket, (struct sockaddr *) &nctx->server, sizeof(nctx->server)) < 0) {
#ifdef _WIN32
        if (WSAGetLastError() != WSAEWOULDBLOCK)
            return error_messages[ERR_FAILED_TO_CONNECT];
#else
        if (errno != EINPROGRESS)
            return error_messages[ERR_FAILED_TO_CONNECT];
#endif
    }

    /*
     * required for windows, on linux does nothing
     */
    time_init();

    /*
     * return null because no error message is to be returned
     */
    return NULL;
}

#ifndef _WIN32
/*
 * fds[0]       = tcp
 * fds[1]       = udp
 * fds[2...]    = clients
 */
enum net_poll_reason net_poll(struct net_context *ctx)
{
    enum net_poll_reason reason = NET_POLL_FAILED;
    if (poll(ctx->fds, ctx->n_fds, -1) < 0)
        return reason;

    if (ctx->fds[0].revents & POLLIN)
        reason |= NET_POLL_INCOMING_CONNECTION;
    if (ctx->fds[1].revents & POLLIN)
        reason |= NET_POLL_INCOMING_UDP;
    for (int i = 2; i < ctx->n_fds; i++)
        if (ctx->fds[i].revents & POLLIN) {
            reason |= NET_POLL_INCOMING_TCP;
            break;
        }

    return reason;
}

net_socket net_accept(struct net_context *ctx)
{
    sockfd_t socket = accept(ctx->tcp_socket, (struct sockaddr *)&ctx->client, &(socklen_t){ sizeof(ctx->client) });
    if (socket < 0)
        return NET_SOCKET_NONE;

    set_nonblocking(socket);
    set_no_delay(socket);

    ctx->fds[ctx->n_fds].fd = socket;
    ctx->fds[ctx->n_fds].events = POLLIN;
    ctx->in_use[ctx->n_fds] = true;

    return ctx->n_fds++;
}

int net_fd_count(struct net_context *ctx)
{
    return ctx->n_fds;
}

bool net_did_event_happen_here(struct net_context *ctx, int fd)
{
    return ctx->in_use[fd] && ctx->fds[fd].revents & POLLIN;
}

void net_close(struct net_context *ctx, int fd)
{
    ctx->in_use[fd] = false;
    ctx->fds[fd].revents = 0;
    ctx->fds[fd].fd = -1;
    close(ctx->fds[fd].fd);
}
#endif

// void net_goodbye(struct net_context *ctx)
// {
// #ifdef _WIN32
//     closesocket(ctx->tcp_socket);
//     closesocket(ctx->udp_socket);
// #else
//     close(ctx->tcp_socket);
//     close(ctx->udp_socket);
// #endif
// }

long net_recv_udp(struct net_context *ctx, void *__restrict __buf, size_t __n, int __flags)
{
    // struct sgl_packet_header *hdr = __buf;

    int res = recvfrom(ctx->udp_socket, __buf, __n, __flags, (struct sockaddr *) &ctx->client, &(socklen_t){ sizeof(ctx->client) });

    // if (res != -1)
    //     printf("recv { .client_id = %d | is_for_server = %hhu | type = %hu | size = %-6hu | index = %-6hu | expected_blocks = %-6hu | signature = %08X }, %ld)\n", 
    //         hdr->client_id, hdr->is_for_server, hdr->type, hdr->size, hdr->index, hdr->expected_blocks, hdr->signature, __n);
    // fflush(stdout);

    return res;
}

long net_send_udp(struct net_context *ctx, const void *__buf, size_t __n, int __flags)
{
    // const struct sgl_packet_header *hdr = __buf;
    // printf("send { .client_id = %d | is_for_server = %hhu | type = %hu | size = %-6hu | index = %-6hu | expected_blocks = %-6hu | signature = %08X }, %ld)\n", 
    //         hdr->client_id, hdr->is_for_server, hdr->type, hdr->size, hdr->index, hdr->expected_blocks, hdr->signature, __n);
    // fflush(stdout);

    if (ctx->is_server)
        return sendto(ctx->udp_socket, __buf, __n, __flags, (struct sockaddr *) &ctx->client, sizeof(ctx->client));
    else
        return sendto(ctx->udp_socket, __buf, __n, __flags, (struct sockaddr *) &ctx->server, sizeof(ctx->server));
}

long net_recv_udp_timeout(struct net_context *ctx, void *__restrict __buf, size_t __n, int __flags, size_t timeout_ms)
{
    int64_t initial = time_ms();
    long res = -1;
    while (res < 0) {
        if (time_ms() - initial > timeout_ms) {
            // fprintf(stderr, "net_recv_udp_timeout: timed out after %lds\n", timeout_ms);
            return -1;
        }

        res = recvfrom(ctx->udp_socket, __buf, __n, __flags, (struct sockaddr *) &ctx->client, &(socklen_t){ sizeof(ctx->client) });
    }

    return res;
}

// struct {
//     size_t size;
//     char name[32];
// } table[5] = {
//     { sizeof(struct sgl_packet_connect), "sgl_packet_connect" },
//     { sizeof(struct sgl_packet_swapbuffers_request), "sgl_packet_swapbuffers_request" },
//     { sizeof(struct sgl_packet_swapbuffers_result), "sgl_packet_swapbuffers_result" },
//     { sizeof(struct sgl_packet_fifo_upload), "sgl_packet_fifo_upload" },
//     { sizeof(struct sgl_packet_retval), "sgl_packet_retval" }
// };

static bool was_operation_invalid(ssize_t n)
{
#ifdef _WIN32
    return (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK);
#else
    return (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK);
#endif
}

bool net_recv_tcp(struct net_context *ctx, int fd, void *__restrict __buf, size_t __n)
{
#ifdef _WIN32
    sockfd_t socket = ctx->tcp_socket;
#else
    sockfd_t socket = fd != NET_SOCKET_SERVER ? ctx->fds[fd].fd : ctx->tcp_socket;
#endif

    // for (int i = 0; i < 5; i++)
    //     if (__n == table[i].size)
    //         printf("net_recv_tcp: %s\n", table[i].name);
    // fflush(stdout);

    size_t bytes_recv = 0;
    while (bytes_recv < __n) {
        sockret_t n = recv(socket, (char *)__buf + bytes_recv, __n - bytes_recv, MSG_NOSIGNAL);
        if (was_operation_invalid(n))
            return false;
        else if (n == INVALID_RETURN_VALUE)
            continue;

        // if (n <= 0)
        //     return false;

        bytes_recv += n;
    }

    return true;
}

bool net_send_tcp(struct net_context *ctx, int fd, const void *__buf, size_t __n)
{
#ifdef _WIN32
    sockfd_t socket = ctx->tcp_socket;
#else
    sockfd_t socket = fd != NET_SOCKET_SERVER ? ctx->fds[fd].fd : ctx->tcp_socket;
#endif

    // for (int i = 0; i < 5; i++)
    //     if (__n == table[i].size)
    //         printf("net_send_tcp: %s\n", table[i].name);
    // fflush(stdout);

    size_t bytes_sent = 0;
    while (bytes_sent < __n) {
        sockret_t n = send(socket, (const char *)__buf + bytes_sent, __n - bytes_sent, MSG_NOSIGNAL);
        if (was_operation_invalid(n)) {
            // printf("net_send_tcp: socket error, [socket = %d | errno = %d]\n", socket, errno);
            return false;
        }
        else if (n == INVALID_RETURN_VALUE)
            continue;
        
        // if (n < 0)
        //     return false;

        bytes_sent += n;
    }

    return true;
}

bool net_recv_tcp_timeout(struct net_context *ctx, int fd, void *__restrict __buf, size_t __n, size_t timeout_ms)
{
#ifdef _WIN32
    sockfd_t socket = ctx->tcp_socket;
#else
    sockfd_t socket = fd != NET_SOCKET_SERVER ? ctx->fds[fd].fd : ctx->tcp_socket;
#endif
    size_t initial = time_ms();

    // for (int i = 0; i < 5; i++)
    //     if (__n == table[i].size)
    //         printf("net_recv_tcp_timeout: %s\n", table[i].name);
    // fflush(stdout);

    size_t bytes_recv = 0;
    while (bytes_recv < __n) {
        sockret_t n = recv(socket, (char *)__buf + bytes_recv, __n - bytes_recv, 0);
        if (was_operation_invalid(n))
            return false;
        else {
            if (time_ms() - initial > timeout_ms) {
                fprintf(stderr, "net_recv_tcp_timeout: timed out after %lds\n", timeout_ms);
                return false;
            }
            if (n == INVALID_RETURN_VALUE)
                continue;
        }

        // if (n <= 0)
        //     return false;

        bytes_recv += n;
    }

    return true;
}