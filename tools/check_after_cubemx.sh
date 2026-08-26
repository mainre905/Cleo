#!/bin/sh
# Verify the customizations CubeMX is able to overwrite.
#
# Run this after every "Generate Code" from CubeMX / STM32CubeIDE. Some of the
# settings this project depends on live in generated regions that no USER CODE
# marker protects, so regeneration silently reverts them.
#
#   sh tools/check_after_cubemx.sh
#
# Exit status is 0 when everything is in place, 1 otherwise.

cd "$(dirname "$0")/.." || exit 1

fail=0

report() {
  # $1 = ok|bad, $2 = description, $3 = how to fix
  if [ "$1" = ok ]; then
    printf '  OK    %s\n' "$2"
  else
    printf '  BROKEN %s\n     -> %s\n' "$2" "$3"
    fail=1
  fi
}

check_grep() {
  # $1 = pattern, $2 = file, $3 = description, $4 = fix hint
  if grep -q -- "$1" "$2" 2>/dev/null; then
    report ok "$3"
  else
    report bad "$3" "$4"
  fi
}

echo "Checking settings CubeMX can overwrite..."
echo

echo "LWIP/Target/lwipopts.h  (CubeMX GUI: LwIP -> Key Options)"
check_grep '^#define CHECKSUM_BY_HARDWARE 0' LWIP/Target/lwipopts.h \
  "CHECKSUM_BY_HARDWARE is 0" \
  "set it to Disabled in CubeMX; the MAC checksum offload is off in both directions"
check_grep '^#define LWIP_RAM_HEAP_POINTER 0x30008000' LWIP/Target/lwipopts.h \
  "LWIP_RAM_HEAP_POINTER is 0x30008000" \
  "0x30004000 overlaps the RX pool at 0x30000200 by 2947 bytes"
echo

echo "Linker scripts  (regenerated when the device or memory layout changes)"
for ld in STM32H750VBTX_FLASH.ld STM32H750VBTX_RAM.ld; do
  check_grep 'Rx_PoolSection' "$ld" \
    "$ld places the ETH DMA sections" \
    "without the .lwip_sec block these become orphan sections in .data (19 KB of flash)"
done
echo

echo "LWIP/Target/ethernetif.c  (largely NOT protected by USER CODE markers)"
check_grep 'Cleo_EthernetifIsCustomized' LWIP/Target/ethernetif.c \
  "customization canary present" \
  "file was regenerated - restore it with: git checkout -- LWIP/Target/ethernetif.c"
check_grep 'DropTCPIPChecksumErrorPacket = DISABLE' LWIP/Target/ethernetif.c \
  "RX checksum-error drop disabled" \
  "the HAL default ENABLE drops frames before LwIP sees them"
check_grep 'ETH_CHECKSUM_DISABLE' LWIP/Target/ethernetif.c \
  "TX checksum offload disabled" \
  "LwIP fills the checksums; the MAC must not overwrite them"
check_grep 'ethernetif_PreInitMDIO' LWIP/Target/ethernetif.c \
  "MDIO pre-init present" \
  "needed so the GSW145 starts the RMII clock before HAL_ETH_Init waits on it"
if grep -q 'lan8742.h' LWIP/Target/ethernetif.c 2>/dev/null; then
  report bad "LAN8742 driver is back" \
    "there is no PHY on this board - restore ethernetif.c from git"
else
  report ok "LAN8742 driver not referenced"
fi
echo

echo "LWIP/App/lwip.c"
check_grep 'USER CODE BEGIN 4_4_1' LWIP/App/lwip.c \
  "link-check suppression hook present" \
  "restore the return in Ethernet_Link_Periodic_Handle"
check_grep 'IP_ADDRESS\[0\] = 192' LWIP/App/lwip.c \
  "static IP configured" \
  "a 0.0.0.0 interface sends packets most hosts drop"
echo

if [ "$fail" -eq 0 ]; then
  echo "All checks passed."
else
  echo "Some checks failed - see the notes above."
  echo "Details: markdown/2026-08-26/GSW145-MAC-to-MAC-bringup-report.md"
fi

exit "$fail"
