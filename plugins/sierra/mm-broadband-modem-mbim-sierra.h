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
 * Copyright (C) 2019 Daniele Palmas <dnlplm@gmail.com>
 */

#ifndef MM_BROADBAND_MODEM_MBIM_SIERRA_H
#define MM_BROADBAND_MODEM_MBIM_SIERRA_H

#include "mm-broadband-modem-mbim.h"

#define MM_TYPE_BROADBAND_MODEM_MBIM_SIERRA              (mm_broadband_modem_mbim_sierra_get_type ())
#define MM_BROADBAND_MODEM_MBIM_SIERRA(obj)              (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BROADBAND_MODEM_MBIM_SIERRA, MMBroadbandModemMbimSierra))
#define MM_BROADBAND_MODEM_MBIM_SIERRA_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_BROADBAND_MODEM_MBIM_SIERRA, MMBroadbandModemMbimSierraClass))
#define MM_IS_BROADBAND_MODEM_MBIM_SIERRA(obj)           (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BROADBAND_MODEM_MBIM_SIERRA))
#define MM_IS_BROADBAND_MODEM_MBIM_SIERRA_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_BROADBAND_MODEM_MBIM_SIERRA))
#define MM_BROADBAND_MODEM_MBIM_SIERRA_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_BROADBAND_MODEM_MBIM_SIERRA, MMBroadbandModemMbimSierraClass))

typedef struct _MMBroadbandModemMbimSierra MMBroadbandModemMbimSierra;
typedef struct _MMBroadbandModemMbimSierraClass MMBroadbandModemMbimSierraClass;
typedef struct _MMBroadbandModemMbimSierraPrivate MMBroadbandModemMbimSierraPrivate;

struct _MMBroadbandModemMbimSierra {
    MMBroadbandModemMbim parent;
    MMBroadbandModemMbimSierraPrivate *priv;
};

struct _MMBroadbandModemMbimSierraClass{
    MMBroadbandModemMbimClass parent;
};

GType mm_broadband_modem_mbim_sierra_get_type (void);

MMBroadbandModemMbimSierra *mm_broadband_modem_mbim_sierra_new (const gchar  *device,
                                                              const gchar **drivers,
                                                              const gchar  *plugin,
                                                              guint16       vendor_id,
                                                              guint16       product_id);

#endif /* MM_BROADBAND_MODEM_SIERRA_H */
