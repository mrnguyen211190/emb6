/**
 * @file	  smartMAC.c
 * @date	  26.06.2016
 * @author 	Phuong Nguyen
 */

/*
********************************************************************************
*                                   INCLUDES
********************************************************************************
*/
#include "emb6.h"

#include "evproc.h"
#include "packetbuf.h"
#include "bsp.h"
#include "logger.h"
#include "crc.h"
#include "rt_tmr.h"
#include "mac_ule.h"
#include "phy_framer_802154.h"
#include "framer_802154.h"
#include "framer_smartmac.h"


/*
********************************************************************************
*                               LOCAL DEFINES
********************************************************************************
*/
#define SMARTMAC_CFG_RXPENDING_TIMEOUT_MAX      1u
#define SMARTMAC_CFG_CHANSCAN_TIMEOUT_MAX       2u
#define SMARTMAC_CFG_SELF_TEST_EN             FALSE


#if 0
#define SMARTMAC_CFG_COUNTER_IX                 2u
#if (DEMO_UDP_SOCKET_ROLE_SERVER == TRUE)
#define SMARTMAC_CFG_CONTINUOUS_TX_BROADCAST_EN       FALSE
#define SMARTMAC_CFG_CONTINUOUS_TX_UNICAST_EN         TRUE
#define SMARTMAC_CFG_CONTINUOUS_RX_EN                 FALSE
#else
#define SMARTMAC_CFG_CONTINUOUS_TX_UNICAST_EN         FALSE
#define SMARTMAC_CFG_CONTINUOUS_TX_BROADCAST_EN       FALSE
#define SMARTMAC_CFG_CONTINUOUS_RX_EN                 TRUE
#endif
#endif

/*
********************************************************************************
*                               LOCAL TYPEDEF
********************************************************************************
*/
typedef enum{
  E_SMARTMAC_STATE_NON_INIT     = 0x00,

  E_SMARTMAC_STATE_OFF          = 0x10,

  E_SMARTMAC_STATE_ON           = 0x20,
  E_SMARTMAC_STATE_SCAN         = 0x30,
  E_SMARTMAC_STATE_RX           = 0x40,
  E_SMARTMAC_STATE_RX_PENDING   = 0x41,
  E_SMARTMAC_STATE_RX_DELAY     = 0x42,
  E_SMARTMAC_STATE_TX           = 0x50,
  E_SMARTMAC_STATE_TX_DELAY     = 0x51,
  E_SMARTMAC_STATE_TX_BROADCAST = 0x52,
  E_SMARTMAC_STATE_TX_UNICAST   = 0x53,
  E_SMARTMAC_STATE_ERR          = 0x60,

}e_smartMACState;


struct s_smartMAC {
  s_ns_t           *p_netstk;
  void             *p_cbTxArg;
  nsTxCbFnct_t      cbTxFnct;
  uint32_t          rxDelay;
  e_smartMACState   state;

  // smartMAC timing parameters in milliseconds that are relied on underlying layer
  uint32_t          sleepTimeout;
  uint8_t           strobeTxInterval;
  uint8_t           scanTimeout;
  uint8_t           rxDelayMin;
  uint8_t           rxTimeout;
  uint8_t           rxPendingTimeoutCount;
  uint8_t           maxUnicastCounter;
  uint8_t           maxBroadcastCounter;

  s_rt_tmr_t        tmrPowerUp;
  s_rt_tmr_t        tmr1Scan;
  s_rt_tmr_t        tmr1RxPending;
  s_rt_tmr_t        tmr1RxDelay;
  s_rt_tmr_t        tmr1TxDelay;
};


/*
********************************************************************************
*                       LOCAL FUNCTIONS DECLARATION
********************************************************************************
*/
static void smartMAC_init (void *p_netstk, e_nsErr_t *p_err);
static void smartMAC_start(e_nsErr_t *p_err);
static void smartMAC_stop(e_nsErr_t *p_err);
static void smartMAC_send(uint8_t *p_data, uint16_t len, e_nsErr_t *p_err);
static void smartMAC_recv(uint8_t *p_data, uint16_t len, e_nsErr_t *p_err);
static void smartMAC_ioctl(e_nsIocCmd_t cmd, void *p_val, e_nsErr_t *p_err);

static void mac_txStrobe(struct s_smartMAC *p_ctx, uint8_t counter, uint16_t dstShortAddr, e_nsErr_t *p_err);
static void mac_txBroadcast(struct s_smartMAC *p_ctx, uint8_t *p_data, uint16_t len, e_nsErr_t *p_err);
static void mac_txUnicast(struct s_smartMAC *p_ctx, uint8_t *p_data, uint16_t len, e_nsErr_t *p_err);
static void mac_lbt(struct s_smartMAC *p_ctx, e_nsErr_t *p_err);

static void mac_eventHandler(c_event_t c_event, p_data_t p_data);

/* unit-test */
#if (SMARTMAC_CFG_SELF_TEST_EN == TRUE)
static void mac_selfTest(struct s_smartMAC *p_ctx);
#endif

/* timeout callback functions */
static void mac_tmrSleepCb(void *p_arg);
static void mac_tmrScanCb(void *p_arg);
static void mac_tmrRxPendingCb(void *p_arg);
static void mac_tmrRxDelayCb(void *p_arg);

static uint32_t mac_calcRxDelay(struct s_smartMAC *p_ctx, uint8_t counter);

/* state-event handling functions */
static void mac_off_entry(struct s_smartMAC *p_ctx);
static void mac_off_sleepTimeout(struct s_smartMAC *p_ctx);
static void mac_off_exit(struct s_smartMAC *p_ctx);

static void mac_on_entry(struct s_smartMAC *p_ctx);
static void mac_on_exit(struct s_smartMAC *p_ctx);

static void mac_scan_entry(struct s_smartMAC *p_ctx);
static void mac_scan_exit(struct s_smartMAC *p_ctx);

static void mac_rxPending_entry(struct s_smartMAC *p_ctx);
static void mac_rxPending_exit(struct s_smartMAC *p_ctx);

static void mac_rxDelay_entry(struct s_smartMAC *p_ctx);
static void mac_rxDelay_exit(struct s_smartMAC *p_ctx);

static void mac_txDelay_entry(struct s_smartMAC *p_ctx);
static void mac_txDelay_exit(struct s_smartMAC *p_ctx);

static void mac_txBroadcast_entry(struct s_smartMAC *p_ctx);
static void mac_txBroadcast_exit(struct s_smartMAC *p_ctx);

static void mac_txUnicast_entry(struct s_smartMAC *p_ctx);
static void mac_txUnicast_exit(struct s_smartMAC *p_ctx);

/*
********************************************************************************
*                               LOCAL VARIABLES
********************************************************************************
*/
static struct s_smartMAC smartMAC;


/*
********************************************************************************
*                               GLOBAL VARIABLES
********************************************************************************
*/
const s_nsMAC_t mac_driver_smartMAC =
{
   "smartMAC",
    smartMAC_init,
    smartMAC_start,
    smartMAC_stop,
    smartMAC_send,
    smartMAC_recv,
    smartMAC_ioctl,
};

extern uip_lladdr_t uip_lladdr;


/*
********************************************************************************
*                           LOCAL FUNCTION DEFINITIONS
********************************************************************************
*/
static void smartMAC_init (void *p_netstk, e_nsErr_t *p_err) {
  struct s_smartMAC *p_ctx = &smartMAC;
  packetbuf_attr_t macShortAddr;

  p_ctx->p_netstk = (s_ns_t *)p_netstk;

  /* set MAC address */
  memcpy(&uip_lladdr.addr, &mac_phy_config.mac_address, 8);
  linkaddr_set_node_addr((linkaddr_t *) mac_phy_config.mac_address);

  macShortAddr = (mac_phy_config.mac_address[7]     ) |
                 (mac_phy_config.mac_address[6] << 8);
  packetbuf_set_attr(PACKETBUF_ATTR_MAC_SHORT_ADDR, macShortAddr);

  /* initialize MAC PIB attributes */
  packetbuf_attr_t macAckWaitDuration;
  packetbuf_attr_t macUnitBackoffPeriod;
  packetbuf_attr_t phyTurnaroundTime;
  packetbuf_attr_t phySHRDuration;
  packetbuf_attr_t phySymbolsPerOctet;
  packetbuf_attr_t phySymbolPeriod;

  phySHRDuration = packetbuf_attr(PACKETBUF_ATTR_PHY_SHR_DURATION);
  phyTurnaroundTime = packetbuf_attr(PACKETBUF_ATTR_PHY_TURNAROUND_TIME);
  phySymbolsPerOctet = packetbuf_attr(PACKETBUF_ATTR_PHY_SYMBOLS_PER_OCTET);
  phySymbolPeriod = packetbuf_attr(PACKETBUF_ATTR_PHY_SYMBOL_PERIOD);

  macUnitBackoffPeriod = 20 * phySymbolPeriod;
  packetbuf_set_attr(PACKETBUF_ATTR_MAC_UNIT_BACKOFF_PERIOD, macUnitBackoffPeriod);

  /* compute and set macAckWaitDuration attribute */
  macAckWaitDuration = macUnitBackoffPeriod + phyTurnaroundTime +
      phySHRDuration + 6 * phySymbolsPerOctet * phySymbolPeriod;
  packetbuf_set_attr(PACKETBUF_ATTR_MAC_ACK_WAIT_DURATION, macAckWaitDuration);

  // FIXME compute smartMAC timing parameters based on settings of underlying layers
  uint8_t strobeLen = 12; /* PHR(1) + FCF(2) + SEQ(1) + PANID(2) + DST.ADDR(2) + SRC.ADDR(2) + CHKSUM(2) */

  /* set SmartMAC attributes */
#if (NETSTK_CFG_LOW_POWER_MODE_EN == TRUE)
  p_ctx->sleepTimeout = mac_phy_config.sleepTimeout;
#endif
  p_ctx->strobeTxInterval = (phySHRDuration + (8 * strobeLen) * phySymbolPeriod + macAckWaitDuration) / 1000 + 1; //1ms offset
  p_ctx->scanTimeout = p_ctx->strobeTxInterval + 5; //>=16ms = 4 sniffs
  p_ctx->rxTimeout = 4 * p_ctx->strobeTxInterval;
  p_ctx->rxDelayMin = 2 * p_ctx->strobeTxInterval;
  p_ctx->maxBroadcastCounter = p_ctx->sleepTimeout / p_ctx->strobeTxInterval + 1;
  p_ctx->maxUnicastCounter = 2 * p_ctx->maxBroadcastCounter;

  /* initialize local attributes */
  rt_tmr_create(&p_ctx->tmrPowerUp, E_RT_TMR_TYPE_PERIODIC, p_ctx->sleepTimeout, mac_tmrSleepCb, p_ctx);
  rt_tmr_create(&p_ctx->tmr1Scan, E_RT_TMR_TYPE_ONE_SHOT, p_ctx->scanTimeout, mac_tmrScanCb, p_ctx);
  rt_tmr_create(&p_ctx->tmr1RxPending, E_RT_TMR_TYPE_ONE_SHOT, p_ctx->rxTimeout, mac_tmrRxPendingCb, p_ctx);

  /* register MAC event */
  evproc_regCallback(NETSTK_MAC_ULE_EVENT, mac_eventHandler);

  /* initial transition */
  mac_off_entry(p_ctx);

  /* run self-testing */
#if (SMARTMAC_CFG_SELF_TEST_EN == TRUE)
  mac_selfTest(p_ctx);
#endif
}


static void smartMAC_start(e_nsErr_t *p_err) {
  struct s_smartMAC *p_ctx = &smartMAC;

  /* start sleeping timer */
  rt_tmr_start(&p_ctx->tmrPowerUp);
  *p_err = NETSTK_ERR_NONE;
}


static void smartMAC_stop(e_nsErr_t *p_err) {

}


static void smartMAC_send(uint8_t *p_data, uint16_t len, e_nsErr_t *p_err) {
  struct s_smartMAC *p_ctx = &smartMAC;
  int isBroadcastTx;

#if (SMARTMAC_CFG_CONTINUOUS_RX_EN == TRUE)
  *p_err = NETSTK_ERR_BUSY;
  return;
#endif

  /* is MAC performing channel scan? */
  if (p_ctx->state == E_SMARTMAC_STATE_SCAN) {
    /* then terminate the channel scan and start TX process */
    mac_scan_exit(p_ctx);
  }
  else if (p_ctx->state == E_SMARTMAC_STATE_OFF) {
    /* state transition: OFF -> ON */
    mac_off_exit(p_ctx);
    mac_on_entry(p_ctx);
  }
  else {
    /* MAC is busy performing other tasks which cannot be overtaken */
    *p_err = NETSTK_ERR_BUSY;
    TRACE_LOG_ERR("smartMAC busy, %02x", p_ctx->state);

    /* was transmission callback function set? */
    if (p_ctx->cbTxFnct) {
      /* then signal the upper layer of the result of transmission process */
      p_ctx->cbTxFnct(p_ctx->p_cbTxArg, p_err);
    }
    return;
  }

  /* handle transmission */
  isBroadcastTx = packetbuf_holds_broadcast();

#if (SMARTMAC_CFG_CONTINUOUS_TX_BROADCAST_EN == TRUE)
  isBroadcastTx = TRUE;
#elif (SMARTMAC_CFG_CONTINUOUS_TX_UNICAST_EN == TRUE)
  linkaddr_t dstAddr;

  /* unicast, ACK required, destination address 0x1120 */
  isBroadcastTx = FALSE;
  packetbuf_set_attr(PACKETBUF_ATTR_MAC_ACK, TRUE);
  memset(&dstAddr, 0, sizeof(dstAddr));
  dstAddr.u8[7] = 0x21;
  dstAddr.u8[6] = 0x11;
  linkaddr_copy((linkaddr_t *)packetbuf_addr(PACKETBUF_ADDR_RECEIVER), &dstAddr);
#else

#endif

#if ((SMARTMAC_CFG_CONTINUOUS_TX_BROADCAST_EN == TRUE) || (SMARTMAC_CFG_CONTINUOUS_TX_UNICAST_EN == TRUE))
  uint32_t txDelayMilli = 100;

  for (;;) {
    txDelayMilli += 100;
    if (txDelayMilli > 2000) {
      txDelayMilli = 100;
    }
    bsp_delay_us(txDelayMilli * 1000);

    if (isBroadcastTx == TRUE) {
      mac_txBroadcast(p_ctx, p_data, len, p_err);
    } else {
      mac_txUnicast(p_ctx, p_data, len, p_err);
    }
  }
#endif

  /* Listen Before Talk */
  mac_lbt(p_ctx, p_err);
  if (*p_err == NETSTK_ERR_NONE) {
    if (isBroadcastTx) {
      mac_txBroadcast(p_ctx, p_data, len, p_err);
    }
    else {
      mac_txUnicast(p_ctx, p_data, len, p_err);
    }

    /* state transition: ON -> OFF */
    mac_on_exit(p_ctx);
    mac_off_entry(p_ctx);
  }
  else {
    mac_scan_entry(p_ctx);
  }

  /* was transmission callback function set? */
  if (p_ctx->cbTxFnct) {
    /* then signal the upper layer of the result of transmission process */
    p_ctx->cbTxFnct(p_ctx->p_cbTxArg, p_err);
  }
}


static void smartMAC_recv(uint8_t *p_data, uint16_t len, e_nsErr_t *p_err) {
  struct s_smartMAC *p_ctx = &smartMAC;
  frame_smartmac_st frame;

  /* set return error code to default value */
  *p_err = NETSTK_ERR_NONE;

  /* parse the received frame */
  frame_smartmac_parse(p_data, len, &frame);

#if (SMARTMAC_CFG_CONTINUOUS_RX_EN == TRUE)
  trace_printHex("<RX>", p_data, 10);
#endif

  /* is MAC in SCAN state? */
  if ((p_ctx->state == E_SMARTMAC_STATE_SCAN)) {
    /* is the received frame a strobe? */
    if (frame.type == SMARTMAC_FRAME_STROBE) {
      /* is the pending frame a broadcast frame? */
      if (frame.dest_addr == FRAME802154_BROADCASTADDR) {
        /* then calculate waiting time before the actual data arrives */
        p_ctx->rxDelay = mac_calcRxDelay(p_ctx, frame.counter);
        if (p_ctx->rxDelay > 0) {
          /* the actual data frame is coming after a significant amount of time,
           * then transition to RXDELAY until the actual data arrives */
          mac_scan_exit(p_ctx);
          mac_on_exit(p_ctx);

          /* state transition to RX_DELAY is handled in mac_eventHandler()
           * As such radio has a chance to perform state transition after RX_FINI */
          evproc_putEvent(E_EVPROC_HEAD, NETSTK_MAC_ULE_EVENT, mac_eventHandler);
        }
        else {
          /* is this last strobe? */
          if (frame.counter == 0) {
            /* then exits SCAN and enters RXPENDING */
            mac_scan_exit(p_ctx);
            mac_rxPending_entry(p_ctx);
          }
          else {
            /* the actual data frame is coming soon, then simply transition to
             * SCAN state. As such all incoming frame is treated in same manner */
            mac_scan_exit(p_ctx);
            mac_scan_entry(p_ctx);
          }
        }
      }
      else {
        /* otherwise the pending frame is a unicast frame, and therefore MAC
         * shall stay on for a certain amount of time to receive the frame */
        mac_scan_exit(p_ctx);
        mac_rxPending_entry(p_ctx);
      }
    }
    else {
      /* otherwise the received frame is not a strobe, then simply forwards to
       * upper layer before going back to OFF state */
      mac_scan_exit(p_ctx);
      mac_off_entry(p_ctx);
      p_ctx->p_netstk->dllc->recv(p_data, len, p_err);
    }
  }
  else if (p_ctx->state == E_SMARTMAC_STATE_RX_PENDING) {
    /* a frame has arrived, then go to Off state */
    mac_rxPending_exit(p_ctx);
    mac_off_entry(p_ctx);
    if (frame.type != SMARTMAC_FRAME_STROBE) {
      p_ctx->p_netstk->dllc->recv(p_data, len, p_err);
    }
  }
  else {
    /* unexpected event */
    TRACE_LOG_ERR("unexpected event in state=%02x", p_ctx->state);
    /* force MAC to OFF state */
    mac_off_entry(p_ctx);
    *p_err = NETSTK_ERR_FATAL;
  }
}


static void smartMAC_ioctl(e_nsIocCmd_t cmd, void *p_val, e_nsErr_t *p_err) {
#if NETSTK_CFG_ARG_CHK_EN
  if (p_err == NULL) {
    return;
  }
#endif

  struct s_smartMAC *p_ctx = &smartMAC;

  *p_err = NETSTK_ERR_NONE;
  switch (cmd) {
    case NETSTK_CMD_TX_CBFNCT_SET:
      if (p_val == NULL) {
        *p_err = NETSTK_ERR_INVALID_ARGUMENT;
      }
      else {
        p_ctx->cbTxFnct = (nsTxCbFnct_t) p_val;
      }
      break;

    case NETSTK_CMD_TX_CBARG_SET:
      p_ctx->p_cbTxArg = p_val;
      break;

    default:
      p_ctx->p_netstk->phy->ioctrl(cmd, p_val, p_err);
      break;
  }
}

static void mac_txStrobe(struct s_smartMAC *p_ctx, uint8_t counter, uint16_t dstShortAddr, e_nsErr_t *p_err) {
  uint16_t len;
  uint8_t p_buf[15];
  uint8_t *p_hdr = &p_buf[PHY_HEADER_LEN];
  frame_smartmac_st frame;

  /* set frame attributes to zero */
  memset(&frame, 0, sizeof(frame));

  /* set frame fields */
  frame.type = SMARTMAC_FRAME_STROBE;
  frame.ack_required = packetbuf_attr(PACKETBUF_ATTR_MAC_ACK);
  frame.counter = packetbuf_attr(PACKETBUF_ATTR_MAC_SEQNO);
  frame.pan_id = packetbuf_attr(PACKETBUF_ATTR_MAC_PAN_ID);
  frame.src_addr = packetbuf_attr(PACKETBUF_ATTR_MAC_SHORT_ADDR);
  frame.dest_addr = dstShortAddr;

  /* write strobe frame to buffer */
  len = frame_smartmac_create(&frame, p_hdr);

  /* issue PHY to deliver the strobe */
  p_ctx->p_netstk->phy->send(p_hdr, len, p_err);
}

static void mac_txBroadcast(struct s_smartMAC *p_ctx, uint8_t *p_data, uint16_t len, e_nsErr_t *p_err) {
  uint8_t counter;
  uint8_t dataSeqNo;
  uint8_t dataAckReq;
  packetbuf_attr_t timeGap;
  packetbuf_attr_t dstShortAddr;

  /* transition to TXBROADCAST state from OFF state*/
  mac_txBroadcast_entry(p_ctx);

  /* transmit broadcast strobes */
  counter = p_ctx->maxBroadcastCounter;
  dstShortAddr = FRAME802154_BROADCASTADDR;
  timeGap = packetbuf_attr(PACKETBUF_ATTR_MAC_ACK_WAIT_DURATION);

  /* store sequence number of the actual data packet to send */
  dataSeqNo = packetbuf_attr(PACKETBUF_ATTR_MAC_SEQNO);
  dataAckReq = packetbuf_attr(PACKETBUF_ATTR_MAC_ACK);

  /* set value of packet buffer ACK required attribute according to transmission
   * of the unicast strobes */
  packetbuf_set_attr(PACKETBUF_ATTR_MAC_ACK, FALSE);

  while (counter--) {
    /* set value of packet buffer sequence number attribute accordingly */
    packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, counter);
    mac_txStrobe(p_ctx, counter, dstShortAddr, p_err);

    /* was the strobe successfully transmitted? */
    if (*p_err != NETSTK_ERR_NONE) {
      /* then terminate the transmission process */
      TRACE_LOG_ERR("txStrobe failed -%d", *p_err);
      break;
    }
    else {
      /* otherwise wait for a certain amount of time after transmission */
      bsp_delay_us(timeGap);

      /* is the strobe the last one? */
      if (counter == 0) {
        /* restore data sequence number and acknowledgment requirement fields */
        packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, dataSeqNo);
        packetbuf_set_attr(PACKETBUF_ATTR_MAC_ACK, dataAckReq);

        /* then transmit the actual data */
        p_ctx->p_netstk->phy->send(p_data, len, p_err);
      }
    }
  }

  /* by all means, values of data frame attributes should be restored */
  packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, dataSeqNo);
  packetbuf_set_attr(PACKETBUF_ATTR_MAC_ACK, dataAckReq);

  /* transition to OFF state */
  mac_txBroadcast_exit(p_ctx);
}

static void mac_txUnicast(struct s_smartMAC *p_ctx, uint8_t *p_data, uint16_t len, e_nsErr_t *p_err) {
  uint8_t isTxDone;
  uint8_t counter;
  uint8_t dataSeqNo;
  uint8_t dataAckReq;
  linkaddr_t dstAddr;
  packetbuf_attr_t dstShortAddr;

  /* transition to TXUNICAST state from OFF state*/
  mac_txUnicast_entry(p_ctx);

  /* transmit broadcast strobes */
  counter = p_ctx->maxUnicastCounter;
  isTxDone = FALSE;

  /* store sequence number of the actual data packet to send */
  dataSeqNo = packetbuf_attr(PACKETBUF_ATTR_MAC_SEQNO);
  dataAckReq = packetbuf_attr(PACKETBUF_ATTR_MAC_ACK);

  /* obtain destination short address */
  linkaddr_copy(&dstAddr, packetbuf_addr(PACKETBUF_ADDR_RECEIVER));
  dstShortAddr = (dstAddr.u8[7]     ) |
                 (dstAddr.u8[6] << 8);

  /* set value of packet buffer ACK required attribute according to transmission
   * of the unicast strobes */
  packetbuf_set_attr(PACKETBUF_ATTR_MAC_ACK, TRUE);

  while ((isTxDone == FALSE) && (counter--)) {
    /* issue transmission request for unicast strobe */
    packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, counter);
    mac_txStrobe(p_ctx, counter, dstShortAddr, p_err);

    switch (*p_err) {
      case NETSTK_ERR_NONE:
        /* the strobe was acknowledged. The MAC then commence transmission of
         * the actual data frame before declaring completion of transmission
         * process */

        /* restore data sequence number and acknowledgment requirement fields */
        packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, dataSeqNo);
        packetbuf_set_attr(PACKETBUF_ATTR_MAC_ACK, dataAckReq);

        /* issue transmission request */
        p_ctx->p_netstk->phy->send(p_data, len, p_err);
        if (*p_err != NETSTK_ERR_NONE) {
          TRACE_LOG_ERR("unicast TX err=-%d", *p_err);
        }

        isTxDone = TRUE;
        break;

      case NETSTK_ERR_TX_NOACK:
        /* the strobe was NOT acknowledged. The MAC shall continue transmission
         * of strobes as long as maximum number of transmission attempts are is
         * not reached */
        isTxDone = FALSE;
        break;

      case NETSTK_ERR_TX_TIMEOUT:
        /* the radio could not transmit the strobe in time. The MAC shall
         * terminate the transmission process */
        isTxDone = TRUE;
        break;

      case NETSTK_ERR_TX_COLLISION:
        /* the strobe was NOT transmitted as the channel was busy. The MAC shall
         * terminate the transmission process */
        isTxDone = TRUE;
        break;

      default:
        TRACE_LOG_ERR("txUnicast failed unexpectedly -%d", *p_err);
        isTxDone = TRUE;
        break;
    }
  }

  /* by all means, values of data frame attributes should be restored */
  packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, dataSeqNo);
  packetbuf_set_attr(PACKETBUF_ATTR_MAC_ACK, dataAckReq);

  /* transition to OFF state */
  mac_txUnicast_exit(p_ctx);
}


static void mac_off_entry(struct s_smartMAC *p_ctx) {
  e_nsErr_t err = NETSTK_ERR_NONE;

  p_ctx->p_netstk->phy->off(&err);
  if (err == NETSTK_ERR_NONE) {
    p_ctx->state = E_SMARTMAC_STATE_OFF;
  }
  else {
    mac_on_entry(p_ctx);
    mac_scan_entry(p_ctx);
  }
}

static void mac_off_sleepTimeout(struct s_smartMAC *p_ctx) {
  mac_on_entry(p_ctx);
  mac_scan_entry(p_ctx);
}

static void mac_off_exit(struct s_smartMAC *p_ctx) {

}

static void mac_on_entry(struct s_smartMAC *p_ctx) {
  e_nsErr_t err;

  p_ctx->state = E_SMARTMAC_STATE_ON;
  p_ctx->p_netstk->phy->on(&err);
}

static void mac_on_exit(struct s_smartMAC *p_ctx) {

}

static void mac_scan_entry(struct s_smartMAC *p_ctx) {
  /* start scan timer */
  p_ctx->state = E_SMARTMAC_STATE_SCAN;
  rt_tmr_start(&p_ctx->tmr1Scan);
}

static void mac_scan_exit(struct s_smartMAC *p_ctx) {
  rt_tmr_stop(&p_ctx->tmr1Scan);
}


static void mac_rxPending_entry(struct s_smartMAC *p_ctx) {
  p_ctx->state = E_SMARTMAC_STATE_RX_PENDING;
  rt_tmr_start(&p_ctx->tmr1RxPending);
}

static void mac_rxPending_exit(struct s_smartMAC *p_ctx) {
  p_ctx->rxPendingTimeoutCount = 0;
  rt_tmr_stop(&p_ctx->tmr1RxPending);
}

static void mac_rxDelay_entry(struct s_smartMAC *p_ctx) {
  p_ctx->state = E_SMARTMAC_STATE_RX_DELAY;
  rt_tmr_stop(&p_ctx->tmr1RxDelay);
  rt_tmr_create(&p_ctx->tmr1RxDelay, E_RT_TMR_TYPE_ONE_SHOT, p_ctx->rxDelay, mac_tmrRxDelayCb, p_ctx);
  rt_tmr_start(&p_ctx->tmr1RxDelay);
}

static void mac_rxDelay_exit(struct s_smartMAC *p_ctx) {
  rt_tmr_stop(&p_ctx->tmr1RxDelay);
}

static void mac_txDelay_entry(struct s_smartMAC *p_ctx) {

}

static void mac_txDelay_exit(struct s_smartMAC *p_ctx) {

}

static void mac_txBroadcast_entry(struct s_smartMAC *p_ctx) {
  p_ctx->state = E_SMARTMAC_STATE_TX_BROADCAST;
}

static void mac_txBroadcast_exit(struct s_smartMAC *p_ctx) {

}

static void mac_txUnicast_entry(struct s_smartMAC *p_ctx) {
  p_ctx->state = E_SMARTMAC_STATE_TX_UNICAST;
}

static void mac_txUnicast_exit(struct s_smartMAC *p_ctx) {

}


static void mac_tmrSleepCb(void *p_arg) {
  struct s_smartMAC *p_ctx = (struct s_smartMAC *)p_arg;

  /* is smartMAC in OFF state? */
  if ((p_ctx->state & 0xF0) == E_SMARTMAC_STATE_OFF) {
    /* then accept sleepTimeout event */
    mac_off_sleepTimeout(p_ctx);
  }
  else {
    /* otherwise ignore the event */
  }
}

static void mac_tmrScanCb(void *p_arg) {
  struct s_smartMAC *p_ctx = (struct s_smartMAC *)p_arg;
  e_nsErr_t err = NETSTK_ERR_NONE;
  uint8_t isPendingRxFrame = FALSE;

  /* is smartMAC in SCAN state? */
  if (p_ctx->state == E_SMARTMAC_STATE_SCAN) {
    /* check if any frame is being received. If yes, restart the timer */
    p_ctx->p_netstk->phy->ioctrl(NETSTK_CMD_RF_IS_RX_BUSY, &isPendingRxFrame, &err);
    if (isPendingRxFrame == TRUE) {
      rt_tmr_start(&p_ctx->tmr1Scan);
    }
    else {
      mac_scan_exit(p_ctx);
      mac_off_entry(p_ctx);
    }
  }
  else {
    /* otherwise ignore the event */
    TRACE_LOG_ERR("<SMARTMAC> unexpected event %02x, expected %02x", p_ctx->state, E_SMARTMAC_STATE_SCAN);
  }
}

static void mac_tmrRxPendingCb(void *p_arg) {
  struct s_smartMAC *p_ctx = (struct s_smartMAC *)p_arg;
  e_nsErr_t err = NETSTK_ERR_NONE;
  uint8_t isPendingRxFrame = FALSE;

  /* is smartMAC in RXPENDING state? */
  if (p_ctx->state == E_SMARTMAC_STATE_RX_PENDING) {
    /* check if any frame is being received. If yes, restart the timer */
    p_ctx->p_netstk->phy->ioctrl(NETSTK_CMD_RF_IS_RX_BUSY, &isPendingRxFrame, &err);
    if (isPendingRxFrame == TRUE) {
      p_ctx->rxPendingTimeoutCount++;
      if (p_ctx->rxPendingTimeoutCount > SMARTMAC_CFG_RXPENDING_TIMEOUT_MAX) {
        TRACE_LOG_ERR("<RXPENDING> timeout max");
        mac_rxPending_exit(p_ctx);
        mac_off_entry(p_ctx);
      }
      else {
        rt_tmr_start(&p_ctx->tmr1RxPending);
      }
    }
    else {
      mac_rxPending_exit(p_ctx);
      mac_off_entry(p_ctx);
    }
  }
  else {
    /* otherwise ignore the event */
    TRACE_LOG_ERR("<SMARTMAC> unexpected event %02x, expected %02x", p_ctx->state, E_SMARTMAC_STATE_RX_PENDING);
  }
}

static void mac_tmrRxDelayCb(void *p_arg) {
  struct s_smartMAC *p_ctx = (struct s_smartMAC *)p_arg;

  /* is smartMAC in RXDELAY state? */
  if (p_ctx->state == E_SMARTMAC_STATE_RX_DELAY) {
    /* then accept rxDelayimeout event */
    mac_rxDelay_exit(p_ctx);
    mac_on_entry(p_ctx);
    mac_scan_entry(p_ctx);
  }
  else {
    /* otherwise ignore the event */
    TRACE_LOG_ERR("<SMARTMAC> unexpected event %02x, expected %02x", p_ctx->state, E_SMARTMAC_STATE_RX_DELAY);
  }
}

static uint32_t mac_calcRxDelay(struct s_smartMAC *p_ctx, uint8_t counter) {
  rt_tmr_tick_t rx_delay = 0;

  if (counter) {
    rx_delay = counter * p_ctx->strobeTxInterval;
    if (rx_delay > p_ctx->rxDelayMin) {
      rx_delay -= p_ctx->rxDelayMin;
    }
    else {
      rx_delay = 0;
    }
  }
  return rx_delay;
}

static void mac_lbt(struct s_smartMAC *p_ctx, e_nsErr_t *p_err) {
  rt_tmr_tick_t timeout;
  rt_tmr_tick_t tickstart;
  uint8_t isPendingRxFrame;

  /* initialize immediate variables */
  *p_err = NETSTK_ERR_NONE;

  p_ctx->state = E_SMARTMAC_STATE_TX;

  isPendingRxFrame = FALSE;
  timeout = p_ctx->strobeTxInterval + 1;
  tickstart = rt_tmr_getCurrenTick();

  /* wait for incoming packet until timeout */
  while ((rt_tmr_getCurrenTick() - tickstart) < timeout) {
    p_ctx->p_netstk->phy->ioctrl(NETSTK_CMD_RF_IS_RX_BUSY, &isPendingRxFrame, p_err);
    if (isPendingRxFrame == TRUE) {
      *p_err = NETSTK_ERR_BUSY;
      break;
    }
  }
}


static void mac_eventHandler(c_event_t c_event, p_data_t p_data) {
  struct s_smartMAC *p_ctx = &smartMAC;
  mac_off_entry(p_ctx);
  mac_rxDelay_entry(p_ctx);
}


/*
********************************************************************************
*                               SELT_TESTING
********************************************************************************
*/
#if (SMARTMAC_CFG_SELF_TEST_EN == TRUE)
static void mac_selfTest(struct s_smartMAC *p_ctx) {
  e_nsErr_t err;
  uint8_t rxStrobeUnicast[] = {0x24, 0x98, 0x00, 0xcd, 0xab, 0xfe, 0xca, 0x21, 0x11, 0x00, 0x00};
  uint8_t rxStrobeBroadcast[] = {0x24, 0x98, 0x00, 0xcd, 0xab, 0xff, 0xff, 0x21, 0x11, 0x00, 0x00};

  uint8_t rxUnicast[] = {0x61, 0xdc, 0x00, 0xcd, 0xab, 0xfe, 0xca, 0x50, 0x51, 0x52, 0x53};
  uint8_t rxBroadcast[] = {0x41, 0xdc, 0x00, 0xcd, 0xab, 0xff, 0xff, 0x21, 0x11, 0x50, 0x51, 0x52, 0x53};


  bsp_enterCritical();
  trace_printf("SmartMAC test: ................. started");

  /* TEST: initial transition */
  {
    assert_equ("INIT: ", E_SMARTMAC_STATE_OFF, p_ctx->state);
    assert_equ("INIT: ", E_RT_TMR_STATE_CREATED, rt_tmr_getState(&p_ctx->tmrPowerUp));
    assert_equ("INIT: ", E_RT_TMR_STATE_CREATED, rt_tmr_getState(&p_ctx->tmr1Scan));
    assert_equ("INIT: ", E_RT_TMR_STATE_CREATED, rt_tmr_getState(&p_ctx->tmr1RxPending));
  }

  /* TEST: starting smartMAC */
  {
    err = NETSTK_ERR_FATAL;
    smartMAC_start(&err);
    assert_equ("START: ", E_RT_TMR_STATE_RUNNING, rt_tmr_getState(&p_ctx->tmrPowerUp));
    assert_equ("START: ", NETSTK_ERR_NONE, err);
  }

  /* TEST: OFF-SleepTimeoutEvt */
  {
    mac_tmrSleepCb(p_ctx);
    assert_equ("OFF-SleepTimeoutEvt: ", E_SMARTMAC_STATE_SCAN, p_ctx->state);
  }

  /* TEST: ON-ScanTimeoutEvt */
  {
    mac_tmrScanCb(p_ctx);
    assert_equ("ON-ScanTimeoutEvt: ", E_SMARTMAC_STATE_OFF, p_ctx->state);
  }

  /* TEST: RX */
  {
    /* TEST: SCAN-RxStrobeUnicastEvt */
    {
      mac_tmrSleepCb(p_ctx);  /* trigger SLEEP_TIMEOUT event */

      err = NETSTK_ERR_FATAL;
      smartMAC_recv(rxStrobeUnicast, sizeof(rxStrobeUnicast), &err);
      assert_equ("SCAN-RxUnicastStrobeEvt: ", NETSTK_ERR_NONE, err);
      assert_equ("SCAN-RxUnicastStrobeEvt: ", E_SMARTMAC_STATE_RX_PENDING, p_ctx->state);
      mac_off_entry(p_ctx);   /* teardown */
    }

    /* TEST: RXPENDING-RxUnicastEvt */
    {
      mac_tmrSleepCb(p_ctx);  /* trigger SLEEP_TIMEOUT event */
      smartMAC_recv(rxStrobeUnicast, sizeof(rxStrobeUnicast), &err);

      err = NETSTK_ERR_FATAL;
      smartMAC_recv(rxUnicast, sizeof(rxUnicast), &err);
      assert_equ("RXPENDING-RxUnicastEvt: ", NETSTK_ERR_NONE, err);
      assert_equ("RXPENDING-RxUnicastEvt: ", E_SMARTMAC_STATE_OFF, p_ctx->state);
      mac_off_entry(p_ctx);   /* teardown */
    }

    /* TEST: RXPENDING-RxPendingTimeoutEvt */
    {
      mac_tmrSleepCb(p_ctx);  /* trigger SLEEP_TIMEOUT event */
      smartMAC_recv(rxStrobeUnicast, sizeof(rxStrobeUnicast), &err);

      mac_tmrRxPendingCb(p_ctx);
      assert_equ("RXPENDING-RxPendingTimeoutEvt: ", E_SMARTMAC_STATE_OFF, p_ctx->state);
      mac_off_entry(p_ctx);   /* teardown */
    }

    /* TEST: RX data frame in SCAN state */
    {
      mac_tmrSleepCb(p_ctx);  /* trigger SLEEP_TIMEOUT event */

      err = NETSTK_ERR_FATAL;
      smartMAC_recv(rxUnicast, sizeof(rxUnicast), &err);
      assert_equ("SCAN-RxUnicastEvt: ", NETSTK_ERR_NONE, err);
      assert_equ("SCAN-RxUnicastEvt: ", E_SMARTMAC_STATE_OFF, p_ctx->state);
      mac_off_entry(p_ctx);   /* teardown */
    }

    /* TEST: RX late broadcast strobes in SCAN state */
    {
      err = NETSTK_ERR_FATAL;
      mac_tmrSleepCb(p_ctx);  /* trigger SLEEP_TIMEOUT event */

      rxStrobeBroadcast[SMARTMAC_CFG_COUNTER_IX] = 1;
      smartMAC_recv(rxStrobeBroadcast, sizeof(rxStrobeBroadcast), &err);
      assert_equ("SCAN-RxBroadcastStrobe1Evt: ", NETSTK_ERR_NONE, err);
      assert_equ("SCAN-RxBroadcastStrobe1Evt: ", E_SMARTMAC_STATE_SCAN, p_ctx->state);

      rxStrobeBroadcast[SMARTMAC_CFG_COUNTER_IX] = 0;
      smartMAC_recv(rxStrobeBroadcast, sizeof(rxStrobeBroadcast), &err);
      assert_equ("SCAN-RxBroadcastStrobe0Evt: ", NETSTK_ERR_NONE, err);
      assert_equ("SCAN-RxBroadcastStrobe0Evt: ", E_SMARTMAC_STATE_SCAN, p_ctx->state);
      mac_off_entry(p_ctx);   /* teardown */
    }

    /* TEST: RX early broadcast strobes followed by actual data frame in SCAN state */
    {
      err = NETSTK_ERR_FATAL;
      mac_tmrSleepCb(p_ctx);  /* trigger SLEEP_TIMEOUT event */

      rxStrobeBroadcast[SMARTMAC_CFG_COUNTER_IX] = 20;
      smartMAC_recv(rxStrobeBroadcast, sizeof(rxStrobeBroadcast), &err);
      assert_equ("SCAN-RxBroadcastStrobeEarlyEvt: ", NETSTK_ERR_NONE, err);
      assert_equ("SCAN-RxBroadcastStrobeEarlyEvt: ", E_SMARTMAC_STATE_RX_DELAY, p_ctx->state);

      mac_tmrRxDelayCb(p_ctx);
      assert_equ("RXDELAY-RxDelayTimeoutEvt: ", E_SMARTMAC_STATE_SCAN, p_ctx->state);

      rxStrobeBroadcast[SMARTMAC_CFG_COUNTER_IX] = 1;
      smartMAC_recv(rxStrobeBroadcast, sizeof(rxStrobeBroadcast), &err);
      rxStrobeBroadcast[SMARTMAC_CFG_COUNTER_IX] = 0;
      smartMAC_recv(rxStrobeBroadcast, sizeof(rxStrobeBroadcast), &err);

      err = NETSTK_ERR_FATAL;
      smartMAC_recv(rxBroadcast, sizeof(rxBroadcast), &err);
      assert_equ("SCAN-RxBroadcastEvt: ", NETSTK_ERR_NONE, err);
      assert_equ("SCAN-RxBroadcastEvt: ", E_SMARTMAC_STATE_OFF, p_ctx->state);

      mac_off_entry(p_ctx);   /* teardown */
    }


    /* TEST: RX late broadcast strobe, followed by actual data frame in  SCAN state */
    {
      mac_tmrSleepCb(p_ctx);  /* trigger SLEEP_TIMEOUT event */

      rxStrobeBroadcast[SMARTMAC_CFG_COUNTER_IX] = 1;
      smartMAC_recv(rxStrobeBroadcast, sizeof(rxStrobeBroadcast), &err);
      rxStrobeBroadcast[SMARTMAC_CFG_COUNTER_IX] = 0;
      smartMAC_recv(rxStrobeBroadcast, sizeof(rxStrobeBroadcast), &err);

      err = NETSTK_ERR_FATAL;
      smartMAC_recv(rxBroadcast, sizeof(rxBroadcast), &err);
      assert_equ("SCAN-RxBroadcastEvt: ", NETSTK_ERR_NONE, err);
      assert_equ("SCAN-RxBroadcastEvt: ", E_SMARTMAC_STATE_OFF, p_ctx->state);
      mac_off_entry(p_ctx);   /* teardown */
    }
  }

  /* TEST: TX */
  {

  }

  trace_printf("SmartMAC test: ................. finished");
  bsp_exitCritical();
  while (1);
}
#endif /* SMARTMAC_CFG_SELF_TEST_EN */

