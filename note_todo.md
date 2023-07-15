# RIPAS and HIPAS
## Realm IPA State (RIPAS)
The memory accessible from one REALM can be logically split into two different 
region based on security. A protected IPA space is the region that can locate 
REALM code and data, and the processor can execute instructions fetched from the 
protected IPA space. To maintain protected IPA space, REALM manages associated 
Realm IPA state (RIPAS) per page residing within the protected IPA space. 
Currently there is only two RIPAS: RAM and EMPTY. 

Realm data access to a Protected IPA whose RIPAS is EMPTY causes a Synchronous
External Abort taken to the Realm. How is it possible? The RIPAS value is stored 
in the RTT, so let's take a look at the RTT related functions and fields!

Also, to change the RIPAS, there are two different ways based on when it needs 
to be changed. The first case is before the REALM is activated and the other is 
after the activation. Before the activation, host can ask RMM to change RIPAS 
through the RMI call, but after the activation Realm can ask changed through RSI
call.






## Host IPA State (HIPAS)
HIPAS value can be different based on whether the page belongs to the protected 
IPA or not. When the page belongs to the protected IPA, then HIPAS can be any 
among three different values: UNASSIGNED (not associated with any granule),
ASSIGNED (associated with DATA granule), DESTROYED (cannot be used for rest of 
the lifetime of the Realm). If not, this HIPAS can be either UNASSIGNED_NS (not 
associated with any granule) or ASSIGNED_NS(Host-owned memory is mapped at this
address), which means unmapped or mapped but not secure. 

HIPAS value is also stored in the RTT as well as RIPAS

## RIPAS and S2 page table
```cpp
#define S2TTE_ATTRS     (S2TTE_MEMATTR_FWB_NORMAL_WB | S2TTE_AP_RW | \
                        S2TTE_SH_IS | S2TTE_AF)

#define S2TTE_BLOCK     (S2TTE_ATTRS | S2TTE_L012_BLOCK)
#define S2TTE_PAGE      (S2TTE_ATTRS | S2TTE_L3_PAGE)
#define S2TTE_BLOCK_NS  (S2TTE_NS | S2TTE_XN | S2TTE_AF | S2TTE_L012_BLOCK)
#define S2TTE_PAGE_NS   (S2TTE_NS | S2TTE_XN | S2TTE_AF | S2TTE_L3_PAGE)
```

Based on where the page belongs to, the different flags are set in the stage2 
page table entries. Different flags affect how MMU translate IPA to PA. For 
example, S2TTE_XN flag is set for the NS pages because the RMM should not be 
able to fetch the instruction from the NS pages. RMM understands which page 
should belong to where based on different RMI calls invoked from the host. 

```cpp
#define S2TTE_INVALID_HIPAS_SHIFT       2
#define S2TTE_INVALID_HIPAS_WIDTH       4
#define S2TTE_INVALID_HIPAS_MASK        MASK(S2TTE_INVALID_HIPAS)

#define S2TTE_INVALID_HIPAS_UNASSIGNED  (INPLACE(S2TTE_INVALID_HIPAS, 0))
#define S2TTE_INVALID_HIPAS_ASSIGNED    (INPLACE(S2TTE_INVALID_HIPAS, 1))
#define S2TTE_INVALID_HIPAS_DESTROYED   (INPLACE(S2TTE_INVALID_HIPAS, 2))
/*      
 * Returns true if @s2tte has HIPAS=@hipas.
 */
static bool s2tte_has_hipas(unsigned long s2tte, unsigned long hipas)
{       
        unsigned long desc_type = s2tte & DESC_TYPE_MASK;
        unsigned long invalid_desc_hipas = s2tte & S2TTE_INVALID_HIPAS_MASK;
                                
        if ((desc_type != S2TTE_Lx_INVALID) || (invalid_desc_hipas != hipas)) {
                return false;
        }       
        return true;
}
        
/*      
 * Returns true if @s2tte has HIPAS=UNASSIGNED or HIPAS=INVALID_NS.
 */     
bool s2tte_is_unassigned(unsigned long s2tte)
{               
        return s2tte_has_hipas(s2tte, S2TTE_INVALID_HIPAS_UNASSIGNED);
}       

/*      
 * Returns true if @s2tte has HIPAS=DESTROYED.
 */     
bool s2tte_is_destroyed(unsigned long s2tte)
{       
        return s2tte_has_hipas(s2tte, S2TTE_INVALID_HIPAS_DESTROYED);
}       

/*
 * Returns true if @s2tte has HIPAS=ASSIGNED.
 */
bool s2tte_is_assigned(unsigned long s2tte, long level)
{       
        (void)level;

        return s2tte_has_hipas(s2tte, S2TTE_INVALID_HIPAS_ASSIGNED);
}
```

Also, bits [2,6) are used to encode the HIPAS of the page using the S2TTE. 


## Map Realm IPA
### Set-up RIPAS 
After relocating the page to the realm, its RIPAS should be set to be used as 
a secure code/data page located inside the secure IPA range. To setup the RIPAS, 
stage2 page table entry associated with the target IPA page is used. Note that 
the last 1 bit of the stage2 page entry indicates if the page mapped by that 
s2tte is used (as code/data page) or not used. 

```cpp
 /*
 * The RmiRipas enumeration representing realm IPA state.
 *      
 * Map RmmRipas to RmiRipas to simplify code/decode operations.
 */     
enum ripas {
        RIPAS_EMPTY = RMI_EMPTY,        /* Unused IPA for Realm */
        RIPAS_RAM = RMI_RAM             /* IPA used for Code/Data by Realm */
};              

unsigned long smc_rtt_init_ripas(unsigned long rd_addr,
                                 unsigned long map_addr,
                                 unsigned long ulevel)
{
        struct granule *g_rd, *g_rtt_root;
        struct rd *rd;
        unsigned long ipa_bits;
        struct rtt_walk wi;
        unsigned long s2tte, *s2tt;
        unsigned long ret;
        long level = (long)ulevel;
        int sl;

        g_rd = find_lock_granule(rd_addr, GRANULE_STATE_RD);
        if (g_rd == NULL) {
                return RMI_ERROR_INPUT;
        }

        rd = granule_map(g_rd, SLOT_RD);

        if (get_rd_state_locked(rd) != REALM_STATE_NEW) {
                buffer_unmap(rd);
                granule_unlock(g_rd);
                return RMI_ERROR_REALM;
        }

        if (!validate_rtt_entry_cmds(map_addr, level, rd)) {
                buffer_unmap(rd);
                granule_unlock(g_rd);
                return RMI_ERROR_INPUT;
        }

        if (!addr_in_par(rd, map_addr)) {
                buffer_unmap(rd);
                granule_unlock(g_rd);
                return RMI_ERROR_INPUT;
        }

        g_rtt_root = rd->s2_ctx.g_rtt;
        sl = realm_rtt_starting_level(rd);
        ipa_bits = realm_ipa_bits(rd);

        granule_lock(g_rtt_root, GRANULE_STATE_RTT);
        granule_unlock(g_rd);

        rtt_walk_lock_unlock(g_rtt_root, sl, ipa_bits,
                                map_addr, level, &wi);
        if (wi.last_level != level) {
                ret = pack_return_code(RMI_ERROR_RTT, wi.last_level);
                goto out_unlock_llt;
        }

        s2tt = granule_map(wi.g_llt, SLOT_RTT);
        s2tte = s2tte_read(&s2tt[wi.index]);

        /* Allowed only for HIPAS=UNASSIGNED */
        if (s2tte_is_table(s2tte, level) || !s2tte_is_unassigned(s2tte)) {
                ret = pack_return_code(RMI_ERROR_RTT, (unsigned int)level);
                goto out_unmap_llt;
        }

        s2tte |= s2tte_create_ripas(RIPAS_RAM);

        s2tte_write(&s2tt[wi.index], s2tte);

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

Note that the map_addr is the address in IPA. Also note that it doesn't require
any physical address that is mapped to the IPA through the stage 2 page table. 
It means that this function just initialize the st2tte entry used for mapping 
the passed IPA to any physical address. 

### Generate data (associate the IPA to HPA)
Initializing RIPAS doesn't mean that the stage 2 page tables are all set to 
translate the IPA to specific HPA. Therefore, to allow the Realm to access the 
actual memory as a result of stage2 page table walking of the MMU, the PA should
be set by another RMI. In addition to generating the mapping, the HIPAS can be 
changed as a result of DATA_CREATE RMI. 

```cpp
unsigned long smc_data_create(unsigned long data_addr, //HPA
                              unsigned long rd_addr,
                              unsigned long map_addr, //IPA
                              unsigned long src_addr, //NULL if there is no copy needed
                              unsigned long flags)
{
        struct granule *g_src;
        unsigned long ret;

        if (flags != RMI_NO_MEASURE_CONTENT && flags != RMI_MEASURE_CONTENT) {
                return RMI_ERROR_INPUT;
        }

        g_src = find_granule(src_addr);
        if ((g_src == NULL) || (g_src->state != GRANULE_STATE_NS)) {
                return RMI_ERROR_INPUT;
        }

        ret = data_create(data_addr, rd_addr, map_addr, g_src, flags);

        return ret;
}
```

Note that the data_addr is the physical address that will be mapped to the IPA,
map_addr. If host wants to copy the code/data from the NS memory to the target 
page belong to Realm PAS, it passes the src_addr parameter to let RMM know the 
address of the page where its content should be copied from. There is another 
RMI, SMC_RMM_DATA_CREATE_UNKNOWN, which doesn't involve any copy but establish 
the IPA to PA mapping. 


```cpp
        s2tt = granule_map(wi.g_llt, SLOT_RTT);
        s2tte = s2tte_read(&s2tt[wi.index]);
        if (!s2tte_is_unassigned(s2tte)) {
                ret = pack_return_code(RMI_ERROR_RTT, RTT_PAGE_LEVEL);
                goto out_unmap_ll_table;
        }    

        ripas = s2tte_get_ripas(s2tte);
```
It walks the stage2 page table and locate the S2TTE translating the passed IPA 
to the HPA. First it will check HIPAS of the S2TTE entry to confirm it is not
ASSIGNED yet. Also, it retrieves RIPAS of the S2TTE.


```cpp
        s2tte = (ripas == RIPAS_EMPTY) ?
                s2tte_create_assigned_empty(data_addr, RTT_PAGE_LEVEL) :
                s2tte_create_valid(data_addr, RTT_PAGE_LEVEL);
```

Based on the current RIPAS of the S2TTE, it will call different functions to 
update S2TTE. Note that both functions require data_addr which is the HPA addr
that should be mapped to the IPA through the current S2TTE. Because the RIPAS 
can be set as part of the S2TTE bits, these two functions will configure S2TTE
with the proper address and different RIPAS.

```cpp
/*                      
 * Creates an invalid s2tte with output address @pa, HIPAS=ASSIGNED and
 * RIPAS=EMPTY, at level @level.
 */             
unsigned long s2tte_create_assigned_empty(unsigned long pa, long level)
{
        assert(level >= RTT_MIN_BLOCK_LEVEL);
        assert(addr_is_level_aligned(pa, level));
        return (pa | S2TTE_INVALID_HIPAS_ASSIGNED | S2TTE_INVALID_RIPAS_EMPTY);
}       
```

When the current RIPAS is RIPAS_EMPTY, then it means that smc_rtt_init_ripas 
function was not invoked before the DATA_CREATE RMI call for this S2TTE. 
Therefore, although the HIPAS will be changed from the UNASSIGNED to ASSIGNED
as a result of DATA_CREATE, it will not be a valid mapping to be used inside 
the REALM. 


```
/*      
 * Creates a page or block s2tte for a Protected IPA, with output address @pa.
 */     
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

In contrast with the previous case, if the smc_rtt_init_ripas has been called 
for the selected S2TTE before, by changing the HIPAS from UNASSIGNED to ASSIGNED
the addresses mapped through this S2TTE can be valid and safe to be used inside
the REALM.  

```cpp
#define S2TTE_ATTRS     (S2TTE_MEMATTR_FWB_NORMAL_WB | S2TTE_AP_RW | \
                        S2TTE_SH_IS | S2TTE_AF)

#define S2TTE_BLOCK     (S2TTE_ATTRS | S2TTE_L012_BLOCK)
#define S2TTE_PAGE      (S2TTE_ATTRS | S2TTE_L3_PAGE)
```

To allow the accesses inside the REALM, it sets up lower memory attribute of the 
S2TTE by set up relevant flags (e.g., Access Permission (AP), Access Flag (AF)).

## Map NS-IPA
Non-secure pages are mapped through the stage 2 page table secured by the RMM.
However, instead of building entire page entry mapped to the NS memory from the 
scratch, host passes the generated page to the RMM and RMM checks and sets the 
required field for security and convenience. 
```cpp
/*
 * Validate the portion of NS S2TTE that is provided by the host.
 */
bool host_ns_s2tte_is_valid(unsigned long s2tte, long level)
{       
        unsigned long mask = addr_level_mask(~0UL, level) |
                             S2TTE_MEMATTR_MASK |
                             S2TTE_AP_MASK |
                             S2TTE_SH_MASK;
        
        /*
         * Test that all fields that are not controlled by the host are zero
         * and that the output address is correctly aligned. Note that
         * the host is permitted to map any physical address outside PAR.
         */
        if ((s2tte & ~mask) != 0UL) {
                return false;
        }
        
        /*
         * Only one value masked by S2TTE_MEMATTR_MASK is invalid/reserved.
         */
        if ((s2tte & S2TTE_MEMATTR_MASK) == S2TTE_MEMATTR_FWB_RESERVED) {
                return false;
        }
        
        /*
         * Only one value masked by S2TTE_SH_MASK is invalid/reserved.
         */
        if ((s2tte & S2TTE_SH_MASK) == S2TTE_SH_RESERVED) {
                return false;
        }
        
        /*
         * Note that all the values that are masked by S2TTE_AP_MASK are valid.
         */
        return true;
}

```

```cpp
/*
 * We don't hold a reference on the NS granule when it is
 * mapped into a realm. Instead we rely on the guarantees
 * provided by the architecture to ensure that a NS access
 * to a protected granule is prohibited even within the realm.
 */
static unsigned long map_unmap_ns(unsigned long rd_addr,
                                  unsigned long map_addr,
                                  long level,
                                  unsigned long host_s2tte,
                                  enum map_unmap_ns_op op)

```

As shown in the code, RMI for mapping the NS memory doesn't require a physical
page address because host_s2tte already provides the address and additional 
attributes required to map the page from the RMM.

```cpp
/*
 * Creates a page or block s2tte for an Unprotected IPA at level @level.
 *
 * The following S2 TTE fields are provided through @s2tte argument:
 * - The physical address
 * - MemAttr
 * - S2AP
 * - Shareability
 */
unsigned long s2tte_create_valid_ns(unsigned long s2tte, long level)
{
        assert(level >= RTT_MIN_BLOCK_LEVEL);
        if (level == RTT_PAGE_LEVEL) {
                return (s2tte | S2TTE_PAGE_NS);
        }
        return (s2tte | S2TTE_BLOCK_NS);
}
```






smc_rtt_init_ripas: when the HIPAS is unassigned, usually when the new page is inserted into the REALM while its creation, 
the RIPAS can be initialized as RAM through this RMI call. Also, RIPAS can only be set for the last RTTE. Therefore,
if the target page is not the leaf, then it returns the current level and make the host to generate RTT before changing 
the RIPAS of the entry. Basically the kernel implementation invokes RTT_INIT_RIPAS and RTT_CREATE RMI calls multiple 
times until the RIPAS can be successfully changed. 


set_memory_encrypted is the RSI call invoked from the realm kernel. It sets the specific memory area from EMPTY RIPAS to 
MEMORY RIPAS. 
