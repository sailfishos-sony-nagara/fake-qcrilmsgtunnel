/*
 * SIM unlock monitoring via ofono D-Bus interface
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SIM_MONITOR_H
#define SIM_MONITOR_H

#include <gio/gio.h>
#include <glib.h>

/**
 * Callback function called when SIM becomes unlocked
 * @param user_data: User data passed to sim_monitor_new()
 */
typedef void (*SimUnlockedCallback)(gpointer user_data);

/**
 * Callback function called when ofono service appears/disappears
 * @param available: TRUE when ofono becomes available, FALSE when it disappears
 * @param user_data: User data passed to sim_monitor_new()
 */
typedef void (*OfonoAvailabilityCallback)(gboolean available,
                                          gpointer user_data);

typedef struct sim_monitor {
  GDBusConnection *connection;
  SimUnlockedCallback sim_unlock_callback;
  OfonoAvailabilityCallback ofono_availability_callback;
  gpointer user_data;
  guint name_watcher_id;
  gboolean ofono_available;

  /* Single SIM state */
  guint sim_index;
  gchar *modem_path;
  gboolean is_unlocked;
  guint signal_id;
  gboolean monitoring;
} SimMonitor;

/**
 * Create new SIM monitor
 * @param sim_unlock_callback: Function to call when SIM becomes unlocked
 * @param ofono_availability_callback: Function to call when ofono
 * appears/disappears (can be NULL)
 * @param user_data: User data passed to callbacks
 * @return: SimMonitor instance or NULL on failure
 */
SimMonitor *
sim_monitor_new(SimUnlockedCallback sim_unlock_callback,
                OfonoAvailabilityCallback ofono_availability_callback,
                gpointer user_data);

/**
 * Start monitoring specific SIM slot
 * @param monitor: SimMonitor instance
 * @param sim_index: SIM slot to monitor (0, 1, 2, ...)
 * @return: TRUE on success, FALSE on failure
 */
gboolean sim_monitor_start(SimMonitor *monitor, guint sim_index);

/**
 * Stop monitoring SIM
 * @param monitor: SimMonitor instance
 */
void sim_monitor_stop(SimMonitor *monitor);

/**
 * Check current unlock status of monitored SIM
 * @param monitor: SimMonitor instance
 * @return: TRUE if SIM is unlocked, FALSE if locked, not monitoring, or ofono
 * unavailable
 */
gboolean sim_monitor_is_unlocked(SimMonitor *monitor);

/**
 * Free SimMonitor and cleanup resources
 * @param monitor: SimMonitor instance
 */
void sim_monitor_free(SimMonitor *monitor);

#endif