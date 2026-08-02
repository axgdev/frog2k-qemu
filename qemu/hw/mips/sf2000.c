// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 UniFrog contributors
 *
 * SF2000 / HC15xx early machine model.
 *
 * This device model is intentionally forgiving: the first goal is to execute
 * stock boot firmware far enough to discover the real device contract. Unknown
 * MMIO is logged and returns stable values instead of aborting the guest.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/mips/mips.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "qemu/audio.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/block-backend.h"
#include "system/block-backend-global-state.h"
#include "system/blockdev.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/dma.h"
#include "exec/tb-flush.h"
#include "elf.h"
#include "ui/input.h"
#include "qom/object.h"
#include "target/mips/internal.h"
#include "ui/console.h"

#define TYPE_SF2000_LCD "sf2000-lcd"
OBJECT_DECLARE_SIMPLE_TYPE(SF2000LCDState, SF2000_LCD)

/*
 * Address map notes
 * -----------------
 *
 * The SF2000/HC15xx boots a MIPS core from the usual reset vector at
 * virtual 0xbfc00000. QEMU sees that through physical 0x1fc00000, so the SPI
 * flash image is mapped there. Firmware later uses kseg0/kseg1 virtual
 * aliases for RAM and peripherals:
 *
 *   0x80000000 -> cached physical RAM at 0x00000000
 *   0xa0000000 -> uncached physical RAM at 0x00000000
 *   0xb8800000 -> uncached physical MMIO at 0x18800000
 *
 * Most constants below are physical addresses, matching what QEMU's memory
 * subsystem receives after MIPS address translation.
 */
#define SF2000_RAM_BASE        0x00000000ULL
#define SF2000_RAM_DEFAULT     (128 * MiB)
#define SF2000_BOOT_BASE       0x1fc00000ULL
#define SF2000_BOOT_ALIAS_BASE 0x0fc00000ULL
#define SF2000_BOOT_SIZE       (4 * MiB)
#define SF2000_CPU_DEFAULT_HZ  918000000ULL
#define SF2000_CPU_MIN_HZ      1000000ULL
#define SF2000_CPU_MAX_HZ      918000000ULL
#define SF2000_BL_FLASH_OFF    0x00005c00ULL
#define SF2000_BL_RAM_BASE     0x01000000ULL
#define SF2000_BL_ENTRY        0x81000490ULL
#define SF2000_ASD_SKIP        0x200ULL
#define SF2000_ASD_LOAD_BASE   0x00000200ULL
#define SF2000_ASD_ENTRY       0x80001000ULL
#define SF2000_LINUX_DTB_ALIGN (64 * KiB)
#define SF2000_BL_INFO_MAGIC_ADDR 0x00000010ULL
#define SF2000_BL_INFO_PTR_ADDR   0x00000014ULL
#define SF2000_BL_INFO_DATA_ADDR  0x00000100ULL
#define SF2000_BL_INFO_DATA_KSEG1 0xa0000100U
#define SF2000_BL_INFO_MAGIC      0xabcd5aa5U
#define SF2000_TVSYS_RGB_LCD      0x16U
#define SF2000_MMIO_BASE       0x10000000ULL
#define SF2000_MMIO_SIZE       0x0fc00000ULL
#define SF2000_MSYSIO_BASE     0x18800000ULL
#define SF2000_AVPHY_BASE      0x18804000ULL
#define SF2000_AVPHY_SIZE      0x00000100ULL
#define SF2000_LCD_MMIO_BASE   0x18000000ULL
#define SF2000_LCD_MMIO_SIZE   0x00001000ULL
#define SF2000_LCD_WIDTH       320
#define SF2000_LCD_HEIGHT      240
#define SF2000_CHIP_ID_VALUE   0x1512a501
#define SF2000_HC15XX_CHIP_ID  0x1512
#define SF2000_IRC_BASE        0x18818100ULL
#define SF2000_IRC_SIZE        0x00000010ULL
#define SF2000_IRC_CFG         0x00
#define SF2000_IRC_FIFOCFG     0x01
#define SF2000_IRC_IER         0x06
#define SF2000_IRC_ISR         0x07
#define SF2000_IRC_FIFO        0x08
#define SF2000_IRC_FIFOCFG_RESET BIT(7)
#define SF2000_IRC_ISR_MASK    0x03
#define SF2000_UART0_BASE      0x18818300ULL
#define SF2000_UART1_BASE      0x18818600ULL
#define SF2000_UART_SIZE       0x00000008ULL
#define SF2000_UART_RBR_THR    0x00
#define SF2000_UART_IER        0x01
#define SF2000_UART_IIR        0x02
#define SF2000_UART_LSR        0x05
#define SF2000_UART_IER_THRI   BIT(1)
#define SF2000_UART_IIR_NONE   0x01
#define SF2000_UART_IIR_THRI   0x02
#define SF2000_UART_LSR_THRE   BIT(5)
#define SF2000_UART_LSR_TEMT   BIT(6)
#define SF2000_I2C0_BASE       0x18818200ULL
#define SF2000_I2C1_BASE       0x18818700ULL
#define SF2000_I2C_SIZE        0x00000040ULL
#define SF2000_I2C_HCR         0x00
#define SF2000_I2C_HSR         0x01
#define SF2000_I2C_IER         0x02
#define SF2000_I2C_ISR         0x03
#define SF2000_I2C_DATA        0x10
#define SF2000_I2C_IER1        0x20
#define SF2000_I2C_ISR1        0x21
#define SF2000_I2C_HCR_ST      BIT(0)
#define SF2000_I2C_ISR_TDI     BIT(0)
#define SF2000_I2C_ISR1_FT     BIT(0)
#define SF2000_PWM_BASE        0x18818410ULL
#define SF2000_PWM_SIZE        0x00000040ULL
#define SF2000_PWM_CLK_CTRL    0x00
#define SF2000_PWM_DIV_BASE    0x04
#define SF2000_PWM_CHANNELS    6
#define SF2000_PWM_CH_STRIDE   0x08
#define SF2000_PWM_CLKSEL_MASK 0x3fff0000u
#define SF2000_PWM_CLKEN       BIT(30)
#define SF2000_PWM_ENABLE      BIT(31)
#define SF2000_PWM_POLARITY    BIT(36 - 32)
#define SF2000_PWM_CH_ENABLE   BIT(39 - 32)
#define SF2000_ADC_BASE        0x18818400ULL
#define SF2000_ADC_SIZE        0x00000100ULL
#define SF2000_ADC_SAMPLE      200
#define SF2000_WDT_BASE        0x18818500ULL
#define SF2000_WDT_SIZE        0x00000100ULL
#define SF2000_SYSCLK_EXT_BASE 0x18801000ULL
#define SF2000_SYSCLK_EXT_SIZE 0x00000100ULL
#define SF2000_AUDIO_I2S_BASE  0x1880a000ULL
#define SF2000_AUDIO_I2S_SIZE  0x00000100ULL
#define SF2000_AUDIO_I2S_CTRL3C (SF2000_AUDIO_I2S_BASE + 0x3c)
#define SF2000_AUDIO_I2S_FADE90 (SF2000_AUDIO_I2S_BASE + 0x90)
#define SF2000_AUDIO_SYS_PLL   0x18800148ULL
#define SF2000_AUDIO_SYS_GATE  0x18800154ULL
#define SF2000_AUDIO_PLL_32K   0x0c000120u
#define SF2000_AUDIO_DMA_CFG_MASK 0x0f330000u
#define SF2000_AUDIO_DMA_MONO_S16 0x09100000u
#define SF2000_SND_DAC_BASE    0x1880b000ULL
#define SF2000_SND_DAC_SIZE    0x00000100ULL
#define SF2000_USB0_BASE       0x18844000ULL
#define SF2000_USB1_BASE       0x18850000ULL
#define SF2000_USB_SIZE        0x00001000ULL
#define SF2000_USB_REG_COUNT   (SF2000_USB_SIZE / 4)
#define SF2000_MUSB_POWER_SOFTCONN 0x40
#define SF2000_MUSB_POWER_HSENAB   0x20
#define SF2000_MUSB_DEVCTL_VBUS    0x18
#define SF2000_MUSB_DEVCTL_HM      0x04
#define SF2000_MUSB_DEVCTL_SESSION 0x01
#define SF2000_MUSB_POWER_RESET_READ 0x70
#define SF2000_MUSB_POWER_OFF_READ    0x20
#define SF2000_MUSB_DEVCTL_RESET_READ 0x99
#define SF2000_MUSB_DEVCTL_OFF_READ   0x80
#define SF2000_DSC_BOOT_BASE   0x18870000ULL
#define SF2000_DSC_BOOT_SIZE   0x00000010ULL
#define SF2000_GE_BASE         0x18806000ULL
#define SF2000_GE_SIZE         0x00000100ULL
#define SF2000_GE_CTRL         (SF2000_GE_BASE + 0x00)
#define SF2000_GE_START        (SF2000_GE_BASE + 0x04)
#define SF2000_GE_STATUS       (SF2000_GE_BASE + 0x08)
#define SF2000_GE_HQ_FIRST     (SF2000_GE_BASE + 0x10)
#define SF2000_GE_HQ_LAST      (SF2000_GE_BASE + 0x14)
#define SF2000_GE_STATUS_DONE  BIT(0)
#define SF2000_GE_STATUS_BUSY  BIT(31)
#define SF2000_GE_IRQ_BIT      4
#define SF2000_GE_IRQ          BIT(SF2000_GE_IRQ_BIT)
/*
 * A full-screen HC15xx blit completes well inside one 60 Hz frame.  Keeping
 * this at 100 us preserves asynchronous completion without manufacturing the
 * command-ring backlog caused by the old, pessimistic 1 ms placeholder.
 */
#define SF2000_GE_LATENCY_NS   (NANOSECONDS_PER_SECOND / 10000)
#define SF2000_GE_QUEUE_DUMP_WORDS 192
#define SF2000_GE_QUEUE_DUMP_DEFAULT 0
#define SF2000_GMA_BASE        0x18808000ULL
#define SF2000_GMA_SIZE        0x00003000ULL
#define SF2000_TIMER_BASE      0x18818a00ULL
#define SF2000_TIMER_SIZE      0x00000080ULL
#define SF2000_IRQ_STATUS1     0x18800030ULL
#define SF2000_IRQ_STATUS2     0x18800034ULL
#define SF2000_IRQ_ENABLE1     0x18800038ULL
#define SF2000_IRQ_ENABLE2     0x1880003cULL
#define SF2000_TIMER_COUNT     8
#define SF2000_TIMER_STEP      0x10
#define SF2000_TIMER_CTRL_EN   BIT(2)
#define SF2000_TIMER_CTRL_INT  BIT(3)
#define SF2000_TIMER_CTRL_IEN  BIT(4)
#define SF2000_TIMER_SEC_COUNT 2
#define SF2000_TIMER5_INDEX    5
#define SF2000_TIMER5_MIN_DEADLINE_NS (NANOSECONDS_PER_SECOND / 500)
#define SF2000_TIMER5_BASE     (SF2000_TIMER_BASE + \
                                SF2000_TIMER5_INDEX * SF2000_TIMER_STEP)
#define SF2000_SB_TIMER_BIT    22
#define SF2000_SB_TIMER_IRQ    (1u << SF2000_SB_TIMER_BIT)
#define SF2000_UART0_IRQ_BIT   16
#define SF2000_UART0_IRQ       (1u << SF2000_UART0_IRQ_BIT)
#define SF2000_UART1_IRQ_BIT   17
#define SF2000_UART1_IRQ       (1u << SF2000_UART1_IRQ_BIT)
#define SF2000_I2C0_IRQ_BIT    18
#define SF2000_I2C0_IRQ        (1u << SF2000_I2C0_IRQ_BIT)
#define SF2000_I2C1_IRQ_BIT    25
#define SF2000_I2C1_IRQ        (1u << SF2000_I2C1_IRQ_BIT)
#define SF2000_IRC_IRQ_BIT     19
#define SF2000_IRC_IRQ         (1u << SF2000_IRC_IRQ_BIT)
#define SF2000_SDIO_IRQ_BIT    10
#define SF2000_SDIO_IRQ        (1u << SF2000_SDIO_IRQ_BIT)
#define SF2000_SND_IRQ_BIT     5
#define SF2000_SND_IRQ         (1u << SF2000_SND_IRQ_BIT)
#define SF2000_USB1_DMA_IRQ2   BIT(18)
#define SF2000_USB1_MC_IRQ2    BIT(19)
#define SF2000_USB0_DMA_IRQ2   BIT(20)
#define SF2000_USB0_MC_IRQ2    BIT(21)
#define SF2000_AUX_BASE        0x1880f000ULL
#define SF2000_AUX_SIZE        0x00000100ULL
#define SF2000_GPIO_IRQ        BIT(0)
#define SF2000_EIRQ3_LINE      3
#define SF2000_GPIO_L_IER      0x18800044ULL
#define SF2000_GPIO_L_RIS_IER  0x18800048ULL
#define SF2000_GPIO_L_IN       0x18800050ULL
#define SF2000_GPIO_L_OUT      0x18800054ULL
#define SF2000_GPIO_L_DIR      0x18800058ULL
#define SF2000_GPIO_L15        BIT(15)
#define SF2000_GPIO_L_ISR      0x1880005cULL
#define SF2000_GPIO_L08        BIT(8)
#define SF2000_GPIO_R_IN       0x188000f0ULL
#define SF2000_GPIO_R_OUT      0x188000f4ULL
#define SF2000_GPIO_R_DIR      0x188000f8ULL
#define SF2000_GPIO_R_ISR      0x188000fcULL
#define SF2000_GPIO_R_POK      BIT(30)
#define SF2000_GPIO_R07        BIT(7)
#define SF2000_PINMUX_R07      0x188004e7ULL
#define SF2000_PINMUX_L15      0x188004afULL
#define SF2000_KEY_DATA_BIT    23
#define SF2000_KEY_CLK_BIT     24
#define SF2000_KEY_COUNT       12
#define SF2000_RF_DATA_BIT     27
#define SF2000_RF_CLK_BIT      28
#define SF2000_RF_CS_BIT       29

typedef struct SF2000BoardProfileSpec {
    const char *name;
    uint32_t lcd_width;
    uint32_t lcd_height;
    uint32_t panel_te_hz;
    uint32_t panel_id;
    uint32_t panel_probe_sig1;
    uint32_t panel_probe_sig2;
    const char *gpio_init;
    const char *audio_route;
    const char *audio_open_route;
    const char *audio_runtime_route;
    const char *audio_open_returns;
    const char *audio_close_returns;
    const char *audio_gate_route;
    uint32_t audio_gate_l0;
    uint32_t audio_gate_l1;
    uint32_t audio_gate_l1_active;
    uint32_t audio_gate_r0;
    uint32_t audio_gate_r1;
    uint32_t audio_gate_r1_active;
    const char *audio_mux_open;
    uint32_t audio_hw_backend;
    uint32_t audio_hw_snd0;
    uint32_t audio_hw_dac;
    const char *audio_hw_close;
    uint32_t audio_volume;
    uint32_t audio_gain;
    uint32_t audio_sample_rate_hz;
    uint32_t audio_channels;
    uint32_t audio_runtime_channels;
    uint32_t audio_period_frames;
    uint32_t audio_periods;
    const char *usb0_route;
    const char *usb1_route;
    const char *usb_root_hub_id;
    uint32_t usb_root_hub_ports;
    uint32_t usb_ctl0;
    uint32_t usb_ctl1;
    uint32_t usb_phy0;
    uint32_t usb_phy1;
    uint32_t usb_phy2;
    uint32_t usb_phy3;
    uint32_t usb_utmi380;
    uint32_t usb_phy384;
    const char *storage_reset;
} SF2000BoardProfileSpec;

static const SF2000BoardProfileSpec sf2000_board_profiles[] = {
    {
        .name = "sf2000",
        .lcd_width = SF2000_LCD_WIDTH,
        .lcd_height = SF2000_LCD_HEIGHT,
        .panel_te_hz = 60,
        .panel_id = 0x00858552,
        .panel_probe_sig1 = 0xf3f3f2f2,
        .panel_probe_sig2 = 0x00000004,
        .gpio_init = "l=0x150004ff/0x050004b2 r=0x00000020/0x00000020 "
                     "mux_l22=0 mux_l23=0 mux_l24=0 mux_l25=0 "
                     "mux_l26=0 mux_l27=0 mux_l28=0 mux_l29=2 mux_r07=7",
        .audio_route = "sf2000-default-amp",
        .audio_open_route = "sf2000_left_only",
        .audio_runtime_route = "sf2000_stereo_safe",
        .audio_open_returns = "volume_ret=-1 mute_ret=0 silence_ret=0 start_ret=0 "
                              "unmute_ret=0 output_ret=0",
        .audio_close_returns = "mute_ret=-1 drop_ret=0 free_ret=0",
        .audio_gate_route = "sf2000_r07",
        .audio_gate_l0 = 0x390004fe,
        .audio_gate_l1 = 0x2b4085b3,
        .audio_gate_l1_active = 0x2b4085b3,
        .audio_gate_r0 = 0x000000a0,
        .audio_gate_r1 = 0x000000a0,
        .audio_gate_r1_active = 0x000000a0,
        .audio_mux_open = "l22=0 l23=0 l24=0 l25=0 l26=0 l27=0 l28=0 l29=0 r07=0",
        .audio_hw_backend = 2,
        .audio_hw_snd0 = 0x14fc0082,
        .audio_hw_dac = 0x4200039e,
        .audio_hw_close = "backend=2 snd0=0x14fc0082 dac=0x420003a8 hw_ret=-1 "
                          "dma=0x00000000/0 hw_rate=0 hw_ch=0 hw_fmt=0 "
                          "hw_period=0 hw_periods=0",
        .audio_volume = 75,
        .audio_gain = 8,
        .audio_sample_rate_hz = 32000,
        .audio_channels = 1,
        .audio_runtime_channels = 2,
        .audio_period_frames = 1024,
        .audio_periods = 8,
        .usb0_route = "micro-usb",
        .usb1_route = "usb-a",
        .usb_root_hub_id = "1d6b:0002",
        .usb_root_hub_ports = 1,
        .usb_ctl0 = 0x07000101,
        .usb_ctl1 = 0x00000002,
        .usb_phy0 = 0x06060000,
        .usb_phy1 = 0x00060606,
        .usb_phy2 = 0x06060606,
        .usb_phy3 = 0x00060606,
        .usb_utmi380 = 0x00570740,
        .usb_phy384 = 0x00000010,
        .storage_reset = "mode=safe experimental=0 status=okay clock=198000000 "
                         "bus-width=1 cap-highspeed=0 supports-highspeed=0 "
                         "uhs-sdr12=0 uhs-sdr25=0 uhs-sdr50=0 no-1v8=1 "
                         "broken-cd=1",
    },
    {
        .name = "gb300",
        .lcd_width = 240,
        .lcd_height = 320,
        .panel_te_hz = 60,
        .panel_id = 0x00009306,
        .panel_probe_sig1 = 0x00000000,
        .panel_probe_sig2 = 0x00000005,
        .gpio_init = "l=0x150004ff/0x050004b2 r=0x00000020/0x00000020 "
                     "mux_l22=0 mux_l23=0 mux_l24=0 mux_l25=0 "
                     "mux_l26=0 mux_l27=0 mux_l28=0 mux_l29=2 mux_r07=7",
        .audio_route = "gb300-family-amp",
        .audio_open_route = "sf2000_left_only",
        .audio_runtime_route = "sf2000_stereo_safe",
        .audio_open_returns = "volume_ret=-1 mute_ret=0 silence_ret=0 start_ret=0 "
                              "unmute_ret=0 output_ret=0",
        .audio_close_returns = "mute_ret=-1 drop_ret=0 free_ret=0",
        .audio_gate_route = "gb300_l15",
        .audio_gate_l0 = 0x350084fe,
        .audio_gate_l1 = 0x25c085b3,
        .audio_gate_l1_active = 0x25c085b3,
        .audio_gate_r0 = 0x00000020,
        .audio_gate_r1 = 0x00000020,
        .audio_gate_r1_active = 0x00000020,
        .audio_mux_open = "l22=0 l23=0 l24=0 l25=0 l26=0 l27=0 l28=0 l29=0 r07=0",
        .audio_hw_backend = 2,
        .audio_hw_snd0 = 0x14fc0082,
        .audio_hw_dac = 0x4200039e,
        .audio_hw_close = "backend=2 snd0=0x14fc0082 dac=0x420003a8 hw_ret=-1 "
                          "dma=0x00000000/0 hw_rate=0 hw_ch=0 hw_fmt=0 "
                          "hw_period=0 hw_periods=0",
        .audio_volume = 75,
        .audio_gain = 8,
        .audio_sample_rate_hz = 32000,
        .audio_channels = 1,
        .audio_runtime_channels = 2,
        .audio_period_frames = 1024,
        .audio_periods = 8,
        .usb0_route = "micro-usb",
        .usb1_route = "usb-a",
        .usb_root_hub_id = "1d6b:0002",
        .usb_root_hub_ports = 1,
        .usb_ctl0 = 0x07000101,
        .usb_ctl1 = 0x00000002,
        .usb_phy0 = 0x06060000,
        .usb_phy1 = 0x00060606,
        .usb_phy2 = 0x06060606,
        .usb_phy3 = 0x00060606,
        .usb_utmi380 = 0x00570740,
        .usb_phy384 = 0x00000010,
        .storage_reset = "mode=safe experimental=0 status=okay clock=198000000 "
                         "bus-width=1 cap-highspeed=0 supports-highspeed=0 "
                         "uhs-sdr12=0 uhs-sdr25=0 uhs-sdr50=0 no-1v8=1 "
                         "broken-cd=1",
    },
};

static char *sf2000_board_profile = NULL;
static bool sf2000_audio_powered;
static uint32_t sf2000_audio_dac_value;
static bool sf2000_audio_dac_written;
static uint32_t sf2000_audio_i2s_ctrl3c;
static uint32_t sf2000_audio_i2s_fade90;
static bool sf2000_storage_selftest_raw;
static bool sf2000_usb_link_powered[2];
static bool sf2000_usb_link_active[2];
static uint32_t sf2000_usb_power_reg[2];
static uint32_t sf2000_usb_devctl_reg[2];
static bool sf2000_mmio_get32(hwaddr addr, uint32_t *value);
static void sf2000_mmio_set32(hwaddr addr, uint32_t value);
static void sf2000_update_irq(void);
static uint32_t sf2000_panel_sample_readback_l(SF2000LCDState *s,
                                               uint32_t value);
static uint32_t sf2000_panel_sample_readback_t(SF2000LCDState *s,
                                               uint32_t value);

static const SF2000BoardProfileSpec *sf2000_board_profile_spec(void)
{
    const char *name = sf2000_board_profile ? sf2000_board_profile : "sf2000";
    size_t i;

    for (i = 0; i < ARRAY_SIZE(sf2000_board_profiles); i++) {
        if (strcmp(name, sf2000_board_profiles[i].name) == 0) {
            return &sf2000_board_profiles[i];
        }
    }

    return &sf2000_board_profiles[0];
}

static const char *sf2000_board_profile_name(void)
{
    return sf2000_board_profile_spec()->name;
}

static AudioBackend *sf2000_audio_be;
static SWVoiceOut *sf2000_audio_voice;
static bool sf2000_audio_backend_ready;
static uint32_t sf2000_audio_wave_phase;
static uint32_t sf2000_audio_wave_step;
static uint32_t sf2000_audio_volume;
static uint32_t sf2000_audio_gain;
static uint32_t sf2000_audio_dma_base;
static uint32_t sf2000_audio_dma_bytes;
static uint32_t sf2000_audio_dma_producer;
static uint32_t sf2000_audio_dma_consumer;
static uint32_t sf2000_audio_dma_queued;
static uint32_t sf2000_audio_snd_ctl04;
static uint32_t sf2000_audio_snd_ctl08;
static uint32_t sf2000_audio_snd_ctl0c;
static uint32_t sf2000_audio_snd_ctl50;
static uint32_t sf2000_audio_stc_control;
static uint32_t sf2000_audio_stc_tick[2];
static int64_t sf2000_audio_stc_epoch_ns[2];
static bool sf2000_audio_dma_reported;
static bool sf2000_audio_callback_reported;
static bool sf2000_audio_apll_reset_seen;

static uint32_t sf2000_audio_stc_read(unsigned id)
{
    uint32_t tick = sf2000_audio_stc_tick[id];

    if (sf2000_audio_stc_control & BIT(16 + id)) {
        int64_t elapsed_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                             sf2000_audio_stc_epoch_ns[id];

        if (elapsed_ns > 0) {
            tick += muldiv64(elapsed_ns, 45000, NANOSECONDS_PER_SECOND);
        }
    }
    return tick;
}

static void sf2000_audio_stc_latch(unsigned id)
{
    sf2000_audio_stc_tick[id] = sf2000_audio_stc_read(id);
    sf2000_audio_stc_epoch_ns[id] =
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void sf2000_audio_stc_control_write(uint32_t value)
{
    sf2000_audio_stc_latch(0);
    sf2000_audio_stc_latch(1);
    sf2000_audio_stc_control = value;
}

static void sf2000_audio_stc_tick_write(unsigned id, uint32_t value)
{
    sf2000_audio_stc_tick[id] = value;
    sf2000_audio_stc_epoch_ns[id] =
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static bool sf2000_audio_dma_configured(void)
{
    uint32_t dma = 0;
    uint32_t pll = 0;
    uint32_t gate = 0;

    sf2000_mmio_get32(SF2000_AUDIO_I2S_BASE + 0x34, &dma);
    sf2000_mmio_get32(SF2000_AUDIO_SYS_PLL, &pll);
    sf2000_mmio_get32(SF2000_AUDIO_SYS_GATE, &gate);

    /*
     * HC15xx SND0 mono S16_LE contract recovered from libauddrv:
     * mono channel code 0, S16 format code 1, mono packing, DMA enable,
     * 12.288 MHz I2S clock and its gate.  Bit 25 is the stereo-channel
     * selector and must be clear for the SF2000's one-channel stream.
     */
    return (dma & SF2000_AUDIO_DMA_CFG_MASK) ==
               SF2000_AUDIO_DMA_MONO_S16 &&
           (sf2000_audio_snd_ctl04 & (BIT(0) | BIT(8) | BIT(16))) ==
               (BIT(0) | BIT(8) | BIT(16)) &&
           pll == SF2000_AUDIO_PLL_32K && (gate & BIT(14)) &&
           sf2000_audio_apll_reset_seen;
}

static bool sf2000_audio_irq_pending(void)
{
    return ((sf2000_audio_snd_ctl08 & (BIT(16) | BIT(0))) ==
            (BIT(16) | BIT(0))) ||
           ((sf2000_audio_snd_ctl0c & (BIT(8) | BIT(0))) ==
            (BIT(8) | BIT(0)));
}

static bool sf2000_audio_output_active(void)
{
    return sf2000_audio_powered && sf2000_audio_i2s_fade90 != 0;
}

static bool sf2000_audio_speaker_enabled(void)
{
    uint32_t output = 0;
    uint32_t direction = 0;
    uint32_t pinmux = 0;

    if (strcmp(sf2000_board_profile_name(), "gb300") == 0) {
        return sf2000_mmio_get32(SF2000_GPIO_L_OUT, &output) &&
               sf2000_mmio_get32(SF2000_GPIO_L_DIR, &direction) &&
               sf2000_mmio_get32(SF2000_PINMUX_L15 & ~3u, &pinmux) &&
               (direction & SF2000_GPIO_L15) &&
               (output & SF2000_GPIO_L15) &&
               ((pinmux >> 24) & 0xff) == 0;
    }

    return sf2000_mmio_get32(SF2000_GPIO_R_OUT, &output) &&
           sf2000_mmio_get32(SF2000_GPIO_R_DIR, &direction) &&
           sf2000_mmio_get32(SF2000_PINMUX_R07 & ~3u, &pinmux) &&
           (direction & SF2000_GPIO_R07) &&
           !(output & SF2000_GPIO_R07) &&
           ((pinmux >> 24) & 0xff) == 0;
}

static bool sf2000_audio_audible(void)
{
    return sf2000_audio_output_active() && sf2000_audio_speaker_enabled();
}

static void sf2000_audio_gate_live_words(const SF2000BoardProfileSpec *profile,
                                         uint32_t *gate_l1,
                                         uint32_t *gate_r1)
{
    if (sf2000_audio_output_active()) {
        *gate_l1 = profile->audio_gate_l1_active;
        *gate_r1 = profile->audio_gate_r1_active;
    } else {
        *gate_l1 = profile->audio_gate_l1;
        *gate_r1 = profile->audio_gate_r1;
    }
}

static void sf2000_audio_set_backend_active(void)
{
    if (sf2000_audio_voice) {
        AUD_set_active_out(sf2000_audio_voice, sf2000_audio_audible());
    }
}

static int16_t sf2000_audio_render_sample(void)
{
    uint32_t volume = sf2000_audio_volume ? sf2000_audio_volume : 75;
    uint32_t gain = sf2000_audio_gain ? sf2000_audio_gain : 8;
    int32_t scaled = 0x0800;

    scaled = (scaled * (int32_t)gain) / 8;
    scaled = (scaled * (int32_t)volume) / 75;
    if (scaled > INT16_MAX) {
        scaled = INT16_MAX;
    }

    sf2000_audio_wave_phase += sf2000_audio_wave_step;
    return (sf2000_audio_wave_phase & 0x80000000u) ?
           -(int16_t)scaled : (int16_t)scaled;
}

static char *sf2000_machine_audio_route_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_spec()->audio_route);
}

static char *sf2000_machine_audio_open_route_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_spec()->audio_open_route);
}

static char *sf2000_machine_audio_runtime_route_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_spec()->audio_runtime_route);
}

static char *sf2000_machine_audio_open_returns_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_spec()->audio_open_returns);
}

static char *sf2000_machine_audio_close_returns_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_spec()->audio_close_returns);
}

static char *sf2000_machine_audio_gate_route_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_spec()->audio_gate_route);
}

static char *sf2000_machine_audio_gate_l_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x/0x%08x",
                           sf2000_board_profile_spec()->audio_gate_l0,
                           sf2000_board_profile_spec()->audio_gate_l1);
}

static char *sf2000_machine_audio_gate_r_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x/0x%08x",
                           sf2000_board_profile_spec()->audio_gate_r0,
                           sf2000_board_profile_spec()->audio_gate_r1);
}

static char *sf2000_machine_audio_gate_l_live_get(Object *obj, Error **errp)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();
    uint32_t gate_l1;
    uint32_t gate_r1;

    sf2000_audio_gate_live_words(profile, &gate_l1, &gate_r1);
    return g_strdup_printf("0x%08x/0x%08x",
                           profile->audio_gate_l0, gate_l1);
}

static char *sf2000_machine_audio_gate_r_live_get(Object *obj, Error **errp)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();
    uint32_t gate_l1;
    uint32_t gate_r1;

    sf2000_audio_gate_live_words(profile, &gate_l1, &gate_r1);
    return g_strdup_printf("0x%08x/0x%08x",
                           profile->audio_gate_r0, gate_r1);
}

static char *sf2000_machine_audio_mux_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_spec()->audio_mux_open);
}

static char *sf2000_machine_audio_hw_backend_get(Object *obj, Error **errp)
{
    return g_strdup_printf("%u", sf2000_board_profile_spec()->audio_hw_backend);
}

static char *sf2000_machine_audio_hw_snd0_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_board_profile_spec()->audio_hw_snd0);
}

static char *sf2000_machine_audio_hw_dac_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_board_profile_spec()->audio_hw_dac);
}

static char *sf2000_machine_audio_hw_close_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_spec()->audio_hw_close);
}

static char *sf2000_machine_audio_volume_get(Object *obj, Error **errp)
{
    return g_strdup_printf("%u", sf2000_audio_volume);
}

static char *sf2000_machine_audio_gain_get(Object *obj, Error **errp)
{
    return g_strdup_printf("%u", sf2000_audio_gain);
}

static char *sf2000_machine_audio_muted_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_audio_output_active() ? "false" : "true");
}

static char *sf2000_machine_audio_gate_state_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_audio_output_active() ? "open" : "closed");
}

static char *sf2000_machine_audio_power_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_audio_powered ? "enabled" : "disabled");
}

static char *sf2000_machine_audio_backend_ready_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_audio_backend_ready ? "true" : "false");
}

static char *sf2000_machine_audio_dac_value_get(Object *obj, Error **errp)
{
    if (!sf2000_audio_dac_written) {
        return g_strdup("unset");
    }

    return g_strdup_printf("0x%08x", sf2000_audio_dac_value);
}

static char *sf2000_machine_audio_sample_rate_get(Object *obj, Error **errp)
{
    return g_strdup_printf("%u",
                           sf2000_board_profile_spec()->audio_sample_rate_hz);
}

static char *sf2000_machine_audio_channels_get(Object *obj, Error **errp)
{
    return g_strdup_printf("%u", sf2000_board_profile_spec()->audio_channels);
}

static char *sf2000_machine_audio_runtime_channels_get(Object *obj, Error **errp)
{
    return g_strdup_printf("%u", sf2000_board_profile_spec()->audio_runtime_channels);
}

static char *sf2000_machine_audio_period_frames_get(Object *obj, Error **errp)
{
    return g_strdup_printf("%u",
                           sf2000_board_profile_spec()->audio_period_frames);
}

static char *sf2000_machine_audio_periods_get(Object *obj, Error **errp)
{
    return g_strdup_printf("%u", sf2000_board_profile_spec()->audio_periods);
}

static char *sf2000_machine_panel_id_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_board_profile_spec()->panel_id);
}

static char *sf2000_machine_panel_te_hz_get(Object *obj, Error **errp)
{
    return g_strdup_printf("%u", sf2000_board_profile_spec()->panel_te_hz);
}

static char *sf2000_machine_panel_probe_sig1_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_board_profile_spec()->panel_probe_sig1);
}

static char *sf2000_machine_panel_probe_sig2_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_board_profile_spec()->panel_probe_sig2);
}

static char *sf2000_machine_gpio_init_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_spec()->gpio_init);
}

static uint32_t sf2000_gpio_l_out;
static uint64_t sf2000_usb_read(hwaddr full_addr, unsigned size);

static char *sf2000_machine_pwm2_backlight_get(Object *obj, Error **errp)
{
    uint32_t pwm_clk_ctrl = 0;
    uint32_t pwm2_lohi = 0;
    uint32_t pwm2_ctrl = 0;

    sf2000_mmio_get32(SF2000_PWM_BASE + SF2000_PWM_CLK_CTRL, &pwm_clk_ctrl);
    sf2000_mmio_get32(SF2000_PWM_BASE + SF2000_PWM_DIV_BASE +
                      2u * SF2000_PWM_CH_STRIDE, &pwm2_lohi);
    sf2000_mmio_get32(SF2000_PWM_BASE + SF2000_PWM_DIV_BASE +
                      2u * SF2000_PWM_CH_STRIDE + 4u, &pwm2_ctrl);
    return g_strdup_printf("clk=0x%08x lohi=0x%08x ctrl=0x%08x",
                           pwm_clk_ctrl, pwm2_lohi, pwm2_ctrl);
}

static bool sf2000_pwm2_backlight_active(void)
{
    uint32_t pwm_clk_ctrl = 0;
    uint32_t pwm2_ctrl = 0;

    sf2000_mmio_get32(SF2000_PWM_BASE + SF2000_PWM_CLK_CTRL, &pwm_clk_ctrl);
    sf2000_mmio_get32(SF2000_PWM_BASE + SF2000_PWM_DIV_BASE +
                      2u * SF2000_PWM_CH_STRIDE + 4u, &pwm2_ctrl);
    return (pwm_clk_ctrl & (SF2000_PWM_CLKEN | SF2000_PWM_ENABLE)) ==
            (SF2000_PWM_CLKEN | SF2000_PWM_ENABLE) &&
           (pwm2_ctrl & SF2000_PWM_CH_ENABLE);
}

static char *sf2000_machine_pwm2_backlight_active_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_pwm2_backlight_active() ? "true" : "false");
}

static char *sf2000_machine_gpio_l_out_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_gpio_l_out);
}

static char *sf2000_machine_audio_i2s_ctrl3c_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_audio_i2s_ctrl3c);
}

static char *sf2000_machine_audio_i2s_fade90_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_audio_i2s_fade90);
}

static char *sf2000_machine_audio_stc_get(Object *obj, Error **errp)
{
    return g_strdup_printf("control=0x%08x stc0=%u stc1=%u",
                           sf2000_audio_stc_control,
                           sf2000_audio_stc_read(0),
                           sf2000_audio_stc_read(1));
}

static char *sf2000_machine_audio_dma_get(Object *obj, Error **errp)
{
    return g_strdup_printf("base=0x%08x bytes=%u producer=%u consumer=%u queued=%u active=%s configured=%s",
                           sf2000_audio_dma_base, sf2000_audio_dma_bytes,
                           sf2000_audio_dma_producer,
                           sf2000_audio_dma_consumer,
                           sf2000_audio_dma_queued,
                           (sf2000_audio_snd_ctl04 & BIT(16)) &&
                           (sf2000_audio_snd_ctl08 & 1) &&
                           (sf2000_audio_snd_ctl0c & 1) &&
                           (sf2000_audio_snd_ctl50 & BIT(29)) ?
                           "true" : "false",
                           sf2000_audio_dma_configured() ? "true" : "false");
}

static bool sf2000_machine_storage_selftest_raw_get(Object *obj, Error **errp)
{
    return sf2000_storage_selftest_raw;
}

static void sf2000_machine_storage_selftest_raw_set(Object *obj, bool value,
                                                    Error **errp)
{
    sf2000_storage_selftest_raw = value;
}

static char *sf2000_machine_usb0_route_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_spec()->usb0_route);
}

static char *sf2000_machine_usb1_route_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_spec()->usb1_route);
}

static char *sf2000_machine_usb0_state_get(Object *obj, Error **errp)
{
    if (sf2000_usb_link_active[0]) {
        return g_strdup("session-active");
    }
    return g_strdup(sf2000_usb_link_powered[0] ? "powered-disconnected"
                                               : "disconnected");
}

static char *sf2000_machine_usb1_state_get(Object *obj, Error **errp)
{
    if (sf2000_usb_link_active[1]) {
        return g_strdup("session-active");
    }
    return g_strdup(sf2000_usb_link_powered[1] ? "powered-disconnected"
                                               : "disconnected");
}

static char *sf2000_machine_usb0_devctl_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x",
                           (uint32_t)sf2000_usb_read(SF2000_USB0_BASE + 0x60, 4));
}

static char *sf2000_machine_usb1_devctl_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x",
                           (uint32_t)sf2000_usb_read(SF2000_USB1_BASE + 0x60, 4));
}

static char *sf2000_machine_usb0_power_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x",
                           (uint32_t)sf2000_usb_read(SF2000_USB0_BASE + 0x00, 4));
}

static char *sf2000_machine_usb1_power_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x",
                           (uint32_t)sf2000_usb_read(SF2000_USB1_BASE + 0x00, 4));
}

static char *sf2000_machine_usb0_utmi380_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x",
                           (uint32_t)sf2000_usb_read(SF2000_USB0_BASE + 0x380, 4));
}

static char *sf2000_machine_usb1_utmi380_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x",
                           (uint32_t)sf2000_usb_read(SF2000_USB1_BASE + 0x380, 4));
}

static char *sf2000_machine_usb0_phy384_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x",
                           (uint32_t)sf2000_usb_read(SF2000_USB0_BASE + 0x384, 4));
}

static char *sf2000_machine_usb1_phy384_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x",
                           (uint32_t)sf2000_usb_read(SF2000_USB1_BASE + 0x384, 4));
}

static char *sf2000_machine_usb_root_hub_id_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_spec()->usb_root_hub_id);
}

static char *sf2000_machine_usb_root_hub_ports_get(Object *obj, Error **errp)
{
    return g_strdup_printf("%u", sf2000_board_profile_spec()->usb_root_hub_ports);
}

static char *sf2000_machine_usb_ctl0_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_board_profile_spec()->usb_ctl0);
}

static char *sf2000_machine_usb_ctl1_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_board_profile_spec()->usb_ctl1);
}

static char *sf2000_machine_usb_phy0_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_board_profile_spec()->usb_phy0);
}

static char *sf2000_machine_usb_phy1_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_board_profile_spec()->usb_phy1);
}

static char *sf2000_machine_usb_phy2_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_board_profile_spec()->usb_phy2);
}

static char *sf2000_machine_usb_phy3_get(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", sf2000_board_profile_spec()->usb_phy3);
}

static char *sf2000_machine_storage_reset_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_spec()->storage_reset);
}

static void sf2000_audio_callback(void *opaque, int free)
{
    int16_t sample_buf[256 * 2];
    unsigned channels = sf2000_board_profile_spec()->audio_channels ?
                        sf2000_board_profile_spec()->audio_channels : 1;

    (void)opaque;

    if (!sf2000_audio_voice || !sf2000_audio_audible()) {
        return;
    }
    if (!sf2000_audio_callback_reported && sf2000_audio_dma_bytes) {
        sf2000_audio_callback_reported = true;
        info_report("sf2000: audio callback producer=%u consumer=%u queued=%u configured=%d ctl=%08x/%08x/%08x/%08x",
                    sf2000_audio_dma_producer, sf2000_audio_dma_consumer,
                    sf2000_audio_dma_queued, sf2000_audio_dma_configured(),
                    sf2000_audio_snd_ctl04, sf2000_audio_snd_ctl08,
                    sf2000_audio_snd_ctl0c, sf2000_audio_snd_ctl50);
    }

    while (free > 0) {
        size_t frames = MIN((size_t)free / (sizeof(int16_t) * channels),
                            ARRAY_SIZE(sample_buf) / channels);
        size_t bytes;
        bool dma_underrun = false;

        if (!frames) {
            break;
        }

        if (sf2000_audio_dma_bytes && sf2000_audio_dma_configured() &&
            (sf2000_audio_snd_ctl04 & BIT(16)) &&
            (sf2000_audio_snd_ctl08 & 1) &&
            (sf2000_audio_snd_ctl0c & 1) &&
            (sf2000_audio_snd_ctl50 & BIT(29))) {
            size_t available = sf2000_audio_dma_queued;
            size_t guest_bytes = MIN(frames * sizeof(int16_t), available);
            size_t first = MIN(guest_bytes,
                               sf2000_audio_dma_bytes -
                               sf2000_audio_dma_consumer);

            dma_underrun = guest_bytes < frames * sizeof(int16_t);
            cpu_physical_memory_read(sf2000_audio_dma_base +
                                     sf2000_audio_dma_consumer,
                                     sample_buf, first);
            if (first < guest_bytes) {
                cpu_physical_memory_read(sf2000_audio_dma_base,
                                         (uint8_t *)sample_buf + first,
                                         guest_bytes - first);
            }
            memset((uint8_t *)sample_buf + guest_bytes, 0,
                   frames * sizeof(int16_t) - guest_bytes);
            if (channels > 1) {
                for (size_t i = frames; i-- > 0;) {
                    int16_t sample = sample_buf[i];

                    for (unsigned ch = 0; ch < channels; ch++) {
                        sample_buf[i * channels + ch] = sample;
                    }
                }
            }
        } else if (sf2000_audio_dma_bytes) {
            memset(sample_buf, 0, frames * sizeof(int16_t) * channels);
        } else {
            for (size_t i = 0; i < frames; i++) {
                int16_t sample = sf2000_audio_render_sample();

                sample_buf[i * channels] = sample;
                for (unsigned ch = 1; ch < channels; ch++) {
                    sample_buf[i * channels + ch] = sample;
                }
                sf2000_audio_wave_phase += sf2000_audio_wave_step;
            }
        }

        bytes = AUD_write(sf2000_audio_voice, sample_buf,
                          frames * sizeof(int16_t) * channels);
        if (!bytes) {
            break;
        }
        if (sf2000_audio_dma_bytes && sf2000_audio_dma_configured() &&
            (sf2000_audio_snd_ctl04 & BIT(16)) &&
            (sf2000_audio_snd_ctl08 & 1) &&
            (sf2000_audio_snd_ctl0c & 1) &&
            (sf2000_audio_snd_ctl50 & BIT(29))) {
            size_t available = sf2000_audio_dma_queued;
            size_t guest_written = MIN((size_t)bytes / channels, available);
            uint32_t old_consumer = sf2000_audio_dma_consumer;
            uint32_t period_frames = 0;
            uint32_t period_bytes;

            sf2000_audio_dma_consumer =
                (sf2000_audio_dma_consumer + guest_written) %
                sf2000_audio_dma_bytes;
            sf2000_audio_dma_queued -= guest_written;
            /*
             * Physical HC1512 raises the SND0 service event substantially
             * more often than the programmed PCM period.  Keep it distinct
             * from the +0x0c period latch so guests must coalesce by cursor.
             */
            if (guest_written) {
                sf2000_audio_snd_ctl08 |= BIT(16);
                sf2000_mmio_set32(SF2000_AUDIO_I2S_BASE + 0x08,
                                  sf2000_audio_snd_ctl08);
            }
            sf2000_mmio_get32(SF2000_AUDIO_I2S_BASE + 0x5c,
                              &period_frames);
            period_frames &= 0xffffu;
            /* SND0 +0x5c is in PCM frames; mono S16 is two bytes/frame. */
            period_bytes = period_frames * sizeof(int16_t);
            if (period_bytes && guest_written &&
                old_consumer / period_bytes !=
                    sf2000_audio_dma_consumer / period_bytes) {
                sf2000_audio_snd_ctl0c |= BIT(8);
                sf2000_mmio_set32(SF2000_AUDIO_I2S_BASE + 0x0c,
                                  sf2000_audio_snd_ctl0c);
            }
            /*
             * HC1512 disarms SND0 after consuming an empty producer ring.
             * The vendor transfer path republishes the cursor and sets bit
             * 16 again.  Preserve that behavior so QEMU catches drivers that
             * only arm the engine once and then silently stall in hardware.
             */
            if (dma_underrun && sf2000_audio_dma_queued == 0) {
                sf2000_audio_snd_ctl04 &= ~BIT(16);
                sf2000_mmio_set32(SF2000_AUDIO_I2S_BASE + 0x04,
                                  sf2000_audio_snd_ctl04);
            }
            sf2000_update_irq();
        }
        free -= bytes;
        if (bytes < frames * sizeof(int16_t) * channels) {
            break;
        }
    }
}

static void sf2000_audio_pcm_selftest(void)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();
    bool saved_powered = sf2000_audio_powered;
    uint32_t saved_fade = sf2000_audio_i2s_fade90;
    uint32_t saved_phase = sf2000_audio_wave_phase;
    int16_t sample0;
    int16_t sample1;

    sf2000_audio_wave_phase = 0;
    sample0 = sf2000_audio_render_sample();
    sf2000_audio_wave_phase = 0x80000000u;
    sample1 = sf2000_audio_render_sample();
    if (sample0 != 0x0800 || sample1 != -0x0800) {
        error_report("sf2000: audio pcm selftest failed board=%s samples=%d/%d",
                     profile->name, sample0, sample1);
        goto restore;
    }

    qemu_log_mask(LOG_UNIMP,
                  "sf2000: audio pcm selftest ok board=%s sample0=%d sample1=%d\n",
                  profile->name, sample0, sample1);

restore:
    sf2000_audio_wave_phase = saved_phase;
    sf2000_audio_powered = saved_powered;
    sf2000_audio_i2s_fade90 = saved_fade;
    sf2000_audio_set_backend_active();
}

static void sf2000_audio_backend_init(MachineState *machine)
{
    struct audsettings settings = {
        .freq = sf2000_board_profile_spec()->audio_sample_rate_hz,
        .nchannels = sf2000_board_profile_spec()->audio_channels,
        .fmt = AUDIO_FORMAT_S16,
        .endianness = 0,
    };
    Error *local_err = NULL;

    if (settings.freq == 0 || settings.nchannels == 0) {
        warn_report("sf2000: audio backend skipped; invalid contract");
        return;
    }

    sf2000_audio_wave_step =
        (uint32_t)(((uint64_t)440 << 32) / settings.freq);
    sf2000_audio_volume = sf2000_board_profile_spec()->audio_volume;
    sf2000_audio_gain = sf2000_board_profile_spec()->audio_gain;

    if (machine->audiodev) {
        sf2000_audio_be = audio_be_by_name(machine->audiodev, &local_err);
    } else {
        sf2000_audio_be = audio_get_default_audio_be(&local_err);
    }
    if (!sf2000_audio_be) {
        warn_report("sf2000: audio backend unavailable: %s",
                    local_err ? error_get_pretty(local_err) : "unknown");
        if (local_err) {
            error_free(local_err);
        }
        return;
    }

    sf2000_audio_voice = AUD_open_out(sf2000_audio_be, sf2000_audio_voice,
                                      "sf2000.audio", NULL,
                                      sf2000_audio_callback, &settings);
    if (!sf2000_audio_voice) {
        warn_report("sf2000: could not open audio voice");
        return;
    }

    sf2000_audio_backend_ready = true;
    info_report("sf2000: audio backend ready route=%s open_route=%s sample_rate=%u channels=%u "
                "period=%u/%u",
                sf2000_board_profile_spec()->audio_route,
                sf2000_board_profile_spec()->audio_open_route,
                sf2000_board_profile_spec()->audio_sample_rate_hz,
                sf2000_board_profile_spec()->audio_channels,
                sf2000_board_profile_spec()->audio_period_frames,
                sf2000_board_profile_spec()->audio_periods);
    sf2000_audio_set_backend_active();
}

typedef struct SF2000RegDefault {
    hwaddr addr;
    uint32_t value;
} SF2000RegDefault;

static const SF2000RegDefault sf2000_reg_defaults[] = {
    /*
     * These defaults come from three sources: the real-device UniFrog hardware
     * probe logs, HCRTOS DTS/pinmux defaults, and stock firmware traces. They
     * are deliberately conservative: return values that keep firmware on the
     * HC15xx/SF2000 path, but still log unknown addresses so later probes can
     * replace table entries with real device models.
     */
    { 0x18800000, 0x1512a501 }, { 0x18800004, 0x00000000 },
    { 0x18800008, 0x00000000 }, { 0x1880000c, 0x00000000 },
    { 0x18800010, 0x00000000 }, { 0x18800014, 0x00000000 },
    { 0x18800018, 0x00000000 }, { 0x1880001c, 0x00000000 },
    { 0x18800024, 0xffff0000 },
    { 0x18800028, 0xffffffff }, { 0x1880002c, 0xffffffff },
    { 0x18800030, 0x00000000 }, { 0x18800034, 0x00000000 },
    { 0x18800038, 0x00480415 }, { 0x1880003c, 0x003c0008 },
    { 0x18800040, 0x00000000 },
    { 0x18800044, 0x00000100 }, { 0x18800048, 0xffbfff83 },
    { 0x1880004c, 0x00000000 },
    { 0x18800050, 0x07800102 }, { 0x18800054, 0x040004b2 },
    { 0x18800058, 0x140004ff }, { 0x1880005c, 0x00000000 },
    { 0x18800060, 0x00000f88 },
    { 0x18800064, 0x0bc04040 }, { 0x18800068, 0x02000000 },
    { 0x18800070, 0x80011717 }, { 0x18800074, 0x00400700 },
    { 0x18800078, 0x00083700 }, { 0x1880007c, 0x000c00c0 },
    { 0x18800080, 0x000023c0 }, { 0x18800084, 0xa00b4000 },
    { 0x18800094, 0x000d0000 }, { 0x188000a4, 0x00002419 },
    { 0x188000f0, 0x00000003 }, { 0x188000f4, 0x00000020 },
    { 0x188000f8, 0x00000020 }, { 0x188000fc, 0x00000000 },
    { 0x18800140, 0x18800140 }, { 0x18800144, 0x88ff4555 },
    { 0x18800148, 0x08182080 }, { 0x1880014c, 0x30303006 },
    { 0x18800150, 0x30030010 }, { 0x18800220, 0x01000002 },
    { 0x18800224, 0x0000ffff }, { 0x18800230, 0x81a20cc9 },
    { 0x18800348, 0xffff8183 }, { 0x18800350, 0x00000083 },
    { 0x18800354, 0x00080283 }, { 0x18800358, 0x0008feff },
    { 0x18800380, 0x81530900 }, { 0x18800384, 0x02000040 },
    { 0x18800388, 0x0000005f }, { 0x188003cc, 0x00005555 },
    { 0x18800470, 0x0021001a }, { 0x18800478, 0x011c000d },
    { 0x18800480, 0x0022001a }, { 0x188004a0, 0x06060000 },
    { 0x188004a4, 0x06060606 }, { 0x188004a8, 0x01060600 },
    { 0x188004ac, 0x01010101 }, { 0x188004b0, 0x04040404 },
    { 0x188004b4, 0x00000404 }, { 0x188004bc, 0x00000200 },
    { 0x188004c0, 0x00000001 }, { 0x188004e4, 0x07000101 },
    { 0x188004e8, 0x00000002 }, { 0x18800500, 0x06060000 },
    { 0x18800504, 0x00060606 }, { 0x18800508, 0x06060606 },
    { 0x1880050c, 0x00060606 }, { 0x1880a03c, 0x0000ff41 },
    { 0x1880a090, 0x008f0000 },
    { 0x18806000, 0x80020000 }, { 0x18806004, 0x00000000 },
    { 0x18806008, 0x00000000 }, { 0x18806010, 0x00e8c420 },
    { 0x18806020, 0x00000000 }, { 0x18806024, 0x00000000 },
    { 0x18806028, 0x00000000 },
    { 0x18808000, 0x00000015 }, { 0x18808004, 0x00122914 },
    { 0x18808008, 0x00650000 }, { 0x1880800c, 0x01300378 },
    { 0x18808080, 0x00020702 }, { 0x18808084, 0x00127002 },
    { 0x18808088, 0x00108080 }, { 0x1880808c, 0x00000004 },
    { 0x18818100, 0x0a160003 }, { 0x18818104, 0x00000000 },
    { 0x18818108, 0x00000000 }, { 0x1881810c, 0x00000000 },
    { 0x18818200, 0x80000180 }, { 0x18818204, 0x00000000 },
    { 0x18818208, 0x00000000 }, { 0x18818210, 0x00000100 },
    { 0x18818400, 0x00000000 }, { 0x18818404, 0x20001400 },
    { 0x18818408, 0x00000f2d }, { 0x1881840c, 0x00000001 },
    { 0x18818410, 0xc0010000 },
    { 0x18818420, 0x00000000 }, { 0x18818424, 0x05470547 },
    { 0x18818428, 0x00000090 },
    { 0x18818500, 0xffffd6e5 }, { 0x18818504, 0x00000000 },
    { 0x18818a00, 0xffffffff }, { 0x18818a04, 0x697703e7 },
    { 0x18818a10, 0xffffffff }, { 0x18818a14, 0x697703e7 },
    { 0x18818a20, 0x00000000 }, { 0x18818a24, 0x00000000 },
    { 0x18818a30, 0x00d783ae }, { 0x18818a34, 0x00000000 },
    { 0x18818a40, 0x00000000 }, { 0x18818a44, 0xffffffff },
    { 0x18818a50, 0x01af078b }, { 0x18818a54, 0x01af3910 },
    { 0x18818a60, 0x00000000 }, { 0x18818a64, 0x00000000 },
    { 0x18818a70, 0x00000000 }, { 0x18818a74, 0x00000000 },
    { 0x1882e098, 0xc1000103 }, { 0x1882e09c, 0x00000000 },
    { 0x1882e0a0, 0x00000000 },
    { 0x1884c000, 0x058de414 }, { 0x1884c004, 0xb3690000 },
    { 0x1884c008, 0x05000200 }, { 0x1884c00c, 0x00020000 },
    { 0x1884c010, 0x0009003f }, { 0x1884c014, 0xd17f0d00 },
    { 0x1884c018, 0x5b590001 }, { 0x1884c01c, 0x400e0032 },
    { 0x1884c020, 0x00fe1200 }, { 0x1884c024, 0x00fe1200 },
    { 0x1884c028, 0x00000200 }, { 0x1884c02c, 0x00000200 },
    { 0x1884c030, 0x0000000a }, { 0x1884c038, 0x44000000 },
    { 0x1884c03c, 0x0000ffff },
};

struct SF2000LCDState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QemuConsole *con;
    AddressSpace *as;

    uint64_t fb_base;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t control;
    uint32_t panel_te_hz;
    bool redraw;
    QEMUTimer *scanout_timer;
    uint32_t scanout_hash;
    bool scanout_hash_valid;
    uint64_t scanout_sequence;

    uint32_t gpio54;
    uint32_t gpio354;
    bool panel_wr;
    bool panel_rs;
    uint16_t panel_cmd;
    uint16_t panel_args[4];
    uint8_t panel_arg_count;
    uint16_t panel_data_count;
    uint16_t panel_x0;
    uint16_t panel_x1;
    uint16_t panel_y0;
    uint16_t panel_y1;
    uint16_t panel_x;
    uint16_t panel_y;
    uint8_t panel_madctl;
    uint8_t panel_ramctrl[2];
    uint8_t panel_ramctrl_count;
    uint8_t panel_readback[5];
    uint8_t panel_readback_len;
    uint8_t panel_readback_byte;
    uint8_t panel_readback_bit;
    bool panel_readback_active;
    uint32_t panel_cmd_count;
    uint32_t panel_pixel_count;
    uint32_t panel_pixels[SF2000_LCD_WIDTH * SF2000_LCD_HEIGHT];
    uint8_t gma_linebuf[8192];
    uint8_t gma_palette[1024];
    uint32_t gma_palette_addr;
    bool gma_palette_valid;
    uint8_t vou_latch_stage;
    bool vou_setup_seen;
    bool vou_unlatched_logged;
    bool panel_vsync_unconnected_logged;
    bool panel_clock_unconnected_logged;
    bool panel_clock_gated_logged;
    bool panel_vou_disconnected_logged;
    bool panel_ramwr_handoff_logged;
    bool panel_ramctrl_handoff_logged;
    bool panel_ramctrl_explicit;
    bool panel_rgb_handoff_synchronized;
    bool panel_rgb_handoff_order_logged;
};

static SF2000LCDState *sf2000_lcd;
static uint32_t sf2000_rgb565_lut[UINT16_MAX + 1u];
static bool sf2000_rgb565_lut_ready;
static uint32_t sf2000_rgb565_to_surface(uint16_t pix);
static void sf2000_gma_present(uint32_t dmba_addr);
static void sf2000_lcd_update(void *opaque);
static uint32_t sf2000_gma_present_block(SF2000LCDState *s, uint32_t dmba_addr,
                                         bool dump_frame);

static bool sf2000_panel_is_gb300(void)
{
    return strcmp(sf2000_board_profile_name(), "gb300") == 0;
}

static unsigned sf2000_panel_surface_width(const SF2000LCDState *s)
{
    return s->width ? s->width :
        (sf2000_panel_is_gb300() ? 240u : SF2000_LCD_WIDTH);
}

static unsigned sf2000_panel_surface_height(const SF2000LCDState *s)
{
    return s->height ? s->height :
        (sf2000_panel_is_gb300() ? 320u : SF2000_LCD_HEIGHT);
}

static unsigned sf2000_panel_raw_width(const SF2000LCDState *s)
{
    return sf2000_panel_is_gb300() ? 240u : sf2000_panel_surface_width(s);
}

static unsigned sf2000_panel_raw_height(const SF2000LCDState *s)
{
    return sf2000_panel_is_gb300() ? 320u : sf2000_panel_surface_height(s);
}

static uint32_t sf2000_panel_color(const SF2000LCDState *s, uint32_t color)
{
    uint32_t red;
    uint32_t blue;

    /* MADCTL bit 3 selects BGR ordering on the physical ST7789 bus. */
    if (!s || !(s->panel_madctl & BIT(3))) {
        return color;
    }
    red = (color >> 16) & 0xff;
    blue = color & 0xff;
    return (color & 0xff00ff00u) | blue << 16 | red;
}

static void sf2000_panel_map_raw(const SF2000LCDState *s,
                                 unsigned raw_x, unsigned raw_y,
                                 unsigned *surface_x, unsigned *surface_y)
{
    unsigned surface_width = sf2000_panel_surface_width(s);
    unsigned surface_height = sf2000_panel_surface_height(s);

    /* The GB300 uses ST7789 MADCTL=0x28: MV is set, with no MX/MY flip. */
    if (s && (s->panel_madctl & BIT(5))) {
        *surface_x = raw_y;
        *surface_y = raw_x;
    } else {
        *surface_x = raw_x;
        *surface_y = raw_y;
    }

    if (s && (s->panel_madctl & BIT(6))) {
        *surface_x = surface_width - 1u - *surface_x;
    }
    if (s && (s->panel_madctl & BIT(7))) {
        *surface_y = surface_height - 1u - *surface_y;
    }
}

static void sf2000_panel_store_raw(SF2000LCDState *s,
                                   unsigned raw_x, unsigned raw_y,
                                   uint32_t color)
{
    DisplaySurface *surface;
    unsigned x;
    unsigned y;

    if (!s || raw_x >= sf2000_panel_raw_width(s) ||
        raw_y >= sf2000_panel_raw_height(s)) {
        return;
    }
    sf2000_panel_map_raw(s, raw_x, raw_y, &x, &y);
    surface = qemu_console_surface(s->con);
    if (surface_bits_per_pixel(surface) != 32 ||
        surface_width(surface) != sf2000_panel_surface_width(s) ||
        surface_height(surface) != sf2000_panel_surface_height(s)) {
        qemu_console_resize(s->con, sf2000_panel_surface_width(s),
                            sf2000_panel_surface_height(s));
        surface = qemu_console_surface(s->con);
    }
    if (surface_bits_per_pixel(surface) != 32 ||
        x >= surface_width(surface) || y >= surface_height(surface)) {
        return;
    }
    ((uint32_t *)(surface_data(surface) + y * surface_stride(surface)))[x] =
        sf2000_panel_color(s, color);
    dpy_gfx_update(s->con, x, y, 1, 1);
}

static void sf2000_vou_track_latch(hwaddr addr, uint32_t value)
{
    SF2000LCDState *s = sf2000_lcd;

    if (!s) {
        return;
    }
    if (addr == 0x18808190 && value == 0x00800100) {
        s->vou_setup_seen = true;
    }

    /*
     * libviddrv's RGB setup is an edge-triggered state machine, not merely a
     * register file.  Track the smallest ordered set of transitions proven by
     * the stock trace so QEMU cannot hide a guest which writes only the final
     * values (physical hardware displays VPO's constant background then).
     */
    switch (s->vou_latch_stage) {
    case 0:
        if (addr == 0x18808000 && value == 0x00000011) {
            s->vou_latch_stage = 1;
        }
        break;
    case 1:
        if (addr == 0x18808000 && value == 0x00000001) {
            s->vou_latch_stage = 2;
        }
        break;
    case 2:
        if (addr == 0x1880807c && value == 0x00000000) {
            s->vou_latch_stage = 3;
        }
        break;
    case 3:
        if (addr == 0x1880807c && value == 0x00010000) {
            s->vou_latch_stage = 4;
        }
        break;
    case 4:
        if (addr == 0x18808080 && value == 0x00030702) {
            s->vou_latch_stage = 5;
        }
        break;
    case 5:
        if (addr == 0x18800078 && (value & 0x00083700) == 0x00083700) {
            s->vou_latch_stage = 6;
        }
        break;
    case 6:
        if (addr == 0x18800078 && (value & 0x00083700) == 0x00003700) {
            s->vou_latch_stage = 7;
        }
        break;
    case 7:
        if (addr == 0x188003c8 && (value & 0x00000777) == 0) {
            s->vou_latch_stage = 8;
        }
        break;
    case 8:
        if (addr == 0x18808000 && value == 0x00000015) {
            s->vou_latch_stage = 9;
        }
        break;
    case 9:
        if (addr == 0x188081ec && value == 0x00050000) {
            uint32_t dmba = 0;

            s->vou_latch_stage = 10;
            info_report("sf2000: VOU RGB compositor latch complete");
            /*
             * HC15 continuously scans the descriptor already latched by GMA.
             * MuFrog legitimately submits its first UI frame before the
             * asynchronous VOU setup finishes and does not ring the same
             * doorbell a second time merely because RGB became ready.
             */
            if (sf2000_mmio_get32(0x18808304, &dmba) && dmba) {
                sf2000_gma_present(dmba);
            }
        }
        break;
    default:
        break;
    }
}

static uint64_t sf2000_cpu_hz(void)
{
    const char *env = g_getenv("SF2000_CPU_HZ");
    uint64_t hz;

    if (!env || !*env) {
        return SF2000_CPU_DEFAULT_HZ;
    }

    hz = g_ascii_strtoull(env, NULL, 0);
    if (hz < SF2000_CPU_MIN_HZ || hz > SF2000_CPU_MAX_HZ) {
        warn_report("sf2000: ignoring invalid SF2000_CPU_HZ=%s, using %" PRIu64,
                    env, (uint64_t)SF2000_CPU_DEFAULT_HZ);
        return SF2000_CPU_DEFAULT_HZ;
    }

    return hz;
}

static struct {
    const char *kernel_filename;
    uint64_t kernel_entry;
    uint64_t dtb_vaddr;
    bool linux_elf;
} loaderparams;

typedef struct SF2000RegState {
    hwaddr addr;
    uint32_t value;
    bool valid;
} SF2000RegState;

typedef struct SF2000PWMChannel {
    uint16_t low_count;
    uint16_t high_count;
    bool polarity;
    bool enabled;
} SF2000PWMChannel;

static SF2000RegState sf2000_regs[256];
static qemu_irq sf2000_eirq3;
static uint32_t sf2000_timer_cnt[SF2000_TIMER_COUNT];
static uint32_t sf2000_timer_aim[SF2000_TIMER_COUNT];
static uint32_t sf2000_timer_ctrl[SF2000_TIMER_COUNT];
static int64_t sf2000_timer_deadline_ns[SF2000_TIMER_COUNT];
static int64_t sf2000_next_tick_ns;
static int64_t sf2000_next_vsync_ns;
static uint32_t sf2000_irq_enable1;
static uint32_t sf2000_irq_enable2;
static uint32_t sf2000_adc_ctrl[4];
static QEMUTimer *sf2000_wdt_timer;
static uint32_t sf2000_wdt_count;
static uint8_t sf2000_wdt_conf;
static uint8_t sf2000_bootrom_bytes[SF2000_BOOT_SIZE];
static QEMUTimer *sf2000_irq_poll_timer;
static QEMUTimer *sf2000_ge_timer;
static bool sf2000_ge_irq_pending;
static uint64_t sf2000_ge_submit_sequence;
static uint64_t sf2000_ge_irq_ack_sequence;
static uint32_t sf2000_ge_queue_min;
static MemoryRegion *sf2000_ram_region;
static bool sf2000_bootrom_ram_entry;
static uint8_t sf2000_sflash_cmd;
static uint32_t sf2000_sdio_arg;
static uint8_t sf2000_sdio_cmd;
static uint32_t sf2000_sdio_resp[4];
static uint32_t sf2000_sdio_dma_addr;
static uint32_t sf2000_sdio_dma_len;
static uint32_t sf2000_sdio_dma_wr_addr;
static uint32_t sf2000_sdio_dma_wr_len;
static bool sf2000_sdio_xfer_done;
static bool sf2000_sdio_xfer_busy;
static bool sf2000_sdio_write_active;
static uint8_t sf2000_sdio_pio_state;
static bool sf2000_sdio_irq_pending;
static bool sf2000_sdio_callback_pending;
static bool sf2000_sdio_app_cmd;
static uint8_t sf2000_sdio_bus_width;
static bool sf2000_sb_timer_irq_masked;
static BlockBackend *sf2000_sdio_blk;
static GHashTable *sf2000_sdio_synth_sectors;
static char sf2000_uart_line[2][256];
static uint32_t sf2000_uart_line_len[2];
static uint8_t sf2000_uart_ier[2];
static bool sf2000_uart_thri_pending[2];
static uint8_t sf2000_i2c_ier[2];
static uint8_t sf2000_i2c_isr[2];
static uint8_t sf2000_i2c_ier1[2];
static uint8_t sf2000_i2c_isr1[2];
static uint8_t sf2000_i2c_data[2];
static uint32_t sf2000_pwm_clk_ctrl;
static SF2000PWMChannel sf2000_pwm_channel[SF2000_PWM_CHANNELS];
static uint32_t sf2000_usb_regs[2][SF2000_USB_REG_COUNT];
static bool sf2000_usb_access_reported[2];
static uint8_t sf2000_irc_fifo_cfg;
static uint8_t sf2000_irc_ier;
static uint8_t sf2000_irc_isr;
static uint32_t sf2000_key_mask;
static unsigned sf2000_key_shift_index;
static uint8_t sf2000_rf_regs[256];
static bool sf2000_rf_cs;
static bool sf2000_rf_clk;
static bool sf2000_rf_have_reg;
static bool sf2000_rf_read_phase;
static uint8_t sf2000_rf_reg;
static uint8_t sf2000_rf_shift;
static uint8_t sf2000_rf_bit_count;
static uint8_t sf2000_rf_response;
static uint8_t sf2000_rf_response_bit;
static unsigned sf2000_gma_dump_count;
static unsigned sf2000_ge_queue_dump_count;
static uint32_t sf2000_active_gma[2];
static bool sf2000_gma_mask_armed[2];
static const char *sf2000_last_pc_landmark;
static unsigned sf2000_pc_sample_count;
static uint32_t sf2000_last_call_pc;
static bool sf2000_stock_security_patched;
static bool sf2000_stock_archive_path_patched;
static bool sf2000_stock_archive_access_patched;
static int sf2000_trace_pc_enabled_cache = -1;
static int sf2000_patch_security_enabled_cache = -1;
static int sf2000_patch_archive_path_enabled_cache = -1;
static int sf2000_patch_archive_access_enabled_cache = -1;
static uint32_t sf2000_last_progress_seq;
static bool sf2000_last_progress_valid;
static uint32_t sf2000_last_unifrog_trace_count;
static bool sf2000_last_unifrog_trace_valid;
static bool sf2000_audio_setup_logged;

typedef struct SF2000KeyMap {
    QKeyCode qcode;
    uint32_t mask;
} SF2000KeyMap;

typedef struct SF2000PCLandmark {
    uint32_t start;
    uint32_t end;
    const char *name;
} SF2000PCLandmark;

enum {
    SF2000_KEY_R = 0,
    SF2000_KEY_Y,
    SF2000_KEY_X,
    SF2000_KEY_L,
    SF2000_KEY_A,
    SF2000_KEY_B,
    SF2000_KEY_SELECT,
    SF2000_KEY_START,
    SF2000_KEY_UP,
    SF2000_KEY_DOWN,
    SF2000_KEY_LEFT,
    SF2000_KEY_RIGHT,
};

static const SF2000KeyMap sf2000_key_map[] = {
    { Q_KEY_CODE_RIGHT, BIT(SF2000_KEY_RIGHT) },
    { Q_KEY_CODE_LEFT, BIT(SF2000_KEY_LEFT) },
    { Q_KEY_CODE_UP, BIT(SF2000_KEY_UP) },
    { Q_KEY_CODE_DOWN, BIT(SF2000_KEY_DOWN) },
    { Q_KEY_CODE_RET, BIT(SF2000_KEY_START) },
    { Q_KEY_CODE_BACKSPACE, BIT(SF2000_KEY_SELECT) },
    { Q_KEY_CODE_Z, BIT(SF2000_KEY_B) },
    { Q_KEY_CODE_X, BIT(SF2000_KEY_A) },
    { Q_KEY_CODE_A, BIT(SF2000_KEY_Y) },
    { Q_KEY_CODE_S, BIT(SF2000_KEY_X) },
    { Q_KEY_CODE_Q, BIT(SF2000_KEY_L) },
    { Q_KEY_CODE_W, BIT(SF2000_KEY_R) },
};

static const SF2000PCLandmark sf2000_pc_landmarks[] = {
    /*
     * These ranges come from orig_firmware/symbols/bisrv_08_03_symbols.csv.
     * They are not used for emulation behavior; they are progress breadcrumbs
     * for long stock-firmware runs where a full instruction trace would be too
     * large. Keep them broad enough that a periodic sample can catch the task
     * even when it does not land exactly on a function entry.
     */
    { 0x802a8700, 0x802a876c, "file_open" },
    { 0x802a8800, 0x802a8850, "file_read" },
    { 0x802ad1d8, 0x802ad240, "fopen" },
    { 0x802ad34c, 0x802ad3c0, "fread" },
    { 0x802acbf4, 0x802acc40, "fclose" },
    { 0x80309264, 0x80309320, "i_start_thread" },
    { 0x80309324, 0x80309420, "os_create_thread" },
    { 0x8030962c, 0x80309830, "make_ready" },
    { 0x80309aa4, 0x80309ac0, "tds_main_dispatch" },
    { 0x80309ac0, 0x80309d64, "create_main_task" },
    { 0x80346248, 0x803462a4, "create_run_task" },
    { 0x803462f0, 0x80346324, "run_task" },
    { 0x80346324, 0x8034c248, "app_main" },
    { 0x8034ce14, 0x8034cf24, "run_blit_from_file" },
    { 0x8034d984, 0x8034de50, "run_menu_readdir_user" },
    { 0x8034e960, 0x8034f618, "run_emulator_menu" },
    { 0x8034f854, 0x80354f74, "run_menu" },
    { 0x8035a794, 0x8035f97c, "run_game" },
    { 0x8035f97c, 0x80365c34, "unwqw_decompress" },
    { 0x80355770, 0x80355b50, "security_check" },
    { 0x047c0050, 0x047d0000, "storage_probe" },
};

#define SF2000_PROGRESS_PHYS      0x07a00000ULL
#define SF2000_PROGRESS_MAGIC     0x52504653U
#define SF2000_PROGRESS_VERSION   1U
#define SF2000_PROGRESS_ENTRIES   1024U
#define SF2000_PROGRESS_NAME_LEN   32U

typedef struct SF2000ProgressEntry {
    uint32_t seq;
    uint32_t kind;
    uint32_t value;
    uint32_t name_ptr;
    char name[SF2000_PROGRESS_NAME_LEN];
} SF2000ProgressEntry;

typedef struct SF2000ProgressLog {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t write_index;
    uint32_t wrapped;
    uint32_t reserved[3];
    SF2000ProgressEntry entries[SF2000_PROGRESS_ENTRIES];
} SF2000ProgressLog;

#define SF2000_UNIFROG_TRACE_PHYS    0x07fff780ULL
#define SF2000_UNIFROG_TRACE_MAGIC   0x55464254U
#define SF2000_UNIFROG_TRACE_VERSION 1U
#define SF2000_UNIFROG_TRACE_ENTRIES 64U
#define SF2000_UNIFROG_TRACE_SIZE    24U

static hwaddr sf2000_guest_phys_addr(uint32_t vaddr)
{
    if (vaddr >= 0x80000000U && vaddr < 0xc0000000U) {
        return vaddr & 0x1fffffffU;
    }
    return vaddr;
}

static bool sf2000_env_flag(const char *name, int *cache)
{
    if (*cache < 0) {
        const char *env = g_getenv(name);

        *cache = env && env[0] && g_strcmp0(env, "0") != 0;
    }
    return *cache != 0;
}

static bool sf2000_trace_pc_enabled(void)
{
    return sf2000_env_flag("SF2000_TRACE_PC", &sf2000_trace_pc_enabled_cache);
}

static bool sf2000_patch_security_enabled(void)
{
    return sf2000_env_flag("SF2000_PATCH_STOCK_SECURITY",
                           &sf2000_patch_security_enabled_cache);
}

static bool sf2000_patch_archive_path_enabled(void)
{
    return sf2000_env_flag("SF2000_PATCH_STOCK_ARCHIVE_SHORTNAME",
                           &sf2000_patch_archive_path_enabled_cache);
}

static bool sf2000_patch_archive_access_enabled(void)
{
    return sf2000_env_flag("SF2000_PATCH_STOCK_ARCHIVE_ACCESS_WAIT",
                           &sf2000_patch_archive_access_enabled_cache);
}

static void sf2000_guest_read_string(uint32_t vaddr, char *buf, size_t len)
{
    hwaddr addr = sf2000_guest_phys_addr(vaddr);
    MemTxResult res;
    size_t i;

    if (!len) {
        return;
    }

    for (i = 0; i + 1 < len; i++) {
        uint8_t ch = address_space_ldub(&address_space_memory, addr + i,
                                        MEMTXATTRS_UNSPECIFIED, &res);
        if (res != MEMTX_OK || ch == 0) {
            break;
        }
        if (ch < 0x20 || ch > 0x7e) {
            buf[i] = '.';
        } else {
            buf[i] = ch;
        }
    }
    buf[i] = 0;
}

static void sf2000_trace_fw_call(uint32_t pc, MIPSCPU *cpu)
{
    bool stderr_trace = sf2000_trace_pc_enabled();
    char arg0[96];
    char arg1[32];
    uint32_t a0 = (uint32_t)cpu->env.active_tc.gpr[4];
    uint32_t a1 = (uint32_t)cpu->env.active_tc.gpr[5];
    uint32_t a2 = (uint32_t)cpu->env.active_tc.gpr[6];
    uint32_t a3 = (uint32_t)cpu->env.active_tc.gpr[7];

    if (!stderr_trace || sf2000_last_call_pc == pc) {
        return;
    }

    switch (pc) {
    case 0x802ad1d8: /* fopen(path, mode) */
        sf2000_guest_read_string(a0, arg0, sizeof(arg0));
        sf2000_guest_read_string(a1, arg1, sizeof(arg1));
        fprintf(stderr, "sf2000: fopen path='%s' mode='%s' ra=0x%08x\n",
                arg0, arg1, (uint32_t)cpu->env.active_tc.gpr[31]);
        sf2000_last_call_pc = pc;
        break;
    case 0x802ad34c: /* fread(ptr, size, nmemb, stream) */
        fprintf(stderr,
                "sf2000: fread dst=0x%08x size=%u count=%u stream=0x%08x ra=0x%08x\n",
                a0, a1, a2, a3, (uint32_t)cpu->env.active_tc.gpr[31]);
        sf2000_last_call_pc = pc;
        break;
    case 0x802acbf4: /* fclose(stream) */
        fprintf(stderr, "sf2000: fclose stream=0x%08x ra=0x%08x\n",
                a0, (uint32_t)cpu->env.active_tc.gpr[31]);
        sf2000_last_call_pc = pc;
        break;
    case 0x80355770:
        fprintf(stderr, "sf2000: security_check arg=0x%08x ra=0x%08x\n",
                a0, (uint32_t)cpu->env.active_tc.gpr[31]);
        sf2000_last_call_pc = pc;
        break;
    case 0x8035578c:
        fprintf(stderr,
                "sf2000: security_check after xmc_get_id v0=0x%08x gp=0x%08x ra=0x%08x\n",
                (uint32_t)cpu->env.active_tc.gpr[2],
                (uint32_t)cpu->env.active_tc.gpr[28],
                (uint32_t)cpu->env.active_tc.gpr[31]);
        sf2000_last_call_pc = pc;
        break;
    case 0x80346334:
        fprintf(stderr,
                "sf2000: app_main after security_check v0=0x%08x gp=0x%08x ra=0x%08x\n",
                (uint32_t)cpu->env.active_tc.gpr[2],
                (uint32_t)cpu->env.active_tc.gpr[28],
                (uint32_t)cpu->env.active_tc.gpr[31]);
        sf2000_last_call_pc = pc;
        break;
    default:
        sf2000_last_call_pc = 0;
        break;
    }
}

static void sf2000_trace_pc_landmark(void)
{
    CPUState *cs = first_cpu;
    MIPSCPU *cpu;
    uint32_t pc;
    bool stderr_trace = sf2000_trace_pc_enabled();
    unsigned i;

    if (!cs) {
        return;
    }

    cpu = MIPS_CPU(cs);
    pc = (uint32_t)cpu->env.active_tc.PC;
    sf2000_trace_fw_call(pc, cpu);
    for (i = 0; i < ARRAY_SIZE(sf2000_pc_landmarks); i++) {
        const SF2000PCLandmark *landmark = &sf2000_pc_landmarks[i];

        if (pc >= landmark->start && pc < landmark->end) {
            if (sf2000_last_pc_landmark != landmark->name) {
                if (stderr_trace) {
                    fprintf(stderr,
                            "sf2000: pc-landmark %s pc=0x%08x ra=0x%08x sp=0x%08x\n",
                            landmark->name, pc,
                            (uint32_t)cpu->env.active_tc.gpr[31],
                            (uint32_t)cpu->env.active_tc.gpr[29]);
                }
                if (stderr_trace) {
                    qemu_log_mask(LOG_UNIMP,
                                  "sf2000: pc-landmark %s pc=0x%08x ra=0x%08x sp=0x%08x\n",
                                  landmark->name, pc,
                                  (uint32_t)cpu->env.active_tc.gpr[31],
                                  (uint32_t)cpu->env.active_tc.gpr[29]);
                }
                sf2000_last_pc_landmark = landmark->name;
            }
            return;
        }
    }

    if (stderr_trace && (++sf2000_pc_sample_count % 1000) == 0) {
        fprintf(stderr, "sf2000: pc-sample pc=0x%08x ra=0x%08x sp=0x%08x\n",
                pc, (uint32_t)cpu->env.active_tc.gpr[31],
                (uint32_t)cpu->env.active_tc.gpr[29]);
    }
    sf2000_last_pc_landmark = NULL;
}

static bool sf2000_progress_read_u32(hwaddr addr, uint32_t *value)
{
    MemTxResult res;

    *value = address_space_ldl_le(&address_space_memory, addr,
                                  MEMTXATTRS_UNSPECIFIED, &res);
    return res == MEMTX_OK;
}

static void sf2000_progress_read_name(hwaddr addr, char *buf, size_t len)
{
    MemTxResult res;
    size_t i;

    if (!len) {
        return;
    }

    for (i = 0; i + 1 < len; i++) {
        uint8_t ch = address_space_ldub(&address_space_memory, addr + i,
                                        MEMTXATTRS_UNSPECIFIED, &res);

        if (res != MEMTX_OK || ch == 0) {
            break;
        }
        buf[i] = (ch < 0x20 || ch > 0x7e) ? '.' : (char)ch;
    }
    buf[i] = 0;
}

static void sf2000_trace_progress_log(void)
{
    hwaddr base = SF2000_PROGRESS_PHYS;
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t write_index;
    uint32_t wrapped;
    uint32_t start_seq;
    uint32_t end_seq;
    uint32_t current_seq;
    uint32_t slot;
    uint32_t entry_seq;
    uint32_t kind;
    uint32_t value;
    uint32_t name_ptr;
    char name[SF2000_PROGRESS_NAME_LEN + 1];

    if (!sf2000_progress_read_u32(base, &magic) ||
        magic != SF2000_PROGRESS_MAGIC) {
        sf2000_last_progress_valid = false;
        return;
    }
    if (!sf2000_progress_read_u32(base + 4, &version) ||
        version != SF2000_PROGRESS_VERSION ||
        !sf2000_progress_read_u32(base + 8, &seq) ||
        !sf2000_progress_read_u32(base + 12, &write_index) ||
        !sf2000_progress_read_u32(base + 16, &wrapped)) {
        return;
    }
    if (seq == 0) {
        return;
    }

    if (sf2000_last_progress_valid && seq < sf2000_last_progress_seq) {
        sf2000_last_progress_valid = false;
    }
    start_seq = sf2000_last_progress_valid ? sf2000_last_progress_seq + 1u : 1u;
    if (start_seq > seq) {
        sf2000_last_progress_seq = seq;
        sf2000_last_progress_valid = true;
        return;
    }

    end_seq = seq;
    if (end_seq - start_seq + 1u > SF2000_PROGRESS_ENTRIES) {
        start_seq = end_seq - SF2000_PROGRESS_ENTRIES + 1u;
    }

    for (current_seq = start_seq; current_seq <= end_seq; current_seq++) {
        uint32_t delta = end_seq - current_seq;
        hwaddr entry_base;

        if (write_index == 0) {
            slot = SF2000_PROGRESS_ENTRIES - 1u - delta;
        } else {
            slot = write_index - 1u - delta;
        }
        slot %= SF2000_PROGRESS_ENTRIES;
        entry_base = base + 0x20u + (hwaddr)slot * sizeof(SF2000ProgressEntry);

        if (!sf2000_progress_read_u32(entry_base, &entry_seq) ||
            !sf2000_progress_read_u32(entry_base + 4, &kind) ||
            !sf2000_progress_read_u32(entry_base + 8, &value) ||
            !sf2000_progress_read_u32(entry_base + 12, &name_ptr)) {
            break;
        }
        (void)name_ptr;
        sf2000_progress_read_name(entry_base + 16, name, sizeof(name));
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: storage-progress seq=%u wrapped=%u kind=0x%08x value=0x%08x name=%s\n",
                      entry_seq, wrapped, kind, value, name);
    }

    sf2000_last_progress_seq = seq;
    sf2000_last_progress_valid = true;
}

static const char *sf2000_unifrog_trace_name(uint32_t event)
{
    switch (event) {
    case 100: return "unifrog.main.start";
    case 101: return "unifrog.app_main.start";
    case 102: return "unifrog.module_init.begin";
    case 103: return "unifrog.module_init.done";
    case 104: return "unifrog.frontend_thread.start";
    case 105: return "unifrog.board_init.begin";
    case 106: return "unifrog.board_init.done";
    case 107: return "unifrog.storage.done";
    case 108: return "unifrog.log_reset.done";
    case 109: return "unifrog.js.begin";
    case 110: return "unifrog.fb_open.begin";
    case 111: return "unifrog.fb_clear.done";
    case 112: return "unifrog.boot_logo.done";
    case 200: return "sdk.pwm.probe_begin";
    case 202: return "sdk.pwm.register_done";
    case 210: return "sdk.backlight.probe_begin";
    case 211: return "sdk.backlight.default_off";
    case 220: return "sdk.lcd.probe_begin";
    case 221: return "sdk.lcd.display_begin";
    case 222: return "sdk.lcd.reset.done";
    case 223: return "sdk.lcd.panel_probe.done";
    case 224: return "sdk.lcd.init_sequence.done";
    case 225: return "sdk.lcd.display_on";
    case 226: return "sdk.lcd.rgb_on";
    case 240: return "sdk.fb.probe_begin";
    case 241: return "sdk.fb.alloc.done";
    case 242: return "sdk.fb.register.done";
    case 243: return "sdk.fb.gma.off";
    default: return "unknown";
    }
}

static void sf2000_trace_unifrog_log(void)
{
    hwaddr base = SF2000_UNIFROG_TRACE_PHYS;
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t i;

    if (!sf2000_progress_read_u32(base, &magic) ||
        magic != SF2000_UNIFROG_TRACE_MAGIC ||
        !sf2000_progress_read_u32(base + 4, &version) ||
        version != SF2000_UNIFROG_TRACE_VERSION ||
        !sf2000_progress_read_u32(base + 8, &count)) {
        sf2000_last_unifrog_trace_valid = false;
        return;
    }
    count = MIN(count, (uint32_t)SF2000_UNIFROG_TRACE_ENTRIES);
    if (!sf2000_last_unifrog_trace_valid ||
        count < sf2000_last_unifrog_trace_count) {
        sf2000_last_unifrog_trace_count = 0;
    }

    for (i = sf2000_last_unifrog_trace_count; i < count; i++) {
        hwaddr entry = base + 16 + (hwaddr)i * SF2000_UNIFROG_TRACE_SIZE;
        uint32_t seq;
        uint32_t event;
        uint32_t arg0;
        uint32_t arg1;
        uint32_t arg2;
        uint32_t r05;

        if (!sf2000_progress_read_u32(entry, &seq) ||
            !sf2000_progress_read_u32(entry + 4, &event) ||
            !sf2000_progress_read_u32(entry + 8, &arg0) ||
            !sf2000_progress_read_u32(entry + 12, &arg1) ||
            !sf2000_progress_read_u32(entry + 16, &arg2) ||
            !sf2000_progress_read_u32(entry + 20, &r05)) {
            break;
        }
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: unifrog-trace seq=%u event=%u name=%s arg0=0x%08x arg1=0x%08x arg2=0x%08x r05=0x%03x\n",
                      seq, event, sf2000_unifrog_trace_name(event),
                      arg0, arg1, arg2, r05);
    }
    sf2000_last_unifrog_trace_count = i;
    sf2000_last_unifrog_trace_valid = true;
}

static uint32_t sf2000_timer_ticks(void)
{
    return (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000);
}

static uint32_t sf2000_timer_ms(void)
{
    return (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000);
}

static void sf2000_patch_stock_security_check(void)
{
    static const uint8_t patch[] = {
        0x01, 0x00, 0x02, 0x24, /* li v0,1 */
        0x08, 0x00, 0xe0, 0x03, /* jr ra */
        0x00, 0x00, 0x00, 0x00, /* nop */
    };
    uint8_t current[sizeof(patch)];
    CPUState *cs = first_cpu;
    MIPSCPU *cpu;

    if (sf2000_stock_security_patched || !sf2000_patch_security_enabled()) {
        return;
    }
    if (!cs) {
        return;
    }
    cpu = MIPS_CPU(cs);
    if ((uint32_t)cpu->env.active_tc.PC >= 0x81000000U) {
        return;
    }

    /*
     * The stock ASD is stored with a 0x200-byte pack header. The bootloader
     * copies file offset N to physical N. The public symbol is 0x80355770,
     * which maps to physical/file offset 0x00355770.
     */
    cpu_physical_memory_read(0x00355770, current, sizeof(current));
    if (current[0] == 0xe0 && current[1] == 0xfe &&
        current[2] == 0xbd && current[3] == 0x27 &&
        current[4] == 0x98 && current[5] == 0xa0 &&
        current[6] == 0x84 && current[7] == 0x27) {
        cpu_physical_memory_write(0x00355770, patch, sizeof(patch));
        queue_tb_flush(cs);
        sf2000_stock_security_patched = true;
        warn_report("sf2000: patched stock security_check for diagnostic boot");
    }
}

static void sf2000_patch_stock_archive_path(void)
{
    static const char original[] = "%s/Resources/Archive.sys";
    static const char replacement[] = "%s/RESOUR~1/ARCHIVE.SYS";
    uint8_t current[sizeof(original)];
    CPUState *cs = first_cpu;
    MIPSCPU *cpu;

    if (sf2000_stock_archive_path_patched ||
        !sf2000_patch_archive_path_enabled()) {
        return;
    }
    if (!cs) {
        return;
    }
    cpu = MIPS_CPU(cs);
    if ((uint32_t)cpu->env.active_tc.PC >= 0x81000000U) {
        return;
    }

    /*
     * The stock launcher formats "%s/Resources/Archive.sys" from this
     * constant before polling fs_access(). Patching it to the FAT short-name
     * spelling is a diagnostic for VFAT/LFN lookup behavior in generated SD
     * images; it is not part of the normal machine model.
     */
    cpu_physical_memory_read(0x0099b11c, current, sizeof(current));
    if (memcmp(current, original, sizeof(original)) == 0) {
        uint8_t patched[sizeof(original)];

        memset(patched, 0, sizeof(patched));
        memcpy(patched, replacement, sizeof(replacement));
        cpu_physical_memory_write(0x0099b11c, patched, sizeof(patched));
        queue_tb_flush(cs);
        sf2000_stock_archive_path_patched = true;
        warn_report("sf2000: patched stock Archive.sys path to FAT short names for diagnostic boot");
    }
}

static void sf2000_patch_stock_archive_access_wait(void)
{
    static const uint8_t branch[] = {
        0xfa, 0xff, 0x40, 0x14, /* bnez v0,0x803463b8 */
        0x32, 0x00, 0x04, 0x24, /* li a0,50 (delay slot) */
    };
    static const uint8_t patch[] = {
        0x00, 0x00, 0x00, 0x00, /* nop */
        0x32, 0x00, 0x04, 0x24, /* preserve delay slot */
    };
    uint8_t current[sizeof(branch)];
    CPUState *cs = first_cpu;
    MIPSCPU *cpu;

    if (sf2000_stock_archive_access_patched ||
        !sf2000_patch_archive_access_enabled()) {
        return;
    }
    if (!cs) {
        return;
    }
    cpu = MIPS_CPU(cs);
    if ((uint32_t)cpu->env.active_tc.PC >= 0x81000000U) {
        return;
    }

    /*
     * Diagnostic only: skip the stock launcher's pre-menu wait for
     * fs_access("/mnt/sda1/Resources/Archive.sys") to succeed. This exposes
     * later boot dependencies while the exact FAT/VFS mismatch is isolated.
     */
    cpu_physical_memory_read(0x003463cc, current, sizeof(current));
    if (memcmp(current, branch, sizeof(branch)) == 0) {
        cpu_physical_memory_write(0x003463cc, patch, sizeof(patch));
        queue_tb_flush(cs);
        sf2000_stock_archive_access_patched = true;
        warn_report("sf2000: patched stock Archive.sys access wait for diagnostic boot");
    }
}

static bool sf2000_timer_is_sec_ms(unsigned index)
{
    return index < SF2000_TIMER_SEC_COUNT;
}

static bool sf2000_timer_decode(hwaddr full_addr, unsigned *index,
                                unsigned *offset)
{
    hwaddr timer_off;

    if (full_addr < SF2000_TIMER_BASE ||
        full_addr >= SF2000_TIMER_BASE + SF2000_TIMER_SIZE) {
        return false;
    }

    timer_off = full_addr - SF2000_TIMER_BASE;
    *index = timer_off / SF2000_TIMER_STEP;
    *offset = timer_off % SF2000_TIMER_STEP;
    return *index < SF2000_TIMER_COUNT;
}

static bool sf2000_timer5_configured(void)
{
    return (sf2000_timer_ctrl[SF2000_TIMER5_INDEX] & SF2000_TIMER_CTRL_EN) &&
           (sf2000_timer_ctrl[SF2000_TIMER5_INDEX] & SF2000_TIMER_CTRL_IEN);
}

static bool sf2000_timer_configured(unsigned index)
{
    return (sf2000_timer_ctrl[index] & SF2000_TIMER_CTRL_EN) &&
           (sf2000_timer_ctrl[index] & SF2000_TIMER_CTRL_IEN);
}

static bool sf2000_irq1_enabled(uint32_t mask)
{
    return (sf2000_irq_enable1 & mask) != 0;
}

static bool sf2000_uart_decode(hwaddr full_addr, unsigned *index,
                               unsigned *offset)
{
    if (full_addr >= SF2000_UART0_BASE &&
        full_addr < SF2000_UART0_BASE + SF2000_UART_SIZE) {
        *index = 0;
        *offset = full_addr - SF2000_UART0_BASE;
        return true;
    }
    if (full_addr >= SF2000_UART1_BASE &&
        full_addr < SF2000_UART1_BASE + SF2000_UART_SIZE) {
        *index = 1;
        *offset = full_addr - SF2000_UART1_BASE;
        return true;
    }
    return false;
}

static bool sf2000_uart_irq_pending(unsigned index)
{
    return sf2000_uart_thri_pending[index] &&
           (sf2000_uart_ier[index] & SF2000_UART_IER_THRI) != 0;
}

static bool sf2000_i2c_decode(hwaddr full_addr, unsigned *index,
                              unsigned *offset)
{
    if (full_addr >= SF2000_I2C0_BASE &&
        full_addr < SF2000_I2C0_BASE + SF2000_I2C_SIZE) {
        *index = 0;
        *offset = full_addr - SF2000_I2C0_BASE;
        return true;
    }
    if (full_addr >= SF2000_I2C1_BASE &&
        full_addr < SF2000_I2C1_BASE + SF2000_I2C_SIZE) {
        *index = 1;
        *offset = full_addr - SF2000_I2C1_BASE;
        return true;
    }
    return false;
}

static uint32_t sf2000_i2c_irq_mask(unsigned index)
{
    return index == 0 ? SF2000_I2C0_IRQ : SF2000_I2C1_IRQ;
}

static bool sf2000_i2c_irq_pending(unsigned index)
{
    return ((sf2000_i2c_isr[index] & sf2000_i2c_ier[index]) != 0) ||
           ((sf2000_i2c_isr1[index] & sf2000_i2c_ier1[index]) != 0);
}

static bool sf2000_usb_decode(hwaddr full_addr, unsigned *index,
                              unsigned *offset)
{
    if (full_addr >= SF2000_USB0_BASE &&
        full_addr < SF2000_USB0_BASE + SF2000_USB_SIZE) {
        *index = 0;
        *offset = full_addr - SF2000_USB0_BASE;
        return true;
    }
    if (full_addr >= SF2000_USB1_BASE &&
        full_addr < SF2000_USB1_BASE + SF2000_USB_SIZE) {
        *index = 1;
        *offset = full_addr - SF2000_USB1_BASE;
        return true;
    }
    return false;
}

static uint64_t sf2000_usb_read(hwaddr full_addr, unsigned size)
{
    unsigned index;
    unsigned offset;
    uint32_t value;

    if (!sf2000_usb_decode(full_addr, &index, &offset)) {
        return 0;
    }

    value = sf2000_usb_regs[index][offset >> 2];
    if (!sf2000_usb_access_reported[index]) {
        sf2000_usb_access_reported[index] = true;
        info_report("sf2000: usb%u controller access=read route=%s offset=0x%02x value=0x%08x",
                    index, index == 0 ? "micro-usb" : "usb-a",
                    offset, value);
    }
    /*
     * The HC15xx DTS exposes two MUSB-like host/peripheral controller windows.
     * On the SF2000 DB-B210 board USB0 is routed to the micro USB connector
     * and USB1 is routed to the USB-A connector. Hardware probes currently
     * show both HCRTOS root hubs as initialized and powered, but disconnected,
     * so this remains an idle shell: no cable/device is attached, endpoint
     * interrupt state is clear, and the session/connect status reads as
     * disconnected.
     */
    switch (offset) {
    case 0x00: /* FAddr/Power byte lane. */
        value = sf2000_usb_power_reg[index];
        break;
    case 0x02: /* IntrTx */
    case 0x04: /* IntrRx */
    case 0x06: /* IntrTxE */
    case 0x08: /* IntrRxE */
    case 0x0a: /* IntrUSB */
    case 0x0b: /* IntrUSBE */
    case 0x60: /* DevCtl */
        if (offset == 0x60) {
            value = sf2000_usb_devctl_reg[index];
        } else {
            value = sf2000_usb_link_active[index] ? 0x10 : 0;
        }
        break;
    default:
        break;
    }

    value >>= ((full_addr & 3u) * 8u);
    if (size < 4) {
        value &= (1u << (size * 8)) - 1u;
    }
    return value;
}

static const char *sf2000_usb_link_state_name(unsigned index)
{
    if (sf2000_usb_link_active[index]) {
        return "session-active";
    }
    return sf2000_usb_link_powered[index] ? "powered-disconnected"
                                          : "disconnected";
}

static void sf2000_usb_write(hwaddr full_addr, uint64_t value, unsigned size)
{
    unsigned index;
    unsigned offset;
    uint32_t old;
    uint32_t mask = size >= 4 ? 0xffffffffu : ((1u << (size * 8)) - 1u);
    unsigned shift = (full_addr & 3u) * 8u;

    if (!sf2000_usb_decode(full_addr, &index, &offset)) {
        return;
    }

    old = sf2000_usb_regs[index][offset >> 2];
    sf2000_usb_regs[index][offset >> 2] =
        (old & ~(mask << shift)) | (((uint32_t)value & mask) << shift);
    if (offset == 0x00) {
        bool powered = value != 0;
        bool active = (value & SF2000_MUSB_POWER_SOFTCONN) != 0;

        sf2000_usb_link_powered[index] = powered;
        sf2000_usb_link_active[index] = powered && active;
        sf2000_usb_power_reg[index] = powered ? (uint32_t)value
                                              : SF2000_MUSB_POWER_OFF_READ;
        sf2000_usb_devctl_reg[index] = sf2000_usb_link_active[index] ?
            0x81 : SF2000_MUSB_DEVCTL_OFF_READ;
    } else if (offset == 0x60) {
        bool powered = (value & SF2000_MUSB_DEVCTL_VBUS) != 0;
        bool active = (value & SF2000_MUSB_DEVCTL_SESSION) != 0;

        sf2000_usb_link_powered[index] = sf2000_usb_link_powered[index] || powered;
        sf2000_usb_link_active[index] = active;
        sf2000_usb_devctl_reg[index] = active ? 0x81 : SF2000_MUSB_DEVCTL_OFF_READ;
    }
    if (!sf2000_usb_access_reported[index]) {
        sf2000_usb_access_reported[index] = true;
        info_report("sf2000: usb%u controller access=write route=%s offset=0x%02x value=0x%08x",
                    index, index == 0 ? "micro-usb" : "usb-a",
                    offset, (uint32_t)value);
    }
    info_report("sf2000: usb%u state=%s", index, sf2000_usb_link_state_name(index));
}

static void sf2000_usb_reset_block_selftest(void)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();
    uint32_t usb0_power;
    uint32_t usb0_devctl;
    uint32_t usb1_power;
    uint32_t usb1_devctl;

    usb0_power = sf2000_usb_read(SF2000_USB0_BASE + 0x00, 4);
    usb0_devctl = sf2000_usb_read(SF2000_USB0_BASE + 0x60, 4);
    usb1_power = sf2000_usb_read(SF2000_USB1_BASE + 0x00, 4);
    usb1_devctl = sf2000_usb_read(SF2000_USB1_BASE + 0x60, 4);

    if (usb0_power != SF2000_MUSB_POWER_RESET_READ ||
        usb0_devctl != SF2000_MUSB_DEVCTL_RESET_READ ||
        usb1_power != SF2000_MUSB_POWER_RESET_READ ||
        usb1_devctl != SF2000_MUSB_DEVCTL_RESET_READ) {
        error_report("sf2000: usb reset block selftest failed board=%s "
                     "usb0=%08x/%08x usb1=%08x/%08x expected=%08x/%08x",
                     profile->name, usb0_power, usb0_devctl,
                     usb1_power, usb1_devctl,
                     SF2000_MUSB_POWER_RESET_READ,
                     SF2000_MUSB_DEVCTL_RESET_READ);
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                  "sf2000: usb reset block selftest ok board=%s usb0=0x%08x/0x%08x "
                  "usb1=0x%08x/0x%08x\n",
                  profile->name, usb0_power, usb0_devctl,
                  usb1_power, usb1_devctl);
}

static void sf2000_usb_link_state_selftest(void)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();
    bool saved_powered[2];
    bool saved_active[2];
    uint32_t saved_power_reg[2];
    uint32_t saved_devctl_reg[2];
    uint32_t value;
    unsigned index;

    for (index = 0; index < ARRAY_SIZE(saved_powered); index++) {
        saved_powered[index] = sf2000_usb_link_powered[index];
        saved_active[index] = sf2000_usb_link_active[index];
        saved_power_reg[index] = sf2000_usb_power_reg[index];
        saved_devctl_reg[index] = sf2000_usb_devctl_reg[index];
    }

    if (sf2000_usb_read(SF2000_USB0_BASE + 0x00, 4) != SF2000_MUSB_POWER_RESET_READ ||
        sf2000_usb_read(SF2000_USB0_BASE + 0x60, 4) != SF2000_MUSB_DEVCTL_RESET_READ ||
        sf2000_usb_read(SF2000_USB1_BASE + 0x00, 4) != SF2000_MUSB_POWER_RESET_READ ||
        sf2000_usb_read(SF2000_USB1_BASE + 0x60, 4) != SF2000_MUSB_DEVCTL_RESET_READ) {
        error_report("sf2000: usb link state selftest failed board=%s reset snapshot mismatch",
                     profile->name);
        goto restore;
    }

    sf2000_usb_write(SF2000_USB0_BASE + 0x00, SF2000_MUSB_POWER_OFF_READ, 4);
    sf2000_usb_write(SF2000_USB1_BASE + 0x00, SF2000_MUSB_POWER_OFF_READ, 4);
    if (sf2000_usb_read(SF2000_USB0_BASE + 0x00, 4) != SF2000_MUSB_POWER_OFF_READ ||
        sf2000_usb_read(SF2000_USB0_BASE + 0x60, 4) != SF2000_MUSB_DEVCTL_OFF_READ ||
        sf2000_usb_read(SF2000_USB1_BASE + 0x00, 4) != SF2000_MUSB_POWER_OFF_READ ||
        sf2000_usb_read(SF2000_USB1_BASE + 0x60, 4) != SF2000_MUSB_DEVCTL_OFF_READ ||
        strcmp(sf2000_usb_link_state_name(0), "disconnected") != 0 ||
        strcmp(sf2000_usb_link_state_name(1), "disconnected") != 0) {
        error_report("sf2000: usb link state selftest failed board=%s disconnected snapshot mismatch "
                     "state0=%s state1=%s",
                     profile->name,
                     sf2000_usb_link_state_name(0),
                     sf2000_usb_link_state_name(1));
        goto restore;
    }

    sf2000_usb_write(SF2000_USB0_BASE + 0x60, SF2000_MUSB_DEVCTL_VBUS, 4);
    sf2000_usb_write(SF2000_USB1_BASE + 0x60, SF2000_MUSB_DEVCTL_VBUS, 4);
    if (sf2000_usb_read(SF2000_USB0_BASE + 0x60, 4) != SF2000_MUSB_DEVCTL_OFF_READ ||
        sf2000_usb_read(SF2000_USB1_BASE + 0x60, 4) != SF2000_MUSB_DEVCTL_OFF_READ ||
        strcmp(sf2000_usb_link_state_name(0), "powered-disconnected") != 0 ||
        strcmp(sf2000_usb_link_state_name(1), "powered-disconnected") != 0) {
        error_report("sf2000: usb link state selftest failed board=%s powered->shell "
                     "state0=%s state1=%s",
                     profile->name,
                     sf2000_usb_link_state_name(0),
                     sf2000_usb_link_state_name(1));
        goto restore;
    }

    sf2000_usb_write(SF2000_USB0_BASE + 0x60,
                     SF2000_MUSB_DEVCTL_VBUS | SF2000_MUSB_DEVCTL_SESSION, 4);
    sf2000_usb_write(SF2000_USB1_BASE + 0x60,
                     SF2000_MUSB_DEVCTL_VBUS | SF2000_MUSB_DEVCTL_SESSION, 4);
    if (sf2000_usb_read(SF2000_USB0_BASE + 0x60, 4) != 0x81 ||
        sf2000_usb_read(SF2000_USB1_BASE + 0x60, 4) != 0x81) {
        error_report("sf2000: usb link state selftest failed board=%s session snapshot mismatch",
                     profile->name);
        goto restore;
    }
    if (strcmp(sf2000_usb_link_state_name(0), "session-active") != 0 ||
        strcmp(sf2000_usb_link_state_name(1), "session-active") != 0) {
        error_report("sf2000: usb link state selftest failed board=%s session->active "
                     "state0=%s state1=%s",
                     profile->name,
                     sf2000_usb_link_state_name(0),
                     sf2000_usb_link_state_name(1));
        goto restore;
    }

restore:
    sf2000_usb_link_powered[0] = saved_powered[0];
    sf2000_usb_link_powered[1] = saved_powered[1];
    sf2000_usb_link_active[0] = saved_active[0];
    sf2000_usb_link_active[1] = saved_active[1];
    sf2000_usb_power_reg[0] = saved_power_reg[0];
    sf2000_usb_power_reg[1] = saved_power_reg[1];
    sf2000_usb_devctl_reg[0] = saved_devctl_reg[0];
    sf2000_usb_devctl_reg[1] = saved_devctl_reg[1];
    sf2000_mmio_get32(SF2000_USB0_BASE + 0x00, &value);
    sf2000_mmio_get32(SF2000_USB1_BASE + 0x00, &value);
    qemu_log_mask(LOG_UNIMP,
                  "sf2000: usb link state selftest ok board=%s\n",
                  profile->name);
}

static void sf2000_usb_phy_snapshot_selftest(void)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();
    uint32_t usb0_utmi;
    uint32_t usb0_phy;
    uint32_t usb1_utmi;
    uint32_t usb1_phy;

    usb0_utmi = sf2000_usb_read(SF2000_USB0_BASE + 0x380, 4);
    usb0_phy = sf2000_usb_read(SF2000_USB0_BASE + 0x384, 4);
    usb1_utmi = sf2000_usb_read(SF2000_USB1_BASE + 0x380, 4);
    usb1_phy = sf2000_usb_read(SF2000_USB1_BASE + 0x384, 4);

    if (usb0_utmi != profile->usb_utmi380 ||
        usb0_phy != profile->usb_phy384 ||
        usb1_utmi != profile->usb_utmi380 ||
        usb1_phy != profile->usb_phy384) {
        error_report("sf2000: usb phy snapshot selftest failed board=%s "
                     "usb0=%08x/%08x usb1=%08x/%08x expected=%08x/%08x",
                     profile->name, usb0_utmi, usb0_phy, usb1_utmi, usb1_phy,
                     profile->usb_utmi380, profile->usb_phy384);
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                  "sf2000: usb phy snapshot selftest ok board=%s "
                  "usb0=0x%08x/0x%08x usb1=0x%08x/0x%08x\n",
                  profile->name, usb0_utmi, usb0_phy, usb1_utmi, usb1_phy);
}

static void sf2000_audio_hw_close_selftest(void)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();
    g_autofree char *hw_close = sf2000_machine_audio_hw_close_get(NULL, NULL);
    const char *expected =
        "backend=2 snd0=0x14fc0082 dac=0x420003a8 hw_ret=-1 "
        "dma=0x00000000/0 hw_rate=0 hw_ch=0 hw_fmt=0 hw_period=0 hw_periods=0";

    if (g_strcmp0(hw_close, expected) != 0) {
        error_report("sf2000: audio hw close selftest failed board=%s got=%s expected=%s",
                     profile->name, hw_close ? hw_close : "(null)", expected);
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                  "sf2000: audio hw close selftest ok board=%s %s\n",
                  profile->name, hw_close);
}

static void sf2000_pwm2_backlight_selftest(void)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();
    uint32_t pwm_clk_ctrl;
    uint32_t pwm2_lohi;
    uint32_t pwm2_ctrl;

    sf2000_mmio_get32(SF2000_PWM_BASE + SF2000_PWM_CLK_CTRL, &pwm_clk_ctrl);
    sf2000_mmio_get32(SF2000_PWM_BASE + SF2000_PWM_DIV_BASE +
                      2u * SF2000_PWM_CH_STRIDE, &pwm2_lohi);
    sf2000_mmio_get32(SF2000_PWM_BASE + SF2000_PWM_DIV_BASE +
                      2u * SF2000_PWM_CH_STRIDE + 4u, &pwm2_ctrl);
    if (pwm_clk_ctrl != 0xc0010000u || pwm2_lohi != 0x05470547u ||
        pwm2_ctrl != 0x00000090u) {
        error_report("sf2000: pwm2 backlight selftest failed board=%s "
                     "clk=0x%08x lohi=0x%08x ctrl=0x%08x expected=0xc0010000/0x05470547/0x00000090",
                     profile->name, pwm_clk_ctrl, pwm2_lohi, pwm2_ctrl);
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                  "sf2000: pwm2 backlight selftest ok board=%s clk=0x%08x lohi=0x%08x ctrl=0x%08x\n",
                  profile->name, pwm_clk_ctrl, pwm2_lohi, pwm2_ctrl);
}

static void sf2000_pwm2_backlight_blank_selftest(void)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();
    SF2000LCDState *s = sf2000_lcd;
    DisplaySurface *surface;
    uint8_t descriptor[40];
    uint8_t pixel[2] = { 0xff, 0xff };
    uint32_t saved_clk_ctrl;
    uint32_t saved_pwm2_ctrl;
    uint32_t saved_pixel = 0;
    uint32_t *dst;
    const hwaddr desc_addr = 0x00002000ULL;
    const hwaddr pixel_addr = 0x00003000ULL;
    uint32_t old;

    if (!s || !s->con) {
        error_report("sf2000: pwm2 backlight blank selftest failed board=%s no console",
                     profile->name);
        return;
    }

    surface = qemu_console_surface(s->con);
    if (surface_bits_per_pixel(surface) != 32 ||
        surface_width(surface) != sf2000_panel_surface_width(s) ||
        surface_height(surface) != sf2000_panel_surface_height(s)) {
        qemu_console_resize(s->con, sf2000_panel_surface_width(s),
                            sf2000_panel_surface_height(s));
        surface = qemu_console_surface(s->con);
    }
    if (surface_bits_per_pixel(surface) != 32) {
        error_report("sf2000: pwm2 backlight blank selftest failed board=%s surface-bpp=%u",
                     profile->name, surface_bits_per_pixel(surface));
        return;
    }

    dst = (uint32_t *)surface_data(surface);
    saved_pixel = dst[0];

    memset(descriptor, 0, sizeof(descriptor));
    /* RGB565, CLUT off, and bit 0 marks this as the final block. */
    stl_le_p(descriptor + 0, 0x00000061u);
    stl_le_p(descriptor + 4, 0x00000000u);
    stl_le_p(descriptor + 8, 0x00000000u);
    stl_le_p(descriptor + 12, 0x00000000u);
    stl_le_p(descriptor + 16, 0x00010001u);
    stl_le_p(descriptor + 20, 0x00020000u);
    stl_le_p(descriptor + 24, 0x00000000u);
    stl_le_p(descriptor + 28, (uint32_t)pixel_addr);
    stl_le_p(descriptor + 32, 0x00000000u);
    stl_le_p(descriptor + 36, 0x00000000u);

    cpu_physical_memory_write(desc_addr, descriptor, sizeof(descriptor));
    cpu_physical_memory_write(pixel_addr, pixel, sizeof(pixel));

    sf2000_mmio_get32(SF2000_PWM_BASE + SF2000_PWM_CLK_CTRL, &saved_clk_ctrl);
    sf2000_mmio_get32(SF2000_PWM_BASE + SF2000_PWM_DIV_BASE +
                      2u * SF2000_PWM_CH_STRIDE + 4u, &saved_pwm2_ctrl);

    (void)sf2000_gma_present_block(s, desc_addr, false);
    old = dst[0];
    if (old != 0xffffffffu) {
        error_report("sf2000: gma final-block selftest failed board=%s pixel=0x%08x",
                     profile->name, old);
        goto restore;
    }
    qemu_log_mask(LOG_UNIMP,
                  "sf2000: gma final-block selftest ok board=%s sample=0x%08x\n",
                  profile->name, old);

    sf2000_mmio_set32(SF2000_PWM_BASE + SF2000_PWM_CLK_CTRL,
                      saved_clk_ctrl & ~(SF2000_PWM_CLKEN | SF2000_PWM_ENABLE));
    sf2000_mmio_set32(SF2000_PWM_BASE + SF2000_PWM_DIV_BASE +
                      2u * SF2000_PWM_CH_STRIDE + 4u,
                      saved_pwm2_ctrl & ~SF2000_PWM_CH_ENABLE);

    if (sf2000_pwm2_backlight_active()) {
        error_report("sf2000: pwm2 backlight blank selftest failed board=%s backlight still active",
                     profile->name);
        goto restore;
    }

    (void)sf2000_gma_present_block(s, desc_addr, false);
    old = dst[0];
    if (old != 0xff000000u) {
        error_report("sf2000: pwm2 backlight blank selftest failed board=%s pixel=0x%08x",
                     profile->name, old);
        goto restore;
    }

    info_report("sf2000: pwm2 backlight blank selftest ok board=%s sample=0x%08x",
                profile->name, old);

restore:
    dst[0] = saved_pixel;
    dpy_gfx_update(s->con, 0, 0, 1, 1);
    sf2000_mmio_set32(SF2000_PWM_BASE + SF2000_PWM_CLK_CTRL, saved_clk_ctrl);
    sf2000_mmio_set32(SF2000_PWM_BASE + SF2000_PWM_DIV_BASE +
                      2u * SF2000_PWM_CH_STRIDE + 4u, saved_pwm2_ctrl);
}

static void sf2000_audio_state_selftest(void)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();
    bool saved_powered = sf2000_audio_powered;
    uint32_t saved_fade = sf2000_audio_i2s_fade90;
    bool after_power;
    bool after_fade;

    sf2000_audio_powered = false;
    sf2000_audio_i2s_fade90 = saved_fade;
    sf2000_audio_set_backend_active();
    if (sf2000_audio_output_active()) {
        error_report("sf2000: audio state selftest failed board=%s reset mute active",
                     profile->name);
        goto restore;
    }

    sf2000_audio_powered = true;
    sf2000_audio_i2s_fade90 = saved_fade;
    sf2000_audio_set_backend_active();
    after_power = sf2000_audio_output_active();
    sf2000_audio_i2s_fade90 = 0;
    sf2000_audio_set_backend_active();
    after_fade = sf2000_audio_output_active();
    if (!after_power || after_fade) {
        error_report("sf2000: audio state selftest failed board=%s power=%d fade=%d",
                     profile->name, after_power, after_fade);
        goto restore;
    }

    qemu_log_mask(LOG_UNIMP,
                  "sf2000: audio state selftest ok board=%s active=%s mute=%s\n",
                  profile->name, after_power ? "true" : "false",
                  after_fade ? "false" : "true");

restore:
    sf2000_audio_powered = saved_powered;
    sf2000_audio_i2s_fade90 = saved_fade;
    sf2000_audio_set_backend_active();
}

static void sf2000_audio_gate_live_selftest(void)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();
    bool saved_powered = sf2000_audio_powered;
    uint32_t saved_fade = sf2000_audio_i2s_fade90;
    uint32_t gate_l1;
    uint32_t gate_r1;

    sf2000_audio_powered = false;
    sf2000_audio_i2s_fade90 = saved_fade;
    sf2000_audio_set_backend_active();
    sf2000_audio_gate_live_words(profile, &gate_l1, &gate_r1);
    if (gate_l1 != profile->audio_gate_l1 || gate_r1 != profile->audio_gate_r1) {
        error_report("sf2000: audio gate live selftest failed board=%s reset gate mismatch",
                     profile->name);
        goto restore;
    }

    sf2000_audio_powered = true;
    sf2000_audio_i2s_fade90 = saved_fade;
    sf2000_audio_set_backend_active();
    sf2000_audio_gate_live_words(profile, &gate_l1, &gate_r1);
    if (gate_l1 != profile->audio_gate_l1_active ||
        gate_r1 != profile->audio_gate_r1_active) {
        error_report("sf2000: audio gate live selftest failed board=%s active gate mismatch",
                     profile->name);
        goto restore;
    }

    qemu_log_mask(LOG_UNIMP,
                  "sf2000: audio gate live selftest ok board=%s active_l1=0x%08x active_r1=0x%08x\n",
                  profile->name, profile->audio_gate_l1_active,
                  profile->audio_gate_r1_active);

restore:
    sf2000_audio_powered = saved_powered;
    sf2000_audio_i2s_fade90 = saved_fade;
    sf2000_audio_set_backend_active();
}

static void sf2000_audio_gate_live_variant_selftest(void)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();
    bool saved_powered = sf2000_audio_powered;
    uint32_t saved_fade = sf2000_audio_i2s_fade90;
    uint32_t gate_l1;
    uint32_t gate_r1;

    sf2000_audio_powered = false;
    sf2000_audio_i2s_fade90 = saved_fade;
    sf2000_audio_set_backend_active();
    sf2000_audio_gate_live_words(profile, &gate_l1, &gate_r1);
    if (gate_l1 != profile->audio_gate_l1 || gate_r1 != profile->audio_gate_r1) {
        error_report("sf2000: audio gate live variant selftest failed board=%s reset mismatch",
                     profile->name);
        goto restore;
    }

    sf2000_audio_powered = true;
    sf2000_audio_i2s_fade90 = saved_fade;
    sf2000_audio_set_backend_active();
    sf2000_audio_gate_live_words(profile, &gate_l1, &gate_r1);
    if (gate_l1 != profile->audio_gate_l1_active ||
        gate_r1 != profile->audio_gate_r1_active) {
        error_report("sf2000: audio gate live variant selftest failed board=%s active mismatch",
                     profile->name);
        goto restore;
    }

    qemu_log_mask(LOG_UNIMP,
                  "sf2000: audio gate live variant selftest ok board=%s reset_l1=0x%08x reset_r1=0x%08x active_l1=0x%08x active_r1=0x%08x\n",
                  profile->name, profile->audio_gate_l1, profile->audio_gate_r1,
                  profile->audio_gate_l1_active, profile->audio_gate_r1_active);

restore:
    sf2000_audio_powered = saved_powered;
    sf2000_audio_i2s_fade90 = saved_fade;
    sf2000_audio_set_backend_active();
}

static bool sf2000_pwm_decode(hwaddr full_addr, unsigned *channel,
                              unsigned *offset)
{
    hwaddr pwm_off;

    if (full_addr < SF2000_PWM_BASE ||
        full_addr >= SF2000_PWM_BASE + SF2000_PWM_SIZE) {
        return false;
    }

    pwm_off = full_addr - SF2000_PWM_BASE;
    if (pwm_off < SF2000_PWM_DIV_BASE) {
        *channel = SF2000_PWM_CHANNELS;
        *offset = pwm_off;
        return true;
    }

    pwm_off -= SF2000_PWM_DIV_BASE;
    *channel = pwm_off / SF2000_PWM_CH_STRIDE;
    *offset = pwm_off % SF2000_PWM_CH_STRIDE;
    return *channel < SF2000_PWM_CHANNELS;
}

static void sf2000_pwm_refresh_channel(unsigned channel)
{
    uint32_t counters = 0;
    uint32_t control = 0;

    if (channel >= SF2000_PWM_CHANNELS) {
        return;
    }

    sf2000_mmio_get32(SF2000_PWM_BASE + SF2000_PWM_DIV_BASE +
                      channel * SF2000_PWM_CH_STRIDE, &counters);
    sf2000_mmio_get32(SF2000_PWM_BASE + SF2000_PWM_DIV_BASE +
                      channel * SF2000_PWM_CH_STRIDE + 4, &control);

    sf2000_pwm_channel[channel].low_count = counters & 0xffffu;
    sf2000_pwm_channel[channel].high_count = counters >> 16;
    sf2000_pwm_channel[channel].polarity = (control & SF2000_PWM_POLARITY) != 0;
    sf2000_pwm_channel[channel].enabled = (control & SF2000_PWM_CH_ENABLE) != 0;
}

static void sf2000_pwm_write(hwaddr full_addr)
{
    unsigned channel;
    unsigned offset;

    if (!sf2000_pwm_decode(full_addr, &channel, &offset)) {
        return;
    }

    if (channel == SF2000_PWM_CHANNELS) {
        sf2000_mmio_get32(SF2000_PWM_BASE + SF2000_PWM_CLK_CTRL,
                          &sf2000_pwm_clk_ctrl);
        return;
    }

    sf2000_pwm_refresh_channel(channel);
}

static bool sf2000_irc_decode(hwaddr full_addr, unsigned *offset)
{
    if (full_addr < SF2000_IRC_BASE ||
        full_addr >= SF2000_IRC_BASE + SF2000_IRC_SIZE) {
        return false;
    }

    *offset = full_addr - SF2000_IRC_BASE;
    return true;
}

static bool sf2000_irc_irq_pending(void)
{
    return (sf2000_irc_isr & sf2000_irc_ier & SF2000_IRC_ISR_MASK) != 0;
}

static uint64_t sf2000_irc_read(hwaddr full_addr)
{
    unsigned offset;
    uint32_t value = 0;

    if (!sf2000_irc_decode(full_addr, &offset)) {
        return 0;
    }

    switch (offset) {
    case SF2000_IRC_FIFOCFG:
        /*
         * Bits 6:0 report FIFO occupancy to the SDK driver. With no emulated
         * IR pulses queued, preserve the configured threshold in storage but
         * expose an empty FIFO count.
         */
        value = 0;
        break;
    case SF2000_IRC_IER:
        value = sf2000_irc_ier;
        break;
    case SF2000_IRC_ISR:
        value = sf2000_irc_isr;
        break;
    case SF2000_IRC_FIFO:
        value = 0;
        break;
    default:
        sf2000_mmio_get32(full_addr, &value);
        value >>= ((full_addr & 3u) * 8u);
        break;
    }

    return value & 0xffu;
}

static void sf2000_irc_write(hwaddr full_addr, uint64_t value)
{
    unsigned offset;

    if (!sf2000_irc_decode(full_addr, &offset)) {
        return;
    }

    switch (offset) {
    case SF2000_IRC_FIFOCFG:
        sf2000_irc_fifo_cfg = value & ~SF2000_IRC_FIFOCFG_RESET;
        if (value & SF2000_IRC_FIFOCFG_RESET) {
            sf2000_irc_isr &= ~SF2000_IRC_ISR_MASK;
        }
        break;
    case SF2000_IRC_IER:
        sf2000_irc_ier = value & SF2000_IRC_ISR_MASK;
        break;
    case SF2000_IRC_ISR:
        sf2000_irc_isr &= ~(value & SF2000_IRC_ISR_MASK);
        break;
    default:
        break;
    }
}

static bool sf2000_gpio_l_vsync_enabled(void)
{
    uint32_t ier = 0;
    uint32_t ris_ier = 0;

    sf2000_mmio_get32(SF2000_GPIO_L_IER, &ier);
    sf2000_mmio_get32(SF2000_GPIO_L_RIS_IER, &ris_ier);
    return (ier & ris_ier & SF2000_GPIO_L08) != 0;
}

static bool sf2000_gpio_l_vsync_pending(void)
{
    uint32_t isr = 0;

    sf2000_mmio_get32(SF2000_GPIO_L_ISR, &isr);
    return (isr & SF2000_GPIO_L08) != 0;
}

static void sf2000_gpio_l_vsync_maybe_raise(void)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t isr = 0;
    uint32_t panel_te_hz = 60;

    if (now < sf2000_next_vsync_ns || !sf2000_gpio_l_vsync_enabled()) {
        return;
    }

    if (sf2000_lcd && sf2000_lcd->panel_te_hz) {
        panel_te_hz = sf2000_lcd->panel_te_hz;
    }

    /*
     * The ST7789V tearing-effect output is routed to PINPAD_L08 and requested
     * as a rising-edge GPIO interrupt by the stock LCD driver. Reuse the board
     * panel timing if available so wait-for-vsync follows the profile data.
     */
    sf2000_mmio_get32(SF2000_GPIO_L_ISR, &isr);
    sf2000_mmio_set32(SF2000_GPIO_L_ISR, isr | SF2000_GPIO_L08);
    sf2000_next_vsync_ns = now + NANOSECONDS_PER_SECOND / panel_te_hz;
}

static bool sf2000_timer_pending(unsigned index)
{
    uint32_t count;

    if (!sf2000_timer_configured(index)) {
        return false;
    }

    if (index == SF2000_TIMER5_INDEX &&
        sf2000_timer_deadline_ns[index]) {
        return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >=
               sf2000_timer_deadline_ns[index];
    }

    /*
     * TIMER0/TIMER1 are second/millisecond timers. Their +0 register is the
     * target seconds value and +4 carries the target milliseconds in bits
     * 0..9 plus tick-per-ms calibration in bits 16..31. They do not behave as
     * free-running compare timers. Treating an unprogrammed target as a normal
     * compare makes INT_ST stick forever after stock maincode enables them.
     */
    if (sf2000_timer_is_sec_ms(index)) {
        uint32_t target_ms = sf2000_timer_cnt[index] * 1000u +
                             (sf2000_timer_aim[index] & 0x3ffu);
        uint32_t elapsed_ms = sf2000_timer_ms();

        if (target_ms == 0) {
            return false;
        }

        return elapsed_ms - target_ms < 0x80000000u;
    }

    /* TIMER3 is also used as a free-running statistics counter. */
    if (!sf2000_timer_aim[index]) {
        return false;
    }

    count = sf2000_timer_ticks();
    return count - sf2000_timer_aim[index] < 0x80000000u;
}

static bool sf2000_timer5_pending(void)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!sf2000_irq1_enabled(SF2000_SB_TIMER_IRQ)) {
        return false;
    }

    /*
     * Stock and HCRTOS both route the scheduler through external interrupt
     * bank 1 bit 22, then into MIPS external interrupt line 3. Early stock boot
     * enables the bank bit before programming TIMER5 and idles around CP0
     * Count, so waiting for an exact hardware timer configuration deadlocks.
     * The synthetic tick keeps boot moving until the firmware writes TIMER5;
     * after that, compare-based behavior is used.
     */
    if (!sf2000_timer5_configured()) {
        return now >= sf2000_next_tick_ns;
    }

    return sf2000_timer_pending(SF2000_TIMER5_INDEX);
}

static bool sf2000_sb_timer_pending(void)
{
    unsigned i;

    if (!sf2000_irq1_enabled(SF2000_SB_TIMER_IRQ)) {
        return false;
    }

    if (sf2000_timer5_pending()) {
        return true;
    }

    /*
     * HCRTOS names bank-1 bit 22 as the south-bridge timer interrupt, not a
     * TIMER5-only interrupt. The stock menu path arms TIMER0 after the SD
     * mount callback and then waits for an IRQ-driven timeout wakeup before it
     * opens menu assets. Route any explicitly configured timer through the same
     * bank bit, while leaving unconfigured TIMER0/TIMER1 quiet so boot does not
     * see a stuck interrupt.
     */
    for (i = 0; i < SF2000_TIMER_COUNT; i++) {
        if (i == SF2000_TIMER5_INDEX) {
            continue;
        }
        if (sf2000_timer_pending(i)) {
            return true;
        }
    }

    return false;
}

static void sf2000_timer_ack(void)
{
    sf2000_next_tick_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          NANOSECONDS_PER_SECOND / 20;
    sf2000_timer_ctrl[SF2000_TIMER5_INDEX] &= ~SF2000_TIMER_CTRL_INT;
}

static void sf2000_update_irq(void)
{
    if (!sf2000_eirq3) {
        return;
    }

    sf2000_gpio_l_vsync_maybe_raise();

    qemu_set_irq(sf2000_eirq3,
                 (sf2000_gpio_l_vsync_pending() &&
                  sf2000_irq1_enabled(SF2000_GPIO_IRQ)) ||
                 (sf2000_uart_irq_pending(0) &&
                  sf2000_irq1_enabled(SF2000_UART0_IRQ)) ||
                 (sf2000_uart_irq_pending(1) &&
                  sf2000_irq1_enabled(SF2000_UART1_IRQ)) ||
                 (sf2000_i2c_irq_pending(0) &&
                  sf2000_irq1_enabled(SF2000_I2C0_IRQ)) ||
                 (sf2000_i2c_irq_pending(1) &&
                  sf2000_irq1_enabled(SF2000_I2C1_IRQ)) ||
                 (sf2000_irc_irq_pending() &&
                  sf2000_irq1_enabled(SF2000_IRC_IRQ)) ||
                 (sf2000_audio_irq_pending() &&
                  sf2000_irq1_enabled(SF2000_SND_IRQ)) ||
                 (sf2000_ge_irq_pending &&
                  sf2000_irq1_enabled(SF2000_GE_IRQ)) ||
                 (sf2000_sb_timer_pending() && !sf2000_sb_timer_irq_masked) ||
                 (sf2000_sdio_irq_pending &&
                  sf2000_irq1_enabled(SF2000_SDIO_IRQ)));
}

static void sf2000_irq_poll_timer_cb(void *opaque)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t next = now + NANOSECONDS_PER_SECOND;
    unsigned i;

    /*
     * The stock FreeRTOS port eventually idles in a tight branch loop and
     * expects the south-bridge timer interrupt to arrive without any MMIO
     * activity. Re-evaluate the level periodically so QEMU can assert EIRQ3
     * after virtual time advances instead of only during register accesses.
     */
    if (sf2000_trace_pc_enabled()) {
        sf2000_trace_pc_landmark();
    }
    sf2000_trace_progress_log();
    sf2000_trace_unifrog_log();
    if (sf2000_patch_security_enabled()) {
        sf2000_patch_stock_security_check();
    }
    if (sf2000_patch_archive_path_enabled()) {
        sf2000_patch_stock_archive_path();
    }
    if (sf2000_patch_archive_access_enabled()) {
        sf2000_patch_stock_archive_access_wait();
    }
    sf2000_update_irq();

    if (sf2000_trace_pc_enabled() ||
        (sf2000_patch_security_enabled() && !sf2000_stock_security_patched) ||
        (sf2000_patch_archive_path_enabled() &&
         !sf2000_stock_archive_path_patched) ||
        (sf2000_patch_archive_access_enabled() &&
         !sf2000_stock_archive_access_patched)) {
        next = now + NANOSECONDS_PER_SECOND / 1000;
    } else {
        if (sf2000_gpio_l_vsync_enabled() && sf2000_next_vsync_ns < next) {
            next = sf2000_next_vsync_ns;
        }
        if (sf2000_irq1_enabled(SF2000_SB_TIMER_IRQ)) {
            if (!sf2000_timer5_configured() && sf2000_next_tick_ns < next) {
                next = sf2000_next_tick_ns;
            }
            for (i = 0; i < SF2000_TIMER_COUNT; i++) {
                if (sf2000_timer_configured(i)) {
                    next = now + NANOSECONDS_PER_SECOND / 1000;
                    break;
                }
            }
        }
    }
    if (next <= now) {
        next = now + NANOSECONDS_PER_SECOND / 1000;
    }
    timer_mod(sf2000_irq_poll_timer,
              next);
}

static bool sf2000_trace_sdio(void)
{
    static int trace = -1;

    if (trace < 0) {
        const char *env = g_getenv("SF2000_TRACE_SDIO");

        trace = env && env[0] && g_strcmp0(env, "0") != 0;
    }
    return trace != 0;
}

static bool sf2000_trace_sysio(void)
{
    static int trace = -1;

    if (trace < 0) {
        const char *env = g_getenv("SF2000_TRACE_SYSIO");

        trace = env && env[0] && g_strcmp0(env, "0") != 0;
    }
    return trace != 0;
}

static bool sf2000_trace_aux(void)
{
    static int trace = -1;

    if (trace < 0) {
        const char *env = g_getenv("SF2000_TRACE_AUX");

        trace = env && env[0] && g_strcmp0(env, "0") != 0;
    }
    return trace != 0;
}

static bool sf2000_trace_gma(void)
{
    static int trace = -1;

    if (trace < 0) {
        const char *env = g_getenv("SF2000_TRACE_GMA");

        trace = env && env[0] && g_strcmp0(env, "0") != 0;
    }
    return trace != 0;
}

static bool sf2000_scanout_oracle(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *env = g_getenv("SF2000_SCANOUT_ORACLE");

        enabled = env && *env && strcmp(env, "0");
    }
    return enabled;
}

static bool sf2000_trace_sflash(void)
{
    static int trace = -1;

    if (trace < 0) {
        const char *env = g_getenv("SF2000_TRACE_SFLASH");

        trace = env && env[0] && g_strcmp0(env, "0") != 0;
    }
    return trace != 0;
}

static bool sf2000_sysio_quiet_addr(hwaddr addr)
{
    /*
     * SYSIO clock, reset, pinmux, GPIO-control, and interrupt-mask registers
     * are noisy during stock display/storage bring-up but are backed by the
     * generic read/write register store or explicit GPIO paths. Keep them
     * visible only when explicitly researching the SYSIO block.
     */
    return addr >= SF2000_MSYSIO_BASE && addr <= 0x1880051c;
}

static void sf2000_log_mmio(const char *kind, hwaddr addr, uint64_t value,
                            unsigned size)
{
    /*
     * These are intentionally modelled as hot polling registers. Logging every
     * byte poll makes the firmware trace much harder to mine for new gaps.
     */
    if ((addr >= SF2000_UART0_BASE &&
         addr < SF2000_UART0_BASE + SF2000_UART_SIZE) ||
        (addr >= SF2000_IRC_BASE &&
         addr < SF2000_IRC_BASE + SF2000_IRC_SIZE) ||
        (addr >= SF2000_UART1_BASE &&
         addr < SF2000_UART1_BASE + SF2000_UART_SIZE) ||
        (addr >= SF2000_I2C0_BASE &&
         addr < SF2000_I2C0_BASE + SF2000_I2C_SIZE) ||
        (addr >= SF2000_I2C1_BASE &&
         addr < SF2000_I2C1_BASE + SF2000_I2C_SIZE) ||
        (addr >= SF2000_PWM_BASE &&
         addr < SF2000_PWM_BASE + SF2000_PWM_SIZE) ||
        (addr == SF2000_MSYSIO_BASE && g_str_equal(kind, "mmio-read")) ||
        (addr >= SF2000_AVPHY_BASE &&
         addr < SF2000_AVPHY_BASE + SF2000_AVPHY_SIZE) ||
        (addr >= SF2000_GE_BASE && addr < SF2000_GE_BASE + SF2000_GE_SIZE) ||
        (addr >= SF2000_GMA_BASE &&
         addr < SF2000_GMA_BASE + SF2000_GMA_SIZE) ||
        (addr >= SF2000_ADC_BASE && addr < SF2000_ADC_BASE + SF2000_ADC_SIZE) ||
        (addr >= SF2000_WDT_BASE && addr < SF2000_WDT_BASE + SF2000_WDT_SIZE) ||
        (addr >= SF2000_SYSCLK_EXT_BASE &&
         addr < SF2000_SYSCLK_EXT_BASE + SF2000_SYSCLK_EXT_SIZE) ||
        (addr >= SF2000_SND_DAC_BASE &&
         addr < SF2000_SND_DAC_BASE + SF2000_SND_DAC_SIZE) ||
        (addr >= SF2000_AUDIO_I2S_BASE &&
         addr < SF2000_AUDIO_I2S_BASE + SF2000_AUDIO_I2S_SIZE) ||
        (addr >= SF2000_DSC_BOOT_BASE &&
         addr < SF2000_DSC_BOOT_BASE + SF2000_DSC_BOOT_SIZE) ||
        (addr >= 0x1884c000 && addr < 0x1884c040 &&
         !sf2000_trace_sdio()) ||
        (addr >= SF2000_AUX_BASE && addr < SF2000_AUX_BASE + SF2000_AUX_SIZE &&
         !sf2000_trace_aux()) ||
        (sf2000_sysio_quiet_addr(addr) && !sf2000_trace_sysio()) ||
        (addr >= 0x1882e098 && addr < 0x1882e0a4) ||
        addr == SF2000_GPIO_L_OUT || addr == 0x18800354 ||
        addr == 0x18800058 || addr == 0x18800358 ||
        addr == 0x1880a038 || addr == 0x1880a03a ||
        addr == SF2000_AUDIO_I2S_CTRL3C || addr == SF2000_AUDIO_I2S_FADE90 ||
        (addr == SF2000_GPIO_L_IN && g_str_equal(kind, "mmio-read")) ||
        (addr == SF2000_GPIO_L_ISR && g_str_equal(kind, "mmio-read")) ||
        (addr == SF2000_GPIO_R_IN && g_str_equal(kind, "mmio-read")) ||
        (addr == SF2000_GPIO_R_ISR && g_str_equal(kind, "mmio-read")) ||
        addr == SF2000_IRQ_STATUS1 || addr == SF2000_IRQ_STATUS2 ||
        (addr >= SF2000_TIMER_BASE &&
         addr < SF2000_TIMER_BASE + SF2000_TIMER_SIZE)) {
        return;
    }

    qemu_log_mask(LOG_UNIMP, "sf2000: %s addr=0x%08" HWADDR_PRIx
                  " size=%u value=0x%08" PRIx64 "\n",
                  kind, addr, size, value);
}

static bool sf2000_mmio_get32(hwaddr addr, uint32_t *value)
{
    unsigned i;

    addr &= ~3u;
    for (i = 0; i < ARRAY_SIZE(sf2000_regs); i++) {
        if (sf2000_regs[i].valid && sf2000_regs[i].addr == addr) {
            *value = sf2000_regs[i].value;
            return true;
        }
    }
    for (i = 0; i < ARRAY_SIZE(sf2000_reg_defaults); i++) {
        if (sf2000_reg_defaults[i].addr == addr) {
            *value = sf2000_reg_defaults[i].value;
            return true;
        }
    }
    *value = 0;
    return false;
}

static void sf2000_mmio_set32(hwaddr addr, uint32_t value)
{
    unsigned i;
    unsigned empty = ARRAY_SIZE(sf2000_regs);

    addr &= ~3u;
    for (i = 0; i < ARRAY_SIZE(sf2000_regs); i++) {
        if (sf2000_regs[i].valid && sf2000_regs[i].addr == addr) {
            sf2000_regs[i].value = value;
            return;
        }
        if (!sf2000_regs[i].valid && empty == ARRAY_SIZE(sf2000_regs)) {
            empty = i;
        }
    }
    if (empty != ARRAY_SIZE(sf2000_regs)) {
        sf2000_regs[empty].addr = addr;
        sf2000_regs[empty].value = value;
        sf2000_regs[empty].valid = true;
    }
}

static bool sf2000_ge_decode(hwaddr full_addr)
{
    return full_addr >= SF2000_GE_BASE &&
           full_addr < SF2000_GE_BASE + SF2000_GE_SIZE;
}

static unsigned sf2000_ge_dump_limit(void)
{
    const char *limit_s = g_getenv("SF2000_GE_DUMP_LIMIT");
    unsigned limit;

    if (!limit_s || !limit_s[0]) {
        return SF2000_GE_QUEUE_DUMP_DEFAULT;
    }
    limit = g_ascii_strtoull(limit_s, NULL, 0);
    return limit;
}

static unsigned sf2000_ge_node_dump_limit(void)
{
    const char *limit_s = g_getenv("SF2000_GE_NODE_DUMP_LIMIT");
    unsigned limit;

    if (!limit_s || !limit_s[0]) {
        return 0;
    }
    limit = g_ascii_strtoull(limit_s, NULL, 0);
    return limit;
}

static bool sf2000_trace_ge(void)
{
    return sf2000_ge_dump_limit() > 0;
}

static bool sf2000_ge_read_node(uint32_t first, uint32_t *node,
                                uint32_t words)
{
    uint32_t i;

    for (i = 0; i < words; i++) {
        if (address_space_read(&address_space_memory, first + i * 4u,
                               MEMTXATTRS_UNSPECIFIED, &node[i],
                               sizeof(node[i])) != MEMTX_OK) {
            return false;
        }
        node[i] = le32_to_cpu(node[i]);
    }
    return true;
}

static void sf2000_ge_memset(uint32_t dst, uint8_t value, size_t len)
{
    uint8_t buf[4096];
    size_t done = 0;

    if (!dst || !len) {
        return;
    }
    memset(buf, value, sizeof(buf));
    while (done < len) {
        size_t chunk = MIN(sizeof(buf), len - done);

        address_space_write(&address_space_memory, dst + done,
                            MEMTXATTRS_UNSPECIFIED, buf, chunk);
        done += chunk;
    }
}

static void sf2000_ge_memcpy(uint32_t dst, uint32_t src, size_t len)
{
    uint8_t *buf;

    if (!dst || !src || !len) {
        return;
    }
    buf = g_malloc(len);
    if (address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED,
                           buf, len) == MEMTX_OK) {
        address_space_write(&address_space_memory, dst,
                            MEMTXATTRS_UNSPECIFIED, buf, len);
    }
    g_free(buf);
}

static void sf2000_ge_rgb16_fill(uint32_t dst, uint32_t pitch,
                                 uint32_t xy, uint32_t wh, uint16_t color)
{
    uint32_t x = xy & 0xfff;
    uint32_t y = xy >> 16 & 0xfff;
    uint32_t width = wh & 0xfff;
    uint32_t height = wh >> 16 & 0xfff;
    uint16_t *line;
    uint32_t row;
    uint32_t column;

    if (!dst || !pitch || !width || !height || width > 4095) {
        return;
    }
    line = g_new(uint16_t, width);
    for (column = 0; column < width; column++) {
        line[column] = cpu_to_le16(color);
    }
    for (row = 0; row < height; row++) {
        address_space_write(&address_space_memory,
                            dst + (y + row) * pitch + x * 2u,
                            MEMTXATTRS_UNSPECIFIED, line, width * 2u);
    }
    g_free(line);
}

static void sf2000_ge_rgb16_blit(uint32_t dst, uint32_t dst_pitch,
                                 uint32_t src, uint32_t src_pitch,
                                 uint32_t dst_xy, uint32_t wh,
                                 uint32_t src_xy)
{
    uint32_t dx = dst_xy & 0xfff;
    uint32_t dy = dst_xy >> 16 & 0xfff;
    uint32_t sx = src_xy & 0xfff;
    uint32_t sy = src_xy >> 16 & 0xfff;
    uint32_t width = wh & 0xfff;
    uint32_t height = wh >> 16 & 0xfff;
    size_t line_size = (size_t)width * 2u;
    uint8_t *image;
    uint32_t row;

    if (!dst || !src || !dst_pitch || !src_pitch || !width || !height) {
        return;
    }
    image = g_malloc(line_size * height);
    for (row = 0; row < height; row++) {
        if (address_space_read(&address_space_memory,
                               src + (sy + row) * src_pitch + sx * 2u,
                               MEMTXATTRS_UNSPECIFIED,
                               image + row * line_size,
                               line_size) != MEMTX_OK) {
            g_free(image);
            return;
        }
    }
    for (row = 0; row < height; row++) {
        address_space_write(&address_space_memory,
                            dst + (dy + row) * dst_pitch + dx * 2u,
                            MEMTXATTRS_UNSPECIFIED,
                            image + row * line_size, line_size);
    }
    g_free(image);
}

static void sf2000_ge_rgb16_stretch(uint32_t dst, uint32_t dst_pitch,
                                    uint32_t src, uint32_t src_pitch,
                                    uint32_t dst_wh, uint32_t src_wh)
{
    uint32_t dst_width = dst_wh & 0xfff;
    uint32_t dst_height = dst_wh >> 16 & 0xfff;
    uint32_t src_width = src_wh & 0xfff;
    uint32_t src_height = src_wh >> 16 & 0xfff;
    uint16_t *src_image;
    uint16_t *dst_line;
    uint32_t x;
    uint32_t y;

    if (!dst || !src || !dst_pitch || !src_pitch || !dst_width ||
        !dst_height || !src_width || !src_height) {
        return;
    }
    src_image = g_new(uint16_t, (size_t)src_width * src_height);
    dst_line = g_new(uint16_t, dst_width);
    for (y = 0; y < src_height; y++) {
        if (address_space_read(&address_space_memory, src + y * src_pitch,
                               MEMTXATTRS_UNSPECIFIED,
                               src_image + (size_t)y * src_width,
                               src_width * 2u) != MEMTX_OK) {
            g_free(dst_line);
            g_free(src_image);
            return;
        }
    }
    for (y = 0; y < dst_height; y++) {
        uint32_t source_y = (uint64_t)y * src_height / dst_height;

        for (x = 0; x < dst_width; x++) {
            uint32_t source_x = (uint64_t)x * src_width / dst_width;

            dst_line[x] = src_image[(size_t)source_y * src_width + source_x];
        }
        address_space_write(&address_space_memory, dst + y * dst_pitch,
                            MEMTXATTRS_UNSPECIFIED, dst_line,
                            dst_width * 2u);
    }
    g_free(dst_line);
    g_free(src_image);
}

static const uint8_t sf2000_ge_group_words[20] = {
    1, 2, 2, 2, 2, 3, 1, 2, 2, 1, 2, 2, 0, 2, 2, 1, 0, 1, 7, 128,
};

static unsigned sf2000_ge_format_bytes(uint32_t context)
{
    switch ((context >> 12) & 0x1f) {
    case 0: /* XRGB8888 */
    case 1: /* ARGB8888 */
        return 4;
    case 5: /* ARGB1555 */
    case 6: /* RGB565 */
    case 3: /* ARGB4444 */
        return 2;
    case 12: /* CLUT8, used by stock firmware */
    case 29: /* A8 source mask */
        return 1;
    default:
        return 0;
    }
}

static uint32_t sf2000_ge_expand_pixel(uint32_t value, unsigned format)
{
    switch (format) {
    case 0:
        return value | 0xff000000u;
    case 1:
        return value;
    case 3:
        return ((value >> 12 & 0xf) * 0x11u << 24) |
               ((value >> 8 & 0xf) * 0x11u << 16) |
               ((value >> 4 & 0xf) * 0x11u << 8) |
               ((value & 0xf) * 0x11u);
    case 5:
        return ((value & 0x8000u) ? 0xff000000u : 0) |
               ((value >> 10 & 0x1f) * 255u / 31u << 16) |
               ((value >> 5 & 0x1f) * 255u / 31u << 8) |
               ((value & 0x1f) * 255u / 31u);
    case 6:
        return 0xff000000u |
               ((value >> 11 & 0x1f) * 255u / 31u << 16) |
               ((value >> 5 & 0x3f) * 255u / 63u << 8) |
               ((value & 0x1f) * 255u / 31u);
    case 29:
        return (value & 0xffu) << 24;
    default:
        return value;
    }
}

static uint32_t sf2000_ge_read_pixel(uint32_t address, uint32_t context)
{
    uint32_t value = 0;
    unsigned format = context >> 12 & 0x1f;
    unsigned bytes = sf2000_ge_format_bytes(context);

    if (!bytes || address_space_read(&address_space_memory, address,
            MEMTXATTRS_UNSPECIFIED, &value, bytes) != MEMTX_OK) {
        return 0;
    }
    return sf2000_ge_expand_pixel(le32_to_cpu(value), format);
}

static void sf2000_ge_write_pixel(uint32_t address, uint32_t context,
                                  uint32_t argb)
{
    uint32_t value;
    unsigned format = context >> 12 & 0x1f;
    unsigned bytes = sf2000_ge_format_bytes(context);

    switch (format) {
    case 0:
    case 1:
        value = argb;
        break;
    case 3:
        value = (argb >> 28 & 0xf) << 12 | (argb >> 20 & 0xf) << 8 |
                (argb >> 12 & 0xf) << 4 | (argb >> 4 & 0xf);
        break;
    case 5:
        value = (argb >> 31) << 15 | (argb >> 19 & 0x1f) << 10 |
                (argb >> 11 & 0x1f) << 5 | (argb >> 3 & 0x1f);
        break;
    case 6:
        value = (argb >> 19 & 0x1f) << 11 |
                (argb >> 10 & 0x3f) << 5 | (argb >> 3 & 0x1f);
        break;
    default:
        value = argb;
        break;
    }
    value = cpu_to_le32(value);
    if (bytes) {
        address_space_write(&address_space_memory, address,
                            MEMTXATTRS_UNSPECIFIED, &value, bytes);
    }
}

static unsigned sf2000_ge_decode_groups(const uint32_t *node, unsigned words,
                                        const uint32_t *groups[20])
{
    uint32_t mask;
    unsigned offset = 1;
    unsigned group;

    memset(groups, 0, sizeof(*groups) * 20);
    if (!words || (node[0] & 0xff000000u) != 0x02000000u) {
        return 0;
    }
    mask = node[0] & 0x000fffffu;
    for (group = 0; group < 20; group++) {
        if (!(mask & (1u << group))) {
            continue;
        }
        if (offset + sf2000_ge_group_words[group] > words) {
            return 0;
        }
        groups[group] = node + offset;
        offset += sf2000_ge_group_words[group];
    }
    return offset;
}

static unsigned sf2000_ge_grouped_node_words(uint32_t header)
{
    uint32_t mask;
    unsigned words = 1;
    unsigned group;

    if ((header & 0xff000000u) != 0x02000000u) {
        return 0;
    }
    mask = header & 0x000fffffu;
    for (group = 0; group < 20; group++) {
        if (mask & (1u << group)) {
            words += sf2000_ge_group_words[group];
        }
    }
    return words;
}

static unsigned sf2000_ge_argb_channel(uint32_t argb, unsigned channel)
{
    return argb >> (channel * 8) & 0xff;
}

static unsigned sf2000_ge_blend_factor(unsigned function, unsigned channel,
                                       uint32_t source, uint32_t destination)
{
    unsigned source_alpha = source >> 24;
    unsigned destination_alpha = destination >> 24;
    unsigned source_channel = sf2000_ge_argb_channel(source, channel);
    unsigned destination_channel = sf2000_ge_argb_channel(destination, channel);

    switch (function) {
    case 1: /* ZERO */
        return 0;
    case 2: /* ONE */
        return 255;
    case 3: /* SRCCOLOR */
        return channel == 3 ? source_alpha : source_channel;
    case 4: /* INVSRCCOLOR */
        return 255 - (channel == 3 ? source_alpha : source_channel);
    case 5: /* SRCALPHA */
        return source_alpha;
    case 6: /* INVSRCALPHA */
        return 255 - source_alpha;
    case 7: /* DESTALPHA */
        return destination_alpha;
    case 8: /* INVDESTALPHA */
        return 255 - destination_alpha;
    case 9: /* DESTCOLOR */
        return channel == 3 ? destination_alpha : destination_channel;
    case 10: /* INVDESTCOLOR */
        return 255 - (channel == 3 ? destination_alpha : destination_channel);
    case 11: /* SRCALPHASAT */
        return channel == 3 ? 255 : MIN(source_alpha,
                                        255 - destination_alpha);
    default:
        return 0;
    }
}

static uint32_t sf2000_ge_modulate_rgb(uint32_t argb, uint32_t color)
{
    unsigned alpha = argb >> 24;
    unsigned red = (argb >> 16 & 0xff) * (color >> 16 & 0xff) / 255;
    unsigned green = (argb >> 8 & 0xff) * (color >> 8 & 0xff) / 255;
    unsigned blue = (argb & 0xff) * (color & 0xff) / 255;

    return alpha << 24 | red << 16 | green << 8 | blue;
}

static uint32_t sf2000_ge_premultiply(uint32_t argb, unsigned alpha)
{
    unsigned red = (argb >> 16 & 0xff) * alpha / 255;
    unsigned green = (argb >> 8 & 0xff) * alpha / 255;
    unsigned blue = (argb & 0xff) * alpha / 255;

    return (argb & 0xff000000u) | red << 16 | green << 8 | blue;
}

static uint32_t sf2000_ge_apply_compositor(uint32_t rop, uint32_t global_color,
                                          uint32_t source,
                                          uint32_t destination)
{
    unsigned blend_selector = rop >> 9 & 7;
    bool colorize = false;
    unsigned channel;
    uint32_t result = source;

    if (rop & 0x00008000u) {
        if (blend_selector >= 3) {
            colorize = true;
            blend_selector -= 3;
        }
        if (blend_selector == 1) {
            result = (result & 0x00ffffffu) | (global_color & 0xff000000u);
        } else if (blend_selector == 2) {
            unsigned alpha = (result >> 24) * (global_color >> 24) / 255;

            result = (result & 0x00ffffffu) | alpha << 24;
        }
    } else if (blend_selector == 3) {
        colorize = true;
    }
    if (colorize) {
        result = sf2000_ge_modulate_rgb(result, global_color);
    }
    if (rop & 0x00000100u) {
        result = sf2000_ge_premultiply(result, global_color >> 24);
    }
    if (rop & 0x00001000u) {
        result = sf2000_ge_premultiply(result, result >> 24);
    }
    if (rop & 0x00002000u) {
        destination = sf2000_ge_premultiply(destination, destination >> 24);
    }
    if (rop & 0x00008000u) {
        uint32_t blended = 0;
        unsigned source_function = rop >> 4 & 0xf;
        unsigned destination_function = rop & 0xf;

        for (channel = 0; channel < 4; channel++) {
            unsigned source_value = sf2000_ge_argb_channel(result, channel);
            unsigned destination_value = sf2000_ge_argb_channel(destination,
                                                                 channel);
            unsigned source_factor = sf2000_ge_blend_factor(source_function,
                channel, result, destination);
            unsigned destination_factor = sf2000_ge_blend_factor(
                destination_function, channel, result, destination);
            unsigned value = (source_value * source_factor +
                              destination_value * destination_factor + 127) /
                             255;

            blended |= MIN(value, 255) << (channel * 8);
        }
        result = blended;
    }
    if ((rop & 0x00040000u) && (result >> 24)) {
        unsigned alpha = result >> 24;
        unsigned red = MIN(255u, (result >> 16 & 0xff) * 255u / alpha);
        unsigned green = MIN(255u, (result >> 8 & 0xff) * 255u / alpha);
        unsigned blue = MIN(255u, (result & 0xff) * 255u / alpha);

        result = alpha << 24 | red << 16 | green << 8 | blue;
    }
    if (rop & 0x00080000u) {
        result ^= destination;
    }
    return result;
}

static bool sf2000_ge_source_key_allows(uint32_t rop, uint32_t pixel,
                                        uint32_t key)
{
    uint32_t mode = rop & 0xf0c00000u;

    switch (mode) {
    case 0xe0400000u:
        return (pixel & 0x00ffffffu) == (key & 0x00ffffffu);
    case 0xe0800000u:
        return (pixel & 0x00ffffffu) != (key & 0x00ffffffu);
    case 0x10800000u:
        return (pixel >> 24) == (key >> 24);
    case 0x10400000u:
        return (pixel >> 24) != (key >> 24);
    default:
        return true;
    }
}

static bool sf2000_ge_destination_key_allows(uint32_t rop, uint32_t pixel,
                                             uint32_t key)
{
    uint32_t mode = rop & 0x0f300000u;

    switch (mode) {
    case 0x0e100000u:
        return (pixel & 0x00ffffffu) == (key & 0x00ffffffu);
    case 0x0e200000u:
        return (pixel & 0x00ffffffu) != (key & 0x00ffffffu);
    case 0x01200000u:
        return (pixel >> 24) == (key >> 24);
    case 0x01100000u:
        return (pixel >> 24) != (key >> 24);
    default:
        return true;
    }
}

static void sf2000_ge_execute_grouped_node(uint32_t *node, uint32_t words)
{
    const uint32_t *group[20];
    const uint32_t *source;
    uint32_t dst_address;
    uint32_t src_address;
    uint32_t dst_context;
    uint32_t src_context;
    uint32_t dst_xy;
    uint32_t dst_wh;
    uint32_t src_xy;
    uint32_t src_wh;
    uint32_t rop;
    uint32_t global_color;
    unsigned dst_bytes;
    unsigned src_bytes;
    unsigned dx, dy, dw, dh, sx, sy, sw, sh, x, y;
    unsigned mask_x = 0;
    unsigned mask_y = 0;
    uint32_t mask_address = 0;
    uint32_t mask_context = 0;
    unsigned rotation = 0;

    if (!sf2000_ge_decode_groups(node, words, group) || !group[0] ||
        !group[1] || !group[8]) {
        return;
    }
    rop = group[15] ? group[15][0] : 0x00004000u;
    global_color = group[17] ? group[17][0] : 0xffffffffu;
    dst_address = group[1][0];
    dst_context = group[1][1];
    dst_bytes = sf2000_ge_format_bytes(dst_context);
    dst_xy = group[8][0];
    dst_wh = group[8][1];
    dx = dst_xy & 0xfff;
    dy = dst_xy >> 16 & 0xfff;
    dw = dst_wh & 0xfff;
    dh = dst_wh >> 16 & 0xfff;
    if (!dst_address || !dst_bytes || !dw || !dh) {
        return;
    }

    /* Preserve the common no-effect solid rectangle fast path. */
    if ((group[0][0] & 0x3f) == 3 && group[5] && !group[7] &&
        rop == 0x00030000u) {
        uint32_t raw = group[5][0];
        uint32_t pitch = (dst_context & 0xfff) * dst_bytes;

        for (y = 0; y < dh; y++) {
            for (x = 0; x < dw; x++) {
                uint32_t address = dst_address + (dy + y) * pitch +
                                   (dx + x) * dst_bytes;
                uint32_t value = cpu_to_le32(raw);

                address_space_write(&address_space_memory, address,
                                    MEMTXATTRS_UNSPECIFIED, &value, dst_bytes);
            }
        }
        return;
    }

    /* Functions 1 and 3 paint an ARGB or native-format solid color. */
    if (((group[0][0] & 0x3f) == 1 ||
         (group[0][0] & 0x3f) == 3) && group[5]) {
        uint32_t source_color = group[5][0];
        uint32_t pitch = (dst_context & 0xfff) * dst_bytes;

        if ((group[0][0] & 0x3f) == 3 && group[6]) {
            source_color = sf2000_ge_expand_pixel(source_color,
                                                   group[6][0] & 0x1f);
        }
        for (y = 0; y < dh; y++) {
            for (x = 0; x < dw; x++) {
                uint32_t address = dst_address + (dy + y) * pitch +
                                   (dx + x) * dst_bytes;
                uint32_t destination_color = sf2000_ge_read_pixel(address,
                                                                   dst_context);

                if (group[7] && !sf2000_ge_destination_key_allows(rop,
                        destination_color, sf2000_ge_expand_pixel(group[7][0],
                            dst_context >> 12 & 0x1f))) {
                    continue;
                }
                sf2000_ge_write_pixel(address, dst_context,
                    sf2000_ge_apply_compositor(rop, global_color,
                                               source_color,
                                               destination_color));
            }
        }
        return;
    }

    /* Direct copies use SRC; composited/scaled copies use PTN. */
    source = group[3] ? group[3] : group[2];
    if (!source) {
        return;
    }
    src_address = source[0];
    src_context = source[1];
    src_bytes = sf2000_ge_format_bytes(src_context);
    if (!src_address || !src_bytes) {
        return;
    }
    if (group[10]) {
        src_xy = group[10][0];
        src_wh = group[10][1];
    } else {
        src_xy = group[9] ? group[9][0] : 0;
        src_wh = dst_wh;
    }
    sx = src_xy & 0xfff;
    sy = src_xy >> 16 & 0xfff;
    sw = src_wh & 0xfff;
    sh = src_wh >> 16 & 0xfff;
    if (!sw || !sh) {
        return;
    }
    if (group[14]) {
        dst_xy = group[14][0];
        dst_wh = group[14][1];
        dx = dst_xy & 0xfff;
        dy = dst_xy >> 16 & 0xfff;
        dw = dst_wh & 0xfff;
        dh = dst_wh >> 16 & 0xfff;
    }
    if (group[18]) {
        static const uint32_t rotate_90[7] = {
            0, 0, 0x8000ffffu, 0, 0x0000ffffu, 0, 0,
        };
        static const uint32_t rotate_180[7] = {
            0, 0x8000ffffu, 0x80000000u, 0,
            0, 0x8000ffffu, 0,
        };
        static const uint32_t rotate_270[7] = {
            0, 0x80000000u, 0x0000ffffu, 0,
            0x8000ffffu, 0x80000000u, 0,
        };

        if (!memcmp(group[18], rotate_90, sizeof(rotate_90))) {
            rotation = 90;
        } else if (!memcmp(group[18], rotate_180, sizeof(rotate_180))) {
            rotation = 180;
        } else if (!memcmp(group[18], rotate_270, sizeof(rotate_270))) {
            rotation = 270;
        }
    }
    if (group[4] && ((group[4][1] >> 12) & 0x1f) == 29) {
        mask_address = group[4][0];
        mask_context = group[4][1];
        if (group[11]) {
            mask_x = group[11][0] & 0xfff;
            mask_y = group[11][0] >> 16 & 0xfff;
        } else {
            mask_x = sx;
            mask_y = sy;
        }
    }
    for (y = 0; y < dh; y++) {
        unsigned source_y = sy + (uint64_t)y * sh / dh;

        if (src_context & 0x00200000u) {
            source_y = sy + sh - 1u - (source_y - sy);
        }
        for (x = 0; x < dw; x++) {
            unsigned source_x = sx + (uint64_t)x * sw / dw;
            uint32_t argb;
            uint32_t destination_argb;
            uint32_t destination_pixel;

            if (rotation == 90) {
                source_x = sx + (uint64_t)y * sw / dh;
                source_y = sy + sh - 1u - (uint64_t)x * sh / dw;
            } else if (rotation == 180) {
                source_x = sx + sw - 1u - (uint64_t)x * sw / dw;
                source_y = sy + sh - 1u - (uint64_t)y * sh / dh;
            } else if (rotation == 270) {
                source_x = sx + sw - 1u - (uint64_t)y * sw / dh;
                source_y = sy + (uint64_t)x * sh / dw;
            }

            if (src_context & 0x00100000u) {
                source_x = sx + sw - 1u - (source_x - sx);
            }
            argb = sf2000_ge_read_pixel(src_address +
                    source_y * (src_context & 0xfff) * src_bytes +
                    source_x * src_bytes, src_context);
            if (mask_address) {
                uint8_t alpha = 0;
                unsigned mx = mask_x + source_x - sx;
                unsigned my = mask_y + source_y - sy;

                if (address_space_read(&address_space_memory,
                        mask_address + my * (mask_context & 0xfff) + mx,
                        MEMTXATTRS_UNSPECIFIED, &alpha, sizeof(alpha)) !=
                        MEMTX_OK) {
                    alpha = 0;
                }
                argb = (argb & 0x00ffffffu) |
                       (((argb >> 24) * alpha + 127u) / 255u) << 24;
            }
            destination_pixel = dst_address +
                (dy + y) * (dst_context & 0xfff) * dst_bytes +
                (dx + x) * dst_bytes;
            destination_argb = sf2000_ge_read_pixel(destination_pixel,
                                                     dst_context);
            if (group[7] && !sf2000_ge_source_key_allows(rop, argb,
                                                          group[7][1])) {
                continue;
            }
            if (group[7] && !sf2000_ge_destination_key_allows(rop,
                    destination_argb, group[7][0])) {
                continue;
            }
            argb = sf2000_ge_apply_compositor(rop, global_color, argb,
                                               destination_argb);
            sf2000_ge_write_pixel(destination_pixel, dst_context, argb);
        }
    }
}

static void sf2000_ge_execute_node(uint32_t *node, uint32_t words)
{
    uint32_t dst;
    uint32_t src;
    uint32_t fmt;
    uint32_t width;
    uint32_t height;
    size_t len;

    if (words == 14 && node[0] == 0x02008367 && node[1] == 0x00a00003 &&
        ((node[3] >> 12) & 0x1f) == 6) {
        sf2000_ge_rgb16_fill(node[2], (node[3] & 0xfff) * 2u,
                             node[10], node[11], node[6]);
        return;
    }
    if (words == 9 && node[0] == 0x02000307 && node[1] == 0x00000002 &&
        ((node[3] >> 12) & 0x1f) == 6 &&
        ((node[5] >> 12) & 0x1f) == 6) {
        sf2000_ge_rgb16_blit(node[2], (node[3] & 0xfff) * 2u,
                             node[4], (node[5] & 0xfff) * 2u,
                             node[6], node[7], node[8]);
        return;
    }
    if (words == 22 && node[0] == 0x0206870f &&
        node[1] == 0x00a03009 && ((node[3] >> 12) & 0x1f) == 6 &&
        ((node[7] >> 12) & 0x1f) == 6 &&
        !(node[7] & 0x00300000u) && node[13] == 0x00004000u &&
        node[15] == 0x00000080) {
        sf2000_ge_rgb16_stretch(node[2], (node[3] & 0xfff) * 2u,
                                node[6], (node[7] & 0xfff) * 2u,
                                node[9], node[12]);
        return;
    }
    if ((node[0] & 0xff000000u) == 0x02000000u) {
        sf2000_ge_execute_grouped_node(node, words);
        return;
    }
    if (words < 26 || node[0] != 0x0201ffff) {
        return;
    }

    dst = node[2];
    src = node[6];
    fmt = node[13] & 0xff;
    width = node[17] >> 16;
    height = node[17] & 0xffff;

    /*
     * This command subset is intentionally small and trace-derived:
     *
     * - 16x16 source nodes copy 256 ARGB8888 palette entries into the GMA
     *   palette buffers referenced by the display descriptor.
     * - Full-screen zero-source nodes clear the eventual GMA bitmap. The stock
     *   firmware emits these while switching between CLUT8 and RGB565 boot
     *   surfaces before the menu assets are drawn.
     *
     * Unknown nodes are still logged, then treated as completed. That keeps the
     * boot trace moving while preserving enough evidence for the next hardware
     * primitive.
     */
    if (src && width == 16 && height == 16) {
        sf2000_ge_memcpy(dst, src, 16 * 16 * 4);
        if (sf2000_trace_ge()) {
            qemu_log_mask(LOG_UNIMP,
                          "sf2000: ge-copy-palette dst=0x%08x src=0x%08x\n",
                          dst, src);
        }
    } else if (src && dst && width && height) {
        if (fmt == 0x0c) {
            len = (size_t)width * height;
        } else if (fmt == 0x01 || fmt == 0x06) {
            len = (size_t)width * height * 2;
        } else {
            len = 0;
        }
        if (len) {
            sf2000_ge_memcpy(dst, src, len);
            if (sf2000_trace_ge()) {
                qemu_log_mask(LOG_UNIMP,
                              "sf2000: ge-copy dst=0x%08x src=0x%08x fmt=0x%x %ux%u len=%zu\n",
                              dst, src, fmt, width, height, len);
            }
        }
    } else if (!src && dst && width && height) {
        if (fmt == 0x0c) {
            len = (size_t)width * height;
        } else if (fmt == 0x01 || fmt == 0x06) {
            len = (size_t)width * height * 2;
        } else {
            len = 0;
        }
        if (len) {
            sf2000_ge_memset(dst, 0, len);
            if (sf2000_trace_ge()) {
                qemu_log_mask(LOG_UNIMP,
                              "sf2000: ge-clear dst=0x%08x fmt=0x%x %ux%u len=%zu\n",
                              dst, fmt, width, height, len);
            }
        }
    }
}

static void sf2000_ge_complete_queue(void)
{
    uint32_t first;
    uint32_t last;
    uint32_t words;
    uint32_t *node;
    uint32_t offset;
    unsigned dump_limit;
    unsigned node_dump_limit;
    unsigned i;

    /*
     * The HCRTOS GE driver programs HQ first/last pointers at 0x10/0x14 and
     * then writes bit 1 to 0x04. Real hardware DMA-executes the command node
     * range and clears the busy state. We currently model completion and dump
     * the first queue once so the command format can be implemented from real
     * stock traces.
     */
    sf2000_mmio_get32(SF2000_GE_HQ_FIRST, &first);
    sf2000_mmio_get32(SF2000_GE_HQ_LAST, &last);
    if (!first || !last || last < first) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: ge-start empty first=0x%08x last=0x%08x\n",
                      first, last);
        sf2000_mmio_set32(SF2000_GE_STATUS, 0);
        return;
    }

    words = (last - first) / 4u + 1u;
    if (words > 0x3c000u / sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sf2000: ge queue too large words=%u\n", words);
        sf2000_mmio_set32(SF2000_GE_STATUS, 0);
        return;
    }
    node = g_new(uint32_t, words);
    if (!sf2000_ge_read_node(first, node, words)) {
        g_free(node);
        sf2000_mmio_set32(SF2000_GE_STATUS, 0);
        return;
    }
    for (offset = 0; offset < words; ) {
        uint32_t header_offset = offset;
        unsigned node_words;

        /* Four optional address-extension records precede the group header. */
        while (header_offset + 1u < words &&
               (node[header_offset] & 0xff0003ffu) == 0x81000001u) {
            header_offset += 2u;
        }
        node_words = sf2000_ge_grouped_node_words(node[header_offset]);
        if (!node_words || header_offset + node_words > words) {
            if (sf2000_trace_ge()) {
                qemu_log_mask(LOG_UNIMP,
                              "sf2000: ge unknown node offset=%u word=%08x\n",
                              offset, node[offset]);
            }
            break;
        }
        sf2000_ge_execute_node(node + header_offset, node_words);
        offset = header_offset + node_words;
    }

    dump_limit = sf2000_ge_dump_limit();
    node_dump_limit = sf2000_ge_node_dump_limit();
    if (sf2000_ge_queue_dump_count < dump_limit) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: ge-start[%u] first=0x%08x last=0x%08x words=%u dst=0x%08x src=0x%08x fmt=0x%x wh=0x%08x aux=0x%08x\n",
                      sf2000_ge_queue_dump_count, first, last, words,
                      words > 2 ? node[2] : 0, words > 6 ? node[6] : 0,
                      words > 13 ? node[13] : 0, words > 17 ? node[17] : 0,
                      words > 25 ? node[25] : 0);
        if (sf2000_ge_queue_dump_count < node_dump_limit) {
            for (i = 0; i < MIN(words,
                         (uint32_t)SF2000_GE_QUEUE_DUMP_WORDS); i++) {
                qemu_log_mask(LOG_UNIMP, "sf2000: ge-node[%02u]=0x%08x\n",
                              i, node[i]);
            }
        }
        if (!words) {
            qemu_log_mask(LOG_UNIMP, "sf2000: ge-node[%02u]=0x%08x\n",
                          0, 0);
        }
        sf2000_ge_queue_dump_count++;
    }

    g_free(node);

    sf2000_mmio_set32(SF2000_GE_STATUS, 0);
    /*
     * HQ_FIRST is the consumer pointer.  The inclusive HQ_LAST word has
     * already been consumed, so hardware advances FIRST to the following
     * word.  Keeping it on LAST hid Linux drivers which incorrectly rewound
     * the same node for every frame instead of extending the vendor DMA ring.
     */
    sf2000_mmio_set32(SF2000_GE_HQ_FIRST, last + sizeof(uint32_t));

    /*
     * The GMA layer scans descriptor chains continuously on hardware. Stock
     * firmware often arms the descriptors first, then uses GE to populate their
     * backing buffers. Re-present active descriptors after GE completion so the
     * emulated console observes those late buffer updates without waiting for a
     * new doorbell write that may never come.
     */
    if (sf2000_active_gma[0]) {
        sf2000_gma_present(sf2000_active_gma[0]);
    }
    if (sf2000_active_gma[1] && sf2000_active_gma[1] != sf2000_active_gma[0]) {
        sf2000_gma_present(sf2000_active_gma[1]);
    }
}

static void sf2000_ge_timer_cb(void *opaque)
{
    uint32_t status = 0;

    (void)opaque;
    sf2000_ge_complete_queue();
    sf2000_mmio_get32(SF2000_GE_STATUS, &status);
    status &= ~SF2000_GE_STATUS_BUSY;
    status |= SF2000_GE_STATUS_DONE;
    sf2000_mmio_set32(SF2000_GE_STATUS, status);
    sf2000_ge_irq_pending = true;
    if (sf2000_scanout_oracle() &&
        (sf2000_ge_submit_sequence <= 8 ||
         !(sf2000_ge_submit_sequence % 300))) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: ge-queue complete seq=%" PRIu64
                      " status=%08x irq=1\n",
                      sf2000_ge_submit_sequence, status);
    }
    sf2000_update_irq();
}

static void sf2000_ge_start_queue(void)
{
    uint32_t status = 0;
    uint32_t first = 0;
    uint32_t last = 0;

    sf2000_mmio_get32(SF2000_GE_STATUS, &status);
    sf2000_mmio_get32(SF2000_GE_HQ_FIRST, &first);
    sf2000_mmio_get32(SF2000_GE_HQ_LAST, &last);
    if (!sf2000_ge_queue_min || first < sf2000_ge_queue_min) {
        sf2000_ge_queue_min = first;
    }
    if (status & SF2000_GE_STATUS_BUSY) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sf2000: GE doorbell while command queue busy\n");
        return;
    }
    status &= ~SF2000_GE_STATUS_DONE;
    status |= SF2000_GE_STATUS_BUSY;
    sf2000_mmio_set32(SF2000_GE_STATUS, status);
    sf2000_ge_irq_pending = false;
    sf2000_ge_submit_sequence++;
    if (sf2000_scanout_oracle() &&
        (sf2000_ge_submit_sequence <= 8 ||
         !(sf2000_ge_submit_sequence % 300))) {
        uint32_t context[7] = { 0 };

        if (sf2000_ge_queue_min >= (8 + 256) * sizeof(uint32_t)) {
            sf2000_ge_read_node(sf2000_ge_queue_min -
                                (8 + 256) * sizeof(uint32_t),
                                context, ARRAY_SIZE(context));
        }
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: ge-queue start seq=%" PRIu64
                      " first=%08x last=%08x latency_ns=%" PRId64
                      " status=%08x producer=%08x wrap_at=%08x\n",
                      sf2000_ge_submit_sequence,
                      first, last,
                      (int64_t)SF2000_GE_LATENCY_NS,
                      status, context[6], context[5]);
    }
    sf2000_update_irq();
    timer_mod(sf2000_ge_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              SF2000_GE_LATENCY_NS);
}

static bool sf2000_adc_decode(hwaddr full_addr, unsigned *index,
                              unsigned *offset)
{
    hwaddr adc_off;

    if (full_addr < SF2000_ADC_BASE ||
        full_addr >= SF2000_ADC_BASE + SF2000_ADC_SIZE) {
        return false;
    }

    adc_off = full_addr - SF2000_ADC_BASE;
    *index = adc_off / 4;
    *offset = adc_off & 3;
    return *index < ARRAY_SIZE(sf2000_adc_ctrl);
}

static uint64_t sf2000_adc_read(hwaddr full_addr, unsigned size)
{
    unsigned index;
    unsigned offset;
    uint32_t value;

    if (!sf2000_adc_decode(full_addr, &index, &offset)) {
        return 0;
    }

    value = sf2000_adc_ctrl[index];
    if (index == 0) {
        /*
         * HCRTOS exposes battery/key ADC through SAR_ADC_CTRL_REG0:
         * bit 8 is interrupt/status, bits 23:16 are the latest 8-bit sample.
         * UniFrog's battery thresholds treat raw 200 as about 4.0 V, so this
         * value presents a healthy battery without requiring analog modelling.
         */
        value &= ~0x00ff0100u;
        value |= (SF2000_ADC_SAMPLE << 16) | BIT(8);
    }
    value >>= offset * 8;
    if (size < 4) {
        value &= (1u << (size * 8)) - 1u;
    }
    return value;
}

static uint64_t sf2000_i2c_read(hwaddr full_addr, unsigned size)
{
    unsigned index;
    unsigned offset;
    uint32_t value = 0;

    if (!sf2000_i2c_decode(full_addr, &index, &offset)) {
        return 0;
    }

    switch (offset) {
    case SF2000_I2C_HSR:
        /*
         * The HC I2C driver polls for host-busy/device-busy clear and FIFO
         * not-full. A zero status is the idle, ready state for write
         * transfers and avoids inventing a device-specific slave response.
         */
        value = 0;
        break;
    case SF2000_I2C_IER:
        value = sf2000_i2c_ier[index];
        break;
    case SF2000_I2C_ISR:
        value = sf2000_i2c_isr[index];
        break;
    case SF2000_I2C_DATA:
        value = sf2000_i2c_data[index];
        break;
    case SF2000_I2C_IER1:
        value = sf2000_i2c_ier1[index];
        break;
    case SF2000_I2C_ISR1:
        value = sf2000_i2c_isr1[index];
        break;
    default:
        sf2000_mmio_get32(full_addr, &value);
        value >>= ((full_addr & 3u) * 8u);
        break;
    }

    if (size < 4) {
        value &= (1u << (size * 8)) - 1u;
    }
    return value;
}

static void sf2000_i2c_write(hwaddr full_addr, uint64_t value)
{
    unsigned index;
    unsigned offset;

    if (!sf2000_i2c_decode(full_addr, &index, &offset)) {
        return;
    }

    switch (offset) {
    case SF2000_I2C_HCR:
        if (value & SF2000_I2C_HCR_ST) {
            /*
             * Model an immediate, successful host-controller transaction. The
             * real block moves bytes through its FIFO and raises TDI; open
             * drivers care first that the transfer contract does not timeout.
             */
            sf2000_i2c_isr[index] |= SF2000_I2C_ISR_TDI;
            sf2000_i2c_isr1[index] |= SF2000_I2C_ISR1_FT;
        }
        break;
    case SF2000_I2C_IER:
        sf2000_i2c_ier[index] = value & 0xff;
        break;
    case SF2000_I2C_ISR:
        sf2000_i2c_isr[index] &= ~(value & 0xff);
        break;
    case SF2000_I2C_DATA:
        sf2000_i2c_data[index] = value & 0xff;
        break;
    case SF2000_I2C_IER1:
        sf2000_i2c_ier1[index] = value & 0xff;
        break;
    case SF2000_I2C_ISR1:
        sf2000_i2c_isr1[index] &= ~(value & 0xff);
        break;
    default:
        break;
    }
}

static bool sf2000_wdt_decode(hwaddr full_addr)
{
    return full_addr >= SF2000_WDT_BASE &&
           full_addr < SF2000_WDT_BASE + SF2000_WDT_SIZE;
}

static int64_t sf2000_wdt_timeout_ns(void)
{
    const char *env = g_getenv("SF2000_WDT_TIMEOUT_MS");
    uint64_t ticks;

    if (env && *env) {
        char *end = NULL;
        uint64_t parsed = g_ascii_strtoull(env, &end, 0);

        if (end && *end == '\0' && parsed > 0) {
            return parsed * SCALE_MS;
        }
    }

    /*
     * The SDK loads -(usec * 27 / 128).  WDT0 therefore advances at
     * 27 MHz / 128 until the 32-bit counter wraps.  A zero count is useful
     * while guests sequence disable/configure writes; retain the historical
     * 20 second diagnostic window instead of treating it as an instant wrap.
     */
    ticks = (uint32_t)(0u - sf2000_wdt_count);
    if (!ticks) {
        return 20000LL * SCALE_MS;
    }
    return MAX((uint64_t)1,
               (ticks * 128000u + 26u) / 27u) * SCALE_NS;
}

static void sf2000_wdt_timer_cb(void *opaque)
{
    (void)opaque;

    if (!sf2000_wdt_conf) {
        return;
    }

    qemu_log_mask(LOG_UNIMP, "sf2000: watchdog timeout reset\n");
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static void sf2000_wdt_arm(void)
{
    if (!sf2000_wdt_timer) {
        return;
    }

    timer_mod(sf2000_wdt_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              sf2000_wdt_timeout_ns());
}

static void sf2000_wdt_disable(void)
{
    sf2000_wdt_conf = 0;
    if (sf2000_wdt_timer) {
        timer_del(sf2000_wdt_timer);
    }
    qemu_log_mask(LOG_UNIMP, "sf2000: watchdog disabled\n");
}

static void sf2000_uart_put(unsigned index, uint8_t ch)
{
    if (ch == '\r') {
        return;
    }
    if (ch == '\n') {
        sf2000_uart_line[index][sf2000_uart_line_len[index]] = 0;
        qemu_log_mask(LOG_UNIMP, "sf2000: uart: %s\n",
                      sf2000_uart_line[index]);
        sf2000_uart_line_len[index] = 0;
        return;
    }
    if (sf2000_uart_line_len[index] + 1 < sizeof(sf2000_uart_line[index])) {
        sf2000_uart_line[index][sf2000_uart_line_len[index]++] =
            ch >= 0x20 && ch < 0x7f ? ch : '.';
    }
}

static uint32_t sf2000_gpio_l_sample(uint32_t value)
{
    CPUState *cs = first_cpu;
    uint32_t dir = 0;
    unsigned index = sf2000_key_shift_index % SF2000_KEY_COUNT;

    /*
     * GB300 scans a separate 16-bit two-wire shifter on L25/L27/L26.  The
     * board-profile model used to expose only the SF2000 L23/L24 shifter,
     * which made Linux's otherwise real automatic board probe select SF2000
     * even when QEMU was explicitly configured as GB300.  Keep the two buses
     * electrically distinct: the direction pattern is the same one used by
     * sf2000-pad.c while the GB300 transaction is in its read phase.
     */
    if (strcmp(sf2000_board_profile_name(), "gb300") == 0 &&
        sf2000_mmio_get32(SF2000_GPIO_L_DIR, &dir) &&
        (dir & BIT(26)) && !(dir & BIT(25)) && !(dir & BIT(27))) {
        static const int8_t gb300_key_for_shift[16] = {
            -1, 0, 1, 2, 3, 4, 5, 6,
            7, 8, 9, 10, 11, -1, -1, -1,
        };
        int8_t key = gb300_key_for_shift[
            sf2000_key_shift_index % ARRAY_SIZE(gb300_key_for_shift)];

        value |= BIT(25) | BIT(27);
        if (key >= 0 && (sf2000_key_mask & BIT((unsigned)key)))
            value &= ~BIT(27);
    }

    if (!(dir & BIT(26))) {
        if (sf2000_key_mask & BIT(index)) {
            value &= ~BIT(SF2000_KEY_DATA_BIT);
        } else {
            value |= BIT(SF2000_KEY_DATA_BIT);
        }
    }

    if (g_getenv("SF2000_TRACE_KEYS") && cs) {
        MIPSCPU *cpu = MIPS_CPU(cs);

        fprintf(stderr,
                "sf2000: key-sample pc=0x%08x index=%u mask=0x%08x value=0x%08x\n",
                (uint32_t)cpu->env.active_tc.PC, index, sf2000_key_mask,
                value);
    }

    if (sf2000_lcd) {
        value = sf2000_panel_sample_readback_l(sf2000_lcd, value);
    }

    return value;
}

static uint8_t sf2000_rf_read_reg(uint8_t reg)
{
    switch (reg) {
    case 0x05:
        /*
         * UniFrog's empirical stock-compatible sequence writes 0x25=0xa5
         * and then validates the bus by reading RF register 0x05 as 0xa5.
         * Keep the ID/test register stable even if guest code never touched
         * it directly.
         */
        return sf2000_rf_regs[reg] ? sf2000_rf_regs[reg] : 0xa5;
    case 0x07:
        /*
         * Packet-ready/status. UniFrog's real-device probe observed 0x0e while
         * the bus was healthy and no wireless packet was pending. Bit 0x40 is
         * the packet-ready bit used by UniFrog, so this remains an idle state.
         */
        return 0x0e;
    case 0x61:
        return 0x00;
    default:
        return sf2000_rf_regs[reg];
    }
}

static void sf2000_rf_reset_transaction(void)
{
    sf2000_rf_have_reg = false;
    sf2000_rf_read_phase = false;
    sf2000_rf_shift = 0;
    sf2000_rf_bit_count = 0;
    sf2000_rf_response = 0;
    sf2000_rf_response_bit = 0;
}

static void sf2000_rf_finish_byte(uint8_t value)
{
    if (!sf2000_rf_have_reg) {
        sf2000_rf_reg = value;
        sf2000_rf_have_reg = true;
        sf2000_rf_response = sf2000_rf_read_reg(sf2000_rf_reg);
        sf2000_rf_response_bit = 0;
        return;
    }

    if (!sf2000_rf_read_phase) {
        sf2000_rf_regs[sf2000_rf_reg] = value;
        if (sf2000_rf_reg == 0x25 && value == 0xa5) {
            sf2000_rf_regs[0x05] = 0xa5;
        }
    }
}

static bool sf2000_rf_data_is_output(void)
{
    uint32_t dir = 0;

    sf2000_mmio_get32(SF2000_GPIO_L_DIR, &dir);
    return (dir & BIT(SF2000_RF_DATA_BIT)) != 0;
}

static void sf2000_rf_gpio_l_write(uint32_t value)
{
    bool new_cs = (value & BIT(SF2000_RF_CS_BIT)) != 0;
    bool new_clk = (value & BIT(SF2000_RF_CLK_BIT)) != 0;
    bool data_is_output;

    if ((!sf2000_rf_cs && new_cs) || (sf2000_rf_cs && !new_cs)) {
        sf2000_rf_reset_transaction();
    }

    sf2000_rf_cs = new_cs;
    if (sf2000_rf_cs) {
        sf2000_rf_clk = new_clk;
        return;
    }

    data_is_output = sf2000_rf_data_is_output();
    if (!data_is_output && sf2000_rf_have_reg) {
        sf2000_rf_read_phase = true;
    }

    if (!sf2000_rf_clk && new_clk) {
        if (data_is_output) {
            sf2000_rf_shift = (sf2000_rf_shift << 1) |
                              ((value >> SF2000_RF_DATA_BIT) & 1u);
            if (++sf2000_rf_bit_count == 8) {
                sf2000_rf_finish_byte(sf2000_rf_shift);
                sf2000_rf_shift = 0;
                sf2000_rf_bit_count = 0;
            }
        }
    }

    sf2000_rf_clk = new_clk;
}

static uint32_t sf2000_rf_gpio_l_sample(uint32_t value)
{
    if (!sf2000_rf_cs && sf2000_rf_have_reg && !sf2000_rf_data_is_output()) {
        uint8_t bit = (sf2000_rf_response >> (7 - sf2000_rf_response_bit)) & 1;

        if (bit) {
            value |= BIT(SF2000_RF_DATA_BIT);
        } else {
            value &= ~BIT(SF2000_RF_DATA_BIT);
        }
        sf2000_rf_response_bit++;
        if (sf2000_rf_response_bit == 8) {
            sf2000_rf_reg++;
            sf2000_rf_response = sf2000_rf_read_reg(sf2000_rf_reg);
            sf2000_rf_response_bit = 0;
        }
    }

    return value;
}

static void sf2000_gpio_l_write(uint32_t value)
{
    bool old_clk = (sf2000_gpio_l_out & BIT(SF2000_KEY_CLK_BIT)) != 0;
    bool new_clk = (value & BIT(SF2000_KEY_CLK_BIT)) != 0;
    uint32_t dir = 0;
    bool data_is_output;

    /*
     * The SF2000 local keypad is a 12-bit active-low shift register on L23
     * with clock on L24. Driving L23 low is the parallel-load phase used by
     * UniFrog and the stock firmware before switching it back to input. This
     * is why keyboard state is not returned as one flat GPIO register: each
     * rising clock edge advances the sampled key bit.
     */
    sf2000_mmio_get32(SF2000_GPIO_L_IN + 8, &dir);
    data_is_output = (dir & BIT(SF2000_KEY_DATA_BIT)) != 0;

    if (data_is_output && !(value & BIT(SF2000_KEY_DATA_BIT))) {
        sf2000_key_shift_index = 0;
    } else if (!old_clk && new_clk) {
        sf2000_key_shift_index =
            (sf2000_key_shift_index + 1) % SF2000_KEY_COUNT;
    }

    if (g_getenv("SF2000_TRACE_KEYS")) {
        fprintf(stderr,
                "sf2000: key-clock old=0x%08x new=0x%08x index=%u mask=0x%08x\n",
                sf2000_gpio_l_out, value, sf2000_key_shift_index,
                sf2000_key_mask);
    }

    sf2000_gpio_l_out = value;
    sf2000_rf_gpio_l_write(value);
}

static void sf2000_keyboard_event(DeviceState *dev, QemuConsole *src,
                                  InputEvent *evt)
{
    InputKeyEvent *key;
    QKeyCode qcode;
    unsigned i;

    assert(evt->type == INPUT_EVENT_KIND_KEY);
    key = evt->u.key.data;
    qcode = qemu_input_key_value_to_qcode(key->key);

    for (i = 0; i < ARRAY_SIZE(sf2000_key_map); i++) {
        if (sf2000_key_map[i].qcode != qcode) {
            continue;
        }
        if (key->down) {
            sf2000_key_mask |= sf2000_key_map[i].mask;
        } else {
            sf2000_key_mask &= ~sf2000_key_map[i].mask;
        }
        qemu_log_mask(LOG_UNIMP, "sf2000: key qcode=%s down=%d mask=0x%08x\n",
                      QKeyCode_str(qcode), key->down, sf2000_key_mask);
        break;
    }
}

static const QemuInputHandler sf2000_keyboard_handler = {
    .name = "sf2000 keypad",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = sf2000_keyboard_event,
};

static void sf2000_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8;
}

static void sf2000_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = v >> 24;
}

static void sf2000_sdio_fill_sector(uint32_t lba, uint8_t sector[512])
{
    memset(sector, 0, 512);

    if (lba == 0) {
        memcpy(sector + 0x1be, "\x80\x01\x01\x00\x0c\xfe\xff\xff", 8);
        sf2000_put_le32(sector + 0x1c6, 2048);
        sf2000_put_le32(sector + 0x1ca, 0x00020000);
    } else if (lba == 2048) {
        memcpy(sector, "\xeb\x58\x90MSDOS5.0", 11);
        sf2000_put_le16(sector + 11, 512);
        sector[13] = 8;
        sf2000_put_le16(sector + 14, 32);
        sector[16] = 2;
        sf2000_put_le32(sector + 32, 0x00020000);
        sector[21] = 0xf8;
        sf2000_put_le32(sector + 36, 128);
        sf2000_put_le32(sector + 44, 2);
        sf2000_put_le16(sector + 510, 0xaa55);
        memcpy(sector + 82, "FAT32   ", 8);
    }

    sector[510] = 0x55;
    sector[511] = 0xaa;
}

static GHashTable *sf2000_sdio_synth_table(void)
{
    if (!sf2000_sdio_synth_sectors) {
        sf2000_sdio_synth_sectors =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    }
    return sf2000_sdio_synth_sectors;
}

static bool sf2000_sdio_synth_read_sector(uint32_t lba, uint8_t sector[512])
{
    uint8_t *stored;

    if (!sf2000_sdio_synth_sectors) {
        return false;
    }

    stored = g_hash_table_lookup(sf2000_sdio_synth_sectors,
                                 GUINT_TO_POINTER(lba));
    if (stored) {
        memcpy(sector, stored, 512);
        return true;
    }
    return false;
}

static void sf2000_sdio_synth_write_sector(uint32_t lba,
                                           const uint8_t sector[512])
{
    uint8_t *copy;

    copy = g_memdup2(sector, 512);
    g_hash_table_replace(sf2000_sdio_synth_table(), GUINT_TO_POINTER(lba),
                         copy);
}

static void sf2000_sdio_synth_writeback_selftest(void)
{
    uint8_t write_sector[512];
    uint8_t read_sector[512];
    uint32_t lba = 0xfffffff0u;

    memset(write_sector, 0x5a, sizeof(write_sector));
    memset(read_sector, 0, sizeof(read_sector));
    sf2000_sdio_synth_write_sector(lba, write_sector);
    if (!sf2000_sdio_synth_read_sector(lba, read_sector) ||
        memcmp(write_sector, read_sector, sizeof(write_sector)) != 0) {
        error_report("sf2000: synthetic FAT probe writeback selftest failed lba=%u",
                     lba);
        return;
    }
    info_report("sf2000: synthetic FAT probe writeback selftest ok lba=%u",
                lba);
}

static void sf2000_sdio_dma_write(uint32_t lba);

static void sf2000_sdio_raw_writeback_selftest(void)
{
    uint8_t write_sector[1024];
    uint8_t read_sector[1024];
    uint32_t lba = 0x10;
    const hwaddr dma_addr = 0x00001000ULL;
    int ret;

    memset(write_sector, 0x5a, sizeof(write_sector));
    memset(read_sector, 0, sizeof(read_sector));

    cpu_physical_memory_write(dma_addr, write_sector, sizeof(write_sector));
    sf2000_sdio_dma_addr = dma_addr;
    sf2000_sdio_dma_len = sizeof(write_sector);
    sf2000_sdio_dma_write(lba);

    ret = blk_flush(sf2000_sdio_blk);
    if (ret < 0) {
        error_report("sf2000: raw SD probe DMA writeback selftest failed flush lba=%u ret=%d",
                     lba, ret);
        return;
    }

    ret = blk_pread(sf2000_sdio_blk, (int64_t)lba * 512,
                    sizeof(read_sector), read_sector, 0);
    if (ret < 0 || memcmp(write_sector, read_sector, sizeof(write_sector)) != 0) {
        error_report("sf2000: raw SD probe DMA writeback selftest failed verify lba=%u ret=%d",
                     lba, ret);
        return;
    }

    info_report("sf2000: raw SD probe DMA writeback selftest ok lba=%u sectors=2",
                lba);
}

static bool sf2000_sdio_read_sector(uint32_t lba, uint8_t sector[512])
{
    int ret;

    if (!sf2000_sdio_blk) {
        if (sf2000_sdio_synth_read_sector(lba, sector)) {
            return false;
        }
        sf2000_sdio_fill_sector(lba, sector);
        return false;
    }

    ret = blk_pread(sf2000_sdio_blk, (int64_t)lba * 512, 512, sector, 0);
    if (ret < 0) {
        sf2000_sdio_fill_sector(lba, sector);
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: sdio-read-image-fallback lba=%u ret=%d\n",
                      lba, ret);
        return false;
    }
    return true;
}

static bool sf2000_sdio_dma_read_image_bulk(uint32_t lba, uint32_t len,
                                            uint32_t *copied,
                                            MemTxResult *result)
{
    dma_addr_t map_len = len;
    void *buf;
    int ret;

    if (!sf2000_sdio_blk || !len) {
        return false;
    }

    buf = dma_memory_map(&address_space_memory, sf2000_sdio_dma_addr,
                         &map_len, DMA_DIRECTION_FROM_DEVICE,
                         MEMTXATTRS_UNSPECIFIED);
    if (!buf || map_len != len) {
        if (buf) {
            dma_memory_unmap(&address_space_memory, buf, map_len,
                             DMA_DIRECTION_FROM_DEVICE, 0);
        }
        return false;
    }

    ret = blk_pread(sf2000_sdio_blk, (int64_t)lba * 512, len, buf, 0);
    if (ret < 0) {
        dma_memory_unmap(&address_space_memory, buf, map_len,
                         DMA_DIRECTION_FROM_DEVICE, 0);
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: sdio-read-image-bulk-fallback lba=%u len=%u ret=%d\n",
                      lba, len, ret);
        return false;
    }

    dma_memory_unmap(&address_space_memory, buf, map_len,
                     DMA_DIRECTION_FROM_DEVICE, len);
    *result = MEMTX_OK;
    *copied = len;
    return true;
}

static void sf2000_sdio_dma_read(uint32_t lba)
{
    uint8_t sector[512];
    MemTxResult result = MEMTX_OK;
    uint32_t len = sf2000_sdio_dma_len ? sf2000_sdio_dma_len : 512;
    uint32_t copied = 0;
    uint32_t sectors = (len + sizeof(sector) - 1) / sizeof(sector);
    bool image_backed = true;
    uint32_t i;

    if (sf2000_sdio_dma_read_image_bulk(lba, len, &copied, &result)) {
        image_backed = true;
    } else {
        image_backed = sf2000_sdio_blk != NULL;
    }

    for (i = copied / sizeof(sector); copied < len && i < sectors; i++) {
        uint32_t chunk = len - copied;

        if (chunk > sizeof(sector)) {
            chunk = sizeof(sector);
        }
        image_backed &= sf2000_sdio_read_sector(lba + i, sector);
        result = dma_memory_write(&address_space_memory,
                                  sf2000_sdio_dma_addr + copied,
                                  sector, chunk, MEMTXATTRS_UNSPECIFIED);
        if (result != MEMTX_OK) {
            break;
        }
        copied += chunk;
    }

    sf2000_sdio_xfer_done = true;
    sf2000_sdio_xfer_busy = false;
    sf2000_sdio_write_active = false;
    sf2000_sdio_pio_state = 0x04;
    sf2000_sdio_irq_pending = true;
    sf2000_sdio_callback_pending = true;
    /*
     * The HC15xx SDIO block has two completion surfaces in the stock traces:
     * the SDIO status bits themselves and a later timer callback path used by
     * the firmware's asynchronous disk layer. Raising both keeps bootloader
     * sector reads and stock firmware FatFs mounting on the same model.
     */
    if (sf2000_trace_sdio()) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: sdio-dma-read lba=%u sectors=%u dst=0x%08x len=%u copied=%u image=%d result=%d\n",
                      lba, sectors, sf2000_sdio_dma_addr, len, copied,
                      image_backed, result);
    }
}

/*
 * HC15xx exposes independent DMA address/length pairs: 0x20/0x28 for
 * card-to-memory and 0x24/0x2c for memory-to-card.  Older firmware sometimes
 * reuses the read pair, so retain that as a compatibility fallback.
 */
static uint32_t sf2000_sdio_dma_write_src(void)
{
    return sf2000_sdio_dma_wr_len ? sf2000_sdio_dma_wr_addr
                                  : sf2000_sdio_dma_addr;
}

static uint32_t sf2000_sdio_dma_write_len(void)
{
    return sf2000_sdio_dma_wr_len ? sf2000_sdio_dma_wr_len
                                  : sf2000_sdio_dma_len;
}

static bool sf2000_sdio_dma_write_image_bulk(uint32_t lba, uint32_t len,
                                             uint32_t *copied,
                                             MemTxResult *dma_result,
                                             int *blk_result)
{
    dma_addr_t map_len = len;
    void *buf;

    if (!sf2000_sdio_blk || !len || (len & 511u)) {
        return false;
    }

    buf = dma_memory_map(&address_space_memory, sf2000_sdio_dma_write_src(),
                         &map_len, DMA_DIRECTION_TO_DEVICE,
                         MEMTXATTRS_UNSPECIFIED);
    if (!buf || map_len != len) {
        if (buf) {
            dma_memory_unmap(&address_space_memory, buf, map_len,
                             DMA_DIRECTION_TO_DEVICE, 0);
        }
        return false;
    }

    *blk_result = blk_pwrite(sf2000_sdio_blk, (int64_t)lba * 512, len, buf, 0);
    dma_memory_unmap(&address_space_memory, buf, map_len,
                     DMA_DIRECTION_TO_DEVICE, len);
    if (*blk_result >= 0) {
        *dma_result = MEMTX_OK;
        *copied = len;
    } else {
        *dma_result = MEMTX_ERROR;
    }
    return true;
}

static void sf2000_sdio_dma_write(uint32_t lba)
{
    uint8_t sector[512];
    MemTxResult dma_result = MEMTX_OK;
    int blk_result = 0;
    uint32_t programmed_len = sf2000_sdio_dma_write_len();
    uint32_t len = programmed_len ? programmed_len : 512;
    uint32_t copied = 0;
    uint32_t sectors = (len + sizeof(sector) - 1) / sizeof(sector);
    bool image_backed = sf2000_sdio_blk != NULL;
    uint32_t i;

    if (!sf2000_sdio_dma_write_image_bulk(lba, len, &copied, &dma_result,
                                          &blk_result)) {
        copied = 0;
    }

    for (i = copied / sizeof(sector); copied < len && i < sectors; i++) {
        uint32_t chunk = len - copied;

        if (chunk > sizeof(sector)) {
            chunk = sizeof(sector);
        }
        memset(sector, 0, sizeof(sector));
        dma_result = dma_memory_read(&address_space_memory,
                                     sf2000_sdio_dma_write_src() + copied,
                                     sector, chunk, MEMTXATTRS_UNSPECIFIED);
        if (dma_result != MEMTX_OK) {
            break;
        }
        if (sf2000_sdio_blk) {
            blk_result = blk_pwrite(sf2000_sdio_blk, (int64_t)(lba + i) * 512,
                                    512, sector, 0);
            if (blk_result < 0) {
                break;
            }
        } else {
            sf2000_sdio_synth_write_sector(lba + i, sector);
        }
        copied += chunk;
    }

    sf2000_sdio_xfer_done = true;
    sf2000_sdio_xfer_busy = false;
    sf2000_sdio_pio_state = 0x04;
    sf2000_sdio_irq_pending = true;
    sf2000_sdio_callback_pending = true;
    if (sf2000_trace_sdio()) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: sdio-dma-write lba=%u sectors=%u src=0x%08x len=%u copied=%u image=%d dma=%d blk=%d\n",
                      lba, sectors, sf2000_sdio_dma_write_src(), len, copied,
                      image_backed, dma_result, blk_result);
    }
}

static void sf2000_sdio_set_short_response(uint32_t response)
{
    /* HC15xx presents a 32-bit short response after one framing byte. */
    sf2000_sdio_resp[0] = response << 8;
    sf2000_sdio_resp[1] = response >> 24;
}

static void sf2000_sdio_dma_read_scr(void)
{
    static const uint8_t scr[8] = {
        0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    uint32_t len = sf2000_sdio_dma_len;
    MemTxResult result;

    if (!len || len > sizeof(scr)) {
        len = sizeof(scr);
    }
    result = dma_memory_write(&address_space_memory, sf2000_sdio_dma_addr,
                              scr, len, MEMTXATTRS_UNSPECIFIED);
    if (sf2000_trace_sdio()) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: sdio-dma-scr dst=0x%08x len=%u result=%d\n",
                      sf2000_sdio_dma_addr, len, result);
    }
}

static void sf2000_sdio_complete_cmd(void)
{
    bool is_app_cmd = sf2000_sdio_app_cmd && sf2000_sdio_cmd != 55;

    sf2000_sdio_resp[0] = 0;
    sf2000_sdio_resp[1] = 0;
    sf2000_sdio_resp[2] = 0;
    sf2000_sdio_resp[3] = 0;

    switch (sf2000_sdio_cmd) {
    case 0:
        break;
    case 6:
        if (is_app_cmd) {
            /*
             * ACMD6 selects the data bus width. Stock boot issues CMD55 then
             * CMD6 arg=2 after selecting the card, which means 4-bit mode.
             */
            sf2000_sdio_bus_width = (sf2000_sdio_arg & 3u) == 2u ? 4 : 1;
            sf2000_sdio_resp[0] = 0;
        } else {
            /*
             * Plain CMD6 is SWITCH_FUNC. The current stock path does not use
             * the 64-byte function-status payload, so keep the command
             * accepted but report no data transfer until a trace needs it.
             */
            sf2000_sdio_resp[0] = 0;
        }
        break;
    case 2:
        sf2000_sdio_resp[0] = 0x12345678;
        sf2000_sdio_resp[1] = 0x9abcdef0;
        sf2000_sdio_resp[2] = 0x00001122;
        sf2000_sdio_resp[3] = 0x33445566;
        break;
    case 3:
        sf2000_sdio_set_short_response(0x00010000); /* R6: RCA 1. */
        break;
    case 8:
        sf2000_sdio_set_short_response(0x000001aa);
        break;
    case 9:
        sf2000_sdio_resp[0] = 0x0009003f;
        sf2000_sdio_resp[1] = 0xd17f0d00;
        sf2000_sdio_resp[2] = 0x5b590001;
        sf2000_sdio_resp[3] = 0x400e0032;
        break;
    case 41:
        if (!is_app_cmd) {
            sf2000_sdio_set_short_response(0x00000900);
            break;
        }
        /*
         * ACMD41 OCR response. The model reports ready with broad voltage
         * support because the goal is testing SF2000 firmware behavior rather
         * than SD-card electrical negotiation.
         */
        sf2000_sdio_set_short_response(0xc0ff8000);
        break;
    case 51:
        if (is_app_cmd) {
            sf2000_sdio_dma_read_scr();
        }
        sf2000_sdio_set_short_response(0x00000900);
        break;
    case 55:
        sf2000_sdio_set_short_response(0x00000020); /* APP_CMD accepted. */
        sf2000_sdio_app_cmd = true;
        break;
    case 13:
        sf2000_sdio_set_short_response(0x00000900); /* Ready, transfer state. */
        break;
    case 7:
    case 16:
        sf2000_sdio_resp[0] = 0;
        break;
    case 17:
        sf2000_sdio_xfer_done = false;
        sf2000_sdio_xfer_busy = true;
        sf2000_sdio_dma_read(sf2000_sdio_arg);
        sf2000_sdio_resp[0] = 0;
        break;
    case 18:
        sf2000_sdio_xfer_done = false;
        sf2000_sdio_xfer_busy = true;
        sf2000_sdio_dma_read(sf2000_sdio_arg);
        sf2000_sdio_resp[0] = 0;
        break;
    case 24:
        sf2000_sdio_xfer_done = false;
        sf2000_sdio_xfer_busy = true;
        sf2000_sdio_write_active = true;
        sf2000_sdio_dma_write(sf2000_sdio_arg);
        sf2000_sdio_resp[0] = 0;
        break;
    case 25:
        sf2000_sdio_xfer_done = false;
        sf2000_sdio_xfer_busy = true;
        sf2000_sdio_write_active = true;
        sf2000_sdio_dma_write(sf2000_sdio_arg);
        sf2000_sdio_resp[0] = 0;
        break;
    default:
        sf2000_sdio_set_short_response(0x00000900);
        break;
    }

    if (sf2000_sdio_cmd != 55) {
        sf2000_sdio_app_cmd = false;
    }

    /* Bit 6 is the HC15xx command/data completion indication. */
    sf2000_sdio_xfer_done = true;
    sf2000_sdio_xfer_busy = false;
    sf2000_sdio_irq_pending = true;
    sf2000_sdio_callback_pending = true;

    if (sf2000_trace_sdio()) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: sdio-cmd cmd=%u%s arg=0x%08x resp=%08x %08x %08x %08x bus=%u\n",
                      sf2000_sdio_cmd, is_app_cmd ? " app" : "",
                      sf2000_sdio_arg, sf2000_sdio_resp[0],
                      sf2000_sdio_resp[1], sf2000_sdio_resp[2],
                      sf2000_sdio_resp[3],
                      sf2000_sdio_bus_width ? sf2000_sdio_bus_width : 1);
    }
}

static void sf2000_panel_update_rect(SF2000LCDState *s, uint16_t x, uint16_t y)
{
    if (!s || x >= sf2000_panel_raw_width(s) ||
        y >= sf2000_panel_raw_height(s)) {
        return;
    }
    sf2000_panel_store_raw(s, x, y,
                           s->panel_pixels[y * sf2000_panel_raw_width(s) + x]);
}

static size_t sf2000_panel_fill_readback(const SF2000BoardProfileSpec *profile,
                                         uint8_t cmd, uint8_t resp[5])
{
    size_t len = 0;

    memset(resp, 0, 5);

    switch (profile->panel_id) {
    case 0x00858552:
        switch (cmd) {
        case 0x00:
            resp[0] = 0xe0;
            resp[1] = 0xe0;
            len = 2;
            break;
        case 0x04:
            resp[0] = 0xe4;
            resp[1] = 0x85;
            resp[2] = 0x85;
            resp[3] = 0x52;
            len = 4;
            break;
        case 0x09:
            resp[0] = 0xe9;
            resp[1] = 0x00;
            resp[2] = 0x61;
            resp[3] = 0x00;
            resp[4] = 0x00;
            len = 5;
            break;
        case 0x0a:
            resp[0] = 0xea;
            resp[1] = 0x08;
            len = 2;
            break;
        case 0x0c:
            resp[0] = 0xec;
            resp[1] = 0x06;
            len = 2;
            break;
        case 0xb3:
            resp[0] = 0xb3;
            resp[1] = 0xf3;
            resp[2] = 0xf3;
            resp[3] = 0xf3;
            len = 4;
            break;
        case 0xd3:
            resp[0] = 0xf3;
            resp[1] = 0xf3;
            resp[2] = 0xf3;
            resp[3] = 0xf3;
            len = 4;
            break;
        case 0xda:
            resp[0] = 0xfa;
            resp[1] = 0x85;
            len = 2;
            break;
        case 0xdb:
            resp[0] = 0xfb;
            resp[1] = 0x85;
            len = 2;
            break;
        case 0xdc:
            resp[0] = 0xfc;
            resp[1] = 0x52;
            len = 2;
            break;
        case 0xf2:
            resp[0] = 0xf2;
            resp[1] = 0xf2;
            resp[2] = 0xf2;
            resp[3] = 0xf2;
            len = 4;
            break;
        default:
            break;
        }
        break;
    case 0x00009306:
        switch (cmd) {
        case 0x00:
            resp[0] = 0x00;
            resp[1] = 0x00;
            len = 2;
            break;
        case 0x04:
            resp[0] = 0x00;
            resp[1] = 0x00;
            resp[2] = 0x93;
            resp[3] = 0x06;
            len = 4;
            break;
        case 0x09:
            resp[0] = 0x94;
            resp[1] = 0x94;
            resp[2] = 0x53;
            resp[3] = 0x04;
            resp[4] = 0x00;
            len = 5;
            break;
        case 0x0a:
            resp[0] = 0x00;
            resp[1] = 0x00;
            len = 2;
            break;
        case 0x0c:
            resp[0] = 0x00;
            resp[1] = 0x00;
            len = 2;
            break;
        case 0xd3:
            resp[0] = 0x00;
            resp[1] = 0x00;
            resp[2] = 0x93;
            resp[3] = 0x06;
            len = 4;
            break;
        case 0xda:
            resp[0] = 0x00;
            resp[1] = 0x00;
            len = 2;
            break;
        case 0xdb:
            resp[0] = 0x93;
            resp[1] = 0x93;
            len = 2;
            break;
        case 0xdc:
            resp[0] = 0x06;
            resp[1] = 0x06;
            len = 2;
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }

    return len;
}

static bool sf2000_panel_is_readback_cmd(uint8_t cmd)
{
    switch (cmd) {
    case 0x00:
    case 0x04:
    case 0x09:
    case 0x0a:
    case 0x0c:
    case 0xb3:
    case 0xd3:
    case 0xda:
    case 0xdb:
    case 0xdc:
    case 0xf2:
        return true;
    default:
        return false;
    }
}

static void sf2000_panel_prepare_readback(SF2000LCDState *s)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();

    s->panel_readback_len =
        sf2000_panel_fill_readback(profile, s->panel_cmd, s->panel_readback);
    s->panel_readback_byte = 0;
    s->panel_readback_bit = 0;
    s->panel_readback_active = s->panel_readback_len > 0;
}

static uint8_t sf2000_panel_readback_value(SF2000LCDState *s)
{
    if (!s->panel_readback_active) {
        return 0;
    }

    return s->panel_readback[s->panel_readback_byte];
}

static uint32_t sf2000_panel_sample_readback_l(SF2000LCDState *s,
                                               uint32_t value)
{
    uint8_t data;

    if (!s->panel_readback_active) {
        return value;
    }

    data = sf2000_panel_readback_value(s);
    value &= ~(0x1fu << 2);
    value |= (data & 0x1f) << 2;
    return value;
}

static uint32_t sf2000_panel_sample_readback_t(SF2000LCDState *s,
                                               uint32_t value)
{
    uint8_t data;

    if (!s->panel_readback_active) {
        return value;
    }

    data = sf2000_panel_readback_value(s);
    value &= ~(0x7u << 9);
    value |= (data & 0xe0) << 4;
    return value;
}

static void sf2000_panel_readback_selftest(void)
{
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();
    static const uint8_t commands[] = {
        0x00, 0x04, 0x09, 0x0a, 0x0c, 0xd3, 0xda, 0xdb, 0xdc,
    };
    uint8_t expected[5];
    uint8_t actual[5];
    size_t i;

    for (i = 0; i < ARRAY_SIZE(commands); i++) {
        size_t expected_len;
        size_t actual_len;

        memset(expected, 0, sizeof(expected));
        switch (profile->panel_id) {
        case 0x00858552:
            switch (commands[i]) {
            case 0x00:
                memcpy(expected, (uint8_t[]){ 0xe0, 0xe0 }, 2);
                expected_len = 2;
                break;
            case 0x04:
                memcpy(expected, (uint8_t[]){ 0xe4, 0x85, 0x85, 0x52 }, 4);
                expected_len = 4;
                break;
            case 0x09:
                memcpy(expected, (uint8_t[]){ 0xe9, 0x00, 0x61, 0x00, 0x00 }, 5);
                expected_len = 5;
                break;
            case 0x0a:
                memcpy(expected, (uint8_t[]){ 0xea, 0x08 }, 2);
                expected_len = 2;
                break;
            case 0x0c:
                memcpy(expected, (uint8_t[]){ 0xec, 0x06 }, 2);
                expected_len = 2;
                break;
            case 0xd3:
                memcpy(expected, (uint8_t[]){ 0xf3, 0xf3, 0xf3, 0xf3 }, 4);
                expected_len = 4;
                break;
            case 0xda:
                memcpy(expected, (uint8_t[]){ 0xfa, 0x85 }, 2);
                expected_len = 2;
                break;
            case 0xdb:
                memcpy(expected, (uint8_t[]){ 0xfb, 0x85 }, 2);
                expected_len = 2;
                break;
            case 0xdc:
                memcpy(expected, (uint8_t[]){ 0xfc, 0x52 }, 2);
                expected_len = 2;
                break;
            default:
                expected_len = 0;
                break;
            }
            break;
        case 0x00009306:
            switch (commands[i]) {
            case 0x00:
                memcpy(expected, (uint8_t[]){ 0x00, 0x00 }, 2);
                expected_len = 2;
                break;
            case 0x04:
                memcpy(expected, (uint8_t[]){ 0x00, 0x00, 0x93, 0x06 }, 4);
                expected_len = 4;
                break;
            case 0x09:
                memcpy(expected, (uint8_t[]){ 0x94, 0x94, 0x53, 0x04, 0x00 }, 5);
                expected_len = 5;
                break;
            case 0x0a:
                memcpy(expected, (uint8_t[]){ 0x00, 0x00 }, 2);
                expected_len = 2;
                break;
            case 0x0c:
                memcpy(expected, (uint8_t[]){ 0x00, 0x00 }, 2);
                expected_len = 2;
                break;
            case 0xd3:
                memcpy(expected, (uint8_t[]){ 0x00, 0x00, 0x93, 0x06 }, 4);
                expected_len = 4;
                break;
            case 0xda:
                memcpy(expected, (uint8_t[]){ 0x00, 0x00 }, 2);
                expected_len = 2;
                break;
            case 0xdb:
                memcpy(expected, (uint8_t[]){ 0x93, 0x93 }, 2);
                expected_len = 2;
                break;
            case 0xdc:
                memcpy(expected, (uint8_t[]){ 0x06, 0x06 }, 2);
                expected_len = 2;
                break;
            default:
                expected_len = 0;
                break;
            }
            break;
        default:
            expected_len = 0;
            break;
        }

        actual_len = sf2000_panel_fill_readback(profile, commands[i], actual);
        if (expected_len != actual_len ||
            memcmp(expected, actual, expected_len) != 0) {
            error_report("sf2000: panel readback selftest failed board=%s cmd=0x%02x "
                         "expected_len=%zu actual_len=%zu expected=%02x:%02x:%02x:%02x:%02x "
                         "actual=%02x:%02x:%02x:%02x:%02x",
                         profile->name, commands[i],
                         expected_len, actual_len,
                         expected[0], expected[1], expected[2], expected[3], expected[4],
                         actual[0], actual[1], actual[2], actual[3], actual[4]);
            return;
        }
    }

    qemu_log_mask(LOG_UNIMP,
                  "sf2000: panel readback selftest ok board=%s panel=0x%08x\n",
                  profile->name, profile->panel_id);
}

static void sf2000_panel_commit_arg(SF2000LCDState *s, uint16_t value)
{
    uint16_t *args = s->panel_args;

    if (s->panel_cmd != 0x2c && s->panel_cmd_count <= 64) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: panel-data cmd=0x%02x index=%u value=0x%04x\n",
                      s->panel_cmd, s->panel_data_count, value);
    }
    s->panel_data_count++;

    switch (s->panel_cmd) {
    case 0xb0: /* RAMCTRL: RAM/display interface ownership */
        if (s->panel_ramctrl_count < ARRAY_SIZE(s->panel_ramctrl)) {
            s->panel_ramctrl[s->panel_ramctrl_count++] = value & 0xff;
        }
        if (s->panel_ramctrl_count == ARRAY_SIZE(s->panel_ramctrl)) {
            s->panel_ramctrl_explicit = true;
            qemu_log_mask(LOG_UNIMP,
                          "sf2000: panel RAMCTRL access=%s display=%s value=%02x:%02x\n",
                          (s->panel_ramctrl[0] & BIT(4)) ? "RGB" : "MCU",
                          (s->panel_ramctrl[0] & 3) == 1 ? "RGB" :
                          (s->panel_ramctrl[0] & 3) == 2 ? "VSYNC" : "MCU",
                          s->panel_ramctrl[0], s->panel_ramctrl[1]);
            if ((s->panel_ramctrl[0] & 0x13) == 0x11) {
                s->panel_rgb_handoff_synchronized =
                    s->vou_setup_seen && s->vou_latch_stage >= 10 &&
                    sf2000_active_gma[0] != 0;
                if (!s->panel_rgb_handoff_synchronized &&
                    !s->panel_rgb_handoff_order_logged) {
                    s->panel_rgb_handoff_order_logged = true;
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "sf2000: panel entered RGB mode before VOU/GMA raster was active\n");
                }
            }
        }
        break;
    case 0x2a:
        if (s->panel_arg_count < ARRAY_SIZE(s->panel_args)) {
            args[s->panel_arg_count++] = value;
        }
        if (s->panel_arg_count == 4) {
            s->panel_x0 = ((args[0] & 0xff) << 8) | (args[1] & 0xff);
            s->panel_x1 = ((args[2] & 0xff) << 8) | (args[3] & 0xff);
        }
        break;
    case 0x2b:
        if (s->panel_arg_count < ARRAY_SIZE(s->panel_args)) {
            args[s->panel_arg_count++] = value;
        }
        if (s->panel_arg_count == 4) {
            s->panel_y0 = ((args[0] & 0xff) << 8) | (args[1] & 0xff);
            s->panel_y1 = ((args[2] & 0xff) << 8) | (args[3] & 0xff);
        }
        break;
    case 0x36: /* MADCTL: orientation and RGB/BGR bus ordering */
        if (s->panel_data_count == 1) {
            s->panel_madctl = value & 0xff;
            qemu_log_mask(LOG_UNIMP,
                          "sf2000: panel MADCTL=0x%02x board=%s\n",
                          s->panel_madctl, sf2000_board_profile_name());
        }
        break;
    case 0x2c:
        if (s->panel_x < sf2000_panel_raw_width(s) &&
            s->panel_y < sf2000_panel_raw_height(s)) {
            s->panel_pixels[s->panel_y * sf2000_panel_raw_width(s) + s->panel_x] =
                sf2000_rgb565_to_surface(value);
            s->panel_pixel_count++;
            if (s->panel_pixel_count <= 16 ||
                (s->panel_pixel_count % 4096) == 0) {
                qemu_log_mask(LOG_UNIMP,
                              "sf2000: panel-pixel n=%u x=%u y=%u rgb565=0x%04x\n",
                              s->panel_pixel_count, s->panel_x, s->panel_y,
                              value);
            }
            sf2000_panel_update_rect(s, s->panel_x, s->panel_y);
        }
        if (s->panel_x >= s->panel_x1) {
            s->panel_x = s->panel_x0;
            if (s->panel_y < s->panel_y1) {
                s->panel_y++;
            }
        } else {
            s->panel_x++;
        }
        s->panel_readback_active = false;
        break;
    default:
        break;
    }
}

static void sf2000_panel_latch(SF2000LCDState *s, uint16_t value)
{
	bool trace_command;

	if (s->panel_rs) {
        sf2000_panel_commit_arg(s, value);
        return;
    }

	s->panel_cmd = value & 0xff;
	s->panel_arg_count = 0;
	s->panel_data_count = 0;
	s->panel_cmd_count++;
	if (s->panel_cmd == 0x01) { /* SWRESET restores ST7789 RAMCTRL defaults. */
		s->panel_madctl = sf2000_panel_is_gb300() ? 0x28 : 0x70;
		s->panel_ramctrl[0] = 0x00;
		s->panel_ramctrl[1] = 0xf0;
		s->panel_ramctrl_count = 0;
		s->panel_ramctrl_explicit = false;
		s->panel_ramctrl_handoff_logged = false;
		s->panel_rgb_handoff_synchronized = false;
		s->panel_rgb_handoff_order_logged = false;
	} else if (s->panel_cmd == 0xb0) {
		s->panel_ramctrl_count = 0;
	}
	/* RAMWR is issued on every TE edge; retain useful early/periodic traces
	 * without filling the QEMU log at the panel refresh rate. */
	trace_command = s->panel_cmd_count <= 64 ||
		(s->panel_cmd == 0x2c && (s->panel_cmd_count % 60u) == 0);
	if (trace_command) {
		qemu_log_mask(LOG_UNIMP,
                      "sf2000: panel-cmd cmd=0x%02x raw=0x%04x count=%u\n",
                      s->panel_cmd, value, s->panel_cmd_count);
	}
	if (s->panel_cmd == 0x2c) {
        s->panel_x = s->panel_x0;
        s->panel_y = s->panel_y0;
		/*
		 * The SF2000 ST7789 driver does not write RAMCTRL.  Its L08 TE
		 * callback reissues CASET/RASET/RAMWR while VOU/GMA keeps running,
		 * then immediately restores the RGB pads.  Treat that recovered
		 * transaction—not an inferred RAMCTRL value—as the handoff latch.
		 */
		if (s->vou_setup_seen && s->vou_latch_stage >= 10 &&
		    sf2000_active_gma[0] != 0) {
			s->panel_rgb_handoff_synchronized = true;
		}
		if (trace_command)
			qemu_log_mask(LOG_UNIMP,
				      "sf2000: panel-ramwr x=%u..%u y=%u..%u pixels=%u\n",
				      s->panel_x0, s->panel_x1, s->panel_y0, s->panel_y1,
				      s->panel_pixel_count);
        s->panel_readback_active = false;
	} else if (s->panel_cmd == 0x29) {
		/*
		 * The stock SF2000 sequence has no final RAMWR: after CASET/RASET it
		 * issues INVON/DISPON and lets the first RGB VSYNC restart the panel
		 * address counter.  This panel-side readiness may precede VOU setup;
		 * scanout remains independently gated on the completed VOU latch.
		 * MuFrog instead supplies the RAMWR branch above.
		 */
		s->panel_rgb_handoff_synchronized = true;
		s->panel_readback_active = false;
    } else if (sf2000_panel_is_readback_cmd(s->panel_cmd)) {
        sf2000_panel_prepare_readback(s);
        if (s->panel_readback_len) {
            qemu_log_mask(LOG_UNIMP,
                          "sf2000: panel-read cmd=0x%02x bytes=%u data=%02x:%02x:%02x:%02x:%02x\n",
                          s->panel_cmd, s->panel_readback_len,
                          s->panel_readback[0], s->panel_readback[1],
                          s->panel_readback[2], s->panel_readback[3],
                          s->panel_readback[4]);
        }
    } else {
        s->panel_readback_active = false;
    }
}

static uint16_t sf2000_panel_data_from_gpio(SF2000LCDState *s)
{
    /*
     * Stock panel init bit-bangs an ST7789V-compatible 8080 bus over split GPIO
     * banks. The mapping below was inferred from the existing UniFrog display
     * path and real logs: GPIO_L contributes low data bits while GPIO bank 3
     * contributes the remaining bus bits and RS. WR rising edge latches.
     */
    return ((s->gpio54 >> 2) & 0x1f) |
           ((s->gpio354 >> 4) & 0x7e0) |
           ((s->gpio354 & 0x7c) << 9);
}

static void sf2000_panel_gpio_write(hwaddr full_addr, uint32_t value)
{
    SF2000LCDState *s = sf2000_lcd;
    bool old_wr;
    bool new_wr;
    bool old_reset_n;
    bool old_rd;
    bool new_rd;

    if (!s) {
        return;
    }

    old_reset_n = !!(s->gpio54 & BIT(1));
    old_rd = !!(s->gpio354 & BIT(0));
    if (full_addr == 0x18800054) {
        s->gpio54 = value;
        /* LCD reset is active-low on L01. */
        if (old_reset_n && !(value & BIT(1))) {
            s->panel_ramctrl[0] = 0x00;
            s->panel_ramctrl[1] = 0xf0;
            s->panel_ramctrl_count = 0;
            s->panel_ramctrl_handoff_logged = false;
            s->panel_rgb_handoff_synchronized = false;
            s->panel_rgb_handoff_order_logged = false;
            qemu_log_mask(LOG_UNIMP,
                          "sf2000: panel hardware reset RAMCTRL=00:f0\n");
        }
    } else if (full_addr == 0x18800354) {
        s->gpio354 = value;
    } else {
        return;
    }

    old_wr = s->panel_wr;
    new_wr = !!(s->gpio54 & BIT(7));
    s->panel_rs = !!(s->gpio354 & BIT(1));
    s->panel_wr = new_wr;
    new_rd = !!(s->gpio354 & BIT(0));

    /*
     * RD low presents one complete byte on the split 8080 data bus.  Guest
     * software samples the individual L/T pads through several MMIO reads;
     * advance only when RD rises, exactly as the physical panel does.
     */
    if (!old_rd && new_rd && s->panel_readback_active) {
        s->panel_readback_byte++;
        if (s->panel_readback_byte >= s->panel_readback_len) {
            s->panel_readback_active = false;
        }
    }

    if (!old_wr && new_wr) {
        sf2000_panel_latch(s, sf2000_panel_data_from_gpio(s));
    }
}

static void sf2000_write_ppm_from_surface(SF2000LCDState *s, const char *path)
{
    DisplaySurface *surface;
    FILE *f;
    int y;

    f = fopen(path, "wb");
    if (!f) {
        qemu_log_mask(LOG_UNIMP, "sf2000: gma-dump-open-failed path=%s\n",
                      path);
        return;
    }

    surface = qemu_console_surface(s->con);
    fprintf(f, "P6\n%u %u\n255\n", sf2000_panel_surface_width(s),
            sf2000_panel_surface_height(s));
    for (y = 0; y < (int)sf2000_panel_surface_height(s); y++) {
        const uint32_t *src = (const uint32_t *)(surface_data(surface) +
                              y * surface_stride(surface));
        int x;

        for (x = 0; x < (int)sf2000_panel_surface_width(s); x++) {
            uint8_t rgb[3] = {
                (uint8_t)(src[x] >> 16),
                (uint8_t)(src[x] >> 8),
                (uint8_t)src[x],
            };
            fwrite(rgb, sizeof(rgb), 1, f);
        }
    }
    fclose(f);
    qemu_log_mask(LOG_UNIMP, "sf2000: gma-dump path=%s\n", path);
}

static void sf2000_dump_ppm_from_surface(SF2000LCDState *s,
                                         const char *tag)
{
    g_autofree char *path = NULL;
    g_autofree char *latest_path = NULL;
    uint32_t limit = 16;
    const char *dir = g_getenv("SF2000_GMA_DUMP_DIR");
    const char *limit_s = g_getenv("SF2000_GMA_DUMP_LIMIT");

    if (!dir || !*dir || !s || !s->con) {
        return;
    }
    if (limit_s && *limit_s) {
        limit = MAX(1, (uint32_t)g_ascii_strtoull(limit_s, NULL, 0));
    }

    g_mkdir_with_parents(dir, 0755);
    latest_path = g_strdup_printf("%s/sf2000-%s-latest.ppm", dir, tag);
    sf2000_write_ppm_from_surface(s, latest_path);

    if (sf2000_gma_dump_count >= limit) {
        return;
    }
    path = g_strdup_printf("%s/sf2000-%s-%04u.ppm", dir, tag,
                           sf2000_gma_dump_count++);
    sf2000_write_ppm_from_surface(s, path);
}

static uint64_t sf2000_unimp_read(void *opaque, hwaddr addr, unsigned size)
{
    hwaddr full_addr = SF2000_MMIO_BASE + addr;
    uint64_t value = 0;
    unsigned timer_index;
    unsigned timer_offset;
    unsigned uart_index;
    unsigned uart_offset;
    unsigned i2c_index;
    unsigned usb_index;
    unsigned usb_offset;
    unsigned pwm_channel;
    unsigned pwm_offset;
    unsigned irc_offset;
    unsigned i;

    if (full_addr == SF2000_IRQ_STATUS1) {
        bool timer_pending = sf2000_sb_timer_pending();
        bool gpio_vsync_pending = sf2000_gpio_l_vsync_pending();

        value = timer_pending ? SF2000_SB_TIMER_IRQ : 0;
        if (gpio_vsync_pending) {
            value |= SF2000_GPIO_IRQ;
        }
        if (sf2000_uart_irq_pending(0)) {
            value |= SF2000_UART0_IRQ;
        }
        if (sf2000_uart_irq_pending(1)) {
            value |= SF2000_UART1_IRQ;
        }
        for (i2c_index = 0; i2c_index < 2; i2c_index++) {
            if (sf2000_i2c_irq_pending(i2c_index)) {
                value |= sf2000_i2c_irq_mask(i2c_index);
            }
        }
        if (sf2000_irc_irq_pending()) {
            value |= SF2000_IRC_IRQ;
        }
        if (sf2000_audio_irq_pending()) {
            value |= SF2000_SND_IRQ;
        }
        if (sf2000_ge_irq_pending) {
            value |= SF2000_GE_IRQ;
        }
        if (sf2000_sdio_irq_pending) {
            value |= SF2000_SDIO_IRQ;
            /*
             * HC15xx SDIO completion is an edge at the cascaded interrupt
             * controller.  The Linux HC15 driver consumes the completion
             * status in the same interrupt transaction; retaining this bit
             * after the aggregate status read turns it into a level IRQ and
             * traps the generic Hichip irqchip in EIRQ3 forever when the
             * userspace screen starts.  The command/DMA state remains
             * available through the SDIO registers, so only the synthetic
             * aggregate edge is consumed here.
             */
            sf2000_sdio_irq_pending = false;
        }
        /*
         * Before TIMER5 is programmed, the model injects a synthetic scheduler
         * tick so stock boot leaves CP0 idle loops. Hardware presents timer
         * interrupts as pulses; keeping the emulated line asserted after the
         * status read lets the firmware re-enter its interrupt dispatcher and
         * trip its nested-interrupt guard. Once TIMER5 exists, control-register
         * acknowledgement handles the edge.
         */
        if (timer_pending && !sf2000_timer5_configured()) {
            sf2000_timer_ack();
        }
        if (timer_pending) {
            /*
             * The interrupt controller reports the south-bridge timer as an
             * edge to the CPU. If the emulated EIRQ3 line remains level-high
             * after the aggregate status read, the stock handler re-enters
             * before it reaches the per-timer clear path and trips its nested
             * interrupt guard. Mask only the CPU line for this timer edge; the
             * child timer control register still exposes INT_ST until firmware
             * acknowledges or reprograms it.
             */
            sf2000_sb_timer_irq_masked = true;
        }
        if (gpio_vsync_pending) {
            /*
             * The emulated ST7789V TE signal is an edge, not a level held by a
             * real GPIO pad. Stock firmware consumes it through the aggregate
             * status. Linux performs only a bounded userspace ownership
             * handoff by sampling the emulated pad, so neither guest requires
             * a persistent GPIO child latch after the aggregate observation.
             */
            sf2000_mmio_set32(SF2000_GPIO_L_ISR, 0);
        }
    } else if (full_addr == SF2000_IRQ_STATUS2) {
        value = 0;
    } else if ((full_addr & ~3u) == SF2000_GPIO_L_IN) {
        value = 0x07800102;
        for (i = 0; i < ARRAY_SIZE(sf2000_regs); i++) {
            if (sf2000_regs[i].valid && sf2000_regs[i].addr == SF2000_GPIO_L_IN) {
                value = sf2000_regs[i].value;
                break;
            }
        }
        if (strcmp(sf2000_board_profile_name(), "gb300") == 0)
            value |= BIT(27);
        /*
         * L08 is the panel's TE input.  Linux's userspace screen polls this
         * pin after taking ownership from the kernel GPIO IRQ, so keep the
         * electrical level moving even when the interrupt enable bit is
         * intentionally masked.  The aggregate IRQ remains gated by
         * sf2000_gpio_l_vsync_enabled(); this only models the observable pad.
         */
        if (sf2000_lcd) {
            uint32_t panel_te_hz = sf2000_lcd->panel_te_hz;
            int64_t half_period;

            if (!panel_te_hz) {
                panel_te_hz = 60;
            }
            half_period = NANOSECONDS_PER_SECOND /
                ((int64_t)panel_te_hz * 2);
            if ((qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / half_period) & 1) {
                value |= SF2000_GPIO_L08;
            } else {
                value &= ~SF2000_GPIO_L08;
            }
        }
        /* SD card detect is PINPAD_L22, active low on the SF2000 DTS. */
        value &= ~BIT(22);
        value = sf2000_gpio_l_sample(value);
        value = sf2000_rf_gpio_l_sample(value);
        value >>= ((full_addr & 3u) * 8u);
        if (size < 4) {
            value &= (1u << (size * 8)) - 1u;
        }
    } else if ((full_addr & ~3u) == 0x18800350) {
        value = 0x00000083;
        for (i = 0; i < ARRAY_SIZE(sf2000_regs); i++) {
            if (sf2000_regs[i].valid &&
                sf2000_regs[i].addr == 0x18800350) {
                value = sf2000_regs[i].value;
                break;
            }
        }
        if (sf2000_lcd) {
            value = sf2000_panel_sample_readback_t(sf2000_lcd, value);
        }
        value >>= ((full_addr & 3u) * 8u);
        if (size < 4) {
            value &= (1u << (size * 8)) - 1u;
        }
    } else if (sf2000_uart_decode(full_addr, &uart_index, &uart_offset) &&
               uart_offset == SF2000_UART_IER) {
        value = sf2000_uart_ier[uart_index];
    } else if (sf2000_uart_decode(full_addr, &uart_index, &uart_offset) &&
               uart_offset == SF2000_UART_IIR) {
        /*
         * The HCRTOS 16550 driver reads IIR to clear the interrupt, then
         * treats 0x02 as transmitter-holding-register empty and completes
         * blocked writes. The emulated FIFO drains immediately, so expose a
         * one-shot THRI interrupt whenever THRI is enabled.
         */
        if (sf2000_uart_irq_pending(uart_index)) {
            value = SF2000_UART_IIR_THRI;
            sf2000_uart_thri_pending[uart_index] = false;
        } else {
            value = SF2000_UART_IIR_NONE;
        }
    } else if (sf2000_uart_decode(full_addr, &uart_index, &uart_offset) &&
               uart_offset == SF2000_UART_LSR) {
        value = SF2000_UART_LSR_THRE | SF2000_UART_LSR_TEMT;
    } else if (sf2000_i2c_decode(full_addr, &i2c_index, &uart_offset)) {
        value = sf2000_i2c_read(full_addr, size);
    } else if (sf2000_usb_decode(full_addr, &usb_index, &usb_offset)) {
        value = sf2000_usb_read(full_addr, size);
    } else if (sf2000_pwm_decode(full_addr, &pwm_channel, &pwm_offset)) {
        uint32_t pwm_value = 0;

        sf2000_mmio_get32(full_addr, &pwm_value);
        value = pwm_value >> ((full_addr & 3u) * 8u);
        if (size < 4) {
            value &= (1u << (size * 8)) - 1u;
        }
    } else if (sf2000_irc_decode(full_addr, &irc_offset)) {
        value = sf2000_irc_read(full_addr);
    } else if (full_addr >= SF2000_ADC_BASE &&
               full_addr < SF2000_ADC_BASE + SF2000_ADC_SIZE) {
        value = sf2000_adc_read(full_addr, size);
    } else if (sf2000_wdt_decode(full_addr)) {
        value = 0;
    } else if ((full_addr & ~3u) == SF2000_AUDIO_I2S_BASE + 0xb4 ||
               (full_addr & ~3u) == SF2000_AUDIO_I2S_BASE + 0xbc) {
        unsigned id = (full_addr & ~3u) ==
                      SF2000_AUDIO_I2S_BASE + 0xbc;

        value = sf2000_audio_stc_read(id);
        value >>= ((full_addr & 3u) * 8u);
        if (size < 4) {
            value &= (1u << (size * 8)) - 1u;
        }
    } else if (full_addr == SF2000_AUDIO_I2S_BASE + 0x38) {
        value = (sf2000_audio_dma_consumer << 12) |
                (sf2000_audio_dma_producer >> 4);
    } else if (full_addr == SF2000_AUDIO_I2S_BASE + 0x3a) {
        value = sf2000_audio_dma_consumer >> 4;
    } else if (full_addr >= 0x1884c010 && full_addr < 0x1884c020) {
        unsigned off = full_addr - 0x1884c010;
        value = sf2000_sdio_resp[off >> 2] >> ((off & 3) * 8);
        if (size < 4) {
            value &= (1u << (size * 8)) - 1u;
        }
    } else if (full_addr == 0x1884c001) {
        /*
         * Stock sd_m33 helpers poll this byte for the command/data state.
         * The helper traces show an in-flight 0xa8 family while the controller
         * is busy and the terminal 0xe4 family once the transfer completes.
         */
        value = (sf2000_sdio_xfer_busy || sf2000_sdio_write_active) ? 0xa8 : 0xe4;
    } else if (full_addr == 0x1884c00e) {
        value = sf2000_sdio_pio_state;
    } else if (full_addr == 0x1884c00b) {
        value = sf2000_sdio_xfer_done ? 0x0c : 0x09;
    } else if (full_addr == 0x1884c030) {
        value = sf2000_sdio_xfer_done ? 0x6c :
                (sf2000_sdio_xfer_busy ? 0x21 : 0x20);
    } else if (sf2000_ge_decode(full_addr)) {
        uint32_t ge_value;

        sf2000_mmio_get32(full_addr, &ge_value);
        value = ge_value >> ((full_addr & 3u) * 8u);
        if (size < 4) {
            value &= (1u << (size * 8)) - 1u;
        }
    } else if (full_addr == SF2000_TIMER_BASE + 0x08 &&
               sf2000_sdio_callback_pending) {
        value = sf2000_timer_ctrl[0] | SF2000_TIMER_CTRL_INT;
    } else if (full_addr == SF2000_TIMER_BASE + 0x18 &&
               sf2000_sdio_callback_pending) {
        value = sf2000_timer_ctrl[1] | SF2000_TIMER_CTRL_INT;
    } else if (full_addr == 0x1880f04b) {
        value = 0; /* AUX command complete / idle. */
    } else if (full_addr == 0x1882e09a) {
        value = 0x40; /* SFLASH RX ready. */
    } else if (full_addr == 0x1882e0a0) {
        value = 0x00000001; /* SFLASH DMA/PIO completion. */
    } else if (sf2000_timer_decode(full_addr, &timer_index, &timer_offset)) {
        switch (timer_offset) {
        case 0:
            if (sf2000_timer_is_sec_ms(timer_index)) {
                value = sf2000_timer_cnt[timer_index];
            } else {
                value = sf2000_timer_ticks() + sf2000_timer_cnt[timer_index];
            }
            break;
        case 4:
            value = sf2000_timer_aim[timer_index];
            break;
        case 8:
            value = sf2000_timer_ctrl[timer_index];
            /*
             * INT_ST is per timer. The CPU interrupt bank only consumes
             * TIMER5 for the scheduler tick, but stock menu code polls other
             * timer control registers directly after mounting the SD card.
             */
            if (timer_index == SF2000_TIMER5_INDEX ? sf2000_timer5_pending() :
                sf2000_timer_pending(timer_index)) {
                value |= SF2000_TIMER_CTRL_INT;
            }
            if (sf2000_timer_is_sec_ms(timer_index)) {
                value |= (sf2000_timer_ms() & 0x3ffu) << 16;
            }
            break;
        case 12:
            value = sf2000_timer_is_sec_ms(timer_index) ?
                    sf2000_timer_ms() / 1000u :
                    sf2000_timer_ticks() / 1000000u;
            break;
        default:
            value = 0;
            break;
        }
    } else {
        for (i = 0; i < ARRAY_SIZE(sf2000_regs); i++) {
            if (sf2000_regs[i].valid &&
                sf2000_regs[i].addr == (full_addr & ~3u)) {
                value = sf2000_regs[i].value >> ((full_addr & 3u) * 8u);
                if (size < 4) {
                    value &= (1u << (size * 8)) - 1u;
                }
                break;
            }
        }
        if (i == ARRAY_SIZE(sf2000_regs)) {
            for (i = 0; i < ARRAY_SIZE(sf2000_reg_defaults); i++) {
                if (sf2000_reg_defaults[i].addr == (full_addr & ~3u)) {
                    value = sf2000_reg_defaults[i].value >>
                            ((full_addr & 3u) * 8u);
                    if (size < 4) {
                        value &= (1u << (size * 8)) - 1u;
                    }
                    break;
                }
            }
        }
    }

    sf2000_update_irq();
    sf2000_log_mmio("mmio-read", full_addr, value, size);
    return value;
}

static void sf2000_unimp_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    hwaddr full_addr = SF2000_MMIO_BASE + addr;
    uint32_t old_value = 0;
    uint32_t mask = size >= 4 ? 0xffffffffu : ((1u << (size * 8)) - 1u);
    unsigned shift = (full_addr & 3u) * 8u;
    unsigned empty = ARRAY_SIZE(sf2000_regs);
    unsigned timer_index;
    unsigned timer_offset;
    unsigned adc_index;
    unsigned adc_offset;
    unsigned uart_index;
    unsigned uart_offset;
    unsigned i2c_index;
    unsigned i2c_offset;
    unsigned usb_index;
    unsigned usb_offset;
    unsigned pwm_channel;
    unsigned pwm_offset;
    unsigned irc_offset;
    unsigned i;

    if (sf2000_trace_gma() && full_addr >= SF2000_GMA_BASE &&
        full_addr < SF2000_GMA_BASE + 0x1000) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: gma-reg-write addr=0x%08" HWADDR_PRIx
                      " size=%u value=0x%08" PRIx64 "\n",
                      full_addr, size, value);
    }
    if (sf2000_trace_gma() &&
        (full_addr == 0x18800060 || full_addr == 0x18800064 ||
         full_addr == 0x18800078 || full_addr == 0x18800084 ||
         full_addr == 0x18800094 || full_addr == 0x188003c8 ||
         (full_addr >= 0x188004a0 && full_addr <= 0x188004ab) ||
         (full_addr >= 0x18800500 && full_addr <= 0x1880050f))) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: rgb-sysio-write addr=0x%08" HWADDR_PRIx
                      " size=%u value=0x%08" PRIx64 "\n",
                      full_addr, size, value);
    }

    for (i = 0; i < ARRAY_SIZE(sf2000_regs); i++) {
        if (sf2000_regs[i].valid && sf2000_regs[i].addr == (full_addr & ~3u)) {
            old_value = sf2000_regs[i].value;
            break;
        }
        if (!sf2000_regs[i].valid && empty == ARRAY_SIZE(sf2000_regs)) {
            empty = i;
        }
    }
    if (i == ARRAY_SIZE(sf2000_regs) && empty != ARRAY_SIZE(sf2000_regs)) {
        i = empty;
        sf2000_regs[i].addr = full_addr & ~3u;
        sf2000_regs[i].valid = true;
    }
    if (i != ARRAY_SIZE(sf2000_regs)) {
        mask <<= shift;
        sf2000_regs[i].value = (old_value & ~mask) |
                               (((uint32_t)value << shift) & mask);
        /*
         * HQ_FIRST is a hardware consumer pointer.  HC15xx only permits it
         * to move backwards after the producer publishes the vendor ring
         * wrap contract in the command-buffer context immediately preceding
         * phy_addr_min.  A Linux driver once rewound this register without
         * point_wrap_addr; the permissive model accepted it while physical
         * hardware permanently stalled.
         */
        if ((full_addr & ~3u) == SF2000_GE_HQ_FIRST && size == 4 &&
            sf2000_regs[i].value < old_value) {
            enum { GE_CMDQ_CONTEXT_BYTES = (8 + 256) * sizeof(uint32_t) };
            uint32_t context[7];
            uint32_t new_first = sf2000_regs[i].value;
            bool valid = new_first >= GE_CMDQ_CONTEXT_BYTES &&
                sf2000_ge_read_node(new_first - GE_CMDQ_CONTEXT_BYTES,
                                    context, ARRAY_SIZE(context)) &&
                context[0] == new_first &&
                context[1] > context[0] &&
                context[5] == old_value &&
                (context[6] & ~1u) >= context[0] &&
                (context[6] & ~1u) <= context[1];

            if (!valid) {
                sf2000_regs[i].value = old_value;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sf2000: GE invalid unmarked ring rewind "
                              "first=0x%08x requested=0x%08x\n",
                              old_value, new_first);
            }
        }
        if ((full_addr & ~3u) == 0x18800080 &&
            (old_value & BIT(5)) && !(sf2000_regs[i].value & BIT(5))) {
            sf2000_audio_apll_reset_seen = true;
        }
    }
    if (size == 4 && ((full_addr >= 0x18808000 &&
                       full_addr <= 0x188081ec) ||
                      full_addr == 0x18800078 ||
                      full_addr == 0x188003c8)) {
        sf2000_vou_track_latch(full_addr, value);
    }
    if (i != ARRAY_SIZE(sf2000_regs)) {
        if ((full_addr & ~3u) == SF2000_IRQ_ENABLE1) {
            sf2000_irq_enable1 = sf2000_regs[i].value;
        } else if ((full_addr & ~3u) == SF2000_IRQ_ENABLE2) {
            sf2000_irq_enable2 = sf2000_regs[i].value;
        }
    }

    if (sf2000_trace_sdio() &&
        full_addr >= 0x1884c000 && full_addr < 0x1884c060) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: sdio-access write addr=0x%08lx value=0x%08lx size=%u\n",
                      (unsigned long)full_addr, (unsigned long)value, size);
    }

    if (sf2000_timer_decode(full_addr, &timer_index, &timer_offset)) {
        switch (timer_offset) {
        case 0:
            if (sf2000_timer_is_sec_ms(timer_index)) {
                sf2000_timer_cnt[timer_index] = value;
            } else {
                sf2000_timer_cnt[timer_index] = value - sf2000_timer_ticks();
            }
            sf2000_timer_deadline_ns[timer_index] = 0;
            break;
        case 4:
            sf2000_timer_aim[timer_index] = value;
            if (timer_index == SF2000_TIMER5_INDEX) {
                uint32_t count = sf2000_timer_ticks() +
                                 sf2000_timer_cnt[timer_index];
                uint32_t delta = value - count;
                int64_t delay_ns;

                if (delta >= 0x80000000u) {
                    delta = 0;
                }
                delay_ns = (int64_t)delta * 1000;
                sf2000_timer_deadline_ns[timer_index] =
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                    MAX(delay_ns, (int64_t)SF2000_TIMER5_MIN_DEADLINE_NS);
            }
            break;
        case 8:
            sf2000_timer_ctrl[timer_index] = value & ~SF2000_TIMER_CTRL_INT;
            if (!(value & SF2000_TIMER_CTRL_EN)) {
                sf2000_timer_deadline_ns[timer_index] = 0;
            }
            sf2000_sb_timer_irq_masked = false;
            if (value & SF2000_TIMER_CTRL_INT) {
                if (timer_index == 0 || timer_index == 1) {
                    sf2000_sdio_callback_pending = false;
                }
                sf2000_timer_ack();
            }
            break;
        default:
            break;
        }
    } else if (sf2000_adc_decode(full_addr, &adc_index, &adc_offset)) {
        uint32_t old = sf2000_adc_ctrl[adc_index];

        if (size >= 4) {
            sf2000_adc_ctrl[adc_index] = value;
        } else {
            uint32_t adc_mask = (1u << (size * 8)) - 1u;
            unsigned adc_shift = adc_offset * 8u;

            sf2000_adc_ctrl[adc_index] =
                (old & ~(adc_mask << adc_shift)) |
                (((uint32_t)value & adc_mask) << adc_shift);
        }
        if (adc_index == 0 && (value & BIT(8))) {
            sf2000_adc_ctrl[0] &= ~BIT(8);
        }
    } else if (full_addr >= SF2000_SND_DAC_BASE &&
               full_addr < SF2000_SND_DAC_BASE + SF2000_SND_DAC_SIZE) {
        sf2000_audio_powered = value != 0;
        sf2000_audio_dac_value = value;
        sf2000_audio_dac_written = true;
        if (sf2000_audio_voice) {
            if (sf2000_audio_powered) {
                sf2000_audio_wave_phase = 0;
            }
            sf2000_audio_set_backend_active();
        }
        if (!sf2000_audio_setup_logged) {
            sf2000_audio_setup_logged = true;
            info_report("sf2000: audio setup route=%s addr=0x%08" HWADDR_PRIx
                        " value=0x%08" PRIx64,
                        sf2000_board_profile_spec()->audio_route,
                        full_addr, value);
        }
    } else if (full_addr == SF2000_AUDIO_I2S_BASE + 0x30) {
        sf2000_audio_dma_base = value & 0x1fffffffu;
    } else if (full_addr == SF2000_AUDIO_I2S_BASE + 0x34) {
        sf2000_audio_dma_bytes = (value & 0xffffu) << 4;
        sf2000_audio_dma_producer = 0;
        sf2000_audio_dma_consumer = 0;
        sf2000_audio_dma_queued = 0;
    } else if (full_addr == SF2000_AUDIO_I2S_BASE + 0x38) {
        uint32_t old_producer = sf2000_audio_dma_producer;
        uint32_t new_producer = (value & 0xffffu) << 4;

        if (sf2000_audio_dma_bytes) {
            uint32_t produced;

            new_producer %= sf2000_audio_dma_bytes;
            produced = (new_producer + sf2000_audio_dma_bytes -
                        old_producer) % sf2000_audio_dma_bytes;
            sf2000_audio_dma_queued =
                MIN(sf2000_audio_dma_bytes,
                    sf2000_audio_dma_queued + produced);
        }
        sf2000_audio_dma_producer = new_producer;
    } else if (full_addr == SF2000_AUDIO_I2S_BASE + 0x04) {
        sf2000_audio_snd_ctl04 = value;
    } else if (full_addr == SF2000_AUDIO_I2S_BASE + 0x08) {
        uint32_t status_mask = BIT(16) | BIT(24) | BIT(27) | BIT(28);
        uint32_t status = sf2000_audio_snd_ctl08 & status_mask;

        status &= ~(value & status_mask);
        sf2000_audio_snd_ctl08 = (value & ~status_mask) | status;
        sf2000_mmio_set32(full_addr, sf2000_audio_snd_ctl08);
    } else if (full_addr == SF2000_AUDIO_I2S_BASE + 0x0c) {
        uint32_t status_mask = BIT(8) | BIT(24);
        uint32_t status = sf2000_audio_snd_ctl0c & status_mask;

        status &= ~(value & status_mask);
        sf2000_audio_snd_ctl0c = (value & ~status_mask) | status;
        sf2000_mmio_set32(full_addr, sf2000_audio_snd_ctl0c);
    } else if (full_addr == SF2000_AUDIO_I2S_BASE + 0x50) {
        sf2000_audio_snd_ctl50 = value;
    } else if ((full_addr & ~3u) == SF2000_AUDIO_I2S_BASE + 0xb0) {
        sf2000_audio_stc_control_write(sf2000_regs[i].value);
    } else if ((full_addr & ~3u) == SF2000_AUDIO_I2S_BASE + 0xb4) {
        sf2000_audio_stc_tick_write(0, sf2000_regs[i].value);
    } else if ((full_addr & ~3u) == SF2000_AUDIO_I2S_BASE + 0xbc) {
        sf2000_audio_stc_tick_write(1, sf2000_regs[i].value);
    } else if (full_addr == SF2000_AUDIO_I2S_CTRL3C) {
        sf2000_audio_i2s_ctrl3c = value;
    } else if (full_addr == SF2000_AUDIO_I2S_FADE90) {
        sf2000_audio_i2s_fade90 = value;
        sf2000_audio_set_backend_active();
    } else if (sf2000_wdt_decode(full_addr)) {
        unsigned wdt_offset = full_addr & 0xff;

        if (wdt_offset == 0x00) {
            sf2000_wdt_count = value;
            if (sf2000_wdt_conf) {
                sf2000_wdt_arm();
            }
        } else if (wdt_offset == 0x04) {
            sf2000_wdt_conf = value & 0xff;
            if (!sf2000_wdt_conf) {
                sf2000_wdt_disable();
            } else {
                if (sf2000_trace_pc_enabled() && current_cpu) {
                    MIPSCPU *cpu = MIPS_CPU(current_cpu);

                    info_report("sf2000: watchdog armed pc=0x%08x ra=0x%08x count=0x%08x conf=0x%02x timeout_ns=%" PRId64,
                                (uint32_t)cpu->env.active_tc.PC,
                                (uint32_t)cpu->env.active_tc.gpr[31],
                                sf2000_wdt_count, sf2000_wdt_conf,
                                sf2000_wdt_timeout_ns());
                }
                qemu_log_mask(LOG_UNIMP,
                              "sf2000: watchdog armed count=0x%08x conf=0x%02x timeout_ns=%" PRId64 "\n",
                              sf2000_wdt_count, sf2000_wdt_conf,
                              sf2000_wdt_timeout_ns());
                sf2000_wdt_arm();
            }
        }
    } else if (full_addr == SF2000_IRQ_STATUS1 && (value & SF2000_SDIO_IRQ)) {
        sf2000_sdio_irq_pending = false;
    } else if (sf2000_uart_decode(full_addr, &uart_index, &uart_offset) &&
               uart_offset == SF2000_UART_IER) {
        sf2000_uart_ier[uart_index] = value & 0xff;
        if (sf2000_uart_ier[uart_index] & SF2000_UART_IER_THRI) {
            sf2000_uart_thri_pending[uart_index] = true;
        }
    } else if (full_addr == SF2000_GPIO_L_ISR && (value & SF2000_GPIO_L08)) {
        uint32_t isr = 0;

        sf2000_mmio_get32(SF2000_GPIO_L_ISR, &isr);
        sf2000_mmio_set32(SF2000_GPIO_L_ISR, isr & ~((uint32_t)value));
    } else if (full_addr == SF2000_GPIO_R_ISR) {
        uint32_t isr = 0;

        /*
         * GPIO ISR registers are write-one-to-clear. The stock power-detect
         * code uses R30 for power-good and R31 for the power-alert latch; if
         * either acknowledgement is stored as a normal register write, the
         * firmware re-enters the low-power alert path and never reaches the
         * launcher.
         */
        sf2000_mmio_get32(SF2000_GPIO_R_ISR, &isr);
        sf2000_mmio_set32(SF2000_GPIO_R_ISR, isr & ~((uint32_t)value));
    } else if (full_addr == SF2000_GPIO_R_OUT ||
               full_addr == SF2000_GPIO_R_DIR ||
               full_addr == SF2000_PINMUX_R07) {
        sf2000_audio_set_backend_active();
    } else if (full_addr == SF2000_GPIO_L_OUT) {
        sf2000_gpio_l_write(sf2000_regs[i].value);
        sf2000_panel_gpio_write(full_addr, sf2000_regs[i].value);
        sf2000_audio_set_backend_active();
    } else if (full_addr == SF2000_GPIO_L_DIR ||
               full_addr == SF2000_PINMUX_L15) {
        sf2000_audio_set_backend_active();
    } else if (full_addr == 0x18800354) {
        sf2000_panel_gpio_write(full_addr, sf2000_regs[i].value);
    } else if (sf2000_ge_decode(full_addr)) {
        if ((full_addr & ~3u) == SF2000_GE_STATUS) {
            uint32_t status = old_value & ~((uint32_t)value);

            /*
             * GE status is write-one-to-clear.  In particular, acknowledging
             * DONE must not manufacture or clear the independently driven
             * BUSY bit.
             */
            if (old_value & SF2000_GE_STATUS_BUSY) {
                status |= SF2000_GE_STATUS_BUSY;
            }
            sf2000_mmio_set32(SF2000_GE_STATUS, status);
            sf2000_ge_irq_pending =
                (status & SF2000_GE_STATUS_DONE) != 0;
            if ((old_value & SF2000_GE_STATUS_DONE) &&
                (value & SF2000_GE_STATUS_DONE)) {
                sf2000_ge_irq_ack_sequence++;
                if (sf2000_scanout_oracle() &&
                    (sf2000_ge_irq_ack_sequence <= 8 ||
                     !(sf2000_ge_irq_ack_sequence % 300))) {
                    qemu_log_mask(LOG_UNIMP,
                                  "sf2000: ge-queue irq-ack seq=%" PRIu64
                                  " submitted=%" PRIu64 "\n",
                                  sf2000_ge_irq_ack_sequence,
                                  sf2000_ge_submit_sequence);
                }
            }
            sf2000_update_irq();
        } else if ((full_addr & ~3u) == SF2000_GE_CTRL) {
            sf2000_mmio_set32(SF2000_GE_CTRL, sf2000_regs[i].value &
                              ~BIT(31));
        } else if ((full_addr & ~3u) == SF2000_GE_START &&
                   (sf2000_regs[i].value & BIT(1))) {
            sf2000_ge_start_queue();
        }
    } else if ((full_addr & ~0x80u) == 0x18808350) {
        unsigned layer = (full_addr & 0x80u) ? 1 : 0;
        uint32_t masks[2] = { 0, 0 };
        unsigned bank;

        if (value & BIT(0)) {
            sf2000_gma_mask_armed[layer] = true;
        } else if (sf2000_lcd && sf2000_lcd->vou_latch_stage >= 10) {
            sf2000_mmio_get32(0x18808350, &masks[0]);
            sf2000_mmio_get32(0x188083d0, &masks[1]);
            /*
             * HC15 stock firmware arms both update-mask banks around every
             * primary transaction.  The second release commits the complete
             * compositor state; treating 0x350 alone as HC16-style layer
             * state lets an incomplete guest look healthy in emulation.
             */
            if (!(masks[0] & BIT(0)) && !(masks[1] & BIT(0)) &&
                sf2000_gma_mask_armed[0] && sf2000_gma_mask_armed[1]) {
                for (bank = 0; bank < 2; bank++) {
                    uint32_t ctl = 0;
                    uint32_t dmba = 0;

                    sf2000_mmio_get32(0x18808300 + bank * 0x80u, &ctl);
                    sf2000_mmio_get32(0x18808304 + bank * 0x80u, &dmba);
                    sf2000_mmio_set32(0x18808b00 + bank * 0x80u, ctl);
                    sf2000_mmio_set32(0x18808b04 + bank * 0x80u, dmba);
                }
                sf2000_gma_mask_armed[0] = false;
                sf2000_gma_mask_armed[1] = false;
            }
        }
    } else if ((full_addr & ~0x80u) == 0x18808304) {
        unsigned layer = (full_addr & 0x80u) ? 1 : 0;

        sf2000_active_gma[layer] = value;
        if (sf2000_trace_gma()) {
            qemu_log_mask(LOG_UNIMP,
                          "sf2000: gma-doorbell layer=%u dmba=0x%08" PRIx64 "\n",
                          (full_addr & 0x80u) ? 1 : 0, value);
        }
        sf2000_gma_present(value);
    } else if (sf2000_uart_decode(full_addr, &uart_index, &uart_offset) &&
               uart_offset == SF2000_UART_RBR_THR) {
        sf2000_uart_put(uart_index, value & 0xff);
        if (sf2000_uart_ier[uart_index] & SF2000_UART_IER_THRI) {
            sf2000_uart_thri_pending[uart_index] = true;
        }
    } else if (sf2000_i2c_decode(full_addr, &i2c_index, &i2c_offset)) {
        sf2000_i2c_write(full_addr, value);
    } else if (sf2000_usb_decode(full_addr, &usb_index, &usb_offset)) {
        sf2000_usb_write(full_addr, value, size);
    } else if (sf2000_pwm_decode(full_addr, &pwm_channel, &pwm_offset)) {
        sf2000_pwm_write(full_addr);
    } else if (sf2000_irc_decode(full_addr, &irc_offset)) {
        sf2000_irc_write(full_addr, value);
    } else if (full_addr == 0x1882e098) {
        sf2000_sflash_cmd = value & 0xff;
        if (sf2000_trace_sflash()) {
            qemu_log_mask(LOG_UNIMP, "sf2000: sflash-cmd cmd=0x%02x\n",
                          sf2000_sflash_cmd);
        }
    } else if (full_addr == 0x1884c004) {
        sf2000_sdio_arg = value;
    } else if (full_addr == 0x1884c002) {
        sf2000_sdio_cmd = value & 0x3f;
        if (sf2000_sdio_cmd == 24 || sf2000_sdio_cmd == 25) {
            sf2000_sdio_write_active = true;
        }
    } else if (full_addr == 0x1884c020) {
        sf2000_sdio_dma_addr = value;
    } else if (full_addr == 0x1884c024) {
        sf2000_sdio_dma_wr_addr = value;
    } else if (full_addr == 0x1884c028) {
        sf2000_sdio_dma_len = value;
    } else if (full_addr == 0x1884c02c) {
        sf2000_sdio_dma_wr_len = value;
    } else if (full_addr == 0x1884c00e) {
        sf2000_sdio_pio_state = value & 0xff;
    } else if (full_addr == 0x1884c00b && (value & 0x04)) {
        sf2000_sdio_xfer_done = false;
        sf2000_sdio_xfer_busy = false;
        sf2000_sdio_write_active = false;
        sf2000_sdio_irq_pending = false;
        sf2000_sdio_pio_state &= ~0x04;
        sf2000_sdio_pio_state |= 0x01;
        sf2000_sdio_pio_state &= ~0x01;
    } else if (full_addr == 0x1884c030 && (value & 0x40)) {
        sf2000_sdio_xfer_done = false;
        sf2000_sdio_xfer_busy = false;
        sf2000_sdio_write_active = false;
        sf2000_sdio_irq_pending = false;
        sf2000_sdio_pio_state &= ~0x04;
        sf2000_sdio_pio_state |= 0x01;
        sf2000_sdio_pio_state &= ~0x01;
    } else if (full_addr == 0x1884c030 && (value & 1)) {
        sf2000_sdio_xfer_done = false;
        sf2000_sdio_xfer_busy = true;
        sf2000_sdio_write_active = true;
        sf2000_sdio_pio_state = 0x04;
    } else if (full_addr == 0x1884c030 && (value & 0x20)) {
        sf2000_sdio_xfer_done = false;
        sf2000_sdio_xfer_busy = false;
        sf2000_sdio_write_active = false;
        sf2000_sdio_irq_pending = false;
        sf2000_sdio_pio_state = 0x04;
    } else if (full_addr == 0x1884c000 && (value & 1)) {
        sf2000_sdio_complete_cmd();
    }

    if ((full_addr & ~3u) == SF2000_AUDIO_I2S_FADE90) {
        sf2000_mmio_get32(SF2000_AUDIO_I2S_FADE90,
                          &sf2000_audio_i2s_fade90);
        sf2000_audio_set_backend_active();
    }

    if (!sf2000_audio_dma_reported && sf2000_audio_dma_bytes &&
        (sf2000_audio_snd_ctl04 & BIT(16)) &&
        (sf2000_audio_snd_ctl08 & 1) && (sf2000_audio_snd_ctl0c & 1) &&
        (sf2000_audio_snd_ctl50 & BIT(29)) && sf2000_audio_audible() &&
        sf2000_audio_dma_configured()) {
        sf2000_audio_dma_reported = true;
        info_report("sf2000: audio guest DMA active base=0x%08x bytes=%u producer=%u consumer=%u",
                    sf2000_audio_dma_base, sf2000_audio_dma_bytes,
                    sf2000_audio_dma_producer, sf2000_audio_dma_consumer);
    }
    sf2000_update_irq();
    sf2000_log_mmio("mmio-write", full_addr, value, size);
}

static const MemoryRegionOps sf2000_unimp_ops = {
    .read = sf2000_unimp_read,
    .write = sf2000_unimp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static bool sf2000_sflash_security_byte(hwaddr addr, uint8_t *byte)
{
    CPUState *cs = first_cpu;
    MIPSCPU *cpu;
    uint32_t pc;
    uint32_t gp;
    uint32_t challenge_ptr;
    hwaddr source;
    MemTxResult res;
    uint8_t check[256];

    if (!cs) {
        return false;
    }
    cpu = MIPS_CPU(cs);
    pc = (uint32_t)cpu->env.active_tc.PC;
    if (pc < 0x80355490 || pc >= 0x803555b4) {
        return false;
    }

    /*
     * Stock security_check reads a tiny per-device/factory blob through
     * xmc_security_read(). Public update bootloader images do not carry that
     * personalized area, but the stock launcher still expects the comparison
     * to succeed before creating the menu task. Model the minimum factory blob
     * contract instead of patching the firmware: key bytes at 0x2000 are
     * neutral, additive bytes at 0x10c0 are neutral, and check bytes at 0x1000
     * are derived from the runtime challenge string whose pointer is stored at
     * gp-30388.
     */
    if (addr >= 0x2000 && addr < 0x2008) {
        *byte = 0;
        return true;
    }
    if (addr >= 0x10c0 && addr < 0x11c0) {
        *byte = 0;
        return true;
    }
    if (addr < 0x1000 || addr >= 0x1100) {
        return false;
    }

    gp = (uint32_t)cpu->env.active_tc.gpr[28];
    source = sf2000_guest_phys_addr(gp - 30388u);
    challenge_ptr = address_space_ldl_le(&address_space_memory, source,
                                         MEMTXATTRS_UNSPECIFIED, &res);
    if (res != MEMTX_OK) {
        return false;
    }
    for (uint32_t i = 0; i <= (uint32_t)(addr - 0x1000); i++) {
        uint8_t challenge;

        source = sf2000_guest_phys_addr(challenge_ptr + i);
        challenge = address_space_ldub(&address_space_memory, source,
                                       MEMTXATTRS_UNSPECIFIED, &res);
        if (res != MEMTX_OK) {
            return false;
        }
        /*
         * With neutral additive bytes, the stock check reduces to:
         *   check[i] == challenge[i] ^ key[i]         for i < 8
         *   check[i] == challenge[i] ^ check[i - 8]   for i >= 8
         */
        if (i < 8) {
            uint8_t key;
            hwaddr key_addr = sf2000_guest_phys_addr(gp - 24424u + i);

            key = address_space_ldub(&address_space_memory, key_addr,
                                     MEMTXATTRS_UNSPECIFIED, &res);
            if (res != MEMTX_OK) {
                return false;
            }
            check[i] = challenge ^ key;
        } else {
            check[i] = challenge ^ check[i - 8];
        }
    }
    *byte = check[addr - 0x1000];
    return true;
}

static uint64_t sf2000_sflash_direct_read(void *opaque, hwaddr addr,
                                          unsigned size)
{
    static const uint8_t jedec_id[] = { 0x20, 0x40, 0x13 };
    uint64_t value = 0;
    unsigned i;
    CPUState *cs = first_cpu;

    for (i = 0; i < size; i++) {
        uint8_t byte;

        switch (sf2000_sflash_cmd) {
        case 0x05: /* Read status register. */
            byte = 0;
            break;
        case 0x9f: /* Read JEDEC ID. */
            byte = jedec_id[(addr + i) % ARRAY_SIZE(jedec_id)];
            break;
        case 0xab:
            /*
             * Release from deep power-down / read electronic signature. SPI
             * NOR parts return the device ID after three dummy bytes; the
             * stock bootloader reads a 32-bit little-endian word from offset 0.
             */
            byte = ((addr + i) & 3u) == 3u ? 0x13 : 0x00;
            break;
        default:
            if (!sf2000_sflash_security_byte(addr + i, &byte)) {
                byte = sf2000_bootrom_bytes[(addr + i) % SF2000_BOOT_SIZE];
            }
            break;
        }
        value |= (uint64_t)byte << (i * 8);
    }

    if (g_getenv("SF2000_TRACE_PC") && cs) {
        MIPSCPU *cpu = MIPS_CPU(cs);
        uint32_t pc = (uint32_t)cpu->env.active_tc.PC;

        if ((pc >= 0x80355088 && pc < 0x80355900) ||
            (pc >= 0x80346324 && pc < 0x80346428)) {
            fprintf(stderr,
                    "sf2000: sflash-direct pc=0x%08x ra=0x%08x gp=0x%08x cmd=0x%02x off=0x%08" HWADDR_PRIx " size=%u value=0x%08" PRIx64 "\n",
                    pc, (uint32_t)cpu->env.active_tc.gpr[31],
                    (uint32_t)cpu->env.active_tc.gpr[28], sf2000_sflash_cmd,
                    addr, size, value);
        }
    }

    if (sf2000_trace_sflash()) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: sflash-read cmd=0x%02x off=0x%08" HWADDR_PRIx
                      " size=%u value=0x%08" PRIx64 "\n",
                      sf2000_sflash_cmd, addr, size, value);
    }
    return value;
}

static void sf2000_sflash_direct_write(void *opaque, hwaddr addr,
                                       uint64_t value, unsigned size)
{
    if (sf2000_trace_sflash()) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: sflash-write off=0x%08" HWADDR_PRIx
                      " size=%u value=0x%08" PRIx64 "\n",
                      addr, size, value);
    }
}

static const MemoryRegionOps sf2000_sflash_direct_ops = {
    .read = sf2000_sflash_direct_read,
    .write = sf2000_sflash_direct_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static uint32_t sf2000_rgb565_to_surface(uint16_t pix)
{
    if (G_UNLIKELY(!sf2000_rgb565_lut_ready)) {
        unsigned i;

        for (i = 0; i < ARRAY_SIZE(sf2000_rgb565_lut); i++) {
            uint32_t r = (i >> 11) & 0x1f;
            uint32_t g = (i >> 5) & 0x3f;
            uint32_t b = i & 0x1f;

            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);
            sf2000_rgb565_lut[i] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
        sf2000_rgb565_lut_ready = true;
    }

    return sf2000_rgb565_lut[pix];
}

static uint32_t sf2000_argb8888_to_surface(uint32_t pix)
{
    return 0xff000000u | (pix & 0x00ffffffu);
}

static void sf2000_vou_present_unlatched_background(SF2000LCDState *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    unsigned width = sf2000_panel_surface_width(s);
    unsigned height = sf2000_panel_surface_height(s);
    int y;

    if (surface_bits_per_pixel(surface) != 32 ||
        surface_width(surface) != width || surface_height(surface) != height) {
        qemu_console_resize(s->con, width, height);
        surface = qemu_console_surface(s->con);
    }
    if (surface_bits_per_pixel(surface) != 32) {
        return;
    }
    for (y = 0; y < (int)height; y++) {
        uint32_t *dst = (uint32_t *)(surface_data(surface) +
                        y * surface_stride(surface));
        int x;

        for (x = 0; x < (int)width; x++) {
            /* Visually distinguish the physical VPO fallback from scanout. */
            dst[x] = 0xff800080u;
        }
    }
    dpy_gfx_update(s->con, 0, 0, width, height);
    if (s->vou_latch_stage < 10 && !s->vou_unlatched_logged) {
        s->vou_unlatched_logged = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sf2000: GMA doorbell before VOU RGB latch stage=%u; displaying VPO background\n",
                      s->vou_latch_stage);
    }
}

static uint32_t sf2000_gma_present_block(SF2000LCDState *s, uint32_t dmba_addr,
                                         bool dump_frame)
{
    DisplaySurface *surface;
    uint8_t header[40];
    uint8_t scaler_table[0x200];
    uint8_t *linebuf;
    uint8_t *linebuf_alloc = NULL;
    bool have_palette = false;
    uint32_t d0, d1, d2, d3, d4, d5, d6, d7, d8, d9;
    uint32_t mode, clut_update, sx, ex, sy, ey, src_w, src_h, pitch;
    uint32_t sample_w, sample_h;
    uint32_t width, height, bpp;
    bool scale_x, scale_y;
    bool scaler_coefficients_valid = true;
    bool backlight_active;
    unsigned phase = 32;
    int y;
    uint32_t rgb_pinmux = 0;
    uint32_t rgb_clock_pinmux = 0;
    uint32_t rgb_clock_gate0 = 0;
    uint32_t rgb_clock_gate = 0;
    uint32_t vou_ctrl = 0;
    uint32_t vou_mode = 0;
    unsigned surface_width_value;
    unsigned surface_height_value;

    if (!s || !dmba_addr) {
        return 0;
    }
    surface_width_value = sf2000_panel_surface_width(s);
    surface_height_value = sf2000_panel_surface_height(s);
    if (s->vou_latch_stage < 10 && s->vou_setup_seen) {
        sf2000_vou_present_unlatched_background(s);
        return 0;
    }
    if (sf2000_mmio_get32(0x188004a8, &rgb_pinmux) &&
        ((rgb_pinmux >> 8) & 0xff) != 6 &&
        ((rgb_pinmux >> 16) & 0xff) != 6) {
        sf2000_vou_present_unlatched_background(s);
        if (!s->panel_vsync_unconnected_logged) {
            s->panel_vsync_unconnected_logged = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sf2000: GMA scanout with panel sync/DE disconnected pinmux=0x%08x\n",
                          rgb_pinmux);
        }
        return 0;
    }
    if (s->panel_ramctrl_explicit && s->panel_rgb_handoff_synchronized &&
        sf2000_mmio_get32(0x188004a4, &rgb_clock_pinmux) &&
        ((rgb_clock_pinmux >> 24) & 0xf) != 6) {
        sf2000_vou_present_unlatched_background(s);
        if (!s->panel_clock_unconnected_logged) {
            s->panel_clock_unconnected_logged = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sf2000: GMA scanout with panel pixel clock disconnected pinmux=0x%08x\n",
                          rgb_clock_pinmux);
        }
        return 0;
    }
    if (s->panel_ramctrl_explicit && s->panel_rgb_handoff_synchronized &&
        sf2000_mmio_get32(0x18800064, &rgb_clock_gate) &&
        sf2000_mmio_get32(0x18800060, &rgb_clock_gate0) &&
        (rgb_clock_gate & 0x00000600) != 0x00000600 &&
        !((rgb_clock_gate0 & 0x00000f88) == 0x00000f88 &&
          (rgb_clock_gate & 0x0bc04040) == 0x0bc04040)) {
        sf2000_vou_present_unlatched_background(s);
        if (!s->panel_clock_gated_logged) {
            s->panel_clock_gated_logged = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sf2000: HC15 RGB scanout clocks gated gate1=0x%08x\n",
                          rgb_clock_gate);
        }
        return 0;
    }
    /*
     * The HC15 panel helper connects the completed VOU raster to PRGB with
     * two writes immediately before changing the shared pad mux:
     *
     *   VOU_HD_CTRL &= ~0x100;
     *   VOU_HD_MODE = (VOU_HD_MODE & ~0xff) | 0x15;
     *
     * Readable GMA shadows and a running frame boundary are not sufficient
     * without this last output-port connection.  Model it so QEMU no longer
     * hides the Linux bank-address bug which produced a live scrambled raster
     * on physical SF2000 hardware.
     */
    if (s->vou_setup_seen && s->panel_rgb_handoff_synchronized &&
        sf2000_mmio_get32(0x188004a4, &rgb_clock_pinmux) &&
        ((rgb_clock_pinmux >> 24) & 0xf) == 6 &&
        sf2000_mmio_get32(0x18808084, &vou_ctrl) &&
        sf2000_mmio_get32(0x18808000, &vou_mode) &&
        ((vou_ctrl & 0x00000100) || (vou_mode & 0xff) != 0x15)) {
        sf2000_vou_present_unlatched_background(s);
        if (!s->panel_vou_disconnected_logged) {
            s->panel_vou_disconnected_logged = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sf2000: VOU raster disconnected from PRGB ctrl=0x%08x mode=0x%08x\n",
                          vou_ctrl, vou_mode);
        }
        return 0;
    }
    if (s->vou_setup_seen && !s->panel_rgb_handoff_synchronized) {
        sf2000_vou_present_unlatched_background(s);
        return 0;
    }
    if (address_space_read(&address_space_memory, dmba_addr,
                           MEMTXATTRS_UNSPECIFIED, header,
                           sizeof(header)) != MEMTX_OK) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: gma-present header-read-failed dmba=0x%08x\n",
                      dmba_addr);
        return 0;
    }

    d0 = ldl_le_p(header + 0);
    d1 = ldl_le_p(header + 4);
    d2 = ldl_le_p(header + 8);
    d3 = ldl_le_p(header + 12);
    d4 = ldl_le_p(header + 16);
    d5 = ldl_le_p(header + 20);
    d6 = ldl_le_p(header + 24);
    d7 = ldl_le_p(header + 28);
    d8 = ldl_le_p(header + 32);
    d9 = ldl_le_p(header + 36);

    /*
     * GMA/GE display updates are driven by a descriptor memory base address
     * written to the 0x18808304/0x18808384 doorbells. The first 40 bytes are a
     * compact descriptor: d0 contains the pixel mode and CLUT-update flag, d1
     * points at a 256-entry ARGB palette for CLUT8, d2/d3 describe the output
     * rectangle, d4/d5 describe source geometry/pitch, and d7 is bitmap data.
     */
    mode = (d0 >> 4) & 0xf;
    clut_update = (d0 >> 10) & 1;
    sx = d2 & 0xfff;
    ex = (d2 >> 16) & 0x1fff;
    sy = d3 & 0xfff;
    ey = (d3 >> 16) & 0xfff;
    src_w = d4 & 0xfff;
    src_h = (d4 >> 16) & 0xfff;
    pitch = (d5 >> 16) & 0x3fff;

    if (ex < sx || ey < sy || sx >= surface_width_value ||
        sy >= surface_height_value || !d7) {
        return d6;
    }

    width = MIN(ex - sx + 1, surface_width_value - sx);
    height = MIN(ey - sy + 1, surface_height_value - sy);
    if (src_w && width > src_w) {
        width = src_w;
    }
    if (src_h && height > src_h) {
        height = src_h;
    }

    switch (mode) {
    case 0x01: /* ARGB8888 */
        bpp = 4;
        break;
    case 0x06: /* RGB565 */
        bpp = 2;
        break;
    case 0x0c: /* CLUT8 */
        bpp = 1;
        if (d1 && (clut_update || !s->gma_palette_valid ||
                   s->gma_palette_addr != d1)) {
            if (address_space_read(&address_space_memory, d1,
                                   MEMTXATTRS_UNSPECIFIED, s->gma_palette,
                                   sizeof(s->gma_palette)) == MEMTX_OK) {
                s->gma_palette_addr = d1;
                s->gma_palette_valid = true;
            }
        }
        have_palette = s->gma_palette_valid && s->gma_palette_addr == d1;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: gma-present unsupported mode=%u dmba=0x%08x bitmap=0x%08x\n",
                      mode, dmba_addr, d7);
        return d6;
    }

    if (!pitch) {
        pitch = width * bpp;
    }
    sample_w = src_w ? src_w : width;
    sample_h = src_h ? src_h : height;
    if (pitch < sample_w * bpp) {
        return d6;
    }

    scale_x = sample_w != width;
    scale_y = sample_h != height;

    /*
     * HC15 scaled GMA blocks carry 32 two-word filter phases at descriptor
     * offsets 0x40..0x23f.  Treating source/output geometry alone as enough
     * made QEMU render Linux descriptors which physical hardware reduced to
     * the VPO background because their enabled scaler table was all zero.
     */
    if ((d0 & (1u << 2)) && (scale_x || scale_y)) {
        if (address_space_read(&address_space_memory, dmba_addr + 0x40,
                               MEMTXATTRS_UNSPECIFIED, scaler_table,
                               sizeof(scaler_table)) != MEMTX_OK) {
            scaler_coefficients_valid = false;
        } else {
            for (phase = 0; phase < 32; phase++) {
                if (!ldl_le_p(scaler_table + phase * 16) &&
                    !ldl_le_p(scaler_table + phase * 16 + 4)) {
                    scaler_coefficients_valid = false;
                    break;
                }
            }
        }
        if (!scaler_coefficients_valid) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sf2000: gma-present missing scaler coefficients dmba=0x%08x phase=%u\n",
                          dmba_addr, phase);
            return d6;
        }
    }

    surface = qemu_console_surface(s->con);
    if (surface_bits_per_pixel(surface) != 32 ||
        surface_width(surface) != surface_width_value ||
        surface_height(surface) != surface_height_value) {
        qemu_console_resize(s->con, surface_width_value,
                            surface_height_value);
        surface = qemu_console_surface(s->con);
    }
    if (surface_bits_per_pixel(surface) != 32) {
        return d6;
    }

    if (pitch <= sizeof(s->gma_linebuf)) {
        linebuf = s->gma_linebuf;
    } else {
        linebuf_alloc = g_malloc(pitch);
        linebuf = linebuf_alloc;
    }
    backlight_active = sf2000_pwm2_backlight_active();
    for (y = 0; y < height; y++) {
        uint32_t src_y = scale_y ? ((uint64_t)y * sample_h) / height :
                         (uint32_t)y;
        int x;

        if (address_space_read(&address_space_memory,
                               d7 + (uint64_t)src_y * pitch,
                               MEMTXATTRS_UNSPECIFIED, linebuf,
                               pitch) != MEMTX_OK) {
            break;
        }
        for (x = 0; x < width; x++) {
            uint32_t src_x = scale_x ? ((uint64_t)x * sample_w) / width :
                             (uint32_t)x;
            uint32_t color;

            if (mode == 0x06) {
                color = sf2000_rgb565_to_surface(
                    lduw_le_p(linebuf + src_x * 2));
            } else if (mode == 0x01) {
                color = sf2000_argb8888_to_surface(
                    ldl_le_p(linebuf + src_x * 4));
            } else if (have_palette) {
                color = sf2000_argb8888_to_surface(
                    ldl_le_p(s->gma_palette + linebuf[src_x] * 4));
            } else {
                uint8_t c = linebuf[src_x];
                color = 0xff000000u | (c << 16) | (c << 8) | c;
            }
            if (!backlight_active) {
                color = 0xff000000u;
            }
            ((uint32_t *)(surface_data(surface) +
                          (sy + y) * surface_stride(surface)))[sx + x] =
                sf2000_panel_color(s, color);
        }
    }
    g_free(linebuf_alloc);

    dpy_gfx_update(s->con, sx, sy, width, height);
    /*
     * QEMU's generic screendump does not always observe this custom console
     * while running headless. The optional SF2000_GMA_DUMP_DIR hook writes PPM
     * frames exactly after a descriptor is presented, giving us deterministic
     * artifacts to compare against real-device photos or captures.
     */
    if (dump_frame) {
        sf2000_dump_ppm_from_surface(s, "gma");
    }
    if (sf2000_trace_gma()) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: gma-present dmba=0x%08x bitmap=0x%08x mode=%u rect=%u,%u %ux%u pitch=%u clut=%u d0=%08x d1=%08x d2=%08x d3=%08x d4=%08x d5=%08x d6=%08x d8=%08x d9=%08x\n",
                      dmba_addr, d7, mode, sx, sy, width, height, pitch,
                      have_palette, d0, d1, d2, d3, d4, d5, d6, d8, d9);
    }
    return d6;
}

static void sf2000_gma_present_with_dump(uint32_t dmba_addr, bool dump_frame)
{
    SF2000LCDState *s = sf2000_lcd;
    uint32_t seen[8];
    uint32_t cur = dmba_addr;
    unsigned i;

    if (!s || !cur) {
        return;
    }

    /*
     * Stock gma_m36f descriptors can chain visible blocks through d6. The
     * doorbell points at the primary block, while menu OSD updates can put the
     * region that carries the actual pixels in following blocks. Walk a small
     * bounded chain and then dump once, after the composed frame is complete.
     */
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < ARRAY_SIZE(seen) && cur; i++) {
        unsigned j;

        for (j = 0; j < i; j++) {
            if (seen[j] == cur) {
                cur = 0;
                break;
            }
        }
        if (!cur) {
            break;
        }
        seen[i] = cur;
        cur = sf2000_gma_present_block(s, cur, false);
    }
    if (dump_frame) {
        sf2000_dump_ppm_from_surface(s, "gma");
    }
}

static void sf2000_gma_present(uint32_t dmba_addr)
{
    sf2000_gma_present_with_dump(dmba_addr, true);
}

static void sf2000_lcd_update(void *opaque)
{
    SF2000LCDState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint8_t *linebuf = NULL;
    int width = s->width ? s->width : SF2000_LCD_WIDTH;
    int height = s->height ? s->height : SF2000_LCD_HEIGHT;
    int stride = s->stride ? s->stride : width * 2;
    int y;

    /*
     * GMA descriptors are continuously scanned by the HC15xx hardware.  The
     * guest can update a framebuffer with CPU stores and cache maintenance
     * without ringing the DMBA doorbell again.  Refresh active layers when
     * QEMU asks the console for a new frame, just as the physical scanout does.
     *
     * Before the VOU latch, the physical output remains on its fallback
     * background even if a descriptor has already been armed.  The latch
     * transition presents that descriptor itself, so continuous scanout starts
     * only afterwards.  This also keeps an actual premature guest doorbell
     * visible to the ordering oracle in sf2000_gma_present_block().
     */
    if (s->vou_latch_stage >= 10) {
        if (sf2000_active_gma[0]) {
            sf2000_gma_present_with_dump(sf2000_active_gma[0], false);
        }
        if (sf2000_active_gma[1] &&
            sf2000_active_gma[1] != sf2000_active_gma[0]) {
            sf2000_gma_present_with_dump(sf2000_active_gma[1], false);
        }
        if (sf2000_active_gma[0] || sf2000_active_gma[1]) {
            s->redraw = false;
            return;
        }
    }

    if (!s->fb_base || !s->control) {
        return;
    }

    if (surface_bits_per_pixel(surface) != 32 ||
        surface_width(surface) != width || surface_height(surface) != height) {
        qemu_console_resize(s->con, width, height);
        surface = qemu_console_surface(s->con);
        if (surface_bits_per_pixel(surface) != 32) {
            return;
        }
    }

    linebuf = g_malloc(stride);
    for (y = 0; y < height; y++) {
        uint32_t *dst = (uint32_t *)(surface_data(surface)
                + y * surface_stride(surface));
        MemTxResult res;
        int x;

        res = address_space_read(s->as, s->fb_base + (uint64_t)y * stride,
                                 MEMTXATTRS_UNSPECIFIED, linebuf, stride);
        if (res != MEMTX_OK) {
            break;
        }

        for (x = 0; x < width; x++) {
            uint16_t pix = lduw_le_p(linebuf + x * 2);
            dst[x] = sf2000_panel_color(s, sf2000_rgb565_to_surface(pix));
        }
    }
    g_free(linebuf);

    dpy_gfx_update(s->con, 0, 0, width, height);
    s->redraw = false;
}

static uint32_t sf2000_lcd_surface_hash(SF2000LCDState *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t hash = 2166136261u;
    int x;
    int y;

    for (y = 0; y < surface_height(surface); y++) {
        const uint32_t *row = (const uint32_t *)(surface_data(surface) +
                              y * surface_stride(surface));

        for (x = 0; x < surface_width(surface); x++) {
            hash ^= row[x];
            hash *= 16777619u;
        }
    }
    return hash;
}

static void sf2000_lcd_report_scanout(SF2000LCDState *s, uint32_t hash)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t colors[17];
    uint32_t nonblack = 0;
    unsigned distinct = 0;
    bool periodic;
    int x;
    int y;

    if (!sf2000_scanout_oracle()) {
        return;
    }
    s->scanout_sequence++;
    periodic = s->scanout_sequence <= 16 ||
               !(s->scanout_sequence % 60);
    for (y = 0; y < surface_height(surface); y++) {
        const uint32_t *row = (const uint32_t *)(surface_data(surface) +
                              y * surface_stride(surface));

        for (x = 0; x < surface_width(surface); x++) {
            unsigned i;

            if (row[x] & 0x00ffffffu) {
                nonblack++;
            }
            for (i = 0; i < distinct; i++) {
                if (colors[i] == row[x]) {
                    break;
                }
            }
            if (i == distinct && distinct < ARRAY_SIZE(colors)) {
                colors[distinct++] = row[x];
            }
            /*
             * A diverse non-periodic frame is healthy.  Stop as soon as that
             * is proven instead of scanning all 76,800 pixels at 60 Hz.
             * Solid and near-solid frames still receive a complete count.
             */
            if (!periodic && distinct > 2) {
                return;
            }
        }
    }
    /*
     * Keep the oracle inexpensive in long emulator runs while guaranteeing a
     * report within one second of a changing 60 Hz game.  A solid/near-solid
     * failure is always reported immediately.
     */
    if (periodic || distinct <= 2) {
        qemu_log_mask(LOG_UNIMP,
                      "sf2000: scanout-oracle seq=%" PRIu64
                      " hash=%08x distinct=%u nonblack=%u pixels=%u"
                      " gma0=%08x gma1=%08x\n",
                      s->scanout_sequence, hash, distinct, nonblack,
                      surface_width(surface) * surface_height(surface),
                      sf2000_active_gma[0], sf2000_active_gma[1]);
    }
}

static void sf2000_lcd_scanout_timer(void *opaque)
{
    SF2000LCDState *s = opaque;
    bool dirty = true;

    if (sf2000_ram_region) {
        DirtyBitmapSnapshot *snap;

        snap = memory_region_snapshot_and_clear_dirty(sf2000_ram_region,
                                                       0x00f00000,
                                                       0x00100000,
                                                       DIRTY_MEMORY_VGA);
        dirty = memory_region_snapshot_get_dirty(sf2000_ram_region, snap,
                                                  0x00f00000,
                                                  0x00100000);
        g_free(snap);
    }

    if (dirty && s->vou_latch_stage >= 10 &&
        (sf2000_active_gma[0] || sf2000_active_gma[1])) {
        uint32_t hash;

        sf2000_lcd_update(s);
        hash = sf2000_lcd_surface_hash(s);
        if (!s->scanout_hash_valid || hash != s->scanout_hash) {
            s->scanout_hash = hash;
            s->scanout_hash_valid = true;
            sf2000_lcd_report_scanout(s, hash);
            sf2000_dump_ppm_from_surface(s, "gma");
        }
    }
    timer_mod(s->scanout_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 30);
}

static void sf2000_lcd_invalidate(void *opaque)
{
    SF2000LCDState *s = opaque;
    s->redraw = true;
}

static const GraphicHwOps sf2000_lcd_gfx_ops = {
    .invalidate = sf2000_lcd_invalidate,
    .gfx_update = sf2000_lcd_update,
};

static uint64_t sf2000_lcd_read(void *opaque, hwaddr addr, unsigned size)
{
    SF2000LCDState *s = opaque;
    uint64_t value = 0;

    switch (addr) {
    case 0x00:
        value = s->control;
        break;
    case 0x04:
        value = s->fb_base;
        break;
    case 0x08:
        value = ((uint64_t)s->height << 16) | s->width;
        break;
    case 0x0c:
        value = s->stride;
        break;
    case 0x10:
        value = s->format;
        break;
    default:
        break;
    }

    sf2000_log_mmio("lcd-read", SF2000_LCD_MMIO_BASE + addr, value, size);
    return value;
}

static void sf2000_lcd_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    SF2000LCDState *s = opaque;

    sf2000_log_mmio("lcd-write", SF2000_LCD_MMIO_BASE + addr, value, size);

    switch (addr) {
    case 0x00:
        s->control = value;
        break;
    case 0x04:
        s->fb_base = value;
        break;
    case 0x08:
        s->width = value & 0xffff;
        s->height = (value >> 16) & 0xffff;
        if (!s->width) {
            s->width = SF2000_LCD_WIDTH;
        }
        if (!s->height) {
            s->height = SF2000_LCD_HEIGHT;
        }
        qemu_console_resize(s->con, s->width, s->height);
        break;
    case 0x0c:
        s->stride = value;
        break;
    case 0x10:
        s->format = value;
        break;
    default:
        break;
    }

    s->redraw = true;
}

static const MemoryRegionOps sf2000_lcd_ops = {
    .read = sf2000_lcd_read,
    .write = sf2000_lcd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void sf2000_lcd_realize(DeviceState *dev, Error **errp)
{
    SF2000LCDState *s = SF2000_LCD(dev);
    const SF2000BoardProfileSpec *profile = sf2000_board_profile_spec();

    if (!s->width || s->width == SF2000_LCD_WIDTH) {
        s->width = profile->lcd_width;
    }
    if (!s->height || s->height == SF2000_LCD_HEIGHT) {
        s->height = profile->lcd_height;
    }
    if (!s->stride) {
        s->stride = s->width * 2;
    }
    if (!s->panel_te_hz) {
        s->panel_te_hz = profile->panel_te_hz;
    }

    s->as = &address_space_memory;
    s->con = graphic_console_init(dev, 0, &sf2000_lcd_gfx_ops, s);
    qemu_console_resize(s->con, s->width, s->height);
    s->scanout_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    sf2000_lcd_scanout_timer, s);
    timer_mod(s->scanout_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 30);
    s->panel_madctl = sf2000_panel_is_gb300() ? 0x28 : 0x70;
    s->panel_x1 = sf2000_panel_raw_width(s) - 1;
    s->panel_y1 = sf2000_panel_raw_height(s) - 1;
    /* The stock ASD inherits the bootloader's already-active RGB handoff. */
    s->gpio54 = 0x040004b2;
    s->panel_wr = !!(s->gpio54 & BIT(7));
    s->panel_ramctrl[0] = 0x11;
    s->panel_ramctrl[1] = 0xf0;
    s->panel_rgb_handoff_synchronized = true;
    sf2000_lcd = s;
    info_report("sf2000: lcd profile=%s panel=0x%08x geometry=%ux%u te=%uHz",
                sf2000_board_profile_name(), profile->panel_id, s->width,
                s->height, s->panel_te_hz);

    memory_region_init_io(&s->iomem, OBJECT(s), &sf2000_lcd_ops, s,
                          TYPE_SF2000_LCD, SF2000_LCD_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const Property sf2000_lcd_properties[] = {
    DEFINE_PROP_UINT64("fb-base", SF2000LCDState, fb_base, 0),
    DEFINE_PROP_UINT32("width", SF2000LCDState, width, SF2000_LCD_WIDTH),
    DEFINE_PROP_UINT32("height", SF2000LCDState, height, SF2000_LCD_HEIGHT),
    DEFINE_PROP_UINT32("stride", SF2000LCDState, stride, SF2000_LCD_WIDTH * 2),
    DEFINE_PROP_UINT32("format", SF2000LCDState, format, 0),
    DEFINE_PROP_UINT32("control", SF2000LCDState, control, 0),
    DEFINE_PROP_UINT32("panel-te-hz", SF2000LCDState, panel_te_hz, 0),
};

static char *sf2000_machine_board_profile_get(Object *obj, Error **errp)
{
    return g_strdup(sf2000_board_profile_name());
}

static void sf2000_machine_board_profile_set(Object *obj, const char *value,
                                             Error **errp)
{
    if (!value || !value[0]) {
        g_free(sf2000_board_profile);
        sf2000_board_profile = g_strdup("sf2000");
        return;
    }
    if (strcmp(value, "sf2000") != 0 && strcmp(value, "gb300") != 0) {
        error_setg(errp, "unsupported SF2000 board profile '%s'", value);
        return;
    }
    g_free(sf2000_board_profile);
    sf2000_board_profile = g_strdup(value);
}

static void sf2000_lcd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sf2000_lcd_realize;
    device_class_set_props(dc, sf2000_lcd_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo sf2000_lcd_info = {
    .name = TYPE_SF2000_LCD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SF2000LCDState),
    .class_init = sf2000_lcd_class_init,
};

static void sf2000_load_bootrom(MachineState *machine, MemoryRegion *sysmem)
{
    const char *bios = machine->firmware;
    MemoryRegion *boot = g_new(MemoryRegion, 1);
    MemoryRegion *flash_direct = g_new(MemoryRegion, 1);
    g_autofree uint8_t *contents = NULL;
    gsize len;
    int64_t size;
    GError *err = NULL;

    memory_region_init_rom(boot, NULL, "sf2000.bootrom", SF2000_BOOT_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, SF2000_BOOT_BASE, boot);
    memory_region_init_io(flash_direct, NULL, &sf2000_sflash_direct_ops, NULL,
                          "sf2000.sflash-direct", SF2000_BOOT_SIZE);
    memory_region_add_subregion(sysmem, SF2000_BOOT_ALIAS_BASE, flash_direct);

    if (!bios) {
        warn_report("sf2000: no -bios supplied; boot ROM is blank");
        return;
    }

    size = load_image_targphys(bios, SF2000_BOOT_BASE, SF2000_BOOT_SIZE, 0);
    if (size < 0) {
        error_report("sf2000: failed to load firmware '%s'", bios);
        exit(1);
    }
    if (!g_file_get_contents(bios, (char **)&contents, &len, &err)) {
        error_report("sf2000: failed to mirror firmware '%s'", bios);
        if (err) {
            error_report("%s", err->message);
            g_error_free(err);
        }
        exit(1);
    }
    memcpy(sf2000_bootrom_bytes, contents, MIN((gsize)SF2000_BOOT_SIZE, len));
    if (len > SF2000_BL_FLASH_OFF &&
        len - SF2000_BL_FLASH_OFF <= machine->ram_size - SF2000_BL_RAM_BASE) {
        gsize bootloader_len = len - SF2000_BL_FLASH_OFF;
        rom_add_blob_fixed("sf2000.bootloader.ram",
                           contents + SF2000_BL_FLASH_OFF, bootloader_len,
                           SF2000_BL_RAM_BASE);
        sf2000_bootrom_ram_entry = true;
        info_report("sf2000: mirrored bootloader %" G_GSIZE_FORMAT
                    " bytes from flash+0x%08" PRIx64
                    " at 0x%08" PRIx64 ", entry 0x%08" PRIx64,
                    bootloader_len, (uint64_t)SF2000_BL_FLASH_OFF,
                    (uint64_t)SF2000_BL_RAM_BASE,
                    (uint64_t)SF2000_BL_ENTRY);
    }

    info_report("sf2000: loaded %" PRId64 " bytes at 0x%08" PRIx64 " from %s",
                size, (uint64_t)SF2000_BOOT_BASE, bios);
}

static void sf2000_load_asd(MachineState *machine)
{
    g_autofree uint8_t *contents = NULL;
    gsize len;
    int64_t size;
    GError *err = NULL;

    loaderparams.kernel_filename = machine->kernel_filename;
    loaderparams.kernel_entry = SF2000_ASD_ENTRY;
    loaderparams.dtb_vaddr = 0;
    loaderparams.linux_elf = false;

    if (!machine->kernel_filename) {
        return;
    }

    if (!g_file_get_contents(machine->kernel_filename, (char **)&contents,
                             &len, &err)) {
        error_report("sf2000: failed to load ASD image '%s'",
                     machine->kernel_filename);
        if (err) {
            error_report("%s", err->message);
            g_error_free(err);
        }
        exit(1);
    }
    if (len <= SF2000_ASD_SKIP ||
        len - SF2000_ASD_SKIP > machine->ram_size - SF2000_ASD_LOAD_BASE) {
        error_report("sf2000: invalid ASD size %" G_GSIZE_FORMAT, len);
        exit(1);
    }

    rom_add_blob_fixed("sf2000.asd", contents + SF2000_ASD_SKIP,
                       len - SF2000_ASD_SKIP, SF2000_ASD_LOAD_BASE);
    size = len - SF2000_ASD_SKIP;

    info_report("sf2000: loaded ASD %" PRId64 " bytes at 0x%08" PRIx64
                " skip=0x%08" PRIx64 ", entry 0x%08" PRIx64 " from %s",
                size, (uint64_t)SF2000_ASD_LOAD_BASE,
                (uint64_t)SF2000_ASD_SKIP,
                (uint64_t)SF2000_ASD_ENTRY, machine->kernel_filename);
}

static bool sf2000_try_load_linux_elf(MachineState *machine)
{
    uint64_t kernel_entry = 0;
    uint64_t kernel_high = 0;
    uint64_t kernel_low = 0;
    ssize_t kernel_size;
    hwaddr dtb_paddr;
    hwaddr dtb_vaddr;
    void *dtb;
    g_autofree char *dtb_contents = NULL;
    gsize dtb_size;
    GError *err = NULL;

    if (!machine->kernel_filename) {
        return false;
    }

    kernel_size = load_elf(machine->kernel_filename, NULL,
                           cpu_mips_kseg0_to_phys, NULL,
                           &kernel_entry, &kernel_low, &kernel_high,
                           NULL, ELFDATA2LSB, EM_MIPS, 1, 0);
    if (kernel_size == ELF_LOAD_NOT_ELF) {
        return false;
    }
    if (kernel_size < 0) {
        error_report("sf2000: failed to load Linux ELF '%s': %s",
                     machine->kernel_filename, load_elf_strerror(kernel_size));
        exit(1);
    }
    if (!machine->dtb) {
        error_report("sf2000: Linux ELF boot requires -dtb");
        exit(1);
    }

    if (!g_file_get_contents(machine->dtb, &dtb_contents, &dtb_size, &err)) {
        error_report("sf2000: failed to load DTB '%s'", machine->dtb);
        if (err) {
            error_report("%s", err->message);
            g_error_free(err);
        }
        exit(1);
    }

    dtb = g_memdup2(dtb_contents, dtb_size);
    dtb_paddr = QEMU_ALIGN_UP(kernel_high, SF2000_LINUX_DTB_ALIGN);
    if (dtb_paddr + dtb_size > machine->ram_size) {
        error_report("sf2000: no RAM left for DTB at 0x%08" HWADDR_PRIx,
                     dtb_paddr);
        g_free(dtb);
        exit(1);
    }
    dtb_vaddr = cpu_mips_phys_to_kseg0(NULL, dtb_paddr);

    machine->fdt = dtb;
    rom_add_blob_fixed("sf2000.dtb", dtb, dtb_size, dtb_paddr);

    loaderparams.kernel_filename = machine->kernel_filename;
    loaderparams.kernel_entry = kernel_entry;
    loaderparams.dtb_vaddr = dtb_vaddr;
    loaderparams.linux_elf = true;

    info_report("sf2000: loaded Linux ELF %" PRId64
                " bytes low=0x%08" PRIx64 " high=0x%08" PRIx64
                " entry=0x%08" PRIx64 " dtb=0x%08" HWADDR_PRIx
                " from %s",
                (int64_t)kernel_size, kernel_low, kernel_high, kernel_entry,
                dtb_paddr, machine->kernel_filename);
    return true;
}

static void sf2000_load_kernel(MachineState *machine)
{
    if (!machine->kernel_filename) {
        return;
    }
    if (!sf2000_try_load_linux_elf(machine)) {
        sf2000_load_asd(machine);
    }
}

static void sf2000_seed_boot_handoff(void)
{
    uint8_t header[8];
    uint8_t data[64];

    /*
     * Stock maincode calls get_sysinfo_from_bl(), which expects the bootloader
     * to leave magic at KSEG1 0xa0000010 and a kseg1 pointer at 0xa0000014.
     * The structure is a compact boot handoff table. Only the display/output
     * type is required for the menu to leave early AppInit; seed RGB_LCD and
     * keep the remaining bytes zero until probing identifies more fields. The
     * pointer advertises 0xa0000100, while the blob is backed at physical RAM
     * 0x100.
     */
    memset(header, 0, sizeof(header));
    memset(data, 0, sizeof(data));
    stl_le_p(header, SF2000_BL_INFO_MAGIC);
    stl_le_p(header + 4, SF2000_BL_INFO_DATA_KSEG1);
    stl_le_p(data, SF2000_TVSYS_RGB_LCD);

    rom_add_blob_fixed("sf2000.bl-handoff.header", header, sizeof(header),
                       SF2000_BL_INFO_MAGIC_ADDR);
    rom_add_blob_fixed("sf2000.bl-handoff.data", data, sizeof(data),
                       SF2000_BL_INFO_DATA_ADDR);
}

static void sf2000_cpu_reset(void *opaque)
{
    MIPSCPU *cpu = opaque;
    CPUMIPSState *env = &cpu->env;

    sf2000_wdt_count = 0;
    sf2000_wdt_disable();
    sf2000_ge_irq_pending = false;
    sf2000_ge_submit_sequence = 0;
    sf2000_ge_irq_ack_sequence = 0;
    sf2000_ge_queue_min = 0;
    if (sf2000_ge_timer) {
        timer_del(sf2000_ge_timer);
    }
    sf2000_mmio_set32(SF2000_GE_STATUS, 0);

    sf2000_sdio_arg = 0;
    sf2000_sdio_cmd = 0;
    memset(sf2000_sdio_resp, 0, sizeof(sf2000_sdio_resp));
    sf2000_sdio_dma_addr = 0;
    sf2000_sdio_dma_len = 0;
    sf2000_sdio_dma_wr_addr = 0;
    sf2000_sdio_dma_wr_len = 0;
    sf2000_sdio_xfer_done = false;
    sf2000_sdio_xfer_busy = false;
    sf2000_sdio_write_active = false;
    sf2000_sdio_pio_state = 0;
    sf2000_sdio_irq_pending = false;
    sf2000_sdio_callback_pending = false;
    sf2000_sdio_app_cmd = false;
    sf2000_sdio_bus_width = 1;
    sf2000_last_unifrog_trace_count = 0;
    sf2000_last_unifrog_trace_valid = false;
    memset(sf2000_active_gma, 0, sizeof(sf2000_active_gma));
    memset(sf2000_gma_mask_armed, 0, sizeof(sf2000_gma_mask_armed));

    memset(sf2000_timer_cnt, 0, sizeof(sf2000_timer_cnt));
    memset(sf2000_timer_aim, 0, sizeof(sf2000_timer_aim));
    memset(sf2000_timer_ctrl, 0, sizeof(sf2000_timer_ctrl));
    memset(sf2000_timer_deadline_ns, 0,
           sizeof(sf2000_timer_deadline_ns));
    sf2000_sb_timer_irq_masked = false;
    sf2000_timer_ack();

    sf2000_audio_dma_base = 0;
    sf2000_audio_dma_bytes = 0;
    sf2000_audio_dma_producer = 0;
    sf2000_audio_dma_consumer = 0;
    sf2000_audio_dma_queued = 0;
    sf2000_audio_snd_ctl04 = 0;
    sf2000_audio_snd_ctl08 = 0;
    sf2000_audio_snd_ctl0c = 0;
    sf2000_audio_snd_ctl50 = 0;
    sf2000_audio_stc_control = 0;
    sf2000_audio_stc_tick[0] = 0;
    sf2000_audio_stc_tick[1] = 0;
    sf2000_audio_stc_epoch_ns[0] =
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    sf2000_audio_stc_epoch_ns[1] =
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    sf2000_audio_dma_reported = false;
    sf2000_audio_callback_reported = false;
    sf2000_audio_apll_reset_seen = false;

    cpu_reset(CPU(cpu));

    if (loaderparams.kernel_filename) {
        env->CP0_Status &= ~((1 << CP0St_BEV) | (1 << CP0St_ERL));
        if (loaderparams.linux_elf) {
            env->active_tc.gpr[4] = -2;
            env->active_tc.gpr[5] = loaderparams.dtb_vaddr;
            env->active_tc.gpr[6] = 0;
            env->active_tc.gpr[7] = 0;
        }
        env->active_tc.PC = loaderparams.kernel_entry;
    } else if (sf2000_bootrom_ram_entry) {
        env->CP0_Status &= ~((1 << CP0St_BEV) | (1 << CP0St_ERL));
        env->active_tc.PC = SF2000_BL_ENTRY;
    }
}

static void sf2000_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    MemoryRegion *mmio = g_new(MemoryRegion, 1);
    Clock *cpuclk;
    MIPSCPU *cpu;
    DeviceState *lcd;
    DriveInfo *dinfo;

    if (machine->ram_size == 0) {
        machine->ram_size = SF2000_RAM_DEFAULT;
    }

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, sf2000_cpu_hz());
    cpu = mips_cpu_create_with_clock(machine->cpu_type, cpuclk, false);
    cpu_mips_irq_init_cpu(cpu);
    sf2000_eirq3 = cpu->env.irq[SF2000_EIRQ3_LINE];
    /*
     * Real-device UniFrog probe logprobe0000 captured these values after the
     * SDK had initialized display, PWM/backlight, storage, and input. Seed the
     * stateful helper models with the same quiet baseline; explicit firmware
     * writes still replace these values through their normal handlers.
     */
    /* HC15xx battery ADC reports volts * 50 in CTRL0[23:16]. */
    sf2000_adc_ctrl[0] = 200u << 16; /* 4.00 V, a realistic charged cell. */
    sf2000_adc_ctrl[1] = 0x20001400;
    sf2000_adc_ctrl[2] = 0x00000f2d;
    sf2000_adc_ctrl[3] = 0x00000001;
    /*
     * Probe captures contain the masks after the vendor SDK has initialized
     * the interrupt controller.  They are not reset values: pre-enabling the
     * SDIO bit lets a polled command hold EIRQ3 high before Linux installs an
     * SDIO child handler, starving the scheduler timer.  Start masked and let
     * each guest establish its own routing.
     */
    sf2000_irq_enable1 = 0;
    sf2000_irq_enable2 = 0;
    sf2000_audio_i2s_ctrl3c = 0x0000ff41;
    sf2000_audio_i2s_fade90 = 0x008f0000;
    sf2000_i2c_data[0] = 0x00;
    sf2000_i2c_isr[0] = 0x00;
    sf2000_i2c_ier[0] = 0x00;
    sf2000_i2c_isr1[0] = 0x00;
    sf2000_i2c_ier1[0] = 0x00;
    sf2000_wdt_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    sf2000_wdt_timer_cb, NULL);
    sf2000_ge_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   sf2000_ge_timer_cb, NULL);
    sf2000_timer_ack();
    sf2000_next_vsync_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                           NANOSECONDS_PER_SECOND / 60;
    sf2000_irq_poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         sf2000_irq_poll_timer_cb, NULL);
    timer_mod(sf2000_irq_poll_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 1000);
    cpu_mips_clock_init(cpu);
    cpu->env.CP0_IntCtl = 0;
    qemu_register_reset(sf2000_cpu_reset, cpu);

    memory_region_init_ram(ram, NULL, "sf2000.ram", machine->ram_size,
                           &error_fatal);
    sf2000_ram_region = ram;
    memory_region_set_log(ram, true, DIRTY_MEMORY_VGA);
    memory_region_add_subregion(sysmem, SF2000_RAM_BASE, ram);
    sf2000_seed_boot_handoff();

    memory_region_init_io(mmio, NULL, &sf2000_unimp_ops, NULL,
                          "sf2000.unimplemented-mmio", SF2000_MMIO_SIZE);
    memory_region_add_subregion(sysmem, SF2000_MMIO_BASE, mmio);

    lcd = qdev_new(TYPE_SF2000_LCD);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(lcd), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(lcd), 0, SF2000_LCD_MMIO_BASE);
    qemu_input_handler_activate(qemu_input_handler_register(
        lcd, &sf2000_keyboard_handler));
    sf2000_panel_readback_selftest();
    sf2000_audio_backend_init(machine);
    sf2000_usb_link_powered[0] = true;
    sf2000_usb_link_powered[1] = true;
    sf2000_usb_link_active[0] = false;
    sf2000_usb_link_active[1] = false;
    sf2000_usb_power_reg[0] = SF2000_MUSB_POWER_RESET_READ;
    sf2000_usb_power_reg[1] = SF2000_MUSB_POWER_RESET_READ;
    sf2000_usb_devctl_reg[0] = SF2000_MUSB_DEVCTL_RESET_READ;
    sf2000_usb_devctl_reg[1] = SF2000_MUSB_DEVCTL_RESET_READ;
    sf2000_usb_regs[0][0x380 >> 2] = sf2000_board_profile_spec()->usb_utmi380;
    sf2000_usb_regs[1][0x380 >> 2] = sf2000_board_profile_spec()->usb_utmi380;
    sf2000_usb_regs[0][0x384 >> 2] = sf2000_board_profile_spec()->usb_phy384;
    sf2000_usb_regs[1][0x384 >> 2] = sf2000_board_profile_spec()->usb_phy384;
    sf2000_audio_state_selftest();
    sf2000_audio_pcm_selftest();
    sf2000_audio_gate_live_selftest();
    sf2000_audio_gate_live_variant_selftest();
    sf2000_audio_hw_close_selftest();
    sf2000_pwm2_backlight_selftest();
    g_autofree char *pwm2_backlight_active =
        sf2000_machine_pwm2_backlight_active_get(NULL, NULL);
    sf2000_pwm2_backlight_blank_selftest();
    sf2000_usb_reset_block_selftest();
    sf2000_usb_link_state_selftest();
    sf2000_usb_phy_snapshot_selftest();
    g_autofree char *gate_live_l = sf2000_machine_audio_gate_l_live_get(NULL, NULL);
    g_autofree char *gate_live_r = sf2000_machine_audio_gate_r_live_get(NULL, NULL);

    g_autofree char *usb0_power = sf2000_machine_usb0_power_get(NULL, NULL);
    g_autofree char *usb1_power = sf2000_machine_usb1_power_get(NULL, NULL);

    info_report("sf2000: board profile=%s panel=0x%08x probe=%08x/%08x gpio=%s audio=%s open=%s runtime_open=%s open_returns=%s close_returns=%s hw_close=%s gate_state=%s gate_live_l=%s gate_live_r=%s sr=%u ch=%u runtime_ch=%u "
                "pwm2_backlight_active=%s "
                "period=%u/%u vol=%u gain=%u gate=%s gate_l=0x%08x/0x%08x gate_r=0x%08x/0x%08x "
                "mux=%s hw=%u snd0=0x%08x dac=0x%08x usb0=%s usb1=%s usb0_power=%s usb1_power=%s hub=%s ports=%u "
                "storage-reset=%s",
                sf2000_board_profile_name(),
                sf2000_board_profile_spec()->panel_id,
                sf2000_board_profile_spec()->panel_probe_sig1,
                sf2000_board_profile_spec()->panel_probe_sig2,
                sf2000_board_profile_spec()->gpio_init,
                sf2000_board_profile_spec()->audio_route,
                sf2000_board_profile_spec()->audio_open_route,
                sf2000_board_profile_spec()->audio_runtime_route,
                sf2000_board_profile_spec()->audio_open_returns,
                sf2000_board_profile_spec()->audio_close_returns,
                sf2000_board_profile_spec()->audio_hw_close,
                sf2000_audio_output_active() ? "open" : "closed",
                gate_live_l,
                gate_live_r,
                sf2000_board_profile_spec()->audio_sample_rate_hz,
                sf2000_board_profile_spec()->audio_channels,
                sf2000_board_profile_spec()->audio_runtime_channels,
                pwm2_backlight_active,
                sf2000_board_profile_spec()->audio_period_frames,
                sf2000_board_profile_spec()->audio_periods,
                sf2000_board_profile_spec()->audio_volume,
                sf2000_board_profile_spec()->audio_gain,
                sf2000_board_profile_spec()->audio_gate_route,
                sf2000_board_profile_spec()->audio_gate_l0,
                sf2000_board_profile_spec()->audio_gate_l1,
                sf2000_board_profile_spec()->audio_gate_r0,
                sf2000_board_profile_spec()->audio_gate_r1,
                sf2000_board_profile_spec()->audio_mux_open,
                sf2000_board_profile_spec()->audio_hw_backend,
                sf2000_board_profile_spec()->audio_hw_snd0,
                sf2000_board_profile_spec()->audio_hw_dac,
                sf2000_board_profile_spec()->usb0_route,
                sf2000_board_profile_spec()->usb1_route,
                usb0_power,
                usb1_power,
                sf2000_board_profile_spec()->usb_root_hub_id,
                sf2000_board_profile_spec()->usb_root_hub_ports,
                sf2000_board_profile_spec()->storage_reset);

    sf2000_sdio_blk = blk_by_name("sd0");
    dinfo = sf2000_sdio_blk ? NULL : drive_get(IF_SD, 0, 0);
    if (dinfo) {
        sf2000_sdio_blk = blk_by_legacy_dinfo(dinfo);
    }
    if (sf2000_sdio_blk) {
        int ret = blk_set_perm(sf2000_sdio_blk,
                               BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                               BLK_PERM_ALL, &error_fatal);

        if (ret < 0) {
            exit(1);
        }
        info_report("sf2000: using SD image '%s'", blk_name(sf2000_sdio_blk));
        if (sf2000_storage_selftest_raw) {
            sf2000_sdio_raw_writeback_selftest();
        }
    } else {
        info_report("sf2000: no SD image supplied; using synthetic FAT probe media");
        sf2000_sdio_synth_writeback_selftest();
    }

    sf2000_load_bootrom(machine, sysmem);
    sf2000_load_kernel(machine);

    /* MIPS reset starts from 0xbfc00000, which maps to physical 0x1fc00000. */
}

static void sf2000_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Data Frog SF2000 / HC15xx bring-up board";
    mc->init = sf2000_init;
    mc->default_ram_size = SF2000_RAM_DEFAULT;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("24Kc");
    mc->no_parallel = true;
    mc->no_floppy = true;
    mc->no_cdrom = true;
    machine_add_audiodev_property(mc);
    object_class_property_add_str(oc, "board-profile",
                                  sf2000_machine_board_profile_get,
                                  sf2000_machine_board_profile_set);
    object_class_property_add_str(oc, "audio-route",
                                  sf2000_machine_audio_route_get, NULL);
    object_class_property_add_str(oc, "audio-open-route",
                                  sf2000_machine_audio_open_route_get, NULL);
    object_class_property_add_str(oc, "audio-runtime-route",
                                  sf2000_machine_audio_runtime_route_get, NULL);
    object_class_property_add_str(oc, "audio-open-returns",
                                  sf2000_machine_audio_open_returns_get, NULL);
    object_class_property_add_str(oc, "audio-close-returns",
                                  sf2000_machine_audio_close_returns_get, NULL);
    object_class_property_add_str(oc, "audio-gate-route",
                                  sf2000_machine_audio_gate_route_get, NULL);
    object_class_property_add_str(oc, "audio-gate-l",
                                  sf2000_machine_audio_gate_l_get, NULL);
    object_class_property_add_str(oc, "audio-gate-r",
                                  sf2000_machine_audio_gate_r_get, NULL);
    object_class_property_add_str(oc, "audio-gate-l-live",
                                  sf2000_machine_audio_gate_l_live_get, NULL);
    object_class_property_add_str(oc, "audio-gate-r-live",
                                  sf2000_machine_audio_gate_r_live_get, NULL);
    object_class_property_add_str(oc, "audio-mux",
                                  sf2000_machine_audio_mux_get, NULL);
    object_class_property_add_str(oc, "audio-hw-backend",
                                  sf2000_machine_audio_hw_backend_get, NULL);
    object_class_property_add_str(oc, "audio-hw-snd0",
                                  sf2000_machine_audio_hw_snd0_get, NULL);
    object_class_property_add_str(oc, "audio-hw-dac",
                                  sf2000_machine_audio_hw_dac_get, NULL);
    object_class_property_add_str(oc, "audio-hw-close",
                                  sf2000_machine_audio_hw_close_get, NULL);
    object_class_property_add_str(oc, "audio-volume",
                                  sf2000_machine_audio_volume_get, NULL);
    object_class_property_add_str(oc, "audio-gain",
                                  sf2000_machine_audio_gain_get, NULL);
    object_class_property_add_str(oc, "audio-muted",
                                  sf2000_machine_audio_muted_get, NULL);
    object_class_property_add_str(oc, "audio-gate-state",
                                  sf2000_machine_audio_gate_state_get, NULL);
    object_class_property_add_str(oc, "audio-power",
                                  sf2000_machine_audio_power_get, NULL);
    object_class_property_add_str(oc, "audio-backend-ready",
                                  sf2000_machine_audio_backend_ready_get, NULL);
    object_class_property_add_str(oc, "audio-dac-value",
                                  sf2000_machine_audio_dac_value_get, NULL);
    object_class_property_add_str(oc, "audio-sample-rate",
                                  sf2000_machine_audio_sample_rate_get, NULL);
    object_class_property_add_str(oc, "audio-channels",
                                  sf2000_machine_audio_channels_get, NULL);
    object_class_property_add_str(oc, "audio-runtime-channels",
                                  sf2000_machine_audio_runtime_channels_get, NULL);
    object_class_property_add_str(oc, "audio-period-frames",
                                  sf2000_machine_audio_period_frames_get, NULL);
    object_class_property_add_str(oc, "audio-periods",
                                  sf2000_machine_audio_periods_get, NULL);
    object_class_property_add_str(oc, "panel-id",
                                  sf2000_machine_panel_id_get, NULL);
    object_class_property_add_str(oc, "panel-te-hz",
                                  sf2000_machine_panel_te_hz_get, NULL);
    object_class_property_add_str(oc, "panel-probe-sig1",
                                  sf2000_machine_panel_probe_sig1_get, NULL);
    object_class_property_add_str(oc, "panel-probe-sig2",
                                  sf2000_machine_panel_probe_sig2_get, NULL);
    object_class_property_add_str(oc, "gpio-init",
                                  sf2000_machine_gpio_init_get, NULL);
    object_class_property_add_str(oc, "gpio-l-out",
                                  sf2000_machine_gpio_l_out_get, NULL);
    object_class_property_add_str(oc, "pwm2-backlight",
                                  sf2000_machine_pwm2_backlight_get, NULL);
    object_class_property_add_str(oc, "pwm2-backlight-active",
                                  sf2000_machine_pwm2_backlight_active_get,
                                  NULL);
    object_class_property_add_str(oc, "audio-i2s-ctrl3c",
                                  sf2000_machine_audio_i2s_ctrl3c_get, NULL);
    object_class_property_add_str(oc, "audio-i2s-fade90",
                                  sf2000_machine_audio_i2s_fade90_get, NULL);
    object_class_property_add_str(oc, "audio-stc",
                                  sf2000_machine_audio_stc_get, NULL);
    object_class_property_add_str(oc, "audio-dma",
                                  sf2000_machine_audio_dma_get, NULL);
    object_class_property_add_bool(oc, "storage-selftest-raw",
                                   sf2000_machine_storage_selftest_raw_get,
                                   sf2000_machine_storage_selftest_raw_set);
    object_class_property_add_str(oc, "usb0-route",
                                  sf2000_machine_usb0_route_get, NULL);
    object_class_property_add_str(oc, "usb1-route",
                                  sf2000_machine_usb1_route_get, NULL);
    object_class_property_add_str(oc, "usb-root-hub-id",
                                  sf2000_machine_usb_root_hub_id_get, NULL);
    object_class_property_add_str(oc, "usb-root-hub-ports",
                                  sf2000_machine_usb_root_hub_ports_get, NULL);
    object_class_property_add_str(oc, "usb-ctl0",
                                  sf2000_machine_usb_ctl0_get, NULL);
    object_class_property_add_str(oc, "usb-ctl1",
                                  sf2000_machine_usb_ctl1_get, NULL);
    object_class_property_add_str(oc, "usb-phy0",
                                  sf2000_machine_usb_phy0_get, NULL);
    object_class_property_add_str(oc, "usb-phy1",
                                  sf2000_machine_usb_phy1_get, NULL);
    object_class_property_add_str(oc, "usb-phy2",
                                  sf2000_machine_usb_phy2_get, NULL);
    object_class_property_add_str(oc, "usb-phy3",
                                  sf2000_machine_usb_phy3_get, NULL);
    object_class_property_add_str(oc, "storage-reset",
                                  sf2000_machine_storage_reset_get, NULL);
    object_class_property_add_str(oc, "usb0-state",
                                  sf2000_machine_usb0_state_get, NULL);
    object_class_property_add_str(oc, "usb1-state",
                                  sf2000_machine_usb1_state_get, NULL);
    object_class_property_add_str(oc, "usb0-devctl",
                                  sf2000_machine_usb0_devctl_get, NULL);
    object_class_property_add_str(oc, "usb1-devctl",
                                  sf2000_machine_usb1_devctl_get, NULL);
    object_class_property_add_str(oc, "usb0-power",
                                  sf2000_machine_usb0_power_get, NULL);
    object_class_property_add_str(oc, "usb1-power",
                                  sf2000_machine_usb1_power_get, NULL);
    object_class_property_add_str(oc, "usb0-utmi380",
                                  sf2000_machine_usb0_utmi380_get, NULL);
    object_class_property_add_str(oc, "usb1-utmi380",
                                  sf2000_machine_usb1_utmi380_get, NULL);
    object_class_property_add_str(oc, "usb0-phy384",
                                  sf2000_machine_usb0_phy384_get, NULL);
    object_class_property_add_str(oc, "usb1-phy384",
                                  sf2000_machine_usb1_phy384_get, NULL);
}

static const TypeInfo sf2000_machine_type = {
    .name = MACHINE_TYPE_NAME("sf2000"),
    .parent = TYPE_MACHINE,
    .class_init = sf2000_machine_class_init,
};

static void sf2000_register_types(void)
{
    type_register_static(&sf2000_lcd_info);
    type_register_static(&sf2000_machine_type);
}

type_init(sf2000_register_types)
