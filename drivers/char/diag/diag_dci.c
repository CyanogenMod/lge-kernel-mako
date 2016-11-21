/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/slab.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/diagchar.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <asm/current.h>
#ifdef CONFIG_DIAG_OVER_USB
#include <mach/usbdiag.h>
#endif
#include "diagchar_hdlc.h"
#include "diagmem.h"
#include "diagchar.h"
#include "diagfwd.h"
#include "diagfwd_cntl.h"
#include "diag_dci.h"

unsigned int dci_max_reg = 100;
unsigned int dci_max_clients = 10;
unsigned char dci_cumulative_log_mask[DCI_LOG_MASK_SIZE];
unsigned char dci_cumulative_event_mask[DCI_EVENT_MASK_SIZE];
struct mutex dci_log_mask_mutex;
struct mutex dci_event_mask_mutex;

#define DCI_CHK_CAPACITY(entry, new_data_len)				\
((entry->data_len + new_data_len > entry->total_capacity) ? 1 : 0)	\

/* Process the data read from the smd dci channel */
int diag_process_smd_dci_read_data(struct diag_smd_info *smd_info, void *buf,
								int recd_bytes)
{
	int read_bytes, dci_pkt_len, i;
	uint8_t recv_pkt_cmd_code;

	/* Each SMD read can have multiple DCI packets */
	read_bytes = 0;
	while (read_bytes < recd_bytes) {
		/* read actual length of dci pkt */
		dci_pkt_len = *(uint16_t *)(buf+2);
		/* process one dci packet */
		pr_debug("diag: bytes read = %d, single dci pkt len = %d\n",
			read_bytes, dci_pkt_len);
		/* print_hex_dump(KERN_DEBUG, "Single DCI packet :",
		 DUMP_PREFIX_ADDRESS, 16, 1, buf, 5 + dci_pkt_len, 1);*/
		recv_pkt_cmd_code = *(uint8_t *)(buf+4);
		if (recv_pkt_cmd_code == LOG_CMD_CODE)
			extract_dci_log(buf+4);
		else if (recv_pkt_cmd_code == EVENT_CMD_CODE)
			extract_dci_events(buf+4);
		else
			extract_dci_pkt_rsp(buf); /* pkt response */
		read_bytes += 5 + dci_pkt_len;
		buf += 5 + dci_pkt_len; /* advance to next DCI pkt */
	}
	/* wake up all sleeping DCI clients which have some data */
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		if (driver->dci_client_tbl[i].client &&
			driver->dci_client_tbl[i].data_len) {
			smd_info->in_busy_1 = 1;
			diag_update_sleeping_process(
				driver->dci_client_tbl[i].client->tgid,
					 DCI_DATA_TYPE);
		}
	}

	return 0;
}

void extract_dci_pkt_rsp(unsigned char *buf)
{
	int i = 0, index = -1, cmd_code_len = 1;
	int curr_client_pid = 0, write_len;
	struct diag_dci_client_tbl *entry;
	void *temp_buf = NULL;
	uint8_t recv_pkt_cmd_code;

	recv_pkt_cmd_code = *(uint8_t *)(buf+4);
	if (recv_pkt_cmd_code != DCI_PKT_RSP_CODE)
		cmd_code_len = 4; /* delayed response */
	write_len = (int)(*(uint16_t *)(buf+2)) - cmd_code_len;
	if (write_len <= 0) {
		pr_err("diag: Invalid length in %s, write_len: %d",
					__func__, write_len);
		return;
	}
	pr_debug("diag: len = %d\n", write_len);
	/* look up DCI client with tag */
	for (i = 0; i < dci_max_reg; i++) {
		if (driver->req_tracking_tbl[i].tag ==
					 *(int *)(buf+(4+cmd_code_len))) {
			*(int *)(buf+4+cmd_code_len) =
					driver->req_tracking_tbl[i].uid;
			curr_client_pid =
					 driver->req_tracking_tbl[i].pid;
			index = i;
			break;
		}
	}
	if (index == -1)
		pr_alert("diag: No matching PID for DCI data\n");
	/* Using PID of client process, find client buffer */
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		if (driver->dci_client_tbl[i].client != NULL) {
			if (curr_client_pid ==
				driver->dci_client_tbl[i].client->tgid) {
				/* copy pkt rsp in client buf */
				entry = &(driver->dci_client_tbl[i]);
				if (DCI_CHK_CAPACITY(entry, 8+write_len)) {
					pr_alert("diag: create capacity for pkt rsp\n");
					entry->total_capacity += 8+write_len;
					temp_buf = krealloc(entry->dci_data,
					entry->total_capacity, GFP_KERNEL);
					if (!temp_buf) {
						pr_err("diag: DCI realloc failed\n");
						break;
					} else {
						entry->dci_data = temp_buf;
					}
				}
				*(int *)(entry->dci_data+entry->data_len) =
							DCI_PKT_RSP_TYPE;
				entry->data_len += 4;
				*(int *)(entry->dci_data+entry->data_len)
								= write_len;
				entry->data_len += 4;
				memcpy(entry->dci_data+entry->data_len,
					buf+4+cmd_code_len, write_len);
				entry->data_len += write_len;
				/* delete immediate response entry */
				if (driver->smd_dci[MODEM_DATA].
					buf_in_1[8+cmd_code_len] != 0x80)
					driver->req_tracking_tbl[index].pid = 0;
				break;
			}
		}
	}
}

void extract_dci_events(unsigned char *buf)
{
	uint16_t event_id, event_id_packet;
	uint8_t *event_mask_ptr, byte_mask, payload_len;
	uint8_t event_data[MAX_EVENT_SIZE], timestamp[8];
	unsigned int byte_index;
	int i, bit_index, length, temp_len;
	int total_event_len, payload_len_field, timestamp_len;
	struct diag_dci_client_tbl *entry;

	length =  *(uint16_t *)(buf+1); /* total length of event series */
	temp_len = 0;
	buf = buf + 3; /* start of event series */
	while (temp_len < length-1) {
		*event_data = EVENT_CMD_CODE;
		event_id_packet = *(uint16_t *)(buf+temp_len);
		event_id = event_id_packet & 0x0FFF; /* extract 12 bits */
		if (event_id_packet & 0x8000) {
			timestamp_len = 2;
		} else {
			timestamp_len = 8;
			memcpy(timestamp, buf+temp_len+2, 8);
		}
		if (((event_id_packet & 0x6000) >> 13) == 3) {
			payload_len_field = 1;
			payload_len = *(uint8_t *)
					(buf+temp_len+2+timestamp_len);
			memcpy(event_data+13, buf+temp_len+2+timestamp_len, 1);
			memcpy(event_data+14, buf+temp_len+2+timestamp_len+1,
								 payload_len);
		} else {
			payload_len_field = 0;
			payload_len = (event_id_packet & 0x6000) >> 13;
			if (payload_len < MAX_EVENT_SIZE)
				memcpy(event_data+13,
				 buf+temp_len+2+timestamp_len, payload_len);
			else
				pr_alert("diag: event > %d\n", MAX_EVENT_SIZE);
		}
		/* 2 bytes for the event id & timestamp len is hard coded to 8,
		   as individual events have full timestamp */
		*(uint16_t *)(event_data+1) = 10+payload_len_field+payload_len;
		*(uint16_t *)(event_data+3) = event_id_packet & 0x7FFF;
		memcpy(event_data+5, timestamp, 8);
		total_event_len = 3 + 10 + payload_len_field + payload_len;
		byte_index = event_id/8;
		bit_index = event_id % 8;
		byte_mask = 0x1 << bit_index;
		/* parse through event mask tbl of each client and check mask */
		for (i = 0; i < MAX_DCI_CLIENTS; i++) {
			if (driver->dci_client_tbl[i].client) {
				entry = &(driver->dci_client_tbl[i]);
				event_mask_ptr = entry->dci_event_mask +
								 byte_index;
				if (*event_mask_ptr & byte_mask) {
					/* copy to client buffer */
					if (DCI_CHK_CAPACITY(entry,
							 4 + total_event_len)) {
						pr_err("diag:DCI event drop\n");
						driver->dci_client_tbl[i].
							dropped_events++;
						return;
					}
					driver->dci_client_tbl[i].
							received_events++;
					*(int *)(entry->dci_data+
					entry->data_len) = DCI_EVENT_TYPE;
					memcpy(entry->dci_data+
				entry->data_len+4, event_data, total_event_len);
					entry->data_len += 4 + total_event_len;
				}
			}
		}
		temp_len += 2 + timestamp_len + payload_len_field + payload_len;
	}
}

void extract_dci_log(unsigned char *buf)
{
	uint16_t log_code, item_num, log_length;
	uint8_t equip_id, *log_mask_ptr, byte_mask;
	unsigned int byte_index;
	int i, found = 0;
	struct diag_dci_client_tbl *entry;

	log_length = *(uint16_t *)(buf + 2);
	log_code = *(uint16_t *)(buf+6);
	equip_id = LOG_GET_EQUIP_ID(log_code);
	item_num = LOG_GET_ITEM_NUM(log_code);
	byte_index = item_num/8 + 2;
	byte_mask = 0x01 << (item_num % 8);

	if (log_length > USHRT_MAX - 4) {
		pr_err("diag: Integer overflow in %s, log_len:%d",
				__func__, log_length);
		return;
	}
	/* parse through log mask table of each client and check mask */
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		if (driver->dci_client_tbl[i].client) {
			entry = &(driver->dci_client_tbl[i]);
			log_mask_ptr = entry->dci_log_mask;
			found = 0;
			while (log_mask_ptr) {
				if (*log_mask_ptr == equip_id) {
					found = 1;
					pr_debug("diag: find equip id = %x at %p\n",
					equip_id, log_mask_ptr);
					break;
				} else {
					pr_debug("diag: did not find equip id = %x at %p\n",
						 equip_id, log_mask_ptr);
					log_mask_ptr += 514;
				}
			}
			if (!found)
				pr_err("diag: dci equip id not found\n");
			log_mask_ptr = log_mask_ptr + byte_index;
			if (*log_mask_ptr & byte_mask) {
				pr_debug("\t log code %x needed by client %d",
					 log_code, entry->client->tgid);
				/* copy to client buffer */
				if (DCI_CHK_CAPACITY(entry,
						 4 + *(uint16_t *)(buf+2))) {
						pr_err("diag:DCI log drop\n");
						driver->dci_client_tbl[i].
								dropped_logs++;
						return;
				}
				driver->dci_client_tbl[i].received_logs++;
				*(int *)(entry->dci_data+entry->data_len) =
								DCI_LOG_TYPE;
				memcpy(entry->dci_data+entry->data_len+4,
                                       buf + 4, log_length);
				entry->data_len += 4 + log_length;
			}
		}
	}
}

void diag_update_smd_dci_work_fn(struct work_struct *work)
{
	struct diag_smd_info *smd_info = container_of(work,
						struct diag_smd_info,
						diag_notify_update_smd_work);
	int i, j;
	char dirty_bits[16];
	uint8_t *client_log_mask_ptr;
	uint8_t *log_mask_ptr;
	int ret;
	int index = smd_info->peripheral;

	/* Update the peripheral(s) with the dci log and event masks */

	/* If the cntl channel is not up, we can't update logs and events */
	if (!driver->smd_cntl[index].ch)
		return;

	memset(dirty_bits, 0, 16 * sizeof(uint8_t));

	/*
	 * From each log entry used by each client, determine
	 * which log entries in the cumulative logs that need
	 * to be updated on the peripheral.
	 */
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		if (driver->dci_client_tbl[i].client) {
			client_log_mask_ptr =
				driver->dci_client_tbl[i].dci_log_mask;
			for (j = 0; j < 16; j++) {
				if (*(client_log_mask_ptr+1))
					dirty_bits[j] = 1;
				client_log_mask_ptr += 514;
			}
		}
	}

	mutex_lock(&dci_log_mask_mutex);
	/* Update the appropriate dirty bits in the cumulative mask */
	log_mask_ptr = dci_cumulative_log_mask;
	for (i = 0; i < 16; i++) {
		if (dirty_bits[i])
			*(log_mask_ptr+1) = dirty_bits[i];

		log_mask_ptr += 514;
	}
	mutex_unlock(&dci_log_mask_mutex);

	ret = diag_send_dci_log_mask(driver->smd_cntl[index].ch);

	ret = diag_send_dci_event_mask(driver->smd_cntl[index].ch);

	smd_info->notify_context = 0;
}

void diag_dci_notify_client(int peripheral_mask, int data)
{
	int i, stat;
	struct siginfo info;
	memset(&info, 0, sizeof(struct siginfo));
	info.si_code = SI_QUEUE;
	info.si_int = (peripheral_mask | data);

	/* Notify the DCI process that the peripheral DCI Channel is up */
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		if (driver->dci_client_tbl[i].list & peripheral_mask) {
			info.si_signo = driver->dci_client_tbl[i].signal_type;
			stat = send_sig_info(
				driver->dci_client_tbl[i].signal_type,
				&info, driver->dci_client_tbl[i].client);
			if (stat)
				pr_err("diag: Err sending dci signal to client, signal data: 0x%x, stat: %d\n",
				info.si_int, stat);
			break;
		}
	} /* end of loop for all DCI clients */
}

static int diag_dci_probe(struct platform_device *pdev)
{
	int err = 0;
	int index;

	if (pdev->id == SMD_APPS_MODEM) {
		index = MODEM_DATA;
		err = smd_open("DIAG_2", &driver->smd_dci[index].ch,
					&driver->smd_dci[index],
					diag_smd_notify);
		driver->smd_dci[index].ch_save =
					driver->smd_dci[index].ch;
		if (err)
			pr_err("diag: In %s, cannot open DCI port, Id = %d, err: %d\n",
				__func__, pdev->id, err);
	}

	return err;
}

int diag_send_dci_pkt(struct diag_master_table entry, unsigned char *buf,
					 int len, int index)
{
	int i, status = 0;
	unsigned int read_len = 0;

	/* The first 4 bytes is the uid tag and the next four bytes is
	   the minmum packet length of a request packet */
	if (len < DCI_PKT_REQ_MIN_LEN) {
		pr_err("diag: dci: Invalid pkt len %d in %s\n", len, __func__);
		return -EIO;
	}
	if (len > APPS_BUF_SIZE - 10) {
		pr_err("diag: dci: Invalid payload length in %s\n", __func__);
		return -EIO;
	}
	/* remove UID from user space pkt before sending to peripheral*/
	buf = buf + sizeof(int);
	read_len += sizeof(int);
	len = len - sizeof(int);
	mutex_lock(&driver->dci_mutex);
	/* prepare DCI packet */
	driver->apps_dci_buf[0] = CONTROL_CHAR; /* start */
	driver->apps_dci_buf[1] = 1; /* version */
	*(uint16_t *)(driver->apps_dci_buf + 2) = len + 4 + 1; /* length */
	driver->apps_dci_buf[4] = DCI_PKT_RSP_CODE;
	*(int *)(driver->apps_dci_buf + 5) =
		driver->req_tracking_tbl[index].tag;
	for (i = 0; i < len; i++)
		driver->apps_dci_buf[i+9] = *(buf+i);
	read_len += len;
	driver->apps_dci_buf[9+len] = CONTROL_CHAR; /* end */
	if ((read_len + 9) >= USER_SPACE_DATA) {
		pr_err("diag: dci: Invalid length while forming dci pkt in %s",
								__func__);
		return -EIO;
	}

	for (i = 0; i < NUM_SMD_DCI_CHANNELS; i++) {
		if (entry.client_id == driver->smd_dci[i].peripheral) {
			if (driver->smd_dci[i].ch) {
				smd_write(driver->smd_dci[i].ch,
					driver->apps_dci_buf, len + 10);
				status = DIAG_DCI_NO_ERROR;
			}
			break;
		}
	}

	if (status != DIAG_DCI_NO_ERROR) {
		pr_alert("diag: check DCI channel\n");
		status = DIAG_DCI_SEND_DATA_FAIL;
	}
	mutex_unlock(&driver->dci_mutex);
	return status;
}

int diag_register_dci_transaction(int uid)
{
	int i, new_dci_client = 1, ret = -1;

	for (i = 0; i < dci_max_reg; i++) {
		if (driver->req_tracking_tbl[i].pid == current->tgid) {
			new_dci_client = 0;
			break;
		}
	}
	mutex_lock(&driver->dci_mutex);
	/* Make an entry in kernel DCI table */
	driver->dci_tag++;
	for (i = 0; i < dci_max_reg; i++) {
		if (driver->req_tracking_tbl[i].pid == 0) {
			driver->req_tracking_tbl[i].pid = current->tgid;
			driver->req_tracking_tbl[i].uid = uid;
			driver->req_tracking_tbl[i].tag = driver->dci_tag;
			ret = i;
			break;
		}
	}
	mutex_unlock(&driver->dci_mutex);
	return ret;
}

int diag_process_dci_transaction(unsigned char *buf, int len)
{
	unsigned char *temp = buf;
	uint16_t subsys_cmd_code, log_code, item_num;
	int subsys_id, cmd_code, ret = -1, index = -1, found = 0;
	struct diag_master_table entry;
	int count, set_mask, num_codes, bit_index, event_id, offset = 0, i;
	unsigned int byte_index, read_len = 0;
	uint8_t equip_id, *log_mask_ptr, *head_log_mask_ptr, byte_mask;
	uint8_t *event_mask_ptr;

	if (!driver->smd_dci[MODEM_DATA].ch) {
		pr_err("diag: DCI smd channel for peripheral %d not valid for dci updates\n",
			driver->smd_dci[MODEM_DATA].peripheral);
		return DIAG_DCI_SEND_DATA_FAIL;
	}

	if (!temp) {
		pr_err("diag: Invalid buffer in %s\n", __func__);
	}

	/* This is Pkt request/response transaction */
	if (*(int *)temp > 0) {
		if (len < DCI_PKT_REQ_MIN_LEN || len > USER_SPACE_DATA) {
			pr_err("diag: dci: Invalid length %d len in %s", len,
								__func__);
			return -EIO;
		}
		/* enter this UID into kernel table and return index */
		index = diag_register_dci_transaction(*(int *)temp);
		if (index < 0) {
			pr_alert("diag: registering new DCI transaction failed\n");
			return DIAG_DCI_NO_REG;
		}
		temp += sizeof(int);
		/*
		 * Check for registered peripheral and fwd pkt to
		 * appropriate proc
		 */
		cmd_code = (int)(*(char *)temp);
		temp++;
		subsys_id = (int)(*(char *)temp);
		temp++;
		subsys_cmd_code = *(uint16_t *)temp;
		temp += sizeof(uint16_t);
		read_len += sizeof(int) + 2 + sizeof(uint16_t);
		if (read_len >= USER_SPACE_DATA) {
			pr_err("diag: dci: Invalid length in %s\n", __func__);
			return -EIO;
		}
		pr_debug("diag: %d %d %d", cmd_code, subsys_id,
			subsys_cmd_code);
		for (i = 0; i < diag_max_reg; i++) {
			entry = driver->table[i];
			if (entry.process_id != NO_PROCESS) {
				if (entry.cmd_code == cmd_code &&
					entry.subsys_id == subsys_id &&
					entry.cmd_code_lo <= subsys_cmd_code &&
					entry.cmd_code_hi >= subsys_cmd_code) {
					ret = diag_send_dci_pkt(entry, buf,
								len, index);
				} else if (entry.cmd_code == 255
					  && cmd_code == 75) {
					if (entry.subsys_id == subsys_id &&
						entry.cmd_code_lo <=
						subsys_cmd_code &&
						entry.cmd_code_hi >=
						subsys_cmd_code) {
						ret = diag_send_dci_pkt(entry,
							buf, len, index);
					}
				} else if (entry.cmd_code == 255 &&
					entry.subsys_id == 255) {
					if (entry.cmd_code_lo <= cmd_code &&
						entry.cmd_code_hi >=
							cmd_code) {
						ret = diag_send_dci_pkt(entry,
							buf, len, index);
					}
				}
			}
		}
	} else if (*(int *)temp == DCI_LOG_TYPE) {
		/* Minimum length of a log mask config is 12 + 2 bytes for
		   atleast one log code to be set or reset */
		if (len < DCI_LOG_CON_MIN_LEN || len > USER_SPACE_DATA) {
			pr_err("diag: dci: Invalid length in %s\n", __func__);
			return -EIO;
		}
		/* find client id and table */
		for (i = 0; i < MAX_DCI_CLIENTS; i++) {
			if (driver->dci_client_tbl[i].client != NULL) {
				if (driver->dci_client_tbl[i].client->tgid ==
							current->tgid) {
					found = 1;
					break;
				}
			}
		}
		if (!found) {
			pr_err("diag: dci client not registered/found\n");
			return ret;
		}
		/* Extract each log code and put in client table */
		temp += sizeof(int);
		read_len += sizeof(int);
		set_mask = *(int *)temp;
		temp += sizeof(int);
		read_len += sizeof(int);
		num_codes = *(int *)temp;
		temp += sizeof(int);
		read_len += sizeof(int);

		if (num_codes == 0 || (num_codes >= (USER_SPACE_DATA - 8)/2)) {
			pr_err("diag: dci: Invalid number of log codes %d\n",
								num_codes);
			return -EIO;
		}

		head_log_mask_ptr = driver->dci_client_tbl[i].dci_log_mask;
		if (!head_log_mask_ptr) {
			pr_err("diag: dci: Invalid Log mask pointer in %s\n",
								__func__);
			return -ENOMEM;
		}
		pr_debug("diag: head of dci log mask %p\n", head_log_mask_ptr);
		count = 0; /* iterator for extracting log codes */
		while (count < num_codes) {
			if (read_len >= USER_SPACE_DATA) {
				pr_err("diag: dci: Invalid length for log type in %s",
								__func__);
				return -EIO;
			}
			log_code = *(uint16_t *)temp;
			equip_id = LOG_GET_EQUIP_ID(log_code);
			item_num = LOG_GET_ITEM_NUM(log_code);
			byte_index = item_num/8 + 2;
			if (byte_index >= (DCI_MAX_ITEMS_PER_LOG_CODE+2)) {
				pr_err("diag: dci: Log type, invalid byte index\n");
				return ret;
			}
			byte_mask = 0x01 << (item_num % 8);
			/*
			 * Parse through log mask table and find
			 * relevant range
			 */
			log_mask_ptr = head_log_mask_ptr;
			found = 0;
			offset = 0;
			while (log_mask_ptr && (offset < DCI_LOG_MASK_SIZE)) {
				if (*log_mask_ptr == equip_id) {
					found = 1;
					pr_debug("diag: find equip id = %x at %p\n",
						 equip_id, log_mask_ptr);
					break;
				} else {
					pr_debug("diag: did not find equip id = %x at %p\n",
						 equip_id, log_mask_ptr);
					log_mask_ptr += 514;
					offset += 514;
				}
			}
			if (!found) {
				pr_err("diag: dci equip id not found\n");
				return ret;
			}
			*(log_mask_ptr+1) = 1; /* set the dirty byte */
			log_mask_ptr = log_mask_ptr + byte_index;
			if (set_mask)
				*log_mask_ptr |= byte_mask;
			else
				*log_mask_ptr &= ~byte_mask;
			/* add to cumulative mask */
			update_dci_cumulative_log_mask(
				offset, byte_index,
				byte_mask);
			temp += 2;
			read_len += 2;
			count++;
			ret = DIAG_DCI_NO_ERROR;
		}
		/* send updated mask to peripherals */
		ret = diag_send_dci_log_mask(driver->smd_cntl[MODEM_DATA].ch);
	} else if (*(int *)temp == DCI_EVENT_TYPE) {
		/* Minimum length of a event mask config is 12 + 4 bytes for
		  atleast one event id to be set or reset. */
		if (len < DCI_EVENT_CON_MIN_LEN || len > USER_SPACE_DATA) {
			pr_err("diag: dci: Invalid length in %s\n", __func__);
			return -EIO;
		}
		/* find client id and table */
		for (i = 0; i < MAX_DCI_CLIENTS; i++) {
			if (driver->dci_client_tbl[i].client != NULL) {
				if (driver->dci_client_tbl[i].client->tgid ==
							current->tgid) {
					found = 1;
					break;
				}
			}
		}
		if (!found) {
			pr_err("diag: dci client not registered/found\n");
			return ret;
		}
		/* Extract each log code and put in client table */
		temp += sizeof(int);
		read_len += sizeof(int);
		set_mask = *(int *)temp;
		temp += sizeof(int);
		read_len += sizeof(int);
		num_codes = *(int *)temp;
		temp += sizeof(int);
		read_len += sizeof(int);

		/* Check for positive number of event ids. Also, the number of
		   event ids should fit in the buffer along with set_mask and
		   num_codes which are 4 bytes each */
		if (num_codes == 0 || (num_codes >= (USER_SPACE_DATA - 8)/2)) {
			pr_err("diag: dci: Invalid number of event ids %d\n",
								num_codes);
			return -EIO;
		}

		event_mask_ptr = driver->dci_client_tbl[i].dci_event_mask;
		if (!event_mask_ptr) {
			pr_err("diag: dci: Invalid event mask pointer in %s\n",
								__func__);
			return -ENOMEM;
		}
		pr_debug("diag: head of dci event mask %p\n", event_mask_ptr);
		count = 0; /* iterator for extracting log codes */
		while (count < num_codes) {
			if (read_len >= USER_SPACE_DATA) {
				pr_err("diag: dci: Invalid length for event type in %s",
								__func__);
				return -EIO;
			}
			event_id = *(int *)temp;
			byte_index = event_id/8;
			if (byte_index >= DCI_EVENT_MASK_SIZE) {
				pr_err("diag: dci: Event type, invalid byte index\n");
				return ret;
			}
			bit_index = event_id % 8;
			byte_mask = 0x1 << bit_index;
			/*
			 * Parse through event mask table and set
			 * relevant byte & bit combination
			 */
			if (set_mask)
				*(event_mask_ptr + byte_index) |= byte_mask;
			else
				*(event_mask_ptr + byte_index) &= ~byte_mask;
			/* add to cumulative mask */
			update_dci_cumulative_event_mask(byte_index, byte_mask);
			temp += sizeof(int);
			read_len += sizeof(int);
			count++;
			ret = DIAG_DCI_NO_ERROR;
		}
		/* send updated mask to peripherals */
		ret = diag_send_dci_event_mask(driver->smd_cntl[MODEM_DATA].ch);
	} else {
		pr_alert("diag: Incorrect DCI transaction\n");
	}
	return ret;
}

void update_dci_cumulative_event_mask(int offset, uint8_t byte_mask)
{
	int i;
	uint8_t *event_mask_ptr;
	uint8_t *update_ptr = dci_cumulative_event_mask;
	bool is_set = false;

	mutex_lock(&dci_event_mask_mutex);
	update_ptr += offset;
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		event_mask_ptr =
			driver->dci_client_tbl[i].dci_event_mask;
		event_mask_ptr += offset;
		if ((*event_mask_ptr & byte_mask) == byte_mask) {
			is_set = true;
			/* break even if one client has the event mask set */
			break;
		}
	}
	if (is_set == false)
		*update_ptr &= ~byte_mask;
	else
		*update_ptr |= byte_mask;
	mutex_unlock(&dci_event_mask_mutex);
}

int diag_send_dci_event_mask(smd_channel_t *ch)
{
	void *buf = driver->buf_event_mask_update;
	int header_size = sizeof(struct diag_ctrl_event_mask);
	int wr_size = -ENOMEM, retry_count = 0, timer;
	int ret = DIAG_DCI_NO_ERROR;

	mutex_lock(&driver->diag_cntl_mutex);
	/* send event mask update */
	driver->event_mask->cmd_type = DIAG_CTRL_MSG_EVENT_MASK;
	driver->event_mask->data_len = 7 + DCI_EVENT_MASK_SIZE;
	driver->event_mask->stream_id = DCI_MASK_STREAM;
	driver->event_mask->status = 3; /* status for valid mask */
	driver->event_mask->event_config = 1; /* event config */
	driver->event_mask->event_mask_size = DCI_EVENT_MASK_SIZE;
	memcpy(buf, driver->event_mask, header_size);
	memcpy(buf+header_size, dci_cumulative_event_mask, DCI_EVENT_MASK_SIZE);
	if (ch) {
		while (retry_count < 3) {
			wr_size = smd_write(ch, buf,
					 header_size + DCI_EVENT_MASK_SIZE);
			if (wr_size == -ENOMEM) {
				retry_count++;
				for (timer = 0; timer < 5; timer++)
					udelay(2000);
			} else {
				break;
			}
		}
		if (wr_size != header_size + DCI_EVENT_MASK_SIZE) {
			pr_err("diag: error writing dci event mask %d, tried %d\n",
				 wr_size, header_size + DCI_EVENT_MASK_SIZE);
			ret = DIAG_DCI_SEND_DATA_FAIL;
		}
	} else {
		pr_err("diag: ch not valid for dci event mask update\n");
		ret = DIAG_DCI_SEND_DATA_FAIL;
	}
	mutex_unlock(&driver->diag_cntl_mutex);

	return ret;
}

void update_dci_cumulative_log_mask(int offset, unsigned int byte_index,
						uint8_t byte_mask)
{
	int i;
	uint8_t *update_ptr = dci_cumulative_log_mask;
	uint8_t *log_mask_ptr;
	bool is_set = false;

	mutex_lock(&dci_log_mask_mutex);
	*update_ptr = 0;
	/* set the equipment IDs */
	for (i = 0; i < 16; i++)
		*(update_ptr + (i*514)) = i;

	update_ptr += offset;
	/* update the dirty bit */
	*(update_ptr+1) = 1;
	update_ptr = update_ptr + byte_index;
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		log_mask_ptr =
			(driver->dci_client_tbl[i].dci_log_mask);
		log_mask_ptr = log_mask_ptr + offset + byte_index;
		if ((*log_mask_ptr & byte_mask) == byte_mask) {
			is_set = true;
			/* break even if one client has the log mask set */
			break;
		}
	}

	if (is_set == false)
		*update_ptr &= ~byte_mask;
	else
		*update_ptr |= byte_mask;
	mutex_unlock(&dci_log_mask_mutex);
}

int diag_send_dci_log_mask(smd_channel_t *ch)
{
	void *buf = driver->buf_log_mask_update;
	int header_size = sizeof(struct diag_ctrl_log_mask);
	uint8_t *log_mask_ptr = dci_cumulative_log_mask;
	int i, wr_size = -ENOMEM, retry_count = 0, timer;
	int ret = DIAG_DCI_NO_ERROR;

	if (!ch) {
		pr_err("diag: ch not valid for dci log mask update\n");
		return DIAG_DCI_SEND_DATA_FAIL;
	}

	mutex_lock(&driver->diag_cntl_mutex);
	for (i = 0; i < 16; i++) {
		driver->log_mask->cmd_type = DIAG_CTRL_MSG_LOG_MASK;
		driver->log_mask->num_items = 512;
		driver->log_mask->data_len  = 11 + 512;
		driver->log_mask->stream_id = DCI_MASK_STREAM;
		driver->log_mask->status = 3; /* status for valid mask */
		driver->log_mask->equip_id = *log_mask_ptr;
		driver->log_mask->log_mask_size = 512;
		memcpy(buf, driver->log_mask, header_size);
		memcpy(buf+header_size, log_mask_ptr+2, 512);
		/* if dirty byte is set and channel is valid */
		if (ch && *(log_mask_ptr+1)) {
			while (retry_count < 3) {
				wr_size = smd_write(ch, buf, header_size + 512);
				if (wr_size == -ENOMEM) {
					retry_count++;
					for (timer = 0; timer < 5; timer++)
						udelay(2000);
				} else
					break;
			}
			if (wr_size != header_size + 512) {
				pr_err("diag: dci log mask update failed %d, tried %d",
					 wr_size, header_size + 512);
				ret = DIAG_DCI_SEND_DATA_FAIL;

			} else {
				*(log_mask_ptr+1) = 0; /* clear dirty byte */
				pr_debug("diag: updated dci log equip ID %d\n",
						 *log_mask_ptr);
			}
		}
		log_mask_ptr += 514;
	}
	mutex_unlock(&driver->diag_cntl_mutex);

	return ret;
}

void create_dci_log_mask_tbl(unsigned char *tbl_buf)
{
	uint8_t i; int count = 0;

	/* create hard coded table for log mask with 16 categories */
	for (i = 0; i < 16; i++) {
		*(uint8_t *)tbl_buf = i;
		pr_debug("diag: put value %x at %p\n", i, tbl_buf);
		memset(tbl_buf+1, 0, 513); /* set dirty bit as 0 */
		tbl_buf += 514;
		count += 514;
	}
}

void create_dci_event_mask_tbl(unsigned char *tbl_buf)
{
	memset(tbl_buf, 0, 512);
}

static int diag_dci_runtime_suspend(struct device *dev)
{
	dev_dbg(dev, "pm_runtime: suspending...\n");
	return 0;
}

static int diag_dci_runtime_resume(struct device *dev)
{
	dev_dbg(dev, "pm_runtime: resuming...\n");
	return 0;
}

static const struct dev_pm_ops diag_dci_dev_pm_ops = {
	.runtime_suspend = diag_dci_runtime_suspend,
	.runtime_resume = diag_dci_runtime_resume,
};

struct platform_driver msm_diag_dci_driver = {
	.probe = diag_dci_probe,
	.driver = {
			.name = "DIAG_2",
			.owner = THIS_MODULE,
			.pm   = &diag_dci_dev_pm_ops,
	},
};

int diag_dci_init(void)
{
	int success = 0;
	int i;

	driver->dci_tag = 0;
	driver->dci_client_id = 0;
	driver->num_dci_client = 0;
	mutex_init(&driver->dci_mutex);
	mutex_init(&dci_log_mask_mutex);
	mutex_init(&dci_event_mask_mutex);
	success = diag_smd_constructor(&driver->smd_dci[MODEM_DATA],
					MODEM_DATA, SMD_DCI_TYPE);
	if (!success)
		goto err;

	if (driver->req_tracking_tbl == NULL) {
		driver->req_tracking_tbl = kzalloc(dci_max_reg *
			sizeof(struct dci_pkt_req_tracking_tbl), GFP_KERNEL);
		if (driver->req_tracking_tbl == NULL)
			goto err;
	}
	if (driver->apps_dci_buf == NULL) {
		driver->apps_dci_buf = kzalloc(APPS_BUF_SIZE, GFP_KERNEL);
		if (driver->apps_dci_buf == NULL)
			goto err;
	}
	if (driver->dci_client_tbl == NULL) {
		driver->dci_client_tbl = kzalloc(MAX_DCI_CLIENTS *
			sizeof(struct diag_dci_client_tbl), GFP_KERNEL);
		if (driver->dci_client_tbl == NULL)
			goto err;
	}
	driver->diag_dci_wq = create_singlethread_workqueue("diag_dci_wq");
	success = platform_driver_register(&msm_diag_dci_driver);
	if (success) {
		pr_err("diag: Could not register DCI driver\n");
		goto err;
	}
	return DIAG_DCI_NO_ERROR;
err:
	pr_err("diag: Could not initialize diag DCI buffers");
	kfree(driver->req_tracking_tbl);
	kfree(driver->dci_client_tbl);
	kfree(driver->apps_dci_buf);
	for (i = 0; i < NUM_SMD_DCI_CHANNELS; i++)
		diag_smd_destructor(&driver->smd_dci[i]);
	if (driver->diag_dci_wq)
		destroy_workqueue(driver->diag_dci_wq);
	return DIAG_DCI_NO_REG;
}

void diag_dci_exit(void)
{
	int i;

	for (i = 0; i < NUM_SMD_DCI_CHANNELS; i++)
		diag_smd_destructor(&driver->smd_dci[i]);

	platform_driver_unregister(&msm_diag_dci_driver);
	kfree(driver->req_tracking_tbl);
	kfree(driver->dci_client_tbl);
	kfree(driver->apps_dci_buf);
	destroy_workqueue(driver->diag_dci_wq);
}
