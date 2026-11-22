/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file soul_transfer.c
 *     Gloomlight mode: soul transfer, how an Archon possesses a creature of its team.
 * @par Purpose:
 *     Runs the per-team cycle
 *       Cooldown -> Ready -> (Archon picks target + sacrifice) -> AwaitKeeper -> Possessing -> Cooldown.
 *     - The sacrifice must be worth at least as much as the target (creature score).
 *     - The Keeper approves or refuses within soul_transfer_decision_time game turns.
 *       Approve: the Archon enters the target and the sacrifice dies.
 *       Refuse: the request is dropped and the Archon may pick again.
 *       No answer: both creatures die and the cooldown starts.
 *     - If either creature becomes unavailable while waiting (dies, leaves, is picked up...),
 *       the request is cancelled and nobody dies.
 *     - The cooldown (soul_transfer_cooldown game turns) starts when a possession ends.
 * @par Comment:
 *     Everything here runs on the synced game path: the Archon's picks arrive as its possess
 *     clicks, the Keeper's answer as PckA_SoulTransferAnswer.
 *     Team messages are synced game messages, so their text must not depend on the client
 *     (no translated names). Personal hints and effects are local, see soul_transfer_gui.c.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "soul_transfer.h"

#include "config_creature.h"
#include "config_magic.h"
#include "config_rules.h"
#include "creature_control.h"
#include "creature_states.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "gui_msgs.h"
#include "magic_powers.h"
#include "net_game.h"
#include "packets.h"
#include "player_data.h"
#include "player_instances.h"
#include "power_hand.h"
#include "soul_transfer_gui.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "user_roles.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/** The Archon hears the alarm bell this many seconds before its cooldown ends. */
#define SOUL_TRANSFER_ALARM_SECONDS 5

struct SoulTransfer *get_team_soul_transfer(PlayerNumber plyr_idx)
{
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon)) {
        return NULL;
    }
    return &dungeon->soul_transfer;
}

/** A creature that may take part in a soul transfer for this team right now, or INVALID_THING. */
static struct Thing *available_creature(ThingIndex idx, GameTurn creation, PlayerNumber plyr_idx)
{
    if (idx == 0) {
        return INVALID_THING;
    }
    struct Thing *thing = thing_get(idx);
    if (!thing_exists(thing) || !thing_is_creature(thing) || (thing->creation_turn != creation) || (thing->owner != plyr_idx)) {
        return INVALID_THING;
    }
    if (creature_is_dying(thing) || creature_is_being_unconscious(thing) || creature_is_leaving_and_cannot_be_stopped(thing)
     || creature_is_kept_in_custody(thing) || thing_is_picked_up(thing)) {
        return INVALID_THING;
    }
    if (get_user_controlling_thing(thing, UCT_Direct | UCT_Passenger | UCT_PossessionEntryZoom) >= 0) {
        return INVALID_THING;
    }
    return thing;
}

static void clear_pair(struct SoulTransfer *st)
{
    st->target_idx = 0;
    st->target_creation = 0;
    st->sacrifice_idx = 0;
    st->sacrifice_creation = 0;
}

static void enter_ready(struct SoulTransfer *st)
{
    st->phase = SoulTr_Ready;
    st->phase_deadline = 0;
    clear_pair(st);
}

static void enter_cooldown(PlayerNumber plyr_idx, struct SoulTransfer *st)
{
    st->phase = SoulTr_Cooldown;
    st->phase_deadline = get_gameturn() + game.conf.rules[plyr_idx].magic.soul_transfer_cooldown;
    clear_pair(st);
}

/** A synced message for the team only, with the Possess icon.
 * TODO: translated GUI strings; they must still be identical on every client. */
static void team_message(PlayerNumber plyr_idx, const char *text)
{
    targeted_message_add(MsgType_KeeperSpell, PwrK_POSSESS, plyr_idx, GUI_MESSAGES_DELAY, "%s", text);
}

/******************************************************************************/

/** Runs one game turn of the soul transfer cycle of a team. Teams without an Archon are left alone. */
void process_soul_transfer(PlayerNumber plyr_idx)
{
    struct SoulTransfer *st = get_team_soul_transfer(plyr_idx);
    if (st == NULL) {
        return;
    }
    NetUserId archon = seat_user_with_role(plyr_idx, UsrRole_Archon);
    if (archon < 0) {
        return;
    }
    switch (st->phase)
    {
    case SoulTr_Cooldown:
        if (get_gameturn() >= st->phase_deadline) {
            enter_ready(st);
        } else if ((GameTurnDelta)(st->phase_deadline - get_gameturn()) == SOUL_TRANSFER_ALARM_SECONDS * turns_per_second) {
            // local sound only; turns_per_second may differ between clients, that's fine for a sound
            soul_transfer_cooldown_alarm(archon);
        }
        break;
    case SoulTr_Ready:
        if ((st->target_idx != 0) && thing_is_invalid(available_creature(st->target_idx, st->target_creation, plyr_idx))) {
            clear_pair(st);
            soul_transfer_hint(archon, "Vessel lost");
        }
        break;
    case SoulTr_AwaitKeeper:
    {
        NetUserId keeper = seat_user_with_role(plyr_idx, UsrRole_Keeper);
        struct Thing *target = available_creature(st->target_idx, st->target_creation, plyr_idx);
        struct Thing *sacrifice = available_creature(st->sacrifice_idx, st->sacrifice_creation, plyr_idx);
        if ((keeper < 0) || thing_is_invalid(target) || thing_is_invalid(sacrifice)) {
            // kill switch: nobody dies, the Archon may pick again
            enter_ready(st);
            team_message(plyr_idx, "Soul transfer cancelled: a creature is no longer available.");
            break;
        }
        if (get_gameturn() >= st->phase_deadline) {
            SYNCLOG("Soul transfer of player %d timed out; both creatures die", (int)plyr_idx);
            kill_creature(target, INVALID_THING, -1, CrDed_NoUnconscious | CrDed_NoRebirth);
            kill_creature(sacrifice, INVALID_THING, -1, CrDed_NoUnconscious | CrDed_NoRebirth);
            enter_cooldown(plyr_idx, st);
            team_message(plyr_idx, "The Keeper did not answer. Both creatures perished.");
        }
        break;
    }
    case SoulTr_Possessing:
    {
        const struct UserState *ustate = get_user_state(archon);
        if ((ustate->controlled_thing_idx == 0) && (ustate->instance_num != PI_DirctCtrl)) {
            enter_cooldown(plyr_idx, st);
        }
        break;
    }
    default:
        enter_ready(st);
        break;
    }
}

/** An Archon's possess click is a soul transfer pick instead.
 * @return True if the packet was taken as a pick, so the caller drops its action.
 */
TbBool soul_transfer_take_archon_pick(NetUserId user, const struct Packet *pckt)
{
    if (!user_has_archon_role(user)) {
        return false;
    }
    if ((pckt->action != PckA_UsePwrOnThing) || (pckt->actn_par1 != PwrK_POSSESS)) {
        return false;
    }
    soul_transfer_archon_pick(user, thing_get(pckt->actn_par2));
    return true;
}

/** The Archon clicked a creature: first click picks the vessel, second the sacrifice (which sends the request). */
void soul_transfer_archon_pick(NetUserId user, struct Thing *thing)
{
    if (!user_has_archon_role(user)) {
        return;
    }
    const PlayerNumber plyr_idx = get_user_state(user)->player_id;
    struct SoulTransfer *st = get_team_soul_transfer(plyr_idx);
    if ((st == NULL) || (st->phase != SoulTr_Ready)) {
        // the HUD already shows the cooldown or the pending request
        return;
    }
    if (seat_user_with_role(plyr_idx, UsrRole_Keeper) < 0) {
        soul_transfer_hint(user, "No Keeper to approve");
        return;
    }
    if (thing_is_invalid(thing) || thing_is_invalid(available_creature(thing->index, thing->creation_turn, plyr_idx))) {
        soul_transfer_hint(user, "Pick a free creature of yours");
        return;
    }
    if (st->target_idx == 0) {
        if (flag_is_set(get_creature_model_flags(thing), CMF_CannotPossess)) {
            soul_transfer_hint(user, "Cannot be possessed");
            return;
        }
        st->target_idx = thing->index;
        st->target_creation = thing->creation_turn;
        return;
    }
    if (thing->index == st->target_idx) {
        clear_pair(st);
        return;
    }
    struct Thing *target = available_creature(st->target_idx, st->target_creation, plyr_idx);
    if (thing_is_invalid(target)) {
        clear_pair(st);
        soul_transfer_hint(user, "Vessel lost");
        return;
    }
    if (get_creature_thing_score(thing) < get_creature_thing_score(target)) {
        soul_transfer_hint(user, "Too weak: %ld < %ld",
            get_creature_thing_score(thing), get_creature_thing_score(target));
        return;
    }
    st->sacrifice_idx = thing->index;
    st->sacrifice_creation = thing->creation_turn;
    st->phase = SoulTr_AwaitKeeper;
    st->phase_deadline = get_gameturn() + game.conf.rules[plyr_idx].magic.soul_transfer_decision_time;
    SYNCLOG("Soul transfer of player %d requested: thing %d for sacrifice %d", (int)plyr_idx, (int)st->target_idx, (int)st->sacrifice_idx);
    team_message(plyr_idx, "The Archon requests a soul transfer.");
}

/** The Keeper answered the pending request. */
void soul_transfer_keeper_answer(NetUserId user, TbBool approve)
{
    const struct UserState *ustate = get_user_state(user);
    if (user_state_invalid(ustate) || (ustate->role != UsrRole_Keeper)) {
        return;
    }
    const PlayerNumber plyr_idx = ustate->player_id;
    struct SoulTransfer *st = get_team_soul_transfer(plyr_idx);
    if ((st == NULL) || (st->phase != SoulTr_AwaitKeeper)) {
        return;
    }
    NetUserId archon = seat_user_with_role(plyr_idx, UsrRole_Archon);
    struct Thing *target = available_creature(st->target_idx, st->target_creation, plyr_idx);
    struct Thing *sacrifice = available_creature(st->sacrifice_idx, st->sacrifice_creation, plyr_idx);
    if ((archon < 0) || thing_is_invalid(target) || thing_is_invalid(sacrifice)) {
        enter_ready(st);
        team_message(plyr_idx, "Soul transfer cancelled: a creature is no longer available.");
        return;
    }
    if (!approve) {
        enter_ready(st);
        team_message(plyr_idx, "The Keeper refused. The Archon's soul is smacked back.");
        soul_transfer_smack_back(archon);
        return;
    }
    // Possess first, so nothing dies if possession fails
    if (magic_use_available_power_on_thing(plyr_idx, archon, PwrK_POSSESS, 0, target->mappos.x.stl.num, target->mappos.y.stl.num,
        target, PwMod_CastForFree) != Lb_SUCCESS) {
        enter_ready(st);
        team_message(plyr_idx, "The soul transfer failed.");
        return;
    }
    SYNCLOG("Soul transfer of player %d approved: user %d enters thing %d, thing %d is sacrificed",
        (int)plyr_idx, (int)archon, (int)target->index, (int)sacrifice->index);
    kill_creature(sacrifice, INVALID_THING, -1, CrDed_NoUnconscious | CrDed_NoRebirth);
    st->phase = SoulTr_Possessing;
    st->phase_deadline = 0;
    clear_pair(st);
    team_message(plyr_idx, "The sacrifice is made. The Archon's soul enters its vessel.");
}

/** For local input: whether the local user is a Keeper with a request to answer. */
TbBool soul_transfer_awaits_local_keeper(void)
{
    const NetUserId user = get_local_user();
    const struct UserState *ustate = get_user_state(user);
    if (user_state_invalid(ustate) || (ustate->role != UsrRole_Keeper) || (ustate->player_id == PLAYER_NONE)) {
        return false;
    }
    const struct SoulTransfer *st = get_team_soul_transfer(ustate->player_id);
    return (st != NULL) && (st->phase == SoulTr_AwaitKeeper);
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif
