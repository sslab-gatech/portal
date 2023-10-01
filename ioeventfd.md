## KVM TOOL
```cpp
int virtio_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
                struct virtio_ops *ops, enum virtio_trans trans,
                int device_id, int subsys_id, int class)
{
        void *virtio;
        int r;

        switch (trans) {
        case VIRTIO_PCI_LEGACY:
                vdev->legacy                    = true;
                /* fall through */
        case VIRTIO_PCI:
                virtio = calloc(sizeof(struct virtio_pci), 1);
                if (!virtio)
                        return -ENOMEM;
                vdev->virtio                    = virtio;
                vdev->ops                       = ops;
                vdev->ops->signal_vq            = virtio_pci__signal_vq;
                vdev->ops->signal_config        = virtio_pci__signal_config;
                vdev->ops->init                 = virtio_pci__init;
                vdev->ops->exit                 = virtio_pci__exit;
                vdev->ops->reset                = virtio_pci__reset;
                r = vdev->ops->init(kvm, dev, vdev, device_id, subsys_id, class);
                break;
```

```cpp
int virtio_pci__init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
                     int device_id, int subsys_id, int class) 
{



}


```

```cpp
static int virtio_pci__bar_activate(struct kvm *kvm,
                                    struct pci_device_header *pci_hdr,
                                    int bar_num, void *data)
{       
        struct virtio_device *vdev = data;
        mmio_handler_fn mmio_fn;
        u32 bar_addr, bar_size;
        int r = -EINVAL;

        if (vdev->legacy) 
                mmio_fn = &virtio_pci_legacy__io_mmio_callback;
        else
                mmio_fn = &virtio_pci_modern__io_mmio_callback;
                
        assert(bar_num <= 2);
        
        bar_addr = pci__bar_address(pci_hdr, bar_num);
        bar_size = pci__bar_size(pci_hdr, bar_num);

        switch (bar_num) {
        case 0:
                r = kvm__register_pio(kvm, bar_addr, bar_size, mmio_fn, vdev);
                break;
        case 1:
                r =  kvm__register_mmio(kvm, bar_addr, bar_size, false, mmio_fn,
                                        vdev);
                break;
        case 2:
                r =  kvm__register_mmio(kvm, bar_addr, bar_size, false,
                                        virtio_pci__msix_mmio_callback, vdev);
                break;
        }

        return r;
}                              
```

```cpp
void virtio_pci_modern__io_mmio_callback(struct kvm_cpu *vcpu, u64 addr,
                                         u8 *data, u32 len, u8 is_write,
                                         void *ptr)
{       
        struct virtio_device *vdev = ptr;
        struct virtio_pci *vpci = vdev->virtio; 
        u32 mmio_addr = virtio_pci__mmio_addr(vpci);
        
        virtio_pci_access(vcpu, vdev, addr - mmio_addr, data, len, is_write);
}
```

```cpp
static bool virtio_pci_access(struct kvm_cpu *vcpu, struct virtio_device *vdev,
                              unsigned long offset, void *data, int size,
                              bool write)
{
        access_handler_t handler = NULL;
                        
        switch (offset) {
        case VPCI_CFG_COMMON_START...VPCI_CFG_COMMON_END:
                if (write)
                        handler = virtio_pci__common_write;
                else
                        handler = virtio_pci__common_read;
                break;
        case VPCI_CFG_NOTIFY_START...VPCI_CFG_NOTIFY_END:
                if (write)
                        handler = virtio_pci__notify_write;
                break;
        case VPCI_CFG_ISR_START...VPCI_CFG_ISR_END:
                if (!write)
                        handler = virtio_pci__isr_read;
                break;
        case VPCI_CFG_DEV_START...VPCI_CFG_DEV_END:
                if (write)
                        handler = virtio_pci__config_write;
                else
                        handler = virtio_pci__config_read;
                break;
        }       
                
        if (!handler)
                return false;
                
        return handler(vdev, offset, data, size);
}            
```


```cpp
static bool virtio_pci__common_write(struct virtio_device *vdev,
                                     unsigned long offset, void *data, int size)
{       
        u64 features;
        u32 val, gsi, vec;
        struct virtio_pci *vpci = vdev->virtio;
                
        switch (offset - VPCI_CFG_COMMON_START) {
        case VIRTIO_PCI_COMMON_DFSELECT:
                vpci->device_features_sel = ioport__read32(data);
                break;
        case VIRTIO_PCI_COMMON_GFSELECT:
                vpci->driver_features_sel = ioport__read32(data);
                break;
        case VIRTIO_PCI_COMMON_GF:
                val = ioport__read32(data);
                if (vpci->driver_features_sel > 1)
                        break;

                features = (u64)val << (32 * vpci->driver_features_sel);
                virtio_set_guest_features(vpci->kvm, vdev, vpci->dev, features);
                break;
        case VIRTIO_PCI_COMMON_MSIX:
                vec = vpci->config_vector = ioport__read16(data);
                gsi = virtio_pci__add_msix_route(vpci, vec);
                if (gsi < 0)
                        break;

                vpci->config_gsi = gsi; 
                break;
        case VIRTIO_PCI_COMMON_STATUS:
                vpci->status = ioport__read8(data);
                virtio_notify_status(vpci->kvm, vdev, vpci->dev, vpci->status);
                break;
        case VIRTIO_PCI_COMMON_Q_SELECT:
                val = ioport__read16(data);
                if (val >= (u32)vdev->ops->get_vq_count(vpci->kvm, vpci->dev))
                        pr_warning("invalid vq number %u", val);
                else
                        vpci->queue_selector = val;
                break;
        case VIRTIO_PCI_COMMON_Q_SIZE:
                vdev->ops->set_size_vq(vpci->kvm, vpci->dev,
                                       vpci->queue_selector,
                                       ioport__read16(data));
                break;
        case VIRTIO_PCI_COMMON_Q_MSIX:
                vec = vpci->vq_vector[vpci->queue_selector] = ioport__read16(data);

                gsi = virtio_pci__add_msix_route(vpci, vec);
                if (gsi < 0)
                        break;

                vpci->gsis[vpci->queue_selector] = gsi;
                if (vdev->ops->notify_vq_gsi)
                        vdev->ops->notify_vq_gsi(vpci->kvm, vpci->dev,
                                                 vpci->queue_selector, gsi);
                break;
        case VIRTIO_PCI_COMMON_Q_ENABLE:
                val = ioport__read16(data);
                if (val)
                        virtio_pci_init_vq(vpci->kvm, vdev, vpci->queue_selector);
                else
                        virtio_pci_exit_vq(vpci->kvm, vdev, vpci->queue_selector);
                break;
	......
```



```cpp
int virtio_pci_init_vq(struct kvm *kvm, struct virtio_device *vdev, int vq)
{       
        int ret;
        struct virtio_pci *vpci = vdev->virtio;
        
        ret = virtio_pci__init_ioeventfd(kvm, vdev, vq);
        if (ret) {
                pr_err("couldn't add ioeventfd for vq %d: %d", vq, ret);
                return ret;
        }
        return vdev->ops->init_vq(kvm, vpci->dev, vq);
}
```


```cpp
int virtio_pci__init_ioeventfd(struct kvm *kvm, struct virtio_device *vdev,
                               u32 vq)
{
        struct ioevent ioevent;
        struct virtio_pci *vpci = vdev->virtio;
        u32 mmio_addr = virtio_pci__mmio_addr(vpci);
        u16 port_addr = virtio_pci__port_addr(vpci);
        off_t offset = vpci->doorbell_offset;
        int r, flags = 0;
        int pio_fd, mmio_fd;

        vpci->ioeventfds[vq] = (struct virtio_pci_ioevent_param) {
                .vdev           = vdev,
                .vq             = vq,
        };

        ioevent = (struct ioevent) {
                .fn             = virtio_pci__ioevent_callback,
                .fn_ptr         = &vpci->ioeventfds[vq],
                .datamatch      = vq,
                .fn_kvm         = kvm,
        };

        /*
         * Vhost will poll the eventfd in host kernel side, otherwise we
         * need to poll in userspace.
         */
        if (!vdev->use_vhost)
                flags |= IOEVENTFD_FLAG_USER_POLL;

        /* ioport */
        ioevent.io_addr = port_addr + offset;
        ioevent.io_len  = sizeof(u16);
        ioevent.fd      = pio_fd = eventfd(0, 0);
        r = ioeventfd__add_event(&ioevent, flags | IOEVENTFD_FLAG_PIO);
        if (r)
                return r;

        /* mmio */
        ioevent.io_addr = mmio_addr + offset;
        ioevent.io_len  = sizeof(u16);
        ioevent.fd      = mmio_fd = eventfd(0, 0);
        r = ioeventfd__add_event(&ioevent, flags);
        if (r)
                goto free_ioport_evt;

        if (vdev->ops->notify_vq_eventfd)
                vdev->ops->notify_vq_eventfd(kvm, vpci->dev, vq,
                                             vdev->legacy ? pio_fd : mmio_fd);
        return 0;

free_ioport_evt:
        ioeventfd__del_event(port_addr + offset, vq);
        return r;
	}
```

ioeventfd__add_event


## KVM 
KVM_IOEVENTFD
