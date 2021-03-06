/*
 * emb6 is licensed under the 3-clause BSD license. This license gives everyone
 * the right to use and distribute the code, either in binary or source code
 * format, as long as the copyright license is retained in the source code.
 *
 * The emb6 is derived from the Contiki OS platform with the explicit approval
 * from Adam Dunkels. However, emb6 is made independent from the OS through the
 * removal of protothreads. In addition, APIs are made more flexible to gain
 * more adaptivity during run-time.
 *
 * The license text is:
 *
 * Copyright (c) 2015,
 * Hochschule Offenburg, University of Applied Sciences
 * Laboratory Embedded Systems and Communications Electronics.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
/*============================================================================*/

/**
 * @file    target.c
 * @author  PN
 * @brief   HAL implementation of TI MSP430x5xx
 */

#include "target.h"


/*
********************************************************************************
*                                  INCLUDES
********************************************************************************
*/

#include <msp430.h>

#include "targetconfig.h"
#include "target.h"

#include "bsp.h"
#include "io.h"
#include "int.h"
#include "lcd.h"
#include "mcu.h"
#include "led.h"
#include "key.h"
#include "spi.h"
#include "tmr.h"
#include "uart.h"
#include "rtc.h"
#include "infoflash.h"
#include "rt_tmr.h"


/*
********************************************************************************
*                               LOCAL DEFINES
********************************************************************************
*/
#define TARGET_CFG_ARG_CHK_EN                   ( 1u )

#define TARGET_CFG_SYSTICK_RESOLUTION           (clock_time_t)( 1000u )
#define TARGET_CFG_SYSTICK_SCALER               (clock_time_t)(    2u )


/*
********************************************************************************
*                               LOCAL DATATYPE
********************************************************************************
*/
typedef uint8_t     spiDesc_t;

/*
********************************************************************************
*                               LOCAL VARIABLES
********************************************************************************
*/
static s_io_pin_desc_t s_target_extIntPin[] =
{
    {&gps_io_port[TARGETCONFIG_RF_GPIO0_PORT], TARGETCONFIG_RF_GPIO0_PIN, TARGETCONFIG_RF_GPIO0_MSK},
    {&gps_io_port[TARGETCONFIG_RF_GPIO2_PORT], TARGETCONFIG_RF_GPIO2_PIN, TARGETCONFIG_RF_GPIO2_MSK},
    {&gps_io_port[TARGETCONFIG_RF_GPIO3_PORT], TARGETCONFIG_RF_GPIO3_PIN, TARGETCONFIG_RF_GPIO3_MSK},

    /* todo added UART interrupt */
};

static clock_time_t volatile    hal_ticks;
static spiDesc_t                hal_rfspi;
static pfn_intCallb_t           hal_uartCb;

/*
********************************************************************************
*                           LOCAL FUNCTION DEFINITIONS
********************************************************************************
*/
static void _hal_systick(void);
static void _hal_isrSysTick( void *p_arg );
static void _hal_isrUartCb(uint8_t c);

/*
********************************************************************************
*                           LOCAL FUNCTION DEFINITIONS
********************************************************************************
*/
static void _hal_isrSysTick( void *p_arg )
{
    hal_ticks++;
    if ((hal_ticks % TARGET_CFG_SYSTICK_SCALER) == 0) {
        rt_tmr_update();
    }
}

static void _hal_isrUartCb(uint8_t c)
{
    if (hal_uartCb) {
        hal_uartCb(&c);
    }
}

static void _hal_systick(void)
{
    uint16_t period;

    period = (uint16_t)(TARGET_CFG_SYSTICK_RESOLUTION /
                        TARGET_CFG_SYSTICK_SCALER);
    tmr_config(E_TMR_0, period);
    tmr_start(E_TMR_0, _hal_isrSysTick );
}

static void _hal_uartInit(void)
{
    uart_init();
    uart_config(E_UART_SEL_UART1, 115200, _hal_isrUartCb);
}


/*
********************************************************************************
*                           GLOBAL FUNCTION DEFINITIONS
********************************************************************************
*/

#ifndef GCC_COMPILER
int putchar(int c)
{
    uart_send(E_UART_SEL_UART1, (char *)&c, 1);
    return c;
}
#endif /* #ifndef GCC_COMPILER */


/*
********************************************************************************
*                            API FUNCTION DEFINITIONS
********************************************************************************
*/
void hal_enterCritical(void)
{
    __disable_interrupt();
}


void hal_exitCritical(void)
{
    __enable_interrupt();
}


/**
 *  \brief      This function should initialize all of the MCU peripherals
 *  \return     status
 */
int8_t hal_init(void)
{
    int i = 0;


    hal_enterCritical();
    {
        /*
         * Initialize peripherals: UART, SPI, LED,...
         * As BSP implementation is common for all platform at the time of writing,
         * platform-specific modules shall be initialized in this function
         */

        /* initialize IO */
        io_init();

        /* initialize system clock */
        mcu_sysClockInit( MCU_SYSCLK_25MHZ );

        /* initialize interrupts */
        int_init();

        /* initialize timer */
        tmr_init();

        /* initialize LEDs */
        led_init();

        /* initialize keys */
        key_init();

        /* initialize RF SPI */
#if (TARGET_CONFIG_RF == TRUE)
        spi_rfInit(4);
#endif

        /* initialize LCD */
#if (TARGET_CONFIG_LCD == TRUE)
        spi_lcdInit(MCU_SYSCLK_8MHZ);
        lcd_init();
#endif

        /* initialize UART */
        _hal_uartInit();
        hal_uartCb = 0;

        /* initialize info flash */
        infoflash_init();

        /* initialize RTC */
        rtc_init();

        /* Initialize System tick timer */
        _hal_systick();

        /* Enable global interrupt */
        _BIS_SR(GIE);

        /* delay */
        for( i = 0; i < TARGET_CONFIG_INIT_DELAY_NUM; i++ )
            __delay_cycles( TARGET_CONFIG_INIT_DELAY_CYCLES );
    }
    hal_exitCritical();

    /*
     * Initialize HAL local variables
     */
    hal_rfspi = 0;
    hal_ticks = 0;
    return 1;
}


uint8_t hal_getrand(void)
{
    /*TODO missing implementation */
    return (uint8_t)hal_ticks;
}


void hal_ledOn(uint16_t ui_led)
{
    switch (ui_led) {
#if TARGET_CONFIG_LED1
        case E_BSP_LED_0:
            led_set(E_LED_1);
            break;
#endif
#if TARGET_CONFIG_LED2
        case E_BSP_LED_1:
            led_set(E_LED_2);
            break;
#endif
#if TARGET_CONFIG_LED3
        case E_BSP_LED_2:
            led_set(E_LED_3);
            break;
#endif
#if TARGET_CONFIG_LED4
        case E_BSP_LED_4:
            led_set(E_LED_4);
            break;
#endif
        default:
            break;
    }
}


void hal_ledOff(uint16_t ui_led)
{
    switch (ui_led) {
#if TARGET_CONFIG_LED1
        case E_BSP_LED_0:
            led_clear(E_LED_1);
            break;
#endif
#if TARGET_CONFIG_LED2
        case E_BSP_LED_1:
            led_clear(E_LED_2);
            break;
#endif
#if TARGET_CONFIG_LED3
        case E_BSP_LED_2:
            led_clear(E_LED_3);
            break;
#endif
#if TARGET_CONFIG_LED4
        case E_BSP_LED_4:
            led_clear(E_LED_4);
            break;
#endif
        default:
            break;
    }
}


void hal_extiRegister(en_targetExtInt_t     e_pin,
                      en_targetIntEdge_t    e_edge,
                      pfn_intCallb_t        pf_cb)
{
    uint8_t int_edge;


#if NETSTK_CFG_ARG_CHK_EN
    if (pf_cb == NULL) {
        return;
    }
#endif

    /* register external interrupts */
    switch (e_pin) {
        case E_TARGET_EXT_INT_0:
        case E_TARGET_EXT_INT_1:
        case E_TARGET_EXT_INT_2:
            /* get corresponding edge configuration */
            if (e_edge == E_TARGET_INT_EDGE_FALLING) {
                int_edge = INT_EDGE_FALLING;
            } else {
                int_edge = INT_EDGE_RISING;
            }

            /* supported external interrupts are treated evenly */
            io_extiRegister(&s_target_extIntPin[e_pin], int_edge, pf_cb);
            break;

        case E_TARGET_USART_INT:
            /* register callback function */
            hal_uartCb = pf_cb;
            break;

        case E_TARGET_RADIO_INT:
        case E_TARGET_EXT_INT_3:
        case E_TARGET_EXT_INT_MAX:
        default:
            /* unsupported external interrupts */
            break;
    }
}

void hal_extiClear(en_targetExtInt_t e_pin)
{
    switch (e_pin) {
        case E_TARGET_EXT_INT_0:
        case E_TARGET_EXT_INT_1:
        case E_TARGET_EXT_INT_2:
            io_extiClear(&s_target_extIntPin[e_pin]);
            break;

        case E_TARGET_USART_INT:
            /* register callback function */
            hal_uartCb = 0;
            break;

        case E_TARGET_RADIO_INT:
        case E_TARGET_EXT_INT_3:
        case E_TARGET_EXT_INT_MAX:
        default:
            /* unsupported external interrupts */
            break;
    }
}

void hal_extiEnable(en_targetExtInt_t e_pin)
{
    switch (e_pin) {
        case E_TARGET_EXT_INT_0:
        case E_TARGET_EXT_INT_1:
        case E_TARGET_EXT_INT_2:
            io_extiEnable(&s_target_extIntPin[e_pin]);
            break;

        case E_TARGET_USART_INT:
            break;

        case E_TARGET_RADIO_INT:
        case E_TARGET_EXT_INT_3:
        case E_TARGET_EXT_INT_MAX:
        default:
            /* unsupported external interrupts */
            break;
    }
}

void hal_extiDisable(en_targetExtInt_t e_pin)
{
    switch (e_pin) {
        case E_TARGET_EXT_INT_0:
        case E_TARGET_EXT_INT_1:
        case E_TARGET_EXT_INT_2:
            io_extiDisable(&s_target_extIntPin[e_pin]);
            break;

        case E_TARGET_USART_INT:
            break;

        case E_TARGET_RADIO_INT:
        case E_TARGET_EXT_INT_3:
        case E_TARGET_EXT_INT_MAX:
        default:
            /* unsupported external interrupts */
            break;
    }
}


void hal_delay_us(uint32_t ul_delay)
{
    /*
     * Note(s)
     *
     * hal_delay_us() is only called by emb6.c to make a delay multiple of 500us,
     * which is equivalent to 1 systick
     */
    uint32_t tick_stop;


    hal_enterCritical();
    tick_stop  = hal_ticks;
    tick_stop += ul_delay / 500;
    hal_exitCritical();
    while (tick_stop > hal_ticks) {
        /* do nothing */
    }
}


uint8_t hal_gpioPinInit(uint8_t c_pin, uint8_t c_dir, uint8_t c_initState)
{
    /*TODO missing implementation */
    return 0;
}


void * hal_ctrlPinInit(en_targetExtPin_t e_pinType)
{
    /*TODO missing implementation */
    return NULL;
}


void hal_pinSet(void * p_pin)
{
    /*TODO missing implementation */
}


void hal_pinClr(void * p_pin)
{
    /*TODO missing implementation */
}


uint8_t hal_pinGet(void * p_pin)
{
    return 0;
}


void *hal_spiInit(void)
{
#if (TARGET_CONFIG_RF == TRUE)
    return &hal_rfspi;
#else
    return NULL;
#endif
}


/**
 * @brief   Select/Deselect a SPI driver
 * @param   p_spi
 * @param   action
 * @return
 */
uint8_t hal_spiSlaveSel(void *p_spi, bool action)
{
#if TARGET_CFG_ARG_CHK_EN
  if (p_spi == NULL) {
    return 0;
  }
#endif

  if (action == TRUE) {
    spi_rfSelect();
  } else {
    spi_rfDeselect();
  }
  return 1;
}

void hal_spiTxRx(uint8_t *p_tx, uint8_t *p_rx, uint16_t len)
{
    spi_rfTxRx(p_tx, p_rx, len);
}

/**
 * @brief   Read a given number of data from a SPI bus
 * @param   p_data  Point to buffer holding data to receive
 * @param   i_len   Length of data to receive
 * @return
 */
uint8_t hal_spiRead(uint8_t *p_data, uint16_t i_len)
{
    uint8_t ret;


#if TARGET_CFG_ARG_CHK_EN
    if ((p_data == NULL) ||
        (i_len == 0)) {
        return 0;
    }
#endif

    ret = spi_rfRead(p_data, i_len);
    return ret;
}


/**
 * @brief   Write a given number of bytes onto a SPI bus
 * @param   p_data  Point to data to send
 * @param   i_len   Length of data to send
 */
void hal_spiWrite(uint8_t *p_data, uint16_t i_len)
{
#if TARGET_CFG_ARG_CHK_EN
    if ((p_data == NULL) ||
        (i_len == 0)) {
        return;
    }
#endif

    spi_rfWrite(p_data, i_len);
}


void hal_watchdogReset(void)
{
    /*TODO missing implementation */
}


void hal_watchdogStart(void)
{
    /*TODO missing implementation */
}


void hal_watchdogStop(void)
{
    /*TODO missing implementation */
}


clock_time_t hal_getTick(void)
{
    return TmrCurTick;
}


clock_time_t hal_getSec(void)
{
    clock_time_t secs = 0;

    secs = TmrCurTick / TARGET_CFG_SYSTICK_RESOLUTION;
    return secs;
}


clock_time_t hal_getTRes(void)
{
    return TARGET_CFG_SYSTICK_RESOLUTION;
}
