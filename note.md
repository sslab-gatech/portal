PORTAL: Secure Channel Connecting Realm and Accelerators

Goal
G1: The portal should not prevent NW host kernel from utilizing peripherals 
S1: Explain what should be checked, like what configuration of the STE should be checked 
not to make it bypass the SMMU translation. The translation table should be protected..
mimic how the TDX manage s-ept. everytime it needs to modify s-ept on behalf of HV, it 
invoked the TDCALL. Similar to this, we can make the host kernel to invoke s-ept like RMI 
call to make the RMM to change the SMMU page tables. Also make it to be protected 
from nw kernel using unsynch gpt and prevent the devices from accessing these 
regions by checking translation table. The every physical address fed to modify
s-smmu-pages should be memorized and checked (bloom filters)
Cause the translation tables and STE entries will be read by SMMU itself and 
SMMU can trigger the GPC fault cause its own access should be checked by the 
GPT too. so for SMMU gpts it should be set as NW pages. 


We can simply set up the GPT in SMMU and NW gpt for processors. 


It actually doesn't need to check the mid level entries, but only the last entries should be 
checked. 

Also we can make host kernel to mirror the stream tables and translation tables so that 
it can minimize the performance to lookup the tables (reducing lots of overhead cause 
it needs to access the STE and other tables frequently to set up tables..) 


In virtualized scenarios, Arm expects a hypervisor to:
• Convert guest STEs into physical SMMU STEs, controlling permissions and features as required.
Note: The physical StreamIDs might be hidden from the guest, which would be given virtual StreamIDs, so a
mapping between virtual and physical StreamIDs must be maintained.
• Read and interpret commands from the guest Command queue. These might result in commands being issued
to the SMMU or invalidation of internal shadowed data structures.
• Consume new entries from the PRI and Event queues, mapping from host StreamIDs to guest, and deliver
appropriate entries to guest Event and PRI queues.



Commands are submitted to the SMMU by writing them to the Command queue then, after ensuring their visibility
to the SMMU, updating the SMMU_(S_)CMDQ_PROD.WR index which notifies the SMMU that there are
commands to process.



platform_get_irq_byname_optional



NoStreamID
GICR_PENDBASER, Redistributor LPI Pending Table Base Address Register
GICR_PROPBASER, Redistributor Properties Base Address Register
GICR_VPENDBASER, Virtual Redistributor LPI Pending Table Base Address Registero
GICR_VPROPBASER, Virtual Redistributor Properties Base Address Register
GITS_BASER<n>, ITS Translation Table Descriptors, n = 0 - 7
GITS_CBASER, ITS Command Queue Descriptors


## Initialize stream table & queues
```cpp
static int arm_smmu_init_structures(struct arm_smmu_device *smmu)
{
        int ret;

        mutex_init(&smmu->streams_mutex);
        smmu->streams = RB_ROOT;

        ret = arm_smmu_init_queues(smmu);
        if (ret)
                return ret;

        return arm_smmu_init_strtab(smmu);
}
```

Below code initialize the strtab (for 2levels). arm_smmu_init_strtab_2lvl func 
allocates the memory for the 2 level stream table and initialize the memory to 
be used as stream table. Also, smmu->strtab_cfg.strtab_base is used to convey 
the physical address of the initialized table for registation. Note that it does
not assign the initialized stream table to the base address of the SMMU yet. 

```cpp
/* basically initialize memory and set-up the registers properly */
static int arm_smmu_init_strtab(struct arm_smmu_device *smmu)
{
        u64 reg;
        int ret;

        if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB)
                ret = arm_smmu_init_strtab_2lvl(smmu);
        else
                ret = arm_smmu_init_strtab_linear(smmu);

        if (ret)
                return ret;

        /* Set the strtab base address */
        reg  = smmu->strtab_cfg.strtab_dma & STRTAB_BASE_ADDR_MASK;
        reg |= STRTAB_BASE_RA;
        // strtab_base contains value that should be written to the register later
        smmu->strtab_cfg.strtab_base = reg;

        /* Allocate the first VMID for stage-2 bypass STEs */
        set_bit(0, smmu->vmid_map);
        return 0;
}

static int arm_smmu_init_strtab_2lvl(struct arm_smmu_device *smmu)
{
        void *strtab;
        u64 reg;
        u32 size, l1size;
        struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;

        /* Calculate the L1 size, capped to the SIDSIZE. */
        size = STRTAB_L1_SZ_SHIFT - (ilog2(STRTAB_L1_DESC_DWORDS) + 3);
        size = min(size, smmu->sid_bits - STRTAB_SPLIT);
        cfg->num_l1_ents = 1 << size;

        size += STRTAB_SPLIT;
        if (size < smmu->sid_bits)
                dev_warn(smmu->dev,
                         "2-level strtab only covers %u/%u bits of SID\n",
                         size, smmu->sid_bits);

        l1size = cfg->num_l1_ents * (STRTAB_L1_DESC_DWORDS << 3);
        strtab = dmam_alloc_coherent(smmu->dev, l1size, &cfg->strtab_dma,
                                     GFP_KERNEL);
        printk("Stream table init for 2-lvl\n");
        printk("l1table entries :%d size:0x%x\n", cfg->num_l1_ents, l1size);
        printk("l1table virt:%llx phys:%llx\n", (uint64_t)strtab, (uint64_t)cfg->strtab_dma );
        if (!strtab) {
                dev_err(smmu->dev,
                        "failed to allocate l1 stream table (%u bytes)\n",
                        l1size);
                return -ENOMEM;
        }
        cfg->strtab = strtab;

        /* Configure strtab_base_cfg for 2 levels */
        reg  = FIELD_PREP(STRTAB_BASE_CFG_FMT, STRTAB_BASE_CFG_FMT_2LVL);
        reg |= FIELD_PREP(STRTAB_BASE_CFG_LOG2SIZE, size);
        reg |= FIELD_PREP(STRTAB_BASE_CFG_SPLIT, STRTAB_SPLIT);
        cfg->strtab_base_cfg = reg;

        return arm_smmu_init_l1_strtab(smmu);
}

It assigns strtab which is the dmam alloc memory region to the strtab field of 
the smmu->strtab_cfg. 

### Initialize L1 stream table descriptor (for 2 lvl)
static int arm_smmu_init_l1_strtab(struct arm_smmu_device *smmu)
{
        unsigned int i;
        struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
        void *strtab = smmu->strtab_cfg.strtab;

        cfg->l1_desc = devm_kcalloc(smmu->dev, cfg->num_l1_ents,
                                    sizeof(*cfg->l1_desc), GFP_KERNEL);
        printk("size of l1 descriptor:0x%lx, entire size:0x%lx\n",
                        sizeof(*cfg->l1_desc), sizeof(*cfg->l1_desc) * cfg->num_l1_ents);
        printk("l1_desc base virt:%llx\n", (uint64_t)cfg->l1_desc);
        if (!cfg->l1_desc)
                return -ENOMEM;

        for (i = 0; i < cfg->num_l1_ents; ++i) {
                arm_smmu_write_strtab_l1_desc(strtab, &cfg->l1_desc[i]);
                strtab += STRTAB_L1_DESC_DWORDS << 3;
        }

        return 0;
}

As strtab conists of multiple strtab l1 descriptors, it iterates all descriptors
managed by the root stratb and initialize them. cfg->l1_desc points to the start
address of the l1_desc arrays that should be copied to the stream table. 

/* Stream table manipulation functions */
static void
arm_smmu_write_strtab_l1_desc(__le64 *dst, struct arm_smmu_strtab_l1_desc *desc)
{
        u64 val = 0;

        val |= FIELD_PREP(STRTAB_L1_DESC_SPAN, desc->span);
        val |= desc->l2ptr_dma & STRTAB_L1_DESC_L2PTR_MASK;

        /* See comment in arm_smmu_write_ctx_desc() */
        WRITE_ONCE(*dst, cpu_to_le64(val));
}
```
Because this is part of the initialization, it will not set any l1 descriptor 
yet. However, when the new device is attached to the system, the l1 descriptor
presenting the new device is needed. To this end, the above function will be 
used again!


## Probing new device
```cpp
/* SMMU private data for each master */
struct arm_smmu_master {
        struct arm_smmu_device          *smmu;
        struct device                   *dev;
        struct arm_smmu_domain          *domain;
        struct list_head                domain_head;
        struct arm_smmu_stream          *streams;
        unsigned int                    num_streams;
        bool                            ats_enabled;
        bool                            stall_enabled;
        bool                            sva_enabled;
        bool                            iopf_enabled;
        struct list_head                bonds;
        unsigned int                    ssid_bits;
};      

/**     
 * struct iommu_fwspec - per-device IOMMU instance data
 * @ops: ops for this device's IOMMU
 * @iommu_fwnode: firmware handle for this device's IOMMU
 * @flags: IOMMU_FWSPEC_* flags
 * @num_ids: number of associated device IDs
 * @ids: IDs which this device may present to the IOMMU
 *
 * Note that the IDs (and any other information, really) stored in this structure should be
 * considered private to the IOMMU device driver and are not to be used directly by IOMMU
 * consumers.
 */     
struct iommu_fwspec {
        const struct iommu_ops  *ops;
        struct fwnode_handle    *iommu_fwnode;
        u32                     flags;
        unsigned int            num_ids;
        u32                     ids[];
};     
```

```cpp
static struct iommu_device *arm_smmu_probe_device(struct device *dev)
{
        int ret;
        struct arm_smmu_device *smmu;
        struct arm_smmu_master *master;
        struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

        if (!fwspec || fwspec->ops != &arm_smmu_ops)
                return ERR_PTR(-ENODEV);

        if (WARN_ON_ONCE(dev_iommu_priv_get(dev)))
                return ERR_PTR(-EBUSY);

        smmu = arm_smmu_get_by_fwnode(fwspec->iommu_fwnode);
        if (!smmu)
                return ERR_PTR(-ENODEV);

        master = kzalloc(sizeof(*master), GFP_KERNEL);
        if (!master)
                return ERR_PTR(-ENOMEM);

        master->dev = dev;
        master->smmu = smmu;
        INIT_LIST_HEAD(&master->bonds);
        dev_iommu_priv_set(dev, master);

        ret = arm_smmu_insert_master(smmu, master);
        if (ret)
                goto err_free_master;

        device_property_read_u32(dev, "pasid-num-bits", &master->ssid_bits);
        master->ssid_bits = min(smmu->ssid_bits, master->ssid_bits);

        /*
         * Note that PASID must be enabled before, and disabled after ATS:
         * PCI Express Base 4.0r1.0 - 10.5.1.3 ATS Control Register
         *
         *   Behavior is undefined if this bit is Set and the value of the PASID
         *   Enable, Execute Requested Enable, or Privileged Mode Requested bits
         *   are changed.
         */
        arm_smmu_enable_pasid(master);

        if (!(smmu->features & ARM_SMMU_FEAT_2_LVL_CDTAB))
                master->ssid_bits = min_t(u8, master->ssid_bits,
                                          CTXDESC_LINEAR_CDMAX);

        if ((smmu->features & ARM_SMMU_FEAT_STALLS &&
             device_property_read_bool(dev, "dma-can-stall")) ||
            smmu->features & ARM_SMMU_FEAT_STALL_FORCE)
                master->stall_enabled = true;

        return &smmu->iommu;

err_free_master:
        kfree(master);
        dev_iommu_priv_set(dev, NULL);
        return ERR_PTR(ret);
}
```

```cpp
static inline void dev_iommu_priv_set(struct device *dev, void *priv)
{
        dev->iommu->priv = priv;
}     
```

master structure associated with current device is set as priv (private data) of
the newly discovered device structure. 


### Set up L2 stream table for new device
```cpp
static int arm_smmu_insert_master(struct arm_smmu_device *smmu,
                                  struct arm_smmu_master *master)
{
        int i;
        int ret = 0;
        struct arm_smmu_stream *new_stream, *cur_stream;
        struct rb_node **new_node, *parent_node = NULL;
        struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(master->dev);

        master->streams = kcalloc(fwspec->num_ids, sizeof(*master->streams),
                                  GFP_KERNEL);
        if (!master->streams)
                return -ENOMEM;
        master->num_streams = fwspec->num_ids;

        mutex_lock(&smmu->streams_mutex);
        for (i = 0; i < fwspec->num_ids; i++) {
                u32 sid = fwspec->ids[i];

                new_stream = &master->streams[i];
                new_stream->id = sid;
                new_stream->master = master;

                ret = arm_smmu_init_sid_strtab(smmu, sid);
                if (ret)
                        break;

                /* Insert into SID tree */
                new_node = &(smmu->streams.rb_node);
                while (*new_node) {
                        cur_stream = rb_entry(*new_node, struct arm_smmu_stream,
                                              node);
                        parent_node = *new_node;
                        if (cur_stream->id > new_stream->id) {
                                new_node = &((*new_node)->rb_left);
                        } else if (cur_stream->id < new_stream->id) {
                                new_node = &((*new_node)->rb_right);
                        } else {
                                dev_warn(master->dev,
                                         "stream %u already in tree\n",
                                         cur_stream->id);
                                ret = -EINVAL;
                                break;
                        }
                }
                if (ret)
                        break;

                rb_link_node(&new_stream->node, parent_node, new_node);
                rb_insert_color(&new_stream->node, &smmu->streams);
        }

        if (ret) {
                for (i--; i >= 0; i--)
                        rb_erase(&master->streams[i].node, &smmu->streams);
                kfree(master->streams);
        }
        mutex_unlock(&smmu->streams_mutex);

        return ret;
}
```

SID of the device is extracted from the fw_spec. How to ensure the fw_spec is 
intact? 

```cpp
static int arm_smmu_init_sid_strtab(struct arm_smmu_device *smmu, u32 sid)
{
        /* Check the SIDs are in range of the SMMU and our stream table */
        if (!arm_smmu_sid_in_range(smmu, sid))
                return -ERANGE;

        /* Ensure l2 strtab is initialised */
        if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB)
                return arm_smmu_init_l2_strtab(smmu, sid);

        return 0;
}


static int arm_smmu_init_l2_strtab(struct arm_smmu_device *smmu, u32 sid)
{
        size_t size;
        void *strtab;
        struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
        struct arm_smmu_strtab_l1_desc *desc = &cfg->l1_desc[sid >> STRTAB_SPLIT];

        if (desc->l2ptr)
                return 0;

        size = 1 << (STRTAB_SPLIT + ilog2(STRTAB_STE_DWORDS) + 3);
        strtab = &cfg->strtab[(sid >> STRTAB_SPLIT) * STRTAB_L1_DESC_DWORDS];

        desc->span = STRTAB_SPLIT + 1;
        desc->l2ptr = dmam_alloc_coherent(smmu->dev, size, &desc->l2ptr_dma,
                                          GFP_KERNEL);
        if (!desc->l2ptr) {
                dev_err(smmu->dev,
                        "failed to allocate l2 stream table for SID %u\n",
                        sid);
                return -ENOMEM;
        }

        arm_smmu_init_bypass_stes(desc->l2ptr, 1 << STRTAB_SPLIT, false);
        arm_smmu_write_strtab_l1_desc(strtab, desc);
        return 0;
}
```

Compared to the previous initialization code zero-out the l1 descriptor through 
the arm_smmu_write_strtab_l1_desc function, it sets the descriptor properly to 
associate the descriptor to the current device. The mapping from the device to 
the descriptor is determined based on the sid. Note that the l1 descriptor entry
associated with the device is selected based on the sid index. Also, it assigns 
memory for the l2ptr because l1 descriptor is used to point to l2 descriptor.

## Attach new device to iommu/smmu 
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

        /*
         * Checking that SVA is disabled ensures that this device isn't bound to
         * any mm, and can be safely detached from its old domain. Bonds cannot
         * be removed concurrently since we're holding the group mutex.
         */
        if (arm_smmu_master_sva_enabled(master)) {
                dev_err(dev, "cannot attach - SVA enabled\n");
                return -EBUSY;
        }

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

The goal of attach function are 
- Generate page table for the context
- Spawn Context Descriptor (CD)
- Set up the stream table entry. 
The first and second will be handled by the **arm_smmu_domain_finalise** and the
last will be done by **arm_smmu_install_ste_for_dev**.

```cpp
static int arm_smmu_domain_finalise(struct iommu_domain *domain,
                                    struct arm_smmu_master *master)
{       
        int ret; 
        unsigned long ias, oas;
        enum io_pgtable_fmt fmt;
        struct io_pgtable_cfg pgtbl_cfg;
        struct io_pgtable_ops *pgtbl_ops;
        int (*finalise_stage_fn)(struct arm_smmu_domain *,
                                 struct arm_smmu_master *,
                                 struct io_pgtable_cfg *);
        struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
        struct arm_smmu_device *smmu = smmu_domain->smmu;
        
        if (domain->type == IOMMU_DOMAIN_IDENTITY) {
                smmu_domain->stage = ARM_SMMU_DOMAIN_BYPASS;
                return 0;
        }
        
        /* Restrict the stage to what we can actually support */
        if (!(smmu->features & ARM_SMMU_FEAT_TRANS_S1))
                smmu_domain->stage = ARM_SMMU_DOMAIN_S2;
        if (!(smmu->features & ARM_SMMU_FEAT_TRANS_S2))
                smmu_domain->stage = ARM_SMMU_DOMAIN_S1;
        
        switch (smmu_domain->stage) {
        case ARM_SMMU_DOMAIN_S1:
                ias = (smmu->features & ARM_SMMU_FEAT_VAX) ? 52 : 48;
                ias = min_t(unsigned long, ias, VA_BITS);
                oas = smmu->ias;
                fmt = ARM_64_LPAE_S1;
                finalise_stage_fn = arm_smmu_domain_finalise_s1;
                break;
        case ARM_SMMU_DOMAIN_NESTED:
        case ARM_SMMU_DOMAIN_S2:
                ias = smmu->ias;
                oas = smmu->oas;
                fmt = ARM_64_LPAE_S2;
                finalise_stage_fn = arm_smmu_domain_finalise_s2;
                break;
        default:
                return -EINVAL;
        }
        
        pgtbl_cfg = (struct io_pgtable_cfg) {
                .pgsize_bitmap  = smmu->pgsize_bitmap,
                .ias            = ias,
                .oas            = oas,
                .coherent_walk  = smmu->features & ARM_SMMU_FEAT_COHERENCY,
                .tlb            = &arm_smmu_flush_ops,
                .iommu_dev      = smmu->dev,
        };
        
        pgtbl_ops = alloc_io_pgtable_ops(fmt, &pgtbl_cfg, smmu_domain);
        if (!pgtbl_ops)
                return -ENOMEM;
        
        domain->pgsize_bitmap = pgtbl_cfg.pgsize_bitmap;
        domain->geometry.aperture_end = (1UL << pgtbl_cfg.ias) - 1;
        domain->geometry.force_aperture = true;
        
        ret = finalise_stage_fn(smmu_domain, master, &pgtbl_cfg);
        if (ret < 0) {
                free_io_pgtable_ops(pgtbl_ops);
                return ret;
        }
        
        smmu_domain->pgtbl_ops = pgtbl_ops;
        return 0;
}
```

The foremost goal of the smmu_domain_finalze function is generating Context 
Descriptor (CD) associated with current device and sub-stream ID (SSID). Through
the CD, the page table associated with the device attached to the SMMU can be 
configured. Also, SMMU walks that page table to translate the addresses. 
Therefore, this function will generate page table used by the SMMU and will set 
the root address to the CD. 

### Initialize SMMU page table
```cpp
static const struct io_pgtable_init_fns *
io_pgtable_init_table[IO_PGTABLE_NUM_FMTS] = {
#ifdef CONFIG_IOMMU_IO_PGTABLE_LPAE
        [ARM_32_LPAE_S1] = &io_pgtable_arm_32_lpae_s1_init_fns,
        [ARM_32_LPAE_S2] = &io_pgtable_arm_32_lpae_s2_init_fns,
        [ARM_64_LPAE_S1] = &io_pgtable_arm_64_lpae_s1_init_fns,
        [ARM_64_LPAE_S2] = &io_pgtable_arm_64_lpae_s2_init_fns,
        [ARM_MALI_LPAE] = &io_pgtable_arm_mali_lpae_init_fns,
#endif          
#ifdef CONFIG_IOMMU_IO_PGTABLE_DART
        [APPLE_DART] = &io_pgtable_apple_dart_init_fns,
        [APPLE_DART2] = &io_pgtable_apple_dart_init_fns,
#endif  
#ifdef CONFIG_IOMMU_IO_PGTABLE_ARMV7S
        [ARM_V7S] = &io_pgtable_arm_v7s_init_fns,
#endif          
#ifdef CONFIG_AMD_IOMMU
        [AMD_IOMMU_V1] = &io_pgtable_amd_iommu_v1_init_fns,
        [AMD_IOMMU_V2] = &io_pgtable_amd_iommu_v2_init_fns,
#endif          
};      

struct io_pgtable_init_fns io_pgtable_arm_64_lpae_s1_init_fns = {
        .alloc  = arm_64_lpae_alloc_pgtable_s1,
        .free   = arm_lpae_free_pgtable,    
};                                          

struct io_pgtable_ops *alloc_io_pgtable_ops(enum io_pgtable_fmt fmt,
                                            struct io_pgtable_cfg *cfg,
                                            void *cookie)
{               
        struct io_pgtable *iop;
        const struct io_pgtable_init_fns *fns;
                
        if (fmt >= IO_PGTABLE_NUM_FMTS)
                return NULL;
        
        fns = io_pgtable_init_table[fmt];
        if (!fns)
                return NULL;    
                
        iop = fns->alloc(cfg, cookie);
        if (!iop)
                return NULL;
        
        iop->fmt        = fmt;
        iop->cookie     = cookie;
        iop->cfg        = *cfg;
        
        return &iop->ops;
}       
EXPORT_SYMBOL_GPL(alloc_io_pgtable_ops);
```

Based on the fmt, the io_pgtable_init_fns is determined. It is array of 
functions required to initialize the page table associated with the device. 
As we go through the stage 1 initialization, it will retrieve the
io_pgtable_arm_64_lpae_s1_init_fns and invoke arm_64_lpae_alloc_pgtable_s1 
for alloc function. 


```cpp
static struct io_pgtable *
arm_64_lpae_alloc_pgtable_s1(struct io_pgtable_cfg *cfg, void *cookie)
{
        u64 reg;
        struct arm_lpae_io_pgtable *data;
        typeof(&cfg->arm_lpae_s1_cfg.tcr) tcr = &cfg->arm_lpae_s1_cfg.tcr;
        bool tg1;

        if (cfg->quirks & ~(IO_PGTABLE_QUIRK_ARM_NS |
                            IO_PGTABLE_QUIRK_ARM_TTBR1 |
                            IO_PGTABLE_QUIRK_ARM_OUTER_WBWA))
                return NULL;

        data = arm_lpae_alloc_pgtable(cfg);
        if (!data)
                return NULL;

        /* TCR */
        if (cfg->coherent_walk) {
                tcr->sh = ARM_LPAE_TCR_SH_IS;
                tcr->irgn = ARM_LPAE_TCR_RGN_WBWA;
                tcr->orgn = ARM_LPAE_TCR_RGN_WBWA;
                if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_OUTER_WBWA)
                        goto out_free_data;
        } else {
                tcr->sh = ARM_LPAE_TCR_SH_OS;
                tcr->irgn = ARM_LPAE_TCR_RGN_NC;
                if (!(cfg->quirks & IO_PGTABLE_QUIRK_ARM_OUTER_WBWA))
                        tcr->orgn = ARM_LPAE_TCR_RGN_NC;
                else
                        tcr->orgn = ARM_LPAE_TCR_RGN_WBWA;
        }

        tg1 = cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1;
        switch (ARM_LPAE_GRANULE(data)) {
        case SZ_4K:
                tcr->tg = tg1 ? ARM_LPAE_TCR_TG1_4K : ARM_LPAE_TCR_TG0_4K;
                break;
        case SZ_16K:
                tcr->tg = tg1 ? ARM_LPAE_TCR_TG1_16K : ARM_LPAE_TCR_TG0_16K;
                break;
        case SZ_64K:
                tcr->tg = tg1 ? ARM_LPAE_TCR_TG1_64K : ARM_LPAE_TCR_TG0_64K;
                break;
        }

        switch (cfg->oas) {
        case 32:
                tcr->ips = ARM_LPAE_TCR_PS_32_BIT;
                break;
        case 36:
                tcr->ips = ARM_LPAE_TCR_PS_36_BIT;
                break;
        case 40:
                tcr->ips = ARM_LPAE_TCR_PS_40_BIT;
                break;
        case 42:
                tcr->ips = ARM_LPAE_TCR_PS_42_BIT;
                break;
        case 44:
                tcr->ips = ARM_LPAE_TCR_PS_44_BIT;
                break;
        case 48:
                tcr->ips = ARM_LPAE_TCR_PS_48_BIT;
                break;
        case 52:
                tcr->ips = ARM_LPAE_TCR_PS_52_BIT;
                break;
        default:
                goto out_free_data;
        }

        tcr->tsz = 64ULL - cfg->ias;

        /* MAIRs */
        reg = (ARM_LPAE_MAIR_ATTR_NC
               << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_NC)) |
              (ARM_LPAE_MAIR_ATTR_WBRWA
               << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_CACHE)) |
              (ARM_LPAE_MAIR_ATTR_DEVICE
               << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_DEV)) |
              (ARM_LPAE_MAIR_ATTR_INC_OWBRWA
               << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_INC_OCACHE));

        cfg->arm_lpae_s1_cfg.mair = reg;

        /* Looking good; allocate a pgd */
        data->pgd = __arm_lpae_alloc_pages(ARM_LPAE_PGD_SIZE(data),
                                           GFP_KERNEL, cfg);
        if (!data->pgd)
                goto out_free_data;

        /* Ensure the empty pgd is visible before any actual TTBR write */
        wmb();

        /* TTBR */
        cfg->arm_lpae_s1_cfg.ttbr = virt_to_phys(data->pgd);
        return &data->iop;

out_free_data:
        kfree(data);
        return NULL;
}
```

This function fills up the io_pgtable_cfg configuration structure before 
generating the CD entry for the device. One of the most important filed of this
structure is ttbr accessible through the arm_lpae_s1_cfg. This will be used to 
fill out CD to set up page table for the device. 

```cpp
static struct arm_lpae_io_pgtable *
arm_lpae_alloc_pgtable(struct io_pgtable_cfg *cfg)
{
        struct arm_lpae_io_pgtable *data;
        int levels, va_bits, pg_shift;

        arm_lpae_restrict_pgsizes(cfg);

        if (!(cfg->pgsize_bitmap & (SZ_4K | SZ_16K | SZ_64K)))
                return NULL;

        if (cfg->ias > ARM_LPAE_MAX_ADDR_BITS)
                return NULL;

        if (cfg->oas > ARM_LPAE_MAX_ADDR_BITS)
                return NULL;

        data = kmalloc(sizeof(*data), GFP_KERNEL);
        if (!data)
                return NULL;

        pg_shift = __ffs(cfg->pgsize_bitmap);
        data->bits_per_level = pg_shift - ilog2(sizeof(arm_lpae_iopte));

        va_bits = cfg->ias - pg_shift;
        levels = DIV_ROUND_UP(va_bits, data->bits_per_level);
        data->start_level = ARM_LPAE_MAX_LEVELS - levels;

        /* Calculate the actual size of our pgd (without concatenation) */
        data->pgd_bits = va_bits - (data->bits_per_level * (levels - 1));

        data->iop.ops = (struct io_pgtable_ops) {
                .map_pages      = arm_lpae_map_pages,
                .unmap_pages    = arm_lpae_unmap_pages,
                .iova_to_phys   = arm_lpae_iova_to_phys,
        };

        return data;
}
```

Also, one important function is assigning the operations for managing the SMMU
page table is also done as part of arm_64_lpae_alloc_pgtable_s1 function. As 
shown in the above code, data->iop.ops filed points to several functions that 
will be invoked by the kernel IOMMU subsystem for managing the pages as a 
result of dma related kernel functions. 



### Set-up context descriptor 

```cpp
static int arm_smmu_domain_finalise_s1(struct arm_smmu_domain *smmu_domain,
                                       struct arm_smmu_master *master,
                                       struct io_pgtable_cfg *pgtbl_cfg)
{
        int ret;
        u32 asid;
        struct arm_smmu_device *smmu = smmu_domain->smmu;
        struct arm_smmu_s1_cfg *cfg = &smmu_domain->s1_cfg;
        typeof(&pgtbl_cfg->arm_lpae_s1_cfg.tcr) tcr = &pgtbl_cfg->arm_lpae_s1_cfg.tcr;
                       
        refcount_set(&cfg->cd.refs, 1);
                
        /* Prevent SVA from modifying the ASID until it is written to the CD */
        mutex_lock(&arm_smmu_asid_lock);
        ret = xa_alloc(&arm_smmu_asid_xa, &asid, &cfg->cd,
                       XA_LIMIT(1, (1 << smmu->asid_bits) - 1), GFP_KERNEL);
        if (ret)
                goto out_unlock;
        
        cfg->s1cdmax = master->ssid_bits;

        smmu_domain->stall_enabled = master->stall_enabled;
        
        ret = arm_smmu_alloc_cd_tables(smmu_domain);
        if (ret)          
                goto out_free_asid;
                          
        cfg->cd.asid    = (u16)asid;
        cfg->cd.ttbr    = pgtbl_cfg->arm_lpae_s1_cfg.ttbr; 
        cfg->cd.tcr     = FIELD_PREP(CTXDESC_CD_0_TCR_T0SZ, tcr->tsz) |
                          FIELD_PREP(CTXDESC_CD_0_TCR_TG0, tcr->tg) |
                          FIELD_PREP(CTXDESC_CD_0_TCR_IRGN0, tcr->irgn) |
                          FIELD_PREP(CTXDESC_CD_0_TCR_ORGN0, tcr->orgn) |
                          FIELD_PREP(CTXDESC_CD_0_TCR_SH0, tcr->sh) |
                          FIELD_PREP(CTXDESC_CD_0_TCR_IPS, tcr->ips) |
                          CTXDESC_CD_0_TCR_EPD1 | CTXDESC_CD_0_AA64;
        cfg->cd.mair    = pgtbl_cfg->arm_lpae_s1_cfg.mair;
        
        /* 
         * Note that this will end up calling arm_smmu_sync_cd() before
         * the master has been added to the devices list for this domain.
         * This isn't an issue because the STE hasn't been installed yet.
         */
        ret = arm_smmu_write_ctx_desc(smmu_domain, 0, &cfg->cd);
        if (ret)
                goto out_free_cd_tables;

        mutex_unlock(&arm_smmu_asid_lock);
        return 0;
        
out_free_cd_tables:
        arm_smmu_free_cd_tables(smmu_domain);
out_free_asid:
        arm_smmu_free_asid(&cfg->cd);
out_unlock:
        mutex_unlock(&arm_smmu_asid_lock);
        return ret;
}
```

This function consists of two parts: generate CD tables and write into the CD
associated with the current device's SSID. 


```cpp
static int arm_smmu_alloc_cd_tables(struct arm_smmu_domain *smmu_domain)
{
        int ret;
        size_t l1size;
        size_t max_contexts;
        struct arm_smmu_device *smmu = smmu_domain->smmu;
        struct arm_smmu_s1_cfg *cfg = &smmu_domain->s1_cfg;
        struct arm_smmu_ctx_desc_cfg *cdcfg = &cfg->cdcfg;

        max_contexts = 1 << cfg->s1cdmax;

        if (!(smmu->features & ARM_SMMU_FEAT_2_LVL_CDTAB) ||
            max_contexts <= CTXDESC_L2_ENTRIES) {
                cfg->s1fmt = STRTAB_STE_0_S1FMT_LINEAR;
                cdcfg->num_l1_ents = max_contexts;

                l1size = max_contexts * (CTXDESC_CD_DWORDS << 3);
        } else {
                cfg->s1fmt = STRTAB_STE_0_S1FMT_64K_L2;
                cdcfg->num_l1_ents = DIV_ROUND_UP(max_contexts,
                                                  CTXDESC_L2_ENTRIES);

                cdcfg->l1_desc = devm_kcalloc(smmu->dev, cdcfg->num_l1_ents,
                                              sizeof(*cdcfg->l1_desc),
                                              GFP_KERNEL);
                if (!cdcfg->l1_desc)
                        return -ENOMEM;

                l1size = cdcfg->num_l1_ents * (CTXDESC_L1_DESC_DWORDS << 3);
        }

        cdcfg->cdtab = dmam_alloc_coherent(smmu->dev, l1size, &cdcfg->cdtab_dma,
                                           GFP_KERNEL);
        if (!cdcfg->cdtab) {
                dev_warn(smmu->dev, "failed to allocate context descriptor\n");
                ret = -ENOMEM;
                goto err_free_l1;
        }

        return 0;

err_free_l1:
        if (cdcfg->l1_desc) {
                devm_kfree(smmu->dev, cdcfg->l1_desc);
                cdcfg->l1_desc = NULL;
        }
        return ret;
}
```

cdtab of the cdcfg is the array of CD that are associated with the device. Note 
that CD can be presented in two level as similar to the Stream table. If the 
SMMU supports 2-level CD, the cdtab refer to the array of the L1 CD. We will see
how the following code will initialize L1 and L2 tables. Also, above function 
allocates memory array for l1_desc. This array is not the CD array but used as 
to save information about the CD. 

### write to context descriptor
```cpp
int arm_smmu_write_ctx_desc(struct arm_smmu_domain *smmu_domain, int ssid,
                            struct arm_smmu_ctx_desc *cd)
{
        /*
         * This function handles the following cases:
         *
         * (1) Install primary CD, for normal DMA traffic (SSID = 0).
         * (2) Install a secondary CD, for SID+SSID traffic.
         * (3) Update ASID of a CD. Atomically write the first 64 bits of the
         *     CD, then invalidate the old entry and mappings.
         * (4) Quiesce the context without clearing the valid bit. Disable
         *     translation, and ignore any translation fault.
         * (5) Remove a secondary CD.
         */
        u64 val;
        bool cd_live;
        __le64 *cdptr;

        if (WARN_ON(ssid >= (1 << smmu_domain->s1_cfg.s1cdmax)))
                return -E2BIG;

        cdptr = arm_smmu_get_cd_ptr(smmu_domain, ssid);
        if (!cdptr)
                return -ENOMEM;

        val = le64_to_cpu(cdptr[0]);
        cd_live = !!(val & CTXDESC_CD_0_V);

        if (!cd) { /* (5) */
                val = 0;
        } else if (cd == &quiet_cd) { /* (4) */
                val |= CTXDESC_CD_0_TCR_EPD0;
        } else if (cd_live) { /* (3) */
                val &= ~CTXDESC_CD_0_ASID;
                val |= FIELD_PREP(CTXDESC_CD_0_ASID, cd->asid);
                /*
                 * Until CD+TLB invalidation, both ASIDs may be used for tagging
                 * this substream's traffic
                 */
        } else { /* (1) and (2) */
                cdptr[1] = cpu_to_le64(cd->ttbr & CTXDESC_CD_1_TTB0_MASK);
                cdptr[2] = 0;
                cdptr[3] = cpu_to_le64(cd->mair);

                /*
                 * STE is live, and the SMMU might read dwords of this CD in any
                 * order. Ensure that it observes valid values before reading
                 * V=1.
                 */
                arm_smmu_sync_cd(smmu_domain, ssid, true);

                val = cd->tcr |
#ifdef __BIG_ENDIAN
                        CTXDESC_CD_0_ENDI |
#endif
                        CTXDESC_CD_0_R | CTXDESC_CD_0_A |
                        (cd->mm ? 0 : CTXDESC_CD_0_ASET) |
                        CTXDESC_CD_0_AA64 |
                        FIELD_PREP(CTXDESC_CD_0_ASID, cd->asid) |
                        CTXDESC_CD_0_V;

                if (smmu_domain->stall_enabled)
                        val |= CTXDESC_CD_0_S;
        }

        /*
         * The SMMU accesses 64-bit values atomically. See IHI0070Ca 3.21.3
         * "Configuration structures and configuration invalidation completion"
         *
         *   The size of single-copy atomic reads made by the SMMU is
         *   IMPLEMENTATION DEFINED but must be at least 64 bits. Any single
         *   field within an aligned 64-bit span of a structure can be altered
         *   without first making the structure invalid.
         */
        WRITE_ONCE(cdptr[0], cpu_to_le64(val));
        arm_smmu_sync_cd(smmu_domain, ssid, true);
        return 0;
}
```

Before writing into the CD, it should retrieve the CD associated with current 
device and SSID. During the retrieving the entry, if it has not been initialized, 
it will initialize the L1 and L2 CD properly. 

```cpp
static __le64 *arm_smmu_get_cd_ptr(struct arm_smmu_domain *smmu_domain,
                                   u32 ssid)
{
        __le64 *l1ptr;
        unsigned int idx;
        struct arm_smmu_l1_ctx_desc *l1_desc;
        struct arm_smmu_device *smmu = smmu_domain->smmu;
        struct arm_smmu_ctx_desc_cfg *cdcfg = &smmu_domain->s1_cfg.cdcfg;

        if (smmu_domain->s1_cfg.s1fmt == STRTAB_STE_0_S1FMT_LINEAR)
                return cdcfg->cdtab + ssid * CTXDESC_CD_DWORDS;

        idx = ssid >> CTXDESC_SPLIT;
        l1_desc = &cdcfg->l1_desc[idx];
        if (!l1_desc->l2ptr) {
                if (arm_smmu_alloc_cd_leaf_table(smmu, l1_desc))
                        return NULL;

                l1ptr = cdcfg->cdtab + idx * CTXDESC_L1_DESC_DWORDS;
                arm_smmu_write_cd_l1_desc(l1ptr, l1_desc);
                /* An invalid L1CD can be cached */
                arm_smmu_sync_cd(smmu_domain, ssid, false);
        }
        idx = ssid & (CTXDESC_L2_ENTRIES - 1);
        return l1_desc->l2ptr + idx * CTXDESC_CD_DWORDS;
}
```

When the l1_desc doesn't have a proper l2ptr, it should assign new array for 
l2_desc for setting up L2 CD. Note that l1_desc is just a meta data not the 
actual CD entries for L1 and L2. 


```cpp
static int arm_smmu_alloc_cd_leaf_table(struct arm_smmu_device *smmu,
                                        struct arm_smmu_l1_ctx_desc *l1_desc)
{
        size_t size = CTXDESC_L2_ENTRIES * (CTXDESC_CD_DWORDS << 3);

        l1_desc->l2ptr = dmam_alloc_coherent(smmu->dev, size,
                                             &l1_desc->l2ptr_dma, GFP_KERNEL);
        if (!l1_desc->l2ptr) {
                dev_warn(smmu->dev,
                         "failed to allocate context descriptor table\n");
                return -ENOMEM;
        }
        return 0;
}
```

After allocation of memory for the L2 CD, based on the ssid, actual L1 CD 
pointer is retrieved (l1ptr). To update the L1 CD entry, 
arm_smmu_write_cd_l1_desc is invoked with this pointer and l1_desc describing
information about L2 cd table. 

```cpp
static void arm_smmu_write_cd_l1_desc(__le64 *dst,
                                      struct arm_smmu_l1_ctx_desc *l1_desc)
{
        u64 val = (l1_desc->l2ptr_dma & CTXDESC_L1_DESC_L2PTR_MASK) |
                  CTXDESC_L1_DESC_V;

        /* See comment in arm_smmu_write_ctx_desc() */
        WRITE_ONCE(*dst, cpu_to_le64(val));
}
```

After setting up the L1 CD, it can retrieve the CD entry!. arm_smmu_get_cd_ptr
function calculates the location of the L2 CD (i.e., actual CD entry). Now the
arm_smmu_write_ctx_desc can sets up the CD properly and writes the entry to the 
table.

### Set-up stream table entry
Stream table entry is used to point to stage 1 page table for SMMU through the 
context descriptor (CD) and stage 2 page table (s2ttb). When the new device is
attached to the IOMMU, it allocates new stream page table entry for the device. 

```cpp
static void arm_smmu_install_ste_for_dev(struct arm_smmu_master *master)
{
        int i, j;
        struct arm_smmu_device *smmu = master->smmu;

        printk("install ste for dev! num_streams:%d\n", master->num_streams);
        for (i = 0; i < master->num_streams; ++i) {
                u32 sid = master->streams[i].id;
                __le64 *step = arm_smmu_get_step_for_sid(smmu, sid);

                /* Bridged PCI devices may end up with duplicated IDs */
                for (j = 0; j < i; j++)
                        if (master->streams[j].id == sid)
                                break;
                if (j < i)
                        continue;

                arm_smmu_write_strtab_ent(master, sid, step);
        }
}
```


```cpp
static __le64 *arm_smmu_get_step_for_sid(struct arm_smmu_device *smmu, u32 sid)
{
        __le64 *step;
        struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;

        if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
                struct arm_smmu_strtab_l1_desc *l1_desc;
                int idx;

                /* Two-level walk */
                idx = (sid >> STRTAB_SPLIT) * STRTAB_L1_DESC_DWORDS;
                l1_desc = &cfg->l1_desc[idx];
                idx = (sid & ((1 << STRTAB_SPLIT) - 1)) * STRTAB_STE_DWORDS;
                step = &l1_desc->l2ptr[idx];
        } else {
                /* Simple linear lookup */
                step = &cfg->strtab[sid * STRTAB_STE_DWORDS];
        }

        return step;
}
```
step is the pointer to the stream table entry mapped to the sid. After 
retrieving stream table entry associated with the sid, it should be properly 
set by the below function. 

```cpp
static void arm_smmu_write_strtab_ent(struct arm_smmu_master *master, u32 sid,
                                      __le64 *dst)
{
        /*
         * This is hideously complicated, but we only really care about
         * three cases at the moment:
         *
         * 1. Invalid (all zero) -> bypass/fault (init)
         * 2. Bypass/fault -> translation/bypass (attach)
         * 3. Translation/bypass -> bypass/fault (detach)
         *
         * Given that we can't update the STE atomically and the SMMU
         * doesn't read the thing in a defined order, that leaves us
         * with the following maintenance requirements:
         *
         * 1. Update Config, return (init time STEs aren't live)
         * 2. Write everything apart from dword 0, sync, write dword 0, sync
         * 3. Update Config, sync
         */
        u64 val = le64_to_cpu(dst[0]);
        bool ste_live = false;
        struct arm_smmu_device *smmu = NULL;
        struct arm_smmu_s1_cfg *s1_cfg = NULL;
        struct arm_smmu_s2_cfg *s2_cfg = NULL;
        struct arm_smmu_domain *smmu_domain = NULL;
        struct arm_smmu_cmdq_ent prefetch_cmd = {
                .opcode         = CMDQ_OP_PREFETCH_CFG,
                .prefetch       = {
                        .sid    = sid,
                },
        };

        if (master) {
                smmu_domain = master->domain;
                smmu = master->smmu;
        }

        if (smmu_domain) {
                switch (smmu_domain->stage) {
                case ARM_SMMU_DOMAIN_S1:
                        s1_cfg = &smmu_domain->s1_cfg;
                        break;
                case ARM_SMMU_DOMAIN_S2:
                case ARM_SMMU_DOMAIN_NESTED:
                        s2_cfg = &smmu_domain->s2_cfg;
                        break;
                default:
                        break;
                }
        }

        if (val & STRTAB_STE_0_V) {
                switch (FIELD_GET(STRTAB_STE_0_CFG, val)) {
                case STRTAB_STE_0_CFG_BYPASS:
                        break;
                case STRTAB_STE_0_CFG_S1_TRANS:
                case STRTAB_STE_0_CFG_S2_TRANS:
                        ste_live = true;
                        break;
                case STRTAB_STE_0_CFG_ABORT:
                        BUG_ON(!disable_bypass);
                        break;
                default:
                        BUG(); /* STE corruption */
                }
        }

        /* Nuke the existing STE_0 value, as we're going to rewrite it */
        val = STRTAB_STE_0_V;

        /* Bypass/fault */
        if (!smmu_domain || !(s1_cfg || s2_cfg)) {
                if (!smmu_domain && disable_bypass)
                        val |= FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_ABORT);
                else
                        val |= FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_BYPASS);

                dst[0] = cpu_to_le64(val);
                dst[1] = cpu_to_le64(FIELD_PREP(STRTAB_STE_1_SHCFG,
                                                STRTAB_STE_1_SHCFG_INCOMING));
                dst[2] = 0; /* Nuke the VMID */
                /*
                 * The SMMU can perform negative caching, so we must sync
                 * the STE regardless of whether the old value was live.
                 */
                if (smmu)
                        arm_smmu_sync_ste_for_sid(smmu, sid);
                return;
        }

        if (s1_cfg) {
                u64 strw = smmu->features & ARM_SMMU_FEAT_E2H ?
                        STRTAB_STE_1_STRW_EL2 : STRTAB_STE_1_STRW_NSEL1;

                BUG_ON(ste_live);
                dst[1] = cpu_to_le64(
                         FIELD_PREP(STRTAB_STE_1_S1DSS, STRTAB_STE_1_S1DSS_SSID0) |
                         FIELD_PREP(STRTAB_STE_1_S1CIR, STRTAB_STE_1_S1C_CACHE_WBRA) |
                         FIELD_PREP(STRTAB_STE_1_S1COR, STRTAB_STE_1_S1C_CACHE_WBRA) |
                         FIELD_PREP(STRTAB_STE_1_S1CSH, ARM_SMMU_SH_ISH) |
                         FIELD_PREP(STRTAB_STE_1_STRW, strw));

                if (smmu->features & ARM_SMMU_FEAT_STALLS &&
                    !master->stall_enabled)
                        dst[1] |= cpu_to_le64(STRTAB_STE_1_S1STALLD);

                val |= (s1_cfg->cdcfg.cdtab_dma & STRTAB_STE_0_S1CTXPTR_MASK) |
                        FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_S1_TRANS) |
                        FIELD_PREP(STRTAB_STE_0_S1CDMAX, s1_cfg->s1cdmax) |
                        FIELD_PREP(STRTAB_STE_0_S1FMT, s1_cfg->s1fmt);
        }

        if (s2_cfg) {
                BUG_ON(ste_live);
                dst[2] = cpu_to_le64(
                         FIELD_PREP(STRTAB_STE_2_S2VMID, s2_cfg->vmid) |
                         FIELD_PREP(STRTAB_STE_2_VTCR, s2_cfg->vtcr) |
#ifdef __BIG_ENDIAN
                         STRTAB_STE_2_S2ENDI |
#endif
                         STRTAB_STE_2_S2PTW | STRTAB_STE_2_S2AA64 |
                         STRTAB_STE_2_S2R);

                dst[3] = cpu_to_le64(s2_cfg->vttbr & STRTAB_STE_3_S2TTB_MASK);

                val |= FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_S2_TRANS);
        }

        if (master->ats_enabled)
                dst[1] |= cpu_to_le64(FIELD_PREP(STRTAB_STE_1_EATS,
                                                 STRTAB_STE_1_EATS_TRANS));

        arm_smmu_sync_ste_for_sid(smmu, sid);
        /* See comment in arm_smmu_write_ctx_desc() */
        WRITE_ONCE(dst[0], cpu_to_le64(val));
        arm_smmu_sync_ste_for_sid(smmu, sid);

        /* It's likely that we'll want to use the new STE soon */
        if (!(smmu->options & ARM_SMMU_OPT_SKIP_PREFETCH))
                arm_smmu_cmdq_issue_cmd(smmu, &prefetch_cmd);
}
```


## Map physical memories for device through IOMMU
Now the Stream table and CDs are all initialized for the device to utilize the 
IOMMU for translation. Now let's see how the device driver associated with the 
device can actually access the DRAM through the SMMU translation. To understand 
it, we should have a deep understanding about the dma sub-system of the kernel. 

### IOVA -> Physical address translation 
IOMMU makes use of IOVA which is the virtual address that the IOMMU connected 
device can access. The real accesses to the DRAM is achieved through the SMMU 
as a result of IOVA to physical address translation. Therefore, the first job 
of the IOMMU sub system is generating the IOVA that can be accessible from the 
device and host processor simultaneously. After the IOVA generation, it should 
be mapped to the physical addresses through manipulating the page table managed 
by the IOMMU. Therefore, when the dma_alloc_XX series of functions are invoked 
in the kernel, it will first invoke the **iommu_dma_alloc** function. 


```cpp
static inline void *dma_alloc_coherent(struct device *dev, size_t size,
                dma_addr_t *dma_handle, gfp_t gfp)
{       
        return dma_alloc_attrs(dev, size, dma_handle, gfp,
                        (gfp & __GFP_NOWARN) ? DMA_ATTR_NO_WARN : 0);
}      

```cpp
void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,
                gfp_t flag, unsigned long attrs)
{
        const struct dma_map_ops *ops = get_dma_ops(dev);
        void *cpu_addr;

        WARN_ON_ONCE(!dev->coherent_dma_mask);

        /*
         * DMA allocations can never be turned back into a page pointer, so
         * requesting compound pages doesn't make sense (and can't even be
         * supported at all by various backends).
         */
        if (WARN_ON_ONCE(flag & __GFP_COMP))
                return NULL;

        if (dma_alloc_from_dev_coherent(dev, size, dma_handle, &cpu_addr))
                return cpu_addr;

        /* let the implementation decide on the zone to allocate from: */
        flag &= ~(__GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM);

        if (dma_alloc_direct(dev, ops))
                cpu_addr = dma_direct_alloc(dev, size, dma_handle, flag, attrs);
        else if (ops->alloc)
                cpu_addr = ops->alloc(dev, size, dma_handle, flag, attrs);
        else
                return NULL;

        debug_dma_alloc_coherent(dev, size, *dma_handle, cpu_addr, attrs);
        return cpu_addr;
}
EXPORT_SYMBOL(dma_alloc_attrs);
```

```cpp
static const struct dma_map_ops iommu_dma_ops = {
        .flags                  = DMA_F_PCI_P2PDMA_SUPPORTED,
        .alloc                  = iommu_dma_alloc,
        .free                   = iommu_dma_free,
        .alloc_pages            = dma_common_alloc_pages,
        .free_pages             = dma_common_free_pages,
        .alloc_noncontiguous    = iommu_dma_alloc_noncontiguous,
        .free_noncontiguous     = iommu_dma_free_noncontiguous,
        .mmap                   = iommu_dma_mmap,
        .get_sgtable            = iommu_dma_get_sgtable,
        .map_page               = iommu_dma_map_page,
        .unmap_page             = iommu_dma_unmap_page,
        .map_sg                 = iommu_dma_map_sg,
        .unmap_sg               = iommu_dma_unmap_sg,
        .sync_single_for_cpu    = iommu_dma_sync_single_for_cpu,
        .sync_single_for_device = iommu_dma_sync_single_for_device,
        .sync_sg_for_cpu        = iommu_dma_sync_sg_for_cpu,
        .sync_sg_for_device     = iommu_dma_sync_sg_for_device,
        .map_resource           = iommu_dma_map_resource,
        .unmap_resource         = iommu_dma_unmap_resource,
        .get_merge_boundary     = iommu_dma_get_merge_boundary,
        .opt_mapping_size       = iommu_dma_opt_mapping_size,
};
```
The ops->alloc will invoke iommu_dma_alloc function if the dev is connected to 
the iommu subsystem.

```cpp
static void *iommu_dma_alloc(struct device *dev, size_t size,
                dma_addr_t *handle, gfp_t gfp, unsigned long attrs)
{
        bool coherent = dev_is_dma_coherent(dev);
        int ioprot = dma_info_to_prot(DMA_BIDIRECTIONAL, coherent, attrs);
        struct page *page = NULL;
        void *cpu_addr;

        gfp |= __GFP_ZERO;

        if (gfpflags_allow_blocking(gfp) &&
            !(attrs & DMA_ATTR_FORCE_CONTIGUOUS)) {
                return iommu_dma_alloc_remap(dev, size, handle, gfp,
                                dma_pgprot(dev, PAGE_KERNEL, attrs), attrs);
        }

        if (IS_ENABLED(CONFIG_DMA_DIRECT_REMAP) &&
            !gfpflags_allow_blocking(gfp) && !coherent)
                page = dma_alloc_from_pool(dev, PAGE_ALIGN(size), &cpu_addr,
                                               gfp, NULL);
        else
                cpu_addr = iommu_dma_alloc_pages(dev, size, &page, gfp, attrs);
        if (!cpu_addr)
                return NULL;

        *handle = __iommu_dma_map(dev, page_to_phys(page), size, ioprot,
                        dev->coherent_dma_mask);
        if (*handle == DMA_MAPPING_ERROR) {
                __iommu_dma_free(dev, size, cpu_addr);
                return NULL;
        }

        return cpu_addr;
}
```

DMA allocation consists of two parts: 1.reserving memory pages to be used as DMA
pages between the CPU and SMMU device and 2.generating mapping in the IOMMU 
table. The first part is done for the CPU parts and the other part is to allow 
devices to access the shared memory region. 

### Reserve DMA memory and retrieve IOVA
```cpp
static void *iommu_dma_alloc_pages(struct device *dev, size_t size,
                struct page **pagep, gfp_t gfp, unsigned long attrs)
{
        bool coherent = dev_is_dma_coherent(dev);
        size_t alloc_size = PAGE_ALIGN(size);
        int node = dev_to_node(dev);
        struct page *page = NULL;
        void *cpu_addr;

        page = dma_alloc_contiguous(dev, alloc_size, gfp);
        if (!page)
                page = alloc_pages_node(node, gfp, get_order(alloc_size));
        if (!page)
                return NULL;

        if (!coherent || PageHighMem(page)) {
                pgprot_t prot = dma_pgprot(dev, PAGE_KERNEL, attrs);

                cpu_addr = dma_common_contiguous_remap(page, alloc_size,
                                prot, __builtin_return_address(0));
                if (!cpu_addr)
                        goto out_free_pages;

                if (!coherent)
                        arch_dma_prep_coherent(page, size);
        } else {
                cpu_addr = page_address(page);
        }

        *pagep = page;
        memset(cpu_addr, 0, alloc_size);
        return cpu_addr;
out_free_pages:
        dma_free_contiguous(dev, page, alloc_size);
        return NULL;
}
```

```cpp
static dma_addr_t __iommu_dma_map(struct device *dev, phys_addr_t phys,
                size_t size, int prot, u64 dma_mask)
{
        struct iommu_domain *domain = iommu_get_dma_domain(dev);
        struct iommu_dma_cookie *cookie = domain->iova_cookie;
        struct iova_domain *iovad = &cookie->iovad;
        size_t iova_off = iova_offset(iovad, phys);
        dma_addr_t iova;

        if (static_branch_unlikely(&iommu_deferred_attach_enabled) &&
            iommu_deferred_attach(dev, domain))
                return DMA_MAPPING_ERROR;

        size = iova_align(iovad, size + iova_off);

        iova = iommu_dma_alloc_iova(domain, size, dma_mask, dev);
        if (!iova)
                return DMA_MAPPING_ERROR;

        if (iommu_map_atomic(domain, iova, phys - iova_off, size, prot)) {
                iommu_dma_free_iova(cookie, iova, size, NULL);
                return DMA_MAPPING_ERROR;
        }
        return iova + iova_off;
}
```

```cpp
static dma_addr_t iommu_dma_alloc_iova(struct iommu_domain *domain,
                size_t size, u64 dma_limit, struct device *dev)
{
        struct iommu_dma_cookie *cookie = domain->iova_cookie;
        struct iova_domain *iovad = &cookie->iovad;
        unsigned long shift, iova_len, iova = 0;

        if (cookie->type == IOMMU_DMA_MSI_COOKIE) {
                cookie->msi_iova += size;
                return cookie->msi_iova - size;
        }

        shift = iova_shift(iovad);
        iova_len = size >> shift;

        dma_limit = min_not_zero(dma_limit, dev->bus_dma_limit);

        if (domain->geometry.force_aperture)
                dma_limit = min(dma_limit, (u64)domain->geometry.aperture_end);

        /* Try to get PCI devices a SAC address */
        if (dma_limit > DMA_BIT_MASK(32) && !iommu_dma_forcedac && dev_is_pci(dev))
                iova = alloc_iova_fast(iovad, iova_len,
                                       DMA_BIT_MASK(32) >> shift, false);

        if (!iova)
                iova = alloc_iova_fast(iovad, iova_len, dma_limit >> shift,
                                       true);

        return (dma_addr_t)iova << shift;
}
```

```cpp
unsigned long
alloc_iova_fast(struct iova_domain *iovad, unsigned long size,
                unsigned long limit_pfn, bool flush_rcache)
{
        unsigned long iova_pfn;
        struct iova *new_iova;
                
        /*
         * Freeing non-power-of-two-sized allocations back into the IOVA caches
         * will come back to bite us badly, so we have to waste a bit of space
         * rounding up anything cacheable to make sure that can't happen. The
         * order of the unadjusted size will still match upon freeing.
         */     
        if (size < (1 << (IOVA_RANGE_CACHE_MAX_SIZE - 1)))
                size = roundup_pow_of_two(size);

        iova_pfn = iova_rcache_get(iovad, size, limit_pfn + 1);
        if (iova_pfn)
                return iova_pfn;
        
retry:  
        new_iova = alloc_iova(iovad, size, limit_pfn, true);
        if (!new_iova) {
                unsigned int cpu;
                
                if (!flush_rcache)
                        return 0;
                
                /* Try replenishing IOVAs by flushing rcache. */
                flush_rcache = false;
                for_each_online_cpu(cpu)
                        free_cpu_cached_iovas(cpu, iovad);
                free_global_cached_iovas(iovad);
                goto retry;
        }       
        
        return new_iova->pfn_lo;
}       
EXPORT_SYMBOL_GPL(alloc_iova_fast);
```


```cpp
/**
 * alloc_iova - allocates an iova
 * @iovad: - iova domain in question
 * @size: - size of page frames to allocate
 * @limit_pfn: - max limit address
 * @size_aligned: - set if size_aligned address range is required
 * This function allocates an iova in the range iovad->start_pfn to limit_pfn,
 * searching top-down from limit_pfn to iovad->start_pfn. If the size_aligned
 * flag is set then the allocated address iova->pfn_lo will be naturally
 * aligned on roundup_power_of_two(size).
 */
struct iova *
alloc_iova(struct iova_domain *iovad, unsigned long size,
        unsigned long limit_pfn,
        bool size_aligned)
{
        struct iova *new_iova;
        int ret;

        new_iova = alloc_iova_mem();
        if (!new_iova)
                return NULL;

        ret = __alloc_and_insert_iova_range(iovad, size, limit_pfn + 1,
                        new_iova, size_aligned);

        if (ret) {
                free_iova_mem(new_iova);
                return NULL;
        }

        return new_iova;
}
EXPORT_SYMBOL_GPL(alloc_iova);
```

```cpp
static struct iova *alloc_iova_mem(void)
{
        return kmem_cache_zalloc(iova_cache, GFP_ATOMIC | __GFP_NOWARN);
}
```
Note that the iova memory is just another kernel memory 


```cpp
static int __alloc_and_insert_iova_range(struct iova_domain *iovad,
                unsigned long size, unsigned long limit_pfn,
                        struct iova *new, bool size_aligned)
{
        struct rb_node *curr, *prev;
        struct iova *curr_iova;
        unsigned long flags;
        unsigned long new_pfn, retry_pfn;
        unsigned long align_mask = ~0UL;
        unsigned long high_pfn = limit_pfn, low_pfn = iovad->start_pfn;

        if (size_aligned)
                align_mask <<= fls_long(size - 1);

        /* Walk the tree backwards */
        spin_lock_irqsave(&iovad->iova_rbtree_lock, flags);
        if (limit_pfn <= iovad->dma_32bit_pfn &&
                        size >= iovad->max32_alloc_size)
                goto iova32_full;

        curr = __get_cached_rbnode(iovad, limit_pfn);
        curr_iova = to_iova(curr);
        retry_pfn = curr_iova->pfn_hi + 1;

retry:
        do {
                high_pfn = min(high_pfn, curr_iova->pfn_lo);
                new_pfn = (high_pfn - size) & align_mask;
                prev = curr;
                curr = rb_prev(curr);
                curr_iova = to_iova(curr);
        } while (curr && new_pfn <= curr_iova->pfn_hi && new_pfn >= low_pfn);

        if (high_pfn < size || new_pfn < low_pfn) {
                if (low_pfn == iovad->start_pfn && retry_pfn < limit_pfn) {
                        high_pfn = limit_pfn;
                        low_pfn = retry_pfn;
                        curr = iova_find_limit(iovad, limit_pfn);
                        curr_iova = to_iova(curr);
                        goto retry;
                }
                iovad->max32_alloc_size = size;
                goto iova32_full;
        }

        /* pfn_lo will point to size aligned address if size_aligned is set */
        new->pfn_lo = new_pfn;
        new->pfn_hi = new->pfn_lo + size - 1;

        /* If we have 'prev', it's a valid place to start the insertion. */
        iova_insert_rbtree(&iovad->rbroot, new, prev);
        __cached_rbnode_insert_update(iovad, new);

        spin_unlock_irqrestore(&iovad->iova_rbtree_lock, flags);
        return 0;

iova32_full:
        spin_unlock_irqrestore(&iovad->iova_rbtree_lock, flags);
        return -ENOMEM;
}


```


### Generating IOVA->PA mapping
```cpp
int iommu_map_atomic(struct iommu_domain *domain, unsigned long iova,
              phys_addr_t paddr, size_t size, int prot)
{       
        return _iommu_map(domain, iova, paddr, size, prot, GFP_ATOMIC);
}               

static int _iommu_map(struct iommu_domain *domain, unsigned long iova,
                      phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{       
        const struct iommu_domain_ops *ops = domain->ops;
        int ret;

        ret = __iommu_map(domain, iova, paddr, size, prot, gfp);
        if (ret == 0 && ops->iotlb_sync_map)
                ops->iotlb_sync_map(domain, iova, size);

        return ret;
	}       

```

```cpp
static int __iommu_map(struct iommu_domain *domain, unsigned long iova,
                       phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
        const struct iommu_domain_ops *ops = domain->ops;
        unsigned long orig_iova = iova;
        unsigned int min_pagesz;
        size_t orig_size = size;
        phys_addr_t orig_paddr = paddr;
        int ret = 0;

        if (unlikely(!(ops->map || ops->map_pages) ||
                     domain->pgsize_bitmap == 0UL))
                return -ENODEV;

        if (unlikely(!(domain->type & __IOMMU_DOMAIN_PAGING)))
                return -EINVAL;

        /* find out the minimum page size supported */
        min_pagesz = 1 << __ffs(domain->pgsize_bitmap);

        /*
         * both the virtual address and the physical one, as well as
         * the size of the mapping, must be aligned (at least) to the
         * size of the smallest page supported by the hardware
         */
        if (!IS_ALIGNED(iova | paddr | size, min_pagesz)) {
                pr_err("unaligned: iova 0x%lx pa %pa size 0x%zx min_pagesz 0x%x\n",
                       iova, &paddr, size, min_pagesz);
                return -EINVAL;
        }

        pr_debug("map: iova 0x%lx pa %pa size 0x%zx\n", iova, &paddr, size);

        while (size) {
                size_t mapped = 0;

                ret = __iommu_map_pages(domain, iova, paddr, size, prot, gfp,
                                        &mapped);
                /*
                 * Some pages may have been mapped, even if an error occurred,
                 * so we should account for those so they can be unmapped.
                 */
                size -= mapped;

                if (ret)
                        break;

                iova += mapped;
                paddr += mapped;
        }

        /* unroll mapping in case something went wrong */
        if (ret)
                iommu_unmap(domain, orig_iova, orig_size - size);
        else
                trace_map(orig_iova, orig_paddr, orig_size);

        return ret;
```

```cpp
static int __iommu_map_pages(struct iommu_domain *domain, unsigned long iova,
                             phys_addr_t paddr, size_t size, int prot,
                             gfp_t gfp, size_t *mapped)
{
        const struct iommu_domain_ops *ops = domain->ops;
        size_t pgsize, count;
        int ret;

        pgsize = iommu_pgsize(domain, iova, paddr, size, &count);

        pr_debug("mapping: iova 0x%lx pa %pa pgsize 0x%zx count %zu\n",
                 iova, &paddr, pgsize, count);

        if (ops->map_pages) {
                ret = ops->map_pages(domain, iova, paddr, pgsize, count, prot,
                                     gfp, mapped);
        } else {
                ret = ops->map(domain, iova, paddr, pgsize, prot, gfp);
                *mapped = ret ? 0 : pgsize;
        }

        return ret;
}
```
Through this long IOMMU subsystem layers, it finally invokes the map / map_pages
functions of the SMMU. Now the iommu abstraction ends and it invokes the funcs
stored in the iommu_domain_ops to handle IOMMU instance specific functions. As
the current domain is set as SMMU, functions invoked through the 
iommu_domain_ops will be the SMMU functions to set-up related data structures to 
enable the iommu mapping. 


```cpp
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
```
This map functions are initialized when the device is attached to the SMMU.
Therefore, the map function will end up invoking the arm_smmu_map_pages.

```cpp
static int arm_smmu_map_pages(struct iommu_domain *domain, unsigned long iova,
                              phys_addr_t paddr, size_t pgsize, size_t pgcount,
                              int prot, gfp_t gfp, size_t *mapped)
{       
        struct io_pgtable_ops *ops = to_smmu_domain(domain)->pgtbl_ops;
        
        if (!ops)
                return -ENODEV;
        
        return ops->map_pages(ops, iova, paddr, pgsize, pgcount, prot, gfp, mapped);
}
```

Now finally through the domain, it invokes the map_pages function set in the 
pgtbl_ops. Note that the functions have been set for managing the SMMU specific
data structures to manipulate the page tables and relevant data structures in 
finalizing domain (refer to alloc_io_pgtable_ops). The map_pages function of the 
retrieved pgtbl_ops is arm_lpae_map_pages.

```cpp
static int arm_lpae_map_pages(struct io_pgtable_ops *ops, unsigned long iova,
                              phys_addr_t paddr, size_t pgsize, size_t pgcount,
                              int iommu_prot, gfp_t gfp, size_t *mapped)
{
        struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
        struct io_pgtable_cfg *cfg = &data->iop.cfg;
        arm_lpae_iopte *ptep = data->pgd;
        int ret, lvl = data->start_level;
        arm_lpae_iopte prot;
        long iaext = (s64)iova >> cfg->ias;

        if (WARN_ON(!pgsize || (pgsize & cfg->pgsize_bitmap) != pgsize))
                return -EINVAL;

        if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1)
                iaext = ~iaext;
        if (WARN_ON(iaext || paddr >> cfg->oas))
                return -ERANGE;

        /* If no access, then nothing to do */
        if (!(iommu_prot & (IOMMU_READ | IOMMU_WRITE)))
                return 0;

        prot = arm_lpae_prot_to_pte(data, iommu_prot);
        ret = __arm_lpae_map(data, iova, paddr, pgsize, pgcount, prot, lvl,
                             ptep, gfp, mapped);
        /*
         * Synchronise all PTE updates for the new mapping before there's
         * a chance for anything to kick off a table walk for the new iova.
         */
        wmb();

        return ret;
}
```

```cpp
static int __arm_lpae_map(struct arm_lpae_io_pgtable *data, unsigned long iova,
                          phys_addr_t paddr, size_t size, size_t pgcount,
                          arm_lpae_iopte prot, int lvl, arm_lpae_iopte *ptep,
                          gfp_t gfp, size_t *mapped)
{
        arm_lpae_iopte *cptep, pte;
        size_t block_size = ARM_LPAE_BLOCK_SIZE(lvl, data);
        size_t tblsz = ARM_LPAE_GRANULE(data);
        struct io_pgtable_cfg *cfg = &data->iop.cfg;
        int ret = 0, num_entries, max_entries, map_idx_start;

        /* Find our entry at the current level */
        map_idx_start = ARM_LPAE_LVL_IDX(iova, lvl, data);
        ptep += map_idx_start;

        /* If we can install a leaf entry at this level, then do so */
        if (size == block_size) {
                max_entries = ARM_LPAE_PTES_PER_TABLE(data) - map_idx_start;
                num_entries = min_t(int, pgcount, max_entries);
                ret = arm_lpae_init_pte(data, iova, paddr, prot, lvl, num_entries, ptep);
                if (!ret)
                        *mapped += num_entries * size;

                return ret;
        }

        /* We can't allocate tables at the final level */
        if (WARN_ON(lvl >= ARM_LPAE_MAX_LEVELS - 1))
                return -EINVAL;

        /* Grab a pointer to the next level */
        pte = READ_ONCE(*ptep);
        if (!pte) {
                cptep = __arm_lpae_alloc_pages(tblsz, gfp, cfg);
                if (!cptep)
                        return -ENOMEM;

                pte = arm_lpae_install_table(cptep, ptep, 0, data);
                if (pte)
                        __arm_lpae_free_pages(cptep, tblsz, cfg);
        } else if (!cfg->coherent_walk && !(pte & ARM_LPAE_PTE_SW_SYNC)) {
                __arm_lpae_sync_pte(ptep, 1, cfg);
        }

        if (pte && !iopte_leaf(pte, lvl, data->iop.fmt)) {
                cptep = iopte_deref(pte, data);
        } else if (pte) {
                /* We require an unmap first */
                WARN_ON(!selftest_running);
                return -EEXIST;
        }

        /* Rinse, repeat */
        return __arm_lpae_map(data, iova, paddr, size, pgcount, prot, lvl + 1,
                              cptep, gfp, mapped);
}
```

```cpp
static arm_lpae_iopte arm_lpae_install_table(arm_lpae_iopte *table,
                                             arm_lpae_iopte *ptep,
                                             arm_lpae_iopte curr,
                                             struct arm_lpae_io_pgtable *data)
{
        arm_lpae_iopte old, new;
        struct io_pgtable_cfg *cfg = &data->iop.cfg;

        new = paddr_to_iopte(__pa(table), data) | ARM_LPAE_PTE_TYPE_TABLE;
        if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_NS)
                new |= ARM_LPAE_PTE_NSTABLE;

        /*
         * Ensure the table itself is visible before its PTE can be.
         * Whilst we could get away with cmpxchg64_release below, this
         * doesn't have any ordering semantics when !CONFIG_SMP.
         */
        dma_wmb();

        old = cmpxchg64_relaxed(ptep, curr, new);

        if (cfg->coherent_walk || (old & ARM_LPAE_PTE_SW_SYNC))
                return old;

        /* Even if it's not ours, there's no point waiting; just kick it */
        __arm_lpae_sync_pte(ptep, 1, cfg);
        if (old == curr)
                WRITE_ONCE(*ptep, new | ARM_LPAE_PTE_SW_SYNC);

        return old;
}
```
When the next level page table entries do not exist, it should be generated (by
the __arm_lpae_alloc_pages) and pointed to by the current level page table entry
pointer (done by arm_lpae_install_table). Since the page table used by the smmu 
is same as of MMU page table, it consists of multiple levels. Therefore, after 
handling the current level, it invokes the same function to handle next level 
until the IOVA is mapped to the physical address. When it reaches to the target 
size of the IOVA, it invokes arm_lpae_init_pte function to generate last page 
table entry. 

```cpp
static int arm_lpae_init_pte(struct arm_lpae_io_pgtable *data,
                             unsigned long iova, phys_addr_t paddr,
                             arm_lpae_iopte prot, int lvl, int num_entries,
                             arm_lpae_iopte *ptep)
{
        int i;

        for (i = 0; i < num_entries; i++) 
                if (iopte_leaf(ptep[i], lvl, data->iop.fmt)) {
                        /* We require an unmap first */
                        WARN_ON(!selftest_running);
                        return -EEXIST;
                } else if (iopte_type(ptep[i]) == ARM_LPAE_PTE_TYPE_TABLE) {
                        /*   
                         * We need to unmap and free the old table before
                         * overwriting it with a block entry.
                         */
                        arm_lpae_iopte *tblp;
                        size_t sz = ARM_LPAE_BLOCK_SIZE(lvl, data);

                        tblp = ptep - ARM_LPAE_LVL_IDX(iova, lvl, data);
                        if (__arm_lpae_unmap(data, NULL, iova + i * sz, sz, 1,
                                             lvl, tblp) != sz) {
                                WARN_ON(1);
                                return -EINVAL;
                        }    
                }    

        __arm_lpae_init_pte(data, paddr, prot, lvl, num_entries, ptep);
        return 0;
}
```

As the dma allocation usually requires chunk of pages at one function call, it 
needs to allocate multiple contiguous pages for servicing one DMA call. Before 
updating the PTE, it first checks whether all pages are unmapped and is ready 
to be mapped with same block size.

```cpp
static void __arm_lpae_init_pte(struct arm_lpae_io_pgtable *data,
                                phys_addr_t paddr, arm_lpae_iopte prot,
                                int lvl, int num_entries, arm_lpae_iopte *ptep)
{
        arm_lpae_iopte pte = prot;
        struct io_pgtable_cfg *cfg = &data->iop.cfg;
        size_t sz = ARM_LPAE_BLOCK_SIZE(lvl, data);
        int i;

        if (data->iop.fmt != ARM_MALI_LPAE && lvl == ARM_LPAE_MAX_LEVELS - 1)
                pte |= ARM_LPAE_PTE_TYPE_PAGE;
        else
                pte |= ARM_LPAE_PTE_TYPE_BLOCK;

        for (i = 0; i < num_entries; i++)
                ptep[i] = pte | paddr_to_iopte(paddr + i * sz, data);

        if (!cfg->coherent_walk)
                __arm_lpae_sync_pte(ptep, num_entries, cfg);
}
```

Based on whether it is 4KB or larger block it sets PAGE type in the pte and 
writes the content to the ptep! After allocating all pages, it synchronize 
pte by submitting smmu commands. 





## Appendix 
The attach_dev callback in iommu_ops is used to create various data structures for the master device on the SMMU side. The following is
the process of arm_smmu_attach_dev:
```
arm_smmu_attach_dev 
  +-> arm_smmu_domain_finalise 
        /* 
         * The purpose is to create pgtbl_ops in smmu_domain, the prototype of this structure is struct io_pgtable_ops 
         * struct io_pgtable_ops 
         * +-> map    
         * +-> unmap 
         * +-> iova_to_phys 
         */ 
    +-> alloc_io_pgtable_ops 
        /* 
         * Taking 64bit s1 as an example, the following function initializes the pgd of the page table, and initializes 
         the * operation function 
         */       +-> arm_64_lpae_alloc_pgtable_s1 of the page table map/unmap


            /* Mainly create page table operation functions*/ 
        +-> arm_lpae_alloc_pgtable 
          +-> map = arm_lpae_map 
          +-> unmap = arm_lpae_unmap 
          +-> iova_to_phys = arm_lpae_iova_to_phys

            /* create pgd */ 
        +-> __arm_lpae_alloc_pages

            /* Get page table base address*/ 
        +-> cfg->arm_lpae_s1_cfg.ttbr = virt_to_phys(data->pgd);

        /* The final configuration is done again, currently it is used to configure the CD table */ 
    +-> finalise_stage_fn 
        /* The obtained io_pgtable_ops is stored in smmu_domain */ 
    +-> smmu_domain->pgtbl_ops

  +-> arm_smmu_install_ste_for_dev
```

We trace down from the kernel DMA API interface to observe the application of dma memory and the process of map. Taking dma_alloc_coherent
as an example, this interface applies for memory according to the user's request, and returns the CPU virtual address and iova.

```
dma_alloc_coherent 
  +-> dma_alloc_attrs /* kernel/dma/mapping.c */ 
    +-> iommu_dma_alloc /* drivers/iommu/dma-iommu.c */ 
          /* 
           * The following is the main logic of memory allocation and map, which can be roughly divided into two piece. The first block is iomm_dma_alloc_remap, 
           * this memory allocation and map are completed together in this function, the second block is the rest of the logic, this part of the logic separates the allocated 
           * memory from the map. In the second part, there are memory allocation from dma pool and direct allocation of memory. We do not analyze 
           the case in *dma pool. 
           * 
           * The core difference between the above case1 and case3 is whether there is a kernel configuration with DMA_REMAP enabled, corresponding to the specific implementation 
           * Yes, in the case of REMAP, you can apply for discontinuous physical pages, and call the remap function to obtain continuous CPU virtual addresses. 
           * It can be seen that REMAP really supports a wide range of dma addresses. If REMAP is not enabled, that is case 3, 
           * iommu_dma_alloc_pages actually calls the interface of the partner system (regardless of the case of CMA), affected by MAX_ORDER 
           *, the contiguous physical memory that can be allocated at one time is limited. 
           */
      +-> iommu_dma_alloc_remap 
            /* 
             * Allocate physical pages according to size, and call the partner system interface multiple times to allocate discontinuous physical page blocks. At the same time 
             * this function also makes a map of iommu. Let's take a closer look at the details of this function. 
             */ 
        +-> __iommu_dma_alloc_noncontiguous 
              /* This coquettish bit operation gets the value of the smallest level, generally the smallest level is the system page size*/ + 
          -> min_size = alloc_sizes & -alloc_sizes; 
              /* 
               * The allocation algorithm is in In the following function, count is the number of pages that need to be allocated, and the page here refers to the system 
               * page size. The order_mask is the mask of the size of blocks at all levels in the page table. Obviously, this information is obtained to 
               * allocate as much as possible from the block when allocating. This information is obtained from the pgsize_bitmap of iommu_domain. 
               * pgsize_bitmap is related to the specific page table implementation. In the specific Assign values ​​in the iommu driver, such as ARM's 
               *SMMUv3 with a 4K page size, its block sizes at all levels are 4K, 2M and 1G, so pgsize_bitmap
               * is SZ_4K|SZ_2M|SZ_1G. 
               */ 
          +-> __iommu_dma_alloc_pages(..., count, order_mask, ...) 
                /* 
                 * This while loop is the main logic of allocation, and calculates the size of each allocation of memory through bit operations. 
                 * (2U << __fls(count) - 1) Get the mask of count, for example, count is 0b1000, 
                 * mask is 0b1111, mask and order_mask are ANDed, and the highest bit is taken out, which is 
                 the maximum block size that can allocate memory for * current count, Then call the interface 
                 * of the partner system to allocate continuous physical memory. Then, jump out of the loop, update the count that needs to be allocated next time, 
                 * Put the physical memory allocated this time into the output pages array page by page. Although the allocation 
                 * can be a block with continuous physical addresses, the output is still saved in pages. 
                 */ 
            +-> while (count) {...} 
              /* allocate iova */ 
          +-> iommu_dam_alloc_iova
              /* 
               * Combine the physical pages allocated above with one sgl data, note that continuous physical pages will 
               * be merged into one sgl node. The following iommu_map can map a block to 
               a block of a page table. However, the specific map logic must 
               be implemented in the specific iommu-driven map * callback function. From the analysis here, it can be seen that 
               the size value input by the iommu-driven map callback function is not necessarily the page size. 
               */ 
          +-> sg_alloc_table_from_pages 
              /* Create a map from iova to physical pages */ 
          +-> iommu_map_sg_atomic             /* Remap discrete physical pages to continuous CPU virtual addresses */         +-> dma_common_pages_remap
       



      +->iommu_dma_alloc_pages

      +-> __iommu_dma_map 
        [...] 
            /* You can see that the while loop of this function is also a similar algorithm allocated from the largest block */ 
        +-> __iommu_map

```

```
/* drivers/iommu/io-pgtbl-arm.c */ 
/* 
* This function is the specific function for page table mapping. The input iova, paddr, size, and iomm_prot of the function have * 
indicated the va and pa of the address to be mapped , size and attributes. 
Here, the specific allocation of iova and paddr has been done in the upper dma * framework. lvl is a parameter related to the ARM SMMUv3 page table level. The 
page table level corresponding to different page sizes, VA digits * stage, and the starting level are different. For example, in the following 48bit, 4K page 
* size cases, there are level0/1/2/3 four-level page tables. What __arm_lpae_map specifically does is add a translation for a given 
*map parameter to the page table. 
*/ 
arm_smmu_map 
  +-> arm_lpae_map 
    +-> __arm_lpae_map(..., iova, paddr, size, prot, lvl, ptep, ...)

```
The implementation of __arm_lpae_map is relatively straightforward, which is to recursively create page tables.
Do page table mapping completely according to the page or block map given by the upper layer .



### Page table for SMMUv3
```
    level 0 1 3 3 
    block size 1G 2M 4K 
+-------+-------+-------+-------+--- -----+--------+-------+ 
|63 56|55 48|47 39|38 30|29 21|20 12|11 0| 
+--- -----+--------+--------+--------+-------+------- +--------+ 
| | | | | | | 
| | | | v 
| | | | | [11:0] in-page offset 
| | | | +-> [20:12] L3 index 
| | +-----------> [29:21] L2 index 
| | +---------------------> [38 :30] L1 index
| +-------------------------------> [47:39] L0 index 
+------- -----------------------------------------> [63] TTBR0/1
```

The above is an ARM64 (SMMUv3) 48bit, 4K page size VA used to divide the page table index at each level. This division is more
common. The riscv sv39 is also divided in this way, but the highest level is missing. Under this division, each level of
page table has 512 entries. If a page table entry is 64bit, each table of each level of page table occupies exactly 4KB of memory.







## Regs should be protected
### Stream table related
SMMU_CR2.E2H should be 1 not to make the SMMU bypass the translation. 
STRW of the stream entry to indicate it goes to NW stream world. 
There is no Realm world stream and its tables so it should be normal world stream.
S1DSS should be set as fault cause we don't want to make substream not registered
in the stream table to access the secure memory.









##

valid in the RMM context means that the s2tte is used as valid mapping.
For example, if the ripas is changed from the ram to empty the valid
mapping accessible as the lsb bits of the s2tte will be deleted by 
the RMI call also. 
