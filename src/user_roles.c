/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file user_roles.c
 *     Gloomlight mode: roles for users sharing a keeper seat (Keeper and Archon).
 * @par Purpose:
 *     Resolves the lobby's role choices into per-user roles, and restricts
 *     what an Archon user is allowed to do.
 * @par Comment:
 *     All restrictions run on the synced packet path, so every client and
 *     every replay drops exactly the same input.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "user_roles.h"

#include "config_magic.h"
#include "config_players.h"
#include "net_game.h"
#include "packets.h"
#include "player_data.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/

/** Lobby label of a role.
 * TODO: move to the translated GUI strings (lang/gtext_*.po) before proposing upstream. */
const char *user_role_name(int8_t role)
{
    switch (role)
    {
    case UsrRole_Archon:
        return "Archon";
    case UsrRole_Keeper:
    default:
        return "Keeper";
    }
}

/** Lobby column header for roles. TODO: translated GUI string, as for user_role_name(). */
const char *user_role_column_title(void)
{
    return "Role";
}

TbBool user_has_archon_role(NetUserId user)
{
    const struct UserState *ustate = get_user_state(user);
    if (user_state_invalid(ustate) || (ustate->player_id == PLAYER_NONE)) {
        return false;
    }
    return (ustate->role == UsrRole_Archon);
}

/** The lowest present user of a seat with the given role, or -1. */
NetUserId seat_user_with_role(PlayerNumber plyr_idx, int8_t role)
{
    for (NetUserId user = 0; user < MAX_NET_USERS; user++) {
        const struct UserState *ustate = get_user_state(user);
        if (!user_present(user) || user_state_invalid(ustate)) {
            continue;
        }
        if ((ustate->player_id == plyr_idx) && (ustate->role == role)) {
            return user;
        }
    }
    return -1;
}

/** Turns the lobby's role choices into the roles used in the game.
 * Unset or invalid choices become Keeper. Each seat then gets the 2v2 shape:
 * - at least one Keeper: if every user of a seat chose Archon, its lowest user becomes the Keeper;
 * - at most one Archon: the lowest user who chose Archon keeps it, any others become Keepers.
 * @param player_slots Seat of each user, PLAYER_NONE for none.
 * @param lobby_roles Role chosen by each user in the lobby, -1 for none.
 * @param roles Output, resolved role of each user.
 */
void resolve_seat_roles(const int8_t player_slots[MAX_NET_USERS], const int8_t lobby_roles[MAX_NET_USERS], int8_t roles[MAX_NET_USERS])
{
    for (NetUserId user = 0; user < MAX_NET_USERS; user++) {
        int8_t role = lobby_roles[user];
        if ((player_slots[user] < 0) || (role < 0) || (role >= UsrRole_Count)) {
            role = UsrRole_Keeper;
        }
        roles[user] = role;
    }
    for (NetUserId user = 0; user < MAX_NET_USERS; user++) {
        if (player_slots[user] < 0) {
            continue;
        }
        // handle each seat once, from its lowest user
        TbBool seen_before = false;
        for (NetUserId other = 0; other < user; other++) {
            if (player_slots[other] == player_slots[user]) {
                seen_before = true;
                break;
            }
        }
        if (seen_before) {
            continue;
        }
        TbBool has_keeper = false;
        for (NetUserId other = user; other < MAX_NET_USERS; other++) {
            if ((player_slots[other] == player_slots[user]) && (roles[other] == UsrRole_Keeper)) {
                has_keeper = true;
                break;
            }
        }
        if (!has_keeper) {
            roles[user] = UsrRole_Keeper;
        }
        TbBool has_archon = false;
        for (NetUserId other = user; other < MAX_NET_USERS; other++) {
            if ((player_slots[other] != player_slots[user]) || (roles[other] != UsrRole_Archon)) {
                continue;
            }
            if (has_archon) {
                roles[other] = UsrRole_Keeper;
            }
            has_archon = true;
        }
    }
}

/** Stores resolved roles on the user states. Call after init_user_state(), which clears them. */
void apply_user_roles(const int8_t roles[MAX_NET_USERS])
{
    for (NetUserId user = 0; user < MAX_NET_USERS; user++) {
        struct UserState *ustate = get_user_state(user);
        if (user_state_invalid(ustate)) {
            continue;
        }
        int8_t role = roles[user];
        if ((role < 0) || (role >= UsrRole_Count)) {
            role = UsrRole_Keeper;
        }
        ustate->role = role;
    }
}

/** Gives a user its role back after init_user_state() when it re-takes a seat.
 * It may only return as the Archon if another present user of the seat is a Keeper
 * and none is already the Archon; otherwise it becomes a Keeper.
 */
void restore_user_role_after_reseat(NetUserId user, int8_t previous_role)
{
    struct UserState *ustate = get_user_state(user);
    if (user_state_invalid(ustate)) {
        return;
    }
    if ((previous_role < 0) || (previous_role >= UsrRole_Count)) {
        previous_role = UsrRole_Keeper;
    }
    ustate->role = previous_role;
    if (ustate->role == UsrRole_Keeper) {
        return;
    }
    TbBool other_keeper = false;
    TbBool other_archon = false;
    for (NetUserId other = 0; other < MAX_NET_USERS; other++) {
        const struct UserState *other_state = get_user_state(other);
        if ((other == user) || !user_present(other) || user_state_invalid(other_state)
         || (other_state->player_id != ustate->player_id)) {
            continue;
        }
        if (other_state->role == UsrRole_Keeper) {
            other_keeper = true;
        } else if (other_state->role == UsrRole_Archon) {
            other_archon = true;
        }
    }
    if (!other_keeper || other_archon) {
        ustate->role = UsrRole_Keeper;
    }
}

/** Whether the role of a user allows the action in its packet.
 * Keepers may do anything. Archons get an allow-list: camera, chat, pause/quit,
 * possessing, and whatever the possessed creature can do. Anything not listed is refused,
 * so new actions are blocked for Archons until someone decides otherwise.
 */
TbBool user_role_allows_packet(NetUserId user, const struct Packet *pckt)
{
    if (!user_has_archon_role(user)) {
        return true;
    }
    switch (pckt->action)
    {
    // Session and chat
    case PckA_None:
    case PckA_NoOperation:
    case PckA_QuitToMainMenu:
    case PckA_ForceApplicationClose:
    case PckA_FinishGame:
    case PckA_GoSpectator:
    case PckA_TogglePause:
    case PckA_UpdatePause:
    case PckA_SetTimestamp:
    case PckA_SetUserPref:
    case PckA_ToggleLights: // local-only effect
    case PckA_PlyrMsgBegin:
    case PckA_PlyrMsgEnd:
    case PckA_PlyrMsgClear:
    case PckA_PlyrMsgLast:
    case PckA_PlyrMsgCmdAutoCompletion:
    case PckA_PlyrFastMsg:
    // Camera and views
    case PckA_BookmarkLoad:
    case PckA_SetMapRotation:
    case PckA_SwitchView:
    case PckA_SaveViewType:
    case PckA_LoadViewType:
    case PckA_ZoomFromMap:
    case PckA_ZoomToEvent:
    case PckA_ZoomToRoom:
    case PckA_ZoomToTrap:
    case PckA_ZoomToDoor:
    case PckA_ZoomToPosition:
    case PckA_ZoomToBattle:
    case PckA_ZoomToSpell:
    case PckA_TurnOffQuery:
    // Inside a possessed creature
    case PckA_DirectCtrlExit:
    case PckA_PasngrCtrlExit:
    case PckA_CtrlCrtrSetInstnc:
    case PckA_DirectCtrlDragDrop:
    case PckA_SetFirstPersonDigMode:
    case PckA_SwitchTeleportDest:
    case PckA_SelectFPPickup:
    case PckA_SetNearestTeleport:
        return true;
    case PckA_SetViewType:
        return (pckt->actn_par1 == PVT_DungeonTop) || (pckt->actn_par1 == PVT_MapScreen);
    case PckA_SetPlyrState:
        return (pckt->actn_par1 == PSt_CtrlDirect);
    case PckA_UsePwrOnThing:
        return (pckt->actn_par1 == PwrK_POSSESS);
    default:
        return false;
    }
}

/** Keeps an Archon in the Possess work state while it looks at the dungeon,
 * so its map clicks can only ever possess a creature.
 */
void enforce_user_role_work_state(NetUserId user)
{
    if (!user_has_archon_role(user)) {
        return;
    }
    struct UserState *ustate = get_user_state(user);
    if (ustate->view_type != PVT_DungeonTop) {
        return;
    }
    if (ustate->work_state != PSt_CtrlDirect) {
        set_user_work_state(user, PSt_CtrlDirect, PwrK_POSSESS);
    }
    // Right-click "cancel" returns to continue_work_state; keep that pointing back here too.
    ustate->continue_work_state = PSt_CtrlDirect;
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif
