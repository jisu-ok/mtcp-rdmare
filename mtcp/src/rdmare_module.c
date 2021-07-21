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

#define MAX_PKT_BURST			64

#define PORT_NUM 1

#define ENTRY_SIZE 9000 /* maximum size of each send buffer */
#define SQ_NUM_DESC 512 /* maximum number of sends waiting for completion */
#define RQ_NUM_DESC 512 /* The maximum receive ring length without processing */

#define MAC_SANCTUARY {0x24, 0x8A, 0x07, 0x91, 0x69, 0xA0}
#define MAC_MOSCOW {0x24, 0x8A, 0x07, 0x91, 0x68, 0xE8}

struct rdmare_private_context {
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct ibv_cq *send_cq;
	struct ibv_cq *recv_cq;
	struct ibv_qp *qp;
	struct ibv_mr *send_mr;
	struct ibv_mr *recv_mr;
	struct ibv_sge send_sg_entry;
	struct ibv_sge recv_sg_entry;
	struct ibv_send_wr send_wr;
	struct ibv_send_wr *send_bad_wr;
	struct ibv_recv_wr recv_wr;
	struct ibv_recv_wr *recv_bad_wr;
	void *send_pktbuf;
	uint16_t send_pkt_size;
	// void *recv_pktbuf[MAX_PKT_BURST];
	void *recv_pktbuf;
	uint16_t recv_pkt_size[RQ_NUM_DESC];
	uint64_t send_n;
	uint64_t recv_n;
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
		TRACE_ERROR("Failed to initialize ctx->io_private_context: "
				"Can't allocate memory\n");
		exit(EXIT_FAILURE);
	}

	rpc = (struct rdmare_private_context *)ctx->io_private_context;

	/* Initialize default values */
	memset(&rpc->send_sg_entry, 0, sizeof(rpc->send_sg_entry));
	memset(&rpc->recv_sg_entry, 0, sizeof(rpc->recv_sg_entry));
	memset(&rpc->send_wr, 0, sizeof(rpc->send_wr));
	memset(&rpc->recv_wr, 0, sizeof(rpc->recv_wr));
	rpc->send_pkt_size = 0;
	for (int i = 0; i < RQ_NUM_DESC; i++)
		rpc->recv_pkt_size[i] = 0;
	rpc->send_n = 0;
	rpc->recv_n = 0;

	/* For now, assume there is only one ibv device in the host */
	struct ibv_device **dev_list;
	struct ibv_device *ib_dev;
	dev_list = ibv_get_device_list(NULL); // assume this does not return NULL. Maybe already tested in SetNetEnv()
	ib_dev = dev_list[0]; // // assume this does not return NULL. Maybe already tested in SetNetEnv()

	/* Get the device context */
	rpc->context = ibv_open_device(ib_dev);
	if (!rpc->context) {
		TRACE_ERROR("Couldn't get context for %s\n", ibv_get_device_name(ib_dev));
		exit(EXIT_FAILURE);
	}
	printf(">>>>J: 2. Get the device context (rdmare_init_handle())\n");

	/* Allocate Protection Domain */
	rpc->pd = ibv_alloc_pd(rpc->context);
	if (!rpc->pd) {
		TRACE_ERROR("Couldn't allocate PD\n");
		exit(EXIT_FAILURE);
	}
	printf(">>>>J: 3. Allocate PD (rdmare_init_handle())\n");

	/* Create Completion Queue (CQ) */
	rpc->send_cq = ibv_create_cq(rpc->context, SQ_NUM_DESC, NULL, NULL, 0);
	if (!rpc->send_cq) {
		TRACE_ERROR("Couldn't create send CQ\n");
		exit(EXIT_FAILURE);
	}

	rpc->recv_cq = ibv_create_cq(rpc->context, RQ_NUM_DESC, NULL, NULL, 0);
	if (!rpc->recv_cq) {
		TRACE_ERROR("Couldn't create recv QP\n");
		exit(EXIT_FAILURE);
	}
	printf(">>>>J: 4. Create CQ (rdmare_init_handle())\n");

	/* Initialize QP */
	struct ibv_qp_init_attr qp_init_attr = {
		.qp_context = NULL,
		.send_cq = rpc->send_cq,
		.recv_cq = rpc->recv_cq,
		.cap = {
			.max_send_wr = SQ_NUM_DESC,
			.max_send_sge = 1,
			.max_recv_wr = RQ_NUM_DESC,
			.max_recv_sge = 1,
			.max_inline_data = 512,
		},
		.qp_type = IBV_QPT_RAW_PACKET
	};
	printf(">>>>J: 5. Initialize QP (rdmare_init_handle())\n");


	/* Create Queue Pair (QP) */
	rpc->qp = ibv_create_qp(rpc->pd, &qp_init_attr);
	if (!rpc->qp) {
		TRACE_ERROR("Couldn't create RSS QP\n");
		exit(EXIT_FAILURE);
	}
	printf(">>>>J: 6. Create QP (rdmare_init_handle())\n");

	/* Initialize the QP (receive ring) and assign a port */
	struct ibv_qp_attr qp_attr;
	int qp_flags;
	memset(&qp_attr, 0, sizeof(qp_attr));

	qp_flags = IBV_QP_STATE | IBV_QP_PORT;
	qp_attr.qp_state = IBV_QPS_INIT;
	qp_attr.port_num = 1;
	int ret = ibv_modify_qp(rpc->qp, &qp_attr, qp_flags);
	if (ret < 0) {
		TRACE_ERROR("Failed modify QP to init\n");
		exit(EXIT_FAILURE);
	}
	memset(&qp_attr, 0, sizeof(qp_attr));
	printf(">>>>J: 7. Initialize QP (receive ring) and assign a port (rdmare_init_handle())\n");

	/* Move the ring to ready-to-receive and then ready-to-send */
	qp_flags = IBV_QP_STATE;
	qp_attr.qp_state = IBV_QPS_RTR;
	ret = ibv_modify_qp(rpc->qp, &qp_attr, qp_flags);
	if (ret < 0) {
		TRACE_ERROR("Failed modify QP to receive\n");
		exit(EXIT_FAILURE);
	}

	qp_flags = IBV_QP_STATE;
	qp_attr.qp_state = IBV_QPS_RTS;
	ret = ibv_modify_qp(rpc->qp, &qp_attr, qp_flags);
	if (ret < 0) {
		TRACE_ERROR("Failed modify QP to send\n");
		exit(EXIT_FAILURE);
	}
	printf(">>>>J: 8. Move the ring to RTS (rdmare_init_handle())\n");

	/* Allocate memory for send/receive packet buffer */
	int send_bufsize = ENTRY_SIZE*SQ_NUM_DESC; /* maximum size of data to be access directly by HW */
	rpc->send_pktbuf = malloc(send_bufsize);
	memset(rpc->send_pktbuf, 0, send_bufsize); // optional?
	if (!rpc->send_pktbuf) {
		TRACE_ERROR("Couldn't allocate memory for send packet buffer\n");
		exit(EXIT_FAILURE);
	}

	int recv_bufsize = ENTRY_SIZE*RQ_NUM_DESC;
	rpc->recv_pktbuf = malloc(recv_bufsize);
	memset(rpc->recv_pktbuf, 0, recv_bufsize); //optional?
	if (!rpc->recv_pktbuf) {
		TRACE_ERROR("Couldn't allocate memory for recv packet buffer\n");
		exit(EXIT_FAILURE);
	}
	printf(">>>>J: 9. Allocate memory for pkt buffer (rdmare_init_handle())\n");

	/* Register the user memory so it can be accessed by the HW directly */
	rpc->send_mr = ibv_reg_mr(rpc->pd, rpc->send_pktbuf, send_bufsize, IBV_ACCESS_LOCAL_WRITE);
	if (!rpc->send_mr) {
		TRACE_ERROR("Couldn't register mr for send buffer\n");
		exit(EXIT_FAILURE);
	}

	rpc->recv_mr = ibv_reg_mr(rpc->pd, rpc->recv_pktbuf, recv_bufsize, IBV_ACCESS_LOCAL_WRITE);
	if (!rpc->recv_mr) {
		TRACE_ERROR("Couldn't register mr for recv buffer\n");
		exit(EXIT_FAILURE);
	}
	printf(">>>>J: 10. Register MR (rdmare_init_handle())\n");



	/* Some initialization for send */
	rpc->send_sg_entry.lkey = rpc->send_mr->lkey;

	rpc->send_wr.num_sge = 1;
	rpc->send_wr.sg_list = &rpc->send_sg_entry;
	rpc->send_wr.next = NULL;
	rpc->send_wr.opcode = IBV_WR_SEND;
#ifdef ENABLE_SEND_INLINE
	rpc->send_wr.send_flags = IBV_SEND_INLINE;
#endif
	rpc->send_wr.send_flags = IBV_SEND_SIGNALED;


	/* Some initialization for receive - attach all buffers to the ring (?) */
	rpc->recv_sg_entry.length = ENTRY_SIZE;
	rpc->recv_sg_entry.lkey = rpc->recv_mr->lkey;

	rpc->recv_wr.num_sge = 1;
	rpc->recv_wr.sg_list = &rpc->recv_sg_entry;
	rpc->recv_wr.next = NULL;

	for (rpc->recv_n = 0; rpc->recv_n < RQ_NUM_DESC; rpc->recv_n++) {
		rpc->recv_sg_entry.addr = (uint64_t)rpc->recv_pktbuf + ENTRY_SIZE*rpc->recv_n;
		rpc->recv_wr.wr_id = rpc->recv_n;
		ibv_post_recv(rpc->qp, &rpc->recv_wr, &rpc->recv_bad_wr);
	}
	printf(">>>>J: 11. Attach all buffers to the ring (rdmare_init_handle())\n");


	/* Steering rule to intercept packet to DEST_MAC and place packet in ring pointed by ->qp */
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

	/* Alternative way */
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

	/* Create (register) steering rule to QP */
	struct ibv_flow *eth_flow;
	// eth_flow = ibv_create_flow(rpc->qp, &attr);
	eth_flow = ibv_create_flow(rpc->qp, &flow_attr.attr);
	if (!eth_flow) {
		TRACE_ERROR("Couldn't attach steering flow\n");
		exit(EXIT_FAILURE);
	}
	printf(">>>>J: 12. Registerd flow steering rule (rdmare_init_handle())\n");





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
	struct ibv_wc wc;
	int ret, msgs_completed, pkt_size;

	rpc = (struct rdmare_private_context *) ctx->io_private_context;
	pkt_size = rpc->send_pkt_size;

	if (pkt_size == 0)
		return 0;

	rpc->send_sg_entry.addr = (uint64_t) rpc->send_pktbuf;
	rpc->send_sg_entry.length = rpc->send_pkt_size;
	// rpc->send_sg_entry.lkey = rpc->send_mr->lkey;

	// rpc->send_wr.num_sge = 1;
	// rpc->send_wr.sg_list = &rpc->send_sg_entry;
	// rpc->send_wr.next = NULL;
	// rpc->send_wr.opcode = IBV_WR_SEND;
	// wr.send_flags = IBV_SEND_INLINE
	// rpc->send_wr.send_flags = IBV_SEND_SIGNALED;
	rpc->send_wr.wr_id = rpc->send_n;

	/* push descriptor to hardware */
	ret = ibv_post_send(rpc->qp, &rpc->send_wr, &rpc->send_bad_wr);
	if (ret < 0) {
		TRACE_ERROR("Failed in post send\n");
		exit(EXIT_FAILURE);
	}
	rpc->send_n++;

	// printf(">>>>J: n is %d\n", rpc->n);
	// printf(">>>>J: After ibv_post_send() (rdmare_send_pkts())\n");

	/* poll for completion */
	msgs_completed = ibv_poll_cq(rpc->send_cq, 1, &wc);
	if (msgs_completed > 0) {
		printf("Send: message %ld, size %d\n", wc.wr_id, wc.byte_len);
	}
	else if (msgs_completed < 0) {
		TRACE_ERROR("Polling error\n");
		exit(EXIT_FAILURE);
	}

	rpc->send_pkt_size = 0;

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
	struct rdmare_private_context *rpc = (struct rdmare_private_context *) ctx->io_private_context;

	while (rpc->send_pkt_size != 0) {}

	rpc->send_pkt_size = len;
	return (uint8_t *) rpc->send_pktbuf;
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
	struct ibv_wc wc;
	int msgs_completed;

	rpc = (struct rdmare_private_context *) ctx->io_private_context;

	/* poll for completion */
	msgs_completed = ibv_poll_cq(rpc->recv_cq, 1, &wc);
	if (msgs_completed > 0) {
		printf("Receive: message %ld, size %d\n", wc.wr_id, wc.byte_len);
		rpc->recv_pkt_size[wc.wr_id] = wc.byte_len;
		
		rpc->recv_sg_entry.addr = (uint64_t)rpc->recv_pktbuf + ENTRY_SIZE*wc.wr_id;
		rpc->recv_wr.wr_id = wc.wr_id;
		ibv_post_recv(rpc->qp, &rpc->recv_wr, &rpc->recv_bad_wr);
	}
	else if (msgs_completed < 0) {
		// printf(">>>>J: msgs_completed < 0 in rdmare_recv_pkts()\n");
		TRACE_ERROR("Polling error\n");
		exit(EXIT_FAILURE);
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

	*len = rpc->recv_pkt_size[index];
	return ((uint8_t *) rpc->recv_pktbuf) + ENTRY_SIZE*index;
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