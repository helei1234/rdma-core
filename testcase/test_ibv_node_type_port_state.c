#include <config.h>
#include <stdio.h>
#include <endian.h>
#include <infiniband/verbs.h>

int main(int argc, char *argv[])
{
    ibv_node_type_str(IBV_NODE_CA);
    ibv_node_type_str(IBV_NODE_SWITCH);
    ibv_node_type_str(IBV_NODE_ROUTER);
    ibv_node_type_str(IBV_NODE_RNIC);
    ibv_node_type_str(IBV_NODE_USNIC);
    ibv_node_type_str(IBV_NODE_USNIC_UDP);
    ibv_node_type_str(IBV_NODE_UNSPECIFIED);
    ibv_port_state_str(IBV_PORT_NOP);
    ibv_port_state_str(IBV_PORT_DOWN);
    ibv_port_state_str(IBV_PORT_INIT);
    ibv_port_state_str(IBV_PORT_ARMED);
    ibv_port_state_str(IBV_PORT_ACTIVE);
    ibv_port_state_str(IBV_PORT_ACTIVE_DEFER);
    return 0;
}