/*
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * killswitchd - reports the hardware privacy switches on the luna bus.
 *
 * Polls rather than waits on an event, deliberately. The FLX1s driver behind
 * these switches (the ODM's kpd_customkey) never calls sysfs_notify(), so its
 * attributes cannot be poll()ed, and its input device is held under an
 * exclusive EVIOCGRAB by a MediaTek vendor HAL - an attempt to grab it returns
 * EBUSY - so evdev delivers nothing to anyone else. Presence-based switches
 * have nothing to wait on either. A one second poll costs almost nothing and
 * works the same on every backend.
 */

#include "killswitch.h"

#include <glib.h>
#include <luna-service2/lunaservice.h>
#include <pbnjson.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_CONFIG  WEBOS_INSTALL_SYSCONFDIR "/killswitchd.conf"
#define DEFAULT_POLL_MS 1000

/*
 * How long to hold a status update waiting for a switch's action to finish.
 * The actions here are two setprop calls; anything slower than this is stuck,
 * and a late update beats a lost one.
 */
#define ACTION_WAIT_MS  2000

static GMainLoop *main_loop = NULL;
static LSHandle *service_handle = NULL;
static GList *switches = NULL;
static int exit_status = 0;

static jvalue_ref build_status(void)
{
	jvalue_ref reply = jobject_create();
	jvalue_ref array = jarray_create(NULL);
	const GList *item;

	for (item = switches; item; item = item->next) {
		const KillSwitch *sw = item->data;
		jvalue_ref entry = jobject_create();

		jobject_put(entry, J_CSTR_TO_JVAL("id"), jstring_create(sw->id));
		jobject_put(entry, J_CSTR_TO_JVAL("state"),
		            jstring_create(killswitch_state_name(sw->state)));
		if (sw->label)
			jobject_put(entry, J_CSTR_TO_JVAL("label"), jstring_create(sw->label));

		/*
		 * Tells a client whether "unknown" means "cannot be read, ever" or
		 * "not read yet", which are different things to show a user.
		 */
		jobject_put(entry, J_CSTR_TO_JVAL("readable"),
		            jboolean_create(sw->source != SOURCE_UNREADABLE));

		jarray_append(array, entry);
	}

	jobject_put(reply, J_CSTR_TO_JVAL("switches"), array);
	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	return reply;
}

static void publish_status(void)
{
	LSError lserror;
	jvalue_ref reply;
	const char *payload;

	if (!service_handle)
		return;

	LSErrorInit(&lserror);
	reply = build_status();
	payload = jvalue_tostring_simple(reply);

	if (!LSSubscriptionReply(service_handle, KILLSWITCH_SUBSCRIPTION_KEY,
	                         payload, &lserror)) {
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	j_release(&reply);
}

/*
 * Actions and the status update they belong with.
 *
 * Subscribers must not be told a switch was released before the action for it
 * has run, and this is not a nicety. The camera action asks init to start
 * camerahalserver; a client that hears "released" first attaches to a HAL that
 * is not there yet, and attaching to a stopped HAL blocks the caller until it
 * turns up - measured at over ten seconds in the camera app, with its UI frozen
 * for the duration. Once the action has run the HAL process is up within about
 * 10ms, and an attach that is merely too early fails in ~140ms instead, which a
 * client can retry without freezing.
 *
 * So actions still run out of the main loop - they reach into the Android
 * container and are not something to do with it blocked - but the status is
 * held until they have finished. The timeout is there so a helper that hangs
 * delays the update rather than losing it.
 */
static int pending_actions;
static bool status_deferred;
static guint status_deadline_source;

static void publish_deferred_status(void)
{
	if (status_deadline_source) {
		g_source_remove(status_deadline_source);
		status_deadline_source = 0;
	}

	if (!status_deferred)
		return;

	status_deferred = false;
	publish_status();
}

static gboolean status_deadline_expired(gpointer user_data)
{
	(void) user_data;

	g_warning("killswitchd: switch action still running after %dms, "
	          "publishing status anyway", ACTION_WAIT_MS);
	status_deadline_source = 0;
	publish_deferred_status();

	return G_SOURCE_REMOVE;
}

/* Signature is GChildWatchFunc's, so the parameter order is not ours to pick. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static void action_finished(GPid pid, gint status, gpointer user_data)
{
	(void) status;
	(void) user_data;

	g_spawn_close_pid(pid);

	if (pending_actions > 0)
		pending_actions--;

	if (pending_actions == 0)
		publish_deferred_status();
}

/*
 * This exists because detection and action are separate problems on these
 * devices. On the FLX1s the vendor's own HAL already sets
 * persist.vendor.radio.disabled and init stops the RIL, so the cellular switch
 * needs no action at all - but there is no equivalent trigger for the camera,
 * so its HAL keeps running unless something stops it. Which of those a machine
 * needs belongs in its config, not in this daemon.
 */
static void run_action(const KillSwitch *sw, const char *command)
{
	GError *error = NULL;
	gchar **argv = NULL;
	GPid pid = 0;

	if (!command || !*command)
		return;

	if (!g_shell_parse_argv(command, NULL, &argv, &error)) {
		g_warning("killswitchd: [%s] cannot parse action '%s': %s",
		          sw->id, command, error->message);
		g_clear_error(&error);
		return;
	}

	if (!g_spawn_async(NULL, argv, NULL,
	                   G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
	                   G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
	                   NULL, NULL, &pid, &error)) {
		g_warning("killswitchd: [%s] action '%s' failed: %s",
		          sw->id, command, error->message);
		g_clear_error(&error);
		g_strfreev(argv);
		return;
	}

	pending_actions++;
	g_child_watch_add(pid, action_finished, NULL);

	g_strfreev(argv);
}

static gboolean poll_switches(gpointer user_data)
{
	GList *item;
	bool changed = false;

	for (item = switches; item; item = item->next) {
		KillSwitch *sw = item->data;
		KillSwitchState state = killswitch_read(sw);

		if (sw->reported && state == sw->state)
			continue;

		/*
		 * Actions run on a real change only, never on the first reading. A
		 * switch already engaged at boot has already had its effect - the
		 * hardware is off - and re-running the action there would, for the
		 * camera, kill a HAL that had just been started for no reason a user
		 * could see.
		 */
		if (sw->reported) {
			g_message("killswitchd: %s %s -> %s", sw->id,
			          killswitch_state_name(sw->state),
			          killswitch_state_name(state));

			if (state == STATE_BLOCKED)
				run_action(sw, sw->on_blocked);
			else if (state == STATE_OPEN)
				run_action(sw, sw->on_open);
		}

		sw->state = state;
		sw->reported = true;
		changed = true;
	}

	if (changed) {
		if (pending_actions > 0) {
			status_deferred = true;
			if (!status_deadline_source)
				status_deadline_source = g_timeout_add(ACTION_WAIT_MS,
				                                      status_deadline_expired,
				                                      NULL);
		} else {
			publish_status();
		}
	}

	return G_SOURCE_CONTINUE;
}

static bool cb_get_status(LSHandle *sh, LSMessage *message, void *ctx)
{
	LSError lserror;
	jvalue_ref reply;
	bool subscribed = false;
	bool result;

	LSErrorInit(&lserror);

	if (LSMessageIsSubscription(message)) {
		if (!LSSubscriptionAdd(sh, KILLSWITCH_SUBSCRIPTION_KEY, message, &lserror)) {
			LSErrorPrint(&lserror, stderr);
			LSErrorFree(&lserror);
		} else {
			subscribed = true;
		}
	}

	reply = build_status();
	jobject_put(reply, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));

	result = LSMessageReply(sh, message, jvalue_tostring_simple(reply), &lserror);
	if (!result) {
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	j_release(&reply);
	return true;
}

/*
 * The registration made with the hub cannot outlive it and nothing here
 * re-establishes one, so end the loop and let systemd start us again.
 */
static void hub_disconnected(LSHandle *sh, void *ctx)
{
	g_warning("killswitchd: lost the luna bus, exiting to be restarted");
	exit_status = 1;
	g_main_loop_quit(main_loop);
}

static LSMethod methods[] =
{
	{ "getStatus", cb_get_status, LUNA_METHOD_FLAGS_NONE },
	{ NULL, NULL, LUNA_METHOD_FLAGS_NONE },
};

int main(int argc, char **argv)
{
	GMainLoop *loop = NULL;
	LSError lserror;
	GError *error = NULL;
	const char *config_path = DEFAULT_CONFIG;
	int poll_ms = DEFAULT_POLL_MS;

	if (argc > 1)
		config_path = argv[1];

	switches = killswitch_config_load(config_path, &error);
	if (!switches) {
		/*
		 * No config is the normal case on a machine with no switches, so it is
		 * not a failure - but there is then nothing to report and no reason to
		 * sit on the bus pretending otherwise.
		 */
		g_message("killswitchd: no switches configured in %s%s%s",
		          config_path,
		          error ? ": " : "",
		          error ? error->message : "");
		g_clear_error(&error);
		return 0;
	}

	loop = g_main_loop_new(NULL, FALSE);
	main_loop = loop;
	LSErrorInit(&lserror);

	if (!LSRegister(KILLSWITCH_SERVICE, &service_handle, &lserror))
		goto fail;

	if (!LSRegisterCategory(service_handle, "/", methods, NULL, NULL, &lserror))
		goto fail;

	if (!LSSetDisconnectHandler(service_handle, hub_disconnected, NULL, &lserror))
		goto fail;

	if (!LSGmainAttach(service_handle, loop, &lserror))
		goto fail;

	/* Seed the state before anyone can ask, so the first getStatus is real. */
	poll_switches(NULL);
	g_timeout_add(poll_ms, poll_switches, NULL);

	g_main_loop_run(loop);

	LSUnregister(service_handle, &lserror);
	g_list_free_full(switches, (GDestroyNotify) killswitch_free);
	g_main_loop_unref(loop);
	return exit_status;

fail:
	LSErrorPrint(&lserror, stderr);
	LSErrorFree(&lserror);
	if (loop)
		g_main_loop_unref(loop);
	return 1;
}
