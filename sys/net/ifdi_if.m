#
# Copyright (c) 2014, Matthew Macy (kmacy@freebsd.org)
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#  1. Redistributions of source code must retain the above copyright notice,
#     this list of conditions and the following disclaimer.
#
#  2. Neither the name of Matthew Macy nor the names of its
#     contributors may be used to endorse or promote products derived from
#     this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/socket.h>

#include <machine/bus.h>
#include <sys/bus.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net/iflib.h>

INTERFACE ifdi;

CODE {

	static void
	null_void_op(if_shared_ctx_t _ctx __unused)
	{
	}

	static void
	null_timer_op(if_shared_ctx_t _ctx __unused, uint16_t _qsidx __unused)
	{
	}

	static int
	null_int_op(if_shared_ctx_t _ctx __unused)
	{
		return (0);
	}

	static void
	null_queue_intr_enable(if_shared_ctx_t _ctx __unused, uint32_t _qid __unused)
	{
	}

	static void
	null_led_func(if_shared_ctx_t _ctx __unused, int _onoff __unused)
	{
	}

	static void
	null_vlan_register_op(if_shared_ctx_t _ctx __unused, uint16_t vtag __unused)
	{
	}

	static int
	null_q_setup(if_shared_ctx_t _ctx __unused, uint32_t _qid __unused)
	{
		return (0);
	}

	static int
	null_i2c_req(if_shared_ctx_t _sctx __unused, struct ifi2creq *_i2c __unused)
	{
		return (ENOTSUP);
	}

	static int
	null_sysctl_int_delay(if_shared_ctx_t _sctx __unused, if_int_delay_info_t _iidi __unused)
	{
		return (0);
	}
};

#
# bus interfaces
#

METHOD int detach {
	if_shared_ctx_t _ctx;
};

METHOD int suspend {
	if_shared_ctx_t _ctx;
} DEFAULT null_int_op;

METHOD int resume {
	if_shared_ctx_t _ctx;
} DEFAULT null_int_op;

#
# downcall to driver to allocate its
# own queue state and tie it to the parent
#

METHOD int queues_alloc {
	if_shared_ctx_t _ctx;
};

METHOD void tx_structures_free {
	if_shared_ctx_t _ctx;
};

METHOD void rx_structures_free {
	if_shared_ctx_t _ctx;
};

#
# interface reset / stop
#

METHOD void init {
	if_shared_ctx_t _ctx;
};

METHOD void stop {
	if_shared_ctx_t _ctx;
};

#
# interrupt manipulation
#

METHOD void intr_enable {
	if_shared_ctx_t _ctx;
};

METHOD void intr_disable {
	if_shared_ctx_t _ctx;
};

METHOD void rx_intr_enable {
	if_shared_ctx_t _ctx;
	uint32_t _rxqid;
} DEFAULT null_queue_intr_enable;

METHOD void link_intr_enable {
	if_shared_ctx_t _ctx;
} DEFAULT null_void_op;

#
# interface configuration
#

METHOD void multi_set {
	if_shared_ctx_t _ctx;
};

METHOD int mtu_set {
	if_shared_ctx_t _ctx;
	uint32_t _mtu;
};

METHOD void media_set{
	if_shared_ctx_t _ctx;
} DEFAULT null_void_op;

METHOD void promisc_set {
	if_shared_ctx_t _ctx;
	int _flags;
};

#
# Device status
#

METHOD void update_link_status {
	if_shared_ctx_t _ctx;
};

METHOD void media_status {
	if_shared_ctx_t _ctx;
	struct ifmediareq *_ifm;
};

METHOD int media_change {
	if_shared_ctx_t _ctx;
};


#
# optional methods
#

METHOD int i2c_req {
	if_shared_ctx_t _ctx;
	struct ifi2creq *_req;
} DEFAULT null_i2c_req;

METHOD int txq_setup {
	if_shared_ctx_t _ctx;
	uint32_t _txqid;
} DEFAULT null_q_setup;

METHOD int rxq_setup {
	if_shared_ctx_t _ctx;
	uint32_t _txqid;
} DEFAULT null_q_setup;

METHOD void timer {
	if_shared_ctx_t _ctx;
	uint16_t _txqid;
} DEFAULT null_timer_op;

METHOD void watchdog_reset {
	if_shared_ctx_t _ctx;
} DEFAULT null_void_op;

METHOD void led_func {
	if_shared_ctx_t _ctx;
	int _onoff;
} DEFAULT null_led_func;

METHOD void vlan_register {
	if_shared_ctx_t _ctx;
	uint16_t _vtag;
} DEFAULT null_vlan_register_op;

METHOD void vlan_unregister {
	if_shared_ctx_t _ctx;
	uint16_t _vtag;
} DEFAULT null_vlan_register_op;

METHOD int sysctl_int_delay {
	if_shared_ctx_t _sctx;
	if_int_delay_info_t _iidi;
} DEFAULT null_sysctl_int_delay;
