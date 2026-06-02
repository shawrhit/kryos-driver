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
#include <crypto/hash.h>

/* ─── Module Metadata ────────────────────────────────────────────────────── */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rohit K. Shaw");
MODULE_DESCRIPTION("KryOS SPI Kernel Driver");
MODULE_VERSION("1.3");

/* ─── Global sysfs handles ───────────────────────────────────────────────── */
static struct class  *kryos_class;
static struct device *kryos_device;

/* Master PSK for SPI bridge HMAC-SHA256 (32 bytes) */
/* Matches KRYOS_MASTER_PSK in kryos_config.h */
static const u8 KRYOS_MASTER_PSK[32] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
};

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

/* ─── Payload Structure (64 bytes, little-endian) ──────────────────────── */
/* Matches consensus_payload_t in firmware protocol.h */
typedef struct __attribute__((packed)) {
    uint32_t round_id;
    uint32_t timestamp;
    uint32_t temp_raw;      /* temperature as IEEE 754 float bits */
    uint8_t node_mask;
    uint8_t rejected_mask;
    uint8_t status_flags;
    uint8_t _pad[1];
    uint8_t hmac[32];
    uint8_t _pad2[16];
} payload_t;

/* ─── Per-device State ───────────────────────────────────────────────────── */
struct kryos_dev {
    struct spi_device       *spi;           /* back-pointer to SPI device  */
    struct kryos_spsc        rb;            /* SPSC ring buffer, embedded  */
    struct workqueue_struct *wq;            /* dedicated workqueue         */
    struct work_struct       work;          /* work item queued by IRQ     */
    atomic_t                 dropped;       /* dropped sample counter      */
    payload_t                last_payload;  /* last parsed payload         */
    struct crypto_shash     *tfm;           /* HMAC-SHA256 transform       */
    atomic_t                 auth_fail;     /* auth failure counter        */
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

    if (!kdev)
        return sysfs_emit(buf, "{\"error\":\"no device\"}\n");

    return sysfs_emit(buf,
        "{\"round_id\":%u,"
        "\"timestamp\":%u,"
        "\"temp_raw\":\"0x%08x BIT-FLOAT\","
        "\"node_mask\":\"0x%02x\","
        "\"rejected_mask\":\"0x%02x\","
        "\"status_flags\":\"0x%02x\","
        "\"dropped\":%d,"
        "\"auth_fail\":%d}\n",
        kdev->last_payload.round_id,
        kdev->last_payload.timestamp,
        kdev->last_payload.temp_raw,
        kdev->last_payload.node_mask,
        kdev->last_payload.rejected_mask,
        kdev->last_payload.status_flags,
        atomic_read(&kdev->dropped),
        atomic_read(&kdev->auth_fail));
}
static DEVICE_ATTR_RO(telemetry);

/* ─── sysfs: dropped ─────────────────────────────────────────────────────── */
static ssize_t dropped_show(struct device *dev,
                             struct device_attribute *attr,
                             char *buf)
{
    struct kryos_dev *kdev = dev_get_drvdata(dev->parent);
    if (!kdev) return sysfs_emit(buf, "-1\n");
    return sysfs_emit(buf, "%d\n", atomic_read(&kdev->dropped));
}
static DEVICE_ATTR_RO(dropped);

/* ─── Workqueue Handler (process context) ────────────────────────────────── */
static void kryos_wq_handler(struct work_struct *work)
{
    struct kryos_dev    *dev = container_of(work, struct kryos_dev, work);
    struct kryos_spsc   *rb  = &dev->rb;
    u8                   rx_buf[64]; // MUST BE 64 BYTES
    u8                   calculated_hmac[32];
    struct shash_desc   *desc;
    int                  ret;

    desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(dev->tfm), GFP_KERNEL);
    if (!desc) {
        pr_err("KryOS: failed to allocate shash descriptor\n");
        return;
    }
    desc->tfm = dev->tfm;

    while (rb->tail != READ_ONCE(rb->head)) {
        struct kryos_sample *sample = &rb->buffer[rb->tail];

        /* Fetch exactly 64 bytes from ESP32 SPI Slave */
        ret = spi_read(dev->spi, rx_buf, 64);
        if (ret) {
            pr_err_ratelimited("KryOS: spi_read failed: %d\n", ret);
        } else {
            payload_t *payload = (payload_t *)rx_buf;

            /* Verify HMAC-SHA256 over fields before hmac array */
            ret = crypto_shash_digest(desc, rx_buf, offsetof(payload_t, hmac), calculated_hmac);
            if (ret) {
                pr_err("KryOS: HMAC computation failed: %d\n", ret);
            } else if (memcmp(calculated_hmac, payload->hmac, 32) != 0) {
                atomic_inc(&dev->auth_fail);
                pr_warn_ratelimited("KryOS: HMAC AUTH FAIL round_id=%u | RAW: %02x %02x %02x %02x %02x %02x %02x %02x\n", 
                                   payload->round_id,
                                   rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3],
                                   rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7]);
            } else {
                dev->last_payload = *payload;
                if (payload->round_id != 0) {
                    pr_info("KryOS: Round %u AUTH_OK | Temp: 0x%08x | IRQ TS: %lld ns\n",
                            payload->round_id,
                            payload->temp_raw,
                            ktime_to_ns(sample->timestamp));
                }
            }
        }

        smp_store_release(&rb->tail, (rb->tail + 1) & (SPSC_SIZE - 1));
    }

    kfree(desc);
}

/* ─── IRQ Handler (atomic context) ──────────────────────────────────────── */
static irqreturn_t kryos_irq_handler(int irq, void *dev_id)
{
    struct kryos_dev  *dev       = dev_id;
    struct kryos_spsc *rb        = &dev->rb;
    unsigned int       next_head = (rb->head + 1) & (SPSC_SIZE - 1);

    if (next_head == READ_ONCE(rb->tail)) {
        atomic_inc(&dev->dropped);
        return IRQ_HANDLED;
    }

    rb->buffer[rb->head].timestamp = ktime_get();
    smp_store_release(&rb->head, next_head);
    queue_work(dev->wq, &dev->work);
    return IRQ_HANDLED;
}

/* ─── Probe ──────────────────────────────────────────────────────────────── */
static int kryos_probe(struct spi_device *spi)
{
    struct kryos_dev *dev;
    int ret;

    dev = devm_kzalloc(&spi->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev) return -ENOMEM;

    dev->tfm = crypto_alloc_shash("hmac(sha256)", 0, 0);
    if (IS_ERR(dev->tfm)) return PTR_ERR(dev->tfm);

    ret = crypto_shash_setkey(dev->tfm, KRYOS_MASTER_PSK, sizeof(KRYOS_MASTER_PSK));
    if (ret) goto err_free_tfm;

    dev->spi = spi;
    spi_set_drvdata(spi, dev);
    atomic_set(&dev->dropped, 0);
    atomic_set(&dev->auth_fail, 0);
    INIT_WORK(&dev->work, kryos_wq_handler);

    dev->wq = alloc_workqueue("kryos_wq", WQ_UNBOUND, 1);
    if (!dev->wq) { ret = -ENOMEM; goto err_free_tfm; }

    kryos_device = device_create(kryos_class, &spi->dev, MKDEV(0, 0), dev, "kryos_device");
    if (IS_ERR(kryos_device)) { ret = PTR_ERR(kryos_device); goto err_destroy_wq; }

    ret = device_create_file(kryos_device, &dev_attr_telemetry);
    if (ret) goto err_destroy_device;

    ret = device_create_file(kryos_device, &dev_attr_dropped);
    if (ret) goto err_remove_telemetry;

    if (spi->irq <= 0) { ret = -ENXIO; goto err_remove_dropped; }

    ret = request_irq(spi->irq, kryos_irq_handler, IRQF_TRIGGER_FALLING, "kryos", dev);
    if (ret) goto err_remove_dropped;

    pr_info("KryOS Driver: probed on IRQ %d, speed %dHz\n", spi->irq, spi->max_speed_hz);
    return 0;

err_remove_dropped:
    device_remove_file(kryos_device, &dev_attr_dropped);
err_remove_telemetry:
    device_remove_file(kryos_device, &dev_attr_telemetry);
err_destroy_device:
    device_destroy(kryos_class, MKDEV(0, 0));
err_destroy_wq:
    destroy_workqueue(dev->wq);
err_free_tfm:
    crypto_free_shash(dev->tfm);
    return ret;
}

static void kryos_remove(struct spi_device *spi)
{
    struct kryos_dev *dev = spi_get_drvdata(spi);
    if (spi->irq > 0) free_irq(spi->irq, dev);
    flush_workqueue(dev->wq);
    destroy_workqueue(dev->wq);
    device_remove_file(kryos_device, &dev_attr_dropped);
    device_remove_file(kryos_device, &dev_attr_telemetry);
    device_destroy(kryos_class, MKDEV(0, 0));
    crypto_free_shash(dev->tfm);
}

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

static int __init kryos_init(void)
{
    kryos_class = class_create("kryos");
    if (IS_ERR(kryos_class)) return PTR_ERR(kryos_class);
    return spi_register_driver(&kryos_spi_driver);
}

static void __exit kryos_exit(void)
{
    spi_unregister_driver(&kryos_spi_driver);
    class_destroy(kryos_class);
}

module_init(kryos_init);
module_exit(kryos_exit);
