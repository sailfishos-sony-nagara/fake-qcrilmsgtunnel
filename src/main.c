/*
 * Copyright (C) 2018 Jolla Ltd.
 * Copyright (C) 2018 Slava Monich <slava.monich@jolla.com>
 *
 * You may use this file under the terms of BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <gbinder.h>

#include <gutil_log.h>

#include <glib-unix.h>

#define RADIO_ALIGNED(x) __attribute__((aligned(x)))

#define DEVICE "/dev/hwbinder"
#define QCRILHOOK_NAME "oemhook0"
#define QCRILHOOK_IFACE "vendor.qti.hardware.radio.qcrilhook@1.0::IQtiOemHook"
#define QCRILHOOK_FQNAME (QCRILHOOK_IFACE "/" QCRILHOOK_NAME)
#define QCRILHOOK_RESP_IFACE                                                   \
  "vendor.qti.hardware.radio.qcrilhook@1.0::IQtiOemHookResponse"
#define QCRILHOOK_IND_IFACE                                                    \
  "vendor.qti.hardware.radio.qcrilhook@1.0::IQtiOemHookIndication"

#define TRANSACTION_setCallback 1
#define TRANSACTION_OEMHOOK_RAW_REQUEST 2

#define QCOM_HOOK_INDICATION_RAW 1

#define OEM_CHARS {'Q', 'O', 'E', 'M', 'H', 'O', 'O', 'K'}
#define OEM_STRING "QOEMHOOK"
#define OEM_STRING_ALT "SOMCHOOK"

#define QCRIL_EVT_HOOK_SET_ATEL_UI_STATUS 524314

#define RET_OK (0)
#define RET_NOTFOUND (1)
#define RET_INVARG (2)
#define RET_ERR (3)

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
  int ret;
} App;

typedef struct {
  char oem[8] RADIO_ALIGNED(8); /* "QOEMHOOK" (no NUL) */
  gint32 requestId
      RADIO_ALIGNED(4); /* QCRIL_EVT_HOOK_SET_ATEL_UI_STATUS (524314) */
  gint32 payloadLen RADIO_ALIGNED(4); /* length of following payload */
  gint8 isReady RADIO_ALIGNED(1);     /* 1 = ready, 0 = not ready */
} RADIO_ALIGNED(4) AtelReadyPayload;

static gint32 serial = 1; // first serial value
static const char pname[] = "fake-qcrilmsgtunnel";

static void app_dump_data(const GBinderReader *reader, const char *prefix) {
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

static gboolean app_signal(gpointer user_data) {
  App *app = user_data;

  GINFO("Caught signal, shutting down...");
  g_main_loop_quit(app->loop);
  return G_SOURCE_CONTINUE;
}

static void app_remote_died(GBinderRemoteObject *obj, void *user_data) {
  App *app = user_data;

  GINFO("Remote has died, exiting...");
  g_main_loop_quit(app->loop);
}

static gboolean app_connect_remote(App *app) {
  app->remote = gbinder_servicemanager_get_service_sync(
      app->sm, QCRILHOOK_FQNAME, NULL); /* autoreleased pointer */

  if (app->remote) {
    GINFO("Connected to %s", QCRILHOOK_FQNAME);
    gbinder_remote_object_ref(app->remote);
    app->client = gbinder_client_new(app->remote, QCRILHOOK_IFACE);
    app->death_id = gbinder_remote_object_add_death_handler(
        app->remote, app_remote_died, app);
    return TRUE;
  }
  return FALSE;
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
  if (resp_size > 0 && data_len < lenmin + *resp_size) {
    GERR("parse_oem_hook_message: data size is smaller (%zu) than expected (%zu)",
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
  GBinderReader reader;
  gbinder_remote_request_init_reader(req, &reader);

  GINFO("Response transaction %u received", code);
  app_dump_data(&reader, "    ");

  *status = GBINDER_STATUS_OK;
  return NULL;
}

static GBinderLocalReply *ind_tx_handler(GBinderLocalObject *obj,
                                         GBinderRemoteRequest *req, guint code,
                                         guint flags, int *status,
                                         void *user_data) {
  GBinderReader reader;
  const char *iface = gbinder_remote_request_interface(req);
  gbinder_remote_request_init_reader(req, &reader);

  GINFO("Indication transaction %u received", code);
  GINFO("iface: %s", iface ? iface : "unknown");

  // app_dump_data(&reader, "    ");

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
        GINFO("Received RIL_UNSOL_OEM_HOOK_RAW with resp_id=%d resp_siz=%d", resp_id, resp_size);
      else
        GINFO("Received unknown message");
      gutil_log_dump(&gutil_log_default, GLOG_LEVEL_DEFAULT,
                     "payload: ", resp_data, resp_size < 256 ? resp_size : 256);
    } else {
      GINFO("Failed to parse indication using RAW format. oem_id=%d. Ignoring "
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
static int send_atel_ready(App *app, int slot) {
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
  gbinder_writer_append_int32(&writer, serial++);
  gbinder_writer_append_hidl_vec(&writer, &payload, buflen, sizeof(gint8));

  int status = 0;

  GINFO("Sending ATEL ready (slot %d), buflen=%zu, transaction=%u", slot,
        buflen, TRANSACTION_OEMHOOK_RAW_REQUEST);

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

    /* Many HIDL calls return a tuple (int32 error, vec<uint8_t> data) or
       nothing. You can try to read an int32 return and/or a hidl vec. We'll
       just dump bytes if present. */
    gsize len = 0, elemsz = 0;
    const void *rdata = gbinder_reader_read_hidl_vec(&reader, &len, &elemsz);
    if (rdata && len > 0) {
      gutil_log_dump(&gutil_log_default, GLOG_LEVEL_DEFAULT,
                     "oemHookRawRequest reply payload: ", rdata,
                     (len * elemsz) < 256 ? (len * elemsz) : 256);
    } else {
      GINFO("oemHookRawRequest: no vec reply or zero length");
    }

    gbinder_remote_reply_unref(reply);
  }

  gbinder_local_request_unref(req);
  return 1;
}

static void app_registration_handler(GBinderServiceManager *sm,
                                     const char *name, void *user_data) {
  App *app = user_data;

  GDEBUG("\"%s\" appeared", name);
  if (!strcmp(name, QCRILHOOK_FQNAME) && app_connect_remote(app)) {
    gbinder_servicemanager_remove_handler(app->sm, app->wait_id);
    app->wait_id = 0;
  }
}

static int app_set_callback(App *app) {
  GBinderLocalRequest *req = gbinder_client_new_request(app->client);

  // write the two strong binder objects into the request:
  gbinder_local_request_append_local_object(req, app->resp);
  gbinder_local_request_append_local_object(req, app->ind);

  int status = 0;
  GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
      app->client, TRANSACTION_setCallback, req, &status);

  if (status == GBINDER_STATUS_OK) {
    GINFO("setCallback succeeded");
  } else {
    GERR("setCallback failed, status %d", status);
    return 0;
  }

  gbinder_remote_reply_unref(reply);
  gbinder_local_request_unref(req);
  return 1;
}

static void app_run(App *app) {
  guint sigtrm = g_unix_signal_add(SIGTERM, app_signal, app);
  guint sigint = g_unix_signal_add(SIGINT, app_signal, app);

  if (!app_connect_remote(app)) {
    GINFO("Waiting for %s", QCRILHOOK_FQNAME);
    app->wait_id = gbinder_servicemanager_add_registration_handler(
        app->sm, QCRILHOOK_FQNAME, app_registration_handler, app);
  }

  // init
  app->resp = gbinder_servicemanager_new_local_object(
      app->sm, QCRILHOOK_RESP_IFACE, resp_tx_handler, app);
  app->ind = gbinder_servicemanager_new_local_object(
      app->sm, QCRILHOOK_IND_IFACE, ind_tx_handler, app);

  // set callback
  if (app_set_callback(app)) {
    if (!send_atel_ready(app, 0)) {
      GERR("Failed to send ATEL ready");
    } else {
      GINFO("ATEL ready sent");
    }

    app->loop = g_main_loop_new(NULL, TRUE);
    app->ret = RET_OK;
    g_main_loop_run(app->loop);
  }

  g_source_remove(sigtrm);
  g_source_remove(sigint);
  g_main_loop_unref(app->loop);

  gbinder_remote_object_remove_handler(app->remote, app->death_id);
  gbinder_remote_object_unref(app->remote);
  gbinder_local_object_drop(app->local);
  gbinder_local_object_drop(app->resp);
  gbinder_local_object_drop(app->ind);
  gbinder_client_unref(app->client);
}

int main(int argc, char *argv[]) {
  App app;

  memset(&app, 0, sizeof(app));
  app.ret = RET_INVARG;

  gutil_log_timestamp = FALSE;
  gutil_log_set_type(GLOG_TYPE_STDERR, pname);
  gutil_log_default.level = GLOG_LEVEL_DEFAULT;

  app.sm = gbinder_servicemanager_new(DEVICE);
  if (app.sm) {
    app.local =
        gbinder_servicemanager_new_local_object(app.sm, NULL, NULL, NULL);
    app_run(&app);
    gbinder_servicemanager_unref(app.sm);
  }

  return app.ret;
}
