#include "ra_runtime_disc.h"
#include "ra_runtime_disc_serial.h"

static void runtime_disc_retry_clear(void);

#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

#include "ra_badge.h"
#include "ra_client.h"
#include "ra_hash.h"
#include "ra_http.h"
#include "ra_notify.h"
#include "ra_runtime_cards.h"
#include "ra_runtime_challenges.h"
#include "ra_runtime_internal.h"
#include "ra_runtime_core.h"
#include "ra_runtime_hashlib.h"
#include "ra_runtime_memory.h"
#include "ra_runtime_tick.h"
#include "ra_server.h"
#include "ra_session.h"

/* A different game on the disc: evaluate nothing and restart the session on that
 * container, forced so the live container is the one hashed. */
void ra_runtime_disc_restart(int disc_index) {
  if (disc_index < 0)
    return;
  ra_runtime_log("runtime: disc %d is a different game; restarting the session for it\n",
        disc_index + 1);
  g_rt.forced_disc_index = disc_index;
  /* A disc index belongs to the PBP it came from; the hash step checks the path. */
  snprintf(g_rt.forced_disc_path, sizeof(g_rt.forced_disc_path), "%s", g_rt.game_filename);
  runtime_disc_retry_clear();
  if (g_rt.running || g_rt.start_pending) {
    g_rt.reuse_session = 1;
    g_rt.stop = 1;
  }
  ra_badge_stop();
  ra_notify_clear();
  ra_runtime_active_challenges_reset();
  /* Same fields the game-change branch resets, so the tick re-attempts the load. */
  g_rt.attempted = 0;
  g_rt.offline_at_launch = 0;
  g_rt.game_unsupported = 0;
  g_rt.load_failures = 0;
  g_rt.game_is_pops = 0;
  g_rt.title_is_pops = 0;
  g_rt.ps1_loaded = 0;
  /* The hold stays until the session changes; `ra_runtime_disc_watch()` clears it there. */
}

/* A disc that could not be resolved (no network) is retried from the tick with the hash
 * already computed, so no container is read again. */

static void runtime_disc_retry_clear(void) {
  g_rt.disc_retry_disc = -1;
  g_rt.disc_retry_hash[0] = '\0';
  g_rt.disc_retry_at_us = 0;
}

static void runtime_disc_retry_arm(int disc_index, const char *hash) {
  g_rt.disc_retry_disc = disc_index;
  snprintf(g_rt.disc_retry_hash, sizeof(g_rt.disc_retry_hash), "%s",
      hash != NULL ? hash : "");
  g_rt.disc_retry_at_us = (unsigned long long)sceKernelGetSystemTimeWide() +
            10ULL * 1000ULL * 1000ULL;
}

/* The one place that hands a hash to rcheevos. Everything that reaches it has already
 * established which game the hash belongs to (/117). */
static void runtime_disc_switch_media(int match, const char *hash_text) {
  ra_session_t *session;
  ra_client_t *client;
  rc_client_t *rc;

  if (hash_text == NULL || hash_text[0] == '\0')
    return;
  client = ra_runtime_client_acquire(&session);
  if (client == NULL)
    return;
  ra_runtime_log("runtime: asking the client to change media to disc %d hash=%s\n",
        match + 1, hash_text);
  snprintf(g_rt.disc_hash, sizeof(g_rt.disc_hash), "%s", hash_text);
  g_rt.session_disc_index = match;
  runtime_disc_retry_clear();
  rc = ra_client_get_rc_client(client);
  if (rc != NULL)
    rc_client_begin_change_media(rc, hash_text,
                  ra_runtime_disc_change_media_callback,
                  (void*)(intptr_t)(match + 1));
  ra_runtime_client_release(session);
}

/* Plan C: a disc that stays unreadable across three samples is an eject. Never while
 * disc work is in flight, because a swap blanks the serial briefly. */

/* Plan C: the quiet callback for the eject and restore switches. The disc card belongs to a
 * real disc change , not to a disc being pulled. */
static void runtime_media_quiet_callback(int result, const char *error_message,
                    rc_client_t *client, void *userdata) {
  (void)client;
  (void)userdata;
  ra_runtime_log("runtime: media switch result=%d%s%s\n", result,
        error_message != NULL ? " " : "",
        error_message != NULL ? error_message : "");
}

/* "[NO HASH]" is rcheevos' own convention: a hash that cannot match, handled locally
 * without a server call (`rc_client.c:3331`), so this works with the network down. */
static void runtime_media_eject(void) {
  ra_session_t *session;
  ra_client_t *client = ra_runtime_client_acquire(&session);
  rc_client_t *rc = client != NULL ? ra_client_get_rc_client(client) : NULL;

  if (rc != NULL)
    rc_client_begin_change_media(rc, "[NO HASH]", runtime_media_quiet_callback,
                  NULL);
  if (client != NULL)
    ra_runtime_client_release(session);
}

static void runtime_media_restore(void) {
  ra_session_t *session;
  ra_client_t *client = ra_runtime_client_acquire(&session);
  rc_client_t *rc = client != NULL ? ra_client_get_rc_client(client) : NULL;
  const char *hash = g_rt.disc_hash[0] != '\0' ? g_rt.disc_hash
                                               : g_rt.session_hash;

  if (rc != NULL && hash[0] != '\0')
    rc_client_begin_change_media(rc, hash, runtime_media_quiet_callback, NULL);
  if (client != NULL)
    ra_runtime_client_release(session);
}

/* Retry an unresolved disc switch (offline when it happened). The serial is re-read first:
 * if the media moved again the ordinary watch owns it. */
void ra_runtime_disc_retry(void) {
  char live[16];
  uint32_t resolved = 0;
  uint32_t session_id = 0;
  int asked = 0;

  if (g_rt.disc_retry_disc < 0)
    return;
  if ((unsigned long long)sceKernelGetSystemTimeWide() < g_rt.disc_retry_at_us)
    return;
  g_rt.disc_retry_at_us = (unsigned long long)sceKernelGetSystemTimeWide() +
            10ULL * 1000ULL * 1000ULL;

  if (!ra_runtime_disc_live_serial(live, sizeof(live)) ||
    strcmp(live, g_rt.last_live_serial) != 0) {
    ra_runtime_log("runtime: disc retry dropped; the media changed again\n");
    runtime_disc_retry_clear();
    return;
  }

  ra_runtime_lock();
  session_id = g_rt.info.game_id;
  ra_runtime_unlock();

  if (ra_server_resolve_hash(g_rt.disc_retry_hash, &resolved, &asked)) {
    if (session_id != 0 && resolved != session_id) {
      ra_runtime_log("runtime: disc %d resolved game=%u (retry); this is another "
            "game (server)\n", g_rt.disc_retry_disc + 1, (unsigned)resolved);
      ra_runtime_disc_restart(g_rt.disc_retry_disc);
      return;
    }
    ra_runtime_log("runtime: disc %d resolved game=%u (retry, same game)\n",
          g_rt.disc_retry_disc + 1, (unsigned)resolved);
    runtime_disc_switch_media(g_rt.disc_retry_disc, g_rt.disc_retry_hash);
  } else if (asked) {
    ra_runtime_log("runtime: disc %d has no game on the server (retry); treating it as "
          "another game\n", g_rt.disc_retry_disc + 1);
    ra_runtime_disc_restart(g_rt.disc_retry_disc);
  } else {
    ra_runtime_log("runtime: disc %d still cannot be resolved; will retry\n",
          g_rt.disc_retry_disc + 1);
  }
}

/* Set by `ra_runtime_disc_change_media_callback` (RA worker thread) when a disc switch succeeds,
 * consumed once by the tick so the card is built from runtime-owned state. */

/* Returns the container index whose serial matches the live one, or -1 when
 * there is nothing to compare. Every -1 exit is logged. */
static int disc_check_match_container(void) {
  char serials[8][16];
  char live[16];
  int discs = 0;
  int match = -1;
  int i;

  for (i = 0; i < 8; ++i) {
    if (!ra_runtime_disc_pbp_serial(g_rt.game_filename, i, serials[i],
                  sizeof(serials[i])))
      break;
    ++discs;
  }
  if (discs <= 1) {
    ra_runtime_log("runtime: disc check: %d container(s) in the EBOOT, nothing to "
          "swap\n", discs);
    /* The banner already went out at session start (/120), naming whatever
         * disc could be resolved then; with one container there is nothing to add. */
    return -2;
  }
  if (!ra_runtime_disc_live_serial(live, sizeof(live))) {
    ra_runtime_log("runtime: disc check: live serial unreadable at 0x%08X "
          "(%d container(s))\n", (unsigned)RA_PSP_DISC_SERIAL_ADDRESS,
          discs);
    return -1;
  }
  for (i = 0; i < discs; ++i) {
    if (strcmp(serials[i], live) == 0) {
      match = i;
      break;
    }
  }
  ra_runtime_log("runtime: disc check: live=%s containers=%d match=%d\n", live, discs,
        match);
  if (match < 0) {
    /* The live serial is in none of the containers: the banner went out at session
         * start with no disc named (/120). */
    return -1;
  }
  return match;
}

/* Hashes the live container. Returns 0 when the check is finished (logged and
 * the evaluation hold kept). */
static int disc_check_hash(int match, ra_hash_result_t *hash) {
  int ok;

  ra_runtime_log("runtime: disc %d is live; hashing that container\n", match + 1);
  ra_hash_set_disc_index(match);
  ok = ra_hash_ps1(g_rt.game_filename, NULL, hash);
  ra_hash_set_disc_index(0);
  if (!ok) {
    ra_runtime_log("runtime: disc %d hash failed: %s\n", match + 1,
          ra_hash_last_error());
    ra_runtime_set_eval_hold(0);
    return 0;
  }
  return 1;
}

/* A disc can be a different game: when the hash library or the server says so, stop
 * evaluating and restart on that container instead of changing media. Returns 1 to
 * continue with the media switch, 0 when this check already ended. */
static int disc_check_identity(int match, const ra_hash_result_t *hash) {
  uint32_t library_id = 0;
  uint32_t session_id = 0;
  uint32_t resolved = 0;
  int asked = 0;

  ra_runtime_lock();
  session_id = g_rt.info.game_id;
  ra_runtime_unlock();

  if (ra_runtime_hash_library_game_id(hash->hash, &library_id)) {
    if (session_id != 0 && library_id != session_id) {
      ra_runtime_log("runtime: disc %d hash=%s belongs to game %u, session "
            "is game %u; this is another game (library)\n",
            match + 1, hash->hash, (unsigned)library_id,
            (unsigned)session_id);
      ra_runtime_disc_restart(match);
      return 0;
    }
    ra_runtime_log("runtime: disc %d hash=%s is game %u (same game, library)\n",
          match + 1, hash->hash, (unsigned)library_id);
    return 1;
  }
  if (ra_server_resolve_hash(hash->hash, &resolved, &asked)) {
    if (session_id != 0 && resolved != session_id) {
      ra_runtime_log("runtime: disc %d resolved game=%u, session is game "
            "%u; this is another game (server)\n", match + 1,
            (unsigned)resolved, (unsigned)session_id);
      ra_runtime_disc_restart(match);
      return 0;
    }
    ra_runtime_log("runtime: disc %d resolved game=%u (same game, server)\n",
          match + 1, (unsigned)resolved);
    return 1;
  }
  if (asked) {
    ra_runtime_log("runtime: disc %d hash=%s has no game on the server; "
          "treating it as another game\n", match + 1, hash->hash);
    ra_runtime_disc_restart(match);
    return 0;
  }
  /* Offline, or the request never completed: nothing on this device can
             * tell a same-game disc from a foreign one, so the disc is not
             * switched and evaluation stays held. The tick retries. */
  ra_runtime_log("runtime: disc %d cannot be resolved (no answer); keeping "
        "disc %d and holding evaluation\n", match + 1,
        g_rt.session_disc_index + 1);
  runtime_disc_retry_arm(match, hash->hash);
  /* The session banner already went out (/120); this error card
             * explains why the disc is not named. */
  if (g_rt.settings.notifications)
    ra_notify_push(RA_NOTIFY_ERROR, 0, "Disc not identified",
          "Achievements paused until the disc can be "
          "checked");
  return 0;
}

/* Switches media, or records the container when it hashes the session. */
static void disc_check_apply(int match, const ra_hash_result_t *hash) {
  if (strcmp(hash->hash, g_rt.session_hash) == 0) {
    g_rt.session_disc_index = match;
    ra_runtime_log("runtime: disc %d hashes the same as the session; nothing to "
          "do\n", match + 1);
    ra_runtime_set_eval_hold(0);
      return;
  }

  runtime_disc_switch_media(match, hash->hash);
}

/* Called when pspemu's official settings close, i.e. right after a possible
 * `Switch Discs`. If the live serial matches a container other than the one the session
 * was opened with, hash that container and ask rcheevos to change media. */
void ra_runtime_disc_check(void) {
  ra_hash_result_t hash;
  int match;

  if (!g_rt.game_is_pops || !ra_runtime_session_ready())
    return;
  if (g_rt.game_filename[0] == '\0')
    return;

  match = disc_check_match_container();
  if (match == -2) {
    ra_runtime_set_eval_hold(0);
    return;
  }
  if (match < 0)
    return;
  if (match == g_rt.session_disc_index) {
    /* Compare against the session index: ra_hash_get_disc_index() resets after
     * every hash, so it never matched a session already on disc 2. */
    ra_runtime_log("runtime: disc check: already on disc %d; nothing to do\n", match + 1);
    ra_runtime_set_eval_hold(0);
    return;
  }
  if (!disc_check_hash(match, &hash))
    return;
  if (!disc_check_identity(match, &hash))
    return;
  disc_check_apply(match, &hash);
}

/* pspemu's settings closed: a Switch Discs may just have happened. Only record it;
 * the tick runs the check on the thread that owns the disc state. */
void ra_runtime_official_settings_closed(void) {
  g_rt.disc_after_settings = 1;
}

/* One line per session when the live serial cannot be read; the raw bytes show
 * which gate refused the read. */

static void runtime_disc_watch_report(void) {
  unsigned char raw[16];
  char hex[sizeof(raw) * 3 + 1];
  int readable = ra_runtime_memory_read_psp(RA_PSP_DISC_SERIAL_ADDRESS, raw, sizeof(raw));
  size_t i;
  size_t at = 0;

  hex[0] = '\0';
  if (readable) {
    for (i = 0; i < sizeof(raw) && at + 4 < sizeof(hex); ++i)
      at += (size_t)snprintf(hex + at, sizeof(hex) - at, "%02X ", raw[i]);
  } else {
    snprintf(hex, sizeof(hex), "-");
  }
  ra_runtime_log("runtime: disc watch: live serial unreadable at 0x%08X "
        "readable=%d valid=%d loaded=%d raw=%s\n",
        (unsigned)RA_PSP_DISC_SERIAL_ADDRESS, readable,
        ra_runtime_game_memory_valid(), g_rt.ps1_loaded, hex);
}

/* POPS can change disc without the settings UI (a Disc 2 save load). The live
 * serial is sampled every 2 s and re-armed per session. */

/* Two-second sampler gate: the cadence belongs to the watcher, not the session. */
static int disc_watch_due(void) {
  static unsigned long long next_check_us;
  unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();

  if (now < next_check_us)
    return 0;
  next_check_us = now + 2000000ULL;
  return 1;
}

/* A new session knows nothing yet: reset the media state and the disc anchor. */
static void disc_watch_session_reset(void) {
  g_rt.disc_watch_session = (const void*)g_rt.session;
  g_rt.last_live_serial[0] = '\0';
  g_rt.disc_watch_reported = 0;
  /* A restart for a foreign disc opens the session on that container ;
       * any other new session starts from container 0, which is what `ra_hash.c` hashes
       * until a swap is detected. */
  g_rt.session_disc_index = g_rt.forced_disc_index;
  g_rt.forced_disc_index = 0;
  ra_runtime_set_eval_hold(0);
  /* A new session knows nothing yet: drop the old media state so the first
     * sample records it again. */
  g_rt.media_state = RA_MEDIA_UNKNOWN;
  g_rt.media_failures = 0;
  /* The banner belongs to the session, so a new one is owed its own (/120). */
  ra_runtime_announced_disc_set(0);
}

/* The serial could not be read: count the failures, then declare the eject. */
static void disc_watch_handle_missing(void) {
  /* Plan C: an unreadable disc is an eject only after three samples and never
   * while disc work is in flight; transient POPS serial gaps are not ejects. */
  if (g_rt.paused || ra_runtime_disc_switch_hold() || g_rt.disc_after_settings ||
    g_rt.restart_disc_pending) {
    g_rt.media_failures = 0;
    return;
  }
  if (++g_rt.media_failures >= 3 && g_rt.media_state != RA_MEDIA_EJECTED) {
    g_rt.media_state = RA_MEDIA_EJECTED;
    ra_runtime_log("runtime: media ejected; achievements paused\n");
    ra_runtime_set_eval_hold(1);
    runtime_disc_retry_clear();
    runtime_media_eject();
    if (g_rt.settings.notifications)
      ra_notify_push(RA_NOTIFY_ERROR, 0, "Disc removed",
            "Achievements paused until a disc is inserted");
  }
  /* Never silent : a session with no `disc watch` line is
       * indistinguishable from a watch that never ran. */
  if (!g_rt.disc_watch_reported) {
    g_rt.disc_watch_reported = 1;
    runtime_disc_watch_report();
  }
}

/* A readable serial: track the eject/restore edge, then compare with the last
 * sample and run the disc check when it changed. */
static void disc_watch_handle_live(const char *live) {
  g_rt.media_failures = 0;
  if (g_rt.media_state == RA_MEDIA_EJECTED) {
    /* Plan C: the media is back. Leave "[NO HASH]" behind, release the hold, and fall
         * through to the ordinary comparison so a *different *disc is still handled. */
    g_rt.media_state = RA_MEDIA_PRESENT;
    ra_runtime_log("runtime: media present serial=%s\n", live);
    ra_runtime_set_eval_hold(0);
    runtime_media_restore();
  } else if (g_rt.media_state == RA_MEDIA_UNKNOWN) {
    g_rt.media_state = RA_MEDIA_PRESENT;
  }
  if (g_rt.last_live_serial[0] == '\0') {
        snprintf(g_rt.last_live_serial, sizeof(g_rt.last_live_serial), "%s", live);
    ra_runtime_log("runtime: disc watch: session disc=%s\n", live);
    /* Evaluate nothing until the switch is resolved: this sample can already be a
         * different game when POPS restored a hand-built multi-disc PBP. */
    ra_runtime_set_eval_hold(1);
    ra_runtime_disc_check();
    return;
  }
  if (strcmp(g_rt.last_live_serial, live) == 0)
    return;
  ra_runtime_log("runtime: disc watch: live disc changed %s -> %s (no menu)\n",
        g_rt.last_live_serial, live);
  snprintf(g_rt.last_live_serial, sizeof(g_rt.last_live_serial), "%s", live);
  /* Same rule as the first sample: no evaluation until this is resolved. */
  ra_runtime_set_eval_hold(1);
  ra_runtime_disc_check();
}

void ra_runtime_disc_watch(void) {
  char live[16];

  if (!disc_watch_due())
    return;

  if ((const void*)g_rt.session != g_rt.disc_watch_session)
    disc_watch_session_reset();

  if (!g_rt.game_is_pops || !ra_runtime_session_ready())
    return;
  if (g_rt.dev_media_forced_blank || !ra_runtime_disc_live_serial(live, sizeof(live))) {
    disc_watch_handle_missing();
    return;
  }
  disc_watch_handle_live(live);
}

void ra_runtime_disc_change_media_callback(int result, const char *error_message,
                    rc_client_t *client, void *userdata) {
  int disc = (int)(intptr_t)userdata;

  (void)client;
  if (result != RC_OK) {
    /* Not fatal by design: the session keeps the disc it already had, which is
         * what a hash belonging to another game must result in. */
    ra_runtime_log("runtime: disc change refused (%d%s%s); keeping the current "
          "disc\n", result, error_message != NULL ? " " : "",
          error_message != NULL ? error_message : "");
    /* change_media accepts a foreign hash with RC_OK, so the hash-library check above is
 * what refuses it. The media did change either way, so restart on that container. */
    g_rt.restart_disc_pending = disc > 0 ? disc - 1 : 0;
    return;
  }
  if (g_rt.disc_hash[0] != '\0') {
    ra_runtime_lock();
    snprintf(g_rt.session_hash, sizeof(g_rt.session_hash), "%s", g_rt.disc_hash);
    ra_runtime_unlock();
  }
  ra_runtime_log("runtime: disc changed; session continues on hash=%s\n",
        g_rt.disc_hash);
  /* Same game: release the evaluation hold and queue the disc card (/113). */
  ra_runtime_set_eval_hold(0);
  g_rt.disc_banner_pending = disc;
}

/* Called from the tick when the running title reports a different file. Adrenaline
 * already has the emulator paused for the swap, so hashing here costs the same kind
 * of stall the initial load pays. */
void ra_runtime_disc_changed(const char *path) {
  ra_hash_result_t hash;
  rc_client_t *rc;
  uint32_t library_id = 0;
  int is_ps1;
  if (g_rt.stop || g_rt.start_pending || !g_rt.running ||
    g_rt.session_hash[0] == '\0' || path == NULL || path[0] == '\0')
    return;
  if (strncmp(path, "disc0:", 6) == 0)
    return;
  if (strcmp(g_rt.disc_tried, path) == 0)
    return;
  ra_runtime_copy_text(g_rt.disc_tried, sizeof(g_rt.disc_tried), path);
  is_ps1 = RA_ENABLE_POPS ? ra_hash_probe_ps1(path) : 0;
  ra_runtime_log("runtime: disc change candidate '%s' ps1=%d\n", path, is_ps1);
  if (!(is_ps1 ? ra_hash_ps1(path, NULL, &hash)
        : ra_hash_game(path, NULL, &hash))) {
    ra_runtime_log("runtime: disc change hash failed: %s\n",
          ra_hash_last_error());
    return;
  }
  if (strcmp(hash.hash, g_rt.session_hash) == 0) {
    ra_runtime_log("runtime: disc change: same hash, nothing to do\n");
    return;
  }
  if (ra_runtime_hash_library_game_id(hash.hash, &library_id)) {
    uint32_t current = 0;
    ra_runtime_lock();
    current = g_rt.info.game_id;
    ra_runtime_unlock();
    if (current != 0 && library_id != current) {
      ra_runtime_log("runtime: disc change hash=%s belongs to game %u, "
            "session is game %u; not switching\n",
            hash.hash, (unsigned)library_id, (unsigned)current);
      return;
    }
    ra_runtime_log("runtime: disc change hash=%s is game %u (known variant)\n",
          hash.hash, (unsigned)library_id);
  } else if (ra_runtime_hash_library_count() > 0) {
    /* The library answered for other hashes but not this one: unknown dump, or
         * a disc this account cannot see. The server has the last word. */
    ra_runtime_log("runtime: disc change hash=%s unknown to the hash library\n",
          hash.hash);
  }
  snprintf(g_rt.disc_hash, sizeof(g_rt.disc_hash), "%s", hash.hash);
  {
    ra_session_t *session;
    ra_client_t *client = ra_runtime_client_acquire(&session);

    rc = client != NULL ? ra_client_get_rc_client(client) : NULL;
    if (rc != NULL) {
      ra_runtime_log("runtime: asking the client to change media to hash=%s\n",
            hash.hash);
      rc_client_begin_change_media(rc, hash.hash,
                    ra_runtime_disc_change_media_callback, NULL);
    }
    if (client != NULL)
      ra_runtime_client_release(session);
  }
}

/* Replay is requested when a link first exists in this session, and again if the
 * link comes back after being lost: an offline session that reconnects must deliver
 * what it queued (ra_offline.h). `g_rt.link_was_up` is the edge detector.
 *
 * Throttled to once every three seconds: `ra_http_link_up()` asks the system, and the
 * tick runs at 60 Hz. */
