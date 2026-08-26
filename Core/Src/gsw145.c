/**
  ******************************************************************************
  * @file    gsw145.c
  * @brief   Minimal driver for the MaxLinear GSW145 Ethernet switch.
  *
  * Every register value below is decoded from
  * Document/620246_GSW145_DS_Rev1.4.pdf Rev 1.4; the page numbers in the
  * comments refer to that document.
  ******************************************************************************
  */

#include "gsw145.h"
#include <stdio.h>

/* Diagnostics over USART1 (main.c retargets _write). */
#ifdef GSW145_NO_LOG
#define GSW145_LOG(...)   do { } while (0)
#else
#define GSW145_LOG(...)   printf(__VA_ARGS__)
#endif

extern ETH_HandleTypeDef heth;

/* -------------------------------------------------------------------------- */
/* SMDIO indirect access (data sheet p.41)                                     */
/* -------------------------------------------------------------------------- */
/* The switch answers on this MDIO address; it is fixed by pin strapping and is
   one of the values SMDIO_CFG.ADDR can take (0, 4, 0x10 or 0x1F - p.47). */
#define GSW145_SMDIO_ADDR         31U
/* Writing register 31 loads SMDIO_BADR, the base address of the target. */
#define GSW145_PTR_REG            31U
/* Register 0 then reads/writes the target at offset 0 from that base. */
#define GSW145_DATA_REG           0U

/* -------------------------------------------------------------------------- */
/* Register offsets and the values this board needs                            */
/* -------------------------------------------------------------------------- */

/* xMII Interface 5 Configuration (p.146). Reset value 0x2044: RGMII mode,
   interface isolated - so no RMII reference clock until we write this. */
#define GSW145_REG_MII_CFG_5      0xF100U
/*  0x40B3 = 0100 0000 1011 0011
 *    RST      [15]    = 0     reset released
 *    EN       [14]    = 1     interface enabled
 *    ISOL     [13]    = 0     not isolated
 *    CLKDIS   [12]    = 0     do NOT gate the clock when the link reads down
 *    CRS      [10:9]  = 00    PHY-mode only, irrelevant here
 *    RGMII_IBS[8]     = 0     RGMII in-band status off
 *    RMII     [7]     = 1     reference clock is an OUTPUT - the switch drives
 *                             the 50 MHz the MCU's RMII needs
 *    MIIRATE  [6:4]   = 011   50 MHz, mandatory in RMII mode
 *    MIIMODE  [3:0]   = 0011  RMII
 */
#define GSW145_MII_CFG5_RMII_50M  0x40B3U
#define GSW145_MII_CFG5_RST       0x8000U

/* PHY Address Register PORT 5 (p.176). Reset value 0x1805. */
#define GSW145_REG_PHY_ADDR_5     0xF410U
/*  0x2AA5 = 0010 1010 1010 0101
 *    AUTO_SEL [15]    = 0     use MDIO polling results as the "automatic" source
 *    LNKST    [14:13] = 01    link forced UP
 *    SPEED    [12:11] = 01    100 Mbps
 *    FDUP     [10:9]  = 01    full duplex
 *    FCONTX   [8:7]   = 01    pause enabled
 *    FCONRX   [6:5]   = 01    pause enabled
 *    ADDR     [4:0]   = 00101 PHY address 5, the reset default
 *
 *  This is the data sheet's own self-start value for port 5 (0x32A5, p.46/47)
 *  with SPEED changed from 1 Gbps to 100 Mbps. Earlier bring-up used 0x2BF5,
 *  which additionally moved ADDR from 5 to 21 and disabled pause - neither was
 *  intended.
 */
#define GSW145_PHY_ADDR5_100M_FDX 0x2AA5U

/* MDC Master Configuration Register 0 (p.162). Reset value 0x006F: the polling
   state machine is enabled on ports 0,1,2,3,5,6. */
#define GSW145_REG_MMDC_CFG_0     0xF40BU
#define GSW145_MMDC_CFG0_PORT5    (1U << 5)

/* -------------------------------------------------------------------------- */

HAL_StatusTypeDef GSW145_ReadReg(uint16_t addr, uint16_t *val)
{
  uint32_t tmp = 0U;

  if (val == NULL)
  {
    return HAL_ERROR;
  }

  if (HAL_ETH_WritePHYRegister(&heth, GSW145_SMDIO_ADDR, GSW145_PTR_REG, addr) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_ETH_ReadPHYRegister(&heth, GSW145_SMDIO_ADDR, GSW145_DATA_REG, &tmp) != HAL_OK)
  {
    return HAL_ERROR;
  }

  *val = (uint16_t)(tmp & 0xFFFFU);
  return HAL_OK;
}

HAL_StatusTypeDef GSW145_WriteReg(uint16_t addr, uint16_t val)
{
  if (HAL_ETH_WritePHYRegister(&heth, GSW145_SMDIO_ADDR, GSW145_PTR_REG, addr) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_ETH_WritePHYRegister(&heth, GSW145_SMDIO_ADDR, GSW145_DATA_REG, val) != HAL_OK)
  {
    return HAL_ERROR;
  }
  return HAL_OK;
}

HAL_StatusTypeDef GSW145_Init(void)
{
  uint16_t mii_cfg_before = 0U;
  uint16_t mmdc_cfg = 0U;
  uint16_t readback = 0U;
  HAL_StatusTypeDef status;

  GSW145_LOG("[GSW145] init\r\n");

  /* Read MII_CFG_5 before touching it. This is the only way to tell how the
     switch actually comes out of reset on this board:
       0x2044 / 0x6044 -> RGMII, no RMII clock yet, we must configure it here
       0x40B3          -> already configured, e.g. by an attached EEPROM
     If a cold boot reports 0x40B3 the ordering below is belt-and-braces; if it
     reports 0x2044 then configuring the switch before HAL_ETH_Init() is what
     makes a cold boot work at all. */
  status = GSW145_ReadReg(GSW145_REG_MII_CFG_5, &mii_cfg_before);
  if (status != HAL_OK)
  {
    /* No MDIO at all - wrong SMDIO address, MDC not toggling, or the switch is
       held in reset. Nothing below can work. */
    GSW145_LOG("[GSW145] MDIO read failed - switch unreachable\r\n");
    return HAL_ERROR;
  }
  GSW145_LOG("[GSW145] MII_CFG_5 at power-up: 0x%04X\r\n", mii_cfg_before);

  /* 1. Stop the auto-polling state machine on port 5.
   *
   *    PHY_ADDR_5 only takes effect "when autopolling in MMDC_CFG_0 is
   *    disabled" (p.176). Port 5 has no external PHY, so the polling FSM reads
   *    all-ones and reports "PHY inactive, link status 0" (p.37) - which can
   *    override the forced link state below and drop the link intermittently.
   *    Read-modify-write so any strapping-dependent bits are preserved. */
  if (GSW145_ReadReg(GSW145_REG_MMDC_CFG_0, &mmdc_cfg) != HAL_OK)
  {
    return HAL_ERROR;
  }
  mmdc_cfg &= (uint16_t)~GSW145_MMDC_CFG0_PORT5;
  if (GSW145_WriteReg(GSW145_REG_MMDC_CFG_0, mmdc_cfg) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* 2. Force port 5 to 100 Mbps full duplex with the link up. There is no
   *    auto-negotiation partner on a direct MAC-to-MAC trace, so both ends have
   *    to be told the same thing; the MAC side is pinned in low_level_init(). */
  if (GSW145_WriteReg(GSW145_REG_PHY_ADDR_5, GSW145_PHY_ADDR5_100M_FDX) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* 3. Switch the interface to RMII and start driving the 50 MHz clock.
   *
   *    Pulse MII_CFG_5.RST around the change: the port is moving from RGMII to
   *    RMII, and RST re-initialises the xMII hardware while keeping the register
   *    contents (p.146). Without it the block can keep running with the old
   *    mode's timing. */
  if (GSW145_WriteReg(GSW145_REG_MII_CFG_5,
                      (uint16_t)(GSW145_MII_CFG5_RMII_50M | GSW145_MII_CFG5_RST)) != HAL_OK)
  {
    return HAL_ERROR;
  }
  HAL_Delay(2);
  if (GSW145_WriteReg(GSW145_REG_MII_CFG_5, GSW145_MII_CFG5_RMII_50M) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* Let the reference clock start and settle before the caller runs
     HAL_ETH_Init(), whose DMA software reset depends on it. */
  HAL_Delay(10);

  /* Verify, so a silent MDIO failure cannot masquerade as a working link. */
  if (GSW145_ReadReg(GSW145_REG_MII_CFG_5, &readback) != HAL_OK)
  {
    return HAL_ERROR;
  }
  GSW145_LOG("[GSW145] MII_CFG_5 = 0x%04X (expect 0x%04X)\r\n",
             readback, GSW145_MII_CFG5_RMII_50M);
  if (readback != GSW145_MII_CFG5_RMII_50M)
  {
    GSW145_LOG("[GSW145] MII_CFG_5 mismatch\r\n");
    return HAL_ERROR;
  }

  if (GSW145_ReadReg(GSW145_REG_PHY_ADDR_5, &readback) != HAL_OK)
  {
    return HAL_ERROR;
  }
  GSW145_LOG("[GSW145] PHY_ADDR_5 = 0x%04X (expect 0x%04X)\r\n",
             readback, GSW145_PHY_ADDR5_100M_FDX);
  if (readback != GSW145_PHY_ADDR5_100M_FDX)
  {
    GSW145_LOG("[GSW145] PHY_ADDR_5 mismatch\r\n");
    return HAL_ERROR;
  }

  GSW145_LOG("[GSW145] port 5 forced 100M/FDX, RMII clock out\r\n");
  return HAL_OK;
}
