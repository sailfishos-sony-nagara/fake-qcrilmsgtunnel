/*
 * Copyright (C) 2018 Jolla Ltd.
 * Copyright (C) 2018 Slava Monich <slava.monich@jolla.com>
 * Copyright (C) 2025 Rinigus https://github.com/rinigus
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tunnel.h"

#include <gutil_log.h>

#include <glib-unix.h>

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