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
	// struct sockaddr_in saddr;
    struct sockaddr_in daddr;
    int sockfd, epfd;

    // time
    struct timespec t1, t2;

    // send_buffer
    char *buf;

    // the amount to send in bytes
    int send_size;


    if (argc < 4) {
        printf("Usage: ./client [IPv4] [port] [amount to send in bytes]\n");
        return -1;
    }
    daddr.sin_family = AF_INET;
    daddr.sin_addr.s_addr = inet_addr(argv[1]);
    daddr.sin_port = htons(atoi(argv[2]));
    send_size = atoi(argv[3]);

    // Send buffer allocation    
    buf = malloc(send_size);
    if (!buf) {
        ERROR("malloc() for buf failed.");
        return -1;
    }
    memset(buf, 0xAA, send_size);
    // buf[send_size-1] = '\0';


    // This must be done before mtcp_init()
    mtcp_getconf(&mcfg);
    mcfg.num_cores = 1;
    mtcp_setconf(&mcfg);

    // Seed RNG
    srand(time(NULL));

    // Init mTCP
    DEBUG("Initializing mTCP...");
    if (mtcp_init("client.conf")) {
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
    DEBUG("Creating pool of TCP source ports...");
    mtcp_init_rss(mctx, INADDR_ANY, IP_RANGE, daddr.sin_addr.s_addr, daddr.sin_port);

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

    // Set client socket as nonblocking mode
    ret = mtcp_setsock_nonblock(mctx, sockfd);
    if (ret < 0 ) {
        ERROR("Failed to set socket in nonblocking mode.");
        return -1;
    }

    // Register client socket to epoll instance
    // ev.events = MTCP_EPOLLIN;
    ev.events = MTCP_EPOLLOUT;
    ev.data.sockid = sockfd;
    mtcp_epoll_ctl(mctx, epfd, MTCP_EPOLL_CTL_ADD, sockfd, &ev);


    // This is client program who sends data
    DEBUG("Connecting socket...");
    ret = mtcp_connect(mctx, sockfd, (struct sockaddr *) &daddr, sizeof(struct sockaddr_in));
    if (ret < 0) {
        ERROR("mtcp_connect() returned < 0.");
        // perror("mtcp_connect()");
        // exit(-1);
        if (errno != EINPROGRESS) {
            perror("mtcp_connect()");
            exit(-1);
            // mtcp_close(mctx, sockfd);
            // mtcp_close(mctx, epfd); // Is this needed?
            // return -1;
        }
    }
    DEBUG("Connection created.");

    // Send and meausre time taken
    int events_ready;
    int wrote_total = 0;
    DEBUG("Start data send.");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    while (1) {
        if (wrote_total < send_size) {
            events_ready = mtcp_epoll_wait(mctx, epfd, events, mcfg.max_num_buffers, -1);
            if (events_ready > 0) {
                if (events_ready > 1) {
                    ERROR("Something wrong - only registered 1 socket to epoll instance, but epoll returned more than 1.");
                    exit(-1);
                }
                if (events[0].events & MTCP_EPOLLOUT) {
                    assert(events[0].data.sockid == sockfd);
                    ret = mtcp_write(mctx, events[0].data.sockid, buf + wrote_total, send_size - wrote_total);
                    if (ret >= 0) {
                        DEBUG("Wrote %d bytes.", ret);
                        wrote_total += ret;
                    }
                    else { // mtcp_write() error handling
                        if (errno == ENOTCONN) {
                            DEBUG("The socket is not writeable.");
                            break;
                        }
                        else {
                            perror("mtcp_write()");
                            exit(-1);
                        }
                    }
                }
            }
            else { // mtcp_epoll_wait() error handling
                perror("mtcp_epoll_wait()");
                exit(-1);
            }
        }
        else // finished data send
            break;
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    DEBUG("Finished data send.");


    mtcp_close(mctx, sockfd);
    mtcp_close(mctx, epfd);
    DEBUG("Sockets are closed.");

    printf("\n");
    int elapsed_time = 0; // in nanoseconds
    elapsed_time += (t2.tv_nsec - t1.tv_nsec);
    elapsed_time += (t2.tv_sec - t1.tv_sec) * 1000000000;
    printf("Time taken to send %d bytes: %d nsec\n", send_size, elapsed_time);

    mtcp_destroy_context(mctx);
    free(ctx);
    mtcp_destroy();

    return 0;
}