## PCI Init (core PCI driver registration)
```cpp
static int __init pci_driver_init(void)
{
        int ret;

        ret = bus_register(&pci_bus_type);
        if (ret)
                return ret;

#ifdef CONFIG_PCIEPORTBUS
        ret = bus_register(&pcie_port_bus_type);
        if (ret)
                return ret;
#endif
        dma_debug_add_bus(&pci_bus_type);
        return 0;
}
postcore_initcall(pci_driver_init);

struct bus_type pci_bus_type = {
        .name           = "pci",
        .match          = pci_bus_match,
        .uevent         = pci_uevent,
        .probe          = pci_device_probe,
        .remove         = pci_device_remove,
        .shutdown       = pci_device_shutdown,
        .dev_groups     = pci_dev_groups,
        .bus_groups     = pci_bus_groups,
        .drv_groups     = pci_drv_groups,
        .pm             = PCI_PM_OPS_PTR,
        .num_vf         = pci_bus_num_vf,
        .dma_configure  = pci_dma_configure,
        .dma_cleanup    = pci_dma_cleanup,
};      
EXPORT_SYMBOL(pci_bus_type);
```

Two important functions of pci bus device is probe and match. When new device is 
attached to the pci bus, then it invokes the match function to configure if the 
new device can be handled one of the registered device driver for pci. When it 
turns out that the new device can be handled, it invokes pci_device_probe to 
configure the device. 



## PCI platform device initialization and registration from DTB
```cpp
static struct platform_driver gen_pci_driver = {
        .driver = {
                .name = "pci-host-generic",
                .of_match_table = gen_pci_of_match,
        },
        .probe = pci_host_common_probe,
        .remove = pci_host_common_remove,
};
module_platform_driver(gen_pci_driver);

static const struct of_device_id gen_pci_of_match[] = {
        { .compatible = "pci-host-cam-generic",
          .data = &gen_pci_cfg_cam_bus_ops },

        { .compatible = "pci-host-ecam-generic",
          .data = &pci_generic_ecam_ops },

        { .compatible = "marvell,armada8k-pcie-ecam",
          .data = &pci_dw_ecam_bus_ops },

        { .compatible = "socionext,synquacer-pcie-ecam",
          .data = &pci_dw_ecam_bus_ops },

        { .compatible = "snps,dw-pcie-ecam",
          .data = &pci_dw_ecam_bus_ops },

        { },
};
MODULE_DEVICE_TABLE(of, gen_pci_of_match);

```


### Probing PCI controller
If the DTB information of the pci controller matches one of the of_device_id 
described in gen_pci_of_match table, then it invokes the probe function to
register pci controller to the linux kernel system. We assume that the platform 
has pci-host-ecam-generic pci controller. 

```cpp
int pci_host_common_probe(struct platform_device *pdev)
{
        struct device *dev = &pdev->dev;
        struct pci_host_bridge *bridge;
        struct pci_config_window *cfg;
        const struct pci_ecam_ops *ops;

        ops = of_device_get_match_data(&pdev->dev);
        if (!ops)
                return -ENODEV;

        bridge = devm_pci_alloc_host_bridge(dev, 0);
        if (!bridge)
                return -ENOMEM;

        platform_set_drvdata(pdev, bridge);

        of_pci_check_probe_only();

        /* Parse and map our Configuration Space windows */
        cfg = gen_pci_init(dev, bridge, ops);
        if (IS_ERR(cfg))
                return PTR_ERR(cfg);

        /* Do not reassign resources if probe only */
        if (!pci_has_flag(PCI_PROBE_ONLY))
                pci_add_flags(PCI_REASSIGN_ALL_BUS);

        bridge->sysdata = cfg;
        bridge->ops = (struct pci_ops *)&ops->pci_ops;
        bridge->msi_domain = true;

        return pci_host_probe(bridge);
}
```
As the above probe function is generic function for multiple pci controller, it 
first retrieves the pci_ecam_ops matching with the pcie controller, here that 
will be pci_generic_ecam_ops.

```cpp
/* ECAM ops */  
const struct pci_ecam_ops pci_generic_ecam_ops = {
        .pci_ops        = {     
                .add_bus        = pci_ecam_add_bus,
                .remove_bus     = pci_ecam_remove_bus,
                .map_bus        = pci_ecam_map_bus,
                .read           = pci_generic_config_read,
                .write          = pci_generic_config_write,
        } 
};
```

## Allocate and initialize pci bridge 
```cpp
int pci_host_common_probe(struct platform_device *pdev)
{
	......
        bridge = devm_pci_alloc_host_bridge(dev, 0);
        if (!bridge)
                return -ENOMEM;
	......
```
The next job of the probe function is to allocate host bridge based on the DTB 
information. The parsed information will be stored in the pci_host_bridge.

```cpp
struct pci_host_bridge {
        struct device   dev;
        struct pci_bus  *bus;           /* Root bus */
        struct pci_ops  *ops;
        struct pci_ops  *child_ops;
        void            *sysdata;
        int             busnr;
        int             domain_nr;
        struct list_head windows;       /* resource_entry */
        struct list_head dma_ranges;    /* dma ranges resource list */
        u8 (*swizzle_irq)(struct pci_dev *, u8 *); /* Platform IRQ swizzler */
        int (*map_irq)(const struct pci_dev *, u8, u8);
        void (*release_fn)(struct pci_host_bridge *);
        void            *release_data;
        unsigned int    ignore_reset_delay:1;   /* For entire hierarchy */
        unsigned int    no_ext_tags:1;          /* No Extended Tags */
        unsigned int    native_aer:1;           /* OS may use PCIe AER */
        unsigned int    native_pcie_hotplug:1;  /* OS may use PCIe hotplug */
        unsigned int    native_shpc_hotplug:1;  /* OS may use SHPC hotplug */
        unsigned int    native_pme:1;           /* OS may use PCIe PME */
        unsigned int    native_ltr:1;           /* OS may use PCIe LTR */
        unsigned int    native_dpc:1;           /* OS may use PCIe DPC */
        unsigned int    preserve_config:1;      /* Preserve FW resource setup */
        unsigned int    size_windows:1;         /* Enable root bus sizing */
        unsigned int    msi_domain:1;           /* Bridge wants MSI domain */

        /* Resource alignment requirements */
        resource_size_t (*align_resource)(struct pci_dev *dev,
                        const struct resource *res,
                        resource_size_t start,
                        resource_size_t size,
                        resource_size_t align);
        unsigned long   private[] ____cacheline_aligned;
};
```

Let's see how devm_pci_alloc_host_bridge parses the DTB and generate this main
data structure for controller. 

```cpp
struct pci_host_bridge *devm_pci_alloc_host_bridge(struct device *dev,
                                                   size_t priv)
{       
        int ret;
        struct pci_host_bridge *bridge;
        
        bridge = pci_alloc_host_bridge(priv);
        if (!bridge)
                return NULL;
        
        bridge->dev.parent = dev;
        
        ret = devm_add_action_or_reset(dev, devm_pci_alloc_host_bridge_release,
                                       bridge);
        if (ret)
                return NULL;
        
        ret = devm_of_pci_bridge_init(dev, bridge);
        if (ret)
                return NULL;
        
        return bridge;
	}

int devm_of_pci_bridge_init(struct device *dev, struct pci_host_bridge *bridge)
{       
        if (!dev->of_node)             
                return 0;
                
        bridge->swizzle_irq = pci_common_swizzle;
        bridge->map_irq = of_irq_parse_and_map_pci;
        
        return pci_parse_request_of_pci_ranges(dev, bridge);
}       
```

As shown in the above code, it allocates new pci_host_bridge for the controller 
and initialize this structure based on the DTB information. We will see how the 
kernel code initialize it in the following code. 

### Parse PCI bridge node from DTB
**devm_of_pci_get_host_bridge_resources** parses bus ranges described in the 
device file. For one PCI controller, there could be multiple memory/IO region
belong to it. The memory range information of the controller is described in the
ranges field of the PCI controller in the DTB. 

```cpp
static int pci_parse_request_of_pci_ranges(struct device *dev,
                                           struct pci_host_bridge *bridge)
{
        int err, res_valid = 0;
        resource_size_t iobase;
        struct resource_entry *win, *tmp; 

        INIT_LIST_HEAD(&bridge->windows);
        INIT_LIST_HEAD(&bridge->dma_ranges);
        
        err = devm_of_pci_get_host_bridge_resources(dev, 0, 0xff, &bridge->windows,
                                                    &bridge->dma_ranges, &iobase);
        if (err)
                return err;
                
        err = devm_request_pci_bus_resources(dev, &bridge->windows);
        if (err)        
                return err;
                        
        resource_list_for_each_entry_safe(win, tmp, &bridge->windows) {
                struct resource *res = win->res;
                        
                switch (resource_type(res)) {
                case IORESOURCE_IO:
                        err = devm_pci_remap_iospace(dev, res, iobase);
                        if (err) {
                                dev_warn(dev, "error %d: failed to map resource %pR\n",
                                         err, res);
                                resource_list_destroy_entry(win);
                        }
                        break;
                case IORESOURCE_MEM:
                        res_valid |= !(res->flags & IORESOURCE_PREFETCH);
                        
                        if (!(res->flags & IORESOURCE_PREFETCH))
                                if (upper_32_bits(resource_size(res)))
                                        dev_warn(dev, "Memory resource size exceeds max for 32 bits\n");
                        
                        break;
                }
        }
        
        if (!res_valid)
                dev_warn(dev, "non-prefetchable memory resource required\n");
        
        return 0;
}
```

devm_of_pci_get_host_bridge_resources parses memory range information of the 
controller and translate them into **resources**. Also, all parsed resources 
will be stored in **bridge->windows**. Each range information consists of pci 
address, CPU address, PCI size. To better understanding of the range of PCI, 
refer to
https://michael2012z.medium.com/understanding-pci-node-in-fdt-769a894a13cc.
Also I will not cover the details of the parsing fucntion. Anyone interested in
how the kernel parsing the bus_range see devm_of_pci_get_host_bridge_resources.

### Allocate device resource for PCI ranges
Now we have PCI range information generated from parsing range field of the PCI
controller's DTB. However, as these addresses are not accessible from the kernel
without mapping, the kernel requires proper mapping for those memory range. 
Since linux kernel manages io addresses as devres, the parsed resource should be
translated into the devres, so that the kernel can manage these memory region,
especially when it needs to be released. 
```cpp
static int pci_parse_request_of_pci_ranges(struct device *dev,
                                           struct pci_host_bridge *bridge)
{
	......
        err = devm_request_pci_bus_resources(dev, &bridge->windows);
	......
}

int devm_request_pci_bus_resources(struct device *dev,
                                   struct list_head *resources)
{
        struct resource_entry *win;
        struct resource *parent, *res;
        int err;

        resource_list_for_each_entry(win, resources) {
                res = win->res;
                switch (resource_type(res)) {
                case IORESOURCE_IO:
                        parent = &ioport_resource;
                        break;
                case IORESOURCE_MEM:
                        parent = &iomem_resource;
                        break; 
                default:
                        continue;
                } 
        
                err = devm_request_resource(dev, parent, res);
                if (err)
                        return err;
        }

        return 0;
}       
```
devres is basically linked list of arbitrarily sized memory areas associated 
with a struct device. Also it sets the function that will be invoked at the time 
of release of the memory region. 

```cpp
struct resource ioport_resource = {
        .name   = "PCI IO",
        .start  = 0,
        .end    = IO_SPACE_LIMIT,
        .flags  = IORESOURCE_IO,
};
EXPORT_SYMBOL(ioport_resource);

struct resource iomem_resource = {
        .name   = "PCI mem",
        .start  = 0,
        .end    = -1,
        .flags  = IORESOURCE_MEM,
};
EXPORT_SYMBOL(iomem_resource);


int devm_request_resource(struct device *dev, struct resource *root,
                          struct resource *new)
{
        struct resource *conflict, **ptr; 

        ptr = devres_alloc(devm_resource_release, sizeof(*ptr), GFP_KERNEL);
        if (!ptr)
                return -ENOMEM;

        *ptr = new;
                
        conflict = request_resource_conflict(root, new);
        if (conflict) {
                dev_err(dev, "resource collision: %pR conflicts with %s %pR\n",
                        new, conflict->name, conflict);
                devres_free(ptr);
                return -EBUSY; 
        }               
                
        devres_add(dev, ptr);
        return 0;
}
```

## Configure ECAM range (gen_pci_init)
So far we parsed the memory region of the PCI controller and register them as 
devres so that the kernel can release the memory region when necessary. Also,
most importantly we allocated kernel data structure, pci_host_bridge, to manage
current PCI controller. Now we need virtual address mapping to the parsed memory
ranges of the controller so that we can access the PCI controller for further 
device probing and configurations. 

If it is PCIe controller, then one of its region is a ECAM region which can be 
used to configure subsequent PCIe devices attached to it through MMIO. 

```cpp
int pci_host_common_probe(struct platform_device *pdev)
{
	......
        /* Parse and map our Configuration Space windows */
        cfg = gen_pci_init(dev, bridge, ops);
        if (IS_ERR(cfg))
                return PTR_ERR(cfg);
	......
}
```
When we go back to the pci_host_common_probe function, we can see the invocation
of the gen_pci_init function. Also, note that it requires the pci_host_bridge 
and ops. pci_host_bridge provides the data about the ECAM (i.e., range) and the 
op provides the functions to map the ECAM properly. 

```cpp
static struct pci_config_window *gen_pci_init(struct device *dev,
                struct pci_host_bridge *bridge, const struct pci_ecam_ops *ops)
{       
        int err;
        struct resource cfgres;         
        struct resource_entry *bus;
        struct pci_config_window *cfg;

        err = of_address_to_resource(dev->of_node, 0, &cfgres);
        if (err) {
                dev_err(dev, "missing \"reg\" property\n");
                return ERR_PTR(err);
        }
        
        bus = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);
        if (!bus)
                return ERR_PTR(-ENODEV);
                
        cfg = pci_ecam_create(dev, &cfgres, bus->res, ops);
        if (IS_ERR(cfg))
                return cfg;
                
        err = devm_add_action_or_reset(dev, gen_pci_unmap_cfg, cfg);
        if (err)
                return ERR_PTR(err);
        
        return cfg;
}       
```

Before we map the ECAM region, we should first retrieve the register values from
the dtb first through of_address_to_resource function. The register value of the
PCI controller is the memory ranges that can be utilized by the devices attached
to it. The start address and its size are stored in cfgres. 

Although I didn't cover the devm_of_pci_get_host_bridge_resources, it also 
parses the bus-range of the pci controller and add the resource to the 
bridge->windows. This IORESOURCE_BUS resource describes how many sub-buses can 
be supported by the PCI controller. These two information is fed to the 
pci_ecam_create. 


### Generate mapping for ECAM 
Now we have all required information to map ECAM region. Let's see the details. 

```cpp
struct pci_config_window *pci_ecam_create(struct device *dev,
                struct resource *cfgres, struct resource *busr,
                const struct pci_ecam_ops *ops)
{
        unsigned int bus_shift = ops->bus_shift;
        struct pci_config_window *cfg;
        unsigned int bus_range, bus_range_max, bsz;
        struct resource *conflict;
        int err;

        if (busr->start > busr->end)
                return ERR_PTR(-EINVAL);

        cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
        if (!cfg)
                return ERR_PTR(-ENOMEM);

        /* ECAM-compliant platforms need not supply ops->bus_shift */
        if (!bus_shift)
                bus_shift = PCIE_ECAM_BUS_SHIFT;

        cfg->parent = dev;
        cfg->ops = ops;
        cfg->busr.start = busr->start;
        cfg->busr.end = busr->end;
        cfg->busr.flags = IORESOURCE_BUS;
        cfg->bus_shift = bus_shift;
        bus_range = resource_size(&cfg->busr);
        bus_range_max = resource_size(cfgres) >> bus_shift;
        if (bus_range > bus_range_max) {
                bus_range = bus_range_max;
                cfg->busr.end = busr->start + bus_range - 1;
                dev_warn(dev, "ECAM area %pR can only accommodate %pR (reduced from %pR desired)\n",
                         cfgres, &cfg->busr, busr);
        }
        bsz = 1 << bus_shift;

        cfg->res.start = cfgres->start;
        cfg->res.end = cfgres->end;
        cfg->res.flags = IORESOURCE_MEM | IORESOURCE_BUSY;
        cfg->res.name = "PCI ECAM";

        conflict = request_resource_conflict(&iomem_resource, &cfg->res);
        if (conflict) {
                err = -EBUSY;
                dev_err(dev, "can't claim ECAM area %pR: address conflict with %s %pR\n",
                        &cfg->res, conflict->name, conflict);
                goto err_exit;
        }

        if (per_bus_mapping) {
                cfg->winp = kcalloc(bus_range, sizeof(*cfg->winp), GFP_KERNEL);
                if (!cfg->winp)
                        goto err_exit_malloc;
        } else {
                cfg->win = pci_remap_cfgspace(cfgres->start, bus_range * bsz);
                if (!cfg->win)
                        goto err_exit_iomap;
        }

        if (ops->init) {
                err = ops->init(cfg);
                if (err)
                        goto err_exit;
        }
        dev_info(dev, "ECAM at %pR for %pR\n", &cfg->res, &cfg->busr);
        return cfg;

err_exit_iomap:
        dev_err(dev, "ECAM ioremap failed\n");
err_exit_malloc:
        err = -ENOMEM;
err_exit:
        pci_ecam_free(cfg);
        return ERR_PTR(err);
}
```

ECAM designates unique portion of the ECAM range to each device, so that the 
processor communicate with the device through the MMIO to that region. As shown
in the code, each region assigned per device is determined bsz. Also for ECAM,
there is a designated bus_shift PCIE_ECAM_BUS_SHIFT, which means that each 
region size is 0x100000. Also as my PCI controller can have up to 0x00 to 0xff
buses, the total ECAM size should be 0x100000 * 0x100. 

```cpp
static inline void __iomem *pci_remap_cfgspace(phys_addr_t offset,
                                               size_t size)
{
        return ioremap_np(offset, size) ?: ioremap(offset, size);
}

#define ioremap_np(addr, size)  \
        ioremap_prot((addr), (size), (PROT_DEVICE_nGnRnE | PROT_NS_SHARED)

```

After calculating the proper region assigned for the ECAM, it invokes 
pci_remap_cfgspace function to ioremap the provided address range to retrieve
virtual address of the ECAM. This virtual address is stored in cfg->win. From
now on through this virtual address, kernel can directly accesses the ECAM. 

```cpp
/*      
 * struct to hold the mappings of a config space window. This
 * is expected to be used as sysdata for PCI controllers that
 * use ECAM.
 */             
struct pci_config_window {
        struct resource                 res;
        struct resource                 busr;
        unsigned int                    bus_shift;
        void                            *priv;
        const struct pci_ecam_ops       *ops;
        union {
                void __iomem            *win;   /* 64-bit single mapping */
                void __iomem            **winp; /* 32-bit per-bus mapping */
        };
        struct device                   *parent;/* ECAM res was from this dev */
};      
```

In addition to win, it configures other information of the pci_config_windows 
and returns it.

```cpp
int pci_host_common_probe(struct platform_device *pdev)                         
{                                                                               
	......
        /* Parse and map our Configuration Space windows */                     
        cfg = gen_pci_init(dev, bridge, ops);                                   
        if (IS_ERR(cfg))                                                        
                return PTR_ERR(cfg);                                            
                                                                                
        /* Do not reassign resources if probe only */                           
        if (!pci_has_flag(PCI_PROBE_ONLY))                                      
                pci_add_flags(PCI_REASSIGN_ALL_BUS);                            
                                                                                
        bridge->sysdata = cfg;                                                  
        bridge->ops = (struct pci_ops *)&ops->pci_ops;                          
        bridge->msi_domain = true;                                              
                                                                                
        return pci_host_probe(bridge);                                          
}                             
```
The returned pci_config_windows is saved in the sysdata of the bridge. We will
see this field will be utilize later to access ECAM. 

## Scanning buses and its attached device of PCI controller.
We have seen that PCI controller can have multiple buses attached to it. Also,
we have a ECAM region per bus to retrieve its information through MMIO. Let's 
probe the potential buses attached to the controller!
```cpp
int pci_host_probe(struct pci_host_bridge *bridge)
{
        struct pci_bus *bus, *child;
        int ret;

        ret = pci_scan_root_bus_bridge(bridge);
        if (ret < 0) {
                dev_err(bridge->dev.parent, "Scanning root bridge failed");
                return ret;
        }

        bus = bridge->bus;

        /*
         * We insert PCI resources into the iomem_resource and
         * ioport_resource trees in either pci_bus_claim_resources()
         * or pci_bus_assign_resources().
         */
        if (pci_has_flag(PCI_PROBE_ONLY)) {
                pci_bus_claim_resources(bus);
        } else {
                pci_bus_size_bridges(bus);
                pci_bus_assign_resources(bus);

                list_for_each_entry(child, &bus->children, node)
                        pcie_bus_configure_settings(child);
        }

        pci_bus_add_devices(bus);
        return 0;
}
```

```cpp
int pci_scan_root_bus_bridge(struct pci_host_bridge *bridge)
{       
        struct resource_entry *window;
        bool found = false;
        struct pci_bus *b;
        int max, bus, ret;
        
        if (!bridge)
                return -EINVAL;
        
        resource_list_for_each_entry(window, &bridge->windows)
                if (window->res->flags & IORESOURCE_BUS) {
                        bridge->busnr = window->res->start;
                        found = true;
                        break;
                }
        
        ret = pci_register_host_bridge(bridge);
        if (ret < 0)
                return ret;
        
        b = bridge->bus;
        bus = bridge->busnr;
        
        if (!found) {
                dev_info(&b->dev,
                 "No busn resource found for root bus, will use [bus %02x-ff]\n",
                        bus);
                pci_bus_insert_busn_res(b, bus, 255);
        }
        
        max = pci_scan_child_bus(b);
        
        if (!found)
                pci_bus_update_busn_res_end(b, max);
        
        return 0;
}
```

Locating buses is split into two big parts: locating the root bus, and scanning
child buses. Let's see how the root bus is located.

**Kernel data structure to represent each pci bus**
```cpp
struct pci_bus {
        struct list_head node;          /* Node in list of buses */
        struct pci_bus  *parent;        /* Parent bus this bridge is on */
        struct list_head children;      /* List of child buses */
        struct list_head devices;       /* List of devices on this bus */
        struct pci_dev  *self;          /* Bridge device as seen by parent */
        struct list_head slots;         /* List of slots on this bus;
                                           protected by pci_slot_mutex */
        struct resource *resource[PCI_BRIDGE_RESOURCE_NUM];
        struct list_head resources;     /* Address space routed to this bus */
        struct resource busn_res;       /* Bus numbers routed to this bus */

        struct pci_ops  *ops;           /* Configuration access functions */
        void            *sysdata;       /* Hook for sys-specific extension */
        struct proc_dir_entry *procdir; /* Directory entry in /proc/bus/pci */

        unsigned char   number;         /* Bus number */
        unsigned char   primary;        /* Number of primary bridge */
        unsigned char   max_bus_speed;  /* enum pci_bus_speed */
        unsigned char   cur_bus_speed;  /* enum pci_bus_speed */
#ifdef CONFIG_PCI_DOMAINS_GENERIC
        int             domain_nr;
#endif

        char            name[48];

        unsigned short  bridge_ctl;     /* Manage NO_ISA/FBB/et al behaviors */
        pci_bus_flags_t bus_flags;      /* Inherited by child buses */
        struct device           *bridge;
        struct device           dev;
        struct bin_attribute    *legacy_io;     /* Legacy I/O for this bus */
        struct bin_attribute    *legacy_mem;    /* Legacy mem */
        unsigned int            is_added:1;
        unsigned int            unsafe_warn:1;  /* warned about RW1C config write */
};
```

```cpp
static int pci_register_host_bridge(struct pci_host_bridge *bridge)
{
        struct device *parent = bridge->dev.parent;
        struct resource_entry *window, *next, *n;
        struct pci_bus *bus, *b;
        resource_size_t offset, next_offset;
        LIST_HEAD(resources);
        struct resource *res, *next_res;
        char addr[64], *fmt;
        const char *name;
        int err;

        bus = pci_alloc_bus(NULL);
        if (!bus)
                return -ENOMEM;

        bridge->bus = bus;

	//bridge->sysdata is virtual address of ECAM
	//bridge->ops is pci_generic_ecam_ops
        bus->sysdata = bridge->sysdata;
        bus->ops = bridge->ops;
        bus->number = bus->busn_res.start = bridge->busnr;
#ifdef CONFIG_PCI_DOMAINS_GENERIC
        if (bridge->domain_nr == PCI_DOMAIN_NR_NOT_SET)
                bus->domain_nr = pci_bus_find_domain_nr(bus, parent);
        else
                bus->domain_nr = bridge->domain_nr;
        if (bus->domain_nr < 0) {
                err = bus->domain_nr;
                goto free;
        }
#endif
```
First it allocates new pci_bus for the root bus and then initialize few fields
such as sysdata, ops, and number. The number is the start value of the bus_range
specified in the DTB. 

```cpp
static int pci_register_host_bridge(struct pci_host_bridge *bridge) {
	......
        b = pci_find_bus(pci_domain_nr(bus), bridge->busnr);
        if (b) {
                /* Ignore it if we already got here via a different bridge */
                dev_dbg(&b->dev, "bus already known\n");
                err = -EEXIST;
                goto free;
        }

        dev_set_name(&bridge->dev, "pci%04x:%02x", pci_domain_nr(bus),
                     bridge->busnr);

        err = pcibios_root_bridge_prepare(bridge);
        if (err)
                goto free;

        /* Temporarily move resources off the list */
        list_splice_init(&bridge->windows, &resources);
        err = device_add(&bridge->dev);
        if (err) {
                put_device(&bridge->dev);
                goto free;
        }
        bus->bridge = get_device(&bridge->dev);
        device_enable_async_suspend(bus->bridge);
        pci_set_bus_of_node(bus);
        pci_set_bus_msi_domain(bus);
        if (bridge->msi_domain && !dev_get_msi_domain(&bus->dev) &&
            !pci_host_of_has_msi_map(parent))
                bus->bus_flags |= PCI_BUS_FLAGS_NO_MSI;

        if (!parent)
                set_dev_node(bus->bridge, pcibus_to_node(bus));

        bus->dev.class = &pcibus_class;
        bus->dev.parent = bus->bridge;

        dev_set_name(&bus->dev, "%04x:%02x", pci_domain_nr(bus), bus->number);
        name = dev_name(&bus->dev);

        err = device_register(&bus->dev);
        if (err)
                goto unregister;

        pcibios_add_bus(bus);

        if (bus->ops->add_bus) {
                err = bus->ops->add_bus(bus);
                if (WARN_ON(err < 0))
                        dev_err(&bus->dev, "failed to add bus: %d\n", err);
        }
```

The attachment of the bus is done by pci_ecam_add_bus function, which is the 
add_bus of the pci_generic_ecam_ops. Also note that the resources, a list of the
resources, is generated, and resources of the pci_host_bridge is copied into the
resources list.

```cpp
/*
 * On 64-bit systems, we do a single ioremap for the whole config space
 * since we have enough virtual address range available.  On 32-bit, we
 * ioremap the config space for each bus individually.
 */
static const bool per_bus_mapping = !IS_ENABLED(CONFIG_64BIT) list.tatic int pci_ecam_add_bus(struct pci_bus *bus)
{       
        struct pci_config_window *cfg = bus->sysdata;
        unsigned int bsz = 1 << cfg->bus_shift;
        unsigned int busn = bus->number;
        phys_addr_t start;
                
        if (!per_bus_mapping)
                return 0;
        
        if (busn < cfg->busr.start || busn > cfg->busr.end)
                return -EINVAL;
        
        busn -= cfg->busr.start;
        start = cfg->res.start + busn * bsz;
        
        cfg->winp[busn] = pci_remap_cfgspace(start, bsz);
        if (!cfg->winp[busn])
                return -ENOMEM;
        
        return 0;
}               
```
Because we assume that the platform is 64 bit system, per_bus_mapping is false,
which means that we don't need separate config space per bus. Instead, we can 
access the config space of all buses through single ECAM. Therefore, instead of 
generating another mapping to the ECAM bus dedicated for bus, it returns. 

```cpp
static int pci_register_host_bridge(struct pci_host_bridge *bridge) { 
	......
        /* Create legacy_io and legacy_mem files for this bus */
        pci_create_legacy_files(bus);

        if (parent)
                dev_info(parent, "PCI host bridge to bus %s\n", name);
        else
                pr_info("PCI host bridge to bus %s\n", name);

        if (nr_node_ids > 1 && pcibus_to_node(bus) == NUMA_NO_NODE)
                dev_warn(&bus->dev, "Unknown NUMA node; performance will be reduced\n");

        /* Coalesce contiguous windows */
        resource_list_for_each_entry_safe(window, n, &resources) {
                if (list_is_last(&window->node, &resources))
                        break;

                next = list_next_entry(window, node);
                offset = window->offset;
                res = window->res;
                next_offset = next->offset;
                next_res = next->res;

                if (res->flags != next_res->flags || offset != next_offset)
                        continue;

                if (res->end + 1 == next_res->start) {
                        next_res->start = res->start;
                        res->flags = res->start = res->end = 0;
                }
        }

        /* Add initial resources to the bus */
        resource_list_for_each_entry_safe(window, n, &resources) {
                offset = window->offset;
                res = window->res;
                if (!res->end)
                        continue;

                list_move_tail(&window->node, &bridge->windows);

                if (res->flags & IORESOURCE_BUS)
                        pci_bus_insert_busn_res(bus, bus->number, res->end);
                else
                        pci_bus_add_resource(bus, res, 0);

                if (offset) {
                        if (resource_type(res) == IORESOURCE_IO)
                                fmt = " (bus address [%#06llx-%#06llx])";
                        else
                                fmt = " (bus address [%#010llx-%#010llx])";

                        snprintf(addr, sizeof(addr), fmt,
                                 (unsigned long long)(res->start - offset),
                                 (unsigned long long)(res->end - offset));
                } else
                        addr[0] = '\0';
                
                dev_info(&bus->dev, "root bus resource %pR%s\n", res, addr);
        }

        down_write(&pci_bus_sem);
        list_add_tail(&bus->node, &pci_root_buses);
        up_write(&pci_bus_sem); 
                        
        return 0;

unregister:
        put_device(&bridge->dev);
        device_del(&bridge->dev);
        
free:           
#ifdef CONFIG_PCI_DOMAINS_GENERIC
        pci_bus_release_domain_nr(bus, parent);
#endif
        kfree(bus);
        return err;
}
```
The last part of the root bus initialization is registering PCI controller 
resources to the root bus. In the DTB, there were three resources belong to the 
controller, and they are allocated as the resources of the root bus. 

```cpp
void pci_bus_add_resource(struct pci_bus *bus, struct resource *res,
                          unsigned int flags)
{
        struct pci_bus_resource *bus_res;
        
        bus_res = kzalloc(sizeof(struct pci_bus_resource), GFP_KERNEL);
        if (!bus_res) {
                dev_err(&bus->dev, "can't add %pR resource\n", res);
                return;
        }
        
        bus_res->res = res;
        bus_res->flags = flags;
        list_add_tail(&bus_res->list, &bus->resources);
}       
```
The registration can be done just adding the bridge's resource to another list 
maintained by the bus, bus->resources. 

## Scanning child bus and attached devices
Now we have root bus (bus # 0x00). However, as we've seen before, there could 
be more than 1 buses in the pci controller up to 0xff. We will see how kernel 
locates potential buses attached to the controller and its devices. Let's go 
back to the pci_scan_root_bus_bridge. It invokes pci_scan_child_bus after 
locating the root bus.

```cpp
int pci_scan_root_bus_bridge(struct pci_host_bridge *bridge)
{       
	......
        max = pci_scan_child_bus(b);
	......
}
        
```cpp
/**
 * pci_scan_child_bus() - Scan devices below a bus
 * @bus: Bus to scan for devices
 *      
 * Scans devices below @bus including subordinate buses. Returns new
 * subordinate number including all the found devices.
 */              
unsigned int pci_scan_child_bus(struct pci_bus *bus)
{               
        return pci_scan_child_bus_extend(bus, 0);
}                      
```

The actual bus registration is done by pci_scan_child_bus_extend. Since there 
are two types of PCI devices can be attached to the bus, end device and bridge,
depending on the device type, it needs different initialization. Compared to 
end device, through the bridge, the bus can be expanded to another bus. 
Therefore, it adopts depth first search. Whenever it encounters a bridge, it 
moves the search window to the next bridge and continue search

```cpp
/**                     
 * pci_scan_child_bus_extend() - Scan devices below a bus
 * @bus: Bus to scan for devices
 * @available_buses: Total number of buses available (%0 does not try to
 *                   extend beyond the minimal)
 *
 * Scans devices below @bus including subordinate buses. Returns new
 * subordinate number including all the found devices. Passing
 * @available_buses causes the remaining bus space to be distributed
 * equally between hotplug-capable bridges to allow future extension of the
 * hierarchy.                   
 */                             
static unsigned int pci_scan_child_bus_extend(struct pci_bus *bus,
                                              unsigned int available_buses)
{                               
        unsigned int used_buses, normal_bridges = 0, hotplug_bridges = 0;
        unsigned int start = bus->busn_res.start;
        unsigned int devfn, cmax, max = start;
        struct pci_dev *dev;                    
                                        
        dev_dbg(&bus->dev, "scanning bus\n");
                                
        /* Go find them, Rover! */ 
        for (devfn = 0; devfn < 256; devfn += 8)
                pci_scan_slot(bus, devfn);

        /* Reserve buses for SR-IOV capability */
        used_buses = pci_iov_bus_range(bus);
        max += used_buses;
                                        
        /*                              
         * After performing arch-dependent fixup of the bus, look behind
         * all PCI-to-PCI bridges on this bus.
         */             
        if (!bus->is_added) {
                dev_dbg(&bus->dev, "fixups for bus\n");
                pcibios_fixup_bus(bus);
                bus->is_added = 1;
        }

        /*
         * Calculate how many hotplug bridges and normal bridges there
         * are on this bus. We will distribute the additional available
         * buses between hotplug bridges.
         */
        for_each_pci_bridge(dev, bus) {
                if (dev->is_hotplug_bridge)
                        hotplug_bridges++;
                else
                        normal_bridges++;
        }

        /*
         * Scan bridges that are already configured. We don't touch them
         * unless they are misconfigured (which will be done in the second
         * scan below).
         */
        for_each_pci_bridge(dev, bus) {
                cmax = max;
                max = pci_scan_bridge_extend(bus, dev, max, 0, 0);

                /*
                 * Reserve one bus for each bridge now to avoid extending
                 * hotplug bridges too much during the second scan below.
                 */
                used_buses++;
                if (max - cmax > 1)
                        used_buses += max - cmax - 1;
        }

        /* Scan bridges that need to be reconfigured */
        for_each_pci_bridge(dev, bus) {
                unsigned int buses = 0;

                if (!hotplug_bridges && normal_bridges == 1) {
                        /*
                         * There is only one bridge on the bus (upstream
                         * port) so it gets all available buses which it
                         * can then distribute to the possible hotplug
                         * bridges below.
                         */
                        buses = available_buses;
                } else if (dev->is_hotplug_bridge) {
                        /*
                         * Distribute the extra buses between hotplug
                         * bridges if any.
                         */
                        buses = available_buses / hotplug_bridges;
                        buses = min(buses, available_buses - used_buses + 1);
                }

                cmax = max;
                max = pci_scan_bridge_extend(bus, dev, cmax, buses, 1);
                /* One bus is already accounted so don't add it again */
                if (max - cmax > 1)
                        used_buses += max - cmax - 1;
        }

        /*
         * Make sure a hotplug bridge has at least the minimum requested
         * number of buses but allow it to grow up to the maximum available
         * bus number if there is room.
         */
        if (bus->self && bus->self->is_hotplug_bridge) {
                used_buses = max_t(unsigned int, available_buses,
                                   pci_hotplug_bus_size - 1);
                if (max - start < used_buses) {
                        max = start + used_buses;

                        /* Do not allocate more buses than we have room left */
                        if (max > bus->busn_res.end)
                                max = bus->busn_res.end;

                        dev_dbg(&bus->dev, "%pR extended by %#02x\n",
                                &bus->busn_res, max - start);
                }
        }

        /*
         * We've scanned the bus and so we know all about what's on
         * the other side of any bridges that may be on this bus plus
         * any devices.
         *
         * Return how far we've got finding sub-buses.
         */
        dev_dbg(&bus->dev, "bus scan returning with max=%02x\n", max);
        return max;
}
```


### Scanning slots on the bus
```cpp
/**
 * pci_scan_slot - Scan a PCI slot on a bus for devices
 * @bus: PCI bus to scan
 * @devfn: slot number to scan (must have zero function)
 *
 * Scan a PCI slot on the specified PCI bus for devices, adding
 * discovered devices to the @bus->devices list.  New devices
 * will not have is_added set.
 *
 * Returns the number of new devices found.
 */ 
int pci_scan_slot(struct pci_bus *bus, int devfn)
{       
        struct pci_dev *dev;
        int fn = 0, nr = 0;
        
        if (only_one_child(bus) && (devfn > 0))
                return 0; /* Already scanned the entire slot */
        
        do {    
                dev = pci_scan_single_device(bus, devfn + fn);
                if (dev) { 
                        if (!pci_dev_is_added(dev))
                                nr++;
                        if (fn > 0)
                                dev->multifunction = 1;
                } else if (fn == 0) {
                        /*
                         * Function 0 is required unless we are running on
                         * a hypervisor that passes through individual PCI
                         * functions.
                         */
                        if (!hypervisor_isolated_pci_functions())
                                break;
                }
                fn = next_fn(bus, dev, fn);
        } while (fn >= 0);
        
        /* Only one slot has PCIe device */
        if (bus->self && nr)
                pcie_aspm_init_link_state(bus->self);
        
        return nr;
}
EXPORT_SYMBOL(pci_scan_slot);
```
The main loop in the function checks if there is any attached device exist in 
particular slot (devfn). Also, if the located device is multi-function device, 
it further searches additional functions of the device. As each device can have 
up to 8 functions, the loop can be continues until 8 different functions can be
located. Let's see how the bus can scan each slot to confirm if there is any 
attached device or not. 



```cpp
struct pci_dev *pci_scan_single_device(struct pci_bus *bus, int devfn)
{
        struct pci_dev *dev;

        dev = pci_get_slot(bus, devfn);
        if (dev) {
                pci_dev_put(dev);
                return dev;
        }

        dev = pci_scan_device(bus, devfn);
        if (!dev)
                return NULL;
        
        pci_device_add(dev, bus);
                
        return dev;
}       
```
Scanning the device that potentially attached to specific slot (devfn) can be 
achieved by two functions. pci_get_slot and pci_scan_device. Because new device 
will be added to the devices field of the bus when it is found on a slot, it can
easily retrieve the device if the device has been seen already on the bus by 
comparing the slot number. 


```cpp
struct pci_dev *pci_get_slot(struct pci_bus *bus, unsigned int devfn)
{       
        struct pci_dev *dev;
        
        down_read(&pci_bus_sem);
        
        list_for_each_entry(dev, &bus->devices, bus_list) {
                if (dev->devfn == devfn)
                        goto out;
        }
        
        dev = NULL;
 out:   
        pci_dev_get(dev);
        up_read(&pci_bus_sem);
        return dev;
}
```
If there is a registered device in the bus->devices matching its devfn to the 
slot that we want to search, it increases the reference count and return the 
device. However, if it returns NULL, which means there were no devices matching 
the devfn in the bus, it falls through to the pci_scan_device that actually 
read ECAM designated for the slot on that bus. 


### Scanning slot on the bus through ECAM
```cpp
/*
 * Read the config data for a PCI device, sanity-check it,
 * and fill in the dev structure.
 */
static struct pci_dev *pci_scan_device(struct pci_bus *bus, int devfn)
{       
        struct pci_dev *dev;
        u32 l;  
        
        if (!pci_bus_read_dev_vendor_id(bus, devfn, &l, 60*1000))
                return NULL;
        
        dev = pci_alloc_dev(bus);
        if (!dev)
                return NULL;

        dev->devfn = devfn;
        dev->vendor = l & 0xffff;
        dev->device = (l >> 16) & 0xffff;

        if (pci_setup_device(dev)) {
                pci_bus_put(dev->bus);
                kfree(dev);
                return NULL;
        }

        return dev;                         
	}         

```


```cpp
bool pci_bus_read_dev_vendor_id(struct pci_bus *bus, int devfn, u32 *l,
                                int timeout)
{
#ifdef CONFIG_PCI_QUIRKS
        struct pci_dev *bridge = bus->self;
        
        /*
         * Certain IDT switches have an issue where they improperly trigger
         * ACS Source Validation errors on completions for config reads.
         */
        if (bridge && bridge->vendor == PCI_VENDOR_ID_IDT &&
            bridge->device == 0x80b5)
                return pci_idt_bus_quirk(bus, devfn, l, timeout);
#endif  
        
        return pci_bus_generic_read_dev_vendor_id(bus, devfn, l, timeout);
}
```
First it needs to read vendor id of the possibly connected device to the slot 
number (devfn). The u32 pointer l is a variable to store information retrieved
from the PCI config space of the slot. In this case, this information will 
convey slot information including device and vendor ID.


```cpp
bool pci_bus_generic_read_dev_vendor_id(struct pci_bus *bus, int devfn, u32 *l,
                                        int timeout)
{       
        if (pci_bus_read_config_dword(bus, devfn, PCI_VENDOR_ID, l))
                return false;
        
        /* Some broken boards return 0 or ~0 (PCI_ERROR_RESPONSE) if a slot is empty: */
        if (PCI_POSSIBLE_ERROR(*l) || *l == 0x00000000 ||
            *l == 0x0000ffff || *l == 0xffff0000)
                return false;
        
        if (pci_bus_crs_vendor_id(*l))
                return pci_bus_wait_crs(bus, devfn, l, timeout);
        
        return true;
}
```

If the error value or NULL value is read from the memory area supposed to 
provide the vendor ID of the slot in the bus, it means that there is no attached
devices in the slot. In that case it just returns false and search the next slot.


```cpp
#define PCI_OP_READ(size, type, len) \
int noinline pci_bus_read_config_##size \
        (struct pci_bus *bus, unsigned int devfn, int pos, type *value) \
{                                                                       \
        int res;                                                        \
        unsigned long flags;                                            \
        u32 data = 0;                                                   \
        if (PCI_##size##_BAD) return PCIBIOS_BAD_REGISTER_NUMBER;       \
        pci_lock_config(flags);                                         \
        res = bus->ops->read(bus, devfn, pos, len, &data);              \
        if (res)                                                        \
                PCI_SET_ERROR_RESPONSE(value);                          \
        else                                                            \
                *value = (type)data;                                    \
        pci_unlock_config(flags);                                       \
        return res;                                                     \
}
        
#define PCI_OP_WRITE(size, type, len) \
int noinline pci_bus_write_config_##size \
        (struct pci_bus *bus, unsigned int devfn, int pos, type value)  \
{                                                                       \
        int res;                                                        \
        unsigned long flags;                                            \
        if (PCI_##size##_BAD) return PCIBIOS_BAD_REGISTER_NUMBER;       \
        pci_lock_config(flags);                                         \
        res = bus->ops->write(bus, devfn, pos, len, value);             \
        pci_unlock_config(flags);                                       \
        return res;                                                     \
}

PCI_OP_READ(byte, u8, 1)
PCI_OP_READ(word, u16, 2)
PCI_OP_READ(dword, u32, 4)
PCI_OP_WRITE(byte, u8, 1)
PCI_OP_WRITE(word, u16, 2)
PCI_OP_WRITE(dword, u32, 4)
```

To easily read from and write to the configuration space, kernel provides macro
functions. The operation is simple, calling read operation of the bus, which is 
the ops of the bridge. Note that these operations could be different based on 
which bridge is installed in the platform. Let's take a look at the details of 
the pci read function.

```cpp
/* ECAM ops */  
const struct pci_ecam_ops pci_generic_ecam_ops = {
        .pci_ops        = {     
                .add_bus        = pci_ecam_add_bus,
                .remove_bus     = pci_ecam_remove_bus,
                .map_bus        = pci_ecam_map_bus,
                .read           = pci_generic_config_read,
                .write          = pci_generic_config_write,
        } 
};
```

```cpp
int pci_generic_config_read(struct pci_bus *bus, unsigned int devfn,
                            int where, int size, u32 *val)
{
        void __iomem *addr;

        addr = bus->ops->map_bus(bus, devfn, where);
        if (!addr)
                return PCIBIOS_DEVICE_NOT_FOUND;

        if (size == 1)
                *val = readb(addr);
        else if (size == 2)
                *val = readw(addr);
        else
                *val = readl(addr);

        return PCIBIOS_SUCCESSFUL;
}
```

Before reading the config space, it first invokes the map_bus function to 
retrieve address to be read or written. 


```cpp
void __iomem *pci_ecam_map_bus(struct pci_bus *bus, unsigned int devfn,
                               int where)
{
        struct pci_config_window *cfg = bus->sysdata;
        unsigned int bus_shift = cfg->ops->bus_shift;
        unsigned int devfn_shift = cfg->ops->bus_shift - 8;
        unsigned int busn = bus->number;
        void __iomem *base;
        u32 bus_offset, devfn_offset;

        if (busn < cfg->busr.start || busn > cfg->busr.end)
                return NULL;

        busn -= cfg->busr.start;
        if (per_bus_mapping) {
                base = cfg->winp[busn];
                busn = 0; 
        } else
                base = cfg->win;

        if (cfg->ops->bus_shift) {
                bus_offset = (busn & PCIE_ECAM_BUS_MASK) << bus_shift;
                devfn_offset = (devfn & PCIE_ECAM_DEVFN_MASK) << devfn_shift;
                where &= PCIE_ECAM_REG_MASK;

                return base + (bus_offset | devfn_offset | where);
        }

        return base + PCIE_ECAM_OFFSET(busn, devfn, where);
}
```

Based on whether it has multiple parsed buses or single bus, it retrieves the 
base address of the bus. As my platform is aarch64, it must have one single ECAM
region, which means single base address. Also, it adds PCIE_ECAM_OFFSET to the 
base to calculate exact location to read on ECAM. This offset is determined 
based on current bus number, slot number, and the offset of the configuration 
that it wants to read (e.g., PCI_VENDOR_ID). Let's see how exactly this offset 
is calculated.


```cpp
#define PCIE_ECAM_BUS_SHIFT     20 /* Bus number */ 
#define PCIE_ECAM_DEVFN_SHIFT   12 /* Device and Function number */

#define PCIE_ECAM_BUS_MASK      0xff
#define PCIE_ECAM_DEVFN_MASK    0xff
#define PCIE_ECAM_REG_MASK      0xfff /* Limit offset to a maximum of 4K */
        
#define PCIE_ECAM_BUS(x)        (((x) & PCIE_ECAM_BUS_MASK) << PCIE_ECAM_BUS_SHIFT)
#define PCIE_ECAM_DEVFN(x)      (((x) & PCIE_ECAM_DEVFN_MASK) << PCIE_ECAM_DEVFN_SHIFT)
#define PCIE_ECAM_REG(x)        ((x) & PCIE_ECAM_REG_MASK)
        
#define PCIE_ECAM_OFFSET(bus, devfn, where) \
        (PCIE_ECAM_BUS(bus) | \
         PCIE_ECAM_DEVFN(devfn) | \
         PCIE_ECAM_REG(where))
```
Remember that ECAM region was mapped in the virtual address space before. The 
total size of the ECAM region is calculated by #of bus (0x100) multiply by the 
size of each ECAM region (0x100000). Also, each ECAM region dedicated to each 
bus can be sub-divided into 4K size 256 config space. Because each bus can have 
up to 256 slots, unique and equal amount of config space should be allocated per 
device in the bus. As each bus has 0x100000 size ECAM space in total, if it is 
divided by the 256 slots, the size of each config space dedicated to each device
will be 4096. In other words, each device in the bus can have 4K config space 
dedicated its device. Since we already map the entire ECAM region, processor can
read/write to the bus config space through the calculated address. 

```cpp
struct pci_dev *pci_alloc_dev(struct pci_bus *bus)
{
        struct pci_dev *dev;
        
        dev = kzalloc(sizeof(struct pci_dev), GFP_KERNEL);
        if (!dev)
                return NULL;

        INIT_LIST_HEAD(&dev->bus_list);
        dev->dev.type = &pci_dev_type;
        dev->bus = pci_bus_get(bus);
        dev->driver_exclusive_resource = (struct resource) {
                .name = "PCI Exclusive",
                .start = 0,
                .end = -1,
        };

#ifdef CONFIG_PCI_MSI
        raw_spin_lock_init(&dev->msi_lock);
#endif
        return dev;
}
```
A valid output of the read from the vendor id field in the ECAM region of the 
slot indicates that there is a attached pci device. To maintain the device 
information, kernel allocates new pci_dev structure through pci_alloc_dev.

### Device initialization
As a result of first ECAM read, we can retrieve device information such as the 
vendor and device ID. However, there are lots of other information of the device 
that can be retrieved from the config space and remaining initialization for the 
device. This will be done by pci_setup_device function.

```cpp
 * pci_setup_device - Fill in class and map information of a device
 * @dev: the device structure to fill
 *
 * Initialize the device structure with information about the device's
 * vendor,class,memory and IO-space addresses, IRQ lines etc.
 * Called at initialisation of the PCI subsystem and by CardBus services.
 * Returns 0 on success and negative if unknown type of device (not normal,
 * bridge or CardBus).  
 */                     
int pci_setup_device(struct pci_dev *dev)
{               
        u32 class;
        u16 cmd;
        u8 hdr_type;
        int pos = 0;
        struct pci_bus_region region;               
        struct resource *res;
                
        hdr_type = pci_hdr_type(dev);
                
        dev->sysdata = dev->bus->sysdata;
        dev->dev.parent = dev->bus->bridge;
        dev->dev.bus = &pci_bus_type;
        dev->hdr_type = hdr_type & 0x7f;
        dev->multifunction = !!(hdr_type & 0x80);
        dev->error_state = pci_channel_io_normal;
        set_pcie_port_type(dev);

        pci_set_of_node(dev);
        pci_set_acpi_fwnode(dev);
                
        pci_dev_assign_slot(dev);
                        
        /*              
         * Assume 32-bit PCI; let 64-bit PCI cards (which are far rarer)
         * set this higher, assuming the system even supports it.
         */                     
        dev->dma_mask = 0xffffffff;

        dev_set_name(&dev->dev, "%04x:%02x:%02x.%d", pci_domain_nr(dev->bus),
                     dev->bus->number, PCI_SLOT(dev->devfn),
                     PCI_FUNC(dev->devfn));

        class = pci_class(dev);

        dev->revision = class & 0xff;
        dev->class = class >> 8;                    /* upper 3 bytes */

        if (pci_early_dump)
                early_dump_pci_device(dev);

        /* Need to have dev->class ready */
        dev->cfg_size = pci_cfg_space_size(dev);

        /* Need to have dev->cfg_size ready */
        set_pcie_thunderbolt(dev);

        set_pcie_untrusted(dev);

        /* "Unknown power state" */
        dev->current_state = PCI_UNKNOWN;

        /* Early fixups, before probing the BARs */
        pci_fixup_device(pci_fixup_early, dev);

        pci_set_removable(dev);

        pci_info(dev, "[%04x:%04x] type %02x class %#08x\n",
                 dev->vendor, dev->device, dev->hdr_type, dev->class);

        /* Device class may be changed after fixup */
        class = dev->class >> 8;

        if (dev->non_compliant_bars && !dev->mmio_always_on) {
                pci_read_config_word(dev, PCI_COMMAND, &cmd);
                if (cmd & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY)) {
                        pci_info(dev, "device has non-compliant BARs; disabling IO/MEM decoding\n");
                        cmd &= ~PCI_COMMAND_IO;
                        cmd &= ~PCI_COMMAND_MEMORY;
                        pci_write_config_word(dev, PCI_COMMAND, cmd);
                }
        }

        dev->broken_intx_masking = pci_intx_mask_broken(dev);
```
pci_setup_device consists of two big parts. The first part is to retrieve 
device specific informatio such as class, header type of the device, device 
capabilities through another ECAM read. Based on the information, each device 
can be configured differently. 

```cpp
static u8 pci_hdr_type(struct pci_dev *dev)
{       
        u8 hdr_type;

#ifdef CONFIG_PCI_IOV
        if (dev->is_virtfn)
                return dev->physfn->sriov->hdr_type;
#endif  
        pci_read_config_byte(dev, PCI_HEADER_TYPE, &hdr_type);
        return hdr_type;
}
```

The header type determines whether the attached device is the end device 
(PCI_HEADER_TYPE_NORMAL) or pci bridge (PCI_HEADER_TYPE_BRIDGE). Let's assume 
that the device is end device not the bridge. 

### Capabilities of the PCI
Before I delve into next part, I will slightly detour and must cover what is the
capabilities and how its information is traversed. After retrieving the 
pci_hdr_type, function set_pcie_port_type is invoked where the capabilities 
linked list is traversed to locate PCI_CAP_ID_EXP to confirm if the device is 
PCIe or not. 

```cpp
void set_pcie_port_type(struct pci_dev *pdev)
{       
        int pos;
        u16 reg16; 
        int type;
        struct pci_dev *parent;
        
        pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
        if (!pos)
                return;
        
        pdev->pcie_cap = pos;
        pci_read_config_word(pdev, pos + PCI_EXP_FLAGS, &reg16);
        pdev->pcie_flags_reg = reg16;
        pci_read_config_dword(pdev, pos + PCI_EXP_DEVCAP, &pdev->devcap);
        pdev->pcie_mpss = FIELD_GET(PCI_EXP_DEVCAP_PAYLOAD, pdev->devcap);

        parent = pci_upstream_bridge(pdev);
        if (!parent)
                return;

        /*
         * Some systems do not identify their upstream/downstream ports
         * correctly so detect impossible configurations here and correct
         * the port type accordingly.
         */ 
        type = pci_pcie_type(pdev);
        if (type == PCI_EXP_TYPE_DOWNSTREAM) {
                /*
                 * If pdev claims to be downstream port but the parent
                 * device is also downstream port assume pdev is actually
                 * upstream port.
                 */
                if (pcie_downstream_port(parent)) {
                        pci_info(pdev, "claims to be downstream port but is acting as upstream port, correcting type\n");
                        pdev->pcie_flags_reg &= ~PCI_EXP_FLAGS_TYPE;
                        pdev->pcie_flags_reg |= PCI_EXP_TYPE_UPSTREAM;
                }
        } else if (type == PCI_EXP_TYPE_UPSTREAM) {
                /*
                 * If pdev claims to be upstream port but the parent
                 * device is also upstream port assume pdev is actually
                 * downstream port.
                 */
                if (pci_pcie_type(parent) == PCI_EXP_TYPE_UPSTREAM) {
                        pci_info(pdev, "claims to be upstream port but is acting as downstream port, correcting type\n");
                        pdev->pcie_flags_reg &= ~PCI_EXP_FLAGS_TYPE;
                        pdev->pcie_flags_reg |= PCI_EXP_TYPE_DOWNSTREAM;
                }
        }
}
```

Note that pci_find_capability is invoked to search if the PCI device supports 
PCI_CAP_ID_EXP capability which indicates the device is PCIe. Let's see how 
the capabilities can be traversed and located through the ECAM and what does 
the return value pos means. 

```cpp
/**
 * pci_find_capability - query for devices' capabilities
 * @dev: PCI device to query
 * @cap: capability code
 *
 * Tell if a device supports a given PCI capability.
 * Returns the address of the requested capability structure within the
 * device's PCI configuration space or 0 in case the device does not
 * support it.  Possible values for @cap include:
 *
 *  %PCI_CAP_ID_PM           Power Management
 *  %PCI_CAP_ID_AGP          Accelerated Graphics Port
 *  %PCI_CAP_ID_VPD          Vital Product Data
 *  %PCI_CAP_ID_SLOTID       Slot Identification
 *  %PCI_CAP_ID_MSI          Message Signalled Interrupts
 *  %PCI_CAP_ID_CHSWP        CompactPCI HotSwap
 *  %PCI_CAP_ID_PCIX         PCI-X
 *  %PCI_CAP_ID_EXP          PCI Express
 */
u8 pci_find_capability(struct pci_dev *dev, int cap)
{
        u8 pos;

        pos = __pci_bus_find_cap_start(dev->bus, dev->devfn, dev->hdr_type);
        if (pos)
                pos = __pci_find_next_cap(dev->bus, dev->devfn, pos, cap);

        return pos;
}
```

```cpp
static u8 __pci_bus_find_cap_start(struct pci_bus *bus,
                                    unsigned int devfn, u8 hdr_type)
{
        u16 status;

        pci_bus_read_config_word(bus, devfn, PCI_STATUS, &status);
        if (!(status & PCI_STATUS_CAP_LIST))
                return 0;

        switch (hdr_type) {
        case PCI_HEADER_TYPE_NORMAL:
        case PCI_HEADER_TYPE_BRIDGE:
                return PCI_CAPABILITY_LIST;
        case PCI_HEADER_TYPE_CARDBUS:
                return PCI_CB_CAPABILITY_LIST;
        }

        return 0;
```

First it read the PCI_STATUS through the ECAM and check if the device supports
the PCI capabilities list. If the status has PCI_STATUS_CAP_LIST flag, it means
that the list can be accessible through the ECAM. Now we need the offset of 
head of the capabilities list. Based on the device type (hdr_type) the offset
can be different in the ECAM region. Because we assume that the current device 
is PCI_HEADER_TYPE_NORMAL, it will be PCI_CAPABILITY_LIST.


```cpp
static u8 __pci_find_next_cap(struct pci_bus *bus, unsigned int devfn,
                              u8 pos, int cap)
{               
        int ttl = PCI_FIND_CAP_TTL;
        
        return __pci_find_next_cap_ttl(bus, devfn, pos, cap, &ttl);
}

static u8 __pci_find_next_cap_ttl(struct pci_bus *bus, unsigned int devfn,
                                  u8 pos, int cap, int *ttl)
{       
        u8 id;
        u16 ent;
        
        pci_bus_read_config_byte(bus, devfn, pos, &pos);
        
        while ((*ttl)--) {
                if (pos < 0x40)
                        break;
                pos &= ~3;
                pci_bus_read_config_word(bus, devfn, pos, &ent);
                
                id = ent & 0xff;
                if (id == 0xff)
                        break; 
                if (id == cap) 
                        return pos;
                pos = (ent >> 8);
        }
        return 0;
}
```
As the capabilities list is a linked list, the head pointer should be traversed
until we find the capability that we want to search or reaches end of the list.
Each item of the linked list consists of two bytes: upper byte for the next 
capability address and the lower byte for type of the capability. When the 
capability is found in the list, it returns the offset of the capability in the 
ECAM, which will be stores in the pdev->pcie_cap. This is stored in the pdev 
because we don't want to traverse the linked list every time to check if the 
device has what capabilities. There are other member field for other 
capabilities too in the pci_dev.

### Continue of the pci_setup_device
Let's come back to the pci_setup_device and see how other information of the 
pci device can be read and configured. 

```cpp
        switch (dev->hdr_type) {                    /* header type */
        case PCI_HEADER_TYPE_NORMAL:                /* standard header */
                if (class == PCI_CLASS_BRIDGE_PCI)
                        goto bad;
                pci_read_irq(dev);
                pci_read_bases(dev, 6, PCI_ROM_ADDRESS);

                pci_subsystem_ids(dev, &dev->subsystem_vendor, &dev->subsystem_device);

                /*
                 * Do the ugly legacy mode stuff here rather than broken chip
                 * quirk code. Legacy mode ATA controllers have fixed
                 * addresses. These are not always echoed in BAR0-3, and
                 * BAR0-3 in a few cases contain junk!
                 */
                if (class == PCI_CLASS_STORAGE_IDE) {
                        u8 progif;
                        pci_read_config_byte(dev, PCI_CLASS_PROG, &progif);
                        if ((progif & 1) == 0) {
                                region.start = 0x1F0;
                                region.end = 0x1F7;
                                res = &dev->resource[0];
                                res->flags = LEGACY_IO_RESOURCE;
                                pcibios_bus_to_resource(dev->bus, res, &region);
                                pci_info(dev, "legacy IDE quirk: reg 0x10: %pR\n",
                                         res);
                                region.start = 0x3F6;
                                region.end = 0x3F6;
                                res = &dev->resource[1];
                                res->flags = LEGACY_IO_RESOURCE;
                                pcibios_bus_to_resource(dev->bus, res, &region);
                                pci_info(dev, "legacy IDE quirk: reg 0x14: %pR\n",
                                         res);
                        }
                        if ((progif & 4) == 0) {
                                region.start = 0x170;
                                region.end = 0x177;
                                res = &dev->resource[2];
                                res->flags = LEGACY_IO_RESOURCE;
                                pcibios_bus_to_resource(dev->bus, res, &region);
                                pci_info(dev, "legacy IDE quirk: reg 0x18: %pR\n",
                                         res);
                                region.start = 0x376;
                                region.end = 0x376;
                                res = &dev->resource[3];
                                res->flags = LEGACY_IO_RESOURCE;
                                pcibios_bus_to_resource(dev->bus, res, &region);
                                pci_info(dev, "legacy IDE quirk: reg 0x1c: %pR\n",
                                         res);
                        }
                }
                break;
	......
        /* We found a fine healthy device, go go go... */
        return 0;
}
```

To initialize the device connected to the bus, we have to retrieve device 
specific information particularly IRQ line and base address of the device memory.

```cpp
/*      
 * Read interrupt line and base address registers.
 * The architecture-dependent code can tweak these, of course.
 */     
static void pci_read_irq(struct pci_dev *dev)
{                                        
        unsigned char irq;
                                
        /* VFs are not allowed to use INTx, so skip the config reads */
        if (dev->is_virtfn) {
                dev->pin = 0;   
                dev->irq = 0;
                return;
        }               
        
        pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &irq);
        dev->pin = irq;
        if (irq) 
                pci_read_config_byte(dev, PCI_INTERRUPT_LINE, &irq);
        dev->irq = irq;
}       

int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{       
        if (pci_dev_is_disconnected(dev)) {
                PCI_SET_ERROR_RESPONSE(val);
                return PCIBIOS_DEVICE_NOT_FOUND;
        }
        return pci_bus_read_config_byte(dev->bus, dev->devfn, where, val);
}
EXPORT_SYMBOL(pci_read_config_byte);
```

Reading and writing device specific information should always be achieved 
through reading pcie config space. To access pcie config space, it should invoke
one of the kernel defined macro functions (e.g., pci_bus_read_config_byte). Note
that the pci_read_config_byte function invokes this macro to read pci config 
space to retrieve the IRQ information of the connected device. 

### Reading PCI BARs
Each PCIe device can have up to 6 different PAR. Therefore, this information 
should be retrieved to initialize a kernel data structure, pci_device. Note that
PCI BARs are also registers of the PCIe devices, which means that CPU, kernel 
can access the registers through the MMIO with the help of ECAM.


```cpp
static void pci_read_bases(struct pci_dev *dev, unsigned int howmany, int rom)
{               
        unsigned int pos, reg;
                        
        if (dev->non_compliant_bars)
                return;
        
        /* Per PCIe r4.0, sec 9.3.4.1.11, the VF BARs are all RO Zero */
        if (dev->is_virtfn)
                return; 
                
        for (pos = 0; pos < howmany; pos++) {
                struct resource *res = &dev->resource[pos];
                reg = PCI_BASE_ADDRESS_0 + (pos << 2);
                pos += __pci_read_base(dev, pci_bar_unknown, res, reg);
        }       
                
        if (rom) {
                struct resource *res = &dev->resource[PCI_ROM_RESOURCE];
                dev->rom_base_reg = rom;
                res->flags = IORESOURCE_MEM | IORESOURCE_PREFETCH |
                                IORESOURCE_READONLY | IORESOURCE_SIZEALIGN;
                __pci_read_base(dev, pci_bar_mem32, res, rom);
        }
}
```
Since PCIe specification defines that each device can have up to 6 BARs, this 
function iterates the loop until it reads all BARs through the PCIe config space.
Also, note that the retrieved BAR information, particularly the base and its
size are stored in the resource array of the device. **This fields will be used 
later by the device driver of the PCIe device**. Note that the BAR information 
is stored as the resource of pci_device structure. Therefore, instead of 
accessing config space from the device driver, it can easily access the BAR 
information and map the region to retrieve further device specific information.

### Add pci device to XX
The scanning of the device is almost done! We now have the information of the 
device including vendor id, device type, and its BAR. The last step of the 
scanning is to initialize XXX

```cpp
struct pci_dev *pci_scan_single_device(struct pci_bus *bus, int devfn)
{
        pci_device_add(dev, bus);
                
        return dev;
}       
```cpp

void pci_device_add(struct pci_dev *dev, struct pci_bus *bus)
{       
        int ret;
        
        pci_configure_device(dev);

        device_initialize(&dev->dev);
        dev->dev.release = pci_release_dev;
        
        set_dev_node(&dev->dev, pcibus_to_node(bus));
        dev->dev.dma_mask = &dev->dma_mask;
        dev->dev.dma_parms = &dev->dma_parms;
        dev->dev.coherent_dma_mask = 0xffffffffull;

        dma_set_max_seg_size(&dev->dev, 65536);
        dma_set_seg_boundary(&dev->dev, 0xffffffff);

        /* Fix up broken headers */
        pci_fixup_device(pci_fixup_header, dev);
        
        pci_reassigndev_resource_alignment(dev);

        dev->state_saved = false;

        pci_init_capabilities(dev);
        
        /*
         * Add the device to our list of discovered devices
         * and the bus list for fixup functions, etc.
         */     
        down_write(&pci_bus_sem);
        list_add_tail(&dev->bus_list, &bus->devices);
        up_write(&pci_bus_sem);
        
        ret = pcibios_device_add(dev);
        WARN_ON(ret < 0);
                
        /* Set up MSI IRQ domain */
        pci_set_msi_domain(dev);
        
        /* Notifier could use PCI capabilities */
        dev->match_driver = false;
        ret = device_add(&dev->dev);
        WARN_ON(ret < 0);
}      
```

XXX



```cpp
static void pci_configure_device(struct pci_dev *dev)
{               
        pci_configure_mps(dev);
        pci_configure_extended_tags(dev, NULL);
        pci_configure_relaxed_ordering(dev);
        pci_configure_ltr(dev);
        pci_configure_eetlp_prefix(dev);
        pci_configure_serr(dev);
        
        pci_acpi_program_hp_params(dev);
}
```

To utilize the device, the basic configuration of the device regarding PCI 
should be initialized. This initial configuration include, for example, 
pci_configure_mps which configures maximum payload size. I will not cover the 
details. 

```cpp
static void pci_init_capabilities(struct pci_dev *dev)
{       
        pci_ea_init(dev);               /* Enhanced Allocation */
        pci_msi_init(dev);              /* Disable MSI */
        pci_msix_init(dev);             /* Disable MSI-X */
                
        /* Buffers for saving PCIe and PCI-X capabilities */
        pci_allocate_cap_save_buffers(dev);
        
        pci_pm_init(dev);               /* Power Management */
        pci_vpd_init(dev);              /* Vital Product Data */
        pci_configure_ari(dev);         /* Alternative Routing-ID Forwarding */
        pci_iov_init(dev);              /* Single Root I/O Virtualization */
        pci_ats_init(dev);              /* Address Translation Services */
        pci_pri_init(dev);              /* Page Request Interface */
        pci_pasid_init(dev);            /* Process Address Space ID */
        pci_acs_init(dev);              /* Access Control Services */
        pci_ptm_init(dev);              /* Precision Time Measurement */
        pci_aer_init(dev);              /* Advanced Error Reporting */
        pci_dpc_init(dev);              /* Downstream Port Containment */
        pci_rcec_init(dev);             /* Root Complex Event Collector */

        pcie_report_downtraining(dev);
        pci_init_reset_methods(dev);
}
```

Also, based on the capabilities of the device, proper configuration should be 
finalized. Each function invoked by the pci_init_capabilities first checks if 
the device has a specific capability. If the device has that capability then 
kernel tries to configure the device properly to enable that capability. Lastly,
as registering the pci device to the kenrel subsystem (by device_add function),
it finishes adding the located pci device (pci_device_add). 

After locating all possible devices on a bus, it tries to expand the search 
through the bridge if one of the found device is pci bridge that connect the 
current bus to another. If anyone interested in this process, please take a look
at the rest part of the pci_scan_child_bus_extend, particularly
pci_scan_bridge_extend. Briefly speaking, this function will invoke the 
pci_scan_child_bus_extend again to extend the search to the new bus. 

## 
This was a very long journey about how to locate the pci device attached to the 
bus. Let's go back to pci_host_probe function and continue the initialization.
```cpp
void pci_bus_assign_resources(const struct pci_bus *bus)
{       
        __pci_bus_assign_resources(bus, NULL, NULL);
}

void __pci_bus_assign_resources(const struct pci_bus *bus,
                                struct list_head *realloc_head,
                                struct list_head *fail_head)
{       
        struct pci_bus *b;
        struct pci_dev *dev;
        
        pbus_assign_resources_sorted(bus, realloc_head, fail_head);
        
        list_for_each_entry(dev, &bus->devices, bus_list) {
                pdev_assign_fixed_resources(dev);
                
                b = dev->subordinate;
                if (!b) 
                        continue;
                
                __pci_bus_assign_resources(b, realloc_head, fail_head);
                
                switch (dev->hdr_type) {
                case PCI_HEADER_TYPE_BRIDGE:
                        if (!pci_is_enabled(dev))
                                pci_setup_bridge(b);
                        break;
                
                case PCI_HEADER_TYPE_CARDBUS:
                        pci_setup_cardbus(b);
                        break;
                
                default:
                        pci_info(dev, "not setting up bridge for bus %04x:%02x\n",
                                 pci_domain_nr(b), b->number);
                        break;
                }
        }
}


```


```cpp
static void pbus_assign_resources_sorted(const struct pci_bus *bus,
                                         struct list_head *realloc_head,
                                         struct list_head *fail_head)
{
        struct pci_dev *dev;
        LIST_HEAD(head);

        list_for_each_entry(dev, &bus->devices, bus_list)
                __dev_sort_resources(dev, &head);

        __assign_resources_sorted(&head, realloc_head, fail_head);
}
```


## PCIe device driver: How to utilize BAR information
Through the long procedure of the PCIe bridge and slot initialization, kernel 
maintains corresponding data structures to manage the devices. Let's see how the 
device driver of the PCIe devices attached to the bridge can access those info 
stored in the core kernel and utilize them to communicate with devices. 

```cpp
static int e1000_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{       
        struct net_device *netdev;
        struct e1000_adapter *adapter = NULL;
        struct e1000_hw *hw;
        
        static int cards_found;
        static int global_quad_port_a; /* global ksp3 port a indication */
        int i, err, pci_using_dac;
        u16 eeprom_data = 0;
        u16 tmp = 0;
        u16 eeprom_apme_mask = E1000_EEPROM_APME;
        int bars, need_ioport;
        bool disable_dev = false;
        
        /* do not allocate ioport bars when not needed */
        need_ioport = e1000_is_need_ioport(pdev);
        if (need_ioport) {
                bars = pci_select_bars(pdev, IORESOURCE_MEM | IORESOURCE_IO);
                err = pci_enable_device(pdev);
        } else {
                bars = pci_select_bars(pdev, IORESOURCE_MEM);
                err = pci_enable_device_mem(pdev);
        }
        if (err)
                return err;
        
        err = pci_request_selected_regions(pdev, bars, e1000_driver_name);
        if (err)
                goto err_pci_reg;
        
        pci_set_master(pdev);
        err = pci_save_state(pdev);
        if (err)
                goto err_alloc_etherdev;
        
        err = -ENOMEM;
        netdev = alloc_etherdev(sizeof(struct e1000_adapter));
        if (!netdev) 
                goto err_alloc_etherdev;
        
        SET_NETDEV_DEV(netdev, &pdev->dev);

	pci_set_drvdata(pdev, netdev);
        adapter = netdev_priv(netdev);
        adapter->netdev = netdev;
        adapter->pdev = pdev; 
        adapter->msg_enable = netif_msg_init(debug, DEFAULT_MSG_ENABLE);
        adapter->bars = bars;
        adapter->need_ioport = need_ioport;
        
        hw = &adapter->hw;
        hw->back = adapter;
        
        err = -EIO; 
        hw->hw_addr = pci_ioremap_bar(pdev, BAR_0);
        if (!hw->hw_addr)
                goto err_ioremap;
        
        if (adapter->need_ioport) { 
                for (i = BAR_1; i < PCI_STD_NUM_BARS; i++) {
                        if (pci_resource_len(pdev, i) == 0)
                                continue;
                        if (pci_resource_flags(pdev, i) & IORESOURCE_IO) {
                                hw->io_base = pci_resource_start(pdev, i);
                                break;
                        }
                }
        }
	......
```

Although the core PCI bridge locate the basic primitives of each pcie slot, for 
example the BARs, each device driver should configure its device by accessing 
the BARs because it is device specific. Also, it needs to configure additional 
PCIe configuration based on what driver wants to achieve with the device. For 
example, to enable the DMA on the device, it should send proper command through 
the PCI config space. 

```cpp
static void __pci_set_master(struct pci_dev *dev, bool enable)
{       
        u16 old_cmd, cmd;
        
        pci_read_config_word(dev, PCI_COMMAND, &old_cmd);
        if (enable)
                cmd = old_cmd | PCI_COMMAND_MASTER;
        else            
                cmd = old_cmd & ~PCI_COMMAND_MASTER;
        if (cmd != old_cmd) {
                pci_dbg(dev, "%s bus mastering\n",
                        enable ? "enabling" : "disabling");
                pci_write_config_word(dev, PCI_COMMAND, cmd);
        }
        dev->is_busmaster = enable;
}
```
Also, to access further information of the device, driver should map the BARs. 
Remember that the PCI core already retrieved the primitive information of the 
BARs (e.g., sizes and base addr). Therefore, each driver can easily access the 
BARs through the MMIO. pci_ioremap_bar is a good place to take a look how the 
BAR is mapped and become accessible from the CPU. 

```cpp
void __iomem *pci_ioremap_bar(struct pci_dev *pdev, int bar)
{       
        return __pci_ioremap_resource(pdev, bar, false);
}

static void __iomem *__pci_ioremap_resource(struct pci_dev *pdev, int bar,
                                            bool write_combine)
{       
        struct resource *res = &pdev->resource[bar];
        resource_size_t start = res->start;
        resource_size_t size = resource_size(res);
        
        /*
         * Make sure the BAR is actually a memory resource, not an IO resource
         */
        if (res->flags & IORESOURCE_UNSET || !(res->flags & IORESOURCE_MEM)) {
                pci_err(pdev, "can't ioremap BAR %d: %pR\n", bar, res);
                return NULL;
        }
        
        if (write_combine)
                return ioremap_wc(start, size);
        
        return ioremap(start, size);
}
```

It is very easy to make the BAR accessible from the CPU because we already have 
the all required information of the BAR such as size and base address. It just 
retrieves the information from the resource of the pci_device and invoke ioremap
function to get CPU accessible address. 

