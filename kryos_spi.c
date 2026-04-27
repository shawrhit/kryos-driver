#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/spi/spi.h>
#include <linux/mod_devicetable.h>
#include <linux/device.h>
#include <linux/sysfs.h>

/* Module metadata */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rohit K. Shaw");
MODULE_DESCRIPTION("KryOS SPI Kernel Driver");
MODULE_VERSION("1.0");

/* Global variables for sysfs hierarchy */
static struct class *kryos_class;
static struct device *kryos_device;


/* 1. The Open Firmware (DTS) Match Table */
static const struct of_device_id kryos_dt_ids[] = {
    { .compatible = "rohit,kryos-root-node", },
    { }
};
MODULE_DEVICE_TABLE(of, kryos_dt_ids);

/* SPI device table - Fallback for non-DTS systems */
static const struct spi_device_id kryos_spi_ids[] = {
    { "kryos-root-node", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, kryos_spi_ids);


static ssize_t telemetry_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "{\"round_id\":12345,\"ts\":\"2026-04-07T10:30:00\",\"temp_c\":4.125,"
                           "\"node_mask\":\"1111\",\"rejected_mask\":\"0000\",\"quorum_ok\":true,\"auth_ok\":true}\n");
}

/* This macro generates the 'dev_attr_telemetry' struct */
static DEVICE_ATTR_RO(telemetry);

static irqreturn_t kryos_irq_handler(int irq, void *dev_id)
{
    struct spi_device *spi = dev_id;

    pr_info_ratelimited("KryOS: IRQ %d received from %s\n", irq, dev_name(&spi->dev));
    return IRQ_HANDLED;
}

/* Called when kernel matches a SPI device to this driver */
static int kryos_probe(struct spi_device *spi)
{
    int ret;

    // Create the device folder: /sys/class/kryos/kryos_device
    kryos_device = device_create(kryos_class, &spi->dev, MKDEV(0, 0), NULL, "kryos_device");
    if (IS_ERR(kryos_device)) {
        pr_err("KryOS: failed to create device\n");
        return PTR_ERR(kryos_device);
    }

    // Create a file inside it: /sys/class/kryos/kryos_device/telemetry
    if (device_create_file(kryos_device, &dev_attr_telemetry) < 0) {
        pr_err("KryOS: failed to create sysfs file\n");
        device_destroy(kryos_class, MKDEV(0, 0));
        return -ENOMEM;
    }

    if (spi->irq <= 0) {
        pr_err("KryOS: no IRQ configured for SPI device\n");
        ret = spi->irq ? spi->irq : -ENXIO;
        goto err_remove_sysfs_file;
    }

    ret = request_irq(spi->irq, kryos_irq_handler, IRQF_TRIGGER_FALLING, "kryos", spi);
    if (ret) {
        pr_err("KryOS: failed to request IRQ %d: %d\n", spi->irq, ret);
        goto err_remove_sysfs_file;
    }

    pr_info("KryOS: SPI device probed, max speed=%d Hz, mode=%d, irq=%d\n",
            spi->max_speed_hz, spi->mode, spi->irq);
    return 0;

err_remove_sysfs_file:
    device_remove_file(kryos_device, &dev_attr_telemetry);
    device_destroy(kryos_class, MKDEV(0, 0));
    return ret;
}

/* Called when device is removed or driver unloaded */
static void kryos_remove(struct spi_device *spi)
{
    // Tear down in reverse order of creation
    if (spi->irq > 0)
        free_irq(spi->irq, spi);

    device_remove_file(kryos_device, &dev_attr_telemetry);
    device_destroy(kryos_class, MKDEV(0, 0));
    pr_info("KryOS: SPI device removed\n");
}

/* SPI driver registration struct */
static struct spi_driver kryos_spi_driver = {
    .driver = {
        .name = "kryos-root-node",
        .owner = THIS_MODULE,
        .of_match_table = kryos_dt_ids,
    },
    .id_table = kryos_spi_ids,
    .probe = kryos_probe,
    .remove = kryos_remove,
};

static int __init kryos_init(void)
{
    int ret;

    kryos_class = class_create("kryos");
    if (IS_ERR(kryos_class)) {
        pr_err("KryOS: failed to create class\n");
        return PTR_ERR(kryos_class);
    }

    ret = spi_register_driver(&kryos_spi_driver);
    if (ret) {
        pr_err("KryOS: failed to register SPI driver\n");
        class_destroy(kryos_class); // Clean up class if registration fails
        return ret;
    }
    
    pr_info("KryOS: SPI driver registered successfully\n");
    return 0;
}

static void __exit kryos_exit(void)
{
    spi_unregister_driver(&kryos_spi_driver);
    class_destroy(kryos_class); // Clean up class on exit
    pr_info("KryOS: driver unloaded\n");
}

module_init(kryos_init);
module_exit(kryos_exit);
