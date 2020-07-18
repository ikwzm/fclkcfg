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

#define DRIVER_VERSION     "1.6.1-rc.1"
#define DRIVER_NAME        "fclkcfg"
#define DEVICE_MAX_NUM      32

#if     (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0))
#define USE_DEV_GROUPS      1
#else
#define USE_DEV_GROUPS      0
#endif

/**
 * DOC: fclkcfg static variables
 *
 * * info_enable      - fclkcfg install/uninstall infomation enable.
 * * debug_print      - fclkcfg debug print enable.
 */

/**
 * info_enable        - fclkcfg install/uninstall infomation enable.
 */
static int            info_enable = 1;
module_param(         info_enable , int, S_IRUGO);
MODULE_PARM_DESC(     info_enable , DRIVER_NAME " install/uninstall infomation enable");

/**
 * debug_print        - fclkcfg debug print enable.
 */
static int            debug_print = 0;
module_param(         debug_print , int, S_IRUGO);
MODULE_PARM_DESC(     debug_print , DRIVER_NAME " debug print enable");

#define DEV_DBG(dev, fmt, ...) {\
    if (debug_print){dev_info(dev, fmt, ##__VA_ARGS__);} \
    else            {dev_dbg (dev, fmt, ##__VA_ARGS__);} \
}

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
    unsigned long        resclk;
    bool                 rate_valid;
    bool                 enable_valid;
    bool                 resclk_valid;
};

/**
 * of_get_fclk_state()  - get rate/enable/resource property from device tree.
 *
 * @dev:         handle to the device structure.
 * @rate_name:   rate property name
 * @enable_name: enable property name
 * @resclk_name: resource clock property name
 * @state:       address of fclk state data.
 * Return:       Success(=0) or error status(<0).
 */
static void of_get_fclk_state(struct device* dev, const char* rate_name, const char* enable_name, const char* resclk_name, struct fclk_state* state)
{
    DEV_DBG(dev, "get %s start.\n", rate_name);
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
    DEV_DBG(dev, "get %s done.\n" , rate_name);
    
    DEV_DBG(dev, "get %s start.\n", enable_name);
    {
        int          retval;
        unsigned int enable;

        retval = of_property_read_u32(dev->of_node, enable_name, &enable);

        if (retval == 0) {
            state->enable_valid = true;
            state->enable       = (enable != 0);
        }
    }
    DEV_DBG(dev, "get %s done.\n", enable_name);

    DEV_DBG(dev, "get %s start.\n", resclk_name);
    {
        int          retval;
        unsigned int resclk;

        retval = of_property_read_u32(dev->of_node, resclk_name, &resclk);

        if (retval == 0) {
            state->resclk_valid = true;
            state->resclk       = resclk;
        }
    }
    DEV_DBG(dev, "get %s done.\n", resclk_name);
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
    struct clk**         resource_clks;
    int                  resource_clks_size;
    int                  resource_clk_id;
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
 * * __fclk_set_enable()       - enable/disable clock.
 * * __fclk_set_rate()         - set clock rate.
 * * __fclk_change_state()     - change clock state.
 * * __fclk_change_resource()  - change resource clock.
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
                DEV_DBG(this->device, "enable success.");
        }
    } else {
        if (__clk_is_enabled(this->clk) == true) {
            clk_disable_unprepare(this->clk);
            DEV_DBG(this->device, "disable done.");
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
        DEV_DBG(this->device, "set_rate(%lu=>%lu) success.", rate, round_rate);

    return status;
}

/**
 * __fclk_change_resource() - change resource clock.
 *
 * @this:       Pointer to the fclk device data.
 * @index:	index of resource_clks.
 * Return:      Success(=0) or error status(<0).
 *
 */
static int __fclk_change_resource(struct fclk_device_data* this, int index)
{
    struct device* dev = this-> device;

    if ((this->resource_clks == NULL) && index == 0) {
        this->resource_clk_id = index;
        return 0;
    }

    if ((this->resource_clks != NULL) && (index >= 0) && (index < this->resource_clks_size)) {
        struct clk*    resource_clk       = this->resource_clks[index];
        int            set_parent_status  = 0;
        bool           found_resource_clk = false;
        struct clk*    curr_clk           = this->clk;
        while (!IS_ERR_OR_NULL(curr_clk)) {
            if (clk_has_parent(curr_clk, resource_clk) == true) {
                found_resource_clk = true;
                set_parent_status  = clk_set_parent(curr_clk, resource_clk);
                break;
            }
            curr_clk = clk_get_parent(curr_clk);
        }
        if (set_parent_status != 0) {
            dev_err(dev, "clk_set_parent(%s, %s) failed.\n" , __clk_get_name(curr_clk), __clk_get_name(resource_clk));
            return set_parent_status;
        }
        if (found_resource_clk == false) {
            dev_err(dev, "%s is not resource clock of %s.\n", __clk_get_name(resource_clk), __clk_get_name(this->clk));
            return -EINVAL;
        }
        this->resource_clk_id = index;
        return 0;
    }
    return -EINVAL;
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
    bool next_resclk = ((next->resclk_valid == true) &&
                        (next->resclk != this->resource_clk_id));

    if (((next->rate_valid == true) || (next_resclk == true)) && (prev_enable == true)) {
        if (0 != (retval = __fclk_set_enable(this, false)))
            return retval;
        prev_enable = false;
    }
    if (next_resclk == true) {
        if (0 != (retval = __fclk_change_resource(this, next->resclk)))
            return retval;
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
 * * /sys/class/udmabuf/<device-name>/resource_clks
 * * /sys/class/udmabuf/<device-name>/resource
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
    ssize_t                  get_result;
    int                      set_result;
    unsigned long            enable;
    struct fclk_device_data* this = dev_get_drvdata(dev);

    if (0 != (get_result = kstrtoul(buf, 0, &enable)))
        return get_result;

    if (0 != (set_result = __fclk_set_enable(this, (enable != 0))))
        return (ssize_t)set_result;

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
    next_state.resclk       = 0;
    next_state.resclk_valid = false;

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
    ssize_t                  get_result;
    unsigned long            round_rate;
    struct fclk_device_data* this = dev_get_drvdata(dev);

    if (0 != (get_result = kstrtoul(buf, 0, &round_rate)))
        return get_result;
    this->round_rate = round_rate;
    return size;
}

/**
 * fclk_set_resource()
 */
static ssize_t fclk_set_resource(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
    ssize_t                  get_result;
    int                      set_result;
    unsigned long            resclk;
    struct fclk_state        next_state;
    struct fclk_device_data* this = dev_get_drvdata(dev);

    if (this->resource_clks == NULL)
        return size;

    if (0 != (get_result = kstrtoul(buf, 0, &resclk)))
        return get_result;
    
    if (resclk >= this->resource_clks_size)
        return -EINVAL;

    next_state.rate         = 0;
    next_state.rate_valid   = false;
    next_state.enable       = false;
    next_state.enable_valid = false;
    next_state.resclk       = resclk;
    next_state.resclk_valid = true;

    if (0 != (set_result = __fclk_change_state(this, &next_state)))
        return (ssize_t)set_result;

    return size;
}

/**
 * fclk_show_resource()
 */
static ssize_t fclk_show_resource(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct fclk_device_data* this = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", this->resource_clk_id);
}

/**
 * fclk_show_resource_clks()
 */
static ssize_t fclk_show_resource_clks(struct device *dev, struct device_attribute *attr, char *buf)
{
    size_t                   size = 0;
    struct fclk_device_data* this = dev_get_drvdata(dev);

    if ((this->resource_clks != NULL) && (this->resource_clks_size > 0)) {
        int    i;
        char*  ptr  = buf;
        for (i = 0; i < this->resource_clks_size; i++) {
            struct clk* resource_clk = this->resource_clks[i];
            size_t      len;
            if (i == this->resource_clks_size-1)
                len  = sprintf(ptr, "%s\n", __clk_get_name(resource_clk));
            else
                len  = sprintf(ptr, "%s, ", __clk_get_name(resource_clk));
            ptr  += len;
            size += len;
        }
    }
    return size;
}

static struct device_attribute fclkcfg_device_attrs[] = {
  __ATTR(driver_version , 0444, fclk_show_driver_version, NULL               ),
  __ATTR(enable         , 0664, fclk_show_enable        , fclk_set_enable    ),
  __ATTR(rate           , 0664, fclk_show_rate          , fclk_set_rate      ),
  __ATTR(round_rate     , 0664, fclk_show_round_rate    , fclk_set_round_rate),
  __ATTR(resource       , 0664, fclk_show_resource      , fclk_set_resource  ),
  __ATTR(resource_clks  , 0444, fclk_show_resource_clks , NULL               ),
  __ATTR_NULL,
};

#if (USE_DEV_GROUPS == 1)
static struct attribute *fclkcfg_attrs[] = {
  &(fclkcfg_device_attrs[0].attr),
  &(fclkcfg_device_attrs[1].attr),
  &(fclkcfg_device_attrs[2].attr),
  &(fclkcfg_device_attrs[3].attr),
  &(fclkcfg_device_attrs[4].attr),
  &(fclkcfg_device_attrs[5].attr),
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
 * * fclkcfg_sys_class        - fclkcfg system class.
 * * fclkcfg_device_number    - fclkcfg device major number.
 * * fclkcfg_device_ida       - fclkcfg device minor number allocator variable.
 * * fclk_device_info()       - Print infomation the fclk device data.
 * * fclkcfg_device_create()  - Create  fclkcfg device.
 * * fclkcfg_device_destroy() - Destroy fclkcfg device.
 */
static struct class*  fclkcfg_sys_class = NULL;
static dev_t          fclkcfg_device_number = 0;
static DEFINE_IDA(    fclkcfg_device_ida );

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

#define RES_INFO(dev, tag, resclk)                                                    \
    if (this->resource_clks != NULL) {                                                \
        if (((resclk) >= 0) && ((resclk) < this->resource_clks_size))                 \
            dev_info(dev, tag "%s\n",  __clk_get_name(this->resource_clks[(resclk)]));\
        else                                                                          \
            dev_info(dev, tag "%d\n",  (int)(resclk));                                \
    }

    dev_info(dev, "driver version : %s\n" , DRIVER_VERSION);
    dev_info(dev, "device name    : %s\n" , dev_name(this->device));
    dev_info(dev, "clock  name    : %s\n" , __clk_get_name(this->clk));
    dev_info(dev, "clock  rate    : %lu\n", clk_get_rate(this->clk));
    dev_info(dev, "clock  enabled : %d\n" , __clk_is_enabled(this->clk));
    RES_INFO(dev, "resource clock : "     , this->resource_clk_id);
    if ((this->resource_clks != NULL) && (this->resource_clks_size > 1)) {
        int i;
        for (i = 0; i < this->resource_clks_size; i++) {
            struct clk* resource_clk = this->resource_clks[i];
            dev_info(dev, "resource clocks: %d => %s"  , i, __clk_get_name(resource_clk));
        }
    }
    {
        if (this->remove.rate_valid   == true)
            dev_info(dev, "remove rate    : %lu\n", this->remove.rate  );
        if (this->remove.enable_valid == true)
            dev_info(dev, "remove enable  : %d\n" , this->remove.enable);
        if (this->remove.resclk_valid == true)
            RES_INFO(dev, "remove resource: "     , this->remove.resclk);
    }
}

/**
 * fclkcfg_device_destroy() - Destroy the fclk device data.
 *
 * @this:       Pointer to the fclk device data.
 * Return:      Success(=0) or error status(<0).
 *
 */
static int fclkcfg_device_destroy(struct fclk_device_data* this)
{
    if (!this)
        return -ENODEV;

    if (this->clk          ) {
        clk_put(this->clk);
        this->clk = NULL;
    }
    if (this->resource_clks != NULL) {
        int i;
        for (i = 0 ; i < this->resource_clks_size ; i++) {
            struct clk* resource_clk = this->resource_clks[i];
            clk_put(resource_clk);
        }
        kfree(this->resource_clks);
        this->resource_clks  = NULL;
    }
    this->resource_clks_size = 0;
    this->resource_clk_id    = 0;
    if (this->device       ) {
        device_destroy(fclkcfg_sys_class, this->device_number);
        this->device = NULL;
    }
    if (this->device_number) {
        ida_simple_remove(&fclkcfg_device_ida, MINOR(this->device_number));
        this->device_number = 0;
    }
    kfree(this);
    return 0;
}

/**
 * fclk_device_get_state_property() - get rate/enable/resource property from device.
 *
 * @this:        Pointer to the fclk device data.
 * @dev:         handle to the device structure.
 * @rate_name:   rate property name
 * @enable_name: enable property name
 * @resclk_name: resource clock property name
 * @state:       address of fclk state data.
 * Return:       Success(=0) or error status(<0).
 *
 */
static int fclk_device_get_state_property(struct fclk_device_data* this, struct device* dev, const char* rate_name, const char* enable_name, const char* resclk_name, struct fclk_state* state)
{
    of_get_fclk_state(dev, rate_name, enable_name, resclk_name, state);
    if ((this->resource_clks != NULL) &&
        (state->resclk_valid == true) &&
        (state->resclk >= this->resource_clks_size)) {
        dev_err(dev, "invalid %s(=%lu).\n", resclk_name, state->resclk);
        return -EINVAL;
    }
    return 0;
}

/**
 * fclkcfg_device_create() -  Create fclkcfg device.
 *
 * @dev:        handle to the device structure.
 * Return:      Pointer to the fclk device data or NULL.
 *
 */
static struct fclk_device_data* fclkcfg_device_create(struct device *dev)
{
    int                      retval = 0;
    struct fclk_device_data* this   = NULL;
    const char*              device_name;

    DEV_DBG(dev, "driver probe start.\n");
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
    DEV_DBG(dev, "get device_number start.\n");
    {
        int minor_number = ida_simple_get(&fclkcfg_device_ida, 0, DEVICE_MAX_NUM, GFP_KERNEL);
        if (minor_number < 0) {
            dev_err(dev, "invalid or conflict minor number %d.\n", minor_number);
            retval = -ENODEV;
            goto failed;
        }
        this->device_number = MKDEV(MAJOR(fclkcfg_device_number), minor_number);
    }
    DEV_DBG(dev, "get device_number done.\n");

    /*
     * get clk
     */
    DEV_DBG(dev, "of_clk_get(0) start.\n");
    {
        this->clk = of_clk_get(dev->of_node, 0);
        if (IS_ERR_OR_NULL(this->clk)) {
            dev_err(dev, "of_clk_get(0) failed.\n");
            retval = PTR_ERR(this->clk);
            this->clk = NULL;
            goto failed;
        }
    }    
    DEV_DBG(dev, "of_clk_get(0) done.\n");

    /*
     * get device name
     */
    DEV_DBG(dev, "get device name start.\n");
    {
        device_name = of_get_property(dev->of_node, "device-name", NULL);
        
        if (IS_ERR_OR_NULL(device_name)) {
            device_name = dev_name(dev);
        }
    }
    DEV_DBG(dev, "get device name done.\n");

    /*
     * create device
     */
    DEV_DBG(dev, "device_create start.\n");
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
    DEV_DBG(dev, "device_create done\n");

    /*
     * get resource clock list
     */
    DEV_DBG(dev, "of_clk_get(1..) start.\n");
    {
        int         clk_index;
        struct clk* clk_list[32];
        
        for (clk_index = 0; clk_index < sizeof(clk_list); clk_index++) {
            struct clk* resource_clk = of_clk_get(dev->of_node, clk_index+1);
            if (IS_ERR_OR_NULL(resource_clk))
                break;
            clk_list[clk_index] = resource_clk;
        }
        this->resource_clks_size = clk_index;
        if (this->resource_clks_size > 0) {
            this->resource_clks = kzalloc(this->resource_clks_size*sizeof(struct clk*), GFP_KERNEL);
            if (IS_ERR_OR_NULL(this->resource_clks)) {
                dev_err(dev, "create resource_clks falied.\n");
                for (clk_index = 0; clk_index < this->resource_clks_size; clk_index++) {
                    clk_put(clk_list[clk_index]);
                }
                retval = PTR_ERR(this->resource_clks);
                this->resource_clks      = NULL;
                this->resource_clks_size = 0;
                this->resource_clk_id    = 0;
                goto failed;
            } else {
                for (clk_index = 0; clk_index < this->resource_clks_size; clk_index++) {
                    this->resource_clks[clk_index] = clk_list[clk_index];
                }
                this->resource_clk_id   = -1;   /* Uninitialized resclk flag */
            }
        } else {
                this->resource_clks      = NULL;
                this->resource_clks_size = 0;
                this->resource_clk_id    = 0;
        }
    }
    DEV_DBG(dev, "of_clk_get(1..) done.\n");

    /*
     * get insert state
     */
    if (this->resource_clks != NULL) {
        this->insert.resclk_valid = true; 
        this->insert.resclk       = 0;
    }
    retval = fclk_device_get_state_property(this, dev, "insert-rate", "insert-enable", "insert-resource", &this->insert);
    if (retval)
        goto failed;

    /*
     * change state to insert
     */
    retval = __fclk_change_state(this, &this->insert);
    if (retval) {
        dev_err(dev, "fclk change state failed(%d).\n", retval);
        goto failed;
    }
    this->insert.enable = __clk_is_enabled(this->clk);
    this->insert.rate   = clk_get_rate(this->clk);
    this->insert.resclk = this->resource_clk_id;

    /*
     * get remove state
     */
    retval = fclk_device_get_state_property(this, dev, "remove-rate", "remove-enable", "remove-resource", &this->remove);
    if (retval)
        goto failed;

    return this;

 failed:
    fclkcfg_device_destroy(this);
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
 * * fclkcfg_platform_driver_done
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

    data = fclkcfg_device_create(&pdev->dev);
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

    fclkcfg_device_destroy(this);
    platform_set_drvdata(pdev, NULL);
    dev_info(&pdev->dev, "driver removed.\n");
    return 0;
}

/**
 * Open Firmware Device Identifier Matching Table
 */
static struct of_device_id fclkcfg_of_match[] = {
    { .compatible = "ikwzm,fclkcfg-0.10.a", },
    { .compatible = "ikwzm,fclkcfg"       , },
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
    if (fclkcfg_device_number != 0   ){unregister_chrdev_region(fclkcfg_device_number, 0);}
    ida_destroy(&fclkcfg_device_ida);
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

    ida_init(&fclkcfg_device_ida);

    retval = alloc_chrdev_region(&fclkcfg_device_number, 0, 0, DRIVER_NAME);
    if (retval != 0) {
        printk(KERN_ERR "%s: couldn't allocate device major number\n", DRIVER_NAME);
        fclkcfg_device_number = 0;
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

