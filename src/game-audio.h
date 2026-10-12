/* game-audio.h
 *
 * "Capture Audio" for the Game Capture source: records the PipeWire playback
 * streams that belong to the hooked game process.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <obs-module.h>
#include <sys/types.h>

struct game_audio;

/**
 * Create the PipeWire capture for a source. The source must have OBS_SOURCE_AUDIO.
 * @return NULL if PipeWire is unavailable
 */
struct game_audio *game_audio_create(obs_source_t *source);

void game_audio_destroy(struct game_audio *ga);

/**
 * Capture the playback streams of this process (pid 0 = capture nothing).
 * Streams are matched by application.process.id; exe is a fallback for games running in
 * another PID namespace, compared against the stream's binary and the start of its app name
 * (vkcapture reports Wine games by their truncated /proc comm, e.g. "GenshinImpact.e").
 */
void game_audio_set_target(struct game_audio *ga, pid_t pid, const char *exe);
