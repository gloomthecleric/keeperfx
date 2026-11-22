/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file user_roles.h
 *     Header file for user_roles.c.
 * @par Purpose:
 *     Gloomlight mode: roles for users sharing a keeper seat (Keeper and Archon).
 * @par Comment:
 *     A Keeper controls the dungeon as usual. An Archon may only move the
 *     camera, chat and possess creatures of the shared dungeon.
 */
/******************************************************************************/
#ifndef DK_USER_ROLES_H
#define DK_USER_ROLES_H

#include "bflib_basics.h"
#include "globals.h"
#include "net_main.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
struct Packet;

/** Role of a user within its seat. Keeper must stay 0, so states saved before roles existed load as keepers. */
enum UserRole {
    UsrRole_Keeper = 0,
    UsrRole_Archon,
    UsrRole_Count,
};

/******************************************************************************/
const char *user_role_name(int8_t role);
const char *user_role_column_title(void);
TbBool user_has_archon_role(NetUserId user);
void resolve_seat_roles(const int8_t player_slots[MAX_NET_USERS], const int8_t lobby_roles[MAX_NET_USERS], int8_t roles[MAX_NET_USERS]);
void apply_user_roles(const int8_t roles[MAX_NET_USERS]);
void restore_user_role_after_reseat(NetUserId user, int8_t previous_role);
TbBool user_role_allows_packet(NetUserId user, const struct Packet *pckt);
void enforce_user_role_work_state(NetUserId user);
/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
