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
#define REQEUST_SIZE    512

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
    int events_ready;
    int core = 0;

    // sockets
	struct sockaddr_in saddr;
	// struct sockaddr_in daddr;
    int listen_sockfd, epfd, conn_sockfd;
    int backlog = 3;

    // time
    // struct timespec t1, t2;

    // buffer
    char *send_buf, *recv_buf;

    // the amount to send in bytes
    int send_size;
    // int recv_buf_size;


    if (argc < 4) {
        printf("Usage: ./server [IPv4] [port] [reply size]\n");
        return -1;
    }
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = inet_addr(argv[1]);
    saddr.sin_port = htons(atoi(argv[2]));
    send_size = atoi(argv[3]);
 
    // Sned Buffer allocation
    send_buf = malloc(send_size);
    if (!send_buf) {
        ERROR("malloc() for send_buf failed.");
        return -1;
    }
    memset(send_buf, 0x41, send_size); // 0x41 = 'A'

    // Receive buffer allocation
    recv_buf = malloc(REQEUST_SIZE);
    if (!recv_buf) {
        ERROR("malloc() for recv_buf failed.");
        return -1;
    }
    memset(recv_buf, 0x00, REQEUST_SIZE);
    recv_buf[REQEUST_SIZE-1] = '\0';


    // This must be done before mtcp_init()
    mtcp_getconf(&mcfg);
    mcfg.num_cores = 1;
    mtcp_setconf(&mcfg);

    // Seed RNG
    // srand(time(NULL));

    // Init mTCP
    DEBUG("Initializing mTCP...");
    if (mtcp_init("app.conf")) {
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
    listen_sockfd = mtcp_socket(mctx, AF_INET, SOCK_STREAM, 0);
    if (listen_sockfd < 0) {
        ERROR("Failed to create socket.");
        return -1;
    }

    // Set listening socket as nonblocking mode
    ret = mtcp_setsock_nonblock(mctx, listen_sockfd);
    if (ret < 0 ) {
        ERROR("Failed to set socket in nonblocking mode.");
        return -1;
    }

    // Register listening socket to epoll instance
    ev.events = MTCP_EPOLLIN;
    ev.data.sockid = listen_sockfd;
    mtcp_epoll_ctl(mctx, epfd, MTCP_EPOLL_CTL_ADD, listen_sockfd, &ev);


    // This is server program who receives data
    ret = mtcp_bind(mctx, listen_sockfd, (struct sockaddr *) &saddr, sizeof(struct sockaddr_in));
    if (ret < 0) {
        ERROR("Failed to bind to the listening socket.");
        perror("mtcp_bind()");
        exit(-1);
    }

    ret = mtcp_listen(mctx, listen_sockfd, backlog);
    if (ret < 0) {
        ERROR("Failed to listen.");
        perror("mtcp_listen()");
    }

    DEBUG("Now socket (sockfd=%d) is listening.\n", listen_sockfd);

    // int i;
    // int read_total = 0;
    // int send_total = 0;
    // int request_received = 0;
    // int finished = 0;
    // while (!finished) {
    //     events_ready = mtcp_epoll_wait(mctx, epfd, events, mcfg.max_num_buffers, -1);
    //     if (events_ready > 0) {
    //         for (i = 0; i < events_ready; i++) {
    //             if (events[i].data.sockid == sockfd) { // listening socket
    //                 conn_sockfd = mtcp_accept(mctx, listen_sockfd, NULL, NULL);
    //                 if (conn_sockfd >= 0) {
    //                     DEBUG("Accepted new connection with new socket %d.", conn_sockfd);
    //                     ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT;
    //                     ev.data.sockid = conn_sockfd;
    //                     mtcp_epoll_ctl(mctx, epfd, MTCP_EPOLL_CTL_ADD, conn_sockfd, &ev);
    //                 }
    //                 else { // mtcp_accept() error handling
    //                     perror("mtcp_accept()");
    //                     exit(-1);
    //                 }
    //             }
    //             else { // conenction socket with client
    //                 if (events[i].events & MTCP_EPOLLIN) {
    //                     // assumption: only 1 client will connect to this server
    //                     ret = mtcp_read(mctx, events[i].data.sockid, recv_buf, REQEUST_SIZE);
    //                     if (ret > 0) {
    //                         DEBUG("Read %d bytes: %s", ret, recv_buf);
    //                         read_total += ret;
    //                         request_received = 1;

    //                         // Now send response data (send_size bytes)
    //                         // if (events[i].events & MTCP_EPOLLOUT)  {
    //                         ret = mtcp_write(mctx, events[0].data.sockid, send_buf + send_total, send_size - send_total);
    //                         if (ret >= 0) {
    //                             DEBUG("Sent %d bytes.", ret);
    //                             send_total += ret;
    //                         }
    //                         else {
    //                             perror("mtcp_write()");
    //                             exit(-1);
    //                         }                                  
    //                         // }
    //                     }
    //                     else if (ret == 0) { // maybe this is FIN-received?
    //                         mtcp_epoll_ctl(mctx, epfd, MTCP_EPOLL_CTL_DEL, events[i].data.sockid, &ev); // the last poniter, &ev, will be just ignored; this is given to be backward-compatible. See epoll_ctl() man page.
    //                         mtcp_close(mctx, events[i].data.sockid);
    //                         DEBUG("Closed connection socket and deleted it from epoll instance.");
    //                         // finished = 1;
    //                     }
    //                     else { // mtcp_read() error handling
    //                         if (errno == ENOTCONN) {
    //                             mtcp_epoll_ctl(mctx, epfd, MTCP_EPOLL_CTL_DEL, events[i].data.sockid, &ev); // the last poniter, &ev, will be just ignored; this is given to be backward-compatible. See epoll_ctl() man page.
    //                             mtcp_close(mctx, events[i].data.sockid);
    //                             DEBUG("Closed connection socket and deleted it from epoll instance.");
    //                             finished = 1;
    //                         }
    //                         else {
    //                             perror("mtcp_read()");
    //                             exit(-1);
    //                         }
    //                     }
    //                 }
    //                 else if (events[i].events & MTCP_EPOLLOUT) {
    //                     if (request_received && send_total < send_size) {
    //                         ret = mtcp_write(mctx, events[0].data.sockid, send_buf + send_total, send_size - send_total);
    //                         if (ret >= 0)
    //                             send_total += ret;
    //                         else {
    //                             perror("mtcp_write()");
    //                             exit(-1);
    //                         }
    //                     }
    //                 }
    //             }
    //         }
    //     }
    //     else { // mtcp_epoll_wait() error handling
    //         perror("mtcp_epoll_wait()");
    //         exit(-1); 
    //     }

    // }

    /* ----------------------------------------------------------------------- */
    // simplified version
    int i, event_sock;
    while (1) {
        events_ready = mtcp_epoll_wait(mctx, epfd, events, mcfg.max_num_buffers, -1);
        for (i = 0; i < events_ready; i++) {
            event_sock = events[i].data.sockid;
            if (event_sock == listen_sockfd) { // event on listening socket
                conn_sockfd = mtcp_accept(mctx, listen_sockfd, NULL, NULL);
                DEBUG("Accepted new connection from client (sockfd=%d).", conn_sockfd);
                ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT;
                ev.data.sockid = conn_sockfd;
                mtcp_epoll_ctl(mctx, epfd, MTCP_EPOLL_CTL_ADD, conn_sockfd, &ev);
            }
            else {
                if (events[i].events & MTCP_EPOLLIN) {
                    ret = mtcp_read(mctx, event_sock, recv_buf, REQEUST_SIZE);
                    if (ret > 0) { // data received
                        DEBUG("Read %d bytes from client: %s", ret, recv_buf);
                        ret = mtcp_write(mctx, event_sock, send_buf, send_size);
                        if (ret < 0) {
                            perror("mtcp_write()");
                            exit(-1);
                        }
                        DEBUG("Sent %d bytes to client.", ret);
                    }
                    else if (ret == 0) { // FIN-received
                        mtcp_epoll_ctl(mctx, epfd, MTCP_EPOLL_CTL_DEL, event_sock, &ev); // the last pointer (&ev) will be ignored
                        mtcp_close(mctx, event_sock);
                        DEBUG("Closed connection socket (sockfd=%d)\n", event_sock);
                    }
                    else {
                        perror("mtcp_read()");
                        exit(-1);
                    }
                }
            }
        }
    }

    /* ----------------------------------------------------------------------- */



    mtcp_close(mctx, listen_sockfd);
    DEBUG("Listening socket (sockfd=%d) closed.", listen_sockfd);

    printf("\n");
    // printf("Total amount of data received: %d bytes\n", read_total);
    // printf("Total amount of data sent: %d bytes\n", send_total);


    mtcp_destroy_context(mctx);
    free(ctx);
    mtcp_destroy();

    free(events);
    free(send_buf);
    free(recv_buf);

    return 0;
}