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
MODULE_VERSION("1.2");

/* ─── Global sysfs handles ───────────────────────────────────────────────── */
static struct class  *kryos_class;
static struct device *kryos_device;

/* ─── SPSC Ring Buffer ───────────────────────────────────────────────────── */
#define SPSC_SIZE 64        /* must be power of 2 */

struct kryos_sample {
    ktime_t timestamp;      /* kernel timestamp — captured in IRQ context */
};

struct kryos_spsc {
    struct kryos_sample buffer[SPSC_SIZE];
    unsigned int head;      /* written by IRQ handler only */
    unsigned int tail;      /* written by workqueue only   */
};

/* ─── Per-device State ───────────────────────────────────────────────────── */
/* ─── Payload Structure (15 bytes, little-endian) ──────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t round_id;
    uint32_t timestamp;
    uint32_t temp_raw;      /* temperature as IEEE 754 float (stored as raw bits) */
    uint8_t node_mask;
    uint8_t rejected_mask;
    uint8_t status_flags;
} payload_t;

/* ─── Per-device State ───────────────────────────────────────────────────── */
struct kryos_dev {
    struct spi_device       *spi;           /* back-pointer to SPI device  */
    struct kryos_spsc        rb;            /* SPSC ring buffer, embedded  */
    struct workqueue_struct *wq;            /* dedicated workqueue         */
    struct work_struct       work;          /* work item queued by IRQ     */
    atomic_t                 dropped;       /* dropped sample counter      */
    payload_t                last_payload;  /* last parsed payload         */
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

static ssize_t telemetry_show(struct device *dev,
                               struct device_attribute *attr,
                               char *buf)
{
    struct kryos_dev *kdev = dev_get_drvdata(dev->parent);
    uint32_t temp_raw;
    int temp_int, temp_frac;

    if (!kdev)
        return sysfs_emit(buf, "{\"error\":\"no device\"}\n");

    /*
     * Format last_payload as JSON telemetry.
     * temp_raw is IEEE 754 float stored as raw bits—no FP ops in kernel.
     * Decode in userspace or format as hex here.
     */
    temp_raw = kdev->last_payload.temp_raw;
    
    /* Simple approximation: treat raw as fixed-point for display */
    /* For proper IEEE 754 decoding, use userspace tools */
    temp_int = temp_raw >> 8;       /* rough scale, for demo only */
    temp_frac = (temp_raw & 0xFF) * 1000 / 256;
    
    return sysfs_emit(buf,
        "{\"round_id\":%u,"
        "\"timestamp\":%u,"
        "\"temp_raw\":\"0x%08x\","
        "\"node_mask\":\"0x%02x\","
        "\"rejected_mask\":\"0x%02x\","
        "\"status_flags\":\"0x%02x\","
        "\"dropped\":%d}\n",
        kdev->last_payload.round_id,
        kdev->last_payload.timestamp,
        kdev->last_payload.temp_raw,
        kdev->last_payload.node_mask,
        kdev->last_payload.rejected_mask,
        kdev->last_payload.status_flags,
        atomic_read(&kdev->dropped));
}
static DEVICE_ATTR_RO(telemetry);

/* ─── sysfs: dropped ─────────────────────────────────────────────────────── */
static ssize_t dropped_show(struct device *dev,
                             struct device_attribute *attr,
                             char *buf)
{
    /*
     * dev here is the sysfs device (kryos_device).
     * dev->parent is the SPI device which holds our kryos_dev via drvdata.
     */
    struct kryos_dev *kdev = dev_get_drvdata(dev->parent);

    if (!kdev)
        return sysfs_emit(buf, "-1\n");

    return sysfs_emit(buf, "%d\n", atomic_read(&kdev->dropped));
}
static DEVICE_ATTR_RO(dropped);

/* ─── Workqueue Handler (process context) ────────────────────────────────── */
static void kryos_wq_handler(struct work_struct *work)
{
    struct kryos_dev  *dev = container_of(work, struct kryos_dev, work);
    struct kryos_spsc *rb  = &dev->rb;
    u8                 rx_buf[15];
    int                ret;

    /*
     * Drain all pending slots from the ring buffer.
     * WQ owns tail — read directly.
     * IRQ owns head — always READ_ONCE.
     */
    while (rb->tail != READ_ONCE(rb->head)) {
        struct kryos_sample *sample = &rb->buffer[rb->tail];

        /*
         * SPI read happens here in process context — safe to sleep,
         * safe to use DMA, safe to take locks.
         * IRQ handler only timestamps the event — actual data
         * is fetched here after the interrupt signals readiness.
         */
        ret = spi_read(dev->spi, rx_buf, sizeof(rx_buf));
        if (ret) {
            pr_err_ratelimited("KryOS: spi_read failed: %d\n", ret);
        } else {
            /* Cast and parse 15-byte payload structure (little-endian) */
            payload_t *payload = (payload_t *)rx_buf;
            dev->last_payload = *payload;
            
            /* No FP ops in kernel — just log raw values */
            pr_info("KryOS: round_id=%u temp_raw=0x%08x status=0x%02x ts=%lld ns\n",
                    payload->round_id,
                    payload->temp_raw,
                    payload->status_flags,
                    ktime_to_ns(sample->timestamp));
        }

        /*
         * Advance tail — smp_store_release ensures sample is fully
         * consumed before IRQ sees the updated tail index.
         */
        smp_store_release(&rb->tail,
                          (rb->tail + 1) & (SPSC_SIZE - 1));
    }
}

/* ─── IRQ Handler (atomic context) ──────────────────────────────────────── */
static irqreturn_t kryos_irq_handler(int irq, void *dev_id)
{
    struct kryos_dev  *dev       = dev_id;
    struct kryos_spsc *rb        = &dev->rb;
    unsigned int       next_head = (rb->head + 1) & (SPSC_SIZE - 1);

    /*
     * Check buffer full.
     * READ_ONCE on tail — WQ owns tail and updates it concurrently.
     */
    if (next_head == READ_ONCE(rb->tail)) {
        atomic_inc(&dev->dropped);
        pr_warn_ratelimited("KryOS: ring buffer full, dropped=%d\n",
                            atomic_read(&dev->dropped));
        return IRQ_HANDLED;
    }

    /*
     * Capture timestamp only — NO spi_read here.
     * spi_read sleeps and uses DMA — strictly forbidden in IRQ context.
     * The actual SPI transfer is deferred to the workqueue handler.
     */
    rb->buffer[rb->head].timestamp = ktime_get();

    /*
     * Publish new head to consumer.
     * smp_store_release — WQ cannot see new head until timestamp is written.
     */
    smp_store_release(&rb->head, next_head);

    /* Kick workqueue to perform SPI read and process sample */
    queue_work(dev->wq, &dev->work);

    return IRQ_HANDLED;
}

/* ─── Probe ──────────────────────────────────────────────────────────────── */
static int kryos_probe(struct spi_device *spi)
{
    struct kryos_dev *dev;
    int ret;

    /* Allocate per-device state — devm frees on device removal */
    dev = devm_kzalloc(&spi->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        pr_err("KryOS: failed to allocate device state\n");
        return -ENOMEM;
    }

    /* Wire back-pointer, store dev for retrieval via spi_get_drvdata */
    dev->spi = spi;
    spi_set_drvdata(spi, dev);

    /* Initialise drop counter and work item */
    atomic_set(&dev->dropped, 0);
    INIT_WORK(&dev->work, kryos_wq_handler);

    /* Dedicated single-threaded workqueue for this device */
    dev->wq = alloc_workqueue("kryos_wq", WQ_UNBOUND, 1);
    if (!dev->wq) {
        pr_err("KryOS: failed to create workqueue\n");
        return -ENOMEM;
    }

    /*
     * Create sysfs device: /sys/class/kryos/kryos_device/
     * Pass dev as drvdata — retrieved in sysfs show functions
     * via dev_get_drvdata(dev->parent).
     */
    kryos_device = device_create(kryos_class, &spi->dev,
                                  MKDEV(0, 0), dev, "kryos_device");
    if (IS_ERR(kryos_device)) {
        pr_err("KryOS: failed to create sysfs device\n");
        ret = PTR_ERR(kryos_device);
        goto err_destroy_wq;
    }

    /* /sys/class/kryos/kryos_device/telemetry */
    ret = device_create_file(kryos_device, &dev_attr_telemetry);
    if (ret) {
        pr_err("KryOS: failed to create telemetry sysfs file\n");
        goto err_destroy_device;
    }

    /* /sys/class/kryos/kryos_device/dropped */
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
     * Pass dev — handler casts dev_id back to kryos_dev *.
     * Must match free_irq call in remove.
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

    /* Stop IRQ first — no new items queued after this */
    if (spi->irq > 0)
        free_irq(spi->irq, dev);

    /* Flush and destroy WQ — waits for any running work to complete */
    flush_workqueue(dev->wq);
    destroy_workqueue(dev->wq);

    /* Tear down sysfs in reverse order */
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
     * Class must exist before spi_register_driver —
     * probe runs immediately if matching device is already present.
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