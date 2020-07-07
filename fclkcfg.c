/*********************************************************************************
 *
 *       Copyright (C) 2016-2020 Ichiro Kawazome
 *       All rights reserved.
 * 
 *       Redistribution and use in source and binary forms, with or without
 *       modification, are permitted provided that the following conditions
 *       are met:
 * 
 *         1. Redistributions of source code must retain the above copyright
 *            notice, this list of conditions and the following disclaimer.
 * 
 *         2. Redistributions in binary form must reproduce the above copyright
 *            notice, this list of conditions and the following disclaimer in
 *            the documentation and/or other materials provided with the
 *            distribution.
 * 
 *       THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *       "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *       LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *       A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 *       OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *       SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *       LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *       DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *       THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 *       (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *       OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 ********************************************************************************/
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/stat.h>
#include <linux/limits.h>
#include <linux/types.h>
#include <linux/file.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/version.h>

/**
 * DOC: fclkcfg constants 
 */

MODULE_DESCRIPTION("FPGA Clock Configuration Driver");
MODULE_AUTHOR("ikwzm");
MODULE_LICENSE("Dual BSD/GPL");

#define DRIVER_VERSION     "1.5.0-rc.1"
#define DRIVER_NAME        "fclkcfg"
#define DEVICE_MAX_NUM      32

#if     (LINUX_VERSION_CODE >= 0x030B00)
#define USE_DEV_GROUPS      1
#else
#define USE_DEV_GROUPS      0
#endif

/**
 * DOC: fclkcfg static variables
 *
 * * fclkcfg_sys_class  - fclkcfg system class.
 * * init_enable        - fclkcfg install/uninstall infomation enable.
 */

/**
 * fclkcfg_sys_class  - fclkcfg system class.
 */
static struct class*  fclkcfg_sys_class = NULL;

/**
 * info_enable        - fclkcfg install/uninstall infomation enable.
 */
static int            info_enable = 1;
module_param(         info_enable , int, S_IRUGO);
MODULE_PARM_DESC(     info_enable , DRIVER_NAME " install/uninstall infomation enable");

/**
 * DOC: fclk state structure
 *
 * This section defines the structure of fclk state.
 *
 * * struct fclk_state   - fclk state data structure.
 * * of_get_fclk_state() - get rate or enable property from device tree.
 *
 */

/**
 * struct fclk_state - fclk state data structure.
 */
struct fclk_state {
    unsigned long        rate;
    bool                 enable;
    bool                 rate_valid;
    bool                 enable_valid;
};

/**
 * of_get_fclk_state()  - get rate or enable property from device tree.
 * @dev:         handle to the device structure.
 * @rate_name:   rate property name
 * @enable_name: enable property name
 * @state:       address of fclk state data.
 * Return:       Success(=0) or error status(<0).
 */
static void of_get_fclk_state(struct device* dev, const char* rate_name, const char* enable_name, struct fclk_state* state)
{
    dev_dbg(dev, "get %s start.\n", rate_name);
    {
        const char* prop;

        prop = of_get_property(dev->of_node, rate_name, NULL);
        
        if (!IS_ERR_OR_NULL(prop)) {
            ssize_t       prop_status;
            unsigned long rate;
            if (0 != (prop_status = kstrtoul(prop, 0, &rate))) {
                dev_err(dev, "invalid %s.\n", rate_name);
            } else {
                state->rate_valid = true;
                state->rate       = rate;
            }
        }
    }
    dev_dbg(dev, "get %s done.\n" , rate_name);
    
    dev_dbg(dev, "get %s start.\n", enable_name);
    {
        int          retval;
        unsigned int enable;

        retval = of_property_read_u32(dev->of_node, enable_name, &enable);

        if (retval == 0) {
            state->enable_valid = true;
            state->enable       = (enable != 0);
        }
    }
    dev_dbg(dev, "get %s done.\n", enable_name);
}

/**
 * DOC: fclk device data structure
 *
 * This section defines the structure of fclk device data.
 *
 */
/**
 * struct fclk_device_data - fclk device data structure.
 */
struct fclk_device_data {
    struct device*       device;
    struct clk*          clk;
    struct clk*          resource_clk;
    dev_t                device_number;
    unsigned long        round_rate;
    struct fclk_state    insert;
    struct fclk_state    remove;
};

/**
 * DOC: fclk device clock operations
 *
 * This section defines the clock operation.
 *
 * * __fclk_set_enable()    - enable/disable clock.
 * * __fclk_set_rate()      - set clock rate.
 * * __fclk_change_state()  - change clock state.
 *
 */
/**
 * __fclk_set_enable() - enable/disable clock.
 *
 * @this:       Pointer to the fclk device data.
 * @enable:	enable/disable value.
 * Return:      Success(=0) or error status(<0).
 *
 */
static int __fclk_set_enable(struct fclk_device_data* this, bool enable)
{
    int status = 0;

    if (enable == true) {
        if (__clk_is_enabled(this->clk) == false) {
            status = clk_prepare_enable(this->clk);
            if (status) 
                dev_err(this->device, "enable failed.");
            else 
                dev_dbg(this->device, "enable success.");
        }
    } else {
        if (__clk_is_enabled(this->clk) == true) {
            clk_disable_unprepare(this->clk);
            dev_dbg(this->device, "disable done.");
        }
    }
    return status;
}

/**
 * __fclk_set_rate() - set clock rate.
 *
 * @this:       Pointer to the fclk device data.
 * @rate:       rate.
 * Return:      Success(=0) or error status(<0).
 *
 */
static int __fclk_set_rate(struct fclk_device_data* this, unsigned long rate)
{
    int           status;
    unsigned long round_rate;

    round_rate = clk_round_rate(this->clk, rate);
    status     = clk_set_rate(this->clk, round_rate);

    if (status)
        dev_err(this->device, "set_rate(%lu=>%lu) failed." , rate, round_rate);
    else
        dev_dbg(this->device, "set_rate(%lu=>%lu) success.", rate, round_rate);

    return status;
}

/**
 * __fclk_change_state() - change clock state.
 *
 * @this:       Pointer to the fclk device data.
 * @next:	next state to change.
 * Return:      Success(=0) or error status(<0).
 *
 */
static int __fclk_change_state(struct fclk_device_data* this, struct fclk_state* next)
{
    int  retval      = 0;
    bool prev_enable = __clk_is_enabled(this->clk);
    bool next_enable = (next->enable_valid == true) ? next->enable : prev_enable;

    if ((next->rate_valid == true) && (prev_enable == true)) {
        if (0 != (retval = __fclk_set_enable(this, false)))
            return retval;
        prev_enable = false;
    }
    if (next->rate_valid == true) {
        if (0 != (retval = __fclk_set_rate(this, next->rate)))
            return retval;
    }
    if (prev_enable != next_enable) {
        if (0 != (retval = __fclk_set_enable(this, next_enable)))
            return retval;
    }
    return retval;
}

/**
 * DOC: fclkcfg system class device file description
 *
 * This section define the device file created in system class when fclkcfg is 
 * loaded into the kernel.
 *
 * The device file created in system class is as follows.
 *
 * * /sys/class/udmabuf/<device-name>/driver_version
 * * /sys/class/udmabuf/<device-name>/enable
 * * /sys/class/udmabuf/<device-name>/rate
 * * /sys/class/udmabuf/<device-name>/round_rate
 * * 
 */

/**
 * fclk_show_driver_version()
 */
static ssize_t fclk_show_driver_version(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%s\n", DRIVER_VERSION);
}

/**
 * fclk_show_enable()
 */
static ssize_t fclk_show_enable(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct fclk_device_data* this = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", __clk_is_enabled(this->clk));
}

/**
 * fclk_set_enable()
 */
static ssize_t fclk_set_enable(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
    ssize_t       get_status = 0;
    int           set_status = 0;
    unsigned long enable;
    struct fclk_device_data* this = dev_get_drvdata(dev);

    if (0 != (get_status = kstrtoul(buf, 0, &enable)))
        return get_status;

    if (0 != (set_status = __fclk_set_enable(this, (enable != 0))))
        return (ssize_t)set_status;

    return size;
}

/**
 * fclk_show_rate()
 */
static ssize_t fclk_show_rate(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct fclk_device_data* this = dev_get_drvdata(dev);
    return sprintf(buf, "%lu\n", clk_get_rate(this->clk));
}

/**
 * fclk_set_rate()
 */
static ssize_t fclk_set_rate(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
    ssize_t                  get_result;
    int                      set_result;
    struct fclk_state        next_state;
    struct fclk_device_data* this = dev_get_drvdata(dev);

    if (0 != (get_result = kstrtoul(buf, 0, &next_state.rate)))
        return get_result;

    next_state.rate_valid   = true;
    next_state.enable       = false;
    next_state.enable_valid = false;

    if (0 != (set_result = __fclk_change_state(this, &next_state)))
        return (ssize_t)set_result;

    return size;
}

/**
 * fclk_show_round_rate()
 */
static ssize_t fclk_show_round_rate(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct fclk_device_data* this = dev_get_drvdata(dev);
    return sprintf(buf, "%lu => %lu\n",
                   this->round_rate,
                   clk_round_rate(this->clk, this->round_rate)
    );
}

/**
 * fclk_set_round_rate()
 */
static ssize_t fclk_set_round_rate(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
    ssize_t       status;
    unsigned long round_rate;
    struct fclk_device_data* this = dev_get_drvdata(dev);

    if (0 != (status = kstrtoul(buf, 0, &round_rate)))
        return status;
    this->round_rate = round_rate;
    return size;
}

static struct device_attribute fclkcfg_device_attrs[] = {
  __ATTR(driver_version , 0444, fclk_show_driver_version, NULL               ),
  __ATTR(enable         , 0664, fclk_show_enable        , fclk_set_enable    ),
  __ATTR(rate           , 0664, fclk_show_rate          , fclk_set_rate      ),
  __ATTR(round_rate     , 0664, fclk_show_round_rate    , fclk_set_round_rate),
  __ATTR_NULL,
};

#if (USE_DEV_GROUPS == 1)
static struct attribute *fclkcfg_attrs[] = {
  &(fclkcfg_device_attrs[0].attr),
  &(fclkcfg_device_attrs[1].attr),
  &(fclkcfg_device_attrs[2].attr),
  &(fclkcfg_device_attrs[3].attr),
  NULL
};
static struct attribute_group  fclkcfg_attr_group = {
  .attrs = fclkcfg_attrs
};
static const struct attribute_group* fclkcfg_attr_groups[] = {
  &fclkcfg_attr_group,
  NULL
};
#define SET_SYS_CLASS_ATTRIBUTES(sys_class) {(sys_class)->dev_groups = fclkcfg_attr_groups; }
#else
#define SET_SYS_CLASS_ATTRIBUTES(sys_class) {(sys_class)->dev_attrs  = fclkcfg_device_attrs;}
#endif

/**
 * DOC: fclk device data operations
 *
 * This section defines the operation of fclk device data.
 *
 * * fclk_device_number      - fclk device major number.
 * * fclk_device_ida         - fclk device minor number allocator variable.
 * * fclk_device_info()      - Print infomation the fclk device data.
 * * fclk_device_create()    - Create fclk device data.
 * * fclk_device_destroy()   - Destroy the fclk device data.
 */
static dev_t          fclk_device_number = 0;
static DEFINE_IDA(    fclk_device_ida );

/**
 * fclk_device_info() -  Print infomation the fclk device data.
 *
 * @this:       Pointer to the fclk device data.
 * @pdev:	handle to the platform device structure or NULL.
 *
 */
static void fclk_device_info(struct fclk_device_data* this, struct platform_device* pdev)
{
    struct device* dev = (pdev != NULL)? &pdev->dev : this->device;

    dev_info(dev, "driver version : %s\n" , DRIVER_VERSION);
    dev_info(dev, "device name    : %s\n" , dev_name(this->device));
    dev_info(dev, "clock  name    : %s\n" , __clk_get_name(this->clk));
    if (this->resource_clk != NULL)
        dev_info(dev, "resource clock : %s\n", __clk_get_name(this->resource_clk));
    dev_info(dev, "clock  rate    : %lu\n", clk_get_rate(this->clk));
    dev_info(dev, "clock  enabled : %d\n" , __clk_is_enabled(this->clk));
    if (this->remove.rate_valid == true)
        dev_info(dev, "remove rate    : %lu\n", this->remove.rate);
    if (this->remove.enable_valid == true)
        dev_info(dev, "remove enable  : %d\n" , this->remove.enable);
           
}

/**
 * fclk_device_destroy() - Destroy the fclk device data.
 *
 * @this:       Pointer to the fclk device data.
 * Return:      Success(=0) or error status(<0).
 *
 */
static int fclk_device_destroy(struct fclk_device_data* this)
{
    if (!this)
        return -ENODEV;

    if (this->clk          ) {
        clk_put(this->clk);
        this->clk = NULL;
    }
    if (this->resource_clk ) {
        clk_put(this->resource_clk);
        this->resource_clk = NULL;
    }
    if (this->device       ) {
        device_destroy(fclkcfg_sys_class, this->device_number);
        this->device = NULL;
    }
    if (this->device_number) {
        ida_simple_remove(&fclk_device_ida, MINOR(this->device_number));
        this->device_number = 0;
    }
    kfree(this);
    return 0;
}

/**
 * fclk_device_create() -  Create fclk device data.
 *
 * @dev:        handle to the device structure.
 * Return:      Pointer to the fclk device data or NULL.
 *
 */
static struct fclk_device_data* fclk_device_create(struct device *dev)
{
    int                      retval = 0;
    struct fclk_device_data* this   = NULL;
    const char*              device_name;

    dev_dbg(dev, "driver probe start.\n");
    /*
     * create (fclk_device_data*) this.
     */
    {
        this = kzalloc(sizeof(*this), GFP_KERNEL);
        if (IS_ERR_OR_NULL(this)) {
            retval = PTR_ERR(this);
            this   = NULL;
            goto failed;
        }
        this->device        = NULL;
        this->clk           = NULL;
        this->device_number = 0;
    }
    /*
     * get device number
     */
    dev_dbg(dev, "get device_number start.\n");
    {
        int minor_number = ida_simple_get(&fclk_device_ida, 0, DEVICE_MAX_NUM, GFP_KERNEL);
        if (minor_number < 0) {
            dev_err(dev, "invalid or conflict minor number %d.\n", minor_number);
            retval = -ENODEV;
            goto failed;
        }
        this->device_number = MKDEV(MAJOR(fclk_device_number), minor_number);
    }
    dev_dbg(dev, "get device_number done.\n");

    /*
     * get clk
     */
    dev_dbg(dev, "of_clk_get(0) start.\n");
    {
        this->clk = of_clk_get(dev->of_node, 0);
        if (IS_ERR_OR_NULL(this->clk)) {
            dev_err(dev, "of_clk_get(0) failed.\n");
            retval = PTR_ERR(this->clk);
            this->clk = NULL;
            goto failed;
        }
    }    
    dev_dbg(dev, "of_clk_get(0) done.\n");

    /*
     * get device name
     */
    dev_dbg(dev, "get device name start.\n");
    {
        device_name = of_get_property(dev->of_node, "device-name", NULL);
        
        if (IS_ERR_OR_NULL(device_name)) {
            device_name = dev_name(dev);
        }
    }
    dev_dbg(dev, "get device name done.\n");

    /*
     * create device
     */
    dev_dbg(dev, "device_create start.\n");
    {
        this->device = device_create(fclkcfg_sys_class,
                                     NULL,
                                     this->device_number,
                                     (void *)this,
                                     device_name);
        if (IS_ERR_OR_NULL(this->device)) {
            dev_err(dev, "device create falied.\n");
            this->device = NULL;
            goto failed;
        }
    }
    dev_dbg(dev, "device_create done\n");

    /*
     * get resource clock
     */
    dev_dbg(dev, "of_clk_get(1) start.\n");
    {
        this->resource_clk = of_clk_get(dev->of_node, 1);
        if (IS_ERR_OR_NULL(this->resource_clk)) {
            this->resource_clk = NULL;
        } else {
            int         set_parent_status  = 0;
            bool        found_resource_clk = false;
            struct clk* curr_clk           = this->clk;
            while (!IS_ERR_OR_NULL(curr_clk)) {
                if (clk_has_parent(curr_clk, this->resource_clk) == true) {
                    found_resource_clk = true;
                    set_parent_status  = clk_set_parent(curr_clk, this->resource_clk);
                    break;
                }
                curr_clk = clk_get_parent(curr_clk);
            }
            if (set_parent_status != 0) {
                dev_err(dev, "clk_set_parent(%s, %s) failed.\n" , __clk_get_name(curr_clk), __clk_get_name(this->resource_clk));
                goto failed;
            }
            if (found_resource_clk == false) {
                dev_err(dev, "%s is not resource clock of %s.\n", __clk_get_name(this->resource_clk), __clk_get_name(this->clk));
                goto failed;
            }
        }
    }
    dev_dbg(dev, "of_clk_get(1) done.\n");
    /*
     * get insert state
     */
    of_get_fclk_state(dev, "insert-rate", "insert-enable", &this->insert);
    
    /*
     * change state
     */
    __fclk_change_state(this, &this->insert);
    this->insert.enable = __clk_is_enabled(this->clk);
    this->insert.rate   = clk_get_rate(this->clk);

    /*
     * get remove state
     */
    of_get_fclk_state(dev, "remove-rate", "remove-enable", &this->remove);

    return this;

 failed:
    fclk_device_destroy(this);
    return ERR_PTR(retval);
}


/**
 * DOC: fclkcfg Platform Driver
 *
 * This section defines the fclkcfg platform driver.
 *
 * * fclkcfg_platform_driver_probe()   - Probe call for the device.
 * * fclkcfg_platform_driver_remove()  - Remove call for the device.
 * * fclkcfg_of_match                  - Open Firmware Device Identifier Matching Table.
 * * fclkcfg_platform_driver           - Platform Driver Structure.
 */

/**
 * fclkcfg_platform_driver_probe() -  Probe call for the device.
 *
 * @pdev:	handle to the platform device structure.
 * Returns 0 on success, negative error otherwise.
 *
 * It does all the memory allocation and registration for the device.
 */
static int fclkcfg_platform_driver_probe(struct platform_device *pdev)
{
    int                      retval = 0;
    struct fclk_device_data* data;

    data = fclk_device_create(&pdev->dev);
    if (IS_ERR_OR_NULL(data)) {
        retval = PTR_ERR(data);
        dev_err(&pdev->dev, "driver create failed. return=%d.\n", retval);
        retval = (retval == 0) ? -EINVAL : retval;
        goto failed;
    }

    platform_set_drvdata(pdev, data);

    if (info_enable) {
        fclk_device_info(data, pdev);
    }

    dev_info(&pdev->dev, "driver installed.\n");
    return 0;

 failed:
    dev_info(&pdev->dev, "driver install failed.\n");
    return retval;
}

/**
 * fclkcfg_platform_driver_remove() -  Remove call for the device.
 *
 * @pdev:	handle to the platform device structure.
 * Returns 0 or error status.
 *
 * Unregister the device after releasing the resources.
 */
static int fclkcfg_platform_driver_remove(struct platform_device *pdev)
{
    struct fclk_device_data* this = dev_get_drvdata(&pdev->dev);

    if (!this)
        return -ENODEV;

    if (this->clk) 
        __fclk_change_state(this, &this->remove);

    fclk_device_destroy(this);
    platform_set_drvdata(pdev, NULL);
    dev_info(&pdev->dev, "driver removed.\n");
    return 0;
}

/**
 * Open Firmware Device Identifier Matching Table
 */
static struct of_device_id fclkcfg_of_match[] = {
    { .compatible = "ikwzm,fclkcfg-0.10.a", },
    { /* end of table */}
};
MODULE_DEVICE_TABLE(of, fclkcfg_of_match);

/**
 * Platform Driver Structure
 */
static struct platform_driver fclkcfg_platform_driver = {
    .probe  = fclkcfg_platform_driver_probe,
    .remove = fclkcfg_platform_driver_remove,
    .driver = {
        .owner = THIS_MODULE,
        .name  = DRIVER_NAME,
        .of_match_table = fclkcfg_of_match,
    },
};
static bool fclkcfg_platform_driver_done = 0;

/**
 * DOC: fclkcfg kernel module operations
 *
 * * fclkcfg_module_cleanup()
 * * fclkcfg_module_init()
 * * fclkcfg_module_exit()
 */

/**
 * fclkcfg_module_cleanup()
 */
static void fclkcfg_module_cleanup(void)
{
    if (fclkcfg_platform_driver_done ){platform_driver_unregister(&fclkcfg_platform_driver);}
    if (fclkcfg_sys_class     != NULL){class_destroy(fclkcfg_sys_class);}
    if (fclk_device_number    != 0   ){unregister_chrdev_region(fclk_device_number, 0);}
    ida_destroy(&fclk_device_ida);
}

/**
 * fclk_module_exit()
 */
static void __exit fclkcfg_module_exit(void)
{
    fclkcfg_module_cleanup();
}

/**
 * fclkcfg_module_init()
 */
static int __init fclkcfg_module_init(void)
{
    int retval = 0;

    ida_init(&fclk_device_ida);

    retval = alloc_chrdev_region(&fclk_device_number, 0, 0, DRIVER_NAME);
    if (retval != 0) {
        printk(KERN_ERR "%s: couldn't allocate device major number\n", DRIVER_NAME);
        fclk_device_number = 0;
        goto failed;
    }

    fclkcfg_sys_class = class_create(THIS_MODULE, DRIVER_NAME);
    if (IS_ERR_OR_NULL(fclkcfg_sys_class)) {
        printk(KERN_ERR "%s: couldn't create sys class\n", DRIVER_NAME);
        retval = PTR_ERR(fclkcfg_sys_class);
        fclkcfg_sys_class = NULL;
        goto failed;
    }
    SET_SYS_CLASS_ATTRIBUTES(fclkcfg_sys_class);

    retval = platform_driver_register(&fclkcfg_platform_driver);
    if (retval) {
        printk(KERN_ERR "%s: couldn't register platform driver\n", DRIVER_NAME);
    } else {
        fclkcfg_platform_driver_done = 1;
    }
    return 0;

 failed:
    fclkcfg_module_cleanup();
    return retval;
}

module_init(fclkcfg_module_init);
module_exit(fclkcfg_module_exit);

