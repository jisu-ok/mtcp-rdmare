#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <assert.h>


#include <mtcp_api.h>
#include <mtcp_epoll.h>

#define CONCURRENCY		1
#define IP_RANGE 		1
// #define RECV_BUFLEN 	(32 * 1024)

#define DEBUG(fmt, args...)	fprintf(stderr, "[DEBUG] " fmt "\n", ## args)
#define ERROR(fmt, args...)	fprintf(stderr, fmt "\n", ## args)


struct thread_context
{
	int core;
	mctx_t mctx;
};

void SignalHandler(int signum) {
    ERROR("Received SIGINT");
    exit(-1);
}

int main(int argc, char **argv) {
    int ret;
    
    // mTCP
    struct mtcp_conf mcfg;
    struct thread_context *ctx;
    mctx_t mctx;
    struct mtcp_epoll_event *events;
    struct mtcp_epoll_event ev;
    int core = 0;

    // sockets
	struct sockaddr_in saddr;
	// struct sockaddr_in daddr;
    int sockfd, epfd, conn_sockfd;
    int backlog = 3;

    // time
    // struct timespec t1, t2;

    // send_buffer
    char *buf;

    // the amount to receive in bytes
    int recv_size;
    int recv_buf_size;


    if (argc < 5) {
        printf("Usage: ./server [IPv4] [port] [amount to recv in bytes] [mode]\n");
        return -1;
    }
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = inet_addr(argv[1]);
    saddr.sin_port = htons(atoi(argv[2]));
    recv_size = atoi(argv[3]);
    recv_buf_size = recv_size + 1024;

    assert(strncmp(argv[4], "do-nothing", strlen("do-nothing")) == 0);

    // Receive buffer allocation
    buf = malloc(recv_buf_size);
    if (!buf) {
        ERROR("malloc() for recv buf failed.");
        return -1;
    }
    memset(buf, 0x00, recv_buf_size);


    // This must be done before mtcp_init()
    mtcp_getconf(&mcfg);
    mcfg.num_cores = 1;
    mtcp_setconf(&mcfg);

    // Seed RNG
    srand(time(NULL));

    // Init mTCP
    DEBUG("Initializing mTCP...");
    if (mtcp_init("server.conf")) {
        ERROR("Failed to initialize mTCP");
        return -1;
    }

    // Default simple config, this must be done after mtcp_init()
    mtcp_getconf(&mcfg);
    mcfg.max_concurrency = 3 * CONCURRENCY;
    mcfg.max_num_buffers = 3 * CONCURRENCY;
    mtcp_setconf(&mcfg);

    // Catch ctrl+c to clean up
    mtcp_register_signal(SIGINT, SignalHandler);


    DEBUG("Creating thread context...");
    mtcp_core_affinitize(core);
    ctx = (struct thread_context *) calloc(1, sizeof(struct thread_context));
    if (!ctx) {
        ERROR("Failed to create context.");
        perror("calloc");
        return -1;
    }
    ctx->core = core;
    ctx->mctx = mtcp_create_context(core);
    if (!ctx->mctx) {
        ERROR("Failed to create mtcp context.");
        return -1;
    }
    mctx = ctx->mctx;

    // Create pool of TCP source ports for outgoing connections
    // DEBUG("Creating pool of TCP source ports...");
    // mtcp_init_rss(mctx, INADDR_ANY, IP_RANGE, daddr.sin_addr.s_addr, daddr.sin_port);

    printf("\n\n");

    // Create epoll instance
    epfd = mtcp_epoll_create(mctx, mcfg.max_num_buffers);
    events = (struct mtcp_epoll_event *) calloc(mcfg.max_num_buffers, sizeof(struct mtcp_epoll_event));
    if (!events) {
        ERROR("Failed to allocate events.");
        return -1;
    }

    // Create socket
    DEBUG("Creating socket...");
    sockfd = mtcp_socket(mctx, AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        ERROR("Failed to create socket.");
        return -1;
    }

    // Set listening socket as nonblocking mode
    ret = mtcp_setsock_nonblock(mctx, sockfd);
    if (ret < 0 ) {
        ERROR("Failed to set socket in nonblocking mode.");
        return -1;
    }

    // Register listening socket to epoll instance
    ev.events = MTCP_EPOLLIN;
    ev.data.sockid = sockfd;
    mtcp_epoll_ctl(mctx, epfd, MTCP_EPOLL_CTL_ADD, sockfd, &ev);


    // This is server program who receives data
    ret = mtcp_bind(mctx, sockfd, (struct sockaddr *) &saddr, sizeof(struct sockaddr_in));
    if (ret < 0) {
        ERROR("Failed to bind to the listening socket.");
    }

    ret = mtcp_listen(mctx, sockfd, backlog);
    if (ret < 0) {
        ERROR("Failed to listen: %s", strerror(errno));
    }
    DEBUG("Now socket (sockid=%d) is listening.", sockfd);

    int events_ready, i;
    int read_total = 0;
    int finished = 0;
    while (!finished) {
        events_ready = mtcp_epoll_wait(mctx, epfd, events, mcfg.max_num_buffers, -1);
        if (events_ready > 0) {
            for (i = 0; i < events_ready; i++) {
                if (events[i].data.sockid == sockfd) { // listening socket
                    conn_sockfd = mtcp_accept(mctx, sockfd, NULL, NULL);
                    if (conn_sockfd >= 0) {
                        DEBUG("Accepted new connection with new socket %d.", conn_sockfd);
                        ev.events = MTCP_EPOLLIN;
                        ev.data.sockid = conn_sockfd;
                        mtcp_epoll_ctl(mctx, epfd, MTCP_EPOLL_CTL_ADD, conn_sockfd, &ev);
                    }
                    else { // mtcp_accept() error handling
                        perror("mtcp_accept()");
                        exit(-1);
                    }
                }
                else { // conenction socket with client
                    if (events[i].events & MTCP_EPOLLIN) {
                        // assumption: only 1 client will connect to this server
                        ret = mtcp_read(mctx, events[i].data.sockid, buf + read_total, recv_size - read_total);
                        if (ret > 0) {
                            DEBUG("Read %d bytes", ret);
                            read_total += ret;
                        }
                        else if (ret == 0) { // maybe this is FIN-received?
                            mtcp_close(mctx, events[i].data.sockid);
                            mtcp_epoll_ctl(mctx, epfd, MTCP_EPOLL_CTL_DEL, events[i].data.sockid, &ev);
                            // the last poniter, &ev, will be just ignored; this is given to be backward-compatible. See epoll_ctl() man page.
                            DEBUG("Closed connection socket and deleted it from epoll instance.");
                            finished = 1;
                        }
                        else { // mtcp_read() error handling
                            if (errno == ENOTCONN) {
                                mtcp_close(mctx, events[i].data.sockid);
                                mtcp_epoll_ctl(mctx, epfd, MTCP_EPOLL_CTL_DEL, events[i].data.sockid, &ev);
                                // the last poniter, &ev, will be just ignored; this is given to be backward-compatible. See epoll_ctl() man page.
                                DEBUG("Closed connection socket and deleted it from epoll instance.");
                                finished = 1;
                            }
                            else {
                                perror("mtcp_read()");
                                exit(-1);
                            }
                        }
                    }
                }
            }
        }
        else { // mtcp_epoll_wait() error handling
            perror("mtcp_epoll_wait()");
            exit(-1); 
        }

    }


    mtcp_close(mctx, sockfd);
    DEBUG("Server (listening) socket closed.");

    printf("\n");
    printf("Total amount of data received: %d bytes\n", read_total);

    mtcp_destroy_context(mctx);
    free(ctx);
    mtcp_destroy();

    return 0;
}