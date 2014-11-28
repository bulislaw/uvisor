#include <uvisor.h>
#include "vmpu.h"

#ifndef MPU_MAX_PRIVATE_FUNCTIONS
#define MPU_MAX_PRIVATE_FUNCTIONS 16
#endif/*MPU_MAX_PRIVATE_FUNCTIONS*/

#if (MPU_MAX_PRIVATE_FUNCTIONS>0x100UL)
#error "MPU_MAX_PRIVATE_FUNCTIONS needs to be lower/equal to 0x100"
#endif

#define MPU_FAULT_USAGE  0x00
#define MPU_FAULT_MEMORY 0x01
#define MPU_FAULT_BUS    0x02
#define MPU_FAULT_HARD   0x03
#define MPU_FAULT_DEBUG  0x04

/* function table hashing support functions */
typedef struct {
    uint32_t addr;
    uint8_t count;
    uint8_t hash;
    uint8_t box_id;
    uint8_t flags;
} TFnTable;

static TFnTable g_fn[MPU_MAX_PRIVATE_FUNCTIONS];
static uint8_t g_fn_hash[0x100];
static int g_fn_count, g_fn_box_count;

static void vmpu_fault(int reason)
{
    uint32_t sperr,t;

    /* print slave port details */
    dprintf("CESR : 0x%08X\n\r", MPU->CESR);
    sperr = (MPU->CESR >> 27);
    for(t=0; t<5; t++)
    {
        if(sperr & 0x10)
            dprintf("  SLAVE_PORT[%i]: @0x%08X (Detail 0x%08X)\n\r",
                t,
                MPU->SP[t].EAR,
                MPU->SP[t].EDR);
        sperr <<= 1;
    }
    dprintf("CFSR : 0x%08X (reason 0x%02x)\n\r", SCB->CFSR, reason);
    while(1);
}

static void vmpu_fault_bus(void)
{
    dprintf("BFAR : 0x%08X\n\r", SCB->BFAR);
    vmpu_fault(MPU_FAULT_BUS);
}

static void vmpu_fault_usage(void)
{
    dprintf("Usage Fault\n\r");
    vmpu_fault(MPU_FAULT_USAGE);
}

static void vmpu_fault_hard(void)
{
    dprintf("HFSR : 0x%08X\n\r", SCB->HFSR);
    vmpu_fault(MPU_FAULT_HARD);
}

static void vmpu_fault_debug(void)
{
    dprintf("MPU_FAULT_DEBUG\n\r");
    vmpu_fault(MPU_FAULT_DEBUG);
}

int vmpu_acl_dev(TACL acl, uint16_t device_id)
{
    return 1;
}

int vmpu_acl_mem(TACL acl, uint32_t addr, uint32_t size)
{
    return 1;
}

int vmpu_acl_reg(TACL acl, uint32_t addr, uint32_t rmask, uint32_t wmask)
{
    return 1;
}

int vmpu_acl_bit(TACL acl, uint32_t addr)
{
    return 1;
}

static uint8_t vmpu_hash_addr(uint32_t data)
{
    return
        (data >> 24) ^
        (data >> 16) ^
        (data >>  8) ^
        data;
}

static int vmpu_box_add_fn(uint8_t box_id, const void **fn, uint32_t count)
{
    TFnTable tmp, *p;
    uint8_t sorting, h, hprev;
    uint32_t addr;
    int i, j;

    /* ensure atomic operation */
    __disable_irq();

    /* add new functions */
    p = &g_fn[g_fn_count];
    for(i=0; i<count; i++)
    {
        /* bail out on table overflow */
        if(g_fn_count>=MPU_MAX_PRIVATE_FUNCTIONS)
        {
            __enable_irq();
            return -1;
        }

        addr = (uint32_t)*fn++;
        p->addr = addr;
        p->hash = vmpu_hash_addr(addr);
        p->box_id = box_id;

        g_fn_count++;
        p++;
    }

    /* silly-sort address hash table & correspondimg addresses */
    do {
        sorting = FALSE;

        for(i=1; i<g_fn_count; i++)
        {
            if(g_fn[i].hash<g_fn[i-1].hash)
            {
                /* swap corresponding adresses */
                tmp = g_fn[i];
                g_fn[i] = g_fn[i-1];
                g_fn[i-1] = tmp;

                sorting = TRUE;
            }
        }
    } while (sorting);

#ifndef NDEBUG
        dprintf("added %i functions for box_id=%i:\n", count, box_id);
#endif/*NDEBUG*/

    /* update g_fn_table_hash and function counts */
    p = g_fn;
    j = 0;
    hprev = g_fn[0].hash;
    for(i=1; i<g_fn_count; i++)
    {
        h = p->hash;
        if(h==hprev)
            p->count = 0;
        else
        {
            /* point hash-table entry to first address in list */
            g_fn_hash[hprev] = j;
            /* remember function count at entry point */
            g_fn[j].count = i-j;

            /* set new entry */
            j = i;
            /* update hash */
            hprev = h;
        }

#ifndef NDEBUG
        dprintf("\tfn_addr:0x08X, box:0x%02X, fn_hash=0x%02X, fn_count=0x%02X\n",
            p->addr, p->box_id, p->hash, p->count);
#endif/*NDEBUG*/

        /* advance to next entry */
        p++;
    }

    /* ensure atomic operation */
    __enable_irq();

    return g_fn_count;
}

int vmpu_box_add(const TBoxDesc *box)
{
    int res;

    /* incrememt box_id */
    g_fn_box_count++;

    if(g_fn_box_count>=0x100UL)

    /* add function pointers to global function table */
    if((box->fn_count) &&
        ((res = vmpu_box_add_fn(g_fn_box_count, box->fn_list, box->fn_count))<0))
            return res;

    return 0;
}

int vmpu_fn_box(uint32_t addr)
{
    TFnTable *p;
    uint8_t hash, fn_index, count;

    /* calculate address hash */
    hash = vmpu_hash_addr(addr);

    /* early bail on invalid addresses */
    if((fn_index = g_fn_hash[hash])==0)
        return -1;

    p = &g_fn[fn_index];
    count = p->count;
    do {
        if(p->addr == addr)
            return p->box_id;
        count--;
    } while(count>0);

    return 0;
}

void vmpu_init(void)
{
    /* setup security "bluescreen" exceptions */
    ISR_SET(BusFault_IRQn,         &vmpu_fault_bus);
    ISR_SET(UsageFault_IRQn,       &vmpu_fault_usage);
    ISR_SET(HardFault_IRQn,        &vmpu_fault_hard);
    ISR_SET(DebugMonitor_IRQn,     &vmpu_fault_debug);

    /* enable mem, bus and usage faults */
    SCB->SHCSR |= 0x70000;
}
