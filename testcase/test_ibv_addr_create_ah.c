#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

struct pp_context {
    struct ibv_device *dev;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_port_attr port_attr;
};

static int verify_state(struct ibv_context *ctx, int port_num)
{
    int ret;
    struct ibv_port_attr attr;
    ret = ibv_query_port(ctx, port_num, &attr);
    if(ret){
        fprintf(stderr, "ibv_query_port attr failed");
        return 1;
    }
    if (attr.state == IBV_PORT_ACTIVE || attr.state == IBV_PORT_INIT) {
        return 0;
    }else{
        return 1;
    }
}

int main(void)
{
    int num, ret;
    struct pp_context *context;
    struct ibv_device **dev_list;
    struct ibv_ah_attr ah_attr;
    struct ibv_ah *ah;
    const char *dev_name = "mlx5_0";
    const uint32_t ib_port = 1;
    union ibv_gid my_gid;

    context = calloc(1, sizeof *context);
    dev_list = ibv_get_device_list(&num);
    
    if (!dev_name){
        context->dev = dev_list[0];
    }else{
        int i;
        for(i=0;dev_list[i];++i)
            if(!strcmp(ibv_get_device_name(dev_list[i]), dev_name))
                break;
        context->dev = dev_list[i];
    }
    if (!context->dev){
        fprintf(stderr, "No IB device found!\n");
        return 1;
    }

    context->ctx = ibv_open_device(context->dev);
    if(verify_state(context->ctx, ib_port)){
        return 1;
    }
    
    context->pd = ibv_alloc_pd(context->ctx);
    ret = ibv_query_port(context->ctx, ib_port, &context->port_attr);
    if (ret) {
        fprintf(stderr, "Failed to query port %u props\n", ib_port);
        return 1;
    }

    if (ibv_query_gid(context->ctx, ib_port, 0, &my_gid)){
        fprintf(stderr, "could not get gid for port %d, index 0\n", ib_port);
    } else {
        memset(&my_gid, 0, sizeof my_gid);
    }

    ah_attr.grh.hop_limit = 1;
    ah_attr.grh.sgid_index = 0;
    ah_attr.dlid = context->port_attr.lid;
    ah_attr.sl = 0;
    ah_attr.is_global = 1;
    ah_attr.port_num = ib_port;

    ah = ibv_create_ah(context->pd, &ah_attr);
    if(!ah){
        fprintf(stderr, "Failed to create ah!\n");
        return 1;
    }
    ibv_destroy_ah(ah);
    return 0;
        
}