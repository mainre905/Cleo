/**
  ******************************************************************************
  * @file    gsw145.h
  * @brief   Minimal driver for the MaxLinear GSW145 Ethernet switch, as wired
  *          on the Cleo SMB board.
  *
  * Port 5 of the switch is connected straight to the STM32H750 RMII pins with
  * no PHY in between, and the switch supplies the 50 MHz reference clock. The
  * MCU reaches the switch registers over the SMDIO slave interface using the
  * indirect access scheme described in the data sheet
  * (620246_GSW145_DS_Rev1.4.pdf, section 3.2.4).
  *
  * Reference: Document/620246_GSW145_DS_Rev1.4.pdf
  ******************************************************************************
  */

#ifndef __GSW145_H__
#define __GSW145_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32h7xx_hal.h"

/**
  * @brief  Configure the switch for a fixed 100 Mbps full-duplex RMII link on
  *         port 5, with the switch driving the reference clock.
  *
  * Must be called after the ETH clocks, RMII pins and the MDC divider are up
  * (ethernetif_PreInitMDIO()) but before HAL_ETH_Init(), because HAL_ETH_Init()
  * spins on the DMA software reset, which only completes once this function has
  * started the switch's 50 MHz reference clock.
  *
  * @retval HAL_OK when every register wrote and read back as expected.
  */
HAL_StatusTypeDef GSW145_Init(void);

/**
  * @brief  Read one 16-bit switch register through the SMDIO indirect scheme.
  * @param  addr  Internal register offset, e.g. 0xF100 for MII_CFG_5.
  * @param  val   Receives the register value.
  */
HAL_StatusTypeDef GSW145_ReadReg(uint16_t addr, uint16_t *val);

/**
  * @brief  Write one 16-bit switch register through the SMDIO indirect scheme.
  */
HAL_StatusTypeDef GSW145_WriteReg(uint16_t addr, uint16_t val);

#ifdef __cplusplus
}
#endif

#endif /* __GSW145_H__ */
