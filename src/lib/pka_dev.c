//
//   BSD LICENSE
//
//   Copyright(c) 2016 Mellanox Technologies, Ltd. All rights reserved.
//   All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions
//   are met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in
//       the documentation and/or other materials provided with the
//       distribution.
//     * Neither the name of Mellanox Technologies nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
//   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
//   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
//   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/timex.h>
#include <linux/vfio.h>

#include "pka_firmware.h"
#else
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#endif

#include "pka_dev.h"

#ifdef __KERNEL__
pka_dev_gbl_config_t pka_gbl_config;

// Global PKA shim resource info table
static pka_dev_gbl_shim_res_info_t pka_gbl_res_tbl[PKA_MAX_NUM_IO_BLOCKS];

// Start a PKA device timer.
static uint64_t pka_dev_timer_start(uint32_t usec)
{
  uint64_t cur_time = get_cycles();
  return (cur_time + (pka_early_cpu_speed() * usec) / 1000000ULL);
}

// Test a PKA device timer for completion.
static int pka_dev_timer_done(uint64_t timer)
{
  return (get_cycles() >= timer);
}

// Return register base address
static uint64_t pka_dev_get_register_base(uint64_t base, uint64_t reg_addr)
{
    return (base + reg_addr) & PAGE_MASK;
}

// Return register offset
static uint64_t pka_dev_get_register_offset(uint64_t base, uint64_t reg_addr)
{
    return (base + reg_addr) & ~PAGE_MASK;
}

// Return word offset within io memory
static uint64_t pka_dev_get_word_offset(uint64_t mem_base, uint64_t word_addr,
                                            uint64_t mem_size)
{
    return (mem_base + word_addr) & (mem_size - 1);
}

static uint64_t pka_dev_io_read(void *mem_ptr, uint64_t mem_off)
{
    uint64_t    data;

    data = pka_mmio_read(mem_ptr + mem_off);

    return data;
}

static void pka_dev_io_write(void *mem_ptr, uint64_t mem_off, uint64_t value)
{
    pka_mmio_write(mem_ptr + mem_off, value);
}

// Add the resource to the global resource table
static int pka_dev_add_resource(pka_dev_res_t *res_ptr, uint32_t shim_idx)
{
    uint8_t res_cnt;

    res_cnt = pka_gbl_res_tbl[shim_idx].res_cnt;

    if (res_cnt >= PKA_DEV_SHIM_RES_CNT)
        return -ENOMEM;

    pka_gbl_res_tbl[shim_idx].res_tbl[res_cnt] = res_ptr;
    pka_gbl_res_tbl[shim_idx].res_cnt++;

    return 0;
}

// Remove the resource from the global resource table
static int pka_dev_put_resource(pka_dev_res_t *res,  uint32_t shim_idx)
{
    pka_dev_res_t *res_ptr;
    uint8_t        res_idx;

    for (res_idx = 0; res_idx < PKA_DEV_SHIM_RES_CNT; res_idx++)
    {
        res_ptr = pka_gbl_res_tbl[shim_idx].res_tbl[res_idx];
        if (res_ptr && strcmp(res_ptr->name, res->name) == 0)
        {
            pka_gbl_res_tbl[shim_idx].res_tbl[res_idx] = NULL;
            pka_gbl_res_tbl[shim_idx].res_cnt--;
            break;
        }
    }

    // Check whether the resource shares the same memory map; If so,
    // the memory map shouldn't be released.
    for (res_idx = 0; res_idx < PKA_DEV_SHIM_RES_CNT; res_idx++)
    {
        res_ptr = pka_gbl_res_tbl[shim_idx].res_tbl[res_idx];
        if (res_ptr && (res_ptr->base == res->base))
            return -EBUSY;
    }

    return 0;
}

static void* pka_dev_get_resource_ioaddr(uint64_t res_base, uint32_t shim_idx)
{
    pka_dev_res_t *res_ptr;
    uint8_t        res_cnt, res_idx;

    res_cnt = pka_gbl_res_tbl[shim_idx].res_cnt;

    if (res_cnt == 0)
        return NULL;

    for (res_idx = 0; res_idx < res_cnt; res_idx++)
    {
        res_ptr = pka_gbl_res_tbl[shim_idx].res_tbl[res_idx];
        if (res_ptr->base == res_base)
            return res_ptr->ioaddr;
    }

    return NULL;
}

// Set PKA device resource config - - map io memory if needed.
static int pka_dev_set_resource_config(pka_dev_shim_t *shim,
                                       pka_dev_res_t  *res_ptr,
                                       uint64_t        res_base,
                                       uint64_t        res_size,
                                       uint64_t        res_type,
                                       char            *res_name)
{
    int ret = 0;

    if (res_ptr->status == PKA_DEV_RES_STATUS_MAPPED)
        return -EPERM;

    if (res_type == PKA_DEV_RES_TYPE_REG)
        res_ptr->base = res_base;

    if (res_type == PKA_DEV_RES_TYPE_MEM)
        res_ptr->base = shim->base + res_base;

    res_ptr->size    = res_size;
    res_ptr->type    = res_type;
    res_ptr->name    = res_name;
    res_ptr->status  = PKA_DEV_RES_STATUS_UNMAPPED;
    res_ptr->ioaddr  = pka_dev_get_resource_ioaddr(res_ptr->base,
                                                   shim->shim_id);
    if (!res_ptr->ioaddr)
    {
        if (!request_mem_region(res_ptr->base, res_ptr->size, res_ptr->name))
        {
            PKA_ERROR(PKA_DEV, "failed to get io memory region\n");
            return -EPERM;
        }

        res_ptr->ioaddr = ioremap_nocache(res_ptr->base, res_ptr->size);
    }

    res_ptr->status = PKA_DEV_RES_STATUS_MAPPED;

    if (!res_ptr->ioaddr || pka_dev_add_resource(res_ptr, shim->shim_id))
    {
        PKA_ERROR(PKA_DEV, "unable to map io memory\n");
        release_mem_region(res_ptr->base, res_ptr->size);
        return -ENOMEM;
    }
    return ret;
}

// Unset PKA device resource config - unmap io memory if needed.
static void pka_dev_unset_resource_config(pka_dev_shim_t *shim,
                                          pka_dev_res_t  *res_ptr)
{
    int ret = -EBUSY;

    if (res_ptr->status != PKA_DEV_RES_STATUS_MAPPED)
        return;

    if (res_ptr->ioaddr &&
            ret != pka_dev_put_resource(res_ptr, shim->shim_id))
    {
        iounmap(res_ptr->ioaddr);
        release_mem_region(res_ptr->base, res_ptr->size);
    }

    res_ptr->status = PKA_DEV_RES_STATUS_UNMAPPED;
}

// Initialize ring. Set ring parameters and configure ring resources.
// It returns 0 on success, a negative error code on failure.
static int pka_dev_init_ring(pka_dev_ring_t *ring, uint32_t ring_id,
                        pka_dev_shim_t *shim)
{
    int ret = 0;

    pka_dev_res_t *ring_info_words_ptr;
    pka_dev_res_t *ring_counters_ptr;
    pka_dev_res_t *ring_window_ram_ptr;

    uint32_t ring_words_off;
    uint32_t ring_cntrs_off;
    uint32_t ring_mem_off;
    uint32_t ring_mem_base;
    uint32_t ring_mem_size;

    uint32_t shim_ring_id;
    uint8_t  window_ram_split;

    if (ring->status != PKA_DEV_RING_STATUS_UNDEFINED)
    {
        PKA_ERROR(PKA_DEV, "PKA ring must be undefined\n");
        return -EPERM;
    }

    if (ring_id > PKA_MAX_NUM_RINGS - 1)
    {
        PKA_ERROR(PKA_DEV, "invalid ring identifier\n");
        return -EINVAL;
    }

    ring->ring_id        = ring_id;
    ring->shim           = shim;
    ring->resources_num  = PKA_MAX_NUM_RING_RESOURCES;

    shim_ring_id              = ring_id % PKA_MAX_NUM_IO_BLOCK_RINGS;
    shim->rings[shim_ring_id] = ring;

    // Configure ring information control/status words resource
    ring_info_words_ptr         = &ring->resources.info_words;
    ring_words_off              = shim_ring_id * PKA_RING_WORDS_SPACING;
    ring_info_words_ptr->base   = ring_words_off + PKA_RING_WORDS_ADDR;
    ring_info_words_ptr->size   = PKA_RING_WORDS_SIZE;
    ring_info_words_ptr->type   = PKA_DEV_RES_TYPE_MEM;
    ring_info_words_ptr->status = PKA_DEV_RES_STATUS_UNMAPPED;
    ring_info_words_ptr->name   = "PKA_RING_INFO";

    // Configure ring counters registers resource
    ring_counters_ptr           = &ring->resources.counters;
    ring_cntrs_off              = shim_ring_id * PKA_RING_CNTRS_SPACING;
    ring_counters_ptr->base     = ring_cntrs_off + PKA_RING_CNTRS_ADDR;
    ring_counters_ptr->size     = PKA_RING_CNTRS_SIZE;
    ring_counters_ptr->type     = PKA_DEV_RES_TYPE_REG;
    ring_counters_ptr->status   = PKA_DEV_RES_STATUS_UNMAPPED;
    ring_counters_ptr->name     = "PKA_RING_CNTRS";

    // Configure ring window RAM resource
    window_ram_split = shim->window_ram_split;
    if (window_ram_split == PKA_SHIM_WINDOW_RAM_SPLIT_ENABLED)
    {
        ring_mem_off  = shim_ring_id * PKA_RING_MEM_1_SPACING;
        ring_mem_base = ring_mem_off + PKA_RING_MEM_1_BASE;
        ring_mem_size = PKA_RING_MEM_1_SIZE;
    }
    else
    {
        ring_mem_off  = shim_ring_id * PKA_RING_MEM_0_SPACING;
        ring_mem_base = ring_mem_off + PKA_RING_MEM_0_BASE;
        ring_mem_size = PKA_RING_MEM_0_SIZE;
    }

    ring_window_ram_ptr         = &ring->resources.window_ram;
    ring_window_ram_ptr->base   = ring_mem_base;
    ring_window_ram_ptr->size   = ring_mem_size;
    ring_window_ram_ptr->type   = PKA_DEV_RES_TYPE_MEM;
    ring_window_ram_ptr->status = PKA_DEV_RES_STATUS_UNMAPPED;
    ring_window_ram_ptr->name   = "PKA_RING_WINDOW";

    ring->ring_info = kzalloc(sizeof(pka_dev_hw_ring_info_t), GFP_KERNEL);
    if (!ring->ring_info)
    {
        PKA_ERROR(PKA_DEV, "unable to kmalloc\n");
        kfree(ring->ring_info);
        return -ENOMEM;
    }

    ring->status  = PKA_DEV_RING_STATUS_INITIALIZED;

    return ret;
}

// Release a given Ring.
static int pka_dev_release_ring(pka_dev_ring_t *ring)
{
    int ret = 0;

    pka_dev_shim_t *shim;
    uint32_t        shim_ring_id;

    if (ring->status == PKA_DEV_RING_STATUS_UNDEFINED)
        return ret;

    if (ring->status == PKA_DEV_RING_STATUS_BUSY)
    {
        PKA_ERROR(PKA_DEV, "PKA ring is busy\n");
        return -EBUSY;
    }

    shim = ring->shim;

    if (shim->status == PKA_SHIM_STATUS_RUNNING)
    {
        PKA_ERROR(PKA_DEV, "PKA shim is running\n");
        return -EPERM;
    }

    pka_dev_unset_resource_config(shim, &ring->resources.info_words);
    pka_dev_unset_resource_config(shim, &ring->resources.counters);
    pka_dev_unset_resource_config(shim, &ring->resources.window_ram);

    kfree(ring->ring_info);

    ring->status = PKA_DEV_RING_STATUS_UNDEFINED;
    shim_ring_id = ring->ring_id % PKA_MAX_NUM_IO_BLOCK_RINGS;
    shim->rings[shim_ring_id] = NULL;
    shim->rings_num--;

    return ret;
}

// Partition the window RAM for a given PKA ring.  Here we statically divide
// the 16K memory region into three partitions:  First partition is reserved
// for command descriptor ring (1K), second partition is reserved for result
// descriptor ring (1K), and the remaining 14K are reserved for vector data.
// Through this memroy partition scheme, command/result descriptor rings hold
// a total of 1KB/64B = 16 descriptors each. The adresses for the rings start
// at offset 0x3800.  Also note that it is possible to have rings full while
// the vector data can support more data,  the opposite can also happen, but
// it is not suitable. For instance ECC point multiplication requires 8 input
// vectors and 2 output vectors, a total of 10 vectors. If each vector has a
// length of 24 words (24x4B = 96B), we can process 14KB/960B = 14 operations
// which is close to 16 the total descriptors supported by rings. On the other
// hand, using 12K vector data region, allows to process only 12 operations,
// while rings can hold 32 descriptors (ring usage is significantly low).
// For ECDSA verify, we have 12 vectors which require 1152B, with 14KB we can
// handle 12 operations, against 10 operations with 12KB vector data memory.
// We believe that the aformentionned memory partition help us to leverage
// the trade-off between supported descriptors and required vectors. Note
// that these examples gives approximative values and does not include buffer
// word padding across vectors.
//
// The function also writes the result descriptor rings base addresses, size
// and type, and initialize the read and write pointers and statistics. It
// returns 0 on success, a negative error code on failure.
//
// This function must be called once per ring, at initialization before any
// other fonctions are called.
static int pka_dev_partition_mem(pka_dev_ring_t *ring)
{
    int ret = 0;

    pka_dev_shim_t         *shim;
    pka_dev_hw_ring_info_t *ring_info;

    uint32_t ring_mem_base;
    uint32_t ring_mem_size;
    uint32_t data_mem_base;
    uint32_t data_mem_size;

    uint64_t cmd_desc_ring_base;
    uint32_t cmd_desc_ring_size;
    uint64_t rslt_desc_ring_base;
    uint32_t rslt_desc_ring_size;

    uint16_t num_cmd_desc;
    uint16_t host_desc_size;
    uint8_t  ring_in_order;

    uint64_t window_ram_base;
    uint64_t window_ram_size;

    shim = ring->shim;

    if (!ring->shim ||
            ring->status != PKA_DEV_RING_STATUS_INITIALIZED)
        return -EPERM;

    ring_in_order   = shim->ring_type;
    window_ram_base = ring->resources.window_ram.base;
    window_ram_size = ring->resources.window_ram.size;
    // Partition ring memory.  Give ring pair (cmmd descriptor ring and rslt
    // descriptor ring) an equal portion of the memory.  The cmmd descriptor
    // ring and result descriptor ring are used as "non-overlapping" ring.
    // Currently set aside 1/8 of the window RAM for command/result descriptor
    // rings - giving a total of 1K/64B = 16 descriptors per ring.
    // The remaining memory is "Data Memory" - i.e. memory to hold the command
    // operands and results - also called input/output vectors (in all cases
    // these vectors are just single large integers - often in the range of
    // hundreds to thousands of bits long).
    ring_mem_size  = PKA_WINDOW_RAM_RING_MEM_SIZE / 2;
    data_mem_size  = PKA_WINDOW_RAM_DATA_MEM_SIZE;
    data_mem_base  = window_ram_base;
    ring_mem_base  = data_mem_base + data_mem_size;

    num_cmd_desc   = ring_mem_size / CMD_DESC_SIZE;
    host_desc_size = CMD_DESC_SIZE / BYTES_PER_WORD;

    cmd_desc_ring_size  = num_cmd_desc * CMD_DESC_SIZE;
    rslt_desc_ring_size = cmd_desc_ring_size;

    ring->num_cmd_desc  = num_cmd_desc;

    // The command and result descriptor rings may be placed at different
    // (non-overlapping) locations in Window RAM memory space. PKI command
    // interface: Most of the functionality is defined by the EIP-154 master
    // firmware on the EIP-154 master controller Sequencer.
    cmd_desc_ring_base  = ring_mem_base;
    rslt_desc_ring_base = ring_mem_base + cmd_desc_ring_size;

    cmd_desc_ring_base  =
            PKA_RING_MEM_ADDR(cmd_desc_ring_base, window_ram_size);
    rslt_desc_ring_base =
            PKA_RING_MEM_ADDR(rslt_desc_ring_base, window_ram_size);

    ring_info = ring->ring_info;
    // Fill ring information.
    ring_info->cmmd_base      = cmd_desc_ring_base;
    ring_info->rslt_base      = rslt_desc_ring_base;
    ring_info->size           = num_cmd_desc - 1;
    ring_info->host_desc_size = host_desc_size;
    ring_info->in_order       = ring_in_order;
    ring_info->cmmd_rd_ptr    = 0x0;
    ring_info->rslt_wr_ptr    = 0x0;
    ring_info->cmmd_rd_stats  = 0x0;
    ring_info->rslt_wr_stats  = 0x0;

    return ret;
}

// Write the ring base address, ring size and type, and initialize (clear)
// the read and write pointers and statistics.
static int pka_dev_write_ring_info(pka_dev_res_t *buffer_ram_ptr,
                                   uint8_t        ring_id,
                                   uint32_t       ring_cmmd_base_val,
                                   uint32_t       ring_rslt_base_val,
                                   uint32_t       ring_size_type_val)
{
    uint32_t  ring_spacing;
    uint64_t  word_off;
    int       ret = 0;

    if (buffer_ram_ptr->status != PKA_DEV_RES_STATUS_MAPPED ||
            buffer_ram_ptr->type != PKA_DEV_RES_TYPE_MEM)
        return -EPERM;

    PKA_DEBUG(PKA_DEV, "Writing ring information control/status words\n");

    ring_spacing = ring_id * PKA_RING_WORDS_SPACING;

    // Write the command ring base address  that  the  EIP-154
    // master firmware uses with the command ring read pointer
    // to get command descriptors from the Host ring. After the
    // initialization, although the word is writeable it should
    // be regarded as read-only.
    word_off = pka_dev_get_word_offset(buffer_ram_ptr->base,
                                    RING_CMMD_BASE_0_ADDR + ring_spacing,
                                    PKA_BUFFER_RAM_SIZE);
    pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, ring_cmmd_base_val);

    // Write the result  ring base address  that  the  EIP-154
    // master firmware uses with the result ring write pointer
    // to put the result descriptors in the Host ring.   After
    // the initialization,  although the word is writeable  it
    // should be regarded as read-only.
    word_off = pka_dev_get_word_offset(buffer_ram_ptr->base,
                                    RING_RSLT_BASE_0_ADDR + ring_spacing,
                                    PKA_BUFFER_RAM_SIZE);
    pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, ring_rslt_base_val);

    // Write the ring size (number of descriptors), the size of
    // the descriptor and the result reporting scheme. After the
    // initialization,  although the word is writeable it should
    // be regarded as read-only.
    word_off = pka_dev_get_word_offset(buffer_ram_ptr->base,
                                    RING_SIZE_TYPE_0_ADDR + ring_spacing,
                                    PKA_BUFFER_RAM_SIZE);
    pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, ring_size_type_val);

    // Write the command and result ring indices that the  EIP-154
    // master firmware uses. This word should be written with zero
    // when the ring information is initialized.  After the
    // initialization, although the word is writeable it should be
    // regarded as read-only.
    word_off = pka_dev_get_word_offset(buffer_ram_ptr->base,
                                    RING_RW_PTRS_0_ADDR + ring_spacing,
                                    PKA_BUFFER_RAM_SIZE);
    pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, 0);

    // Write the ring statistics   (two 16-bit counters,  one for
    // commands and one for results) from EIP-154 master firmware
    // point of view.  This word should be written with zero when
    // the ring information is initialized.  After the initializa-
    // -tion, although the word is writeable it should be regarded
    // as read-only.
    word_off = pka_dev_get_word_offset(buffer_ram_ptr->base,
                                    RING_RW_STAT_0_ADDR + ring_spacing,
                                    PKA_BUFFER_RAM_SIZE);
    pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, 0);

    return ret;
}

// Set up the control/status words. Upon a PKI command the EIP-154 master
// firmware will read and partially update the ring information.
static int pka_dev_set_ring_info(pka_dev_ring_t *ring)
{
    int ret = 0;

    pka_dev_shim_t         *shim;
    pka_dev_hw_ring_info_t *ring_info;
    pka_dev_res_t          *buffer_ram_ptr;

    uint32_t ring_cmmd_base_val;
    uint32_t ring_rslt_base_val;
    uint32_t ring_size_type_val;

    uint8_t  ring_id;

    shim = ring->shim;
    // Ring info configuration MUST be done when the PKA ring
    // is initilaized.
    if ((shim->status != PKA_SHIM_STATUS_INITIALIZED &&
         shim->status != PKA_SHIM_STATUS_RUNNING     &&
         shim->status != PKA_SHIM_STATUS_STOPPED)    ||
            ring->status != PKA_DEV_RING_STATUS_INITIALIZED)
        return -EPERM;

    ring_id = ring->ring_id % PKA_MAX_NUM_IO_BLOCK_RINGS;

    // Partition ring memory.
    ret = pka_dev_partition_mem(ring);
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "failed to initialize ring memory\n");
        return ret;
    }

    // Fill ring infomation.
    ring_info = ring->ring_info;

    ring_cmmd_base_val  = ring_info->cmmd_base;
    ring_rslt_base_val  = ring_info->rslt_base;

    ring_size_type_val  = (ring_info->in_order       & 0x0001) << 31;
    ring_size_type_val |= (ring_info->host_desc_size & 0x03FF) << 18;
    ring_size_type_val |= (ring->num_cmd_desc - 1)   & 0xFFFF;

    buffer_ram_ptr = &shim->resources.buffer_ram;
    // Write ring information status/control words in the PKA Buffer RAM
    ret = pka_dev_write_ring_info(buffer_ram_ptr, ring_id, ring_cmmd_base_val,
                                    ring_rslt_base_val, ring_size_type_val);
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "failed to wirte ring information\n");
        return ret;
    }

    ring->status = PKA_DEV_RING_STATUS_READY;

    return ret;
}

// Create shim. Set shim parameters and configure shim resources.
// It returns 0 on success, a negative error code on failure.
static int pka_dev_create_shim(pka_dev_shim_t *shim, uint32_t shim_id,
                        uint64_t shim_base, uint64_t shim_size, uint8_t split)
{
    int ret = 0;

    uint64_t reg_base;
    uint64_t reg_size;

    if (shim->status == PKA_SHIM_STATUS_CREATED)
        return ret;

    if (shim->status != PKA_SHIM_STATUS_UNDEFINED)
    {
        PKA_ERROR(PKA_DEV, "PKA device must be undefined\n");
        return -EPERM;
    }

    if (shim_id > PKA_MAX_NUM_IO_BLOCKS - 1)
    {
        PKA_ERROR(PKA_DEV, "invalid shim identifier\n");
        return -EINVAL;
    }

    shim->shim_id    = shim_id;
    shim->base       = shim_base;
    shim->size       = shim_size;

    if (split)
        shim->window_ram_split = PKA_SHIM_WINDOW_RAM_SPLIT_ENABLED;
    else
        shim->window_ram_split = PKA_SHIM_WINDOW_RAM_SPLIT_DISABLED;

    shim->ring_type     = PKA_RING_TYPE_IN_ORDER;
    shim->ring_priority = PKA_RING_OPTIONS_PRIORITY;
    shim->rings_num     = PKA_MAX_NUM_IO_BLOCK_RINGS;
    shim->rings = kzalloc(sizeof(pka_dev_ring_t) * shim->rings_num,
                                GFP_KERNEL);
    if (!shim->rings)
    {
        PKA_ERROR(PKA_DEV, "unable to kmalloc\n");
        kfree(shim->rings);
        return -ENOMEM;
    }

    // Set PKA device Buffer RAM config
    ret = pka_dev_set_resource_config(shim, &shim->resources.buffer_ram,
                                        PKA_BUFFER_RAM_BASE,
                                        PKA_BUFFER_RAM_SIZE,
                                        PKA_DEV_RES_TYPE_MEM,
                                        "PKA_BUFFER_RAM");
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "unable to set Buffer RAM config\n");
        return ret;
    }

    // Set PKA device Master Program RAM config
    ret = pka_dev_set_resource_config(shim, &shim->resources.master_prog_ram,
                                        PKA_MASTER_PROG_RAM_BASE,
                                        PKA_MASTER_PROG_RAM_SIZE,
                                        PKA_DEV_RES_TYPE_MEM,
                                        "PKA_MASTER_PROG_RAM");
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "unable to set Master Program RAM config\n");
        return ret;
    }

    // Set PKA device Master Controller register
    reg_size = PAGE_SIZE;
    reg_base = pka_dev_get_register_base(shim->base,
                                            PKA_MASTER_SEQ_CTRL_ADDR);
    ret = pka_dev_set_resource_config(shim, &shim->resources.master_seq_ctrl,
                                        reg_base, reg_size,
                                        PKA_DEV_RES_TYPE_REG,
                                        "PKA_MASTER_SEQ_CTRL");
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "unable to set Master Controller register "
                                    "config\n");
        return ret;
    }

    // Set PKA device AIC registers
    reg_size = PAGE_SIZE;
    reg_base = pka_dev_get_register_base(shim->base,
                                            AIC_POL_CTRL_ADDR);
    ret = pka_dev_set_resource_config(shim, &shim->resources.aic_csr,
                                      reg_base, reg_size,
                                      PKA_DEV_RES_TYPE_REG,
                                      "PKA_AIC_CSR");
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "unable to set AIC registers config\n");
        return ret;
    }

    // Set PKA device TRNG registers
    reg_size = PAGE_SIZE;
    reg_base = pka_dev_get_register_base(shim->base,
                                            TRNG_OUTPUT_0_ADDR);
    ret = pka_dev_set_resource_config(shim, &shim->resources.trng_csr,
                                      reg_base, reg_size,
                                      PKA_DEV_RES_TYPE_REG,
                                      "PKA_TRNG_CSR");
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "unable to setup the TRNG\n");
        return ret;
    }

    shim->status = PKA_SHIM_STATUS_CREATED;

    return ret;
}

// Delete shim and unset shim resources.
static int pka_dev_delete_shim(pka_dev_shim_t *shim)
{
    int ret = 0;
    pka_dev_res_t *res_buffer_ram, *res_master_prog_ram;
    pka_dev_res_t *res_master_seq_ctrl, *res_aic_csr, *res_trng_csr;

    PKA_DEBUG(PKA_DEV, "PKA device delete shim\n");

    if (shim->status == PKA_SHIM_STATUS_UNDEFINED)
        return ret;

    if (shim->status != PKA_SHIM_STATUS_FINALIZED &&
            shim->status != PKA_SHIM_STATUS_CREATED)
    {
        PKA_ERROR(PKA_DEV, "PKA device status must be finalized\n");
        return -EPERM;
    }

    res_buffer_ram      = &shim->resources.buffer_ram;
    res_master_prog_ram = &shim->resources.master_prog_ram;
    res_master_seq_ctrl = &shim->resources.master_seq_ctrl;
    res_aic_csr         = &shim->resources.aic_csr;
    res_trng_csr        = &shim->resources.trng_csr;

    pka_dev_unset_resource_config(shim, res_buffer_ram);
    pka_dev_unset_resource_config(shim, res_master_prog_ram);
    pka_dev_unset_resource_config(shim, res_master_seq_ctrl);
    pka_dev_unset_resource_config(shim, res_aic_csr);
    pka_dev_unset_resource_config(shim, res_trng_csr);

    kfree(shim->rings);

    shim->status = PKA_SHIM_STATUS_UNDEFINED;

    return ret;
}

static int pka_dev_config_aic_interrupts(pka_dev_res_t *aic_csr_ptr)
{
    int ret = 0;

    uint64_t  csr_reg_base, csr_reg_off;
    void     *csr_reg_ptr;

    if (aic_csr_ptr->status != PKA_DEV_RES_STATUS_MAPPED ||
            aic_csr_ptr->type != PKA_DEV_RES_TYPE_REG)
        return -EPERM;

    PKA_DEBUG(PKA_DEV, "configure the AIC so that all interrupts "
                "are properly recognized\n");

    csr_reg_base = aic_csr_ptr->base;
    csr_reg_ptr  = aic_csr_ptr->ioaddr;

    // Configure the signal polarity for each interrupt.
    csr_reg_off =
            pka_dev_get_register_offset(csr_reg_base, AIC_POL_CTRL_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, PKA_AIC_POL_CTRL_REG_VAL);

    // Configure the signal type for each interrupt
    csr_reg_off =
            pka_dev_get_register_offset(csr_reg_base, AIC_TYPE_CTRL_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, PKA_AIC_TYPE_CTRL_REG_VAL);

    // Set the enable control register
    csr_reg_off =
            pka_dev_get_register_offset(csr_reg_base, AIC_ENABLE_CTRL_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, PKA_AIC_ENABLE_CTRL_REG_VAL);

    // Set the enabled status register
    csr_reg_off =
            pka_dev_get_register_offset(csr_reg_base, AIC_ENABLED_STAT_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, PKA_AIC_ENABLE_STAT_REG_VAL);

    // *TBD* Write PKA_INT_MASK_RESET with 1's for each interrupt bit
    // to allow them to propagate out the interrupt controller.
    // EIP-154 interrupts can still be programmed and observed via polling
    // regardless of whether PKA_INT_MASK is masking out the interrupts or
    // not. The mask is for system propagation, i.e. propagate to the GIC.
    // Bit positions are as follows:
    //  Bit  10   - parity_error_irq (non EIP-154 interrupt)
    //  Bit   9   - trng_irq
    //  Bit   8   - pka_master_irq
    //  Bits  7:4 - pka_queue_*_result_irq
    //  Bits  3:0 - pka_queue_*_empty_irq

    return ret;
}

static int pka_dev_load_image(pka_dev_res_t *res_ptr, const uint32_t *data_buf,
                                    uint32_t size)
{
    uint64_t data_rd;
    int      mismatches;
    int      i, ret = 0;

    if (res_ptr->status != PKA_DEV_RES_STATUS_MAPPED ||
            res_ptr->type != PKA_DEV_RES_TYPE_MEM)
        return -EPERM;

    if (res_ptr->size < size)
    {
        PKA_ERROR(PKA_DEV, "image size greater than memory size\n");
        return -EINVAL;
    }

    for (i = 0; i < size; i++)
        pka_dev_io_write(res_ptr->ioaddr, i * BYTES_PER_DOUBLE_WORD,
                            (uint64_t) data_buf[i]);

    mismatches = 0;
    PKA_DEBUG(PKA_DEV, "PKA DEV: verifying image (%u bytes)\n", size);
    for (i = 0; i < size; i++)
    {
        data_rd = pka_dev_io_read(res_ptr->ioaddr, i * BYTES_PER_DOUBLE_WORD);
        if (data_rd != (uint64_t) data_buf[i])
        {
            mismatches += 1;
            PKA_DEBUG(PKA_DEV, "error while loading image: "
                    "addr:0x%llx expected data: 0x%x actual data: 0x%llx\n",
                    res_ptr->base + i * BYTES_PER_DOUBLE_WORD,
                    data_buf[i], data_rd);
        }
    }

    if (mismatches > 0)
    {
        PKA_PANIC(PKA_DEV, "error while loading image: mismatches: %d\n",
                        mismatches);
        return -EAGAIN;
    }

    return ret;
}

static int
pka_dev_config_master_seq_controller(pka_dev_shim_t *shim,
                                     pka_dev_res_t *master_seq_ctrl_ptr)
{
    pka_dev_res_t *master_prog_ram;
    pka_dev_res_t *aic_csr_ptr;
    void          *aic_reg_ptr;
    void          *master_reg_ptr;

    uint64_t       aic_reg_base;
    uint64_t       aic_reg_off;
    uint64_t       master_reg_base;
    uint64_t       master_reg_off;

    uint32_t       boot_image_size;
    uint32_t       master_image_size;

    uint32_t       pka_master_irq;

    uint64_t       timer;
    uint8_t        status_bits;
    int            ret = 0;

    if (master_seq_ctrl_ptr->status != PKA_DEV_RES_STATUS_MAPPED ||
            master_seq_ctrl_ptr->type != PKA_DEV_RES_TYPE_REG)
        return -EPERM;

    master_reg_base = master_seq_ctrl_ptr->base;
    master_reg_ptr  = master_seq_ctrl_ptr->ioaddr;
    master_reg_off  = pka_dev_get_register_offset(master_reg_base,
                                                    PKA_MASTER_SEQ_CTRL_ADDR);

    PKA_DEBUG(PKA_DEV, "push the EIP-154 master controller into reset\n");
    pka_dev_io_write(master_reg_ptr, master_reg_off,
                            PKA_MASTER_SEQ_CTRL_RESET_VAL);

    // Load boot image into PKA_MASTER_PROG_RAM
    boot_image_size = sizeof(boot_image_data_buf) / sizeof(uint32_t);
    PKA_DEBUG(PKA_DEV, "loading boot image (%d bytes)\n", boot_image_size);

    ret = pka_dev_load_image(&shim->resources.master_prog_ram,
                                boot_image_data_buf, boot_image_size);
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "failed to load boot image\n");
        return ret;
    }

    PKA_DEBUG(PKA_DEV, "take the EIP-154 master controller out of reset\n");
    pka_dev_io_write(master_reg_ptr, master_reg_off, 0);

    // Poll for 'pka_master_irq' bit in AIC_ENABLED_STAT register to indicate
    // sequencer is initialized
    aic_csr_ptr = &shim->resources.aic_csr;
    if (aic_csr_ptr->status != PKA_DEV_RES_STATUS_MAPPED ||
            aic_csr_ptr->type != PKA_DEV_RES_TYPE_REG)
        return -EPERM;

    aic_reg_base = aic_csr_ptr->base;
    aic_reg_ptr  = aic_csr_ptr->ioaddr;
    aic_reg_off  = pka_dev_get_register_offset(aic_reg_base,
                                                AIC_ENABLED_STAT_ADDR);

    pka_master_irq = 0;
    PKA_DEBUG(PKA_DEV, "poll for 'pka_master_irq'\n");
    timer = pka_dev_timer_start(100000);       // 100 msec
    while (pka_master_irq == 0)
    {
        pka_master_irq |= pka_dev_io_read(aic_reg_ptr, aic_reg_off)
                                & PKA_AIC_ENABLED_STAT_MASTER_IRQ_MASK;
        if (pka_dev_timer_done(timer))
        {
            //PKA_PANIC(PKA_DEV, "failed to load firmware\n");
            return -EAGAIN;
        }
    }
    PKA_DEBUG(PKA_DEV, "'pka_master_irq' is active\n");

    // Verify that the EIP-154 boot firmware has finished without errors
    status_bits = (uint8_t)((pka_dev_io_read(master_reg_ptr,
                        master_reg_off) >> PKA_MASTER_SEQ_CTRL_MASTER_IRQ_BIT)
                        & 0xff);
    if (status_bits != PKA_MASTER_SEQ_CTRL_STATUS_BYTE)
    {
        // If the error indication (bit [15]) is set,
        // the EIP-154 boot firmware encountered an error and is stopped.
        if ((status_bits >> (PKA_MASTER_SEQ_CTRL_MASTER_IRQ_BIT - 1)) == 1)
        {
            PKA_ERROR(PKA_DEV,
                "boot firmware encountered an error 0x%x and is stopped\n",
                 status_bits);
            return -EAGAIN;
        }
        PKA_DEBUG(PKA_DEV, "boot firmware in progress %d", status_bits);
    }
    PKA_DEBUG(PKA_DEV, "boot firmware has finished successfully\n");

    PKA_DEBUG(PKA_DEV, "push the EIP-154 master controller into reset\n");
    pka_dev_io_write(master_reg_ptr, master_reg_off,
                        PKA_MASTER_SEQ_CTRL_RESET_VAL);

    // Load Master image into PKA_MASTER_PROG_RAM
    master_image_size = sizeof(master_image_data_buf) / sizeof(uint32_t);
    PKA_DEBUG(PKA_DEV, "loading master image (%d bytes)\n",
                master_image_size);
    master_prog_ram = &shim->resources.master_prog_ram;
    ret = pka_dev_load_image(master_prog_ram, master_image_data_buf,
                                master_image_size);
    if (ret)
    {
        pr_err("PKA DEV: failed to load master image\n");
        return ret;
    }

    PKA_DEBUG(PKA_DEV, "take the EIP-154 master controller out of reset\n");
    pka_dev_io_write(master_reg_ptr, master_reg_off, 0);

    return ret;
}

// Configure ring options.
static int pka_dev_config_ring_options(pka_dev_res_t *buffer_ram_ptr,
                                    uint32_t rings_num, uint8_t ring_priority)
{
    uint64_t control_word;
    uint64_t word_off;
    int      ret = 0;

    if (buffer_ram_ptr->status != PKA_DEV_RES_STATUS_MAPPED ||
            buffer_ram_ptr->type != PKA_DEV_RES_TYPE_MEM)
        return -EPERM;

    if (rings_num > PKA_MAX_NUM_RINGS ||
            rings_num < 1)
    {
        PKA_ERROR(PKA_DEV, "invalid rings number\n");
        return -EINVAL;
    }

    PKA_DEBUG(PKA_DEV, "Configure PKA ring options control word\n");

    // Write PKA_RING_OPTIONS control word located in the PKA_BUFFER_RAM. The
    // value of this word is determined by the PKA I/O block (Shim). Set the
    // number of implemented command/result ring pairs that is available in
    // this EIP-154, encoded as binary value, which is 4.
    control_word  = (uint64_t) 0x0;
    control_word |= ring_priority & 0xff;
    control_word |= ((rings_num - 1) << 8) & 0xff00;
    control_word |= (PKA_RING_OPTIONS_SIGNATURE_BYTE << 24) & 0xff000000;
    word_off = pka_dev_get_word_offset(buffer_ram_ptr->base,
                                PKA_RING_OPTIONS_ADDR, PKA_BUFFER_RAM_SIZE);
    pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, control_word);

    return ret;
}

static int pka_dev_config_trng_clk(pka_dev_res_t *aic_csr_ptr)
{
    int ret = 0;

    uint64_t  csr_reg_base, csr_reg_off;
    uint64_t  timer;
    uint32_t  trng_clk_en = 0;
    void     *csr_reg_ptr;

    if (aic_csr_ptr->status != PKA_DEV_RES_STATUS_MAPPED ||
            aic_csr_ptr->type != PKA_DEV_RES_TYPE_REG)
        return -EPERM;

    PKA_DEBUG(PKA_DEV, "Turn on TRNG clock\n");

    csr_reg_base = aic_csr_ptr->base;
    csr_reg_ptr  = aic_csr_ptr->ioaddr;

    // Enable the TRNG clock in PKA_CLK_FORCE.
    // In general, this register should be left in its default state of all
    // zeroes! Only when the TRNG is directly controlled via the Host slave
    // interface, the engine needs to be turned on using the ’trng_clk_on’
    // bit in this register. In case the TRNG is controlled via internal
    // firmware, this is not required.
    csr_reg_off =
        pka_dev_get_register_offset(csr_reg_base, PKA_CLK_FORCE_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, PKA_CLK_FORCE_TRNG_ON);
    // Check whether the system clock for TRNG engine is enabled. The clock
    // MUST be running to provide access to the TRNG.
    timer = pka_dev_timer_start(100000); // 100 msec
    while (trng_clk_en == 0)
    {
        trng_clk_en |= pka_dev_io_read(csr_reg_ptr, csr_reg_off)
                                & PKA_CLK_FORCE_TRNG_ON;
        if (pka_dev_timer_done(timer))
        {
            PKA_DEBUG(PKA_DEV, "Failed to enable TRNG clock\n");
            return -EAGAIN;
        }
    }
    PKA_DEBUG(PKA_DEV, "'trng_clk_on' is enabled\n");

    return ret;
}

// Configure the TRNG.
static int pka_dev_config_trng(pka_dev_res_t *aic_csr_ptr,
                               pka_dev_res_t *trng_csr_ptr)
{
    int ret = 0;

    uint64_t  csr_reg_base, csr_reg_off;
    void     *csr_reg_ptr;

    if (trng_csr_ptr->status != PKA_DEV_RES_STATUS_MAPPED ||
            trng_csr_ptr->type != PKA_DEV_RES_TYPE_REG)
        return -EPERM;

    PKA_DEBUG(PKA_DEV, "Starting up the TRNG\n");

    ret = pka_dev_config_trng_clk(aic_csr_ptr);
    if (ret)
        return ret;

    csr_reg_base = trng_csr_ptr->base;
    csr_reg_ptr  = trng_csr_ptr->ioaddr;

    // Starting up the TRNG without a DRBG (default configuration);
    // When not using the AES-256 DRBG, the startup sequence is relatively
    // straightforward and the engine will generate data automatically to
    // keep the output register and buffer RAM filled.

    // Make sure the engine is idle.
    csr_reg_off =
            pka_dev_get_register_offset(csr_reg_base, TRNG_CONTROL_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

    // Disable all FROs initially
    csr_reg_off =
            pka_dev_get_register_offset(csr_reg_base, TRNG_FROENABLE_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);
    csr_reg_off =
            pka_dev_get_register_offset(csr_reg_base, TRNG_FRODETUNE_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

    // Write all configuration values in the TRNG_CONFIG and TRNG_ALARMCNT,
    // write zeroes to the TRNG_ALARMMASK and TRNG_ALARMSTOP registers.
    csr_reg_off =
            pka_dev_get_register_offset(csr_reg_base, TRNG_CONFIG_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, PKA_TRNG_CONFIG_REG_VAL);
    csr_reg_off =
            pka_dev_get_register_offset(csr_reg_base, TRNG_ALARMCNT_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, PKA_TRNG_ALARMCNT_REG_VAL);

    csr_reg_off =
            pka_dev_get_register_offset(csr_reg_base, TRNG_ALARMMASK_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);
    csr_reg_off =
            pka_dev_get_register_offset(csr_reg_base, TRNG_ALARMSTOP_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

    // Enable all FROs in the TRNG_FROENABLE register. Note that this can
    // only be done after clearing the TRNG_ALARMSTOP register.
    csr_reg_off =
            pka_dev_get_register_offset(csr_reg_base, TRNG_FROENABLE_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, PKA_TRNG_FROENABLE_REG_VAL);

    // Start the actual engine by setting the 'enable_trng' bit in the
    // TRNG_CONTROL register (also a nice point to set the interrupt mask
    // bits).
    csr_reg_off =
            pka_dev_get_register_offset(csr_reg_base, TRNG_CONTROL_ADDR);
    pka_dev_io_write(csr_reg_ptr, csr_reg_off, PKA_TRNG_CONTROL_REG_VAL);

    // Optionally, when buffer RAM is configured: Set a data available
    // interrupt threshold using the 'load_thresh' and 'blocks_thresh'
    // fields of the TRNG_INTACK register. This allows delaying the data
    // available interrupt until the indicated number of 128-bit words are
    // available in the buffer RAM.

    return ret;
}

// Initialize PKA IO block refered to as shim. It configures shim's
// parameters and prepare resources by mapping corresponding memory.
// The function also configures shim registers and load firmware to
// shim internal rams. The pka_dev_shim_t passed as input is also an
// output. It returns 0 on success, a negative error code on failure.
static int pka_dev_init_shim(pka_dev_shim_t *shim)
{
    uint32_t farm_image_size;

    int      ret = 0;

    if (shim->status != PKA_SHIM_STATUS_CREATED)
    {
        PKA_ERROR(PKA_DEV, "PKA device must be created\n");
        return -EPERM;
    }

    // Configure AIC registers
    ret = pka_dev_config_aic_interrupts(&shim->resources.aic_csr);
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "failed to configure AIC\n");
        return ret;
    }

    // Load Farm image into PKA_BUFFER_RAM for non-High Assurance mode
    // or into PKA_SECURE_RAM for High Assurance mode.
    farm_image_size = sizeof(farm_image_data_buf) / sizeof(uint32_t);
    PKA_DEBUG(PKA_DEV, "loading farm image (%d bytes)\n", farm_image_size);

    ret = pka_dev_load_image(&shim->resources.buffer_ram,
                                farm_image_data_buf, farm_image_size);
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "failed to load farm image\n");
        return ret;
    }

    // Configure EIP-154 Master controller Sequencer
    ret = pka_dev_config_master_seq_controller(shim,
                                        &shim->resources.master_seq_ctrl);
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "failed to configure Master controller "
                                    "Sequencer\n");
        return ret;
    }

    // Configure PKA Ring options control word
    ret = pka_dev_config_ring_options(&shim->resources.buffer_ram,
                                      shim->rings_num, shim->ring_priority);
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "failed to configure ring options\n");
        return ret;
    }

    shim->trng_enabled   = PKA_SHIM_TRNG_ENABLED;
    shim->trng_err_cycle = 0;

    // Configure the TRNG
    ret = pka_dev_config_trng(&shim->resources.aic_csr,
                                &shim->resources.trng_csr);
    if (ret)
    {
        // Keep running without TRNG since it does not hurt, but
        // notify users.
        PKA_ERROR(PKA_DEV, "failed to configure TRNG\n");
        shim->trng_enabled = PKA_SHIM_TRNG_DISABLED;
    }

    shim->busy_ring_num  = 0;
    shim->status         = PKA_SHIM_STATUS_INITIALIZED;

    return ret;
}

// Release a given shim.
static int pka_dev_release_shim(pka_dev_shim_t *shim)
{
    int ret = 0;

    uint32_t ring_idx;

    if (shim->status != PKA_SHIM_STATUS_INITIALIZED &&
            shim->status != PKA_SHIM_STATUS_STOPPED)
    {
        PKA_ERROR(PKA_DEV, "PKA device must be initialized or stopped\n");
        return -EPERM;
    }

    // Release rings which belong to the shim. The operating system might
    // release ring devices before shim devices. The global configuration
    // must be checked before proceeding to the release of ring devices.
    if (pka_gbl_config.dev_rings_cnt)
    {
        for (ring_idx = 0; ring_idx < shim->rings_num; ring_idx++)
        {
            ret = pka_dev_release_ring(shim->rings[ring_idx]);
            if (ret)
            {
                PKA_ERROR(PKA_DEV, "failed to release ring %d\n", ring_idx);
                return ret;
            }
        }
    }

    shim->busy_ring_num = 0;
    shim->status        = PKA_SHIM_STATUS_FINALIZED;

    return ret;
}

// Return the ring associated with the given identifier.
pka_dev_ring_t *pka_dev_get_ring(uint32_t ring_id)
{
    return pka_gbl_config.dev_rings[ring_id];
}

// Return the shim associated with the given identifier.
pka_dev_shim_t *pka_dev_get_shim(uint32_t shim_id)
{
    return pka_gbl_config.dev_shims[shim_id];
}


static pka_dev_ring_t *__pka_dev_register_ring(uint32_t ring_id,
                                               uint32_t shim_id)
{
    pka_dev_shim_t *shim;
    pka_dev_ring_t *ring;

    int ret;

    shim = pka_dev_get_shim(shim_id);
    if (!shim)
        return NULL;

    ring = kzalloc(sizeof(pka_dev_ring_t), GFP_KERNEL);
    if (!ring)
        return ring;

    ring->status = PKA_DEV_RING_STATUS_UNDEFINED;

    // Initialize ring.
    ret = pka_dev_init_ring(ring, ring_id, shim);
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "failed to initialize ring %d\n", ring_id);
        pka_dev_release_ring(ring);
        kfree(ring);
        return NULL;
    }

    return ring;
}

pka_dev_ring_t *pka_dev_register_ring(uint32_t ring_id, uint32_t shim_id)
{
    pka_dev_ring_t *ring;

    ring = __pka_dev_register_ring(ring_id, shim_id);
    if (ring)
    {
        pka_gbl_config.dev_rings[ring->ring_id]  = ring;
        pka_gbl_config.dev_rings_cnt            += 1;
    }

    return ring;
}

static int __pka_dev_unregister_ring(pka_dev_ring_t *ring)
{
    int ret;

    if (!ring)
       return -EINVAL;

    // Release ring
    ret = pka_dev_release_ring(ring);
    if (ret)
        return ret;

    kfree(ring);

    return ret;
}

int pka_dev_unregister_ring(pka_dev_ring_t *ring)
{
    pka_gbl_config.dev_rings[ring->ring_id]  = NULL;
    pka_gbl_config.dev_rings_cnt            -= 1;

    return __pka_dev_unregister_ring(ring);
}

static pka_dev_shim_t *__pka_dev_register_shim(uint32_t shim_id,
                                               uint64_t shim_base,
                                               uint64_t shim_size)
{
    pka_dev_shim_t *shim;

    uint8_t  split;
    int      ret = 0;

    PKA_DEBUG(PKA_DEV, "register shim id=%u, start=0x%llx end=0x%llx\n",
               shim_id, shim_base, shim_base + shim_size);

    shim = kzalloc(sizeof(pka_dev_shim_t), GFP_KERNEL);
    if (!shim)
        return shim;

    // Shim state MUST be set to undefined before calling 'pka_dev_create_shim'
    // function
    shim->status = PKA_SHIM_STATUS_UNDEFINED;

    // Set the Window RAM user mode
    split = PKA_SPLIT_WINDOW_RAM_MODE;

    // Create PKA shim
    ret = pka_dev_create_shim(shim, shim_id, shim_base, shim_size, split);
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "failed to create shim %u\n", shim_id);
        pka_dev_delete_shim(shim);
        kfree(shim);
        return NULL;
    }

    // Initialize PKA shim
    ret = pka_dev_init_shim(shim);
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "failed to init shim %u\n", shim_id);
        pka_dev_release_shim(shim);
        pka_dev_delete_shim(shim);
        kfree(shim);
        return NULL;
    }

    return shim;
}

pka_dev_shim_t *pka_dev_register_shim(uint32_t shim_id, uint64_t shim_base,
                           uint64_t shim_size)
{
    pka_dev_shim_t *shim;

    shim = __pka_dev_register_shim(shim_id, shim_base, shim_size);
    if (shim)
    {
        pka_gbl_config.dev_shims[shim->shim_id]  = shim;
        pka_gbl_config.dev_shims_cnt            += 1;
    }

    return shim;
}

static int __pka_dev_unregister_shim(pka_dev_shim_t *shim)
{
    int ret = 0;

    if (!shim)
        return -EINVAL;

    // Release shim
    ret = pka_dev_release_shim(shim);
    if (ret)
        return ret;

    // Delete shim
    ret = pka_dev_delete_shim(shim);
    if (ret)
        return ret;

    kfree(shim);

    return ret;
}

int pka_dev_unregister_shim(pka_dev_shim_t *shim)
{
    pka_gbl_config.dev_shims[shim->shim_id]  = NULL;
    pka_gbl_config.dev_shims_cnt            -= 1;

    return __pka_dev_unregister_shim(shim);
}

static bool pka_dev_trng_shutdown_oflo(pka_dev_res_t *trng_csr_ptr,
                                       uint64_t      *err_cycle)
{
    uint64_t  csr_reg_base, csr_reg_off, csr_reg_value;
    uint64_t  curr_cycle_cnt, fro_stopped_mask, fro_enabled_mask;
    void     *csr_reg_ptr;

    csr_reg_base = trng_csr_ptr->base;
    csr_reg_ptr  = trng_csr_ptr->ioaddr;

    csr_reg_off   =
            pka_dev_get_register_offset(csr_reg_base, TRNG_STATUS_ADDR);
    csr_reg_value = pka_dev_io_read(csr_reg_ptr, csr_reg_off);

    if (csr_reg_value & PKA_TRNG_STATUS_SHUTDOWN_OFLO)
    {
        curr_cycle_cnt = get_cycles();
        // See if any FROs were shut down. If they were, toggle bits in the
        // FRO detune register and reenable the FROs.
        csr_reg_off   = pka_dev_get_register_offset(csr_reg_base,
                                                    TRNG_ALARMSTOP_ADDR);
        fro_stopped_mask = pka_dev_io_read(csr_reg_ptr, csr_reg_off);
        if (fro_stopped_mask)
        {
            csr_reg_off      = pka_dev_get_register_offset(csr_reg_base,
                                                    TRNG_FROENABLE_ADDR);
            fro_enabled_mask = pka_dev_io_read(csr_reg_ptr, csr_reg_off);

            csr_reg_off      = pka_dev_get_register_offset(csr_reg_base,
                                                    TRNG_FRODETUNE_ADDR);
            pka_dev_io_write(csr_reg_ptr, csr_reg_off, fro_stopped_mask);

            csr_reg_off      = pka_dev_get_register_offset(csr_reg_base,
                                                    TRNG_FROENABLE_ADDR);
            pka_dev_io_write(csr_reg_ptr, csr_reg_off,
                                fro_stopped_mask | fro_enabled_mask);
        }

        // Reset the error
        csr_reg_off = pka_dev_get_register_offset(csr_reg_base,
                                                    TRNG_ALARMMASK_ADDR);
        pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

        csr_reg_off = pka_dev_get_register_offset(csr_reg_base,
                                                    TRNG_ALARMSTOP_ADDR);
        pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

        csr_reg_off = pka_dev_get_register_offset(csr_reg_base,
                                                    TRNG_INTACK_ADDR);
        pka_dev_io_write(csr_reg_ptr, csr_reg_off,
                            PKA_TRNG_STATUS_SHUTDOWN_OFLO);

        // If we're seeing this error again within about a second,
        // the hardware is malfunctioning. Disable the trng and return
        // an error.
        if (*err_cycle && (curr_cycle_cnt - *err_cycle < 1000000000))
        {
            csr_reg_off    = pka_dev_get_register_offset(csr_reg_base,
                                                    TRNG_CONTROL_ADDR);
            csr_reg_value  = pka_dev_io_read(csr_reg_ptr, csr_reg_off);
            csr_reg_value &= ~PKA_TRNG_CONTROL_REG_VAL;
            pka_dev_io_write(csr_reg_ptr, csr_reg_off, csr_reg_value);
            return false;
        }

        *err_cycle = curr_cycle_cnt;
    }

    return true;
}

int pka_dev_trng_read(pka_dev_shim_t *shim, uint32_t *data, uint32_t cnt)
{
    int ret = 0;

    pka_dev_res_t *trng_csr_ptr;
    uint64_t       csr_reg_base, csr_reg_off, csr_reg_value;
    uint64_t       timer;
    uint32_t       data_idx, word_cnt;
    uint8_t        output_idx, trng_ready = 0;
    void          *csr_reg_ptr;

    if (!shim || ! data || (cnt % PKA_TRNG_OUTPUT_CNT != 0))
        return -EINVAL;

    if (!cnt)
        return ret;

    trng_csr_ptr = &shim->resources.trng_csr;

    if (trng_csr_ptr->status != PKA_DEV_RES_STATUS_MAPPED ||
            trng_csr_ptr->type != PKA_DEV_RES_TYPE_REG)
        return -EPERM;

    csr_reg_base = trng_csr_ptr->base;
    csr_reg_ptr  = trng_csr_ptr->ioaddr;

    if (!pka_dev_trng_shutdown_oflo(trng_csr_ptr,
                                    &shim->trng_err_cycle))
        return -EWOULDBLOCK;

    // Determine the number of 32-bit words.
    word_cnt = cnt >> 2;

    for (data_idx = 0; data_idx < word_cnt; data_idx++)
    {
        output_idx = data_idx % PKA_TRNG_OUTPUT_CNT;
        // Tell the hardware to advance
        if (output_idx == 0)
        {
            csr_reg_off = pka_dev_get_register_offset(csr_reg_base,
                                                        TRNG_INTACK_ADDR);
            pka_dev_io_write(csr_reg_ptr, csr_reg_off, PKA_TRNG_STATUS_READY);
        }

        // Wait until a data word is available in the TRNG_OUTPUT_X
        // registers (using the interrupt and/or 'ready' status bit in the
        // TRNG_STATUS register. The only way this would hang if the TRNG
        // never initialized, and we would not call this function if that
        // happened.
        timer = pka_dev_timer_start(1000000); // 1000 ms
        csr_reg_off =
                pka_dev_get_register_offset(csr_reg_base, TRNG_STATUS_ADDR);
        while (trng_ready == 0)
        {
            csr_reg_value = pka_dev_io_read(csr_reg_ptr, csr_reg_off);
            trng_ready    = csr_reg_value & PKA_TRNG_STATUS_READY;

            if (pka_dev_timer_done(timer))
            {
                PKA_DEBUG(PKA_DEV,
                    "Shim %u got error obtaining random number\n",
                                shim->shim_id);
                return -EBUSY;
            }
        }

        // Read the registers
        csr_reg_off    =  pka_dev_get_register_offset(csr_reg_base,
                                    TRNG_OUTPUT_0_ADDR + (output_idx * 0x8));
        csr_reg_value  = pka_dev_io_read(csr_reg_ptr, csr_reg_off);
        data[data_idx] = (uint32_t) csr_reg_value;
    }

    return ret;
}

bool pka_dev_has_trng(pka_dev_shim_t *shim)
{
    if (!shim)
        return false;

    return (shim->trng_enabled == PKA_SHIM_TRNG_ENABLED);
}

#endif // __KERNEL__

#ifndef __KERNEL__
static uint32_t next_ring_id = 0;

// Verify if there is an available ring.
bool pka_dev_has_avail_ring(pka_ring_info_t *ring_info,
                            uint32_t         rings_num)
{
    uint32_t errors;

    errors = 0;

    while ((PKA_MAX_NUM_RINGS - errors) >= rings_num)
    {
        if (next_ring_id == (PKA_MAX_NUM_RINGS - 1))
        {
            ring_info->ring_id = next_ring_id;
            next_ring_id       = 0;
        }
        else
        {
            ring_info->ring_id  = next_ring_id * PKA_MAX_NUM_IO_BLOCKS;
            ring_info->ring_id %= PKA_MAX_NUM_RINGS - 1;
            next_ring_id       += 1;
        }

        // Open the ring corresponding to the given ring identifier.
        if (pka_dev_open_ring(ring_info))
        {
            PKA_ERROR(PKA_DEV, "failed to open ring %d\n",
                        ring_info->ring_id);
            pka_dev_close_ring(ring_info);
            errors += 1;
            continue;
        }

        // Map the ring
        if (pka_dev_mmap_ring(ring_info))
        {
            PKA_ERROR(PKA_DEV, "failed to map ring %d\n",
                        ring_info->ring_id);
            pka_dev_close_ring(ring_info);
            errors += 1;
            continue;
        }

        // Get ring information
        if (pka_dev_get_ring_info(ring_info))
            PKA_ERROR(PKA_DEV, "failed to get ring %d information\n",
                            ring_info->ring_id);

        return true;
    }

    return false;
}

// Return ring information and initialize ring descriptors.
int pka_dev_get_ring_info(pka_ring_info_t *ring_info)
{
    pka_dev_hw_ring_info_t hw_ring_info;
    uint32_t               desc_size;
    uint32_t               operand_base;
    uint32_t               operand_ring_len;

    int ret = 0;

    if (!ring_info)
        return -EINVAL;

    // Get ring parameters
    ret = ioctl(ring_info->fd, PKA_VFIO_GET_RING_INFO, &hw_ring_info);
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "failed to get ring information\n");
        return ret;
    }

    ring_info->ring_desc.cmd_ring_base  = hw_ring_info.cmmd_base;
    ring_info->ring_desc.rslt_ring_base = hw_ring_info.rslt_base;
    ring_info->ring_desc.cmd_idx        = 0;
    ring_info->ring_desc.rslt_idx       = 0;

    // Ring descriptor size should be equal to CMD_DESC_SIZE (64 bytes).
    desc_size = hw_ring_info.host_desc_size * BYTES_PER_WORD;

    ring_info->ring_desc.desc_size      = desc_size;
    ring_info->ring_desc.num_descs      = hw_ring_info.size + 1;
    ring_info->ring_desc.cmd_desc_cnt   = 0;
    ring_info->ring_desc.rslt_desc_cnt  = 0;
    ring_info->ring_desc.cmd_desc_mask  = 0;

    // This code assumes that Data Memory is in the bottom 14KB of the "PKA
    // window RAM" and so the addresses for the rings start at offset 0x3800.
    operand_base     = hw_ring_info.cmmd_base & ~(ring_info->mem_size - 1);
    operand_ring_len = PKA_WINDOW_RAM_DATA_MEM_SIZE;

    ring_info->ring_desc.operands_base  = operand_base;
    ring_info->ring_desc.operands_end   = operand_base + operand_ring_len;

    return ret;
}


// Returns a prefix associated to the given ring. Note that prefix is set
// according to either the linux device-tree (DT) and the ACPI tables.
static char *pka_dev_get_ring_prefix(uint32_t ring_id, bool dt)
{
    switch(ring_id)
    {
    case 0 ... 3:
        return ((dt) ? PKA_DEV_RING_DT_PREFIX_0 : PKA_DEV_RING_ACPI_PREFIX);
    case 4 ... 7:
        return ((dt) ? PKA_DEV_RING_DT_PREFIX_1 : PKA_DEV_RING_ACPI_PREFIX);
    case 8 ... 11:
        return ((dt) ? PKA_DEV_RING_DT_PREFIX_2 : PKA_DEV_RING_ACPI_PREFIX);
    case 12 ... 15:
        return ((dt) ? PKA_DEV_RING_DT_PREFIX_3 : PKA_DEV_RING_ACPI_PREFIX);

    default:
        PKA_DEBUG(PKA_DEV,
                  "failed to return ring %d prefix\n", ring_id);
        return NULL;
    }
}

// split group name into tokens
static int pka_dev_split_group_name(char  *string,
                                    int    string_len,
                                    char **tokens,
                                    int    max_tokens,
                                    char   delim)
{
    int i, tok = 0;
    int tok_start = 1; // first token is right at start of string

    if (string == NULL || tokens == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    for (i = 0; i < string_len; i++)
    {
        if (string[i] == '\0' || tok >= max_tokens)
            break;
        if (tok_start)
        {
            tok_start = 0;
            tokens[tok++] = &string[i];
        }
        if (string[i] == delim)
        {
            string[i] = '\0';
            tok_start = 1;
        }
    }
    return tok;
}

// Get IOMMU group number for a ring device
static int pka_dev_get_group_no(const char *sysfs_base,
                                const char *dev_addr,
                                int        *iommu_group_no)
{
    char linkname[PATH_MAX];
    char filename[PATH_MAX];
    char *tok[16], *group_tok, *end;
    int ret;

    memset(linkname, 0, sizeof(linkname));
    memset(filename, 0, sizeof(filename));

    // try to find out IOMMU group for this device
    snprintf(linkname, sizeof(linkname), "%s/%s/iommu_group",
                sysfs_base, dev_addr);

    ret = readlink(linkname, filename, sizeof(filename));
    // if the link doesn't exist, no VFIO for us
    if (ret < 0)
    {
        PKA_DEBUG(PKA_DEV, "%s: readlink failed\n", linkname);
        return 0;
    }

    ret = pka_dev_split_group_name(filename, sizeof(filename),
            tok, PKA_DIM(tok), '/');
    if (ret <= 0)
    {
        PKA_DEBUG(PKA_DEV, "%s cannot get IOMMU group\n", dev_addr);
        return -1;
    }

    // IOMMU group is always the last token
    errno           = 0;
    group_tok       = tok[ret - 1];
    end             = group_tok;
    *iommu_group_no = strtol(group_tok, &end, 10);
    if ((end != group_tok && *end != '\0') || errno != 0)
    {
        PKA_DEBUG(PKA_DEV, "%s error parsing IOMMU number!\n", dev_addr);
        return -1;
    }

    return 1;
}
#endif

#ifdef __KERNEL__
// Syscall to open ring.
int __pka_dev_open_ring(uint32_t ring_id)
{
    pka_dev_shim_t *shim;
    pka_dev_ring_t *ring;

    int ret = 0;

    if (pka_gbl_config.dev_rings_cnt == 0)
        return -EPERM;

    ring = pka_dev_get_ring(ring_id);
    if (!ring || !ring->shim)
        return -ENXIO;

    shim = ring->shim;

    if (shim->status == PKA_SHIM_STATUS_UNDEFINED ||
          shim->status == PKA_SHIM_STATUS_CREATED ||
          shim->status == PKA_SHIM_STATUS_FINALIZED)
        return -EPERM;

    if (ring->status != PKA_DEV_RING_STATUS_INITIALIZED)
        return -EPERM;

    // Set ring information words.
    ret = pka_dev_set_ring_info(ring);
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "failed to set ring information\n");
        return -EWOULDBLOCK;
    }

    if (shim->busy_ring_num == 0)
        shim->status = PKA_SHIM_STATUS_RUNNING;

    ring->status = PKA_DEV_RING_STATUS_BUSY;
    shim->busy_ring_num += 1;

    return ret;
}
#endif

// Open ring.
int pka_dev_open_ring(pka_ring_info_t *ring_info)
{
#ifdef __KERNEL__
    return __pka_dev_open_ring(ring_info->ring_id);
#else

    struct vfio_group_status  group_status;

    char  file[32];
    char  ring_name[32];
    char *ring_prefix;
    int   iommu_group_no;
    int   error = -EWOULDBLOCK;

    group_status.argsz = sizeof(group_status);

    // Get ring device name.
    ring_prefix = pka_dev_get_ring_prefix(ring_info->ring_id, false);
    if (!ring_prefix)
    {
        PKA_ERROR(PKA_DEV, "failed to get ring %d device name\n",
                    ring_info->ring_id);
        return error;
    }

    // Get group number
    snprintf(ring_name, sizeof(ring_name), ring_prefix, ring_info->ring_id);
    if (!pka_dev_get_group_no(PKA_SYSFS_RING_DEVICES, ring_name,
                                    &iommu_group_no))
    {
        ring_prefix = pka_dev_get_ring_prefix(ring_info->ring_id, false);
        snprintf(ring_name, sizeof(ring_name), ring_prefix,
                            ring_info->ring_id);

        if (!pka_dev_get_group_no(PKA_SYSFS_RING_DEVICES, ring_name,
                                        &iommu_group_no))
        {
            PKA_ERROR(PKA_DEV, "failed to get group number for ring %d\n",
                        ring_info->ring_id);
            return error;
        }
    }

    // Open the group
    snprintf(file, sizeof(file), PKA_VFIO_GROUP_FMT, iommu_group_no);
    ring_info->group = open(file, O_RDWR);
    if (ring_info->group < 0)
    {
        PKA_ERROR(PKA_DEV,
                  "cannot open the VFIO group for ring %d\n",
                    ring_info->ring_id);
        return error;
    }

    // Test if the group is viable and available
    ioctl(ring_info->group, VFIO_GROUP_GET_STATUS, &group_status);
    if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE))
    {
        // Group is not viable (ie, not all devices bound for VFIO)
        close(ring_info->group);
        return error;
    }

    // check if group does not have a container yet
    if (!(group_status.flags & VFIO_GROUP_FLAGS_CONTAINER_SET))
    {
        // Add the group to the container
        if (0 > ioctl(ring_info->group, VFIO_GROUP_SET_CONTAINER,
                            &ring_info->container))
        {
            close(ring_info->group);
            return error;
        }
        // Set an IOMMU type. Needs to be done only once, only when at least
        // one group is assigned to a container and only in primary process.
        ioctl(ring_info->container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);
    }

    // Get a file descriptor for the device
    ring_info->fd = ioctl(ring_info->group, VFIO_GROUP_GET_DEVICE_FD,
                                ring_name);
    if (ring_info->fd < 0)
    {
        PKA_ERROR(PKA_DEV, "failed to get file descriptor for ring %d\n",
                    ring_info->ring_id);
        return error;
    }

    return 0;
#endif
}

#ifdef __KERNEL__
// Syscall to close ring.
int __pka_dev_close_ring(uint32_t ring_id)
{
    pka_dev_shim_t *shim;
    pka_dev_ring_t *ring;

    int ret = 0;

    if (pka_gbl_config.dev_rings_cnt == 0)
        return -EPERM;

    ring = pka_dev_get_ring(ring_id);
    if (!ring || !ring->shim)
        return -ENXIO;

    shim = ring->shim;

    if (shim->status != PKA_SHIM_STATUS_RUNNING &&
            ring->status != PKA_DEV_RING_STATUS_BUSY)
        return -EPERM;

    ring->status         = PKA_DEV_RING_STATUS_INITIALIZED;
    shim->busy_ring_num -= 1;

    if (shim->busy_ring_num == 0)
        shim->status = PKA_SHIM_STATUS_STOPPED;

    return ret;
}
#endif

// Close ring.
int pka_dev_close_ring(pka_ring_info_t *ring_info)
{
    if (ring_info)
    {
#ifdef __KERNEL__
        return __pka_dev_close_ring(ring_info->ring_id);
#else
        // Close ring file descriptor.
        close(ring_info->fd);

        // Close ring group.
        close(ring_info->group);
#endif
    }

    return 0;
}

#ifdef __KERNEL__
// Syscall to map ring into memory (kernel-space).
static int __pka_dev_mmap_ring(uint32_t ring_id)
{
    //not implemented
    return -1;
}
#endif

// Map ring into memory (user-space).
int pka_dev_mmap_ring(pka_ring_info_t *ring_info)
{
#ifdef __KERNEL__
    return __pka_dev_mmap_ring(ring_info->ring_id);
#else

    pka_dev_region_info_t region_info;

    int ret = 0;

    if (!ring_info)
        return -EINVAL;

    // Get ring region information
    ret = ioctl(ring_info->fd, PKA_VFIO_GET_REGION_INFO, &region_info);
    if (ret)
    {
        PKA_ERROR(PKA_DEV, "failed to get ring region info\n");
        return ret;
    }

    ring_info->reg_size = region_info.reg_size;
    ring_info->reg_off  = region_info.reg_offset;

    // Set Control/Status registers
    ring_info->reg_ptr = mmap(NULL, ring_info->reg_size,
                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                     ring_info->fd, ring_info->reg_off);
    if (ring_info->reg_ptr == MAP_FAILED)
    {
        PKA_ERROR(PKA_DEV, "ring %d failed to map counters\n",
                    ring_info->ring_id);
        return -ENOMEM;
    }

    PKA_DEBUG(PKA_DEV, "ring %d - counters mapped\n", ring_info->ring_id);

    ring_info->mem_size = region_info.mem_size;
    ring_info->mem_off  = region_info.mem_offset;

    // Set Window RAM config
    ring_info->mem_ptr = mmap(NULL, ring_info->mem_size,
                                    PROT_READ | PROT_WRITE, MAP_SHARED,
                                    ring_info->fd, ring_info->mem_off);
    if (ring_info->mem_ptr == MAP_FAILED)
    {
        PKA_ERROR(PKA_DEV, "ring %d failed to map window RAM\n",
                    ring_info->ring_id);
        return -ENOMEM;
    }

    PKA_DEBUG(PKA_DEV, "ring %d - window RAM mapped\n", ring_info->ring_id);

    return ret;
#endif
}

#ifdef __KERNEL__
// Syscall to unmap ring (kernel-space).
static int __pka_dev_munmap_ring(uint32_t ring_id)
{
    //not implemented
    return -1;
}
#endif

// Unmap ring (user-space).
int pka_dev_munmap_ring(pka_ring_info_t *ring_info)
{
    if (ring_info)
    {
#ifdef __KERNEL__
        return __pka_dev_munmap_ring(ring_info->ring_id);
#else
        munmap(ring_info->mem_ptr, ring_info->mem_size);
        munmap(ring_info->reg_ptr, ring_info->reg_size);
#endif
    }

    return 0;
}

