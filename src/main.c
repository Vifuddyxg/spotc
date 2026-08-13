#include "spotc.h"
#include <ncurses.h>
#include <curl/curl.h>
#include <signal.h>

static long long pipeline_started_ms;

static int pipeline_start(App *a) {
    int rc = player_start(a);
    if (rc == 0) pipeline_started_ms = now_ms();
    return rc;
}

/* librespot/fx/pacat exited: restart once it ran long enough that this
 * isn't a crash loop, otherwise leave it down and point at the log */
static void pipeline_died(App *a) {
    if (now_ms() - pipeline_started_ms > 10000 && pipeline_start(a) == 0) {
        set_status(a, "audio pipeline restarted");
        return;
    }
    set_status(a, "librespot exited — see ~/.cache/spotc/librespot.log, R retries");
}

static void open_sidebar_sel(App *a) {
    set_status(a, "loading...");
    ui_draw(a);
    int rc;
    if (a->pl_sel == 0) rc = api_load_liked(a);
    else if (a->pl_sel <= a->n_albums)
        rc = api_load_album_tracks(a, &a->albums[a->pl_sel - 1]);
    else
        rc = api_load_playlist_tracks(a, &a->playlists[a->pl_sel - 1 - a->n_albums]);
    if (!rc) set_status(a, "");
    if (!rc) a->focus = FOCUS_TRACKS;
}

static void play_selected(App *a) {
    if (a->tr_sel >= a->n_tracks) return;
    Track *t = &a->tracks[a->tr_sel];
    if (t->is_album) {
        Playlist alb = {0};
        snprintf(alb.id, sizeof alb.id, "%s", t->id);
        snprintf(alb.uri, sizeof alb.uri, "%s", t->uri);
        snprintf(alb.name, sizeof alb.name, "%s", t->name);
        set_status(a, "loading...");
        ui_draw(a);
        if (!api_load_album_tracks(a, &alb)) set_status(a, "");
        return;
    }
    /* no active device? play straight on the local spotc device */
    const char *dev = NULL;
    if (!a->have_state || !a->active_device[0]) {
        api_fetch_devices(a);
        for (int i = 0; i < a->n_devices; i++)
            if (!strcmp(a->devices[i].name, a->cfg.device_name)) {
                dev = a->devices[i].id;
                break;
            }
    }
    int rc = a->tracks_ctx[0] ? api_play_context(a, a->tracks_ctx, a->tr_sel, dev)
                              : api_play_tracks(a, a->tracks, a->n_tracks, a->tr_sel, dev);
    if (!rc) {
        set_status(a, "");
        if (dev)
            snprintf(a->active_device, sizeof a->active_device, "%s", a->cfg.device_name);
        a->playing = 1;
        a->progress_ms = 0;
        a->now = *t;
        a->state_at_ms = now_ms();
        a->have_state = 1;
        a->last_poll_ms = now_ms() - 1000;   /* re-poll soon for real state */
    }
}

static void do_search(App *a) {
    char q[256] = {0};
    if (ui_prompt(" search › ", q, sizeof q) == 0) {
        set_status(a, "searching...");
        ui_draw(a);
        if (!api_search(a, q)) set_status(a, "");
        a->focus = FOCUS_TRACKS;
    }
}

static void set_fx(App *a, float speed, float reverb) {
    a->speed = speed;
    a->reverb = reverb;
    player_set_fx(a);
    a->cfg.speed = a->speed;
    a->cfg.reverb = a->reverb;
    config_save(&a->cfg);
    if (!a->pipeline_up || strcmp(a->active_device, a->cfg.device_name))
        set_status(a, "slow/reverb apply when playing on '%s' (d)", a->cfg.device_name);
}

static void set_retune(App *a, int preset) {
    const RetunePreset *p = &retune_presets[preset];
    /* retune and slow+reverb are exclusive: stacked they would drag the
     * pitch off the advertised Hz, and the whole point is hitting it */
    int had_fx = a->speed < 0.999f || a->reverb > 0.001f;
    a->retune = p->ratio;
    a->cfg.retune = a->retune;
    if (preset != 0) {
        a->speed = 1.0f;
        a->reverb = p->reverb;   /* deep presets bring their own reverb */
        a->cfg.speed = a->speed;
        a->cfg.reverb = a->reverb;
    }
    player_set_fx(a);
    config_save(&a->cfg);
    if (!a->pipeline_up || strcmp(a->active_device, a->cfg.device_name)) {
        set_status(a, "retune applies when playing on '%s' (d)", a->cfg.device_name);
        return;
    }
    if (preset == 0)
        set_status(a, "retune off — standard 440 Hz tuning");
    else if (p->reverb > 0.001f)
        set_status(a, "retune %s = %d Hz (%.3fx) + reverb %d%%",
                   p->note, p->hz, a->retune, (int)(p->reverb * 100));
    else
        set_status(a, "retune %s = %d Hz (%.3fx)%s", p->note, p->hz, a->retune,
                   had_fx ? "  ·  slow+reverb off" : "");
}

static int key_is(int ch, int configured) {
    return configured > 0 && ch == configured;
}

static void slow_reverb(App *a, int stronger) {
    static const float speeds[] = { 1.00f, 0.92f, 0.86f, 0.80f, 0.74f,
                                    0.68f, 0.62f, 0.56f, 0.50f };
    static const float reverbs[] = { 0.00f, 0.10f, 0.18f, 0.26f, 0.34f,
                                     0.42f, 0.50f, 0.58f, 0.65f };
    enum { FX_LEVELS = (int)(sizeof speeds / sizeof *speeds) - 1 };
    /* clear retune even when toggling off: a deep preset's slowdown must not
     * survive an "o" press that reads as "turn the effects off" */
    int had_retune = a->retune < 0.999f || a->retune > 1.001f;
    int level = 0;
    if (!had_retune) {
        /* a deep preset's reverb is not a slow level: matching it against the
         * ladder would make the first "p" jump straight to 0.80x */
        float nearest = 1000.0f;
        for (int i = 0; i <= FX_LEVELS; i++) {
            float d = (a->speed - speeds[i]) * (a->speed - speeds[i]) +
                      (a->reverb - reverbs[i]) * (a->reverb - reverbs[i]);
            if (d < nearest) { nearest = d; level = i; }
        }
    }
    if (!stronger) level = level ? 0 : 1;
    else if (level < FX_LEVELS) level++;
    if (had_retune) {
        a->retune = 1.0f;
        a->cfg.retune = 1.0f;
    }
    set_fx(a, speeds[level], reverbs[level]);
    if (a->pipeline_up && !strcmp(a->active_device, a->cfg.device_name)) {
        if (level)
            set_status(a, "slow + reverb %d/%d  %.2fx  rev %d%%%s",
                       level, FX_LEVELS, speeds[level], (int)(reverbs[level] * 100),
                       had_retune ? "  ·  retune off" : "");
        else
            set_status(a, "slow + reverb off%s",
                       had_retune ? "  ·  retune off" : "");
    }
}

static void logout(App *a) {
    player_stop(a);
    auth_logout(a);
    a->have_state = a->playing = 0;
    a->active_device[0] = 0;
    a->screen = SCREEN_LOGIN;
    if (pipeline_start(a) != 0)
        set_status(a, "logged out — librespot missing");
    else
        set_status(a, "logged out — press l to sign in again");
}

static void request_volume(App *a, int delta) {
    int base = a->volume_target >= 0 ? a->volume_target : a->volume;
    int target = base + delta;
    if (target < 0) target = 0;
    if (target > 100) target = 100;
    a->volume = target;              /* never let a stale poll move the UI */
    a->volume_target = target;
    a->volume_send_at_ms = now_ms() + 140;  /* coalesce rapid +/- input */
    a->volume_hold_until_ms = now_ms() + 3000;
}

static void flush_volume(App *a) {
    if (a->volume_target < 0 || !a->volume_send_at_ms ||
        now_ms() < a->volume_send_at_ms) return;
    int target = a->volume_target;
    a->volume_send_at_ms = 0;
    if (api_set_volume(a, target) != 0) {
        a->volume_target = -1;
        return;
    }
    a->volume = target;
    a->volume_hold_until_ms = now_ms() + 3000;
    a->last_poll_ms = now_ms() - 1000;
}

static void seek_rel(App *a, int delta_ms) {
    if (!a->have_state || !a->now.duration_ms) return;
    int prog = a->progress_ms;
    if (a->playing) prog += (int)(now_ms() - a->state_at_ms);
    int target = prog + delta_ms;
    if (target > a->now.duration_ms) target = a->now.duration_ms - 1000;
    if (api_seek(a, target) == 0) {
        a->progress_ms = target < 0 ? 0 : target;
        a->state_at_ms = now_ms();
    }
}

static void handle_popup_key(App *a, int ch) {
    if (ch == 27 || key_is(ch, a->cfg.key_quit) || key_is(ch, a->cfg.key_devices) ||
        key_is(ch, a->cfg.key_help)) { a->popup = POPUP_NONE; return; }
    if (a->popup == POPUP_RETUNE) {
        if (ch >= '0' && ch < '0' + RETUNE_PRESETS) {
            set_retune(a, ch - '0');
            a->popup = POPUP_NONE;
        } else if (key_is(ch, a->cfg.key_retune)) {
            a->popup = POPUP_NONE;
        }
        return;
    }
    if (a->popup != POPUP_DEVICES) return;
    if (key_is(ch, a->cfg.key_down) || ch == KEY_DOWN) {
        if (a->dev_sel < a->n_devices - 1) a->dev_sel++;
    } else if (key_is(ch, a->cfg.key_up) || ch == KEY_UP) {
        if (a->dev_sel > 0) a->dev_sel--;
    } else if (ch == '\n' || ch == KEY_ENTER || key_is(ch, a->cfg.key_play_pause)) {
        if (a->dev_sel < a->n_devices) {
            if (api_transfer(a, a->devices[a->dev_sel].id) == 0)
                set_status(a, "playing on %s", a->devices[a->dev_sel].name);
            a->popup = POPUP_NONE;
            a->last_poll_ms = now_ms() - 1500;
        }
    }
}

static void handle_key(App *a, int ch) {
    if (a->popup != POPUP_NONE) { handle_popup_key(a, ch); return; }

    if (key_is(ch, a->cfg.key_quit)) { a->running = 0; return; }
    if (key_is(ch, a->cfg.key_focus) || ch == KEY_LEFT || ch == KEY_RIGHT) {
        a->focus = a->focus == FOCUS_SIDEBAR ? FOCUS_TRACKS : FOCUS_SIDEBAR;
    } else if (key_is(ch, a->cfg.key_down) || ch == KEY_DOWN) {
        if (a->focus == FOCUS_SIDEBAR) {
            if (a->pl_sel < sidebar_total(a) - 1) a->pl_sel++;
        } else if (a->tr_sel < a->n_tracks - 1) a->tr_sel++;
    } else if (key_is(ch, a->cfg.key_up) || ch == KEY_UP) {
        if (a->focus == FOCUS_SIDEBAR) {
            if (a->pl_sel > 0) a->pl_sel--;
        } else if (a->tr_sel > 0) a->tr_sel--;
    } else if (ch == KEY_NPAGE || ch == KEY_PPAGE) {
        if (a->focus == FOCUS_TRACKS) {
            int page = LINES - 8;   /* visible track rows */
            if (page < 1) page = 1;
            a->tr_sel += ch == KEY_NPAGE ? page : -page;
            if (a->tr_sel >= a->n_tracks) a->tr_sel = a->n_tracks - 1;
            if (a->tr_sel < 0) a->tr_sel = 0;
        }
    } else if (key_is(ch, a->cfg.key_top)) {
        if (a->focus == FOCUS_TRACKS) a->tr_sel = 0; else a->pl_sel = 0;
    } else if (key_is(ch, a->cfg.key_bottom)) {
        if (a->focus == FOCUS_TRACKS && a->n_tracks) a->tr_sel = a->n_tracks - 1;
        else a->pl_sel = sidebar_total(a) - 1;
    } else if (key_is(ch, a->cfg.key_play_pause)) {
        if (a->playing) { if (!api_pause(a)) { a->playing = 0;
            a->progress_ms += (int)(now_ms() - a->state_at_ms); } }
        else if (!api_resume(a)) { a->playing = 1; a->state_at_ms = now_ms(); }
    } else if (ch == '\n' || ch == KEY_ENTER) {
        if (a->focus == FOCUS_SIDEBAR) open_sidebar_sel(a);
        else play_selected(a);
    } else if (key_is(ch, a->cfg.key_next)) {
        if (!api_next(a)) a->last_poll_ms = now_ms() - 1500;
    } else if (key_is(ch, a->cfg.key_prev)) {
        if (!api_prev(a)) a->last_poll_ms = now_ms() - 1500;
    } else if (key_is(ch, a->cfg.key_seek_back)) {
        seek_rel(a, -10000);
    } else if (key_is(ch, a->cfg.key_seek_forward)) {
        seek_rel(a, 10000);
    } else if (key_is(ch, a->cfg.key_volume_down)) {
        request_volume(a, -5);
    } else if (key_is(ch, a->cfg.key_volume_up)) {
        request_volume(a, +5);
    } else if (key_is(ch, a->cfg.key_shuffle)) {
        if (!api_set_shuffle(a, !a->shuffle)) a->shuffle = !a->shuffle;
    } else if (key_is(ch, a->cfg.key_repeat)) {
        if (!api_set_repeat(a, (a->repeat + 1) % 3)) a->repeat = (a->repeat + 1) % 3;
    } else if (key_is(ch, a->cfg.key_queue)) {
        if (a->focus == FOCUS_TRACKS && a->tr_sel < a->n_tracks) {
            if (!api_queue_add(a, a->tracks[a->tr_sel].uri))
                set_status(a, "queued: %s", a->tracks[a->tr_sel].name);
        }
    } else if (key_is(ch, a->cfg.key_slow_reverb)) {
        slow_reverb(a, 0);
    } else if (key_is(ch, a->cfg.key_more_slow_reverb)) {
        slow_reverb(a, 1);
    } else if (key_is(ch, a->cfg.key_fx_reset)) {
        a->retune = 1.0f;
        a->cfg.retune = 1.0f;
        set_fx(a, 1.0f, 0.0f);
    } else if (key_is(ch, a->cfg.key_retune)) {
        a->popup = POPUP_RETUNE;
    } else if (key_is(ch, a->cfg.key_search)) {
        do_search(a);
    } else if (key_is(ch, a->cfg.key_devices)) {
        api_fetch_devices(a);
        a->popup = POPUP_DEVICES;
    } else if (key_is(ch, a->cfg.key_help)) {
        a->popup = POPUP_HELP;
    } else if (key_is(ch, a->cfg.key_reload)) {
        int pipeline_ok = a->pipeline_up || pipeline_start(a) == 0;
        int playlists_ok = api_load_playlists(a) == 0;
        int albums_ok = api_load_albums(a) == 0;
        if (playlists_ok && albums_ok)
            set_status(a, pipeline_ok ? "reloaded" : "reloaded — librespot still missing");
    } else if (key_is(ch, a->cfg.key_logout)) {
        logout(a);
    }
}

static void enter_main(App *a) {
    a->screen = SCREEN_MAIN;
    set_status(a, "loading library...");
    ui_draw(a);
    int playlists_ok = api_load_playlists(a) == 0;
    int albums_ok = api_load_albums(a) == 0;
    int state_ok = api_fetch_state(a) == 0;
    if (playlists_ok && albums_ok && state_ok) set_status(a, "");
}

int main(void) {
    App a = {0};
    a.running = 1;
    a.volume_target = -1;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    config_load(&a.cfg);
    a.speed = a.cfg.speed;
    a.reverb = a.cfg.reverb;
    a.retune = a.cfg.retune;
    signal(SIGPIPE, SIG_IGN);

    /* stale tokens minted with a different client id must not short-circuit login */
    int logged = a.cfg.client_id[0] && tokens_load(&a.tok) == 0
                 && strcmp(a.tok.cid, a.cfg.client_id) == 0;
    if (!logged) memset(&a.tok, 0, sizeof a.tok);
    a.screen = logged ? SCREEN_MAIN : SCREEN_LOGIN;

    ui_init();

    if (pipeline_start(&a) != 0)
        set_status(&a, "librespot missing — remote control only");
    if (a.screen == SCREEN_MAIN)
        enter_main(&a);

    while (a.running) {
        ui_draw(&a);
        int ch = getch();

        if (a.screen == SCREEN_LOGIN) {
            if (key_is(ch, a.cfg.key_quit)) break;
            if (key_is(ch, a.cfg.key_client_id)) {
                char id[128] = {0};
                if (ui_prompt(" Client ID › ", id, sizeof id) == 0) {
                    char *trimmed = xstrdup_trim(id);
                    snprintf(a.cfg.client_id, sizeof a.cfg.client_id, "%s", trimmed);
                    free(trimmed);
                    config_save(&a.cfg);
                    set_status(&a, "saved — now press l to sign in");
                }
            } else if (key_is(ch, a.cfg.key_login)) {
                if (!a.cfg.client_id[0]) {
                    set_status(&a, "press i first and paste your Client ID");
                } else {
                    set_status(&a, "browser opened — finish signing in there...");
                    ui_draw(&a);
                    if (auth_login(&a) == 0) enter_main(&a);
                    else set_status(&a, "sign-in failed — check the Redirect URI, then press l");
                }
            }
            continue;
        }

        if (ch != ERR) handle_key(&a, ch);

        if (player_check(&a)) pipeline_died(&a);
        flush_volume(&a);

        if (now_ms() - a.last_poll_ms > 2500)
            api_fetch_state(&a);
    }

    ui_shutdown();
    player_stop(&a);
    curl_global_cleanup();
    return 0;
}
