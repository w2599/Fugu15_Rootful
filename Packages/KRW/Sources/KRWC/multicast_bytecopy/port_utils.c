#include "port_utils.h"

#include <mach/mach.h>
#include <stdlib.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations" // mach_port_destroy

mach_port_t mcbc_port_new(void)
{
    mach_port_options_t options = { .flags = MPO_INSERT_SEND_RIGHT };
    mach_port_t port;
    
    mach_port_construct(mach_task_self(), &options, 0, &port);
    
    return port;
}

void mcbc_port_destroy(mach_port_t p)
{
    mach_port_destroy(mach_task_self(), p);
}

void mcbc_port_deallocate(mach_port_t p)
{
    mach_port_deallocate(mach_task_self(), p);
}

void mcbc_port_destroy_n(mach_port_t *p, unsigned int count)
{
    for (int i = 0; i < count; ++i)
    {
        mach_port_destroy(mach_task_self(), p[i]);
        p[i] = 0;
    }
}

void mcbc_port_deallocate_n(mach_port_t *p, unsigned int count)
{
    for (int i = 0; i < count; ++i)
    {
        mach_port_deallocate(mach_task_self(), p[i]);
    }
}

int mcbc_port_has_msg(mach_port_t p)
{
    mach_msg_header_t msg = { 0 };

    mach_msg(&msg, MACH_RCV_LARGE | MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, 0x10, p, 0, 0);

    return msg.msgh_size;
}

int mcbc_port_peek_trailer_size(mach_port_t p)
{
    mach_port_seqno_t msg_seqno = 0;
    mach_msg_size_t msg_size = 0;
    mach_msg_id_t msg_id = 0;
    mach_msg_trailer_t msg_trailer;
    mach_msg_type_number_t msg_trailer_size = sizeof(msg_trailer);
    
    mach_port_peek(mach_task_self(),
                                  p,
                                  MACH_RCV_TRAILER_NULL,
                                  &msg_seqno,
                                  &msg_size,
                                  &msg_id,
                                  (mach_msg_trailer_info_t)&msg_trailer,
                                  &msg_trailer_size);

    return msg_trailer.msgh_trailer_size;
}

void mcbc_port_receive_msg(mach_port_t p, uint8_t *buf, unsigned int n)
{
    mach_msg((mach_msg_header_t *)buf,
              MACH_RCV_MSG | MACH_MSG_TIMEOUT_NONE,
              0,
              n,
              p,
              0,
              0);
}

void mcbc_port_receive_msg_n(mach_port_t *p, unsigned int count)
{
    uint8_t buf[0x1000];
    
    for (int i = 0; i < count; ++i)
    {
        mcbc_port_receive_msg(p[i], buf, 8);
    }
}

void mcbc_port_receive_msg_all_n(mach_port_t *p, unsigned int count)
{
    uint8_t buf[0x1000];
    
    for (int i = 0; i < count; ++i)
    {
        do
        {
            mcbc_port_receive_msg(p[i], buf, 8);
        } while (mcbc_port_has_msg(p[i]));
    }
}

void mcbc_port_receive_msg_and_deallocate_n(mach_port_t *p, unsigned int count)
{
    mcbc_port_receive_msg_n(p, count);
    mcbc_port_deallocate_n(p, count);
    for (int i = 0; i < count; ++i)
    {
        p[i] = MACH_PORT_NULL;
    }
}
