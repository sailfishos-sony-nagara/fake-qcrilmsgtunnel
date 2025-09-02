/*
 * Copyright (C) 2018 Jolla Ltd.
 * Copyright (C) 2018 Slava Monich <slava.monich@jolla.com>
 * Copyright (C) 2025 Rinigus https://github.com/rinigus
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tunnel.h"

#include <gutil_log.h>

static gint32 global_serial = 1; // first serial value

static void dump_data(const GBinderReader *reader, const char *prefix) {
  const int level = GLOG_LEVEL_DEFAULT;
  gsize size = 0;
  const guint8 *data = gbinder_reader_get_data(reader, &size);

  if (data && size > 0) {
    gutil_log_dump(&gutil_log_default, level, prefix ? prefix : "  ", data,
                   size);
    GINFO("data dumped above: size=%zu", size);
  } else {
    GINFO("(no data)");
  }
}

static gboolean parse_oem_hook_message(const void *data, gsize data_len,
                                       gint32 *oem_hook_id, gint32 *resp_id,
                                       gint32 *resp_size,
                                       const void **resp_data) {
  const void *ptr = (const void *)data;

  // Initialize output parameters
  *oem_hook_id = 0;
  *resp_id = 0;
  *resp_size = 0;
  *resp_data = NULL;

  // Check minimum size for oem_hook_id
  if (data_len < sizeof(gint32)) {
    return FALSE;
  }

  // Extract OEM Hook ID
  *oem_hook_id = *(const gint32 *)ptr;
  ptr += sizeof(gint32);

  // Check if data is sufficient to proceed as raw oem hook message
  const gsize oem_strlen = strlen(OEM_STRING);
  const gsize lenmin = sizeof(gint32) + oem_strlen + 2 * sizeof(gint32);
  if (data_len < lenmin)
    return FALSE;

  // Check if it's the expected OEM Hook ID
  // Note that in this case, last `\0` is not a part of comparison
  if (strncmp(ptr, OEM_STRING, oem_strlen) != 0 &&
      strncmp(ptr, OEM_STRING, oem_strlen) != 0) {
    return FALSE;
  }

  ptr += oem_strlen;

  *resp_id = *(const gint32 *)ptr;
  ptr += sizeof(gint32);

  *resp_size = *(const gint32 *)ptr;
  ptr += sizeof(gint32);

  // validate size for payload
  if (*resp_size > 0 && data_len < lenmin + *resp_size) {
    GERR("parse_oem_hook_message: data size is smaller (%zu) than expected "
         "(%zu)",
         data_len, lenmin + *resp_size);
    return FALSE;
  }

  *resp_data = ptr;

  return TRUE;
}

static GBinderLocalReply *resp_tx_handler(GBinderLocalObject *obj,
                                          GBinderRemoteRequest *req, guint code,
                                          guint flags, int *status,
                                          void *user_data) {
  App *app = user_data;
  GBinderReader reader;
  gbinder_remote_request_init_reader(req, &reader);

  // GINFO("Response transaction %u received", code);
  // dump_data(&reader, "    ");

  if (code == QCOM_HOOK_RESPONSE_RAW) {
    gint32 serial = 0;
    gint32 err = 0;
    gsize len, elemsize;
    const void *data;
    if (gbinder_reader_read_int32(&reader, &serial) &&
        gbinder_reader_read_int32(&reader, &err)) {
      data = gbinder_reader_read_hidl_vec(&reader, &len, &elemsize);
      const gsize buflen = len * elemsize;
      GINFO("Response QCOM_HOOK_RESPONSE_RAW: serial=%d; err=%d; "
            "data_len=%lu",
            serial, err, buflen);
      if (buflen > 0 && data)
        gutil_log_dump(&gutil_log_default, GLOG_LEVEL_DEFAULT,
                       "payload: ", data, buflen < 256 ? buflen : 256);

    } else {
      GERR("Error while reading response transaction %u", code);
    }

  } else {
    GINFO("Unhandled response transaction %u", code);
  }

  *status = GBINDER_STATUS_OK;
  return NULL;
}

static const char *get_oem_response_action(gint32 response_id) {
  switch (response_id) {
  case 525299:
    return "IncrNwScanInd";
  case 525300:
    return "EngineerMode";
  case 525302:
    return "DeviceConfig";
  case 525303:
    return "AudioStateChanged";
  case 525305:
    return "ClearConfigs";
  case 525311:
    return "ValidateConfigs";
  case 525312:
    return "ValidateDumped";
  case 525320:
    return "PdcConfigsList";
  case 525322:
    return "AdnInitDone";
  case 525323:
    return "AdnRecordsInd";
  case 525340:
    return "CsgChangedInd";
  case 525341:
    return "RacChange";
  default:
    return "";
  }
}

static GBinderLocalReply *ind_tx_handler(GBinderLocalObject *obj,
                                         GBinderRemoteRequest *req, guint code,
                                         guint flags, int *status,
                                         void *user_data) {
  App *app = user_data;
  GBinderReader reader;
  gbinder_remote_request_init_reader(req, &reader);

  // dump_data(&reader, "ind    ");

  if (code == QCOM_HOOK_INDICATION_RAW) {
    gsize len, elemsize;
    const void *data = gbinder_reader_read_hidl_vec(&reader, &len, &elemsize);
    const size_t buflen = len * elemsize;

    gint32 oem_hook_id;
    gint32 resp_id;
    gint32 resp_size;
    const void *resp_data;

    if (parse_oem_hook_message(data, buflen, &oem_hook_id, &resp_id, &resp_size,
                               &resp_data)) {
      if (oem_hook_id == 1028)
        GINFO("Received RIL_UNSOL_OEM_HOOK_RAW with resp_id=%d %s; "
              "resp_size=%d",
              resp_id, get_oem_response_action(resp_id), resp_size);
      else
        GINFO("Received unknown QCOM_HOOK_INDICATION_RAW indication");
      if (resp_size > 0 && resp_data)
        gutil_log_dump(&gutil_log_default, GLOG_LEVEL_DEFAULT,
                       "payload: ", resp_data,
                       resp_size < 256 ? resp_size : 256);
    } else {
      GINFO("Failed to parse QCOM_HOOK_INDICATION_RAW indication using RAW "
            "format. oem_id=%d. Ignoring "
            "message",
            oem_hook_id);
    }
  } else {
    GINFO("Unhandled indication transaction %u", code);
  }

  *status = GBINDER_STATUS_OK;
  return NULL;
}

// send ATEL ready over IQtiOemHook
int send_atel_ready(App *app) {
  AtelReadyPayload payload = {.oem = OEM_CHARS,
                              .requestId = QCRIL_EVT_HOOK_SET_ATEL_UI_STATUS,
                              .payloadLen = 1,
                              .isReady = 1};

  const gsize buflen = sizeof(payload);

  GBinderLocalRequest *req = gbinder_client_new_request(app->client);
  GBinderWriter writer;
  if (!req)
    return 0;

  gbinder_local_request_init_writer(req, &writer);
  gbinder_writer_append_int32(&writer, global_serial++);
  gbinder_writer_append_hidl_vec(&writer, &payload, buflen, sizeof(gint8));

  int status = 0;

  GINFO("Sending ATEL ready, buflen=%zu, transaction=%u", buflen,
        TRANSACTION_OEMHOOK_RAW_REQUEST);

  GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
      app->client, TRANSACTION_OEMHOOK_RAW_REQUEST, req, &status);

  if (status != GBINDER_STATUS_OK) {
    GERR("oemHookRawRequest transact failed, status=%d", status);
    gbinder_local_request_unref(req);
    if (reply)
      gbinder_remote_reply_unref(reply);
    return 0;
  }

  if (reply) {
    GBinderReader reader;
    gbinder_remote_reply_init_reader(reply, &reader);

    gsize len = 0, elemsz = 0;
    const void *rdata = gbinder_reader_read_hidl_vec(&reader, &len, &elemsz);
    if (rdata && len > 0) {
      gutil_log_dump(&gutil_log_default, GLOG_LEVEL_DEFAULT,
                     "oemHookRawRequest reply payload: ", rdata,
                     (len * elemsz) < 256 ? (len * elemsz) : 256);
    } else {
      GINFO("oemHookRawRequest: zero length reply");
    }

    gbinder_remote_reply_unref(reply);
  }

  gbinder_local_request_unref(req);

  GINFO("ATEL ready sent successfully");
  return 1;
}

gboolean app_set_callback(App *app) {
  // check if callback has been set already
  if (app->callbacks_set)
    return;

  GBinderLocalRequest *req = gbinder_client_new_request(app->client);

  app->resp = gbinder_servicemanager_new_local_object(
      app->sm, app->config.resp_iface, resp_tx_handler, app);
  app->ind = gbinder_servicemanager_new_local_object(
      app->sm, app->config.ind_iface, ind_tx_handler, app);

  // write the two strong binder objects into the request:
  gbinder_local_request_append_local_object(req, app->resp);
  gbinder_local_request_append_local_object(req, app->ind);

  int status = 0;
  GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
      app->client, TRANSACTION_setCallback, req, &status);

  if (status == GBINDER_STATUS_OK) {
    GINFO("%s: setCallback succeeded", app->config.interface);
    app->callbacks_set = TRUE;
  } else {
    GERR("%s: setCallback failed, status %d", app->config.interface, status);
    app->callbacks_set = FALSE;
    return FALSE;
  }

  gbinder_remote_reply_unref(reply);
  gbinder_local_request_unref(req);
  return TRUE;
}
