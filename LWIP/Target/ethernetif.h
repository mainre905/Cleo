/* USER CODE BEGIN Header */
/**
 ******************************************************************************
  * File Name          : ethernetif.h
  * Description        : This file provides initialization code for LWIP
  *                      middleWare.
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
  */
/* USER CODE END Header */

#ifndef __ETHERNETIF_H__
#define __ETHERNETIF_H__

#include "lwip/err.h"
#include "lwip/netif.h"

/* Within 'USER CODE' section, code will be kept by default at each generation */
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* Exported functions ------------------------------------------------------- */
err_t ethernetif_init(struct netif *netif);

void ethernetif_input(struct netif *netif);
void ethernet_link_check_state(struct netif *netif);

void Error_Handler(void);
u32_t sys_jiffies(void);
u32_t sys_now(void);

/* USER CODE BEGIN 1 */

/**
  * @brief  Bring up just enough of the ETH peripheral to drive MDIO.
  *
  * Must be called before MX_LWIP_Init(). It enables the ETH clocks, configures
  * the RMII pins and sets the MDC divider, so the GSW145 can be programmed over
  * MDIO while HAL_ETH_Init() has not run yet. This is what lets the switch start
  * its 50 MHz RMII reference clock before HAL_ETH_Init() waits on the DMA reset.
  */
void ethernetif_PreInitMDIO(void);

/* USER CODE END 1 */
#endif
