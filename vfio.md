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
The userspace can configure the IOMMU for the container by invoking 
VFIO_SET_IOMMU ioctl  on the file descriptor of the container. 

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
It iterates the list of registered drivers in the iommu_drivers_list and \XXX


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

static int __init vfio_iommu_type1_init(void)
{
        return vfio_register_iommu_driver(&vfio_iommu_driver_ops_type1);
}       
```

```cpp
/*
 * IOMMU driver registration
 */
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
EXPORT_SYMBOL_GPL(vfio_register_iommu_driver);
```
There is only registered driver in the iommu_drivers_list unless the noiommu is
used. Therefore, the open call through the selected driver invokes 
vfio_iommu_type1_open function. Note that the list_add adds the current driver 
to the iommu_drivers_list of vfio.

```cpp
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
```
Therefore, the selected driver in the iteration loop is the driver of the 
vfio_iommu_type1_init.

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

This function allocates vfio_iommu struct instance and fill out the information. 
Retrieved vfio_iommu will be used by __vfio_container_attach_groups function 
to XXX


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
It iterates groups registered to the container and invokes the attach_group 
function of the  vfio_iommu_driver_ops_type1 to attach group to iommu. 

```cpp
static int vfio_iommu_type1_attach_group(void *iommu_data,
                struct iommu_group *iommu_group, enum vfio_group_type type) 
		//iommu_group is the iommu_group of the group in container
		//iommu_data is the vfio_iommu as a result of open of vfio_iommu_typ1
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

        ret = iommu_attach_group(domain->domain, group->iommu_group);
        if (ret)
                goto out_domain;

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


### Allocate new domain 
First it needs to have new domain which is same as of the target device belongs 
to the group.

```cpp
static int vfio_iommu_domain_alloc(struct device *dev, void *data)
{
        struct iommu_domain **domain = data;

        *domain = iommu_domain_alloc(dev->bus);
        return 1; /* Don't iterate */
}

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


### Bind the devices to the new iommu domain
```cpp
/**     
 * iommu_attach_group - Attach an IOMMU domain to an IOMMU group
 * @domain: IOMMU domain to attach
 * @group: IOMMU group that will be attached
 *      
 * Returns 0 on success and error code on failure
 *              
 * Note that EINVAL can be treated as a soft failure, indicating
 * that certain configuration of the domain is incompatible with
 * the group. In this case attaching a different domain to the
 * group may succeed.
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

The above functions are used in attaching devices of the group to the previously
generated IOMMU domain. To accomplish this task, it invokes attach_dev function
registered in the domain, which is the arm_smmu_attach_dev in this case. 


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
```

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



##
### Userspace invoking VFIO_IOMMU_MAP_DMA
Generally, there would be two KVM_MEM_TYPE_RAM memory banks: one for the guest 
kernel and the other for the para-virtualized space. For those two memory 
regions or more, the vfio_map_mem_bank function is invoked and register GPA in 
IOMMU. 

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

KVMTOOL iterates all KVM_MEM_TYPE_RAM and invokes ioctl to vfio_container. 

### kernel vfio side handling for VFIO_IOMMU_MAP_DMA

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
However, there is no VFIO_IOMMU_MAP_DMA ioctl function, and it invokes the ioctl 
of the driver maintained by the container. 

```cpp
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
