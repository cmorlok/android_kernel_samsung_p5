/*
 * Copyright (C) 2012 Samsung Electronics.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define DEBUG

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/irq.h>
#include <linux/poll.h>
#include <linux/gpio.h>
#include <linux/if_arp.h>
#include <linux/platform_device.h>
#include <linux/suspend.h>

#include "modem_link_pm_usb.h"

bool link_pm_is_connected(struct usb_link_device *usb_ld)
{
	if (has_hub(usb_ld)) {
		if (usb_ld->link_pm_data->hub_init_lock)
			return false;

		if (usb_ld->link_pm_data->hub_status != HUB_STATE_ACTIVE) {
			pr_debug("hub_status=%d\n",
					usb_ld->link_pm_data->hub_status);
			schedule_delayed_work(
					&usb_ld->link_pm_data->link_pm_hub, 0);
			return false;
		}
	}

	if (!usb_ld->if_usb_connected) {
		pr_err("mif: if not connected\n");
		return false;
	}

	return true;
}

static void link_pm_hub_work(struct work_struct *work)
{
	int err;
	struct link_pm_data *pm_data =
		container_of(work, struct link_pm_data, link_pm_hub.work);

	if (pm_data->hub_status == HUB_STATE_ACTIVE)
		return;

	if (!pm_data->port_enable) {
		pr_err("mif: %s: hub power func not assinged\n", __func__);
		return;
	}
	wake_lock(&pm_data->hub_lock);

	/* If kernel if suspend, wait the ehci resume */
	if (pm_data->dpm_suspending) {
		pr_info("mif: %s: dpm_suspending\n", __func__);
		schedule_delayed_work(&pm_data->link_pm_hub,
						msecs_to_jiffies(500));
		goto exit;
	}

	switch (pm_data->hub_status) {
	case HUB_STATE_OFF:
		pm_data->hub_status = HUB_STATE_RESUMMING;
		pr_info("mif: %s: hub off->on\n", __func__);

		/* skip 1st time before first probe */
		if (pm_data->root_hub)
			pm_runtime_get_sync(pm_data->root_hub);
		err = pm_data->port_enable(2, 1);
		if (err < 0) {
			pr_err("mif: %s: hub on fail err=%d\n", __func__, err);
			err = pm_data->port_enable(2, 0);
			if (err < 0)
				pr_err("mif: %s: hub off fail err=%d\n",
						__func__, err);
			pm_data->hub_status = HUB_STATE_OFF;
			if (pm_data->root_hub)
				pm_runtime_put_sync(pm_data->root_hub);
			goto exit;
		}
		/* resume root hub */
		schedule_delayed_work(&pm_data->link_pm_hub,
						msecs_to_jiffies(100));
		break;
	case HUB_STATE_RESUMMING:
		if (pm_data->hub_on_retry_cnt++ > 50) {
			pm_data->hub_on_retry_cnt = 0;
			pm_data->hub_status = HUB_STATE_OFF;
			if (pm_data->root_hub)
				pm_runtime_put_sync(pm_data->root_hub);
		}
		pr_info("mif: %s: hub resumming\n", __func__);
		schedule_delayed_work(&pm_data->link_pm_hub,
						msecs_to_jiffies(200));
		break;
	case HUB_STATE_PREACTIVE:
		pm_data->hub_status = HUB_STATE_ACTIVE;
		pr_info("hub active\n");
		pm_data->hub_on_retry_cnt = 0;
		wake_unlock(&pm_data->hub_lock);
		complete(&pm_data->hub_active);
		if (pm_data->root_hub)
			pm_runtime_put_sync(pm_data->root_hub);
		break;
	}
exit:
	return;
}

static int link_pm_hub_standby(struct link_pm_data *pm_data)
{
	struct usb_link_device *usb_ld = pm_data->usb_ld;
	int err = 0;

	pr_info("mif: %s: wait hub standby\n", __func__);

	if (!pm_data->port_enable) {
		pr_err("mif: %s: hub power func not assinged\n", __func__);
		return -ENODEV;
	}

	err = pm_data->port_enable(2, 0);
	if (err < 0)
		pr_err("mif: %s: hub off fail err=%d\n", __func__, err);

	pm_data->hub_status = HUB_STATE_OFF;

	return err;
}

bool link_pm_set_active(struct usb_link_device *usb_ld)
{
	int ret;
	struct link_pm_data *pm_data = usb_ld->link_pm_data;
	struct device *dev;

	if (has_hub(usb_ld)) {
		if (pm_data->hub_status != HUB_STATE_ACTIVE) {
			INIT_COMPLETION(pm_data->hub_active);
			SET_SLAVE_WAKEUP(usb_ld->pdata, 1);
			ret = wait_for_completion_timeout(&pm_data->hub_active,
				msecs_to_jiffies(2000));
			if (!ret) { /*timeout*/
				pr_err("%s: hub on timeout - retry\n",
						__func__);
				SET_SLAVE_WAKEUP(usb_ld->pdata, 0);
				queue_delayed_work(usb_ld->ld.tx_wq,
						&usb_ld->ld.tx_delayed_work, 0);
				return false;
			} else {
				pr_err("wait done\n");
				usb_make_resume(usb_ld);
				return true;
			}
		}
	} else {
		/* TODO do something */
	}
	return true;
}

static long link_pm_ioctl(struct file *file, unsigned int cmd,
						unsigned long arg)
{
	int value, err = 0;
	struct task_struct *task = get_current();
	struct link_pm_data *pm_data = file->private_data;
	struct usb_link_device *usb_ld = pm_data->usb_ld;
	char taskname[TASK_COMM_LEN];

	pr_info("mif: %s: 0x%08x\n", __func__, cmd);

	switch (cmd) {
	case IOCTL_LINK_CONTROL_ACTIVE:
		if (copy_from_user(&value, (const void __user *)arg,
							sizeof(int)))
			return -EFAULT;
		gpio_set_value(pm_data->gpio_link_active, value);
		break;
	case IOCTL_LINK_GET_HOSTWAKE:
		return !gpio_get_value(pm_data->gpio_link_hostwake);
	case IOCTL_LINK_CONNECTED:
		return pm_data->usb_ld->if_usb_connected;
	case IOCTL_LINK_PORT_ON: /* hub only */
		/* ignore cp host wakeup irq, set the hub_init_lock when AP try
		 CP off and release hub_init_lock when CP boot done */
		pm_data->hub_init_lock = 0;
		if (pm_data->root_hub) {
			pm_runtime_resume(pm_data->root_hub);
			pm_runtime_forbid(pm_data->root_hub->parent);
		}
		if (pm_data->port_enable) {
			err = pm_data->port_enable(2, 1);
			if (err < 0) {
				pr_err("mif: %s: hub on fail err=%d\n",
						__func__, err);
				goto exit;
			}
			pm_data->hub_status = HUB_STATE_RESUMMING;
		}
		break;
	case IOCTL_LINK_PORT_OFF: /* hub only */
		if (pm_data->usb_ld->if_usb_connected) {
			struct usb_device *udev =
					pm_data->usb_ld->usbdev->parent;
			pm_runtime_get_sync(&udev->dev);
			if (udev->state != USB_STATE_NOTATTACHED) {
				usb_force_disconnect(udev);
				pr_info("force disconnect maybe cp-reset!!\n");
			}
			pm_runtime_put_autosuspend(&udev->dev);
		}
		err = link_pm_hub_standby(pm_data);
		if (err < 0) {
			pr_err("mif: %s: usb3503 active fail\n", __func__);
			goto exit;
		}
		pm_data->hub_init_lock = 1;
		pm_data->hub_handshake_done = 0;

		break;
	case IOCTL_LINK_BLOCK_AUTOSUSPEND: /* block autosuspend forever */
		mif_info("blocked autosuspend by `%s(%d)'\n",
				get_task_comm(taskname, task), task->pid);
		pm_data->block_autosuspend = true;
		if (usb_ld->usbdev)
			pm_runtime_forbid(&usb_ld->usbdev->dev);
		else {
			mif_err("Block autosuspend failed\n");
			err = -ENODEV;
		}
		break;
	case IOCTL_LINK_ENABLE_AUTOSUSPEND: /* Enable autosuspend */
		mif_info("autosuspend enabled by `%s(%d)'\n",
		get_task_comm(taskname, task), task->pid);
		pm_data->block_autosuspend = false;
		if (usb_ld->usbdev)
			pm_runtime_allow(&usb_ld->usbdev->dev);
		else {
			mif_err("Enable autosuspend failed\n");
			err = -ENODEV;
		}
		break;
	default:
		break;
	}
exit:
	return err;
}

static int link_pm_open(struct inode *inode, struct file *file)
{
	struct link_pm_data *pm_data =
		(struct link_pm_data *)file->private_data;
	file->private_data = (void *)pm_data;
	return 0;
}

static int link_pm_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static const struct file_operations link_pm_fops = {
	.owner = THIS_MODULE,
	.open = link_pm_open,
	.release = link_pm_release,
	.unlocked_ioctl = link_pm_ioctl,
};

static int link_pm_notifier_event(struct notifier_block *this,
				unsigned long event, void *ptr)
{
	struct link_pm_data *pm_data =
			container_of(this, struct link_pm_data,	pm_notifier);

	switch (event) {
	case PM_SUSPEND_PREPARE:
		pm_data->dpm_suspending = true;
		link_pm_hub_standby(pm_data);
		return NOTIFY_OK;
	case PM_POST_SUSPEND:
		pm_data->dpm_suspending = false;
		return NOTIFY_OK;
	}
	return NOTIFY_DONE;
}

int link_pm_init(struct usb_link_device *usb_ld, void *data)
{
	int err;
	int irq;
	struct platform_device *pdev = (struct platform_device *)data;
	struct modem_data *pdata =
			(struct modem_data *)pdev->dev.platform_data;
	struct modemlink_pm_data *pm_pdata = pdata->link_pm_data;
	struct link_pm_data *pm_data =
			kzalloc(sizeof(struct link_pm_data), GFP_KERNEL);
	if (!pm_data) {
		pr_err("%s: link_pm_data is NULL\n", __func__);
		return -ENOMEM;
	}
	/* get link pm data from modemcontrol's platform data */
	pm_data->gpio_link_active = pm_pdata->gpio_link_active;
	pm_data->gpio_link_hostwake = pm_pdata->gpio_link_hostwake;
	pm_data->gpio_link_slavewake = pm_pdata->gpio_link_slavewake;
	pm_data->link_reconnect = pm_pdata->link_reconnect;
	pm_data->port_enable = pm_pdata->port_enable;
	pm_data->cpufreq_lock = pm_pdata->cpufreq_lock;
	pm_data->cpufreq_unlock = pm_pdata->cpufreq_unlock;
	pm_data->autosuspend_delay_ms = pm_pdata->autosuspend_delay_ms;
	pm_data->block_autosuspend = false;

	pm_data->usb_ld = usb_ld;
	pm_data->link_pm_active = false;
	usb_ld->link_pm_data = pm_data;

	pm_data->miscdev.minor = MISC_DYNAMIC_MINOR;
	pm_data->miscdev.name = "link_pm";
	pm_data->miscdev.fops = &link_pm_fops;

	err = misc_register(&pm_data->miscdev);
	if (err < 0) {
		pr_err("mif: %s: fail to register pm device(%d)\n",
				__func__, err);
		goto err_misc_register;
	}

	pm_data->hub_init_lock = 1;
	irq = gpio_to_irq(usb_ld->pdata->gpio_host_wakeup);
	err = request_threaded_irq(irq, NULL, usb_resume_irq,
		IRQF_TRIGGER_HIGH | IRQF_ONESHOT, "modem_usb_wake", usb_ld);
	if (err) {
		pr_err("mif: %s: Failed to allocate an interrupt(%d)\n",
				__func__, irq);
		goto err_request_irq;
	}
	enable_irq_wake(irq);

	pm_data->has_usbhub = pm_pdata->has_usbhub;

	if (has_hub(usb_ld)) {
		init_completion(&pm_data->hub_active);
		pm_data->hub_status = HUB_STATE_OFF;
		pm_pdata->p_hub_status = &pm_data->hub_status;
		pm_data->hub_handshake_done = 0;
		pm_data->root_hub = NULL;
		wake_lock_init(&pm_data->hub_lock, WAKE_LOCK_SUSPEND,
				"modem_hub_enum_lock");
		INIT_DELAYED_WORK(&pm_data->link_pm_hub, link_pm_hub_work);
	}

	pm_data->pm_notifier.notifier_call = link_pm_notifier_event;
	register_pm_notifier(&pm_data->pm_notifier);

	return 0;

err_request_irq:
	misc_deregister(&pm_data->miscdev);
err_misc_register:
	kfree(pm_data);
	return err;
}
