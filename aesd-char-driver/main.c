/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/string.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
#include "aesd_ioctl.h"
int aesd_major = 0; // use dynamic major
int aesd_minor = 0;

MODULE_AUTHOR("Anuja Joshi"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;



static size_t aesd_circ_count(const struct aesd_circular_buffer *cb)
{
    if (cb->full) 
    {
    	return AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }
    if (cb->in_offs >= cb->out_offs)
    {
    	 return cb->in_offs - cb->out_offs;
    }
    return AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED - cb->out_offs + cb->in_offs;
}


static size_t aesd_bytes_before_idx(const struct aesd_circular_buffer *cb, uint8_t idx)
{
    size_t total = 0;
    uint8_t i = cb->out_offs;

    while (i != idx) 
    {
        total += cb->entry[i].size;
        i = (i + 1u) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }
    return total;
}


static long aesd_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    long retval = 0;  

    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC || _IOC_NR(cmd) > AESDCHAR_IOC_MAXNR)
    {
        return -ENOTTY;
    }

    switch (cmd) 
    {
    case AESDCHAR_IOCSEEKTO:
        if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)))
            return -EFAULT;

        mutex_lock(&dev->lock);

        {
            size_t nvalid = aesd_circ_count(&dev->circ);
            if (seekto.write_cmd >= nvalid) 
            {
                mutex_unlock(&dev->lock);
                return -EINVAL;
            }

            uint8_t idx = (uint8_t)((dev->circ.out_offs + seekto.write_cmd) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);
            const struct aesd_buffer_entry *e = &dev->circ.entry[idx];

            if (!e->buffptr || seekto.write_cmd_offset >= e->size) 
            {
                mutex_unlock(&dev->lock);
                return -EINVAL;
            }


            loff_t newpos = (loff_t)aesd_bytes_before_idx(&dev->circ, idx) + (loff_t)seekto.write_cmd_offset;

            filp->f_pos = newpos;
        }

        mutex_unlock(&dev->lock);
        break;

    default:
        retval = -ENOTTY;
        break;
    }

    return retval;
}


static size_t aesd_total_bytes_locked(struct aesd_dev *dev)
{
    /* dev->lock must be held by caller */
    size_t total = 0;
    uint8_t i;
    struct aesd_buffer_entry *e;

    AESD_CIRCULAR_BUFFER_FOREACH(e, &dev->circ, i) {
        if (e->buffptr && e->size)
            total += e->size;
    }
    return total;
}

static loff_t aesd_llseek(struct file *filp, loff_t off, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t newpos, total;
    
    mutex_lock(&dev->lock);

    total = (loff_t)aesd_total_bytes_locked(dev);

    switch (whence) {
    case SEEK_SET:
        newpos = off;
        break;
    case SEEK_CUR:
        newpos = filp->f_pos + off;
        break;
    case SEEK_END:
        newpos = total + off;
        break;
    default:
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    /* Valid absolute range is [0, total]; total == EOF */
    if (newpos < 0 || newpos > total) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }


    filp->f_pos = newpos;

    mutex_unlock(&dev->lock);
    return newpos;
}

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    struct aesd_dev *dev;


    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev; 

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    filp->private_data = NULL;
    return 0;
}


ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = 0;           
    loff_t pos = *f_pos;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    if (!buf || count == 0)
    {
        return 0;
    }

    mutex_lock(&dev->lock);

    while (retval < count) {
        size_t entry_offset = 0;
        const struct aesd_buffer_entry *entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circ,
                                                            					pos,
                                                            					&entry_offset);
        if (!entry) 
        {
            break;
        }

        size_t bytes_avail = entry->size - entry_offset;

        size_t bytes_to_copy = min(count - retval, bytes_avail);

        if (copy_to_user(buf + retval, entry->buffptr + entry_offset, bytes_to_copy)) 
        {     
            retval = retval ? retval : -EFAULT;
            break;
        }

        retval += bytes_to_copy;   
        pos+= bytes_to_copy;   

        if (bytes_to_copy < bytes_avail)
        {
            break;
        }
     
    }

    if (retval > 0)
    {
        *f_pos += retval;         
    }

    mutex_unlock(&dev->lock);
    return retval;                
}



ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    char *kbuf = NULL;
    ssize_t retval = count;
    
    if (!count)
    {
    	return 0;
    }
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle write
     */
     
     kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf) 
    {
    	return -ENOMEM;
    }

    if (copy_from_user(kbuf, buf, count)) 
    {
        kfree(kbuf);
        return -EFAULT;
    }

    mutex_lock(&dev->lock);


    
        char *newb = krealloc(dev->working.buffptr, dev->working.size + count, GFP_KERNEL);
        if (!newb) 
        {
            retval = -ENOMEM;
            goto out_unlock;
        }
        memcpy(newb + dev->working.size, kbuf, count);
        dev->working.buffptr = newb;
        dev->working.size   += count;
    

    while (dev->working.size) 
    {
        void *nl = memchr(dev->working.buffptr, '\n', dev->working.size);
        if (!nl) 
        {
        	break;  
        }                   

        size_t cmd_len = (char *)nl - dev->working.buffptr + 1;


        
            struct aesd_buffer_entry e;
            e.buffptr = kmemdup(dev->working.buffptr, cmd_len, GFP_KERNEL);
            if (!e.buffptr) 
            {                 
                retval = -ENOMEM;
                break;
            }
            e.size = cmd_len;

            if (dev->circ.full) 
            {
                struct aesd_buffer_entry *old = &dev->circ.entry[dev->circ.in_offs];
                if (old->buffptr) 
                {
                    kfree(old->buffptr);
                    old->buffptr = NULL;
                    old->size = 0;
                }
            }

            aesd_circular_buffer_add_entry(&dev->circ, &e);
        


        
            size_t remain = dev->working.size - cmd_len;

            if (remain) 
            {
                memmove(dev->working.buffptr, dev->working.buffptr + cmd_len, remain);
                dev->working.size = remain;

                
                (void)krealloc(dev->working.buffptr, remain, GFP_KERNEL);
            } 
            else 
            {
                kfree(dev->working.buffptr);
                dev->working.buffptr = NULL;
                dev->working.size    = 0;
            }
        
    }

out_unlock:
    mutex_unlock(&dev->lock);
    kfree(kbuf);
    return retval;
}
struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek  = aesd_llseek,
    .unlocked_ioctl = aesd_unlocked_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.circ);
    aesd_device.working.buffptr= NULL;
    aesd_device.working.size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */

     mutex_lock(&aesd_device.lock);
    {
        uint8_t idx;
        struct aesd_buffer_entry *e;

        AESD_CIRCULAR_BUFFER_FOREACH(e, &aesd_device.circ, idx) 
        {
            if (e->buffptr) 
            
            {
                kfree(e->buffptr);
                e->buffptr = NULL;
                e->size = 0;
            }
        }

        if (aesd_device.working.buffptr) 
        {
            kfree(aesd_device.working.buffptr);
            aesd_device.working.buffptr = NULL;
            aesd_device.working.size = 0;
        }
    }
    mutex_unlock(&aesd_device.lock);
    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
