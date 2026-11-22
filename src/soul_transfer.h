/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file soul_transfer.h
 *     Header file for soul_transfer.c.
 * @par Purpose:
 *     Gloomlight mode: soul transfer, how an Archon possesses a creature of its team.
 * @par Comment:
 *     The Archon picks a creature to inhabit and a sacrifice worth at least as much.
 *     The Keeper has a few seconds to approve or refuse; if it doesn't answer,
 *     both creatures die. After a possession ends there is a cooldown.
 */
/******************************************************************************/
#ifndef DK_SOUL_TRANSFER_H
#define DK_SOUL_TRANSFER_H

#include "bflib_basics.h"
#include "globals.h"
#include "net_main.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
struct Packet;
struct Thing;

enum SoulTransferPhase {
    SoulTr_Cooldown = 0, /**< Waiting for phase_deadline. A zeroed dungeon starts here and becomes Ready at once. */
    SoulTr_Ready,        /**< The Archon may pick a target, then a sacrifice. */
    SoulTr_AwaitKeeper,  /**< A request waits for the Keeper until phase_deadline. */
    SoulTr_Possessing,   /**< The Archon is inside the target. */
};

#pragma pack(1)
/** Soul transfer state of one dungeon (one team). Synced and saved, part of struct Dungeon. */
struct SoulTransfer {
    unsigned char phase; /**< enum SoulTransferPhase */
    GameTurn phase_deadline;
    ThingIndex target_idx;
    GameTurn target_creation;
    ThingIndex sacrifice_idx;
    GameTurn sacrifice_creation;
};
#pragma pack()

/******************************************************************************/
void process_soul_transfer(PlayerNumber plyr_idx);
TbBool soul_transfer_take_archon_pick(NetUserId user, const struct Packet *pckt);
void soul_transfer_archon_pick(NetUserId user, struct Thing *thing);
void soul_transfer_keeper_answer(NetUserId user, TbBool approve);
TbBool soul_transfer_awaits_local_keeper(void);
/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
