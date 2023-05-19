#include <config.h>
#include <stdio.h>
#include <endian.h>
#include <infiniband/verbs.h>

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list;
    struct ibv_context *dev_cont;
    int num, i;
    dev_list = ibv_get_device_list(&num);
    for(i = 0; i < num; ++i){
        dev_cont = ibv_open_device(dev_list[i]);
        if(!dev_cont){
            printf("Failed to open %-16s\t", ibv_get_device_name(dev_list[i]));
            return 1;
        }else{
            printf("Success to open %-16s\t", ibv_get_device_name(dev_list[i]));
        }
        ibv_close_device(dev_cont);
    }
    return 0;
}