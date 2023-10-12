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


### Protected IPA and UNPROTECTED IPA
```cpp
#define S2TTE_MEMATTR_FWB_NORMAL_WB     ((1UL << 4) | (2UL << 2))
#define S2TTE_AF                        (1UL << 10)
#define S2TTE_XN                        (2UL << 53)
#define S2TTE_NS                        (1UL << 55)
```

Note that all other fields except S2TTE_NS bit (which is 55th bit) of the s2tte 
does not exist in the spec. This bit doesn't affect the hardware interpretation. 
This bit is utilized to distinguish UNPROTECTED pages which are not delegated to
the Realm from the PROTECTED pages. 



```cpp
static bool s2tte_check(unsigned long s2tte, long level, unsigned long ns)
{
        unsigned long desc_type;

        if ((s2tte & S2TTE_NS) != ns) {
                return false;
        }

        desc_type = s2tte & DESC_TYPE_MASK;

        /* Only pages at L3 and valid blocks at L2 allowed */
        if (((level == RTT_PAGE_LEVEL) && (desc_type == S2TTE_L3_PAGE)) ||
            ((level == RTT_MIN_BLOCK_LEVEL) && (desc_type == S2TTE_L012_BLOCK))) {
                return true;
        }

        return false;
}

/*
 * Returns true if @s2tte is a page or block s2tte, and NS=0.
 */
bool s2tte_is_valid(unsigned long s2tte, long level)
{
        return s2tte_check(s2tte, level, 0UL);
}

/*
 * Returns true if @s2tte is a page or block s2tte, and NS=1.
 */
bool s2tte_is_valid_ns(unsigned long s2tte, long level)
{
        return s2tte_check(s2tte, level, S2TTE_NS);
}
```

## Map Realm IPA
### Set-up RIPAS 
After relocating the page to the realm, its RIPAS should be initialized to be 
used as a secure code/data page located inside the secure IPA range. To setup 
the RIPAS, stage2 page table entry associated with the target IPA page is used. 
Note that the last 1 bit of the stage2 page entry indicates if the page mapped 
by that s2tte is used (as code/data page) or not used. 

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

### Generate data (associate the IPA to HPA **for trusted pages**)
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
the IPA to PA mapping. Therefore it does require the granule for the copy page.
Let's see how data_create function works!



```cpp
static unsigned long data_create(unsigned long data_addr,
                                 unsigned long rd_addr,
                                 unsigned long map_addr,
                                 struct granule *g_src,
                                 unsigned long flags)
{       
        struct granule *g_data;
        struct granule *g_rd;
        struct granule *g_table_root;
        struct rd *rd;
        struct rtt_walk wi;
        unsigned long s2tte, *s2tt;
        enum ripas ripas;
        enum granule_state new_data_state = GRANULE_STATE_DELEGATED;
        unsigned long ipa_bits;
        unsigned long ret;
        int __unused meas_ret;
        int sl;
        
        if (!find_lock_two_granules(data_addr,
                                    GRANULE_STATE_DELEGATED,
                                    &g_data,
                                    rd_addr,
                                    GRANULE_STATE_RD,
                                    &g_rd)) {
                return RMI_ERROR_INPUT;
        }
        
        rd = granule_map(g_rd, SLOT_RD);
        
        ret = (g_src != NULL) ?
                validate_data_create(map_addr, rd) :
                validate_data_create_unknown(map_addr, rd);
        
        if (ret != RMI_SUCCESS) {
                goto out_unmap_rd;
        }
        
        g_table_root = rd->s2_ctx.g_rtt;
        sl = realm_rtt_starting_level(rd);
        ipa_bits = realm_ipa_bits(rd);
        granule_lock(g_table_root, GRANULE_STATE_RTT);
        rtt_walk_lock_unlock(g_table_root, sl, ipa_bits,
                             map_addr, RTT_PAGE_LEVEL, &wi);
        if (wi.last_level != RTT_PAGE_LEVEL) {
                ret = pack_return_code(RMI_ERROR_RTT, wi.last_level);
                goto out_unlock_ll_table;
        }

        s2tt = granule_map(wi.g_llt, SLOT_RTT);
        s2tte = s2tte_read(&s2tt[wi.index]);
        if (!s2tte_is_unassigned(s2tte)) {
                ret = pack_return_code(RMI_ERROR_RTT, RTT_PAGE_LEVEL);
                goto out_unmap_ll_table;
        }    

        ripas = s2tte_get_ripas(s2tte);
	......
```
First it need to walk the stage2 page table and locate the S2TTE translating the
passed IPA to the HPA. This is done by rtt_walk_lock_unlock function. For the 
details of internal page walking for s2tte, refer to [[]]. The purpose of the 
walk is to retrieve the parent rtt entry of the target rtt so that it can 
validate if the mapping operation is permitted and establish the mapping. To
confirm the mapping has not been established, it checks HIPAS of the s2tte. 

```cpp
static unsigned long data_create(unsigned long data_addr,
                                 unsigned long rd_addr,
                                 unsigned long map_addr,
                                 struct granule *g_src,
                                 unsigned long flags)
	......
        if (g_src != NULL) {
                bool ns_access_ok;
                void *data = granule_map(g_data, SLOT_DELEGATED);

                ns_access_ok = ns_buffer_read(SLOT_NS, g_src, 0U,
                                              GRANULE_SIZE, data);

                if (!ns_access_ok) {
                        /*
                         * Some data may be copied before the failure. Zero
                         * g_data granule as it will remain in delegated state.
                         */
                        (void)memset(data, 0, GRANULE_SIZE);
                        buffer_unmap(data);
                        ret = RMI_ERROR_INPUT;
                        goto out_unmap_ll_table;
                }
                
                
                data_granule_measure(rd, data, map_addr, flags);
        
                buffer_unmap(data);
        }
```
If the RMI request was the SMC_RMM_DATA_CREATE, then it should copy the data 
from the host pages to the destination page and measure it for attestation. 
Whether we need the copy operation or not, we need the last level entry of the 
RTT to establish the mapping. Let's see how the last page for the mapping is 
generated. 

```cpp
static unsigned long data_create(unsigned long data_addr,
                                 unsigned long rd_addr,
                                 unsigned long map_addr,
                                 struct granule *g_src,
                                 unsigned long flags)
	......
        s2tte = (ripas == RIPAS_EMPTY) ?
                s2tte_create_assigned_empty(data_addr, RTT_PAGE_LEVEL) :
                s2tte_create_valid(data_addr, RTT_PAGE_LEVEL);
```

Note that the s2tte points to the leaf page of the s2tt connecting the realm
ipa to host provided delegated page. Based on the current RIPAS of the S2TTE, it
will call different functions to update last page. Note that both functions 
require data_addr which is the HPA addr that should be mapped to the IPA through 
the S2TTE. The major difference of two different s2tte is HIPAS and RIPAS.
Note that the RIPAS can be set as part of the S2TTE bits.

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

When the current RIPAS is RIPAS_EMPTY, then it means that **smc_rtt_init_ripas 
function was not invoked** before the DATA_CREATE RMI call for this S2TTE. 
Therefore, although the HIPAS will be changed from the UNASSIGNED to ASSIGNED
as a result of DATA_CREATE, it will not be a valid mapping to be used inside 
the REALM. 

```cpp
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
Therefore, when the realm tries to access the page mapped through the 
s2tte_create_assigned_empty, then it raise the execution fault because the flags
set for that page preventing the MMU from accessing the page. 

```cpp
static unsigned long data_create(unsigned long data_addr,
                                 unsigned long rd_addr,
                                 unsigned long map_addr,
                                 struct granule *g_src,
                                 unsigned long flags)
	......
	s2tte_write(&s2tt[wi.index], s2tte);

        __granule_get(wi.g_llt);

        ret = RMI_SUCCESS;
                
out_unmap_ll_table:
        buffer_unmap(s2tt);
out_unlock_ll_table:
        granule_unlock(wi.g_llt); 
out_unmap_rd:
        buffer_unmap(rd);
        granule_unlock(g_rd);
        granule_unlock_transition(g_data, new_data_state);
        return ret;
}               
```
After generating the s2tte, just updating the parent s2tt establish the proper
mapping!



## Map Untrusted IPA
Non-secure pages are mapped through the stage 2 page table secured by the RMM.
However, instead of building entire page entry mapped to the NS memory from the 
scratch, **host passes the generated page to the RMM** and RMM validates the 
provided s2tte and patch security critical field to provide some security 
guarantees. 


```cpp
unsigned long smc_rtt_map_unprotected(unsigned long rd_addr,
                                      unsigned long map_addr,
                                      unsigned long ulevel,
                                      unsigned long s2tte)
{
        long level = (long)ulevel;

        if (!host_ns_s2tte_is_valid(s2tte, level)) {
                return RMI_ERROR_INPUT;
        }

        return map_unmap_ns(rd_addr, map_addr, level, s2tte, MAP_NS);
}

unsigned long smc_rtt_unmap_unprotected(unsigned long rd_addr,
                                        unsigned long map_addr,
                                        unsigned long ulevel)
{
        return map_unmap_ns(rd_addr, map_addr, (long)ulevel, 0UL, UNMAP_NS);
}
```

As shown in the above two RMI handling functions, both utilize the map_unmap_ns
function to map or unmap untrusted pages in the s2tt. 


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
	......
        rtt_walk_lock_unlock(g_table_root, sl, ipa_bits,
                                map_addr, level, &wi);
        if (wi.last_level != level) {
                ret = pack_return_code(RMI_ERROR_RTT, wi.last_level);
                goto out_unlock_llt;
        }

        s2tt = granule_map(wi.g_llt, SLOT_RTT);
        s2tte = s2tte_read(&s2tt[wi.index]);
        if (op == MAP_NS) {
                if (!s2tte_is_unassigned(s2tte)) {
                        ret = pack_return_code(RMI_ERROR_RTT,
                                                (unsigned int)level);
                        goto out_unmap_table;
                }

                s2tte = s2tte_create_valid_ns(host_s2tte, level);
                s2tte_write(&s2tt[wi.index], s2tte);
                __granule_get(wi.g_llt);

        } else if (op == UNMAP_NS) {
                /*
                 * The following check also verifies that map_addr is outside
                 * PAR, as valid_NS s2tte may only cover outside PAR IPA range.
                 */
                if (!s2tte_is_valid_ns(s2tte, level)) {
                        ret = pack_return_code(RMI_ERROR_RTT,
                                                (unsigned int)level);
                        goto out_unmap_table;
                }

                s2tte = s2tte_create_invalid_ns();
                s2tte_write(&s2tt[wi.index], s2tte);
                __granule_put(wi.g_llt);
                if (level == RTT_PAGE_LEVEL) {
                        invalidate_page(&s2_ctx, map_addr);
                } else {
                        invalidate_block(&s2_ctx, map_addr);
                }
        }
```

As shown in the code, RMI for mapping the NS memory doesn't require a physical
page address because host_s2tte already provides the address and additional 
attributes required to map IPA. When the target s2tte RIPAS is set as unassigned,
it can create valid s2tte for untrusted mapping.

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

/*
 * We set HCR_EL2.FWB So we set bit[4] to 1 and bits[3:2] to 2 and force
 * everyting to be Normal Write-Back
 */
#define S2TTE_MEMATTR_FWB_NORMAL_WB     ((1UL << 4) | (2UL << 2))
#define S2TTE_AF                        (1UL << 10)
#define S2TTE_XN                        (2UL << 53)
#define S2TTE_NS                        (1UL << 55)

#define S2TTE_ATTRS     (S2TTE_MEMATTR_FWB_NORMAL_WB | S2TTE_AP_RW | \
                        S2TTE_SH_IS | S2TTE_AF)

#define S2TTE_TABLE     S2TTE_L012_TABLE
#define S2TTE_BLOCK     (S2TTE_ATTRS | S2TTE_L012_BLOCK)
#define S2TTE_PAGE      (S2TTE_ATTRS | S2TTE_L3_PAGE)
#define S2TTE_BLOCK_NS  (S2TTE_NS | S2TTE_XN | S2TTE_AF | S2TTE_L012_BLOCK)
#define S2TTE_PAGE_NS   (S2TTE_NS | S2TTE_XN | S2TTE_AF | S2TTE_L3_PAGE)
#define S2TTE_INVALID   0

```







set_memory_encrypted is the RSI call invoked from the realm kernel. It sets the specific memory area from EMPTY RIPAS to 
MEMORY RIPAS. 



There is no PORTAL pas but we need to filter the PORTAL Region in the RMM.

ALL portal region should be filtered by the bloom filter. so we utilize HIPAS RIPAS for the memory 
The thing is actually PORTAL region is set as NW PAS and REALM_PAS in the R-GPT and NR-GPT respectively,
but we maintain those regions are portal pas. and check the PMI PSI to filter those addresses 

PORTAL_ASSIGNED -> Delegated and utilized as memory pages for SMMU -> these addresses should be bloom filtered! 
PORTAL_EMPTY -> Delegated as PORTAL but not utilized by any entities yet -> RMM should check SW pas for PMI PSI. 
//when kernel utilize this function?


