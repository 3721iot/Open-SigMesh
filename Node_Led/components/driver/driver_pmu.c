/**
 * Copyright (c) 2019, Freqchip
 *
 * All rights reserved.
 *
 *
 */

/*
 * INCLUDES
 */
#include <stdint.h>

#include "co_printf.h"
#include "co_math.h"
#include "jump_table.h"

#include "sys_utils.h"
#include "driver_plf.h"
#include "driver_pmu.h"
#include "driver_adc.h"
#include "driver_wdt.h"
#include "driver_efuse.h"
#include "driver_system.h"
#include "driver_flash.h"

#define ENABLE_SYSTEM_PROTECTION_IN_LVD             1

/*
 * EXTERNAL FUNCTIONS
 */
void pmu_enable_isr(uint8_t isr_map);
void pmu_enable_isr2(uint8_t isr_map);
void pmu_disable_isr(uint8_t isr_map);
void pmu_disable_isr2(uint8_t isr_map);
uint16_t pmu_get_isr_state(void);

uint8_t adc_ref_internal_trim = 80;

/*********************************************************************
 * @fn      pmu_set_debounce_clk
 *
 * @brief   set the debounce base clock used for most modules inside PMU
 *
 * @param   div     - debounce cycles
 *
 * @return  None.
 */
static void pmu_set_debounce_clk(uint16_t div)
{
    //Debounce period = pmu_clk_period * div * 2
    ool_write(PMU_REG_BT_WKUP_CTRL, (ool_read(PMU_REG_BT_WKUP_CTRL) & (~PMU_DEB_CLK_DIV_MSK)) | (div<<PMU_DEB_CLK_DIV_POS));
    ool_write(PMU_REG_CLK_CTRL, ool_read(PMU_REG_CLK_CTRL) | PMU_DEB_CLK_EN);   //enalbe debounce clk
}

/*********************************************************************
 * @fn      pmu_onkey_set_debounce_cnt
 *
 * @brief   set the debounce of onkey checking module
 *
 * @param   cnt     - debounce cycles, max: 0x0f, min: 0x00
 *                    onkey anti-shake time = (debounce period) * ( cnt<<5)
 *
 * @return  None.
 */
static void pmu_onkey_set_debounce_cnt(uint8_t cnt)
{
    ool_write(PMU_REG_DEB_CFG, (ool_read(PMU_REG_DEB_CFG) & 0xf0 ) | ((cnt&0xf)<<PMU_DEB_ONKEY_POS) );  // 10ms
}

/*********************************************************************
 * @fn      pmu_adkey_set_debounce_cnt
 *
 * @brief   set the debounce of adkey checking module
 *
 * @param   cnt     - debounce cycles, max: 0x0f, min: 0x00
 *                    onkey anti-shake time = (debounce period) * ( cnt<<5)
 *
 * @return  None.
 */
static void pmu_adkey_set_debounce_cnt(uint8_t cnt)
{
    ool_write(PMU_REG_DEB_CFG, (ool_read(PMU_REG_DEB_CFG) & 0x0f ) | ((cnt&0xf)<<PMU_DEB_ADKEY_POS) );  // 10ms
}

/*********************************************************************
 * @fn      pmu_bat_full_set_debounce_cnt
 *
 * @brief   set the debounce of battery charging module
 *
 * @param   cnt     - debounce cycles, max: 0xff, min: 0x00
 *                    onkey anti-shake time = (debounce period) * ( cnt<<5)
 *
 * @return  None.
 */
static void pmu_bat_full_set_debounce_cnt(uint8_t cnt)
{
    ool_write(PMU_REG_BAT_DEB_LEN, cnt);   // {0x5d, 5'h0} = 2976
    ool_write(PMU_REG_LVD_BAT_DEB_CFG, ool_read(PMU_REG_LVD_BAT_DEB_CFG) & (~PMU_BAT_DEB_SEL));  // reset debounce block

    /* release reset when enable irq */
    //co_delay_10us(12);
    //ool_write(PMU_REG_LVD_BAT_DEB_CFG, ool_read(PMU_REG_LVD_BAT_DEB_CFG) | PMU_BAT_DEB_SEL);    //enalbe debounce
}

/*********************************************************************
 * @fn      pmu_lvd_set_debounce_cnt
 *
 * @brief   set the debounce of battery low voltage detect module
 *
 * @param   cnt     - debounce cycles, max: 0xff, min: 0x00
 *                    onkey anti-shake time = (debounce period) * ( cnt<<5)
 *
 * @return  None.
 */
static void pmu_lvd_set_debounce_cnt(uint8_t cnt)
{
    ool_write(PMU_REG_LVD_DEB_LEN, cnt);   // {0x5d, 5'h0} = 2976
    ool_write(PMU_REG_LVD_BAT_DEB_CFG, ool_read(PMU_REG_LVD_BAT_DEB_CFG) & (~PMU_LVD_DEB_SEL));  // reset debounce block

    /* release reset when enable irq */
    //co_delay_10us(12);
    //ool_write(PMU_REG_LVD_BAT_DEB_CFG, ool_read(PMU_REG_LVD_BAT_DEB_CFG) | PMU_LVD_DEB_SEL);    //enalbe debounce
}

/*********************************************************************
 * @fn      pmu_chg_dec_set_debounce_cnt
 *
 * @brief   set the debounce of charging pull-out and insert detect module
 *
 * @param   cnt     - debounce cycles, max: 0x1f, min: 0x00
 *                    onkey anti-shake time = (debounce period) * ( cnt<<5)
 *
 * @return  None.
 */
static void pmu_chg_dec_set_debounce_cnt(uint8_t cnt)
{
    ool_write(PMU_REG_ACOK_DEB_NEW_CFG, ool_read(PMU_REG_ACOK_DEB_NEW_CFG) & (~PMU_ACOK_DEB_NEW_SEL));  // reset debounce block
    ool_write(PMU_REG_ACOK_DEB_NEW_CFG, (cnt&0x1f)<<PMU_ACOK_DEB_NEW_LEN_POS );  // {0x06, 5'h0} = 192

    /* release reset when enable irq */
    //co_delay_10us(12);
    //ool_write(PMU_REG_ACOK_DEB_NEW_CFG, ool_read(PMU_REG_ACOK_DEB_NEW_CFG) | (PMU_ACOK_DEB_NEW_SEL));  // release debounce block
}

/*********************************************************************
 * @fn      pmu_set_pin_to_PMU
 *
 * @brief   Hand over the IO control from digital to PMU (always on),
 *          this function can be used to set more than one IO belong
 *          to the same port.
 *          example usage: pmu_set_pin_to_PMU(GPIO_PORT_A, (1<<GPIO_BIT_0) | (1<<GPIO_BIT_1))
 *
 * @param   port    - which group the io belongs to, @ref system_port_t
 *          bits    - the numbers of io
 *
 * @return  None.
 */
void pmu_set_pin_to_PMU(enum system_port_t port, uint8_t bits)
{
    uint8_t sel_reg = PMU_REG_PORTA_SEL;
    sel_reg += port;
    ool_write(sel_reg, (ool_read(sel_reg) & (~bits)));
}

/*********************************************************************
 * @fn      pmu_set_pin_to_CPU
 *
 * @brief   Hand over the IO control from digital to CPU, this function
 *          can be used to set more than one IO belong to the same port.
 *          example usage: pmu_set_pin_to_CPU(GPIO_PORT_A, (1<<GPIO_BIT_0) | (1<<GPIO_BIT_1))
 *
 * @param   port    - which group the io belongs to, @ref system_port_t
 *          bits    - the numbers of io
 *
 * @return  None.
 */
void pmu_set_pin_to_CPU(enum system_port_t port, uint8_t bits)
{
    uint8_t sel_reg = PMU_REG_PORTA_SEL;
    sel_reg += port;
    ool_write(sel_reg, (ool_read(sel_reg) | bits));
}

/*********************************************************************
 * @fn      pmu_set_port_mux
 *
 * @brief   used to set the function of IO controlled by PMU.
 *          example usage:
 *          pmu_set_port_mux(GPIO_PORT_A, GPIO_BIT_0, PMU_PORT_MUX_KEYSCAN)
 *
 * @param   port    - which group the io belongs to, @ref system_port_t
 *          bit     - the number of io
 *          func    - the function of gpio, @ref pmu_gpio_mux_t
 *
 * @return  None.
 */
void pmu_set_port_mux(enum system_port_t port, enum system_port_bit_t bit, enum pmu_gpio_mux_t func)
{
    uint16_t value;
    uint8_t pmu_port_reg;

    if(port == GPIO_PORT_A)
        pmu_port_reg = PMU_REG_PORTA_MUX_L;
    else if(port == GPIO_PORT_B)
        pmu_port_reg = PMU_REG_PORTB_MUX_L;
    else if(port == GPIO_PORT_C)
        pmu_port_reg = PMU_REG_PORTC_MUX_L;
    else if(port == GPIO_PORT_D)
        pmu_port_reg = PMU_REG_PORTD_MUX_L;

    value = ool_read16(pmu_port_reg);
    value &= (~(PMU_PORT_MUX_MSK<<(PMU_PORT_MUX_LEN*bit)));
    value |= (func << (PMU_PORT_MUX_LEN*bit));
    ool_write16(pmu_port_reg, value );
}

/*********************************************************************
 * @fn      pmu_set_pin_dir
 *
 * @brief   set the in-out of IOs which are controlled by PMU and in GPIO mode.
 *          example usage:
 *          pmu_set_pin_dir(GPIO_PORT_A, (1<<GPIO_BIT_0) | (1<<GPIO_BIT_1), GPIO_DIR_OUT)
 *
 * @param   port    - which group the io belongs to, @ref system_port_t
 *          bits    - the numbers of io
 *          dir     - the direction of in-out
 *
 * @return  None.
 */
void pmu_set_pin_dir(enum system_port_t port, uint8_t bits, uint8_t dir)
{
    uint8_t sel_reg = PMU_REG_PORTA_OEN;
    sel_reg += port;
    if(dir == GPIO_DIR_OUT)
        ool_write(sel_reg, (ool_read(sel_reg) & (~bits)));
    else
        ool_write(sel_reg, (ool_read(sel_reg) | bits));
}

/*********************************************************************
 * @fn      pmu_set_pin_pull
 *
 * @brief   set pull-up of IOs which are controlled by PMU.
 *          example usage:
 *          pmu_set_pin_pull(GPIO_PORT_A, (1<<GPIO_BIT_0) | (1<<GPIO_BIT_1), true)
 *
 * @param   port    - which group the io belongs to, @ref system_port_t
 *          bits    - the numbers of io
 *          flag    - true: enable pull-up, false: disable pull-up.
 *
 * @return  None.
 */
void pmu_set_pin_pull(enum system_port_t port, uint8_t bits, bool flag)
{
    uint8_t sel_reg = PMU_REG_PORTA_PUL;
    sel_reg += port;

    if(flag == false)
        ool_write(sel_reg, (ool_read(sel_reg) | bits));
    else
        ool_write(sel_reg, (ool_read(sel_reg) & (~bits)));
}

/*********************************************************************
 * @fn      pmu_set_gpio_value
 *
 * @brief   set value of IOs which are controlled by PMU and in GPIO mode.
 *          example usage:
 *          pmu_set_gpio_value(GPIO_PORT_A, (1<<GPIO_BIT_0) | (1<<GPIO_BIT_1), 1)
 *
 * @param   port    - which group the io belongs to, @ref system_port_t
 *          bits    - the numbers of io
 *          value   - 1: set the IO to high, 0: set the IO to low.
 *
 * @return  None.
 */
void pmu_set_gpio_value(enum system_port_t port, uint8_t bits, uint8_t value)
{
    uint8_t sel_reg = PMU_REG_GPIOA_V;
    sel_reg += port;
    if( value == 0 )
        ool_write(sel_reg, (ool_read(sel_reg) & (~bits)) );
    else
        ool_write(sel_reg, (ool_read(sel_reg) | bits ) );
}

void pmu_set_led2_value(uint8_t value)
{
    if( value == 0 )
    {
        ool_write(PMU_REG_LED_CTRL, ool_read(PMU_REG_LED_CTRL) & (~ (BIT(6)|BIT(2)) ) );  //set as output,clr bit6 bit2
        ool_write(PMU_REG_LED_PULL, ool_read(PMU_REG_LED_PULL) | BIT(6) );  //pull down
    }
    else
    {
        ool_write(PMU_REG_LED_CTRL, ool_read(PMU_REG_LED_CTRL) | BIT(6) );  //set as input      0x40
        ool_write(PMU_REG_LED_PULL, ool_read(PMU_REG_LED_PULL) & (~ BIT(6)) );  //pull up
    }
//another method
/*
    if( value == 0 )
    {
        ool_write(PMU_REG_LED_CTRL, 0x00);
    }
    else
    {
        ool_write(PMU_REG_LED_CTRL, 0x04 );
    }
*/
}
void pmu_set_led2_as_pwm(void)
{
    ool_write(PMU_REG_LED_PULL, ool_read(PMU_REG_LED_PULL) | BIT(2) );  
}
void pmu_set_led1_value(uint8_t value)
{
    if( value == 0 )
    {
        ool_write(PMU_REG_LED_CTRL, ool_read(PMU_REG_LED_CTRL) & (~ (BIT(5)|BIT(1)) ) );  //set as output,clr bit5 bit1
        ool_write(PMU_REG_LED_PULL, ool_read(PMU_REG_LED_PULL) | BIT(5) );  //pull down
    }
    else
    {
        ool_write(PMU_REG_LED_CTRL, ool_read(PMU_REG_LED_CTRL) | BIT(5) );  //set as input      0x40
        ool_write(PMU_REG_LED_PULL, ool_read(PMU_REG_LED_PULL) & (~ BIT(5)) );  //pull up
    }
}
void pmu_set_led1_as_pwm(void)
{
    ool_write(PMU_REG_LED_PULL, ool_read(PMU_REG_LED_PULL) | BIT(1) );  
}


/*********************************************************************
 * @fn      pmu_get_gpio_value
 *
 * @brief   get value of IO which are controlled by PMU and in GPIO mode.
 *          example usage:
 *          pmu_get_gpio_value(GPIO_PORT_A, GPIO_BIT_0)
 *
 * @param   port    - which group the io belongs to, @ref system_port_t
 *          bit     - the number of io
 *
 *
 * @return  1: the IO is high, 0: the IO is low..
 */
uint8_t pmu_get_gpio_value(enum system_port_t port, uint8_t bit)
{
    uint8_t sel_reg = PMU_REG_GPIOA_V;
    sel_reg += port;
    return ( (ool_read(sel_reg) & CO_BIT(bit))>>bit );
}
uint8_t pmu_get_gpio_port_value(enum system_port_t port)
{
    uint8_t sel_reg = PMU_REG_GPIOA_V;
    sel_reg += port;
    return  (ool_read(sel_reg) );
}

/*********************************************************************
 * @fn      pmu_set_sys_power_mode
 *
 * @brief   used to set system power supply mode, BUCK or LDO
 *
 * @param   mode    - indicate the mode to be used, @ref pmu_sys_pow_mode_t
 *
 * @return  None.
 */
void pmu_set_sys_power_mode(enum pmu_sys_pow_mode_t mode)
{
    if(mode == PMU_SYS_POW_LDO)
    {
        ool_write(PMU_REG_PWR_OPTION, PMU_PWR_SWITCH_BUCK_MODE_EN);
    }
    else
    {
        ool_write(PMU_REG_PWR_OPTION, PMU_PWR_SWITCH_BUCK_MODE_EN | PMU_PWR_BUCK_MODE); // set mode to buck
    }

    ool_write(PMU_REG_BUCK_CTRL0, ool_read(PMU_REG_BUCK_CTRL0) & (~(1<<3)));
}

/*********************************************************************
 * @fn      pmu_bg_trim
 *
 * @brief   do bg triming.
 *
 * @param   trim_value  - read from efuse bit[31:0]
 *
 * @return  None.
 */
void pmu_bg_trim(uint32_t trim_value)
{
    uint32_t data[5];
    uint16_t ref_internal = 0,ref_avdd = 0;
    uint8_t bg_set = 0;
    uint8_t correction_value = 0;
    uint16_t internal_adj, avdd_adj;

    if(trim_value != 0)
    {
        ref_internal = ((trim_value >> 12) & 0xfff);
        ref_avdd = (trim_value & 0xfff);
    }
    else
    {
        flash_OTP_read(0x1000, 5*sizeof(uint32_t), (void *)data);
        if(data[0] == 0x31303030) {
            ref_internal = ((400*1024)/(data[3] / 32) + (800*1024)/(data[4] / 32)) / 2;
            ref_avdd = ((2400*1024)/(data[1] / 32) + (800*1024)/(data[2] / 32)) / 2;
        }
    }

    if(ref_avdd >= 2925)
    {
        bg_set = ((ref_avdd*10-29000u)*100)/(125*29);
    }
    else if(ref_avdd < 2875)
    {
        bg_set = ((29000u-ref_avdd*10)*100)/(125*29);
    }
    else
    {
        bg_set = 0;       
    }
    correction_value = bg_set % 10;
    bg_set = bg_set / 10;
    if(correction_value >= 5u)
    {
        bg_set = bg_set+1;
    }

    internal_adj = 15 * bg_set;
    avdd_adj = 37 * bg_set;
   
    if(ref_avdd >= 2900)
    {
        ool_write(PMU_REG_BG_CTRL, (ool_read(PMU_REG_BG_CTRL)&0xF0)|(8-bg_set));
        ref_internal -= internal_adj;
        ref_avdd -= avdd_adj;
    }
    else
    {
        ool_write(PMU_REG_BG_CTRL, (ool_read(PMU_REG_BG_CTRL)&0xF0)|(8+bg_set));
        ref_internal += internal_adj;
        ref_avdd += avdd_adj;
    }

    adc_set_ref_voltage(ref_avdd, ref_internal);
}

/*********************************************************************
 * @fn      pmu_sub_init
 *
 * @brief   pmu init procedure, sleep control, etc.
 *
 * @param   None
 *
 * @return  None.
 */
void pmu_sub_init(void)
{
    /* check first power on */
    if(pmu_first_power_on(true))
    {
    }

    /* remove internal osc cap */
    ool_write(PMU_REG_OSC_CAP_CTRL, 0x00);

    /* remove the resistor load of BUCK */
    ool_write(PMU_REG_RL_CTRL, ool_read(PMU_REG_RL_CTRL) & (~(1<<6)));
    ool_write(PMU_REG_BUCK_CTRL0, ool_read(PMU_REG_BUCK_CTRL0) & (~(1<<3)));
    /* BIT3 ALDO bypass mode，这样底电流会小1uA，跟PB0连接到cs线有关 */
    //ool_write(PMU_REG_ADKEY_ALDO_CTRL, ool_read(PMU_REG_ADKEY_ALDO_CTRL) | (1<<3));

    /* change BUCK setting for better sensitivity performance */
    ool_write(PMU_REG_BUCK_CTRL0, 0x40);
    /* set BUCK voltage to min */
    ool_write(PMU_REG_BUCK_CTRL1, 0x25);

    /* set DLDO voltage to min */
    //ool_write(PMU_REG_DLDO_CTRL, 0x42);

    /* separate PKVDD and PKVDDH */
    ool_write(PMU_REG_ALDO_BG_CTRL, 0x06);
    ool_write(PMU_REG_ALDO_BG_CTRL, 0x0e);
    ool_write(PMU_REG_PKVDD_CTRL, 0x09 | 0xa4);
    ool_write(PMU_REG_PKVDD_CTRL, 0x49 | 0xa4);

    /* set PKVDDH to min */
    ool_write(PMU_REG_OTD_PKVDDH_CTRL, ool_read(PMU_REG_OTD_PKVDDH_CTRL) & 0xcf);

    #ifdef CFG_FT_CODE
    ool_write(PMU_REG_PKVDD_CTRL2, (ool_read(PMU_REG_PKVDD_CTRL2) & 0xF0) | 0x09);
    #else   // CFG_FT_CODE
    /* set PKVDD voltage to 0.85v */
    uint32_t data0, data1, data2;
    uint8_t dldo_v = 0;
    
    efuse_read(&data0, &data1, &data2);
    for(uint8_t i=8; i<12; i++) {
        dldo_v <<= 1;
        if(data0 & (1<<i)) {
            dldo_v |= 1;
        }
    }
    if(dldo_v != 0) {
        if(dldo_v/2 >= 4) {
            ool_write(PMU_REG_PKVDD_CTRL2, (ool_read(PMU_REG_PKVDD_CTRL2) & 0xF0) | 0x06);
        }
        else {
            ool_write(PMU_REG_PKVDD_CTRL2, (ool_read(PMU_REG_PKVDD_CTRL2) & 0xF0) | (0x0a-dldo_v/2));
        }
    }
    else {
        ool_write(PMU_REG_PKVDD_CTRL2, (ool_read(PMU_REG_PKVDD_CTRL2) & 0xF0) | 0x07);
    }
    /* set DLDO voltage to lower value */
    ool_write(PMU_REG_DLDO_CTRL, 0x62);
    #endif  // CFG_FT_CODE

    /*
        1. set PMU interrupt wake up pmu ctrl first
        2. enable gpio monitor module
     */
    ool_write(PMU_REG_BT_WKUP_CTRL, (ool_read(PMU_REG_BT_WKUP_CTRL) & (~(PMU_BT_WKUP_IRQ_EN|PMU_BT_WKUP_OSCMASK)))  | PMU_GPIO_MONITOR_EN);

    /*
        1. enable wdt reset pmu function
        2. release bt sleep timer reset
        3. enable external reset pin
     */

    ool_write(PMU_REG_RST_CTRL, (ool_read(PMU_REG_RST_CTRL) & (~(PMU_RST_WDT_EN|PMU_RST_EXT_PIN_EN))) | PMU_RST_SLP_TIMER);

    /* enable bt timer clock */
    ool_write(PMU_REG_CLK_CTRL, ool_read(PMU_REG_CLK_CTRL) | PMU_BT_TIMER_CLK_EN);

    /* enable interrupt wake up pmu ctrl */
    ool_write(PMU_REG_WAKEUP_SRC, PMU_WAKEUP_SRC_B0 | PMU_IRQ_WAKEUP_EN);

    ool_write(PMU_REG_BT_SLP_CTRL, PMU_BT_WKUP_PMU_EN | PMU_BT_CTRL_PMU_EN | PMU_BT_OSC_SLP_EN);

#if 0
    /* sleep and wakeup timing settings */
    ool_write(PMU_REG_WKUP_PWO_DLY, 0x40);
    ool_write(PMU_REG_WKUP_PMUFSM_CHG_DLY, 0x41);
    ool_write(PMU_REG_BT_TIMER_WU_IRQ_PROTECT, 0x42);
#endif

    ool_write(PMU_REG_BUCK_OFF_DLY, 0x00);
    ool_write(PMU_REG_GPIO_PDVDD_ON, 0x09);
    ool_write(PMU_REG_GPIO_PKVDDH_OFF, 0x0a);

    /* RAM and IO isolation control */
#ifndef KEEP_CACHE_SRAM_RETENTION
    ool_write(PMU_REG_MEM_ISO_EN_CTRL, 0xef);
    ool_write(PMU_REG_MEM_RET_CTRL, 0x3f);
#else
    ool_write(PMU_REG_MEM_ISO_EN_CTRL, 0x1f);
    ool_write(PMU_REG_MEM_RET_CTRL, 0x1f);
#endif
    ool_write(PMU_REG_ISO_CTRL, 0x02);

    /* debounce settings */
    pmu_set_debounce_clk(16);   // set debounce clock to 1kHz
    pmu_onkey_set_debounce_cnt(9);
    pmu_adkey_set_debounce_cnt(9);
    pmu_bat_full_set_debounce_cnt(10);
    pmu_lvd_set_debounce_cnt(2);
    pmu_chg_dec_set_debounce_cnt(10);
    ool_write(PMU_REG_POFWARN_CTRL, ool_read(PMU_REG_POFWARN_CTRL) | 0xf0);

#if 0
    /*
     * init PB0 used for internal flash cs control. CS pin is bonded with
     * PB0, use PB0 to keep cs pin in high level after system enter deep
     * sleep mode.
     */
    pmu_port_set_mux(GPIO_PORT_B, GPIO_BIT_0, PMU_PORT_MUX_GPIO);
    pmu_gpio_set_dir(GPIO_PORT_B, GPIO_BIT_0, GPIO_DIR_OUT);
    pmu_port_set_mux(GPIO_PORT_B, GPIO_BIT_1, PMU_PORT_MUX_GPIO);
    pmu_gpio_set_dir(GPIO_PORT_B, GPIO_BIT_1, GPIO_DIR_OUT);
    pmu_port_set_mux(GPIO_PORT_B, GPIO_BIT_2, PMU_PORT_MUX_GPIO);
    pmu_gpio_set_dir(GPIO_PORT_B, GPIO_BIT_2, GPIO_DIR_OUT);
    pmu_port_set_mux(GPIO_PORT_B, GPIO_BIT_3, PMU_PORT_MUX_GPIO);
    pmu_gpio_set_dir(GPIO_PORT_B, GPIO_BIT_3, GPIO_DIR_OUT);
    pmu_port_set_mux(GPIO_PORT_B, GPIO_BIT_4, PMU_PORT_MUX_GPIO);
    pmu_gpio_set_dir(GPIO_PORT_B, GPIO_BIT_4, GPIO_DIR_OUT);
    pmu_port_set_mux(GPIO_PORT_B, GPIO_BIT_5, PMU_PORT_MUX_GPIO);
    pmu_gpio_set_dir(GPIO_PORT_B, GPIO_BIT_5, GPIO_DIR_OUT);
    ool_write(PMU_REG_PORTB_PUL, ool_read(PMU_REG_PORTB_PUL) & 0xc0);
    ool_write(PMU_REG_GPIOB_V, ool_read(PMU_REG_GPIOB_V) | 0x01);
    ool_write(PMU_REG_GPIOB_V, ool_read(PMU_REG_GPIOB_V) & 0x3e);
    ool_write(PMU_REG_PORTB_SEL, ool_read(PMU_REG_PORTB_SEL) | 0x3f);
#endif

    /* LED1 is bounded with efuse power */
    ool_write(PMU_REG_LED_CTRL, 0x00);

    /* PD4~7如果被配置上拉，如果这个地方不配置，会导致漏电，跟SAR-ADC有关 */
    ool_write(PMU_REG_PWR_CTRL, ool_read(PMU_REG_PWR_CTRL) & 0xfc);

    /* Enable PMU SIGNAL diagport output, only for debug usage */
#if 0
#if 0
    ool_write(PMU_REG_PORTA_SEL, 0x0f);
    ool_write(PMU_REG_PORTA_OEN, 0x00);
    ool_write16(PMU_REG_PORTA_MUX_L, 0xAAAA);
#endif
#if 1
    ool_write(PMU_REG_PORTB_SEL, 0x0b);
    ool_write(PMU_REG_PORTB_OEN, 0x00);
    ool_write16(PMU_REG_PORTB_MUX_L, 0xAAAA);
#endif
#if 1
    ool_write(PMU_REG_PORTC_SEL, 0xf0);
    ool_write(PMU_REG_PORTC_OEN, 0x00);
    ool_write16(PMU_REG_PORTC_MUX_L, 0xAAAA);
#endif
#if 0
    ool_write(PMU_REG_PORTD_SEL, 0x00);
    ool_write(PMU_REG_PORTD_OEN, 0x00);
    ool_write16(PMU_REG_PORTD_MUX_L, 0xAAAA);
#endif

    ool_write(PMU_REG_DIAG_SEL, 0x42);
#endif

#ifndef CFG_FT_CODE
//    pmu_bg_trim(data2);
#endif
}

/*********************************************************************
 * @fn      pmu_enable_irq
 *
 * @brief   enable the irq of modules inside PMU.
 *
 * @param   irqs    -- indicate which irq should be enabled
 *
 * @return  None.
 */
void pmu_enable_irq(uint16_t irqs)
{
    if(irqs & 0xff)
    {
        pmu_enable_isr(irqs & 0xff);
    }
    if((irqs >> 8) & 0xff)
    {
        pmu_enable_isr2((irqs >> 8) & 0xff);
    }

    if(irqs & (PMU_ISR_BIT_ACOK|PMU_ISR_BIT_ACOFF))
    {
        ool_write(PMU_REG_ACOK_DEB_NEW_CFG, ool_read(PMU_REG_ACOK_DEB_NEW_CFG) | (PMU_ACOK_DEB_NEW_SEL));  // release debounce block
    }
    if(irqs & PMU_ISR_BIT_BAT)
    {
        ool_write(PMU_REG_LVD_BAT_DEB_CFG, ool_read(PMU_REG_LVD_BAT_DEB_CFG) | PMU_BAT_DEB_SEL);    // release debounce block
    }
    if(irqs & PMU_ISR_BIT_LVD)
    {
        ool_write(PMU_REG_LVD_BAT_DEB_CFG, ool_read(PMU_REG_LVD_BAT_DEB_CFG) | PMU_LVD_DEB_SEL);    // release debounce block
    }
}

/*********************************************************************
 * @fn      pmu_disable_irq
 *
 * @brief   disable the irq of modules inside PMU.
 *
 * @param   irqs    -- indicate which irq should be disabled
 *
 * @return  None.
 */
void pmu_disable_irq(uint16_t irqs)
{
    if(irqs & 0xff)
    {
        pmu_disable_isr(irqs & 0xff);
    }
    if((irqs >> 8) & 0xff)
    {
        pmu_disable_isr2((irqs >> 8) & 0xff);
    }
}

/*********************************************************************
 * @fn      pmu_clear_isr_state
 *
 * @brief   clear PMU interrupt.
 *
 * @param   state_map   - indicate which irq should be clear
 *
 * @return  None.
 */
__attribute__((section("ram_code"))) void pmu_clear_isr_state(uint16_t state_map)
{
    GLOBAL_INT_DISABLE();
    ool_write16(PMU_REG_ISR_CLR, state_map);
    co_delay_100us(1);
    //co_delay_10us(12);
    ool_write16(PMU_REG_ISR_CLR, 0);
    GLOBAL_INT_RESTORE();
}

/*********************************************************************
 * @fn      pmu_codec_power_enable
 *
 * @brief   enable codec power supply.
 *
 * @param   None.
 *
 * @return  None.
 */
void pmu_codec_power_enable(void)
{
    ool_write(PMU_REG_PWR_CTRL, ool_read(PMU_REG_PWR_CTRL) | 0x40);
}

/*********************************************************************
 * @fn      pmu_codec_power_disable
 *
 * @brief   remove codec power supply.
 *
 * @param   None.
 *
 * @return  None.
 */
void pmu_codec_power_disable(void)
{
    ool_write(PMU_REG_PWR_CTRL, ool_read(PMU_REG_PWR_CTRL) & 0xBF);
}

/*********************************************************************
 * @fn      pmu_set_io_voltage
 *
 * @brief   set the aldo output voltage, also known as IO voltage.
 *
 * @param   mode    - bypass to VBAT or not.
 *          value   - voltage target value.
 *
 * @return  None.
 */
void pmu_set_aldo_voltage(enum pmu_aldo_work_mode_t mode, enum pmu_aldo_voltage_t value)
{
    if(mode == PMU_ALDO_MODE_NORMAL)
    {
        ool_write(PMU_REG_ADKEY_ALDO_CTRL, (ool_read(PMU_REG_ADKEY_ALDO_CTRL) & (~(0x1f<<3))) | value);
    }
    else
    {
        ool_write(PMU_REG_ADKEY_ALDO_CTRL, ool_read(PMU_REG_ADKEY_ALDO_CTRL) | (1<<3));
    }
}

/*********************************************************************
 * @fn      pmu_set_lp_clk_src
 *
 * @brief   select low power clock source.
 *
 * @param   src - internal RC or external 32768, @ref pmu_lp_clk_src_t
 *
 * @return  None.
 */
void pmu_set_lp_clk_src(enum pmu_lp_clk_src_t src)
{
    extern uint8_t pmu_clk_src; // 0: internal RC, 1: external 32768
    if(src == PMU_LP_CLK_SRC_EX_32768)
    {
        ool_write(PMU_REG_OSC32_CTRL_0, ool_read(PMU_REG_OSC32_CTRL_0) & 0xbf);
        /* disalbe 32768 osc PD */
        ool_write(PMU_REG_OSC32K_OTD_CTRL, ool_read(PMU_REG_OSC32K_OTD_CTRL) & 0xfe);
        /* set clock source to external 32768 */
        ool_write(PMU_REG_CLK_CONFIG, (ool_read(PMU_REG_CLK_CONFIG) & (~PMU_SYS_CLK_SEL_MSK)) | 0x80);

        pmu_clk_src = 1;
    }
    else
    {
        /* set clock source to RC/2 */
        ool_write(PMU_REG_CLK_CONFIG, (ool_read(PMU_REG_CLK_CONFIG) & (~PMU_SYS_CLK_SEL_MSK)) | 0x40);
        /* enable 32768 osc PD */
        ool_write(PMU_REG_OSC32K_OTD_CTRL, ool_read(PMU_REG_OSC32K_OTD_CTRL) | 0x01);
        ool_write(PMU_REG_OSC32_CTRL_0, ool_read(PMU_REG_OSC32_CTRL_0) | 0x40);

        pmu_clk_src = 0;
    }
}
/*********************************************************************
 * @fn      pmu_enable_charge
 *
 * @brief   enable charge function,and set charge current & voltage
 *
 * @param   cur - charge current, @ref enum charge_current_t
 *          vol - charge terminal voltage, @ref enum charge_term_vol_t
 *          en - true = enable charge, false = disable charge
 *
 * @return  None.
 */
void pmu_enable_charge(enum charge_current_t cur,enum charge_term_vol_t vol,bool en)
{
    ool_write(0xA,0x15);
    ool_write(0xC,0x55);

    //BIT[5:0], charge current
    // 00'0000:29mA; 00'0010:40mA; 00 0100:72mA; 00 1000:113mA; 00 1100:152mA; 01 0000:185mA
    ool_write(0xB,(ool_read(0xB)&(~ 0x7F)) | (cur<<0) | (en<<6) );

    //BIT[2:0], charge termination voltage control
    // 000=4.1; 001=4.15; 010=4.2; 011=4.25; 100=4.3; 101=4.35; 110=4.4;
    ool_write(0xD,(ool_read(0xD)&(~ 0x7)) | (vol<<0) );
}

/*********************************************************************
 * @fn      pmu_port_wakeup_func_set
 *
 * @brief   indicate which ports should be checked by PMU GPIO monitor module.
 *          once the state of corresponding GPIO changes, an PMU interrupt
 *          will be generated.
 *
 * @param   gpios   - 32bit value, bit num corresponding to pin num.
 *                    sample: 0x08080808 means PA3, PB3, PC3, PD3 will be
 *                    checked.
 *
 * @return  None.
 */
void pmu_port_wakeup_func_set(uint32_t gpios)
{    
    uint8_t mux_regs_addr[] = {PMU_REG_PORTA_MUX_L, PMU_REG_PORTB_MUX_L, PMU_REG_PORTC_MUX_L, PMU_REG_PORTD_MUX_L};
    
    /* set pmu to handle gpio */
    ool_write32(PMU_REG_PORTA_SEL, ool_read32(PMU_REG_PORTA_SEL) & (~gpios));

    /* set gpio function mux */
    for(uint8_t j=0; j<4; j++)
    {
        uint8_t tmp_gpio;
        uint16_t current_mux;
        uint8_t mux_reg_addr = mux_regs_addr[j];
        tmp_gpio = (gpios>>(j*8)) & 0xff;
        if(tmp_gpio)
        {
            current_mux = ool_read16(mux_reg_addr);
            for(uint8_t i=0; i<8; i++)
            {
                if(tmp_gpio & (1<<i))
                {
                    current_mux &= (~(PMU_PORT_MUX_MSK<<(i*2)));
                    current_mux |= (PMU_PORT_MUX_GPIO<<(i*2));
                }
            }
            ool_write16(mux_reg_addr, current_mux);
        }
    }

    /* set gpio in input mode */
    ool_write32(PMU_REG_PORTA_OEN, ool_read32(PMU_REG_PORTA_OEN) | gpios);

    /* get current gpio value and initial gpio last value */
    ool_write32(PMU_REG_PORTA_LAST, ool_read32(PMU_REG_GPIOA_V));

    /* set gpio wakeup mask */
    ool_write32(PMU_REG_PORTA_TRIG_MASK, ool_read32(PMU_REG_PORTA_TRIG_MASK) | gpios);
}

extern void wdt_isr_ram(unsigned int* hardfault_args);
extern void qdec_isr_ram(void);
extern void rtc_isr_ram(uint8_t idx);
extern void keyscan_isr_ram(void);
extern void charge_isr_ram(uint8_t type);
extern void lvd_isr_ram(void);
extern void otd_isr_ram(void);
extern void onkey_isr_ram(void);
extern void pmu_gpio_isr_ram(void);


__attribute__((weak)) __attribute__((section("ram_code"))) void charge_isr_ram(uint8_t type)
{
    if(type == 2)
    {
        co_printf("charge full\r\n");
        pmu_disable_isr(PMU_ISR_BAT_EN);
    }
    else if(type == 1)
        co_printf("charge out\r\n");
    else if(type == 0)
    {
        pmu_enable_isr(PMU_ISR_BAT_EN);
        co_printf("charge in\r\n");
    }
}

__attribute__((weak)) __attribute__((section("ram_code"))) void lvd_isr_ram(void)
{
//    co_printf("lvd\r\n");
    pmu_disable_isr(PMU_ISR_LVD_EN);
}

__attribute__((weak)) __attribute__((section("ram_code"))) void otd_isr_ram(void)
{
    co_printf("otd\r\n");
    pmu_disable_isr(PMU_ISR_OTP_EN);
}
__attribute__((weak)) __attribute__((section("ram_code"))) void pmu_gpio_isr_ram(void)
{
    //co_printf("gpio new value = 0x%08x.\r\n", ool_read32(PMU_REG_GPIOA_V));
    //uart_putc_noint_no_wait(UART1, 'g');
    uint32_t gpio_value = ool_read32(PMU_REG_GPIOA_V);
    ool_write32(PMU_REG_PORTA_LAST, gpio_value);
}

__attribute__((weak)) __attribute__((section("ram_code"))) void onkey_isr_ram(void)
{
    if(ool_read(PMU_REG_ANA_RAW_STATUS) & PMU_ONKEY_RAW_STATUS)
    {
        co_printf("onkey high\r\n");
        pmu_disable_isr2(PMU_ISR_ONKEY_HIGH_EN);
        pmu_enable_isr2(PMU_ISR_ONKEY_LOW_EN);
    }
    else
    {
        co_printf("onkey low\r\n");
        pmu_disable_isr2(PMU_ISR_ONKEY_LOW_EN);
        pmu_enable_isr2(PMU_ISR_ONKEY_HIGH_EN);
    }
}

__attribute__((weak)) void adc_set_ref_voltage(uint16_t ref_avdd, uint16_t ref_internal)
{
}

__attribute__((section("ram_code"))) void pmu_isr_ram_C(unsigned int* hardfault_args)
{
    uint16_t clr_bits=0;
    uint16_t pmu_irq_st;

    pmu_irq_st = pmu_get_isr_state();

    if(pmu_irq_st & PMU_ISR_BAT_STATE)
    {
        clr_bits |= PMU_ISR_BAT_CLR;
        charge_isr_ram(2);
    }

    if(pmu_irq_st & PMU_ISR_POWER_OFF_STATE)
    {
        clr_bits |= PMU_ISR_POWER_OFF_CLR;
    }

    if(pmu_irq_st & PMU_ISR_LVD_STATE)
    {
        clr_bits |= PMU_ISR_LVD_CLR;
        lvd_isr_ram();
    }

    if(pmu_irq_st & PMU_ISR_OTD_STATE)
    {
        clr_bits |= PMU_ISR_OTD_CLR;
        otd_isr_ram();
    }

    if(pmu_irq_st & PMU_ISR_ACOK_STATE)
    {
        // ool_write(PMU_REG_ISR_ENABLE, (ool_read(PMU_REG_ISR_ENABLE)
        //                       & (~PMU_ISR_ACOK_EN))
        //  | PMU_ISR_ACOFF_EN);
        clr_bits |= PMU_ISR_ACOK_CLR;
        charge_isr_ram(0);
    }

    if(pmu_irq_st & PMU_ISR_CALI_STATE)
    {
        clr_bits |= PMU_ISR_CALI_CLR;
    }

    if(pmu_irq_st & PMU_ISR_ACOFF_STATE)
    {
        // ool_write(PMU_REG_ISR_ENABLE, (ool_read(PMU_REG_ISR_ENABLE)
        //                     & (~PMU_ISR_ACOFF_EN))
        // | PMU_ISR_ACOK_EN);
        clr_bits |= PMU_ISR_ACOFF_CLR;
        charge_isr_ram(1);
    }

    if(pmu_irq_st & PMU_ISR_KEYSCAN_STATE)
    {
        clr_bits |= PMU_ISR_KEYSCAN_CLR;
        keyscan_isr_ram();
    }

    if(pmu_irq_st & PMU_ISR_ALARM_A_STATE)
    {
        //co_printf("RTC ALARM A interrupt.\r\n");
        clr_bits |= PMU_ISR_RTC_ALARM_A_CLR;
        rtc_isr_ram(0);
    }
    if(pmu_irq_st & PMU_ISR_ALARM_B_STATE)
    {
        //co_printf("RTC ALARM A interrupt.\r\n");
        clr_bits |= PMU_ISR_RTC_ALARM_B_CLR;
        rtc_isr_ram(1);
    }
    if(pmu_irq_st & PMU_ISR_WDT_STATE)
    {
        //co_printf("RTC ALARM A interrupt.\r\n");
        clr_bits |= PMU_ISR_WDT_CLR;
        wdt_isr_ram(hardfault_args);
    }

    if(pmu_irq_st & PMU_ISR_ONKEY_STATE)
    {
        //co_printf("ONKEY pressed.\r\n");
        clr_bits |= PMU_ISR_ONKEY_CLR;
        onkey_isr_ram();
    }

    if(pmu_irq_st & PMU_ISR_GPIO_STATE)
    {
#if ENABLE_SYSTEM_PROTECTION_IN_LVD == 1
        if(ool_read(PMU_REG_GPIOC_V) & CO_BIT(4))
        {
            co_printf("lvd.\r\n");
            wdt_init(WDT_ACT_RST_CHIP,1);
            wdt_start();
            system_lvd_protect_handle();
            wdt_stop();
        }
#else
        uint8_t portc_value = ool_read(PMU_REG_GPIOC_V);
        if(portc_value & CO_BIT(4))
        {
            ool_write(PMU_REG_PORTC_LAST, portc_value);
        }
#endif
        clr_bits |= PMU_ISR_GPIO_CLR;
        pmu_gpio_isr_ram();
    }

    if(pmu_irq_st & PMU_ISR_QDEC_STATE)
    {
        //co_printf("%04x", ool_read16(PMU_REG_QDEC_CNTA_VALUE));
        clr_bits |= PMU_ISR_QDEC_CLR;
        qdec_isr_ram();
    }

    if(clr_bits)
        pmu_clear_isr_state(clr_bits);
}

