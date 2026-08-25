/**
  ******************************************************************************
  * @file    envnode_identity.c
  * @brief   The compiled-in fallback LoRaWAN identity. **This is the file to
  *          edit when you want a build to ship with different keys.**
  *
  *          See envnode_identity.h for where these sit in the lookup order.
  ******************************************************************************
  */
#include "envnode_identity.h"

/* ===========================================================================
 *  vvv  EDIT HERE  vvv
 *
 *  AppKey — 16 bytes, big-endian, exactly as The Things Network shows it.
 *
 *  The placeholder spells "ENVNODE-PLACEHLD" in ASCII, so it is unmistakable in
 *  a radio-core key dump and can never be confused with a real key — or with the
 *  stock LoRaMAC defaults. (An earlier placeholder used the FIPS-197 AES test
 *  vector 2B7E1516…, which is *also* the LoRaMAC default AppSKey/NwkSKey: the
 *  CM0+ boot log then printed the same 16 bytes as two different things. Found
 *  on the first hardware run — see docs/LOGBOOK.md.)
 *
 *  Replace it, or provision each node over the console instead:
 *
 *      nucleo lorawan appkey 000102030405060708090A0B0C0D0E0F
 *
 *  TTN copy/paste helper — the placeholder as one string:
 *      454E564E4F44452D504C414345484C44
 * =========================================================================== */
const uint8_t envnode_default_app_key[16] = {
  'E', 'N', 'V', 'N', 'O', 'D', 'E', '-',
  'P', 'L', 'A', 'C', 'E', 'H', 'L', 'D'
};

/* JoinEUI / AppEUI. All-zero is what TTN uses unless you set one explicitly. */
const uint8_t envnode_default_join_eui[8] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* DevEUI. Leave ALL-ZERO to use the DevEUI the radio core derives from the
   STM32's unique device ID: it is already unique per board, needs no
   provisioning, and is what `info` prints for TTN registration. Only fill this
   in if your network requires a specific DevEUI on this node. */
const uint8_t envnode_default_dev_eui[8] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* ===========================================================================
 *  ^^^  EDIT ABOVE  ^^^
 * =========================================================================== */

int envnode_identity_default_is_usable(void)
{
  uint8_t nz = 0u;
  for (int i = 0; i < 16; ++i) nz |= envnode_default_app_key[i];
  return nz ? 1 : 0;
}
