/*
 * SIM unlock monitoring via ofono D-Bus interface
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sim_monitor.h"
#include <gutil_log.h>

#define OFONO_SERVICE "org.ofono"
#define OFONO_MANAGER_PATH "/"
#define OFONO_MANAGER_IFACE "org.nemomobile.ofono.ModemManager"
#define OFONO_SIM_MANAGER_IFACE "org.ofono.SimManager"

static void on_ofono_name_appeared(GDBusConnection *connection,
                                   const gchar *name, const gchar *name_owner,
                                   gpointer user_data) {
  SimMonitor *monitor = user_data;

  GINFO("ofono service appeared (owner: %s)", name_owner);
  monitor->ofono_available = TRUE;

  if (monitor->ofono_availability_callback) {
    monitor->ofono_availability_callback(TRUE, monitor->user_data);
  }

  /* Start monitoring if we have a target SIM set */
  if (monitor->sim_index != UINT_MAX) {
    if (!sim_monitor_start(monitor, monitor->sim_index)) {
      GERR("Failed to start SIM monitoring after ofono appeared");
    }
  }
}

static void on_ofono_name_vanished(GDBusConnection *connection,
                                   const gchar *name, gpointer user_data) {
  SimMonitor *monitor = user_data;

  GINFO("ofono service vanished");
  monitor->ofono_available = FALSE;

  /* Stop monitoring */
  sim_monitor_stop(monitor);

  if (monitor->ofono_availability_callback) {
    monitor->ofono_availability_callback(FALSE, monitor->user_data);
  }
}

static gboolean sim_monitor_get_modem_path(SimMonitor *monitor, guint sim_index,
                                           gchar **modem_path) {
  GError *error = NULL;
  GVariant *result;

  /* Get available modems from ofono */
  result = g_dbus_connection_call_sync(
      monitor->connection, OFONO_SERVICE, OFONO_MANAGER_PATH,
      OFONO_MANAGER_IFACE, "GetAvailableModems", NULL, G_VARIANT_TYPE("(ao)"),
      G_DBUS_CALL_FLAGS_NONE, 5000, /* 5 second timeout */
      NULL, &error);

  if (!result) {
    GERR("Failed to get available modems: %s", error->message);
    g_error_free(error);
    return FALSE;
  }

  GVariantIter *iter;
  g_variant_get(result, "(ao)", &iter);

  gchar *path;
  guint index = 0;
  gboolean found = FALSE;

  while (g_variant_iter_loop(iter, "o", &path)) {
    if (index == sim_index) {
      *modem_path = g_strdup(path);
      found = TRUE;
      break;
    }
    index++;
  }

  g_variant_iter_free(iter);
  g_variant_unref(result);

  if (!found) {
    GERR("SIM index %u not found in available modems", sim_index);
    return FALSE;
  }

  GDEBUG("SIM %u mapped to modem path: %s", sim_index, *modem_path);
  return TRUE;
}

static gboolean sim_monitor_get_current_properties(SimMonitor *monitor) {
  GError *error = NULL;
  GVariant *result;

  /* Get current SIM properties */
  result = g_dbus_connection_call_sync(
      monitor->connection, OFONO_SERVICE, monitor->modem_path,
      OFONO_SIM_MANAGER_IFACE, "GetProperties", NULL, G_VARIANT_TYPE("(a{sv})"),
      G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);

  if (!result) {
    GERR("Failed to get SIM properties for %s: %s", monitor->modem_path,
         error->message);
    g_error_free(error);
    return FALSE;
  }

  GVariantIter *iter;
  g_variant_get(result, "(a{sv})", &iter);

  const gchar *key;
  GVariant *value;

  // several properties have to either exist or have specific value to
  // indicate that the card is unlocked. Otherwise false positive could
  // happen while card is not loaded into ofono
  gboolean has_cardidentifier = FALSE;
  gboolean has_nopin = FALSE;

  while (g_variant_iter_loop(iter, "{sv}", &key, &value)) {
    gchar *value_str = g_variant_print(value, TRUE);
    GINFO("Properties: %s -> %s", key, value_str);
    g_free(value_str);

    if (g_strcmp0(key, "CardIdentifier") == 0)
      has_cardidentifier = TRUE;
    else if (g_strcmp0(key, "PinRequired") == 0) {
      const gchar *pin_required = g_variant_get_string(value, NULL);
      has_nopin = (g_strcmp0(pin_required, "none") == 0);
    }
  }

  monitor->is_unlocked = (has_cardidentifier && has_nopin);
  GINFO("SIM %u current unlocked: %s", monitor->sim_index,
        monitor->is_unlocked ? "YES" : "NO");

  g_variant_iter_free(iter);
  g_variant_unref(result);
  return TRUE;
}

static void sim_monitor_property_changed(
    GDBusConnection *connection, const gchar *sender_name,
    const gchar *object_path, const gchar *interface_name,
    const gchar *signal_name, GVariant *parameters, gpointer user_data) {
  SimMonitor *monitor = user_data;

  if (g_strcmp0(interface_name, OFONO_SIM_MANAGER_IFACE) != 0 ||
      g_strcmp0(signal_name, "PropertyChanged") != 0) {
    return;
  }

  /* Check if this signal is for our monitored modem */
  if (!monitor->monitoring ||
      g_strcmp0(monitor->modem_path, object_path) != 0) {
    return;
  }

  const gchar *property_name;
  GVariant *property_value;
  g_variant_get(parameters, "(sv)", &property_name, &property_value);

  gchar *value_str = g_variant_print(property_value, TRUE);
  GINFO("SIM property changed: %s -> %s", property_name, value_str);
  g_free(value_str);

  if (g_strcmp0(property_name, "PinRequired") == 0) {
    gboolean was_unlocked = monitor->is_unlocked;

    // update unlock properties
    sim_monitor_get_current_properties(monitor);

    /* Call callback when SIM becomes unlocked */
    if (!was_unlocked && monitor->is_unlocked && monitor->sim_unlock_callback) {
      GINFO("SIM %u unlocked, calling callback", monitor->sim_index);
      monitor->sim_unlock_callback(monitor->user_data);
    }
  }

  g_variant_unref(property_value);
}

SimMonitor *
sim_monitor_new(SimUnlockedCallback sim_unlock_callback,
                OfonoAvailabilityCallback ofono_availability_callback,
                gpointer user_data) {
  GError *error = NULL;

  SimMonitor *monitor = g_new0(SimMonitor, 1);
  monitor->sim_unlock_callback = sim_unlock_callback;
  monitor->ofono_availability_callback = ofono_availability_callback;
  monitor->user_data = user_data;
  monitor->ofono_available = FALSE;
  monitor->sim_index = UINT_MAX; /* Invalid index initially */
  monitor->is_unlocked = FALSE;
  monitor->monitoring = FALSE;

  /* Connect to system D-Bus */
  monitor->connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!monitor->connection) {
    GERR("Failed to connect to system D-Bus: %s", error->message);
    g_error_free(error);
    sim_monitor_free(monitor);
    return NULL;
  }

  /* Watch for ofono service availability */
  monitor->name_watcher_id = g_bus_watch_name(
      G_BUS_TYPE_SYSTEM, OFONO_SERVICE, G_BUS_NAME_WATCHER_FLAGS_NONE,
      on_ofono_name_appeared, on_ofono_name_vanished, monitor, NULL);

  GINFO("SIM monitor initialized");
  return monitor;
}

gboolean sim_monitor_start(SimMonitor *monitor, guint sim_index) {
  if (!monitor)
    return FALSE;

  /* Stop any existing monitoring */
  sim_monitor_stop(monitor);

  /* Check if ofono is available */
  if (!monitor->ofono_available) {
    GDEBUG("ofono not available, storing target SIM index %u for later",
           sim_index);
    monitor->sim_index = sim_index;
    return TRUE; /* Will start when ofono appears */
  }

  monitor->sim_index = sim_index;

  /* Get modem path for this SIM index */
  if (!sim_monitor_get_modem_path(monitor, sim_index, &monitor->modem_path)) {
    return FALSE;
  }

  /* Get current properties */
  if (!sim_monitor_get_current_properties(monitor)) {
    GWARN("Could not get current properties for SIM %u, will monitor anyway",
          sim_index);
  }

  /* Subscribe to PropertyChanged signals */
  monitor->signal_id = g_dbus_connection_signal_subscribe(
      monitor->connection, OFONO_SERVICE, OFONO_SIM_MANAGER_IFACE,
      "PropertyChanged", monitor->modem_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
      sim_monitor_property_changed, monitor, NULL);

  if (monitor->signal_id == 0) {
    GERR("Failed to subscribe to PropertyChanged signals for SIM %u",
         sim_index);
    g_free(monitor->modem_path);
    monitor->modem_path = NULL;
    return FALSE;
  }

  monitor->monitoring = TRUE;

  GINFO("Started monitoring SIM %u (path: %s, currently %s)", sim_index,
        monitor->modem_path, monitor->is_unlocked ? "unlocked" : "locked");

  /* Call callback immediately if SIM is already unlocked */
  if (monitor->is_unlocked && monitor->sim_unlock_callback) {
    GINFO("SIM %u already unlocked, calling callback", sim_index);
    monitor->sim_unlock_callback(monitor->user_data);
  }

  return TRUE;
}

void sim_monitor_stop(SimMonitor *monitor) {
  if (!monitor || !monitor->monitoring)
    return;

  if (monitor->signal_id > 0) {
    g_dbus_connection_signal_unsubscribe(monitor->connection,
                                         monitor->signal_id);
    monitor->signal_id = 0;
  }

  g_free(monitor->modem_path);
  monitor->modem_path = NULL;
  monitor->is_unlocked = FALSE;
  monitor->monitoring = FALSE;

  GINFO("Stopped monitoring SIM %u", monitor->sim_index);
}

gboolean sim_monitor_is_unlocked(SimMonitor *monitor) {
  if (!monitor || !monitor->ofono_available || !monitor->monitoring)
    return FALSE;

  return monitor->is_unlocked;
}

void sim_monitor_free(SimMonitor *monitor) {
  if (!monitor)
    return;

  sim_monitor_stop(monitor);

  if (monitor->name_watcher_id > 0) {
    g_bus_unwatch_name(monitor->name_watcher_id);
  }

  if (monitor->connection) {
    g_object_unref(monitor->connection);
  }

  g_free(monitor);
  GINFO("SIM monitor freed");
}
