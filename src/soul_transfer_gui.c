/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file soul_transfer_gui.c
 *     Gloomlight mode: what the Archon and Keeper see of a soul transfer.
 * @par Purpose:
 *     - A status box in the free area below the side panel: the Archon's cooldown, readiness
 *       and current pick, or the Keeper's view of its Archon and of a pending request.
 *     - Markers in the thought cloud over the vessel (Possess icon) and the sacrifice (Temple icon).
 *     - The "smacked back" feedback when the Keeper refuses, and the cooldown alarm.
 *     - Short personal hints for the Archon.
 * @par Comment:
 *     Local only. Everything reads the synced state but never changes it.
 *     TODO: the texts are hardcoded English; move them to translated GUI strings.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include "soul_transfer_gui.h"

#include "bflib_keybrd.h"
#include "bflib_sprfnt.h"
#include "bflib_vidraw.h"
#include "bflib_video.h"
#include "config_creature.h"
#include "config_magic.h"
#include "config_sounds.h"
#include "config_strings.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_graphics.h"
#include "custom_sprites.h"
#include "dungeon_data.h"
#include "frontend.h"
#include "frontmenu_ingame_evnt.h"
#include "frontmenu_ingame_tabs.h"
#include "sprites.h"
#include "game_legacy.h"
#include "gui_draw.h"
#include "gui_frontbtns.h"
#include "gui_frontmenu.h"
#include "gui_msgs.h"
#include "kjm_input.h"
#include "packets.h"
#include "player_data.h"
#include "room_data.h"
#include "soul_transfer.h"
#include "sounds.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "user_roles.h"
#include "vidfade.h"
#include <stdarg.h>
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define SOUL_TRANSFER_HINT_SECONDS 4
#define SOUL_TRANSFER_SMACK_PAIN 8
#define SOUL_TRANSFER_HUD_LINES 5
#define SOUL_TRANSFER_HUD_LINE_LEN 96

static char hint_text[SOUL_TRANSFER_HUD_LINE_LEN];
static GameTurn hint_until_turn = 0;

/** A short personal hint, shown in the status box of the local user for a few seconds. */
void soul_transfer_hint(NetUserId user, const char *fmt_str, ...)
{
    if ((user < 0) || !is_local_user(user)) {
        return;
    }
    va_list val;
    va_start(val, fmt_str);
    vsnprintf(hint_text, sizeof(hint_text), fmt_str, val);
    va_end(val);
    hint_until_turn = get_gameturn() + SOUL_TRANSFER_HINT_SECONDS * turns_per_second;
}

/** The Keeper refused: the Archon's screen flashes red and it hears a slap. */
void soul_transfer_smack_back(NetUserId archon)
{
    PaletteApplyPainToUser(archon, SOUL_TRANSFER_SMACK_PAIN);
    play_user_sample(archon, get_power_model_stats(PwrK_SLAP)->select_sound_idx);
    soul_transfer_hint(archon, "Refused! Soul smacked back");
}

/** The cooldown is about to end: the Archon hears the alarm bell. */
void soul_transfer_cooldown_alarm(NetUserId archon)
{
    play_user_sample(archon, snd_alarm);
}

/******************************************************************************/

static const struct UserState *local_team_user(PlayerNumber *plyr_idx, const struct SoulTransfer **st)
{
    const struct UserState *ustate = get_user_state(get_local_user());
    if (user_state_invalid(ustate) || (ustate->player_id < 0) || (ustate->player_id >= PLAYERS_COUNT)) {
        return NULL;
    }
    if (seat_user_with_role(ustate->player_id, UsrRole_Archon) < 0) {
        return NULL; // no Gloomlight on this team
    }
    *st = get_team_soul_transfer(ustate->player_id);
    if (*st == NULL) {
        return NULL;
    }
    *plyr_idx = ustate->player_id;
    return ustate;
}

static const struct Thing *pair_creature(ThingIndex idx, GameTurn creation, PlayerNumber plyr_idx)
{
    if (idx == 0) {
        return NULL;
    }
    const struct Thing *thing = thing_get(idx);
    if (!thing_exists(thing) || !thing_is_creature(thing) || (thing->creation_turn != creation) || (thing->owner != plyr_idx)) {
        return NULL;
    }
    return thing;
}

/** "Dragon L5" */
static void creature_short_name(const struct Thing *thing, char *buf, size_t size)
{
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    snprintf(buf, size, "%s L%d", get_string(crconf->namestr_idx), (int)cctrl->exp_level + 1);
}

static int seconds_left(GameTurn deadline)
{
    GameTurn now = get_gameturn();
    if ((deadline <= now) || (turns_per_second <= 0)) {
        return 0;
    }
    return (int)((deadline - now + turns_per_second - 1) / turns_per_second);
}

/** Where the side panel's "place filler" frame is drawn, if it is (same scale and condition as draw_placefiller()). */
static TbBool get_placefiller_frame(long *frame_x, long *frame_y, long *frame_w, long *frame_h)
{
    const MenuNumber mnu_idx = menu_id_to_number(GMnu_MAIN);
    if (mnu_idx == MENU_INVALID_ID) {
        return false;
    }
    const struct GuiMenu *gmnu = get_active_menu(mnu_idx);
    const unsigned char placefill_threshold = (RendererPhysicalHeight() >= 400) ? 80 : 40;
    if (RendererPhysicalHeight() - gmnu->height < placefill_threshold) {
        return false;
    }
    const int fs_units_per_px = (gmnu->height * 16 + 8) / LbTiledSpriteHeight(&status_panel);
    const struct TbSprite *spr = get_panel_sprite(GPS_rpanel_rpanel_extra);
    *frame_x = 0;
    *frame_y = gmnu->pos_y + gmnu->height;
    *frame_w = spr->SWidth * fs_units_per_px / 16;
    *frame_h = spr->SHeight * fs_units_per_px / 16;
    return (*frame_w > 0) && (*frame_h > 0);
}

/** Draws the lines inside the side panel's place filler frame, shrinking the text to fit it.
 * Without that frame (short window or hidden panel), draws its own box in the bottom-left corner of the view. */
static void draw_status_box(char lines[][SOUL_TRANSFER_HUD_LINE_LEN], int count)
{
    if (count <= 0) {
        return;
    }
    LbTextSetFont(winfont);
    int tx_units_per_px = font_size_units_per_px(chat_font_size);
    long text_w = 1;
    for (int i = 0; i < count; i++) {
        text_w = max(text_w, (long)LbTextStringWidth(lines[i]));
    }
    long box_x;
    long box_y;
    long box_w;
    long box_h;
    long frame_x;
    long frame_y;
    long frame_w;
    long frame_h;
    if (get_placefiller_frame(&frame_x, &frame_y, &frame_w, &frame_h)) {
        // the recessed area inside the frame's bevelled rim (sprite is 140x80; rim about 10 units wide)
        box_x = frame_x + frame_w * 10 / 140;
        box_w = frame_w * 120 / 140;
        box_y = frame_y + frame_h * 10 / 80;
        box_h = frame_h * 60 / 80;
        if (text_w * tx_units_per_px / 16 > box_w) {
            tx_units_per_px = (int)(box_w * 16 / text_w);
        }
        if (count * LbTextLineHeight() * tx_units_per_px / 16 > box_h) {
            tx_units_per_px = (int)(box_h * 16 / (count * LbTextLineHeight()));
        }
        tx_units_per_px = max(4, tx_units_per_px);
    } else {
        const long margin = 6 * units_per_pixel_ui / 16;
        const long pad = 6 * units_per_pixel_ui / 16;
        box_w = text_w * tx_units_per_px / 16 + 2 * pad;
        box_h = count * LbTextLineHeight() * tx_units_per_px / 16 + 2 * pad;
        const MenuNumber mnu_idx = menu_id_to_number(GMnu_MAIN);
        box_x = margin;
        if (mnu_idx != MENU_INVALID_ID) {
            const struct GuiMenu *gmnu = get_active_menu(mnu_idx);
            box_x = gmnu->pos_x + gmnu->width + margin;
        }
        box_y = MyScreenHeight - box_h - margin;
        draw_slab64k(box_x, box_y, units_per_pixel, box_w, box_h);
    }
    const long line_h = LbTextLineHeight() * tx_units_per_px / 16;
    const long text_top = box_y + (box_h - count * line_h) / 2;
    RendererSetDrawFlags(Lb_TEXT_HALIGN_CENTER);
    for (int i = 0; i < count; i++) {
        LbTextSetWindow(box_x, text_top + i * line_h, box_w, line_h);
        LbTextDrawResized(0, 0, tx_units_per_px, lines[i]);
    }
    RendererSetDrawFlags(0);
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
}

/** Draws the soul transfer status of the local user, if its team plays Gloomlight. */
void draw_soul_transfer_hud(void)
{
    PlayerNumber plyr_idx;
    const struct SoulTransfer *st;
    const struct UserState *ustate = local_team_user(&plyr_idx, &st);
    if (ustate == NULL) {
        return;
    }
    char lines[SOUL_TRANSFER_HUD_LINES][SOUL_TRANSFER_HUD_LINE_LEN];
    int count = 0;
    char name[64];
    const int seconds = seconds_left(st->phase_deadline);
    const struct Thing *target = pair_creature(st->target_idx, st->target_creation, plyr_idx);
    const struct Thing *sacrifice = pair_creature(st->sacrifice_idx, st->sacrifice_creation, plyr_idx);
    if (ustate->role == UsrRole_Archon)
    {
        if ((ustate->view_type == PVT_CreatureContrl) || (ustate->view_type == PVT_CreaturePasngr)) {
            return; // inside the vessel
        }
        switch (st->phase)
        {
        case SoulTr_Cooldown:
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "Soul transfer");
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "in %d:%02d", seconds / 60, seconds % 60);
            break;
        case SoulTr_Ready:
            if (target != NULL) {
                creature_short_name(target, name, sizeof(name));
                snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "Vessel: %s", name);
                snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "Sacrifice worth %ld+", get_creature_thing_score(target));
            } else {
                snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "Soul transfer ready");
                snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "Click a vessel");
            }
            break;
        case SoulTr_AwaitKeeper:
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "Waiting for Keeper");
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "%ds", seconds);
            break;
        default:
            return;
        }
        if (get_gameturn() < hint_until_turn) {
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "%s", hint_text);
        }
    } else
    {
        switch (st->phase)
        {
        case SoulTr_AwaitKeeper:
            if ((target == NULL) || (sacrifice == NULL) || menu_is_active(GMnu_SOUL_TRANSFER)) {
                return; // the popup shows the request
            }
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "SOUL TRANSFER");
            creature_short_name(target, name, sizeof(name));
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "Vessel: %s", name);
            creature_short_name(sacrifice, name, sizeof(name));
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "Sacrifice: %s", name);
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "[Y] yes  [N] no  %ds", seconds);
            break;
        case SoulTr_Cooldown:
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "Archon");
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "ready in %d:%02d", seconds / 60, seconds % 60);
            break;
        case SoulTr_Ready:
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "Archon");
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "ready");
            break;
        case SoulTr_Possessing:
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "Archon");
            snprintf(lines[count++], SOUL_TRANSFER_HUD_LINE_LEN, "possessing");
            break;
        default:
            return;
        }
    }
    draw_status_box(lines, count);
}

/******************************************************************************/
// The Keeper's comparison popup (GMnu_SOUL_TRANSFER): vessel and sacrifice side by side,
// a countdown bar, and approve / refuse buttons. Not modal: the Keeper keeps playing around it.
// Y and N are its hotkeys and also work while it isn't shown (eg. while the Keeper possesses).

/** The deadline of the request the local Keeper already answered, so the popup doesn't reopen
 * in the turns before the answer is processed. */
static GameTurn answered_deadline = 0;
/** An answer waiting for a free packet action slot: -1 none, 0 refuse, 1 approve. */
static int pending_answer = -1;

static void queue_answer(TbBool approve)
{
    PlayerNumber plyr_idx;
    const struct SoulTransfer *st;
    if ((local_team_user(&plyr_idx, &st) == NULL) || !soul_transfer_awaits_local_keeper()) {
        return;
    }
    answered_deadline = st->phase_deadline;
    pending_answer = approve ? 1 : 0;
}

static void soul_transfer_popup_approve(struct GuiButton *gbtn)
{
    SYNCLOG("Soul transfer popup: approve clicked");
    queue_answer(true);
}

static void soul_transfer_popup_refuse(struct GuiButton *gbtn)
{
    SYNCLOG("Soul transfer popup: refuse clicked");
    queue_answer(false);
}

/** The approve / refuse button: the usual button sprite, plus a gold frame around its clickable area
 * while the mouse is over it. */
static void draw_soul_transfer_answer_button(struct GuiButton *gbtn)
{
    gui_area_normal_button(gbtn);
    if (((gbtn->flags & LbBtnF_MouseOver) == 0) || ((gbtn->flags & LbBtnF_Enabled) == 0)) {
        return;
    }
    const unsigned long flags_mem = RendererGetDrawFlags();
    RendererSetDrawFlags(0);
    const long t = max(2L, (long)(2 * units_per_pixel_ui / 16));
    const long x = gbtn->pos_x - t;
    const long y = gbtn->pos_y - t;
    const long w = gbtn->width + 2 * t;
    const long h = gbtn->height + 2 * t;
    // Deep amber gold, closer to the popup's ornate border than a pure yellow.
    const TbPixel gold = colours[13][9][2];
    LbDrawBox(x, y, w, t, gold);
    LbDrawBox(x, y + h - t, w, t, gold);
    LbDrawBox(x, y, t, h, gold);
    LbDrawBox(x + w - t, y, t, h, gold);
    RendererSetDrawFlags(flags_mem);
}

/** Draws one side of the comparison: label, portrait in a hex frame, name, level, value and health. */
static void draw_popup_creature(const struct Thing *thing, const char *label, long x, long y, long w, long h, int tx_units_per_px)
{
    const long line_h = LbTextLineHeight() * tx_units_per_px / 16;
    RendererSetDrawFlags(Lb_TEXT_HALIGN_CENTER);
    LbTextSetWindow(x, y, w, line_h);
    LbTextDrawResized(0, 0, tx_units_per_px, label);
    long text_y = y + line_h;
    // portrait, framed like the creature query panel
    const struct TbSprite *frame = get_panel_sprite(GPS_rpanel_frame_double_hex_med);
    long frame_w = w * 55 / 100;
    long frame_h = h - 5 * line_h;
    if ((frame != NULL) && (frame->SWidth > 0) && (frame->SHeight > 0) && (frame_h > 0)) {
        int ps_units_per_px = min((int)(frame_w * 16 / frame->SWidth), (int)(frame_h * 16 / frame->SHeight));
        frame_w = frame->SWidth * ps_units_per_px / 16;
        frame_h = frame->SHeight * ps_units_per_px / 16;
        const long frame_x = x + (w - frame_w) / 2;
        const long frame_y = text_y + line_h / 4;
        const long spr_idx = get_creature_model_graphics(thing->model, CGI_QuerySymbol);
        if (spr_idx > 0) {
            const struct TbSprite *portrait = get_button_sprite_for_player(spr_idx, thing->owner);
            if ((portrait != NULL) && (portrait->SWidth > 0)) {
                const int bs_units_per_px = (frame_w * 67 / 100) * 16 / portrait->SWidth;
                const long pw = portrait->SWidth * bs_units_per_px / 16;
                const long ph = portrait->SHeight * bs_units_per_px / 16;
                LbSpriteDrawResized(frame_x + (frame_w - pw) / 2, frame_y + (frame_h - ph) / 2, bs_units_per_px, portrait);
            }
        }
        draw_gui_panel_sprite_left(frame_x, frame_y, ps_units_per_px, GPS_rpanel_frame_double_hex_med);
        text_y = frame_y + frame_h + line_h / 4;
    }
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const long health_pct = (cctrl->max_health > 0) ? (100 * (long)thing->health / cctrl->max_health) : 0;
    char text[SOUL_TRANSFER_HUD_LINE_LEN];
    const char *lines[4];
    char level_text[32];
    char worth_text[48];
    char health_text[32];
    snprintf(level_text, sizeof(level_text), "Level %d", (int)cctrl->exp_level + 1);
    snprintf(worth_text, sizeof(worth_text), "Worth %ld", get_creature_thing_score(thing));
    snprintf(health_text, sizeof(health_text), "Health %ld%%", health_pct);
    snprintf(text, sizeof(text), "%s", get_string(crconf->namestr_idx));
    lines[0] = text;
    lines[1] = level_text;
    lines[2] = worth_text;
    lines[3] = health_text;
    for (int i = 0; i < 4; i++) {
        LbTextSetWindow(x, text_y + i * line_h, w, line_h);
        LbTextDrawResized(0, 0, tx_units_per_px, lines[i]);
    }
    RendererSetDrawFlags(0);
}

/** Draw callback of the popup's information area. */
static void draw_soul_transfer_popup(struct GuiButton *gbtn)
{
    PlayerNumber plyr_idx;
    const struct SoulTransfer *st;
    if ((local_team_user(&plyr_idx, &st) == NULL) || (st->phase != SoulTr_AwaitKeeper)) {
        return;
    }
    const struct Thing *target = pair_creature(st->target_idx, st->target_creation, plyr_idx);
    const struct Thing *sacrifice = pair_creature(st->sacrifice_idx, st->sacrifice_creation, plyr_idx);
    if ((target == NULL) || (sacrifice == NULL)) {
        return;
    }
    LbTextSetFont(winfont);
    // the font height decides everything else; the widest block of text is about 12 characters per column
    int tx_units_per_px = gbtn->height * 16 / (13 * LbTextLineHeight());
    tx_units_per_px = max(4, tx_units_per_px);
    const long line_h = LbTextLineHeight() * tx_units_per_px / 16;
    const long x = gbtn->scr_pos_x;
    const long y = gbtn->scr_pos_y;
    const long w = gbtn->width;
    const long h = gbtn->height;
    // title
    RendererSetDrawFlags(Lb_TEXT_HALIGN_CENTER);
    LbTextSetWindow(x, y, w, line_h);
    LbTextDrawResized(0, 0, tx_units_per_px, "SOUL TRANSFER");
    RendererSetDrawFlags(0);
    // the two creatures, with room left for the countdown below them
    const long body_y = y + line_h * 3 / 2;
    const long body_h = h - line_h * 3 / 2 - line_h * 2;
    draw_popup_creature(target, "Vessel", x, body_y, w / 2, body_h, tx_units_per_px);
    draw_popup_creature(sacrifice, "Sacrifice", x + w / 2, body_y, w - w / 2, body_h, tx_units_per_px);
    // countdown bar: full at the request, empty when both creatures die
    const GameTurnDelta total = max((GameTurnDelta)1, game.conf.rules[plyr_idx].magic.soul_transfer_decision_time);
    const GameTurn now = get_gameturn();
    const GameTurnDelta left = (st->phase_deadline > now) ? (GameTurnDelta)(st->phase_deadline - now) : 0;
    const long bar_x = x + w / 10;
    const long bar_w = w - w / 5;
    const long bar_h = max(4L, line_h / 2);
    const long bar_y = y + h - line_h - bar_h - line_h / 4;
    LbDrawBox(bar_x, bar_y, bar_w, bar_h, colours[0][0][0]);
    const long fill_w = bar_w * left / total;
    if (fill_w > 0) {
        // red, flashing in the last two seconds
        const TbBool flash = (left < 2 * turns_per_second) && ((now / 4) % 2 == 0);
        LbDrawBox(bar_x, bar_y, fill_w, bar_h, flash ? colours[15][15][0] : colours[15][0][0]);
    }
    char text[SOUL_TRANSFER_HUD_LINE_LEN];
    snprintf(text, sizeof(text), "[Y] approve   %ds   [N] refuse", seconds_left(st->phase_deadline));
    RendererSetDrawFlags(Lb_TEXT_HALIGN_CENTER);
    LbTextSetWindow(x, y + h - line_h, w, line_h);
    LbTextDrawResized(0, 0, tx_units_per_px, text);
    RendererSetDrawFlags(0);
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
}

struct GuiButtonInit soul_transfer_buttons[] = {
  { 0,  0, 0, 0, NULL,                      NULL, NULL, 0,  10,  10,  10,  10, 300, 200, draw_soul_transfer_popup, 0, GUIStr_Empty, 0, {0}, 0, NULL },
  // The tick/cross sprites have empty space above the glyph: drawn from scr_pos, the visible glyph sits about
  // (+2,+34) lower. Armageddon's confirmation uses (+2,+34) and 46x32; ours is trimmed to (+2,+32) and 46x28 so
  // the hover frame hugs the glyph (32 tall ran past its bottom edge).
  { 0,  0, 0, 1, soul_transfer_popup_refuse,  NULL, NULL, 0,  90, 218,  92, 250,  46,  28, draw_soul_transfer_answer_button, 46, GUIStr_Empty, 0, {0}, 0, NULL },
  { 0,  0, 0, 1, soul_transfer_popup_approve, NULL, NULL, 0, 184, 218, 186, 250,  46,  28, draw_soul_transfer_answer_button, 48, GUIStr_Empty, 0, {0}, 0, NULL },
  {-1,  0, 0, 0, NULL,                      NULL, NULL, 0,   0,   0,   0,   0,   0,   0, NULL,                     0, 0,            0, {0}, 0, NULL },
};

// Monopoly like the Armageddon confirmation: while it's up only its buttons take clicks (other menus' buttons
// could otherwise take them). Clicks on the dungeon itself still work.
struct GuiMenu soul_transfer_menu =
 { GMnu_SOUL_TRANSFER, 0, 4, soul_transfer_buttons, POS_GAMECTR, POS_GAMECTR, 320, 260, gui_pretty_background, 0, NULL, NULL, 0, 1, 0,};

/** Local input of the Keeper each frame: opens or closes the popup, sends a queued answer, handles Y/N.
 * @return True if a key was used. */
TbBool soul_transfer_keeper_inputs(void)
{
    PlayerNumber plyr_idx;
    const struct SoulTransfer *st;
    const struct UserState *ustate = local_team_user(&plyr_idx, &st);
    const TbBool awaiting = (ustate != NULL) && soul_transfer_awaits_local_keeper();
    const TbBool unanswered = awaiting && (st->phase_deadline != answered_deadline);
    const TbBool show = unanswered && (ustate->view_type != PVT_CreatureContrl) && (ustate->view_type != PVT_CreaturePasngr);
    if (show && !menu_is_active(GMnu_SOUL_TRANSFER)) {
        turn_on_menu(GMnu_SOUL_TRANSFER);
    } else if (!show && menu_is_active(GMnu_SOUL_TRANSFER)) {
        turn_off_menu(GMnu_SOUL_TRANSFER);
    }
    if (pending_answer >= 0) {
        struct PlayerInfo *player = get_my_player();
        if (!awaiting) {
            pending_answer = -1;
        } else if (get_players_packet_action(player) == PckA_None) {
            set_players_packet_action(player, PckA_SoulTransferAnswer, pending_answer, 0, 0, 0);
            pending_answer = -1;
        }
    }
    if (!unanswered) {
        return false;
    }
    if (is_key_pressed(KC_Y, KMod_NONE)) {
        clear_key_pressed(KC_Y);
        queue_answer(true);
        return true;
    }
    if (is_key_pressed(KC_N, KMod_NONE)) {
        clear_key_pressed(KC_N);
        queue_answer(false);
        return true;
    }
    return false;
}

/** The icon for the thought cloud of a creature in the local team's soul transfer, or NULL.
 * Only the team's Archon and Keeper see it.
 */
const struct TbSprite *soul_transfer_marker_sprite(const struct Thing *thing)
{
    PlayerNumber plyr_idx;
    const struct SoulTransfer *st;
    const struct UserState *ustate = local_team_user(&plyr_idx, &st);
    if ((ustate == NULL) || (thing->owner != plyr_idx)) {
        return NULL;
    }
    const TbBool pending = (st->phase == SoulTr_AwaitKeeper);
    const TbBool picking = (st->phase == SoulTr_Ready) && (ustate->role == UsrRole_Archon);
    if (!pending && !picking) {
        return NULL;
    }
    if ((thing->index == st->target_idx) && (thing->creation_turn == st->target_creation)) {
        return get_panel_sprite(get_power_model_stats(PwrK_POSSESS)->medsym_sprite_idx);
    }
    if (pending && (thing->index == st->sacrifice_idx) && (thing->creation_turn == st->sacrifice_creation)) {
        return get_panel_sprite(get_room_kind_stats(RoK_TEMPLE)->medsym_sprite_idx);
    }
    return NULL;
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif
