#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>

#define REG_CONTROL 0x00
#define REG_STATUS  0x04
#define REG_DATA    0x08

struct vperiph_dev {
    void __iomem *base;
};

static int vperiph_probe(struct platform_device *pdev)
{
    struct vperiph_dev *vdev;
    u32 data;
    u32 status;

    dev_info(&pdev->dev, "vperiph probe called\n");

    vdev = devm_kzalloc(&pdev->dev, sizeof(*vdev), GFP_KERNEL);
    if (!vdev)
        return -ENOMEM;

    vdev->base = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(vdev->base))
        return PTR_ERR(vdev->base);

    platform_set_drvdata(pdev, vdev);

    writel(10, vdev->base + REG_DATA);
    writel(1, vdev->base + REG_CONTROL);

    data = readl(vdev->base + REG_DATA);
    status = readl(vdev->base + REG_STATUS);

    dev_info(&pdev->dev,
             "DATA=%u STATUS=%u\n",
             data, status);

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
