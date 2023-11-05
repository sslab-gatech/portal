# Concept of VFIO



# Bind target device to vifo-pci driver
To utilize physical device as vfio and pass it to the guest, the device driver
bound to the target device should be unbound first and then bound to the 
vfio-pci device driver. Also, when the device is attached to the vfio-pci, it 
will generate /dev/vfio/gid

Usually, the user-side code related with the bindings are not implemented on 
QEMU or other user processes that talks to KVM. Therefore, the user should first
unbind and bind the target device properly. We will skip the details of binding.

## Kernel side of the device binding to vfio-pci driver
When the new device is attached to the vfio-pci driver, the probe function of 
the driver, vfio_pci_probe, is invoked. Below is the sequence of functions to 
all the way up to generating VFIO character device for the attached device.

vfio_pci_probe-> vfio_pci_core_register_device -> vfio_register_group_dev ->
__vfio_register_dev -> vfio_device_set_group -> vfio_group_find_or_alloc -> 
vfio_create_group                                                                              

```cpp
static int vfio_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
        struct vfio_pci_core_device *vdev;
        int ret;

        if (vfio_pci_is_denylisted(pdev))
                return -EINVAL;

        vdev = vfio_alloc_device(vfio_pci_core_device, vdev, &pdev->dev,
                                 &vfio_pci_ops);
        if (IS_ERR(vdev))
                return PTR_ERR(vdev);

        dev_set_drvdata(&pdev->dev, vdev);
        ret = vfio_pci_core_register_device(vdev);
        if (ret)
                goto out_put_vdev;
        return 0;

out_put_vdev:
        vfio_put_device(&vdev->vdev);
        return ret;
}
```

### Create vfio_device
The first job of the probe function is generating new vfio_pci device. This will
be handled by vfio_alloc_device function. Nothing is interesting here, but it 
has multiple functions associated with the device which will be called later. 

```cpp
static const struct vfio_device_ops vfio_pci_ops = {
        .name           = "vfio-pci",
        .init           = vfio_pci_core_init_dev,
        .release        = vfio_pci_core_release_dev,
        .open_device    = vfio_pci_open_device,
        .close_device   = vfio_pci_core_close_device,
        .ioctl          = vfio_pci_core_ioctl,
        .device_feature = vfio_pci_core_ioctl_feature,
        .read           = vfio_pci_core_read,
        .write          = vfio_pci_core_write,
        .mmap           = vfio_pci_core_mmap,
        .request        = vfio_pci_core_request,
        .match          = vfio_pci_core_match,
        .bind_iommufd   = vfio_iommufd_physical_bind,
        .unbind_iommufd = vfio_iommufd_physical_unbind,
        .attach_ioas    = vfio_iommufd_physical_attach_ioas,
};    
```


### Create vfio_group
The minimal granularity that can be assigned to a VM through the VFIO is a group.
Group consists of multiple VFIO devices which is bound to vfio device driver. 


```cpp
int vfio_device_set_group(struct vfio_device *device,
                          enum vfio_group_type type)
{               
        struct vfio_group *group;
        
        if (type == VFIO_IOMMU)
                group = vfio_group_find_or_alloc(device->dev);
        else
                group = vfio_noiommu_group_alloc(device->dev, type);
        
        if (IS_ERR(group))
                return PTR_ERR(group);
                          
        /* Our reference on group is moved to the device */
        device->group = group;
        return 0;
}       
```

```cpp
static struct vfio_group *vfio_group_find_or_alloc(struct device *dev)
{                         
        struct iommu_group *iommu_group;
        struct vfio_group *group;
        
        iommu_group = iommu_group_get(dev);
        if (!iommu_group && vfio_noiommu) {
                /*
                 * With noiommu enabled, create an IOMMU group for devices that
                 * don't already have one, implying no IOMMU hardware/driver
                 * exists.  Taint the kernel because we're about to give a DMA
                 * capable device to a user without IOMMU protection.
                 */       
                group = vfio_noiommu_group_alloc(dev, VFIO_NO_IOMMU);
                if (!IS_ERR(group)) {
                        add_taint(TAINT_USER, LOCKDEP_STILL_OK);
                        dev_warn(dev, "Adding kernel taint for vfio-noiommu group on device\n");
                }
                return group;
        }

        if (!iommu_group)
                return ERR_PTR(-EINVAL);

        /*
         * VFIO always sets IOMMU_CACHE because we offer no way for userspace to
         * restore cache coherency. It has to be checked here because it is only
         * valid for cases where we are using iommu groups.
         */
        if (!device_iommu_capable(dev, IOMMU_CAP_CACHE_COHERENCY)) {
                iommu_group_put(iommu_group);
                return ERR_PTR(-EINVAL);
        }

        mutex_lock(&vfio.group_lock);
        group = vfio_group_find_from_iommu(iommu_group);
        if (group) {
                if (WARN_ON(vfio_group_has_device(group, dev)))
                        group = ERR_PTR(-EINVAL);
                else    
                        refcount_inc(&group->drivers);
        } else {
                group = vfio_create_group(iommu_group, VFIO_IOMMU);
        }
        mutex_unlock(&vfio.group_lock);
        
        /* The vfio_group holds a reference to the iommu_group */
        iommu_group_put(iommu_group);
        return group;
}
```

Note that it first retrieve the IOMMU group associated with the device. Also the
device is the pci device that we want to pass to guest VM through VFIO. If the 
target PCI device is connected to the IOMMU, it will return the **iommu_group**.
Unless there is existing vfio_group associated with the retrieved IOMMU group,
it creates new vfio_group (vfio_create_group).


```cpp
static struct vfio_group *vfio_create_group(struct iommu_group *iommu_group,
                enum vfio_group_type type)
{                               
        struct vfio_group *group;
        struct vfio_group *ret;
        int err;
                        
        lockdep_assert_held(&vfio.group_lock);

        group = vfio_group_alloc(iommu_group, type);
        if (IS_ERR(group))
                return group;
                
        err = dev_set_name(&group->dev, "%s%d",
                           group->type == VFIO_NO_IOMMU ? "noiommu-" : "",
                           iommu_group_id(iommu_group));
        if (err) {
                ret = ERR_PTR(err);
                goto err_put;
        }

        err = cdev_device_add(&group->cdev, &group->dev);
        if (err) {
                ret = ERR_PTR(err);
                goto err_put;
        }

        list_add(&group->vfio_next, &vfio.group_list);

        return group;

err_put:
        put_device(&group->dev);
        return ret;
}
```

vfio_group_alloc function creates new group for the device. If the group is 
newly created, it will be registered in the vfio's group_list. Note that it is
global variable, so generated vfio_group can be accessible through vfio's list.

```cpp
struct vfio_group {
        struct device                   dev;
        struct cdev                     cdev;
        /*
         * When drivers is non-zero a driver is attached to the struct device
         * that provided the iommu_group and thus the iommu_group is a valid
         * pointer. When drivers is 0 the driver is being detached. Once users
         * reaches 0 then the iommu_group is invalid.
         */
        refcount_t                      drivers;
        unsigned int                    container_users;
        struct iommu_group              *iommu_group;
        struct vfio_container           *container;
        struct list_head                device_list;
        struct mutex                    device_lock;
        struct list_head                vfio_next;
#if IS_ENABLED(CONFIG_VFIO_CONTAINER)
        struct list_head                container_next;
#endif
        enum vfio_group_type            type;
        struct mutex                    group_lock;
        struct kvm                      *kvm;
        struct file                     *opened_file;
        struct blocking_notifier_head   notifier;
        struct iommufd_ctx              *iommufd;
};
```
```cpp
static struct vfio_group *vfio_group_alloc(struct iommu_group *iommu_group,
                                           enum vfio_group_type type)
{       
        struct vfio_group *group;
        int minor;
        
        group = kzalloc(sizeof(*group), GFP_KERNEL);
        if (!group)
                return ERR_PTR(-ENOMEM);
        
        minor = ida_alloc_max(&vfio.group_ida, MINORMASK, GFP_KERNEL);
        if (minor < 0) {
                kfree(group);
                return ERR_PTR(minor);
        }
        
        device_initialize(&group->dev);
        group->dev.devt = MKDEV(MAJOR(vfio.group_devt), minor);
        group->dev.class = vfio.class;
        group->dev.release = vfio_group_release;
        cdev_init(&group->cdev, &vfio_group_fops);
        group->cdev.owner = THIS_MODULE;
        
        refcount_set(&group->drivers, 1);
        mutex_init(&group->group_lock);
        INIT_LIST_HEAD(&group->device_list);
        mutex_init(&group->device_lock);
        group->iommu_group = iommu_group;
        /* put in vfio_group_release() */
        iommu_group_ref_get(iommu_group);
        group->type = type;
        BLOCKING_INIT_NOTIFIER_HEAD(&group->notifier);
        
        return group;
}
```

The most important part is assigning iommu_group of the target device to the 
vfio_group meember field iommu_group. The reason why vfio_group needs the 
iommu_group is because it bridges the vfio interface and iommu. 


```cpp
struct vfio_device {
        struct device *dev;
        const struct vfio_device_ops *ops;
        /*
         * mig_ops/log_ops is a static property of the vfio_device which must
         * be set prior to registering the vfio_device.
         */
        const struct vfio_migration_ops *mig_ops;
        const struct vfio_log_ops *log_ops;
        struct vfio_group *group;
        struct vfio_device_set *dev_set;
        struct list_head dev_set_list;
        unsigned int migration_flags;
        /* Driver must reference the kvm during open_device or never touch it */
        struct kvm *kvm;

        /* Members below here are private, not for driver use */
        unsigned int index;
        struct device device;   /* device.kref covers object life circle */
        refcount_t refcount;    /* user count on registered device*/
        unsigned int open_count;
        struct completion comp;
        struct list_head group_next;
        struct list_head iommu_entry;
        struct iommufd_access *iommufd_access;
#if IS_ENABLED(CONFIG_IOMMUFD)
        struct iommufd_device *iommufd_device;
        struct iommufd_ctx *iommufd_ictx;
        bool iommufd_attached;
#endif
};
```


## Binding group to container \FIXME{Change}
Remember that covered content is about the vfio-pci driver. There is a core 
vfio driver that manages entire VFIO in the system. This driver is accessible
through the cdev installed at /dev/vfio/vfio. This driver binds the groups to 
the container and bridges groups to the IOMMU sub-system. 


### vfio driver initialization
```cpp
static int __init vfio_init(void)
{
        int ret;

        ida_init(&vfio.device_ida);

        ret = vfio_group_init();
        if (ret)
                return ret;

        ret = vfio_virqfd_init();
        if (ret)
                goto err_virqfd;       

        /* /sys/class/vfio-dev/vfioX */
        vfio.device_class = class_create(THIS_MODULE, "vfio-dev");
        if (IS_ERR(vfio.device_class)) {
                ret = PTR_ERR(vfio.device_class);
                goto err_dev_class;
        }
        
        pr_info(DRIVER_DESC " version: " DRIVER_VERSION "\n");
        return 0;
        
err_dev_class:
        vfio_virqfd_exit();
err_virqfd:     
        vfio_group_cleanup();
        return ret;
}
```

```cpp

int __init vfio_container_init(void)
{
        int ret;

        mutex_init(&vfio.iommu_drivers_lock);
        INIT_LIST_HEAD(&vfio.iommu_drivers_list);

        ret = misc_register(&vfio_dev);
        if (ret) {
                pr_err("vfio: misc device register failed\n");
                return ret;
        }

        if (IS_ENABLED(CONFIG_VFIO_NOIOMMU)) {
                ret = vfio_register_iommu_driver(&vfio_noiommu_ops);
                if (ret)
                        goto err_misc;
        }
        return 0;

err_misc:
        misc_deregister(&vfio_dev);
        return ret;
}
```

During initializing container, it spawn misc device at /dev/vfio, which is 
/dev/vfio/vfio.


### Open VFIO dev node -> Generate container 
```cpp
static int vfio_fops_open(struct inode *inode, struct file *filep)
{
        struct vfio_container *container;

        container = kzalloc(sizeof(*container), GFP_KERNEL);
        if (!container)
                return -ENOMEM;

        INIT_LIST_HEAD(&container->group_list);
        init_rwsem(&container->group_lock);
        kref_init(&container->kref);
                
        filep->private_data = container;
        
        return 0;
}       
```


## Bind group to the container
We have a container fd obtained by opening the /dev/vfio/vfio and a group file
descriptor obtained by opening the /dev/vfio/$gid.  The next step is to 
associate this group with the container, accomplished by invoking the ioctl call 
with the argument VFIO_GROUP_SET_CONTAINER on the group file descriptor. 

### User-space code to bind generated group to container
```cpp
static struct vfio_group *vfio_group_create(struct kvm *kvm, unsigned long id) 
{
        int ret;
        struct vfio_group *group;
        char group_node[PATH_MAX];
        struct vfio_group_status group_status = {
                .argsz = sizeof(group_status),
        };

        group = calloc(1, sizeof(*group));
        if (!group)
                return NULL;

        group->id       = id;
        group->refs     = 1;

        ret = snprintf(group_node, PATH_MAX, VFIO_DEV_DIR "/%lu", id);
        if (ret < 0 || ret == PATH_MAX)
                return NULL;

        group->fd = open(group_node, O_RDWR);
        if (group->fd < 0) {
                pr_err("Failed to open IOMMU group %s", group_node);
                goto err_free_group;
        }

        if (ioctl(group->fd, VFIO_GROUP_GET_STATUS, &group_status)) {
                pr_err("Failed to determine status of IOMMU group %lu", id);
                goto err_close_group;
        }

        if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
                pr_err("IOMMU group %lu is not viable", id);
                goto err_close_group;
        }

        if (ioctl(group->fd, VFIO_GROUP_SET_CONTAINER, &vfio_container)) {
                pr_err("Failed to add IOMMU group %lu to VFIO container", id);
                goto err_close_group;
        }

        list_add(&group->list, &vfio_groups);

        return group;

err_close_group:
        close(group->fd);
err_free_group:
        free(group);

        return NULL;
}
```
Note that the vfio_container passed to the ioctl is the file descriptor of the 
/dev/vfio/vfio. Also, the ioctl invokes the function belong to the group, not 
the vfio. 

### Kernel space handling the group to container binding (ioctl of vfio_group)
```cpp
static int vfio_group_ioctl_set_container(struct vfio_group *group,
                                          int __user *arg)
{       
        struct vfio_container *container;
        struct iommufd_ctx *iommufd;
        struct fd f;
        int ret;
        int fd;

        if (get_user(fd, arg))
                return -EFAULT;
        
        f = fdget(fd);
        if (!f.file)
                return -EBADF;
        
        mutex_lock(&group->group_lock);
        if (vfio_group_has_iommu(group)) {
                ret = -EINVAL;
                goto out_unlock;
        }
        if (!group->iommu_group) {
                ret = -ENODEV;
                goto out_unlock;
        }

        container = vfio_container_from_file(f.file);
        if (container) {
                ret = vfio_container_attach_group(container, group);
                goto out_unlock;
        }

        iommufd = iommufd_ctx_from_file(f.file);
        if (!IS_ERR(iommufd)) {
                u32 ioas_id;
                
                ret = iommufd_vfio_compat_ioas_id(iommufd, &ioas_id);
                if (ret) {
                        iommufd_ctx_put(group->iommufd);
                        goto out_unlock;
                }
                
                group->iommufd = iommufd;
                goto out_unlock;
        }
        
        /* The FD passed is not recognized. */
        ret = -EBADFD;

out_unlock:
        mutex_unlock(&group->group_lock);
        fdput(f);
        return ret;
}
```
Through the user-passed arg which is the file descriptor of the vfio driver, it 
can access the container. Remember that the container was created at the time of
when the vfio dev is open and stored in the private_data filed of the file. As 
we retrieved the container and group, let's take a look how the group can be 
attached to the container. 

```cpp
int vfio_container_attach_group(struct vfio_container *container,
                                struct vfio_group *group)
{       
        struct vfio_iommu_driver *driver;
        int ret = 0;
                
        lockdep_assert_held(&group->group_lock);
                        
        if (group->type == VFIO_NO_IOMMU && !capable(CAP_SYS_RAWIO))
                return -EPERM;
        
        down_write(&container->group_lock);
                
        /* Real groups and fake groups cannot mix */
        if (!list_empty(&container->group_list) &&
            container->noiommu != (group->type == VFIO_NO_IOMMU)) {
                ret = -EPERM;
                goto out_unlock_container;
        }

        if (group->type == VFIO_IOMMU) {
                ret = iommu_group_claim_dma_owner(group->iommu_group, group);
                if (ret)
                        goto out_unlock_container;
        }

        driver = container->iommu_driver;
        if (driver) {
                ret = driver->ops->attach_group(container->iommu_data,
                                                group->iommu_group,
                                                group->type);
                if (ret) {
                        if (group->type == VFIO_IOMMU)
                                iommu_group_release_dma_owner(
                                        group->iommu_group);
                        goto out_unlock_container;
                }
        }
        
        group->container = container;
        group->container_users = 1;
        container->noiommu = (group->type == VFIO_NO_IOMMU);
        list_add(&group->container_next, &container->group_list);
        
        /* Get a reference on the container and mark a user within the group */
        vfio_container_get(container);

out_unlock_container:
        up_write(&container->group_lock);
        return ret;
}
```

This function has two important roles: 1. Attach the current group to the IOMMU
driver. 2. Add the group to the container's group_list.

### Claim DMA OWNER
```cpp
 *
 * This is to support backward compatibility for vfio which manages the dma
 * ownership in iommu_group level. New invocations on this interface should be
 * prohibited. Only a single owner may exist for a group.
 */ 
int iommu_group_claim_dma_owner(struct iommu_group *group, void *owner)
{       
        int ret = 0;
        
        if (WARN_ON(!owner))
                return -EINVAL;
        
        mutex_lock(&group->mutex);
        if (group->owner_cnt) {
                ret = -EPERM;
                goto unlock_out;
        }
        
        ret = __iommu_take_dma_ownership(group, owner);
unlock_out:
        mutex_unlock(&group->mutex);
        
        return ret;
}
EXPORT_SYMBOL_GPL(iommu_group_claim_dma_owner);
```

```cpp
static int __iommu_take_dma_ownership(struct iommu_group *group, void *owner)
{       
        int ret;
        
        if ((group->domain && group->domain != group->default_domain) ||
            !xa_empty(&group->pasid_array))
                return -EBUSY;

        ret = __iommu_group_alloc_blocking_domain(group);
        if (ret)
                return ret;
        ret = __iommu_group_set_domain(group, group->blocking_domain);
        if (ret)
                return ret;

        group->owner = owner;
        group->owner_cnt++;
        return 0;
}

```


## Associate IOMMU to VFIO group
The userspace can **configure the IOMMU for the container** by invoking 
VFIO_SET_IOMMU ioctl on the file descriptor of the container. 

```cpp
static long vfio_ioctl_set_iommu(struct vfio_container *container,
                                 unsigned long arg)
{               
        struct vfio_iommu_driver *driver;
        long ret = -ENODEV;
                
        down_write(&container->group_lock);
                
        /*              
         * The container is designed to be an unprivileged interface while
         * the group can be assigned to specific users.  Therefore, only by
         * adding a group to a container does the user get the privilege of
         * enabling the iommu, which may allocate finite resources.  There
         * is no unset_iommu, but by removing all the groups from a container,
         * the container is deprivileged and returns to an unset state.
         */
        if (list_empty(&container->group_list) || container->iommu_driver) {
                up_write(&container->group_lock);
                return -EINVAL;
        }
                
        mutex_lock(&vfio.iommu_drivers_lock);
        list_for_each_entry(driver, &vfio.iommu_drivers_list, vfio_next) {
                void *data;
        
                if (!vfio_iommu_driver_allowed(container, driver))
                        continue;
                if (!try_module_get(driver->ops->owner))
                        continue;

                /*
                 * The arg magic for SET_IOMMU is the same as CHECK_EXTENSION,
                 * so test which iommu driver reported support for this
                 * extension and call open on them.  We also pass them the
                 * magic, allowing a single driver to support multiple
                 * interfaces if they'd like.
                 */
                if (driver->ops->ioctl(NULL, VFIO_CHECK_EXTENSION, arg) <= 0) {
                        module_put(driver->ops->owner);
                        continue;
                }

                data = driver->ops->open(arg);
                if (IS_ERR(data)) {
                        ret = PTR_ERR(data);
                        module_put(driver->ops->owner);
                        continue;
                }

                ret = __vfio_container_attach_groups(container, driver, data);
                if (ret) {
                        driver->ops->release(data);
                        module_put(driver->ops->owner);
                        continue;
                }

                container->iommu_driver = driver;
                container->iommu_data = data;
                break;
        }

        mutex_unlock(&vfio.iommu_drivers_lock);
        up_write(&container->group_lock);

        return ret;
}
```
It iterates the list of registered drivers in the **iommu_drivers_list** of the 
vfio and \XXX.
Therefore, understanding which device driver is registers in the list is 
important to understand how the vfio_ioctl_set_iommu call initialize IOMMU 
drivers for the vfio group. 

### vfio_iommu_type1 for iommu 
There is only registered driver in the iommu_drivers_list unless the noiommu is
used. 

**vfio_iommu_type1.c**
```cpp
static int __init vfio_iommu_type1_init(void)
{
        return vfio_register_iommu_driver(&vfio_iommu_driver_ops_type1);
}       

int vfio_register_iommu_driver(const struct vfio_iommu_driver_ops *ops)
{       
        struct vfio_iommu_driver *driver, *tmp;

        if (WARN_ON(!ops->register_device != !ops->unregister_device))
                return -EINVAL;
        
        driver = kzalloc(sizeof(*driver), GFP_KERNEL);
        if (!driver)            
                return -ENOMEM; 
        
        driver->ops = ops;      
        
        mutex_lock(&vfio.iommu_drivers_lock);
        
        /* Check for duplicates */
        list_for_each_entry(tmp, &vfio.iommu_drivers_list, vfio_next) {
                if (tmp->ops == ops) {
                        mutex_unlock(&vfio.iommu_drivers_lock);
                        kfree(driver);
                        return -EINVAL;
                }
        }

        list_add(&driver->vfio_next, &vfio.iommu_drivers_list);

        mutex_unlock(&vfio.iommu_drivers_lock);

        return 0;
}       
```
Note that the current iommu driver is added to the iommu_drivers_list of vfio.

```cpp
static const struct vfio_iommu_driver_ops vfio_iommu_driver_ops_type1 = {
        .name                   = "vfio-iommu-type1",
        .owner                  = THIS_MODULE,
        .open                   = vfio_iommu_type1_open,
        .release                = vfio_iommu_type1_release, 
        .ioctl                  = vfio_iommu_type1_ioctl,
        .attach_group           = vfio_iommu_type1_attach_group,
        .detach_group           = vfio_iommu_type1_detach_group,
        .pin_pages              = vfio_iommu_type1_pin_pages,
        .unpin_pages            = vfio_iommu_type1_unpin_pages,
        .register_device        = vfio_iommu_type1_register_device,
        .unregister_device      = vfio_iommu_type1_unregister_device,
        .dma_rw                 = vfio_iommu_type1_dma_rw,
        .group_iommu_domain     = vfio_iommu_type1_group_iommu_domain,
        .notify                 = vfio_iommu_type1_notify,
};
```

Therefore, the open call of the selected driver in vfio_ioctl_set_iommu will 
invoke vfio_iommu_type1_open function. 

```cpp
static void *vfio_iommu_type1_open(unsigned long arg)
{       
        struct vfio_iommu *iommu;
        
        iommu = kzalloc(sizeof(*iommu), GFP_KERNEL);
        if (!iommu)
                return ERR_PTR(-ENOMEM);
        
        switch (arg) {          
        case VFIO_TYPE1_IOMMU:  
                break;          
        case VFIO_TYPE1_NESTING_IOMMU:
                iommu->nesting = true;
                fallthrough;    
        case VFIO_TYPE1v2_IOMMU:
                iommu->v2 = true; 
                break;
        default:
                kfree(iommu);   
                return ERR_PTR(-EINVAL);
        }

        INIT_LIST_HEAD(&iommu->domain_list);
        INIT_LIST_HEAD(&iommu->iova_list);
        iommu->dma_list = RB_ROOT;
        iommu->dma_avail = dma_entry_limit;
        iommu->container_open = true;
        mutex_init(&iommu->lock);
        mutex_init(&iommu->device_list_lock);
        INIT_LIST_HEAD(&iommu->device_list);
        init_waitqueue_head(&iommu->vaddr_wait);
        iommu->pgsize_bitmap = PAGE_MASK;
        INIT_LIST_HEAD(&iommu->emulated_iommu_groups);

        return iommu;
}       
```
It allocates vfio_iommu struct instance, fill out the information, and return it. 


### Attach vfio groups to vfio-iommu-driver (vfio_iommu_type1_attach_group)
Retrieved vfio_iommu will be used by __vfio_container_attach_groups function to
attach the group to the IOMMU driver. Remember that the container is the bigger
higher level concept embracing multiple groups of vfio. 

```cpp
/* hold write lock on container->group_lock */
static int __vfio_container_attach_groups(struct vfio_container *container,
                                          struct vfio_iommu_driver *driver, // driver is vfio_iommu_type1
                                          void *data) //data is the vfio_iommu!
{       
        struct vfio_group *group;
        int ret = -ENODEV;
                
        list_for_each_entry(group, &container->group_list, container_next) {
                ret = driver->ops->attach_group(data, group->iommu_group,
                                                group->type);
                if (ret)
                        goto unwind;
        }

        return ret;

unwind:                        
        list_for_each_entry_continue_reverse(group, &container->group_list,
                                             container_next) {
                driver->ops->detach_group(data, group->iommu_group); 
        }
        
        return ret;
}
```

It iterates groups registered to the container and invokes the **attach_group**
function of the vfio_iommu_driver_ops_type1 to attach group to iommu. Since the 
vfio_iommu_type1_attach_group is complex and long, I will break down this 
function into multiple sections based on crucial roles related with the 
IOMMU. 

```cpp
static int vfio_iommu_type1_attach_group(void *iommu_data,
                struct iommu_group *iommu_group, enum vfio_group_type type) 
{       
        struct vfio_iommu *iommu = iommu_data;
        struct vfio_iommu_group *group;
        struct vfio_domain *domain, *d;
        bool resv_msi, msi_remap;
        phys_addr_t resv_msi_base = 0;
        struct iommu_domain_geometry *geo;
        LIST_HEAD(iova_copy);   
        LIST_HEAD(group_resv_regions);
        int ret = -EINVAL;      
        
        mutex_lock(&iommu->lock); 
        
        /* Check for duplicates */
        if (vfio_iommu_find_iommu_group(iommu, iommu_group))
                goto out_unlock;
        
        ret = -ENOMEM;
        group = kzalloc(sizeof(*group), GFP_KERNEL);
        if (!group)
                goto out_unlock;
        group->iommu_group = iommu_group;

        if (type == VFIO_EMULATED_IOMMU) {
                list_add(&group->next, &iommu->emulated_iommu_groups);
                /*
                 * An emulated IOMMU group cannot dirty memory directly, it can
                 * only use interfaces that provide dirty tracking.
                 * The iommu scope can only be promoted with the addition of a
                 * dirty tracking group.
                 */
                group->pinned_page_dirty_scope = true;
                ret = 0;
                goto out_unlock;
        }

        ret = -ENOMEM;
        domain = kzalloc(sizeof(*domain), GFP_KERNEL);
        if (!domain)
                goto out_free_group;
```
What is the input of this function? 
- iommu_data: the vfio_iommu as a result of open of vfio_iommu_type1
- iommu_group: the iommu_group of the group of the container
Note that the second parameter is generated when the group was created by the
vfio_group_find_or_alloc function. Most of the time, the iommu_group attached to
the device used for generating the group is set as iommu_group of the vfio group. 

What would be the result of this function? (possibly?)
Note, it allocates vfio_iommu_group and vfio_domain in this function. Let's 
focus on how those two data structure are initialized and embedded into other 
parts of the VFIO drivers. 

### Allocate new domain 
```cpp
static int vfio_iommu_type1_attach_group(void *iommu_data,                          
                struct iommu_group *iommu_group, enum vfio_group_type type)     
{         
	......
        /*
         * Going via the iommu_group iterator avoids races, and trivially gives
         * us a representative device for the IOMMU API call. We don't actually
         * want to iterate beyond the first device (if any).
         */
        ret = -EIO;
        iommu_group_for_each_dev(iommu_group, &domain->domain,
                                 vfio_iommu_domain_alloc);
        if (!domain->domain)
                goto out_free_domain;

        if (iommu->nesting) {
                ret = iommu_enable_nesting(domain->domain);
                if (ret)
                        goto out_domain;
        }
```

The role of the above part of the attach_group function is to invoke 
vfio_iommu_domain_alloc function to all devices in the iommu_group. 

```cpp
int iommu_group_for_each_dev(struct iommu_group *group, void *data,
                             int (*fn)(struct device *, void *))
{
        int ret;
        
        mutex_lock(&group->mutex);
        ret = __iommu_group_for_each_dev(group, data, fn);
        mutex_unlock(&group->mutex);

        return ret;
}       

static int __iommu_group_for_each_dev(struct iommu_group *group, void *data,
                                      int (*fn)(struct device *, void *))
{
        struct group_device *device;
        int ret = 0;

        list_for_each_entry(device, &group->devices, list) {
                ret = fn(device->dev, data);
                if (ret)
                        break;
        }
        return ret;
}       
```

```cpp
static int vfio_iommu_domain_alloc(struct device *dev, void *data)
{
        struct iommu_domain **domain = data;

        *domain = iommu_domain_alloc(dev->bus);
        return 1; /* Don't iterate */
}

Although the list_for_each_entry is supposed to invoke __iommu_domain_alloc 
function for every devices of the iommu_domain, but as vfio_iommu_domain_alloc 
function returns 1, the function will be invoked for the first device in the 
group and exit.

struct iommu_domain *iommu_domain_alloc(struct bus_type *bus)
{       
        return __iommu_domain_alloc(bus, IOMMU_DOMAIN_UNMANAGED);
}

static struct iommu_domain *__iommu_domain_alloc(struct bus_type *bus,
                                                 unsigned type)
{               
        struct iommu_domain *domain;
                        
        if (bus == NULL || bus->iommu_ops == NULL)
                return NULL;
                                    
        domain = bus->iommu_ops->domain_alloc(type);
        if (!domain)
                return NULL;
        
        domain->type = type;          
        /* Assume all sizes by default; the driver may override this later */
        domain->pgsize_bitmap = bus->iommu_ops->pgsize_bitmap;
        if (!domain->ops)
                domain->ops = bus->iommu_ops->default_domain_ops;

        if (iommu_is_dma_domain(domain) && iommu_get_dma_cookie(domain)) {
                iommu_domain_free(domain);
                domain = NULL;
        }
        return domain;
}               
```

As we work on the aarch64, the iommu_ops of the bus should be the arm_smmu_ops. 
Therefore, the domain_alloc function should invoke arm_smmu_domain_alloc. 

```cpp
static struct iommu_ops arm_smmu_ops = {
        .capable                = arm_smmu_capable,
        .domain_alloc           = arm_smmu_domain_alloc, 
        .probe_device           = arm_smmu_probe_device,
        .release_device         = arm_smmu_release_device,
        .device_group           = arm_smmu_device_group,
        .of_xlate               = arm_smmu_of_xlate,
        .get_resv_regions       = arm_smmu_get_resv_regions,
        .remove_dev_pasid       = arm_smmu_remove_dev_pasid,
        .dev_enable_feat        = arm_smmu_dev_enable_feature,
        .dev_disable_feat       = arm_smmu_dev_disable_feature,
        .page_response          = arm_smmu_page_response,
        .def_domain_type        = arm_smmu_def_domain_type,
        .pgsize_bitmap          = -1UL, /* Restricted during device attach */
        .owner                  = THIS_MODULE,
        .default_domain_ops = &(const struct iommu_domain_ops) {
                .attach_dev             = arm_smmu_attach_dev,
                .map_pages              = arm_smmu_map_pages,
                .unmap_pages            = arm_smmu_unmap_pages,
                .flush_iotlb_all        = arm_smmu_flush_iotlb_all,
                .iotlb_sync             = arm_smmu_iotlb_sync,
                .iova_to_phys           = arm_smmu_iova_to_phys,
                .enable_nesting         = arm_smmu_enable_nesting,
                .free                   = arm_smmu_domain_free,
        }
};      

static struct iommu_domain *arm_smmu_domain_alloc(unsigned type)
{
        struct arm_smmu_domain *smmu_domain;

        if (type == IOMMU_DOMAIN_SVA)
                return arm_smmu_sva_domain_alloc();

        if (type != IOMMU_DOMAIN_UNMANAGED &&
            type != IOMMU_DOMAIN_DMA &&
            type != IOMMU_DOMAIN_DMA_FQ &&
            type != IOMMU_DOMAIN_IDENTITY)
                return NULL;

        /*
         * Allocate the domain and initialise some of its data structures.
         * We can't really do anything meaningful until we've added a
         * master.
         */
        smmu_domain = kzalloc(sizeof(*smmu_domain), GFP_KERNEL);
        if (!smmu_domain)
                return NULL;
                
        mutex_init(&smmu_domain->init_mutex);
        INIT_LIST_HEAD(&smmu_domain->devices);
        spin_lock_init(&smmu_domain->devices_lock);
        INIT_LIST_HEAD(&smmu_domain->mmu_notifiers);
                
        return &smmu_domain->domain;
}               
```
Now the iommu_domain is allocated to domain->domain.

### Attach new IOMMU domain to the IOMMU group
So far we created the smmu domain for the group!. The generated domain should be
attached to the iommu_group. 

```cpp
static int vfio_iommu_type1_attach_group(void *iommu_data,                          
                struct iommu_group *iommu_group, enum vfio_group_type type)     
{         
	......
        ret = iommu_attach_group(domain->domain, group->iommu_group);
        if (ret)
                goto out_domain;
```

```cpp
/**     
 * iommu_attach_group - Attach an IOMMU domain to an IOMMU group
 * @domain: IOMMU domain to attach
 * @group: IOMMU group that will be attached
 */
int iommu_attach_group(struct iommu_domain *domain, struct iommu_group *group)
{               
        int ret;
                
        mutex_lock(&group->mutex);
        ret = __iommu_attach_group(domain, group);
        mutex_unlock(&group->mutex);
        
        return ret;
}               

static int __iommu_attach_group(struct iommu_domain *domain,
                                struct iommu_group *group)
{
        int ret;
        
        if (group->domain && group->domain != group->default_domain &&
            group->domain != group->blocking_domain)
                return -EBUSY;

        ret = __iommu_group_for_each_dev(group, domain,
                                         iommu_group_do_attach_device);
        if (ret == 0)
                group->domain = domain;
                
        return ret;
}       

/*      
 * IOMMU groups are really the natural working unit of the IOMMU, but
 * the IOMMU API works on domains and devices.  Bridge that gap by
 * iterating over the devices in a group.  Ideally we'd have a single
 * device which represents the requestor ID of the group, but we also
 * allow IOMMU drivers to create policy defined minimum sets, where
 * the physical hardware may be able to distiguish members, but we
 * wish to group them at a higher level (ex. untrusted multi-function
 * PCI devices).  Thus we attach each device.
 */
static int iommu_group_do_attach_device(struct device *dev, void *data)
{
        struct iommu_domain *domain = data;
        
        return __iommu_attach_device(domain, dev);
}               
```
The iommu_attach_group function can be explained as an invocation of 
iommu_group_do_attach_device function for all devices in the IOMMU group with 
IOMMU domain as its parameter.

```cpp
static int __iommu_attach_device(struct iommu_domain *domain,
                                 struct device *dev)
{       
        int ret;
                        
        if (unlikely(domain->ops->attach_dev == NULL)) 
                return -ENODEV;
        
        ret = domain->ops->attach_dev(domain, dev);
        if (!ret)
                trace_attach_device_to_domain(dev);
        return ret;
}    
```
Note that the domain passed to the __iommu_attach_device function is the domain
generated as a result of __iommu_domain_alloc-> arm_smmu_domain_alloc function. 
Therefore, its ops is the arm_smmu_ops, and attach_dev will invoke 
arm_smmu_attach_dev.

```cpp
static int arm_smmu_attach_dev(struct iommu_domain *domain, struct device *dev)
{               
        int ret = 0;
        unsigned long flags;
        struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
        struct arm_smmu_device *smmu;   
        struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
        struct arm_smmu_master *master; 
        
        if (!fwspec)
                return -ENOENT;

        master = dev_iommu_priv_get(dev); 
        smmu = master->smmu;       
                                   
        dev_info(dev, "attaching new device!\n");
        /*                         
         * Checking that SVA is disabled ensures that this device isn't bound to
         * any mm, and can be safely detached from its old domain. Bonds cannot
         * be removed concurrently since we're holding the group mutex.
         */
        if (arm_smmu_master_sva_enabled(master)) {
                dev_err(dev, "cannot attach - SVA enabled\n");
                return -EBUSY;
        }                                     
                
        //detach devices of the domain only when the dev needs to be attached to 
        //existing domain
        arm_smmu_detach_dev(master);
        
        mutex_lock(&smmu_domain->init_mutex);
        
        if (!smmu_domain->smmu) {
                smmu_domain->smmu = smmu;
                ret = arm_smmu_domain_finalise(domain, master);
                if (ret) {
                        smmu_domain->smmu = NULL;
                        goto out_unlock;
                }
        } else if (smmu_domain->smmu != smmu) {
                ret = -EINVAL;
                goto out_unlock;
        } else if (smmu_domain->stage == ARM_SMMU_DOMAIN_S1 &&
                   master->ssid_bits != smmu_domain->s1_cfg.s1cdmax) {
                ret = -EINVAL;
                goto out_unlock;
        } else if (smmu_domain->stage == ARM_SMMU_DOMAIN_S1 &&
                   smmu_domain->stall_enabled != master->stall_enabled) {
                ret = -EINVAL;
                goto out_unlock;
        }

        master->domain = smmu_domain;

        if (smmu_domain->stage != ARM_SMMU_DOMAIN_BYPASS)
                master->ats_enabled = arm_smmu_ats_supported(master);

        arm_smmu_install_ste_for_dev(master);

        spin_lock_irqsave(&smmu_domain->devices_lock, flags);
        list_add(&master->domain_head, &smmu_domain->devices);
        spin_unlock_irqrestore(&smmu_domain->devices_lock, flags);

        arm_smmu_enable_ats(master);

out_unlock:
        mutex_unlock(&smmu_domain->init_mutex);
        return ret;
}
```

This function initialize the CD and STE for the device. The detailed information
of the initialize is described in the previous posting [].


###
```cpp
static int vfio_iommu_type1_attach_group(void *iommu_data,
                struct iommu_group *iommu_group, enum vfio_group_type type)
{
	......
        /* Get aperture info */
        geo = &domain->domain->geometry;
        if (vfio_iommu_aper_conflict(iommu, geo->aperture_start,
                                     geo->aperture_end)) {
                ret = -EINVAL;
                goto out_detach;
        }

        ret = iommu_get_group_resv_regions(iommu_group, &group_resv_regions);
        if (ret)
                goto out_detach;

        if (vfio_iommu_resv_conflict(iommu, &group_resv_regions)) {
                ret = -EINVAL;
                goto out_detach;
        }
        
        /*
         * We don't want to work on the original iova list as the list
         * gets modified and in case of failure we have to retain the
         * original list. Get a copy here.
         */ 
        ret = vfio_iommu_iova_get_copy(iommu, &iova_copy);
        if (ret)
                goto out_detach;
        
        ret = vfio_iommu_aper_resize(&iova_copy, geo->aperture_start,
                                     geo->aperture_end);
        if (ret)
                goto out_detach;
        
        ret = vfio_iommu_resv_exclude(&iova_copy, &group_resv_regions);
        if (ret)
                goto out_detach;
        
        resv_msi = vfio_iommu_has_sw_msi(&group_resv_regions, &resv_msi_base);
        
        INIT_LIST_HEAD(&domain->group_list);
        list_add(&group->next, &domain->group_list);
        
        msi_remap = irq_domain_check_msi_remap() ||
                    iommu_group_for_each_dev(iommu_group, (void *)IOMMU_CAP_INTR_REMAP,
                                             vfio_iommu_device_capable);
        
        if (!allow_unsafe_interrupts && !msi_remap) {
                pr_warn("%s: No interrupt remapping support.  Use the module param \"allow_unsafe_interrupts\" to enable VFIO IOMMU support on this platform\n",
                       __func__);
                ret = -EPERM;
                goto out_detach;
        }
        
        /*
         * If the IOMMU can block non-coherent operations (ie PCIe TLPs with
         * no-snoop set) then VFIO always turns this feature on because on Intel
         * platforms it optimizes KVM to disable wbinvd emulation.
         */
        if (domain->domain->ops->enforce_cache_coherency)
                domain->enforce_cache_coherency =
                        domain->domain->ops->enforce_cache_coherency(
                                domain->domain);
        
        /*
         * Try to match an existing compatible domain.  We don't want to
         * preclude an IOMMU driver supporting multiple bus_types and being
         * able to include different bus_types in the same IOMMU domain, so
         * we test whether the domains use the same iommu_ops rather than
         * testing if they're on the same bus_type.
         */
        list_for_each_entry(d, &iommu->domain_list, next) {
                if (d->domain->ops == domain->domain->ops &&
                    d->enforce_cache_coherency ==
                            domain->enforce_cache_coherency) {
                        iommu_detach_group(domain->domain, group->iommu_group);
                        if (!iommu_attach_group(d->domain,
                                                group->iommu_group)) {
                                list_add(&group->next, &d->group_list);
                                iommu_domain_free(domain->domain);
                                kfree(domain);
                                goto done;
                        }
                        
                        ret = iommu_attach_group(domain->domain,
                                                 group->iommu_group);
                        if (ret)
                                goto out_domain;
                }
        }

        vfio_test_domain_fgsp(domain);

        /* replay mappings on new domains */
        ret = vfio_iommu_replay(iommu, domain);
        if (ret)
                goto out_detach;

        if (resv_msi) {
                ret = iommu_get_msi_cookie(domain->domain, resv_msi_base);
                if (ret && ret != -ENODEV)
                        goto out_detach;
        }

        list_add(&domain->next, &iommu->domain_list);
        vfio_update_pgsize_bitmap(iommu);
done:
        /* Delete the old one and insert new iova list */
        vfio_iommu_iova_insert_copy(iommu, &iova_copy);

        /*
         * An iommu backed group can dirty memory directly and therefore
         * demotes the iommu scope until it declares itself dirty tracking
         * capable via the page pinning interface.
         */
        iommu->num_non_pinned_groups++;
        mutex_unlock(&iommu->lock);
        vfio_iommu_resv_free(&group_resv_regions);

        return 0;

out_detach:
        iommu_detach_group(domain->domain, group->iommu_group);
out_domain:
        iommu_domain_free(domain->domain);
        vfio_iommu_iova_free(&iova_copy);
        vfio_iommu_resv_free(&group_resv_regions);
out_free_domain:
        kfree(domain);
out_free_group:
        kfree(group);
out_unlock:
        mutex_unlock(&iommu->lock);
        return ret;
}
```



```cpp
static int __iommu_group_set_domain(struct iommu_group *group,
                                    struct iommu_domain *new_domain)
{       
        int ret;
        
        if (group->domain == new_domain)
                return 0;
        
        /*
         * New drivers should support default domains and so the detach_dev() op
         * will never be called. Otherwise the NULL domain represents some
         * platform specific behavior.
         */
        if (!new_domain) {
                if (WARN_ON(!group->domain->ops->detach_dev))
                        return -EINVAL;
                __iommu_group_for_each_dev(group, group->domain,
                                           iommu_group_do_detach_device);
                group->domain = NULL;
                return 0;
        }
        
        /*
         * Changing the domain is done by calling attach_dev() on the new
         * domain. This switch does not have to be atomic and DMA can be
         * discarded during the transition. DMA must only be able to access
         * either new_domain or group->domain, never something else.
         *
         * Note that this is called in error unwind paths, attaching to a
         * domain that has already been attached cannot fail.
         */ 
        ret = __iommu_group_for_each_dev(group, new_domain,
                                         iommu_group_do_attach_device);
        if (ret)
                return ret;
        group->domain = new_domain;
        return 0;
}
```



## Establish IOMMU mapping for pci (vfio_iommu_driver)
To establish IOMMU mapping for vfio device, vfio_iommu_driver provides service
for user processes. I will go through ioctl function dedicated for this and go
through several functions called up until invoking the function related with 
generating IOMMU mapping. 


### Userspace (kvmtool) invoking VFIO_IOMMU_MAP_DMA
```cpp
static int vfio_container_init(struct kvm *kvm)
{       
        int api, i, ret, iommu_type;;
        
        pr_info("%s: Start\n", __func__);
        /* Create a container for our IOMMU groups */
        vfio_container = open(VFIO_DEV_NODE, O_RDWR);
        if (vfio_container == -1) {
                ret = errno;
                pr_err("Failed to open %s", VFIO_DEV_NODE);
                return ret;
        }
        
        api = ioctl(vfio_container, VFIO_GET_API_VERSION);
        if (api != VFIO_API_VERSION) {
                pr_err("Unknown VFIO API version %d", api);
                return -ENODEV;
        }
        
        iommu_type = vfio_get_iommu_type();
        pr_info("%s: get_iommu_type\n", __func__);
        if (iommu_type < 0) {
                pr_err("VFIO type-1 IOMMU not supported on this platform");
                return iommu_type;
        }
        
        /* Create groups for our devices and add them to the container */
        for (i = 0; i < kvm->cfg.num_vfio_devices; ++i) {
                vfio_devices[i].params = &kvm->cfg.vfio_devices[i];
                ret = vfio_device_init(kvm, &vfio_devices[i]);
                if (ret)
                        return ret;
        }
        pr_info("FINISHING Attaching devices in the group to iommu_group");

        /* Finalise the container */
        pr_info("VFIO_SET_IOMMU");
        if (ioctl(vfio_container, VFIO_SET_IOMMU, iommu_type)) {
                ret = -errno;
                pr_err("Failed to set IOMMU type %d for VFIO container",
                       iommu_type);
                return ret;
        } else {
                pr_info("Using IOMMU type %d for VFIO container", iommu_type);
        }

        return kvm__for_each_mem_bank(kvm, KVM_MEM_TYPE_RAM, vfio_map_mem_bank,
                                      NULL);
}
```

Generally, there would be two KVM_MEM_TYPE_RAM memory banks: one for the guest 
kernel and the other for the para-virtualized space. For those two memory 
regions or more, the vfio_map_mem_bank function is invoked and register GPA in 
IOMMU. Note that the kvmtool maintains all memory region assigned to the VM in 
kvm->mem_banks. Take a look at kvm__register_mem function together to understand
how the kvmtool manages the memory mapped to the vm.

```cpp
static int vfio_map_mem_bank(struct kvm *kvm, struct kvm_mem_bank *bank, void *data)
{                                     
        int ret = 0;    
        struct vfio_iommu_type1_dma_map dma_map = {
                .argsz  = sizeof(dma_map),
                .flags  = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
                .vaddr  = (unsigned long)bank->host_addr,
                .iova   = (u64)bank->guest_phys_addr,
                .size   = bank->size,
        };      
        
        /* Map the guest memory for DMA (i.e. provide isolation) */
        if (ioctl(vfio_container, VFIO_IOMMU_MAP_DMA, &dma_map)) {
                ret = -errno;
                pr_err("Failed to map 0x%llx -> 0x%llx (%llu) for DMA",
                       dma_map.iova, dma_map.vaddr, dma_map.size);
        }

        return ret;
}
```

KVMTOOL iterates all KVM_MEM_TYPE_RAM and invokes ioctl to vfio_container. Note 
that dma_map variable passes memory information that needs to be mapped by the 
IOMMU. Note that the guest_phys_addr, IPA of guest VM, is passed as the iova,
and host_addr, HVA mapped to that GPA is passed as vaddr. When kernel receives 
the IPA -> HVA addresses pair, it can retrieve the physical address mapped to 
the vaddr and bind iova(GPA) to HPA in IOMMU mapping so that device can access 
HPA through the IPA. We will see!

### kernel-side handling for VFIO_IOMMU_MAP_DMA
The IOMMU should be controlled by the host kernel, and VFIO_IOMMU_MAP_DMA ioctl
function handles the IOMMU mapping request from the user. 

```cpp
static long vfio_fops_unl_ioctl(struct file *filep,
                                unsigned int cmd, unsigned long arg)
{       
        struct vfio_container *container = filep->private_data;
        struct vfio_iommu_driver *driver;
        void *data;
        long ret = -EINVAL;
        
        if (!container)
                return ret;
        
        switch (cmd) {
        case VFIO_GET_API_VERSION:
                ret = VFIO_API_VERSION;
                break;
        case VFIO_CHECK_EXTENSION:
                ret = vfio_container_ioctl_check_extension(container, arg);
                break;
        case VFIO_SET_IOMMU:
                ret = vfio_ioctl_set_iommu(container, arg);
                break;
        default:
                driver = container->iommu_driver;
                data = container->iommu_data;
                
                if (driver) /* passthrough all unrecognized ioctls */
                        ret = driver->ops->ioctl(data, cmd, arg);
        }
        
        return ret;
}
```

Above function is the ioctl handling function of the vfio container driver. When
VFIO_IOMMU_MAP_DMA ioctl is requested, it will be considered as default case 
because there is no exact matching case. Since container has registered iommu 
driver, its ioctl handling function will be invoked.

```cpp
static long vfio_iommu_type1_ioctl(void *iommu_data,
                                   unsigned int cmd, unsigned long arg)
{               
        struct vfio_iommu *iommu = iommu_data;
                
        switch (cmd) {
        case VFIO_CHECK_EXTENSION:
                return vfio_iommu_type1_check_extension(iommu, arg);
        case VFIO_IOMMU_GET_INFO: 
                return vfio_iommu_type1_get_info(iommu, arg);
        case VFIO_IOMMU_MAP_DMA:
                return vfio_iommu_type1_map_dma(iommu, arg);
        case VFIO_IOMMU_UNMAP_DMA:  
                return vfio_iommu_type1_unmap_dma(iommu, arg);
        case VFIO_IOMMU_DIRTY_PAGES:
                return vfio_iommu_type1_dirty_pages(iommu, arg);
        default: 
                return -ENOTTY;
        }
}       

static int vfio_iommu_type1_map_dma(struct vfio_iommu *iommu,
                                    unsigned long arg)
{               
        struct vfio_iommu_type1_dma_map map;
        unsigned long minsz;
        uint32_t mask = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE |
                        VFIO_DMA_MAP_FLAG_VADDR;
        
        minsz = offsetofend(struct vfio_iommu_type1_dma_map, size);
        
        if (copy_from_user(&map, (void __user *)arg, minsz))
                return -EFAULT;

        if (map.argsz < minsz || map.flags & ~mask) 
                return -EINVAL;
        
        return vfio_dma_do_map(iommu, &map);
}       
```

### Main function to do DMA for guest vm mem
IOMMU allows the device to access HPA through the IOVA. As VFIO allows the guest 
to configure device to utilize address in GPA, IOMMU should have a valid mapping 
translating GPA (iova) to HPA (hpa mapped to GPA) to allow device access guest 
memory accesses. 
 
Since we only provided information about GPA and its HVA, kernel should retrieve
the HPA mapped to the GPA. By walking the page table, it can easily retrieve the 
mapped HPA. 

```cpp
static int vfio_dma_do_map(struct vfio_iommu *iommu,
                           struct vfio_iommu_type1_dma_map *map)
{
        bool set_vaddr = map->flags & VFIO_DMA_MAP_FLAG_VADDR;
        dma_addr_t iova = map->iova;
        unsigned long vaddr = map->vaddr;
        size_t size = map->size;
        int ret = 0, prot = 0;
        size_t pgsize;
        struct vfio_dma *dma;

        /* Verify that none of our __u64 fields overflow */
        if (map->size != size || map->vaddr != vaddr || map->iova != iova)
                return -EINVAL;

        /* READ/WRITE from device perspective */
        if (map->flags & VFIO_DMA_MAP_FLAG_WRITE)

                prot |= IOMMU_WRITE;
        if (map->flags & VFIO_DMA_MAP_FLAG_READ)
                prot |= IOMMU_READ;

        if ((prot && set_vaddr) || (!prot && !set_vaddr))
                return -EINVAL;

        mutex_lock(&iommu->lock);

        pgsize = (size_t)1 << __ffs(iommu->pgsize_bitmap);

        WARN_ON((pgsize - 1) & PAGE_MASK);
        
        if (!size || (size | iova | vaddr) & (pgsize - 1)) {
                ret = -EINVAL;  
                goto out_unlock;
        }
        
        /* Don't allow IOVA or virtual address wrap */
        if (iova + size - 1 < iova || vaddr + size - 1 < vaddr) {
                ret = -EINVAL;
                goto out_unlock;
        }

        dma = vfio_find_dma(iommu, iova, size);
        if (set_vaddr) {
                if (!dma) {
                        ret = -ENOENT;
                } else if (!dma->vaddr_invalid || dma->iova != iova ||
                           dma->size != size) {
                        ret = -EINVAL;
                } else {
                        dma->vaddr = vaddr;
                        dma->vaddr_invalid = false;
                        iommu->vaddr_invalid_count--;
                        wake_up_all(&iommu->vaddr_wait);
                }
                goto out_unlock;
        } else if (dma) {
                ret = -EEXIST;
                goto out_unlock;
        }

        if (!iommu->dma_avail) {
                ret = -ENOSPC;
                goto out_unlock;
        }

        if (!vfio_iommu_iova_dma_valid(iommu, iova, iova + size - 1)) {
                ret = -EINVAL;
                goto out_unlock;
        }

        dma = kzalloc(sizeof(*dma), GFP_KERNEL);
        if (!dma) {
                ret = -ENOMEM;
                goto out_unlock;
        }

        iommu->dma_avail--;
        dma->iova = iova;
        dma->vaddr = vaddr;
        dma->prot = prot;

        /*
         * We need to be able to both add to a task's locked memory and test
         * against the locked memory limit and we need to be able to do both
         * outside of this call path as pinning can be asynchronous via the
         * external interfaces for mdev devices.  RLIMIT_MEMLOCK requires a
         * task_struct and VM locked pages requires an mm_struct, however
         * holding an indefinite mm reference is not recommended, therefore we
         * only hold a reference to a task.  We could hold a reference to
         * current, however QEMU uses this call path through vCPU threads,
         * which can be killed resulting in a NULL mm and failure in the unmap
         * path when called via a different thread.  Avoid this problem by
         * using the group_leader as threads within the same group require
         * both CLONE_THREAD and CLONE_VM and will therefore use the same
         * mm_struct.
         *
         * Previously we also used the task for testing CAP_IPC_LOCK at the
         * time of pinning and accounting, however has_capability() makes use
         * of real_cred, a copy-on-write field, so we can't guarantee that it
         * matches group_leader, or in fact that it might not change by the
         * time it's evaluated.  If a process were to call MAP_DMA with
         * CAP_IPC_LOCK but later drop it, it doesn't make sense that they
         * possibly see different results for an iommu_mapped vfio_dma vs
         * externally mapped.  Therefore track CAP_IPC_LOCK in vfio_dma at the
         * time of calling MAP_DMA.
         */
        get_task_struct(current->group_leader);
        dma->task = current->group_leader;
        dma->lock_cap = capable(CAP_IPC_LOCK);

        dma->pfn_list = RB_ROOT;

        /* Insert zero-sized and grow as we map chunks of it */
        vfio_link_dma(iommu, dma);

        /* Don't pin and map if container doesn't contain IOMMU capable domain*/
        if (list_empty(&iommu->domain_list))
                dma->size = size;
        else
                ret = vfio_pin_map_dma(iommu, dma, size);

        if (!ret && iommu->dirty_page_tracking) {
                ret = vfio_dma_bitmap_alloc(dma, pgsize);
                if (ret)
                        vfio_remove_dma(iommu, dma);
        }

out_unlock:
        mutex_unlock(&iommu->lock);
        return ret;
}
```

The function vfio_dma_do_map is responsible for pinning the physical pages of 
the HVA used by kvmtool. It then invokes vfio_iommu_map to perform the mapping 
of IOVA to HPA. This function subsequently calls iommu_map and, finally, the map
function within the iommu_ops. 

\XXX{vfio_pin_map_dma -> vfio_pin_pages_remote need to be analyzed} 





## Register VFIO BAR as KVM memory
To generate the mapping from the **GPA (IPA) to the BAR region (HPA)** of the 
platform, the KVM should have mapping from the GPA to HVA. In details, when the 
guest exits due to accessing the BAR region of the guest (emulated), it exits to
the host, and host checks if there is a memslot that can translate the faultin 
IPA. IF there is a memslot, then the KVM can map the faultin IPA to the HPA!
The most important job of the vfio device is to generate this memslot mapping.

To allow the guest VM to access the BAR regions, vfio-pci driver maps the BAR 
region in HPA and generate HVA (through ioremap). Then it asks the KVM driver 
to generate memslot for IPA (mapped for the guest BAR) to the generated HVA. 
Although it passes the HVA, it will be translated to the GPA by the KVM and the 
mapping can be successfully established. 

### Configure vfio devices focusing on pci
```cpp
static int vfio__init(struct kvm *kvm)
{       
        int ret;
        
        if (!kvm->cfg.num_vfio_devices)
                return 0;
        
        vfio_devices = calloc(kvm->cfg.num_vfio_devices, sizeof(*vfio_devices));
        if (!vfio_devices)
                return -ENOMEM;
                
        ret = vfio_container_init(kvm);
        if (ret)
                return ret;
        
        ret = vfio_configure_groups(kvm);
        if (ret)
                return ret;
                
        ret = vfio_configure_devices(kvm);
        if (ret)
                return ret;
        
        return 0;
}       
```

```cpp
static int vfio_configure_devices(struct kvm *kvm)
{
        int i, ret;

        for (i = 0; i < kvm->cfg.num_vfio_devices; ++i) {
                ret = vfio_configure_device(kvm, &vfio_devices[i]);
                if (ret)
                        return ret;
        }

        return 0;
}
```

```cpp
static int vfio_configure_device(struct kvm *kvm, struct vfio_device *vdev)
{       
        int ret;
        struct vfio_group *group = vdev->group;
        
        vdev->fd = ioctl(group->fd, VFIO_GROUP_GET_DEVICE_FD,
                         vdev->params->name);
        if (vdev->fd < 0) {
                vfio_dev_warn(vdev, "failed to get fd");
                
                /* The device might be a bridge without an fd */
                return 0;
        }
        
        vdev->info.argsz = sizeof(vdev->info);
        if (ioctl(vdev->fd, VFIO_DEVICE_GET_INFO, &vdev->info)) {
                ret = -errno;
                vfio_dev_err(vdev, "failed to get info");
                goto err_close_device;
        }
        
        if (vdev->info.flags & VFIO_DEVICE_FLAGS_RESET &&
            ioctl(vdev->fd, VFIO_DEVICE_RESET) < 0)
                vfio_dev_warn(vdev, "failed to reset device");
        
        vdev->regions = calloc(vdev->info.num_regions, sizeof(*vdev->regions));
        if (!vdev->regions) {
                ret = -ENOMEM;
                goto err_close_device;
        }
        
        /* Now for the bus-specific initialization... */
        switch (vdev->params->type) {
        case VFIO_DEVICE_PCI:
                BUG_ON(!(vdev->info.flags & VFIO_DEVICE_FLAGS_PCI));
                ret = vfio_pci_setup_device(kvm, vdev);
                break;
        default:
                BUG_ON(1);
                ret = -EINVAL;
        }
        
        if (ret)
                goto err_free_regions;
        
        vfio_dev_info(vdev, "assigned to device number 0x%x in group %lu",
                      vdev->dev_hdr.dev_num, group->id);
        
        return 0;

err_free_regions:
        free(vdev->regions);
err_close_device:
        close(vdev->fd);
        
        return ret;
}       
```
First of all, it invokes VFIO_GROUP_GET_DEVICE_FD ioctl to get the descriptor 
of the device. Through this ioctl call, kernel opens the device file matching 
the provided device name in the group. If there is matching device, it returns
the associated file descriptor. The return value will be stored in vdev->fd.

### Retrieve PCIe specific information of the device
To configure the vfio devices, it needs extra information about the vfio device.
Through VFIO_DEVICE_GET_INFO ioctl it can retrieve information of the vfio
device. Because we assume that the target device is bind to vfio-pci driver, it 
will invoke vfio_pci_ioctl_get_info which is the handler function of the 
VFIO_DEVICE_GET_INFO


```cpp
static int vfio_pci_ioctl_get_info(struct vfio_pci_core_device *vdev,
                                   struct vfio_device_info __user *arg)
{       
        unsigned long minsz = offsetofend(struct vfio_device_info, num_irqs);
        struct vfio_device_info info;
        struct vfio_info_cap caps = { .buf = NULL, .size = 0 };
        unsigned long capsz;
        int ret;
        
        /* For backward compatibility, cannot require this */
        capsz = offsetofend(struct vfio_iommu_type1_info, cap_offset);
                
        if (copy_from_user(&info, arg, minsz))
                return -EFAULT;
        
        if (info.argsz < minsz)
                return -EINVAL;
                
        if (info.argsz >= capsz) {
                minsz = capsz;
                info.cap_offset = 0;
        }

        info.flags = VFIO_DEVICE_FLAGS_PCI;

        if (vdev->reset_works)
                info.flags |= VFIO_DEVICE_FLAGS_RESET;
        
        info.num_regions = VFIO_PCI_NUM_REGIONS + vdev->num_regions;
        info.num_irqs = VFIO_PCI_NUM_IRQS;
        
        ret = vfio_pci_info_zdev_add_caps(vdev, &caps);
        if (ret && ret != -ENODEV) {
                pci_warn(vdev->pdev,
                         "Failed to setup zPCI info capabilities\n");
                return ret;
        }

        if (caps.size) {
                info.flags |= VFIO_DEVICE_FLAGS_CAPS;
                if (info.argsz < sizeof(info) + caps.size) {
                        info.argsz = sizeof(info) + caps.size;
                } else {
                        vfio_info_cap_shift(&caps, sizeof(info));
                        if (copy_to_user(arg + 1, caps.buf, caps.size)) {
                                kfree(caps.buf);
                                return -EFAULT;
                        }
                        info.cap_offset = sizeof(*arg);
                }

                kfree(caps.buf);
        }

        return copy_to_user(arg, &info, minsz) ? -EFAULT : 0;
}

```
The information includes the number of regions and IRQ assigned to the device. 
It allocates the regions array and assign it to vdev. 


### Setup PCIe specific information 
After retrieving the device information from the kernel side, it invokes the 
vfio_pci_setup_device if the device's type is VFIO_DEVICE_PCI. As PCIe device 
has its own semantic such as PCI config space and BAR used in communication with
the device, we will focus on how vfio-pci device driver allows the VM to 
communicate seamlessly through those channels. 


```cpp
static int vfio_configure_device(struct kvm *kvm, struct vfio_device *vdev)
{       
	......
        /* Now for the bus-specific initialization... */
        switch (vdev->params->type) {
        case VFIO_DEVICE_PCI:
                BUG_ON(!(vdev->info.flags & VFIO_DEVICE_FLAGS_PCI));
                ret = vfio_pci_setup_device(kvm, vdev);
	......
}
```


```cpp
int vfio_pci_setup_device(struct kvm *kvm, struct vfio_device *vdev)
{       
        int ret;
        
        ret = vfio_pci_configure_dev_regions(kvm, vdev);
        if (ret) {                        
                vfio_dev_err(vdev, "failed to configure regions");
                return ret;
        }
        
        vdev->dev_hdr = (struct device_header) {
                .bus_type       = DEVICE_BUS_PCI,
                .data           = &vdev->pci.hdr,
        };
        
        ret = device__register(&vdev->dev_hdr);
        if (ret) {
                vfio_dev_err(vdev, "failed to register VFIO device");
                return ret;
        }       
                        
        ret = vfio_pci_configure_dev_irqs(kvm, vdev);
        if (ret) {
                vfio_dev_err(vdev, "failed to configure IRQs");
                return ret;
        }       
                        
        return 0;
}
```
Note that this function initialize information of the pdev such as regions. 

### Brief tutorial about the VFIO bus drivers.
Before we take a look at how kvmtool communicate with the vfio-pci driver to 
retrieve further information and set-up the device, we should understand how the
vfio device driver communicate with the user-space kvmtool, which is the ioctl
and device operations through the device file. Read below descriptions of the 
vfio driver carefully, excerpted from the kernel documentation.

```cpp
VFIO bus drivers, such as vfio-pci make use of only a few interfaces
into VFIO core.  When devices are bound and unbound to the driver,
the driver should call vfio_register_group_dev() and
vfio_unregister_group_dev() respectively::

        void vfio_init_group_dev(struct vfio_device *device,
                                struct device *dev,
                                const struct vfio_device_ops *ops);
        void vfio_uninit_group_dev(struct vfio_device *device);
        int vfio_register_group_dev(struct vfio_device *device);
        void vfio_unregister_group_dev(struct vfio_device *device);

The driver should embed the vfio_device in its own structure and call
vfio_init_group_dev() to pre-configure it before going to registration
and call vfio_uninit_group_dev() after completing the un-registration.
vfio_register_group_dev() indicates to the core to begin tracking the
iommu_group of the specified dev and register the dev as owned by a VFIO bus
driver. Once vfio_register_group_dev() returns it is possible for userspace to
start accessing the driver, thus the driver should ensure it is completely
ready before calling it. The driver provides an ops structure for callbacks
similar to a file operations structure::

        struct vfio_device_ops {
                int     (*open)(struct vfio_device *vdev);
                void    (*release)(struct vfio_device *vdev);
                ssize_t (*read)(struct vfio_device *vdev, char __user *buf,
                                size_t count, loff_t *ppos);
                ssize_t (*write)(struct vfio_device *vdev,
                                 const char __user *buf,
                                 size_t size, loff_t *ppos);
                long    (*ioctl)(struct vfio_device *vdev, unsigned int cmd,
                                 unsigned long arg);
                int     (*mmap)(struct vfio_device *vdev,
                                struct vm_area_struct *vma);
        };

Each function is passed the vdev that was originally registered
in the vfio_register_group_dev() call above.  This allows the bus driver
to obtain its private data using container_of().  The open/release
callbacks are issued when a new file descriptor is created for a
device (via VFIO_GROUP_GET_DEVICE_FD).  The ioctl interface provides
a direct pass through for VFIO_DEVICE_* ioctls.  The read/write/mmap
interfaces implement the device region access defined by the device's
own VFIO_DEVICE_GET_REGION_INFO ioctl.
```

Based on the documentation, we can understand that the device file serve as an
very important interface between the user and kernel allowing the user (e.g., 
kvmtool) to access the device specific information. Below vfio_pci_ops is 
registered as a vfio_device_ops of the vfio-pci device driver. 

```cpp
static const struct vfio_device_ops vfio_pci_ops = {
        .name           = "vfio-pci",
        .init           = vfio_pci_core_init_dev,
        .release        = vfio_pci_core_release_dev,
        .open_device    = vfio_pci_open_device,
        .close_device   = vfio_pci_core_close_device,
        .ioctl          = vfio_pci_core_ioctl,
        .device_feature = vfio_pci_core_ioctl_feature,
        .read           = vfio_pci_core_read,
        .write          = vfio_pci_core_write,
        .mmap           = vfio_pci_core_mmap,
        .request        = vfio_pci_core_request,
        .match          = vfio_pci_core_match,
        .bind_iommufd   = vfio_iommufd_physical_bind,
        .unbind_iommufd = vfio_iommufd_physical_unbind,
        .attach_ioas    = vfio_iommufd_physical_attach_ioas,
};
```

### Retrieve PCI header information 
Let's see how the vfio-pci driver allows the user to access PCIe header info
through the vfio-pci device driver. 

```cpp
static int vfio_pci_configure_dev_regions(struct kvm *kvm,
                                          struct vfio_device *vdev)
{       
        int ret;
        u32 bar;
        size_t i;
        bool is_64bit = false; 
        struct vfio_pci_device *pdev = &vdev->pci;
        
        ret = vfio_pci_parse_cfg_space(vdev);
        if (ret)
                return ret;
        
        if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSIX) {
                ret = vfio_pci_create_msix_table(kvm, vdev);
                if (ret)
                        return ret;
        }
        
        if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSI) {
                ret = vfio_pci_create_msi_cap(kvm, pdev);
                if (ret)
                        return ret;
        }
        
        for (i = VFIO_PCI_BAR0_REGION_INDEX; i <= VFIO_PCI_BAR5_REGION_INDEX; ++i) {
                /* Ignore top half of 64-bit BAR */
                if (is_64bit) {
                        is_64bit = false;
                        continue;
                }
                
                ret = vfio_pci_configure_bar(kvm, vdev, i);
                if (ret)
                        return ret;
                
                bar = pdev->hdr.bar[i];
                is_64bit = (bar & PCI_BASE_ADDRESS_SPACE) ==
                           PCI_BASE_ADDRESS_SPACE_MEMORY &&
                           bar & PCI_BASE_ADDRESS_MEM_TYPE_64;
        }
        
        /* We've configured the BARs, fake up a Configuration Space */
        ret = vfio_pci_fixup_cfg_space(vdev);
        if (ret)
                return ret;
        
        return pci__register_bar_regions(kvm, &pdev->hdr, vfio_pci_bar_activate,
                                         vfio_pci_bar_deactivate, vdev);
}
```

### Retrieving PCI config space info (VFIO_DEVICE_GET_REGION_INFO)
```cpp
static int vfio_pci_parse_cfg_space(struct vfio_device *vdev)
{                                         
        ssize_t sz = PCI_DEV_CFG_SIZE_LEGACY;
        struct vfio_region_info *info;
        struct vfio_pci_device *pdev = &vdev->pci;
        
        if (vdev->info.num_regions < VFIO_PCI_CONFIG_REGION_INDEX) {
                vfio_dev_err(vdev, "Config Space not found");
                return -ENODEV;
        }
        
        info = &vdev->regions[VFIO_PCI_CONFIG_REGION_INDEX].info;
        *info = (struct vfio_region_info) {
                        .argsz = sizeof(*info),
                        .index = VFIO_PCI_CONFIG_REGION_INDEX,
        };      
                        
        ioctl(vdev->fd, VFIO_DEVICE_GET_REGION_INFO, info);
        if (!info->size) {
                vfio_dev_err(vdev, "Config Space has size zero?!");
                return -EINVAL;
        }       
                        
        /* Read standard headers and capabilities */
        if (pread(vdev->fd, &pdev->hdr, sz, info->offset) != sz) {
                vfio_dev_err(vdev, "failed to read %zd bytes of Config Space", sz);
                return -EIO;
        }       
                        
        /* Strip bit 7, that indicates multifunction */
        pdev->hdr.header_type &= 0x7f;

        if (pdev->hdr.header_type != PCI_HEADER_TYPE_NORMAL) {
                vfio_dev_err(vdev, "unsupported header type %u",
                             pdev->hdr.header_type);
                return -EOPNOTSUPP;
        }       

        if (pdev->hdr.irq_pin)
                pdev->irq_modes |= VFIO_PCI_IRQ_MODE_INTX;

        vfio_pci_parse_caps(vdev);

        return 0;
}
```

As described in [[]], reading pci device specific information is achieved 
through the registered file operations such as read (pread in this code). When 
it passes proper offset of the region that wants to read through the read system
call to the file descriptor of the vfio-pci device, the kernel driver will read 
information from the PCI device memory. 

The proper offset of the region it wants to read can be retrieved from the 
vfio-pci driver through the ioctl call (VFIO_DEVICE_GET_REGION_INFO). In this 
case, it wants to read PCI header, so it configures index vfio_region_info as 
VFIO_PCI_CONFIG_REGION_INDEX and pass it to the driver.

**Kernel side of the VFIO_DEVICE_GET_REGION_INFO**
```cpp
static int vfio_pci_ioctl_get_region_info(struct vfio_pci_core_device *vdev,
                                          struct vfio_region_info __user *arg)
{
        unsigned long minsz = offsetofend(struct vfio_region_info, offset);
        struct pci_dev *pdev = vdev->pdev;
        struct vfio_region_info info;
        struct vfio_info_cap caps = { .buf = NULL, .size = 0 };
        int i, ret;

        if (copy_from_user(&info, arg, minsz))
                return -EFAULT;

        if (info.argsz < minsz)
                return -EINVAL;

        switch (info.index) {
        case VFIO_PCI_CONFIG_REGION_INDEX:
                info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
                info.size = pdev->cfg_size;
                info.flags = VFIO_REGION_INFO_FLAG_READ |
                             VFIO_REGION_INFO_FLAG_WRITE;
                break;
        case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
                info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
                info.size = pci_resource_len(pdev, info.index);
                if (!info.size) {
                        info.flags = 0;
                        break;
                }

                info.flags = VFIO_REGION_INFO_FLAG_READ |
                             VFIO_REGION_INFO_FLAG_WRITE;
                if (vdev->bar_mmap_supported[info.index]) {
                        info.flags |= VFIO_REGION_INFO_FLAG_MMAP;
                        if (info.index == vdev->msix_bar) {
                                ret = msix_mmappable_cap(vdev, &caps);
                                if (ret)
                                        return ret;

					}
                }

                break;
        case VFIO_PCI_ROM_REGION_INDEX: {
                void __iomem *io;
                size_t size;
                u16 cmd;

                info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
                info.flags = 0;

                /* Report the BAR size, not the ROM size */
                info.size = pci_resource_len(pdev, info.index);
                if (!info.size) {
                        /* Shadow ROMs appear as PCI option ROMs */
                        if (pdev->resource[PCI_ROM_RESOURCE].flags &
                            IORESOURCE_ROM_SHADOW)
                                info.size = 0x20000;
                        else
                                break;
                }

                /*
                 * Is it really there?  Enable memory decode for implicit access
                 * in pci_map_rom().
                 */
                cmd = vfio_pci_memory_lock_and_enable(vdev);
                io = pci_map_rom(pdev, &size);
                if (io) {
                        info.flags = VFIO_REGION_INFO_FLAG_READ;
                        pci_unmap_rom(pdev, io);
                } else {
                        info.size = 0;
                }
                vfio_pci_memory_unlock_and_restore(vdev, cmd);

                break;
        }
        case VFIO_PCI_VGA_REGION_INDEX:
                if (!vdev->has_vga)
                        return -EINVAL;

                info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
                info.size = 0xc0000;
                info.flags = VFIO_REGION_INFO_FLAG_READ |
                             VFIO_REGION_INFO_FLAG_WRITE;

                break;
        default: {
                struct vfio_region_info_cap_type cap_type = {
                        .header.id = VFIO_REGION_INFO_CAP_TYPE,
                        .header.version = 1
                };
                
                if (info.index >= VFIO_PCI_NUM_REGIONS + vdev->num_regions)
                        return -EINVAL;
                info.index = array_index_nospec(
                        info.index, VFIO_PCI_NUM_REGIONS + vdev->num_regions);
                
                i = info.index - VFIO_PCI_NUM_REGIONS;
                
                info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
                info.size = vdev->region[i].size;
                info.flags = vdev->region[i].flags;
                
                cap_type.type = vdev->region[i].type;
                cap_type.subtype = vdev->region[i].subtype;
                
                ret = vfio_info_add_capability(&caps, &cap_type.header,
                                               sizeof(cap_type));
                if (ret)
                        return ret;
                
                if (vdev->region[i].ops->add_capability) {
                        ret = vdev->region[i].ops->add_capability(
                                vdev, &vdev->region[i], &caps);
                        if (ret)
                                return ret;
                }
        }
        }
        
        if (caps.size) {
                info.flags |= VFIO_REGION_INFO_FLAG_CAPS;
                if (info.argsz < sizeof(info) + caps.size) {
                        info.argsz = sizeof(info) + caps.size;
                        info.cap_offset = 0;
                } else {
                        vfio_info_cap_shift(&caps, sizeof(info));
                        if (copy_to_user(arg + 1, caps.buf, caps.size)) {
                                kfree(caps.buf);
                                return -EFAULT;
                        }
                        info.cap_offset = sizeof(*arg);
                }
                
                kfree(caps.buf);
        }
        
        return copy_to_user(arg, &info, minsz) ? -EFAULT : 0;
}
```
The kernel driver simply returns the offset, size, and flags information of 
different regions to the user. 

### Reading PCI memory through pread
After retrieving the offset of region that we want to read from the PCI device, 
we can now read the PCI information indirectly thorough the pread to the file 
descriptor of the target vfio-pci device. Let's see the how kernel allows the 
user to read header information through read. 

```cpp
ssize_t vfio_pci_core_read(struct vfio_device *core_vdev, char __user *buf,
                size_t count, loff_t *ppos)
{       
        struct vfio_pci_core_device *vdev =
                container_of(core_vdev, struct vfio_pci_core_device, vdev);
        
        if (!count)     
                return 0; 
        
        return vfio_pci_rw(vdev, buf, count, ppos, false);
}       

vfio_pci_core_read function is called when the read system call is invoked 
through the vfio-pci device. Refer to [[]]. The actual read of the pci memory
is accomplished by vfio_pci_rw. 

static ssize_t vfio_pci_rw(struct vfio_pci_core_device *vdev, char __user *buf,
                           size_t count, loff_t *ppos, bool iswrite)
{       
        unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
        int ret;
        
        if (index >= VFIO_PCI_NUM_REGIONS + vdev->num_regions)
                return -EINVAL;
        
        ret = pm_runtime_resume_and_get(&vdev->pdev->dev);
        if (ret) {
                pci_info_ratelimited(vdev->pdev, "runtime resume failed %d\n",
                                     ret);
                return -EIO;
        }
        
        switch (index) {
        case VFIO_PCI_CONFIG_REGION_INDEX:
                ret = vfio_pci_config_rw(vdev, buf, count, ppos, iswrite);
                break;
        
        case VFIO_PCI_ROM_REGION_INDEX:
                if (iswrite)
                        ret = -EINVAL;
                else    
                        ret = vfio_pci_bar_rw(vdev, buf, count, ppos, false);
                break;
        
        case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
                ret = vfio_pci_bar_rw(vdev, buf, count, ppos, iswrite);
                break;
        
        case VFIO_PCI_VGA_REGION_INDEX:
                ret = vfio_pci_vga_rw(vdev, buf, count, ppos, iswrite);
                break;
        
        default:
                index -= VFIO_PCI_NUM_REGIONS;
                ret = vdev->region[index].ops->rw(vdev, buf,
                                                   count, ppos, iswrite);
                break;
        }
        
        pm_runtime_put(&vdev->pdev->dev);
        return ret;
}
```

As described, based on the offset, switch allows user process to read different
information of the pci device. Because the current request wants to read the 
pci header information, vfio_pci_config_rw will be invoked and read PCI info. 
I will not describe further details. 

### PCI device header
Note that pread passes pointer of the pdev->hdr to store the header information.

```cpp
struct pci_device_header {              
        /* Configuration space, as seen by the guest */
        union { 
                struct { 
                        u16             vendor_id;
                        u16             device_id; 
                        u16             command;
                        u16             status;
                        u8              revision_id;
                        u8              class[3];
                        u8              cacheline_size;
                        u8              latency_timer;
                        u8              header_type;
                        u8              bist;
                        u32             bar[6];
                        u32             card_bus;
                        u16             subsys_vendor_id;
                        u16             subsys_id;
                        u32             exp_rom_bar;
                        u8              capabilities;
                        u8              reserved1[3];
                        u32             reserved2;
                        u8              irq_line;
                        u8              irq_pin;
                        u8              min_gnt;
                        u8              max_lat;
                        struct msix_cap msix;
                        /* Used only by architectures which support PCIE */
                        struct pci_exp_cap pci_exp;
                        struct virtio_caps virtio;
                } __attribute__((packed));
                /* Pad to PCI config space size */
                u8      __pad[PCI_DEV_CFG_SIZE];
        };
        
        /* Private to lkvm */
        u32                     bar_size[6];
        bool                    bar_active[6];
        bar_activate_fn_t       bar_activate_fn;
        bar_deactivate_fn_t     bar_deactivate_fn;
        void *data;
        struct pci_config_operations    cfg_ops;
        /*
         * PCI INTx# are level-triggered, but virtual device often feature
         * edge-triggered INTx# for convenience.
         */
        enum irq_type   irq_type;
};
```
After the pread, the kvmtool can access the PCI device information through the
pdev->hdr without accessing the PCI device config space everytime. 

### Read BAR Information from pcie
As we ask the vfio-pci device driver to read pci device specific information,
such as the pci header, we can read the PCI BAR info through the same way. 
Lets go back to vfio_pci_configure_dev_regions and see how it retrieves the 
PCI BAR information.

```cpp
static int vfio_pci_configure_dev_regions(struct kvm *kvm,                      
                                          struct vfio_device *vdev)             
{                                                                               
	......
        for (i = VFIO_PCI_BAR0_REGION_INDEX; i <= VFIO_PCI_BAR5_REGION_INDEX; ++i) {
                /* Ignore top half of 64-bit BAR */                             
                if (is_64bit) {                                                 
                        is_64bit = false;                                       
                        continue;                                               
                }                                                               
                                                                                
                ret = vfio_pci_configure_bar(kvm, vdev, i);                     
                if (ret)                                                        
                        return ret;                                             
                                                                                
                bar = pdev->hdr.bar[i];                                         
                is_64bit = (bar & PCI_BASE_ADDRESS_SPACE) ==                    
                           PCI_BASE_ADDRESS_SPACE_MEMORY &&                     
                           bar & PCI_BASE_ADDRESS_MEM_TYPE_64;                  
        }                                                     
	......
```


```cpp
static int vfio_pci_configure_bar(struct kvm *kvm, struct vfio_device *vdev,
                                  size_t nr)
{                       
        int ret;
        u32 bar;
        size_t map_size;
        struct vfio_pci_device *pdev = &vdev->pci;
        struct vfio_region *region;

        if (nr >= vdev->info.num_regions)
                return 0;
        
        region = &vdev->regions[nr];
        bar = pdev->hdr.bar[nr];
        
        region->vdev = vdev;
        region->is_ioport = !!(bar & PCI_BASE_ADDRESS_SPACE_IO);
        
        ret = vfio_pci_get_region_info(vdev, nr, &region->info);
        if (ret) 
                return ret;
                        
        /* Ignore invalid or unimplemented regions */
        if (!region->info.size)
                return 0;
                
        if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSIX) {
                /* Trap and emulate MSI-X table */
                if (nr == pdev->msix_table.bar) {
                        region->guest_phys_addr = pdev->msix_table.guest_phys_addr;
                        return 0;
                } else if (nr == pdev->msix_pba.bar) {
                        region->guest_phys_addr = pdev->msix_pba.guest_phys_addr;
                        return 0;
                }
        }                                
                
        if (region->is_ioport) {
                region->port_base = pci_get_io_port_block(region->info.size);
        } else {
                /* Grab some MMIO space in the guest */
                map_size = ALIGN(region->info.size, PAGE_SIZE);
                region->guest_phys_addr = pci_get_mmio_block(map_size);
        }
                        
        return 0;
}
```

Although the name is confusing to make reader misunderstand the above function, 
the main role of it is retrieving information of BARs of the PCIe device not 
modifying/configuring BARs. The information is retrieved through ioctl
VFIO_DEVICE_GET_REGION_INFO and stored in the regions field of the vfio_device. 
The end result we will have is the information of BARs saved in dev->regions. 

```cpp
static int vfio_pci_get_region_info(struct vfio_device *vdev, u32 index,
                                    struct vfio_region_info *info)
{
        int ret;

        *info = (struct vfio_region_info) {
                .argsz = sizeof(*info),
                .index = index,
        };

        ret = ioctl(vdev->fd, VFIO_DEVICE_GET_REGION_INFO, info);
        if (ret) {
                ret = -errno;
                vfio_dev_err(vdev, "cannot get info for BAR %u", index);
                return ret;
        }

        if (info->size && !is_power_of_two(info->size)) {
                vfio_dev_err(vdev, "region is not power of two: 0x%llx",
                                info->size);
                return -EINVAL;
        }

        return 0;
}
```


For each different architecture, it can have default memory map for different 
region of the system. 

```cpp
/*
 * The memory map used for ARM guests (not to scale):
 *
 * 0      64K  16M     32M     48M            1GB       2GB
 * +-------+----+-------+-------+--------+-----+---------+---......
 * |  PCI  |////| plat  |       |        |     |         |
 * |  I/O  |////| MMIO: | Flash | virtio | GIC |   PCI   |  DRAM
 * | space |////| UART, |       |  MMIO  |     |  (AXI)  |
 * |       |////| RTC,  |       |        |     |         |
 * |       |////| PVTIME|       |        |     |         |
 * +-------+----+-------+-------+--------+-----+---------+---......
 */     

#define ARM_MMIO_AREA           _AC(0x0000000001000000, UL)
#define ARM_AXI_AREA            _AC(0x0000000040000000, UL)

#define KVM_PCI_CFG_AREA        ARM_AXI_AREA
#define ARM_PCI_CFG_SIZE        (1ULL << 28)
#define KVM_PCI_MMIO_AREA       (KVM_PCI_CFG_AREA + ARM_PCI_CFG_SIZE)
static u32 mmio_blocks                  = KVM_PCI_MMIO_AREA;
u32 pci_get_mmio_block(u32 size)
{               
        u32 block = ALIGN(mmio_blocks, size);
        mmio_blocks = block + size; 
        return block;
}       
```

Also, because we have information of the PCI BAR retrieved from the pci-vfio 
driver, such as the size of the bar, it can easily retrieve the guest physical
address of the BAR. 

### Fix up config space for guest
Note that the retrieved BAR information is stored in the regions, not the bar.
We want to make the guest XXX


XXX: Why pdev->hdr_bar should be patched? Was it originally pointing to the 
BAR of the host? and then it is patched to point to guest through this?

```cpp
static int vfio_pci_fixup_cfg_space(struct vfio_device *vdev)
{               
        int i;  
        u64 base;       
        ssize_t hdr_sz;
        struct msix_cap *msix;
        struct vfio_region_info *info;
        struct vfio_pci_device *pdev = &vdev->pci; 
        struct vfio_region *region;
                        
        /* Initialise the BARs */        
        for (i = VFIO_PCI_BAR0_REGION_INDEX; i <= VFIO_PCI_BAR5_REGION_INDEX; ++i) {
                if ((u32)i == vdev->info.num_regions)
                        break;
                
                region = &vdev->regions[i];
                /* Construct a fake reg to match what we've mapped. */
                if (region->is_ioport) {
                        base = (region->port_base & PCI_BASE_ADDRESS_IO_MASK) |
                                PCI_BASE_ADDRESS_SPACE_IO;
                } else {
                        base = (region->guest_phys_addr &
                                PCI_BASE_ADDRESS_MEM_MASK) |
                                PCI_BASE_ADDRESS_SPACE_MEMORY;
                }
                           
                pdev->hdr.bar[i] = base;
        
                if (!base)
                        continue;
        
                pdev->hdr.bar_size[i] = region->info.size;
        }       
	......

```
The first part patches the bar information in the header (hdr.bar) to make it 
point to guest physical address of each bar. Also, because the BAR address not
only presents its bus address, but also other information such as if the address
is PIO or MEM. Therefore, based on the BAR type, it sets different flags 
together with the address. There are other fix-up, but I will skip them and 
jump into the patching part. 

```cpp
static int vfio_pci_fixup_cfg_space(struct vfio_device *vdev)
{               
	......
        /* Install our fake Configuration Space */
        info = &vdev->regions[VFIO_PCI_CONFIG_REGION_INDEX].info;
        /*
         * We don't touch the extended configuration space, let's be cautious
         * and not overwrite it all with zeros, or bad things might happen.
         */
        hdr_sz = PCI_DEV_CFG_SIZE_LEGACY;
        if (pwrite(vdev->fd, &pdev->hdr, hdr_sz, info->offset) != hdr_sz) {
                vfio_dev_err(vdev, "failed to write %zd bytes to Config Space",
                             hdr_sz);
                return -EIO;
        }

        /* Register callbacks for cfg accesses */
        pdev->hdr.cfg_ops = (struct pci_config_operations) {
                .read   = vfio_pci_cfg_read,
                .write  = vfio_pci_cfg_write,
        };

        pdev->hdr.irq_type = IRQ_TYPE_LEVEL_HIGH;

        return 0;
}
```

Note that info structure utilized to get the offset field, passed to the pwrite,
is an info that we have retrieved from the pci driver before [cite]. Therefore, 
the pwrite will overwrite the header of the device. Lastly, it updates the 
cfg_ops. Then why we need to fix-up PCI header information, especially the BARs?
XXXX

### MMAP GPA BAR to HVA
Let's see the last part of the vfio_pci_configure_dev_regions.
```cpp
static int vfio_pci_configure_dev_regions(struct kvm *kvm,                      
                                          struct vfio_device *vdev)             
{   
	......
        return pci__register_bar_regions(kvm, &pdev->hdr, vfio_pci_bar_activate,
                                         vfio_pci_bar_deactivate, vdev); 
}

We have located the BAR, however, it doesn't mean that the BAR has been mapped 
to virtual address. To make the memslot mapping, the pair of the gpa and hva is 
necessary. Therefore, the BAR should be mapped in host virtual address space. 
Let's see how to map the bar through another interface call to vfio driver,
particularly mmap.  

int pci__register_bar_regions(struct kvm *kvm, struct pci_device_header *pci_hdr,
                              bar_activate_fn_t bar_activate_fn,
                              bar_deactivate_fn_t bar_deactivate_fn, void *data)
{               
        int i, r;

        assert(bar_activate_fn && bar_deactivate_fn);

        pci_hdr->bar_activate_fn = bar_activate_fn;
        pci_hdr->bar_deactivate_fn = bar_deactivate_fn;
        pci_hdr->data = data;

        for (i = 0; i < 6; i++) {
                if (!pci_bar_is_implemented(pci_hdr, i))
                        continue;
        
                assert(!pci_bar_is_active(pci_hdr, i));
        
                if (pci__bar_is_io(pci_hdr, i) &&
                    pci__io_space_enabled(pci_hdr)) {
                        r = pci_activate_bar(kvm, pci_hdr, i);
                        if (r < 0)
                                return r;
                }
        
                if (pci__bar_is_memory(pci_hdr, i) &&
                    pci__memory_space_enabled(pci_hdr)) {
                        r = pci_activate_bar(kvm, pci_hdr, i);
                        if (r < 0)
                                return r;
                }
        }       
        
        return 0;
}                       
```

```cpp
static int pci_activate_bar(struct kvm *kvm, struct pci_device_header *pci_hdr,
                            int bar_num)
{
        int r = 0;

        if (pci_bar_is_active(pci_hdr, bar_num))
                goto out;

        r = pci_hdr->bar_activate_fn(kvm, pci_hdr, bar_num, pci_hdr->data);
        if (r < 0) {
                pci_dev_warn(pci_hdr, "Error activating emulation for BAR %d",
                             bar_num);
                goto out;
        }
        pci_hdr->bar_active[bar_num] = true;

out:
        return r;
}       
```

```cpp
static int vfio_pci_bar_activate(struct kvm *kvm,
                                 struct pci_device_header *pci_hdr,
                                 int bar_num, void *data)
{       
        struct vfio_device *vdev = data;
        struct vfio_pci_device *pdev = &vdev->pci;
        struct vfio_pci_msix_pba *pba = &pdev->msix_pba;
        struct vfio_pci_msix_table *table = &pdev->msix_table;
        struct vfio_region *region;
        u32 bar_addr;
        bool has_msix;
        int ret;
        
        assert((u32)bar_num < vdev->info.num_regions);
        
        region = &vdev->regions[bar_num];
        has_msix = pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSIX;
        
        bar_addr = pci__bar_address(pci_hdr, bar_num);
        if (pci__bar_is_io(pci_hdr, bar_num))
                region->port_base = bar_addr;
        else    
                region->guest_phys_addr = bar_addr;
        
        if (has_msix && (u32)bar_num == table->bar) {
                table->guest_phys_addr = region->guest_phys_addr;
                ret = kvm__register_mmio(kvm, table->guest_phys_addr,
                                         table->size, false,
                                         vfio_pci_msix_table_access, pdev);
                /*
                 * The MSIX table and the PBA structure can share the same BAR,
                 * but for convenience we register different regions for mmio
                 * emulation. We want to we update both if they share the same
                 * BAR.
                 */
                if (ret < 0 || table->bar != pba->bar)
                        goto out;
        }
        
        if (has_msix && (u32)bar_num == pba->bar) {
                if (pba->bar == table->bar)
                        pba->guest_phys_addr = table->guest_phys_addr + pba->bar_offset;
                else    
                        pba->guest_phys_addr = region->guest_phys_addr;
                ret = kvm__register_mmio(kvm, pba->guest_phys_addr,
                                         pba->size, false,
                                         vfio_pci_msix_pba_access, pdev);
                goto out;
        }
        
        ret = vfio_map_region(kvm, vdev, region);
out:    
        return ret;
}
```

```cpp
int vfio_map_region(struct kvm *kvm, struct vfio_device *vdev,
                    struct vfio_region *region)
{       
        void *base;
        int ret, prot = 0;
        /* KVM needs page-aligned regions */
        u64 map_size = ALIGN(region->info.size, PAGE_SIZE);
        
        if (!(region->info.flags & VFIO_REGION_INFO_FLAG_MMAP))
                return vfio_setup_trap_region(kvm, vdev, region);
        
        /*
         * KVM_SET_USER_MEMORY_REGION will fail because the guest physical
         * address isn't page aligned, let's emulate the region ourselves.
         */
        if (region->guest_phys_addr & (PAGE_SIZE - 1))
                return kvm__register_mmio(kvm, region->guest_phys_addr,
                                          region->info.size, false,
                                          vfio_mmio_access, region);
        
        if (region->info.flags & VFIO_REGION_INFO_FLAG_READ)
                prot |= PROT_READ;
        if (region->info.flags & VFIO_REGION_INFO_FLAG_WRITE)
                prot |= PROT_WRITE;
        
        base = mmap(NULL, region->info.size, prot, MAP_SHARED, vdev->fd,
                    region->info.offset);
        if (base == MAP_FAILED) {
                /* TODO: support sparse mmap */
                vfio_dev_warn(vdev, "failed to mmap region %u (0x%llx bytes), falling back to trapping",
                         region->info.index, region->info.size);
                return vfio_setup_trap_region(kvm, vdev, region);
        }
        region->host_addr = base;
        
        ret = kvm__register_dev_mem(kvm, region->guest_phys_addr, map_size,
                                    region->host_addr);
        if (ret) {
                vfio_dev_err(vdev, "failed to register region with KVM");
                return ret;
        }
        
        return 0;
}
```
Note that mmap is called to the vdev->fd. Let's see how the pci-device driver 
allows this mapping. Also the mmap address will be stored in the region as a 
host_addr field. This address will be passed to the kvm to generate memslot 
later together with the GPA of the BAR. 

### VFIO mmap to establish GPA -> HVA mapping for BAR
To map the bar region from the user space, it should interface with the kernel. 
Through the mmap call, it can ask the kernel driver to map the region for user
space. 

```cpp
int vfio_pci_core_mmap(struct vfio_device *core_vdev, struct vm_area_struct *vma)
{       
        struct vfio_pci_core_device *vdev =
                container_of(core_vdev, struct vfio_pci_core_device, vdev);
        struct pci_dev *pdev = vdev->pdev;
        unsigned int index;
        u64 phys_len, req_len, pgoff, req_start;
        int ret;
                
        index = vma->vm_pgoff >> (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT);
                             
        if (index >= VFIO_PCI_NUM_REGIONS + vdev->num_regions)
                return -EINVAL;
        if (vma->vm_end < vma->vm_start)
                return -EINVAL;
        if ((vma->vm_flags & VM_SHARED) == 0)
                return -EINVAL;
        if (index >= VFIO_PCI_NUM_REGIONS) {
                int regnum = index - VFIO_PCI_NUM_REGIONS;
                struct vfio_pci_region *region = vdev->region + regnum;
                
                if (region->ops && region->ops->mmap &&
                    (region->flags & VFIO_REGION_INFO_FLAG_MMAP))
                        return region->ops->mmap(vdev, region, vma);
                return -EINVAL;
        }
        if (index >= VFIO_PCI_ROM_REGION_INDEX)
                return -EINVAL; 
        if (!vdev->bar_mmap_supported[index])
                return -EINVAL;
        
        phys_len = PAGE_ALIGN(pci_resource_len(pdev, index));
        req_len = vma->vm_end - vma->vm_start;
        pgoff = vma->vm_pgoff &
                ((1U << (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT)) - 1);
        req_start = pgoff << PAGE_SHIFT;
                                                   
        if (req_start + req_len > phys_len)
                return -EINVAL;

        /*      
         * Even though we don't make use of the barmap for the mmap,
         * we need to request the region and the barmap tracks that.
         */             
        if (!vdev->barmap[index]) {
                ret = pci_request_selected_regions(pdev,
                                                   1 << index, "vfio-pci");
                if (ret)
                        return ret;
        
                vdev->barmap[index] = pci_iomap(pdev, index, 0);
                if (!vdev->barmap[index]) {
                        pci_release_selected_regions(pdev, 1 << index);
                        return -ENOMEM;
                }
        }
        
        vma->vm_private_data = vdev;
        vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
        vma->vm_pgoff = (pci_resource_start(pdev, index) >> PAGE_SHIFT) + pgoff;

        /*
         * See remap_pfn_range(), called from vfio_pci_fault() but we can't
         * change vm_flags within the fault handler.  Set them now.
         */
        vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
        vma->vm_ops = &vfio_pci_mmap_ops;

        return 0;
}
```
Instead of generating the mapping here, usually achieved by remap_pfn_range,
it registers the operation to generate mapping as the fault happens to the 
memory due to its accesses through user virtual address. 


XXX
At the time of fault,
the HVA -> GPA mapping for the bar is generated. 
actually mapped, it will raise the fault and invoke the allocated function. 

```cpp
static const struct vm_operations_struct vfio_pci_mmap_ops = {
        .open = vfio_pci_mmap_open,
        .close = vfio_pci_mmap_close,
        .fault = vfio_pci_mmap_fault,
};

static vm_fault_t vfio_pci_mmap_fault(struct vm_fault *vmf)
{
        struct vm_area_struct *vma = vmf->vma;
        struct vfio_pci_core_device *vdev = vma->vm_private_data;
        struct vfio_pci_mmap_vma *mmap_vma;
        vm_fault_t ret = VM_FAULT_NOPAGE;

        mutex_lock(&vdev->vma_lock);
        down_read(&vdev->memory_lock);
                
        /*              
         * Memory region cannot be accessed if the low power feature is engaged
         * or memory access is disabled.
         */
        if (vdev->pm_runtime_engaged || !__vfio_pci_memory_enabled(vdev)) {
                ret = VM_FAULT_SIGBUS;
                goto up_out;
        }       
                
        /*
         * We populate the whole vma on fault, so we need to test whether
         * the vma has already been mapped, such as for concurrent faults
         * to the same vma.  io_remap_pfn_range() will trigger a BUG_ON if
         * we ask it to fill the same range again.
         */
        list_for_each_entry(mmap_vma, &vdev->vma_list, vma_next) {
                if (mmap_vma->vma == vma)
                        goto up_out;
        }
        
        if (io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
                               vma->vm_end - vma->vm_start,
                               vma->vm_page_prot)) {
                ret = VM_FAULT_SIGBUS;
                zap_vma_ptes(vma, vma->vm_start, vma->vm_end - vma->vm_start);
                goto up_out;
        }
        
        if (__vfio_pci_add_vma(vdev, vma)) {
                ret = VM_FAULT_OOM;
                zap_vma_ptes(vma, vma->vm_start, vma->vm_end - vma->vm_start);
        }       
        
up_out: 
        up_read(&vdev->memory_lock);
        mutex_unlock(&vdev->vma_lock);
        return ret;
}       
```


###
After generating the HVA mapped to the BAR, the KVMTOOL register this memory 
region to the guest through the kvm module. 

```cpp
int vfio_map_region(struct kvm *kvm, struct vfio_device *vdev,
                    struct vfio_region *region)
{       
	......
        base = mmap(NULL, region->info.size, prot, MAP_SHARED, vdev->fd,
                    region->info.offset);
        if (base == MAP_FAILED) {
                /* TODO: support sparse mmap */
                vfio_dev_warn(vdev, "failed to mmap region %u (0x%llx bytes), falling back to trapping",
                         region->info.index, region->info.size);
                return vfio_setup_trap_region(kvm, vdev, region);
        }
        region->host_addr = base;
        
        ret = kvm__register_dev_mem(kvm, region->guest_phys_addr, map_size,
                                    region->host_addr);
        if (ret) {
                vfio_dev_err(vdev, "failed to register region with KVM");
                return ret;
        }
        
        return 0;
}

```cpp
static inline int kvm__register_dev_mem(struct kvm *kvm, u64 guest_phys,
                                        u64 size, void *userspace_addr)
{
        return kvm__register_mem(kvm, guest_phys, size, userspace_addr,
                                 KVM_MEM_TYPE_DEVICE);
}
```

```cpp
int kvm__register_mem(struct kvm *kvm, u64 guest_phys, u64 size,
                      void *userspace_addr, enum kvm_mem_type type)
{
        struct kvm_userspace_memory_region mem;
        struct kvm_mem_bank *merged = NULL;
        struct kvm_mem_bank *bank;      
        struct list_head *prev_entry;   
        u32 slot;
        u32 flags = 0;                  
        int ret;
        
        mutex_lock(&kvm->mem_banks_lock);
        /* Check for overlap and find first empty slot. */
        slot = 0;
        prev_entry = &kvm->mem_banks;
        list_for_each_entry(bank, &kvm->mem_banks, list) {
                u64 bank_end = bank->guest_phys_addr + bank->size - 1;
                u64 end = guest_phys + size - 1;
                if (guest_phys > bank_end || end < bank->guest_phys_addr) {
                        /*
                         * Keep the banks sorted ascending by slot, so it's
                         * easier for us to find a free slot.
                         */
                        if (bank->slot == slot) {
                                slot++;
                                prev_entry = &bank->list;
                        }
                        continue;
                }
        
                /* Merge overlapping reserved regions */
                if (bank->type == KVM_MEM_TYPE_RESERVED &&
                    type == KVM_MEM_TYPE_RESERVED) {
                        bank->guest_phys_addr = min(bank->guest_phys_addr, guest_phys);
                        bank->size = max(bank_end, end) - bank->guest_phys_addr + 1;
        
                        if (merged) {
                                /*
                                 * This is at least the second merge, remove
                                 * previous result.
                                 */
                                list_del(&merged->list);
                                free(merged);
                        }
                
                        guest_phys = bank->guest_phys_addr;
                        size = bank->size;
                        merged = bank;
        
                        /* Keep checking that we don't overlap another region */
                        continue;
                }       
        
                pr_err("%s region [%llx-%llx] would overlap %s region [%llx-%llx]",
                       kvm_mem_type_to_string(type), guest_phys, guest_phys + size - 1,
                       kvm_mem_type_to_string(bank->type), bank->guest_phys_addr,
                       bank->guest_phys_addr + bank->size - 1);
        
                ret = -EINVAL;
                goto out;
        }       
                
        if (merged) {
                ret = 0;
                goto out;
        }

        bank = malloc(sizeof(*bank));
        if (!bank) {
                ret = -ENOMEM;
                goto out;
        }

        INIT_LIST_HEAD(&bank->list);
        bank->guest_phys_addr           = guest_phys;
        bank->host_addr                 = userspace_addr;
        bank->size                      = size;
        bank->type                      = type;
        bank->slot                      = slot;

        if (type & KVM_MEM_TYPE_READONLY)
                flags |= KVM_MEM_READONLY;

        if (type != KVM_MEM_TYPE_RESERVED) {
                mem = (struct kvm_userspace_memory_region) {
                        .slot                   = slot,
                        .flags                  = flags,
                        .guest_phys_addr        = guest_phys,
                        .memory_size            = size,
                        .userspace_addr         = (unsigned long)userspace_addr,
                };

                ret = ioctl(kvm->vm_fd, KVM_SET_USER_MEMORY_REGION, &mem);
                if (ret < 0) {
                        ret = -errno;
                        goto out;
                }
        }

        list_add(&bank->list, prev_entry);
        kvm->mem_slots++;
        ret = 0;

out:
        mutex_unlock(&kvm->mem_banks_lock);
        return ret;
}
```


### KVM module side to set new user memory region
```cpp
static long kvm_vm_ioctl(struct file *filp,
                           unsigned int ioctl, unsigned long arg)
{
        struct kvm *kvm = filp->private_data;
        void __user *argp = (void __user *)arg;
        int r;

        if (kvm->mm != current->mm || kvm->vm_dead)
                return -EIO;
        switch (ioctl) {
        case KVM_CREATE_VCPU:
                r = kvm_vm_ioctl_create_vcpu(kvm, arg);
                break;
        case KVM_ENABLE_CAP: {
                struct kvm_enable_cap cap;

                r = -EFAULT;
                if (copy_from_user(&cap, argp, sizeof(cap)))
                        goto out;
                r = kvm_vm_ioctl_enable_cap_generic(kvm, &cap);
                break;
        }
        case KVM_SET_USER_MEMORY_REGION: {
                struct kvm_userspace_memory_region kvm_userspace_mem;

                r = -EFAULT;
                if (copy_from_user(&kvm_userspace_mem, argp,
                                                sizeof(kvm_userspace_mem)))
                        goto out;

                r = kvm_vm_ioctl_set_memory_region(kvm, &kvm_userspace_mem);
                break;
        }
	......
```

```cpp
static int kvm_vm_ioctl_set_memory_region(struct kvm *kvm,
                                          struct kvm_userspace_memory_region *mem)
{       
        if ((u16)mem->slot >= KVM_USER_MEM_SLOTS)
                return -EINVAL;
        
        return kvm_set_memory_region(kvm, mem);
}

int kvm_set_memory_region(struct kvm *kvm,
                          const struct kvm_userspace_memory_region *mem)
{
        int r;            
        
        mutex_lock(&kvm->slots_lock);
        r = __kvm_set_memory_region(kvm, mem);
        mutex_unlock(&kvm->slots_lock);
        return r;
}       
EXPORT_SYMBOL_GPL(kvm_set_memory_region);
```


```cpp
int __kvm_set_memory_region(struct kvm *kvm,
                            const struct kvm_userspace_memory_region *mem)
{
        struct kvm_memory_slot *old, *new;
        struct kvm_memslots *slots;
        enum kvm_mr_change change;
        unsigned long npages;
        gfn_t base_gfn;
        int as_id, id;
        int r;

        r = check_memory_region_flags(mem);
        if (r)
                return r;

        as_id = mem->slot >> 16;
        id = (u16)mem->slot;

        /* General sanity checks */
        if ((mem->memory_size & (PAGE_SIZE - 1)) ||
            (mem->memory_size != (unsigned long)mem->memory_size))
                return -EINVAL;
        if (mem->guest_phys_addr & (PAGE_SIZE - 1))
                return -EINVAL;
        /* We can read the guest memory with __xxx_user() later on. */
        if ((mem->userspace_addr & (PAGE_SIZE - 1)) ||
            (mem->userspace_addr != untagged_addr(mem->userspace_addr)) ||
             !access_ok((void __user *)(unsigned long)mem->userspace_addr,
                        mem->memory_size))
                return -EINVAL;
        if (as_id >= KVM_ADDRESS_SPACE_NUM || id >= KVM_MEM_SLOTS_NUM)
                return -EINVAL;
        if (mem->guest_phys_addr + mem->memory_size < mem->guest_phys_addr)
                return -EINVAL;
        if ((mem->memory_size >> PAGE_SHIFT) > KVM_MEM_MAX_NR_PAGES)
                return -EINVAL;

        slots = __kvm_memslots(kvm, as_id);

        /*
         * Note, the old memslot (and the pointer itself!) may be invalidated
         * and/or destroyed by kvm_set_memslot().
         */
        old = id_to_memslot(slots, id);

        if (!mem->memory_size) {
                if (!old || !old->npages)
                        return -EINVAL;

                if (WARN_ON_ONCE(kvm->nr_memslot_pages < old->npages))
                        return -EIO;

                return kvm_set_memslot(kvm, old, NULL, KVM_MR_DELETE);
        }

        base_gfn = (mem->guest_phys_addr >> PAGE_SHIFT);
        npages = (mem->memory_size >> PAGE_SHIFT);

        if (!old || !old->npages) {
                change = KVM_MR_CREATE;

                /*
                 * To simplify KVM internals, the total number of pages across
                 * all memslots must fit in an unsigned long.
                 */
                if ((kvm->nr_memslot_pages + npages) < kvm->nr_memslot_pages)
                        return -EINVAL;
        } else { /* Modify an existing slot. */
                if ((mem->userspace_addr != old->userspace_addr) ||
                    (npages != old->npages) ||
                    ((mem->flags ^ old->flags) & KVM_MEM_READONLY))
                        return -EINVAL;

                if (base_gfn != old->base_gfn)
                        change = KVM_MR_MOVE;
                else if (mem->flags != old->flags)
                        change = KVM_MR_FLAGS_ONLY;
                else /* Nothing to change. */
                        return 0;
        }

        if ((change == KVM_MR_CREATE || change == KVM_MR_MOVE) &&
            kvm_check_memslot_overlap(slots, id, base_gfn, base_gfn + npages))
                return -EEXIST;

        /* Allocate a slot that will persist in the memslot. */
        new = kzalloc(sizeof(*new), GFP_KERNEL_ACCOUNT);
        if (!new)
                return -ENOMEM;

        new->as_id = as_id;
        new->id = id;
        new->base_gfn = base_gfn;
        new->npages = npages;
        new->flags = mem->flags;
        new->userspace_addr = mem->userspace_addr;

        r = kvm_set_memslot(kvm, old, new, change);
        if (r)
                kfree(new);
        return r;
}
```

As a result, the memslot that maps GPA of the BAR to the HVA. Because guest has 
memslot for the BAR, whenever the guest accesses the BAR GPA, which makes the 
data abort fault, the KVM module can tell there is a memslot for that address.
If there is a memslot, then the KVM can easily establish new mapping for the 
BAR GPA to the actual BAR HPA through the memslot. If there is no memslot for 
the GPA, then the MMIO emulation on the KVM side should be involved every access.
However, for the BARs because we have memslot, after the initial fault on that 
region, KVM can map the faultin GPA to HPA in stage2 table. For the detailed
handling see [[]].

