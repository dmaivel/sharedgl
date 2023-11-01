#include <network/packet.h>
#include <network/net.h>
#include <server/dynarr.h>

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#else
#include <winsock2.h>
#include <Ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")
#endif

/*
 * context definition; users plz dont use
 */
struct net_context {
    bool is_server;

    int socket;

    struct sockaddr_in client;
    struct sockaddr_in server;
};

#define ERR_FAILED_TO_CREATE_SOCKET 0
#define ERR_FAILED_TO_BIND 1
#define ERR_WSA_STARTUP_FAILED 2

char *error_messages[] = {
    "failed to create socket",
    "failed to bind to port",
    "wsa startup failed"
};

int net_generate_signature() 
{
    static int seed = -1;
    if (seed == -1)
        seed = time(NULL);

    seed = (214013*seed+2531011);
    return seed;
}

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

    nctx->socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (nctx->socket < 0)
        return error_messages[ERR_FAILED_TO_CREATE_SOCKET];

    nctx->server.sin_family      = AF_INET; 
    nctx->server.sin_port        = port;
    nctx->server.sin_addr.s_addr = INADDR_ANY;

#ifndef _WIN32
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100;
    setsockopt(nctx->socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

    if (bind(nctx->socket, (struct sockaddr *)&nctx->server, sizeof(nctx->server)) < 0)
        return error_messages[ERR_FAILED_TO_BIND];

    /*
     * return null because no error message is to be returned
     */
    return NULL;
}

char *net_init_client(struct net_context **ctx, char *hostname, int port)
{
    printf("host name: %s\nport: %d\n", hostname, port);
    *ctx = malloc(sizeof(struct net_context));
    struct net_context *nctx = *ctx;

    nctx->is_server = false;

#ifdef _WIN32
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != NO_ERROR)
        return error_messages[ERR_WSA_STARTUP_FAILED];
#endif

    nctx->socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (nctx->socket < 0)
        return error_messages[ERR_FAILED_TO_CREATE_SOCKET];

    nctx->server.sin_family      = AF_INET;
    nctx->server.sin_port        = port;
    nctx->server.sin_addr.s_addr = inet_addr(hostname);

    /*
     * return null because no error message is to be returned
     */
    return NULL;
}

long net_recvfrom(struct net_context *ctx, void *__restrict __buf, size_t __n, int __flags)
{
    struct sgl_packet_header *hdr = __buf;
    
    int res = recvfrom(ctx->socket, __buf, __n, __flags, (struct sockaddr *) &ctx->client, &(socklen_t){ sizeof(ctx->client) });
    if (res != -1)
        printf("recv { .client_id = %d | is_for_server = %hhu | type = %hu | size = %-6hu | index = %-6hu | expected_blocks = %-6hu | signature = %08X }, %ld)\n", 
            hdr->client_id, hdr->is_for_server, hdr->type, hdr->size, hdr->index, hdr->expected_blocks, hdr->signature, __n);
    fflush(stdout);

    return res;
}

long net_sendto(struct net_context *ctx, const void *__buf, size_t __n, int __flags)
{
    const struct sgl_packet_header *hdr = __buf;
    printf("send { .client_id = %d | is_for_server = %hhu | type = %hu | size = %-6hu | index = %-6hu | expected_blocks = %-6hu | signature = %08X }, %ld)\n", 
            hdr->client_id, hdr->is_for_server, hdr->type, hdr->size, hdr->index, hdr->expected_blocks, hdr->signature, __n);
    fflush(stdout);

    if (ctx->is_server)
        return sendto(ctx->socket, __buf, __n, __flags, (struct sockaddr *) &ctx->client, sizeof(ctx->client));
    else
        return sendto(ctx->socket, __buf, __n, __flags, (struct sockaddr *) &ctx->server, sizeof(ctx->server));
}