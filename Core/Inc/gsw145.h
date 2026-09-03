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
  * @brief  Defined in LWIP/Target/ethernetif.c, outside every USER CODE
  *         section, so that a CubeMX regeneration of that file breaks the
  *         link instead of silently restoring the LAN8742 PHY template.
  *         GSW145_Init() references it to force that link dependency.
  */
extern const uint32_t Cleo_EthernetifIsCustomized;

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

/* --------------------------------------------------------------------------
 * Internal GPHY access and the test-packet generator
 *
 * Ports 0..3 are the internal gigabit PHYs behind the RJ45 jacks. Their
 * registers are NOT in the switch's own address space: they sit on the MDIO
 * master bus and are reached through the proxy registers MMDIO_CTRL / _READ /
 * _WRITE (data sheet p.40, p.160-161). GSW145_PhyRead/Write drive that proxy.
 * -------------------------------------------------------------------------- */

/** Number of internal GPHY ports (0..3), each with an RJ45 in front of it. */
#define GSW145_GPHY_PORT_COUNT    4U

/**
  * @brief  Read a PHY register over the switch's MDIO master interface.
  * @param  phy_addr  MDIO address of the PHY (see GSW145_GetPortPhyAddr).
  * @param  reg       Register offset, 0..31.
  */
HAL_StatusTypeDef GSW145_PhyRead(uint8_t phy_addr, uint8_t reg, uint16_t *val);

/**
  * @brief  Write a PHY register over the switch's MDIO master interface.
  */
HAL_StatusTypeDef GSW145_PhyWrite(uint8_t phy_addr, uint8_t reg, uint16_t val);

/**
  * @brief  Read back the MDIO address the switch assigns to a port's PHY.
  *
  * Taken from PHY_ADDR_<port>.ADDR rather than assumed, since pin strapping
  * and the boot loader can change it.
  */
HAL_StatusTypeDef GSW145_GetPortPhyAddr(uint8_t port, uint8_t *phy_addr);

/**
  * @brief  Report the negotiated link of an internal GPHY port.
  * @param  speed_mbps    Receives 10, 100 or 1000; 0 when not linked.
  * @param  full_duplex   Receives 1 for full duplex, 0 for half.
  */
HAL_StatusTypeDef GSW145_GetPortLink(uint8_t port, uint16_t *speed_mbps,
                                     uint8_t *full_duplex);

/**
  * @brief  Start the PHY test-packet generator on a port, saturating the link.
  *
  * The frames are produced inside the PHY and go straight out on the twisted
  * pair, so the rate is set by the negotiated link speed and does not touch the
  * switch fabric or the MCU's 100 Mbit RMII link. On a 1000BASE-T link this is
  * roughly 987 Mbit/s of payload (1518-byte frames, standard 96 bit-time gap).
  *
  * Fails if the port is not linked at 1000 Mbit/s, since anything slower would
  * not be the measurement that was asked for.
  *
  * @note The generated frames carry a fixed unicast destination address
  *       (00-03-19-FF-FF-Fx), so a PC NIC discards them unless it is in
  *       promiscuous mode - start a Wireshark capture before measuring.
  */
HAL_StatusTypeDef GSW145_TpgStart(uint8_t port);

/**
  * @brief  Stop the test-packet generator and restore the port's PHY polling.
  */
HAL_StatusTypeDef GSW145_TpgStop(uint8_t port);

#ifdef __cplusplus
}
#endif

#endif /* __GSW145_H__ */
