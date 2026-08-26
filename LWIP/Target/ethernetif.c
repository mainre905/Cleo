/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : ethernetif.c
  * Description        : Ethernet interface for a fixed MAC-to-MAC RMII link
  *                      between the STM32H750 ETH MAC and a GSW145 switch.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  *
  * There is no PHY on this link. Port 5 of the GSW145 is wired straight to the
  * MCU's RMII pins and the switch supplies the 50 MHz reference clock, so the
  * LAN8742 driver CubeMX configured is not used at all: speed and duplex are
  * forced here to match what main.c forces on the switch side.
  *
  * CubeMX still believes the PHY is a LAN8742. Regenerating code overwrites
  * everything in this file outside the USER CODE markers, so re-apply it (or
  * keep this copy) after any pin or peripheral change in the .ioc.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "lwip/opt.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"
#include "lwip/ethip6.h"
#include "ethernetif.h"
#include <string.h>

/* Within 'USER CODE' section, code will be kept by default at each generation */
/* USER CODE BEGIN 0 */

#include "gsw145.h"

/* USER CODE END 0 */

/* Private define ------------------------------------------------------------*/

/* Network interface name */
#define IFNAME0 's'
#define IFNAME1 't'

/* ETH Setting  */
#define ETH_DMA_TRANSMIT_TIMEOUT               ( 20U )
/* ETH_RX_BUFFER_SIZE parameter is defined in lwipopts.h */

/* USER CODE BEGIN 1 */

/* Largest frame we ever hand to the DMA: 1500 byte payload + 14 byte header,
   rounded up to a cache line. */
#define ETH_TX_SCRATCH_SIZE                    ( 1536U )

/* Bring-up diagnostics over USART1 (main.c retargets _write). Define
   ETHERNETIF_NO_LOG to compile these out. */
#ifdef ETHERNETIF_NO_LOG
#define ETHERNETIF_LOG(...)                    do { } while (0)
#else
#include <stdio.h>
#define ETHERNETIF_LOG(...)                    printf(__VA_ARGS__)
#endif

/* USER CODE END 1 */

/* Private variables ---------------------------------------------------------*/
/*
@Note: This interface is implemented to operate in zero-copy mode only:
        - Rx Buffers will be allocated from LwIP stack Rx memory pool,
          then passed to ETH HAL driver.
        - Tx Buffers are copied into a single scratch buffer, then passed to the
          ETH HAL driver (see low_level_output).

@Notes:
  1.a. ETH DMA Rx descriptors must be contiguous, the default count is 4,
       to customize it please redefine ETH_RX_DESC_CNT in ETH GUI (Rx Descriptor Length)
       so that updated value will be generated in stm32xxxx_hal_conf.h
  1.b. ETH DMA Tx descriptors must be contiguous, the default count is 4,
       to customize it please redefine ETH_TX_DESC_CNT in ETH GUI (Tx Descriptor Length)
       so that updated value will be generated in stm32xxxx_hal_conf.h

  2.a. Rx Buffers number must be between ETH_RX_DESC_CNT and 2*ETH_RX_DESC_CNT
  2.b. Rx Buffers must have the same size: ETH_RX_BUFFER_SIZE, this value must
       passed to ETH DMA in the init field (heth.Init.RxBuffLen)
  2.c  The RX Ruffers addresses and sizes must be properly defined to be aligned
       to L1-CACHE line size (32 bytes).
*/

/* Data Type Definitions */
typedef enum
{
  RX_ALLOC_OK       = 0x00,
  RX_ALLOC_ERROR    = 0x01
} RxAllocStatusTypeDef;

typedef struct
{
  struct pbuf_custom pbuf_custom;
  uint8_t buff[(ETH_RX_BUFFER_SIZE + 31) & ~31] __ALIGNED(32);
} RxBuff_t;

/* Memory Pool Declaration */
#define ETH_RX_BUFFER_CNT             12U
LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(RxBuff_t), "Zero-copy RX PBUF pool");

/* Variable Definitions */
static uint8_t RxAllocStatus;

#if defined ( __ICCARM__ ) /*!< IAR Compiler */

#pragma location=0x30000000
ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT]; /* Ethernet Rx DMA Descriptors */
#pragma location=0x30000100
ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT]; /* Ethernet Tx DMA Descriptors */

#elif defined ( __CC_ARM )  /* MDK ARM Compiler */

__attribute__((at(0x30000000))) ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT]; /* Ethernet Rx DMA Descriptors */
__attribute__((at(0x30000100))) ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT]; /* Ethernet Tx DMA Descriptors */

#elif defined ( __GNUC__ ) /* GNU Compiler */

/* Placed in D2 SRAM by the .lwip_sec block in STM32H750VBTX_*.ld. The ETH DMA
   cannot reach DTCM, and without that linker block these are orphan sections. */
ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT] __attribute__((section(".RxDecripSection"), aligned(32))); /* Ethernet Rx DMA Descriptors */
ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT] __attribute__((section(".TxDecripSection"), aligned(32)));   /* Ethernet Tx DMA Descriptors */

#endif

#if defined ( __ICCARM__ ) /*!< IAR Compiler */
#pragma location = 0x30000200
extern u8_t memp_memory_RX_POOL_base[];

#elif defined ( __CC_ARM ) /* MDK ARM Compiler */
__attribute__((section(".Rx_PoolSection"))) extern u8_t memp_memory_RX_POOL_base[];

#elif defined ( __GNUC__ ) /* GNU */
__attribute__((section(".Rx_PoolSection"))) extern u8_t memp_memory_RX_POOL_base[];
#endif

/* USER CODE BEGIN 2 */

/* Single staging buffer for transmission. low_level_output() copies the pbuf
   chain here and HAL_ETH_Transmit() blocks until the DMA is done, so one buffer
   is enough and it can be reused on the next call. */
static uint8_t TxScratch[ETH_TX_SCRATCH_SIZE] __ALIGNED(32);

/* USER CODE END 2 */

/* Global Ethernet handle */
ETH_HandleTypeDef heth;
ETH_TxPacketConfigTypeDef TxConfig;

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN 3 */

/* USER CODE END 3 */

/* Private functions ---------------------------------------------------------*/
void pbuf_free_custom(struct pbuf *p);

/* USER CODE BEGIN 4 */

/**
  * @brief  Enable the ETH clocks, pins and MDC divider without touching the MAC.
  *
  * HAL_ETH_Init() sets DMAMR.SWR and spins until the hardware clears it, which
  * only happens once the RMII reference clock is running. On this board that
  * clock comes from the GSW145, and the switch powers up in RGMII mode with the
  * interface isolated (MII_CFG_5 reset value 0x2044), so it is not driving the
  * clock yet. Programming the switch first requires MDIO, and MDIO requires the
  * ETH clocks and the MDC divider - but not the reference clock, since MDC is
  * derived from HCLK.
  *
  * So this runs first, main.c then programs the switch over MDIO, and only then
  * does MX_LWIP_Init() reach HAL_ETH_Init() with a live reference clock.
  *
  * Calling HAL_ETH_MspInit() here is safe: HAL_ETH_Init() calls it again while
  * heth.gState is still HAL_ETH_STATE_RESET, and it only re-enables clocks and
  * re-applies the same GPIO configuration.
  */
void ethernetif_PreInitMDIO(void)
{
  heth.Instance = ETH;

  /* Clocks + RMII pins, including MDC (PC1) and MDIO (PA2). */
  HAL_ETH_MspInit(&heth);

  /* Select RMII before the MAC is used, mirroring HAL_ETH_Init(). */
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  HAL_SYSCFG_ETHInterfaceSelect(SYSCFG_ETH_RMII);
  (void)SYSCFG->PMCR;

  /* MDC divider from HCLK. Independent of the RMII reference clock. */
  HAL_ETH_SetMDIOClockRange(&heth);
}

/* USER CODE END 4 */

/*******************************************************************************
                       LL Driver Interface ( LwIP stack --> ETH)
*******************************************************************************/
/**
 * @brief In this function, the hardware should be initialized.
 * Called from ethernetif_init().
 *
 * @param netif the already initialized lwip network interface structure
 *        for this ethernetif
 */
static void low_level_init(struct netif *netif)
{
  ETH_MACConfigTypeDef MACConf = {0};
  HAL_StatusTypeDef hal_eth_init_status = HAL_OK;
  uint32_t retry;

  /* Start ETH HAL Init */

  /* static: heth.Init.MACAddr keeps pointing at this array after low_level_init
     returns, so it must outlive the call. */
  static uint8_t MACAddr[6];
  heth.Instance = ETH;
  /* Locally administered unicast address (bit 1 of the first octet set). */
  MACAddr[0] = 0x02;
  MACAddr[1] = 0x80;
  MACAddr[2] = 0xE1;
  MACAddr[3] = 0x00;
  MACAddr[4] = 0x00;
  MACAddr[5] = 0x01;
  heth.Init.MACAddr = &MACAddr[0];
  heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
  heth.Init.TxDesc = DMATxDscrTab;
  heth.Init.RxDesc = DMARxDscrTab;
  heth.Init.RxBuffLen = ETH_RX_BUFFER_SIZE;

  /* USER CODE BEGIN MACADDRESS */

  /* Order matters here, and it is the whole reason this hook is used.
     HAL_ETH_Init() below spins on the DMA software reset, which only clears
     once the RMII reference clock is running. That clock comes from the GSW145,
     which powers up in RGMII mode with the interface isolated. So: bring up the
     clocks, pins and MDC divider, program the switch over MDIO, and only then
     let HAL_ETH_Init() run.

     Getting this backwards still works on the bench, because the switch keeps
     its configuration across an MCU-only reset from the debugger - it only
     fails on a real cold boot. */
  ethernetif_PreInitMDIO();
  if (GSW145_Init() != HAL_OK)
  {
    ETHERNETIF_LOG("[ETH] GSW145 bring-up failed\r\n");
    /* Fall through: HAL_ETH_Init() retries below and reports if the clock never
       appeared, which is a clearer symptom than stopping here. */
  }

  /* USER CODE END MACADDRESS */

  /* The DMA software reset inside HAL_ETH_Init() needs the RMII reference clock
     the switch was just told to drive. Retry a few times so a slow clock
     start-up does not turn into a dead interface, and complain loudly rather
     than returning silently if it never appears. */
  for (retry = 0U; retry < 3U; retry++)
  {
    hal_eth_init_status = HAL_ETH_Init(&heth);
    if (hal_eth_init_status == HAL_OK)
    {
      break;
    }
    /* Allow a fresh attempt: HAL_ETH_Init() leaves the state at ERROR. */
    heth.gState = HAL_ETH_STATE_RESET;
    HAL_Delay(10);
  }

  if (hal_eth_init_status != HAL_OK)
  {
    /* Almost always means no 50 MHz RMII reference clock on PA1, i.e. the
       GSW145 MII_CFG_5 write did not take effect. */
    ETHERNETIF_LOG("[ETH] HAL_ETH_Init failed - no RMII REF_CLK from GSW145?\r\n");
    netif_set_link_down(netif);
    netif_set_down(netif);
    return;
  }

  /* This link has no PHY and no auto-negotiation, so pin the MAC to the same
     100 Mbps full duplex GSW145_Init() forced on the switch side. */
  HAL_ETH_GetMACConfig(&heth, &MACConf);
  MACConf.DuplexMode = ETH_FULLDUPLEX_MODE;
  MACConf.Speed      = ETH_SPEED_100M;
  /* The HAL default is ENABLE, which makes the MAC drop frames whose checksum
     it dislikes before LwIP ever sees them - silently, with no error callback
     and no counter. Checksums are verified in software instead
     (CHECKSUM_CHECK_* in lwipopts.h). */
  MACConf.DropTCPIPChecksumErrorPacket = DISABLE;
  HAL_ETH_SetMACConfig(&heth, &MACConf);

  memset(&TxConfig, 0 , sizeof(ETH_TxPacketConfigTypeDef));
  TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
  /* Transmit side of the same decision: LwIP fills the checksums in
     (CHECKSUM_GEN_* in lwipopts.h), the MAC must not overwrite them. */
  TxConfig.ChecksumCtrl = ETH_CHECKSUM_DISABLE;
  TxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;

  /* End ETH HAL Init */

  /* Initialize the RX POOL */
  LWIP_MEMPOOL_INIT(RX_POOL);
  RxAllocStatus = RX_ALLOC_OK;

#if LWIP_ARP || LWIP_ETHERNET

  /* set MAC hardware address length */
  netif->hwaddr_len = ETH_HWADDR_LEN;

  /* set MAC hardware address */
  netif->hwaddr[0] =  heth.Init.MACAddr[0];
  netif->hwaddr[1] =  heth.Init.MACAddr[1];
  netif->hwaddr[2] =  heth.Init.MACAddr[2];
  netif->hwaddr[3] =  heth.Init.MACAddr[3];
  netif->hwaddr[4] =  heth.Init.MACAddr[4];
  netif->hwaddr[5] =  heth.Init.MACAddr[5];

  /* maximum transfer unit */
  netif->mtu = ETH_MAX_PAYLOAD;

  /* Accept broadcast address and ARP traffic */
  /* don't set NETIF_FLAG_ETHARP if this device is not an ethernet one */
  #if LWIP_ARP
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
  #else
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHERNET;
  #endif /* LWIP_ARP */

/* USER CODE BEGIN PHY_PRE_CONFIG */

  /* No PHY to probe: the link is a fixed point-to-point trace to the switch,
     so it is up as soon as the MAC is running and stays up. */
  netif->flags |= NETIF_FLAG_LINK_UP;

/* USER CODE END PHY_PRE_CONFIG */

  /* Polled mode: MX_LWIP_Process() drains the DMA from the main loop via
     ethernetif_input(). Nothing services an ETH interrupt - stm32h7xx_it.c has
     no ETH_IRQHandler and the NVIC line is never enabled - so starting in
     interrupt mode would only create callbacks that can never fire. */
  if (HAL_ETH_Start(&heth) != HAL_OK)
  {
    ETHERNETIF_LOG("[ETH] HAL_ETH_Start failed\r\n");
    netif_set_link_down(netif);
    netif_set_down(netif);
    return;
  }

#endif /* LWIP_ARP || LWIP_ETHERNET */

/* USER CODE BEGIN LOW_LEVEL_INIT */

/* USER CODE END LOW_LEVEL_INIT */
}

/**
 * @brief This function should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @param p the MAC packet to send (e.g. IP packet including MAC addresses and type)
 * @return ERR_OK if the packet could be sent
 *         an err_t value if the packet couldn't be sent
 */

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
  ETH_BufferTypeDef Txbuffer = {0};
  uint16_t framelen = p->tot_len;

  LWIP_UNUSED_ARG(netif);

  if ((framelen == 0U) || (framelen > ETH_TX_SCRATCH_SIZE))
  {
    return ERR_BUF;
  }

  /* Flatten the chain into one buffer. Slower than handing the DMA the pbuf
     segments directly, but it keeps the descriptor handling trivial and the
     buffer alignment under our control. */
  if (pbuf_copy_partial(p, TxScratch, framelen, 0U) != framelen)
  {
    return ERR_BUF;
  }

  Txbuffer.buffer = TxScratch;
  Txbuffer.len    = framelen;
  Txbuffer.next   = NULL;

  TxConfig.Length   = framelen;
  TxConfig.TxBuffer = &Txbuffer;
  /* The payload was copied out, so there is no pbuf for the HAL to release and
     HAL_ETH_TxFreeCallback() has nothing to free. */
  TxConfig.pData    = NULL;

  /* Reclaim descriptors from previous transmissions. */
  (void)HAL_ETH_ReleaseTxPacket(&heth);

  if (HAL_ETH_Transmit(&heth, &TxConfig, ETH_DMA_TRANSMIT_TIMEOUT) != HAL_OK)
  {
    return ERR_IF;
  }

  return ERR_OK;
}

/**
 * @brief Should allocate a pbuf and transfer the bytes of the incoming
 * packet from the interface into the pbuf.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return a pbuf filled with the received packet (including MAC header)
 *         NULL on memory error
   */
static struct pbuf * low_level_input(struct netif *netif)
{
  struct pbuf *p = NULL;

  LWIP_UNUSED_ARG(netif);

  if(RxAllocStatus == RX_ALLOC_OK)
  {
    if (HAL_ETH_ReadData(&heth, (void **)&p) != HAL_OK)
    {
      p = NULL;
    }
  }

  return p;
}

/**
 * @brief This function should be called when a packet is ready to be read
 * from the interface. It uses the function low_level_input() that
 * should handle the actual reception of bytes from the network
 * interface. Then the type of the received packet is determined and
 * the appropriate input function is called.
 *
 * @param netif the lwip network interface structure for this ethernetif
 */
void ethernetif_input(struct netif *netif)
{
  struct pbuf *p = NULL;

  do
  {
    p = low_level_input( netif );
    if (p != NULL)
    {
      if (netif->input( p, netif) != ERR_OK )
      {
        pbuf_free(p);
      }
    }
  } while(p!=NULL);

/* USER CODE BEGIN ETHERNETIF_INPUT */

  /* Clear the latched "receive buffer unavailable" status. Reception itself
     restarts on its own: HAL_ETH_ReadData() calls ETH_UpdateDescriptor(), which
     rewrites DMACRDTPR once pbufs are handed back. Leaving the sticky bit set
     just makes the DMA status register confusing under a debugger. */
  if ((heth.Instance->DMACSR & ETH_DMACSR_RBU) != 0U)
  {
    heth.Instance->DMACSR = ETH_DMACSR_RBU;
  }

/* USER CODE END ETHERNETIF_INPUT */
}

#if !LWIP_ARP
/**
 * This function has to be completed by user in case of ARP OFF.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if ...
 */
static err_t low_level_output_arp_off(struct netif *netif, struct pbuf *q, const ip4_addr_t *ipaddr)
{
  err_t errval;
  errval = ERR_OK;

/* USER CODE BEGIN 5 */

/* USER CODE END 5 */

  return errval;

}
#endif /* LWIP_ARP */

/**
 * @brief Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 * This function should be passed as a parameter to netif_add().
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if the loopif is initialized
 *         ERR_MEM if private data couldn't be allocated
 *         any other err_t on error
 */
err_t ethernetif_init(struct netif *netif)
{
  LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
  /* Initialize interface hostname */
  netif->hostname = "lwip";
#endif /* LWIP_NETIF_HOSTNAME */

  netif->name[0] = IFNAME0;
  netif->name[1] = IFNAME1;
  /* We directly use etharp_output() here to save a function call.
   * You can instead declare your own function an call etharp_output()
   * from it if you have to do some checks before sending (e.g. if link
   * is available...) */

#if LWIP_IPV4
#if LWIP_ARP || LWIP_ETHERNET
#if LWIP_ARP
  netif->output = etharp_output;
#else
  /* The user should write its own code in low_level_output_arp_off function */
  netif->output = low_level_output_arp_off;
#endif /* LWIP_ARP */
#endif /* LWIP_ARP || LWIP_ETHERNET */
#endif /* LWIP_IPV4 */

#if LWIP_IPV6
  netif->output_ip6 = ethip6_output;
#endif /* LWIP_IPV6 */

  netif->linkoutput = low_level_output;

  /* initialize the hardware */
  low_level_init(netif);

  return ERR_OK;
}

/**
  * @brief  Custom Rx pbuf free callback
  * @param  pbuf: pbuf to be freed
  * @retval None
  */
void pbuf_free_custom(struct pbuf *p)
{
  struct pbuf_custom* custom_pbuf = (struct pbuf_custom*)p;
  LWIP_MEMPOOL_FREE(RX_POOL, custom_pbuf);

  /* If the Rx Buffer Pool was exhausted, signal the ethernetif_input task to
   * call HAL_ETH_GetRxDataBuffer to rebuild the Rx descriptors. */

  if (RxAllocStatus == RX_ALLOC_ERROR)
  {
    RxAllocStatus = RX_ALLOC_OK;
  }
}

/* USER CODE BEGIN 6 */

/**
* @brief  Returns the current time in milliseconds
*         when LWIP_TIMERS == 1 and NO_SYS == 1
* @param  None
* @retval Current Time value
*/
u32_t sys_now(void)
{
  return HAL_GetTick();
}

/* USER CODE END 6 */

/**
  * @brief  Initializes the ETH MSP.
  * @param  ethHandle: ETH handle
  * @retval None
  */

void HAL_ETH_MspInit(ETH_HandleTypeDef* ethHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(ethHandle->Instance==ETH)
  {
  /* USER CODE BEGIN ETH_MspInit 0 */

  /* USER CODE END ETH_MspInit 0 */
    /* Enable Peripheral clock */
    __HAL_RCC_ETH1MAC_CLK_ENABLE();
    __HAL_RCC_ETH1TX_CLK_ENABLE();
    __HAL_RCC_ETH1RX_CLK_ENABLE();

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**ETH GPIO Configuration
    PC1     ------> ETH_MDC
    PA1     ------> ETH_REF_CLK
    PA2     ------> ETH_MDIO
    PA7     ------> ETH_CRS_DV
    PC4     ------> ETH_RXD0
    PC5     ------> ETH_RXD1
    PB11     ------> ETH_TX_EN
    PB12     ------> ETH_TXD0
    PB13     ------> ETH_TXD1
    */
    GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_11|GPIO_PIN_12|GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN ETH_MspInit 1 */

  /* USER CODE END ETH_MspInit 1 */
  }
}

void HAL_ETH_MspDeInit(ETH_HandleTypeDef* ethHandle)
{
  if(ethHandle->Instance==ETH)
  {
  /* USER CODE BEGIN ETH_MspDeInit 0 */

  /* USER CODE END ETH_MspDeInit 0 */
    /* Disable Peripheral clock */
    __HAL_RCC_ETH1MAC_CLK_DISABLE();
    __HAL_RCC_ETH1TX_CLK_DISABLE();
    __HAL_RCC_ETH1RX_CLK_DISABLE();

    /**ETH GPIO Configuration
    PC1     ------> ETH_MDC
    PA1     ------> ETH_REF_CLK
    PA2     ------> ETH_MDIO
    PA7     ------> ETH_CRS_DV
    PC4     ------> ETH_RXD0
    PC5     ------> ETH_RXD1
    PB11     ------> ETH_TX_EN
    PB12     ------> ETH_TXD0
    PB13     ------> ETH_TXD1
    */
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5);

    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_7);

    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_11|GPIO_PIN_12|GPIO_PIN_13);

  /* USER CODE BEGIN ETH_MspDeInit 1 */

  /* USER CODE END ETH_MspDeInit 1 */
  }
}

/**
  * @brief  Check the ETH link state then update ETH driver and netif link accordingly.
  * @retval None
  */
void ethernet_link_check_state(struct netif *netif)
{
/* USER CODE BEGIN ETHERNET_LINK_CHECK_STATE */

  /* Deliberately empty.
   *
   * MX_LWIP_Process() calls this every 100 ms. The stock body polls the LAN8742
   * over MDIO, but there is no PHY on this link - port 5 of the GSW145 is wired
   * directly to the RMII pins - so any read would return 0xFFFF, be read as
   * "link down", and tear the interface down every 100 ms.
   *
   * The link is a fixed trace that is up whenever both ends are powered, so
   * NETIF_FLAG_LINK_UP is set once in low_level_init() and left alone.
   *
   * The trade-off is real: a broken trace, a switch reset or a GSW145
   * reconfiguration is now invisible to the stack. If that matters, poll
   * PHY_ADDR_5 / the port-5 link status over MDIO here instead of returning.
   */
  LWIP_UNUSED_ARG(netif);

/* USER CODE END ETHERNET_LINK_CHECK_STATE */
}

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
/* USER CODE BEGIN HAL ETH RxAllocateCallback */
  struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);
  if (p)
  {
    /* Get the buff from the struct pbuf address. */
    *buff = (uint8_t *)p + offsetof(RxBuff_t, buff);
    p->custom_free_function = pbuf_free_custom;
    /* Initialize the struct pbuf.
    * This must be performed whenever a buffer's allocated because it may be
    * changed by lwIP or the app, e.g., pbuf_free decrements ref. */
    pbuf_alloced_custom(PBUF_RAW, 0, PBUF_REF, p, *buff, ETH_RX_BUFFER_SIZE);
  }
  else
  {
    RxAllocStatus = RX_ALLOC_ERROR;
    *buff = NULL;
  }
/* USER CODE END HAL ETH RxAllocateCallback */
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
/* USER CODE BEGIN HAL ETH RxLinkCallback */

  struct pbuf **ppStart = (struct pbuf **)pStart;
  struct pbuf **ppEnd = (struct pbuf **)pEnd;
  struct pbuf *p = NULL;

  /* Get the struct pbuf from the buff address. */
  p = (struct pbuf *)(buff - offsetof(RxBuff_t, buff));
  p->next = NULL;
  p->tot_len = 0;
  p->len = Length;

  /* Chain the buffer. */
  if (!*ppStart)
  {
    /* The first buffer of the packet. */
    *ppStart = p;
  }
  else
  {
    /* Chain the buffer to the end of the packet. */
    (*ppEnd)->next = p;
  }
  *ppEnd  = p;

  /* Update the total length of all the buffers of the chain. Each pbuf in the chain should have its tot_len
   * set to its own length, plus the length of all the following pbufs in the chain. */
  for (p = *ppStart; p != NULL; p = p->next)
  {
    p->tot_len += Length;
  }

  /* No cache maintenance here: the D-Cache is never enabled on this project
     (nothing calls SCB_EnableDCache), so the DMA and the core see the same
     memory. If you ever enable the D-Cache, invalidate this buffer here AND
     handle DMARxDscrTab / DMATxDscrTab, which are not covered by any
     invalidate in this file. */

/* USER CODE END HAL ETH RxLinkCallback */
}

void HAL_ETH_TxFreeCallback(uint32_t * buff)
{
/* USER CODE BEGIN HAL ETH TxFreeCallback */

  /* low_level_output() copies into TxScratch and leaves TxConfig.pData NULL,
     so there is normally nothing to free. Guard anyway. */
  if (buff != NULL)
  {
    pbuf_free((struct pbuf *)buff);
  }

/* USER CODE END HAL ETH TxFreeCallback */
}

/* USER CODE BEGIN 8 */

/* USER CODE END 8 */
