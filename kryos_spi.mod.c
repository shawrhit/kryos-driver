#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x92997ed8, "_printk" },
	{ 0xc1514a3b, "free_irq" },
	{ 0x9166fc03, "__flush_workqueue" },
	{ 0x8c03d20c, "destroy_workqueue" },
	{ 0x923a6430, "device_remove_file" },
	{ 0xdf484963, "device_destroy" },
	{ 0xe783e261, "sysfs_emit" },
	{ 0x59c02473, "class_create" },
	{ 0xf0b59e42, "__spi_register_driver" },
	{ 0x6775d5d3, "class_destroy" },
	{ 0x36a78de3, "devm_kmalloc" },
	{ 0x49cd25ed, "alloc_workqueue" },
	{ 0xb63fdcdb, "device_create" },
	{ 0x6e68f847, "device_create_file" },
	{ 0x92d5838e, "request_threaded_irq" },
	{ 0x92893115, "driver_unregister" },
	{ 0xdcb764ad, "memset" },
	{ 0x20b64901, "spi_sync" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0xb43f9365, "ktime_get" },
	{ 0xc5b6f236, "queue_work_on" },
	{ 0xf5edea2e, "___ratelimit" },
	{ 0xa65c6def, "alt_cb_patch_nops" },
	{ 0x474e54d2, "module_layout" },
};

MODULE_INFO(depends, "");

MODULE_ALIAS("spi:kryos-root-node");
MODULE_ALIAS("of:N*T*Crohit,kryos-root-node");
MODULE_ALIAS("of:N*T*Crohit,kryos-root-nodeC*");

MODULE_INFO(srcversion, "8E2C54CB02504EC70CC0489");
