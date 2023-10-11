## Four different types of S2TTE in RMM
RIPAS and HIPAS is additional concept only applicable for realms. Also, it is 
implemented utilizing the reserved bits of the descriptor. Therefore, basic
understanding of how stage 2 page table descriptors are interpreted is necessary.
MMU determines the type of the descriptor by looking at Desc[1:0] bits. 

```cpp
#define S2TTE_L012_TABLE                0x3UL
#define S2TTE_L012_BLOCK                0x1UL
#define S2TTE_L3_PAGE                   0x3UL
#define S2TTE_Lx_INVALID                0x0UL
```
RTT utilize four basic stage 2 table descriptor types. Then why it is important
in terms of HIPAS and RIPAS?

### Which bits are used for HIPAS / RIPAS? 
RMM utilize the Desc[5:2] as HIPAS and Desc[6] as RIPAS. Also note that the 
concept of RIPAS and HIPAS is invented for invalid stage 2 descriptor. Since 
the lower attributes of the invalid descriptor is ignored by the MMU, RMM can 
utilize these bits to convey additional non-architectural information. 

However, when it comes to valid descriptor including PAGE and BLOCK, bits 
assigned for HIPAS and RIPAS will be interpreted as architectural information by
the MMU. For example, the bits used by the HIPAS completely overlaps with the 
MemAttr which determines memory attribute and cache behavior of that page. Also, 
Desc[6] is interpreted as S2AP[0] which determines the access permission of the
memory page. 

There could be two design decision to utilize HIPAS and RIPAS to avoid problem 
due to overlapping region of RIPAS/ HIPAS. 

1. Apply HIPAS and RIPAS to **invalid** stage 2 page descriptor and make RMM to 
check whether the page is valid or not. 
2. Define semantic of HIPAS and RIPAS to considering possible interpretation of 
those fields as lower memory attributes for valid descriptor.

Keep in mind this and let's see how the RMM implements the RIPAS and HIPAS!

## RTT creation focusing on HIPAS and RIPAS
Because RIPAS/HIPAS and other attributes I mentioned are relevant to the stage 2
descriptor, I will go through relevant RMIs that changes the S2TT which is 
descriptor of stage 2 page table managed by RMM. let's take a look at how the 
RMM creates the descriptor and enforce particular rules on it. 

```cpp
unsigned long smc_rtt_create(unsigned long rtt_addr, //host provided address that can be used as rtt table!
                             unsigned long rd_addr,
                             unsigned long map_addr, //IPA address of guest that should be mapped in RTT
                             unsigned long ulevel)
{       
	......
        rtt_walk_lock_unlock(g_table_root, sl, ipa_bits,
                                map_addr, level - 1L, &wi);
        if (wi.last_level != level - 1L) {
                INFO("UPPER level RTT should be generated\n");
                //check why it works with 3level at first :*
                ret = pack_return_code(RMI_ERROR_RTT, wi.last_level);
                goto out_unlock_llt;
        }
        
        parent_s2tt = granule_map(wi.g_llt, SLOT_RTT);
        parent_s2tte = s2tte_read(&parent_s2tt[wi.index]);
        s2tt = granule_map(g_tbl, SLOT_DELEGATED); //new s2tt that needs to be added under parent_s2tt
```

Let's assume that the we only have the root pointer of the stage 2 page table 
and want to initialize the subsequent descriptors. In my settings, start_level
which is the level of the root rtt table is 2. As we already have RTT page for 
the root level which was generated during the REC creation, we can reasonably 
assume that ulevel of the smc_rtt_create will be 3. 

To generate new S2TT descriptor at level N, we need its parent descriptor at 
level (N-1). Because we already have root page table at level 2, we need another 
descriptor at level 3. To abstract the process traversing the S2TT, RMM provides
API function rtt_walk_lock_unlock. See [[]] for details. 

Thanks to this function, we can retrieve parent descriptor. Also, based on the
map address, the index number of the parent descriptor will be determined. Since
the root page was initialized as zero, all entries pointed to by the root will 
be presented as invalid descriptor because (Desc[1:0] == 0x0) means invalid 
descriptor (S2TTE_Lx_INVALID). Also, naturally, HIPAS and RIPAS of all root 
entries will be zero indicating unassigned and EMPTY, respectively.


```cpp
unsigned long smc_rtt_create(unsigned long rtt_addr, //host provided address that can be used as rtt table!
                             unsigned long rd_addr,
                             unsigned long map_addr, //IPA address of guest that should be mapped in RTT
                             unsigned long ulevel)
{
	......
        if (s2tte_is_unassigned(parent_s2tte)) {
                INFO("s2tte_is_unassigned\n");
                /*
                 * Note that if map_addr is an Unprotected IPA, the RIPAS field
                 * is guaranteed to be zero, in both parent and child s2ttes.
                 */
                enum ripas ripas = s2tte_get_ripas(parent_s2tte);

                s2tt_init_unassigned(s2tt, ripas);

                /*
                 * Increase the refcount of the parent, the granule was
                 * locked while table walking and hand-over-hand locking.
                 * Atomicity and acquire/release semantics not required because
                 * the table is accessed always locked.
                 */
                __granule_get(wi.g_llt);

/*                                              
 * Returns true if @s2tte has HIPAS=UNASSIGNED or HIPAS=INVALID_NS.
 */
bool s2tte_is_unassigned(unsigned long s2tte)
{        
        return s2tte_has_hipas(s2tte, S2TTE_INVALID_HIPAS_UNASSIGNED);
}        

static bool s2tte_has_hipas(unsigned long s2tte, unsigned long hipas)
{               
        unsigned long desc_type = s2tte & DESC_TYPE_MASK;
        unsigned long invalid_desc_hipas = s2tte & S2TTE_INVALID_HIPAS_MASK;
        
        if ((desc_type != S2TTE_Lx_INVALID) || (invalid_desc_hipas != hipas)) {
                return false;
        }
        return true;
}               
                        
```

Why this information HIPAS, RIPAS and descriptor types are important in creation
of new page descriptors? Because smc_rtt_create initialize new descriptor page
based on its parent descriptor information. You can easily follow the above code
and confirm that the parent_s2tte will be considered as unassigned descriptor. 


```cpp
/*
 * Populates @s2tt with s2ttes which have HIPAS=UNASSIGNED and RIPAS=@ripas.
 *              
 * The granule is populated before it is made a table,
 * hence, don't use s2tte_write for access.
 */
void s2tt_init_unassigned(unsigned long *s2tt, enum ripas ripas)
{        
        for (unsigned int i = 0U; i < S2TTES_PER_S2TT; i++) {
                s2tt[i] = s2tte_create_unassigned(ripas);
        }

        dsb(ish);
}

/*
 * Creates a value which can be OR'd with an s2tte to set RIPAS=@ripas.
 */
unsigned long s2tte_create_ripas(enum ripas ripas)
{       
        if (ripas == RIPAS_EMPTY) {
                return S2TTE_INVALID_RIPAS_EMPTY;
        }
        return S2TTE_INVALID_RIPAS_RAM;
}

/*
 * Creates an invalid s2tte with HIPAS=UNASSIGNED and RIPAS=@ripas.
 */
unsigned long s2tte_create_unassigned(enum ripas ripas)
{       
        return S2TTE_INVALID_HIPAS_UNASSIGNED | s2tte_create_ripas(ripas);
}
```

As the parent_s2tte's RIPAS is RIPAS_EMPTY, the level 3 RTT entries will be all
initialized as unassigned and EMPTY for HIPAS and RIPAS, respectively. This 
process will be repeated until it reaches the end-level. Because we initialized 
newly added s2tt page's entries as unassigned empty, the next level page table 
for RTT will be added in the same way. 



```cpp

unsigned long smc_rtt_create(unsigned long rtt_addr, //host provided address that can be used as rtt table!
                             unsigned long rd_addr,
                             unsigned long map_addr, //IPA address of guest that should be mapped in RTT
                             unsigned long ulevel)
{
	......
        parent_s2tte = s2tte_create_table(rtt_addr, level - 1L);
        s2tte_write(&parent_s2tt[wi.index], parent_s2tte);
```

Before updating the parent_s2tte, the generated new s2tt at level 3 is recreated
as a s2tt table. 

```cpp
#define S2TTE_L012_TABLE                0x3UL
#define S2TTE_TABLE     S2TTE_L012_TABLE
unsigned long s2tte_create_table(unsigned long pa, long level)
{        
        assert(level < RTT_PAGE_LEVEL);
        assert(GRANULE_ALIGNED(pa));
        
        return (pa | S2TTE_TABLE);
}          
```

Creating the table is not really changing anything related with the address or 
HIPAS and RIPAS, but it changes the s2tt descriptor type as S2TTE_TABLE. 


### Meaning of descriptors 
Based on how the descriptor is utilized in the translation, for example, whether
it is page, block or table, the last 2 bits of the descriptor should be properly
set. 


## HIPAS and RIPAS changed in the leaf entries
We've seen how the page descriptor is created as we build the s2tt for a realm. 
During this process, we've seen that each s2tt entry has RIPAS and HIPAS which 
were set as EMPTY and UNASSIGNED. Then when and which RMI request this HIPAS and
RIPAS change? And what is the actual meaning behind of HIPAS and RIPAS? 


### Change RIPAS from empty to RAM before realm start
When the HIPAS is unassigned, usually when the new page is inserted into a realm 
while its creation, the RIPAS can be initialized as RAM through RTT_INIT_RIPAS 
RMI call. Also, **RIPAS can only be set for the leaf rtte**. Therefore, if the 
target page is not the leaf, then it returns the current level and make the host
to generate RTT before changing the RIPAS of the leaf entry. Basically the KVM
implementation invokes RTT_INIT_RIPAS and RTT_CREATE RMI calls multiple times 
until the RIPAS can be successfully changed.


```cpp
                
unsigned long smc_rtt_init_ripas(unsigned long rd_addr,
                                 unsigned long map_addr,
                                 unsigned long ulevel)
 {  
 	......
        s2tte |= s2tte_create_ripas(RIPAS_RAM);
        s2tte_write(&s2tt[wi.index], s2tte);
        s2tte = s2tte_read(&s2tt[wi.index]);
                
        ripas_granule_measure(rd, map_addr, level);

        ret = RMI_SUCCESS;
	......
}

unsigned long s2tte_create_ripas(enum ripas ripas)
{        
        if (ripas == RIPAS_EMPTY) {
                return S2TTE_INVALID_RIPAS_EMPTY;
        }
        return S2TTE_INVALID_RIPAS_RAM;
}   
```


### Unassigned RAM page to Assigned RAM page (trusted IPA)
When the leaf s2tte is changed from empty to ram, now this page can be utilized
as trusted IPA after the data_create RMI. 


```cpp
static unsigned long data_create(unsigned long data_addr,
                                 unsigned long rd_addr, 
                                 unsigned long map_addr,
                                 struct granule *g_src,
                                 unsigned long flags)
{
	......
        ret = (g_src != NULL) ?
                validate_data_create(map_addr, rd) :
                validate_data_create_unknown(map_addr, rd);
	......
}

static unsigned long validate_data_create_unknown(unsigned long map_addr,
                                                  struct rd *rd)
{       
        if (!addr_in_par(rd, map_addr)) {
                return RMI_ERROR_INPUT;
        }
        
        if (!validate_map_addr(map_addr, RTT_PAGE_LEVEL, rd)) {
                return RMI_ERROR_INPUT;
        }
        
        return RMI_SUCCESS;
}       
        
static unsigned long validate_data_create(unsigned long map_addr,
                                          struct rd *rd)
{       
        if (get_rd_state_locked(rd) != REALM_STATE_NEW) {
                return RMI_ERROR_REALM;
        }                           
                                    
        return validate_data_create_unknown(map_addr, rd);
}               
```
RMI_DATA_CREATE and RMI_DATA_CREATE_UNKNOWN will invoke the same function on the
RMM side, but based on the realm status, proper RMI should be invoked. As shown
in the code, it checks current realm status, whether it is REALM_STATE_NEW for 
the case when the RMI_DATA_CREATE RMI is invoked (it can be distinguished 
whether the host provides the source address of the copy page or not). 
Therefore, after the realm is initialized, no code/data pages can be added from
the host with the content, and only the new pages without content can be added 
to the realm through the RMI_DATA_CREATE_UNKNOWN.

### Checking RIPAS and HIPAS
```cpp
static unsigned long data_create(unsigned long data_addr,                       
                                 unsigned long rd_addr,                         
                                 unsigned long map_addr,                        
                                 struct granule *g_src,                         
                                 unsigned long flags)                           
{                                                      
	......
        s2tt = granule_map(wi.g_llt, SLOT_RTT);
        s2tte = s2tte_read(&s2tt[wi.index]);
        if (!s2tte_is_unassigned(s2tte)) {
                ret = pack_return_code(RMI_ERROR_RTT, RTT_PAGE_LEVEL);
                goto out_unmap_ll_table;
        }

        ripas = s2tte_get_ripas(s2tte);
	......

	new_data_state = GRANULE_STATE_DATA;

        s2tte = (ripas == RIPAS_EMPTY) ?
                s2tte_create_assigned_empty(data_addr, RTT_PAGE_LEVEL) :
                s2tte_create_valid(data_addr, RTT_PAGE_LEVEL);

        s2tte_write(&s2tt[wi.index], s2tte);
        s2tte = s2tte_read(&s2tt[wi.index]);
        INFO(" -> s2tte:%lx\n", s2tte);

        __granule_get(wi.g_llt);

        ret = RMI_SUCCESS;
```

Regardless of which RMI was invoked, it checks HIPAS and RIPAS. As we've seen 
the leaf page table descriptors were set as empty and unassigned for RIPAS and
HIPAS, respectively. First it checks if the HIPAS of s2tte is unassigned. If it 
is not unassigned, then it should be treated as error. If it is unassigned, then 
it retrieves the RIPAS. Based on current RIPAS, different leaf entry with 
different RIPAS and HIPAS will be generated.

## S2TT is stage 2 table for realm
Before explaining the meaning of HIPAS and RIPAS, we should remind one thing. 
One very easy to forget when it comes to s2tt is that it is stage 2 page table 
that can control accesses of specific page. The reason we have separate stage 2
page tables, called s2tt, is because we cannot trust the kvm. However, still its
role in IPA->HPA translation including accessing permission check is same as for 
vanilla stage2 page table for aarch64.

Therefore, to allow the accesses inside the REALM, it sets up lower and upper
memory attribute of stage 2 page table descriptor properly. Note that HIPAS and 
RIPAS is part of the lower memory attributes of the leaf descriptor. Let's see 
what flags should be additional set for the leaf descriptor to allow its access.

```cpp
#define S2TTE_ATTRS     (S2TTE_MEMATTR_FWB_NORMAL_WB | S2TTE_AP_RW | \
                        S2TTE_SH_IS | S2TTE_AF)
#define S2TTE_BLOCK     (S2TTE_ATTRS | S2TTE_L012_BLOCK)
#define S2TTE_PAGE      (S2TTE_ATTRS | S2TTE_L3_PAGE)

unsigned long s2tte_create_valid(unsigned long pa, long level)
{               
        assert(level >= RTT_MIN_BLOCK_LEVEL);
        assert(addr_is_level_aligned(pa, level));
        if (level == RTT_PAGE_LEVEL) {
                return (pa | S2TTE_PAGE);
        }
        return (pa | S2TTE_BLOCK);
}
```

Let's assume that the target page is PAGE_LEVEL. In that case, the s2tte entry
is (pa | S2TTE_PAGE). S2TTE_PAGE consists of its attributes S2TTE_ATTRS allowing
the realm to access this page and the page descriptor type S2TTE_L3_PAGE. Let's
see the attributes one by one.

### Memory attributes (Desc[5:2])
```cpp
#define S2TTE_MEMATTR_FWB_NORMAL_WB     ((1UL << 4) | (2UL << 2)) 
```

Desc[5:2] is used to set-up memattr filed of the descriptor. Because FWB should 
be set for the realm, Desc[5] is reserved and Desc[4] determines interpretation
of Desc[3:2]. When Desc[4] is zero, the two bits in the Desc[3:2] determines 
memory attributes for the device. Because trusted IPA will not be used as device
memory, it should not be zero. That is the reason of (1UL << 4) to set Desc[4]. 
When Desc[4] == 1, memory attribute for normal write back is 10, and that is why
(2UL << 2).


### S2AP data access perimission flag Desc[6:7]
To allow read write access to the page, the S2AP flag should be properly set as 
0x3, which indicates Read Write access perimission.

```cpp
#define S2TTE_AP_SHIFT                  6
#define S2TTE_AP_MASK                   (3UL << S2TTE_AP_SHIFT)
#define S2TTE_AP_RW                     (3UL << S2TTE_AP_SHIFT)
```

Note that S2AP overlaps with the RIPAS.. Need more explanation here..


### Shearability Desc[9:8]
#define S2TTE_SH_SHIFT                  8
#define S2TTE_SH_MASK                   (3UL << S2TTE_SH_SHIFT)
#define S2TTE_SH_NS                     (0UL << S2TTE_SH_SHIFT)
#define S2TTE_SH_RESERVED               (1UL << S2TTE_SH_SHIFT)
#define S2TTE_SH_OS                     (2UL << S2TTE_SH_SHIFT)
#define S2TTE_SH_IS                     (3UL << S2TTE_SH_SHIFT) /* Inner Shareable */


### Access flag
#define S2TTE_AF                        (1UL << 10)


### Setting for Assigned but Empty IPA
```cpp
unsigned long s2tte_create_assigned_empty(unsigned long pa, long level)
{       
        assert(level >= RTT_MIN_BLOCK_LEVEL);
        assert(addr_is_level_aligned(pa, level)); 
        return (pa | S2TTE_INVALID_HIPAS_ASSIGNED | S2TTE_INVALID_RIPAS_EMPTY);
}  
```
Compared to valid s2tte, it doesn't set any attribute of the s2tte. Therefore, 
when the realm tries to access the target page, it raise the execution fault
even though there is a mapping from teh IPA to HPA because the flags set for 
that page preventing the MMU from accessing the page.


### Meaning of RIPAS
To represent RIPAS of the page, bit 6 of the leaf descriptor is utilized, which 
is the S2AP[0]. When the s2tte is set as valid, then S2AP[1:0] will be set as 
0x3 thanks to the flag S2TTE_AP_RW. 

However, if the ripas is changed, for example, from the RAM to the EMPTY through 
other RMI calls, how the s2tte will be changed accordingly? Because RIPAS is 
part of the S2AP, if the RIPAS is only changed from RAM to EMPTY, then the S2AP
will be 0x3 to 0x1, which means read/write access permission to write access 
permission. Does it just remove the RIPAS (Desc[6]) or entire S2AP which is 
Desc[7:6]? For this question please refer to [[]]

### Meaning of HIPAS
HIPAS is 4 bits of Desc[5:2] which completely overlaps the memattrs of the
stage2 page table descriptor. Currently, three different HIPAS are used by the 
realm so let's see how RIPAS can affect the memattr.

```cpp
#define S2TTE_INVALID_HIPAS_SHIFT       2
#define S2TTE_INVALID_HIPAS_WIDTH       4
#define S2TTE_INVALID_HIPAS_MASK        MASK(S2TTE_INVALID_HIPAS)

#define S2TTE_INVALID_HIPAS_UNASSIGNED  (INPLACE(S2TTE_INVALID_HIPAS, 0))
#define S2TTE_INVALID_HIPAS_ASSIGNED    (INPLACE(S2TTE_INVALID_HIPAS, 1))
#define S2TTE_INVALID_HIPAS_DESTROYED   (INPLACE(S2TTE_INVALID_HIPAS, 2))
```

As I mentioned previously, when the FWB is set, the last two bits in memattrs,
- When Desc[3:2] == 0x0 => Reserved
- When Desc[3:2] == 0x1 =>
- When Desc[3:2] == 0x2 =>




## Apendix
### What happens to S2TTE when RIPAS is changed? 
We've seen that RIPAS and HIPAS can be changed when the trusted IPA is generated
through the RMI_DATA_CREATE_UNKNOWN. There are several other RMI calls that need
to change the RIPAS and HIPAS. I will cover one of them, particularly 
RMI_RTT_SET_RIPAS which changes RIPAS of the realm page. 

```cpp
static bool update_ripas(unsigned long *s2tte, unsigned long level,
                         enum ripas ripas)
{
        if (s2tte_is_table(*s2tte, level)) {
                return false;
        }

        if (s2tte_is_valid(*s2tte, level)) {
                if (ripas == RIPAS_EMPTY) {
                        unsigned long pa = s2tte_pa(*s2tte, level);
                        *s2tte = s2tte_create_assigned_empty(pa, level);
                }
                return true;
        }
        
        if (s2tte_is_unassigned(*s2tte) || s2tte_is_assigned(*s2tte, level)) {
                *s2tte |= s2tte_create_ripas(ripas);
                return true;
        }

        return false;
}
```
There are two main cases I will cover for the RIPAS update.


```cpp
unsigned long s2tte_create_assigned_empty(unsigned long pa, long level)
{               
        assert(level >= RTT_MIN_BLOCK_LEVEL);
        assert(addr_is_level_aligned(pa, level));
        return (pa | S2TTE_INVALID_HIPAS_ASSIGNED | S2TTE_INVALID_RIPAS_EMPTY);
}  
```

When it needs to change RIPAS from the RAM to EMPTY, it should reset all other 
attributes together with the RIPAS, so it generate new S2TTE with the HPA. As 
shown in the code, it doesn't set any attributes for the assigned and empty 
page. What about the opposite case? Before thinking about this, we have to know
that smc_rtt_set_ripas is only allowed to be invoked when the realm explicitly
requested the changes. Therefore, I think there would not be the case where 
the realm accesses the assigned but empty page and ask ripas change to the host.
Not sure, I need to check!






```cpp
unsigned long smc_rtt_init_ripas(unsigned long rd_addr,
                                 unsigned long map_addr,
                                 unsigned long ulevel)
{       
	......
        /* Allowed only for HIPAS=UNASSIGNED */
        if (s2tte_is_table(s2tte, level) || !s2tte_is_unassigned(s2tte)) {
                ret = pack_return_code(RMI_ERROR_RTT, (unsigned int)level);
                goto out_unmap_llt;
        }

	s2tte |= s2tte_create_ripas(RIPAS_RAM);
                
        s2tte_write(&s2tt[wi.index], s2tte);
        s2tte = s2tte_read(&s2tt[wi.index]);

	ripas_granule_measure(rd, map_addr, level);

        ret = RMI_SUCCESS;

out_unmap_llt:
        buffer_unmap(s2tt);
out_unlock_llt:
        buffer_unmap(rd);
        granule_unlock(wi.g_llt);
        return ret;
}
```

Note that the rtt_init_ripas RMI is that it can only set the RIPAS 
as RAM for the **leaf page or block**. Note that when the s2tte is table, 
it returns error message. Also, the s2tte should be unassigned when the rmi is 
invoked. Let's see how the ripas, hipas, and the table descriptor type changes 
as the RTT pages are generated by the RMI. 

