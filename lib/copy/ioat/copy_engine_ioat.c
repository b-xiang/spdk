/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <errno.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_lcore.h>
#include <rte_debug.h>

#include "spdk/copy_engine.h"
#include "spdk/vtophys.h"
#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/event.h"
#include "spdk/pci.h"
#include "spdk/io_channel.h"

#include "spdk/ioat.h"

#define IOAT_MAX_CHANNELS		64

struct ioat_device {
	struct spdk_ioat_chan *ioat;
	bool is_allocated;
	/** linked list pointer for device list */
	TAILQ_ENTRY(ioat_device) tailq;
};

static TAILQ_HEAD(, ioat_device) g_devices = TAILQ_HEAD_INITIALIZER(g_devices);
static int g_unbindfromkernel = 0;
static pthread_mutex_t g_ioat_mutex = PTHREAD_MUTEX_INITIALIZER;

struct ioat_whitelist {
	uint32_t bus;
	uint32_t dev;
	uint32_t func;
};

struct ioat_io_channel {
	struct spdk_ioat_chan	*ioat_ch;
	struct ioat_device	*ioat_dev;
	struct spdk_poller	*poller;
};

static int
ioat_find_dev_by_whitelist_bdf(struct spdk_pci_device *dev,
			       struct ioat_whitelist *whitelist,
			       int num_whitelist_devices)
{
	int i;

	for (i = 0; i < num_whitelist_devices; i++) {
		if (spdk_pci_device_get_bus(dev) == whitelist[i].bus &&
		    spdk_pci_device_get_dev(dev) == whitelist[i].dev &&
		    spdk_pci_device_get_func(dev) == whitelist[i].func)
			return 1;
	}
	return 0;
}

static struct ioat_device *
ioat_allocate_device(void)
{
	struct ioat_device *dev;

	pthread_mutex_lock(&g_ioat_mutex);
	TAILQ_FOREACH(dev, &g_devices, tailq) {
		if (!dev->is_allocated) {
			dev->is_allocated = true;
			pthread_mutex_unlock(&g_ioat_mutex);
			return dev;
		}
	}
	pthread_mutex_unlock(&g_ioat_mutex);

	return NULL;
}

static void
ioat_free_device(struct ioat_device *dev)
{
	pthread_mutex_lock(&g_ioat_mutex);
	dev->is_allocated = false;
	pthread_mutex_unlock(&g_ioat_mutex);
}

struct ioat_task {
	copy_completion_cb	cb;
};

static int copy_engine_ioat_init(void);
static void copy_engine_ioat_exit(void);

static int
copy_engine_ioat_get_ctx_size(void)
{
	return sizeof(struct ioat_task) + sizeof(struct copy_task);
}

SPDK_COPY_MODULE_REGISTER(copy_engine_ioat_init, copy_engine_ioat_exit, NULL,
			  copy_engine_ioat_get_ctx_size)

static void
copy_engine_ioat_exit(void)
{
	struct ioat_device *dev;

	while (!TAILQ_EMPTY(&g_devices)) {
		dev = TAILQ_FIRST(&g_devices);
		TAILQ_REMOVE(&g_devices, dev, tailq);
		spdk_ioat_detach(dev->ioat);
		ioat_free_device(dev);
		rte_free(dev);
	}
	return;
}

static void
ioat_done(void *cb_arg)
{
	struct copy_task *copy_req;
	struct ioat_task *ioat_task = cb_arg;

	copy_req = (struct copy_task *)
		   ((uintptr_t)ioat_task -
		    offsetof(struct copy_task, offload_ctx));

	ioat_task->cb(copy_req, 0);
}

static int64_t
ioat_copy_submit(void *cb_arg, struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes,
		 copy_completion_cb cb)
{
	struct ioat_task *ioat_task = (struct ioat_task *)cb_arg;
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);

	RTE_VERIFY(ioat_ch->ioat_ch != NULL);

	ioat_task->cb = cb;

	return spdk_ioat_submit_copy(ioat_ch->ioat_ch, ioat_task, ioat_done, dst, src, nbytes);
}

static int64_t
ioat_copy_submit_fill(void *cb_arg, struct spdk_io_channel *ch, void *dst, uint8_t fill,
		      uint64_t nbytes, copy_completion_cb cb)
{
	struct ioat_task *ioat_task = (struct ioat_task *)cb_arg;
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);
	uint64_t fill64 = 0x0101010101010101ULL * fill;

	RTE_VERIFY(ioat_ch->ioat_ch != NULL);

	ioat_task->cb = cb;

	return spdk_ioat_submit_fill(ioat_ch->ioat_ch, ioat_task, ioat_done, dst, fill64, nbytes);
}

static void
ioat_poll(void *arg)
{
	struct spdk_ioat_chan *chan = arg;

	spdk_ioat_process_events(chan);
}

static struct spdk_io_channel *ioat_get_io_channel(uint32_t priority);

static struct spdk_copy_engine ioat_copy_engine = {
	.copy		= ioat_copy_submit,
	.fill		= ioat_copy_submit_fill,
	.get_io_channel	= ioat_get_io_channel,
};

static int
ioat_create_cb(void *io_device, uint32_t priority, void *ctx_buf, void *unique_ctx)
{
	struct ioat_io_channel *ch = ctx_buf;
	struct ioat_device *ioat_dev;

	ioat_dev = ioat_allocate_device();
	if (ioat_dev == NULL) {
		return -1;
	}

	ch->ioat_dev = ioat_dev;
	ch->ioat_ch = ioat_dev->ioat;
	spdk_poller_register(&ch->poller, ioat_poll, ch->ioat_ch,
			     spdk_app_get_current_core(), NULL, 0);
	return 0;
}

static void
ioat_destroy_cb(void *io_device, void *ctx_buf)
{
	struct ioat_io_channel *ch = ctx_buf;

	ioat_free_device(ch->ioat_dev);
	spdk_poller_unregister(&ch->poller, NULL);
}

static struct spdk_io_channel *
ioat_get_io_channel(uint32_t priority)
{
	return spdk_get_io_channel(&ioat_copy_engine, priority, false, NULL);
}

struct ioat_probe_ctx {
	int num_whitelist_devices;
	struct ioat_whitelist whitelist[IOAT_MAX_CHANNELS];
};

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
	struct ioat_probe_ctx *ctx = cb_ctx;

	SPDK_NOTICELOG(" Found matching device at %d:%d:%d vendor:0x%04x device:0x%04x\n   name:%s\n",
		       spdk_pci_device_get_bus(pci_dev),
		       spdk_pci_device_get_dev(pci_dev),
		       spdk_pci_device_get_func(pci_dev),
		       spdk_pci_device_get_vendor_id(pci_dev),
		       spdk_pci_device_get_device_id(pci_dev),
		       spdk_pci_device_get_device_name(pci_dev));

	if (ctx->num_whitelist_devices > 0 &&
	    !ioat_find_dev_by_whitelist_bdf(pci_dev, ctx->whitelist, ctx->num_whitelist_devices)) {
		return false;
	}

	if (spdk_pci_device_has_non_uio_driver(pci_dev)) {
		if (g_unbindfromkernel && ctx->num_whitelist_devices > 0) {
			if (spdk_pci_device_switch_to_uio_driver(pci_dev)) {
				return false;
			}
		} else {
			SPDK_WARNLOG("Device has kernel ioat driver attached, skipping...\n");
			return false;
		}
	} else {
		if (spdk_pci_device_bind_uio_driver(pci_dev)) {
			SPDK_WARNLOG("Device %s %d:%d:%d bind to uio driver failed\n",
				     spdk_pci_device_get_device_name(pci_dev),
				     spdk_pci_device_get_bus(pci_dev),
				     spdk_pci_device_get_dev(pci_dev),
				     spdk_pci_device_get_func(pci_dev));
			return false;
		}
	}

	/* Claim the device in case conflict with other process */
	if (spdk_pci_device_claim(pci_dev) != 0) {
		return false;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_ioat_chan *ioat)
{
	struct ioat_device *dev;

	dev = rte_malloc(NULL, sizeof(*dev), 0);
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return;
	}

	dev->ioat = ioat;
	TAILQ_INSERT_TAIL(&g_devices, dev, tailq);
}

static int
copy_engine_ioat_init(void)
{
	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "Ioat");
	const char *val, *pci_bdf;
	int i;
	struct ioat_probe_ctx probe_ctx = {};

	if (sp != NULL) {
		val = spdk_conf_section_get_val(sp, "Disable");
		if (val != NULL) {
			/* Disable Ioat */
			if (!strcmp(val, "Yes")) {
				return 0;
			}
		}
		/*Init the whitelist*/
		for (i = 0; i < IOAT_MAX_CHANNELS; i++) {
			pci_bdf = spdk_conf_section_get_nmval(sp, "Whitelist", i, 0);
			if (!pci_bdf)
				break;
			sscanf(pci_bdf, "%02x:%02x.%1u",
			       &probe_ctx.whitelist[probe_ctx.num_whitelist_devices].bus,
			       &probe_ctx.whitelist[probe_ctx.num_whitelist_devices].dev,
			       &probe_ctx.whitelist[probe_ctx.num_whitelist_devices].func);
			probe_ctx.num_whitelist_devices++;
		}

		val = spdk_conf_section_get_val(sp, "UnbindFromKernel");
		if (val != NULL) {
			if (!strcmp(val, "Yes")) {
				g_unbindfromkernel = 1;
			}
		}
	}

	if (spdk_ioat_probe(&probe_ctx, probe_cb, attach_cb) != 0) {
		SPDK_ERRLOG("spdk_ioat_probe() failed\n");
		return -1;
	}

	SPDK_NOTICELOG("Ioat Copy Engine Offload Enabled\n");
	spdk_copy_engine_register(&ioat_copy_engine);
	spdk_io_device_register(&ioat_copy_engine, ioat_create_cb, ioat_destroy_cb,
				sizeof(struct ioat_io_channel));

	return 0;
}
