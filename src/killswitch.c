/*
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Config parsing and switch reading. Kept apart from the bus plumbing in
 * main.c so the reading can be exercised without a hub.
 */

#include "killswitch.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

const char *killswitch_state_name(KillSwitchState state)
{
	switch (state) {
	case STATE_OPEN:    return "open";
	case STATE_BLOCKED: return "blocked";
	default:            return "unknown";
	}
}

void killswitch_free(KillSwitch *sw)
{
	if (!sw)
		return;

	g_free(sw->id);
	g_free(sw->label);
	g_free(sw->path);
	g_free(sw->blocked_value);
	g_free(sw->on_blocked);
	g_free(sw->on_open);
	g_free(sw);
}

KillSwitchState killswitch_read(const KillSwitch *sw)
{
	if (!sw)
		return STATE_UNKNOWN;

	switch (sw->source) {
	case SOURCE_UNREADABLE:
		return STATE_UNKNOWN;

	case SOURCE_PRESENCE:
		if (!sw->path)
			return STATE_UNKNOWN;
		return g_file_test(sw->path, G_FILE_TEST_EXISTS) ? STATE_OPEN : STATE_BLOCKED;

	case SOURCE_SYSFS: {
		gchar *contents = NULL;
		gsize length = 0;
		KillSwitchState state;

		if (!sw->path || !sw->blocked_value)
			return STATE_UNKNOWN;

		/*
		 * A read failure is reported as unknown rather than as either
		 * position. The attribute disappears when the driver is unbound, and
		 * guessing "open" there would quietly tell the user their camera is
		 * live when nothing knows whether it is.
		 */
		if (!g_file_get_contents(sw->path, &contents, &length, NULL))
			return STATE_UNKNOWN;

		g_strstrip(contents);
		state = (g_strcmp0(contents, sw->blocked_value) == 0) ? STATE_BLOCKED : STATE_OPEN;
		g_free(contents);
		return state;
	}
	}

	return STATE_UNKNOWN;
}

/*
 * Config format - one group per switch, so a machine declares only what it has:
 *
 *   [camera]
 *   label=Camera
 *   source=sysfs
 *   path=/sys/devices/platform/custom-keys/cam_switch
 *   blocked-value=0
 *   on-blocked=/usr/libexec/killswitchd/camera-off
 *
 *   [microphone]
 *   label=Microphone
 *   source=unreadable
 */
GList *killswitch_config_load(const char *path, GError **error)
{
	GKeyFile *keyfile = g_key_file_new();
	GList *switches = NULL;
	gchar **groups = NULL;
	gsize count = 0, i;

	if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, error)) {
		g_key_file_free(keyfile);
		return NULL;
	}

	groups = g_key_file_get_groups(keyfile, &count);

	for (i = 0; i < count; i++) {
		KillSwitch *sw = g_new0(KillSwitch, 1);
		gchar *source = g_key_file_get_string(keyfile, groups[i], "source", NULL);

		sw->id = g_strdup(groups[i]);
		sw->label = g_key_file_get_string(keyfile, groups[i], "label", NULL);
		sw->path = g_key_file_get_string(keyfile, groups[i], "path", NULL);
		sw->blocked_value = g_key_file_get_string(keyfile, groups[i], "blocked-value", NULL);
		sw->on_blocked = g_key_file_get_string(keyfile, groups[i], "on-blocked", NULL);
		sw->on_open = g_key_file_get_string(keyfile, groups[i], "on-open", NULL);
		sw->state = STATE_UNKNOWN;
		sw->reported = false;

		if (g_strcmp0(source, "sysfs") == 0) {
			sw->source = SOURCE_SYSFS;
			if (!sw->blocked_value)
				sw->blocked_value = g_strdup("0");
		} else if (g_strcmp0(source, "presence") == 0) {
			sw->source = SOURCE_PRESENCE;
		} else {
			/*
			 * Anything unrecognised, including a missing source, is treated as
			 * unreadable. A switch the daemon cannot interpret is still a
			 * switch the device has, and saying so is more useful than
			 * dropping it from the list - the UI can say "unknown".
			 */
			sw->source = SOURCE_UNREADABLE;
		}

		if (sw->source != SOURCE_UNREADABLE && !sw->path) {
			g_warning("killswitchd: [%s] has source=%s but no path; treating as unreadable",
			          sw->id, source ? source : "(none)");
			sw->source = SOURCE_UNREADABLE;
		}

		g_free(source);
		switches = g_list_append(switches, sw);
	}

	g_strfreev(groups);
	g_key_file_free(keyfile);
	return switches;
}
