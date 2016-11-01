/*
 * Copyright (c) 2013-2016, ARM Limited, All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <uvisor.h>
#include "box_init.h"
#include "debug.h"
#include "halt.h"
#include "svc.h"
#include "unvic.h"
#include "vmpu.h"
#include "page_allocator.h"

/* these symbols are linked in this scope from the ASM code in __svc_irq and
 * are needed for sanity checks */
UVISOR_EXTERN const uint32_t jump_table_unpriv;
UVISOR_EXTERN const uint32_t jump_table_unpriv_end;
UVISOR_EXTERN const uint32_t jump_table_priv;
UVISOR_EXTERN const uint32_t jump_table_priv_end;

/* default function for not implemented handlers */
void __svc_not_implemented(void)
{
    HALT_ERROR(NOT_IMPLEMENTED, "function not implemented\n\r");
}

/* SVC handlers */
const void * const g_svc_vtor_tbl[] = {
    __svc_not_implemented,      //  0
    unvic_isr_set,              //  1
    unvic_isr_get,              //  2
    unvic_irq_enable,           //  3
    unvic_irq_disable,          //  4
    unvic_irq_pending_clr,      //  5
    unvic_irq_pending_set,      //  6
    unvic_irq_pending_get,      //  7
    unvic_irq_priority_set,     //  8
    unvic_irq_priority_get,     //  9
    __svc_not_implemented,      // 10 Deprecated: benchmark_configure
    __svc_not_implemented,      // 11 Deprecated: benchmark_start
    __svc_not_implemented,      // 12 Deprecated: benchmark_stop
    halt_user_error,            // 13
    unvic_irq_level_get,        // 14
    __svc_not_implemented,      // 15 Deprecated: vmpu_box_id_self
    __svc_not_implemented,      // 16 Deprecated: vmpu_box_id_caller
    vmpu_box_namespace_from_id, // 17
    debug_reboot,               // 18
    /* FIXME: This function will be made automatic when the debug box ACL is
     *        introduced. The initialization will happen at uVisor boot time. */
    debug_register_driver,      // 19
    unvic_irq_disable_all,      // 20
    unvic_irq_enable_all,       // 21
    page_allocator_malloc,      // 22
    page_allocator_free,        // 23
};

/*******************************************************************************
 *
 * Function name:   __svc_irq
 * Brief:           SVC handlers multiplexer
 *
 * This function is the main SVC IRQ handler. Execution is multiplexed to the
 * proper handler based on the SVC opcode immediate.There are 2 kinds of SVC
 * handler:
 *
 *     1. Regular (unprivileged or privileged)
 * Regular SVC handlers are likely to be mapped to user APIs for unprivileged
 * code. They allow a maximum of 4 32bit arguments and return a single 32bit
 * argument.
 *
 *     2. Secure context (unprivileged) / interrupt (privileged) switch
 * A special SVC handler is given a shortcut to speed up execution. It is used
 * to switch the context between 2 boxes, during normnal execution
 * (unprivileged) or due to an interrupt (privileged). It accepts 4 arguments
 * generated by the asm code below.
 *
 * Note: The implementation of this handler relies on the bit configuration of
 * the 8 bit immediate field of the svc opcode. If this changes (svc_exports.h)
 * then also the handler implementation must change.
 *
 ******************************************************************************/
/* FIXME add register clearing */
/* FIXME add support for floating point in context switches */
void UVISOR_NAKED SVCall_IRQn_Handler(void)
{
    asm volatile(
        "tst    lr, #4\n"                   // privileged/unprivileged mode
        "it     eq\n"
        "beq    called_from_priv\n"

    /* the code here serves calls from unprivileged code and is mirrored below
     * for the privileged case; only minor changes exists between the two */
    "called_from_unpriv:\n"
        "mrs    r0, PSP\n"                  // stack pointer
        "ldrt   r1, [r0, #24]\n"            // stacked pc
        "add    r1, r1, #-2\n"              // pc at SVC call
        "ldrbt  r2, [r1]\n"                 // SVC immediate
        // Call the priviliged SVC 0 handler, keeping LR as EXC_RETURN.
        "cbnz   r2, uvisor_unpriv_svc_handler\n" // If SVC is not 0: run uVisor handler
        "ldr    r0, %[priv_svc_0]\n"
        "bx     r0\n"                       // Run the priv_svc_0 hook.
    "uvisor_unpriv_svc_handler:\n"
        /***********************************************************************
         *  ATTENTION
         ***********************************************************************
         * the handlers hardcoded in the jump table are called here with 3
         * arguments:
         *    r0 - PSP
         *    r1 - pc of SVCall
         *    r2 - immediate value in SVC opcode
         * these arguments are defined by the asm code you are reading; when
         * changing this code make sure the same format is used or changed
         * accordingly
         **********************************************************************/
        "tst    r2, %[svc_mode_mask]\n"             // Check mode: fast/slow.
        "it     eq\n"
        "beq    custom_table_unpriv\n"
        "and    r3, r2, %[svc_fast_index_mask]\n"   // Isolate index for fast table.
        "adr    r12, jump_table_unpriv\n"           // address of jump table
        "ldr    pc, [r12, r3, lsl #2]\n"            // branch to handler
        ".align 4\n"                                // the jump table must be aligned
    "jump_table_unpriv:\n"
        ".word  unvic_gateway_out\n"
        ".word  __svc_not_implemented\n" // Deprecated: secure_gateway_in
        ".word  __svc_not_implemented\n" // Deprecated: secure_gateway_out
        ".word  register_gateway_perform_operation\n"
        ".word  box_init_first\n"
        ".word  box_init_next\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
    "jump_table_unpriv_end:\n"

    ".thumb_func\n"                                 // needed for correct referencing
    "custom_table_unpriv:\n"
        /* there is no need to mask the lower 4 bits of the SVC# because
         * custom_table_unpriv is only when SVC# <= 0x0F */
        "cmp    r2, %[svc_vtor_tbl_count]\n"        // check SVC table overflow
        "ite    ls\n"                               // note: this ITE order speeds it up
        "ldrls  r1, =g_svc_vtor_tbl\n"              // fetch handler from table
        "bxhi   lr\n"                               // abort if overflowing SVC table
        "add    r1, r1, r2, lsl #2\n"               // SVC table offset
        "ldr    r1, [r1]\n"                         // SVC handler
        "push   {lr}\n"                             // save lr for later
        "ldr    lr, =svc_thunk_unpriv\n"            // after handler return to thunk
        "push   {r1}\n"                             // save SVC handler to fetch args
        "ldrt   r3, [r0, #12]\n"                    // fetch args (unprivileged)
        "ldrt   r2, [r0, #8]\n"                     // pass args from stack (unpriv)
        "ldrt   r1, [r0, #4]\n"                     // pass args from stack (unpriv)
        "ldrt   r0, [r0, #0]\n"                     // pass args from stack (unpriv)
        "pop    {pc}\n"                             // execute handler (return to thunk)

    ".thumb_func\n"                                 // needed for correct referencing
    "svc_thunk_unpriv:\n"
        "mrs    r1, PSP\n"                          // unpriv stack may have changed
        "strt   r0, [r1]\n"                         // store result on stacked r0
        "pop    {pc}\n"                             // return from SVCall

    "called_from_priv:\n"
        "mrs    r0, MSP\n"                          // stack pointer
        "ldr    r1, [r0, #24]\n"                    // stacked pc
        "add    r1, r1, #-2\n"                      // pc at SVC call
        "ldrb   r2, [r1]\n"                         // SVC immediate
        // Call the priviliged SVC 0 handler, keeping LR as EXC_RETURN.
        "cbnz   r2, uvisor_priv_svc_handler\n"      // If SVC is not 0: run uVisor handler
        "ldr    r0, %[priv_svc_0]\n"
        "bx     r0\n"                               // Run the priv_svc_0 hook.
    "uvisor_priv_svc_handler:\n"
        /***********************************************************************
         *  ATTENTION
         ***********************************************************************
         * the handlers hardcoded in the jump table are called here with 3
         * arguments:
         *    r0 - MSP
         *    r1 - pc of SVCall
         *    r2 - immediate value in SVC opcode
         * these arguments are defined by the asm code you are reading; when
         * changing this code make sure the same format is used or changed
         * accordingly
         **********************************************************************/
        "tst    r2, %[svc_mode_mask]\n"             // Check mode: fast/slow.
        "it     eq\n"
        "beq    custom_table_priv\n"
        "and    r3, r2, %[svc_fast_index_mask]\n"   // Isolate index for fast table.
        "adr    r12, jump_table_priv\n"             // address of jump table
        "ldr    pc, [r12, r3, lsl #2]\n"            // branch to handler
        ".align 4\n"                                // the jump table must be aligned
    "jump_table_priv:\n"
        ".word  unvic_gateway_in\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
        ".word  __svc_not_implemented\n"
    "jump_table_priv_end:\n"

    ".thumb_func\n"                                 // needed for correct referencing
    "custom_table_priv:\n"
        /* there is no need to mask the lower 4 bits of the SVC# because
         * custom_table_unpriv is only when SVC# <= 0x0F */
        "cmp    r2, %[svc_vtor_tbl_count]\n"        // check SVC table overflow
        "ite    ls\n"                               // note: this ITE order speeds it up
        "ldrls  r1, =g_svc_vtor_tbl\n"              // fetch handler from table
        "bxhi   lr\n"                               // abort if overflowing SVC table
        "add    r1, r1, r2, lsl #2\n"               // SVC table offset
        "ldr    r1, [r1]\n"                         // SVC handler
        "push   {lr}\n"                             // save lr for later
        "ldr    lr, =svc_thunk_priv\n"              // after handler return to thunk
        "push   {r1}\n"                             // save SVC handler to fetch args
        "ldm    r0, {r0-r3}\n"                      // pass args from stack
        "pop    {pc}\n"                             // execute handler (return to thunk)

    ".thumb_func\n"                                 // needed for correct referencing
    "svc_thunk_priv:\n"
        "str    r0, [sp, #4]\n"                     // store result on stacked r0
        "pop    {pc}\n"                             // return from SVCall

        :: [svc_mode_mask]       "I" ((UVISOR_SVC_MODE_MASK) & 0xFF),
           [svc_fast_index_mask] "I" ((UVISOR_SVC_FAST_INDEX_MASK) & 0xFF),
           [svc_vtor_tbl_count]  "i" (UVISOR_ARRAY_COUNT(g_svc_vtor_tbl) - 1),
           [priv_svc_0]          "m" (g_priv_sys_hooks.priv_svc_0)
    );
}

/*******************************************************************************
 *
 * Function name:   svc_init
 * Brief:           SVC initialization
 *
 ******************************************************************************/
void svc_init(void)
{
    /* sanity checks */
    assert((&jump_table_unpriv_end - &jump_table_unpriv) == UVISOR_SVC_FAST_INDEX_MAX);
    assert((&jump_table_priv_end - &jump_table_priv) == UVISOR_SVC_FAST_INDEX_MAX);
    assert(UVISOR_ARRAY_COUNT(g_svc_vtor_tbl) <= UVISOR_SVC_SLOW_INDEX_MAX);
}
