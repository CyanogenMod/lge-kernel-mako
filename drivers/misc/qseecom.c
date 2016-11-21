

/*Qualcomm Secure Execution Environment Communicator (QSEECOM) driver
 *
 * Copyright (c) 2012, The Linux Foundation. All rights reserved.
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

#define pr_fmt(fmt) "QSEECOM: %s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/msm_ion.h>
#include <linux/types.h>
#include <linux/clk.h>
#include <linux/qseecom.h>
#include <linux/elf.h>
#include <linux/firmware.h>
#include <linux/freezer.h>
#include <mach/board.h>
#include <mach/msm_bus.h>
#include <mach/msm_bus_board.h>
#include <mach/scm.h>
#include <mach/peripheral-loader.h>
#include <mach/socinfo.h>
#include "qseecom_legacy.h"
#include "qseecom_kernel.h"

#define QSEECOM_DEV			"qseecom"
#define QSEOS_VERSION_13		0x13
#define QSEOS_VERSION_14		0x14
#define QSEOS_CHECK_VERSION_CMD		0x00001803;

enum qseecom_command_scm_resp_type {
	QSEOS_APP_ID = 0xEE01,
	QSEOS_LISTENER_ID
};

enum qseecom_qceos_cmd_id {
	QSEOS_APP_START_COMMAND      = 0x01,
	QSEOS_APP_SHUTDOWN_COMMAND,
	QSEOS_APP_LOOKUP_COMMAND,
	QSEOS_REGISTER_LISTENER,
	QSEOS_DEREGISTER_LISTENER,
	QSEOS_CLIENT_SEND_DATA_COMMAND,
	QSEOS_LISTENER_DATA_RSP_COMMAND,
	QSEOS_LOAD_EXTERNAL_ELF_COMMAND,
	QSEOS_UNLOAD_EXTERNAL_ELF_COMMAND,
	QSEOS_CMD_MAX     = 0xEFFFFFFF
};

enum qseecom_qceos_cmd_status {
	QSEOS_RESULT_SUCCESS = 0,
	QSEOS_RESULT_INCOMPLETE,
	QSEOS_RESULT_FAILURE  = 0xFFFFFFFF
};

enum qseecom_clk_definitions {
	CLK_DFAB = 0,
	CLK_SFPB,
};

enum qseecom_client_handle_type {
	QSEECOM_CLIENT_APP = 1,
	QSEECOM_LISTENER_SERVICE,
	QSEECOM_SECURE_SERVICE,
	QSEECOM_GENERIC,
	QSEECOM_UNAVAILABLE_CLIENT_APP,
};

__packed struct qseecom_check_app_ireq {
	uint32_t qsee_cmd_id;
	char     app_name[MAX_APP_NAME_SIZE];
};

__packed struct qseecom_load_app_ireq {
	uint32_t qsee_cmd_id;
	uint32_t mdt_len;		/* Length of the mdt file */
	uint32_t img_len;		/* Length of .bxx and .mdt files */
	uint32_t phy_addr;		/* phy addr of the start of image */
	char     app_name[MAX_APP_NAME_SIZE];	/* application name*/
};

__packed struct qseecom_unload_app_ireq {
	uint32_t qsee_cmd_id;
	uint32_t  app_id;
};

__packed struct qseecom_register_listener_ireq {
	uint32_t qsee_cmd_id;
	uint32_t listener_id;
	void *sb_ptr;
	uint32_t sb_len;
};

__packed struct qseecom_unregister_listener_ireq {
	uint32_t qsee_cmd_id;
	uint32_t  listener_id;
};

__packed struct qseecom_client_send_data_ireq {
	uint32_t qsee_cmd_id;
	uint32_t app_id;
	void *req_ptr;
	uint32_t req_len;
	void *rsp_ptr;   /* First 4 bytes should always be the return status */
	uint32_t rsp_len;
};

/* send_data resp */
__packed struct qseecom_client_listener_data_irsp {
	uint32_t qsee_cmd_id;
	uint32_t listener_id;
};

/*
 * struct qseecom_command_scm_resp - qseecom response buffer
 * @cmd_status: value from enum tz_sched_cmd_status
 * @sb_in_rsp_addr: points to physical location of response
 *                buffer
 * @sb_in_rsp_len: length of command response
 */
__packed struct qseecom_command_scm_resp {
	uint32_t result;
	enum qseecom_command_scm_resp_type resp_type;
	unsigned int data;
};

static struct class *driver_class;
static dev_t qseecom_device_no;
static struct cdev qseecom_cdev;

/* Data structures used in legacy support */
static void *pil;
static uint32_t pil_ref_cnt;
static DEFINE_MUTEX(pil_access_lock);

static DEFINE_MUTEX(qsee_bw_mutex);
static DEFINE_MUTEX(app_access_lock);

static int qsee_bw_count;
static int qsee_sfpb_bw_count;
static uint32_t qsee_perf_client;

struct qseecom_registered_listener_list {
	struct list_head                 list;
	struct qseecom_register_listener_req svc;
	u8  *sb_reg_req;
	uint32_t user_virt_sb_base;
	u8 *sb_virt;
	s32 sb_phys;
	size_t sb_length;
	struct ion_handle *ihandle; /* Retrieve phy addr */

	wait_queue_head_t          rcv_req_wq;
	int                        rcv_req_flag;
};

struct qseecom_registered_app_list {
	struct list_head                 list;
	u32  app_id;
	u32  ref_cnt;
};

struct qseecom_registered_kclient_list {
	struct list_head list;
	struct qseecom_handle *handle;
};

struct qseecom_control {
	struct ion_client *ion_clnt;		/* Ion client */
	struct list_head  registered_listener_list_head;
	spinlock_t        registered_listener_list_lock;

	struct list_head  registered_app_list_head;
	spinlock_t        registered_app_list_lock;

	struct list_head   registered_kclient_list_head;
	spinlock_t        registered_kclient_list_lock;

	wait_queue_head_t send_resp_wq;
	int               send_resp_flag;

	uint32_t          qseos_version;
	struct device *pdev;
};

struct qseecom_client_handle {
	u32  app_id;
	u8 *sb_virt;
	s32 sb_phys;
	uint32_t user_virt_sb_base;
	size_t sb_length;
	struct ion_handle *ihandle;		/* Retrieve phy addr */
};

struct qseecom_listener_handle {
	u32               id;
};

static struct qseecom_control qseecom;

struct qseecom_dev_handle {
	enum qseecom_client_handle_type type;
	bool               service;
	union {
		struct qseecom_client_handle client;
		struct qseecom_listener_handle listener;
	};
	bool released;
	int               abort;
	wait_queue_head_t abort_wq;
	atomic_t          ioctl_count;
};

struct clk *ce_core_clk;
struct clk *ce_clk;
struct clk *ce_core_src_clk;
struct clk *ce_bus_clk;

/* Function proto types */
static int qsee_vote_for_clock(int32_t);
static void qsee_disable_clock_vote(int32_t);
static int __qseecom_init_clk(void);
static void __qseecom_disable_clk(void);

static int __qseecom_is_svc_unique(struct qseecom_dev_handle *data,
		struct qseecom_register_listener_req *svc)
{
	struct qseecom_registered_listener_list *ptr;
	int unique = 1;
	unsigned long flags;

	spin_lock_irqsave(&qseecom.registered_listener_list_lock, flags);
	list_for_each_entry(ptr, &qseecom.registered_listener_list_head, list) {
		if (ptr->svc.listener_id == svc->listener_id) {
			pr_err("Service id: %u is already registered\n",
					ptr->svc.listener_id);
			unique = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&qseecom.registered_listener_list_lock, flags);
	return unique;
}

static struct qseecom_registered_listener_list *__qseecom_find_svc(
						int32_t listener_id)
{
	struct qseecom_registered_listener_list *entry = NULL;
	unsigned long flags;

	spin_lock_irqsave(&qseecom.registered_listener_list_lock, flags);
	list_for_each_entry(entry, &qseecom.registered_listener_list_head, list)
	{
		if (entry->svc.listener_id == listener_id)
			break;
	}
	spin_unlock_irqrestore(&qseecom.registered_listener_list_lock, flags);
	return entry;
}

static int __qseecom_set_sb_memory(struct qseecom_registered_listener_list *svc,
				struct qseecom_dev_handle *handle,
				struct qseecom_register_listener_req *listener)
{
	int ret = 0;
	struct qseecom_register_listener_ireq req;
	struct qseecom_command_scm_resp resp;
	ion_phys_addr_t pa;

	/* Get the handle of the shared fd */
	svc->ihandle = ion_import_dma_buf(qseecom.ion_clnt,
					listener->ifd_data_fd);
	if (svc->ihandle == NULL) {
		pr_err("Ion client could not retrieve the handle\n");
		return -ENOMEM;
	}

	/* Get the physical address of the ION BUF */
	ret = ion_phys(qseecom.ion_clnt, svc->ihandle, &pa, &svc->sb_length);

	/* Populate the structure for sending scm call to load image */
	svc->sb_virt = (char *) ion_map_kernel(qseecom.ion_clnt, svc->ihandle);
	svc->sb_phys = pa;

	if (qseecom.qseos_version == QSEOS_VERSION_14) {
		req.qsee_cmd_id = QSEOS_REGISTER_LISTENER;
		req.listener_id = svc->svc.listener_id;
		req.sb_len = svc->sb_length;
		req.sb_ptr = (void *)svc->sb_phys;

		resp.result = QSEOS_RESULT_INCOMPLETE;

		ret = scm_call(SCM_SVC_TZSCHEDULER, 1,  &req,
					sizeof(req), &resp, sizeof(resp));
		if (ret) {
			pr_err("qseecom_scm_call failed with err: %d\n", ret);
			return -EINVAL;
		}

		if (resp.result != QSEOS_RESULT_SUCCESS) {
			pr_err("Error SB registration req: resp.result = %d\n",
					resp.result);
			return -EPERM;
		}
	} else {
		struct qseecom_command cmd;
		struct qseecom_response resp;
		struct qse_pr_init_sb_req_s sb_init_req;
		struct qse_pr_init_sb_rsp_s sb_init_rsp;

		svc->sb_reg_req = kzalloc((sizeof(sb_init_req) +
					sizeof(sb_init_rsp)), GFP_KERNEL);

		sb_init_req.pr_cmd = TZ_SCHED_CMD_ID_REGISTER_LISTENER;
		sb_init_req.listener_id = svc->svc.listener_id;
		sb_init_req.sb_len = svc->sb_length;
		sb_init_req.sb_ptr = svc->sb_phys;

		memcpy(svc->sb_reg_req, &sb_init_req, sizeof(sb_init_req));

		/* It will always be a new cmd from this method */
		cmd.cmd_type = TZ_SCHED_CMD_NEW;
		cmd.sb_in_cmd_addr = (u8 *)(virt_to_phys(svc->sb_reg_req));
		cmd.sb_in_cmd_len = sizeof(sb_init_req);

		resp.cmd_status = TZ_SCHED_STATUS_INCOMPLETE;

		ret = scm_call(SCM_SVC_TZSCHEDULER, 1, &cmd, sizeof(cmd)
				, &resp, sizeof(resp));

		if (ret) {
			pr_err("qseecom_scm_call failed with err: %d\n", ret);
			return -EINVAL;
		}

		if (resp.cmd_status != TZ_SCHED_STATUS_COMPLETE) {
			pr_err("SB registration fail resp.cmd_status %d\n",
							resp.cmd_status);
			return -EINVAL;
		}
		memset(svc->sb_virt, 0, svc->sb_length);
	}
	return 0;
}

static int qseecom_register_listener(struct qseecom_dev_handle *data,
					void __user *argp)
{
	int ret = 0;
	unsigned long flags;
	struct qseecom_register_listener_req rcvd_lstnr;
	struct qseecom_registered_listener_list *new_entry;

	ret = copy_from_user(&rcvd_lstnr, argp, sizeof(rcvd_lstnr));
	if (ret) {
		pr_err("copy_from_user failed\n");
		return ret;
	}
	if (!access_ok(VERIFY_WRITE, (void __user *)rcvd_lstnr.virt_sb_base,
			rcvd_lstnr.sb_size))
		return -EFAULT;

	data->listener.id = 0;
	data->service = true;
	if (!__qseecom_is_svc_unique(data, &rcvd_lstnr)) {
		pr_err("Service is not unique and is already registered\n");
		data->released = true;
		return -EBUSY;
	}

	new_entry = kmalloc(sizeof(*new_entry), GFP_KERNEL);
	if (!new_entry) {
		pr_err("kmalloc failed\n");
		return -ENOMEM;
	}
	memcpy(&new_entry->svc, &rcvd_lstnr, sizeof(rcvd_lstnr));
	new_entry->rcv_req_flag = 0;

	new_entry->svc.listener_id = rcvd_lstnr.listener_id;
	new_entry->sb_length = rcvd_lstnr.sb_size;
	new_entry->user_virt_sb_base = rcvd_lstnr.virt_sb_base;
	if (__qseecom_set_sb_memory(new_entry, data, &rcvd_lstnr)) {
		pr_err("qseecom_set_sb_memoryfailed\n");
		kzfree(new_entry);
		return -ENOMEM;
	}

	data->listener.id = rcvd_lstnr.listener_id;
	init_waitqueue_head(&new_entry->rcv_req_wq);

	spin_lock_irqsave(&qseecom.registered_listener_list_lock, flags);
	list_add_tail(&new_entry->list, &qseecom.registered_listener_list_head);
	spin_unlock_irqrestore(&qseecom.registered_listener_list_lock, flags);

	return ret;
}

static int qseecom_unregister_listener(struct qseecom_dev_handle *data)
{
	int ret = 0;
	unsigned long flags;
	uint32_t unmap_mem = 0;
	struct qseecom_register_listener_ireq req;
	struct qseecom_registered_listener_list *ptr_svc = NULL;
	struct qseecom_command_scm_resp resp;
	struct ion_handle *ihandle = NULL;		/* Retrieve phy addr */

	if (qseecom.qseos_version == QSEOS_VERSION_14) {
		req.qsee_cmd_id = QSEOS_DEREGISTER_LISTENER;
		req.listener_id = data->listener.id;
		resp.result = QSEOS_RESULT_INCOMPLETE;

		ret = scm_call(SCM_SVC_TZSCHEDULER, 1,  &req,
					sizeof(req), &resp, sizeof(resp));
		if (ret) {
			pr_err("qseecom_scm_call failed with err: %d\n", ret);
			return ret;
		}

		if (resp.result != QSEOS_RESULT_SUCCESS) {
			pr_err("SB deregistartion: result=%d\n", resp.result);
			return -EPERM;
		}
	} else {
		struct qse_pr_init_sb_req_s sb_init_req;
		struct qseecom_command cmd;
		struct qseecom_response resp;
		struct qseecom_registered_listener_list *svc;

		svc = __qseecom_find_svc(data->listener.id);
		sb_init_req.pr_cmd = TZ_SCHED_CMD_ID_REGISTER_LISTENER;
		sb_init_req.listener_id = data->listener.id;
		sb_init_req.sb_len = 0;
		sb_init_req.sb_ptr = 0;

		memcpy(svc->sb_reg_req, &sb_init_req, sizeof(sb_init_req));

		/* It will always be a new cmd from this method */
		cmd.cmd_type = TZ_SCHED_CMD_NEW;
		cmd.sb_in_cmd_addr = (u8 *)(virt_to_phys(svc->sb_reg_req));
		cmd.sb_in_cmd_len = sizeof(sb_init_req);
		resp.cmd_status = TZ_SCHED_STATUS_INCOMPLETE;

		ret = scm_call(SCM_SVC_TZSCHEDULER, 1, &cmd, sizeof(cmd),
					&resp, sizeof(resp));
		if (ret) {
			pr_err("qseecom_scm_call failed with err: %d\n", ret);
			return ret;
		}
		kzfree(svc->sb_reg_req);
		if (resp.cmd_status != TZ_SCHED_STATUS_COMPLETE) {
			pr_err("Error with SB initialization\n");
			return -EPERM;
		}
	}
	data->abort = 1;
	spin_lock_irqsave(&qseecom.registered_listener_list_lock, flags);
	list_for_each_entry(ptr_svc, &qseecom.registered_listener_list_head,
			list) {
		if (ptr_svc->svc.listener_id == data->listener.id) {
			wake_up_all(&ptr_svc->rcv_req_wq);
			break;
		}
	}
	spin_unlock_irqrestore(&qseecom.registered_listener_list_lock, flags);

	while (atomic_read(&data->ioctl_count) > 1) {
		if (wait_event_freezable(data->abort_wq,
				atomic_read(&data->ioctl_count) <= 1)) {
			pr_err("Interrupted from abort\n");
			ret = -ERESTARTSYS;
			break;
		}
	}

	spin_lock_irqsave(&qseecom.registered_listener_list_lock, flags);
	list_for_each_entry(ptr_svc,
			&qseecom.registered_listener_list_head,
			list)
	{
		if (ptr_svc->svc.listener_id == data->listener.id) {
			if (ptr_svc->sb_virt) {
				unmap_mem = 1;
				ihandle = ptr_svc->ihandle;
				}
			list_del(&ptr_svc->list);
			kzfree(ptr_svc);
			break;
		}
	}
	spin_unlock_irqrestore(&qseecom.registered_listener_list_lock, flags);

	/* Unmap the memory */
	if (unmap_mem) {
		if (!IS_ERR_OR_NULL(ihandle)) {
			ion_unmap_kernel(qseecom.ion_clnt, ihandle);
			ion_free(qseecom.ion_clnt, ihandle);
			}
	}
	data->released = true;
	return ret;
}

static int qseecom_set_client_mem_param(struct qseecom_dev_handle *data,
						void __user *argp)
{
	ion_phys_addr_t pa;
	int32_t ret;
	struct qseecom_set_sb_mem_param_req req;
	uint32_t len;

	/* Copy the relevant information needed for loading the image */
	if (copy_from_user(&req, (void __user *)argp, sizeof(req)))
		return -EFAULT;
	if (!access_ok(VERIFY_WRITE, (void __user *)req.virt_sb_base,
			req.sb_len))
		return -EFAULT;

	if ((req.ifd_data_fd <= 0) || (req.virt_sb_base == 0) ||
					(req.sb_len == 0)) {
		pr_err("Inavlid input(s)ion_fd(%d), sb_len(%d), vaddr(0x%x)\n",
			req.ifd_data_fd, req.sb_len, req.virt_sb_base);
		return -EFAULT;
	}
	/* Get the handle of the shared fd */
	data->client.ihandle = ion_import_dma_buf(qseecom.ion_clnt,
						req.ifd_data_fd);
	if (IS_ERR_OR_NULL(data->client.ihandle)) {
		pr_err("Ion client could not retrieve the handle\n");
		return -ENOMEM;
	}
	/* Get the physical address of the ION BUF */
	ret = ion_phys(qseecom.ion_clnt, data->client.ihandle, &pa, &len);
	/* Populate the structure for sending scm call to load image */
	data->client.sb_virt = (char *) ion_map_kernel(qseecom.ion_clnt,
							data->client.ihandle);
	data->client.sb_phys = pa;
	data->client.sb_length = req.sb_len;
	data->client.user_virt_sb_base = req.virt_sb_base;
	return 0;
}

static int __qseecom_listener_has_sent_rsp(struct qseecom_dev_handle *data)
{
	int ret;
	ret = (qseecom.send_resp_flag != 0);
	return ret || data->abort;
}

static int __qseecom_process_incomplete_cmd(struct qseecom_dev_handle *data,
					struct qseecom_command_scm_resp *resp)
{
	int ret = 0;
	uint32_t lstnr;
	unsigned long flags;
	struct qseecom_client_listener_data_irsp send_data_rsp;
	struct qseecom_registered_listener_list *ptr_svc = NULL;
	sigset_t new_sigset;
	sigset_t old_sigset;


	while (resp->result == QSEOS_RESULT_INCOMPLETE) {
		lstnr = resp->data;
		/*
		 * Wake up blocking lsitener service with the lstnr id
		 */
		spin_lock_irqsave(&qseecom.registered_listener_list_lock,
					flags);
		list_for_each_entry(ptr_svc,
				&qseecom.registered_listener_list_head, list) {
			if (ptr_svc->svc.listener_id == lstnr) {
				ptr_svc->rcv_req_flag = 1;
				wake_up_interruptible(&ptr_svc->rcv_req_wq);
				break;
			}
		}
		spin_unlock_irqrestore(&qseecom.registered_listener_list_lock,
				flags);
		if (ptr_svc->svc.listener_id != lstnr) {
			pr_warning("Service requested for does on exist\n");
			return -ERESTARTSYS;
		}
		pr_debug("waking up rcv_req_wq and "
				"waiting for send_resp_wq\n");

		/* initialize the new signal mask with all signals*/
		sigfillset(&new_sigset);
		/* block all signals */
		sigprocmask(SIG_SETMASK, &new_sigset, &old_sigset);

		do {
			if (!wait_event_freezable(qseecom.send_resp_wq,
				__qseecom_listener_has_sent_rsp(data)))
				break;
		} while (1);

		/* restore signal mask */
		sigprocmask(SIG_SETMASK, &old_sigset, NULL);
		if (data->abort) {
			pr_err("Abort clnt %d waiting on lstnr svc %d, ret %d",
				data->client.app_id, lstnr, ret);
			return -ENODEV;

		}


		qseecom.send_resp_flag = 0;
		send_data_rsp.qsee_cmd_id = QSEOS_LISTENER_DATA_RSP_COMMAND;
		send_data_rsp.listener_id  = lstnr ;

		ret = scm_call(SCM_SVC_TZSCHEDULER, 1,
					(const void *)&send_data_rsp,
					sizeof(send_data_rsp), resp,
					sizeof(*resp));
		if (ret) {
			pr_err("qseecom_scm_call failed with err: %d\n", ret);
			return ret;
		}
		if (resp->result == QSEOS_RESULT_FAILURE) {
			pr_err("Response result %d not supported\n",
							resp->result);
			return -EINVAL;
		}
	}
	return ret;
}

static int __qseecom_check_app_exists(struct qseecom_check_app_ireq req)
{
	int32_t ret;
	struct qseecom_command_scm_resp resp;

	/*  SCM_CALL  to check if app_id for the mentioned app exists */
	ret = scm_call(SCM_SVC_TZSCHEDULER, 1,  &req,
				sizeof(struct qseecom_check_app_ireq),
				&resp, sizeof(resp));
	if (ret) {
		pr_err("scm_call to check if app is already loaded failed\n");
		return -EINVAL;
	}

	if (resp.result == QSEOS_RESULT_FAILURE) {
			return 0;
	} else {
		switch (resp.resp_type) {
		/*qsee returned listener type response */
		case QSEOS_LISTENER_ID:
			pr_err("resp type is of listener type instead of app");
			return -EINVAL;
			break;
		case QSEOS_APP_ID:
			return resp.data;
		default:
			pr_err("invalid resp type (%d) from qsee",
					resp.resp_type);
			return -ENODEV;
			break;
		}
	}
}

static int qseecom_load_app(struct qseecom_dev_handle *data, void __user *argp)
{
	struct qseecom_registered_app_list *entry = NULL;
	unsigned long flags = 0;
	u32 app_id = 0;
	struct ion_handle *ihandle;	/* Ion handle */
	struct qseecom_load_img_req load_img_req;
	int32_t ret;
	ion_phys_addr_t pa = 0;
	uint32_t len;
	struct qseecom_command_scm_resp resp;
	struct qseecom_check_app_ireq req;
	struct qseecom_load_app_ireq load_req;

	/* Copy the relevant information needed for loading the image */
	if (copy_from_user(&load_img_req,
				(void __user *)argp,
				sizeof(struct qseecom_load_img_req))) {
		pr_err("copy_from_user failed\n");
		return -EFAULT;
	}
	/* Vote for the SFPB clock */
	ret = qsee_vote_for_clock(CLK_SFPB);
	if (ret)
		pr_warning("Unable to vote for SFPB clock");
	req.qsee_cmd_id = QSEOS_APP_LOOKUP_COMMAND;
	load_img_req.img_name[MAX_APP_NAME_SIZE-1] = '\0';
	memcpy(req.app_name, load_img_req.img_name, MAX_APP_NAME_SIZE);

	ret = __qseecom_check_app_exists(req);
	if (ret < 0)
		return ret;
	else
		app_id = ret;

	if (app_id) {
		pr_warn("App id %d (%s) already exists\n", app_id,
			(char *)(req.app_name));
		spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
		list_for_each_entry(entry,
		&qseecom.registered_app_list_head, list){
			if (entry->app_id == app_id) {
				entry->ref_cnt++;
				break;
			}
		}
		spin_unlock_irqrestore(
		&qseecom.registered_app_list_lock, flags);
	} else {
		pr_warn("App (%s) does'nt exist, loading apps for first time\n",
			(char *)(load_img_req.img_name));
		/* Get the handle of the shared fd */
		ihandle = ion_import_dma_buf(qseecom.ion_clnt,
					load_img_req.ifd_data_fd);
		if (IS_ERR_OR_NULL(ihandle)) {
			pr_err("Ion client could not retrieve the handle\n");
			qsee_disable_clock_vote(CLK_SFPB);
			return -ENOMEM;
		}

		/* Get the physical address of the ION BUF */
		ret = ion_phys(qseecom.ion_clnt, ihandle, &pa, &len);

		/* Populate the structure for sending scm call to load image */
		memcpy(load_req.app_name, load_img_req.img_name,
						MAX_APP_NAME_SIZE);
		load_req.qsee_cmd_id = QSEOS_APP_START_COMMAND;
		load_req.mdt_len = load_img_req.mdt_len;
		load_req.img_len = load_img_req.img_len;
		load_req.phy_addr = pa;

		/*  SCM_CALL  to load the app and get the app_id back */
		ret = scm_call(SCM_SVC_TZSCHEDULER, 1,  &load_req,
			sizeof(struct qseecom_load_app_ireq),
			&resp, sizeof(resp));
		if (ret) {
			pr_err("scm_call to load app failed\n");
			return -EINVAL;
		}

		if (resp.result == QSEOS_RESULT_FAILURE) {
			pr_err("scm_call rsp.result is QSEOS_RESULT_FAILURE\n");
			if (!IS_ERR_OR_NULL(ihandle))
				ion_free(qseecom.ion_clnt, ihandle);
			qsee_disable_clock_vote(CLK_SFPB);
			return -EFAULT;
		}

		if (resp.result == QSEOS_RESULT_INCOMPLETE) {
			ret = __qseecom_process_incomplete_cmd(data, &resp);
			if (ret) {
				pr_err("process_incomplete_cmd failed err: %d\n",
					ret);
				if (!IS_ERR_OR_NULL(ihandle))
					ion_free(qseecom.ion_clnt, ihandle);
				qsee_disable_clock_vote(CLK_SFPB);
				return ret;
			}
		}

		if (resp.result != QSEOS_RESULT_SUCCESS) {
			pr_err("scm_call failed resp.result unknown, %d\n",
				resp.result);
			if (!IS_ERR_OR_NULL(ihandle))
				ion_free(qseecom.ion_clnt, ihandle);
			qsee_disable_clock_vote(CLK_SFPB);
			return -EFAULT;
		}

		app_id = resp.data;

		entry = kmalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			pr_err("kmalloc failed\n");
			qsee_disable_clock_vote(CLK_SFPB);
			return -ENOMEM;
		}
		entry->app_id = app_id;
		entry->ref_cnt = 1;

		/* Deallocate the handle */
		if (!IS_ERR_OR_NULL(ihandle))
			ion_free(qseecom.ion_clnt, ihandle);

		spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
		list_add_tail(&entry->list, &qseecom.registered_app_list_head);
		spin_unlock_irqrestore(&qseecom.registered_app_list_lock,
									flags);

		pr_warn("App with id %d (%s) now loaded\n", app_id,
		(char *)(load_img_req.img_name));
	}
	data->client.app_id = app_id;
	load_img_req.app_id = app_id;
	if (copy_to_user(argp, &load_img_req, sizeof(load_img_req))) {
		pr_err("copy_to_user failed\n");
		kzfree(entry);
		qsee_disable_clock_vote(CLK_SFPB);
		return -EFAULT;
	}
	qsee_disable_clock_vote(CLK_SFPB);
	return 0;
}

static int __qseecom_cleanup_app(struct qseecom_dev_handle *data)
{
	wake_up_all(&qseecom.send_resp_wq);
	while (atomic_read(&data->ioctl_count) > 1) {
		if (wait_event_freezable(data->abort_wq,
					atomic_read(&data->ioctl_count) <= 1)) {
			pr_err("Interrupted from abort\n");
			return -ERESTARTSYS;
			break;
		}
	}
	/* Set unload app */
	return 1;
}

static int qseecom_unload_app(struct qseecom_dev_handle *data)
{
	unsigned long flags;
	int ret = 0;
	struct qseecom_command_scm_resp resp;
	struct qseecom_registered_app_list *ptr_app;
	bool unload = false;
	bool found_app = false;

	if ((qseecom.qseos_version == QSEOS_VERSION_14) &&
				(data->client.app_id > 0)) {
		spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
		list_for_each_entry(ptr_app, &qseecom.registered_app_list_head,
								list) {
			if (ptr_app->app_id == data->client.app_id) {
				found_app = true;
				if (ptr_app->ref_cnt == 1) {
					unload = true;
					break;
				} else {
					ptr_app->ref_cnt--;
					pr_warn("Can't unload app(%d) inuse\n",
							ptr_app->app_id);
					break;
				}
			}
		}
		spin_unlock_irqrestore(&qseecom.registered_app_list_lock,
								flags);
		if (found_app == false) {
			pr_err("Cannot find app with id = %d\n",
						data->client.app_id);
			return -EINVAL;
		}
	}

	if ((unload) && (qseecom.qseos_version == QSEOS_VERSION_14)) {
		struct qseecom_unload_app_ireq req;

		__qseecom_cleanup_app(data);
		spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
		list_del(&ptr_app->list);
		kzfree(ptr_app);
		spin_unlock_irqrestore(&qseecom.registered_app_list_lock,
								flags);
		/* Populate the structure for sending scm call to load image */
		req.qsee_cmd_id = QSEOS_APP_SHUTDOWN_COMMAND;
		req.app_id = data->client.app_id;

		/* SCM_CALL to unload the app */
		ret = scm_call(SCM_SVC_TZSCHEDULER, 1,  &req,
				sizeof(struct qseecom_unload_app_ireq),
				&resp, sizeof(resp));
		if (ret) {
			pr_err("scm_call to unload app (id = %d) failed\n",
							req.app_id);
			return -EFAULT;
		} else {
			pr_warn("App id %d now unloaded\n", req.app_id);
		}
		if (resp.result == QSEOS_RESULT_INCOMPLETE) {
			ret = __qseecom_process_incomplete_cmd(data, &resp);
			if (ret) {
				pr_err("process_incomplete_cmd fail err: %d\n",
						ret);
				return ret;
			}
		}
	}

	if (qseecom.qseos_version == QSEOS_VERSION_13) {
		data->abort = 1;
		wake_up_all(&qseecom.send_resp_wq);
		while (atomic_read(&data->ioctl_count) > 0) {
			if (wait_event_freezable(data->abort_wq,
					atomic_read(&data->ioctl_count) <= 0)) {
				pr_err("Interrupted from abort\n");
				ret = -ERESTARTSYS;
				break;
			}
		}
	}
	if (!IS_ERR_OR_NULL(data->client.ihandle)) {
		ion_unmap_kernel(qseecom.ion_clnt, data->client.ihandle);
		ion_free(qseecom.ion_clnt, data->client.ihandle);
		data->client.ihandle = NULL;
	}
	data->released = true;
	return ret;
}

static uint32_t __qseecom_uvirt_to_kphys(struct qseecom_dev_handle *data,
						uint32_t virt)
{
	return data->client.sb_phys + (virt - data->client.user_virt_sb_base);
}

static uint32_t __qseecom_uvirt_to_kvirt(struct qseecom_dev_handle *data,
					 uint32_t virt)
{
	return (uint32_t)data->client.sb_virt +
				(virt - data->client.user_virt_sb_base);
}

static int __qseecom_send_cmd_legacy(struct qseecom_dev_handle *data,
				struct qseecom_send_cmd_req *req)
{
	int ret = 0;
	unsigned long flags;
	u32 reqd_len_sb_in = 0;
	struct qseecom_command cmd;
	struct qseecom_response resp;


	if (req->cmd_req_buf == NULL || req->resp_buf == NULL) {
		pr_err("cmd buffer or response buffer is null\n");
		return -EINVAL;
	}

	if (req->cmd_req_len <= 0 ||
		req->resp_len <= 0 ||
		req->cmd_req_len > data->client.sb_length ||
		req->resp_len > data->client.sb_length) {
		pr_err("cmd buffer length or "
				"response buffer length not valid\n");
		return -EINVAL;
	}

	reqd_len_sb_in = req->cmd_req_len + req->resp_len;
	if (reqd_len_sb_in > data->client.sb_length) {
		pr_debug("Not enough memory to fit cmd_buf and "
			"resp_buf. Required: %u, Available: %u\n",
				reqd_len_sb_in, data->client.sb_length);
		return -ENOMEM;
	}
	cmd.cmd_type = TZ_SCHED_CMD_NEW;
	cmd.sb_in_cmd_addr = (u8 *) data->client.sb_phys;
	cmd.sb_in_cmd_len = req->cmd_req_len;

	resp.cmd_status = TZ_SCHED_STATUS_INCOMPLETE;
	resp.sb_in_rsp_addr = (u8 *)data->client.sb_phys + req->cmd_req_len;
	resp.sb_in_rsp_len = req->resp_len;

	ret = scm_call(SCM_SVC_TZSCHEDULER, 1, (const void *)&cmd,
					sizeof(cmd), &resp, sizeof(resp));

	if (ret) {
		pr_err("qseecom_scm_call_legacy failed with err: %d\n", ret);
		return ret;
	}

	while (resp.cmd_status != TZ_SCHED_STATUS_COMPLETE) {
		/*
		 * If cmd is incomplete, get the callback cmd out from SB out
		 * and put it on the list
		 */
		struct qseecom_registered_listener_list *ptr_svc = NULL;
		/*
		 * We don't know which service can handle the command. so we
		 * wake up all blocking services and let them figure out if
		 * they can handle the given command.
		 */
		spin_lock_irqsave(&qseecom.registered_listener_list_lock,
					flags);
		list_for_each_entry(ptr_svc,
				&qseecom.registered_listener_list_head, list) {
				ptr_svc->rcv_req_flag = 1;
				wake_up_interruptible(&ptr_svc->rcv_req_wq);
		}
		spin_unlock_irqrestore(&qseecom.registered_listener_list_lock,
				flags);

		pr_debug("waking up rcv_req_wq and "
				"waiting for send_resp_wq\n");
		if (wait_event_freezable(qseecom.send_resp_wq,
				__qseecom_listener_has_sent_rsp(data))) {
			pr_warning("qseecom Interrupted: exiting send_cmd loop\n");
			return -ERESTARTSYS;
		}

		if (data->abort) {
			pr_err("Aborting driver\n");
			return -ENODEV;
		}
		qseecom.send_resp_flag = 0;
		cmd.cmd_type = TZ_SCHED_CMD_PENDING;
		ret = scm_call(SCM_SVC_TZSCHEDULER, 1, (const void *)&cmd,
					sizeof(cmd), &resp, sizeof(resp));
		if (ret) {
			pr_err("qseecom_scm_call failed with err: %d\n", ret);
			return ret;
		}
	}
	return ret;
}

static int __qseecom_send_cmd(struct qseecom_dev_handle *data,
				struct qseecom_send_cmd_req *req)
{
	int ret = 0;
	u32 reqd_len_sb_in = 0;
	struct qseecom_client_send_data_ireq send_data_req;
	struct qseecom_command_scm_resp resp;

	if (req->cmd_req_buf == NULL || req->resp_buf == NULL) {
		pr_err("cmd buffer or response buffer is null\n");
		return -EINVAL;
	}
	if (((uint32_t)req->cmd_req_buf < data->client.user_virt_sb_base) ||
		((uint32_t)req->cmd_req_buf >= (data->client.user_virt_sb_base +
					data->client.sb_length))) {
		pr_err("cmd buffer address not within shared bufffer\n");
		return -EINVAL;
	}


	if (((uint32_t)req->resp_buf < data->client.user_virt_sb_base)  ||
		((uint32_t)req->resp_buf >= (data->client.user_virt_sb_base +
					data->client.sb_length))){
		pr_err("response buffer address not within shared bufffer\n");
		return -EINVAL;
	}

	if ((req->cmd_req_len == 0) || (req->resp_len == 0) ||
		req->cmd_req_len > data->client.sb_length ||
		req->resp_len > data->client.sb_length) {
		pr_err("cmd buffer length or "
				"response buffer length not valid\n");
		return -EINVAL;
	}

	if (req->cmd_req_len > UINT_MAX - req->resp_len) {
		pr_err("Integer overflow detected in req_len & rsp_len, exiting now\n");
		return -EINVAL;
	}

	reqd_len_sb_in = req->cmd_req_len + req->resp_len;
	if (reqd_len_sb_in > data->client.sb_length) {
		pr_debug("Not enough memory to fit cmd_buf and "
			"resp_buf. Required: %u, Available: %u\n",
				reqd_len_sb_in, data->client.sb_length);
		return -ENOMEM;
	}

	send_data_req.qsee_cmd_id = QSEOS_CLIENT_SEND_DATA_COMMAND;
	send_data_req.app_id = data->client.app_id;
	send_data_req.req_ptr = (void *)(__qseecom_uvirt_to_kphys(data,
					(uint32_t)req->cmd_req_buf));
	send_data_req.req_len = req->cmd_req_len;
	send_data_req.rsp_ptr = (void *)(__qseecom_uvirt_to_kphys(data,
					(uint32_t)req->resp_buf));
	send_data_req.rsp_len = req->resp_len;

	ret = scm_call(SCM_SVC_TZSCHEDULER, 1, (const void *) &send_data_req,
					sizeof(send_data_req),
					&resp, sizeof(resp));
	if (ret) {
		pr_err("qseecom_scm_call failed with err: %d\n", ret);
		return ret;
	}

	if (resp.result == QSEOS_RESULT_INCOMPLETE) {
		ret = __qseecom_process_incomplete_cmd(data, &resp);
		if (ret) {
			pr_err("process_incomplete_cmd failed err: %d\n", ret);
			return ret;
		}
	} else {
		if (resp.result != QSEOS_RESULT_SUCCESS) {
			pr_err("Response result %d not supported\n",
							resp.result);
			ret = -EINVAL;
		}
	}
	return ret;
}


static int qseecom_send_cmd(struct qseecom_dev_handle *data, void __user *argp)
{
	int ret = 0;
	struct qseecom_send_cmd_req req;

	ret = copy_from_user(&req, argp, sizeof(req));
	if (ret) {
		pr_err("copy_from_user failed\n");
		return ret;
	}
	if (qseecom.qseos_version == QSEOS_VERSION_14)
		ret = __qseecom_send_cmd(data, &req);
	else
		ret = __qseecom_send_cmd_legacy(data, &req);
	if (ret)
		return ret;

	pr_debug("sending cmd_req->rsp size: %u, ptr: 0x%p\n",
			req.resp_len, req.resp_buf);
	return ret;
}

static int __qseecom_send_cmd_req_clean_up(
			struct qseecom_send_modfd_cmd_req *req)
{
	char *field;
	uint32_t *update;
	int ret = 0;
	int i = 0;

	for (i = 0; i < MAX_ION_FD; i++) {
		if (req->ifd_data[i].fd > 0) {
			field = (char *)req->cmd_req_buf +
					req->ifd_data[i].cmd_buf_offset;
			update = (uint32_t *) field;
			*update = 0;
		}
	}
	return ret;
}

static int __qseecom_update_with_phy_addr(
			struct qseecom_send_modfd_cmd_req *req)
{
	struct ion_handle *ihandle;
	char *field;
	uint32_t *update;
	ion_phys_addr_t pa;
	int ret = 0;
	int i = 0;
	uint32_t length;

	for (i = 0; i < MAX_ION_FD; i++) {
		if (req->ifd_data[i].fd > 0) {
			/* Get the handle of the shared fd */
			ihandle = ion_import_dma_buf(qseecom.ion_clnt,
						req->ifd_data[i].fd);
			if (IS_ERR_OR_NULL(ihandle)) {
				pr_err("Ion client can't retrieve the handle\n");
				return -ENOMEM;
			}
			field = (char *) req->cmd_req_buf +
						req->ifd_data[i].cmd_buf_offset;
			update = (uint32_t *) field;

			/* Populate the cmd data structure with the phys_addr */
			ret = ion_phys(qseecom.ion_clnt, ihandle, &pa, &length);
			if (ret)
				return -ENOMEM;

			*update = (uint32_t)pa;
			/* Deallocate the handle */
			if (!IS_ERR_OR_NULL(ihandle))
				ion_free(qseecom.ion_clnt, ihandle);
		}
	}
	return ret;
}

static int qseecom_send_modfd_cmd(struct qseecom_dev_handle *data,
					void __user *argp)
{
	int ret = 0;
	int i;
	struct qseecom_send_modfd_cmd_req req;
	struct qseecom_send_cmd_req send_cmd_req;

	ret = copy_from_user(&req, argp, sizeof(req));
	if (ret) {
		pr_err("copy_from_user failed\n");
		return ret;
	}

	if (req.cmd_req_buf == NULL || req.resp_buf == NULL) {
		pr_err("cmd buffer or response buffer is null\n");
		return -EINVAL;
	}
	if (((uint32_t)req.cmd_req_buf < data->client.user_virt_sb_base) ||
		((uint32_t)req.cmd_req_buf >= (data->client.user_virt_sb_base +
					data->client.sb_length))) {
		pr_err("cmd buffer address not within shared bufffer\n");
		return -EINVAL;
	}

	if (((uint32_t)req.resp_buf < data->client.user_virt_sb_base)  ||
		((uint32_t)req.resp_buf >= (data->client.user_virt_sb_base +
					data->client.sb_length))){
		pr_err("response buffer address not within shared bufffer\n");
		return -EINVAL;
	}
	if (req.cmd_req_len == 0 || req.cmd_req_len > data->client.sb_length ||
			req.resp_len > data->client.sb_length) {
		pr_err("cmd or response buffer length not valid\n");
		return -EINVAL;
	}

	send_cmd_req.cmd_req_buf = req.cmd_req_buf;
	send_cmd_req.cmd_req_len = req.cmd_req_len;
	send_cmd_req.resp_buf = req.resp_buf;
	send_cmd_req.resp_len = req.resp_len;

	/* validate offsets */
	for (i = 0; i < MAX_ION_FD; i++) {
		if (req.ifd_data[i].cmd_buf_offset >= req.cmd_req_len) {
			pr_err("Invalid offset %d = 0x%x\n",
						 i, req.ifd_data[i].cmd_buf_offset);
			return -EINVAL;
		}
	}
	req.cmd_req_buf = (void *)__qseecom_uvirt_to_kvirt(data,
						(uint32_t)req.cmd_req_buf);
	req.resp_buf = (void *)__qseecom_uvirt_to_kvirt(data,
						(uint32_t)req.resp_buf);

	ret = __qseecom_update_with_phy_addr(&req);
	if (ret)
		return ret;
	if (qseecom.qseos_version == QSEOS_VERSION_14)
		ret = __qseecom_send_cmd(data, &send_cmd_req);
	else
		ret = __qseecom_send_cmd_legacy(data, &send_cmd_req);
	__qseecom_send_cmd_req_clean_up(&req);

	if (ret)
		return ret;

	pr_debug("sending cmd_req->rsp size: %u, ptr: 0x%p\n",
			req.resp_len, req.resp_buf);
	return ret;
}

static int __qseecom_listener_has_rcvd_req(struct qseecom_dev_handle *data,
		struct qseecom_registered_listener_list *svc)
{
	int ret;
	ret = (svc->rcv_req_flag != 0);
	return ret || data->abort;
}

static int qseecom_receive_req(struct qseecom_dev_handle *data)
{
	int ret = 0;
	struct qseecom_registered_listener_list *this_lstnr;

	this_lstnr = __qseecom_find_svc(data->listener.id);
	while (1) {
		if (wait_event_freezable(this_lstnr->rcv_req_wq,
				__qseecom_listener_has_rcvd_req(data,
				this_lstnr))) {
			pr_warning("Interrupted: exiting wait_rcv_req loop\n");
			/* woken up for different reason */
			return -ERESTARTSYS;
		}

		if (data->abort) {
			pr_err("Aborting driver!\n");
			return -ENODEV;
		}
		this_lstnr->rcv_req_flag = 0;
		if (qseecom.qseos_version == QSEOS_VERSION_13) {
			if (*((uint32_t *)this_lstnr->sb_virt) != 0)
				break;
		} else {
			break;
		}
	}
	return ret;
}

static bool __qseecom_is_fw_image_valid(const struct firmware *fw_entry)
{
	struct elf32_hdr *ehdr;

	if (fw_entry->size < sizeof(*ehdr)) {
		pr_err("%s: Not big enough to be an elf header\n",
				 qseecom.pdev->init_name);
		return false;
	}
	ehdr = (struct elf32_hdr *)fw_entry->data;
	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG)) {
		pr_err("%s: Not an elf header\n",
				 qseecom.pdev->init_name);
		return false;
	}

	if (ehdr->e_phnum == 0) {
		pr_err("%s: No loadable segments\n",
				 qseecom.pdev->init_name);
		return false;
	}
	if (sizeof(struct elf32_phdr) * ehdr->e_phnum +
	    sizeof(struct elf32_hdr) > fw_entry->size) {
		pr_err("%s: Program headers not within mdt\n",
				 qseecom.pdev->init_name);
		return false;
	}
	return true;
}

static int __qseecom_get_fw_size(char *appname, uint32_t *fw_size)
{
	int ret = -1;
	int i = 0, rc = 0;
	const struct firmware *fw_entry = NULL;
	struct elf32_phdr *phdr;
	char fw_name[MAX_APP_NAME_SIZE];
	struct elf32_hdr *ehdr;
	int num_images = 0;

	snprintf(fw_name, sizeof(fw_name), "%s.mdt", appname);
	rc = request_firmware(&fw_entry, fw_name,  qseecom.pdev);
	if (rc) {
		pr_err("error with request_firmware\n");
		ret = -EIO;
		goto err;
	}
	if (!__qseecom_is_fw_image_valid(fw_entry)) {
		ret = -EIO;
		goto err;
	}
	*fw_size = fw_entry->size;
	phdr = (struct elf32_phdr *)(fw_entry->data + sizeof(struct elf32_hdr));
	ehdr = (struct elf32_hdr *)fw_entry->data;
	num_images = ehdr->e_phnum;
	release_firmware(fw_entry);
	for (i = 0; i < num_images; i++, phdr++) {
		memset(fw_name, 0, sizeof(fw_name));
		snprintf(fw_name, ARRAY_SIZE(fw_name), "%s.b%02d", appname, i);
		ret = request_firmware(&fw_entry, fw_name, qseecom.pdev);
		if (ret)
			goto err;
		*fw_size += fw_entry->size;
		release_firmware(fw_entry);
	}
	return ret;
err:
	if (fw_entry)
		release_firmware(fw_entry);
	*fw_size = 0;
	return ret;
}

static int __qseecom_get_fw_data(char *appname, u8 *img_data,
					struct qseecom_load_app_ireq *load_req)
{
	int ret = -1;
	int i = 0, rc = 0;
	const struct firmware *fw_entry = NULL;
	char fw_name[MAX_APP_NAME_SIZE];
	u8 *img_data_ptr = img_data;
	struct elf32_hdr *ehdr;
	int num_images = 0;

	snprintf(fw_name, sizeof(fw_name), "%s.mdt", appname);
	rc = request_firmware(&fw_entry, fw_name,  qseecom.pdev);
	if (rc) {
		ret = -EIO;
		goto err;
	}
	load_req->img_len = fw_entry->size;
	memcpy(img_data_ptr, fw_entry->data, fw_entry->size);
	img_data_ptr = img_data_ptr + fw_entry->size;
	load_req->mdt_len = fw_entry->size; /*Get MDT LEN*/
	ehdr = (struct elf32_hdr *)fw_entry->data;
	num_images = ehdr->e_phnum;
	release_firmware(fw_entry);
	for (i = 0; i < num_images; i++) {
		snprintf(fw_name, ARRAY_SIZE(fw_name), "%s.b%02d", appname, i);
		ret = request_firmware(&fw_entry, fw_name,  qseecom.pdev);
		if (ret) {
			pr_err("Failed to locate blob %s\n", fw_name);
			goto err;
		}
		memcpy(img_data_ptr, fw_entry->data, fw_entry->size);
		img_data_ptr = img_data_ptr + fw_entry->size;
		load_req->img_len += fw_entry->size;
		release_firmware(fw_entry);
	}
	load_req->phy_addr = virt_to_phys(img_data);
	return ret;
err:
	release_firmware(fw_entry);
	return ret;
}

static int __qseecom_load_fw(struct qseecom_dev_handle *data, char *appname)
{
	int ret = -1;
	uint32_t fw_size = 0;
	struct qseecom_load_app_ireq load_req = {0, 0, 0, 0};
	struct qseecom_command_scm_resp resp;
	u8 *img_data = NULL;

	if (__qseecom_get_fw_size(appname, &fw_size))
		return -EIO;

	img_data = kzalloc(fw_size, GFP_KERNEL);
	if (!img_data) {
		pr_err("Failied to allocate memory for copying image data\n");
		return -ENOMEM;
	}
	ret = __qseecom_get_fw_data(appname, img_data, &load_req);
	if (ret) {
		kzfree(img_data);
		return -EIO;
	}

	/* Populate the remaining parameters */
	load_req.qsee_cmd_id = QSEOS_APP_START_COMMAND;
	memcpy(load_req.app_name, appname, MAX_APP_NAME_SIZE);
	ret = qsee_vote_for_clock(CLK_SFPB);
	if (ret) {
		kzfree(img_data);
		pr_warning("Unable to vote for SFPB clock");
		return -EIO;
	}

	/* SCM_CALL to load the image */
	ret = scm_call(SCM_SVC_TZSCHEDULER, 1,	&load_req,
			sizeof(struct qseecom_load_app_ireq),
			&resp, sizeof(resp));
	kzfree(img_data);
	if (ret) {
		pr_err("scm_call to load failed : ret %d\n", ret);
		qsee_disable_clock_vote(CLK_SFPB);
		return -EIO;
	}

	switch (resp.result) {
	case QSEOS_RESULT_SUCCESS:
		ret = resp.data;
		break;
	case QSEOS_RESULT_INCOMPLETE:
		ret = __qseecom_process_incomplete_cmd(data, &resp);
		if (ret)
			pr_err("process_incomplete_cmd FAILED\n");
		else
			ret = resp.data;
		break;
	case QSEOS_RESULT_FAILURE:
		pr_err("scm call failed with response QSEOS_RESULT FAILURE\n");
		break;
	default:
		pr_err("scm call return unknown response %d\n", resp.result);
		ret = -EINVAL;
		break;
	}
	qsee_disable_clock_vote(CLK_SFPB);

	return ret;
}

int qseecom_start_app(struct qseecom_handle **handle,
						char *app_name, uint32_t size)
{
	int32_t ret;
	unsigned long flags = 0;
	struct qseecom_dev_handle *data = NULL;
	struct qseecom_check_app_ireq app_ireq;
	struct qseecom_registered_app_list *entry = NULL;
	struct qseecom_registered_kclient_list *kclient_entry = NULL;
	bool found_app = false;
	uint32_t len;
	ion_phys_addr_t pa;

	if (qseecom.qseos_version == QSEOS_VERSION_13) {
		pr_err("This functionality is UNSUPPORTED in version 1.3\n");
		return -EINVAL;
	}

	*handle = kzalloc(sizeof(struct qseecom_handle), GFP_KERNEL);
	if (!(*handle)) {
		pr_err("failed to allocate memory for kernel client handle\n");
		return -ENOMEM;
	}

	app_ireq.qsee_cmd_id = QSEOS_APP_LOOKUP_COMMAND;
	memcpy(app_ireq.app_name, app_name, MAX_APP_NAME_SIZE);
	ret = __qseecom_check_app_exists(app_ireq);
	if (ret < 0)
		return -EINVAL;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		pr_err("kmalloc failed\n");
		if (ret == 0) {
			kfree(*handle);
			*handle = NULL;
		}
		return -ENOMEM;
	}
	data->abort = 0;
	data->type = QSEECOM_CLIENT_APP;
	data->service = false;
	data->released = false;
	data->client.app_id = ret;
	data->client.sb_length = size;
	data->client.user_virt_sb_base = 0;
	data->client.ihandle = NULL;

	init_waitqueue_head(&data->abort_wq);
	atomic_set(&data->ioctl_count, 0);

	data->client.ihandle = ion_alloc(qseecom.ion_clnt, size, 4096,
				ION_HEAP(ION_QSECOM_HEAP_ID), 0);
	if (IS_ERR_OR_NULL(data->client.ihandle)) {
		pr_err("Ion client could not retrieve the handle\n");
		kfree(data);
		kfree(*handle);
		*handle = NULL;
		return -EINVAL;
	}

	if (ret > 0) {
		pr_warn("App id %d for [%s] app exists\n", ret,
			(char *)app_ireq.app_name);
		spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
		list_for_each_entry(entry,
				&qseecom.registered_app_list_head, list){
			if (entry->app_id == ret) {
				entry->ref_cnt++;
				found_app = true;
				break;
			}
		}
		spin_unlock_irqrestore(
				&qseecom.registered_app_list_lock, flags);
		if (!found_app)
			pr_warn("App_id %d [%s] was loaded but not registered\n",
					ret, (char *)app_ireq.app_name);
	} else {
		/* load the app and get the app_id  */
		pr_debug("%s: Loading app for the first time'\n",
				qseecom.pdev->init_name);
		mutex_lock(&app_access_lock);
		ret = __qseecom_load_fw(data, app_name);
		mutex_unlock(&app_access_lock);

		if (ret < 0) {
			kfree(*handle);
			*handle = NULL;
			return ret;
		}
		data->client.app_id = ret;
	}
	if (!found_app) {
		entry = kmalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			pr_err("kmalloc failed\n");
			return -ENOMEM;
		}
		entry->app_id = ret;
		entry->ref_cnt = 1;

		spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
		list_add_tail(&entry->list, &qseecom.registered_app_list_head);
		spin_unlock_irqrestore(&qseecom.registered_app_list_lock,
									flags);
	}

	/* Get the physical address of the ION BUF */
	ret = ion_phys(qseecom.ion_clnt, data->client.ihandle, &pa, &len);
	/* Populate the structure for sending scm call to load image */
	data->client.sb_virt = (char *) ion_map_kernel(qseecom.ion_clnt,
							data->client.ihandle);
	data->client.user_virt_sb_base = (uint32_t)data->client.sb_virt;
	data->client.sb_phys = pa;
	(*handle)->dev = (void *)data;
	(*handle)->sbuf = (unsigned char *)data->client.sb_virt;
	(*handle)->sbuf_len = data->client.sb_length;

	kclient_entry = kzalloc(sizeof(*kclient_entry), GFP_KERNEL);
	if (!kclient_entry) {
		pr_err("kmalloc failed\n");
		return -ENOMEM;
	}
	kclient_entry->handle = *handle;

	spin_lock_irqsave(&qseecom.registered_kclient_list_lock, flags);
	list_add_tail(&kclient_entry->list,
			&qseecom.registered_kclient_list_head);
	spin_unlock_irqrestore(&qseecom.registered_kclient_list_lock, flags);

	return 0;
}
EXPORT_SYMBOL(qseecom_start_app);

int qseecom_shutdown_app(struct qseecom_handle **handle)
{
	int ret = -EINVAL;
	struct qseecom_dev_handle *data;

	struct qseecom_registered_kclient_list *kclient = NULL;
	unsigned long flags = 0;
	bool found_handle = false;

	if (qseecom.qseos_version == QSEOS_VERSION_13) {
		pr_err("This functionality is UNSUPPORTED in version 1.3\n");
		return -EINVAL;
	}
	if ((handle == NULL)  || (*handle == NULL)) {
		pr_err("Handle is not initialized\n");
		return -EINVAL;
	}
	data =	(struct qseecom_dev_handle *) ((*handle)->dev);
	spin_lock_irqsave(&qseecom.registered_kclient_list_lock, flags);
	list_for_each_entry(kclient, &qseecom.registered_kclient_list_head,
				list) {
		if (kclient->handle == (*handle)) {
			list_del(&kclient->list);
			found_handle = true;
			break;
		}
	}
	spin_unlock_irqrestore(&qseecom.registered_kclient_list_lock, flags);
	if (!found_handle)
		pr_err("Unable to find the handle, exiting\n");
	else
		ret = qseecom_unload_app(data);
	if (ret == 0) {
		kzfree(data);
		kzfree(*handle);
		kzfree(kclient);
		*handle = NULL;
	}
	return ret;
}
EXPORT_SYMBOL(qseecom_shutdown_app);

int qseecom_send_command(struct qseecom_handle *handle, void *send_buf,
			uint32_t sbuf_len, void *resp_buf, uint32_t rbuf_len)
{
	int ret = 0;
	struct qseecom_send_cmd_req req = {0, 0, 0, 0};
	struct qseecom_dev_handle *data;

	if (qseecom.qseos_version == QSEOS_VERSION_13) {
		pr_err("This functionality is UNSUPPORTED in version 1.3\n");
		return -EINVAL;
	}

	if (handle == NULL) {
		pr_err("Handle is not initialized\n");
		return -EINVAL;
	}
	data = handle->dev;

	req.cmd_req_len = sbuf_len;
	req.resp_len = rbuf_len;
	req.cmd_req_buf = send_buf;
	req.resp_buf = resp_buf;

	mutex_lock(&app_access_lock);
	atomic_inc(&data->ioctl_count);

	ret = __qseecom_send_cmd(data, &req);

	atomic_dec(&data->ioctl_count);
	mutex_unlock(&app_access_lock);

	if (ret)
		return ret;

	pr_debug("sending cmd_req->rsp size: %u, ptr: 0x%p\n",
			req.resp_len, req.resp_buf);
	return ret;
}
EXPORT_SYMBOL(qseecom_send_command);

int qseecom_set_bandwidth(struct qseecom_handle *handle, bool high)
{
	int ret = 0;
	if ((handle == NULL) || (handle->dev == NULL)) {
		pr_err("No valid kernel client\n");
		return -EINVAL;
	}
	if (high) {
		ret = qsee_vote_for_clock(CLK_DFAB);
		if (ret)
			pr_err("Failed to vote for DFAB clock%d\n", ret);
		ret = qsee_vote_for_clock(CLK_SFPB);
		if (ret) {
			pr_err("Failed to vote for SFPB clock%d\n", ret);
			qsee_disable_clock_vote(CLK_DFAB);
		}
	} else {
		qsee_disable_clock_vote(CLK_DFAB);
		qsee_disable_clock_vote(CLK_SFPB);
	}
	return ret;
}
EXPORT_SYMBOL(qseecom_set_bandwidth);

static int qseecom_send_resp(void)
{
	qseecom.send_resp_flag = 1;
	wake_up_interruptible(&qseecom.send_resp_wq);
	return 0;
}

static int qseecom_get_qseos_version(struct qseecom_dev_handle *data,
						void __user *argp)
{
	struct qseecom_qseos_version_req req;

	if (copy_from_user(&req, argp, sizeof(req))) {
		pr_err("copy_from_user failed");
		return -EINVAL;
	}
	req.qseos_version = qseecom.qseos_version;
	if (copy_to_user(argp, &req, sizeof(req))) {
		pr_err("copy_to_user failed");
		return -EINVAL;
	}
	return 0;
}

static int qsee_vote_for_clock(int32_t clk_type)
{
	int ret = 0;

	if (!qsee_perf_client)
		return ret;

	switch (clk_type) {
	case CLK_DFAB:
		mutex_lock(&qsee_bw_mutex);
		if (!qsee_bw_count) {
			if (qsee_sfpb_bw_count > 0)
				ret = msm_bus_scale_client_update_request(
						qsee_perf_client, 3);
			else
				ret = msm_bus_scale_client_update_request(
						qsee_perf_client, 1);
			if (ret)
				pr_err("DFAB Bandwidth req failed (%d)\n",
								ret);
			else
				qsee_bw_count++;
		} else {
			qsee_bw_count++;
		}
		mutex_unlock(&qsee_bw_mutex);
		break;
	case CLK_SFPB:
		mutex_lock(&qsee_bw_mutex);
		if (!qsee_sfpb_bw_count) {
			if (qsee_bw_count > 0)
				ret = msm_bus_scale_client_update_request(
						qsee_perf_client, 3);
			else
				ret = msm_bus_scale_client_update_request(
						qsee_perf_client, 2);

			if (ret)
				pr_err("SFPB Bandwidth req failed (%d)\n",
								ret);
			else
				qsee_sfpb_bw_count++;
		} else {
			qsee_sfpb_bw_count++;
		}
		mutex_unlock(&qsee_bw_mutex);
		break;
	default:
		pr_err("Clock type not defined\n");
		break;
	}
	return ret;
}

static void qsee_disable_clock_vote(int32_t clk_type)
{
	int32_t ret = 0;

	if (!qsee_perf_client)
		return;

	switch (clk_type) {
	case CLK_DFAB:
		mutex_lock(&qsee_bw_mutex);
		if (qsee_bw_count == 0) {
			pr_err("Client error.Extra call to disable DFAB clk\n");
			mutex_unlock(&qsee_bw_mutex);
			return;
		}

		if ((qsee_bw_count > 0) && (qsee_bw_count-- == 1)) {
			if (qsee_sfpb_bw_count > 0)
				ret = msm_bus_scale_client_update_request(
						qsee_perf_client, 2);
			else
				ret = msm_bus_scale_client_update_request(
						qsee_perf_client, 0);
			if (ret)
				pr_err("SFPB Bandwidth req fail (%d)\n",
								ret);
		}
		mutex_unlock(&qsee_bw_mutex);
		break;
	case CLK_SFPB:
		mutex_lock(&qsee_bw_mutex);
		if (qsee_sfpb_bw_count == 0) {
			pr_err("Client error.Extra call to disable SFPB clk\n");
			mutex_unlock(&qsee_bw_mutex);
			return;
		}
		if ((qsee_sfpb_bw_count > 0) && (qsee_sfpb_bw_count-- == 1)) {
			if (qsee_bw_count > 0)
				ret = msm_bus_scale_client_update_request(
						qsee_perf_client, 1);
			else
				ret = msm_bus_scale_client_update_request(
						qsee_perf_client, 0);
			if (ret)
				pr_err("SFPB Bandwidth req fail (%d)\n",
								ret);
		}
		mutex_unlock(&qsee_bw_mutex);
		break;
	default:
		pr_err("Clock type not defined\n");
		break;
	}

}

static int qseecom_load_external_elf(struct qseecom_dev_handle *data,
				void __user *argp)
{
	struct ion_handle *ihandle;	/* Ion handle */
	struct qseecom_load_img_req load_img_req;
	int ret;
	int set_cpu_ret = 0;
	ion_phys_addr_t pa = 0;
	uint32_t len;
	struct cpumask mask;
	struct qseecom_load_app_ireq load_req;
	struct qseecom_command_scm_resp resp;

	/* Copy the relevant information needed for loading the image */
	if (copy_from_user(&load_img_req,
				(void __user *)argp,
				sizeof(struct qseecom_load_img_req))) {
		pr_err("copy_from_user failed\n");
		return -EFAULT;
	}

	/* Get the handle of the shared fd */
	ihandle = ion_import_dma_buf(qseecom.ion_clnt,
				load_img_req.ifd_data_fd);
	if (IS_ERR_OR_NULL(ihandle)) {
		pr_err("Ion client could not retrieve the handle\n");
		return -ENOMEM;
	}

	/* Get the physical address of the ION BUF */
	ret = ion_phys(qseecom.ion_clnt, ihandle, &pa, &len);

	/* Populate the structure for sending scm call to load image */
	load_req.qsee_cmd_id = QSEOS_LOAD_EXTERNAL_ELF_COMMAND;
	load_req.mdt_len = load_img_req.mdt_len;
	load_req.img_len = load_img_req.img_len;
	load_req.phy_addr = pa;

	/* SCM_CALL tied to Core0 */
	mask = CPU_MASK_CPU0;
	set_cpu_ret = set_cpus_allowed_ptr(current, &mask);
	if (set_cpu_ret) {
		pr_err("set_cpus_allowed_ptr failed : ret %d\n",
				set_cpu_ret);
		ret = -EFAULT;
		goto qseecom_load_external_elf_set_cpu_err;
	}

	/*  SCM_CALL to load the external elf */
	ret = scm_call(SCM_SVC_TZSCHEDULER, 1,  &load_req,
			sizeof(struct qseecom_load_app_ireq),
			&resp, sizeof(resp));
	if (ret) {
		pr_err("scm_call to load failed : ret %d\n",
				ret);
		ret = -EFAULT;
		goto qseecom_load_external_elf_scm_err;
	}

	if (resp.result == QSEOS_RESULT_INCOMPLETE) {
		ret = __qseecom_process_incomplete_cmd(data, &resp);
		if (ret)
			pr_err("process_incomplete_cmd failed err: %d\n",
					ret);
	} else {
		if (resp.result != QSEOS_RESULT_SUCCESS) {
			pr_err("scm_call to load image failed resp.result =%d\n",
						resp.result);
			ret = -EFAULT;
		}
	}

qseecom_load_external_elf_scm_err:
	/* Restore the CPU mask */
	mask = CPU_MASK_ALL;
	set_cpu_ret = set_cpus_allowed_ptr(current, &mask);
	if (set_cpu_ret) {
		pr_err("set_cpus_allowed_ptr failed to restore mask: ret %d\n",
				set_cpu_ret);
		ret = -EFAULT;
	}

qseecom_load_external_elf_set_cpu_err:
	/* Deallocate the handle */
	if (!IS_ERR_OR_NULL(ihandle))
		ion_free(qseecom.ion_clnt, ihandle);

	return ret;
}

static int qseecom_unload_external_elf(struct qseecom_dev_handle *data)
{
	int ret = 0;
	int set_cpu_ret = 0;
	struct qseecom_command_scm_resp resp;
	struct qseecom_unload_app_ireq req;
	struct cpumask mask;

	/* unavailable client app */
	data->type = QSEECOM_UNAVAILABLE_CLIENT_APP;

	/* Populate the structure for sending scm call to unload image */
	req.qsee_cmd_id = QSEOS_UNLOAD_EXTERNAL_ELF_COMMAND;

	/* SCM_CALL tied to Core0 */
	mask = CPU_MASK_CPU0;
	ret = set_cpus_allowed_ptr(current, &mask);
	if (ret) {
		pr_err("set_cpus_allowed_ptr failed : ret %d\n",
				ret);
		return -EFAULT;
	}

	/* SCM_CALL to unload the external elf */
	ret = scm_call(SCM_SVC_TZSCHEDULER, 1,  &req,
			sizeof(struct qseecom_unload_app_ireq),
			&resp, sizeof(resp));
	if (ret) {
		pr_err("scm_call to unload failed : ret %d\n",
				ret);
		ret = -EFAULT;
		goto qseecom_unload_external_elf_scm_err;
	}
	if (resp.result == QSEOS_RESULT_INCOMPLETE) {
		ret = __qseecom_process_incomplete_cmd(data, &resp);
		if (ret)
			pr_err("process_incomplete_cmd fail err: %d\n",
					ret);
	} else {
		if (resp.result != QSEOS_RESULT_SUCCESS) {
			pr_err("scm_call to unload image failed resp.result =%d\n",
						resp.result);
			ret = -EFAULT;
		}
	}

qseecom_unload_external_elf_scm_err:
	/* Restore the CPU mask */
	mask = CPU_MASK_ALL;
	set_cpu_ret = set_cpus_allowed_ptr(current, &mask);
	if (set_cpu_ret) {
		pr_err("set_cpus_allowed_ptr failed to restore mask: ret %d\n",
				set_cpu_ret);
		ret = -EFAULT;
	}

	return ret;
}

static int qseecom_query_app_loaded(struct qseecom_dev_handle *data,
					void __user *argp)
{

	int32_t ret;
	struct qseecom_qseos_app_load_query query_req;
	struct qseecom_check_app_ireq req;
	struct qseecom_registered_app_list *entry = NULL;
	unsigned long flags = 0;

	/* Copy the relevant information needed for loading the image */
	if (copy_from_user(&query_req,
				(void __user *)argp,
				sizeof(struct qseecom_qseos_app_load_query))) {
		pr_err("copy_from_user failed\n");
		return -EFAULT;
	}

	req.qsee_cmd_id = QSEOS_APP_LOOKUP_COMMAND;
	query_req.app_name[MAX_APP_NAME_SIZE-1] = '\0';
	memcpy(req.app_name, query_req.app_name, MAX_APP_NAME_SIZE);

	ret = __qseecom_check_app_exists(req);

	if ((ret == -EINVAL) || (ret == -ENODEV)) {
		pr_err(" scm call to check if app is loaded failed");
		return ret;	/* scm call failed */
	} else if (ret > 0) {
		pr_warn("App id %d (%s) already exists\n", ret,
			(char *)(req.app_name));
		spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
		list_for_each_entry(entry,
				&qseecom.registered_app_list_head, list){
			if (entry->app_id == ret) {
				entry->ref_cnt++;
				break;
			}
		}
		spin_unlock_irqrestore(
				&qseecom.registered_app_list_lock, flags);
		data->client.app_id = ret;
		query_req.app_id = ret;

		if (copy_to_user(argp, &query_req, sizeof(query_req))) {
			pr_err("copy_to_user failed\n");
			return -EFAULT;
		}
		return -EEXIST;	/* app already loaded */
	} else {
		return 0;	/* app not loaded */
	}
}

static long qseecom_ioctl(struct file *file, unsigned cmd,
		unsigned long arg)
{
	int ret = 0;
	struct qseecom_dev_handle *data = file->private_data;
	void __user *argp = (void __user *) arg;

	if (data->abort) {
		pr_err("Aborting qseecom driver\n");
		return -ENODEV;
	}

	switch (cmd) {
	case QSEECOM_IOCTL_REGISTER_LISTENER_REQ: {
		if (data->type != QSEECOM_GENERIC) {
			pr_err("reg lstnr req: invalid handle (%d)\n",
								data->type);
			ret = -EINVAL;
			break;
		}
		pr_debug("ioctl register_listener_req()\n");
		atomic_inc(&data->ioctl_count);
		data->type = QSEECOM_LISTENER_SERVICE;
		ret = qseecom_register_listener(data, argp);
		atomic_dec(&data->ioctl_count);
		wake_up_all(&data->abort_wq);
		if (ret)
			pr_err("failed qseecom_register_listener: %d\n", ret);
		break;
	}
	case QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ: {
		if ((data->listener.id == 0) ||
			(data->type != QSEECOM_LISTENER_SERVICE)) {
			pr_err("unreg lstnr req: invalid handle (%d) lid(%d)\n",
						data->type, data->listener.id);
			ret = -EINVAL;
			break;
		}
		pr_debug("ioctl unregister_listener_req()\n");
		atomic_inc(&data->ioctl_count);
		ret = qseecom_unregister_listener(data);
		atomic_dec(&data->ioctl_count);
		wake_up_all(&data->abort_wq);
		if (ret)
			pr_err("failed qseecom_unregister_listener: %d\n", ret);
		break;
	}
	case QSEECOM_IOCTL_SEND_CMD_REQ: {
		if ((data->client.app_id == 0) ||
			(data->type != QSEECOM_CLIENT_APP)) {
			pr_err("send cmd req: invalid handle (%d) app_id(%d)\n",
					data->type, data->client.app_id);
			ret = -EINVAL;
			break;
		}
		/* Only one client allowed here at a time */
		mutex_lock(&app_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_send_cmd(data, argp);
		atomic_dec(&data->ioctl_count);
		wake_up_all(&data->abort_wq);
		mutex_unlock(&app_access_lock);
		if (ret)
			pr_err("failed qseecom_send_cmd: %d\n", ret);
		break;
	}
	case QSEECOM_IOCTL_SEND_MODFD_CMD_REQ: {
		if ((data->client.app_id == 0) ||
			(data->type != QSEECOM_CLIENT_APP)) {
			pr_err("send mdfd cmd: invalid handle (%d) appid(%d)\n",
					data->type, data->client.app_id);
			ret = -EINVAL;
			break;
		}
		/* Only one client allowed here at a time */
		mutex_lock(&app_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_send_modfd_cmd(data, argp);
		atomic_dec(&data->ioctl_count);
		wake_up_all(&data->abort_wq);
		mutex_unlock(&app_access_lock);
		if (ret)
			pr_err("failed qseecom_send_cmd: %d\n", ret);
		break;
	}
	case QSEECOM_IOCTL_RECEIVE_REQ: {
		if ((data->listener.id == 0) ||
			(data->type != QSEECOM_LISTENER_SERVICE)) {
			pr_err("receive req: invalid handle (%d), lid(%d)\n",
						data->type, data->listener.id);
			ret = -EINVAL;
			break;
		}
		atomic_inc(&data->ioctl_count);
		ret = qseecom_receive_req(data);
		atomic_dec(&data->ioctl_count);
		wake_up_all(&data->abort_wq);
		if (ret)
			pr_err("failed qseecom_receive_req: %d\n", ret);
		break;
	}
	case QSEECOM_IOCTL_SEND_RESP_REQ: {
		if ((data->listener.id == 0) ||
			(data->type != QSEECOM_LISTENER_SERVICE)) {
			pr_err("send resp req: invalid handle (%d), lid(%d)\n",
						data->type, data->listener.id);
			ret = -EINVAL;
			break;
		}
		atomic_inc(&data->ioctl_count);
		ret = qseecom_send_resp();
		atomic_dec(&data->ioctl_count);
		wake_up_all(&data->abort_wq);
		if (ret)
			pr_err("failed qseecom_send_resp: %d\n", ret);
		break;
	}
	case QSEECOM_IOCTL_SET_MEM_PARAM_REQ: {
		if ((data->type != QSEECOM_CLIENT_APP) &&
				(data->type != QSEECOM_GENERIC) &&
				(data->type != QSEECOM_SECURE_SERVICE)) {
			pr_err("set mem param req: invalid handle (%d)\n",
				data->type);
			ret = -EINVAL;
			break;
		}
		ret = qseecom_set_client_mem_param(data, argp);
		if (ret)
			pr_err("failed Qqseecom_set_mem_param request: %d\n",
								ret);
		break;
	}
	case QSEECOM_IOCTL_LOAD_APP_REQ: {
		if ((data->type != QSEECOM_GENERIC) &&
				(data->type != QSEECOM_CLIENT_APP)) {
			pr_err("load app req: invalid handle (%d)\n",
				data->type);
			ret = -EINVAL;
			break;
		}
		data->type = QSEECOM_CLIENT_APP;
		mutex_lock(&app_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_load_app(data, argp);
		atomic_dec(&data->ioctl_count);
		mutex_unlock(&app_access_lock);
		if (ret)
			pr_err("failed load_app request: %d\n", ret);
		break;
	}
	case QSEECOM_IOCTL_UNLOAD_APP_REQ: {
		if ((data->client.app_id == 0) ||
				(data->type != QSEECOM_CLIENT_APP)) {
			pr_err("unload app req:invalid handle(%d) app_id(%d)\n",
				data->type, data->client.app_id);
			ret = -EINVAL;
			break;
		}
		mutex_lock(&app_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_unload_app(data);
		atomic_dec(&data->ioctl_count);
		mutex_unlock(&app_access_lock);
		if (ret)
			pr_err("failed unload_app request: %d\n", ret);
		break;
	}
	case QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ: {
		atomic_inc(&data->ioctl_count);
		ret = qseecom_get_qseos_version(data, argp);
		if (ret)
			pr_err("qseecom_get_qseos_version: %d\n", ret);
		atomic_dec(&data->ioctl_count);
		break;
	}
	case QSEECOM_IOCTL_PERF_ENABLE_REQ:{
		if ((data->type != QSEECOM_GENERIC) &&
			(data->type != QSEECOM_CLIENT_APP)) {
			pr_err("perf enable req: invalid handle (%d)\n",
								data->type);
			ret = -EINVAL;
			break;
		}
		if ((data->type == QSEECOM_CLIENT_APP) &&
			(data->client.app_id == 0)) {
			pr_err("perf enable req:invalid handle(%d) appid(%d)\n",
					data->type, data->client.app_id);
			ret = -EINVAL;
			break;
		}
		atomic_inc(&data->ioctl_count);
		ret = qsee_vote_for_clock(CLK_DFAB);
		if (ret)
			pr_err("Failed to vote for DFAB clock%d\n", ret);
		ret = qsee_vote_for_clock(CLK_SFPB);
		if (ret)
			pr_err("Failed to vote for SFPB clock%d\n", ret);
		atomic_dec(&data->ioctl_count);
		break;
	}
	case QSEECOM_IOCTL_PERF_DISABLE_REQ:{
		if ((data->type != QSEECOM_SECURE_SERVICE) &&
				(data->type != QSEECOM_CLIENT_APP)) {
			pr_err("perf disable req: invalid handle (%d)\n",
				data->type);
			ret = -EINVAL;
			break;
		}
		if ((data->type == QSEECOM_CLIENT_APP) &&
				(data->client.app_id == 0)) {
			pr_err("perf disable: invalid handle (%d)app_id(%d)\n",
				data->type, data->client.app_id);
			ret = -EINVAL;
			break;
		}
		atomic_inc(&data->ioctl_count);
		qsee_disable_clock_vote(CLK_DFAB);
		qsee_disable_clock_vote(CLK_SFPB);
		atomic_dec(&data->ioctl_count);
		break;
	}
	case QSEECOM_IOCTL_LOAD_EXTERNAL_ELF_REQ: {
		if (data->type != QSEECOM_UNAVAILABLE_CLIENT_APP) {
			pr_err("unload ext elf req: invalid handle (%d)\n",
				data->type);
			ret = -EINVAL;
			break;
		}
		data->type = QSEECOM_UNAVAILABLE_CLIENT_APP;
		data->released = true;
		if (qseecom.qseos_version == QSEOS_VERSION_13) {
			pr_err("Loading External elf image unsupported in rev 0x13\n");
			ret = -EINVAL;
			break;
		}
		mutex_lock(&app_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_load_external_elf(data, argp);
		atomic_dec(&data->ioctl_count);
		mutex_unlock(&app_access_lock);
		if (ret)
			pr_err("failed load_external_elf request: %d\n", ret);
		break;
	}
	case QSEECOM_IOCTL_UNLOAD_EXTERNAL_ELF_REQ: {
		if (data->type != QSEECOM_UNAVAILABLE_CLIENT_APP) {
			pr_err("unload ext elf req: invalid handle (%d)\n",
								data->type);
			ret = -EINVAL;
			break;
		}
		data->released = true;
		if (qseecom.qseos_version == QSEOS_VERSION_13) {
			pr_err("Unloading External elf image unsupported in rev 0x13\n");
			ret = -EINVAL;
			break;
		}
		mutex_lock(&app_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_unload_external_elf(data);
		atomic_dec(&data->ioctl_count);
		mutex_unlock(&app_access_lock);
		if (ret)
			pr_err("failed unload_app request: %d\n", ret);
		break;
	}
	case QSEECOM_IOCTL_APP_LOADED_QUERY_REQ: {
		data->type = QSEECOM_CLIENT_APP;
		mutex_lock(&app_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_query_app_loaded(data, argp);
		atomic_dec(&data->ioctl_count);
		mutex_unlock(&app_access_lock);
		break;
	}
	default:
		pr_err("Invalid IOCTL: %d\n", cmd);
		return -EINVAL;
	}
	return ret;
}

static int qseecom_open(struct inode *inode, struct file *file)
{
	int ret = 0;
	struct qseecom_dev_handle *data;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		pr_err("kmalloc failed\n");
		return -ENOMEM;
	}
	file->private_data = data;
	data->abort = 0;
	data->type = QSEECOM_GENERIC;
	data->service = false;
	data->released = false;
	init_waitqueue_head(&data->abort_wq);
	atomic_set(&data->ioctl_count, 0);
	if (qseecom.qseos_version == QSEOS_VERSION_13) {
		int pil_error;
		mutex_lock(&pil_access_lock);
		if (pil_ref_cnt == 0) {
			pil = pil_get("tzapps");
			if (IS_ERR(pil)) {
				pr_err("Playready PIL image load failed\n");
				pil_error = PTR_ERR(pil);
				pil = NULL;
				pr_debug("tzapps image load FAILED\n");
				mutex_unlock(&pil_access_lock);
				return pil_error;
			}
		}
		pil_ref_cnt++;
		mutex_unlock(&pil_access_lock);
	}
	return ret;
}

static int qseecom_release(struct inode *inode, struct file *file)
{
	struct qseecom_dev_handle *data = file->private_data;
	int ret = 0;

	if (data->released == false) {
		pr_warn("data->released == false\n");
		if (data->service)
			ret = qseecom_unregister_listener(data);
		else
			ret = qseecom_unload_app(data);
		if (ret) {
			pr_err("Close failed\n");
			return ret;
		}
	}
	if (qseecom.qseos_version == QSEOS_VERSION_13) {
		mutex_lock(&pil_access_lock);
		if (pil_ref_cnt == 1)
			pil_put(pil);
		pil_ref_cnt--;
		mutex_unlock(&pil_access_lock);
	}
	kfree(data);

	return ret;
}

static const struct file_operations qseecom_fops = {
		.owner = THIS_MODULE,
		.unlocked_ioctl = qseecom_ioctl,
		.open = qseecom_open,
		.release = qseecom_release
};

static int __qseecom_init_clk()
{
	int rc = 0;
	struct device *pdev;

	pdev = qseecom.pdev;
	/* Get CE3 src core clk. */
	ce_core_src_clk = clk_get(pdev, "core_clk_src");
	if (!IS_ERR(ce_core_src_clk)) {
		ce_core_src_clk = ce_core_src_clk;

		/* Set the core src clk @100Mhz */
		rc = clk_set_rate(ce_core_src_clk, 100000000);
		if (rc) {
			clk_put(ce_core_src_clk);
			pr_err("Unable to set the core src clk @100Mhz.\n");
			goto err_clk;
		}
	} else {
		pr_warn("Unable to get CE core src clk, set to NULL\n");
		ce_core_src_clk = NULL;
	}

	/* Get CE core clk */
	ce_core_clk = clk_get(pdev, "core_clk");
	if (IS_ERR(ce_core_clk)) {
		rc = PTR_ERR(ce_core_clk);
		pr_err("Unable to get CE core clk\n");
		if (ce_core_src_clk != NULL)
			clk_put(ce_core_src_clk);
		goto err_clk;
	}

	/* Get CE Interface clk */
	ce_clk = clk_get(pdev, "iface_clk");
	if (IS_ERR(ce_clk)) {
		rc = PTR_ERR(ce_clk);
		pr_err("Unable to get CE interface clk\n");
		if (ce_core_src_clk != NULL)
			clk_put(ce_core_src_clk);
		clk_put(ce_core_clk);
		goto err_clk;
	}

	/* Get CE AXI clk */
	ce_bus_clk = clk_get(pdev, "bus_clk");
	if (IS_ERR(ce_bus_clk)) {
		rc = PTR_ERR(ce_bus_clk);
		pr_err("Unable to get CE BUS interface clk\n");
		if (ce_core_src_clk != NULL)
			clk_put(ce_core_src_clk);
		clk_put(ce_core_clk);
		clk_put(ce_clk);
		goto err_clk;
	}

	/* Enable CE core clk */
	rc = clk_prepare_enable(ce_core_clk);
	if (rc) {
		pr_err("Unable to enable/prepare CE core clk\n");
		if (ce_core_src_clk != NULL)
			clk_put(ce_core_src_clk);
		clk_put(ce_core_clk);
		clk_put(ce_clk);
		goto err_clk;
	} else {
		/* Enable CE clk */
		rc = clk_prepare_enable(ce_clk);
		if (rc) {
			pr_err("Unable to enable/prepare CE iface clk\n");
			clk_disable_unprepare(ce_core_clk);
			if (ce_core_src_clk != NULL)
				clk_put(ce_core_src_clk);
			clk_put(ce_core_clk);
			clk_put(ce_clk);
			goto err_clk;
		} else {
			/* Enable AXI clk */
			rc = clk_prepare_enable(ce_bus_clk);
			if (rc) {
				pr_err("Unable to enable/prepare CE iface clk\n");
				clk_disable_unprepare(ce_core_clk);
				clk_disable_unprepare(ce_clk);
				if (ce_core_src_clk != NULL)
					clk_put(ce_core_src_clk);
				clk_put(ce_core_clk);
				clk_put(ce_clk);
				goto err_clk;
			}
		}
	}
	return rc;

err_clk:
	if (rc)
		pr_err("Unable to init CE clks, rc = %d\n", rc);
	clk_disable_unprepare(ce_clk);
	clk_disable_unprepare(ce_core_clk);
	clk_disable_unprepare(ce_bus_clk);
	if (ce_core_src_clk != NULL)
		clk_put(ce_core_src_clk);
	clk_put(ce_clk);
	clk_put(ce_core_clk);
	clk_put(ce_bus_clk);
	return rc;
}



static void __qseecom_disable_clk()
{
	clk_disable_unprepare(ce_clk);
	clk_disable_unprepare(ce_core_clk);
	clk_disable_unprepare(ce_bus_clk);
	if (ce_core_src_clk != NULL)
		clk_put(ce_core_src_clk);
	clk_put(ce_clk);
	clk_put(ce_core_clk);
	clk_put(ce_bus_clk);
}

static int __devinit qseecom_probe(struct platform_device *pdev)
{
	int rc;
	int ret;
	struct device *class_dev;
	char qsee_not_legacy = 0;
	struct msm_bus_scale_pdata *qseecom_platform_support = NULL;
	uint32_t system_call_id = QSEOS_CHECK_VERSION_CMD;

	qsee_bw_count = 0;
	qsee_perf_client = 0;

	rc = alloc_chrdev_region(&qseecom_device_no, 0, 1, QSEECOM_DEV);
	if (rc < 0) {
		pr_err("alloc_chrdev_region failed %d\n", rc);
		return rc;
	}

	driver_class = class_create(THIS_MODULE, QSEECOM_DEV);
	if (IS_ERR(driver_class)) {
		rc = -ENOMEM;
		pr_err("class_create failed %d\n", rc);
		goto unregister_chrdev_region;
	}

	class_dev = device_create(driver_class, NULL, qseecom_device_no, NULL,
			QSEECOM_DEV);
	if (!class_dev) {
		pr_err("class_device_create failed %d\n", rc);
		rc = -ENOMEM;
		goto class_destroy;
	}

	cdev_init(&qseecom_cdev, &qseecom_fops);
	qseecom_cdev.owner = THIS_MODULE;

	rc = cdev_add(&qseecom_cdev, MKDEV(MAJOR(qseecom_device_no), 0), 1);
	if (rc < 0) {
		pr_err("cdev_add failed %d\n", rc);
		goto err;
	}

	INIT_LIST_HEAD(&qseecom.registered_listener_list_head);
	spin_lock_init(&qseecom.registered_listener_list_lock);
	INIT_LIST_HEAD(&qseecom.registered_app_list_head);
	spin_lock_init(&qseecom.registered_app_list_lock);
	INIT_LIST_HEAD(&qseecom.registered_kclient_list_head);
	spin_lock_init(&qseecom.registered_kclient_list_lock);
	init_waitqueue_head(&qseecom.send_resp_wq);
	qseecom.send_resp_flag = 0;

	rc = scm_call(6, 1, &system_call_id, sizeof(system_call_id),
				&qsee_not_legacy, sizeof(qsee_not_legacy));
	if (rc) {
		pr_err("Failed to retrieve QSEE version information %d\n", rc);
		goto err;
	}
	if (qsee_not_legacy)
		qseecom.qseos_version = QSEOS_VERSION_14;
	else {
		qseecom.qseos_version = QSEOS_VERSION_13;
		pil = NULL;
		pil_ref_cnt = 0;
	}

	qseecom.pdev = class_dev;
	/* Create ION msm client */
	qseecom.ion_clnt = msm_ion_client_create(-1, "qseecom-kernel");
	if (qseecom.ion_clnt == NULL) {
		pr_err("Ion client cannot be created\n");
		rc = -ENOMEM;
		goto err;
	}

	/* register client for bus scaling */
	if (pdev->dev.of_node) {
		ret = __qseecom_init_clk();
		if (ret)
			goto err;
		qseecom_platform_support = (struct msm_bus_scale_pdata *)
						msm_bus_cl_get_pdata(pdev);
	} else {
		qseecom_platform_support = (struct msm_bus_scale_pdata *)
						pdev->dev.platform_data;
	}

	qsee_perf_client = msm_bus_scale_register_client(
					qseecom_platform_support);

	if (!qsee_perf_client)
		pr_err("Unable to register bus client\n");
	return 0;
err:
	device_destroy(driver_class, qseecom_device_no);
class_destroy:
	class_destroy(driver_class);
unregister_chrdev_region:
	unregister_chrdev_region(qseecom_device_no, 1);
	return rc;
}

static int __devinit qseecom_remove(struct platform_device *pdev)
{
	struct qseecom_registered_kclient_list *kclient = NULL;
	unsigned long flags = 0;
	int ret = 0;

	if (pdev->dev.platform_data != NULL)
		msm_bus_scale_unregister_client(qsee_perf_client);

	spin_lock_irqsave(&qseecom.registered_kclient_list_lock, flags);
	kclient = list_entry((&qseecom.registered_kclient_list_head)->next,
		struct qseecom_registered_kclient_list, list);
	if (list_empty(&kclient->list)) {
		spin_unlock_irqrestore(&qseecom.registered_kclient_list_lock,
			flags);
		return 0;
	}
	list_for_each_entry(kclient, &qseecom.registered_kclient_list_head,
				list) {
			if (kclient)
				list_del(&kclient->list);
			break;
	}
	spin_unlock_irqrestore(&qseecom.registered_kclient_list_lock, flags);


	while (kclient->handle != NULL) {
		ret = qseecom_unload_app(kclient->handle->dev);
		if (ret == 0) {
			kzfree(kclient->handle->dev);
			kzfree(kclient->handle);
			kzfree(kclient);
		}
		spin_lock_irqsave(&qseecom.registered_kclient_list_lock, flags);
		kclient = list_entry(
				(&qseecom.registered_kclient_list_head)->next,
				struct qseecom_registered_kclient_list, list);
		if (list_empty(&kclient->list)) {
			spin_unlock_irqrestore(
				&qseecom.registered_kclient_list_lock, flags);
			return 0;
		}
		list_for_each_entry(kclient,
				&qseecom.registered_kclient_list_head, list) {
			if (kclient)
				list_del(&kclient->list);
			break;
		}
		spin_unlock_irqrestore(&qseecom.registered_kclient_list_lock,
				flags);
		if (!kclient) {
			ret = 0;
			break;
		}
	}
	return ret;
};

static struct of_device_id qseecom_match[] = {
	{
		.compatible = "qcom,qseecom",
	},
	{}
};

static struct platform_driver qseecom_plat_driver = {
	.probe = qseecom_probe,
	.remove = qseecom_remove,
	.driver = {
		.name = "qseecom",
		.owner = THIS_MODULE,
		.of_match_table = qseecom_match,
	},
};

static int __devinit qseecom_init(void)
{
	return platform_driver_register(&qseecom_plat_driver);
}

static void __devexit qseecom_exit(void)
{

	__qseecom_disable_clk();

	device_destroy(driver_class, qseecom_device_no);
	class_destroy(driver_class);
	unregister_chrdev_region(qseecom_device_no, 1);
	ion_client_destroy(qseecom.ion_clnt);
}

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm Secure Execution Environment Communicator");

module_init(qseecom_init);
module_exit(qseecom_exit);
