#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>

#define REG_CONTROL 0x00
#define REG_STATUS  0x04
#define REG_DATA    0x08
#define REG_IRQ_ACK 0x0C

#define VPERIPH_IOC_MAGIC 'v'
#define VPERIPH_START _IO(VPERIPH_IOC_MAGIC, 0)

struct vperiph_dev {
    void __iomem *base;
	struct miscdevice miscdev;
	int irq;
};

struct vperiph_result {
    u32 data;
    u32 status;
};

static ssize_t vperiph_read(struct file *file,
                            char __user *buf,
                            size_t count,
                            loff_t *ppos)
{
    struct miscdevice *misc = file->private_data;
    struct vperiph_dev *vdev =
        container_of(misc, struct vperiph_dev, miscdev);

    struct vperiph_result result;

    if (count < sizeof(result))
        return -EINVAL;

    result.data = readl(vdev->base + REG_DATA);
    result.status = readl(vdev->base + REG_STATUS);

    if (copy_to_user(buf, &result, sizeof(result)))
        return -EFAULT;

    return sizeof(result);
}

static ssize_t vperiph_write(struct file *file,
                             const char __user *buf,
                             size_t count,
                             loff_t *ppos)
{
    struct miscdevice *misc = file->private_data;
    struct vperiph_dev *vdev =
        container_of(misc, struct vperiph_dev, miscdev);

    u32 value;

    if (count < sizeof(value))
        return -EINVAL;

    if (copy_from_user(&value, buf, sizeof(value)))
        return -EFAULT;

    writel(value, vdev->base + REG_DATA);

    return sizeof(value);
}

static long vperiph_ioctl(struct file *file,
                          unsigned int cmd,
                          unsigned long arg)
{
    struct miscdevice *misc = file->private_data;
    struct vperiph_dev *vdev =
        container_of(misc, struct vperiph_dev, miscdev);

    switch (cmd) {
    case VPERIPH_START:
        writel(1, vdev->base + REG_CONTROL);
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations vperiph_fops = {
    .owner = THIS_MODULE,
	.read = vperiph_read,
    .write = vperiph_write,
	.unlocked_ioctl = vperiph_ioctl,
};

static irqreturn_t vperiph_irq_handler(int irq, void *dev_id)
{
    struct vperiph_dev *vdev = dev_id;

    dev_info(vdev->miscdev.parent, "vperiph interrupt received\n");
	
	writel(1, vdev->base + REG_IRQ_ACK);

    return IRQ_HANDLED;
}

static int vperiph_probe(struct platform_device *pdev)
{
    struct vperiph_dev *vdev;
	int ret;

    dev_info(&pdev->dev, "vperiph probe called\n");

    vdev = devm_kzalloc(&pdev->dev, sizeof(*vdev), GFP_KERNEL);
    if (!vdev)
        return -ENOMEM;

    vdev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vdev->base))
		return PTR_ERR(vdev->base);

	vdev->irq = platform_get_irq(pdev, 0);
	if (vdev->irq < 0)
    	return vdev->irq;

	ret = devm_request_irq(&pdev->dev,
						   vdev->irq,
	                       vperiph_irq_handler,
	                       0,
	                       "vperiph",
	                       vdev);
	if (ret)
	    return ret;

    platform_set_drvdata(pdev, vdev);

	vdev->miscdev.minor = MISC_DYNAMIC_MINOR;
    vdev->miscdev.name = "vperiph";
    vdev->miscdev.fops = &vperiph_fops;
    vdev->miscdev.parent = &pdev->dev;

    ret = misc_register(&vdev->miscdev);
    if (ret)
        return ret;

    dev_info(&pdev->dev, "registered /dev/vperiph\n");

    return 0;
}

static const struct of_device_id vperiph_of_match[] = {
    { .compatible = "qemu,vperiph" },
    { }
};

MODULE_DEVICE_TABLE(of, vperiph_of_match);

static struct platform_driver vperiph_driver = {
    .probe = vperiph_probe,

    .driver = {
        .name = "vperiph",
        .of_match_table = vperiph_of_match,
    },
};

module_platform_driver(vperiph_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("whxodus0121");
MODULE_DESCRIPTION("QEMU vperiph platform driver");
