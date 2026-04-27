#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/spi/spi.h>
#include <linux/mod_devicetable.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/ktime.h>

/* ─── Module Metadata ────────────────────────────────────────────────────── */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rohit K. Shaw");
MODULE_DESCRIPTION("KryOS SPI Kernel Driver");
MODULE_VERSION("1.1");

/* ─── Global sysfs handles ───────────────────────────────────────────────── */
static struct class  *kryos_class;
static struct device *kryos_device;

/* ─── SPSC Ring Buffer ───────────────────────────────────────────────────── */
#define SPSC_SIZE 64        /* must be power of 2 */

struct kryos_sample {
    u32     raw;            /* raw sensor reading          */
    ktime_t timestamp;      /* kernel timestamp of capture */
};

struct kryos_spsc {
    struct kryos_sample buffer[SPSC_SIZE];
    unsigned int head;      /* written by IRQ handler only */
    unsigned int tail;      /* written by workqueue only   */
};

/* ─── Per-device State ───────────────────────────────────────────────────── */
struct kryos_dev {
    struct spi_device       *spi;       /* back-pointer to SPI device   */
    struct kryos_spsc        rb;        /* SPSC ring buffer, embedded   */
    struct workqueue_struct *wq;        /* dedicated workqueue          */
    struct work_struct       work;      /* work item queued by IRQ      */
    atomic_t                 dropped;   /* dropped sample counter       */
};

/* ─── Device Tree Match Table ────────────────────────────────────────────── */
static const struct of_device_id kryos_dt_ids[] = {
    { .compatible = "rohit,kryos-root-node", },
    { }
};
MODULE_DEVICE_TABLE(of, kryos_dt_ids);

/* ─── SPI Device Table (non-DTS fallback) ────────────────────────────────── */
static const struct spi_device_id kryos_spi_ids[] = {
    { "kryos-root-node", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, kryos_spi_ids);

/* ─── sysfs: telemetry ───────────────────────────────────────────────────── */
static ssize_t telemetry_show(struct device *dev,
                               struct device_attribute *attr,
                               char *buf)
{
    /*
     * Placeholder — real implementation will read latest processed
     * sample from kryos_dev and format it as JSON telemetry.
     */
    return sysfs_emit(buf,
        "{\"round_id\":12345,\"ts\":\"2026-04-07T10:30:00\","
        "\"temp_c\":4.125,\"node_mask\":\"1111\","
        "\"rejected_mask\":\"0000\",\"quorum_ok\":true,"
        "\"auth_ok\":true}\n");
}
static DEVICE_ATTR_RO(telemetry);

/* ─── sysfs: dropped ─────────────────────────────────────────────────────── */
static ssize_t dropped_show(struct device *dev,
                             struct device_attribute *attr,
                             char *buf)
{
    struct kryos_dev *kdev = dev_get_drvdata(dev);
    return sysfs_emit(buf, "%d\n", atomic_read(&kdev->dropped));
}
static DEVICE_ATTR_RO(dropped);

/* ─── Workqueue Handler (process context) ────────────────────────────────── */
static void kryos_wq_handler(struct work_struct *work)
{
    struct kryos_dev  *dev = container_of(work, struct kryos_dev, work);
    struct kryos_spsc *rb  = &dev->rb;

    /*
     * WQ owns tail — read directly, no READ_ONCE needed.
     * IRQ owns head — always READ_ONCE to prevent compiler caching.
     */
    while (rb->tail != READ_ONCE(rb->head)) {
        struct kryos_sample *sample = &rb->buffer[rb->tail];

        pr_info("KryOS: sample raw=0x%08x ts=%lld\n",
                sample->raw,
                ktime_to_ns(sample->timestamp));

        /*
         * Ensure sample is fully consumed before advancing tail.
         * smp_store_release acts as a write barrier — IRQ sees
         * updated tail only after we are done with the sample.
         */
        smp_store_release(&rb->tail,
                          (rb->tail + 1) & (SPSC_SIZE - 1));
    }
}

/* ─── IRQ Handler (atomic context) ──────────────────────────────────────── */
static irqreturn_t kryos_irq_handler(int irq, void *dev_id)
{
    struct kryos_dev  *dev      = dev_id;
    struct kryos_spsc *rb       = &dev->rb;
    unsigned int       next_head = (rb->head + 1) & (SPSC_SIZE - 1);

    /*
     * Check if buffer is full.
     * READ_ONCE on tail — WQ owns tail and may update it concurrently.
     */
    if (next_head == READ_ONCE(rb->tail)) {
        atomic_inc(&dev->dropped);
        pr_warn_ratelimited("KryOS: ring buffer full, dropped=%d\n",
                            atomic_read(&dev->dropped));
        return IRQ_HANDLED;
    }

    /*
     * Write sample into the slot at current head.
     * Placeholder raw value — replace with real spi_sync() read.
     */
    rb->buffer[rb->head].raw       = 0xDEADBEEF;
    rb->buffer[rb->head].timestamp = ktime_get();

    /*
     * Publish the new head to the consumer.
     * smp_store_release ensures sample is fully written before
     * head is updated — WQ cannot see the new head until data is ready.
     */
    smp_store_release(&rb->head, next_head);

    /* Kick the workqueue to consume the new sample */
    queue_work(dev->wq, &dev->work);

    pr_info_ratelimited("KryOS: IRQ %d fired, head=%u\n", irq, rb->head);

    return IRQ_HANDLED;
}

/* ─── Probe ──────────────────────────────────────────────────────────────── */
static int kryos_probe(struct spi_device *spi)
{
    struct kryos_dev *dev;
    int ret;

    /* Allocate per-device state — devm handles free on device removal */
    dev = devm_kzalloc(&spi->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        pr_err("KryOS: failed to allocate device state\n");
        return -ENOMEM;
    }

    /* Wire up back-pointer and store dev in spi_device for retrieval */
    dev->spi = spi;
    spi_set_drvdata(spi, dev);

    /* Initialise drop counter and work item */
    atomic_set(&dev->dropped, 0);
    INIT_WORK(&dev->work, kryos_wq_handler);

    /* Create a dedicated single-threaded workqueue for this device */
    dev->wq = alloc_workqueue("kryos_wq", WQ_UNBOUND, 1);
    if (!dev->wq) {
        pr_err("KryOS: failed to create workqueue\n");
        return -ENOMEM;
    }

    /* Create sysfs device: /sys/class/kryos/kryos_device/ */
    kryos_device = device_create(kryos_class, &spi->dev,
                                  MKDEV(0, 0), dev, "kryos_device");
    if (IS_ERR(kryos_device)) {
        pr_err("KryOS: failed to create sysfs device\n");
        ret = PTR_ERR(kryos_device);
        goto err_destroy_wq;
    }

    /* Create /sys/class/kryos/kryos_device/telemetry */
    ret = device_create_file(kryos_device, &dev_attr_telemetry);
    if (ret) {
        pr_err("KryOS: failed to create telemetry sysfs file\n");
        goto err_destroy_device;
    }

    /* Create /sys/class/kryos/kryos_device/dropped */
    ret = device_create_file(kryos_device, &dev_attr_dropped);
    if (ret) {
        pr_err("KryOS: failed to create dropped sysfs file\n");
        goto err_remove_telemetry;
    }

    /* Validate IRQ from Device Tree */
    if (spi->irq <= 0) {
        pr_err("KryOS: no IRQ configured in DTS\n");
        ret = spi->irq ? spi->irq : -ENXIO;
        goto err_remove_dropped;
    }

    /*
     * Register IRQ handler.
     * Pass dev not spi — handler casts dev_id back to kryos_dev *.
     */
    ret = request_irq(spi->irq, kryos_irq_handler,
                      IRQF_TRIGGER_FALLING, "kryos", dev);
    if (ret) {
        pr_err("KryOS: failed to request IRQ %d: %d\n", spi->irq, ret);
        goto err_remove_dropped;
    }

    pr_info("KryOS: probed — speed=%dHz mode=%d irq=%d\n",
            spi->max_speed_hz, spi->mode, spi->irq);
    return 0;

    /* Unwind in reverse order of creation */
err_remove_dropped:
    device_remove_file(kryos_device, &dev_attr_dropped);
err_remove_telemetry:
    device_remove_file(kryos_device, &dev_attr_telemetry);
err_destroy_device:
    device_destroy(kryos_class, MKDEV(0, 0));
err_destroy_wq:
    destroy_workqueue(dev->wq);
    return ret;
}

/* ─── Remove ─────────────────────────────────────────────────────────────── */
static void kryos_remove(struct spi_device *spi)
{
    struct kryos_dev *dev = spi_get_drvdata(spi);

    /* Stop IRQ first — no new work after this */
    if (spi->irq > 0)
        free_irq(spi->irq, dev);

    /* Drain and destroy workqueue — waits for running work to finish */
    flush_workqueue(dev->wq);
    destroy_workqueue(dev->wq);

    /* Tear down sysfs */
    device_remove_file(kryos_device, &dev_attr_dropped);
    device_remove_file(kryos_device, &dev_attr_telemetry);
    device_destroy(kryos_class, MKDEV(0, 0));

    pr_info("KryOS: removed\n");
}

/* ─── SPI Driver Struct ──────────────────────────────────────────────────── */
static struct spi_driver kryos_spi_driver = {
    .driver = {
        .name           = "kryos-root-node",
        .owner          = THIS_MODULE,
        .of_match_table = kryos_dt_ids,
    },
    .id_table = kryos_spi_ids,
    .probe    = kryos_probe,
    .remove   = kryos_remove,
};

/* ─── Init ───────────────────────────────────────────────────────────────── */
static int __init kryos_init(void)
{
    int ret;

    /*
     * Create class first — probe runs immediately on spi_register_driver
     * if a matching device exists, so class must exist before registration.
     */
    kryos_class = class_create("kryos");
    if (IS_ERR(kryos_class)) {
        pr_err("KryOS: failed to create class\n");
        return PTR_ERR(kryos_class);
    }

    ret = spi_register_driver(&kryos_spi_driver);
    if (ret) {
        pr_err("KryOS: failed to register SPI driver\n");
        class_destroy(kryos_class);
        return ret;
    }

    pr_info("KryOS: driver loaded\n");
    return 0;
}

/* ─── Exit ───────────────────────────────────────────────────────────────── */
static void __exit kryos_exit(void)
{
    spi_unregister_driver(&kryos_spi_driver);
    class_destroy(kryos_class);
    pr_info("KryOS: driver unloaded\n");
}

module_init(kryos_init);
module_exit(kryos_exit);