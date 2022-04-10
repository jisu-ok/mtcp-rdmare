/* for io_module_func def'ns */
#include "io_module.h"
#ifndef DISABLE_RDMARE
/* for mtcp related def'ns */
#include "mtcp.h"
/* for errno */
#include <errno.h>
/* for logging */
#include "debug.h"
/* for num_devices_* */
#include "config.h"

/* for using RDMA Raw Ethernet */
#include <infiniband/verbs.h>

#define ENABLE_SEND_INLINE
#define MAX_INLINE_DATA	972
#define ENABLE_SEND_SIGNAL

#define MAX_SEND_BURST	64 /* maximum number of send pkt burst */
#define MAX_RECV_BURST	64 /* maximum number of recv pkt burst */

#define ENTRY_SIZE 9000 /* maximum size of each packet */

#define PORT_NUM 1

#define SQ_NUM_DESC 512 /* maximum number of sends waiting for completion */
#define RQ_NUM_DESC 512 /* The maximum receive ring length without processing */

#define MAC_SANCTUARY {0x24, 0x8A, 0x07, 0x91, 0x69, 0xA0}
#define MAC_MOSCOW {0x24, 0x8A, 0x07, 0x91, 0x68, 0xE8}

struct ibv_env {
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct ibv_cq *send_cq;
	struct ibv_cq *recv_cq;
	struct ibv_qp *qp;
	struct ibv_mr *send_mr;
	struct ibv_mr *recv_mr;
	struct ibv_sge *send_sge;
	struct ibv_sge *recv_sge;
	struct ibv_send_wr *send_wr;
	struct ibv_recv_wr *recv_wr;
	struct ibv_wc send_wc;
	struct ibv_wc *recv_wc;
	struct ibv_flow *eth_flow;
};

struct rdmare_private_context {
	// struct ibv_context *context;
	// struct ibv_pd *pd;
	// struct ibv_cq *send_cq;
	// struct ibv_cq *recv_cq;
	// struct ibv_qp *qp;
	// struct ibv_mr *send_mr;
	// struct ibv_mr *recv_mr;
	// struct ibv_sge send_sg_entry;
	// struct ibv_sge recv_sg_entry;
	// struct ibv_send_wr send_wr;
	// struct ibv_send_wr *send_bad_wr;
	// struct ibv_recv_wr recv_wr;
	// struct ibv_recv_wr *recv_bad_wr;
	int num_ibv_devices;
	struct ibv_env env[MAX_DEVICES];
	void *send_pktbuf[MAX_DEVICES];
	void *recv_pktbuf[MAX_DEVICES];
	uint16_t send_pktsize[MAX_DEVICES][MAX_SEND_BURST];
	uint16_t recv_pktsize[MAX_DEVICES][MAX_RECV_BURST];
	int send_headidx[MAX_DEVICES];
	int recv_headidx[MAX_DEVICES];
	uint16_t send_pktnum[MAX_DEVICES];
	// uint64_t send_n[MAX_DEVICES];
	// uint64_t recv_n;
} __attribute__((aligned(__WORDSIZE)));

/*----------------------------------------------------------------------------*/

/* Used to set system-wide I/O module initialization */
void
rdmare_load_module(void)
{

}

/*----------------------------------------------------------------------------*/

/*
 *  Used to initialize the driver library.
 *  Also use the context to create/initialize a private packet I/O data structures.
 */
void
rdmare_init_handle(struct mtcp_thread_context *ctx)
{
	struct rdmare_private_context *rpc;

	/* create and initialize private I/O module context */
	ctx->io_private_context = calloc(1, sizeof(struct rdmare_private_context));
	if (ctx->io_private_context == NULL) {
		TRACE_ERROR("Failed to initialize ctx->io_private_context: Can't allocate memory\n");
		exit(EXIT_FAILURE);
	}
	rpc = (struct rdmare_private_context *)ctx->io_private_context;

	/* Initialize default values */ // no need?
	// memset(&rpc->send_sg_entry, 0, sizeof(rpc->send_sg_entry));
	// memset(&rpc->recv_sg_entry, 0, sizeof(rpc->recv_sg_entry));
	// memset(&rpc->send_wr, 0, sizeof(rpc->send_wr));
	// memset(&rpc->recv_wr, 0, sizeof(rpc->recv_wr));
	// rpc->send_pkt_size = 0;
	// for (int i = 0; i < RQ_NUM_DESC; i++)
	// 	rpc->recv_pkt_size[i] = 0;
	// rpc->send_n = 0;
	// rpc->recv_n = 0;
	// rpc->recv_pkt_idx = 0;

	struct ibv_device **dev_list;
	struct ibv_device *ib_dev;
	int ib_idx, ret;
	struct ibv_env *env;
	
	/* Get the list of ibv devcies */
	dev_list = ibv_get_device_list(&rpc->num_ibv_devices);
	if (!dev_list) {
		TRACE_ERROR("Failed to get IB devices list\n");
		exit(EXIT_FAILURE);
	}

	printf("Found %d IB devices\n", rpc->num_ibv_devices);

	for (ib_idx = 0; ib_idx < rpc->num_ibv_devices; ib_idx++) {
		ib_dev = dev_list[ib_idx];
		if (!ib_dev) {
			TRACE_ERROR("IB device not found\n");
			exit(EXIT_FAILURE);
		}

		env = &rpc->env[ib_idx];

		/* Get the device context */
		env->context = ibv_open_device(ib_dev);
		if (!env->context) {
			TRACE_ERROR("Couldn't get context for %s\n", ibv_get_device_name(ib_dev));
			exit(EXIT_FAILURE);
		}

		/* Allocate Protection Domain */
		env->pd = ibv_alloc_pd(env->context);
		if (!env->pd) {
			TRACE_ERROR("Couldn't allocate PD\n");
			exit(EXIT_FAILURE);
		}

		/* Create Completion Queue (CQ) */
		env->send_cq = ibv_create_cq(env->context, SQ_NUM_DESC, NULL, NULL, 0);
		if (!env->send_cq) {
			TRACE_ERROR("Couldn't create send CQ\n");
			exit(EXIT_FAILURE);
		}

		env->recv_cq = ibv_create_cq(env->context, RQ_NUM_DESC, NULL, NULL, 0);
		if (!env->recv_cq) {
			TRACE_ERROR("Couldn't create recv QP\n");
			exit(EXIT_FAILURE);
		}
		
		/* Set QP initialize attributes */
		struct ibv_qp_init_attr qp_init_attr = {
			.qp_context = NULL,
			.send_cq = env->send_cq,
			.recv_cq = env->recv_cq,
			.cap = {
				.max_send_wr = SQ_NUM_DESC,
				.max_send_sge = 1,
				.max_recv_wr = RQ_NUM_DESC,
				.max_recv_sge = 1,
				.max_inline_data = MAX_INLINE_DATA, // measured max number for ConnectX-4
			},
			.qp_type = IBV_QPT_RAW_PACKET,
			.sq_sig_all = 0
		};

		/* Create QP */
		env->qp = ibv_create_qp(env->pd, &qp_init_attr);
		if (!env->qp) {
			TRACE_ERROR("Couldn't create RSS QP\n");
			exit(EXIT_FAILURE);
		}

		/* Initialize the QP (receive ring) and assign a port */
		struct ibv_qp_attr qp_attr;
		int qp_flags;
		memset(&qp_attr, 0, sizeof(qp_attr));

		qp_flags = IBV_QP_STATE | IBV_QP_PORT;
		qp_attr.qp_state = IBV_QPS_INIT;
		qp_attr.port_num = 1;
		ret = ibv_modify_qp(env->qp, &qp_attr, qp_flags);
		if (ret < 0) {
			TRACE_ERROR("Failed modify QP to init\n");
			exit(EXIT_FAILURE);
		}

		/* Move the ring to ready-to-receive and then ready-to-send */
		memset(&qp_attr, 0, sizeof(qp_attr));
		qp_flags = IBV_QP_STATE;
		qp_attr.qp_state = IBV_QPS_RTR;
		ret = ibv_modify_qp(env->qp, &qp_attr, qp_flags);
		if (ret < 0) {
			TRACE_ERROR("Failed modify QP to RTR\n");
			exit(EXIT_FAILURE);
		}

		qp_flags = IBV_QP_STATE;
		qp_attr.qp_state = IBV_QPS_RTS;
		ret = ibv_modify_qp(env->qp, &qp_attr, qp_flags);
		if (ret < 0) {
			TRACE_ERROR("Failed modify QP to RTS\n");
			exit(EXIT_FAILURE);
		}

		/* Allocate memory for send/receive packet buffer */
		int send_bufsize = ENTRY_SIZE*MAX_SEND_BURST; /* maximum size of data to be access directly by HW */
		rpc->send_pktbuf[ib_idx] = malloc(send_bufsize);
		if (!rpc->send_pktbuf) {
			TRACE_ERROR("Couldn't allocate memory for send packet buffer\n");
			exit(EXIT_FAILURE);
		}
		memset(rpc->send_pktbuf[ib_idx], 0, send_bufsize); // optional?

		int recv_bufsize = ENTRY_SIZE*MAX_RECV_BURST;
		rpc->recv_pktbuf[ib_idx] = malloc(recv_bufsize);
		if (!rpc->recv_pktbuf) {
			TRACE_ERROR("Couldn't allocate memory for recv packet buffer\n");
			exit(EXIT_FAILURE);
		}
		memset(rpc->recv_pktbuf[ib_idx], 0, recv_bufsize); //optional?

		/* Register the user memory so it can be accessed by the HW directly */
		env->send_mr = ibv_reg_mr(env->pd, rpc->send_pktbuf[ib_idx], send_bufsize, IBV_ACCESS_LOCAL_WRITE);
		if (!env->send_mr) {
			TRACE_ERROR("Couldn't register MR for send packet buffer\n");
			exit(EXIT_FAILURE);
		}

		env->recv_mr = ibv_reg_mr(env->pd, rpc->recv_pktbuf[ib_idx], recv_bufsize, IBV_ACCESS_LOCAL_WRITE);
		if (!env->recv_mr) {
			TRACE_ERROR("Couldn't register MR for recv packet buffer\n");
			exit(EXIT_FAILURE);
		}

		/* Create steering rule */
		struct raw_eth_flow_attr {
			struct ibv_flow_attr attr;
			struct ibv_flow_spec_eth spec_eth;
		} __attribute__((packed)) flow_attr = {
			.attr = {
				.comp_mask = 0,
				.type = IBV_FLOW_ATTR_NORMAL,
				.size = sizeof(flow_attr),
				.priority = 0,
				.num_of_specs = 1,
				.port = PORT_NUM,
				.flags = 0,
			},
			.spec_eth = {
				.type = IBV_FLOW_SPEC_ETH,
				.size = sizeof(struct ibv_flow_spec_eth),
				.val = {
					.dst_mac = MAC_SANCTUARY, // need to be specified
					.src_mac = MAC_MOSCOW, // need to be specified
					.ether_type = 0,
					.vlan_tag = 0,
				},
				.mask = {
					.dst_mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
					.src_mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
					.ether_type = 0,
					.vlan_tag = 0,
				}
			}
		};
		/* Alternative way */
		// struct ibv_flow_attr attr = {
		// 	.comp_mask = 0,
		// 	// .type = IBV_FLOW_ATTR_ALL_DEFAULT, // another option: IBV_FLOW_ATTR_SNIFFER
		// 	.type = IBV_FLOW_ATTR_SNIFFER,
		// 	.size = sizeof(struct ibv_flow_attr),
		// 	.priority = 0,
		// 	.num_of_specs = 0,
		// 	.port = PORT_NUM,
		// 	.flags = 0
		// };

		/* Register steering rule to QP */
		env->eth_flow = ibv_create_flow(env->qp, &flow_attr.attr);
		// env->eth_flow = ibv_create_flow(env->qp, &attr);
		if (!env->eth_flow) {
			TRACE_ERROR("Couldn't attach steering flow\n");
			exit(EXIT_FAILURE);
		}


		int array_size, i;
		/* Allocate and initialize SG entries and WR templates for send*/
		array_size = sizeof(struct ibv_sge) * MAX_SEND_BURST;
		env->send_sge = (struct ibv_sge *) malloc(array_size);
		if (!env->send_sge) {
			TRACE_ERROR("Couldn't allocate memory for send SG entries\n");
			exit(EXIT_FAILURE);
		}
		memset(env->send_sge, 0, array_size);

		array_size = sizeof(struct ibv_send_wr) * MAX_SEND_BURST;
		env->send_wr = (struct ibv_send_wr *) malloc(array_size);
		if (!env->send_wr) {
			TRACE_ERROR("Couldn't allocate memory for send WRs\n");
			exit(EXIT_FAILURE);
		}
		memset(env->send_wr, 0, array_size);

		for (i = 0; i < MAX_SEND_BURST; i++) {
			env->send_sge[i].addr = (uint64_t) rpc->send_pktbuf[ib_idx] + i*ENTRY_SIZE;
			env->send_sge[i].lkey = env->send_mr->lkey;

			env->send_wr[i].next = NULL;
			env->send_wr[i].sg_list = &env->send_sge[i];
			env->send_wr[i].num_sge = 1;
			env->send_wr[i].opcode = IBV_WR_SEND;

			env->send_wr[i].send_flags |= IBV_SEND_IP_CSUM;
#ifdef ENABLE_SEND_INLINE
			// env->send_wr[i].send_flags |= IBV_SEND_INLINE;
#endif
			if (i == MAX_SEND_BURST-1) {
				env->send_wr[i].wr_id = i;
				env->send_wr[i].send_flags |= IBV_SEND_SIGNALED;			
			}
		}

	// 	env->send_sge.lkey = env->send_mr->lkey;

	// 	env->send_wr.num_sge = 1;
	// 	env->send_wr.sg_list = &env->send_sge;
	// 	env->send_wr.next = NULL;
	// 	env->send_wr.opcode = IBV_WR_SEND;
	// #ifdef ENABLE_SEND_INLINE
	// 	env->send_wr.send_flags |= IBV_SEND_INLINE;
	// #endif

		/* Allocate and initialize SG entries and WR templates for recv, and post WRs */
		array_size = sizeof(struct ibv_sge) * MAX_RECV_BURST;
		env->recv_sge = (struct ibv_sge *) malloc(array_size);
		if (!env->recv_sge) {
			TRACE_ERROR("Couldn't allocate memory for recv SG entries\n");
			exit(EXIT_FAILURE);
		}
		memset(env->recv_sge, 0, array_size);

		array_size = sizeof(struct ibv_recv_wr) * MAX_RECV_BURST;
		env->recv_wr = (struct ibv_recv_wr *) malloc(array_size);
		if (!env->recv_wr) {
			TRACE_ERROR("Couldn't allocate memory for recv WRs\n");
			exit(EXIT_FAILURE);
		}
		memset(env->recv_wr, 0, array_size);

		struct ibv_recv_wr *recv_bad_wr;
		for (i = 0; i < MAX_RECV_BURST; i++) {
			env->recv_sge[i].addr = (uint64_t) rpc->recv_pktbuf[ib_idx] + i*ENTRY_SIZE;
			env->recv_sge[i].length = ENTRY_SIZE;
			env->recv_sge[i].lkey = env->recv_mr->lkey;

			env->recv_wr[i].next = NULL;
			env->recv_wr[i].sg_list = &env->recv_sge[i];
			env->recv_wr[i].num_sge = 1;
			env->recv_wr[i].wr_id = i;
			ibv_post_recv(env->qp, &env->recv_wr[i], &recv_bad_wr);	
		}

		// env->recv_sge.length = ENTRY_SIZE;
		// env->recv_sge.lkey = env->recv_mr->lkey;

		// env->recv_wr.num_sge = 1;
		// env->recv_wr.sg_list = &env->recv_sge;
		// env->recv_wr.next = NULL;


		/* Post recv WRs */
		// int n;
		// for (n = 0; n < MAX_RECV_BURST; n++) {
		// 	// env->recv_sge.addr = (uint64_t) rpc->recv_pktbuf[ib_idx] + ENTRY_SIZE*n;
		// 	// env->recv_wr.wr_id = n;
		// 	// ibv_post_recv(env->qp, &env->recv_wr, &recv_bad_wr);
		// }
		// for (rpc->recv_n = 0; rpc->recv_n < RQ_NUM_DESC; rpc->recv_n++) {
		// 	rpc->recv_sg_entry.addr = (uint64_t)rpc->recv_pktbuf + ENTRY_SIZE*rpc->recv_n;
		// 	rpc->recv_wr.wr_id = rpc->recv_n;
		// 	ibv_post_recv(rpc->qp, &rpc->recv_wr, &rpc->recv_bad_wr);
		// }


		/* Allocate WC array for recv*/
		array_size = sizeof(struct ibv_wc) * MAX_RECV_BURST;
		env->recv_wc = (struct ibv_wc *) malloc(array_size);
		if (!env->recv_wc) {
			TRACE_ERROR("Couldn't allocate memory for recv WC array\n");
			exit(EXIT_FAILURE);
		}
		memset(env->recv_wc, 0, array_size);
	}

	ibv_free_device_list(dev_list);
}

/*----------------------------------------------------------------------------*/

/*
 *  Used to add link(s) to the mtcp stack.
 *  Returns 0 on success; -1 on failure
 */
int
rdmare_link_devices(struct mtcp_thread_context *ctx)
{
	return 0;
}

/*----------------------------------------------------------------------------*/

/*
 * Release the packet if mTCP does not need to process it
 * (e.g. non-IPv4, non-TCP pkts).
 */
void
rdmare_release_pkt(struct mtcp_thread_context *ctx, int ifidx, unsigned char *pdt_data, int len)
{

}

/*----------------------------------------------------------------------------*/

/*
 * Transmit batch of packets via interface idx (=nif)
 * Returns 0 on success; -1 on failure
 */
int
rdmare_send_pkts(struct mtcp_thread_context *ctx, int nif)
{
	// printf(">>>>J: rdmare_send_pkts() called!\n");

	struct rdmare_private_context *rpc;
	struct ibv_env *env;
	// struct ibv_wc wc;
	int ret, msgs_completed, i, pkt_index;
	int idx = 0;

	rpc = (struct rdmare_private_context *) ctx->io_private_context;
	env = &rpc->env[idx];

	if (rpc->send_pktnum[idx] == 0)
		return 0;

	// printf("send_pktnum=%d\n", rpc->send_pktnum[idx]);

	struct ibv_send_wr *send_bad_wr;
	for (i = 0; i < rpc->send_pktnum[idx]; i++) {
		pkt_index = (rpc->send_headidx[idx] + i) % MAX_SEND_BURST;

		env->send_sge[pkt_index].length = rpc->send_pktsize[idx][pkt_index];
		// if (env->send_sge[pkt_index].length < MAX_INLINE_DATA)
		// 	env->send_wr[pkt_index].send_flags |= IBV_SEND_INLINE;
		ret = ibv_post_send(env->qp, &env->send_wr[pkt_index], &send_bad_wr);
		if (ret < 0) {
			TRACE_ERROR("Failed in post send\n");
			exit(EXIT_FAILURE);
		}

		if (pkt_index == MAX_SEND_BURST-1) {
			do
			msgs_completed = ibv_poll_cq(env->send_cq, 1, &env->send_wc);
			while (msgs_completed <= 0);
			if (msgs_completed < 0) {
				TRACE_ERROR("Polling error\n");
				exit(EXIT_FAILURE);
			}
		}
	}

	rpc->send_headidx[idx] += rpc->send_pktnum[idx];
	rpc->send_pktnum[idx] = 0;


	// env->send_sge.addr = (uint64_t) rpc->send_pktbuf[idx];
	// env->send_sge.length = rpc->send_pkt_size[idx];
	// env->send_wr.send_flags = 0;
	// // rpc->send_sg_entry.lkey = rpc->send_mr->lkey;

	// // rpc->send_wr.num_sge = 1;
	// // rpc->send_wr.sg_list = &rpc->send_sg_entry;
	// // rpc->send_wr.next = NULL;
	// // rpc->send_wr.opcode = IBV_WR_SEND;
	// // wr.send_flags = IBV_SEND_INLINE
	// // rpc->send_wr.send_flags = IBV_SEND_SIGNALED;

	// if ((rpc->send_n[idx] % SQ_NUM_DESC/2) == 0) {
	// 	env->send_wr.wr_id = rpc->send_n[idx];
	// 	env->send_wr.send_flags |= IBV_SEND_SIGNALED;
	// }

	// /* push descriptor to hardware */
	// // printf("before ibv_post_send()\n");
	// struct ibv_send_wr *send_bad_wr;
	// ret = ibv_post_send(env->qp, &env->send_wr, &send_bad_wr);
	// // printf("after ibv_post_send(), ret = %d\n", ret);
	// if (ret < 0) {
	// 	TRACE_ERROR("Failed in post send\n");
	// 	exit(EXIT_FAILURE);
	// }

	// // printf(">>>>J: n is %d\n", rpc->n);
	// // printf(">>>>J: After ibv_post_send() (rdmare_send_pkts())\n");

	// // memset(&wc, 0, sizeof(wc));

	// if ((rpc->send_n[idx] % SQ_NUM_DESC/2) == 0) {
	// 	// msgs_completed = 0;
	// 	/* poll for completion */
	// 	do
	// 	msgs_completed = ibv_poll_cq(env->send_cq, 1, &wc);
	// 	while (msgs_completed <= 0);
	// 	if (msgs_completed < 0) {
	// 		TRACE_ERROR("Polling error\n");
	// 		exit(EXIT_FAILURE);
	// 	}		
	// }


	// rpc->send_n[idx]++;

	// rpc->send_pkt_size[idx] = 0;

	return 0;
}

/*----------------------------------------------------------------------------*/

/*
 * Retrieve the next empty pkt buffer for the application for packet writing.
 * Returns ptr to pkt buffer.
 */
uint8_t *
rdmare_get_wptr(struct mtcp_thread_context *ctx, int nif, uint16_t len)
{
	// printf(">>>>J: rdmare_get_wptr(): called!\n");

	struct rdmare_private_context *rpc = (struct rdmare_private_context *) ctx->io_private_context;
	int idx = 0;

	// struct ibv_env *env;
	// env = &rpc->env[idx];

	if (rpc->send_pktnum[idx] >= MAX_SEND_BURST)
		rdmare_send_pkts(ctx, nif);
	
	int pkt_index = (rpc->send_headidx[idx] + rpc->send_pktnum[idx]) % MAX_SEND_BURST;
	rpc->send_pktsize[idx][pkt_index] = len;
	rpc->send_pktnum[idx]++;
	// printf("get_wptr(): pkt_index=%d is used, len=%d\n", pkt_index, len);
	return (uint8_t *) rpc->send_pktbuf[idx] + ENTRY_SIZE*pkt_index;

// 	while (rpc->send_pkt_size[idx] != 0) {
// 		rdmare_send_pkts(ctx, nif);
// 		// printf(">>>>J: rdmare_get_wptr(): send_pkt_size is %d\n", rpc->send_pkt_size);
// 		// exit(EXIT_FAILURE);
// 	}

// 	rpc->send_pkt_size[idx] = len;
// 	return (uint8_t *) rpc->send_pktbuf[idx];
}

/*----------------------------------------------------------------------------*/

/*
 * Receive batch of packets from the interface, ifidx.
 * Returns no. of packets that are read from the iface.
 */
int32_t
rdmare_recv_pkts(struct mtcp_thread_context *ctx, int ifidx)
{
	// printf(">>>>J: rdmare_recv_pkts() is called!\n");

	struct rdmare_private_context *rpc;
	struct ibv_env *env;
	// struct ibv_wc *wc;
	int msgs_completed, i;
	int idx = 0;

	rpc = (struct rdmare_private_context *) ctx->io_private_context;
	env = &rpc->env[idx];

	// memset(&wc, 0, sizeof(wc));
	/* poll for completion */
	msgs_completed = ibv_poll_cq(env->recv_cq, MAX_RECV_BURST, env->recv_wc);
	if (msgs_completed < 0) {
		TRACE_ERROR("Polling error\n");
		exit(EXIT_FAILURE);
	}

	// if (msgs_completed > 0)
	// 	printf("msgs_completed=%d\n", msgs_completed);

	struct ibv_recv_wr *recv_bad_wr;
	for (i = 0; i < msgs_completed; i++) {
		// printf("Receive: message %ld, status is %d, size %d\n", wc.wr_id, wc.status, wc.byte_len);
		rpc->recv_pktsize[idx][env->recv_wc[i].wr_id] = env->recv_wc[i].byte_len;
		// rpc->recv_pktidx[idx] = wc.wr_id;
		
		// env->recv_sge.addr = (uint64_t)rpc->recv_pktbuf[idx] + ENTRY_SIZE*wc.wr_id;
		// env->recv_wr.wr_id = wc.wr_id;
		ibv_post_recv(env->qp, &env->recv_wr[env->recv_wc[i].wr_id], &recv_bad_wr);
	}
	// else {
	// 	printf(">>>>J: msgs_completed = 0 in rdmare_recv_pkts()\n");		
	// }

	// printf(">>>>J: rdmare_recv_pkts() returns!\n");
	return msgs_completed;
}

/*----------------------------------------------------------------------------*/

/*
 * Retrieve next pkt for application for packet read.
 * Returns ptr to pkt buffer.
 */
uint8_t *
rdmare_get_rptr(struct mtcp_thread_context *ctx, int ifidx, int index, uint16_t *len)
{
	struct rdmare_private_context *rpc = (struct rdmare_private_context *) ctx->io_private_context;
	// struct ibv_env *env;
	int idx = 0;
	int headidx = rpc->recv_headidx[idx];
	// *len = rpc->recv_pkt_size[index];
	*len = rpc->recv_pktsize[idx][headidx];
	// printf("get_rptr(): headidx=%d is used, len=%d\n", headidx, *len);
	rpc->recv_headidx[idx] = (headidx + 1) % MAX_RECV_BURST;
	return ((uint8_t *) rpc->recv_pktbuf[idx]) + ENTRY_SIZE*headidx;
}

/*----------------------------------------------------------------------------*/

/* For blocking I/O */
int32_t
rdmare_select(struct mtcp_thread_context *ctx)
{


	return 0;
}

/*----------------------------------------------------------------------------*/

/*
 * Free up resources allocated during init_handle().
 * Normally called during process termination.
 */
void
rdmare_destroy_handle(struct mtcp_thread_context *ctx)
{
	struct rdmare_private_context *rpc;
	int ib_idx, ret;
	struct ibv_env *env;

	rpc = (struct rdmare_private_context *) ctx->io_private_context;

	for (ib_idx = 0; ib_idx < rpc->num_ibv_devices; ib_idx++) {
		env = &rpc->env[ib_idx];

		free(env->recv_wc);
		free(env->send_sge);
		free(env->send_wr);
		free(env->recv_sge);
		free(env->recv_wr);

		ret = ibv_destroy_flow(env->eth_flow);
		ret = ibv_dereg_mr(env->send_mr);
		ret = ibv_dereg_mr(env->recv_mr);
		ret = ibv_destroy_qp(env->qp);
		ret = ibv_destroy_cq(env->send_cq);
		ret = ibv_destroy_cq(env->recv_cq);
		ret = ibv_dealloc_pd(env->pd);
		ret = ibv_close_device(env->context);
		free(rpc->send_pktbuf[ib_idx]);
		free(rpc->recv_pktbuf[ib_idx]);

		printf("rdmare_destroy_handle(): ret=%d\n", ret);
	}
	
	free(rpc);
}

/*----------------------------------------------------------------------------*/
io_module_func rdmare_module_func = {
	.load_module		   = rdmare_load_module,
	.init_handle		   = rdmare_init_handle,
	.link_devices		   = rdmare_link_devices,
	.release_pkt		   = rdmare_release_pkt,
	.send_pkts		   = rdmare_send_pkts,
	.get_wptr   		   = rdmare_get_wptr,
	.recv_pkts		   = rdmare_recv_pkts,
	.get_rptr	   	   = rdmare_get_rptr,
	.select			   = rdmare_select,
	.destroy_handle		   = rdmare_destroy_handle,
	.dev_ioctl		   = NULL
};
/*----------------------------------------------------------------------------*/
#else
io_module_func rdmare_module_func = {
	.load_module		   = NULL,
	.init_handle		   = NULL,
	.link_devices		   = NULL,
	.release_pkt		   = NULL,
	.send_pkts		   = NULL,
	.get_wptr   		   = NULL,
	.recv_pkts		   = NULL,
	.get_rptr	   	   = NULL,
	.select			   = NULL,
	.destroy_handle		   = NULL,
	.dev_ioctl		   = NULL
};
/*----------------------------------------------------------------------------*/
#endif /* !DISABLE_RDMARE */