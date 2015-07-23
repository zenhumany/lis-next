/*
 * Copyright (c) 2009, Microsoft Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 * Authors:
 *   Haiyang Zhang <haiyangz@microsoft.com>
 *   Hank Janssen  <hjanssen@microsoft.com>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/completion.h>
#include "include/linux/hyperv.h"

#include "hyperv_vmbus.h"

struct vmbus_channel_message_table_entry {
	enum vmbus_channel_message_type message_type;
	void (*message_handler)(struct vmbus_channel_message_header *msg);
};


/**
 * vmbus_prep_negotiate_resp() - Create default response for Hyper-V Negotiate message
 * @icmsghdrp: Pointer to msg header structure
 * @icmsg_negotiate: Pointer to negotiate message structure
 * @buf: Raw buffer channel data
 *
 * @icmsghdrp is of type &struct icmsg_hdr.
 * @negop is of type &struct icmsg_negotiate.
 * Set up and fill in default negotiate response message.
 *
 * The fw_version specifies the  framework version that
 * we can support and srv_version specifies the service
 * version we can support.
 *
 * Mainly used by Hyper-V drivers.
 */
bool vmbus_prep_negotiate_resp(struct icmsg_hdr *icmsghdrp,
				struct icmsg_negotiate *negop, u8 *buf,
				int fw_version, int srv_version)
{
	int icframe_major, icframe_minor;
	int icmsg_major, icmsg_minor;
	int fw_major, fw_minor;
	int srv_major, srv_minor;
	int i;
	bool found_match = false;

	icmsghdrp->icmsgsize = 0x10;
	fw_major = (fw_version >> 16);
	fw_minor = (fw_version & 0xFFFF);

	srv_major = (srv_version >> 16);
	srv_minor = (srv_version & 0xFFFF);

	negop = (struct icmsg_negotiate *)&buf[
		sizeof(struct vmbuspipe_hdr) +
		sizeof(struct icmsg_hdr)];

	icframe_major = negop->icframe_vercnt;
	icframe_minor = 0;

	icmsg_major = negop->icmsg_vercnt;
	icmsg_minor = 0;

	/*
	 * Select the framework version number we will
	 * support.
	 */

	for (i = 0; i < negop->icframe_vercnt; i++) {
		if ((negop->icversion_data[i].major == fw_major) &&
		   (negop->icversion_data[i].minor == fw_minor)) {
			icframe_major = negop->icversion_data[i].major;
			icframe_minor = negop->icversion_data[i].minor;
			found_match = true;
		}
	}

	if (!found_match)
		goto fw_error;

	found_match = false;

	for (i = negop->icframe_vercnt;
		 (i < negop->icframe_vercnt + negop->icmsg_vercnt); i++) {
		if ((negop->icversion_data[i].major == srv_major) &&
		   (negop->icversion_data[i].minor == srv_minor)) {
			icmsg_major = negop->icversion_data[i].major;
			icmsg_minor = negop->icversion_data[i].minor;
			found_match = true;
		}
	}

	/*
	 * Respond with the framework and service
	 * version numbers we can support.
	 */

fw_error:
	if (!found_match) {
		negop->icframe_vercnt = 0;
		negop->icmsg_vercnt = 0;
	} else {
		negop->icframe_vercnt = 1;
		negop->icmsg_vercnt = 1;
	}

	negop->icversion_data[0].major = icframe_major;
	negop->icversion_data[0].minor = icframe_minor;
	negop->icversion_data[1].major = icmsg_major;
	negop->icversion_data[1].minor = icmsg_minor;
	return found_match;
}

EXPORT_SYMBOL_GPL(vmbus_prep_negotiate_resp);


static void vmbus_process_device_unregister(struct work_struct *work)
{
	struct device *dev;
	struct vmbus_channel *channel = container_of(work,
							struct vmbus_channel,
							work);

	dev = get_device(&channel->device_obj->device);
	if (dev) {
		vmbus_device_unregister(channel->device_obj);
		put_device(dev);
	}
}

static void vmbus_sc_creation_cb(struct work_struct *work)
{
	struct vmbus_channel *newchannel = container_of(work,
							struct vmbus_channel,
							work);
	struct vmbus_channel *primary_channel = newchannel->primary_channel;

	/*
	 * On entry sc_creation_callback has been already verified to
	 * be non-NULL.
	 */
	primary_channel->sc_creation_callback(newchannel);
}


/*
 * alloc_channel - Allocate and initialize a vmbus channel object
 */
static struct vmbus_channel *alloc_channel(void)
{
	struct vmbus_channel *channel;

	channel = kzalloc(sizeof(*channel), GFP_ATOMIC);
	if (!channel)
		return NULL;

	spin_lock_init(&channel->inbound_lock);
	spin_lock_init(&channel->lock);

	INIT_LIST_HEAD(&channel->sc_list);
	INIT_LIST_HEAD(&channel->percpu_list);

	channel->controlwq = create_workqueue("hv_vmbus_ctl");
	if (!channel->controlwq) {
		kfree(channel);
		return NULL;
	}

	return channel;
}

/*
 * release_hannel - Release the vmbus channel object itself
 */
static void release_channel(struct work_struct *work)
{
	struct vmbus_channel *channel = container_of(work,
						     struct vmbus_channel,
						     work);

	destroy_workqueue(channel->controlwq);

	kfree(channel);
}

/*
 * free_channel - Release the resources used by the vmbus channel object
 */
static void free_channel(struct vmbus_channel *channel)
{

	/*
	 * We have to release the channel's workqueue/thread in the vmbus's
	 * workqueue/thread context
	 * ie we can't destroy ourselves.
	 */
	INIT_WORK(&channel->work, release_channel);
	queue_work(vmbus_connection.work_queue, &channel->work);
}

static void percpu_channel_enq(void *arg)
{
	struct vmbus_channel *channel = arg;
	int cpu = smp_processor_id();

	list_add_tail(&channel->percpu_list, &hv_context.percpu_list[cpu]);
}

static void percpu_channel_deq(void *arg)
{
	struct vmbus_channel *channel = arg;

	list_del(&channel->percpu_list);
}


void vmbus_free_channels(void)
{
	struct vmbus_channel *channel;

	list_for_each_entry(channel, &vmbus_connection.chn_list, listentry) {
		vmbus_device_unregister(channel->device_obj);
		kfree(channel->device_obj);
		free_channel(channel);
	}
}

/*
 * vmbus_process_offer - Process the offer by creating a channel/device
 * associated with this offer
 */
static void vmbus_process_offer(struct vmbus_channel *newchannel)
{
	struct vmbus_channel *channel;
	bool fnew = true;
	bool enq = false;
	int ret;
	unsigned long flags;

	/* Make sure this is a new offer */
	spin_lock_irqsave(&vmbus_connection.channel_lock, flags);

	list_for_each_entry(channel, &vmbus_connection.chn_list, listentry) {
		if (!uuid_le_cmp(channel->offermsg.offer.if_type,
			newchannel->offermsg.offer.if_type) &&
			!uuid_le_cmp(channel->offermsg.offer.if_instance,
				newchannel->offermsg.offer.if_instance)) {
			fnew = false;
			break;
		}
	}

	if (fnew) {
		list_add_tail(&newchannel->listentry,
			      &vmbus_connection.chn_list);
		enq = true;
	}

	spin_unlock_irqrestore(&vmbus_connection.channel_lock, flags);

	if (enq) {
		if (newchannel->target_cpu != get_cpu()) {
			put_cpu();
			smp_call_function_single(newchannel->target_cpu,
						 percpu_channel_enq,
						 newchannel, true);
		} else {
			percpu_channel_enq(newchannel);
			put_cpu();
		}
	}
	if (!fnew) {
		/*
		 * Check to see if this is a sub-channel.
		 */
		if (newchannel->offermsg.offer.sub_channel_index != 0) {
			/*
			 * Process the sub-channel.
			 */
			newchannel->primary_channel = channel;
			spin_lock_irqsave(&channel->lock, flags);
			list_add_tail(&newchannel->sc_list, &channel->sc_list);
			spin_unlock_irqrestore(&channel->lock, flags);

			if (newchannel->target_cpu != get_cpu()) {
				put_cpu();
				smp_call_function_single(newchannel->target_cpu,
							 percpu_channel_enq,
							 newchannel, true);
			} else {
				percpu_channel_enq(newchannel);
				put_cpu();
			}

			newchannel->state = CHANNEL_OPEN_STATE;
			if (channel->sc_creation_callback != NULL)
			
				/*
				 * We need to invoke the sub-channel creation
				 * callback; invoke this in a seperate work
				 * context since we are currently running on
				 * the global work context in which we handle
				 * messages from the host.
				 */
				INIT_WORK(&newchannel->work,
					  vmbus_sc_creation_cb);
				queue_work(newchannel->controlwq,
					   &newchannel->work);

			return;		

		}

		goto err_free_chan;
	}

	/*
	 * This state is used to indicate a successful open
	 * so that when we do close the channel normally, we
	 * can cleanup properly
	 */
	newchannel->state = CHANNEL_OPEN_STATE;

	/*
	 * Start the process of binding this offer to the driver
	 * We need to set the DeviceObject field before calling
	 * vmbus_child_dev_add()
	 */
	newchannel->device_obj = vmbus_device_create(
		&newchannel->offermsg.offer.if_type,
		&newchannel->offermsg.offer.if_instance,
		newchannel);
	if (!newchannel->device_obj)
		goto err_free_chan;

	/*
	 * Add the new device to the bus. This will kick off device-driver
	 * binding which eventually invokes the device driver's AddDevice()
	 * method.
	 */
	ret = vmbus_device_register(newchannel->device_obj);
	if (ret != 0) {
		pr_err("unable to add child device object (relid %d)\n",
			   newchannel->offermsg.child_relid);

		spin_lock_irqsave(&vmbus_connection.channel_lock, flags);
		list_del(&newchannel->listentry);
		spin_unlock_irqrestore(&vmbus_connection.channel_lock, flags);
		kfree(newchannel->device_obj);
		goto err_free_chan;
	}

	return;
err_free_chan:
	free_channel(newchannel);
}

enum {
	IDE = 0,
	SCSI,
	NIC,
	MAX_PERF_CHN,
};

/*
 * This is an array of device_ids (device types) that are performance critical.
 * We attempt to distribute the interrupt load for these devices across
 * all available CPUs.
 */
static const struct hv_vmbus_device_id hp_devs[] = {
	/* IDE */
	{ HV_IDE_GUID, },
	/* Storage - SCSI */
	{ HV_SCSI_GUID, },
	/* Network */
	{ HV_NIC_GUID, },
};


/*
 * We use this state to statically distribute the channel interrupt load.
 */

static int next_numa_node_id;

/*
 * Starting with Win8, we can statically distribute the incoming
 * channel interrupt load by binding a channel to VCPU.
 * We do this in a hierarchical fashion:
 * First distribute the primary channels across available NUMA nodes
 * and then distribute the subchannels amongst the CPUs in the NUMA
 * node assigned to the primary channel.
 *
 * For pre-win8 hosts or non-performance critical channels we assign the
 * first CPU in the first NUMA node.
 */
static void init_vp_index(struct vmbus_channel *channel, const uuid_le *type_guid)
{
	u32 cur_cpu;
	int i;
	bool perf_chn = false;
	struct vmbus_channel *primary = channel->primary_channel;
        int next_node;
        struct cpumask available_mask;

	for (i = IDE; i < MAX_PERF_CHN; i++) {
		if (!memcmp(type_guid->b, hp_devs[i].guid,
				 sizeof(uuid_le))) {
			perf_chn = true;
			break;
		}
	}
	if ((vmbus_proto_version == VERSION_WS2008) ||
	    (vmbus_proto_version == VERSION_WIN7) || (!perf_chn)) {
		/*
		 * Prior to win8, all channel interrupts are
		 * delivered on cpu 0.
		 * Also if the channel is not a performance critical
		 * channel, bind it to cpu 0.
		 */
		channel->numa_node = 0;
		cpumask_set_cpu(0, &channel->alloced_cpus_in_node);
		channel->target_cpu = 0;
		channel->target_vp = hv_context.vp_index[0];
		return;
	}

	/*
        * We distribute primary channels evenly across all the available
        * NUMA nodes and within the assigned NUMA node we will assign the
        * first available CPU to the primary channel.
        * The sub-channels will be assigned to the CPUs available in the
        * NUMA node evenly.
        */

        if (!primary) {
               while (true) {
                       next_node = next_numa_node_id++;
                       if (next_node == nr_node_ids)
                               next_node = next_numa_node_id = 0;
                       if (cpumask_empty(cpumask_of_node(next_node)))
                               continue;
                       break;
               }
               channel->numa_node = next_node;
               primary = channel;
        }

       if (cpumask_weight(&primary->alloced_cpus_in_node) ==
           cpumask_weight(cpumask_of_node(primary->numa_node))) {

	/* 
	*We have cycled through all the CPUs in the node;
        * reset the alloced map.
        */
               cpumask_clear(&primary->alloced_cpus_in_node);
        }

        cpumask_xor(&available_mask, &primary->alloced_cpus_in_node,
                   cpumask_of_node(primary->numa_node));

        cur_cpu = cpumask_next(-1, &available_mask);
        cpumask_set_cpu(cur_cpu, &primary->alloced_cpus_in_node);

	channel->target_cpu = cur_cpu;
	channel->target_vp = hv_context.vp_index[cur_cpu];
}

/*
 * vmbus_onoffer - Handler for channel offers from vmbus in parent partition.
 *
 */
static void vmbus_onoffer(struct vmbus_channel_message_header *hdr)
{
	struct vmbus_channel_offer_channel *offer;
	struct vmbus_channel *newchannel;

	offer = (struct vmbus_channel_offer_channel *)hdr;

	/* Allocate the channel object and save this offer. */
	newchannel = alloc_channel();
	if (!newchannel) {
		pr_err("Unable to allocate channel object\n");
		return;
	}

	/*
	 * By default we setup state to enable batched
	 * reading. A specific service can choose to
	 * disable this prior to opening the channel.
	 */
	newchannel->batched_reading = true;

	/*
	 * Setup state for signalling the host.
	 */
	newchannel->sig_event = (struct hv_input_signal_event *)
				(ALIGN((unsigned long)
				&newchannel->sig_buf,
				HV_HYPERCALL_PARAM_ALIGN));

	newchannel->sig_event->connectionid.asu32 = 0;
	newchannel->sig_event->connectionid.u.id = VMBUS_EVENT_CONNECTION_ID;
	newchannel->sig_event->flag_number = 0;
	newchannel->sig_event->rsvdz = 0;

	if (vmbus_proto_version != VERSION_WS2008) {
		newchannel->is_dedicated_interrupt =
				(offer->is_dedicated_interrupt != 0);
		newchannel->sig_event->connectionid.u.id =
				offer->connection_id;
	}

	init_vp_index(newchannel, &offer->offer.if_type);

	memcpy(&newchannel->offermsg, offer,
	       sizeof(struct vmbus_channel_offer_channel));
	newchannel->monitor_grp = (u8)offer->monitorid / 32;
	newchannel->monitor_bit = (u8)offer->monitorid % 32;
	
	vmbus_process_offer(newchannel);
}

/*
 * vmbus_onoffer_rescind - Rescind offer handler.
 *
 * We queue a work item to process this offer synchronously
 */
static void vmbus_onoffer_rescind(struct vmbus_channel_message_header *hdr)
{
	struct vmbus_channel_rescind_offer *rescind;
	struct vmbus_channel *channel;
	unsigned long flags;

	rescind = (struct vmbus_channel_rescind_offer *)hdr;
	channel = relid2channel(rescind->child_relid);
	
	if (channel == NULL) {
		hv_process_channel_removal(NULL, rescind->child_relid);
		return;

	spin_lock_irqsave(&channel->lock, flags);
	channel->rescind = true;

	spin_unlock_irqrestore(&channel->lock, flags);

	if (channel->device_obj) {
		/*
		 * We will have to unregister this device from the
		 * driver core. Do this in the per-channel work context.
		 * Note that we are currently executing on the global
		 * workq for handling messages from the host.
		 */
		INIT_WORK(&channel->work, vmbus_process_device_unregister);
		queue_work(channel->controlwq, &channel->work);
	} else {
		hv_process_channel_removal(channel,
					   channel->offermsg.child_relid);
	}

}

/*
 * vmbus_onoffers_delivered -
 * This is invoked when all offers have been delivered.
 *
 * Nothing to do here.
 */
static void vmbus_onoffers_delivered(
			struct vmbus_channel_message_header *hdr)
{
}

/*
 * vmbus_onopen_result - Open result handler.
 *
 * This is invoked when we received a response to our channel open request.
 * Find the matching request, copy the response and signal the requesting
 * thread.
 */
static void vmbus_onopen_result(struct vmbus_channel_message_header *hdr)
{
	struct vmbus_channel_open_result *result;
	struct vmbus_channel_msginfo *msginfo;
	struct vmbus_channel_message_header *requestheader;
	struct vmbus_channel_open_channel *openmsg;
	unsigned long flags;

	result = (struct vmbus_channel_open_result *)hdr;

	/*
	 * Find the open msg, copy the result and signal/unblock the wait event
	 */
	spin_lock_irqsave(&vmbus_connection.channelmsg_lock, flags);

	list_for_each_entry(msginfo, &vmbus_connection.chn_msg_list,
				msglistentry) {
		requestheader =
			(struct vmbus_channel_message_header *)msginfo->msg;

		if (requestheader->msgtype == CHANNELMSG_OPENCHANNEL) {
			openmsg =
			(struct vmbus_channel_open_channel *)msginfo->msg;
			if (openmsg->child_relid == result->child_relid &&
			    openmsg->openid == result->openid) {
				memcpy(&msginfo->response.open_result,
				       result,
				       sizeof(
					struct vmbus_channel_open_result));
				complete(&msginfo->waitevent);
				break;
			}
		}
	}
	spin_unlock_irqrestore(&vmbus_connection.channelmsg_lock, flags);
}

/*
 * vmbus_ongpadl_created - GPADL created handler.
 *
 * This is invoked when we received a response to our gpadl create request.
 * Find the matching request, copy the response and signal the requesting
 * thread.
 */
static void vmbus_ongpadl_created(struct vmbus_channel_message_header *hdr)
{
	struct vmbus_channel_gpadl_created *gpadlcreated;
	struct vmbus_channel_msginfo *msginfo;
	struct vmbus_channel_message_header *requestheader;
	struct vmbus_channel_gpadl_header *gpadlheader;
	unsigned long flags;

	gpadlcreated = (struct vmbus_channel_gpadl_created *)hdr;

	/*
	 * Find the establish msg, copy the result and signal/unblock the wait
	 * event
	 */
	spin_lock_irqsave(&vmbus_connection.channelmsg_lock, flags);

	list_for_each_entry(msginfo, &vmbus_connection.chn_msg_list,
				msglistentry) {
		requestheader =
			(struct vmbus_channel_message_header *)msginfo->msg;

		if (requestheader->msgtype == CHANNELMSG_GPADL_HEADER) {
			gpadlheader =
			(struct vmbus_channel_gpadl_header *)requestheader;

			if ((gpadlcreated->child_relid ==
			     gpadlheader->child_relid) &&
			    (gpadlcreated->gpadl == gpadlheader->gpadl)) {
				memcpy(&msginfo->response.gpadl_created,
				       gpadlcreated,
				       sizeof(
					struct vmbus_channel_gpadl_created));
				complete(&msginfo->waitevent);
				break;
			}
		}
	}
	spin_unlock_irqrestore(&vmbus_connection.channelmsg_lock, flags);
}

/*
 * vmbus_ongpadl_torndown - GPADL torndown handler.
 *
 * This is invoked when we received a response to our gpadl teardown request.
 * Find the matching request, copy the response and signal the requesting
 * thread.
 */
static void vmbus_ongpadl_torndown(
			struct vmbus_channel_message_header *hdr)
{
	struct vmbus_channel_gpadl_torndown *gpadl_torndown;
	struct vmbus_channel_msginfo *msginfo;
	struct vmbus_channel_message_header *requestheader;
	struct vmbus_channel_gpadl_teardown *gpadl_teardown;
	unsigned long flags;

	gpadl_torndown = (struct vmbus_channel_gpadl_torndown *)hdr;

	/*
	 * Find the open msg, copy the result and signal/unblock the wait event
	 */
	spin_lock_irqsave(&vmbus_connection.channelmsg_lock, flags);

	list_for_each_entry(msginfo, &vmbus_connection.chn_msg_list,
				msglistentry) {
		requestheader =
			(struct vmbus_channel_message_header *)msginfo->msg;

		if (requestheader->msgtype == CHANNELMSG_GPADL_TEARDOWN) {
			gpadl_teardown =
			(struct vmbus_channel_gpadl_teardown *)requestheader;

			if (gpadl_torndown->gpadl == gpadl_teardown->gpadl) {
				memcpy(&msginfo->response.gpadl_torndown,
				       gpadl_torndown,
				       sizeof(
					struct vmbus_channel_gpadl_torndown));
				complete(&msginfo->waitevent);
				break;
			}
		}
	}
	spin_unlock_irqrestore(&vmbus_connection.channelmsg_lock, flags);
}

/*
 * vmbus_onversion_response - Version response handler
 *
 * This is invoked when we received a response to our initiate contact request.
 * Find the matching request, copy the response and signal the requesting
 * thread.
 */
static void vmbus_onversion_response(
		struct vmbus_channel_message_header *hdr)
{
	struct vmbus_channel_msginfo *msginfo;
	struct vmbus_channel_message_header *requestheader;
	struct vmbus_channel_version_response *version_response;
	unsigned long flags;

	version_response = (struct vmbus_channel_version_response *)hdr;
	spin_lock_irqsave(&vmbus_connection.channelmsg_lock, flags);

	list_for_each_entry(msginfo, &vmbus_connection.chn_msg_list,
				msglistentry) {
		requestheader =
			(struct vmbus_channel_message_header *)msginfo->msg;

		if (requestheader->msgtype ==
		    CHANNELMSG_INITIATE_CONTACT) {
			memcpy(&msginfo->response.version_response,
			      version_response,
			      sizeof(struct vmbus_channel_version_response));
			complete(&msginfo->waitevent);
		}
	}
	spin_unlock_irqrestore(&vmbus_connection.channelmsg_lock, flags);
}

/* Channel message dispatch table */
static struct vmbus_channel_message_table_entry
	channel_message_table[CHANNELMSG_COUNT] = {
	{CHANNELMSG_INVALID,			NULL},
	{CHANNELMSG_OFFERCHANNEL,		vmbus_onoffer},
	{CHANNELMSG_RESCIND_CHANNELOFFER,	vmbus_onoffer_rescind},
	{CHANNELMSG_REQUESTOFFERS,		NULL},
	{CHANNELMSG_ALLOFFERS_DELIVERED,	vmbus_onoffers_delivered},
	{CHANNELMSG_OPENCHANNEL,		NULL},
	{CHANNELMSG_OPENCHANNEL_RESULT,	vmbus_onopen_result},
	{CHANNELMSG_CLOSECHANNEL,		NULL},
	{CHANNELMSG_GPADL_HEADER,		NULL},
	{CHANNELMSG_GPADL_BODY,		NULL},
	{CHANNELMSG_GPADL_CREATED,		vmbus_ongpadl_created},
	{CHANNELMSG_GPADL_TEARDOWN,		NULL},
	{CHANNELMSG_GPADL_TORNDOWN,		vmbus_ongpadl_torndown},
	{CHANNELMSG_RELID_RELEASED,		NULL},
	{CHANNELMSG_INITIATE_CONTACT,		NULL},
	{CHANNELMSG_VERSION_RESPONSE,		vmbus_onversion_response},
	{CHANNELMSG_UNLOAD,			NULL},
};

/*
 * vmbus_onmessage - Handler for channel protocol messages.
 *
 * This is invoked in the vmbus worker thread context.
 */
void vmbus_onmessage(void *context)
{
	struct hv_message *msg = context;
	struct vmbus_channel_message_header *hdr;
	int size;

	hdr = (struct vmbus_channel_message_header *)msg->u.payload;
	size = msg->header.payload_size;

	if (hdr->msgtype >= CHANNELMSG_COUNT) {
		pr_err("Received invalid channel message type %d size %d\n",
			   hdr->msgtype, size);
		print_hex_dump_bytes("", DUMP_PREFIX_NONE,
				     (unsigned char *)msg->u.payload, size);
		return;
	}

	if (channel_message_table[hdr->msgtype].message_handler)
		channel_message_table[hdr->msgtype].message_handler(hdr);
	else
		pr_err("Unhandled channel message type %d\n", hdr->msgtype);
}

/*
 * vmbus_request_offers - Send a request to get all our pending offers.
 */
int vmbus_request_offers(void)
{
	struct vmbus_channel_message_header *msg;
	struct vmbus_channel_msginfo *msginfo;
	int ret, t;

	msginfo = kmalloc(sizeof(*msginfo) +
			  sizeof(struct vmbus_channel_message_header),
			  GFP_KERNEL);
	if (!msginfo)
		return -ENOMEM;

	init_completion(&msginfo->waitevent);

	msg = (struct vmbus_channel_message_header *)msginfo->msg;

	msg->msgtype = CHANNELMSG_REQUESTOFFERS;


	ret = vmbus_post_msg(msg,
			       sizeof(struct vmbus_channel_message_header));
	if (ret != 0) {
		pr_err("Unable to request offers - %d\n", ret);

		goto cleanup;
	}

	t = wait_for_completion_timeout(&msginfo->waitevent, 5*HZ);
	if (t == 0) {
		ret = -ETIMEDOUT;
		goto cleanup;
	}



cleanup:
	kfree(msginfo);

	return ret;
}

/*
 * Retrieve the (sub) channel on which to send an outgoing request.
 * When a primary channel has multiple sub-channels, we choose a
 * channel whose VCPU binding is closest to the VCPU on which
 * this call is being made.
 */
struct vmbus_channel *vmbus_get_outgoing_channel(struct vmbus_channel *primary)
{
	struct list_head *cur, *tmp;
	int cur_cpu;
	struct vmbus_channel *cur_channel;
	struct vmbus_channel *outgoing_channel = primary;
	int cpu_distance, new_cpu_distance;

	if (list_empty(&primary->sc_list))
		return outgoing_channel;

	cur_cpu = hv_context.vp_index[get_cpu()];
	put_cpu();
	list_for_each_safe(cur, tmp, &primary->sc_list) {
		cur_channel = list_entry(cur, struct vmbus_channel, sc_list);
		if (cur_channel->state != CHANNEL_OPENED_STATE)
			continue;

		if (cur_channel->target_vp == cur_cpu)
			return cur_channel;

		cpu_distance = ((outgoing_channel->target_vp > cur_cpu) ?
				(outgoing_channel->target_vp - cur_cpu) :
				(cur_cpu - outgoing_channel->target_vp));

		new_cpu_distance = ((cur_channel->target_vp > cur_cpu) ?
				(cur_channel->target_vp - cur_cpu) :
				(cur_cpu - cur_channel->target_vp));

		if (cpu_distance < new_cpu_distance)
			continue;

		outgoing_channel = cur_channel;
	}

	return outgoing_channel;
}
EXPORT_SYMBOL_GPL(vmbus_get_outgoing_channel);

static void invoke_sc_cb(struct vmbus_channel *primary_channel)
{
	struct list_head *cur, *tmp;
	struct vmbus_channel *cur_channel;

	if (primary_channel->sc_creation_callback == NULL)
		return;

	list_for_each_safe(cur, tmp, &primary_channel->sc_list) {
		cur_channel = list_entry(cur, struct vmbus_channel, sc_list);

		primary_channel->sc_creation_callback(cur_channel);
	}
}

void vmbus_set_sc_create_callback(struct vmbus_channel *primary_channel,
				void (*sc_cr_cb)(struct vmbus_channel *new_sc))
{
	primary_channel->sc_creation_callback = sc_cr_cb;
}
EXPORT_SYMBOL_GPL(vmbus_set_sc_create_callback);

bool vmbus_are_subchannels_present(struct vmbus_channel *primary)
{
	bool ret;

	ret = !list_empty(&primary->sc_list);

	if (ret) {
		/*
		 * Invoke the callback on sub-channel creation.
		 * This will present a uniform interface to the
		 * clients.
		 */
		invoke_sc_cb(primary);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(vmbus_are_subchannels_present);
