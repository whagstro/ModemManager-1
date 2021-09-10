/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (c) 2021 Digi International Inc.
 */

#include <config.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "ModemManager.h"
#include "mm-log-object.h"
#include "mm-modem-helpers.h"
#include "mm-iface-modem.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem-signal.h"
#include "mm-broadband-modem-mbim-sierra.h"
#include "mm-iface-modem-messaging.h"
#include "mbim-message.h"
#include "mm-log.h"

static void iface_modem_messaging_init (MMIfaceModemMessaging *iface);
static void iface_modem_signal_init (MMIfaceModemSignal *iface);

static MMIfaceModemSignal *iface_signal_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemMbimSierra, mm_broadband_modem_mbim_sierra, MM_TYPE_BROADBAND_MODEM_MBIM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_MESSAGING, iface_modem_messaging_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_SIGNAL, iface_modem_signal_init))

typedef struct {
    MMSignal *umts;
    MMSignal *lte;
} DetailedSignal;

struct _MMBroadbandModemMbimSierraPrivate {
    DetailedSignal detailed_signal;
};

static void
detailed_signal_clear (DetailedSignal *signal)
{
    g_clear_object (&signal->umts);
    g_clear_object (&signal->lte);
}

static void
detailed_signal_free (DetailedSignal *signal)
{
    detailed_signal_clear (signal);
    g_slice_free (DetailedSignal, signal);
}

/*****************************************************************************/
/* signal_load_values (Signal interface) */
/* GSTATUS Possible Responses:
* 4G
*
* Current Time:  675              Temperature: 32
* Modem Mitigate Level: 0         ModemProc Mitigate Level: 0
* Reset Counter: 1                Mode:        ONLINE
* System mode:   LTE              PS state:    Attached
* LTE band:      B7               LTE bw:      20 MHz
* LTE Rx chan:   3350             LTE Tx chan: 21350
* EMM state:     Registered       Normal Service
* RRC state:     RRC Idle
* IMS reg state: NOT REGISTERED   IMS mode:    Normal
* IMS Srv State: UNKNOWN SMS,UNKNOWN VoIP
*
* PCC RxM RSSI:  -46              PCC RxM RSRP:  -72
* PCC RxD RSSI:  -45              PCC RxD RSRP:  -71
* Tx Power:      --               TAC:         0001 (1)
* RSRQ (dB):     -6.0             Cell ID:     01a2d001 (27447297)
* SINR (dB):     25.4
*
* NR5G band:       ---            NR5G bw:         ---
* NR5G Rx chan:    ---            NR5G Tx chan:    ---
* NR5G RSRP (dBm): ---            NR5G RSRQ (dB):  ---
* NR5G SINR (dB):  ---            '"'"
*
*/
static gboolean
signal_load_values_finish (MMIfaceModemSignal *self,
                           GAsyncResult *res,
                           MMSignal **cdma,
                           MMSignal **evdo,
                           MMSignal **gsm,
                           MMSignal **umts,
                           MMSignal **lte,
                           GError **error)
{
    mm_obj_dbg (self, "GSTATUS signal_load_values_finish...");
    DetailedSignal *signals;

    signals = g_task_propagate_pointer (G_TASK (res), error);
    if (!signals)
        return FALSE;

    *umts = signals->umts ? g_object_ref (signals->umts) : NULL;
    *lte  = signals->lte ? g_object_ref (signals->lte) : NULL;

    if (gsm)
        *gsm = NULL;
    if (cdma)
        *cdma = NULL;
    if (evdo)
        *evdo = NULL;

    detailed_signal_free (signals);
    return TRUE;
}

// Get the value after the keyword
gchar *get_gstatus_field(MMBroadbandModemMbimSierra *self, const gchar *response, const gchar *keyword,
                         const gchar *format)
{
    GRegex *r;
    GMatchInfo *match_info;
    GError *inner_error = NULL;
    gchar regex[256];
    gchar *value = NULL;

    sprintf (regex, "%s:\\s+(%s)", keyword, format);

    mm_obj_dbg (self, "%s: using regex [%s]", __func__, regex);

    r = g_regex_new (regex, 0, 0, NULL);
    g_assert (r != NULL);
    g_regex_match_full (r, response, strlen (response), 0, 0, &match_info, &inner_error);

    if (inner_error) {
        mm_obj_dbg (self, "%s: GSTATUS inner error", __func__ );
        goto finish;
    }

    if (!g_match_info_matches (match_info)) {
        mm_obj_dbg (self, "%s Couldn't get value", __func__);
        goto finish;
    }

    value = mm_get_string_unquoted_from_match_info (match_info, 1);
    if (value) {
        mm_obj_dbg (self, "Got GSTATUS value [%s] = [%s]", keyword, value);
    }
    else {
        mm_obj_dbg (self, "Could not get GSTATUS keyword = [%s]", value);
    }

finish:
    if (match_info)
        g_match_info_free (match_info);
    if (inner_error)
        g_error_free (inner_error);

    return value;
}


static void update_nr5g_signal(const gchar *response, MMBroadbandModemMbimSierra *self) {
    gdouble rsrp;
    gdouble rsrq;
    gdouble sinr;
    gdouble rssi;
    gchar *tmp;

    /* TODO: HACK store 5G signal info in the LTE structure until we can upgrade ModemManger to get 5G support */
    self->priv->detailed_signal.lte = mm_signal_new ();

    tmp = get_gstatus_field (self, response, "NR5G\\(sub6\\) RxM RSSI \\(dbm\\)", "-?\\d+");
    if (tmp) {
        if (mm_get_double_from_str (tmp, &rssi)) {
            mm_obj_dbg (self, "RSSI is [%f]", rssi);
            mm_signal_set_rsrp (self->priv->detailed_signal.lte, rssi);
        }
    }

    tmp = get_gstatus_field (self, response, "NR5G RSRP \\(dBm\\)", "-?\\d+");
    if (tmp) {
        if (mm_get_double_from_str (tmp, &rsrp)) {
            mm_obj_dbg (self, "RSRP is [%f]", rsrp);
            mm_signal_set_rsrp (self->priv->detailed_signal.lte, rsrp);
        }
    }

    tmp = get_gstatus_field(self, response, "NR5G RSRQ \\(dB\\)", "-?\\d+");
    if (tmp) {
        if (mm_get_double_from_str (tmp, &rsrq)) {
            mm_obj_dbg (self, "RSRQ is [%f]", rsrq);
            mm_signal_set_rsrp (self->priv->detailed_signal.lte, rsrq);
        }
    }

    tmp = get_gstatus_field(self, response, "NR5G SINR \\(dB\\)", "-?\\d+");
    if (tmp) {
        if (mm_get_double_from_str (tmp, &sinr)) {
            mm_obj_dbg (self, "SINR is [%f]", sinr);
            mm_signal_set_sinr (self->priv->detailed_signal.lte, sinr);
        }
    }
}

static void update_lte_signal(const gchar *response, MMBroadbandModemMbimSierra *self) {
    gdouble rsrp;
    gdouble rsrq;
    gdouble sinr;
    gdouble rssi;
    gchar *tmp;

    self->priv->detailed_signal.lte = mm_signal_new ();

    /*
     * Note: in GSTATUS?  PCC RxM indicates the value at the primary input, RxD is the
     * secondary input, so we get the values for the primary port.
     */
    tmp = get_gstatus_field (self, response, "PCC RxM RSRP", "-?\\d+");
    if (tmp) {
        if (mm_get_double_from_str (tmp, &rsrp)) {
            mm_obj_dbg (self, "RSRP is [%f]", rsrp);
            mm_signal_set_rsrp (self->priv->detailed_signal.lte, rsrp);
        }
    }

    tmp = get_gstatus_field (self, response, "PCC RxM RSSI", "-?\\d+");
    if (tmp) {
        if (mm_get_double_from_str (tmp, &rssi)) {
            mm_obj_dbg (self, "RSSI is [%f]", rssi);
            mm_signal_set_rssi (self->priv->detailed_signal.lte, rssi);
        }
    }

    tmp = get_gstatus_field (self, response, "RSRQ \\(dB\\)", "-?\\d+");
    if (tmp) {
        if (mm_get_double_from_str (tmp, &rsrq)) {
            mm_obj_dbg (self, "RSRQ is [%f]", rsrq);
            mm_signal_set_rsrq (self->priv->detailed_signal.lte, rsrq);
        }
    }

    tmp = get_gstatus_field (self, response, "SINR \\(dB\\)", "-?\\d+");
    if (tmp) {
        if (mm_get_double_from_str (tmp, &sinr)) {
            mm_obj_dbg (self, "SINR is [%f]", sinr);
            mm_signal_set_sinr (self->priv->detailed_signal.lte, sinr);
        }
    }
}

static void update_3g_signal(const gchar *response, MMBroadbandModemMbimSierra *self) {
    gdouble rssi;
    gchar *tmp;

    self->priv->detailed_signal.umts = mm_signal_new ();

    tmp = get_gstatus_field (self, response, "RxM RSSI C0", "-?\\d+");
    if (tmp) {
        if (mm_get_double_from_str (tmp, &rssi)) {
            mm_obj_dbg (self, "RSSI is [%f]", rssi);
            mm_signal_set_rssi (self->priv->detailed_signal.umts, rssi);
        }
    }
}

static void
gstatus_ready (MMBaseModem *_self,
              GAsyncResult *res,
              GTask *task)
    {
    MMBroadbandModemMbimSierra *self = MM_BROADBAND_MODEM_MBIM_SIERRA (_self);
    const gchar *response;
    GError *error = NULL;
    gchar *mode;
    DetailedSignal *signals;

    mm_obj_dbg (self, "Getting GSTATUS response gstatus_ready...");

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (_self), res, &error);
    if (error || !response) {
        mm_obj_dbg (self, "!GSTATUS failed: %s", error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mode = get_gstatus_field (self, response, "System mode", "\\w+");
    if (!mode) {
        mm_obj_dbg (self, "!GSTATUS could not find System mode");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /*
     * In ENDC mode which is NR5G-NSA, return just the LTE signal values
     * When ModemManager is updated we can return both
     */
    if (!strcmp (mode, "LTE") || !strcmp (mode, "ENDC")) {
        update_lte_signal (response, self);
    } else if (!strcmp (mode, "NR5G")) {
        update_nr5g_signal (response, self);
    } else if  (!strcmp (mode, "WCDMA")) {
        update_3g_signal (response, self);
    } else {
        mm_obj_dbg (self, "Unrecognized system mode [%s]", mode);
        g_task_return_error (task, error);
        g_object_unref (task);
    }

    signals = g_slice_new0 (DetailedSignal);
    signals->lte = self->priv->detailed_signal.lte ? g_object_ref (self->priv->detailed_signal.lte) : NULL;
    signals->umts = self->priv->detailed_signal.umts ? g_object_ref (self->priv->detailed_signal.umts) : NULL;
    g_task_return_pointer (task, signals, (GDestroyNotify)detailed_signal_free);
    g_object_unref (task);

    return;
}

static void
signal_load_values (MMIfaceModemSignal *_self,
                    GCancellable *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    MMBroadbandModemMbimSierra *self = MM_BROADBAND_MODEM_MBIM_SIERRA (_self);
    GTask *task;

    task = g_task_new (self, cancellable, callback, user_data);

    /* Clear any previous detailed signal values to get new ones */
    detailed_signal_clear (&self->priv->detailed_signal);

    mm_obj_dbg (_self, "***signal_load_values: Getting GSTATUS***");
    mm_base_modem_at_command (MM_BASE_MODEM (_self),
                              "!GSTATUS?",
                              20,
                              FALSE,
                              (GAsyncReadyCallback)gstatus_ready,
                              task);
}

/* Enabling unsolicited events (3GPP interface) */

static gboolean
messaging_enable_unsolicited_events_finish (MMIfaceModemMessaging  *_self,
                                             GAsyncResult      *res,
                                             GError           **error)
{
    return !!mm_base_modem_at_command_finish (MM_BASE_MODEM (_self), res, error);
}


static void
messaging_enable_unsolicited_events (MMIfaceModemMessaging    *_self,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
    mm_base_modem_at_command (MM_BASE_MODEM (_self),
                            "AT+CNMI=1,1,0,2,0",
                            10,
                            FALSE,
                            callback,
                            user_data);
}

static void
iface_modem_messaging_init (MMIfaceModemMessaging *iface)
{
    iface->enable_unsolicited_events = messaging_enable_unsolicited_events;
    iface->enable_unsolicited_events_finish = messaging_enable_unsolicited_events_finish;
}


/*****************************************************************************/

MMBroadbandModemMbimSierra *
mm_broadband_modem_mbim_sierra_new (const gchar  *device,
                                   const gchar **drivers,
                                   const gchar  *plugin,
                                   guint16       vendor_id,
                                   guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_MBIM_SIERRA,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         MM_IFACE_MODEM_SIM_HOT_SWAP_SUPPORTED, TRUE,
                         MM_IFACE_MODEM_SIM_HOT_SWAP_CONFIGURED, FALSE,
                         NULL);
}

static void
dispose (GObject *object)
{
    MMBroadbandModemMbimSierra *self = MM_BROADBAND_MODEM_MBIM_SIERRA (object);

    detailed_signal_clear (&self->priv->detailed_signal);

    G_OBJECT_CLASS (mm_broadband_modem_mbim_sierra_parent_class)->dispose (object);
}

static void
iface_modem_signal_init (MMIfaceModemSignal *iface)
{
    iface_signal_parent = g_type_interface_peek_parent (iface);
    iface->load_values = signal_load_values;
    iface->load_values_finish = signal_load_values_finish;
}

static void
mm_broadband_modem_mbim_sierra_init (MMBroadbandModemMbimSierra *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_BROADBAND_MODEM_MBIM_SIERRA,
                                              MMBroadbandModemMbimSierraPrivate);

}

static void
mm_broadband_modem_mbim_sierra_class_init (MMBroadbandModemMbimSierraClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    g_type_class_add_private (object_class, sizeof (MMBroadbandModemMbimSierraPrivate));
    object_class->dispose = dispose;
}
