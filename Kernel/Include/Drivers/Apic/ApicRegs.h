#pragma once

#include <Types.h>

/*
 * Local APIC Register Map
 */
#define LAPIC_ID                0x020
#define LAPIC_VERSION           0x030
#define LAPIC_TPR               0x080
#define LAPIC_APR               0x090
#define LAPIC_PPR               0x0A0
#define LAPIC_EOI               0x0B0
#define LAPIC_RRD               0x0C0
#define LAPIC_LDR               0x0D0
#define LAPIC_DFR               0x0E0
#define LAPIC_SVR               0x0F0
#define LAPIC_ISR               0x100
#define LAPIC_TMR               0x180
#define LAPIC_IRR               0x200
#define LAPIC_ESR               0x280
#define LAPIC_ICR_LOW           0x300
#define LAPIC_ICR_HIGH          0x310
#define LAPIC_LVT_TIMER         0x320
#define LAPIC_LVT_THERMAL       0x330
#define LAPIC_LVT_PERFMON       0x340
#define LAPIC_LVT_LINT0         0x350
#define LAPIC_LVT_LINT1         0x360
#define LAPIC_LVT_ERROR         0x370
#define LAPIC_TIMER_INITCNT     0x380
#define LAPIC_TIMER_CURRCNT     0x390
#define LAPIC_TIMER_DIV         0x3E0
#define APIC_VERSION_MASK       0xFF

/*
 * LVT bits
 */
#define LVT_MASKED              (1 << 16)
#define LVT_TRIGGER_LEVEL        (1 << 15)
#define LVT_REMOTE_IRR           (1 << 14)
#define LVT_PIN_POLARITY         (1 << 13)
#define LVT_DELIVERY_STATUS      (1 << 12)
#define LVT_DEST_MODE            (1 << 11)

/*
 * Delivery Mode
 */
#define DELIVERY_FIXED           (0 << 8)
#define DELIVERY_SMI             (2 << 8)
#define DELIVERY_NMI             (4 << 8)
#define DELIVERY_INIT            (5 << 8)
#define DELIVERY_EXTINT          (7 << 8)

/*
 * Timer Mode
 */
#define TIMER_ONESHOT            (0 << 17)
#define TIMER_PERIODIC           (1 << 17)

/*
 * Timer Divide
 */
#define TIMER_DIV_2              0x0
#define TIMER_DIV_4              0x1
#define TIMER_DIV_8              0x2
#define TIMER_DIV_16             0x3
#define TIMER_DIV_32             0x8
#define TIMER_DIV_64             0x9
#define TIMER_DIV_128            0xA
#define TIMER_DIV_256            0xB

/*
 * ICR shortcuts
 */
#define ICR_DEST_SELF            (0x1 << 18)
#define ICR_DEST_ALL             (0x2 << 18)
#define ICR_DEST_ALL_BUT_SELF    (0x3 << 18)

/*
 * ============================================================================= IO APIC Register Map ============================================================================
 */

#define IOAPIC_ID                0x00
#define IOAPIC_VERSION           0x01
#define IOAPIC_ARB               0x02
#define IOAPIC_REDIR_TABLE       0x10

/*
 * Redirection Entry Bits
 */
#define IOAPIC_REDIR_VECTOR      0x000000FF
#define IOAPIC_REDIR_DELIVERY    0x00000700
#define IOAPIC_REDIR_DEST_MODE   0x00000800
#define IOAPIC_REDIR_STATUS      0x00001000
#define IOAPIC_REDIR_POLARITY    0x00002000
#define IOAPIC_REDIR_REMOTE_IRR  0x00004000
#define IOAPIC_REDIR_TRIGGER     0x00008000
#define IOAPIC_REDIR_MASKED      0x00010000
#define IOAPIC_REDIR_DEST        0xFF000000

#ifndef IOAPIC_REDIR_MASKED
#define IOAPIC_REDIR_MASKED       0x00010000
#define IOAPIC_REDIR_TRIGGER      0x00008000
#define IOAPIC_REDIR_POLARITY     0x00002000
#define IOAPIC_REDIR_STATUS       0x00001000
#define IOAPIC_REDIR_DEST_MODE    0x00000800
#define IOAPIC_REDIR_DELIVERY     0x00000700
#define IOAPIC_REDIR_VECTOR       0x000000FF
#endif

#define DELIVERY_STARTUP         (6 << 8)
#define ICR_DEST_PHYSICAL        (0 << 11)
#define ICR_DEST_LOGICAL         (1 << 11)
#define ICR_LEVEL_ASSERT         (1 << 14)
#define ICR_LEVEL_DEASSERT       (0 << 14)

/*
 * IA32_APIC_BASE MSR (0x1B)
 */
#define IA32_APIC_BASE_MSR       0x1B
#define APIC_BASE_ENABLE         (1ULL << 11)
#define APIC_BASE_X2APIC         (1ULL << 10)
#define APIC_BASE_ADDR_MASK      0xFFFFFF000ULL

/*
 * x2APIC MSR addresses (offset >> 4 + 0x800)
 */
#define X2APIC_MSR_BASE          0x800
#define X2APIC_MSR_ID            0x802
#define X2APIC_MSR_VERSION       0x803
#define X2APIC_MSR_TPR           0x808
#define X2APIC_MSR_EOI           0x80B
#define X2APIC_MSR_SVR           0x80F
#define X2APIC_MSR_ICR           0x830
#define X2APIC_MSR_LVT_TIMER     0x832
#define X2APIC_MSR_LVT_LINT0     0x835
#define X2APIC_MSR_LVT_LINT1     0x836
#define X2APIC_MSR_TIMER_INIT    0x838
#define X2APIC_MSR_TIMER_CURR    0x839
#define X2APIC_MSR_TIMER_DIV     0x83E

#define ICR_DELIVERY_PENDING     (1 << 12)
#define ICR_DEST_FIELD_SHIFT     32

#define X2APIC_OFFSET_TO_MSR(Offset)  (X2APIC_MSR_BASE + ((Offset) >> 4))
