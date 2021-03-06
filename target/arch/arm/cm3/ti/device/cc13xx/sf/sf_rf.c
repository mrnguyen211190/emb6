/**
* @code
*  ___ _____ _   ___ _  _____ ___  ___  ___ ___
* / __|_   _/_\ / __| |/ / __/ _ \| _ \/ __| __|
* \__ \ | |/ _ \ (__| ' <| _| (_) |   / (__| _|
* |___/ |_/_/ \_\___|_|\_\_| \___/|_|_\\___|___|
* embedded.connectivity.solutions.==============
* @endcode
*
* @file       sf_rf.c
* @copyright  STACKFORCE GmbH, Heitersheim, Germany, http://www.stackforce.de
* @author     STACKFORCE
* @brief      Implementation of general rf function for the CC13xx rf module.
*/
#ifdef __cplusplus
extern "C" {
#endif

#define __DECL_SF_RF_API_H__
#define __DECL_SF_RF_H__

#include "hwinit.h"

#ifndef USE_TI_RTOS
#error Please define if TI-RTOS is in use ore not
#endif /* USE_TI_RTOS */

/*==============================================================================
                            INCLUDE FILES
==============================================================================*/
/* Standart libraries */
#include <stdint.h>
#include <stdbool.h>

/**************************** CC13xx includes *********************************/
/* RF dataqueue headers */
#include "rflib/rf_cc1350.h"
#include "rflib/rf_dbell.h"
#include "rflib/rf_queue.h"
#include "driverlib/rf_mailbox.h"

#if USE_TI_RTOS
#include <ti/drivers/rf/RF.h>
#include "rfLib/patch/apply_wmbus_tmode_mce_patch.h"
#else
/* BoardSupportPacket */
#include "bsp/srf06eb_cc26xx/drivers/source/bsp.h"
#endif /* USE_TI_RTOS */
#include "sf_rf.h"
/*==============================================================================
                            DEFINES
==============================================================================*/
#ifndef CC13XX_TX_ENABLED
#error Please define if the device support TX
#endif /* CC13XX_TX_ENABLED */

#ifndef CC13XX_RX_ENABLED
#error Please define if the device support RTX
#endif /* CC13XX_RX_ENABLED */

#ifndef CC13XX_IS_POLLED_RADIO
#error Please define if the radio will be polled by the run function ore not
#endif /* CC13XX_IS_POLLED_RADIO */


#ifndef CC13XX_MAX_TELEGRAM_LEN
#error Please define the maximum length needed to send or receive one telegram
#else
/* For some strange reasons the rf needs 3 additional bytes to receice telegrams */
#define RF_RX_ARRAY_SIZE   (CC13XX_MAX_TELEGRAM_LEN + 3U)
#endif /* CC13XX_MAX_TELEGRAM_LEN */



/*********************** START SF SPECIFIC MACROS *****************************/
#ifndef SF_INLINE
#define SF_INLINE
#endif /* SF_INLINE */

#ifndef MEMCPY
/*!
 @brief Copies a byte array from one to another memory.
 @param x   Destination memory.
 @param y   Source memory.
 @param z   Number of bytes to copy.
 */
#define MEMCPY(x,y,z)                 memcpy((x), (y), (z))
#endif /* MEMCPY */

#ifndef MEMSET
/*!
  @brief Writes bytes into a specific memory.
  @param x   Destination memory to write to.
  @param y   Byte to write into the destination memory.
  @param z   Number of bytes to write.
 */
#define MEMSET(x,y,z)                 memset((x), (y), (z))
#endif /* MEMSET */
/************************* END SF SPECIFIC MACROS *****************************/

#define DATA_ENTRY_HEADER_SIZE 8  /* Constant header size of a Generic Data Entry */
#define NUM_DATA_ENTRIES       1  /* NOTE: Only two data entries supported at the moment */
#define NUM_APPENDED_BYTES     2  /* The Data Entries data field will contain:
                                   * - the length indicator (if RF_cmdPropRx.rxConf.bIncludeHdr = 0)
                                   *   - Not included
                                   * - the payload
                                   *   - Included by default, does not affect appended bytes
                                   * - CRC (if RF_cmdPropRx.rxConf.bIncludeCrc = 1)
                                   *   - NOT included
                                   * - RSSI (if RF_cmdPropRx.rxConf.bAppendRssi = 1)
                                   *   - Included
                                   * - Timestamp (if RF_cmdPropRx.rxConf.bAppendTimestamp = 1)
                                   *   - NOT included
                                   * - Status (if RF_cmdPropRx.rxConf.bAppendStatus = 1
                                   *   - Included
                                   *--------------------------------------------
                                   * Total default appended bytes: 2 */

/* Threshold value for the rx process. */
#define RF_RX_THRESHOLD        0x05U

/* Defines the default TX-power of the device: Here 0dBm is used */
#define RF_DEFAULT_TX_POWER    130U
/*==============================================================================
                            TYPEDEF ENUMS
==============================================================================*/
/*==============================================================================
                            TYPEDEF ENUMS
==============================================================================*/
/*==============================================================================
                            TYPEDEF STRUCTS
==============================================================================*/

 /** TX data ctx of the rf module. */
 typedef struct S_RF_TX_CTX_T
 {
   /** Size of the hole telegram. */
   uint16_t i_telegramLength;
   /** Number of bytes in data array */
   uint16_t i_fillLevel;
   /** Len of the last transmitted block */
   uint16_t i_lastTxLen;
  /** Pointer to the current tx command */
  rfc_CMD_PROP_TX_ADV_t* ps_cmdPropTxAdv;
 } s_rf_txCtx_t;

/** RX data ctx of the rf module. */
typedef struct S_RF_RX_CTX_T
{
  /**  Quality byte. */
  uint8_t *pc_quality;
  /** Size of the quality memory. */
  uint8_t c_lenQuality;
  /** Total number of received byte */
  uint16_t i_rxFillLevel;
  /** Number of readed bytes by higher layer */
  uint16_t i_numberOfReadedByte;
  /** Number of bytes requested by higher layer  */
  uint16_t i_numberOfRequestedByte;
  /** Pointer to the buffer of the higher layer */
  uint8_t *pc_dataPointer;
  /** Receive dataQueue for RF Core to fill in data */
  dataQueue_t s_dataQueue;
  /** Pointer to the current data entry */
  rfc_dataEntryPartial_t* ps_currentDataEntry;
  /** The RX Output struct contains statistics about the RX operation of the radio. */
  rfc_propRxOutput_t s_rxStatistics;
  /** Pointer to the current rx command */
  rfc_CMD_PROP_RX_ADV_t* ps_cmdPropRxAdv;
  /* Meassured RSSI value */
  uint8_t c_rssiValue;
} s_rf_rxCtx_t;

/*==============================================================================
                            VARIABLES
==============================================================================*/

/** Variable to store the PA power during shutdown (only register lost) */
uint8_t gc_signalStrength;

/* Static variable to store the current radio channel configuration */
rfc_radioOp_t* gps_cmdFs = NULL;

 /** Postamble to use for the current Tx packet */
uint8_t gc_rfPostamble;
 /** Size of Postamble to use for the current Tx packet */
uint8_t gc_rfPostambleSize;

/** Status of the RF module. */
volatile E_RF_STATUS_t ge_rfStatus;

/** Global interrupt flag. Represents the last occured interrupt. */
uint32_t gl_interruptFlag = 0x00U;

#if defined(__TI_COMPILER_VERSION__)
	#pragma DATA_ALIGN (gac_data, 4);
#elif defined(__IAR_SYSTEMS_ICC__)
	#pragma data_alignment = 4
#endif
/** Storage of the telegram payload for RX/TX */
static uint8_t gac_data[RF_QUEUE_DATA_ENTRY_BUFFER_SIZE(NUM_DATA_ENTRIES,
                                        RF_RX_ARRAY_SIZE,
                                        NUM_APPENDED_BYTES)];

/* Pointer to the command "cmdPropRadioDivSetup" */
rfc_radioOp_t* gps_cmdPropRadioDivSetup;
/************************* RX specific variabled ******************************/
#if CC13XX_RX_ENABLED
/** Global call back function pointer for rx events */
static fp_rf_evt_rx gfp_rx = NULL;
/** Global call back to get the rx config  */
static fp_rf_evt_configRx gfp_confRx = NULL;
/** Context for RX opperations*/
s_rf_rxCtx_t gs_rf_rxCtx;

#if USE_TI_RTOS
/* handle of the rx command ==> needed for aborting the rx command */
RF_CmdHandle gsi_rx_cmd;
#endif /* USE_TI_RTOS */

#endif /* CC13XX_RX_ENABLED */

/************************* TX specific variabled ******************************/
#if CC13XX_TX_ENABLED
 /** Global call back function pointer for tx events */
static fp_rf_evt_tx gfp_tx = NULL;
/** Context for TX opperations*/
s_rf_txCtx_t gs_rf_txCtx;
#endif /* CC13XX_TX_ENABLED */

/* TI RTOS variables */
#if USE_TI_RTOS
/* RF parameter struct */
static RF_Params gs_rfParams;
static RF_Object gs_rfObject;
static RF_Handle gps_rfHandle;
/* TI-RTOS RF Mode Object */
RF_Mode RF_prop =
{
    .rfMode = RF_MODE_PROPRIETARY,
    .cpePatchFxn = &enterWmbusTmodeMcePatch,
    .mcePatchFxn = 0,
    .rfePatchFxn = 0,
};
#endif /* USE_TI_RTOS */

/*==============================================================================
                            FUNCTION PROTOTYPES
==============================================================================*/

bool loc_reset(bool b_isrEnabled, E_RF_MODE_t e_mode);
void loc_rxIsr(uint32_t l_flag);

#if USE_TI_RTOS
static void loc_txIsr(RF_Handle h, RF_CmdHandle ch, RF_Event e);
static void loc_rfClose(void);
static void loc_rfOpen(void);
static RF_CmdHandle loc_rfPostCmd(RF_Op* pOp, RF_Priority ePri, RF_Callback pCb);
#else
void loc_txIsr(uint32_t l_flag);
static void loc_switchToHfXtalOsc(void);
#endif /* USE_TI_RTOS */

/** Configuration helper functions. @{ */
#if CC13XX_RX_ENABLED
static SF_INLINE bool loc_startRx(void);
#endif /* CC13XX_RX_ENABLED */


/*==============================================================================
                         LOCAL FUNCTIONS
==============================================================================*/
#if !USE_TI_RTOS
/*******************************************************************************
 * @brief   Switch to use HF XTAL
 *
 * @param   None
 *
 * @return  None
*******************************************************************************/
static void loc_switchToHfXtalOsc(void)
{
    /* Enable OSC DIG interface to change clock sources */
    OSCInterfaceEnable();

    /* Check to not enable XOSC if already enabled */
    if(OSCClockSourceGet(OSC_SRC_CLK_HF) != OSC_XOSC_HF)
    {
        /* Disable autostart of XOSC state machine */
        DDI32BitsSet(AUX_DDI0_OSC_BASE,
                     DDI_0_OSC_O_AMPCOMPCTL,
                     DDI_0_OSC_AMPCOMPCTL_AMPCOMP_SW_CTRL);

        /* Request to switch to the crystal to enable radio operation. */
        OSCClockSourceSet(OSC_SRC_CLK_MF | OSC_SRC_CLK_HF, OSC_XOSC_HF);

        /* Wait for a while */
        CPUdelay(48000000/3*0.002);

        /* Enable autostart, will start the state machine */
        DDI32BitsClear(AUX_DDI0_OSC_BASE,
                       DDI_0_OSC_O_AMPCOMPCTL,
                       DDI_0_OSC_AMPCOMPCTL_AMPCOMP_SW_CTRL);

        /* Switch the HF source - Done via ROM API*/
        OSCHfSourceSwitch();
    }

    /* Disable OSC DIG interface */
    OSCInterfaceDisable();
}/* loc_switchToHfXtalOsc() */
#endif /* !USE_TI_RTOS */

#if USE_TI_RTOS
/*******************************************************************************
 * @brief   Close the rf handle. The transceiver is allowed to go to sleep now
 *
 * @param   None
 *
 * @return  None
*******************************************************************************/
static void loc_rfClose(void)
{
  if(gps_rfHandle != NULL)
  {
    RF_close(gps_rfHandle);
    gps_rfHandle= NULL;
  }
  else
  {
    while(1);
    #warning remove debugging while loop
  }/* if...else */
}/* loc_rfClose() */
#endif /* USE_TI_RTOS */

#if USE_TI_RTOS
/*******************************************************************************
 * @brief   Open the rf handle. The transceiver is not allowed to go to sleep now
 *
 * @param   None
 *
 * @return  None
*******************************************************************************/
static void loc_rfOpen(void)
{
  if(gps_rfHandle == NULL)
  {
    gps_rfHandle = RF_open(&gs_rfObject, &RF_prop, (RF_RadioSetup*)gps_cmdPropRadioDivSetup, &gs_rfParams);
    if(gps_rfHandle == NULL)
    {
      while(1);
#warning remove debugging while loop
    }
  }
  else
  {
    while(1);
#warning remove debugging while loop
  }/* if ... else */
}/* loc_rfOpen() */
#endif /* USE_TI_RTOS */

#if USE_TI_RTOS
/*******************************************************************************
 * @brief   Send a command to the rf module
 *
 *  @param pOp   Pointer to the #RF_Op. Must normally be in persistent and writeable memory
 *  @param ePri  Priority of this RF command (used for arbitration in multi-client systems)
 *  @param pCb   Callback function called upon command completion (and some other events).
 *               If RF_postCmd() fails no callback is made
 *  @return      A handle to the RF command. Negative value indicates an error
*******************************************************************************/
static RF_CmdHandle loc_rfPostCmd(RF_Op* pOp, RF_Priority ePri, RF_Callback pCb)
{
  RF_CmdHandle cmdHandle = -5;
  if (NULL != gps_rfHandle)
  {
      cmdHandle = RF_postCmd(gps_rfHandle, pOp, ePri, pCb);
  }
  else
  {
    while(1);
#warning remove debugging while loop
  }
  return cmdHandle;
}/* loc_rfPostCmd() */
#endif /* USE_TI_RTOS */

/*============================================================================*/
/**
 * @brief  Resets RX mode.
 * @param b_irEnable  Set to enable the interrupts.
 * @param e_mode      Mode which should be used after the reset is completed.
 * @return            Returns @ref true, if successfully, @ref false otherwise.
 */
/*============================================================================*/
bool loc_reset(bool b_isrEnabled, E_RF_MODE_t e_mode)
{
  /* Return value of this function */
  bool b_return = false;

  /* Check if the transceiver is already in the needed state */
  if((b_isrEnabled == true) &&
     (e_mode == E_RF_MODE_RUN) &&
     (ge_rfStatus == E_RF_STATUS_RX_LISTEN))
  {
    b_return = true;
  }/* if */
  else
  {
    /* Disable interrupt */
    RFC_registerCpe0Isr(NULL);
    sf_rf_setPowerMode(E_RF_POWERMODE_IDLE);

    #if CC13XX_RX_ENABLED
    /* Check if the device has to be configurated for receive mode */
    if(E_RF_MODE_RUN == e_mode)
    {
      sf_rf_setConfigRx();
    }/* if */
    #endif /* CC13XX_RX_ENABLED */

    if(b_isrEnabled && (E_RF_MODE_RUN == e_mode))
    {
      #if CC13XX_RX_ENABLED
      b_return = loc_startRx();
      #endif /* CC13XX_RX_ENABLED */
    }/* if */
    else if((false == b_isrEnabled) && (E_RF_MODE_WAIT == e_mode))
    {
      #if USE_TI_RTOS
      ge_rfStatus = E_RF_STATUS_SLEEP;
      gps_cmdFs = NULL;
      b_return = true;
      loc_rfClose();
      #else
      /* Set transceiver to lpm */
      if (RFC_disableRadio() == RFC_OK)
      {
        ge_rfStatus = E_RF_STATUS_SLEEP;
        gps_cmdFs = NULL;
        b_return = true;
      }/* if */
      #endif /* USE_TI_RTOS */
    } /* if ... else */
  }/* if..else */
  return b_return;
} /* loc_reset() */

#if CC13XX_RX_ENABLED
/*============================================================================*/
/**
 * @brief  Prepare and start RX mode.
 * @return true if the receiving was started successfully
 */
/*============================================================================*/
static SF_INLINE bool loc_startRx(void)
{
  /* Return value of this function */
  bool b_return = true;

  if((ge_rfStatus != E_RF_STATUS_RX_LISTEN) &&
     (NULL != gs_rf_rxCtx.ps_cmdPropRxAdv))
  {
    /****** STEP 4 - Prepare Data Entries **************************************
     * Create a Data Queue with one Data Entry
     **************************************************************************/
    if(RFQueue_defineQueue(&gs_rf_rxCtx.s_dataQueue, gac_data, sizeof(gac_data),
                       NUM_DATA_ENTRIES, RF_RX_ARRAY_SIZE) == 0x00U)
    {
      /* Set the length for the  "Rx_Data_Written" interrupt */
      gs_rf_rxCtx.ps_currentDataEntry = (rfc_dataEntryPartial_t*)gs_rf_rxCtx.s_dataQueue.pCurrEntry;
      /* Define the data entry as partial rx entry */
      gs_rf_rxCtx.ps_currentDataEntry->config.type = DATA_ENTRY_TYPE_PARTIAL;
      /* For partial read Rx entry only: The number of bytes between interrupt generated */
      gs_rf_rxCtx.ps_currentDataEntry->config.irqIntv = RF_RX_THRESHOLD;
      /* Reset the index parameter */
      gs_rf_rxCtx.ps_currentDataEntry->nextIndex = 0x00U;

      /****** STEP 5 - RX ********************************************************
       * 1. Modify CMD_PROP_RX command for application needs
       * 2  Setup interrupt on IRQ_RX_N_DATA_WRITTEN
       * 3. Send CMD_PROP_RX command using non-blocking RadioLib call
       **************************************************************************/

      /* 5.1. Modify CMD_PROP_RX command for application needs */
      /* Set the Data Entity queue for received data */
      gs_rf_rxCtx.ps_cmdPropRxAdv->pQueue = &gs_rf_rxCtx.s_dataQueue;
      /* Set rx statistics output struct */
      gs_rf_rxCtx.ps_cmdPropRxAdv->pOutput = (uint8_t*)&gs_rf_rxCtx.s_rxStatistics;

      /* 5.2. Setup interrupt on IRQ_RX_N_DATA_WRITTEN */
      RFC_registerCpe0Isr(loc_rxIsr);
      RFC_enableCpe0Interrupt(IRQ_RX_N_DATA_WRITTEN);

      ge_rfStatus = E_RF_STATUS_RX_LISTEN;
      gs_rf_rxCtx.i_rxFillLevel = 0x00U;
      gs_rf_rxCtx.i_numberOfReadedByte = 0x00U;
      gs_rf_rxCtx.i_numberOfRequestedByte = 0xFFFFU;
      gs_rf_rxCtx.pc_dataPointer = NULL;
      gs_rf_rxCtx.c_rssiValue = 0x00U;

      #if USE_TI_RTOS
      /* Send none blocking rx command */
      gsi_rx_cmd = loc_rfPostCmd((RF_Op*)gs_rf_rxCtx.ps_cmdPropRxAdv, RF_PriorityNormal, NULL);
      if(gsi_rx_cmd < 0)
      {
        /* Something is wrong */
        b_return = false;
      }/* if */
      #else
      /* 5.3. Send CMD_PROP_RX command to RF Core */
      RFC_sendRadioOp_nb((rfc_radioOp_t*)gs_rf_rxCtx.ps_cmdPropRxAdv, NULL);
      #endif /* USE_TI_RTOS */
    }/* if */
    else
    {
      /* Failed to allocate space for all data entries */
      b_return = false;
    }/* if...else */
  }/* if */

  return b_return;
} /* loc_startRx() */
#endif /* CC13XX_RX_ENABLED */

#if USE_TI_RTOS
/*******************************************************************************
 * @brief RF callback function pointer type
 *  RF callbacks can occur at the completion of posted RF operation (chain). The
 *  callback is called from SWI context and provides the relevant #RF_Handle,
 *  pointer to the relevant radio operation as well as an #RF_Event that indicates
 *  what has occurred.
*******************************************************************************/
void loc_txIsr(RF_Handle h, RF_CmdHandle ch, RF_Event e)
{
  if(e == RF_EventCmdDone)
  {
    /* Store the interrupt. The other stuff will be handled in the run function */
    gl_interruptFlag = IRQ_LAST_COMMAND_DONE;
  }/* if */
}/* loc_txIsr() */
#else
/*******************************************************************************
 * @brief   ISR function which will be called after a telegram is sent
 *
 * @param   l_flag Interrupt flag of the dbell
 *
 * @return  None
*******************************************************************************/
void loc_txIsr(uint32_t l_flag)
{
  /* Store the interrupt. The other stuff will be handled in the run function */
  gl_interruptFlag = l_flag;
}/* loc_txIsr() */
#endif /* USE_TI_RTOS */

/*******************************************************************************
 * @brief   ISR function which will be called after some bytes are receievd
 *
 * @param   l_flag Interrupt flag of the dbell
 *
 * @return  None
*******************************************************************************/
void loc_rxIsr(uint32_t l_flag)
{
  if(IRQ_RX_N_DATA_WRITTEN == (l_flag & IRQ_RX_N_DATA_WRITTEN))
  {
    /* Store the interrupt. The other stuff will be handled in the run function */
    gl_interruptFlag = IRQ_RX_N_DATA_WRITTEN;
    /* Store the number of received byte */
    gs_rf_rxCtx.i_rxFillLevel += RF_RX_THRESHOLD;

    /* If the radio will not be polled we have to call the run function here */
    #if (false == CC13XX_IS_POLLED_RADIO)
    sf_rf_run();
    #endif /* CC13XX_IS_POLLED_RADIO */
  }/* if */

  /* Clear all unmasked CPE/HW interrupts. */
  MB_ClearInts();
}/* loc_rxIsr() */

/*==============================================================================
                            PRIVATE FUNCTION
==============================================================================*/
#if CC13XX_RX_ENABLED
/*============================================================================*/
/** sf_rf_setConfigRx() */
/*============================================================================*/
void sf_rf_setConfigRx(void)
{
  s_rf_rxConf_t s_rxConf;

  /* Init the default return value */
  s_rxConf.b_return = false;

  if(NULL != gfp_confRx)
  {
    /* Request the rx config */
    gfp_confRx(&s_rxConf);

    if(s_rxConf.b_return)
    {
      gs_rf_rxCtx.ps_cmdPropRxAdv = s_rxConf.ps_cmdPropRxAdv;
      sf_rf_setRfChannel(s_rxConf.ps_cmdPropRadioDivSetup, s_rxConf.ps_cmdFs);
    }/* if */
  }/* if */
}/* sf_rf_setConfigRx()  */
#endif /* CC13XX_RX_ENABLED */

/*============================================================================*/
/** sf_rf_getRfStatus() */
/*============================================================================*/
E_RF_STATUS_t sf_rf_getRfStatus(void)
{
  return ge_rfStatus;
}/* sf_rf_getRfStatus() */
/*==============================================================================
                            FUNCTIONS
==============================================================================*/

/*============================================================================*/
/** sf_rf_init() */
/*============================================================================*/
bool sf_rf_init(rfc_RPID_t e_rpid, rfc_radioOp_t* ps_cmdPropRadioDivSetup)
{
  /* Rerturn value of the function */
  bool b_ret = false;

  #if !USE_TI_RTOS
  /* Return value of lib functions */
  rfc_returnValue_t returnValue;
  #endif /* !USE_TI_RTOS */

  /* Initialize global variables */
  gs_rf_txCtx.i_telegramLength = 0x00U;
  MEMSET(gac_data, 0x00U, sizeof(gac_data));
  gl_interruptFlag = 0x00U;
  ge_rfStatus = E_RF_STATUS_IDLE;
  gps_cmdFs = NULL;
  gs_rf_rxCtx.ps_cmdPropRxAdv = NULL;
  gs_rf_txCtx.ps_cmdPropTxAdv = NULL;
  gps_cmdPropRadioDivSetup = ps_cmdPropRadioDivSetup;
  gc_signalStrength = RF_DEFAULT_TX_POWER;

  #if USE_TI_RTOS
  gsi_rx_cmd = -5;
  RF_Params_init(&gs_rfParams);
  loc_rfOpen();
  if(gps_rfHandle != NULL)
  {
    b_ret = true;
  }/* if */
  #else
  /* 1.1. Select mode */
  returnValue = RFC_selectRadioMode(e_rpid);

  if(returnValue == RFC_OK)
  {
    /* 1.2. Enable RF Core */
    returnValue = RFC_enableRadio();
    if(returnValue == RFC_OK)
    {
      /* 1.3. Switch to HF XTAL. Needs to be done before running the RFC setup. */
      loc_switchToHfXtalOsc();

      b_ret = true;
    }/* if */
  }/* if */
  #endif /* USE_TI_RTOS */
  return b_ret;
} /* sf_rf_init() */

/*============================================================================*/
/** sf_rf_start() */
/*============================================================================*/
void sf_rf_start(void)
{
  /* Initial reset of the rf-module */
  sf_rf_reset();
}/* sf_rf_start() */

/*============================================================================*/
/* sf_rf_setCallback() */
/*============================================================================*/
bool sf_rf_setCallback(fp_rf_evt_tx fp_tx, fp_rf_evt_rx fp_rx, fp_rf_evt_configRx fp_confRx)
{
  bool b_ret = false;

  if((NULL != fp_tx) && (NULL != fp_rx) && (NULL != fp_confRx))
  {
    #if CC13XX_TX_ENABLED
    gfp_tx = fp_tx;
    #endif /* CC13XX_TX_ENABLED */
    #if CC13XX_RX_ENABLED
    gfp_rx = fp_rx;
    gfp_confRx = fp_confRx;
    #endif /* CC13XX_RX_ENABLED */
    b_ret = true;
  } /* if */

  return b_ret;
} /* sf_rf_setCallback() */

/*============================================================================*/
/** sf_rf_reset() */
/*============================================================================*/
bool sf_rf_reset(void)
{
  /* Set the transceiver back to rx mode */
  return loc_reset(true, E_RF_MODE_RUN);
} /* sf_rf_reset() */

#if CC13XX_TX_ENABLED
/*============================================================================*/
/** sf_rf_txInit() */
/*============================================================================*/
bool sf_rf_txInit(uint16_t i_len, bool b_useLongPreamble,
                  rfc_CMD_PROP_TX_ADV_t* ps_cmdPropTxAdv,
                  rfc_radioOp_t* ps_cmdPropRadioDivSetup,
                  rfc_radioOp_t* ps_cmdFs)
{
  bool b_return = false;

  /* Store the length of the telegramm in the global ctx */
  gs_rf_txCtx.i_telegramLength = i_len;
  /* Set the pointer to the start of the data array */
  gs_rf_txCtx.i_fillLevel = 0x00U;
  /* Reset last tx len value */
  gs_rf_txCtx.i_lastTxLen = 0x00U;

  /* Wake up the transceiver if it is in sleep state */
  if(ge_rfStatus != E_RF_STATUS_IDLE)
  {
    sf_rf_setPowerMode(E_RF_POWERMODE_IDLE);
  }/* if */

  gs_rf_txCtx.ps_cmdPropTxAdv = ps_cmdPropTxAdv;

  b_return = sf_rf_setRfChannel(ps_cmdPropRadioDivSetup, ps_cmdFs);

  sf_rf_setSignalStrength(gc_signalStrength);

  /* Check if a long preamble must be used */
  if(b_useLongPreamble)
  {
    ge_rfStatus = E_RF_STATUS_TX_PREPARE_LONG;
  }/* if */

  return b_return;
} /* sf_rf_txInit() */
#endif /* CC13XX_TX_ENABLED */

#if CC13XX_TX_ENABLED
/*============================================================================*/
/** sf_rf_txFinish() */
/*============================================================================*/
bool sf_rf_txFinish(void)
{
  /* Return value of the function */
  bool b_return = false;

  b_return = loc_reset(true, E_RF_MODE_RUN);

  return b_return;
} /* sf_rf_txFinish() */
#endif /* CC13XX_TX_ENABLED */

#if CC13XX_TX_ENABLED
/*============================================================================*/
/** sf_rf_txData() */
/*============================================================================*/
bool sf_rf_txData(uint8_t *pc_data, uint16_t i_len)
{
  /* return variable */
  bool b_return = false;

  #if USE_TI_RTOS
  /* Result of the tx command */
  RF_CmdHandle si_result;
  #endif /* USE_TI_RTOS */

  if(((gs_rf_txCtx.i_fillLevel + i_len) <= gs_rf_txCtx.i_telegramLength) &&
     (NULL != gs_rf_txCtx.ps_cmdPropTxAdv))
  {
    /* Copy the telegram data in to local buffer */
    MEMCPY(&gac_data[gs_rf_txCtx.i_fillLevel], pc_data, i_len);
    /* Increment the fill level*/
    gs_rf_txCtx.i_fillLevel += i_len;

    /* Check if the telegram is completely stored */
    if((gs_rf_txCtx.i_fillLevel) != gs_rf_txCtx.i_telegramLength)
    {
      /* Fire the tx event to get further telegram data by the stack */
      if(NULL != gfp_tx)
      {
        gfp_tx(i_len);
        b_return = true;
      }/* if */
    }/* if */
    else
    {
      /* Store the length of the last transmitted block for the tx event */
      gs_rf_txCtx.i_lastTxLen = i_len;

      /* Telegram is completly stored. Now we start transmiting */
      /****** STEP 4 - TX ********************************************************
       * 1. Modify CMD_PROP_TX_ADV command for application needs
       * 2. Send CMD_PROP_TX_ADV command with blocking RadioLib call
       ****************************************************************************/
      /* 4.1. Modify CMD_PROP_TX command for application needs */
      gs_rf_txCtx.ps_cmdPropTxAdv->pktLen = gs_rf_txCtx.i_telegramLength;
      /* Set the pointer to the payload */
      gs_rf_txCtx.ps_cmdPropTxAdv->pPkt = gac_data;

      #if USE_TI_RTOS
      si_result = loc_rfPostCmd((RF_Op*)&RF_868_30_cmdPropTxAdv, RF_PriorityNormal, loc_txIsr);

      /* Negative value indicates an error */
      if (si_result >= 0x00)
      {
        b_return = true;
      }
      #else
      /* 4.2. Send packet using CMD_PROP_TX with non blocking call */
      RFC_sendRadioOp_nb((rfc_radioOp_t*)gs_rf_txCtx.ps_cmdPropTxAdv, loc_txIsr);
      b_return = true;
      #endif /* USE_TI_RTOS */

      if(true == b_return)
      {
        /* Set the rf state mashine in tx state */
        ge_rfStatus = E_RF_STATUS_TX;
      }/* if */
    }/* if...else */
  }/* if */

  return b_return;
} /* sf_rf_txData() */
#endif /* CC13XX_TX_ENABLED */


#if CC13XX_TX_ENABLED
/*============================================================================*/
/** sf_rf_txSetPostamble() */
/*============================================================================*/
void sf_rf_txSetPostamble(E_RF_POSTAMBLE_t e_postamble)
{
  uint8_t c_postamble = e_postamble & 0xFF;
  if(e_postamble == E_RF_POSTAMBLE_NONE)
  {
    gc_rfPostambleSize = 0;
  }
  else
  {
    /* further possibilities for postamble sizes */
    gc_rfPostambleSize = 1;
    gc_rfPostamble     = c_postamble;
  }
} /* wmbus_phy_txSetPostamble() */
#endif /*CC13XX_TX_ENABLED*/



#if CC13XX_RX_ENABLED
/*============================================================================*/
/** sf_rf_rxInit() */
/*============================================================================*/
bool sf_rf_rxInit(uint8_t *pc_quality, uint8_t c_len)
{
  /* Return value of the function */
  bool b_return = false;

  /* Initialisation is only allowed if SYNC sequence is received. */
  if(E_RF_STATUS_RX == ge_rfStatus)
  {
    gs_rf_rxCtx.pc_quality = pc_quality;
    if(NULL != pc_quality)
    {
      gs_rf_rxCtx.c_lenQuality = c_len;
    } /* if */

    b_return = true;
  }/* if */

  return b_return;
} /* sf_rf_rxInit() */
#endif /* CC13XX_RX_ENABLED */

#if CC13XX_RX_ENABLED
/*============================================================================*/
/* sf_rf_rxData() */
/*============================================================================*/
bool sf_rf_rxData(uint8_t *pc_data, uint16_t i_len)
{
  /* Return value of the function */
  bool b_return = false;

  if(E_RF_STATUS_RX == ge_rfStatus)
  {
    gs_rf_rxCtx.i_numberOfRequestedByte = i_len;
    gs_rf_rxCtx.pc_dataPointer = pc_data;
    b_return = true;
  }/* if */

  return b_return;
} /* sf_rf_rxData() */
#endif /* CC13XX_RX_ENABLED */

#if CC13XX_RX_ENABLED
/*============================================================================*/
/** sf_rf_rxFinish() */
/*============================================================================*/
bool sf_rf_rxFinish(E_RF_MODE_t e_mode)
{
  /* Return value of the function */
  bool b_return = false;

  if((gs_rf_rxCtx.pc_quality != NULL) && (gs_rf_rxCtx.c_lenQuality >= 1))
  {
    /* Convert the meassured RSSI value in to our format. The meassured value is
    in 2's complement. The stack needs the following resoulution:
    Quality of a received telegram:
                                    FF  no link quality available
                                    FE  -254 dBm
                                    ...
                                    00  0 dBm */
    if(gs_rf_rxCtx.c_rssiValue & 0x80)
    {
      /* It is a negativ rssi value */
      gs_rf_rxCtx.c_rssiValue = ~gs_rf_rxCtx.c_rssiValue;
      gs_rf_rxCtx.c_rssiValue += 1;

      *(gs_rf_rxCtx.pc_quality) = gs_rf_rxCtx.c_rssiValue;
    }/* if */
    else
    {
      /* Positive values can't be displayed. */
      *(gs_rf_rxCtx.pc_quality) = 0x00U;
    }/* else .. if */
  }/* if */

  /* Reset the transceiver in a defined state */
  b_return = loc_reset(true, e_mode);

  return b_return;
} /* sf_rf_rxFinish() */
#endif /* CC13XX_RX_ENABLED */

/*============================================================================*/
/** sf_rf_setRfChannel() */
/*============================================================================*/
bool sf_rf_setRfChannel(rfc_radioOp_t* ps_cmdPropRadioDivSetup, rfc_radioOp_t* ps_cmdFs)
{
  /* Return value of this function */
  bool b_return = false;

  #if USE_TI_RTOS
  RF_CmdHandle si_cmdFsHandle;
  #else
  /* Return value of lib functions */
  rfc_returnValue_t returnValue;
  /* State of the receiver */
  rfc_cmdStatus_t cmdStatus;
  #endif /* USE_TI_RTOS */

  gps_cmdPropRadioDivSetup = ps_cmdPropRadioDivSetup;

  /* Avoid to set serveral times the same channel */
  if((gps_cmdFs != ps_cmdFs) && (ps_cmdFs != NULL))
  {

    #if USE_TI_RTOS
    /* To update the setting we have to close the old handle and open a new one */
    loc_rfClose();
    /* Open the new rf handle with the correct div setup */
    loc_rfOpen();

    /* Set the frequency */
    si_cmdFsHandle = loc_rfPostCmd((RF_Op*)ps_cmdFs, RF_PriorityNormal, 0);
    if(si_cmdFsHandle > 0)
    {
      /* Wait until the command is done */
      RF_waitCmd(gps_rfHandle, si_cmdFsHandle, RF_EventCmdDone);
      b_return = true;
    }/* if */
    #else
    /****** STEP 2 - Setup radio ***********************************************/
    /* Send CMD_PROP_RADIO_DIV_SETUP command to RF Core */
    returnValue = RFC_setupRadio(gps_cmdPropRadioDivSetup);
    if(returnValue == RFC_OK)
    {
      /****** STEP 3 - Set frequency *********************************************/
      /* Send CMD_FS command to RF Core */
      volatile uint16_t i = 0x00U;
      for(i; i<0x7FFF;i++);
      cmdStatus = RFC_sendRadioOp(ps_cmdFs);
      if(cmdStatus == RFC_CMDSTATUS_DONE)
      {
        b_return = true;
      }/* if */
    }/* if */
    #endif /* USE_TI_RTOS */

    if(true == b_return)
    {
      /* Set the current channel in the global struct */
      gps_cmdFs = ps_cmdFs;
    }/* if */
  }/* if */
  else if(gps_cmdFs == ps_cmdFs)
  {
	/* The radio is already configured */
	b_return = true;
  }
  return b_return;
} /* sf_rf_setRfChannel() */

/*============================================================================*/
/** sf_rf_sleep() */
/*============================================================================*/
void sf_rf_sleep(void)
{
  if(ge_rfStatus != E_RF_STATUS_SLEEP)
  {
    loc_reset(false, E_RF_MODE_WAIT);
  }/* if */
  return;
} /* sf_rf_sleep() */

/*============================================================================*/
/** sf_rf_wake() */
/*============================================================================*/
void sf_rf_wake(void)
{
  loc_reset(true, E_RF_MODE_RUN);
  return;
} /* sf_rf_wake() */

/*============================================================================*/
/** sf_rf_setSignalStrength() */
/*============================================================================*/
bool sf_rf_setSignalStrength(uint8_t c_signal)
{
  bool b_return = true;
  /** Command for setting the tx power */
  rfc_CMD_SET_TX_POWER_t s_cmdTxPower;
  s_cmdTxPower.commandNo = CMD_SET_TX_POWER;

  /* Values from smart rf studio v2.1.0 ***************************************/
 /* Set the minimum */
  if(c_signal <= 124U) /* -10dbm */
  {
    s_cmdTxPower.txPower.IB =        0x00U;
    s_cmdTxPower.txPower.GC =        0x03U;
    s_cmdTxPower.txPower.boost =     0x00U;
    s_cmdTxPower.txPower.tempCoeff = 0x00U;
    gc_signalStrength = 120U;
  }/* if */
  /* Set the maximum */
  else if(c_signal >= 144) /*  14dbm */
  {
    s_cmdTxPower.txPower.IB =        0x3FU;
    s_cmdTxPower.txPower.GC =        0x00U;
    s_cmdTxPower.txPower.boost =     0x01U;
    s_cmdTxPower.txPower.tempCoeff = 0x00U;
    gc_signalStrength = 144U;
  }/* if .. else if */
  /* Calculate the register settings */
  else
  {
    /* Check which setting should be used. */
    switch(c_signal)
    {
      case 143U:                                                     /* 13dBm */
        s_cmdTxPower.txPower.IB =        0x3FU;
        s_cmdTxPower.txPower.GC =        0x00U;
        s_cmdTxPower.txPower.boost =     0x00U;
        s_cmdTxPower.txPower.tempCoeff = 0x00U;
        break;

      case 142U:                                                     /* 12dBm */
        s_cmdTxPower.txPower.IB =        0x14U;
        s_cmdTxPower.txPower.GC =        0x00U;
        s_cmdTxPower.txPower.boost =     0x00U;
        s_cmdTxPower.txPower.tempCoeff = 0x00U;
        break;

      case 141U: case 140U: case 139U: case 138U:                    /* 10dBm */
        s_cmdTxPower.txPower.IB =        0x11U;
        s_cmdTxPower.txPower.GC =        0x03U;
        s_cmdTxPower.txPower.boost =     0x00U;
        s_cmdTxPower.txPower.tempCoeff = 0x00U;
        break;

      case 137U: case 136U: case 135U: case 134U: case 133U:         /*  5dBm */
        s_cmdTxPower.txPower.IB =        0x05U;
        s_cmdTxPower.txPower.GC =        0x03U;
        s_cmdTxPower.txPower.boost =     0x00U;
        s_cmdTxPower.txPower.tempCoeff = 0x00U;
        break;

      case 132U: case 131U: case 130U: case 129U: case 128U: case 127U:
      case 126U: case 125U:                                          /*  0dBm */
        s_cmdTxPower.txPower.IB =        0x02U;
        s_cmdTxPower.txPower.GC =        0x03U;
        s_cmdTxPower.txPower.boost =     0x00U;
        s_cmdTxPower.txPower.tempCoeff = 0x00U;
        break;
    }/* switch */
    gc_signalStrength = c_signal;
  } /* else */

  if(E_RF_STATUS_SLEEP != ge_rfStatus)
  {
    /* Transmit the tx power setting to the rf-core */
    MB_SendCommand((uint32_t) &s_cmdTxPower);
  }/* if */
  return b_return;
} /* sf_rf_setSignalStrength() */

/*============================================================================*/
/** sf_rf_getSignalStrength() */
/*============================================================================*/
uint8_t sf_rf_getSignalStrength(void)
{
  return gc_signalStrength;
}/* sf_rf_getSignalStrength() */

/*============================================================================*/
/*! sf_rf_setFrequencyOffset() */
/*============================================================================*/
bool sf_rf_setFrequencyOffset(int16_t si_freqOffset)
{
  bool b_ret = false;

  return b_ret;
}/* sf_rf_setFrequencyOffset() */

/*============================================================================*/
/** sf_rf_setPowerMode() */
/*============================================================================*/
bool sf_rf_setPowerMode(E_RF_POWERMODE_t e_powermode)
{
  bool b_return = false;

  /* Go to required power mode immediately. */
  switch(e_powermode)
  {
    case E_RF_POWERMODE_IDLE:
      if(ge_rfStatus != E_RF_STATUS_IDLE)
      {
        if(ge_rfStatus == E_RF_STATUS_SLEEP)
        {
          #if USE_TI_RTOS
          loc_rfOpen();
          ge_rfStatus = E_RF_STATUS_IDLE;
          #else
          if (RFC_enableRadio() == RFC_OK)
          {
            ge_rfStatus = E_RF_STATUS_IDLE;
          }/* if */
          #endif /* USE_TI_RTOS */
        }/* if */
        else
        {
          /* Disable interrupt */
          RFC_registerCpe0Isr(NULL);

          #if USE_TI_RTOS
          /* First stop the receiving */
          if(gsi_rx_cmd >= 0)
          {
            RF_abortCmd(gps_rfHandle, gsi_rx_cmd);
            gsi_rx_cmd = -5;
          }/* if */
          #else
          /* Stop any radio opperration */
          RFC_sendDirectCmd(CMDR_DIR_CMD(CMD_ABORT));
          /* Wait until radio operation is completed */
          #endif
          ge_rfStatus = E_RF_STATUS_IDLE;
        }/* if...else */
      }/* if */
      b_return = true;
      break;
    case E_RF_POWERMODE_OFF:
      if(ge_rfStatus != E_RF_STATUS_SLEEP)
      {
        loc_reset(false, E_RF_MODE_WAIT);
      }/* if */
      b_return = true;
      break;

    case E_RF_POWERMODE_RX:
      loc_reset(true, E_RF_MODE_RUN);
      b_return = true;
      break;

    default:
      /* Invalid power mode. */
      b_return = false;
      break;
  } /* switch */

  return b_return;
} /* sf_rf_setPowerMode() */

/*============================================================================*/
/** sf_rf_run() */
/*============================================================================*/
void sf_rf_run(void)
{
  uint8_t *pc_packetDataPointer;
  /* Value of the status register. Needed to read ot the rssi */
  uint32_t l_cmdStateValue;
  /** Command for setting the tx power */
  rfc_CMD_GET_RSSI_t s_cmdGetRssi;
  s_cmdGetRssi.commandNo = CMD_GET_RSSI;
  /* Temp len value */
  uint16_t i_tmpLen;

  switch(ge_rfStatus)
  {
    /* TX interrupt handling ************************************************/
    case E_RF_STATUS_TX:
      if((gl_interruptFlag & IRQ_LAST_COMMAND_DONE) == IRQ_LAST_COMMAND_DONE)
      {
        /* Sending is finished. Inform the higher layers */
        ge_rfStatus = E_RF_STATUS_IDLE;
        /* Reset the interrupt flag */
        gl_interruptFlag = 0x00U;

        /* Fire the tx event */
        if(NULL != gfp_tx)
        {
          gfp_tx(gs_rf_txCtx.i_lastTxLen);
        }/* if */
      }/* if */
      break;

    case E_RF_STATUS_RX_LISTEN:
      if((gl_interruptFlag & IRQ_RX_N_DATA_WRITTEN) == IRQ_RX_N_DATA_WRITTEN)
      {
        /* The first bytes of a telegram where received */
        ge_rfStatus = E_RF_STATUS_RX;
        /* Reset the interrupt flag */
        gl_interruptFlag = 0x00U;

        if(NULL != gfp_rx)
        {
          gfp_rx(RF_NEW_TLG);
        }/* if */

        /* Read out the RSSI value of the current telegram */
        if(CMDSTA_Done == MB_SendCommand((uint32_t) &s_cmdGetRssi))
        {
          /* "The  RSSI  is  returned  in result byte 2 (bit 23:16) of CMDSTA,
             cf. Figure 3. The RSSI is given on signed form in dBm. If no RSSI
             is available, this is signaled with a special value of the RSSI
            (-128, or 0x80)." cc13xx_pg2_rfcore_hal.pdf v.0.2 */
          l_cmdStateValue = HWREG(RFC_DBELL_BASE + RFC_DBELL_O_CMDSTA);
          l_cmdStateValue = l_cmdStateValue>>16;
          l_cmdStateValue &= 0xFF;
          gs_rf_rxCtx.c_rssiValue = (uint8_t)l_cmdStateValue;
        }/* if */
      }/* if */
      break;
    /* RX interrupt handling ************************************************/
    case E_RF_STATUS_RX:
      if((gs_rf_rxCtx.i_rxFillLevel - gs_rf_rxCtx.i_numberOfReadedByte) >=
         gs_rf_rxCtx.i_numberOfRequestedByte)
      {
        gs_rf_rxCtx.ps_currentDataEntry = (rfc_dataEntryPartial_t*)RFQueue_getDataEntry();
        if(NULL != gs_rf_rxCtx.pc_dataPointer)
        {
          pc_packetDataPointer = &gs_rf_rxCtx.ps_currentDataEntry->rxData;
          MEMCPY(gs_rf_rxCtx.pc_dataPointer,
                 &pc_packetDataPointer[gs_rf_rxCtx.i_numberOfReadedByte],
                 gs_rf_rxCtx.i_numberOfRequestedByte);
        }
        gs_rf_rxCtx.i_numberOfReadedByte += gs_rf_rxCtx.i_numberOfRequestedByte;
        if(NULL != gfp_rx)
        {
          i_tmpLen = gs_rf_rxCtx.i_numberOfRequestedByte;
          gs_rf_rxCtx.i_numberOfRequestedByte = 0xFFFFU;
          gfp_rx(i_tmpLen);
        }/* if */

        /* Reset the interrupt flag */
        gl_interruptFlag = 0x00U;
      }/* if */
      break;

    default:
      break;
  }/* switch */
}/* sf_rf_run() */

/*============================================================================*/
/** sf_rf_getRxStartAddr() */
/*============================================================================*/
uint8_t* sf_rf_getRxStartAddr(void)
{
  uint8_t* pc_return = NULL;

  if(E_RF_STATUS_RX == ge_rfStatus)
  {
    pc_return = &gs_rf_rxCtx.ps_currentDataEntry->rxData;
  }/* if */

  return pc_return;
}/* sf_rf_getRxStartAddr() */

/*============================================================================*/
/** sf_rf_test_generate_carrier_modulated() */
/*============================================================================*/
bool sf_rf_test_generate_carrier_modulated(void)
{
  bool b_ret = false;

  return b_ret;
} /* sf_rf_test_generate_carrier_modulated */

/*============================================================================*/
/** sf_rf_test_generate_carrier() */
/*============================================================================*/
bool sf_rf_test_generate_carrier(void)
{
  return false;
} /* sf_rf_test_generate_carrier */

/*============================================================================*/
/** sf_rf_test_reset_deviation() */
/*============================================================================*/
bool sf_rf_test_reset_deviation(void)
{
  uint8_t b_ret = false;

  /* return */
  return b_ret;
} /* sf_rf_test_reset_deviation() */

#if CC13XX_RX_ENABLED
/*============================================================================*/
/** sf_rf_test_set_to_rx() */
/*============================================================================*/
bool sf_rf_test_set_to_rx(void)
{
  uint8_t b_ret = false;

  return b_ret;
} /* sf_rf_test_set_to_rx() */
#endif /* CC13XX_RX_ENABLED */
/*============================================================================*/
/** sf_rf_test_set_to_idle() */
/*============================================================================*/
bool sf_rf_test_set_to_idle(void)
{
  /* init return */
  uint8_t b_ret = false;

  return b_ret;
} /* sf_rf_test_set_to_idle() */


#ifdef __cplusplus
}
#endif
