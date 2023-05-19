#define _GNU_SOURCE
#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <inttypes.h>

#include "pingpong.h"

static int page_size;
//这个page_size是后面用来给ctx分配buff的，需要分配已页为单位的buff
//后面可以考察下，页buff的对齐是否会对其性能的影响

struct pingpong_context {
	struct ibv_context	*context;
	struct ibv_pd		*pd;
	struct ibv_mr		*mr;
	struct ibv_dm		*dm;
	union {
		struct ibv_cq		*cq;
		struct ibv_cq_ex	*cq_ex;
	} cq_s;
	struct ibv_qp		*qp;
	char			*buf;
	int			 size;
	int			 send_flags;
	int			 rx_depth;
	int			 pending;
	struct ibv_port_attr     portinfo;
};
//最主要的结构体，将ctx,pd,mr/dm,cq,qp,buff等一些重要的属性都包含在内

struct pingpong_dest {
	int lid;
	int qpn;
	int psn;
	union ibv_gid gid;
};
//发送所需要的结构体lid,qpn,psn,gid
//注意这个gid

enum {
	PINGPONG_RECV_WRID = 1,
	PINGPONG_SEND_WRID = 2,
};
//两种发射模式，这里是自己定义的

static int pp_connnect_ctx(struct pingpong_context *ctx, int port, int my_psn, 
					struct pingpong_dest *dest, int sgid_idx)
//这个函数主要是在配置qp,应在创建qp之后
{
	struct ibv_qp_attr attr = {
		.qp_state = IBV_QPS_RTR,
		.path_mtu = IBV_MTU_1024,
		.dest_qp_num = dest->qpn,
		.rq_psn = dest->psn,
		.max_dest_rd_atomic = 1,
		.min_rnr_timer = 12,
		.ah_attr = {
			.is_global = 0,
			.dlid = dest->lid,
			.sl = 0,
			.src_path_bits = 0,
			.port_num = port
		}
	};
	//qp的属性配置
	if (dest->gid.global.interface_id){
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = sgid_idx;		
	}
	//

	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state	    = IBV_QPS_RTS;
	attr.timeout	    = 14;
	attr.retry_cnt	    = 7;
	attr.rnr_retry	    = 7;
	attr.sq_psn	    = my_psn;
	attr.max_rd_atomic  = 1;
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_TIMEOUT            |
			  IBV_QP_RETRY_CNT          |
			  IBV_QP_RNR_RETRY          |
			  IBV_QP_SQ_PSN             |
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}

static struct pingpong_context *init_ctx(struct ibv_device *ib_dev, int size,
					    int rx_depth, int port)
{
	struct pingpong_context *ctx;
	struct ibv_device_attr_ex attrx;
	int access_flags = IBV_ACCESS_LOCAL_WRITE;
	ctx = calloc(1, sizeof *ctx);
	if (!ctx){
		return NULL;
	}
	ctx->size = size;
	ctx->send_flags = IBV_SEND_SIGNALED;
	ctx->rx_depth = rx_depth;
	ctx->buf = memalign(page_size, size);

	
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		goto clean_ctx;
	}
	memset(ctx->buf, 0x7b, size);
	
	ctx->context =  ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
		goto clean_buffer;
	}
	

	ctx->pd = ibv_alloc_pd(ctx->context);

	
	if (ibv_query_device_ex(ctx->context, NULL, &attrx)) {
		fprintf(stderr, "Couldn't query device for its features\n");
		goto clean_pd;
	}
	
	//无视三个选项，use_odp，use_ts，use_dm	
	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, access_flags);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't register MR\n");
		goto clean_mr;
	}
	
	ctx->cq_s.cq =  ibv_create_cq(ctx->context, rx_depth+1, NULL, NULL, 0);
	if (!ctx->cq_s.cq){
		fprintf(stderr, "Couldn't create CQ\n");
		goto clean_mr;		
	}
	{
		//配置qp的初始化属性
		struct ibv_qp_attr attr;
		struct ibv_qp_init_attr init_attr = {
			.send_cq = ctx->cq_s.cq,
			.recv_cq = ctx->cq_s.cq,
			.cap = {
				.max_send_wr  = 1,
				.max_recv_wr  = rx_depth,
				.max_send_sge = 1,
				.max_recv_sge = 1				
			},
			//设置qp的类型即传输模式
			.qp_type = IBV_QPT_RC
		};
		ctx->qp = ibv_create_qp(ctx->pd, &init_attr);
		if (!ctx->qp){
			fprintf(stderr, "Couldn't create QP\n");
			goto clean_cq;
		}
		ibv_query_qp(ctx->qp, &attr, IBV_QP_CAP, &init_attr);
	}
	
	{
		struct ibv_qp_attr attr = {
			.qp_state = IBV_QPS_INIT,
			.pkey_index = 0,
			.port_num = port,
			.qp_access_flags = 0
		};
		if (ibv_modify_qp(ctx->qp, &attr, 
				IBV_QP_STATE |
				IBV_QP_PKEY_INDEX |
				IBV_QP_PORT |
				IBV_QP_ACCESS_FLAGS)){
			fprintf(stderr, "Failed to modify QP to INIT\n");
			goto clean_qp;			
				}
	}
	printf("we have init ctx successfully\n");
	
	return ctx;
clean_qp:
	ibv_destroy_qp(ctx->qp);
clean_cq:
	ibv_destroy_cq(ctx->cq_s.cq);
clean_mr:
	ibv_dereg_mr(ctx->mr);
clean_pd:
	ibv_dealloc_pd(ctx->pd);
clean_buffer:
	free(ctx->buf);
clean_ctx:
	free(ctx);
	return NULL;

}

static struct pingpong_dest *pp_client_exch_dest(const char *servername, int port, 
											const struct pingpong_dest *my_dest)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];

	if (asprintf(&service, "%d", port) < 0)
		return NULL;

	n = getaddrinfo(servername, service, &hints, &res);
	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return NULL;
	}

	gid_to_wire_gid(&my_dest->gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
							my_dest->psn, gid);
	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}

	if (read(sockfd, msg, sizeof msg) != sizeof msg ||
	    write(sockfd, "done", sizeof "done") != sizeof "done") {
		perror("client read/write");
		fprintf(stderr, "Couldn't read/write remote address\n");
		goto out;
	}

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
						&rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

out:
	close(sockfd);
	return rem_dest;
}

static int pp_post_send(struct pingpong_context *ctx)
{
	struct ibv_sge list = {
		.addr = (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey = ctx->mr->lkey
	};
	struct ibv_send_wr wr = {
		.wr_id = PINGPONG_SEND_WRID,
		.sg_list = &list,
		.num_sge = 1,
		.opcode = IBV_WR_SEND,
		.send_flags = ctx->send_flags
	};
	struct ibv_send_wr *bad_wr;

	return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

static int pp_post_recv(struct pingpong_context *ctx, int n)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_recv_wr wr = {
		.wr_id	    = PINGPONG_RECV_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;
	int i;

	for (i = 0; i < n; ++i)
		if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
			break;

	return i;
}

static int close_ctx(struct pingpong_context *ctx)
{
    if(ibv_destroy_qp(ctx->qp)){
		fprintf(stderr, "Couldn't destroy QP\n");
		return 1;
	}
    if(ibv_destroy_cq(ctx->cq_s.cq)){
		fprintf(stderr, "Couldn't destroy CQ\n");
		return 1;
	}
    if(ibv_dereg_mr(ctx->mr)){
		fprintf(stderr, "Couldn't deregister MR\n");
		return 1;
	}
    if(ibv_dealloc_pd(ctx->pd)){
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}
    if(ibv_close_device(ctx->context)){
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}
	free(ctx->buf);
	free(ctx);
	return 0;

}

static inline int parse_singe_wc(struct pingpong_context *ctx, int *scnt, int *rcnt, int *routs, int iters,
								uint64_t wr_id, enum ibv_wc_status status)
{
	if (status != IBV_WC_SUCCESS) {
		fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
			ibv_wc_status_str(status),
			status, (int)wr_id);
		return 1;
	}
	switch ((int)wr_id)
	{
	case PINGPONG_SEND_WRID:
		// printf("PINGPONG_SEND_WRID\n");
		++(*scnt);
		break;

	case PINGPONG_RECV_WRID:
		// printf("PINGPONG_RECV_WRID\n");
		if (--(*routs) <= 1) {
			*routs += pp_post_recv(ctx, ctx->rx_depth - *routs);
			if (*routs < ctx->rx_depth) {
				fprintf(stderr,
					"Couldn't post receive (%d)\n",
					*routs);
				return 1;
			}
		}
		++(*rcnt);
		break;		
	default:
		fprintf(stderr, "Completion for unknown wr_id %d\n",
			(int)wr_id);
		return 1;
	}
	
	ctx->pending &= ~(int)wr_id;
	if (*scnt < iters && !ctx->pending) {
		if (pp_post_send(ctx)) {
			fprintf(stderr, "Couldn't post send\n");
			return 1;
		}
		ctx->pending = PINGPONG_RECV_WRID |
			PINGPONG_SEND_WRID;
	}
	return 0;	
}

int main(void)
{
	int size = 4096;
	int num;
	int routs;
	int port = 1;
	int rcnt, scnt;
	int iters = 1000;
	struct timeval start, end;
	unsigned int rx_depth = 500;
	struct ibv_device **dev_list;
	struct ibv_device *dev;
	const char *ib_devname = "mlx5_0";
	struct pingpong_context *ctx;
	struct pingpong_dest my_dest;
	struct pingpong_dest *rem_dest;
	const char *servername = "172.17.29.154";
	int gidx = -1;
	char gid[33];

	srand48(getpid() * time(NULL));
	page_size = sysconf(_SC_PAGESIZE);

	dev_list = ibv_get_device_list(&num);
	printf("we find %d ib device\n", num);

	if(!dev_list){
		perror("Failed to get IB devices list");
		return 1;
	}

	if (!ib_devname) {
		dev = dev_list[0];
		if (!dev) {
			fprintf(stderr, "No IB devices found\n");
			return 1;
		}
	} else {
		int i;
		for (i = 0; dev_list[i]; ++i)
			if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
				break;
		dev = dev_list[i];
		printf("get the ib device %s\n", ibv_get_device_name(dev));
		if (!dev) {
			fprintf(stderr, "IB device %s not found\n", ib_devname);
			return 1;
		}
	}

	ctx = init_ctx(dev, size, rx_depth, port);
	if(!ctx)
		return 1;
	
	routs = pp_post_recv(ctx, ctx->rx_depth);
	if (routs < ctx->rx_depth){
		fprintf(stderr, "Couldn't post receive (%d)\n", routs);
		return 1;
	}


	if(pp_get_port_info(ctx->context, port, &ctx->portinfo)){
		fprintf(stderr, "Couldn't get port info\n");
		return 1;		
	}
	printf("port:\t%u\n", port);	
	printf("port_lid:\t%d\n", ctx->portinfo.lid);
	

	my_dest.lid = ctx->portinfo.lid;
	if (ctx->portinfo.link_layer != IBV_LINK_LAYER_ETHERNET && !my_dest.lid){
		fprintf(stderr, "Couldn't get local LID\n");
		return 1;
	}
	if (ibv_query_gid(ctx->context, 1, 0, &my_dest.gid)) {
		fprintf(stderr, "can't read sgid of index %d\n", gidx);
		return 1;		
	}
	

	memset(&my_dest.gid, 0, sizeof my_dest.gid);
	my_dest.qpn = ctx->qp->qp_num;
	my_dest.psn = lrand48() & 0xffffff;
	inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);
	printf("local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       my_dest.lid, my_dest.qpn, my_dest.psn, gid);	
	
	rem_dest = pp_client_exch_dest(servername, 18515, &my_dest);
	if (!rem_dest)
		return 1;
	inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
	printf("remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);	
	
	if(pp_connnect_ctx(ctx, port, my_dest.psn, rem_dest, gidx))
		return 1;

	ctx->pending = PINGPONG_RECV_WRID;

	if (pp_post_send(ctx)) {
		fprintf(stderr, "Couldn't post send");
		return 1;
	}
	ctx->pending |= PINGPONG_SEND_WRID;

	if (gettimeofday(&start, NULL)) {
		perror("gettimeofday");
		return 1;
	}	

	rcnt = scnt = 0;
	while (rcnt < iters || scnt < iters)
	{
		int ne, ret;
		struct ibv_wc wc[2];
		do {
			ne = ibv_poll_cq(ctx->cq_s.cq, 2, wc);
			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
			// printf("%d", ne);
		} while (ne < 1);

		for(int i=0; i < ne; ++i){
			ret = parse_singe_wc(ctx, &scnt, &rcnt, &routs, iters, wc[i].wr_id, wc[i].status);
			if (ret) {
			fprintf(stderr, "parse WC failed %d\n", ne);
			return 1;
		}
		}
	}
	if (gettimeofday(&end, NULL)) {
		perror("gettimeofday");
		return 1;
	}	

	float usec = (end.tv_sec - start.tv_sec) * 1000000 +
		(end.tv_usec - start.tv_usec);
	long long bytes = (long long) size * iters * 2;

	printf("%lld bytes in %.2f seconds = %.2f Mbit/sec\n",
			bytes, usec / 1000000., bytes * 8. / usec);
	printf("%d iters in %.2f seconds = %.2f usec/iter\n",
			iters, usec / 1000000., usec / iters);	
	
	ibv_ack_cq_events(ctx->cq_s.cq, 0);
	if (close_ctx(ctx))
		return 1;
	ibv_free_device_list(dev_list);
	free(rem_dest);
    return 0;
}