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
	{ 0x6459621a, "cdev_add" },
	{ 0xb6c08e4c, "class_create" },
	{ 0xf350d701, "device_create" },
	{ 0x12ad300e, "iounmap" },
	{ 0x0c92f06e, "cdev_del" },
	{ 0x0bc5fb0d, "unregister_chrdev_region" },
	{ 0xfd4b4a36, "class_destroy" },
	{ 0x7e2232fb, "ioread32" },
	{ 0xfad8f384, "iowrite32" },
	{ 0x30a11079, "device_destroy" },
	{ 0xbd03ed67, "__ref_stack_chk_guard" },
	{ 0x092a35a2, "_copy_from_user" },
	{ 0xa96d32ba, "__udelay" },
	{ 0x092a35a2, "_copy_to_user" },
	{ 0x23f25c0a, "__dynamic_pr_debug" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0xd272d446, "__fentry__" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0x97dd6ca9, "ioremap" },
	{ 0xe8213e80, "_printk" },
	{ 0x9f222e1e, "alloc_chrdev_region" },
	{ 0x4c075f7d, "cdev_init" },
	{ 0x814e12e5, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0x6459621a,
	0xb6c08e4c,
	0xf350d701,
	0x12ad300e,
	0x0c92f06e,
	0x0bc5fb0d,
	0xfd4b4a36,
	0x7e2232fb,
	0xfad8f384,
	0x30a11079,
	0xbd03ed67,
	0x092a35a2,
	0xa96d32ba,
	0x092a35a2,
	0x23f25c0a,
	0xd272d446,
	0xd272d446,
	0xd272d446,
	0x97dd6ca9,
	0xe8213e80,
	0x9f222e1e,
	0x4c075f7d,
	0x814e12e5,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"cdev_add\0"
	"class_create\0"
	"device_create\0"
	"iounmap\0"
	"cdev_del\0"
	"unregister_chrdev_region\0"
	"class_destroy\0"
	"ioread32\0"
	"iowrite32\0"
	"device_destroy\0"
	"__ref_stack_chk_guard\0"
	"_copy_from_user\0"
	"__udelay\0"
	"_copy_to_user\0"
	"__dynamic_pr_debug\0"
	"__stack_chk_fail\0"
	"__fentry__\0"
	"__x86_return_thunk\0"
	"ioremap\0"
	"_printk\0"
	"alloc_chrdev_region\0"
	"cdev_init\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "2C857AA9609C4FB0BFC4BFA");
