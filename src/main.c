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

#define DEVICE_DEFAULT "/dev/hwbinder"
#define QCRILHOOK_NAME_DEFAULT "oemhook0"
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

static gint32 global_serial = 1; // first serial value
static const char pname[] = "fake-qcrilmsgtunnel";

// Command line options
static char *opt_device = NULL;
static char *opt_interface = NULL;
static char *opt_name = NULL;
static gboolean opt_verbose = FALSE;

static GOptionEntry option_entries[] = {
    {"device", 'd', 0, G_OPTION_ARG_STRING, &opt_device,
     "Binder device path (default: " DEVICE_DEFAULT ")", "PATH"},
    {"interface", 'i', 0, G_OPTION_ARG_STRING, &opt_interface,
     "HIDL/AIDL interface name (default: " QCRILHOOK_IFACE_DEFAULT ")",
     "INTERFACE"},
    {"name", 'n', 0, G_OPTION_ARG_STRING, &opt_name,
     "Service name (default: " QCRILHOOK_NAME_DEFAULT ")", "NAME"},
    {"verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose,
     "Enable verbose logging", NULL},
    {NULL}};

static void app_config_init(AppConfig *config) {
  config->device = g_strdup(opt_device ? opt_device : DEVICE_DEFAULT);
  config->interface =
      g_strdup(opt_interface ? opt_interface : QCRILHOOK_IFACE_DEFAULT);
  config->name = g_strdup(opt_name ? opt_name : QCRILHOOK_NAME_DEFAULT);

  // Build FQNAME from interface and name
  config->fqname = g_strdup_printf("%s/%s", config->interface, config->name);

  // Build used interfaces
  config->resp_iface = g_strdup_printf("%sResponse", config->interface);
  config->ind_iface = g_strdup_printf("%sIndication", config->interface);

  GINFO("Configuration:");
  GINFO("  Device: %s", config->device);
  GINFO("  Interface: %s", config->interface);
  GINFO("  Name: %s", config->name);
  GINFO("  FQNAME: %s", config->fqname);
  GINFO("  Response Interface: %s", config->resp_iface);
  GINFO("  Indication Interface: %s", config->ind_iface);
}

static void app_config_cleanup(AppConfig *config) {
  g_free(config->device);
  g_free(config->interface);
  g_free(config->name);
  g_free(config->fqname);
  g_free(config->resp_iface);
  g_free(config->ind_iface);
}

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
      app->sm, app->config.fqname, NULL); /* autoreleased pointer */

  if (app->remote) {
    GINFO("Connected to %s", app->config.fqname);
    gbinder_remote_object_ref(app->remote);
    app->client = gbinder_client_new(app->remote, app->config.interface);
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
  // app_dump_data(&reader, "    ");

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

  // app_dump_data(&reader, "ind    ");

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
static int send_atel_ready(App *app) {
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
  if (!strcmp(name, app->config.fqname) && app_connect_remote(app)) {
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
    GINFO("Waiting for %s", app->config.fqname);
    app->wait_id = gbinder_servicemanager_add_registration_handler(
        app->sm, app->config.fqname, app_registration_handler, app);
  }

  // init
  app->resp = gbinder_servicemanager_new_local_object(
      app->sm, app->config.resp_iface, resp_tx_handler, app);
  app->ind = gbinder_servicemanager_new_local_object(
      app->sm, app->config.ind_iface, ind_tx_handler, app);

  // set callback
  if (app_set_callback(app)) {
    if (!send_atel_ready(app)) {
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

static gboolean parse_options(int argc, char *argv[]) {
  GError *error = NULL;
  GOptionContext *context;

  context = g_option_context_new("- QCom RIL message tunnel");
  g_option_context_add_main_entries(context, option_entries, NULL);
  g_option_context_set_summary(context,
                               "Fake QCRil message tunnel for gbinder "
                               "communication with QCom RIL services.");

  if (!g_option_context_parse(context, &argc, &argv, &error)) {
    g_printerr("Option parsing failed: %s\n", error->message);
    g_error_free(error);
    g_option_context_free(context);
    return FALSE;
  }

  g_option_context_free(context);
  return TRUE;
}

int main(int argc, char *argv[]) {
  App app;

  memset(&app, 0, sizeof(app));
  app.ret = RET_INVARG;

  // Parse command line options
  if (!parse_options(argc, argv)) {
    return RET_INVARG;
  }

  // Initialize configuration from parsed options
  app_config_init(&app.config);

  gutil_log_timestamp = FALSE;
  gutil_log_set_type(GLOG_TYPE_STDERR, app.config.name);
  gutil_log_default.level =
      opt_verbose ? GLOG_LEVEL_VERBOSE : GLOG_LEVEL_DEFAULT;

  app.sm = gbinder_servicemanager_new(app.config.device);
  if (app.sm) {
    app.local =
        gbinder_servicemanager_new_local_object(app.sm, NULL, NULL, NULL);
    app_run(&app);
    gbinder_servicemanager_unref(app.sm);
    // Cleanup configuration
    app_config_cleanup(&app.config);
  } else {
    GERR("Failed to create service manager for device: %s", app.config.device);
    app.ret = RET_ERR;
  }

  return app.ret;
}