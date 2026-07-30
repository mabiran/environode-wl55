/**
  ******************************************************************************
  * @file    korero_mailbox.h
  * @brief   Shared CM4 <-> CM0+ mailbox: LoRaWAN uplink commands + trace bridge.
  *
  * Two independent lock-free channels live in a fixed 1 KB region of SRAM2 at
  * 0x2000FC00, reachable by both cores (STM32WL has no data cache, so plain
  * `volatile` is sufficient). The region is reserved out of CM0+'s RAM in its
  * linker script (top 1 KB); CM4's RAM (0x20000000..0x20007FFF) never reaches it.
  *
  *  1) Command channel  (CM4 -> CM0+):
  *       CM4 fills payload[]/len/port, then bumps req_seq LAST (the commit).
  *       CM0+ polls; when req_seq != ack_seq it transmits the payload over
  *       LoRaWAN, writes `status`, then sets ack_seq = req_seq.
  *
  *  2) Trace channel    (CM0+ -> CM4):
  *       A byte ring buffer. CM0+ pushes its LoRaWAN trace at trace_head;
  *       CM4 drains from trace_tail and prints it on the ST-Link VCP so the
  *       single USB serial shows the radio core's logs. Single producer /
  *       single consumer => no locking needed.
  *
  *  3) Downlink channel (CM0+ -> CM4):
  *       CM0+ stores LoRaWAN downlinks the gateway sends (sensor-set config
  *       strings, FPort-10 commands) in a small ring; CM4 drains it from its
  *       main loop in EnvNode_DrainDownlinks() and applies each frame.
  *
  * IMPORTANT: keep this file byte-for-byte identical in the CM4 and CM0+
  * projects, and keep KORERO_MB_ADDR reserved in CM0+'s STM32WL55JCIX_FLASH.ld.
  ******************************************************************************
  */
#ifndef KORERO_MAILBOX_H
#define KORERO_MAILBOX_H

#include <stdint.h>

#define KORERO_MB_ADDR         (0x2000FC00UL)  /* top 1 KB of SRAM2            */
#define KORERO_MB_MAGIC        (0x4B4E4554UL)  /* 'KNET'                      */
#define KORERO_MB_MAX_PAYLOAD  (64U)           /* max LoRaWAN payload we relay */
#define KORERO_TRACE_SIZE      (512U)          /* trace ring (power of two)   */
#define KORERO_TRACE_MASK      (KORERO_TRACE_SIZE - 1U)
#define KORERO_DL_SLOTS        (4U)            /* stored gateway downlinks    */
#define KORERO_DL_MAX          (64U)           /* max bytes kept per downlink */

/* CM0+ -> CM4 status of the last command request */
#define KORERO_ST_IDLE         (0U)
#define KORERO_ST_SENT         (1U)  /* uplink requested successfully        */
#define KORERO_ST_NOJOIN       (2U)  /* not joined yet / send refused        */
#define KORERO_ST_BUSY         (3U)  /* radio busy / duty-cycle restricted   */

typedef struct
{
  /* ---- command channel: CM4 -> CM0+ ---- */
  volatile uint32_t magic;      /* KORERO_MB_MAGIC once initialized by CM4   */
  volatile uint32_t req_seq;    /* incremented by CM4 for each new command   */
  volatile uint32_t ack_seq;    /* set by CM0+ to req_seq once handled       */
  volatile uint32_t status;     /* CM0+ -> CM4 result of the last request    */
  volatile uint8_t  port;       /* LoRaWAN FPort to use (0 => app default)   */
  volatile uint8_t  len;        /* payload length, 0..KORERO_MB_MAX_PAYLOAD  */
  volatile uint8_t  rsvd[2];
  volatile uint8_t  payload[KORERO_MB_MAX_PAYLOAD];

  /* ---- trace channel: CM0+ -> CM4 (byte ring) ---- */
  volatile uint32_t trace_head; /* CM0+ write index (free-running, masked)   */
  volatile uint32_t trace_tail; /* CM4 read index                            */
  volatile uint8_t  trace[KORERO_TRACE_SIZE];

  /* ---- OTAA key provisioning: CM4 -> CM0+ ----
     CM4 writes dev_eui/join_eui/app_key, then bumps key_seq. CM0+ applies them
     via LmHandlerSet* and re-joins, then sets key_ack = key_seq. */
  volatile uint32_t key_seq;
  volatile uint32_t key_ack;
  volatile uint8_t  dev_eui[8];   /* big-endian, as entered                  */
  volatile uint8_t  join_eui[8];  /* a.k.a. AppEUI                            */
  volatile uint8_t  app_key[16];  /* TTN 1.0.x: used for both AppKey & NwkKey */

  /* ---- downlink channel: CM0+ -> CM4 (gateway -> node config push) ----
     CM0+ stores each received LoRaWAN downlink (port + bytes) at dl_head; CM4
     drains from dl_tail in EnvNode_DrainDownlinks() and applies it — a payload
     starting with '{' is a sensor-set config string, anything else on FPort 10
     is a binary command (see docs/PAYLOAD.md). Single producer (CM0+) / single
     consumer (CM4) => no locking. If more than KORERO_DL_SLOTS arrive before CM4
     drains, the oldest are overwritten (newest config wins). */
  volatile uint32_t dl_head;                          /* CM0+ write index       */
  volatile uint32_t dl_tail;                          /* CM4 read index         */
  volatile uint8_t  dl_port[KORERO_DL_SLOTS];         /* FPort of each downlink */
  volatile uint8_t  dl_len [KORERO_DL_SLOTS];         /* length of each downlink*/
  volatile uint8_t  dl_data[KORERO_DL_SLOTS][KORERO_DL_MAX];

  /* ---- device identity: CM0+ -> CM4 ----
     CM0+ writes its active DevEUI here at init and after key provisioning, then
     sets deveui_ready=1, so CM4 can print it on `nucleo deveui` / `info`
     without a reset / boot-log parse. */
  volatile uint32_t deveui_ready;     /* 0 until dev_eui_now is populated         */
  volatile uint8_t  dev_eui_now[8];   /* big-endian DevEUI the stack is using     */

  /* ---- legacy B1/B2 button channel: CM0+ -> CM4 (inherited, never read) ----
     KoreroNet used these two fields to let the user buttons (wired to CM0+)
     switch the Raspberry Pi's 5V optocoupler on PC1 (owned by CM4). EnviroNode
     has no Pi and CM4 no longer acts on them — it only zeroes them at init.
     They MUST stay anyway: CM0+ still writes them on a button press, and this
     struct is a shared memory ABI, so deleting them here would shift `joined`
     by 8 bytes and silently break join reporting. Reclaim them only by editing
     both copies of this header together. */
  volatile uint32_t pi_pwr_seq;       /* CM0+ bumps on each B1/B2 press           */
  volatile uint8_t  pi_pwr_on;        /* 1 = B1 pressed, 0 = B2; ignored by CM4   */

  /* ---- LoRaWAN join status: CM0+ -> CM4 ----
     CM0+ sets joined=1 in OnJoinRequest on a successful join, 0 on failure. CM4
     polls it and, on each change, emits "JOINED"/"JOIN FAILED" on the console
     (the join result is otherwise only logged to the CM0+ trace ring, which is
     noisy to watch); `info` and `nucleo lorawan status` also report it. */
  volatile uint32_t joined;           /* 1 = radio has joined TTN, 0 = not joined  */
} KoreroMailbox_t;

#define KORERO_MAILBOX  ((volatile KoreroMailbox_t *)KORERO_MB_ADDR)

#endif /* KORERO_MAILBOX_H */
