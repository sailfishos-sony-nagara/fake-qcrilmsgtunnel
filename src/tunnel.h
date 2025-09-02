/*
 * Copyright (C) 2018 Jolla Ltd.
 * Copyright (C) 2018 Slava Monich <slava.monich@jolla.com>
 * Copyright (C) 2025 Rinigus https://github.com/rinigus
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

 #ifndef TUNNEL_DEFINED
#define TUNNEL_DEFINED

#include <gbinder.h>

#include "sim_monitor.h"

#define RADIO_ALIGNED(x) __attribute__((aligned(x)))

#define DEVICE_DEFAULT "/dev/hwbinder"
#define QCRILHOOK_NAME_BASE "oemhook"
#define QCRILHOOK_IFACE_DEFAULT                                                \
  "vendor.qti.hardware.radio.qcrilhook@1.0::IQtiOemHook"

#define TRANSACTION_setCallback 1
#define TRANSACTION_OEMHOOK_RAW_REQUEST 2

#define QCOM_HOOK_RESPONSE_RAW 1
#define QCOM_HOOK_INDICATION_RAW 1

#define OEM_CHARS {'Q', 'O', 'E', 'M', 'H', 'O', 'O', 'K'}
#define OEM_STRING "QOEMHOOK"
#define OEM_STRING_ALT "SOMCHOOK"

#define QCRIL_EVT_HOOK_SET_ATEL_UI_STATUS 524314

#define RET_OK (0)
#define RET_NOTFOUND (1)
#define RET_INVARG (2)
#define RET_ERR (3)

typedef struct app_config {
  char *device;
  char *interface;
  char *name;
  char *fqname;
  char *resp_iface;
  char *ind_iface;
  int sim;
} AppConfig;

typedef struct app {
  GMainLoop *loop;
  GBinderServiceManager *sm;
  GBinderLocalObject *local;
  GBinderRemoteObject *remote;
  gulong wait_id;
  gulong death_id;
  GBinderClient *client;
  GBinderLocalObject *resp;
  GBinderLocalObject *ind;
  SimMonitor *sim_monitor;
  gboolean hidl_connected;
  gboolean callbacks_set;
  AppConfig config;
  int ret;
} App;

typedef struct {
  char oem[8] RADIO_ALIGNED(8); /* "QOEMHOOK" (no NUL) */
  gint32 requestId
      RADIO_ALIGNED(4); /* QCRIL_EVT_HOOK_SET_ATEL_UI_STATUS (524314) */
  gint32 payloadLen RADIO_ALIGNED(4); /* length of following payload */
  gint8 isReady RADIO_ALIGNED(1);     /* 1 = ready, 0 = not ready */
} RADIO_ALIGNED(4) AtelReadyPayload;

////
extern int app_set_callback(App *app);

extern int send_atel_ready(App *app);

#endif