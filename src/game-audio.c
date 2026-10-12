/* game-audio.c
 *
 * "Capture Audio" for the Game Capture source: records the PipeWire playback
 * streams that belong to the hooked game process.
 *
 * Adapted from pipewire-audio-capture-app.c of obs-pipewire-audio-capture
 * (https://github.com/dimtpap/obs-pipewire-audio-capture)
 * Copyright 2022-2025 Dimitris Papaioannou <dimtpap@protonmail.com>
 * The app selection by name was replaced with matching the game's process.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "game-audio.h"
#include "pipewire-audio.h"

#include <util/dstr.h>

/** How it works (same as the app capture source it's based on):
    - Keep track of playback streams, their ports and clients, the system sinks and the default sink
    - Create a virtual capture sink with the default sink's channels and connect the OBS stream to it
    - Link the ports of every stream whose process is the hooked game to the capture sink */

struct target_node_port {
	const char *channel;
	uint32_t id;
};

struct target_node {
	struct game_audio *ga;
	const char *name;
	const char *app_name;
	const char *binary;
	pid_t pid;
	uint32_t client_id;
	uint32_t id;
	struct obs_pw_audio_proxy_list ports;

	struct spa_hook node_listener;
};

struct target_client {
	struct game_audio *ga;
	pid_t pid;
	uint32_t id;

	struct spa_hook client_listener;
};

struct system_sink {
	const char *name;
	uint32_t id;
};

struct capture_sink_link {
	uint32_t id;
};

struct capture_sink_port {
	const char *channel;
	uint32_t id;
};

struct game_audio {
	obs_source_t *source;

	struct obs_pw_audio_instance pw;

	struct {
		struct pw_proxy *proxy;
		struct spa_hook proxy_listener;
		bool autoconnect_targets;
		uint32_t id;
		uint32_t serial;
		uint32_t channels;
		struct dstr position;
		DARRAY(struct capture_sink_port) ports;

		struct obs_pw_audio_proxy_list links;
	} sink;

	struct obs_pw_audio_proxy_list system_sinks;
	struct {
		struct obs_pw_audio_default_node_metadata metadata;
		struct pw_proxy *proxy;
		struct spa_hook node_listener;
		struct spa_hook proxy_listener;
	} default_sink;

	struct obs_pw_audio_proxy_list clients;
	struct obs_pw_audio_proxy_list nodes;

	/* The hooked game; only touched with the thread loop locked */
	pid_t target_pid;
	char target_exe[64];
};

/* Targeting */
static pid_t client_pid(struct game_audio *ga, uint32_t client_id)
{
	struct obs_pw_audio_proxy_list_iter iter;
	obs_pw_audio_proxy_list_iter_init(&iter, &ga->clients);

	struct target_client *client;
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&client)) {
		if (client->id == client_id) {
			return client->pid;
		}
	}
	return 0;
}

static bool name_matches(const char *exe, const char *name)
{
	size_t len = strlen(exe);
	return name && len >= 3 && strncmp(name, exe, len) == 0;
}

static bool node_is_targeted(struct game_audio *ga, struct target_node *node)
{
	if (!ga->target_pid && !*ga->target_exe) {
		return false;
	}

	pid_t pid = node->pid ? node->pid : client_pid(ga, node->client_id);
	if (ga->target_pid && pid == ga->target_pid) {
		return true;
	}

	/* The PID only matches when the game shares our PID namespace (native, Steam, Proton).
	 * Fall back to the executable name, e.g. for Flatpak games. */
	return *ga->target_exe &&
	       (name_matches(ga->target_exe, node->binary) || name_matches(ga->target_exe, node->app_name));
}
/* ------------------------------------------------- */

/* App streams <-> Capture sink links */
static void link_bound_cb(void *data, uint32_t global_id)
{
	struct capture_sink_link *link = data;
	link->id = global_id;
}

static void link_destroy_cb(void *data)
{
	struct capture_sink_link *link = data;
	blog(LOG_DEBUG, "[linux-vkcapture] Audio link %u destroyed", link->id);
}

static void link_port_to_sink(struct game_audio *ga, struct target_node_port *port, uint32_t node_id)
{
	uint32_t p = 0;
	if (ga->sink.channels == 1 && /* Mono capture sink */
	    ga->sink.ports.num >= 1) {
		p = ga->sink.ports.array[0].id;
	} else {
		for (size_t i = 0; i < ga->sink.ports.num; i++) {
			if (astrcmpi(ga->sink.ports.array[i].channel, port->channel) == 0) {
				p = ga->sink.ports.array[i].id;
				break;
			}
		}
	}

	if (!p) {
		blog(LOG_WARNING,
		     "[linux-vkcapture] Could not connect port %u of node %u to the game audio sink. No sink port has channel %s",
		     port->id, node_id, port->channel);
		return;
	}

	struct pw_properties *link_props = pw_properties_new(PW_KEY_OBJECT_LINGER, "false", NULL);

	pw_properties_setf(link_props, PW_KEY_LINK_OUTPUT_NODE, "%u", node_id);
	pw_properties_setf(link_props, PW_KEY_LINK_OUTPUT_PORT, "%u", port->id);

	pw_properties_setf(link_props, PW_KEY_LINK_INPUT_NODE, "%u", ga->sink.id);
	pw_properties_setf(link_props, PW_KEY_LINK_INPUT_PORT, "%u", p);

	struct pw_proxy *link_proxy = pw_core_create_object(ga->pw.core, "link-factory", PW_TYPE_INTERFACE_Link,
							    PW_VERSION_LINK, &link_props->dict,
							    sizeof(struct capture_sink_link));

	pw_properties_free(link_props);

	if (!link_proxy) {
		blog(LOG_WARNING, "[linux-vkcapture] Could not connect port %u of node %u to the game audio sink",
		     port->id, node_id);
		return;
	}

	struct capture_sink_link *link = pw_proxy_get_user_data(link_proxy);
	link->id = SPA_ID_INVALID;

	obs_pw_audio_proxy_list_append(&ga->sink.links, link_proxy);
}

static void link_node_to_sink(struct game_audio *ga, struct target_node *node)
{
	blog(LOG_INFO, "[linux-vkcapture] Capturing audio of stream %u (\"%s\", pid %d)", node->id,
	     node->app_name ? node->app_name : node->name, (int)node->pid);

	struct obs_pw_audio_proxy_list_iter iter;
	obs_pw_audio_proxy_list_iter_init(&iter, &node->ports);

	struct target_node_port *port;
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&port)) {
		link_port_to_sink(ga, port, node->id);
	}
}

static void connect_targets(struct game_audio *ga)
{
	if (!ga->sink.proxy) {
		return;
	}

	obs_pw_audio_proxy_list_clear(&ga->sink.links);

	struct obs_pw_audio_proxy_list_iter iter;
	obs_pw_audio_proxy_list_iter_init(&iter, &ga->nodes);

	struct target_node *node;
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&node)) {
		if (node_is_targeted(ga, node)) {
			link_node_to_sink(ga, node);
		}
	}
}
/* ------------------------------------------------- */

/* System sinks */
static void system_sink_destroy_cb(void *data)
{
	struct system_sink *s = data;
	bfree((void *)s->name);
}

static void register_system_sink(struct game_audio *ga, uint32_t global_id, const char *name)
{
	struct pw_proxy *sink_proxy = pw_registry_bind(ga->pw.registry, global_id, PW_TYPE_INTERFACE_Node,
						       PW_VERSION_NODE, sizeof(struct system_sink));
	if (!sink_proxy) {
		return;
	}

	struct system_sink *sink = pw_proxy_get_user_data(sink_proxy);
	sink->name = bstrdup(name);
	sink->id = global_id;

	obs_pw_audio_proxy_list_append(&ga->system_sinks, sink_proxy);
}
/* ------------------------------------------------- */

/* Target clients */
static void client_destroy_cb(void *data)
{
	struct target_client *client = data;
	spa_hook_remove(&client->client_listener);
}

static void on_client_info_cb(void *data, const struct pw_client_info *info)
{
	if ((info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS) == 0 || !info->props || !info->props->n_items) {
		return;
	}

	const char *pid_str = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_ID);
	if (!pid_str) {
		return;
	}

	struct target_client *client = data;
	struct game_audio *ga = client->ga;
	pid_t pid = (pid_t)strtol(pid_str, NULL, 10);
	if (pid == client->pid) {
		return;
	}

	/* Nodes that only had the client's PID to go by may start (or stop) matching */
	struct obs_pw_audio_proxy_list_iter iter;
	obs_pw_audio_proxy_list_iter_init(&iter, &ga->nodes);
	struct target_node *node;
	pid_t old_pid = client->pid;
	bool relink = false;
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&node)) {
		if (node->client_id == client->id && !node->pid) {
			client->pid = old_pid;
			bool before = node_is_targeted(ga, node);
			client->pid = pid;
			relink |= before != node_is_targeted(ga, node);
		}
	}
	client->pid = pid;

	if (relink && ga->sink.autoconnect_targets) {
		connect_targets(ga);
	}
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info = on_client_info_cb,
};

static void register_target_client(struct game_audio *ga, uint32_t global_id)
{
	struct pw_proxy *client_proxy = pw_registry_bind(ga->pw.registry, global_id, PW_TYPE_INTERFACE_Client,
							 PW_VERSION_CLIENT, sizeof(struct target_client));
	if (!client_proxy) {
		return;
	}

	struct target_client *client = pw_proxy_get_user_data(client_proxy);
	client->ga = ga;
	client->pid = 0;
	client->id = global_id;

	obs_pw_audio_proxy_list_append(&ga->clients, client_proxy);
	pw_proxy_add_object_listener(client_proxy, &client->client_listener, &client_events, client);
}
/* ------------------------------------------------- */

/* Target nodes and ports */
static void port_destroy_cb(void *data)
{
	struct target_node_port *p = data;
	bfree((void *)p->channel);
}

static void node_destroy_cb(void *data)
{
	struct target_node *node = data;

	spa_hook_remove(&node->node_listener);

	obs_pw_audio_proxy_list_clear(&node->ports);

	bfree((void *)node->binary);
	bfree((void *)node->app_name);
	bfree((void *)node->name);
}

static struct target_node_port *node_register_port(struct target_node *node, uint32_t global_id,
						   struct pw_registry *registry, const char *channel)
{
	struct pw_proxy *port_proxy = pw_registry_bind(registry, global_id, PW_TYPE_INTERFACE_Port, PW_VERSION_PORT,
						       sizeof(struct target_node_port));
	if (!port_proxy) {
		return NULL;
	}

	struct target_node_port *port = pw_proxy_get_user_data(port_proxy);
	port->channel = bstrdup(channel);
	port->id = global_id;

	obs_pw_audio_proxy_list_append(&node->ports, port_proxy);

	return port;
}

static void on_node_info_cb(void *data, const struct pw_node_info *info)
{
	if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) == 0 || !info->props || !info->props->n_items) {
		return;
	}

	struct target_node *node = data;
	struct game_audio *ga = node->ga;
	bool before = node_is_targeted(ga, node);

	/* The process properties are only in the full info, not in the registry globals */
	const char *binary = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY);
	if (binary) {
		bfree((void *)node->binary);
		node->binary = bstrdup(binary);
	}
	const char *pid_str = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_ID);
	if (pid_str) {
		node->pid = (pid_t)strtol(pid_str, NULL, 10);
	}

	bool after = node_is_targeted(ga, node);
	if (!ga->sink.autoconnect_targets || before == after) {
		return;
	}
	if (after) {
		/* Ports registered before the info arrived weren't linked yet */
		link_node_to_sink(ga, node);
	} else {
		connect_targets(ga);
	}
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_node_info_cb,
};

static void register_target_node(struct game_audio *ga, uint32_t global_id, uint32_t client_id, const char *app_name,
				 const char *name)
{
	struct pw_proxy *node_proxy = pw_registry_bind(ga->pw.registry, global_id, PW_TYPE_INTERFACE_Node,
						       PW_VERSION_NODE, sizeof(struct target_node));
	if (!node_proxy) {
		return;
	}

	struct target_node *node = pw_proxy_get_user_data(node_proxy);
	node->ga = ga;
	node->name = bstrdup(name);
	node->app_name = bstrdup(app_name);
	node->binary = NULL;
	node->pid = 0;
	node->id = global_id;
	node->client_id = client_id;
	obs_pw_audio_proxy_list_init(&node->ports, NULL, port_destroy_cb);

	obs_pw_audio_proxy_list_append(&ga->nodes, node_proxy);
	pw_proxy_add_object_listener(node_proxy, &node->node_listener, &node_events, node);
}
/* ------------------------------------------------- */

/* Game audio capture sink */
static void finalize_capture_sink(struct game_audio *ga)
{
	if (!ga->sink.proxy || ga->sink.id == SPA_ID_INVALID || ga->sink.serial == SPA_ID_INVALID ||
	    ga->sink.ports.num != ga->sink.channels) {
		return;
	}

	connect_targets(ga);

	ga->sink.autoconnect_targets = true;

	if (obs_pw_audio_stream_connect(&ga->pw.audio, ga->sink.id, ga->sink.serial, ga->sink.channels) < 0) {
		blog(LOG_WARNING, "[linux-vkcapture] Error connecting stream %p to the game audio sink %u",
		     ga->pw.audio.stream, ga->sink.id);
	}
}

static void on_sink_proxy_bound_cb(void *data, uint32_t global_id)
{
	struct game_audio *ga = data;
	ga->sink.id = global_id;
	da_init(ga->sink.ports);
}

static void on_sink_proxy_removed_cb(void *data)
{
	struct game_audio *ga = data;
	blog(LOG_WARNING, "[linux-vkcapture] Game audio sink %u has been destroyed by the PipeWire remote",
	     ga->sink.id);
	pw_proxy_destroy(ga->sink.proxy);
}

static void on_sink_proxy_destroy_cb(void *data)
{
	struct game_audio *ga = data;

	spa_hook_remove(&ga->sink.proxy_listener);
	spa_zero(ga->sink.proxy_listener);

	for (size_t i = 0; i < ga->sink.ports.num; i++) {
		struct capture_sink_port *p = &ga->sink.ports.array[i];
		bfree((void *)p->channel);
	}
	da_free(ga->sink.ports);

	ga->sink.channels = 0;
	dstr_free(&ga->sink.position);

	ga->sink.autoconnect_targets = false;
	ga->sink.proxy = NULL;
	ga->sink.id = SPA_ID_INVALID;
}

static void on_sink_proxy_error_cb(void *data, int seq, int res, const char *message)
{
	UNUSED_PARAMETER(data);
	blog(LOG_ERROR, "[linux-vkcapture] Game audio sink error: seq:%d res:%d :%s", seq, res, message);
}

static const struct pw_proxy_events sink_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.bound = on_sink_proxy_bound_cb,
	.removed = on_sink_proxy_removed_cb,
	.destroy = on_sink_proxy_destroy_cb,
	.error = on_sink_proxy_error_cb,
};

static void register_capture_sink_port(struct game_audio *ga, uint32_t global_id, const char *channel)
{
	struct capture_sink_port *port = da_push_back_new(ga->sink.ports);
	port->channel = bstrdup(channel);
	port->id = global_id;

	finalize_capture_sink(ga);
}

static void make_capture_sink(struct game_audio *ga, uint32_t channels, const char *position)
{
	struct pw_properties *sink_props = pw_properties_new(PW_KEY_FACTORY_NAME, "support.null-audio-sink",
							     PW_KEY_MEDIA_CLASS, "Stream/Input/Audio",
							     PW_KEY_NODE_VIRTUAL, "true", SPA_KEY_AUDIO_POSITION,
							     position, NULL);

	pw_properties_setf(sink_props, PW_KEY_NODE_NAME, "OBS: %s", obs_source_get_name(ga->source));

	pw_properties_setf(sink_props, PW_KEY_AUDIO_CHANNELS, "%u", channels);

	ga->sink.proxy = pw_core_create_object(ga->pw.core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
					       &sink_props->dict, 0);

	pw_properties_free(sink_props);

	if (!ga->sink.proxy) {
		blog(LOG_WARNING, "[linux-vkcapture] Failed to create the game audio sink");
		return;
	}

	ga->sink.channels = channels;
	dstr_copy(&ga->sink.position, position);

	ga->sink.id = SPA_ID_INVALID;
	ga->sink.serial = SPA_ID_INVALID;

	pw_proxy_add_listener(ga->sink.proxy, &ga->sink.proxy_listener, &sink_proxy_events, ga);
}

static void destroy_capture_sink(struct game_audio *ga)
{
	/* Links are automatically destroyed by PipeWire */

	if (!ga->sink.proxy) {
		return;
	}

	if (pw_stream_get_state(ga->pw.audio.stream, NULL) != PW_STREAM_STATE_UNCONNECTED) {
		pw_stream_disconnect(ga->pw.audio.stream);
	}

	ga->sink.autoconnect_targets = false;
	pw_proxy_destroy(ga->sink.proxy);
}
/* ------------------------------------------------- */

/* Default system sink */
static void on_default_sink_info_cb(void *data, const struct pw_node_info *info)
{
	if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) == 0 || !info->props || !info->props->n_items) {
		return;
	}

	struct game_audio *ga = data;

	/* Use stereo for Pro Audio sinks and for sinks without channel info */
	const char *channels = spa_dict_lookup(info->props, PW_KEY_AUDIO_CHANNELS);
	const char *position = spa_dict_lookup(info->props, SPA_KEY_AUDIO_POSITION);
	if (!channels || !position) {
		if (ga->sink.proxy) {
			return;
		}
		channels = "2";
		position = "FL,FR";
	} else if (astrstri(position, "AUX")) {
		channels = "2";
		position = "FL,FR";
	}

	uint32_t c = strtoul(channels, NULL, 10);
	if (!c) {
		return;
	}

	if (ga->sink.channels == c && !dstr_is_empty(&ga->sink.position) &&
	    dstr_cmp(&ga->sink.position, position) == 0) {
		return;
	}

	destroy_capture_sink(ga);

	make_capture_sink(ga, c, position);
}

static const struct pw_node_events default_sink_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_default_sink_info_cb,
};

static void on_default_sink_proxy_removed_cb(void *data)
{
	struct game_audio *ga = data;
	pw_proxy_destroy(ga->default_sink.proxy);
}

static void on_default_sink_proxy_destroy_cb(void *data)
{
	struct game_audio *ga = data;
	spa_hook_remove(&ga->default_sink.node_listener);
	spa_zero(ga->default_sink.node_listener);

	spa_hook_remove(&ga->default_sink.proxy_listener);
	spa_zero(ga->default_sink.proxy_listener);

	ga->default_sink.proxy = NULL;
}

static const struct pw_proxy_events default_sink_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = on_default_sink_proxy_removed_cb,
	.destroy = on_default_sink_proxy_destroy_cb,
};

static void default_node_cb(void *data, const char *name)
{
	struct game_audio *ga = data;

	struct obs_pw_audio_proxy_list_iter iter;
	obs_pw_audio_proxy_list_iter_init(&iter, &ga->system_sinks);

	struct system_sink *temp, *default_sink = NULL;
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&temp)) {
		if (strcmp(name, temp->name) == 0) {
			default_sink = temp;
			break;
		}
	}
	if (!default_sink) {
		return;
	}

	if (ga->default_sink.proxy) {
		pw_proxy_destroy(ga->default_sink.proxy);
	}

	ga->default_sink.proxy =
		pw_registry_bind(ga->pw.registry, default_sink->id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
	if (!ga->default_sink.proxy) {
		if (!ga->sink.proxy) {
			blog(LOG_WARNING, "[linux-vkcapture] No default sink info, game audio defaulting to stereo");
			make_capture_sink(ga, 2, "FL,FR");
		}
		return;
	}

	pw_proxy_add_object_listener(ga->default_sink.proxy, &ga->default_sink.node_listener, &default_sink_events,
				     ga);
	pw_proxy_add_listener(ga->default_sink.proxy, &ga->default_sink.proxy_listener, &default_sink_proxy_events,
			      ga);
}
/* ------------------------------------------------- */

/* Registry */
static void on_global_cb(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version,
			 const struct spa_dict *props)
{
	UNUSED_PARAMETER(permissions);
	UNUSED_PARAMETER(version);

	if (!props || !type) {
		return;
	}

	struct game_audio *ga = data;

	if (id == ga->sink.id) {
		const char *ser = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
		if (!ser) {
			blog(LOG_ERROR, "[linux-vkcapture] No object serial found on the game audio sink %u", id);
			ga->sink.serial = 0;
		} else {
			ga->sink.serial = strtoul(ser, NULL, 10);
			finalize_capture_sink(ga);
		}
	}

	if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
		const char *nid, *dir, *chn;
		if (!(nid = spa_dict_lookup(props, PW_KEY_NODE_ID)) ||
		    !(dir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) ||
		    !(chn = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL))) {
			return;
		}

		uint32_t node_id = strtoul(nid, NULL, 10);

		if (astrcmpi(dir, "in") == 0 && node_id == ga->sink.id) {
			register_capture_sink_port(ga, id, chn);
		} else if (astrcmpi(dir, "out") == 0) {
			struct obs_pw_audio_proxy_list_iter iter;
			obs_pw_audio_proxy_list_iter_init(&iter, &ga->nodes);

			struct target_node *temp, *node = NULL;
			while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&temp)) {
				if (temp->id == node_id) {
					node = temp;
					break;
				}
			}
			if (!node) {
				return;
			}

			struct target_node_port *port = node_register_port(node, id, ga->pw.registry, chn);

			if (port && ga->sink.autoconnect_targets && node_is_targeted(ga, node)) {
				link_port_to_sink(ga, port, node->id);
			}
		}
	} else if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		const char *node_name, *media_class;
		if (!(node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) ||
		    !(media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS))) {
			return;
		}

		if (strcmp(media_class, "Stream/Output/Audio") == 0) {
			const char *node_app_name = spa_dict_lookup(props, PW_KEY_APP_NAME);
			if (!node_app_name) {
				node_app_name = node_name;
			}

			uint32_t client_id = 0;
			const char *client_id_str = spa_dict_lookup(props, PW_KEY_CLIENT_ID);
			if (client_id_str) {
				client_id = strtoul(client_id_str, NULL, 10);
			}

			register_target_node(ga, id, client_id, node_app_name, node_name);
		} else if (strcmp(media_class, "Audio/Sink") == 0) {
			register_system_sink(ga, id, node_name);
		}

	} else if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
		register_target_client(ga, id);
	} else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
		const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
		if (!name || strcmp(name, "default") != 0) {
			return;
		}

		if (!obs_pw_audio_default_node_metadata_listen(&ga->default_sink.metadata, &ga->pw, id, true,
							       default_node_cb, ga) &&
		    !ga->sink.proxy) {
			blog(LOG_WARNING, "[linux-vkcapture] No default metadata, game audio defaulting to stereo");
			make_capture_sink(ga, 2, "FL,FR");
		}
	}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global_cb,
};
/* ------------------------------------------------- */

struct game_audio *game_audio_create(obs_source_t *source)
{
	struct game_audio *ga = bzalloc(sizeof(struct game_audio));

	if (!obs_pw_audio_instance_init(&ga->pw, &registry_events, ga, true, false, source)) {
		obs_pw_audio_instance_destroy(&ga->pw);

		bfree(ga);
		return NULL;
	}

	ga->source = source;

	obs_pw_audio_proxy_list_init(&ga->nodes, NULL, node_destroy_cb);
	obs_pw_audio_proxy_list_init(&ga->clients, NULL, client_destroy_cb);
	obs_pw_audio_proxy_list_init(&ga->sink.links, link_bound_cb, link_destroy_cb);
	obs_pw_audio_proxy_list_init(&ga->system_sinks, NULL, system_sink_destroy_cb);

	ga->sink.id = SPA_ID_INVALID;
	dstr_init(&ga->sink.position);

	pw_thread_loop_unlock(ga->pw.thread_loop);

	return ga;
}

void game_audio_destroy(struct game_audio *ga)
{
	if (!ga) {
		return;
	}

	pw_thread_loop_lock(ga->pw.thread_loop);

	obs_pw_audio_proxy_list_clear(&ga->nodes);
	obs_pw_audio_proxy_list_clear(&ga->system_sinks);
	obs_pw_audio_proxy_list_clear(&ga->clients);

	destroy_capture_sink(ga);

	if (ga->default_sink.proxy) {
		pw_proxy_destroy(ga->default_sink.proxy);
	}
	if (ga->default_sink.metadata.proxy) {
		pw_proxy_destroy(ga->default_sink.metadata.proxy);
	}

	obs_pw_audio_instance_destroy(&ga->pw);

	dstr_free(&ga->sink.position);

	bfree(ga);
}

void game_audio_set_target(struct game_audio *ga, pid_t pid, const char *exe)
{
	if (!ga) {
		return;
	}

	pw_thread_loop_lock(ga->pw.thread_loop);

	if (!exe) {
		exe = "";
	}
	if (ga->target_pid != pid || strncmp(ga->target_exe, exe, sizeof(ga->target_exe)) != 0) {
		ga->target_pid = pid;
		snprintf(ga->target_exe, sizeof(ga->target_exe), "%s", exe);
		if (pid || *exe) {
			blog(LOG_INFO, "[linux-vkcapture] Capture Audio: following pid %d (%s)", (int)pid, exe);
		}
		connect_targets(ga);
	}

	pw_thread_loop_unlock(ga->pw.thread_loop);
}
