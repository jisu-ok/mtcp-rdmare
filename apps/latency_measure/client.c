#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>

#include "get_clock.h"

#include <mtcp_api.h>
#include <mtcp_epoll.h>

#include <rte_cycles.h>


#define CONCURRENCY		1
#define IP_RANGE 		1
#define REQEUST_SIZE    512

#define DEBUG(fmt, args...)	fprintf(stderr, "[DEBUG] " fmt "\n", ## args)
#define ERROR(fmt, args...)	fprintf(stderr, fmt "\n", ## args)

int time_diff_nsec(struct timespec t1, struct timespec t2) {
    return (t2.tv_nsec - t1.tv_nsec) + 1000000000 * (t2.tv_sec - t1.tv_sec);
}

int compare(const void *a, const void *b) {
    double num1 = *(double *)a;
    double num2 = *(double *)b;

    if (num1 < num2)
        return -1;

    if (num1 > num2)
        return 1;

    return 0;
}

double median(double *vals, int n) {
    if (n % 2 == 1) // odd number of elements
        return vals[(n-1) / 2];
    else // even number of elements
        return (vals[n/2 - 1] + vals[n/2]) / 2;
}

void compare_data(char *answer, char *received, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (answer[i] != received[i]) {
            ERROR("Received data is wrong: %d-th byte should be %c, but received %c", i, answer[i], received[i]);
            exit(-1);
        }
    }
    DEBUG("Received data is correct!");
    return;
}

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
    int core = 13;

    // sockets
	// struct sockaddr_in saddr;
    struct sockaddr_in daddr;
    int sockfd, epfd;

    // time
    // struct timespec t1, t2;
    // struct timespec t3, t4;

    // buffer
    char *send_buf, *recv_buf, *answer_buf;

    // the amount to reqeust in bytes
    int fetch_size;


    if (argc < 4) {
        printf("Usage: ./client [IPv4] [port] [amount to request in bytes]\n");
        return -1;
    }
    daddr.sin_family = AF_INET;
    daddr.sin_addr.s_addr = inet_addr(argv[1]);
    daddr.sin_port = htons(atoi(argv[2]));
    fetch_size = atoi(argv[3]);

    // Send buffer allocation and fill request string in
    send_buf = malloc(REQEUST_SIZE);
    if (!send_buf) {
        ERROR("malloc() for send_buf failed.");
        return -1;
    }
    memset(send_buf, 0, REQEUST_SIZE);
    sprintf(send_buf, "Request - give me %d bytes of data!", fetch_size);

    // Recv buffer allocation    
    recv_buf = malloc(fetch_size + 1);
    if (!recv_buf) {
        ERROR("malloc() for recv_buf failed.");
        return -1;
    }
    memset(recv_buf, 0, fetch_size);
    recv_buf[fetch_size] = '\0';

    // Answer buffer allocation and set
    answer_buf = malloc(fetch_size);
    if (!answer_buf) {
        ERROR("malloc() for answer_buf failed.");
        return -1;
    }
    int j;
    for (j=0; j < fetch_size; j++)
        answer_buf[j] = 0x41 + (j % 26);



    // This must be done before mtcp_init()
    mtcp_getconf(&mcfg);
    mcfg.num_cores = 16;
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
    ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT;
    ev.data.sockid = sockfd;
    mtcp_epoll_ctl(mctx, epfd, MTCP_EPOLL_CTL_ADD, sockfd, &ev);


    // This is client program who sends data
    DEBUG("Connecting socket...");
    ret = mtcp_connect(mctx, sockfd, (struct sockaddr *) &daddr, sizeof(struct sockaddr_in));

    if (ret < 0) {
        ERROR("mtcp_connect() returned < 0.");
        if (errno != EINPROGRESS) {
            perror("mtcp_connect()");
            exit(-1);
        }
    }

    events_ready = mtcp_epoll_wait(mctx, epfd, events, mcfg.max_num_buffers, -1);
    assert(events_ready == 1);
    int sock_err, sock_err_len = sizeof(int);
    ret = mtcp_getsockopt(mctx, sockfd, SOL_SOCKET, SO_ERROR, &sock_err, (socklen_t *) &sock_err_len);
    if (ret < 0) {
        perror("mtcp_getsockopt()");
        exit(-1);
    }
    if (sock_err != 0) {
        DEBUG("connect() didn't complete well: %s", strerror(sock_err));
        exit(-1);
    }
    DEBUG("Connection established!");

    // if (events[0].events & MTCP_EPOLLIN)
    //     DEBUG("MTCP_EPOLLIN");
    // if (events[0].events & MTCP_EPOLLOUT)
    //     DEBUG("MTCP_EPOLLOUT");


    /* ----------------------------------------------------------------------- */
    // DEBUG("Start request.");
    // clock_gettime(CLOCK_MONOTONIC, &t1);    
    // // Send request
    // int send_size = strlen(send_buf);
    // int wrote_total = 0;
    // if (events[0].events & MTCP_EPOLLOUT)   {
    //     ret = mtcp_write(mctx, events[0].data.sockid, send_buf, send_size);
    //     if (ret >= 0)
    //         wrote_total += ret;
    //     else {
    //         perror("mtcp_write()");
    //         exit(-1);
    //     }
    // }
    // else {
    //     DEBUG("Something wrong");
    //     exit(-1);
    // }

    // // If write isn't done in the first try
    // while (1) {
    //     if (wrote_total < send_size) {
    //         events_ready = mtcp_epoll_wait(mctx, epfd, events, mcfg.max_num_buffers, -1);
    //         if (events_ready > 0) {
    //             assert(events_ready == 1);
    //             if (events[0].events & MTCP_EPOLLOUT) {
    //                 ret = mtcp_write(mctx, events[0].data.sockid, send_buf + wrote_total, send_size - wrote_total);
    //                 if (ret >= 0)
    //                     wrote_total += ret;
    //                 else {
    //                     perror("mtcp_write()");
    //                     exit(-1);
    //                 }
    //             }
    //         }
    //         else {
    //             perror("mtcp_epoll_wait()");
    //             exit(-1);
    //         }
    //     }
    //     else
    //         break;
    // }

    // // DEBUG("Receive data from server");
    // clock_gettime(CLOCK_MONOTONIC, &t2);    
    // // Read response
    // int read_total = 0;

    // // first, check if there already exist readable event
    // if ((events[0].data.sockid == sockfd) && (events[0].events & MTCP_EPOLLIN)) {
    //     ret = mtcp_read(mctx, events[0].data.sockid, recv_buf, fetch_size);
    //     if (ret > 0) {
    //         DEBUG("Received %d bytes data.", ret);
    //         read_total += ret;
    //     }
    //     else if (ret == 0) {
    //         DEBUG("Server closed socket before fetch is done.");
    //         exit(-1);
    //     }
    //     else {
    //         perror("mtcp_read()");
    //         exit(-1);
    //     }
    // }

    // clock_gettime(CLOCK_MONOTONIC, &t3);    

    // // epoll for read
    // while (read_total < fetch_size) {
    //     events_ready = mtcp_epoll_wait(mctx, epfd, events, mcfg.max_num_buffers, -1);
    //     if (events_ready > 0) {
    //         assert(events_ready == 1);
    //         if (events[0].events & MTCP_EPOLLIN) {
    //             ret = mtcp_read(mctx, events[0].data.sockid, recv_buf + read_total, fetch_size - read_total);
    //             if (ret > 0)
    //                 read_total += ret;
    //             else if (ret == 0) {
    //                 DEBUG("Server closed socket before fetch is done.");
    //                 exit(-1);
    //             }
    //             else {
    //                 perror("mtcp_read()");
    //                 exit(-1);
    //             }
    //         }
    //     }
    //     else { // mtcp_epoll_wait() error handling
    //         perror("mtcp_epoll_wait()");
    //         exit(-1);
    //     }
    // }

    // clock_gettime(CLOCK_MONOTONIC, &t4);
    // DEBUG("Data fetch finished.");
    /* ----------------------------------------------------------------------- */

    /* ----------------------------------------------------------------------- */
    // Simplified version
    int iteration = 1000;

    double mhz = get_cpu_mhz(0);
    DEBUG("CPU frequency: %f", mhz);
    // cycles_t c1, c2;
    uint64_t c1, c2;
    // cycles_t c3, c4;
    double latency_vals[iteration];

    int i;
    int send_size = strlen(send_buf);
    int read_total;

    // clock_gettime(CLOCK_MONOTONIC, &t1);    
    // clock_gettime(CLOCK_MONOTONIC, &t2);

    for (i=0; i < iteration; i++) {
        // DEBUG("Start request.");
        read_total = 0;

        // c1 = get_cycles();
        c1 = rte_get_tsc_cycles();
        ret = mtcp_write(mctx, sockfd, send_buf, send_size);
        // c4 = get_cycles();

        if (ret <= 0) {
            perror("mtcp_write()");
            exit(-1);
        }
        // DEBUG("Wrote %d bytes.", ret);
        
        while (read_total < fetch_size) {
            // c3 = get_cycles();
            events_ready = mtcp_epoll_wait(mctx, epfd, events, mcfg.max_num_buffers, -1);
            // c4 = get_cycles();
            // DEBUG("mtcp_epoll_wait() took %lld cycle", c4 - c3);
            
            if (events[0].events & MTCP_EPOLLIN) {
                // c3 = get_cycles();
                ret = mtcp_read(mctx, sockfd, recv_buf + read_total, fetch_size - read_total);
                // c4 = get_cycles();
                // DEBUG("mtcp_read() took %lld cycle", c4 - c3);
               
                if (ret <= 0) {
                    perror("mtcp_read()");
                    exit(-1);
                }
                read_total += ret;
            }
        }

        // No epoll version
        // while (read_total < fetch_size) {
        //     ret = mtcp_read(mctx, sockfd, recv_buf + read_total, fetch_size - read_total);
        //     if (ret <= 0)
        //         continue;
        //     read_total += ret;
        // }



        // do
        //     events_ready = mtcp_epoll_wait(mctx, epfd, events, mcfg.max_num_buffers, -1);
        // while (!(events[0].events & MTCP_EPOLLIN));
        // ret = mtcp_read(mctx, sockfd, recv_buf, fetch_size);
        // if (ret <= 0) {
        //     perror("mtcp_read()");
        //     exit(-1);
        // }

        // c2 = get_cycles();
        c2 = rte_get_tsc_cycles();
        // printf("latency: %f nsec\n", (c2 - c1) / mhz);
        printf("%f, i=%d\n", (c2 - c1) / mhz, i);
        latency_vals[i] = (double) ((c2 - c1) / mhz);
        compare_data(answer_buf, recv_buf, fetch_size);
        // DEBUG("Read %d bytes. Go to next iteration", read_total);
    }


    // DEBUG("Data fetch finished. cnt = %d", cnt);

    /* ----------------------------------------------------------------------- */



    // Send and meausre time taken
    // int cnt = 0;
    // int wrote_total = 0;
    // DEBUG("Start data send.");
    // clock_gettime(CLOCK_MONOTONIC, &t1);
    // while (1) {
    //     if (wrote_total < send_size) {
    //         // DEBUG("Before epoll_wait()...");
    //         events_ready = mtcp_epoll_wait(mctx, epfd, events, mcfg.max_num_buffers, -1);
    //         clock_gettime(CLOCK_MONOTONIC, &t3);
    //         cnt += 1;
    //         if (events_ready > 0) {
    //             if (events_ready > 1) {
    //                 ERROR("Something wrong - only registered 1 socket to epoll instance, but epoll returned more than 1.");
    //                 exit(-1);
    //             }
    //             if (events[0].events & MTCP_EPOLLOUT) {
    //                 // assert(events[0].data.sockid == sockfd);
    //                 // clock_gettime(CLOCK_MONOTONIC, &t3);
    //                 ret = mtcp_write(mctx, events[0].data.sockid, buf + wrote_total, send_size - wrote_total);
    //                 // clock_gettime(CLOCK_MONOTONIC, &t4);
    //                 // printf("mtcp_write() took %ld nsec.\n", t4.tv_nsec - t3.tv_nsec + 1000000000*(t4.tv_sec - t3.tv_sec));
    //                 if (ret >= 0) {
    //                     // DEBUG("Wrote %d bytes.", ret);
    //                     wrote_total += ret;
    //                 }
    //                 else { // mtcp_write() error handling
    //                     if (errno == ENOTCONN) {
    //                         // DEBUG("The socket is not writeable.");
    //                         break;
    //                     }
    //                     else {
    //                         perror("mtcp_write()");
    //                         exit(-1);
    //                     }
    //                 }
    //             }
    //         }
    //         else { // mtcp_epoll_wait() error handling
    //             perror("mtcp_epoll_wait()");
    //             exit(-1);
    //         }
    //     }
    //     else // finished data send
    //         break;
    // }
    // clock_gettime(CLOCK_MONOTONIC, &t2);
    // DEBUG("Finished data send. cnt=%d, t3-t1 = %ld", cnt, t3.tv_nsec - t1.tv_nsec + 1000000000*(t3.tv_sec - t1.tv_sec));


    mtcp_close(mctx, sockfd);
    mtcp_close(mctx, epfd);
    DEBUG("Sockets are closed.");

    printf("\n");
    // printf("Received data: %s\n", recv_buf);
    // printf("Total Time taken to fetch %d bytes: %d nsec\n", fetch_size, time_diff_nsec(t1, t2));


    qsort(latency_vals, iteration, sizeof(double), compare);
    DEBUG("Median: %f", median(latency_vals, iteration));

    // printf("Detailed:\n");
    // printf("  t2 - t1: %d\n", time_diff_nsec(t1, t2));
    // printf("  t3 - t2: %d\n", time_diff_nsec(t2, t3));
    // printf("  t4 - t3: %d\n", time_diff_nsec(t3, t4));

    mtcp_destroy_context(mctx);
    free(ctx);
    mtcp_destroy();

    free(events);
    free(send_buf);
    free(recv_buf);
    return 0;
}