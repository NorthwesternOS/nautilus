/*
 * This file is part of the Nautilus AeroKernel developed
 * by the Hobbes and V3VEE Projects with funding from the
 * United States National Science Foundation and the Department of Energy.
 *
 * The V3VEE Project is a joint project between Northwestern University
 * and the University of New Mexico.  The Hobbes Project is a collaboration
 * led by Sandia National Laboratories that includes several national
 * laboratories and universities. You can find out more at:
 * http://www.v3vee.org  and
 * http://xstack.sandia.gov/hobbes
 *
 * Copyright (c) 2019  Peter Dinda, Alex van der Heijden, Conor Hetland
 * Copyright (c) 2019, The Intereaving Project <http://www.interweaving.org>
 *                     The Hobbes Project <http://xstack.sandia.gov/hobbes>
 * All rights reserved.
 *
 * Authors: Peter Dinda <pdinda@northwestern.edu>
 *          Alex van der Heijden
 *          Conor Hetland
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "LICENSE.txt".
 */

/*
  This is stub code for the CS 343 Driver Lab at Northwestern.

  This driver provides access to the modern virtio GPU interface,
  which numerous virtual machine monitors, including QEMU and KVM
  use to provide an emulated GPU for graphics, or to expose a real
  underlying hardware GPU.

  Virtio is a general mechanism for interfacing with VMM-provided
  devices.  Virtio-PCI is that mechanism instantiated for the PCI bus.
  Virtio-GPU is a driver for GPUs that talk via the PCI instantiation
  of Virtio.

  General specification of Virtio, Virtio-PCI, and Virtio-GPU:

  https://docs.oasis-open.org/virtio/virtio/v1.1/csprd01/virtio-v1.1-csprd01.html

  Note that the documentation for virtio on osdev is for
  the "legacy" version.   The virtio drivers for block and network
  devices (virtio_net.c, virtio_blk.c) in NK are also for the
  "legacy" version.   It is important to note that Virtio-GPU is
  a "modern" device, and so while the concepts are similar, the
  implementation is a bit different.

*/

#include <nautilus/nautilus.h>


// eventually this will be gpudev.h
// - for now we are a generic device
// - one of your tasks for later is to develop an interface
//   and define it in your own gpudev.h
#include <nautilus/dev.h>



#include <nautilus/irq.h>
#include <dev/pci.h>
#include <dev/virtio_gpu.h>

///////////////////////////////////////////////////////////////////
// Wrappers for debug and other output so that
// they can be enabled/disabled at compile time using kernel
// build configuration (Kconfig)
//
#ifndef NAUT_CONFIG_DEBUG_VIRTIO_GPU
#undef DEBUG_PRINT
#define DEBUG_PRINT(fmt, args...)
#endif

#define INFO(fmt, args...) INFO_PRINT("virtio_gpu: " fmt, ##args)
#define DEBUG(fmt, args...) DEBUG_PRINT("virtio_gpu: " fmt, ##args)
#define ERROR(fmt, args...) ERROR_PRINT("virtio_gpu: " fmt, ##args)


///////////////////////////////////////////////////////////////////
// Wrappers for locking the software state of a device
//
#define STATE_LOCK_CONF uint8_t _state_lock_flags
#define STATE_LOCK(state) _state_lock_flags = spin_lock_irq_save(&((state)->lock))
#define STATE_UNLOCK(state) spin_unlock_irq_restore(&(((state)->lock)), _state_lock_flags)

///////////////////////////////////////////////////////////////////
// Macros for manipulating feature bits on virtio pci devices
//
#define FBIT_ISSET(features, bit) ((features) & (0x01 << (bit)))
#define FBIT_SETIF(features_out, features_in, bit)                      \
    if (FBIT_ISSET(features_in,bit)) {                                  \
        features_out |= (0x01 << (bit)) ;                               \
    }

#define DEBUG_FBIT(features, bit)                                       \
    if (FBIT_ISSET(features, bit)) {					\
	DEBUG("feature bit set: %s\n", #bit);				\
    }


///////////////////////////////////////////////////////////////////
// We can support multiple virtio-gpu devices - this variable
// is usd to create an enumeration
//
static uint64_t num_devs = 0;

///////////////////////////////////////////////////////////////////
// The software state of a device
//
struct virtio_gpu_dev {
    struct nk_dev               *gpu_dev;     // we are a generic device
                                              // eventually, we will have a gpu device abstraction
    
    struct virtio_pci_dev       *virtio_dev;  // we are also a virtio pci device

    spinlock_t                   lock;        // we have a lock
};


///////////////////////////////////////////////////////////////////
// This next chunk of code imports the abstractions and data types
// defined in the virtio documentation for this device
//

// They have their own names for various base types
// "le" means "little endian" (x64 native)
#define u8   uint8_t
#define le8  uint8_t
#define le16 uint16_t
#define le32 uint32_t
#define le64 uint64_t

// A virtio GPU may do 3D mode (VIRGL)
// and it may support extended display info (EDID)
// We will do neither
#define VIRTIO_GPU_F_VIRGL 0x1
#define VIRTIO_GPU_F_EDID  0x2

// We can ask the device for statistics
// You do not need to
struct virtio_gpu_config {
    le32 events_read;
    le32 events_clear;
    le32 num_scanouts;
    le32 reserved;
};

// This is very important - it enumerates
// the different requests that we can make of the device
// as well as its valid responses.
enum virtio_gpu_ctrl_type {
    
    /* 2d commands */
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
    VIRTIO_GPU_CMD_RESOURCE_UNREF,
    
    VIRTIO_GPU_CMD_SET_SCANOUT,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,
    VIRTIO_GPU_CMD_GET_CAPSET_INFO,
    VIRTIO_GPU_CMD_GET_CAPSET,
    VIRTIO_GPU_CMD_GET_EDID,
    
    /* cursor commands */
    VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR,
    
    /* success responses */
    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET,
    VIRTIO_GPU_RESP_OK_EDID,
    
    /* error responses */
    VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
};

#define VIRTIO_GPU_FLAG_FENCE (1 << 0)


////////////////////////////////////////////////////////
// All requests and responses include this
// header as their first (and sometimes only) part
struct virtio_gpu_ctrl_hdr {
    le32 type;             // from virtio_gpu_ctrl_type
    le32 flags;            // generally zero
    le64 fence_id;         // memory barrier - you can ignore
    le32 ctx_id;           // zero
    le32 padding;
};


////////////////////////////////////////////////////////
// The following are for the
// VIRTIO_GPU_CMD_GET_DISPLAY_INFO request
// which tells you about attached monitors and their
// capabilities


// "scanout" means monitor
#define VIRTIO_GPU_MAX_SCANOUTS 16

// monitors (and other things are represented 
struct virtio_gpu_rect {
    le32 x;
    le32 y;
    le32 width;
    le32 height;
};


// the request for display information is simply
// a bare struct virtio_gpu_ctrl_hdr

// the response for display information is this
struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;    // contains the return code in type
    struct virtio_gpu_display_one {    // this is a scanout/display
	struct virtio_gpu_rect r;      // width+height and where it is placed in the space
	le32 enabled;                  // is it attached?
	le32 flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS]; // there are up to this many scanouts
};


////////////////////////////////////////////////////////
// The following are for the VIRTIO_GPU_CMD_GET_EDID
// request, which can access extended display information
//

// the request for extended display information (EDID) is
// this.   You will not need this.
struct virtio_gpu_get_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    le32 scanout;
    le32 padding;
};

// the response for extended display information (EDID) is
// this.   You will not need this.
struct virtio_gpu_resp_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    le32 size;
    le32 padding;
    u8 edid[1024];
};

////////////////////////////////////////////////////////
// The following are for the VIRTIO_GPU_CMD_RESOURCE_CREATE_2D
// request, which creates a graphics canvas resource within
// the GPU.   This canvas is then rendered onto
// a scanout/monitor
//

// The possible pixel formats for a resource
// B8G8R8X8 means "4 bytes per pixel, 1 byte of blue
// followed by 1 byte of green followed by 1 byte
// of red followed by 1 byte that is ignored"
enum virtio_gpu_formats {
    VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM  = 1,
    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM  = 2,
    VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM  = 3,
    VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM  = 4,
    VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM  = 67,
    VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM  = 68,
    VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM  = 121,
    VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM  = 134,
};

// the resource (canvas) creation request
struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    le32 resource_id;    // we need to supply the id, it cannot be zero
    le32 format;         // pixel format (as above)
    le32 width;          // resource size in pixels
    le32 height;          
};

// the response for create_2d is simply
// a bare struct virtio_gpu_ctrl_hdr

////////////////////////////////////////////////////////
// The following is for a the VIRTIO_GPU_CMD_RESOURCE_UNREF
// request, which frees a graphics canvas resource within
// the GPU.   

// the request
struct virtio_gpu_resource_unref { 
    struct virtio_gpu_ctrl_hdr hdr; 
    le32 resource_id;  // which resource we are freeing
    le32 padding; 
};

// the response for resource_unref is simply
// a bare struct virtio_gpu_ctrl_hdr


////////////////////////////////////////////////////////
// The following is for a the VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING
// request, which associates region(s) of memory with
// a graphics canvas resource on the GPU.
//
// For simple 2D graphics we will have just one region of memory,
// which we call the framebuffer.  We write pixels into the
// framebuffer, and then tell the GPU to transfer them to
// the relevant graphics canvas resource.  The GPU will
// do this using DMA

// Request
struct virtio_gpu_resource_attach_backing { 
    struct virtio_gpu_ctrl_hdr hdr; 
    le32 resource_id;   // which resource
    le32 nr_entries;    // how many regions of memory
}; 

// A description of a region of memory
// the attach_backing request is followed by nr_entries of these
struct virtio_gpu_mem_entry { 
    le64 addr;         // the physical address of our region / framebuffer
    le32 length;       // length of the region in bytes
    le32 padding; 
};

// the response for attach_backing is simply
// a bare struct virtio_gpu_ctrl_hdr

////////////////////////////////////////////////////////
// The following is for a the VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING
// request, which disassociates the region(s) of memory
// we previously attached from a graphics canvas resource on the GPU.
//

// request
struct virtio_gpu_resource_detach_backing { 
    struct virtio_gpu_ctrl_hdr hdr; 
    le32 resource_id;  // the resource we are detaching all regions from
    le32 padding; 
};

// the response for detach_backing is simply
// a bare struct virtio_gpu_ctrl_hdr


////////////////////////////////////////////////////////
// The following is for a the VIRTIO_GPU_CMD_SET_SCANOUT
// request, which ties a graphics canvas resource to
// a particular monitor (scanout).  The resource will
// be rendered into the scanout:
//
// framebuffer -> resource -> scanout -> eyeball
//

// request
// associate this resource with that scanout for
// this rectangle of its screen pixels
// having multiple resources "cover" the scanout (screen)
// is a way of accelerating things like windows with movie playback 
struct virtio_gpu_set_scanout { 
    struct virtio_gpu_ctrl_hdr hdr; 
    struct virtio_gpu_rect r;    // for us, this will be the whole scanout
    le32 scanout_id;             // the monitor
    le32 resource_id;            // the resource
};


// the response for set_scanout is simply
// a bare struct virtio_gpu_ctrl_hdr


////////////////////////////////////////////////////////
// The following is for a the VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D
// request, which tells the GPU to copy the data (via DMA) within the
// framebuffer (or other backing regions) to the graphics canvas
// resource.
//
// framebuffer -> resource -> scanout -> eyeball
//

// request copies from the backing regions to the
// resource for the rectangle of pixels
struct virtio_gpu_transfer_to_host_2d { 
    struct virtio_gpu_ctrl_hdr hdr; 
    struct virtio_gpu_rect r;   // which part of the resource is being with our pixels
    le64 offset;                // where to start fetching the data from us
    le32 resource_id; 
    le32 padding; 
};

// the response for transfer_to_host_2d  is simply
// a bare struct virtio_gpu_ctrl_hdr

////////////////////////////////////////////////////////
// The following is for a the VIRTIO_GPU_CMD_RESOURCE_FLUSH
// request, which tells the GPU to render the graphics canvas
// resource on its scanout (monitor)
//
// framebuffer -> resource -> scanout -> eyeball
//

// request to transfer a particular rectangle of pixels
// from the resource to the scanout
struct virtio_gpu_resource_flush { 
    struct virtio_gpu_ctrl_hdr hdr; 
    struct virtio_gpu_rect r;  
    le32 resource_id; 
    le32 padding; 
};

// the response for resource_flush  is simply
// a bare struct virtio_gpu_ctrl_hdr


////////////////////////////////////////////////////////
// VIRTIO_GPU_CMD_GET_CAPSET and VIRTIO_GPU_CMD_GET_CAPSET_INFO
// are not defined (yet) in the standard
//



////////////////////////////////////////////////////////
// We can also have a 64x64 resource which represents
// the mouse cursor image.  Then there are special commands
// for moving and changing this image.    This is designed
// by analogy to a real GPU's "hardware cursor" feature
// which provides low-cost sprites
//
// cursor movements and image changes are handled via
// a separate virtq than other commands.


////////////////////////////////////////////////////////
// VIRTIO_GPU_CMD_UPDATE_CURSOR lets us simultaneously
// change the hardware cursor shape (to that of a different
// resource, and also move it.

// Cursor position "coordinate (x,y) on scanout/monitor n"
struct virtio_gpu_cursor_pos { 
    le32 scanout_id;   // monitor
    le32 x;            // position
    le32 y; 
    le32 padding; 
};

// request - use this resource as the cursor image, and
// move the cursor image to the given position.
//
// hot_x, hot_y are unclear in the standard.  They may mean
// the activity coordinate within the 64x64 cursor image
// (i.e., which of the 64x64 pixels do you mean when you click)
struct virtio_gpu_update_cursor { 
    struct virtio_gpu_ctrl_hdr hdr; 
    struct virtio_gpu_cursor_pos pos; 
    le32 resource_id; 
    le32 hot_x; 
    le32 hot_y; 
    le32 padding; 
};

//
// If we use the exact same structure for the request, but
// have the type be VIRTIO_GPU_MOVE_CURSOR, then we don't
// change the cursor image, just move it.   This is faster
// than a full update
//

// the response for an update_cursor is simply a bare struct
// virtio_gpu_ctrl_hdr (and optionally can be disabled)



////////////////////////////////////////////////////////
// Core functions of the driver follow
//
//

// this function will be invoked by the virtio framework
// within NK when it wants us to stop controlling the
// device
static void teardown(struct virtio_pci_dev *dev) 
{
    
    DEBUG("teardown\n");

    // We would actually do frees, etc, here
    
    virtio_pci_virtqueue_deinit(dev);
}


// Our interrupt handler - the device will interrupt
// us whenever the state of a virtq changes.  This is how
// it notifies us of changes it has made.  We notify it
// when *we* make changes via the notification register
// it maps into the physical address space
static int interrupt_handler(excp_entry_t *exp, excp_vec_t vec, void *priv_data)
{
    DEBUG("interrupt invoked\n");

    // see the parport code for why we must do this
    IRQ_HANDLER_END();
    
    return 0;
}


// Given features the virtio-gpu device supports, this function will
// determine which ones the driver will also support.
static uint64_t select_features(uint64_t features) 
{
    DEBUG("device features: 0x%0lx\n",features);
    DEBUG_FBIT(features, VIRTIO_GPU_F_VIRGL);
    DEBUG_FBIT(features, VIRTIO_GPU_F_EDID);

    // choose accepted features
    uint64_t accepted = 0;

    // we will not support either VIRGL (3D) or
    // EDID (better display info) for now
    // if we did, we would enable the following
    //FBIT_SETIF(accepted,features,VIRTIO_GPU_F_VIRGL);
    //FBIT_SETIF(accepted,features,VIRTIO_GPU_F_EDID);
    
    DEBUG("features accepted: 0x%0lx\n", accepted);
    return accepted;
}



// Debugging support - print out count descriptors within a virtq
// starting at a given position
static void debug_dump_descriptors(struct virtq *vq, int start, int count)
{
    int i;
    for (i=start;i<(start+count);i++) {
	DEBUG("vq[%d] = %p len=%u flags=0x%hx next=%hu\n",
	      i,
	      vq->desc[i].addr,
	      vq->desc[i].len,
	      vq->desc[i].flags,
	      vq->desc[i].next);
    }
}


//
// Helper function to do a virtq transaction on the device
// you are indicating that for virtq qidx, you want to push
// descriptor didx into the available ring, and then let
// the device know changed the virtq.
//
//
// available ring is being pushed to
//    it will afterwards contain a pointer (didx) to
//      the first descriptor in a chain (linked list)
//
// We will then notify the device of this change
//
// Finally, we will wait for the device to push didx into
// the used ring, indicating it has finished the request
//
// In an interrupt-driven model, we would not do any waiting
// here.  Instead, it would be the interrupt handler that would
// be fired when the device moved didx to the used ring, and
// the handler would then need to inform the original caller somehow,
// probably through a callback function
static int transact_base(struct virtio_pci_dev *dev,
			 uint16_t qidx,
			 uint16_t didx)
{
    struct virtio_pci_virtq *virtq = &dev->virtq[qidx];
    struct virtq *vq = &virtq->vq;
    uint16_t waitidx;
    uint16_t usedidx;

    // the following steps push didx onto the virtqueue
    // in a manner acceptable to the hardware
    vq->avail->ring[vq->avail->idx % vq->qsz] = didx;
    // this memory barrier makes sure the device sees
    // the above write *before*...
    mbarrier();
    // ... this write:
    vq->avail->idx++;
    // we will stash away the index in the used ring
    // which we will wait on
    waitidx = vq->avail->idx;
    // and memory barrier again to be sure these
    // two writes are globally visible
    mbarrier(); 

    // Now we are going to notify the device
    // The device's registers are memory mapped, meaning that
    // the structure read/writes below are going all the way
    // to the device

    // select the virtqueue we want to notify
    virtio_pci_atomic_store(&dev->common->queue_select, qidx);

    // make sure it is running
    virtio_pci_atomic_store(&dev->common->queue_enable, 1);

    //debug_dump_descriptors(vq,0,8);

    // ask the virtio-pci subsystem we live in to actually do the
    // notification write
    virtio_pci_virtqueue_notify(dev, qidx);

    // The device has our request now
    
    //DEBUG("request initiated\n");

    // wait for the hardware to complete our request and
    // move it to the used ring
    // Ideally we would not do this dumb polling here, but
    // make everything interrupt driven.
    do {
      usedidx = virtio_pci_atomic_load(&virtq->vq.used->idx);
    } while (usedidx != waitidx);

    // now we are done with the descriptor chain, so ask
    // the virtio-pci system to clean it up for us
    if (virtio_pci_desc_chain_free(dev,qidx,didx)) {
	ERROR("Failed to free descriptor chain\n");
	return -1;
    }

    //DEBUG("transaction complete\n");
    
    return 0;
}
    

// helper function to do a simple transaction using
// a descriptor chain of length 2.
//
//   descriptor 0:   read only  (contains request)
//   descriptor 1:   write only (where we want the response to go)
//
static int transact_rw(struct virtio_pci_dev *dev,
		       uint16_t qidx,
		       void    *req,
		       uint32_t reqlen,
		       void    *resp,
		       uint32_t resplen)
{
    uint16_t desc_idx[2];

    // allocate a two element descriptor chain, the descriptor
    // numbers will be placed in the desc_idx array.
    if (virtio_pci_desc_chain_alloc(dev, qidx, desc_idx, 2)) {
	ERROR("Failed to allocate descriptor chain\n");
	return -1;
    }

    //DEBUG("allocated chain %hu -> %hu\n",desc_idx[0],desc_idx[1]);

    
    
    // Now get pointers to the specific descriptors in the virtq struct
    // (which is shared with the hardware)
    struct virtq_desc *desc[2] = {&dev->virtq[qidx].vq.desc[desc_idx[0]],
				  &dev->virtq[qidx].vq.desc[desc_idx[1]]};

    // now build a linked list of 2 elements in this space

    // this is the "read" part - the request
    // first element of the linked list
    desc[0]->addr = (le64) req;
    desc[0]->len = reqlen;
    desc[0]->flags |= 0;
    desc[0]->next = desc_idx[1];  // next pointer is next descriptor

    // this is the "write" part - the response
    // this is where we want the device to put the response
    desc[1]->addr = (le64) resp;
    desc[1]->len = resplen;
    desc[1]->flags |= VIRTQ_DESC_F_WRITE;  
    desc[1]->next = 0;            // next pointer is null   
 
    return transact_base(dev,qidx,desc_idx[0]);
}

// helper function to do a simple transaction using
// a descriptor chain of length 3.
//
//   descriptor 0:   read only  (contains request)
//   descriptor 1:   read only  (contains more of the request (for variable length stuff))
//   descriptor 2:   write only (where we want the response to go)
//
static int transact_rrw(struct virtio_pci_dev *dev,
			uint16_t qidx,
			void    *req,
			uint32_t reqlen,
			void    *more,
			uint32_t morelen,
			void    *resp,
			uint32_t resplen)
{
    uint16_t desc_idx[3];

    // allocate a three element descriptor chain, the descriptor
    // numbers will be placed in the desc_idx array.
    if (virtio_pci_desc_chain_alloc(dev, qidx, desc_idx, 3)) {
	ERROR("Failed to allocate descriptor chain\n");
	return -1;
    }

    //    DEBUG("allocated chain %hu -> %hu -> %hu\n",desc_idx[0],desc_idx[1],desc_idx[2]);

    // Now get pointers to the specific descriptors in the virtq struct
    // (which is shared with the hardware)
    struct virtq_desc *desc[3] = {&dev->virtq[qidx].vq.desc[desc_idx[0]],
				  &dev->virtq[qidx].vq.desc[desc_idx[1]],
				  &dev->virtq[qidx].vq.desc[desc_idx[2]] };

    // this is the "read" part - the request
    // first element of the linked list
    desc[0]->addr = (le64) req;
    desc[0]->len = reqlen;
    desc[0]->flags |= 0;
    desc[0]->next = desc_idx[1];  // next pointer is next descriptor

    // more readable data, but perhaps in a different, non-consecutive address
    desc[1]->addr = (le64) more;
    desc[1]->len = morelen;
    desc[1]->flags |= 0;
    desc[1]->next = desc_idx[2]; // next pointer is next descriptor

    // this is the "write" part - the response
    // this is where we want the device to put the response
    desc[2]->addr = (le64) resp;
    desc[2]->len = resplen;
    desc[2]->flags |= VIRTQ_DESC_F_WRITE;
    desc[2]->next = 0;           // next pointer is null

    return transact_base(dev,qidx,desc_idx[0]);
}


// helper to zero requests - always a good idea!
#define ZERO(a) memset(a,0,sizeof(*a))


// the resource ids we will use
// it is important to note that resource id 0 has special
// meaning - it means "disabled" or "none"
#define SCREEN_RID 42     // for the whole screen (scanout)
#define CURSOR_RID 23     // for the cursor (if implemented)

// helper macro to make sure that response we get are quickly and easily checked
#define CHECK_RESP(h,ok,errstr) if (h.type!=ok) { ERROR(errstr " rc=%x\n",h.type); return -1; }

//
//
// Startup testing - switch to graphics mode and draw something on the screen
//
static int test_gpu(struct virtio_gpu_dev *gdev)
{
    struct virtio_pci_dev *dev = gdev->virtio_dev;
    
    // 1. We will need to find what scanouts (monitors) are attached,
    // and what their resolutions are

    // Our request/response pair
    struct virtio_gpu_ctrl_hdr          disp_info_req;
    struct virtio_gpu_resp_display_info disp_info_resp;

    // Be paranoid about these things - you want them to start with all zeros
    ZERO(&disp_info_req);
    ZERO(&disp_info_resp);

    // we are making the get display info request
    disp_info_req.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    // now issue the request via virtqueue
    if (transact_rw(dev,
		    0,
		    &disp_info_req,
		    sizeof(disp_info_req),
		    &disp_info_resp,
		    sizeof(disp_info_resp))) {
	ERROR("Failed to get display info\n");
	return -1;
    }

    // If we get here, we have a response, but we don't know if the response is OK
    // ALWAYS CHECK
    CHECK_RESP(disp_info_resp.hdr,VIRTIO_GPU_RESP_OK_DISPLAY_INFO,"Failed to get display info");

    // now just print out the monitors and their resolutions
    for (int i = 0; i < 16; i++) {
	if (disp_info_resp.pmodes[i].enabled) { 
	    DEBUG("scanout (monitor) %u has info: %u, %u, %ux%u, %u, %u\n", i, disp_info_resp.pmodes[i].r.x, disp_info_resp.pmodes[i].r.y, disp_info_resp.pmodes[i].r.width, disp_info_resp.pmodes[i].r.height, disp_info_resp.pmodes[i].flags, disp_info_resp.pmodes[i].enabled);
	}
    }

    // 2. we would create a resource for the screen
    
    struct virtio_gpu_resource_create_2d create_2d_req;
    struct virtio_gpu_ctrl_hdr           create_2d_resp;

    ZERO(&create_2d_req);
    ZERO(&create_2d_resp);

    //
    // WRITE ME
    //


    // 3. we would create a framebuffer that we can write pixels into

    // this assumes that we want scanout/monitor 0 only
    uint64_t fb_length = disp_info_resp.pmodes[0].r.width * disp_info_resp.pmodes[0].r.height * 4;
    uint32_t *framebuffer = malloc(fb_length);

    if (!framebuffer) {
	ERROR("failed to allocate framebuffer of length %lu\n",fb_length);
    } else {
	DEBUG("allocated framebuffer of length %lu\n",fb_length);
    }

    // 4. we should probably fill the framebuffer with some initial data
    // A typical driver would fill it with zeros (black screen), but we
    // might want to put something more exciting there.

    //
    // WRITE ME
    //
    

    // 5. Now we need to associate our framebuffer with our resource (step 2)
    struct virtio_gpu_resource_attach_backing backing_req;
    struct virtio_gpu_mem_entry               backing_entry;
    struct virtio_gpu_ctrl_hdr                backing_resp;

    ZERO(&backing_req);
    ZERO(&backing_entry);
    ZERO(&backing_resp);

    //
    // WRITE ME
    //

    // 6. Now we need to associate our resource (step 2) with the scanout (step 1)

    struct virtio_gpu_set_scanout setso_req;
    struct virtio_gpu_ctrl_hdr    setso_resp;

    ZERO(&setso_req);
    ZERO(&setso_resp);

    //
    // WRITE ME
    //

    // 7. Now we need to command the GPU to transfer the data from our framebuffer (step 3)
    // to our resource (step 2)
    struct virtio_gpu_transfer_to_host_2d xfer_req;
    struct virtio_gpu_ctrl_hdr            xfer_resp;

    ZERO(&xfer_req);
    ZERO(&xfer_resp);
    
    //
    // WRITE ME
    //


    // 8. Now we need to command the GPU to render our resource (step 2) onto the scanout
    // (monitor) (step 6)
    
    struct virtio_gpu_resource_flush flush_req;
    struct virtio_gpu_ctrl_hdr       flush_resp;

    ZERO(&flush_req);
    ZERO(&flush_resp);
    
    //
    // WRITE ME
    //

    // 9. At this point, you should have a picture on the screen!

    
    //
    // ADVANCED TASK A:  Make an animation.   You can do this by modifying the 
    // pixels in your framebuffer (using regular C code), and then repeating
    // steps 7 and 8.   In other words doing steps 4, 7, and 8 in a loop will
    // let you render a sequence of images
    //

    // ADVANCED TASK B: Change and move the cursor.   To enable the mouse cursor,
    // you need to repeat steps 2, 3, 4, 5, 7, 8 for a 64x64 pixel framebuffer/resource
    // these pixels are the cursor image.
    // To move the cursor, or change its image, you would issue virtio_gpu_update_cursor
    // requests ****to virtqueue 1****.  
    
    DEBUG("done with test\n");

    // HERE YOU SHOULD CAREFULLY UNDO THINGS
    //  1. detach backing framebuffers from resources
    //  2. reset all configured scanouts to disabled (resource id 0)
    //  3. unref all of your resources
    //  4. free framebuffers and other memory
    
    free(framebuffer);

    // attempt to reset to VGA text mode
    DEBUG("reseting device back to VGA compatibility mode (we hope - this will fail on older QEMUs)\n");
    
    virtio_pci_atomic_store(&dev->common->device_status,0);

    return 0;
}



////////////////////////////////////////////////////////
// Basic device interface functions.
// Currently, this is a "generic" device, and so
// there is nothing much that the rest of the kernel
// can do with it.
//
//

static int open(void *state)
{
    return 0;  
}

static int close(void *state)
{
    return 0;
}

static struct nk_dev_int ops = {
    .open = open,
    .close = close,
};



////////////////////////////////////////////////////////
// Device initialization.
//
// In NK's virtio_pci framework, the framework discovers
// and does basic interogation of virtio devices.  Then,
// for each device, it invokes an initialization function,
// like this one.   The initialization function is responsble
// for device configuration and then registering the
// device to the rest of the kernel can use it.  The virtio_pci
// framework provides functions to do the common elements of this
int virtio_gpu_init(struct virtio_pci_dev *dev)
{
    char buf[DEV_NAME_LEN];
    
    DEBUG("initialize device\n");
    
    // allocate and zero a state structure for this device
    struct virtio_gpu_dev *d = malloc(sizeof(*d));
    if (!d) {
	ERROR("cannot allocate state\n");
	return -1;
    }
    memset(d,0,sizeof(*d));

    // acknowledge to the device that we see it
    if (virtio_pci_ack_device(dev)) {
        ERROR("Could not acknowledge device\n");
        free(d);
        return -1;
    }

    // ask the device for what features it supports
    if (virtio_pci_read_features(dev)) {
        ERROR("Unable to read device features\n");
        free(d);
        return -1;
    }

    // tell the device what features we will support
    if (virtio_pci_write_features(dev, select_features(dev->feat_offered))) {
        ERROR("Unable to write device features\n");
        free(d);
        return -1;
    }
    
    // initilize the device's virtqs.   The virtio-gpu device
    // has two of them.  The first is for most requests/responses,
    // while the second is for (mouse) cursor updates and movement
    if (virtio_pci_virtqueue_init(dev)) {
	ERROR("failed to initialize virtqueues\n");
	free(d);
	return -1;
    }

    // associate our state with the general virtio-pci device structure,
    // and vice-versa:
    dev->state = d;
    dev->teardown = teardown;    // the function we provide for deletion
    d->virtio_dev = dev;

    // make sure our lock is in a known state
    spinlock_init(&d->lock);
    
    
    // build a name for this device
    snprintf(buf,DEV_NAME_LEN,"virtio-gpu%u",__sync_fetch_and_add(&num_devs,1));
    
    // register the device, currently just as a generic device
    // note that this also creates an association with the generic
    // device represention elesewhere in the kenrel
    d->gpu_dev = nk_dev_register(buf,             // our name
				 NK_DEV_GENERIC,  // generic device
				 0,               // no flags
				 &ops,            // our "interface"
				 d);              // our state			         
    
    if (!d->gpu_dev) {
	ERROR("failed to register block device\n");
	virtio_pci_virtqueue_deinit(dev);
	free(d);
	return -1;
    }
    
    // Now we want to enable interrupts for the device
    // and register our handler
    //
    // This is MUCH more complicated than interrupt setup for
    // the parport device because the interrupt number or even
    // how many interrupts sources there are are not known beforehand
    // we have to figure it out as we boot.
    //
    // Also, interrupts for this device use a PCI technology called
    // MSI-X ("message signalled interrupts extended").  This setup code
    // is also similar to what would happen for a non-virtio PCI device
    // (see e1000e.c if you're curious)

    // if this is too terrifying you can shield your eyes until "device inited"

    // Note that this code will leak badly if interrupts cannot be configured

    // grab the pci device aspect of the virtio device
    struct pci_dev *p = dev->pci_dev;
    uint8_t i;
    ulong_t vec;
    
    if (dev->itype==VIRTIO_PCI_MSI_X_INTERRUPT) {
	// we assume MSI-X has been enabled on the device
	// already, that virtqueue setup is done, and
	// that queue i has been mapped to MSI-X table entry i
	// MSI-X is on but whole function is masked

	DEBUG("setting up interrupts via MSI-X\n");
	
	if (dev->num_virtqs != p->msix.size) {
	    DEBUG("weird mismatch: numqueues=%u msixsize=%u\n", dev->num_virtqs, p->msix.size);
	    // continue for now...
	    // return -1;
	}
	
	// this should really go by virtqueue, not entry
	// and ideally pulled into a per-queue setup routine
	// in virtio_pci...
	uint16_t num_vec = p->msix.size;
        
	// now fill out the device's MSI-X table
	for (i=0;i<num_vec;i++) {
	    // find a free vector
	    // note that prioritization here is your problem
	    if (idt_find_and_reserve_range(1,0,&vec)) {
		ERROR("cannot get vector...\n");
		return -1;
	    }
	    // register your handler for that vector
	    if (register_int_handler(vec, interrupt_handler, d)) {
		ERROR("failed to register int handler\n");
		return -1;
	    }
	    // set the table entry to point to your handler
	    if (pci_dev_set_msi_x_entry(p,i,vec,0)) {
		ERROR("failed to set MSI-X entry\n");
		virtio_pci_virtqueue_deinit(dev);
		free(d);
		return -1;
	    }
	    // and unmask it (device is still masked)
	    if (pci_dev_unmask_msi_x_entry(p,i)) {
		ERROR("failed to unmask entry\n");
		return -1;
	    }
	    DEBUG("finished setting up entry %d for vector %u on cpu 0\n",i,vec);
	}
	
	// unmask entire function
	if (pci_dev_unmask_msi_x_all(p)) {
	    ERROR("failed to unmask device\n");
	    return -1;
	}
	
    } else {

	ERROR("This device must operate with MSI-X\n");
	return -1;
    }
    
    DEBUG("device inited\n");

    // At this point a real driver would be done.   The rest of the kernel
    // would invoke functions in an abstract gpu framework to find screen resolutions,
    // switch graphics/text modes, draw, etc.
    //
    // However, we do not have such an abstraction.    For part of this lab
    // you will design one (and maybe implement it)
    //
    // To start, we want you to simply drive the GPU here and get it to the point
    // where you can draw things on the screen, and perhaps move the mouse cursor around
   
    if (test_gpu(d)) {
	ERROR("Test GPU failed\n");
	return -1;
    }
	     

    DEBUG("done\n");

    return 0;
}
