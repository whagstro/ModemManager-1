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
 * Copyright (C) 2016 Velocloud, Inc.
 * Copyright (C) 2020 Aleksander Morgado <aleksander@aleksander.es>
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include <ModemManager-tags.h>

#include "mm-kernel-device-generic.h"
#include "mm-kernel-device-generic-rules.h"
#include "mm-log-object.h"

#if !defined UDEVRULESDIR
# error UDEVRULESDIR is not defined
#endif

static void initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_EXTENDED (MMKernelDeviceGeneric, mm_kernel_device_generic,  MM_TYPE_KERNEL_DEVICE, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init))

enum {
    PROP_0,
    PROP_PROPERTIES,
    PROP_RULES,
    PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

struct _MMKernelDeviceGenericPrivate {
    /* Input properties */
    MMKernelEventProperties *properties;
    /* Rules to apply */
    GArray *rules;

    /* Contents from sysfs */
    gchar   *driver;
    gchar   *sysfs_path;
    gchar   *interface_sysfs_path;
    guint8   interface_class;
    guint8   interface_subclass;
    guint8   interface_protocol;
    guint8   interface_number;
    gchar   *interface_description;
    gchar   *physdev_sysfs_path;
    guint16  physdev_vid;
    guint16  physdev_pid;
    guint16  physdev_revision;
    gchar   *physdev_subsystem;
    gchar   *physdev_manufacturer;
    gchar   *physdev_product;
};

static gboolean
has_sysfs_attribute (const gchar *path,
                     const gchar *attribute)
{
    g_autofree gchar *aux_filepath = NULL;

    aux_filepath = g_strdup_printf ("%s/%s", path, attribute);
    return g_file_test (aux_filepath, G_FILE_TEST_EXISTS);
}

static gchar *
read_sysfs_attribute_as_string (const gchar *path,
                                const gchar *attribute)
{
    g_autofree gchar *aux = NULL;
    gchar            *contents = NULL;

    aux = g_strdup_printf ("%s/%s", path, attribute);
    if (g_file_get_contents (aux, &contents, NULL, NULL)) {
        g_strdelimit (contents, "\r\n", ' ');
        g_strstrip (contents);
    }
    return contents;
}

static guint
read_sysfs_attribute_as_hex (const gchar *path,
                             const gchar *attribute)
{
    g_autofree gchar *contents = NULL;
    guint             val = 0;

    contents = read_sysfs_attribute_as_string (path, attribute);
    if (contents)
        mm_get_uint_from_hex_str (contents, &val);
    return val;
}

static gchar *
read_sysfs_attribute_link_basename (const gchar *path,
                                    const gchar *attribute)
{
    g_autofree gchar *aux_filepath = NULL;
    g_autofree gchar *canonicalized_path = NULL;

    aux_filepath = g_strdup_printf ("%s/%s", path, attribute);
    if (!g_file_test (aux_filepath, G_FILE_TEST_EXISTS))
        return NULL;

    canonicalized_path = realpath (aux_filepath, NULL);
    return g_path_get_basename (canonicalized_path);
}

/*****************************************************************************/
/* Load contents */

static void
preload_sysfs_path (MMKernelDeviceGeneric *self)
{
    g_autofree gchar *tmp = NULL;

    if (self->priv->sysfs_path)
        return;

    /* sysfs can be built directly using subsystem and name; e.g. for subsystem
     * usbmisc and name cdc-wdm0:
     *    $ realpath /sys/class/usbmisc/cdc-wdm0
     *    /sys/devices/pci0000:00/0000:00:1d.0/usb4/4-1/4-1.3/4-1.3:1.8/usbmisc/cdc-wdm0
     */
    tmp = g_strdup_printf ("/sys/class/%s/%s",
                           mm_kernel_event_properties_get_subsystem (self->priv->properties),
                           mm_kernel_event_properties_get_name      (self->priv->properties));

    self->priv->sysfs_path = realpath (tmp, NULL);
    if (!self->priv->sysfs_path || !g_file_test (self->priv->sysfs_path, G_FILE_TEST_EXISTS)) {
        mm_obj_warn (self, "invalid sysfs path read for %s/%s",
                     mm_kernel_event_properties_get_subsystem (self->priv->properties),
                     mm_kernel_event_properties_get_name      (self->priv->properties));
        g_clear_pointer (&self->priv->sysfs_path, g_free);
    }

    if (self->priv->sysfs_path) {
        const gchar *devpath;

        mm_obj_dbg (self, "sysfs path: %s", self->priv->sysfs_path);
        devpath = (g_str_has_prefix (self->priv->sysfs_path, "/sys") ?
                   &self->priv->sysfs_path[4] :
                   self->priv->sysfs_path);
        g_object_set_data_full (G_OBJECT (self), "DEVPATH", g_strdup (devpath), g_free);
    }
}

/*****************************************************************************/

static void
preload_common_properties (MMKernelDeviceGeneric *self)
{
    if (self->priv->interface_sysfs_path) {
        mm_obj_dbg (self, "  ID_USB_INTERFACE_NUM: 0x%02x", self->priv->interface_number);
        g_object_set_data_full (G_OBJECT (self), "ID_USB_INTERFACE_NUM", g_strdup_printf ("%02x", self->priv->interface_number), g_free);
    }

    if (self->priv->physdev_product) {
        mm_obj_dbg (self, "  ID_MODEL: %s", self->priv->physdev_product);
        g_object_set_data_full (G_OBJECT (self), "ID_MODEL", g_strdup (self->priv->physdev_product), g_free);
    }

    if (self->priv->physdev_manufacturer) {
        mm_obj_dbg (self, "  ID_VENDOR: %s", self->priv->physdev_manufacturer);
        g_object_set_data_full (G_OBJECT (self), "ID_VENDOR", g_strdup (self->priv->physdev_manufacturer), g_free);
    }

    if (self->priv->physdev_sysfs_path) {
        mm_obj_dbg (self, "  ID_VENDOR_ID: 0x%04x", self->priv->physdev_vid);
        g_object_set_data_full (G_OBJECT (self), "ID_VENDOR_ID", g_strdup_printf ("%04x", self->priv->physdev_vid), g_free);
        mm_obj_dbg (self, "  ID_MODEL_ID: 0x%04x", self->priv->physdev_pid);
        g_object_set_data_full (G_OBJECT (self), "ID_MODEL_ID", g_strdup_printf ("%04x", self->priv->physdev_pid), g_free);
        mm_obj_dbg (self, "  ID_REVISION: 0x%04x", self->priv->physdev_revision);
        g_object_set_data_full (G_OBJECT (self), "ID_REVISION", g_strdup_printf ("%04x", self->priv->physdev_revision), g_free);
    }
}

static void
preload_contents_other (MMKernelDeviceGeneric *self)
{
    /* For any other kind of bus (or the absence of one, as in virtual devices),
     * assume this is a single port device and don't try to match multiple ports
     * together. Also, obviously, no vendor, product, revision or interface. */
    self->priv->driver = read_sysfs_attribute_link_basename (self->priv->sysfs_path, "driver");
}

static void
preload_contents_platform (MMKernelDeviceGeneric *self,
                           const gchar           *platform)
{
    g_autofree gchar *iter = NULL;

    iter = g_strdup (self->priv->sysfs_path);
    while (iter && (g_strcmp0 (iter, "/") != 0)) {
        gchar       *parent;
        const gchar *current_subsystem;

        /* Store the first driver found */
        if (!self->priv->driver)
            self->priv->driver = read_sysfs_attribute_link_basename (iter, "driver");

        /* Take first parent with the given platform subsystem as physical device */
        current_subsystem = read_sysfs_attribute_link_basename (iter, "subsystem");
        if (!self->priv->physdev_sysfs_path && (g_strcmp0 (current_subsystem, platform) == 0)) {
            self->priv->physdev_sysfs_path = g_strdup (iter);
            /* stop traversing as soon as the physical device is found */
            break;
        }

        parent = g_path_get_dirname (iter);
        g_clear_pointer (&iter, g_free);
        iter = parent;
    }
}

static void
preload_contents_pcmcia (MMKernelDeviceGeneric *self)
{
    g_autofree gchar *iter = NULL;
    gboolean          pcmcia_subsystem_found = FALSE;

    iter = g_strdup (self->priv->sysfs_path);
    while (iter && (g_strcmp0 (iter, "/") != 0)) {
        g_autofree gchar *subsys = NULL;
        g_autofree gchar *parent = NULL;
        g_autofree gchar *parent_subsys = NULL;

        /* Store the first driver found */
        if (!self->priv->driver)
            self->priv->driver = read_sysfs_attribute_link_basename (iter, "driver");

        subsys = read_sysfs_attribute_link_basename (iter, "subsystem");
        if (g_strcmp0 (subsys, "pcmcia") == 0)
            pcmcia_subsystem_found = TRUE;

        parent = g_path_get_dirname (iter);
        if (parent)
            parent_subsys = read_sysfs_attribute_link_basename (parent, "subsystem");

        if (pcmcia_subsystem_found  && parent_subsys && (g_strcmp0 (parent_subsys, "pcmcia") != 0)) {
            self->priv->physdev_sysfs_path = g_strdup (iter);
            self->priv->physdev_vid = read_sysfs_attribute_as_hex (self->priv->physdev_sysfs_path, "manf_id");
            self->priv->physdev_pid = read_sysfs_attribute_as_hex (self->priv->physdev_sysfs_path, "card_id");
            /* stop traversing as soon as the physical device is found */
            break;
        }

        g_clear_pointer (&iter, g_free);
        iter = g_steal_pointer (&parent);
    }
}

static void
preload_contents_pci (MMKernelDeviceGeneric *self)
{
    g_autofree gchar *iter = NULL;

    iter = g_strdup (self->priv->sysfs_path);
    while (iter && (g_strcmp0 (iter, "/") != 0)) {
        g_autofree gchar *subsys = NULL;
        gchar            *parent;

        /* Store the first driver found */
        if (!self->priv->driver)
            self->priv->driver = read_sysfs_attribute_link_basename (iter, "driver");

        /* the PCI channel specific devices have their own drivers and
         * subsystems, we can rely on the physical device being the first
         * one that reports the 'pci' subsystem */
        subsys = read_sysfs_attribute_link_basename (iter, "subsystem");
        if (!self->priv->physdev_sysfs_path && (g_strcmp0 (subsys, "pci") == 0)) {
            self->priv->physdev_sysfs_path = g_strdup (iter);
            self->priv->physdev_vid = read_sysfs_attribute_as_hex (self->priv->physdev_sysfs_path, "vendor");
            self->priv->physdev_pid = read_sysfs_attribute_as_hex (self->priv->physdev_sysfs_path, "device");
            self->priv->physdev_revision = read_sysfs_attribute_as_hex (self->priv->physdev_sysfs_path, "revision");
            self->priv->physdev_subsystem = g_steal_pointer (&subsys);
            /* stop traversing as soon as the physical device is found */
            break;
        }

        parent = g_path_get_dirname (iter);
        g_clear_pointer (&iter, g_free);
        iter = parent;
    }
}

static void
preload_contents_usb (MMKernelDeviceGeneric *self)
{
    g_autofree gchar *iter = NULL;

    iter = g_strdup (self->priv->sysfs_path);
    while (iter && (g_strcmp0 (iter, "/") != 0)) {
        gchar *parent;

        /* is this the USB interface? */
        if (!self->priv->interface_sysfs_path && has_sysfs_attribute (iter, "bInterfaceClass")) {
            self->priv->interface_sysfs_path = g_strdup (iter);
            self->priv->interface_class = read_sysfs_attribute_as_hex (self->priv->interface_sysfs_path, "bInterfaceClass");
            self->priv->interface_subclass = read_sysfs_attribute_as_hex (self->priv->interface_sysfs_path, "bInterfaceSubClass");
            self->priv->interface_protocol = read_sysfs_attribute_as_hex (self->priv->interface_sysfs_path, "bInterfaceProtocol");
            self->priv->interface_number = read_sysfs_attribute_as_hex (self->priv->interface_sysfs_path, "bInterfaceNumber");
            self->priv->interface_description = read_sysfs_attribute_as_string (self->priv->interface_sysfs_path, "interface");
            self->priv->driver = read_sysfs_attribute_link_basename (self->priv->interface_sysfs_path, "driver");
        }
        /* is this the USB physdev? */
        else if (!self->priv->physdev_sysfs_path && has_sysfs_attribute (iter, "idVendor")) {
            self->priv->physdev_sysfs_path = g_strdup (iter);
            self->priv->physdev_vid = read_sysfs_attribute_as_hex (self->priv->physdev_sysfs_path, "idVendor");
            self->priv->physdev_pid = read_sysfs_attribute_as_hex (self->priv->physdev_sysfs_path, "idProduct");
            self->priv->physdev_revision = read_sysfs_attribute_as_hex (self->priv->physdev_sysfs_path, "bcdDevice");
            self->priv->physdev_subsystem = read_sysfs_attribute_link_basename (self->priv->physdev_sysfs_path, "subsystem");
            self->priv->physdev_manufacturer = read_sysfs_attribute_as_string (self->priv->physdev_sysfs_path, "manufacturer");
            self->priv->physdev_product = read_sysfs_attribute_as_string (self->priv->physdev_sysfs_path, "product");
            /* stop traversing as soon as the physical device is found */
            break;
        }

        parent = g_path_get_dirname (iter);
        g_clear_pointer (&iter, g_free);
        iter = parent;
    }
}

static gchar *
find_device_bus_subsystem (MMKernelDeviceGeneric *self)
{
    g_autofree gchar *iter = NULL;

    iter = g_strdup (self->priv->sysfs_path);
    while (iter && (g_strcmp0 (iter, "/") != 0)) {
        g_autofree gchar *subsys = NULL;
        gchar            *parent;

        subsys = read_sysfs_attribute_link_basename (iter, "subsystem");

        /* stop search as soon as we find a parent object
         * of one of the supported bus subsystems */
        if (subsys &&
            ((g_strcmp0 (subsys, "usb") == 0)      ||
             (g_strcmp0 (subsys, "pcmcia") == 0)   ||
             (g_strcmp0 (subsys, "pci") == 0)      ||
             (g_strcmp0 (subsys, "platform") == 0) ||
             (g_strcmp0 (subsys, "pnp") == 0)      ||
             (g_strcmp0 (subsys, "sdio") == 0)))
            return g_steal_pointer (&subsys);

        parent = g_path_get_dirname (iter);
        g_clear_pointer (&iter, g_free);
        iter = parent;
    }

    /* no more parents to check */
    return NULL;
}

static void
preload_contents (MMKernelDeviceGeneric *self)
{
    g_autofree gchar *bus_subsys = NULL;

    if (self->priv->sysfs_path)
        return;

    preload_sysfs_path (self);
    bus_subsys = find_device_bus_subsystem (self);

    if (g_strcmp0 (bus_subsys, "usb") == 0)
        preload_contents_usb (self);
    else if (g_strcmp0 (bus_subsys, "pcmcia") == 0)
        preload_contents_pcmcia (self);
    else if (g_strcmp0 (bus_subsys, "pci") == 0)
        preload_contents_pci (self);
    else if ((g_strcmp0 (bus_subsys, "platform") == 0) ||
             (g_strcmp0 (bus_subsys, "pnp") == 0)      ||
             (g_strcmp0 (bus_subsys, "sdio") == 0))
        preload_contents_platform (self, bus_subsys);
    else
        preload_contents_other (self);

    if (!bus_subsys)
        return;

    mm_obj_dbg (self, "port contents loaded:");
    mm_obj_dbg (self, "  bus: %s", bus_subsys ? bus_subsys : "n/a");
    if (self->priv->interface_sysfs_path) {
        mm_obj_dbg (self, "  interface: %s", self->priv->interface_sysfs_path);
        mm_obj_dbg (self, "  interface class: %02x", self->priv->interface_class);
        mm_obj_dbg (self, "  interface subclass: %02x", self->priv->interface_subclass);
        mm_obj_dbg (self, "  interface protocol: %02x", self->priv->interface_protocol);
        mm_obj_dbg (self, "  interface number: %02x", self->priv->interface_number);
    }
    if (self->priv->interface_description)
        mm_obj_dbg (self, "  interface description: %s", self->priv->interface_description);
    if (self->priv->physdev_sysfs_path)
        mm_obj_dbg (self, "  device: %s", self->priv->physdev_sysfs_path);
    if (self->priv->driver)
        mm_obj_dbg (self, "  driver: %s", self->priv->driver);
    if (self->priv->physdev_vid)
        mm_obj_dbg (self, "  vendor: %04x", self->priv->physdev_vid);
    if (self->priv->physdev_pid)
        mm_obj_dbg (self, "  product: %04x", self->priv->physdev_pid);
    if (self->priv->physdev_revision)
        mm_obj_dbg (self, "  revision: %04x", self->priv->physdev_revision);
    if (self->priv->physdev_manufacturer)
        mm_obj_dbg (self, "  manufacturer: %s", self->priv->physdev_manufacturer);
    if (self->priv->physdev_product)
        mm_obj_dbg (self, "  product: %s", self->priv->physdev_product);

    preload_common_properties (self);
}

/*****************************************************************************/

static const gchar *
kernel_device_get_subsystem (MMKernelDevice *self)
{
    return mm_kernel_event_properties_get_subsystem (MM_KERNEL_DEVICE_GENERIC (self)->priv->properties);
}

static const gchar *
kernel_device_get_name (MMKernelDevice *self)
{
    return mm_kernel_event_properties_get_name (MM_KERNEL_DEVICE_GENERIC (self)->priv->properties);
}

static const gchar *
kernel_device_get_sysfs_path (MMKernelDevice *self)
{
    return MM_KERNEL_DEVICE_GENERIC (self)->priv->sysfs_path;
}

static gint
kernel_device_get_interface_class (MMKernelDevice *self)
{
    return (gint) MM_KERNEL_DEVICE_GENERIC (self)->priv->interface_class;
}

static gint
kernel_device_get_interface_subclass (MMKernelDevice *self)
{
    return (gint) MM_KERNEL_DEVICE_GENERIC (self)->priv->interface_subclass;
}

static gint
kernel_device_get_interface_protocol (MMKernelDevice *self)
{
    return (gint) MM_KERNEL_DEVICE_GENERIC (self)->priv->interface_protocol;
}

static const gchar *
kernel_device_get_interface_sysfs_path (MMKernelDevice *self)
{
    return MM_KERNEL_DEVICE_GENERIC (self)->priv->interface_sysfs_path;
}

static const gchar *
kernel_device_get_interface_description (MMKernelDevice *self)
{
    return MM_KERNEL_DEVICE_GENERIC (self)->priv->interface_description;
}

static const gchar *
kernel_device_get_physdev_uid (MMKernelDevice *self)
{
    const gchar *uid;

    /* Prefer the one coming in the properties, if any */
    if ((uid = mm_kernel_event_properties_get_uid (MM_KERNEL_DEVICE_GENERIC (self)->priv->properties)) != NULL)
        return uid;

    /* Try to load from properties set */
    if ((uid = mm_kernel_device_get_property (self, ID_MM_PHYSDEV_UID)) != NULL)
        return uid;

    /* Use physical device path, if any */
    if (MM_KERNEL_DEVICE_GENERIC (self)->priv->physdev_sysfs_path)
        return MM_KERNEL_DEVICE_GENERIC (self)->priv->physdev_sysfs_path;

    /* If there is no physdev sysfs path, e.g. for platform ports, use the device sysfs itself */
    return MM_KERNEL_DEVICE_GENERIC (self)->priv->sysfs_path;
}

static const gchar *
kernel_device_get_driver (MMKernelDevice *self)
{
    return MM_KERNEL_DEVICE_GENERIC (self)->priv->driver;
}

static guint16
kernel_device_get_physdev_vid (MMKernelDevice *self)
{
    return MM_KERNEL_DEVICE_GENERIC (self)->priv->physdev_vid;
}

static guint16
kernel_device_get_physdev_pid (MMKernelDevice *self)
{
    return MM_KERNEL_DEVICE_GENERIC (self)->priv->physdev_pid;
}

static guint16
kernel_device_get_physdev_revision (MMKernelDevice *self)
{
    return MM_KERNEL_DEVICE_GENERIC (self)->priv->physdev_revision;
}

static const gchar *
kernel_device_get_physdev_sysfs_path (MMKernelDevice *self)
{
    return MM_KERNEL_DEVICE_GENERIC (self)->priv->physdev_sysfs_path;
}

static const gchar *
kernel_device_get_physdev_subsystem (MMKernelDevice *self)
{
    return MM_KERNEL_DEVICE_GENERIC (self)->priv->physdev_subsystem;
}

static const gchar *
kernel_device_get_physdev_manufacturer (MMKernelDevice *self)
{
    return MM_KERNEL_DEVICE_GENERIC (self)->priv->physdev_manufacturer;
}

static const gchar *
kernel_device_get_physdev_product (MMKernelDevice *self)
{
    return MM_KERNEL_DEVICE_GENERIC (self)->priv->physdev_product;
}

static gboolean
kernel_device_cmp (MMKernelDevice *a,
                   MMKernelDevice *b)
{
    return (!g_strcmp0 (mm_kernel_device_get_subsystem (a), mm_kernel_device_get_subsystem (b)) &&
            !g_strcmp0 (mm_kernel_device_get_name      (a), mm_kernel_device_get_name      (b)));
}

/*****************************************************************************/

static gboolean
string_match (const gchar *str,
              const gchar *original_pattern)
{
    gchar    *pattern;
    gchar    *start;
    gboolean  open_prefix = FALSE;
    gboolean  open_suffix = FALSE;
    gboolean  match;

    pattern = g_strdup (original_pattern);
    start = pattern;

    if (start[0] == '*') {
        open_prefix = TRUE;
        start++;
    }

    if (start[strlen (start) - 1] == '*') {
        open_suffix = TRUE;
        start[strlen (start) - 1] = '\0';
    }

    if (open_suffix && !open_prefix)
        match = g_str_has_prefix (str, start);
    else if (!open_suffix && open_prefix)
        match = g_str_has_suffix (str, start);
    else if (open_suffix && open_prefix)
        match = !!strstr (str, start);
    else
        match = g_str_equal (str, start);

    g_free (pattern);
    return match;
}

static gboolean
check_condition (MMKernelDeviceGeneric *self,
                 MMUdevRuleMatch       *match)
{
    gboolean condition_equal;

    condition_equal = (match->type == MM_UDEV_RULE_MATCH_TYPE_EQUAL);

    /* We only apply 'add' rules */
    if (g_str_equal (match->parameter, "ACTION"))
        return ((!!strstr (match->value, "add")) == condition_equal);

    /* We look for the subsystem string in the whole sysfs path.
     *
     * Note that we're not really making a difference between "SUBSYSTEMS"
     * (where the whole device tree is checked) and "SUBSYSTEM" (where just one
     * single device is checked), because a lot of the MM udev rules are meant
     * to just tag the physical device (e.g. with ID_MM_DEVICE_IGNORE) instead
     * of the single ports. In our case with the custom parsing, we do tag all
     * independent ports.
     */
    if (g_str_equal (match->parameter, "SUBSYSTEMS") || g_str_equal (match->parameter, "SUBSYSTEM"))
        return ((self->priv->sysfs_path && !!strstr (self->priv->sysfs_path, match->value)) == condition_equal);

    /* Exact DRIVER match? We also include the check for DRIVERS, even if we
     * only apply it to this port driver. */
    if (g_str_equal (match->parameter, "DRIVER") || g_str_equal (match->parameter, "DRIVERS"))
        return ((!g_strcmp0 (match->value, mm_kernel_device_get_driver (MM_KERNEL_DEVICE (self)))) == condition_equal);

    /* Device name checks */
    if (g_str_equal (match->parameter, "KERNEL"))
        return (string_match (mm_kernel_device_get_name (MM_KERNEL_DEVICE (self)), match->value) == condition_equal);

    /* Device sysfs path checks; we allow both a direct match and a prefix patch */
    if (g_str_equal (match->parameter, "DEVPATH")) {
        gchar    *prefix_match = NULL;
        gboolean  result = FALSE;

        /* If sysfs path invalid (e.g. path doesn't exist), no match */
        if (!self->priv->sysfs_path)
            return FALSE;

        /* If not already doing a prefix match, do an implicit one. This is so that
         * we can add properties to the usb_device owning all ports, and then apply
         * the property to all ports individually processed here. */
        if (match->value[0] && match->value[strlen (match->value) - 1] != '*')
            prefix_match = g_strdup_printf ("%s/*", match->value);

        if (string_match (self->priv->sysfs_path, match->value) == condition_equal) {
            result = TRUE;
            goto out;
        }

        if (prefix_match && string_match (self->priv->sysfs_path, prefix_match) == condition_equal) {
            result = TRUE;
            goto out;
        }

        if (g_str_has_prefix (self->priv->sysfs_path, "/sys")) {
            if (string_match (&self->priv->sysfs_path[4], match->value) == condition_equal) {
                result = TRUE;
                goto out;
            }
            if (prefix_match && string_match (&self->priv->sysfs_path[4], prefix_match) == condition_equal) {
                result = TRUE;
                goto out;
            }
        }
    out:
        g_free (prefix_match);
        return result;
    }

    /* Attributes checks */
    if (g_str_has_prefix (match->parameter, "ATTRS")) {
        gchar    *attribute;
        gchar    *contents = NULL;
        gboolean  result = FALSE;
        guint     val;

        attribute = g_strdup (&match->parameter[5]);
        g_strdelimit (attribute, "{}", ' ');
        g_strstrip (attribute);

        /* VID/PID directly from our API */
        if (g_str_equal (attribute, "idVendor") || g_str_equal (attribute, "vendor"))
            result = ((mm_get_uint_from_hex_str (match->value, &val)) &&
                      ((mm_kernel_device_get_physdev_vid (MM_KERNEL_DEVICE (self)) == val) == condition_equal));
        else if (g_str_equal (attribute, "idProduct") || g_str_equal (attribute, "device"))
            result = ((mm_get_uint_from_hex_str (match->value, &val)) &&
                      ((mm_kernel_device_get_physdev_pid (MM_KERNEL_DEVICE (self)) == val) == condition_equal));
        /* manufacturer in the physdev */
        else if (g_str_equal (attribute, "manufacturer"))
            result = ((self->priv->physdev_manufacturer && g_str_equal (self->priv->physdev_manufacturer, match->value)) == condition_equal);
        /* product in the physdev */
        else if (g_str_equal (attribute, "product"))
            result = ((self->priv->physdev_product && g_str_equal (self->priv->physdev_product, match->value)) == condition_equal);
        /* interface class/subclass/protocol/number in the interface */
        else if (g_str_equal (attribute, "bInterfaceClass"))
            result = (g_str_equal (match->value, "?*") || ((mm_get_uint_from_hex_str (match->value, &val)) &&
                                                           ((self->priv->interface_class == val) == condition_equal)));
        else if (g_str_equal (attribute, "bInterfaceSubClass"))
            result = (g_str_equal (match->value, "?*") || ((mm_get_uint_from_hex_str (match->value, &val)) &&
                                                           ((self->priv->interface_subclass == val) == condition_equal)));
        else if (g_str_equal (attribute, "bInterfaceProtocol"))
            result = (g_str_equal (match->value, "?*") || ((mm_get_uint_from_hex_str (match->value, &val)) &&
                                                           ((self->priv->interface_protocol == val) == condition_equal)));
        else if (g_str_equal (attribute, "bInterfaceNumber"))
            result = (g_str_equal (match->value, "?*") || ((mm_get_uint_from_hex_str (match->value, &val)) &&
                                                           ((self->priv->interface_number == val) == condition_equal)));
        else
            mm_obj_warn (self, "unknown attribute: %s", attribute);

        g_free (contents);
        g_free (attribute);
        return result;
    }

    /* Previously set property checks */
    if (g_str_has_prefix (match->parameter, "ENV")) {
        gchar    *property;
        gboolean  result = FALSE;

        property = g_strdup (&match->parameter[3]);
        g_strdelimit (property, "{}", ' ');
        g_strstrip (property);

        result = ((!g_strcmp0 ((const gchar *) g_object_get_data (G_OBJECT (self), property), match->value)) == condition_equal);

        g_free (property);
        return result;
    }

    mm_obj_warn (self, "unknown match condition parameter: %s", match->parameter);
    return FALSE;
}

static guint
check_rule (MMKernelDeviceGeneric *self,
            guint                  rule_i)
{
    MMUdevRule *rule;
    gboolean    apply = TRUE;

    g_assert (rule_i < self->priv->rules->len);

    rule = &g_array_index (self->priv->rules, MMUdevRule, rule_i);
    if (rule->conditions) {
        guint condition_i;

        for (condition_i = 0; condition_i < rule->conditions->len; condition_i++) {
            MMUdevRuleMatch *match;

            match = &g_array_index (rule->conditions, MMUdevRuleMatch, condition_i);
            if (!check_condition (self, match)) {
                apply = FALSE;
                break;
            }
        }
    }

    if (apply) {
        switch (rule->result.type) {
        case MM_UDEV_RULE_RESULT_TYPE_PROPERTY: {
            gchar *property_value_read = NULL;

            if (g_str_equal (rule->result.content.property.value, "$attr{bInterfaceClass}"))
                property_value_read = g_strdup_printf ("%02x", self->priv->interface_class);
            else if (g_str_equal (rule->result.content.property.value, "$attr{bInterfaceSubClass}"))
                property_value_read = g_strdup_printf ("%02x", self->priv->interface_subclass);
            else if (g_str_equal (rule->result.content.property.value, "$attr{bInterfaceProtocol}"))
                property_value_read = g_strdup_printf ("%02x", self->priv->interface_protocol);
            else if (g_str_equal (rule->result.content.property.value, "$attr{bInterfaceNumber}"))
                property_value_read = g_strdup_printf ("%02x", self->priv->interface_number);

            /* add new property */
            mm_obj_dbg (self, "property added: %s=%s",
                        rule->result.content.property.name,
                        property_value_read ? property_value_read : rule->result.content.property.value);

            if (!property_value_read)
                /* NOTE: we keep a reference to the list of rules ourselves, so it isn't
                 * an issue if we re-use the same string (i.e. without g_strdup-ing it)
                 * as a property value. */
                g_object_set_data (G_OBJECT (self),
                                   rule->result.content.property.name,
                                   rule->result.content.property.value);
            else
                g_object_set_data_full (G_OBJECT (self),
                                        rule->result.content.property.name,
                                        property_value_read,
                                        g_free);
            break;
        }

        case MM_UDEV_RULE_RESULT_TYPE_LABEL:
            /* noop */
            break;

        case MM_UDEV_RULE_RESULT_TYPE_GOTO_INDEX:
            /* Jump to a new index */
            return rule->result.content.index;

        case MM_UDEV_RULE_RESULT_TYPE_GOTO_TAG:
        case MM_UDEV_RULE_RESULT_TYPE_UNKNOWN:
        default:
            g_assert_not_reached ();
        }
    }

    /* Go to the next rule */
    return rule_i + 1;
}

static void
preload_rule_properties (MMKernelDeviceGeneric *self)
{
    guint i;

    g_assert (self->priv->rules);
    g_assert (self->priv->rules->len > 0);

    /* Start to process rules */
    i = 0;
    while (i < self->priv->rules->len) {
        guint next_rule;

        next_rule = check_rule (self, i);
        i = next_rule;
    }
}

static void
check_preload (MMKernelDeviceGeneric *self)
{
    /* Only preload when properties and rules are set */
    if (!self->priv->properties || !self->priv->rules)
        return;

    /* Don't preload on "remove" actions, where we don't have the device any more */
    if (g_strcmp0 (mm_kernel_event_properties_get_action (self->priv->properties), "remove") == 0)
        return;

    /* Don't preload for devices in the 'virtual' subsystem */
    if (g_strcmp0 (mm_kernel_event_properties_get_subsystem (self->priv->properties), "virtual") == 0)
        return;

    mm_obj_dbg (self, "preloading contents and properties...");
    preload_contents (self);
    preload_rule_properties (self);
}

static gboolean
kernel_device_has_property (MMKernelDevice *self,
                            const gchar    *property)
{
    return !!g_object_get_data (G_OBJECT (self), property);
}

static const gchar *
kernel_device_get_property (MMKernelDevice *self,
                            const gchar    *property)
{
    return g_object_get_data (G_OBJECT (self), property);
}

/*****************************************************************************/

MMKernelDevice *
mm_kernel_device_generic_new_with_rules (MMKernelEventProperties  *props,
                                         GArray                   *rules,
                                         GError                  **error)
{
    /* Note: we allow NULL rules, e.g. for virtual devices */

    return MM_KERNEL_DEVICE (g_initable_new (MM_TYPE_KERNEL_DEVICE_GENERIC,
                                             NULL,
                                             error,
                                             "properties", props,
                                             "rules",      rules,
                                             NULL));
}

MMKernelDevice *
mm_kernel_device_generic_new (MMKernelEventProperties  *props,
                              GError                  **error)
{
    static GArray *rules = NULL;

    /* We only try to load the default list of rules once */
    if (G_UNLIKELY (!rules)) {
        rules = mm_kernel_device_generic_rules_load (UDEVRULESDIR, error);
        if (!rules)
            return NULL;
    }

    return mm_kernel_device_generic_new_with_rules (props, rules, error);
}

/*****************************************************************************/

static void
mm_kernel_device_generic_init (MMKernelDeviceGeneric *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_KERNEL_DEVICE_GENERIC, MMKernelDeviceGenericPrivate);
}

static void
set_property (GObject      *object,
              guint         prop_id,
              const GValue *value,
              GParamSpec   *pspec)
{
    MMKernelDeviceGeneric *self = MM_KERNEL_DEVICE_GENERIC (object);

    switch (prop_id) {
    case PROP_PROPERTIES:
        g_assert (!self->priv->properties);
        self->priv->properties = g_value_dup_object (value);
        break;
    case PROP_RULES:
        g_assert (!self->priv->rules);
        self->priv->rules = g_value_dup_boxed (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
get_property (GObject    *object,
              guint       prop_id,
              GValue     *value,
              GParamSpec *pspec)
{
    MMKernelDeviceGeneric *self = MM_KERNEL_DEVICE_GENERIC (object);

    switch (prop_id) {
    case PROP_PROPERTIES:
        g_value_set_object (value, self->priv->properties);
        break;
    case PROP_RULES:
        g_value_set_boxed (value, self->priv->rules);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static gboolean
initable_init (GInitable     *initable,
               GCancellable  *cancellable,
               GError       **error)
{
    MMKernelDeviceGeneric *self = MM_KERNEL_DEVICE_GENERIC (initable);
    const gchar *subsystem;

    check_preload (self);

    subsystem = mm_kernel_device_get_subsystem (MM_KERNEL_DEVICE (self));
    if (!subsystem) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                     "subsystem is mandatory in kernel device");
        return FALSE;
    }

    if (!mm_kernel_device_get_name (MM_KERNEL_DEVICE (self))) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                     "name is mandatory in kernel device");
        return FALSE;
    }

    /* sysfs path is mandatory as output, and will only be given if the
     * specified device exists; but only if this wasn't a 'remove' event
     * and not a virtual device.
     */
    if (self->priv->properties &&
        g_strcmp0 (mm_kernel_event_properties_get_action (self->priv->properties), "remove") &&
        g_strcmp0 (mm_kernel_event_properties_get_subsystem (self->priv->properties), "virtual") &&
        !self->priv->sysfs_path) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                     "device %s/%s not found",
                     mm_kernel_event_properties_get_subsystem (self->priv->properties),
                     mm_kernel_event_properties_get_name      (self->priv->properties));
        return FALSE;
    }

    return TRUE;
}

static void
dispose (GObject *object)
{
    MMKernelDeviceGeneric *self = MM_KERNEL_DEVICE_GENERIC (object);

    g_clear_pointer (&self->priv->physdev_product,       g_free);
    g_clear_pointer (&self->priv->physdev_manufacturer,  g_free);
    g_clear_pointer (&self->priv->physdev_subsystem,     g_free);
    g_clear_pointer (&self->priv->physdev_sysfs_path,    g_free);
    g_clear_pointer (&self->priv->interface_description, g_free);
    g_clear_pointer (&self->priv->interface_sysfs_path,  g_free);
    g_clear_pointer (&self->priv->sysfs_path,            g_free);
    g_clear_pointer (&self->priv->driver,                g_free);
    g_clear_pointer (&self->priv->rules,                 g_array_unref);
    g_clear_object  (&self->priv->properties);

    G_OBJECT_CLASS (mm_kernel_device_generic_parent_class)->dispose (object);
}

static void
initable_iface_init (GInitableIface *iface)
{
    iface->init = initable_init;
}

static void
mm_kernel_device_generic_class_init (MMKernelDeviceGenericClass *klass)
{
    GObjectClass        *object_class        = G_OBJECT_CLASS (klass);
    MMKernelDeviceClass *kernel_device_class = MM_KERNEL_DEVICE_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMKernelDeviceGenericPrivate));

    object_class->dispose      = dispose;
    object_class->get_property = get_property;
    object_class->set_property = set_property;

    kernel_device_class->get_subsystem             = kernel_device_get_subsystem;
    kernel_device_class->get_name                  = kernel_device_get_name;
    kernel_device_class->get_driver                = kernel_device_get_driver;
    kernel_device_class->get_sysfs_path            = kernel_device_get_sysfs_path;
    kernel_device_class->get_physdev_uid           = kernel_device_get_physdev_uid;
    kernel_device_class->get_physdev_vid           = kernel_device_get_physdev_vid;
    kernel_device_class->get_physdev_pid           = kernel_device_get_physdev_pid;
    kernel_device_class->get_physdev_revision      = kernel_device_get_physdev_revision;
    kernel_device_class->get_physdev_sysfs_path    = kernel_device_get_physdev_sysfs_path;
    kernel_device_class->get_physdev_subsystem     = kernel_device_get_physdev_subsystem;
    kernel_device_class->get_physdev_manufacturer  = kernel_device_get_physdev_manufacturer;
    kernel_device_class->get_physdev_product       = kernel_device_get_physdev_product;
    kernel_device_class->get_interface_class       = kernel_device_get_interface_class;
    kernel_device_class->get_interface_subclass    = kernel_device_get_interface_subclass;
    kernel_device_class->get_interface_protocol    = kernel_device_get_interface_protocol;
    kernel_device_class->get_interface_sysfs_path  = kernel_device_get_interface_sysfs_path;
    kernel_device_class->get_interface_description = kernel_device_get_interface_description;
    kernel_device_class->cmp                       = kernel_device_cmp;
    kernel_device_class->has_property              = kernel_device_has_property;
    kernel_device_class->get_property              = kernel_device_get_property;

    /* Device-wide properties are stored per-port in the generic backend */
    kernel_device_class->has_global_property = kernel_device_has_property;
    kernel_device_class->get_global_property = kernel_device_get_property;

    properties[PROP_PROPERTIES] =
        g_param_spec_object ("properties",
                             "Properties",
                             "Generic kernel event properties",
                             MM_TYPE_KERNEL_EVENT_PROPERTIES,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
    g_object_class_install_property (object_class, PROP_PROPERTIES, properties[PROP_PROPERTIES]);

    properties[PROP_RULES] =
        g_param_spec_boxed ("rules",
                            "Rules",
                            "List of rules to apply",
                            G_TYPE_ARRAY,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
    g_object_class_install_property (object_class, PROP_RULES, properties[PROP_RULES]);
}
