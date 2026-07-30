/**
  ******************************************************************************
  * @file    envnode_identity.h
  * @brief   Compiled-in fallback LoRaWAN identity — the "factory" keys.
  *
  *          ============================================================
  *           EDIT envnode_identity.c TO CHANGE THE KEYS OF A NEW BUILD.
  *          ============================================================
  *
  *          A node looks for its OTAA identity in three places, in order:
  *
  *            1. RTC backup registers — survive a reset, and a power cut too
  *               when VBAT is held up by a coin cell (see docs/LOGBOOK.md).
  *            2. Flash page 63 (`envnode_keystore.c`) — survives any power loss
  *               and an application re-flash.
  *            3. **These compiled-in defaults** — used when neither of the above
  *               holds a key, i.e. a virgin board or one that has just been told
  *               `nucleo lorawan forget`.
  *
  *          So the node always has *some* identity and always attempts to join;
  *          it never sits silent waiting to be provisioned. Provisioning over
  *          the console (`nucleo lorawan appkey …`) overrides these and is
  *          persisted; `nucleo lorawan forget` drops back to them.
  *
  *          The shipped AppKey is an obvious placeholder, not a secret. Replace
  *          it — either by editing envnode_identity.c and rebuilding, or per
  *          node over the console, which is the normal fleet workflow.
  ******************************************************************************
  */
#ifndef ENVNODE_IDENTITY_H
#define ENVNODE_IDENTITY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** 16-byte OTAA AppKey (TTN 1.0.x uses it for both AppKey and NwkKey). */
extern const uint8_t envnode_default_app_key[16];

/** 8-byte JoinEUI / AppEUI. All-zero is the usual TTN value. */
extern const uint8_t envnode_default_join_eui[8];

/** 8-byte DevEUI. **All-zero means "use the DevEUI derived from the chip's
 *  unique ID"**, which is what you want unless the network insists on a
 *  specific value — it is unique per board with no provisioning step. */
extern const uint8_t envnode_default_dev_eui[8];

/** @brief  1 if the compiled-in AppKey is non-zero, i.e. usable as a fallback. */
int envnode_identity_default_is_usable(void);

#ifdef __cplusplus
}
#endif
#endif /* ENVNODE_IDENTITY_H */
