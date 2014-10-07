/*-
 * Copyright (c) 2014, Matthew Macy (kmacy@freebsd.org)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Neither the name of Matthew Macy nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/bus.h>
#include <sys/eventhandler.h>
#include <sys/sockio.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/kobj.h>
#include <sys/rman.h>
#include <sys/smp.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/taskqueue.h>


#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/if_media.h>
#include <net/ethernet.h>

#include <netinet/in.h>
#include <netinet/tcp_lro.h>

#include <machine/bus.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <dev/led/led.h>

#include <net/iflib.h>

#include "ifdi_if.h"

/*
 * File organization:
 *  - private structures
 *  - iflib private utility functions
 *  - ifnet functions
 *  - vlan registry and other exported functions
 *  - iflib public core functions
 *
 *
 * Next steps:
 *  - validate the default tx path
 *  - validate the default rx path
 *
 *  - validate queue initialization paths
 *  - validate queue teardown
 *  - validate that all structure fields are initialized

 *  - add rx_buf recycling
 *  - add SW RSS to demux received data packets to buf_rings for deferred processing
 *    look at handling tx ack processing
 *
 */

struct iflib_txq;
typedef struct iflib_txq *iflib_txq_t;
struct iflib_rxq;
typedef struct iflib_rxq *iflib_rxq_t;
struct iflib_qset;
typedef struct iflib_qset *iflib_qset_t;
struct iflib_fl;
typedef struct iflib_fl *iflib_fl_t;

typedef struct iflib_filter_info {
	driver_filter_t *ifi_filter;
	void *ifi_filter_arg;
	struct grouptask *ifi_task;
} *iflib_filter_info_t;

struct iflib_ctx {

	/*
   * Pointer to hardware driver's softc
   */
	if_shared_ctx_t ifc_sctx;
	struct mtx ifc_mtx;
	char ifc_mtx_name[16];
	iflib_txq_t ifc_txqs;
	iflib_rxq_t ifc_rxqs;
	iflib_qset_t ifc_qsets;
	uint32_t ifc_if_flags;
	uint32_t ifc_flags;
	int			ifc_in_detach;

	int ifc_link_state;
	int ifc_link_irq;
	eventhandler_tag ifc_vlan_attach_event;
	eventhandler_tag ifc_vlan_detach_event;
	struct cdev *ifc_led_dev;

	struct if_irq ifc_legacy_irq;
	struct grouptask ifc_link_task;
	struct iflib_filter_info ifc_filter_info;
};
#define LINK_ACTIVE(ctx) ((ctx)->ifc_link_state == LINK_STATE_UP)

typedef struct iflib_dma_info {
	bus_addr_t			idi_paddr;
	caddr_t				idi_vaddr;
	bus_dma_tag_t		idi_tag;
	bus_dmamap_t		idi_map;
	bus_dma_segment_t	idi_seg;
	int					idi_nseg;
	uint32_t			idi_size;
} *iflib_dma_info_t;

struct iflib_qset {
	iflib_dma_info_t ifq_ifdi;
	uint16_t ifq_nhwqs;
};

#define RX_SW_DESC_MAP_CREATED	(1 << 0)
#define TX_SW_DESC_MAP_CREATED	(1 << 1)
#define RX_SW_DESC_INUSE        (1 << 3)
#define TX_SW_DESC_MAPPED       (1 << 4)

typedef struct iflib_sw_desc {
	bus_dmamap_t    ifsd_map;         /* bus_dma map for packet */
	struct mbuf    *ifsd_m;           /* rx: uninitialized mbuf
									   * tx: pkthdr for the packet
									   */
	caddr_t         ifsd_cl;          /* direct cluster pointer for rx */
	int             ifsd_flags;

	struct mbuf		*ifsd_mh;
	struct mbuf		*ifsd_mt;
} *iflib_sd_t;

/* magic number that should be high enough for any hardware */
#define IFLIB_MAX_TX_SEGS 128
#define IFLIB_RX_COPY_THRESH 128
#define IFLIB_QUEUE_IDLE			0
#define IFLIB_QUEUE_HUNG		1
#define IFLIB_QUEUE_WORKING	2

#define IFLIB_LEGACY 1

struct iflib_txq {
	iflib_ctx_t	ift_ctx;
	uint64_t	ift_flags;
	uint32_t	ift_in_use;
	uint32_t	ift_size;
	uint32_t	ift_processed; /* need to have device tx interrupt update this with credits */
	uint32_t	ift_cleaned;
	uint32_t	ift_stop_thres;
	uint32_t	ift_cidx;
	uint32_t	ift_pidx;
	uint32_t	ift_db_pending;
	uint32_t	ift_npending;
	uint32_t	ift_tqid;
	uint64_t	ift_tx_direct_packets;
	uint64_t	ift_tx_direct_bytes;
	uint64_t	ift_no_tx_dma_setup;
	uint64_t	ift_no_desc_avail;
	uint64_t	ift_mbuf_defrag_failed;
	uint64_t	ift_tx_irq;
	bus_dma_tag_t		    ift_desc_tag;
	bus_dma_segment_t	ift_segs[IFLIB_MAX_TX_SEGS];
	struct callout	ift_timer;

	struct mtx              ift_mtx;
	char                    ift_mtx_name[16];
	int                     ift_id;
	iflib_sd_t              ift_sds;
	int                     ift_nbr;
	struct buf_ring        **ift_br;
	struct grouptask		ift_task;
	int			            ift_qstatus;
	int                     ift_active;
	int                     ift_watchdog_time;
	struct iflib_filter_info ift_filter_info;
	iflib_dma_info_t		ift_ifdi;
};

struct iflib_fl {
	uint32_t	ifl_cidx;
	uint32_t	ifl_pidx;
	uint32_t	ifl_size;
	uint32_t	ifl_credits;
	uint32_t	ifl_buf_size;
	int			ifl_cltype;
	uma_zone_t	ifl_zone;

	iflib_sd_t	ifl_sds;
	iflib_rxq_t	ifl_rxq;
	uint8_t		ifl_id;
	iflib_dma_info_t	ifl_ifdi;
};

/* XXX check this */
#define TXQ_AVAIL(txq) ((txq)->ift_size - (txq)->ift_pidx + (txq)->ift_cidx)

typedef struct iflib_global_context {
	struct taskqgroup	*igc_io_tqg;		/* per-cpu taskqueues for io */
	struct taskqgroup	*igc_config_tqg;	/* taskqueue for config operations */
} iflib_global_context_t;

struct iflib_global_context global_ctx, *gctx;

struct iflib_rxq {
	iflib_ctx_t	ifr_ctx;
	uint32_t	ifr_size;
	uint32_t	ifr_cidx;
	uint32_t	ifr_pidx;
	uint64_t	ifr_rx_irq;
	uint16_t	ifr_id;
	int			ifr_lro_enabled;
	iflib_fl_t	ifr_fl;
	uint8_t		ifr_nfl;
	struct lro_ctrl			ifr_lc;
	struct mtx				ifr_mtx;
	char                    ifr_mtx_name[16];
	struct grouptask        ifr_task;
	bus_dma_tag_t           ifr_desc_tag;
	iflib_dma_info_t		ifr_ifdi;
	struct iflib_filter_info ifr_filter_info;
};


#define mtx_held(m)	(((m)->mtx_lock & ~MTX_FLAGMASK) != (uintptr_t)0)


#define CTX_ACTIVE(ctx) ((if_getdrvflags((ctx)->ifc_sctx->isc_ifp) & IFF_DRV_RUNNING))

#define CTX_LOCK_INIT(_sc, _name)  mtx_init(&(_sc)->ifc_mtx, _name, "iflib ctx lock", MTX_DEF)

#define CTX_LOCK(ctx) (mtx_lock(&(ctx)->ifc_mtx))
#define CTX_UNLOCK(ctx) (mtx_unlock(&(ctx)->ifc_mtx))
#define CTX_LOCK_DESTROY(ctx) (mtx_destroy(&(ctx)->ifc_mtx))

#define SCTX_LOCK(sctx) CTX_LOCK((sctx)->isc_ctx)
#define SCTX_UNLOCK(sctx) CTX_UNLOCK((sctx)->isc_ctx)


#define TXQ_LOCK(txq) (mtx_lock(&(txq)->ift_mtx))
#define TXQ_LOCK_HELD(txq) (mtx_held(&(txq)->ift_mtx))
#define TXQ_LOCK_ASSERT(txq) (mtx_assert(&(txq)->ift_mtx, MA_OWNED))
#define TXQ_TRYLOCK(txq) (mtx_trylock(&(txq)->ift_mtx))
#define TXQ_UNLOCK(txq) (mtx_unlock(&(txq)->ift_mtx))
#define TXQ_LOCK_DESTROY(txq) (mtx_destroy(&(txq)->ift_mtx))

#define RXQ_LOCK(rxq) (mtx_lock(&(rxq)->ifr_mtx))
#define RXQ_LOCK_ASSERT(rxq) (mtx_assert(&(rxq)->ifr_mtx), MA_OWNED)
#define RXQ_TRYLOCK(rxq) (mtx_trylock(&(rxq)->ifr_mtx))
#define RXQ_UNLOCK(rxq) (mtx_unlock(&(rxq)->ifr_mtx))
#define RXQ_LOCK_DESTROY(rxq) (mtx_destroy(&(rxq)->ifr_mtx))

static int iflib_recycle_enable;

/* Our boot-time initialization hook */
static int	iflib_module_event_handler(module_t, int, void *);

static moduledata_t iflib_moduledata = {
	"iflib",
	iflib_module_event_handler,
	NULL
};

DECLARE_MODULE(iflib, iflib_moduledata, SI_SUB_SMP, SI_ORDER_ANY);
MODULE_VERSION(iflib, 1);

TASKQGROUP_DEFINE(if_io_tqg, mp_ncpus, 1);
TASKQGROUP_DEFINE(if_config_tqg, 1, 1);

static void iflib_tx_structures_free(if_shared_ctx_t sctx);
static void iflib_rx_structures_free(if_shared_ctx_t sctx);



#if defined(__i386__) || defined(__amd64__)
static __inline void
prefetch(void *x)
{
	__asm volatile("prefetcht0 %0" :: "m" (*(unsigned long *)x));
}
#else
#define prefetch(x)
#endif

static void
_iflib_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nseg, int err)
{
	if (err)
		return;
	*(bus_addr_t *) arg = segs[0].ds_addr;
}

static int
iflib_dma_alloc(iflib_ctx_t ctx, bus_size_t size, iflib_dma_info_t dma,
				int mapflags)
{
	int err;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	device_t dev = sctx->isc_dev;

	err = bus_dma_tag_create(bus_get_dma_tag(dev), /* parent */
				sctx->isc_q_align, 0,	/* alignment, bounds */
				BUS_SPACE_MAXADDR,	/* lowaddr */
				BUS_SPACE_MAXADDR,	/* highaddr */
				NULL, NULL,		/* filter, filterarg */
				size,			/* maxsize */
				1,			/* nsegments */
				size,			/* maxsegsize */
				0,			/* flags */
				NULL,			/* lockfunc */
				NULL,			/* lockarg */
				&dma->idi_tag);
	if (err) {
		device_printf(dev,
		    "%s: bus_dma_tag_create failed: %d\n",
		    __func__, err);
		goto fail_0;
	}

	err = bus_dmamem_alloc(dma->idi_tag, (void**) &dma->idi_vaddr,
	    BUS_DMA_NOWAIT | BUS_DMA_COHERENT, &dma->idi_map);
	if (err) {
		device_printf(dev,
		    "%s: bus_dmamem_alloc(%ju) failed: %d\n",
		    __func__, (uintmax_t)size, err);
		goto fail_2;
	}

	dma->idi_paddr = 0;
	err = bus_dmamap_load(dma->idi_tag, dma->idi_map, dma->idi_vaddr,
	    size, _iflib_dmamap_cb, &dma->idi_paddr, mapflags | BUS_DMA_NOWAIT);
	if (err || dma->idi_paddr == 0) {
		device_printf(dev,
		    "%s: bus_dmamap_load failed: %d\n",
		    __func__, err);
		goto fail_3;
	}

	dma->idi_size = size;
	return (0);

fail_3:
	bus_dmamap_unload(dma->idi_tag, dma->idi_map);
fail_2:
	bus_dmamem_free(dma->idi_tag, dma->idi_vaddr, dma->idi_map);
	bus_dma_tag_destroy(dma->idi_tag);
fail_0:
	dma->idi_tag = NULL;

	return (err);
}

static void
iflib_dma_free(iflib_dma_info_t dma)
{
	if (dma->idi_tag == NULL)
		return;
	if (dma->idi_paddr != 0) {
		bus_dmamap_sync(dma->idi_tag, dma->idi_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(dma->idi_tag, dma->idi_map);
		dma->idi_paddr = 0;
	}
	if (dma->idi_vaddr != NULL) {
		bus_dmamem_free(dma->idi_tag, dma->idi_vaddr, dma->idi_map);
		dma->idi_vaddr = NULL;
	}
	bus_dma_tag_destroy(dma->idi_tag);
	dma->idi_tag = NULL;
}

static int
iflib_fast_intr(void *arg)
{
	iflib_filter_info_t info = arg;
	struct grouptask *gtask = info->ifi_task;


	if (info->ifi_filter != NULL && info->ifi_filter(info->ifi_filter_arg) == FILTER_HANDLED)
		return (FILTER_HANDLED);

	GROUPTASK_ENQUEUE(gtask);
	return (FILTER_HANDLED);
}

static int
_iflib_irq_alloc(iflib_ctx_t ctx, if_irq_t irq, int rid,
	driver_filter_t filter, driver_intr_t handler, void *arg,
	char *name)
{
	int rc;
	struct resource *res;
	void *tag;
	device_t dev = ctx->ifc_sctx->isc_dev;

	irq->ii_rid = rid;
	res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &irq->ii_rid,
	    RF_SHAREABLE | RF_ACTIVE);
	if (res == NULL) {
		device_printf(dev,
		    "failed to allocate IRQ for rid %d, name %s.\n", rid, name);
		return (ENOMEM);
	}

	/*
	 * Sort out handler versus filter XXX
	 */
	rc = bus_setup_intr(dev, res, INTR_MPSAFE | INTR_TYPE_NET,
	    NULL, handler, arg, &tag);
	if (rc != 0) {
		device_printf(dev,
		    "failed to setup interrupt for rid %d, name %s: %d\n",
					  rid, name ? name : "unknown", rc);
	} else if (name)
		bus_describe_intr(dev, res, tag, name);

	irq->ii_tag = tag;
	irq->ii_res = res;
	return (0);
}


/*********************************************************************
 *
 *  Allocate memory for tx_buffer structures. The tx_buffer stores all
 *  the information needed to transmit a packet on the wire. This is
 *  called only once at attach, setup is done every reset.
 *
 **********************************************************************/

static int
iflib_txsd_alloc(iflib_txq_t txq)
{
	iflib_ctx_t ctx = txq->ift_ctx;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	device_t dev = sctx->isc_dev;
	iflib_sd_t txsd;
	int err, i;

	/*
	 * Setup DMA descriptor areas.
	 */
	if ((err = bus_dma_tag_create(bus_get_dma_tag(dev),
			       1, 0,			/* alignment, bounds */
			       BUS_SPACE_MAXADDR,	/* lowaddr */
			       BUS_SPACE_MAXADDR,	/* highaddr */
			       NULL, NULL,		/* filter, filterarg */
			       sctx->isc_tx_maxsize,		/* maxsize */
			       sctx->isc_tx_nsegments,	/* nsegments */
			       sctx->isc_tx_maxsegsize,	/* maxsegsize */
			       0,			/* flags */
			       NULL,			/* lockfunc */
			       NULL,			/* lockfuncarg */
			       &txq->ift_desc_tag))) {
		device_printf(dev,"Unable to allocate TX DMA tag\n");
		goto fail;
	}

	if (!(txq->ift_sds =
	    (iflib_sd_t) malloc(sizeof(struct iflib_sw_desc) *
	    sctx->isc_ntxd, M_DEVBUF, M_NOWAIT | M_ZERO))) {
		device_printf(dev, "Unable to allocate tx_buffer memory\n");
		err = ENOMEM;
		goto fail;
	}

        /* Create the descriptor buffer dma maps */
	txsd = txq->ift_sds;
	for (i = 0; i < sctx->isc_ntxd; i++, txsd++) {
		err = bus_dmamap_create(txq->ift_desc_tag, 0, &txsd->ifsd_map);
		if (err != 0) {
			device_printf(dev, "Unable to create TX DMA map\n");
			goto fail;
		}
	}

	return 0;
fail:
	/* We free all, it handles case where we are in the middle */
	iflib_tx_structures_free(sctx);
	return (err);
}

/*
 * XXX Review tx cleaning and buffer mapping
 *
 */

static void
iflib_txsd_destroy(iflib_ctx_t ctx, iflib_txq_t txq, iflib_sd_t txsd)
{
	if (txsd->ifsd_m != NULL) {
		if (txsd->ifsd_map != NULL) {
			bus_dmamap_destroy(txq->ift_desc_tag, txsd->ifsd_map);
			txsd->ifsd_map = NULL;
		}
	} else if (txsd->ifsd_map != NULL) {
		bus_dmamap_unload(txq->ift_desc_tag,
						  txsd->ifsd_map);
		bus_dmamap_destroy(txq->ift_desc_tag,
						   txsd->ifsd_map);
		txsd->ifsd_map = NULL;
	}
}

static void
iflib_txq_destroy(iflib_txq_t txq)
{
	iflib_ctx_t ctx = txq->ift_ctx;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	iflib_sd_t sd = txq->ift_sds;

	for (int i = 0; i < sctx->isc_ntxd; i++, sd++)
		iflib_txsd_destroy(ctx, txq, sd);
	if (txq->ift_sds != NULL) {
		free(txq->ift_sds, M_DEVBUF);
		txq->ift_sds = NULL;
	}
	if (txq->ift_desc_tag != NULL) {
		bus_dma_tag_destroy(txq->ift_desc_tag);
		txq->ift_desc_tag = NULL;
	}
	TXQ_LOCK_DESTROY(txq);
}

static void
iflib_txsd_free(iflib_ctx_t ctx, iflib_txq_t txq, iflib_sd_t txsd)
{
	if (txsd->ifsd_m == NULL)
		return;
	bus_dmamap_sync(txq->ift_desc_tag,
				    txsd->ifsd_map,
				    BUS_DMASYNC_POSTWRITE);
	bus_dmamap_unload(txq->ift_desc_tag,
					  txsd->ifsd_map);
	m_freem(txsd->ifsd_m);
	txsd->ifsd_m = NULL;
}

static int
iflib_txq_setup(iflib_txq_t txq)
{
	iflib_ctx_t ctx = txq->ift_ctx;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	iflib_qset_t qset = &ctx->ifc_qsets[txq->ift_id];
	iflib_sd_t txsd;
	iflib_dma_info_t di;
	int i;
#ifdef DEV_NETMAP
	struct netmap_slot *slot;
	struct netmap_adapter *na = netmap_getna(sctx->isc_ifp);
#endif /* DEV_NETMAP */

	TXQ_LOCK(txq);
#ifdef DEV_NETMAP
	slot = netmap_reset(na, NR_TX, txq->ift_id, 0);
#endif /* DEV_NETMAP */

    /* Set number of descriptors available */
	txq->ift_qstatus = IFLIB_QUEUE_IDLE;

	/* Reset indices */
	txq->ift_pidx = txq->ift_cidx = txq->ift_npending = 0;

	/* Free any existing tx buffers. */
	txsd = txq->ift_sds;
	for (int i = 0; i < sctx->isc_ntxd; i++, txsd++) {
		iflib_txsd_free(ctx, txq, txsd);
#ifdef DEV_NETMAP
		if (slot) {
			int si = netmap_idx_n2k(&na->tx_rings[txq->ift_id], i);
			uint64_t paddr;
			void *addr;

			addr = PNMB(na, slot + si, &paddr);
			/*
			 * XXX need netmap down call
			 */
			txq->tx_base[i].buffer_addr = htole64(paddr);
			/* reload the map for netmap mode */
			netmap_load_map(na, txq->ift_desc_tag, txsd->ifsd_map, addr);
		}
#endif /* DEV_NETMAP */

	}
	for (i = 0, di = qset->ifq_ifdi; i < qset->ifq_nhwqs; i++, di++)
		bzero((void *)di->idi_vaddr, di->idi_size);

	IFDI_TXQ_SETUP(sctx, txq->ift_id);
	for (i = 0, di = qset->ifq_ifdi; i < qset->ifq_nhwqs; i++, di++)
		bus_dmamap_sync(di->idi_tag, di->idi_map,
						BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	TXQ_UNLOCK(txq);
	return (0);
}

/*********************************************************************
 *
 *  Allocate memory for rx_buffer structures. Since we use one
 *  rx_buffer per received packet, the maximum number of rx_buffer's
 *  that we'll need is equal to the number of receive descriptors
 *  that we've allocated.
 *
 **********************************************************************/
static int
iflib_rxsd_alloc(iflib_rxq_t rxq)
{
	iflib_ctx_t ctx = rxq->ifr_ctx;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	device_t dev = sctx->isc_dev;
	iflib_fl_t fl;
	iflib_sd_t	rxsd;
	int			err;

	fl = malloc(sizeof(struct iflib_fl) *
	    1, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (fl == NULL) {
		device_printf(dev, "Unable to allocate free list memory\n");
		return (ENOMEM);
	}
	fl->ifl_sds = malloc(sizeof(struct iflib_sw_desc) *
	    sctx->isc_nrxd, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (fl->ifl_sds == NULL) {
		device_printf(dev, "Unable to allocate rx sw desc memory\n");
		return (ENOMEM);
	}
	fl->ifl_rxq = rxq;
	fl->ifl_size = sctx->isc_nrxd; /* this isn't necessarily the same */
	err = bus_dma_tag_create(bus_get_dma_tag(dev), /* parent */
				1, 0,			/* alignment, bounds */
				BUS_SPACE_MAXADDR,	/* lowaddr */
				BUS_SPACE_MAXADDR,	/* highaddr */
				NULL, NULL,		/* filter, filterarg */
				sctx->isc_rx_maxsize,	/* maxsize */
				sctx->isc_rx_nsegments,	/* nsegments */
				sctx->isc_rx_maxsegsize,	/* maxsegsize */
				0,			/* flags */
				NULL,			/* lockfunc */
				NULL,			/* lockarg */
				&rxq->ifr_desc_tag);
	if (err) {
		device_printf(dev, "%s: bus_dma_tag_create failed %d\n",
		    __func__, err);
		goto fail;
	}

	rxsd = fl->ifl_sds;
	for (int i = 0; i < sctx->isc_nrxd; i++, rxsd++) {
		err = bus_dmamap_create(rxq->ifr_desc_tag, 0, &rxsd->ifsd_map);
		if (err) {
			device_printf(dev, "%s: bus_dmamap_create failed: %d\n",
			    __func__, err);
			goto fail;
		}
	}

	return (0);

fail:
	iflib_rx_structures_free(sctx);
	return (err);
}

/**
 *	rxq_refill - refill an rxq  free-buffer list
 *	@ctx: the iflib context
 *	@rxq: the free-list to refill
 *	@n: the number of new buffers to allocate
 *
 *	(Re)populate an rxq free-buffer list with up to @n new packet buffers.
 *	The caller must assure that @n does not exceed the queue's capacity.
 */
static void
_iflib_fl_refill(iflib_ctx_t ctx, iflib_fl_t fl, int n)
{
	struct mbuf *m;
	iflib_sd_t rxsd = &fl->ifl_sds[fl->ifl_pidx];
	caddr_t cl;
	int err;
	uint64_t phys_addr;
	if_shared_ctx_t sctx = ctx->ifc_sctx;

	while (n--) {
		/*
		 * We allocate an uninitialized mbuf + cluster, mbuf is
		 * initialized after rx.
		 */
		if ((cl = m_cljget(NULL, M_NOWAIT, fl->ifl_buf_size)) == NULL)
			break;
		if ((m = m_gethdr(M_NOWAIT, MT_NOINIT)) == NULL) {
			uma_zfree(fl->ifl_zone, cl);
			break;
		}
		if ((rxsd->ifsd_flags & RX_SW_DESC_MAP_CREATED) == 0) {
			if ((err = bus_dmamap_create(fl->ifl_ifdi->idi_tag, 0, &rxsd->ifsd_map))) {
				log(LOG_WARNING, "bus_dmamap_create failed %d\n", err);
				uma_zfree(fl->ifl_zone, cl);
				goto done;
			}
			rxsd->ifsd_flags |= RX_SW_DESC_MAP_CREATED;
		}
#if !defined(__i386__) && !defined(__amd64__)
		{
			struct refill_rxq_cb_arg cb_arg;
			cb_arg.error = 0;
			err = bus_dmamap_load(q->ifr_desc_tag, sd->ifsd_map,
		         cl, q->ifr_buf_size, refill_rxq_cb, &cb_arg, 0);

			if (err != 0 || cb_arg.error) {
				/*
				 * !zone_pack ?
				 */
				if (q->zone == zone_pack)
					uma_zfree(q->ifr_zone, cl);
				m_free(m);
				goto done;
			}
			phys_addr = cb_arg.seg.ds_addr;
		}
#else
		phys_addr = pmap_kextract((vm_offset_t)cl);
#endif
		rxsd->ifsd_flags |= RX_SW_DESC_INUSE;
		rxsd->ifsd_cl = cl;
		rxsd->ifsd_m = m;
		sctx->isc_rxd_refill(sctx, fl->ifl_rxq->ifr_id, 0, fl->ifl_pidx, &phys_addr, &cl, 1);

		if (++fl->ifl_pidx == fl->ifl_size) {
			fl->ifl_pidx = 0;
			rxsd = fl->ifl_sds;
		}
		fl->ifl_credits++;
	}

done:
	sctx->isc_rxd_flush(sctx, fl->ifl_rxq->ifr_id, fl->ifl_id, fl->ifl_pidx);
}

static __inline void
__iflib_fl_refill_lt(iflib_ctx_t ctx, iflib_fl_t fl, int max)
{
	uint32_t reclaimable = fl->ifl_size - fl->ifl_credits;

	if (reclaimable > 0)
		_iflib_fl_refill(ctx, fl, min(max, reclaimable));
}

static void
iflib_fl_bufs_free(iflib_fl_t fl)
{
	uint32_t cidx = fl->ifl_cidx;

	while (fl->ifl_credits--) {
		iflib_sd_t d = &fl->ifl_sds[cidx];

		if (d->ifsd_flags & RX_SW_DESC_INUSE) {
			bus_dmamap_unload(fl->ifl_rxq->ifr_desc_tag, d->ifsd_map);
			bus_dmamap_destroy(fl->ifl_rxq->ifr_desc_tag, d->ifsd_map);
			m_init(d->ifsd_m, zone_mbuf, MLEN,
				   M_NOWAIT, MT_DATA, 0);
			uma_zfree(zone_mbuf, d->ifsd_m);
			uma_zfree(fl->ifl_zone, d->ifsd_cl);
		}				
		d->ifsd_cl = NULL;
		d->ifsd_m = NULL;
		if (++cidx == fl->ifl_size)
			cidx = 0;
	}
}

/*********************************************************************
 *
 *  Initialize a receive ring and its buffers.
 *
 **********************************************************************/
static int
iflib_fl_setup(iflib_fl_t fl)
{
	iflib_rxq_t rxq = fl->ifl_rxq;
	iflib_ctx_t ctx = rxq->ifr_ctx;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	int			err = 0;
#ifdef DEV_NETMAP
	struct netmap_slot *slot;
	struct netmap_adapter *na = netmap_getna(sctx->isc_ifp);
#endif

	/* Clear the ring contents */
#ifdef DEV_NETMAP
	slot = netmap_reset(na, NR_RX, rxq->ifr_id, 0);
#endif

	/*
	 * XXX don't set the max_frame_size to larger
	 * than the hardware can handle
	 */
	if (sctx->isc_max_frame_size <= 2048)
		fl->ifl_buf_size = MCLBYTES;
	else if (sctx->isc_max_frame_size <= 4096)
		fl->ifl_buf_size = MJUMPAGESIZE;
	else if (sctx->isc_max_frame_size <= 9216)
		fl->ifl_buf_size = MJUM9BYTES;
	else
		fl->ifl_buf_size = MJUM16BYTES;
	fl->ifl_cltype = m_gettype(fl->ifl_buf_size);
	fl->ifl_zone = m_getzone(fl->ifl_buf_size);

	/*
	** Free current RX buffer structs and their mbufs
	*/
	iflib_fl_bufs_free(fl);

	/* Now replenish the mbufs */
	_iflib_fl_refill(ctx, fl, fl->ifl_size);

	/*
	 * handle failure
	 */
	bus_dmamap_sync(rxq->ifr_ifdi->idi_tag, rxq->ifr_ifdi->idi_map,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	return (err);
}

/*********************************************************************
 *
 *  Free receive ring data structures
 *
 **********************************************************************/
static void
iflib_rx_sds_free(iflib_rxq_t rxq)
{

	if (rxq->ifr_fl != NULL) {
		if (rxq->ifr_fl->ifl_sds != NULL)
			free(rxq->ifr_fl->ifl_sds, M_DEVBUF);

		free(rxq->ifr_fl, M_DEVBUF);
		rxq->ifr_fl = NULL;
		rxq->ifr_cidx = rxq->ifr_pidx = 0;
	}

	if (rxq->ifr_desc_tag != NULL) {
		bus_dma_tag_destroy(rxq->ifr_desc_tag);
		rxq->ifr_desc_tag = NULL;
	}
}

/*
 * MI independent logic
 *
 */
static void
iflib_timer(void *arg)
{
	iflib_txq_t txq = arg;
	iflib_ctx_t ctx = txq->ift_ctx;
	if_shared_ctx_t sctx = ctx->ifc_sctx;

	/*
	** Check on the state of the TX queue(s), this
	** can be done without the lock because its RO
	** and the HUNG state will be static if set.
	*/
	IFDI_TIMER(sctx, txq->ift_id);
	if ((txq->ift_qstatus == IFLIB_QUEUE_HUNG) &&
		(sctx->isc_pause_frames == 0))
		goto hung;

	if (TXQ_AVAIL(txq) <= sctx->isc_tx_nsegments)
		GROUPTASK_ENQUEUE(&txq->ift_task);

	sctx->isc_pause_frames = 0;
	callout_reset_on(&txq->ift_timer, hz/2, iflib_timer, txq, txq->ift_timer.c_cpu);
	return;
hung:
	CTX_LOCK(ctx);
	if_setdrvflagbits(sctx->isc_ifp, 0, IFF_DRV_RUNNING);
	device_printf(sctx->isc_dev,  "TX(%d) desc avail = %d, pidx = %d\n",
				  txq->ift_id, TXQ_AVAIL(txq), txq->ift_pidx);

	IFDI_WATCHDOG_RESET(sctx);
	sctx->isc_watchdog_events++;
	sctx->isc_pause_frames = 0;

	IFDI_INIT(sctx);
	CTX_UNLOCK(ctx);
}

static void
iflib_init_locked(iflib_ctx_t ctx)
{
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	iflib_txq_t txq = ctx->ifc_txqs;
	int i;

	IFDI_INTR_DISABLE(sctx);
	for (i = 0; i < sctx->isc_nqsets; i++, txq++)
		callout_stop(&txq->ift_timer);
	IFDI_INIT(sctx);
	if_setdrvflagbits(sctx->isc_ifp, IFF_DRV_RUNNING, 0);
	IFDI_INTR_ENABLE(sctx);
	txq = ctx->ifc_txqs;
	for (i = 0; i < sctx->isc_nqsets; i++, txq++)
		callout_reset_on(&txq->ift_timer, hz/2, iflib_timer, txq,
			txq->ift_timer.c_cpu);
}

static int
iflib_media_change(if_t ifp)
{
	iflib_ctx_t ctx = if_getsoftc(ifp);
	int err;

	CTX_LOCK(ctx);
	if ((err = IFDI_MEDIA_CHANGE(ctx->ifc_sctx)) == 0)
		iflib_init_locked(ctx);
	CTX_UNLOCK(ctx);
	return (err);
}

static void
iflib_media_status(if_t ifp, struct ifmediareq *ifmr)
{
	iflib_ctx_t ctx = if_getsoftc(ifp);

	CTX_LOCK(ctx);
	IFDI_UPDATE_LINK_STATUS(ctx->ifc_sctx);
	IFDI_MEDIA_STATUS(ctx->ifc_sctx, ifmr);
	CTX_UNLOCK(ctx);
}

static void
iflib_stop(iflib_ctx_t ctx)
{
	iflib_txq_t txq = ctx->ifc_txqs;
	if_shared_ctx_t sctx = ctx->ifc_sctx;

	IFDI_INTR_DISABLE(sctx);
	/* Tell the stack that the interface is no longer active */
	if_setdrvflagbits(sctx->isc_ifp, IFF_DRV_OACTIVE, IFF_DRV_RUNNING);

	/* Wait for curren tx queue users to exit to disarm watchdog timer. */
	for (int i = 0; i < sctx->isc_nqsets; i++, txq++) {
		TXQ_LOCK(txq);
		txq->ift_qstatus = IFLIB_QUEUE_IDLE;
		callout_stop(&txq->ift_timer);
		TXQ_UNLOCK(txq);
	}
	IFDI_STOP(sctx);
}

static int
iflib_recycle_rx_buf(iflib_fl_t fl)
{
#if 0
	/* XXX just reassign */
	if ((err = IFDI_RECYCLE_RX_BUF(sctx, rxq, idx)) != 0)
		return (err);

	rxq->ifr_sds[rxq->ifr_pidx] = rxq->ifr_sds[idx];
	rxq->ifr_credits++;
	if (++rxq->ifr_pidx == rxq->ifr_size)
		rxq->ifr_pidx = 0;
#endif
	return (0);
}

/*
 * Internal service routines
 */

#if !defined(__i386__) && !defined(__amd64__)
struct rxq_refill_cb_arg {
	int               error;
	bus_dma_segment_t seg;
	int               nseg;
};

static void
_rxq_refill_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	struct rxq_refill_cb_arg *cb_arg = arg;
	
	cb_arg->error = error;
	cb_arg->seg = segs[0];
	cb_arg->nseg = nseg;
}
#endif

/*
 * Process one software descriptor
 */
static struct mbuf *
iflib_rxd_pkt_get(iflib_fl_t fl, if_rxd_info_t ri)
{
	iflib_sd_t sd_next, sd = &fl->ifl_sds[fl->ifl_cidx];
	uint32_t flags = M_EXT;
	caddr_t cl;
	struct mbuf *m;
	int cidx_next, len = ri->iri_len;

	if (iflib_recycle_enable && ri->iri_len <= IFLIB_RX_COPY_THRESH) {
		panic(" not all cases handled");
		if ((m = m_gethdr(M_NOWAIT, MT_DATA)) == NULL)
			goto skip_recycle;
		cl = mtod(m, void *);
		memcpy(cl, sd->ifsd_cl, ri->iri_len);
		iflib_recycle_rx_buf(fl);
		m->m_pkthdr.len = m->m_len = ri->iri_len;
		if (ri->iri_pad) {
			m->m_data += ri->iri_pad;
			len -= ri->iri_pad;
		}
	} else {
	skip_recycle:
		bus_dmamap_unload(fl->ifl_rxq->ifr_desc_tag, sd->ifsd_map);
		cl = sd->ifsd_cl;
		m = sd->ifsd_m;

		if (sd->ifsd_mh == NULL)
			flags |= M_PKTHDR;
		m_init(m, fl->ifl_zone, fl->ifl_buf_size, M_NOWAIT, MT_DATA, flags);
		m_cljset(m, cl, fl->ifl_cltype);

		if (ri->iri_pad) {
			m->m_data += ri->iri_pad;
			len -= ri->iri_pad;
		}
		m->m_len = len;
		if (sd->ifsd_mh == NULL)
			m->m_pkthdr.len = len;
		else
			sd->ifsd_mh->m_pkthdr.len += len;
		}

	if (sd->ifsd_mh != NULL && 	ri->iri_next_offset != 0) {
		/* We're in the middle of a packet and thus
		 * need to pass this packet's data on to the
		 * next descriptor
		 */
		cidx_next = ri->iri_cidx + ri->iri_next_offset;
		if (cidx_next >= fl->ifl_size)
			cidx_next -= fl->ifl_size;
		sd_next = &fl->ifl_sds[cidx_next];
		sd_next->ifsd_mh = sd->ifsd_mh;
		sd_next->ifsd_mt = sd->ifsd_mt;
		sd->ifsd_mh = sd->ifsd_mt = NULL;
		sd_next->ifsd_mt->m_next = m;
		sd_next->ifsd_mt = m;
		m = NULL;
	} else if (sd->ifsd_mh == NULL && ri->iri_next_offset != 0) {
		/*
		 * We're at the start of a multi-fragment packet
		 */
		cidx_next = ri->iri_cidx + ri->iri_next_offset;
		if (cidx_next >= fl->ifl_size)
			cidx_next -= fl->ifl_size;
		sd_next = &fl->ifl_sds[cidx_next];
		sd_next->ifsd_mh = sd_next->ifsd_mt = m;
		m = NULL;
	} else if (sd->ifsd_mh != NULL && ri->iri_next_offset == 0) {
		/*
		 * We're at the end of a multi-fragment packet
		 */
		sd->ifsd_mt->m_next = m;
		sd->ifsd_mt = m;
		m = sd->ifsd_mh;
		sd->ifsd_mh = sd->ifsd_mt = NULL;
	}
	if (m == NULL)
		return (NULL);

	m->m_pkthdr.rcvif = ri->iri_ifp;
	m->m_flags |= ri->iri_flags;

	if (ri->iri_flags & M_VLANTAG)
		if_setvtag(m, ri->iri_vtag);
	m->m_pkthdr.flowid = ri->iri_flowid;
	M_HASHTYPE_SET(m, ri->iri_hash_type);
	m->m_pkthdr.csum_flags = ri->iri_csum_flags;
	m->m_pkthdr.csum_data = ri->iri_csum_data;
	if_inc_counter(ri->iri_ifp, IFCOUNTER_IBYTES, m->m_pkthdr.len);
	if_inc_counter(ri->iri_ifp, IFCOUNTER_IPACKETS, 1);
	return (m);
}

static bool
iflib_rxeof(iflib_rxq_t rxq, int budget)
{
	iflib_ctx_t ctx = rxq->ifr_ctx;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	int fl_cidx, cidx = rxq->ifr_cidx;
	struct if_rxd_info ri;
	iflib_dma_info_t di;
	int qsid, err, budget_left = budget;
	iflib_txq_t txq;
	iflib_fl_t fl;
	struct lro_entry *queued;
	int8_t qidx;
	/*
	 * XXX early demux data packets so that if_input processing only handles
	 * acks in interrupt context
	 */
	struct mbuf *m, *mh, *mt;

	if (sctx->isc_txd_credits_update != NULL) {
		qsid = rxq->ifr_id;
		txq = &ctx->ifc_txqs[qsid];
		if (sctx->isc_txd_credits_update(sctx, qsid, txq->ift_cidx))
			GROUPTASK_ENQUEUE(&txq->ift_task);
	}

	if (!RXQ_TRYLOCK(rxq))
		return (false);
#ifdef DEV_NETMAP
	if (netmap_rx_irq(ifp, rxq->ifr_id, &processed)) {
		RXQ_UNLOCK(rxq);
		return (FALSE);
	}
#endif /* DEV_NETMAP */

	ri.iri_qsidx = rxq->ifr_id;
	mh = mt = NULL;
	while (__predict_true(budget_left--)) {
		if (__predict_false(!CTX_ACTIVE(ctx)))
			break;
		di = rxq->ifr_ifdi;
		bus_dmamap_sync(di->idi_tag, di->idi_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		if (__predict_false(!sctx->isc_rxd_available(sctx, rxq->ifr_id, cidx)))
			return (false);

		ri.iri_cidx = cidx;
		/*
		 * Reset client set fields to their default values
		 */
		ri.iri_flags = 0;
		ri.iri_m = NULL;
		ri.iri_next_offset = 0;
		ri.iri_pad = 0;
		ri.iri_qidx = 0;
		ri.iri_ifp = sctx->isc_ifp;
		err = sctx->isc_rxd_pkt_get(sctx, &ri);

		qidx = ri.iri_qidx;
		if (++cidx == sctx->isc_nrxd)
			cidx = 0;
		if (ri.iri_m != NULL) {
			m = ri.iri_m;
			ri.iri_m = NULL;
			goto imm_pkt;
		}
		/* was this only a completion queue message? */
		if (qidx == -1)
			continue;
		fl = &rxq->ifr_fl[qidx];
		fl_cidx = fl->ifl_cidx;
		bus_dmamap_unload(rxq->ifr_desc_tag, fl->ifl_sds[fl_cidx].ifsd_map);

		if (ri.iri_len == 0) {
			/*
			 * XXX Note currently we don't free the initial pieces
			 * of a multi-fragment packet
			 */
			iflib_recycle_rx_buf(fl);
			if (++fl_cidx == fl->ifl_size)
				fl_cidx = 0;
			fl->ifl_cidx = fl_cidx;
			continue;
		}
		m = iflib_rxd_pkt_get(fl, &ri);
		if (++fl_cidx == fl->ifl_size)
			fl_cidx = 0;
		fl->ifl_cidx = fl_cidx;
		__iflib_fl_refill_lt(ctx, fl, /* XXX em value */ 8);

		if (m == NULL)
			continue;
	imm_pkt:
		if (mh == NULL)
			mh = mt = m;
		else {
			mt->m_nextpkt = m;
			mt = m;
		}
	}
	rxq->ifr_cidx = cidx;
	RXQ_UNLOCK(rxq);

	while (mh != NULL) {
		m = mh;
		mh = mh->m_nextpkt;
		m->m_nextpkt = NULL;
		if (rxq->ifr_lc.lro_cnt != 0 &&
			tcp_lro_rx(&rxq->ifr_lc, m, 0) == 0)
			continue;
		if_input(sctx->isc_ifp, m);
	}
	/*
	 * Flush any outstanding LRO work
	 */
	while ((queued = SLIST_FIRST(&rxq->ifr_lc.lro_active)) != NULL) {
		SLIST_REMOVE_HEAD(&rxq->ifr_lc.lro_active, next);
		tcp_lro_flush(&rxq->ifr_lc, queued);
	}

	return sctx->isc_rxd_available(sctx, rxq->ifr_id, rxq->ifr_cidx);
}

#define M_CSUM_FLAGS(m) ((m)->m_pkthdr.csum_flags)
#define M_HAS_VLANTAG(m) (m->m_flags & M_VLANTAG)

static __inline void
iflib_txd_db_check(iflib_ctx_t ctx, iflib_txq_t txq, int ring)
{
	if_shared_ctx_t sctx;
	uint32_t dbval;
	iflib_sd_t txsd;
	struct if_pkt_info pi;

	if (ring || ++txq->ift_db_pending >= 32) {
		sctx = ctx->ifc_sctx;
		txsd = &txq->ift_sds[txq->ift_pidx];

		/*
		 * Flush deferred buffers first
		 */
		if (__predict_false(txsd->ifsd_m != NULL)) {
			pi.ipi_m = NULL;
			pi.ipi_qsidx = txq->ift_id;
			pi.ipi_pidx = txq->ift_pidx;
			sctx->isc_txd_encap(sctx, &pi);
			txq->ift_pidx = pi.ipi_new_pidx;
		}
		dbval = txq->ift_npending ? txq->ift_npending : txq->ift_pidx;
		wmb();
		sctx->isc_txd_flush(sctx, txq->ift_id, dbval);
		txq->ift_npending = 0;
	}
}

static int
iflib_encap(iflib_txq_t txq, struct mbuf **m_headp)
{
	iflib_ctx_t ctx = txq->ift_ctx;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	bus_dma_segment_t	*segs = txq->ift_segs;
	struct mbuf		*m, *m_head = *m_headp;
	int pidx = txq->ift_pidx;
	iflib_sd_t txsd = &txq->ift_sds[pidx];
	bus_dmamap_t		map = txsd->ifsd_map;
	struct if_pkt_info pi;
	bool remap = TRUE;
	int err, nsegs, ndesc;

retry:

	err = bus_dmamap_load_mbuf_sg(txq->ift_desc_tag, map,
	    *m_headp, segs, &nsegs, BUS_DMA_NOWAIT);

	if (__predict_false(err)) {
		switch (err) {
		case EFBIG:
			/* try defrag once */
			if (remap == TRUE) {
				remap = FALSE;
				m = m_defrag(*m_headp, M_NOWAIT);
				if (m == NULL) {
					txq->ift_mbuf_defrag_failed++;
					m_freem(*m_headp);
					*m_headp = NULL;
					err = ENOBUFS;
				} else {
					*m_headp = m;
					goto retry;
				}
			}
			break;
		case ENOMEM:
			txq->ift_no_tx_dma_setup++;
			break;
		default:
			txq->ift_no_tx_dma_setup++;
			m_freem(*m_headp);
			*m_headp = NULL;
			break;
		}
		return (err);
	}

	/*
	 * XXX assumes a 1 to 1 relationship between segments and
	 *        descriptors - this does not hold true on all drivers, e.g.
	 *        cxgb
	 */
	if (nsegs > TXQ_AVAIL(txq)) {
		txq->ift_no_desc_avail++;
		bus_dmamap_unload(txq->ift_desc_tag, map);
		return (ENOBUFS);
	}
	m_head = *m_headp;
	pi.ipi_m = m_head;
	pi.ipi_segs = segs;
	pi.ipi_nsegs = nsegs;
	pi.ipi_pidx = pidx;
	pi.ipi_ndescs = 0;
	pi.ipi_qsidx = txq->ift_id;

	if ((err = sctx->isc_txd_encap(sctx, &pi)) == 0) {
		bus_dmamap_sync(txq->ift_ifdi->idi_tag, txq->ift_ifdi->idi_map,
						BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

		if (pi.ipi_m != NULL) {
			if (txsd->ifsd_m != NULL)
				pi.ipi_m->m_nextpkt = txsd->ifsd_m;
			txsd->ifsd_m = pi.ipi_m;
		}

		if (pi.ipi_new_pidx >= pi.ipi_pidx)
			ndesc = pi.ipi_new_pidx - pi.ipi_pidx;
		else
			ndesc = pi.ipi_new_pidx - pi.ipi_pidx + sctx->isc_ntxd;

		txq->ift_in_use += ndesc;
		txq->ift_pidx = pi.ipi_new_pidx;
		txq->ift_npending += pi.ipi_ndescs;
		iflib_txd_db_check(ctx, txq, 0);
	}
	return (err);
}

#define BRBITS 8
#define FIRST_QSET(ctx) 0
#define NQSETS(ctx) ((ctx)->ifc_sctx->isc_nqsets)
#define QIDX(ctx, m) ((((m)->m_pkthdr.flowid >> BRBITS) % NQSETS(ctx)) + FIRST_QSET(ctx))
#define BRIDX(txq, m) ((m)->m_pkthdr.flowid % txq->ift_nbr)
#define DESC_RECLAIMABLE(q) ((int)((q)->ift_processed - (q)->ift_cleaned - (q)->ift_ctx->ifc_sctx->isc_tx_nsegments))
#define RECLAIM_THRESH(ctx) ((ctx)->ifc_sctx->isc_tx_reclaim_thresh)
#define MAX_TX_DESC(ctx) ((ctx)->ifc_sctx->isc_tx_nsegments)


static inline int
iflib_enqueue_pkt(if_t ifp, iflib_txq_t txq, struct mbuf *m)
{
	int bridx = 0;

	if (m->m_flags & M_FLOWID)
		bridx = BRIDX(txq, m);

	return (drbr_enqueue(ifp, txq->ift_br[bridx], m));
}

static inline int
iflib_txq_softq_empty(if_t ifp, iflib_txq_t txq)
{
	int i;

	for (i = 0; i < txq->ift_nbr; i++)
		if (drbr_peek(ifp, txq->ift_br[i]) != NULL)
			return (FALSE);

	return (TRUE);
}

static void
iflib_tx_desc_free(iflib_txq_t txq, int n)
{
	iflib_sd_t txsd;
	uint32_t qsize, cidx, mask;
	struct mbuf *m;

	TXQ_LOCK_ASSERT(txq);
	cidx = txq->ift_cidx;
	qsize = txq->ift_ctx->ifc_sctx->isc_ntxd;
	mask = qsize-1;
	txsd = &txq->ift_sds[cidx];

	while (n--) {
		prefetch(txq->ift_sds[(cidx + 1) & mask].ifsd_m);
		prefetch(txq->ift_sds[(cidx + 2) & mask].ifsd_m);

		if (txsd->ifsd_m != NULL) {
			if (txsd->ifsd_flags & TX_SW_DESC_MAPPED) {
				bus_dmamap_unload(txq->ift_desc_tag, txsd->ifsd_map);
				txsd->ifsd_flags &= ~TX_SW_DESC_MAPPED;
			}
			while (txsd->ifsd_m) {
				m = txsd->ifsd_m;
				txsd->ifsd_m = m->m_nextpkt;
				m_freem(m);
			}
		}

		++txsd;
		if (++cidx == qsize) {
			cidx = 0;
			txsd = txq->ift_sds;
		}
	}
	txq->ift_cidx = cidx;
}

static __inline int
iflib_completed_tx_reclaim(iflib_txq_t txq, int thresh)
{
	int reclaim;
	if_shared_ctx_t sctx = txq->ift_ctx->ifc_sctx;

	KASSERT(thresh >= 0, ("invalid threshold to reclaim"));
	TXQ_LOCK_ASSERT(txq);

	reclaim = DESC_RECLAIMABLE(txq);
	/*
	 * Add some rate-limiting check so that that
	 * this isn't called every time
	 */
	if (sctx->isc_txd_credits_update != NULL &&
		reclaim <= thresh)
		sctx->isc_txd_credits_update(sctx, txq->ift_id, txq->ift_cidx);

	reclaim = DESC_RECLAIMABLE(txq);
	if (reclaim <= thresh)
		return (0);

	iflib_tx_desc_free(txq, reclaim);
	txq->ift_cleaned += reclaim;
	txq->ift_in_use -= reclaim;

	if (txq->ift_active == FALSE)
		txq->ift_active = TRUE;

	return (reclaim);
}

static void
iflib_tx_timeout(void *arg)
{

	/* XXX */
}

static int
iflib_txq_start(iflib_txq_t txq)
{
	iflib_ctx_t ctx = txq->ift_ctx;
	if_t ifp = ctx->ifc_sctx->isc_ifp;
	struct mbuf *next;
	int err, bridx, resid, enq;

	err = enq = 0;
	do {
		resid = FALSE;
		for (bridx = 0; bridx < txq->ift_nbr; bridx++) {
			if ((next = drbr_peek(ifp, txq->ift_br[bridx])) == NULL)
				continue;
			if (!(ifp->if_drv_flags & IFF_DRV_RUNNING) ||
				!LINK_ACTIVE(ctx))
				goto done;
			resid = TRUE;
			iflib_completed_tx_reclaim(txq, RECLAIM_THRESH(ctx));
			if (TXQ_AVAIL(txq) < MAX_TX_DESC(ctx))
				break;
			if ((err = iflib_encap(txq, &next)) != 0) {
				if (next == NULL)
					drbr_advance(ifp, txq->ift_br[bridx]);
				else
					drbr_putback(ifp, txq->ift_br[bridx], next);
				goto done;
			}
			drbr_advance(ifp, txq->ift_br[bridx]);
			enq++;

			if_inc_counter(ifp, IFCOUNTER_OBYTES, next->m_pkthdr.len);
			if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
			if (next->m_flags & M_MCAST)
				if_inc_counter(ifp, IFCOUNTER_OMCASTS, 1);
			if_etherbpfmtap(ifp, next);
		}
	} while (resid);

	done:
	if (enq > 0) {
		/* Set the watchdog */
		txq->ift_qstatus = IFLIB_QUEUE_WORKING;
		txq->ift_watchdog_time = ticks;
	}
	if (txq->ift_db_pending)
		iflib_txd_db_check(ctx, txq, 1);
	if (!iflib_txq_softq_empty(ifp, txq) && LINK_ACTIVE(ctx))
		callout_reset_on(&txq->ift_timer, 1, iflib_tx_timeout, txq,
						 txq->ift_timer.c_cpu);
	/*
	 * XXX we should allot ourselves a budget and return non-zero
	 * if it is exceeded
	 */
	return (0);
}

static int
iflib_txq_transmit(if_t ifp, iflib_txq_t txq, struct mbuf *m)
{
	iflib_ctx_t ctx = txq->ift_ctx;
	int             avail, err = 0;

	avail = txq->ift_size - txq->ift_in_use;
	TXQ_LOCK_ASSERT(txq);

	if (iflib_txq_softq_empty(ifp, txq) && avail >= MAX_TX_DESC(ctx)) {
		if (iflib_encap(txq, &m)) {
			if (m != NULL && (err = iflib_txq_transmit(ifp, txq, m)) != 0)
				return (err);
		} else {
			if (txq->ift_db_pending)
				iflib_txd_db_check(ctx, txq, 1);
			txq->ift_tx_direct_packets++;
			txq->ift_tx_direct_bytes += m->m_pkthdr.len;
		}
	} else if ((err = iflib_enqueue_pkt(ifp, txq, m)) != 0)
		return (err);

	iflib_completed_tx_reclaim(txq, RECLAIM_THRESH(ctx));

	if (!iflib_txq_softq_empty(ifp, txq) && LINK_ACTIVE(ctx))
		iflib_txq_start(txq);

	return (0);
}

static void
_task_fn_tx(void *context, int pending)
{
	iflib_txq_t txq = context;
	if_shared_ctx_t sctx = txq->ift_ctx->ifc_sctx;
	int more = 0;

	if (!(if_getdrvflags(sctx->isc_ifp) & IFF_DRV_RUNNING))
		return;

	if (TXQ_TRYLOCK(txq)) {
		more = iflib_txq_start(txq);
		TXQ_UNLOCK(txq);
	}
	if (more)
		GROUPTASK_ENQUEUE(&txq->ift_task);
}

static void
_task_fn_rx(void *context, int pending)
{
	iflib_rxq_t rxq = context;
	iflib_ctx_t ctx = rxq->ifr_ctx;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	int more = 0;

	if (!(if_getdrvflags(sctx->isc_ifp) & IFF_DRV_RUNNING))
		return;

	if (RXQ_TRYLOCK(rxq)) {
		if ((more = iflib_rxeof(rxq, 8 /* XXX */)) == 0) {
			if (ctx->ifc_flags & IFLIB_LEGACY)
				IFDI_INTR_ENABLE(sctx);
			else
				IFDI_RX_INTR_ENABLE(sctx, rxq->ifr_id);
		}
		RXQ_UNLOCK(rxq);
	}
	if (more)
		GROUPTASK_ENQUEUE(&rxq->ifr_task);
}

static void
_task_fn_link(void *context, int pending)
{
	if_shared_ctx_t sctx = context;
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_txq_t txq = ctx->ifc_txqs;
	int i;

	if (!(if_getdrvflags(sctx->isc_ifp) & IFF_DRV_RUNNING))
		return;

	CTX_LOCK(ctx);
	for (i = 0; i < sctx->isc_nqsets; i++, txq++)
		callout_stop(&txq->ift_timer);
	IFDI_UPDATE_LINK_STATUS(sctx);
	for (txq = ctx->ifc_txqs, i = 0; i < sctx->isc_nqsets; i++, txq++)
		callout_reset_on(&txq->ift_timer, hz/2, iflib_timer, txq, txq->ift_timer.c_cpu);
	IFDI_LINK_INTR_ENABLE(sctx);
	CTX_UNLOCK(ctx);

	if (LINK_ACTIVE(ctx) == 0)
		return;

	for (i = 0; i < sctx->isc_nqsets; i++, txq++) {
		if (TXQ_TRYLOCK(txq) == 0)
			continue;
		iflib_txq_start(txq);
		TXQ_UNLOCK(txq);
	}
}

#if 0
void
iflib_intr_rx(void *arg)
{
	iflib_rxq_t rxq = arg;

	++rxq->ifr_rx_irq;
	_task_fn_rx(arg, 0);
}

void
iflib_intr_tx(void *arg)
{
	iflib_txq_t txq= arg;

	++txq->ift_tx_irq;
	_task_fn_tx(arg, 0);
}

void
iflib_intr_link(void *arg)
{
	iflib_ctx_t ctx = arg;

	++ctx->ifc_link_irq;
	_task_fn_link(arg, 0);
}
#endif

static int
iflib_sysctl_int_delay(SYSCTL_HANDLER_ARGS)
{
	int err;
	if_int_delay_info_t info;
	if_shared_ctx_t sctx;

	info = (if_int_delay_info_t)arg1;
	sctx = info->iidi_sctx;
	info->iidi_req = req;
	info->iidi_oidp = oidp;
	SCTX_LOCK(sctx);
	err = IFDI_SYSCTL_INT_DELAY(sctx, info);
	SCTX_UNLOCK(sctx);
	return (err);
}

/*********************************************************************
 *
 *  IFNET FUNCTIONS
 *
 **********************************************************************/

static void
iflib_if_init(void *arg)
{
	iflib_ctx_t ctx = arg;

	iflib_init(ctx->ifc_sctx);
}

static int
iflib_if_transmit(if_t ifp, struct mbuf *m)
{
	iflib_ctx_t	ctx = if_getsoftc(ifp);
	iflib_txq_t txq;
	int err = 0, qidx = 0;

	if ((ifp->if_drv_flags & IFF_DRV_RUNNING) == 0 || !LINK_ACTIVE(ctx)) {
		m_freem(m);
		return (0);
	}

	if ((NQSETS(ctx) > 1) && (m->m_flags & M_FLOWID))
		qidx = QIDX(ctx, m);
	/*
	 * XXX calculate buf_ring based on flowid (divvy up bits?)
	 */
	txq = &ctx->ifc_txqs[qidx];

	if (!TXQ_LOCK_HELD(txq) && TXQ_TRYLOCK(txq)) {
		err = iflib_txq_transmit(ifp, txq, m);
		TXQ_UNLOCK(txq);
	} else if (m != NULL) {
		err = iflib_enqueue_pkt(ifp, txq, m);
		/* Minimize a small race between another thread dropping the
		 * lock and us enqueuing the buffer on the buf_ring
		 */
		if (err == 0 && !TXQ_LOCK_HELD(txq) && TXQ_TRYLOCK(txq)) {
			iflib_txq_start(txq);
			TXQ_UNLOCK(txq);
		}
	}
	return (err);
}

static void
iflib_if_qflush(if_t ifp)
{
	iflib_ctx_t ctx = if_getsoftc(ifp);
	iflib_txq_t txq = ctx->ifc_txqs;
	struct mbuf     *m;

	for (int i = 0; i < NQSETS(ctx); i++, txq++) {
		TXQ_LOCK(txq);
		for (int j = 0; j < txq->ift_nbr; j++) {
			while ((m = buf_ring_dequeue_sc(txq->ift_br[j])) != NULL)
				m_freem(m);
		}
		TXQ_UNLOCK(txq);
	}
	if_qflush(ifp);
}

static int
iflib_if_ioctl(if_t ifp, u_long command, caddr_t data)
{
	iflib_ctx_t ctx = if_getsoftc(ifp);
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	struct ifreq	*ifr = (struct ifreq *)data;
#if defined(INET) || defined(INET6)
	struct ifaddr	*ifa = (struct ifaddr *)data;
#endif
	bool		avoid_reset = FALSE;
	int		err = 0;

	switch (command) {
	case SIOCSIFADDR:
#ifdef INET
		if (ifa->ifa_addr->sa_family == AF_INET)
			avoid_reset = TRUE;
#endif
#ifdef INET6
		if (ifa->ifa_addr->sa_family == AF_INET6)
			avoid_reset = TRUE;
#endif
		/*
		** Calling init results in link renegotiation,
		** so we avoid doing it when possible.
		*/
		if (avoid_reset) {
			if_setflagbits(ifp, IFF_UP,0);
			if (!(if_getdrvflags(ifp)& IFF_DRV_RUNNING))
				iflib_init(sctx);
#ifdef INET
			if (!(if_getflags(ifp) & IFF_NOARP))
				arp_ifinit_drv(ifp, ifa);
#endif
		} else
			err = ether_ioctl(ifp, command, data);
		break;
	case SIOCSIFMTU:
		CTX_LOCK(ctx);
		/* detaching ?*/
		if ((err = IFDI_MTU_SET(sctx, ifr->ifr_mtu)) == 0) {
			iflib_init_locked(ctx);
			if_setmtu(ifp, ifr->ifr_mtu);
		}
		CTX_UNLOCK(ctx);
		break;
	case SIOCSIFFLAGS:
		CTX_LOCK(ctx);
		if (if_getflags(ifp) & IFF_UP) {
			if (if_getdrvflags(ifp) & IFF_DRV_RUNNING) {
				if ((if_getflags(ifp) ^ ctx->ifc_if_flags) &
				    (IFF_PROMISC | IFF_ALLMULTI)) {
					IFDI_PROMISC_SET(sctx, if_getflags(ifp));
				}
			} else
				IFDI_INIT(sctx);
		} else
			if (if_getdrvflags(ifp) & IFF_DRV_RUNNING)
				IFDI_STOP(sctx);
		ctx->ifc_if_flags = if_getflags(ifp);
		CTX_UNLOCK(ctx);
		break;

		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		if (if_getdrvflags(ifp) & IFF_DRV_RUNNING) {
			CTX_LOCK(ctx);
			IFDI_INTR_DISABLE(sctx);
			IFDI_MULTI_SET(sctx);
			IFDI_INTR_ENABLE(sctx);
			CTX_LOCK(ctx);
		}
		break;
	case SIOCSIFMEDIA:
		CTX_LOCK(ctx);
		IFDI_MEDIA_SET(sctx);
		CTX_UNLOCK(ctx);
		/* falls thru */
	case SIOCGIFMEDIA:
		err = ifmedia_ioctl(ifp, ifr, &sctx->isc_media, command);
		break;
	case SIOCGI2C:
	{
		struct ifi2creq i2c;

		err = copyin(ifr->ifr_data, &i2c, sizeof(i2c));
		if (err != 0)
			break;
		if (i2c.dev_addr != 0xA0 && i2c.dev_addr != 0xA2) {
			err = EINVAL;
			break;
		}
		if (i2c.len > sizeof(i2c.data)) {
			err = EINVAL;
			break;
		}

		if ((err = IFDI_I2C_REQ(sctx, &i2c)) == 0)
			err = copyout(&i2c, ifr->ifr_data, sizeof(i2c));
		break;
	}
	case SIOCSIFCAP:
	    {
		int mask, reinit;

		reinit = 0;
		mask = ifr->ifr_reqcap ^ if_getcapenable(ifp);

#ifdef TCP_OFFLOAD
		if (mask & IFCAP_TOE4) {
			if_togglecapenable(ifp, IFCAP_TOE4);
			reinit = 1;
		}
#endif
		if (mask & IFCAP_HWCSUM) {
			if_togglecapenable(ifp, IFCAP_HWCSUM);
			reinit = 1;
		}
		if (mask & IFCAP_TSO4) {
			if_togglecapenable(ifp, IFCAP_TSO4);
			reinit = 1;
		}
		if (mask & IFCAP_TSO6) {
			if_togglecapenable(ifp, IFCAP_TSO6);
			reinit = 1;
		}
		if (mask & IFCAP_VLAN_HWTAGGING) {
			if_togglecapenable(ifp, IFCAP_VLAN_HWTAGGING);
			reinit = 1;
		}
		if (mask & IFCAP_VLAN_MTU) {
			if_togglecapenable(ifp, IFCAP_VLAN_MTU);
			reinit = 1;
		}
		if (mask & IFCAP_VLAN_HWFILTER) {
			if_togglecapenable(ifp, IFCAP_VLAN_HWFILTER);
			reinit = 1;
		}
		if (mask & IFCAP_VLAN_HWTSO) {
			if_togglecapenable(ifp, IFCAP_VLAN_HWTSO);
			reinit = 1;
		}
		if ((mask & IFCAP_WOL) &&
		    (if_getcapabilities(ifp) & IFCAP_WOL) != 0) {
			if (mask & IFCAP_WOL_MCAST)
				if_togglecapenable(ifp, IFCAP_WOL_MCAST);
			if (mask & IFCAP_WOL_MAGIC)
				if_togglecapenable(ifp, IFCAP_WOL_MAGIC);
		}
		if (reinit && (if_getdrvflags(ifp) & IFF_DRV_RUNNING)) {
			iflib_init(sctx);
		}
		if_vlancap(ifp);
		break;
	    }

	default:
		err = ether_ioctl(ifp, command, data);
		break;
	}

	return (err);
}

static uint64_t
iflib_if_get_counter(if_t ifp, ift_counter cnt)
{
	iflib_ctx_t ctx = if_getsoftc(ifp);

	return (IFDI_GET_COUNTER(ctx->ifc_sctx, cnt));
}

uint64_t
iflib_get_counter_default(if_shared_ctx_t sctx, ift_counter cnt)
{
	struct if_common_stats *stats = &sctx->isc_common_stats;

	switch (cnt) {
	case IFCOUNTER_COLLISIONS:
		return (stats->ics_colls);
	case IFCOUNTER_IERRORS:
		return (stats->ics_ierrs);
	case IFCOUNTER_OERRORS:
		return (stats->ics_ierrs);
	default:
		return (if_get_counter_default(sctx->isc_ifp, cnt));
	}
}

/*********************************************************************
 *
 *  OTHER FUNCTIONS EXPORTED TO THE STACK
 *
 **********************************************************************/

static void
iflib_vlan_register(void *arg, if_t ifp, uint16_t vtag)
{
	iflib_ctx_t ctx = if_getsoftc(ifp);

	if ((void *)ctx != arg)
		return;

	if ((vtag == 0) || (vtag > 4095))
		return;

	CTX_LOCK(ctx);
	IFDI_VLAN_REGISTER(ctx->ifc_sctx, vtag);
	/* Re-init to load the changes */
	if (if_getcapenable(ifp) & IFCAP_VLAN_HWFILTER)
		iflib_init_locked(ctx);
	CTX_UNLOCK(ctx);
}

static void
iflib_vlan_unregister(void *arg, if_t ifp, uint16_t vtag)
{
	iflib_ctx_t ctx = if_getsoftc(ifp);

	if ((void *)ctx != arg)
		return;

	if ((vtag == 0) || (vtag > 4095))
		return;

	CTX_LOCK(ctx);
	IFDI_VLAN_UNREGISTER(ctx->ifc_sctx, vtag);
	/* Re-init to load the changes */
	if (if_getcapenable(ifp) & IFCAP_VLAN_HWFILTER)
		iflib_init_locked(ctx);
	CTX_UNLOCK(ctx);
}

static void
iflib_led_func(void *arg, int onoff)
{
	if_shared_ctx_t sctx = arg;

	SCTX_LOCK(sctx);
	IFDI_LED_FUNC(sctx, onoff);
	SCTX_UNLOCK(sctx);
}

/*********************************************************************
 *
 *  BUS FUNCTION DEFINITIONS
 *
 **********************************************************************/

int
iflib_device_detach(device_t dev)
{
	if_shared_ctx_t sctx = device_get_softc(dev);
	iflib_ctx_t ctx = sctx->isc_ctx;
	if_t ifp = sctx->isc_ifp;
	iflib_txq_t txq;
	int i;

	/* Make sure VLANS are not using driver */
	if (if_vlantrunkinuse(ifp)) {
		device_printf(dev,"Vlan in use, detach first\n");
		return (EBUSY);
	}

	CTX_LOCK(ctx);
	ctx->ifc_in_detach = 1;
	iflib_stop(ctx);
	CTX_UNLOCK(ctx);
	CTX_LOCK_DESTROY(ctx);

	/* Unregister VLAN events */
	if (ctx->ifc_vlan_attach_event != NULL)
		EVENTHANDLER_DEREGISTER(vlan_config, ctx->ifc_vlan_attach_event);
	if (ctx->ifc_vlan_detach_event != NULL)
		EVENTHANDLER_DEREGISTER(vlan_unconfig, ctx->ifc_vlan_detach_event);

	ether_ifdetach(sctx->isc_ifp);
	if (ctx->ifc_led_dev != NULL)
		led_destroy(ctx->ifc_led_dev);
	/* XXX drain any dependent tasks */
	IFDI_DETACH(sctx);
	for (txq = ctx->ifc_txqs, i = 0; i < sctx->isc_nqsets; i++, txq++)
		callout_drain(&txq->ift_timer);

#ifdef DEV_NETMAP
	netmap_detach(ifp);
#endif /* DEV_NETMAP */

	bus_generic_detach(dev);
	if_free(sctx->isc_ifp);

	iflib_tx_structures_free(sctx);
	iflib_rx_structures_free(sctx);
	return (0);
}

int
iflib_device_suspend(device_t dev)
{
	if_shared_ctx_t sctx = device_get_softc(dev);

	SCTX_LOCK(sctx);
	IFDI_SUSPEND(sctx);
	SCTX_UNLOCK(sctx);

	return bus_generic_suspend(dev);
}

int
iflib_device_resume(device_t dev)
{
	if_shared_ctx_t sctx = device_get_softc(dev);
	iflib_txq_t txq = sctx->isc_ctx->ifc_txqs;

	SCTX_LOCK(sctx);
	IFDI_RESUME(sctx);
	iflib_init_locked(sctx->isc_ctx);
	SCTX_UNLOCK(sctx);
	for (int i = 0; i < sctx->isc_nqsets; i++, txq++)
		if (TXQ_TRYLOCK(txq)) {
			iflib_txq_start(txq);
			TXQ_UNLOCK(txq);
		}
	return (bus_generic_resume(dev));
}

/*********************************************************************
 *
 *  MODULE FUNCTION DEFINITIONS
 *
 **********************************************************************/

/*
 * - Start a fast taskqueue thread for each core
 * - Start a taskqueue for control operations
 */
static int
iflib_module_init(void)
{

	gctx = &global_ctx;
	gctx->igc_io_tqg = qgroup_if_io_tqg;
	gctx->igc_config_tqg = qgroup_if_config_tqg;

	return (0);
}

static int
iflib_module_event_handler(module_t mod, int what, void *arg)
{
	int err;

	switch (what) {
	case MOD_LOAD:
		if ((err = iflib_module_init()) != 0)
			return (err);
		break;
	case MOD_UNLOAD:
		return (EBUSY);
	default:
		return (EOPNOTSUPP);
	}

	return (0);
}

/*********************************************************************
 *
 *  PUBLIC FUNCTION DEFINITIONS
 *     ordered as in iflib.h
 *
 **********************************************************************/

int
iflib_register(device_t dev, driver_t *driver, uint8_t addr[ETH_ADDR_LEN])
{
	if_shared_ctx_t sctx = device_get_softc(dev);
	iflib_ctx_t ctx;
	if_t ifp;

	ctx = malloc(sizeof(struct iflib_ctx), M_DEVBUF, M_WAITOK);
	if (ctx == NULL)
		return (ENOMEM);
	CTX_LOCK_INIT(ctx, device_get_nameunit(dev));
	sctx->isc_ctx = ctx;
	ctx->ifc_sctx = sctx;
	ifp = sctx->isc_ifp = if_gethandle(IFT_ETHER);
	if (ifp == NULL) {
		device_printf(dev, "can not allocate ifnet structure\n");
		return (ENOMEM);
	}

	/*
	 * Initialize our context's device specific methods
	 */
	kobj_init((kobj_t) sctx, (kobj_class_t) driver);
	kobj_class_compile((kobj_class_t) driver);
	driver->refs++;

	if_initname(ifp, device_get_name(dev), device_get_unit(dev));
	if_setsoftc(ifp, ctx);
	if_setdev(ifp, dev);
	if_setinitfn(ifp, iflib_if_init);
	if_setioctlfn(ifp, iflib_if_ioctl);
	if_settransmitfn(ifp, iflib_if_transmit);
	if_setqflushfn(ifp, iflib_if_qflush);
	if_setgetcounterfn(ifp, iflib_if_get_counter);
	if_setflags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST);
	ether_ifattach(ifp, addr);

	if_setcapabilities(ifp, 0);
	if_setcapenable(ifp, 0);

	ctx->ifc_vlan_attach_event =
		EVENTHANDLER_REGISTER(vlan_config, iflib_vlan_register, ctx,
							  EVENTHANDLER_PRI_FIRST);
	ctx->ifc_vlan_detach_event =
		EVENTHANDLER_REGISTER(vlan_unconfig, iflib_vlan_unregister, ctx,
							  EVENTHANDLER_PRI_FIRST);

	ifmedia_init(&sctx->isc_media, IFM_IMASK,
					 iflib_media_change, iflib_media_status);

	return (0);
}

int
iflib_queues_alloc(if_shared_ctx_t sctx, uint32_t *qsizes, uint8_t nqs)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	device_t dev = sctx->isc_dev;
	int nqsets = sctx->isc_nqsets;
	iflib_txq_t txq = NULL;
	iflib_rxq_t rxq = NULL;
	iflib_qset_t qset = NULL;
	iflib_fl_t fl = NULL;
	int i, j, err, txconf, rxconf;
	iflib_dma_info_t ifdip;
	int nfree_lists = sctx->isc_nfl ? sctx->isc_nfl : 1; 
	int nbuf_rings = 1; /* XXX determine dynamically */

	if (!(qset =
	    (iflib_qset_t) malloc(sizeof(struct iflib_qset) *
	    nqsets, M_DEVBUF, M_NOWAIT | M_ZERO))) {
		device_printf(dev, "Unable to allocate TX ring memory\n");
		err = ENOMEM;
		goto fail;
	}

	ctx->ifc_qsets = qset;

/* Allocate the TX ring struct memory */
	if (!(txq =
	    (iflib_txq_t) malloc(sizeof(struct iflib_txq) *
	    nqsets, M_DEVBUF, M_NOWAIT | M_ZERO))) {
		device_printf(dev, "Unable to allocate TX ring memory\n");
		err = ENOMEM;
		goto fail;
	}

	/* Now allocate the RX */
	if (!(rxq =
	    (iflib_rxq_t) malloc(sizeof(struct iflib_rxq) *
	    nqsets, M_DEVBUF, M_NOWAIT | M_ZERO))) {
		device_printf(dev, "Unable to allocate RX ring memory\n");
		err = ENOMEM;
		goto rx_fail;
	}

	/*
	 * XXX handle allocation failure
	 */
	for (qset = ctx->ifc_qsets, rxconf = txconf = i = 0; i < nqsets;
		 i++, txconf++, rxconf++, qset++) {
		/* Set up some basics */

		if ((ifdip = malloc(sizeof(struct iflib_dma_info) * nqs, M_DEVBUF, M_WAITOK)) == NULL) {
			err = ENOMEM;
			goto fail;
		}
		qset->ifq_ifdi = ifdip;
		qset->ifq_nhwqs = nqs;
		for (j = 0; j < nqs; j++, ifdip++) {
			if (iflib_dma_alloc(ctx, qsizes[j], ifdip, BUS_DMA_NOWAIT)) {
				device_printf(dev, "Unable to allocate Descriptor memory\n");
				err = ENOMEM;
				goto err_tx_desc;
			}
			bzero((void *)ifdip->idi_vaddr, qsizes[j]);
		}
		txq = &ctx->ifc_txqs[i];
		txq->ift_ctx = ctx;
		txq->ift_id = i;
		txq->ift_timer.c_cpu = i % mp_ncpus;
		txq->ift_nbr = nbuf_rings;
		txq->ift_ifdi = &qset->ifq_ifdi[0];

		if (iflib_txsd_alloc(txq)) {
			device_printf(dev,
						  "Critical Failure setting up transmit buffers\n");
			err = ENOMEM;
			goto err_tx_desc;
		}

		/* Initialize the TX lock */
		snprintf(txq->ift_mtx_name, sizeof(txq->ift_mtx_name), "%s:tx(%d)",
		    device_get_nameunit(dev), txq->ift_id);
		mtx_init(&txq->ift_mtx, txq->ift_mtx_name, NULL, MTX_DEF);
		callout_init_mtx(&txq->ift_timer, &txq->ift_mtx, 0);

		/* Allocate a buf ring */
		for (j = 0; j < nbuf_rings; j++)
			if ((txq->ift_br[j] = buf_ring_alloc(4096, M_DEVBUF,
												 M_WAITOK, &txq->ift_mtx)) == NULL) {
				device_printf(dev, "Unable to allocate buf_ring\n");
				err = ENOMEM;
				goto fail;
			}

		/*
     * Next the RX queues...
	 */
		rxq = &ctx->ifc_rxqs[i];
		rxq->ifr_ctx = ctx;
		rxq->ifr_id = i;
		rxq->ifr_ifdi = &qset->ifq_ifdi[1];
		rxq->ifr_nfl = nfree_lists; 
		if (!(fl =
			  (iflib_fl_t) malloc(sizeof(struct iflib_fl) * nfree_lists, M_DEVBUF, M_NOWAIT | M_ZERO))) {
			device_printf(dev, "Unable to allocate free list memory\n");
			err = ENOMEM;
			goto fail;
		}
		rxq->ifr_fl = fl;
		for (j = 0; j < nfree_lists; j++) {
			rxq->ifr_fl[j].ifl_rxq = rxq;
			rxq->ifr_fl[j].ifl_id = j;
		}
        /* Allocate receive buffers for the ring*/
		if (iflib_rxsd_alloc(rxq)) {
			device_printf(dev,
			    "Critical Failure setting up receive buffers\n");
			err = ENOMEM;
			goto err_rx_desc;
		}

		/* Initialize the RX lock */
		snprintf(rxq->ifr_mtx_name, sizeof(rxq->ifr_mtx_name), "%s:rx(%d)",
		    device_get_nameunit(dev), rxq->ifr_id);
		mtx_init(&rxq->ifr_mtx, rxq->ifr_mtx_name, NULL, MTX_DEF);
	}
	ctx->ifc_txqs = txq;
	ctx->ifc_rxqs = rxq;
	if ((err = IFDI_QUEUES_ALLOC(sctx)) != 0)
		iflib_tx_structures_free(sctx);

	return (0);
err_rx_desc:
err_tx_desc:
	free(ctx->ifc_rxqs, M_DEVBUF);
rx_fail:
	free(ctx->ifc_txqs, M_DEVBUF);
fail:
	return (err);
}

static int
iflib_tx_structures_setup(if_shared_ctx_t sctx)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_txq_t txq = ctx->ifc_txqs;
	int i;

	for (i = 0; i < sctx->isc_nqsets; i++, txq++)
		iflib_txq_setup(txq);

	return (0);
}

static void
iflib_tx_structures_free(if_shared_ctx_t sctx)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_txq_t txq = ctx->ifc_txqs;
	iflib_qset_t qset = ctx->ifc_qsets;
	int i, j;

	for (i = 0; i < sctx->isc_nqsets; i++, txq++, qset++) {
		iflib_txq_destroy(txq);
		for (j = 0; j < qset->ifq_nhwqs; j++)
			iflib_dma_free(&qset->ifq_ifdi[j]);
	}
	free(ctx->ifc_txqs, M_DEVBUF);
	free(ctx->ifc_qsets, M_DEVBUF);
	IFDI_QSET_STRUCTURES_FREE(sctx);
}

/*********************************************************************
 *
 *  Initialize all receive rings.
 *
 **********************************************************************/
static int
iflib_rx_structures_setup(if_shared_ctx_t sctx)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_rxq_t rxq = ctx->ifc_rxqs;
	iflib_fl_t fl;
	int i,  q, err;

	for (q = 0; q < sctx->isc_nrxq; q++, rxq++) {
		RXQ_LOCK(rxq);
		tcp_lro_free(&rxq->ifr_lc);
		for (i = 0, fl = rxq->ifr_fl; i < rxq->ifr_nfl; i++, fl++)
			if (iflib_fl_setup(fl)) {
				err = ENOBUFS;
				goto fail;
			}
		if (sctx->isc_ifp->if_capenable & IFCAP_LRO) {
			if ((err = tcp_lro_init(&rxq->ifr_lc)) != 0) {
				device_printf(sctx->isc_dev, "LRO Initialization failed!\n");
				goto fail;
			}
			rxq->ifr_lro_enabled = TRUE;
			rxq->ifr_lc.ifp = sctx->isc_ifp;
		}

		IFDI_RXQ_SETUP(sctx, rxq->ifr_id);
		RXQ_UNLOCK(rxq);
	}
	return (0);
fail:
	/*
	 * Free RX software descriptors allocated so far, we will only handle
	 * the rings that completed, the failing case will have
	 * cleaned up for itself. 'q' failed, so its the terminus.
	 */
	rxq = ctx->ifc_rxqs;
	for (i = 0; i < q; ++i, rxq++) {
		iflib_rx_sds_free(rxq);
		rxq->ifr_cidx = rxq->ifr_pidx = 0;
	}
	RXQ_UNLOCK(rxq);
	return (err);
}

/*********************************************************************
 *
 *  Free all receive rings.
 *
 **********************************************************************/
static void
iflib_rx_structures_free(if_shared_ctx_t sctx)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_rxq_t rxq = ctx->ifc_rxqs;

	for (int i = 0; i < sctx->isc_nrxq; i++, rxq++) {
		iflib_rx_sds_free(rxq);
	}
}

int
iflib_qset_structures_setup(if_shared_ctx_t sctx)
{
	int err;

	if ((err = iflib_tx_structures_setup(sctx)) != 0)
		return (err);

	if ((err = iflib_rx_structures_setup(sctx)) != 0) {
		iflib_tx_structures_free(sctx);
		iflib_rx_structures_free(sctx);
	}
	return (err);
}

int
iflib_qset_addr_get(if_shared_ctx_t sctx, int qidx, caddr_t *vaddrs, uint64_t *paddrs, int nqs)
{
	iflib_dma_info_t di = sctx->isc_ctx->ifc_qsets[qidx].ifq_ifdi;
	int i, nhwqs = sctx->isc_ctx->ifc_qsets[qidx].ifq_nhwqs;

	if (nqs != nhwqs)
		return (EINVAL);
	for (i = 0; i < nhwqs; i++, di++) {
		vaddrs[i] = di->idi_vaddr;
		paddrs[i] = di->idi_paddr;
	}
	return (0);
}

int
iflib_irq_alloc(if_shared_ctx_t sctx, if_irq_t irq, int rid,
				driver_filter_t filter, driver_intr_t handler, void *arg, char *name)
{

	return (_iflib_irq_alloc(sctx->isc_ctx, irq, rid, filter, handler, arg, name));
}

int
iflib_irq_alloc_generic(if_shared_ctx_t sctx, if_irq_t irq, int rid,
						intr_type_t type, driver_filter_t *filter,
						void *filter_arg, int qid, char *name)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	struct grouptask *gtask;
	struct taskqgroup *tqg;
	iflib_filter_info_t info;
	int tqrid;
	void *q;
	int err;

	switch (type) {
	case IFLIB_INTR_TX:
		q = &ctx->ifc_txqs[qid];
		info = &ctx->ifc_txqs[qid].ift_filter_info;
		gtask = &ctx->ifc_txqs[qid].ift_task;
		tqg = gctx->igc_io_tqg;
		tqrid = irq->ii_rid;
		break;
	case IFLIB_INTR_RX:
		q = &ctx->ifc_rxqs[qid];
		info = &ctx->ifc_rxqs[qid].ifr_filter_info;
		gtask = &ctx->ifc_rxqs[qid].ifr_task;
		tqg = gctx->igc_io_tqg;
		tqrid = irq->ii_rid;
		break;
	case IFLIB_INTR_LINK:
		q = ctx;
		info = &ctx->ifc_filter_info;
		gtask = &ctx->ifc_link_task;
		tqg = gctx->igc_config_tqg;
		tqrid = -1;
		break;
	default:
		panic("unknown net intr type");
	}
	info->ifi_filter = filter;
	info->ifi_filter_arg = filter_arg;
	info->ifi_task = gtask;

	err = _iflib_irq_alloc(ctx, irq, rid, iflib_fast_intr, NULL, info,  name);
	if (err != 0)
		return (err);
	taskqgroup_attach(tqg, gtask, q, tqrid, name);
	return (0);
}

int
iflib_legacy_setup(if_shared_ctx_t sctx, driver_filter_t filter, int *rid)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_txq_t txq = ctx->ifc_txqs;
	iflib_rxq_t rxq = ctx->ifc_rxqs;
	if_irq_t irq = &ctx->ifc_legacy_irq;
	int err;

	ctx->ifc_flags |= IFLIB_LEGACY;
	/* We allocate a single interrupt resource */
	if ((err = iflib_irq_alloc(sctx, irq, *rid, filter, NULL, sctx, NULL)) != 0)
		return (err);

	/*
	 * Allocate a fast interrupt and the associated
	 * deferred processing contexts.
	 *
	 */
	GROUPTASK_INIT(&txq->ift_task, 0, _task_fn_tx, txq);
	taskqgroup_attach(gctx->igc_io_tqg, &txq->ift_task, txq, irq->ii_rid, "tx");
	GROUPTASK_INIT(&rxq->ifr_task, 0, _task_fn_rx, rxq);
	taskqgroup_attach(gctx->igc_io_tqg, &rxq->ifr_task, rxq, irq->ii_rid, "rx");
	GROUPTASK_INIT(&ctx->ifc_link_task, 0, _task_fn_link, ctx);
	taskqgroup_attach(gctx->igc_config_tqg, &ctx->ifc_link_task, ctx, -1, "link");

	return (0);
}

void
iflib_led_create(if_shared_ctx_t sctx)
{
	iflib_ctx_t ctx = sctx->isc_ctx;

	ctx->ifc_led_dev = led_create(iflib_led_func, sctx,
								  device_get_nameunit(sctx->isc_dev));
}

void
iflib_init(if_shared_ctx_t sctx)
{
	SCTX_LOCK(sctx);
	iflib_init_locked(sctx->isc_ctx);
	SCTX_UNLOCK(sctx);
}

void
iflib_tx_intr_deferred(if_shared_ctx_t sctx, int txqid)
{

	GROUPTASK_ENQUEUE(&sctx->isc_ctx->ifc_txqs[txqid].ift_task);
}

void
iflib_rx_intr_deferred(if_shared_ctx_t sctx, int rxqid)
{

	GROUPTASK_ENQUEUE(&sctx->isc_ctx->ifc_rxqs[rxqid].ifr_task);
}

void
iflib_link_intr_deferred(if_shared_ctx_t sctx)
{

	GROUPTASK_ENQUEUE(&sctx->isc_ctx->ifc_link_task);
}

void
iflib_link_state_change(if_shared_ctx_t sctx, uint64_t baudrate, int link_state)
{
	if_t ifp = sctx->isc_ifp;
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_txq_t txq = ctx->ifc_txqs;

	if_setbaudrate(ifp, baudrate);
	/* If link down, disable watchdog */
	if ((ctx->ifc_link_state == LINK_STATE_UP) && (link_state == LINK_STATE_DOWN)) {
		for (int i = 0; i < sctx->isc_nqsets; i++, txq++)
			txq->ift_qstatus = IFLIB_QUEUE_IDLE;
	}
	ctx->ifc_link_state = link_state;
	if_link_state_change(ifp, link_state);
}

int
iflib_tx_cidx_get(if_shared_ctx_t sctx, int txqid)
{

	return (sctx->isc_ctx->ifc_txqs[txqid].ift_cidx);
}

void
iflib_tx_credits_update(if_shared_ctx_t sctx, int txqid, int credits)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	ctx->ifc_txqs[txqid].ift_processed += credits;
}

void
iflib_add_int_delay_sysctl(if_shared_ctx_t sctx, const char *name,
	const char *description, if_int_delay_info_t info,
	int offset, int value)
{
	info->iidi_sctx = sctx;
	info->iidi_offset = offset;
	info->iidi_value = value;
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(sctx->isc_dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(sctx->isc_dev)),
	    OID_AUTO, name, CTLTYPE_INT|CTLFLAG_RW,
	    info, 0, iflib_sysctl_int_delay, "I", description);
}

void
iflib_taskqgroup_attach(struct grouptask *gtask, void *uniq, char *name)
{

	taskqgroup_attach(gctx->igc_config_tqg, gtask, uniq, -1, name);
}

struct mtx *
iflib_sctx_lock_get(if_shared_ctx_t sctx)
{

	return (&sctx->isc_ctx->ifc_mtx);
}

struct mtx *
iflib_qset_lock_get(if_shared_ctx_t sctx, uint16_t qsidx)
{

	return (&sctx->isc_ctx->ifc_txqs[qsidx].ift_mtx);
}
