/*
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KILLSWITCH_H
#define KILLSWITCH_H

#include <glib.h>
#include <stdbool.h>

#define KILLSWITCH_SERVICE "org.webosports.service.killswitch"
#define KILLSWITCH_SUBSCRIPTION_KEY "/getStatus"

/*
 * How a switch's position is discovered. The machines that carry these do not
 * agree, so the daemon knows no device: a config file says which switches exist
 * and how to read each one.
 */
typedef enum {
	/*
	 * A sysfs attribute whose contents are compared against blocked_value.
	 * The FuriPhone FLX1s exposes its camera and cellular switches this way,
	 * through the ODM's custom_keys driver.
	 */
	SOURCE_SYSFS = 0,

	/*
	 * Blocked when a path has gone away. The PinePhone and PinePhone Pro DIP
	 * switches cut power rather than signalling anything, so the modem, the
	 * WiFi/BT chip and the cameras simply stop existing - their absence is the
	 * only reading available.
	 */
	SOURCE_PRESENCE,

	/*
	 * There is a switch, and its position cannot be read at all. Not an error,
	 * and it must never be reported as open or blocked: the FLX1s microphone
	 * switch physically breaks the analogue line (measured: -46 dBFS to
	 * -86 dBFS) and appears in no attribute, input device or property. Finding
	 * out would mean opening the microphone, which is the one thing the switch
	 * exists to prevent.
	 */
	SOURCE_UNREADABLE,
} KillSwitchSource;

typedef enum {
	STATE_UNKNOWN = 0,
	STATE_OPEN,
	STATE_BLOCKED,
} KillSwitchState;

typedef struct {
	gchar *id;               /* "camera", "camera-front", "cellular", ... */
	gchar *label;            /* human readable, optional */
	KillSwitchSource source;
	gchar *path;             /* sysfs attribute, or the path whose absence blocks */
	gchar *blocked_value;    /* SOURCE_SYSFS: contents meaning "blocked" */
	gchar *on_blocked;       /* optional command to run when it engages */
	gchar *on_open;          /* optional command to run when it disengages */
	KillSwitchState state;
	bool reported;           /* have we ever published a state for this one */
} KillSwitch;

const char *killswitch_state_name(KillSwitchState state);
GList *killswitch_config_load(const char *path, GError **error);
void killswitch_free(KillSwitch *sw);
KillSwitchState killswitch_read(const KillSwitch *sw);

#endif /* KILLSWITCH_H */
