/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file soul_transfer_gui.h
 *     Header file for soul_transfer_gui.c.
 * @par Purpose:
 *     Gloomlight mode: what the Archon and Keeper see of a soul transfer.
 * @par Comment:
 *     Local only. Nothing here changes the synced game state.
 */
/******************************************************************************/
#ifndef DK_SOUL_TRANSFER_GUI_H
#define DK_SOUL_TRANSFER_GUI_H

#include "bflib_basics.h"
#include "globals.h"
#include "net_main.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
struct Thing;
struct TbSprite;
struct GuiMenu;

extern struct GuiMenu soul_transfer_menu;

TbBool soul_transfer_keeper_inputs(void);
void soul_transfer_hint(NetUserId user, const char *fmt_str, ...);
void soul_transfer_smack_back(NetUserId archon);
void soul_transfer_cooldown_alarm(NetUserId archon);
void draw_soul_transfer_hud(void);
const struct TbSprite *soul_transfer_marker_sprite(const struct Thing *thing);
/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
