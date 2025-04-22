#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/of_gpio.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/stat.h>
#include <linux/device.h>
#include <linux/tty.h>
#include <linux/kmod.h>
#include <linux/gfp.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>

enum Num {
	// derg_mesh_en = 0,
	// derg_wifi_en,
	derg_ble_en = 0,
	derg_ble_conn ,
	derg_mesh_burn
};

#define DERG_GPIO_NUM 3
#define GPIO_MAGIC 'g'
#define DERG_BLE_EN	_IOW(GPIO_MAGIC, 0x02, unsigned long)
// #define DERG_MESH_EN	_IOW(GPIO_MAGIC, 0x03, unsigned long)
// #define DERG_WIFI_EN	_IOW(GPIO_MAGIC, 0x04, unsigned long)
#define DERG_BLE_CONN   _IOR(GPIO_MAGIC, 0x05, unsigned long)
#define DERG_MESH_BURN	_IOW(GPIO_MAGIC, 0x06, unsigned long)

static struct work_struct gpio_work;
static struct semaphore gpio_sem;

static char *gpio_name[3] = {
	//"derg-mesh-en-gpios", "derg-wifi-en-gpios",
	"derg-ble-en-gpios", "derg-ble-conn-gpios", "derg-mesh-burn-gpios"
};

static int gpio_num[DERG_GPIO_NUM];

static const struct of_device_id derg_gpio_of_match[] = {
	{ .compatible = "allwinner,sunxi-t113-derg", },
	{ },
};

static long gpio_drv_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	down(&gpio_sem);

	if (GPIO_MAGIC != _IOC_TYPE(cmd)) {
		up(&gpio_sem);
		return -EINVAL;
	}

	switch (cmd) {
	case DERG_BLE_EN:
		if (gpio_num[derg_ble_en] == -1) {
			pr_err("%s not defined in dts\n", gpio_name[derg_ble_en]);
			goto error;
		}

		if (arg != 0 && arg != 1) {
			pr_err("DERG_BLE_EN arg error\n");
			goto error;
		}

		gpio_set_value(gpio_num[derg_ble_en], arg);
		break;
	/*
	case DERG_MESH_EN:
		if (gpio_num[derg_mesh_en] == -1) {
			pr_err("%s not defined in dts\n", gpio_name[derg_mesh_en]);
			goto error;
		}

		if (arg != 0 && arg != 1) {
			pr_err("DERG_MESH_EN arg error\n");
			goto error;
		}

		gpio_set_value(gpio_num[derg_mesh_en], arg);
		break;

	case DERG_WIFI_EN:
		if (gpio_num[derg_wifi_en] == -1) {
			pr_err("%s not defined in dts\n", gpio_name[derg_wifi_en]);
			goto error;
		}

		if (arg != 0 && arg != 1) {
			pr_err("DERG_WIFI_EN arg error\n");
			goto error;
		}

		gpio_set_value(gpio_num[derg_wifi_en], !arg);
		break;
	*/
	case DERG_BLE_CONN:
	{
		int err, val;
		if (gpio_num[derg_ble_conn] == -1) {
			pr_err("%s not defined in dts\n", gpio_name[derg_ble_conn]);
			goto error;
		}
		val = gpio_get_value(gpio_num[derg_ble_conn]);
		err = copy_to_user((int __user *)arg, &val, sizeof(int));
		if (err)
			return -EFAULT;
	}
		break;

	case DERG_MESH_BURN:
		if (gpio_num[derg_mesh_burn] == -1) {
			pr_err("%s not defined in dts\n", gpio_name[derg_mesh_burn]);
			goto error;
		}

		if (arg != 0 && arg != 1) {
			pr_err("DERG_MESH_BURN arg error\n");
			goto error;
		}

		gpio_set_value(gpio_num[derg_mesh_burn], !arg);
		break;

	default:
		pr_err("error cmd\n");
		goto error;
	}

	up(&gpio_sem);
	return 0;

error:
	up(&gpio_sem);
	return -1;
}

static void gpio_work_func(struct work_struct *work)
{
	/* derg gpio init */
	/*
	if (gpio_num[derg_wifi_en] != -1) {
		gpio_direction_output(gpio_num[derg_wifi_en], 0);
		msleep(500);
	}

	if (gpio_num[derg_mesh_en] != -1) {
		gpio_direction_output(gpio_num[derg_mesh_en], 1);
	}
	*/

	if (gpio_num[derg_ble_en] != -1) {
		gpio_direction_output(gpio_num[derg_ble_en], 1);
		msleep(50);
		gpio_set_value(gpio_num[derg_ble_en], 0);
		msleep(300);
		gpio_set_value(gpio_num[derg_ble_en], 1);
		msleep(500);
	}

	if (gpio_num[derg_mesh_burn] != -1) {
		gpio_direction_output(gpio_num[derg_mesh_burn], 1);
	}

	if (gpio_num[derg_ble_conn] != -1) {
		gpio_direction_input(gpio_num[derg_ble_conn]);
	}
}

static const struct file_operations gpio_ops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= gpio_drv_ioctl,
};

static struct miscdevice derg_gpio_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "derg-gpio",
	.fops  = &gpio_ops,
};

static int derg_gpio_probe(struct platform_device *pdev)
{
	int ret;

	printk("%s %s line %d\n", __FILE__, __FUNCTION__, __LINE__);
	if (!pdev->dev.of_node) {
		return -1;
	}

	sema_init(&gpio_sem, 1);

	for (int i = 0; i < DERG_GPIO_NUM; i++) {
		gpio_num[i] = of_get_named_gpio(pdev->dev.of_node, gpio_name[i], 0);
		if (!gpio_is_valid(gpio_num[i])) {
			pr_info("%s not defined in dts\n", gpio_name[i]);
			gpio_num[i] = -1;
			continue;
		}

		ret = gpio_request(gpio_num[i], NULL);
		if (ret) {
			pr_err("%s request gpio failed\n", gpio_name[i]);
			return -1;
		}
	}

	INIT_WORK(&gpio_work, gpio_work_func);
	schedule_work(&gpio_work);

	ret = misc_register(&derg_gpio_dev);
	if (ret) {
		pr_err("misc_register failed\n");
		for (int i = 0; i < DERG_GPIO_NUM; i++) {
			if (gpio_num[i] != -1) {
				gpio_free(gpio_num[i]);
			}
		}
	}

	return ret;
}

static int derg_gpio_remove(struct platform_device *pdev)
{
	misc_deregister(&derg_gpio_dev);

	for (int i = 0; i < DERG_GPIO_NUM; i++) {
		if (gpio_num[i] != -1) {
			gpio_free(gpio_num[i]);
		}
	}

	return 0;
}

static struct platform_driver derg_gpio_driver = {
	.probe		= derg_gpio_probe,
	.remove		= derg_gpio_remove,
	.driver		= {
		.name	= "derg_gpio_drv",
		.of_match_table = of_match_ptr(derg_gpio_of_match),
	}
};

static int __init derg_gpio_init(void)
{
	int ret;

	ret = platform_driver_register(&derg_gpio_driver);
	if (ret < 0) {
		pr_err("fastrpc: failed to register cb driver\n");
	}

	return ret;
}

static void __exit derg_gpio_exit(void)
{
	platform_driver_unregister(&derg_gpio_driver);
}

module_init(derg_gpio_init);
module_exit(derg_gpio_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SKX");
