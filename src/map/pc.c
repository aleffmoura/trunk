// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/cbasetypes.h"
#include "../common/core.h" // get_git_hash()
#include "../common/malloc.h"
#include "../common/nullpo.h"
#include "../common/random.h"
#include "../common/showmsg.h"
#include "../common/socket.h" // session[]
#include "../common/strlib.h" // safestrncpy()
#include "../common/timer.h"
#include "../common/utils.h"
#include "../common/mmo.h" //NAME_LENGTH
#include "../common/conf.h"

#include "map.h"
#include "atcommand.h" // get_atcommand_level()
#include "battle.h" // battle_config
#include "battleground.h"
#include "channel.h"
#include "chat.h"
#include "chrif.h"
#include "clan.h"
#include "clif.h"
#include "date.h" // is_day_of_*()
#include "duel.h"
#include "intif.h"
#include "itemdb.h"
#include "log.h"
#include "mail.h"
#include "path.h"
#include "homunculus.h"
#include "instance.h"
#include "mercenary.h"
#include "elemental.h"
#include "npc.h" // fake_nd
#include "pet.h" // pet_unlocktarget()
#include "party.h" // party_search()
#include "guild.h" // guild_search(), guild_request_info()
#include "script.h" // script_config
#include "skill.h"
#include "status.h" // struct status_data
#include "storage.h"
#include "pc.h"
#include "pc_groups.h"
#include "quest.h"
#include "achievement.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

struct config_t stylist_db_conf;
struct config_t job_exp_db_conf;
static inline bool pc_attendance_rewarded_today(struct map_session_data *sd);

#define PVP_CALCRANK_INTERVAL 1000 //PVP calculation interval
#define MAX_LEVEL_BASE_EXP 99999999 //Max Base EXP for player on Max Base Level
#define MAX_LEVEL_JOB_EXP 999999999 //Max Job EXP for player on Max Job Level

static unsigned int statp[MAX_LEVEL + 1];
#if defined(RENEWAL_DROP) || defined(RENEWAL_EXP)
static unsigned int level_penalty[3][CLASS_MAX][MAX_LEVEL * 2 + 1];
#endif

//H-files are for declarations, not for implementations [Shinomori]
struct skill_tree_entry skill_tree[CLASS_COUNT][MAX_SKILL_TREE];

//Timer for night and day implementation
int day_timer_tid = INVALID_TIMER;
int night_timer_tid = INVALID_TIMER;

int pc_expiration_tid = INVALID_TIMER;

struct eri *pc_sc_display_ers = NULL;

struct fame_list smith_fame_list[MAX_FAME_LIST];
struct fame_list chemist_fame_list[MAX_FAME_LIST];
struct fame_list taekwon_fame_list[MAX_FAME_LIST];

struct s_job_info job_info[CLASS_COUNT];

struct s_attendance_reward {
	uint16 itemid;
	uint16 amount;
};

struct s_attendance_period {
	uint32 start;
	uint32 end;
	struct s_attendance_reward *rewards;
	uint8 reward_count;
};

struct s_attendance_period *attendance_periods;

struct s_stylist_data_req {
	int zeny;
	int16 itemid;
	int16 boxid;
};

struct s_stylist_data {
	int16 id;
	struct s_stylist_data_req req[2];
};

#define MAX_STYLIST_TYPE LOOK_MAX
struct s_stylist_data *stylist_datas[MAX_STYLIST_TYPE];
uint8 stylist_data_count;

#define MOTD_LINE_SIZE 128
static char motd_text[MOTD_LINE_SIZE][CHAT_SIZE_MAX]; //Message of the day buffer [Valaris]

struct s_unequipped {
	int index;
	int position;
};

struct s_unequipped *unequipped;
uint8 unequipped_count;

//Translation table from athena equip index to aegis bitmask
unsigned int equip_bitmask[EQI_MAX] = {
	EQP_ACC_L,
	EQP_ACC_R,
	EQP_SHOES,
	EQP_GARMENT,
	EQP_HEAD_LOW,
	EQP_HEAD_MID,
	EQP_HEAD_TOP,
	EQP_ARMOR,
	EQP_HAND_L,
	EQP_HAND_R,
	EQP_COSTUME_HEAD_TOP,
	EQP_COSTUME_HEAD_MID,
	EQP_COSTUME_HEAD_LOW,
	EQP_COSTUME_GARMENT,
	EQP_PET,
	EQP_AMMO,
	EQP_SHADOW_ARMOR,
	EQP_SHADOW_WEAPON,
	EQP_SHADOW_SHIELD,
	EQP_SHADOW_SHOES,
	EQP_SHADOW_ACC_R,
	EQP_SHADOW_ACC_L
};

// Links related info to the sd->hate_mob[] / sd->feel_map[] entries
const struct sg_data sg_info[MAX_PC_FEELHATE] = {
		{ SG_SUN_ANGER, SG_SUN_BLESS, SG_SUN_COMFORT, "PC_FEEL_SUN", "PC_HATE_MOB_SUN", is_day_of_sun },
		{ SG_MOON_ANGER, SG_MOON_BLESS, SG_MOON_COMFORT, "PC_FEEL_MOON", "PC_HATE_MOB_MOON", is_day_of_moon },
		{ SG_STAR_ANGER, SG_STAR_BLESS, SG_STAR_COMFORT, "PC_FEEL_STAR", "PC_HATE_MOB_STAR", is_day_of_star }
	};

/**
 * Item Cool Down Delay Saving
 * Struct item_cd is not a member of struct map_session_data
 * to keep cooldowns in memory between player log-ins.
 * All cooldowns are reset when server is restarted.
 */
DBMap *itemcd_db = NULL; // char_id -> struct item_cd
struct item_cd {
	unsigned int tick[MAX_ITEMDELAYS]; //tick
	unsigned short nameid[MAX_ITEMDELAYS]; //item id
};

/**
 * Converts a class to its array index for CLASS_COUNT defined arrays.
 * Note that it does not do a validity check for speed purposes, where parsing
 * player input make sure to use a pcdb_checkid first!
 * @param class_ Job ID see enum e_job
 * @return Class Index
 */
int pc_class2idx(int class_) {
	if (class_ >= JOB_NOVICE_HIGH)
		return class_- JOB_NOVICE_HIGH + JOB_MAX_BASIC;
	return class_;
}

/**
 * Get player's group ID
 * @param sd
 * @return Group ID
 */
inline int pc_get_group_id(struct map_session_data *sd) {
	return sd->group_id;
}

/** Get player's group Level
 * @param sd
 * @return Group Level
 */
inline int pc_get_group_level(struct map_session_data *sd) {
	return sd->group_level;
}

static TIMER_FUNC(pc_invincible_timer)
{
	struct map_session_data *sd;

	if( (sd = (struct map_session_data *)map_id2sd(id)) == NULL || sd->bl.type!=BL_PC )
		return 1;

	if( sd->invincible_timer != tid ) {
		ShowError("invincible_timer %d != %d\n",sd->invincible_timer,tid);
		return 0;
	}
	sd->invincible_timer = INVALID_TIMER;
	skill_unit_move(&sd->bl,tick,1);

	return 0;
}

void pc_setinvincibletimer(struct map_session_data *sd, int val)
{
	nullpo_retv(sd);

	if( sd->invincible_timer != INVALID_TIMER )
		delete_timer(sd->invincible_timer,pc_invincible_timer);
	sd->invincible_timer = add_timer(gettick() + val,pc_invincible_timer,sd->bl.id,0);
}

void pc_delinvincibletimer(struct map_session_data *sd)
{
	nullpo_retv(sd);

	if( sd->invincible_timer != INVALID_TIMER ) {
		delete_timer(sd->invincible_timer,pc_invincible_timer);
		sd->invincible_timer = INVALID_TIMER;
		skill_unit_move(&sd->bl,gettick(),1);
	}
}

/**
 * Spirit ball expiration timer.
 * @see TimerFunc
 */
static TIMER_FUNC(pc_spiritball_timer)
{
	struct map_session_data *sd;
	uint8 i;

	if( !(sd = (struct map_session_data *)map_id2sd(id)) || sd->bl.type != BL_PC )
		return 1;

	if( sd->spiritball <= 0 ) {
		ShowError("pc_spiritball_timer: %d spiritball's available. (aid=%d cid=%d tid=%d)\n", sd->spiritball, sd->status.account_id, sd->status.char_id, tid);
		sd->spiritball = 0;
		return 0;
	}

	ARR_FIND(0, sd->spiritball, i, sd->spiritball_timer[i] == tid);

	if( i == sd->spiritball ) {
		ShowError("pc_spiritball_timer: timer not found (aid=%d cid=%d tid=%d)\n", sd->status.account_id, sd->status.char_id, tid);
		return 0;
	}

	sd->spiritball--;

	if( i != sd->spiritball )
		memmove(sd->spiritball_timer + i, sd->spiritball_timer + i + 1, (sd->spiritball - i) * sizeof(int));

	sd->spiritball_timer[sd->spiritball] = INVALID_TIMER;
	clif_spiritball(&sd->bl);

	return 0;
}

/**
 * Get the possible number of spiritball that a player can call.
 * @param sd the affected player structure
 * @param min the minimum number of spiritball regardless the level of MO_CALLSPIRITS
 * @retval total number of spiritball
 */
int pc_getmaxspiritball(struct map_session_data *sd, int min) {
	int result;

	nullpo_ret(sd);

	result = pc_checkskill(sd, MO_CALLSPIRITS);

	if( min && result < min )
		result = min;
	else if( sd->sc.data[SC_RAISINGDRAGON] )
		result += sd->sc.data[SC_RAISINGDRAGON]->val1;

	if( result > MAX_SPIRITBALL )
		result = MAX_SPIRITBALL;

	return result;
}

/**
 * Adds a spiritball to player for 'interval' ms
 * @param sd
 * @param interval
 * @param max
 */
void pc_addspiritball(struct map_session_data *sd, int interval, int max)
{
	int tid;
	uint8 i;

	nullpo_retv(sd);

	if( max > MAX_SPIRITBALL )
		max = MAX_SPIRITBALL;

	if( sd->spiritball < 0 )
		sd->spiritball = 0;

	if( sd->spiritball && sd->spiritball >= max ) {
		if( sd->spiritball_timer[0] != INVALID_TIMER )
			delete_timer(sd->spiritball_timer[0], pc_spiritball_timer);
		sd->spiritball--;
		if( sd->spiritball )
			memmove(sd->spiritball_timer + 0, sd->spiritball_timer + 1, sd->spiritball * sizeof(int));
		sd->spiritball_timer[sd->spiritball] = INVALID_TIMER;
	}

	tid = add_timer(gettick() + interval, pc_spiritball_timer, sd->bl.id, 0);
	ARR_FIND(0, sd->spiritball, i, (sd->spiritball_timer[i] == INVALID_TIMER || DIFF_TICK(get_timer(tid)->tick, get_timer(sd->spiritball_timer[i])->tick) < 0));

	if( i != sd->spiritball )
		memmove(sd->spiritball_timer + i + 1, sd->spiritball_timer + i, (sd->spiritball - i) * sizeof(int));

	sd->spiritball_timer[i] = tid;
	sd->spiritball++;
	clif_spiritball(&sd->bl);
}

/**
 * Removes number of spiritball from player
 * @param sd
 * @param count
 * @param type 1 = doesn't give client effect
 */
void pc_delspiritball(struct map_session_data *sd, int count, int type)
{
	uint8 i;

	nullpo_retv(sd);

	if( sd->spiritball <= 0 ) {
		sd->spiritball = 0;
		return;
	}

	if( !count )
		return;

	if( count > sd->spiritball )
		count = sd->spiritball;

	sd->spiritball -= count;

	if( count > MAX_SPIRITBALL )
		count = MAX_SPIRITBALL;

	for( i = 0; i < count; i++ ) {
		if( sd->spiritball_timer[i] != INVALID_TIMER ) {
			delete_timer(sd->spiritball_timer[i], pc_spiritball_timer);
			sd->spiritball_timer[i] = INVALID_TIMER;
		}
	}

	for( i = count; i < MAX_SPIRITBALL; i++ ) {
		sd->spiritball_timer[i - count] = sd->spiritball_timer[i];
		sd->spiritball_timer[i] = INVALID_TIMER;
	}

	if( !type )
		clif_spiritball(&sd->bl);
}

/**
 * Shieldball expiration timer.
 * @see TimerFunc
 */
static TIMER_FUNC(pc_shieldball_timer)
{
	struct map_session_data *sd;
	uint8 i;

	if( !(sd = (struct map_session_data *)map_id2sd(id)) || sd->bl.type != BL_PC )
		return 1;

	if( sd->shieldball <= 0 ) {
		ShowError("pc_shieldball_timer: %d shieldball's available. (aid=%d cid=%d tid=%d)\n", sd->shieldball, sd->status.account_id, sd->status.char_id, tid);
		sd->shieldball = 0;
		return 0;
	}

	ARR_FIND(0, sd->shieldball, i, sd->shieldball_timer[i] == tid);

	if( i == sd->shieldball ) {
		ShowError("pc_shieldball_timer: timer not found (aid=%d cid=%d tid=%d)\n", sd->status.account_id, sd->status.char_id, tid);
		return 0;
	}

	sd->shieldball--;

	if( sd->shieldball <= 0 )
		status_change_end(&sd->bl, SC_MILLENNIUMSHIELD, INVALID_TIMER);

	if( i != sd->shieldball )
		memmove(sd->shieldball_timer + i, sd->shieldball_timer + i + 1, (sd->shieldball - i) * sizeof(int));

	sd->shieldball_timer[sd->shieldball] = INVALID_TIMER;
	clif_millenniumshield(&sd->bl, sd->shieldball);

	return 0;
}

/**
 * Adds a shieldball to player for 'interval' ms
 * @param sd
 * @param interval
 * @param max
 * @param shield_health
 */
 void pc_addshieldball(struct map_session_data *sd, int interval, int max, int shield_health)
{
	int tid;
	uint8 i;

	nullpo_retv(sd);

	if( max > MAX_SHIELDBALL )
		max = MAX_SHIELDBALL;

	if( sd->shieldball < 0 )
		sd->shieldball = 0;

	if( sd->shieldball_set_health != shield_health && shield_health > 0 )
		sd->shieldball_set_health = shield_health;

	if( sd->shieldball && sd->shieldball >= max ) {
		if( sd->shieldball_timer[0] != INVALID_TIMER )
			delete_timer(sd->shieldball_timer[0], pc_shieldball_timer);
		sd->shieldball--;
		if( sd->shieldball )
			memmove(sd->shieldball_timer + 0, sd->shieldball_timer + 1, sd->shieldball * sizeof(int));
		sd->shieldball_timer[sd->shieldball] = INVALID_TIMER;
	}

	tid = add_timer(gettick() + interval, pc_shieldball_timer, sd->bl.id, 0);
	ARR_FIND(0, sd->shieldball, i, (sd->shieldball_timer[i] == INVALID_TIMER || DIFF_TICK(get_timer(tid)->tick, get_timer(sd->shieldball_timer[i])->tick) < 0));

	if( i != sd->shieldball )
		memmove(sd->shieldball_timer + i + 1, sd->shieldball_timer + i, (sd->shieldball - i) * sizeof(int));

	sd->shieldball_timer[i] = tid;
	sd->shieldball++;
	sd->shieldball_health = sd->shieldball_set_health;
	sc_start(&sd->bl, &sd->bl, SC_MILLENNIUMSHIELD, 100, 0, -1);
	clif_millenniumshield(&sd->bl, sd->shieldball);
}

/**
 * Removes number of shieldball from player
 * @param sd
 * @param count
 * @param type 1 = doesn't give client effect
 */
 void pc_delshieldball(struct map_session_data *sd, int count, int type)
{
	uint8 i;

	nullpo_retv(sd);

	if( sd->shieldball <= 0 ) {
		sd->shieldball = 0;
		return;
	}

	if( !count )
		return;

	if( count > sd->shieldball )
		count = sd->shieldball;

	sd->shieldball -= count;

	if( sd->shieldball <= 0 )
		status_change_end(&sd->bl, SC_MILLENNIUMSHIELD, INVALID_TIMER);

	if( sd->shieldball > 0 )
		sd->shieldball_health = sd->shieldball_set_health;

	if( count > MAX_SHIELDBALL )
		count = MAX_SHIELDBALL;

	for( i = 0; i < count; i++ ) {
		if( sd->shieldball_timer[i] != INVALID_TIMER ) {
			delete_timer(sd->shieldball_timer[i], pc_shieldball_timer);
			sd->shieldball_timer[i] = INVALID_TIMER;
		}
	}

	for( i = count; i < MAX_SHIELDBALL; i++ ) {
		sd->shieldball_timer[i - count] = sd->shieldball_timer[i];
		sd->shieldball_timer[i] = INVALID_TIMER;
	}

	if( !type )
		clif_millenniumshield(&sd->bl, sd->shieldball);
}

/**
 * Rageball expiration timer.
 * @see TimerFunc
 */
static TIMER_FUNC(pc_rageball_timer)
{
	struct map_session_data *sd;
	uint8 i;

	if( !(sd = (struct map_session_data *)map_id2sd(id)) || sd->bl.type != BL_PC )
		return 1;

	if( sd->rageball <= 0 ) {
		ShowError("pc_rageball_timer: %d rageball's available. (aid=%d cid=%d tid=%d)\n", sd->rageball, sd->status.account_id, sd->status.char_id, tid);
		sd->rageball = 0;
		return 0;
	}

	ARR_FIND(0, sd->rageball, i, sd->rageball_timer[i] == tid);

	if( i == sd->rageball ) {
		ShowError("pc_rageball_timer: timer not found (aid=%d cid=%d tid=%d)\n", sd->status.account_id, sd->status.char_id, tid);
		return 0;
	}

	sd->rageball--;

	if( i != sd->rageball )
		memmove(sd->rageball_timer + i, sd->rageball_timer + i + 1, (sd->rageball - i) * sizeof(int));

	sd->rageball_timer[sd->rageball] = INVALID_TIMER;
	clif_millenniumshield(&sd->bl, sd->rageball);

	return 0;
}

/**
 * Adds a rageball to player for 'interval' ms
 * @param sd
 * @param interval
 * @param max
 */
void pc_addrageball(struct map_session_data *sd, int interval, int max)
{
	int tid;
	uint8 i;

	nullpo_retv(sd);

	if( max > MAX_RAGEBALL )
		max = MAX_RAGEBALL;

	if( sd->rageball < 0 )
		sd->rageball = 0;

	if( sd->rageball && sd->rageball >= max ) {
		if( sd->rageball_timer[0] != INVALID_TIMER )
			delete_timer(sd->rageball_timer[0], pc_rageball_timer);
		sd->rageball--;
		if( sd->rageball )
			memmove(sd->rageball_timer + 0, sd->rageball_timer + 1, sd->rageball * sizeof(int));
		sd->rageball_timer[sd->rageball] = INVALID_TIMER;
	}

	tid = add_timer(gettick() + interval, pc_rageball_timer, sd->bl.id, 0);
	ARR_FIND(0, sd->rageball, i, (sd->rageball_timer[i] == INVALID_TIMER || DIFF_TICK(get_timer(tid)->tick, get_timer(sd->rageball_timer[i])->tick) < 0));

	if( i != sd->rageball )
		memmove(sd->rageball_timer + i + 1, sd->rageball_timer + i, (sd->rageball - i) * sizeof(int));

	sd->rageball_timer[i] = tid;
	sd->rageball++;
	clif_millenniumshield(&sd->bl, sd->rageball);
}

/**
 * Removes number of rageball from player
 * @param sd
 * @param count
 * @param type 1 = doesn't give client effect
 */
void pc_delrageball(struct map_session_data *sd, int count, int type)
{
	uint8 i;

	nullpo_retv(sd);

	if( sd->rageball <= 0 ) {
		sd->rageball = 0;
		return;
	}

	if( !count )
		return;

	if( count > sd->rageball )
		count = sd->rageball;

	sd->rageball -= count;

	if( count > MAX_RAGEBALL )
		count = MAX_RAGEBALL;

	for( i = 0; i < count; i++ ) {
		if( sd->rageball_timer[i] != INVALID_TIMER ) {
			delete_timer(sd->rageball_timer[i], pc_rageball_timer);
			sd->rageball_timer[i] = INVALID_TIMER;
		}
	}

	for( i = count; i < MAX_RAGEBALL; i++ ) {
		sd->rageball_timer[i - count] = sd->rageball_timer[i];
		sd->rageball_timer[i] = INVALID_TIMER;
	}

	if( !type )
		clif_millenniumshield(&sd->bl, sd->rageball);
}

/**
 * Charmball expiration timer.
 * @see TimerFunc
 */
static TIMER_FUNC(pc_charmball_timer)
{
	struct map_session_data *sd;
	int i;

	if( !(sd = (struct map_session_data *)map_id2sd(id)) || sd->bl.type != BL_PC )
		return 1;

	if( sd->charmball <= 0 ) {
		ShowError("pc_charmball_timer: %d charmball's available. (aid=%d cid=%d tid=%d)\n", sd->charmball, sd->status.account_id, sd->status.char_id, tid);
		sd->charmball = 0;
		sd->charmball_type = CHARM_TYPE_NONE;
		return 0;
	}

	ARR_FIND(0, sd->charmball, i, sd->charmball_timer[i] == tid);

	if( i == sd->charmball ) {
		ShowError("pc_charmball_timer: timer not found (aid=%d cid=%d tid=%d)\n", sd->status.account_id, sd->status.char_id, tid);
		return 0;
	}

	sd->charmball--;

	if( i != sd->charmball )
		memmove(sd->charmball_timer + i, sd->charmball_timer + i + 1, (sd->charmball - i) * sizeof(int));

	sd->charmball_timer[sd->charmball] = INVALID_TIMER;

	if( sd->charmball <= 0 )
		sd->charmball_type = CHARM_TYPE_NONE;

	clif_charmball(sd);

	return 0;
}

/**
 * Adds a charmball.
 * @param sd       Target character.
 * @param interval Duration.
 * @param max      Maximum amount of charms to add.
 * @param type     Charm type (@see charmball_types)
 */
void pc_addcharmball(struct map_session_data *sd, int interval, int max, int type)
{
	int tid, i;

	nullpo_retv(sd);

	if( sd->charmball_type != CHARM_TYPE_NONE && sd->charmball_type != type )
		pc_delcharmball(sd, sd->charmball, sd->charmball_type);

	if( max > MAX_CHARMBALL )
		max = MAX_CHARMBALL;

	if( sd->charmball < 0 )
		sd->charmball = 0;

	if( sd->charmball && sd->charmball >= max ) {
		if( sd->charmball_timer[0] != INVALID_TIMER )
			delete_timer(sd->charmball_timer[0], pc_charmball_timer);
		sd->charmball--;
		if( sd->charmball )
			memmove(sd->charmball_timer + 0, sd->charmball_timer + 1, sd->charmball * sizeof(int));
		sd->charmball_timer[sd->charmball] = INVALID_TIMER;
	}

	tid = add_timer(gettick() + interval, pc_charmball_timer, sd->bl.id, 0);
	ARR_FIND(0, sd->charmball, i, (sd->charmball_timer[i] == INVALID_TIMER || DIFF_TICK(get_timer(tid)->tick, get_timer(sd->charmball_timer[i])->tick) < 0));

	if( i != sd->charmball )
		memmove(sd->charmball_timer + i + 1, sd->charmball_timer + i, (sd->charmball - i) * sizeof(int));

	sd->charmball_timer[i] = tid;
	sd->charmball++;
	sd->charmball_type = type;

	clif_charmball(sd);
}

/**
 * Removes one or more charmballs.
 * @param sd    The target character.
 * @param count Amount of charms to remove.
 * @param type  Type of charm to remove.
 */
void pc_delcharmball(struct map_session_data *sd, int count, int type)
{
	int i;

	nullpo_retv(sd);

	if( sd->charmball_type != type )
		return;

	if( sd->charmball <= 0 ) {
		sd->charmball = 0;
		return;
	}

	if( count <= 0 )
		return;

	if( count > sd->charmball )
		count = sd->charmball;

	sd->charmball -= count;

	if( count > MAX_CHARMBALL )
		count = MAX_CHARMBALL;

	for( i = 0; i < count; i++ ) {
		if( sd->charmball_timer[i] != INVALID_TIMER ) {
			delete_timer(sd->charmball_timer[i], pc_charmball_timer);
			sd->charmball_timer[i] = INVALID_TIMER;
		}
	}

	for( i = count; i < MAX_CHARMBALL; i++ ) {
		sd->charmball_timer[i - count] = sd->charmball_timer[i];
		sd->charmball_timer[i] = INVALID_TIMER;
	}

	if( sd->charmball <= 0 )
		sd->charmball_type = CHARM_TYPE_NONE;

	clif_charmball(sd);
}

/**
 * Soulball expiration timer.
 * @see TimerFunc
 */
static TIMER_FUNC(pc_soulball_timer)
{
	struct map_session_data *sd;
	int i;

	if( !(sd = (struct map_session_data *)map_id2sd(id)) || sd->bl.type != BL_PC )
		return 1;

	if( sd->soulball <= 0 ) {
		ShowError("pc_soulball_timer: %d soulball's available. (aid=%d cid=%d tid=%d)\n", sd->soulball, sd->status.account_id, sd->status.char_id, tid);
		sd->soulball = 0;
		return 0;
	}

	ARR_FIND(0, sd->soulball, i, sd->soulball_timer[i] == tid);

	if( i == sd->soulball ) {
		ShowError("pc_soulball_timer: timer not found (aid=%d cid=%d tid=%d)\n", sd->status.account_id, sd->status.char_id, tid);
		return 0;
	}

	sd->soulball--;

	if( i != sd->soulball )
		memmove(sd->soulball_timer + i, sd->soulball_timer + i + 1, (sd->soulball - i) * sizeof(int));

	sd->soulball_timer[sd->soulball] = INVALID_TIMER;

	clif_soulball(sd);

	return 0;
}

/**
 * Adds a soulball.
 * @param sd       Target character.
 * @param interval Duration.
 * @param max      Maximum amount of souls to add.
 */
void pc_addsoulball(struct map_session_data *sd, int interval, int max)
{
	int tid, i;

	nullpo_retv(sd);

	if( max > MAX_SOULBALL )
		max = MAX_SOULBALL;

	if( sd->soulball < 0 )
		sd->soulball = 0;

	if( sd->soulball && sd->soulball >= max ) {
		if( sd->soulball_timer[0] != INVALID_TIMER )
			delete_timer(sd->soulball_timer[0], pc_soulball_timer);
		sd->soulball--;
		if( sd->soulball )
			memmove(sd->soulball_timer + 0, sd->soulball_timer + 1, sd->soulball * sizeof(int));
		sd->soulball_timer[sd->soulball] = INVALID_TIMER;
	}

	tid = add_timer(gettick() + interval, pc_soulball_timer, sd->bl.id, 0);
	ARR_FIND(0, sd->soulball, i, (sd->soulball_timer[i] == INVALID_TIMER || DIFF_TICK(get_timer(tid)->tick, get_timer(sd->soulball_timer[i])->tick) < 0));

	if( i != sd->soulball )
		memmove(sd->soulball_timer + i + 1, sd->soulball_timer + i, (sd->soulball - i) * sizeof(int));

	sd->soulball_timer[i] = tid;
	sd->soulball++;

	clif_soulball(sd);
}

/**
 * Removes one or more soulballs.
 * @param sd    The target character.
 * @param count Amount of souls to remove.
 * @param type 1 = doesn't give client effect
 */
void pc_delsoulball(struct map_session_data *sd, int count, int type)
{
	int i;

	nullpo_retv(sd);

	if( sd->soulball <= 0 ) {
		sd->soulball = 0;
		return;
	}

	if( count <= 0 )
		return;

	if( count > sd->soulball )
		count = sd->soulball;

	sd->soulball -= count;

	if( count > MAX_SOULBALL )
		count = MAX_SOULBALL;

	for( i = 0; i < count; i++ ) {
		if( sd->soulball_timer[i] != INVALID_TIMER ) {
			delete_timer(sd->soulball_timer[i], pc_soulball_timer);
			sd->soulball_timer[i] = INVALID_TIMER;
		}
	}

	for( i = count; i < MAX_SOULBALL; i++ ) {
		sd->soulball_timer[i - count] = sd->soulball_timer[i];
		sd->soulball_timer[i] = INVALID_TIMER;
	}

	if( !type )
		clif_soulball(sd);
}

/**
 * Increases a player's fame points and displays a notice to him
 * @param sd Player
 * @param count Fame point
 */
void pc_addfame(struct map_session_data *sd, int count)
{
	enum e_rank rankingtype;

	nullpo_retv(sd);

	sd->status.fame += count;
	if(sd->status.fame > MAX_FAME)
		sd->status.fame = MAX_FAME;

	switch(sd->class_&MAPID_UPPERMASK) {
		case MAPID_BLACKSMITH: rankingtype = RANK_BLACKSMITH; break;
		case MAPID_ALCHEMIST: rankingtype = RANK_ALCHEMIST; break;
		case MAPID_TAEKWON: rankingtype = RANK_TAEKWON; break;
		default:
			ShowWarning("pc_addfame: Trying to add fame to class '%s'(%d).\n", job_name(sd->class_), sd->class_);
			return;
	}

	clif_update_rankingpoint(sd, rankingtype, count);
	chrif_updatefamelist(sd);
}

/**
 * Check whether a player ID is in the fame rankers list of its job, returns his/her position if so, 0 else
 * @param sd
 * @param job Job use enum e_mapid
 * @return Rank
 */
unsigned char pc_famerank(int char_id, int job)
{
	uint8 i;

	switch(job) {
		case MAPID_BLACKSMITH: // Blacksmith
		    for(i = 0; i < MAX_FAME_LIST; i++) {
				if(smith_fame_list[i].id == char_id)
				    return i + 1;
			}
			break;
		case MAPID_ALCHEMIST: // Alchemist
			for(i = 0; i < MAX_FAME_LIST; i++) {
				if(chemist_fame_list[i].id == char_id)
					return i + 1;
			}
			break;
		case MAPID_TAEKWON: // Taekwon
			for(i = 0; i < MAX_FAME_LIST; i++) {
				if(taekwon_fame_list[i].id == char_id)
					return i + 1;
			}
			break;
	}

	return 0;
}

/**
 * Restart player's HP & SP value
 * @param sd
 * @param type Restart type: 1 - Normal Resurection
 */
void pc_setrestartvalue(struct map_session_data *sd, char type) {
	struct status_data *status, *b_status;

	nullpo_retv(sd);

	b_status = &sd->base_status;
	status = &sd->battle_status;

	if(type&1) { //Normal resurrection
		status->hp = 1; //Otherwise, status_heal may fail if dead
		status_heal(&sd->bl, b_status->hp, 0, 1);
		if(status->sp < b_status->sp)
			status_set_sp(&sd->bl, b_status->sp, 1);
	} else { //Just for saving on the char-server (with values as if respawned)
		sd->status.hp = b_status->hp;
		sd->status.sp = (status->sp < b_status->sp) ? b_status->sp : status->sp;
	}
}

/*==========================================
	Rental System
 *------------------------------------------*/

/**
 * Ends a rental and removes the item/effect
 * @param tid: Tick ID
 * @param tick: Timer
 * @param id: Timer ID
 * @param data: Data
 * @return 0 - failure, 1 - success
 */
static TIMER_FUNC(pc_inventory_rental_end)
{
	struct map_session_data *sd = map_id2sd(id);

	if( !sd )
		return 0;
	if( tid != sd->rental_timer ) {
		ShowError("pc_inventory_rental_end: invalid timer id.\n");
		return 0;
	}

	pc_inventory_rentals(sd);
	return 1;
}

/**
 * Removes the rental timer from the player
 * @param sd: Player data
 */
void pc_inventory_rental_clear(struct map_session_data *sd)
{
	if( sd->rental_timer != INVALID_TIMER ) {
		delete_timer(sd->rental_timer, pc_inventory_rental_end);
		sd->rental_timer = INVALID_TIMER;
	}
}

/**
 * Check for items in the player's inventory that are rental type
 * @param sd: Player data
 */
void pc_inventory_rentals(struct map_session_data *sd)
{
	int i, c = 0;
	unsigned int next_tick = UINT_MAX;

	for( i = 0; i < MAX_INVENTORY; i++ ) { // Check for Rentals on Inventory
		if( !sd->inventory.u.items_inventory[i].nameid )
			continue; // Nothing here
		if( !sd->inventory.u.items_inventory[i].expire_time )
			continue;
		if( sd->inventory.u.items_inventory[i].expire_time <= time(NULL) ) {
			if( sd->inventory_data[i]->unequip_script )
				run_script(sd->inventory_data[i]->unequip_script, 0, sd->bl.id, fake_nd->bl.id);
			clif_rental_expired(sd->fd, i, sd->inventory.u.items_inventory[i].nameid);
			pc_delitem(sd, i, sd->inventory.u.items_inventory[i].amount, 0, 0, LOG_TYPE_OTHER);
		} else {
			unsigned int expire_tick = (unsigned int)(sd->inventory.u.items_inventory[i].expire_time - time(NULL));

			clif_rental_time(sd->fd, sd->inventory.u.items_inventory[i].nameid, (int)expire_tick);
			next_tick = umin(expire_tick * 1000, next_tick);
			c++;
		}
	}

	if( c > 0 ) // umin(next_tick, 3600000) 1 hour each timer to keep announcing to the owner, and to avoid a but with rental time > 15 days
		sd->rental_timer = add_timer(gettick() + umin(next_tick, 3600000), pc_inventory_rental_end, sd->bl.id, 0);
	else
		sd->rental_timer = INVALID_TIMER;
}

/**
 * Add a rental item to the player and adjusts the rental timer appropriately
 * @param sd: Player data
 * @param seconds: Rental time
 */
void pc_inventory_rental_add(struct map_session_data *sd, unsigned int seconds)
{
	unsigned int tick = seconds * 1000;

	if( !sd )
		return;

	if( sd->rental_timer != INVALID_TIMER ) {
		const struct TimerData *td;

		td = get_timer(sd->rental_timer);
		if( DIFF_TICK(td->tick, gettick()) > tick ) { // Update Timer as this one ends first than the current one
			pc_inventory_rental_clear(sd);
			sd->rental_timer = add_timer(gettick() + tick, pc_inventory_rental_end, sd->bl.id, 0);
		}
	} else
		sd->rental_timer = add_timer(gettick() + umin(tick, 3600000), pc_inventory_rental_end, sd->bl.id, 0);
}

/**
 * Check if the player can sell the current item
 * @param sd: map_session_data of the player
 * @param item: struct of the checking item
 * @param shoptype: NPC's sub type see enum npc_subtype
 * @return bool 'true' is sellable, 'false' otherwise
 */
bool pc_can_sell_item(struct map_session_data *sd, struct item *item, enum npc_subtype shoptype)
{
	if(!sd || !item)
		return false;

	if(itemdb_ishatched_egg(item))
		return false;

	if(item->equip > 0 || item->amount < 0)
		return false;

	if(battle_config.hide_fav_sell && item->favorite)
		return false; //Cannot sell favs (optional config)

	if(!battle_config.rental_transaction && item->expire_time)
		return false; //Cannot Sell Rental Items

	if(item->equipSwitch)
		return false;

	switch(shoptype) {
		case NPCTYPE_SHOP:
			if(item->bound && (battle_config.allow_bound_sell&ISR_BOUND_SELLABLE) &&
				(item->bound != BOUND_GUILD ||
				(sd->guild && sd->status.char_id == sd->guild->member[0].char_id) ||
				(item->bound == BOUND_GUILD && !(battle_config.allow_bound_sell&ISR_BOUND_GUILDLEADER_ONLY))))
				return true;
			break;
		case NPCTYPE_ITEMSHOP:
			if(item->bound && (battle_config.allow_bound_sell&ISR_BOUND) &&
				(item->bound != BOUND_GUILD ||
				(sd->guild && sd->status.char_id == sd->guild->member[0].char_id) ||
				(item->bound == BOUND_GUILD && !(battle_config.allow_bound_sell&ISR_BOUND_GUILDLEADER_ONLY))))
				return true;
			else if(!item->bound) {
				struct item_data *itd = itemdb_search(item->nameid);

				if(itd && (itd->flag.trade_restriction&ITR_NOSELLTONPC) && (battle_config.allow_bound_sell&ISR_SELLABLE))
					return true;
			}
			break;
	}

	if(!itemdb_cansell(item, pc_get_group_level(sd)))
		return false;

	if(item->bound && !pc_can_give_bounded_items(sd))
		return false; //Don't allow sale of bound items
	return true;
}

/**
 * Determines if player can give / drop / trade / vend items
 */
bool pc_can_give_items(struct map_session_data *sd)
{
	return pc_has_permission(sd, PC_PERM_TRADE);
}

/**
 * Determines if player can give / drop / trade / vend bounded items
 */
bool pc_can_give_bounded_items(struct map_session_data *sd)
{
	return pc_has_permission(sd, PC_PERM_TRADE_BOUNDED);
}

/**
 * Prepares character for saving.
 * @param sd
 */
void pc_makesavestatus(struct map_session_data *sd)
{
	nullpo_retv(sd);

	if(!battle_config.save_clothcolor)
		sd->status.clothes_color = 0;

	if(!battle_config.save_body_style)
		sd->status.body = 0;

#ifdef NEW_CARTS //Only copy the Cart/Peco/Falcon options, the rest are handled via status change load/saving [Skotlex]
	sd->status.option = sd->sc.option&(OPTION_INVISIBLE|OPTION_FALCON|OPTION_RIDING|OPTION_DRAGON|OPTION_WUG|OPTION_WUGRIDER|OPTION_MADOGEAR);
#else
	sd->status.option = sd->sc.option&(OPTION_INVISIBLE|OPTION_CART|OPTION_FALCON|OPTION_RIDING|OPTION_DRAGON|OPTION_WUG|OPTION_WUGRIDER|OPTION_MADOGEAR);
#endif

	if(sd->sc.data[SC_JAILED]) { //When Jailed, do not move last point
		if(pc_isdead(sd)) {
			pc_setrestartvalue(sd,0);
		} else {
			sd->status.hp = sd->battle_status.hp;
			sd->status.sp = sd->battle_status.sp;
		}
		sd->status.last_point.map = sd->mapindex;
		sd->status.last_point.x = sd->bl.x;
		sd->status.last_point.y = sd->bl.y;
		return;
	}

	if(pc_isdead(sd)) {
		pc_setrestartvalue(sd,0);
		memcpy(&sd->status.last_point,&sd->status.save_point,sizeof(sd->status.last_point));
	} else {
		sd->status.hp = sd->battle_status.hp;
		sd->status.sp = sd->battle_status.sp;
		sd->status.last_point.map = sd->mapindex;
		sd->status.last_point.x = sd->bl.x;
		sd->status.last_point.y = sd->bl.y;
	}

	if(mapdata[sd->bl.m].flag.nosave) {
		struct map_data *m = &mapdata[sd->bl.m];

		if(m->save.map)
			memcpy(&sd->status.last_point,&m->save,sizeof(sd->status.last_point));
		else
			memcpy(&sd->status.last_point,&sd->status.save_point,sizeof(sd->status.last_point));
	}
}

/*==========================================
 * Off init ? Connection ?
 *------------------------------------------*/
void pc_setnewpc(struct map_session_data *sd, int account_id, int char_id, int login_id1, unsigned int client_tick, int sex, int fd)
{
	nullpo_retv(sd);

	sd->bl.id = account_id;
	sd->status.account_id = account_id;
	sd->status.char_id = char_id;
	sd->status.sex = sex;
	sd->login_id1 = login_id1;
	sd->login_id2 = 0; //At this point, we can not know the value
	sd->client_tick = client_tick;
	sd->state.active = 0; //To be set to 1 after player is fully authed and loaded
	sd->bl.type = BL_PC;
	if(battle_config.prevent_logout_trigger&PLT_LOGIN)
		sd->canlog_tick = gettick();
	//Required to prevent homunculus copying a base speed of 0
	sd->battle_status.speed = sd->base_status.speed = DEFAULT_WALK_SPEED;
	sd->state.warp_clean = 1;
}

/**
 * Get equip point for an equip
 * @param sd
 * @param id
 */
int pc_equippoint_sub(struct map_session_data *sd, struct item_data *id)
{
	int ep = 0;

	nullpo_ret(sd);

	if(!id || !itemdb_isequip2(id))
		return 0; //Not equippable by players

	ep = id->equip;
	if(id->look == W_DAGGER	|| id->look == W_1HSWORD || id->look == W_1HAXE) {
		if((pc_checkskill(sd,AS_LEFT) > 0 ||
			(sd->class_&MAPID_UPPERMASK) == MAPID_ASSASSIN ||
			(sd->class_&MAPID_UPPERMASK) == MAPID_KAGEROUOBORO)) { //Kagerou and Oboro can dual wield daggers [Rytech]
			if(ep == EQP_WEAPON)
				return EQP_ARMS;
			if(ep == EQP_SHADOW_WEAPON)
				return EQP_SHADOW_ARMS;
		}
	}
	return ep;
}

/**
 * Get equip point for an equip
 * @param sd
 * @param n Equip index in inventory
 */
int pc_equippoint(struct map_session_data *sd, int n)
{
	nullpo_ret(sd);

	return pc_equippoint_sub(sd,sd->inventory_data[n]);
}

/**
 * Fill inventory_data with struct *item_data through inventory (fill with struct *item)
 * @param sd : player session
 * @return 0 sucess, 1:invalid sd
 */
void pc_setinventorydata(struct map_session_data *sd)
{
	uint8 i;

	nullpo_retv(sd);

	for(i = 0; i < MAX_INVENTORY; i++) {
		unsigned short id = sd->inventory.u.items_inventory[i].nameid;

		sd->inventory_data[i] = (id ? itemdb_search(id) : NULL);
	}
}

/**
 * 'Calculates' weapon type
 * @param sd
 */
void pc_calcweapontype(struct map_session_data *sd)
{
	nullpo_retv(sd);

	//Single-hand
	if(sd->weapontype2 == W_FIST) {
		sd->status.weapon = sd->weapontype1;
		return;
	}
	if(sd->weapontype1 == W_FIST) {
		sd->status.weapon = sd->weapontype2;
		return;
	}
	//Dual-wield
	sd->status.weapon = 0;
	switch(sd->weapontype1) {
		case W_DAGGER:
			switch(sd->weapontype2) {
				case W_DAGGER:  sd->status.weapon = W_DOUBLE_DD; break;
				case W_1HSWORD: sd->status.weapon = W_DOUBLE_DS; break;
				case W_1HAXE:   sd->status.weapon = W_DOUBLE_DA; break;
			}
			break;
		case W_1HSWORD:
			switch(sd->weapontype2) {
				case W_DAGGER:  sd->status.weapon = W_DOUBLE_DS; break;
				case W_1HSWORD: sd->status.weapon = W_DOUBLE_SS; break;
				case W_1HAXE:   sd->status.weapon = W_DOUBLE_SA; break;
			}
			break;
		case W_1HAXE:
			switch(sd->weapontype2) {
				case W_DAGGER:  sd->status.weapon = W_DOUBLE_DA; break;
				case W_1HSWORD: sd->status.weapon = W_DOUBLE_SA; break;
				case W_1HAXE:   sd->status.weapon = W_DOUBLE_AA; break;
			}
			break;
	}
	//Unknown, default to right hand type
	if(!sd->status.weapon)
		sd->status.weapon = sd->weapontype1;
}

/**
 * Set equip index
 * @param sd
 */
void pc_setequipindex(struct map_session_data *sd)
{
	uint16 i;

	nullpo_retv(sd);

	for(i = 0; i < EQI_MAX; i++) {
		sd->equip_index[i] = -1;
		sd->equip_switch_index[i] = -1;
	}

	for(i = 0; i < MAX_INVENTORY; i++) {
		if(sd->inventory.u.items_inventory[i].nameid <= 0)
			continue;
		if(sd->inventory.u.items_inventory[i].equip) {
			uint8 j;

			for(j = 0; j < EQI_MAX; j++) {
				if(sd->inventory.u.items_inventory[i].equip&equip_bitmask[j])
					sd->equip_index[j] = i;
			}
			if(sd->inventory.u.items_inventory[i].equip&EQP_HAND_R) {
				if(sd->inventory_data[i])
					sd->weapontype1 = sd->inventory_data[i]->look;
				else
					sd->weapontype1 = 0;
			}
			if(sd->inventory.u.items_inventory[i].equip&EQP_HAND_L) {
				if(sd->inventory_data[i] && sd->inventory_data[i]->type == IT_WEAPON)
					sd->weapontype2 = sd->inventory_data[i]->look;
				else
					sd->weapontype2 = 0;
			}
		}
		if(sd->inventory.u.items_inventory[i].equipSwitch) {
			uint8 j;

			for(j = 0; j < EQI_MAX; j++) {
				if(sd->inventory.u.items_inventory[i].equipSwitch&equip_bitmask[j])
					sd->equip_switch_index[j] = i;
			}
		}
	}
	pc_calcweapontype(sd);
}

/*static int pc_isAllowedCardOn(struct map_session_data *sd,int s,int eqindex,int flag)
{
	int i;
	struct item *item = &sd->inventory.u.items_inventory[eqindex];
	struct item_data *data;

	//Crafted/made/hatched items.
	if(itemdb_isspecial(item->card[0]))
		return 1;

	//Scan for enchant armor gems
	if(item->card[MAX_SLOTS - 1] && s < MAX_SLOTS - 1)
		s = MAX_SLOTS - 1;

	ARR_FIND(0, s, i, item->card[i] && (data = itemdb_exists(item->card[i])) != NULL && data->flag.no_equip&flag);
	return(i < s) ? 0 : 1;
}*/

/**
 * Check if an item is equiped by player
 * (Check if the itemid is equiped then search if that match the index in inventory (should be))
 * @param sd : player session
 * @param nameid : itemid
 * @return true : yes, false : no
 */
bool pc_isequipped(struct map_session_data *sd, unsigned short nameid)
{
	uint8 i;

	for( i = 0; i < EQI_MAX; i++ ) {
		short index = sd->equip_index[i];
		uint8 j;

		if( index < 0 )
			continue;
		if( pc_is_same_equip_index((enum equip_index)i, sd->equip_index, index) )
			continue;
		if( !sd->inventory_data[index] )
			continue;
		if( sd->inventory_data[index]->nameid == nameid )
			return true;
		for( j = 0; j < sd->inventory_data[index]->slot; j++ )
			if( sd->inventory.u.items_inventory[index].card[j] == nameid )
				return true;
	}

	return false;
}

/**
 * Check adoption rules
 * @param p1_sd: Player 1
 * @param p2_sd: Player 2
 * @param b_sd: Player that will be adopted
 * @return ADOPT_ALLOWED - Sent message to Baby to accept or deny
 *         ADOPT_ALREADY_ADOPTED - Already adopted
 *         ADOPT_MARRIED_AND_PARTY - Need to be married and in the same party
 *         ADOPT_EQUIP_RINGS - Need wedding rings equipped
 *         ADOPT_NOT_NOVICE - Adoptee is not a Novice
 *         ADOPT_CHARACTER_NOT_FOUND - Parent or Baby not found
 *         ADOPT_MORE_CHILDREN - Cannot adopt more than 1 Baby (client message)
 *         ADOPT_LEVEL_70 - Parents need to be level 70+ (client message)
 *         ADOPT_MARRIED - Cannot adopt a married person (client message)
 */
enum adopt_responses pc_try_adopt(struct map_session_data *p1_sd, struct map_session_data *p2_sd, struct map_session_data *b_sd)
{
	if( !p1_sd || !p2_sd || !b_sd )
		return ADOPT_CHARACTER_NOT_FOUND;

	if( b_sd->status.father || b_sd->status.mother || b_sd->adopt_invite )
		return ADOPT_ALREADY_ADOPTED; // Already adopted baby / in adopt request

	if( !p1_sd->status.partner_id || !p1_sd->status.party_id || p1_sd->status.party_id != b_sd->status.party_id )
		return ADOPT_MARRIED_AND_PARTY; // You need to be married and in party with baby to adopt

	if( p1_sd->status.partner_id != p2_sd->status.char_id || p2_sd->status.partner_id != p1_sd->status.char_id )
		return ADOPT_MARRIED_AND_PARTY; // Not married, wrong married

	if( p2_sd->status.party_id != p1_sd->status.party_id )
		return ADOPT_MARRIED_AND_PARTY; // Both parents need to be in the same party

	// Parents need to have their ring equipped
	if( !pc_isequipped(p1_sd, WEDDING_RING_M) && !pc_isequipped(p1_sd, WEDDING_RING_F) )
		return ADOPT_EQUIP_RINGS;

	if( !pc_isequipped(p2_sd, WEDDING_RING_M) && !pc_isequipped(p2_sd, WEDDING_RING_F) )
		return ADOPT_EQUIP_RINGS;

	// Already adopted a baby
	if( p1_sd->status.child || p2_sd->status.child ) {
		clif_Adopt_reply(p1_sd, ADOPT_REPLY_MORE_CHILDREN);
		return ADOPT_MORE_CHILDREN;
	}

	// Parents need at least lvl 70 to adopt
	if( p1_sd->status.base_level < 70 || p2_sd->status.base_level < 70 ) {
		clif_Adopt_reply(p1_sd, ADOPT_REPLY_LEVEL_70);
		return ADOPT_LEVEL_70;
	}

	if( b_sd->status.partner_id ) {
		clif_Adopt_reply(p1_sd, ADOPT_REPLY_MARRIED);
		return ADOPT_MARRIED;
	}

	if( !((b_sd->status.class_ >= JOB_NOVICE && b_sd->status.class_ <= JOB_THIEF) || b_sd->status.class_ == JOB_SUMMONER ||
		b_sd->status.class_ == JOB_SUPER_NOVICE || b_sd->status.class_ == JOB_NINJA || b_sd->status.class_ == JOB_TAEKWON ||
		b_sd->status.class_ == JOB_GUNSLINGER) )
		return ADOPT_NOT_NOVICE;

	return ADOPT_ALLOWED;
}

/*==========================================
 * Adoption Process
 *------------------------------------------*/
bool pc_adoption(struct map_session_data *p1_sd, struct map_session_data *p2_sd, struct map_session_data *b_sd)
{
	int job, joblevel;
	uint32 jobexp;

	if( pc_try_adopt(p1_sd, p2_sd, b_sd) != ADOPT_ALLOWED )
		return false;

	// Preserve current job levels and progress
	joblevel = b_sd->status.job_level;
	jobexp = b_sd->status.job_exp;

	job = pc_mapid2jobid(b_sd->class_|JOBL_BABY, b_sd->status.sex);
	if( job != -1 && pc_jobchange(b_sd, job, 0) ) {
		// Success, proceed to configure parents and baby skills
		p1_sd->status.child = b_sd->status.char_id;
		p2_sd->status.child = b_sd->status.char_id;
		b_sd->status.father = p1_sd->status.char_id;
		b_sd->status.mother = p2_sd->status.char_id;

		// Restore progress
		b_sd->status.job_level = joblevel;
		clif_updatestatus(b_sd, SP_JOBLEVEL);
		b_sd->status.job_exp = jobexp;
		clif_updatestatus(b_sd, SP_JOBEXP);

		// Baby Skills
		pc_skill(b_sd, WE_BABY, 1, 0);
		pc_skill(b_sd, WE_CALLPARENT, 1, 0);
		pc_skill(b_sd, WE_CHEERUP, 1, 0);

		// Parents Skills
		pc_skill(p1_sd, WE_CALLBABY, 1, 0);
		pc_skill(p2_sd, WE_CALLBABY, 1, 0);

		achievement_update_objective(b_sd, AG_BABY, 1, 1);
		achievement_update_objective(p1_sd, AG_BABY, 1, 2);
		achievement_update_objective(p2_sd, AG_BABY, 1, 2);

		chrif_save(p1_sd, CSAVE_NORMAL);
		chrif_save(p2_sd, CSAVE_NORMAL);
		chrif_save(b_sd, CSAVE_NORMAL);

		return true;
	}

	return false; // Job Change Fail
}

/*==========================================
 * Check if player can use/equip selected item. Used by pc_isUseitem and pc_isequip
   Returns:
    false : Cannot use/equip
    true  : Can use/equip
 * Credits:
    [Inkfish] for first idea
    [Haru] for third-classes extension
    [Cydh] finishing :D
 *------------------------------------------*/
static bool pc_isItemClass(struct map_session_data *sd, struct item_data *item) {
	while( 1 ) {
		//Normal classes (no upper, no baby, no third)
		if( item->class_upper&ITEMJ_NORMAL && !(sd->class_&(JOBL_UPPER|JOBL_THIRD|JOBL_BABY)) )
			break;
#ifndef RENEWAL
		//Allow third classes to use trans. class items
		if( item->class_upper&ITEMJ_UPPER && sd->class_&(JOBL_UPPER|JOBL_THIRD) ) //Trans. classes
			break;
		//Third-baby classes can use same item too
		if( item->class_upper&ITEMJ_BABY && sd->class_&JOBL_BABY ) //Baby classes
			break;
		//Don't need to decide specific rules for third-classes?
		//Items for third classes can be used for all third classes
		if( item->class_upper&(ITEMJ_THIRD|ITEMJ_THIRD_TRANS|ITEMJ_THIRD_BABY) && sd->class_&JOBL_THIRD )
			break;
#else
		//Trans. classes (exl. third-trans.)
		if( item->class_upper&ITEMJ_UPPER && sd->class_&JOBL_UPPER && !(sd->class_&JOBL_THIRD) )
			break;
		//Baby classes (exl. third-baby)
		if( item->class_upper&ITEMJ_BABY && sd->class_&JOBL_BABY && !(sd->class_&JOBL_THIRD) )
			break;
		//Third classes (exl. third-trans. and baby-third)
		if( item->class_upper&ITEMJ_THIRD && sd->class_&JOBL_THIRD && !(sd->class_&(JOBL_UPPER|JOBL_BABY)) )
			break;
		//Trans-third classes
		if( item->class_upper&ITEMJ_THIRD_TRANS && sd->class_&JOBL_THIRD && sd->class_&JOBL_UPPER )
			break;
		//Third-baby classes
		if( item->class_upper&ITEMJ_THIRD_BABY && sd->class_&JOBL_THIRD && sd->class_&JOBL_BABY )
			break;
#endif
		return false;
	}
	return true;
}

/**
 * Checks if the player can equip the item at index n in inventory.
 * @param sd
 * @param n Item index in inventory
 * @return ITEM_EQUIP_ACK_OK(0) if can be equipped, or ITEM_EQUIP_ACK_FAIL(1)/ITEM_EQUIP_ACK_FAILLEVEL(2) if can't
 */
uint8 pc_isequip(struct map_session_data *sd, int n)
{
	struct item_data *item;

	nullpo_retr(ITEM_EQUIP_ACK_FAIL, sd);

	item = sd->inventory_data[n];

	if (!item)
		return ITEM_EQUIP_ACK_FAIL;

	if (pc_has_permission(sd, PC_PERM_USE_ALL_EQUIPMENT))
		return ITEM_EQUIP_ACK_OK;

	if ((item->elv && sd->status.base_level < (unsigned int)item->elv) ||
		(item->elvmax && sd->status.base_level > (unsigned int)item->elvmax)) {
		clif_msg(sd,ITEM_CANT_EQUIP_NEED_LEVEL); // You cannot equip this item with your current level.
		return ITEM_EQUIP_ACK_FAILLEVEL;
	}

	if (item->sex != 2 && sd->status.sex != item->sex)
		return ITEM_EQUIP_ACK_FAIL;

	if (!battle_config.allow_equip_restricted_item && itemdb_isNoEquip(item,sd->bl.m))
		return ITEM_EQUIP_ACK_FAIL; //Fail to equip if item is restricted

	if (item->equip&EQP_AMMO) {
		switch (item->look) {
			case AMMO_ARROW:
				if (battle_config.ammo_check_weapon && sd->status.weapon != W_BOW &&
					sd->status.weapon != W_MUSICAL && sd->status.weapon != W_WHIP) {
					clif_msg(sd,ITEM_NEED_BOW);
					return ITEM_EQUIP_ACK_FAIL;
				}
				break;
			case AMMO_THROWABLE_DAGGER:
				if (!pc_checkskill(sd,AS_VENOMKNIFE))
					return ITEM_EQUIP_ACK_FAIL;
				break;
			case AMMO_BULLET:
			case AMMO_SHELL:
				if (battle_config.ammo_check_weapon && sd->status.weapon != W_REVOLVER && sd->status.weapon != W_RIFLE &&
					sd->status.weapon != W_GATLING && sd->status.weapon != W_SHOTGUN
#ifdef RENEWAL
					&& sd->status.weapon != W_GRENADE
#endif
				) {
					clif_msg(sd,ITEM_BULLET_EQUIP_FAIL);
					return ITEM_EQUIP_ACK_FAIL;
				}
				break;
#ifndef RENEWAL
			case AMMO_GRENADE:
				if (battle_config.ammo_check_weapon && sd->status.weapon != W_GRENADE) {
					clif_msg(sd,ITEM_BULLET_EQUIP_FAIL);
					return ITEM_EQUIP_ACK_FAIL;
				}
				break;
#endif
			case AMMO_CANNONBALL:
				if (!pc_ismadogear(sd) && (sd->status.class_ == JOB_MECHANIC_T || sd->status.class_ == JOB_MECHANIC)) {
					clif_msg(sd,ITEM_NEED_MADOGEAR); // Item can only be used when Mado Gear is mounted.
					return ITEM_EQUIP_ACK_FAIL;
				}
				if (sd->state.active && !pc_iscarton(sd) && //Check if sc data is already loaded
					(sd->status.class_ == JOB_GENETIC_T || sd->status.class_ == JOB_GENETIC)) {
					clif_msg(sd,ITEM_NEED_CART); // Only available when cart is mounted.
					return ITEM_EQUIP_ACK_FAIL;
				}
				break;
		}
	}

	if (sd->sc.count) {
		if ((item->equip&EQP_ARMS) && item->type == IT_WEAPON && sd->sc.data[SC_STRIPWEAPON])
			return ITEM_EQUIP_ACK_FAIL; //Also works with left-hand weapons [DracoRPG]
		if ((item->equip&EQP_SHIELD) && item->type == IT_ARMOR && sd->sc.data[SC_STRIPSHIELD])
			return ITEM_EQUIP_ACK_FAIL;
		if ((item->equip&EQP_ARMOR) && sd->sc.data[SC_STRIPARMOR])
			return ITEM_EQUIP_ACK_FAIL;
		if ((item->equip&EQP_HEAD_TOP) && sd->sc.data[SC_STRIPHELM])
			return ITEM_EQUIP_ACK_FAIL;
		if ((item->equip&EQP_ARMS) && sd->sc.data[SC__WEAKNESS])
			return ITEM_EQUIP_ACK_FAIL;
		if ((item->equip&EQP_ACC) && sd->sc.data[SC__STRIPACCESSORY])
			return ITEM_EQUIP_ACK_FAIL;
		if (item->equip && (sd->sc.data[SC_KYOUGAKU] || sd->sc.data[SC_SUHIDE]))
			return ITEM_EQUIP_ACK_FAIL;
		//Spirit of Super Novice equip bonuses [Skotlex]
		if (sd->sc.data[SC_SPIRIT] && sd->sc.data[SC_SPIRIT]->val2 == SL_SUPERNOVICE) {
			if (sd->status.base_level > 90 && (item->equip&EQP_HELM))
				return ITEM_EQUIP_ACK_OK; //Can equip all helms
			if (sd->status.base_level > 96 && (item->equip&EQP_ARMS) && item->type == IT_WEAPON && item->wlv == 4) {
				switch (item->look) { //In weapons, the look determines type of weapon
					case W_DAGGER: //All level 4 - Daggers
					case W_1HSWORD: //All level 4 - 1H Swords
					case W_1HAXE: //All level 4 - 1H Axes
					case W_MACE: //All level 4 - 1H Maces
					case W_STAFF: //All level 4 - 1H Staves
					case W_2HSTAFF: //All level 4 - 2H Staves
						return ITEM_EQUIP_ACK_OK;
				}
			}
		}
	}

	//Not equipable by class [Skotlex]
	if (!(1<<(sd->class_&MAPID_BASEMASK)&item->class_base[(sd->class_&JOBL_2_1 ? 1 : (sd->class_&JOBL_2_2 ? 2 : 0))]))
		return ITEM_EQUIP_ACK_FAIL;

	if (!pc_isItemClass(sd,item))
		return ITEM_EQUIP_ACK_FAIL;

	return ITEM_EQUIP_ACK_OK;
}

/*==========================================
 * No problem with the session id
 * set the status that has been sent from char server
 *------------------------------------------*/
bool pc_authok(struct map_session_data *sd, int login_id2, time_t expiration_time, int group_id, struct mmo_charstatus *st, bool changing_mapservers)
{
	int i;
	unsigned long tick = gettick();
	uint32 ip = session[sd->fd]->client_addr;

	sd->login_id2 = login_id2;
	sd->group_id = group_id;

	//Load user permissions
	pc_group_pc_load(sd);

	memcpy(&sd->status, st, sizeof(*st));

	if (st->sex != sd->status.sex) {
		clif_authfail_fd(sd->fd, 0);
		return false;
	}

	//Set the map-server used job id [Skotlex]
	i = pc_jobid2mapid(sd->status.class_);
	if (i == -1) { //Invalid class?
		ShowError("pc_authok: Invalid class %d for player %s (%d:%d). Class was changed to novice.\n", sd->status.class_, sd->status.name, sd->status.account_id, sd->status.char_id);
		sd->status.class_ = JOB_NOVICE;
		sd->class_ = MAPID_NOVICE;
	} else
		sd->class_ = i;

	//Checks and fixes to character status data, that are required
	//in case of configuration change or stuff, which cannot be
	//checked on char-server
	if ((sd->class_&MAPID_BASEMASK) == MAPID_SUMMONER) {
		sd->status.hair = cap_value(sd->status.hair,MIN_DORAM_HAIR_STYLE,MAX_DORAM_HAIR_STYLE);
		sd->status.hair_color = cap_value(sd->status.hair_color,MIN_DORAM_HAIR_COLOR,MAX_DORAM_HAIR_COLOR);
		sd->status.clothes_color = cap_value(sd->status.clothes_color,MIN_DORAM_CLOTH_COLOR,MAX_DORAM_CLOTH_COLOR);
	} else {
		sd->status.hair = cap_value(sd->status.hair,MIN_HAIR_STYLE,MAX_HAIR_STYLE);
		sd->status.hair_color = cap_value(sd->status.hair_color,MIN_HAIR_COLOR,MAX_HAIR_COLOR);
		sd->status.clothes_color = cap_value(sd->status.clothes_color,MIN_CLOTH_COLOR,MAX_CLOTH_COLOR);
	}
	sd->status.body = cap_value(sd->status.body,MIN_BODY_STYLE,MAX_BODY_STYLE);

	//Hair style 0 and body dye 1 arn't allowed on official servers
	//Adjust them to hair style 1 and body dye 0 which are the same things but officially used
	//This prevents visual glitches on the character select and equip window
	//Example: Warlock on body dye 1 will show color glitch on the crystal shards on the outfit
	if (sd->status.hair == 0)
		sd->status.hair = 1;
	if (sd->status.clothes_color == 1)
		sd->status.clothes_color = 0;

	//Initializations to null/0 unneeded since map_session_data was filled with 0 upon allocation
	sd->state.connect_new = 1;

	sd->followtimer = INVALID_TIMER; //[MouseJstr]
	sd->invincible_timer = INVALID_TIMER;
	sd->npc_timer_id = INVALID_TIMER;
	sd->pvp_timer = INVALID_TIMER;
	sd->expiration_tid = INVALID_TIMER;
	sd->autotrade_tid = INVALID_TIMER;

#ifdef SECURE_NPCTIMEOUT
	//Initialize to defaults/expected
	sd->npc_idle_timer = INVALID_TIMER;
	sd->npc_idle_tick = tick;
	sd->npc_idle_type = NPCT_INPUT;
	sd->state.ignoretimeout = false;
#endif

	sd->canuseitem_tick = tick;
	sd->canequip_tick = tick;
	sd->cantalk_tick = tick;
	sd->canskill_tick = tick;
	sd->cansendmail_tick = tick;

	for (i = 0; i < MAX_SPIRITBALL; i++)
		sd->spiritball_timer[i] = INVALID_TIMER;
	for (i = 0; i < MAX_SHIELDBALL; i++)
		sd->shieldball_timer[i] = INVALID_TIMER;
	for (i = 0; i < MAX_RAGEBALL; i++)
		sd->rageball_timer[i] = INVALID_TIMER;
	for (i = 0; i < MAX_CHARMBALL; i++)
		sd->charmball_timer[i] = INVALID_TIMER;
	for (i = 0; i < MAX_SOULBALL; i++)
		sd->soulball_timer[i] = INVALID_TIMER;
	for (i = 0; i < MAX_PC_BONUS; i++) {
		sd->autobonus[i].active = INVALID_TIMER;
		sd->autobonus2[i].active = INVALID_TIMER;
		sd->autobonus3[i].active = INVALID_TIMER;
	}

	if (battle_config.item_auto_get)
		sd->state.autoloot = 10000;

	if (battle_config.disp_experience)
		sd->state.showexp = 1;
	if (battle_config.disp_zeny)
		sd->state.showzeny = 1;
#ifdef VIP_ENABLE
	if (!battle_config.vip_disp_rate)
		sd->vip.disableshowrate = 1;
#endif

	if (!(battle_config.display_skill_fail&2))
		sd->state.showdelay = 1;

	memset(&sd->inventory, 0, sizeof(struct s_storage));
	memset(&sd->cart, 0, sizeof(struct s_storage));
	memset(&sd->storage, 0, sizeof(struct s_storage));
	memset(&sd->premiumStorage, 0, sizeof(struct s_storage));
	memset(&sd->equip_index, -1, sizeof(sd->equip_index));
	memset(&sd->equip_switch_index, -1, sizeof(sd->equip_switch_index));

	if (pc_isinvisible(sd) && !pc_can_use_command(sd, "hide", COMMAND_ATCOMMAND))
		sd->status.option &=~ OPTION_INVISIBLE;

	status_change_init(&sd->bl);

	sd->sc.option = sd->status.option; //This is the actual option used in battle

	unit_dataset(&sd->bl);

	sd->guild_x = -1;
	sd->guild_y = -1;

	sd->delayed_damage = 0;

	//Event Timers
	for (i = 0; i < MAX_EVENTTIMER; i++)
		sd->eventtimer[i] = INVALID_TIMER;

	//Rental Timer
	sd->rental_timer = INVALID_TIMER;

	for (i = 0; i < 3; i++)
		sd->hate_mob[i] = -1;

	sd->quest_log = NULL;
	sd->num_quests = 0;
	sd->avail_quests = 0;
	sd->save_quest = false;
	sd->count_rewarp = 0;
	sd->qi_display = NULL;
	sd->qi_count = 0;

	//Warp player
	if ((i = pc_setpos(sd,sd->status.last_point.map, sd->status.last_point.x, sd->status.last_point.y, CLR_OUTSIGHT)) != SETPOS_OK) {
		ShowError ("Last_point_map %s - id %d not found (error code %d)\n", mapindex_id2name(sd->status.last_point.map), sd->status.last_point.map, i);
		//Try warping to a default map instead (church graveyard)
		if (pc_setpos(sd, mapindex_name2id(MAP_PRONTERA), 273, 354, CLR_OUTSIGHT) != SETPOS_OK) {
			//If we fail again
			clif_authfail_fd(sd->fd, 0);
			return false;
		}
	}

	clif_authok(sd);

	//Prevent S. Novices from getting the no-death bonus just yet [Skotlex]
	sd->die_counter = -1;

	//Display login notice
	ShowInfo("'"CL_WHITE"%s"CL_RESET"' logged in."
	         " (AID/CID: '"CL_WHITE"%d/%d"CL_RESET"',"
	         " IP: '"CL_WHITE"%d.%d.%d.%d"CL_RESET"',"
	         " Group '"CL_WHITE"%d"CL_RESET"').\n",
	         sd->status.name, sd->status.account_id, sd->status.char_id,
	         CONVIP(ip), sd->group_id);

	//Send friends list
	clif_friendslist_send(sd);

	if (!changing_mapservers) {
		if (battle_config.display_version == 1)
			pc_show_version(sd);
		//Message of the Day [Valaris]
		for (i = 0; i < MOTD_LINE_SIZE && motd_text[i][0]; i++) {
			if (battle_config.motd_type)
				clif_messagecolor(&sd->bl, color_table[COLOR_LIGHT_GREEN], motd_text[i], false, SELF);
			else
				clif_displaymessage(sd->fd, motd_text[i]);
		}
		if (expiration_time)
			sd->expiration_time = expiration_time;
		//Fixes login-without-aura glitch (the screen won't blink at this point, don't worry)
		clif_changemap(sd,sd->bl.m,sd->bl.x,sd->bl.y);
	}

	//[Ind]
	sd->sc_display = NULL;
	sd->sc_display_count = 0;

	//Player has not yet received the CashShop list
	sd->status.cashshop_sent = false;

	sd->last_addeditem_index = -1;

	sd->bonus_script.head = NULL;
	sd->bonus_script.count = 0;

	sd->hatEffectIDs = NULL;
	sd->hatEffectCount = 0;

	sd->catch_target_class = PET_CATCH_FAIL;

	//Check EXP overflow, since in previous revision EXP on Max Level can be more than 'official' Max EXP
	if (pc_is_maxbaselv(sd) && sd->status.base_exp > MAX_LEVEL_BASE_EXP) {
		sd->status.base_exp = MAX_LEVEL_BASE_EXP;
		clif_updatestatus(sd,SP_BASEEXP);
	}
	if (pc_is_maxjoblv(sd) && sd->status.job_exp > MAX_LEVEL_JOB_EXP) {
		sd->status.job_exp = MAX_LEVEL_JOB_EXP;
		clif_updatestatus(sd,SP_JOBEXP);
	}

	//Request all registries (auth is considered completed whence they arrive)
	intif_request_registry(sd,7);
	return true;
}

/*==========================================
 * Closes a connection because it failed to be authenticated from the char server.
 *------------------------------------------*/
void pc_authfail(struct map_session_data *sd)
{
	clif_authfail_fd(sd->fd, 0);
}

/**
 * Player register a bl as hatred
 * @param sd : player session
 * @param pos : hate position [0;2]
 * @param bl : target bl
 * @return false : failed, true : success
 */
bool pc_set_hate_mob(struct map_session_data *sd, int pos, struct block_list *bl)
{
	int class_;

	if (!sd || !bl || pos < 0 || pos > 2)
		return false;
	if (sd->hate_mob[pos] != -1) { //Can't change hate targets
		clif_hate_info(sd, pos, sd->hate_mob[pos], 0); //Display current
		return false;
	}
	class_ = status_get_class(bl);
	if (!pcdb_checkid(class_)) {
		unsigned int max_hp = status_get_max_hp(bl);

		if ((pos == 1 && max_hp < 6000) || (pos == 2 && max_hp < 20000))
			return false;
		if (pos != status_get_size(bl))
			return false; //Wrong size
	}
	sd->hate_mob[pos] = class_;
	pc_setglobalreg(sd, sg_info[pos].hate_var, class_ + 1);
	clif_hate_info(sd, pos, class_, 1);
	return true;
}

/*==========================================
 * Invoked once after the char/account/account2 registry variables are received. [Skotlex]
 * We didn't receive item information at this point so DO NOT attempt to do item operations here.
 * See intif_parse_StorageReceived() for item operations [lighta]
 *------------------------------------------*/
void pc_reg_received(struct map_session_data *sd)
{
	uint8 i;

	sd->change_level_2nd = pc_readglobalreg(sd,"jobchange_level");
	sd->change_level_3rd = pc_readglobalreg(sd,"jobchange_level_3rd");
	sd->die_counter = pc_readglobalreg(sd,PCDIECOUNTER_VAR);

	sd->langtype = pc_readaccountreg(sd,LANGTYPE_VAR);
	if (msg_checklangtype(sd->langtype,true) < 0)
		sd->langtype = 0; //Invalid langtype reset to default

	//Cash shop
	sd->cashPoints = pc_readaccountreg(sd,CASHPOINT_VAR);
	sd->kafraPoints = pc_readaccountreg(sd,KAFRAPOINT_VAR);

	//Cooking Exp
	sd->cook_mastery = pc_readglobalreg(sd,COOKMASTERY_VAR);

	//Better check for class rather than skill to prevent "skill resets" from unsetting this
	if ((sd->class_&MAPID_BASEMASK) == MAPID_TAEKWON) {
		sd->mission_mobid = pc_readglobalreg(sd,"TK_MISSION_ID");
		sd->mission_count = pc_readglobalreg(sd,"TK_MISSION_COUNT");
	}

	if (battle_config.feature_banking)
		sd->bank_vault = pc_readreg2(sd,BANK_VAULT_VAR);

	if (battle_config.feature_roulette) {
		sd->roulette_point.bronze = pc_readreg2(sd,ROULETTE_BRONZE_VAR);
		sd->roulette_point.silver = pc_readreg2(sd,ROULETTE_SILVER_VAR);
		sd->roulette_point.gold = pc_readreg2(sd,ROULETTE_GOLD_VAR);
	}

	sd->roulette.prizeIdx = -1;

	//SG map and mob read [Komurka]
	for (i = 0; i < MAX_PC_FEELHATE; i++) { //For now - someone need to make reading from txt/sql
		uint16 j;

		if ((j = pc_readglobalreg(sd,sg_info[i].feel_var)) != 0) {
			sd->feel_map[i].index = j;
			sd->feel_map[i].m = map_mapindex2mapid(j);
		} else {
			sd->feel_map[i].index = 0;
			sd->feel_map[i].m = -1;
		}
		sd->hate_mob[i] = pc_readglobalreg(sd,sg_info[i].hate_var) - 1;
	}

	if ((i = pc_checkskill(sd,RG_PLAGIARISM)) > 0) {
		sd->cloneskill_idx = skill_get_index(pc_readglobalreg(sd,SKILL_VAR_PLAGIARISM));
		if (sd->cloneskill_idx >= 0) {
			sd->status.skill[sd->cloneskill_idx].id = pc_readglobalreg(sd,SKILL_VAR_PLAGIARISM);
			sd->status.skill[sd->cloneskill_idx].lv = pc_readglobalreg(sd,SKILL_VAR_PLAGIARISM_LV);
			if (sd->status.skill[sd->cloneskill_idx].lv > i)
				sd->status.skill[sd->cloneskill_idx].lv = i;
			sd->status.skill[sd->cloneskill_idx].flag = SKILL_FLAG_PLAGIARIZED;
		}
	}
	if ((i = pc_checkskill(sd,SC_REPRODUCE)) > 0) {
		sd->reproduceskill_idx = skill_get_index(pc_readglobalreg(sd,SKILL_VAR_REPRODUCE));
		if (sd->reproduceskill_idx >= 0) {
			sd->status.skill[sd->reproduceskill_idx].id = pc_readglobalreg(sd,SKILL_VAR_REPRODUCE);
			sd->status.skill[sd->reproduceskill_idx].lv = pc_readglobalreg(sd,SKILL_VAR_REPRODUCE_LV);
			if (i < sd->status.skill[sd->reproduceskill_idx].lv)
				sd->status.skill[sd->reproduceskill_idx].lv = i;
			sd->status.skill[sd->reproduceskill_idx].flag = SKILL_FLAG_PLAGIARIZED;
		}
	}

	//Weird, maybe registries were reloaded?
	if (sd->state.active)
		return;
	sd->state.active = 1;
	sd->state.pc_loaded = false; //Ensure inventory data and status data is loaded before we calculate player stats

	intif_storage_request(sd, TABLE_STORAGE, 0, STOR_MODE_ALL); //Request storage data
	intif_storage_request(sd, TABLE_CART, 0, STOR_MODE_ALL); //Request cart data
	intif_storage_request(sd, TABLE_INVENTORY, 0, STOR_MODE_ALL); //Request inventory data

	if (sd->status.party_id)
		party_member_joined(sd);
	if (sd->status.guild_id)
		guild_member_joined(sd);
	if (sd->status.clan_id)
		clan_member_joined(sd);

	//Pet
	if (sd->status.pet_id > 0)
		intif_request_petdata(sd->status.account_id, sd->status.char_id, sd->status.pet_id);

	//Homunculus [albator]
	if (sd->status.hom_id > 0)
		intif_homunculus_requestload(sd->status.account_id, sd->status.hom_id);
	if (sd->status.mer_id > 0)
		intif_mercenary_request(sd->status.mer_id, sd->status.char_id);
	if (sd->status.ele_id > 0)
		intif_elemental_request(sd->status.ele_id, sd->status.char_id);

	map_addiddb(&sd->bl);
	map_delnickdb(sd->status.char_id, sd->status.name);
	if (!chrif_auth_finished(sd))
		ShowError("pc_reg_received: Failed to properly remove player %d:%d from logging db!\n", sd->status.account_id, sd->status.char_id);

	chrif_skillcooldown_request(sd->status.account_id, sd->status.char_id);
	chrif_bsdata_request(sd->status.char_id);
#ifdef VIP_ENABLE
	sd->vip.time = 0;
	sd->vip.enabled = 0;
	chrif_req_login_operation(sd->status.account_id, sd->status.name, CHRIF_OP_LOGIN_VIP, 0, 0x1|0x8); //Request VIP information
#endif
	intif_Mail_requestinbox(sd->status.char_id, 0, MAIL_INBOX_NORMAL); //MAIL SYSTEM - Request Mail Inbox
	intif_request_questlog(sd);

	if (battle_config.feature_achievement) {
		sd->achievement_data.total_score = 0;
		sd->achievement_data.level = 0;
		sd->achievement_data.save = false;
		sd->achievement_data.count = 0;
		sd->achievement_data.incompleteCount = 0;
		sd->achievement_data.achievements = NULL;
		intif_request_achievements(sd->status.char_id);
	}

	if (!sd->state.connect_new && sd->fd) { //Character already loaded map! Gotta trigger LoadEndAck manually
		sd->state.connect_new = 1;
		clif_parse_LoadEndAck(sd->fd, sd);
	}

	if (pc_isinvisible(sd)) {
		sd->vd.class_ = JT_INVISIBLE;
		clif_displaymessage(sd->fd, msg_txt(sd, 11)); // Invisible: On
		//Decrement the number of pvp players on the map
		mapdata[sd->bl.m].users_pvp--;
		if (mapdata[sd->bl.m].flag.pvp && !mapdata[sd->bl.m].flag.pvp_nocalcrank && sd->pvp_timer != INVALID_TIMER) {
			//Unregister the player for ranking
			delete_timer(sd->pvp_timer, pc_calc_pvprank_timer);
			sd->pvp_timer = INVALID_TIMER;
		}
		clif_changeoption(&sd->bl);
	}

	channel_autojoin(sd);
}

static int pc_calc_skillpoint(struct map_session_data *sd)
{
	uint16 i, skill_point = 0;

	nullpo_ret(sd);

	for (i = 1; i < MAX_SKILL; i++) {
		uint8 lv;

		if ((lv = pc_checkskill(sd,i)) > 0) {
			uint16 inf2 = skill_get_inf2(i);

			if ((!(inf2&INF2_QUEST_SKILL) || battle_config.quest_skill_learn) &&
				!(inf2&(INF2_WEDDING_SKILL|INF2_SPIRIT_SKILL))) { //Do not count wedding/link skills [Skotlex]
				if (sd->status.skill[i].flag == SKILL_FLAG_PERMANENT)
					skill_point += lv;
				else if (sd->status.skill[i].flag == SKILL_FLAG_REPLACED_LV_0)
					skill_point += (sd->status.skill[i].flag - SKILL_FLAG_REPLACED_LV_0);
			}
		}
	}

	return skill_point;
}

/**
 * Calculation of skill level.
 * @param sd
 */
void pc_calc_skilltree(struct map_session_data *sd)
{
	int i, flag;
	int c = 0;

	nullpo_retv(sd);

	i = pc_calc_skilltree_normalize_job(sd);
	c = pc_mapid2jobid(i, sd->status.sex);

	if( c == -1 ) { //Unable to normalize job??
		ShowError("pc_calc_skilltree: Unable to normalize job %d for character %s (%d:%d)\n", i, sd->status.name, sd->status.account_id, sd->status.char_id);
		return;
	}

	c = pc_class2idx(c);

	for( i = 0; i < MAX_SKILL; i++ ) {
		if( sd->status.skill[i].flag != SKILL_FLAG_PLAGIARIZED && sd->status.skill[i].flag != SKILL_FLAG_PERM_GRANTED ) //Don't touch these
			sd->status.skill[i].id = 0; //First clear skills
		//Permanent skills that must be re-checked
		if( sd->status.skill[i].flag == SKILL_FLAG_PERM_GRANTED ) {
			switch( i ) {
				case NV_TRICKDEAD:
					if( (sd->class_&MAPID_UPPERMASK) != MAPID_NOVICE ) {
						sd->status.skill[i].id = 0;
						sd->status.skill[i].lv = 0;
						sd->status.skill[i].flag = SKILL_FLAG_PERMANENT;
					}
					break;
			}
		}
	}

	for( i = 0; i < MAX_SKILL; i++ ) {
		if( sd->status.skill[i].flag != SKILL_FLAG_PERMANENT && sd->status.skill[i].flag != SKILL_FLAG_PERM_GRANTED &&
			sd->status.skill[i].flag != SKILL_FLAG_PLAGIARIZED ) { //Restore original level of skills after deleting earned skills
			sd->status.skill[i].lv = (sd->status.skill[i].flag == SKILL_FLAG_TEMPORARY) ? 0 : sd->status.skill[i].flag - SKILL_FLAG_REPLACED_LV_0;
			sd->status.skill[i].flag = SKILL_FLAG_PERMANENT;
		}
		if( sd->sc.count && sd->sc.data[SC_SPIRIT] &&
			sd->sc.data[SC_SPIRIT]->val2 == SL_BARDDANCER && i >= DC_HUMMING && i<= DC_SERVICEFORYOU ) { //Enable Bard/Dancer spirit linked skills
			if( sd->status.sex ) { //Link dancer skills to bard
				if( sd->status.skill[i - 8].lv < 10 )
					continue;
				sd->status.skill[i].id = i;
				sd->status.skill[i].lv = sd->status.skill[i - 8].lv; //Set the level to the same as the linking skill
				sd->status.skill[i].flag = SKILL_FLAG_TEMPORARY; //Tag it as a non-savable, non-uppable, bonus skill
			} else { //Link bard skills to dancer
				if( sd->status.skill[i].lv < 10 )
					continue;
				sd->status.skill[i - 8].id = i - 8;
				sd->status.skill[i - 8].lv = sd->status.skill[i].lv; //Set the level to the same as the linking skill
				sd->status.skill[i - 8].flag = SKILL_FLAG_TEMPORARY; //Tag it as a non-savable, non-uppable, bonus skill
			}
		}
	}

	//Removes Taekwon Ranker skill bonus
	if( (sd->class_&MAPID_UPPERMASK) != MAPID_TAEKWON ) {
		uint16 c_ = pc_class2idx(JOB_TAEKWON);

		for( i = 0; i < MAX_SKILL_TREE; i++ ) {
			uint16 x = skill_get_index(skill_tree[c_][i].id), skill_id;

			if( (skill_id = sd->status.skill[x].id) && x > 0 &&
				sd->status.skill[x].flag != SKILL_FLAG_PLAGIARIZED &&
				sd->status.skill[x].flag != SKILL_FLAG_PERM_GRANTED )
			{
				if( skill_id == NV_BASIC || skill_id == NV_FIRSTAID || skill_id == WE_CALLBABY )
					continue;
				sd->status.skill[x].id = 0;
			}
		}
	}

	if( pc_has_permission(sd, PC_PERM_ALL_SKILL) ) {
		for( i = 0; i < MAX_SKILL; i++ ) {
			switch( i ) {
				/**
				 * Dummy skills must be added here otherwise they'll be displayed in the,
				 * skill tree and since they have no icons they'll give resource errors
				 */
				case SM_SELFPROVOKE:
				case AB_DUPLELIGHT_MELEE:
				case AB_DUPLELIGHT_MAGIC:
				case WL_CHAINLIGHTNING_ATK:
				case WL_TETRAVORTEX_FIRE:
				case WL_TETRAVORTEX_WATER:
				case WL_TETRAVORTEX_WIND:
				case WL_TETRAVORTEX_GROUND:
				case WL_SUMMON_ATK_FIRE:
				case WL_SUMMON_ATK_WIND:
				case WL_SUMMON_ATK_WATER:
				case WL_SUMMON_ATK_GROUND:
				case NC_MAGMA_ERUPTION_DOTDAMAGE:
				case LG_OVERBRAND_BRANDISH:
				case LG_OVERBRAND_PLUSATK:
				case WM_REVERBERATION_MELEE:
				case WM_REVERBERATION_MAGIC:
				case WM_SEVERE_RAINSTORM_MELEE:
				case GN_CRAZYWEED_ATK:
				case GN_HELLS_PLANT_ATK:
				case GN_SLINGITEM_RANGEMELEEATK:
				case OB_OBOROGENSOU_TRANSITION_ATK:
				case RL_R_TRIP_PLUSATK:
				case RL_B_FLICKER_ATK:
				case SU_SV_ROOTTWIST_ATK:
				case SU_PICKYPECK_DOUBLE_ATK:
				case SU_CN_METEOR2:
				case SU_LUNATICCARROTBEAT2:
					continue;
				default:
					break;
			}
			if( skill_get_inf2(i)&(INF2_NPC_SKILL|INF2_GUILD_SKILL) )
				continue; //Only skills you can't have are npc/guild ones
			if( skill_get_max(i) > 0 )
				sd->status.skill[i].id = i;
		}
		return;
	}

	do {
		short id = 0;

		flag = 0;
		for( i = 0; i < MAX_SKILL_TREE && (id = skill_tree[c][i].id) > 0; i++ ) {
			int f;

			if( sd->status.skill[id].id )
				continue; //Skill already known

			f = 1;
			if( !battle_config.skillfree ) {
				int j;

				for( j = 0; j < MAX_PC_SKILL_REQUIRE; j++ ) {
					int k;

					if( (k = skill_tree[c][i].need[j].id) ) {
						if( !sd->status.skill[k].id || sd->status.skill[k].flag == SKILL_FLAG_TEMPORARY || sd->status.skill[k].flag == SKILL_FLAG_PLAGIARIZED )
							k = 0; //Not learned
						else if( sd->status.skill[k].flag >= SKILL_FLAG_REPLACED_LV_0 ) //Real learned level
							k = sd->status.skill[skill_tree[c][i].need[j].id].flag - SKILL_FLAG_REPLACED_LV_0;
						else
							k = pc_checkskill(sd,k);
						if( k < skill_tree[c][i].need[j].lv ) {
							f = 0;
							break;
						}
					}
				}
				if( sd->status.base_level < skill_tree[c][i].baselv ) { //We need to get the actual class in this case
					int class_ = pc_mapid2jobid(sd->class_, sd->status.sex);

					class_ = pc_class2idx(class_);
					if( class_ == c || (class_ != c && sd->status.base_level < skill_tree[class_][i].baselv) )
						f = 0; //Base level requirement wasn't satisfied
				}
				if( sd->status.job_level < skill_tree[c][i].joblv ) { //We need to get the actual class in this case
					int class_ = pc_mapid2jobid(sd->class_, sd->status.sex);

					class_ = pc_class2idx(class_);
					if( class_ == c || (class_ != c && sd->status.job_level < skill_tree[class_][i].joblv) )
						f = 0; //Job level requirement wasn't satisfied
				}
			}

			if( f ) {
				int inf2 = skill_get_inf2(id);

				if( !sd->status.skill[id].lv && (
					(inf2&INF2_QUEST_SKILL && !battle_config.quest_skill_learn) ||
					inf2&INF2_WEDDING_SKILL ||
					(inf2&INF2_SPIRIT_SKILL && !sd->sc.data[SC_SPIRIT])
				) )
					continue; //Cannot be learned via normal means. Note this check DOES allows raising already known skills.

				sd->status.skill[id].id = id;

				if( inf2&INF2_SPIRIT_SKILL ) { //Spirit skills cannot be learned, they will only show up on your tree when you get buffed.
					sd->status.skill[id].lv = 1; //Need to manually specify a skill level
					sd->status.skill[id].flag = SKILL_FLAG_TEMPORARY; //So it is not saved, and tagged as a "bonus" skill.
				}
				flag = 1; //Skill list has changed, perform another pass
			}
		}
	} while( flag );

	if( c > 0 && !sd->status.skill_point && pc_is_taekwon_ranker(sd) ) {
		short id = 0;

		/* Taekwon Ranker Bonus Skill Tree
		============================================
		- Grant All Taekwon Tree, but only as Bonus Skills in case they drop from ranking.
		- (c > 0) to avoid grant Novice Skill Tree in case of Skill Reset (need more logic)
		- (sd->status.skill_point == 0) to wait until all skill points are assigned to avoid problems with Job Change quest. */
		for( i = 0; i < MAX_SKILL_TREE && (id = skill_tree[c][i].id) > 0; i++ ) {
			if( (skill_get_inf2(id)&(INF2_QUEST_SKILL|INF2_WEDDING_SKILL)) )
				continue; //Do not include Quest/Wedding skills.
			if( sd->status.skill[id].id == 0 ) {
				sd->status.skill[id].id = id;
				sd->status.skill[id].flag = SKILL_FLAG_TEMPORARY; //So it is not saved, and tagged as a "bonus" skill.
			} else if( id != NV_BASIC )
				sd->status.skill[id].flag = SKILL_FLAG_REPLACED_LV_0 + sd->status.skill[id].lv; //Remember original level
			sd->status.skill[id].lv = skill_tree_get_max(id, sd->status.class_);
		}
	}
}

//Checks if you can learn a new skill after having leveled up a skill.
static void pc_check_skilltree(struct map_session_data *sd, int skill)
{
	int i, flag;
	int c = 0;

	if( battle_config.skillfree )
		return; //Function serves no purpose if this is set

	i = pc_calc_skilltree_normalize_job(sd);
	c = pc_mapid2jobid(i, sd->status.sex);
	if( c == -1 ) { //Unable to normalize job??
		ShowError("pc_check_skilltree: Unable to normalize job %d for character %s (%d:%d)\n", i, sd->status.name, sd->status.account_id, sd->status.char_id);
		return;
	}
	c = pc_class2idx(c);
	do {
		short id = 0;

		flag = 0;
		for( i = 0; i < MAX_SKILL_TREE && (id = skill_tree[c][i].id) > 0; i++ ) {
			int j, f = 1;

			if( sd->status.skill[id].id ) //Already learned
				continue;
			for( j = 0; j < MAX_PC_SKILL_REQUIRE; j++ ) {
				int k = skill_tree[c][i].need[j].id;

				if( k != 0 ) {
					if( sd->status.skill[k].id == 0 || sd->status.skill[k].flag == SKILL_FLAG_TEMPORARY || sd->status.skill[k].flag == SKILL_FLAG_PLAGIARIZED )
						k = 0; //Not learned
					else if( sd->status.skill[k].flag >= SKILL_FLAG_REPLACED_LV_0) //Real lerned level
						k = sd->status.skill[skill_tree[c][i].need[j].id].flag - SKILL_FLAG_REPLACED_LV_0;
					else
						k = pc_checkskill(sd,k);
					if( k < skill_tree[c][i].need[j].lv ) {
						f = 0;
						break;
					}
				}
			}
			if( !f )
				continue;
			if( sd->status.base_level < skill_tree[c][i].baselv || sd->status.job_level < skill_tree[c][i].joblv )
				continue;
			j = skill_get_inf2(id);
			if( !sd->status.skill[id].lv &&
				((j&INF2_QUEST_SKILL && !battle_config.quest_skill_learn) ||
				j&INF2_WEDDING_SKILL ||
				(j&INF2_SPIRIT_SKILL && !sd->sc.data[SC_SPIRIT])) )
				continue; //Cannot be learned via normal means
			sd->status.skill[id].id = id;
			flag = 1;
		}
	} while( flag );
}

// Make sure all the skills are in the correct condition
// before persisting to the backend. [MouseJstr]
void pc_clean_skilltree(struct map_session_data *sd)
{
	uint16 i;

	for (i = 0; i < MAX_SKILL; i++) {
		if (sd->status.skill[i].flag == SKILL_FLAG_TEMPORARY || sd->status.skill[i].flag == SKILL_FLAG_PLAGIARIZED) {
			sd->status.skill[i].id = 0;
			sd->status.skill[i].lv = 0;
			sd->status.skill[i].flag = SKILL_FLAG_PERMANENT;
		} else if (sd->status.skill[i].flag == SKILL_FLAG_REPLACED_LV_0) {
			sd->status.skill[i].lv = sd->status.skill[i].flag - SKILL_FLAG_REPLACED_LV_0;
			sd->status.skill[i].flag = SKILL_FLAG_PERMANENT;
		}
	}
}

int pc_calc_skilltree_normalize_job(struct map_session_data *sd)
{
	int skill_point, novice_skills;
	int c = sd->class_;

	if (!battle_config.skillup_limit || pc_has_permission(sd, PC_PERM_ALL_SKILL))
		return c;

	skill_point = pc_calc_skillpoint(sd);
	novice_skills = job_info[pc_class2idx(JOB_NOVICE)].max_level[1] - 1;

	//Limit 1st class and above to novice job levels
	if (skill_point < novice_skills && (sd->class_&MAPID_BASEMASK) != MAPID_SUMMONER)
		c = MAPID_NOVICE;
	//Limit 2nd class and above to first class job levels (super novices are exempt)
	else if ((sd->class_&JOBL_2) && (sd->class_&MAPID_UPPERMASK) != MAPID_SUPER_NOVICE) {
		//Regenerate change_level_2nd
		if (!sd->change_level_2nd) {
			if (sd->class_&JOBL_THIRD) {
				//If neither 2nd nor 3rd jobchange levels are known, we have to assume a default for 2nd
				if (!sd->change_level_3rd)
					sd->change_level_2nd = job_info[pc_class2idx(pc_mapid2jobid(sd->class_&MAPID_UPPERMASK, sd->status.sex))].max_level[1];
				else
					sd->change_level_2nd = 1 + skill_point + sd->status.skill_point
						- (sd->status.job_level - 1)
						- (sd->change_level_3rd - 1)
						- novice_skills;
			} else {
				sd->change_level_2nd = 1 + skill_point + sd->status.skill_point
						- (sd->status.job_level - 1)
						- novice_skills;
			}
			pc_setglobalreg (sd, "jobchange_level", sd->change_level_2nd);
		}
		if (skill_point < novice_skills + (sd->change_level_2nd - 1))
			c &= MAPID_BASEMASK;
		//Limit 3rd class to 2nd class/trans job levels
		else if (sd->class_&JOBL_THIRD) {
			//Regenerate change_level_3rd
			if (!sd->change_level_3rd) {
					sd->change_level_3rd = 1 + skill_point + sd->status.skill_point
						- (sd->status.job_level - 1)
						- (sd->change_level_2nd - 1)
						- novice_skills;
					pc_setglobalreg (sd, "jobchange_level_3rd", sd->change_level_3rd);
			}
			if (skill_point < novice_skills + (sd->change_level_2nd - 1) + (sd->change_level_3rd - 1))
				c &= MAPID_UPPERMASK;
		}
	}

	//Restore non-limiting flags
	c |= sd->class_&(JOBL_UPPER|JOBL_BABY);

	return c;
}

/*==========================================
 * Updates the weight status
 *------------------------------------------
 * 1: overweight 50% for pre-renewal and 70% for renewal
 * 2: overweight 90%
 * It's assumed that SC_WEIGHT50 and SC_WEIGHT90 are only started/stopped here.
 */
void pc_updateweightstatus(struct map_session_data *sd)
{
	int old_overweight;
	int new_overweight;

	nullpo_retv(sd);

	old_overweight = (sd->sc.data[SC_WEIGHT90] ? 2 : (sd->sc.data[SC_WEIGHT50] ? 1 : 0));
#ifdef RENEWAL
	new_overweight = (pc_is90overweight(sd) ? 2 : (pc_is70overweight(sd) ? 1 : 0));
#else
	new_overweight = (pc_is90overweight(sd) ? 2 : (pc_is50overweight(sd) ? 1 : 0));
#endif

	if( old_overweight == new_overweight )
		return; //No change

	//Stop old status change
	if( old_overweight == 1 )
		status_change_end(&sd->bl, SC_WEIGHT50, INVALID_TIMER);
	else if( old_overweight == 2 )
		status_change_end(&sd->bl, SC_WEIGHT90, INVALID_TIMER);

	//Start new status change
	if( new_overweight == 1 )
		sc_start(&sd->bl, &sd->bl, SC_WEIGHT50, 100, 0, 0);
	else if( new_overweight == 2 )
		sc_start(&sd->bl, &sd->bl, SC_WEIGHT90, 100, 0, 0);

	//Update overweight status
	sd->regen.state.overweight = new_overweight;
}

int pc_disguise(struct map_session_data *sd, int class_)
{
	if (!class_ && !sd->disguise)
		return 0;
	if (class_ && sd->disguise == class_)
		return 0;
	if (pc_isinvisible(sd)) { //Character is invisible, stealth class-change [Skotlex]
		sd->disguise = class_; //Viewdata is set on uncloaking
		return 2;
	}
	if (sd->bl.prev != NULL) {
		pc_stop_walking(sd, USW_NONE);
		clif_clearunit_area(&sd->bl, CLR_OUTSIGHT);
	}
	if (!class_) {
		sd->disguise = 0;
		class_ = sd->status.class_;
	} else
		sd->disguise = class_;

	status_set_viewdata(&sd->bl, class_);
	clif_changeoption(&sd->bl);

	//We need to update the client so it knows that a costume is being used
	if (sd->sc.option&OPTION_COSTUME) {
		clif_changelook(&sd->bl, LOOK_BASE, sd->vd.class_);
		clif_changelook(&sd->bl, LOOK_WEAPON, 0);
		clif_changelook(&sd->bl, LOOK_SHIELD, 0);
		clif_changelook(&sd->bl, LOOK_CLOTHES_COLOR, sd->vd.cloth_color);
	}
	if (sd->bl.prev != NULL) {
		clif_spawn(&sd->bl);
		if (class_ == sd->status.class_ && pc_iscarton(sd)) { //It seems the cart info is lost on undisguise
			clif_cartlist(sd);
			clif_updatestatus(sd, SP_CARTINFO);
		}
		if (sd->chatID) {
			struct chat_data *cd = (struct chat_data *)map_id2bl(sd->chatID);

			if (cd)
				clif_dispchat(cd, 0);
		}
	}
	return 1;
}

//Show error message
#define PC_BONUS_SHOW_ERROR(type,type2,val) { ShowError("%s: %s: Invalid %s %d.\n",__FUNCTION__,#type,#type2,(val)); break; }
//Check for valid Element, break & show error message if invalid Element
#define PC_BONUS_CHK_ELEMENT(ele,bonus) { if (!CHK_ELEMENT((ele))) { PC_BONUS_SHOW_ERROR((bonus),Element,(ele)); }}
//Check for valid Race, break & show error message if invalid Race
#define PC_BONUS_CHK_RACE(rc,bonus) { if (!CHK_RACE((rc))) { PC_BONUS_SHOW_ERROR((bonus),Race,(rc)); }}
//Check for valid Race2, break & show error message if invalid Race2
#define PC_BONUS_CHK_RACE2(rc2,bonus) { if (!CHK_RACE2((rc2))) { PC_BONUS_SHOW_ERROR((bonus),Race2,(rc2)); }}
//Check for valid Class, break & show error message if invalid Class
#define PC_BONUS_CHK_CLASS(cl,bonus) { if (!CHK_CLASS((cl))) { PC_BONUS_SHOW_ERROR((bonus),Class,(cl)); }}
//Check for valid Size, break & show error message if invalid Size
#define PC_BONUS_CHK_SIZE(sz,bonus) { if (!CHK_MOBSIZE((sz))) { PC_BONUS_SHOW_ERROR((bonus),Size,(sz)); }}
// Check for valid SC, break & show error message if invalid SC
#define PC_BONUS_CHK_SC(sc,bonus) { if ((sc) <= SC_NONE || (sc) >= SC_MAX) { PC_BONUS_SHOW_ERROR((bonus),Effect,(sc)); }}

/**
 * Add auto spell bonus for player while attacking/attacked
 * @param spell: Spell array
 * @param id: Skill to cast
 * @param lv: Skill level
 * @param rate: Success chance
 * @param flag: Battle flag
 * @param card_id: Used to prevent card stacking
 */
static void pc_bonus_autospell(struct s_autospell *spell, short id, short lv, short rate, short flag, unsigned short card_id)
{
	uint8 i;

	if( !rate )
		return;
	if( !(flag&BF_RANGEMASK) )
		flag |= BF_SHORT|BF_LONG; //No range defined? Use both
	if( !(flag&BF_WEAPONMASK) )
		flag |= BF_WEAPON; //No attack type defined? Use weapon
	if( !(flag&BF_SKILLMASK) ) {
		if( flag&(BF_MAGIC|BF_MISC) )
			flag |= BF_SKILL; //These two would never trigger without BF_SKILL
		if( flag&BF_WEAPON )
			flag |= BF_NORMAL; //By default autospells should only trigger on normal weapon attacks
	}
	for( i = 0; i < MAX_PC_BONUS && spell[i].id; i++ ) {
		if( (spell[i].card_id == card_id || spell[i].rate < 0 || rate < 0) && spell[i].id == id && spell[i].lv == lv && spell[i].flag == flag ) {
			if( !battle_config.autospell_stacking && spell[i].rate > 0 && rate > 0 )
				return;
			spell[i].rate = cap_value(spell[i].rate + rate, -1000, 1000);
			return;
		}
	}
	if( i == MAX_PC_BONUS ) {
		ShowWarning("pc_bonus_autospell: Reached max (%d) number of autospells per character!\n", MAX_PC_BONUS);
		return;
	}
	if( rate < -1000 || rate > 1000 )
		ShowWarning("pc_bonus_autospell: Item bonus rate %d exceeds -1000~1000 range, capping.\n", rate);
	spell[i].id = id;
	spell[i].lv = lv;
	spell[i].rate = cap_value(rate, -1000, 1000);
	spell[i].flag = flag;
	spell[i].card_id = card_id;
}

/**
 * Add auto spell bonus for player while using skills
 * @param spell: Spell array
 * @param src_skill: Trigger skill
 * @param id: Support or target type
 * @param lv: Skill level
 * @param rate: Success chance
 * @param card_id: Used to prevent card stacking
 */
static void pc_bonus_autospell_onskill(struct s_autospell *spell, short src_skill, short id, short lv, short rate, unsigned short card_id)
{
	uint8 i;

	if( !rate )
		return;
	for( i = 0; i < MAX_PC_BONUS && spell[i].id; i++ ) {
		if( (spell[i].card_id == card_id || spell[i].rate < 0 || rate < 0) && spell[i].flag == src_skill && spell[i].id == id && spell[i].lv == lv ) {
			if( !battle_config.autospell_stacking && spell[i].rate > 0 && rate > 0 )
				return;
			spell[i].rate = cap_value(spell[i].rate + rate, -1000, 1000);
			return;
		}
	}
	if( i == MAX_PC_BONUS ) {
		ShowWarning("pc_bonus_autospell_onskill: Reached max (%d) number of autospells per character!\n", MAX_PC_BONUS);
		return;
	}
	if( rate < -1000 || rate > 1000 )
		ShowWarning("pc_bonus_autospell_onskill: Item bonus rate %d exceeds -1000~1000 range, capping.\n", rate);
	spell[i].flag = src_skill;
	spell[i].id	= id;
	spell[i].lv = lv;
	spell[i].rate = cap_value(rate, -1000, 1000);
	spell[i].card_id = card_id;
}

/**
 * Add inflict effect bonus for player while attacking/attacked
 * @param effect: Effect array
 * @param sc: SC/Effect type
 * @param rate: Success chance
 * @param arrow_rate: success chance if bonus comes from arrow-type item
 * @param flag: Target flag
 * @param duration: Duration. If 0 use default duration lookup for associated skill with level 7
 */
static void pc_bonus_addeff(struct s_addeffect *effect, enum sc_type sc, short rate, short arrow_rate, unsigned char flag, unsigned int duration)
{
	uint8 i;

	if( !(flag&(ATF_SHORT|ATF_LONG)) )
		flag |= ATF_SHORT|ATF_LONG; //Default range: both
	if( !(flag&(ATF_TARGET|ATF_SELF)) )
		flag |= ATF_TARGET; //Default target: enemy
	if( !(flag&(ATF_WEAPON|ATF_MAGIC|ATF_MISC)) )
		flag |= ATF_WEAPON; //Default type: weapon
	if( !duration )
		duration = (unsigned int)skill_get_time2(status_sc2skill(sc), 7);
	for( i = 0; i < MAX_PC_BONUS && effect[i].flag; i++ ) {
		if( effect[i].sc == sc && effect[i].flag == flag ) {
			effect[i].rate = cap_value(effect[i].rate + rate, -10000, 10000);
			effect[i].arrow_rate = cap_value(effect[i].arrow_rate + arrow_rate, -10000, 10000);
			effect[i].duration = umax(effect[i].duration, duration);
			return;
		}
	}
	if( i == MAX_PC_BONUS ) {
		ShowWarning("pc_bonus_addeff: Reached max (%d) number of add effects per character!\n", MAX_PC_BONUS);
		return;
	}
	if( rate < -10000 || rate > 10000 || arrow_rate < -10000 || arrow_rate > 10000 )
		ShowWarning("pc_bonus_addeff: Item bonus rate %d exceeds -10000~10000 range, capping.\n", rate);
	effect[i].sc = sc;
	effect[i].rate = cap_value(rate, -10000, 10000);
	effect[i].arrow_rate = cap_value(arrow_rate, -10000, 10000);
	effect[i].flag = flag;
	effect[i].duration = duration;
}

/**
 * Add inflict effect bonus for player while attacking using skill
 * @param effect: Effect array
 * @param sc: SC/Effect type
 * @param rate: Success chance
 * @param skill_id: Skill to cast
 * @param target: Target type
 * @param duration: Duration. If 0 use default duration lookup for associated skill with level 7
 */
static void pc_bonus_addeff_onskill(struct s_addeffectonskill *effect, enum sc_type sc, short rate, uint16 skill_id, unsigned char target, unsigned int duration)
{
	uint8 i;

	if( !duration )
		duration = (unsigned int)skill_get_time2(status_sc2skill(sc), 7);
	for( i = 0; i < MAX_PC_BONUS && effect[i].skill_id; i++ ) {
		if( effect[i].sc == sc && effect[i].skill_id == skill_id && effect[i].target == target ) {
			effect[i].rate = cap_value(effect[i].rate + rate, -10000, 10000);
			effect[i].duration = umax(effect[i].duration, duration);
			return;
		}
	}
	if( i == MAX_PC_BONUS ) {
		ShowWarning("pc_bonus_addeff_onskill: Reached max (%d) number of add effects on skill per character!\n", MAX_PC_BONUS);
		return;
	}
	if( rate < -10000 || rate > 10000 )
		ShowWarning("pc_bonus_addeff_onskill: Item bonus rate %d exceeds -10000~10000 range, capping.\n", rate);
	effect[i].sc = sc;
	effect[i].rate = cap_value(rate, -10000, 10000);
	effect[i].skill_id = skill_id;
	effect[i].target = target;
	effect[i].duration = duration;
}

/** Adjust/add drop rate modifier for player
 * @param drop: Player's sd->add_drop (struct s_add_drop)
 * @param max: Max bonus can be received
 * @param nameid: item id that will be dropped
 * @param group: group id
 * @param class_: target class
 * @param race: target race. if < 0, means monster_id
 * @param rate: rate value: 1 ~ 10000. If < 0, it will be multiplied with mob level/10
 */
static void pc_bonus_item_drop(struct s_add_drop *drop, unsigned short nameid, uint16 group, int class_, short race, short rate)
{
	uint8 i;

	if( !nameid && !group ) {
		ShowWarning("pc_bonus_item_drop: No Item ID nor Item Group ID specified.\n");
		return;
	}
	if( nameid && !itemdb_exists(nameid) ) {
		ShowWarning("pc_bonus_item_drop: Invalid item id %hu\n", nameid);
		return;
	}
	if( group && !itemdb_group_exists(group) ) {
		ShowWarning("pc_bonus_item_drop: Invalid item group %hu\n", group);
		return;
	}
	//Apply config rate adjustment settings
	if( rate >= 0 ) { //Absolute drop
		if( battle_config.item_rate_adddrop != 100 )
			rate = rate * battle_config.item_rate_adddrop / 100;
		if( rate < battle_config.item_drop_adddrop_min )
			rate = battle_config.item_drop_adddrop_min;
		else if( rate > battle_config.item_drop_adddrop_max )
			rate = battle_config.item_drop_adddrop_max;
	} else { //Relative drop, max/min limits are applied at drop time
		if( battle_config.item_rate_adddrop != 100 )
			rate = rate * battle_config.item_rate_adddrop / 100;
		if( rate > -1 )
			rate = -1;
	}
	for( i = 0; i < MAX_PC_BONUS && &drop[i] && (drop[i].nameid || drop[i].group); i++ ) {
		//Find match entry, and adjust the rate only
		if( drop[i].nameid == nameid && drop[i].group == group && drop[i].race == race && drop[i].class_ == class_ ) {
			//Adjust the rate if it has same classification
			if( (rate < 0 && drop[i].rate < 0) || (rate > 0 && drop[i].rate > 0) ) {
				drop[i].rate = cap_value(drop[i].rate + rate, -10000, 10000);
				return;
			}
		}
	}
	if( i == MAX_PC_BONUS ) {
		ShowWarning("pc_bonus_item_drop: Reached max (%d) number of added drops per character! (nameid:%hu group:%d class_:%d race:%d rate:%d)\n", MAX_PC_BONUS, nameid, group, class_, race, rate);
		return;
	}
	if( rate < -10000 || rate > 10000 )
		ShowWarning("pc_bonus_item_drop: Item bonus rate %d exceeds -10000~10000 range, capping.\n", rate);
	drop[i].nameid = nameid;
	drop[i].group = group;
	drop[i].race = race;
	drop[i].class_ = class_;
	drop[i].rate = cap_value(rate, -10000, 10000);
}

/**
 * Add autobonus to player when attacking/attacked
 * @param bonus: Bonus array
 * @param script: Script to execute
 * @param rate: Success chance
 * @param dur: Duration
 * @param flag: Battle flag/skill
 * @param other_script: Secondary script to execute
 * @param pos: Item equip position
 * @param onskill: Skill used to trigger autobonus
 * @return True on success or false otherwise
 */
bool pc_addautobonus(struct s_autobonus *bonus, const char *script, short rate, unsigned int dur, short atk_type, const char *other_script, unsigned int pos, bool onskill)
{
	uint8 i;

	ARR_FIND(0, MAX_PC_BONUS, i, bonus[i].rate == 0); //Check for free slot
	if( i == MAX_PC_BONUS ) { //No free slot
		ShowWarning("pc_addautobonus: Reached max (%d) number of autobonus per character!\n", MAX_PC_BONUS);
		return false;
	}
	if( !onskill ) {
		if( !(atk_type&BF_RANGEMASK) )
			atk_type |= BF_SHORT|BF_LONG; //No range defined? Use both
		if( !(atk_type&BF_WEAPONMASK) )
			atk_type |= BF_WEAPON; //No attack type defined? Use weapon
		if( !(atk_type&BF_SKILLMASK) ) {
			if( atk_type&(BF_MAGIC|BF_MISC) )
				atk_type |= BF_SKILL; //These two would never trigger without BF_SKILL
			if( atk_type&BF_WEAPON )
				atk_type |= BF_NORMAL|BF_SKILL;
		}
	}
	if( rate < -1000 || rate > 1000 )
		ShowWarning("pc_addautobonus: Item bonus rate %d exceeds -1000~1000 range, capping.\n", rate);
	bonus[i].rate = cap_value(rate, -1000, 1000);
	bonus[i].duration = dur;
	bonus[i].active = INVALID_TIMER;
	bonus[i].atk_type = atk_type;
	bonus[i].pos = pos;
	bonus[i].bonus_script = aStrdup(script);
	bonus[i].other_script = (other_script ? aStrdup(other_script) : NULL);
	return true;
}

/**
 * Remove an autobonus from player
 * @param sd: Player data
 * @param bonus: Autobonus array
 * @param restore: Run script or clear it
 */
void pc_delautobonus(struct map_session_data *sd, struct s_autobonus *autobonus, bool restore)
{
	uint8 i;

	nullpo_retv(sd);

	for( i = 0; i < MAX_PC_BONUS; i++ ) {
		if( restore && (sd->state.autobonus&autobonus[i].pos) == autobonus[i].pos ) {
			if( autobonus[i].active == INVALID_TIMER )
				continue;
			if( autobonus[i].bonus_script ) {
				int j;
				unsigned int equip_pos = 0;

				//Create a list of all equipped positions to see if all items needed for the autobonus are still present [Playtester]
				for( j = 0; j < EQI_MAX; j++ ) {
					if( sd->equip_index[j] >= 0 )
						equip_pos |= sd->inventory.u.items_inventory[sd->equip_index[j]].equip;
					else if( j == EQI_PET && sd->status.pet_id > 0 && sd->pd )
						equip_pos |= EQP_PET;
				}
				if( (equip_pos&autobonus[i].pos) == autobonus[i].pos )
					script_run_autobonus(autobonus[i].bonus_script,sd,autobonus[i].pos);
			}
			continue;
		}
		if( autobonus[i].bonus_script )
			aFree(autobonus[i].bonus_script);
		if( autobonus[i].other_script )
			aFree(autobonus[i].other_script);
		autobonus[i].bonus_script = autobonus[i].other_script = NULL;
		autobonus[i].rate = autobonus[i].atk_type = autobonus[i].duration = autobonus[i].pos = 0;
		autobonus[i].active = INVALID_TIMER;
	}
}

/**
 * Execute autobonus on player
 * @param sd: Player data
 * @param autobonus: Autobonus to run
 * @param onskill: Skill used to trigger autobonus
 */
void pc_exeautobonus(struct map_session_data *sd, struct s_autobonus *autobonus, short atk_type, bool onskill)
{
	uint8 i;

	nullpo_retv(sd);

	if( !autobonus[0].rate )
		return;

	for( i = 0; i < MAX_PC_BONUS && autobonus[i].rate; i++ ) {
		if( !onskill ) {
			if( !(((autobonus[i].atk_type)&atk_type)&BF_WEAPONMASK &&
				 ((autobonus[i].atk_type)&atk_type)&BF_RANGEMASK &&
				 ((autobonus[i].atk_type)&atk_type)&BF_SKILLMASK) )
				continue; //One or more trigger conditions were not fulfilled
		} else {
			if( autobonus[i].atk_type != atk_type )
				continue;
		}
		if( rnd()%1000 >= autobonus[i].rate )
			continue;
		if( autobonus[i].active != INVALID_TIMER )
			delete_timer(autobonus[i].active,pc_endautobonus);
		if( autobonus[i].other_script ) {
			int j;
			unsigned int equip_pos = 0;

			//Create a list of all equipped positions to see if all items needed for the autobonus are still present [Playtester]
			for( j = 0; j < EQI_MAX; j++ ) {
				if( sd->equip_index[j] >= 0 )
					equip_pos |= sd->inventory.u.items_inventory[sd->equip_index[j]].equip;
				else if( j == EQI_PET && sd->status.pet_id > 0 && sd->pd )
					equip_pos |= EQP_PET;
			}
			if( (equip_pos&autobonus[i].pos) == autobonus[i].pos )
				script_run_autobonus(autobonus[i].other_script,sd,autobonus[i].pos);
		}
		autobonus[i].active = add_timer(gettick() + autobonus[i].duration,pc_endautobonus,sd->bl.id,(intptr_t)&autobonus[i]);
		sd->state.autobonus |= autobonus[i].pos;
	}
	status_calc_pc(sd,SCO_FORCE);
}

/**
 * Remove autobonus timer from player
 */
TIMER_FUNC(pc_endautobonus)
{
	struct map_session_data *sd = map_id2sd(id);
	struct s_autobonus *autobonus = (struct s_autobonus *)data;

	nullpo_ret(sd);
	nullpo_ret(autobonus);

	autobonus->active = INVALID_TIMER;
	sd->state.autobonus &= ~autobonus->pos;
	status_calc_pc(sd,SCO_FORCE);
	return 0;
}

/**
 * Add damage to player against monster defense element when attacking
 * @param sd: Player data
 * @param ele: Element to adjust
 * @param rate: Success chance
 * @param flag: Battle flag
 */
static void pc_bonus_adddefele(struct s_item_bonus_ele2 *bonus, unsigned char ele, short rate, short flag)
{
	uint8 i;

	if( !(flag&BF_RANGEMASK) )
		flag |= BF_SHORT|BF_LONG;
	if( !(flag&BF_WEAPONMASK) )
		flag |= BF_WEAPON;
	if( !(flag&BF_SKILLMASK) ) {
		if( flag&(BF_MAGIC|BF_MISC) )
			flag |= BF_SKILL;
		if( flag&BF_WEAPON )
			flag |= BF_NORMAL|BF_SKILL;
	}
	for( i = 0; i < MAX_PC_BONUS && bonus[i].rate; i++ ) {
		if( bonus[i].ele == ele && bonus[i].flag == flag ) {
			bonus[i].rate += rate;
			return;
		}
	}
	if( i == MAX_PC_BONUS ) {
		ShowWarning("pc_bonus_adddefele: Reached max (%d) possible bonuses for this player.\n", MAX_PC_BONUS);
		return;
	}
	bonus[i].ele = ele;
	bonus[i].rate = rate;
	bonus[i].flag = flag;
}

/**
 * Reduce damage to player against element when attacked
 * @param sd: Player data
 * @param ele: Element to adjust
 * @param rate: Success chance
 * @param flag: Battle flag
 */
static void pc_bonus_subele(struct s_item_bonus_ele2 *bonus, unsigned char ele, short rate, short flag)
{
	uint8 i;

	if( !(flag&BF_RANGEMASK) )
		flag |= BF_SHORT|BF_LONG;
	if( !(flag&BF_WEAPONMASK) )
		flag |= BF_WEAPON;
	if( !(flag&BF_SKILLMASK) ) {
		if( flag&(BF_MAGIC|BF_MISC) )
			flag |= BF_SKILL;
		if( flag&BF_WEAPON )
			flag |= BF_NORMAL|BF_SKILL;
	}
	for( i = 0; i < MAX_PC_BONUS && bonus[i].rate; i++ ) {
		if( bonus[i].ele == ele && bonus[i].flag == flag ) {
			bonus[i].rate += rate;
			return;
		}
	}
	if( i == MAX_PC_BONUS ) {
		ShowWarning("pc_bonus_subele: Reached max (%d) possible bonuses for this player.\n", MAX_PC_BONUS);
		return;
	}
	bonus[i].ele = ele;
	bonus[i].rate = rate;
	bonus[i].flag = flag;
}

/**
 * General item bonus for player
 * @param bonus: Bonus array
 * @param type: Bonus type used by bBonusName
 * @param id: Key
 * @param val: Value
 * @param cap_rate: If Value is a rate value that needs to be capped
 */
static void pc_bonus_itembonus(struct s_item_bonus *bonus, int type, uint16 id, int val, bool cap_rate)
{
	uint8 i;

	for( i = 0; i < MAX_PC_BONUS && bonus[i].val; i++ ) {
		if( bonus[i].id == id ) {
			if( cap_rate )
				bonus[i].val = cap_value(bonus[i].val + val, -10000, 10000);
			else
				bonus[i].val += val;
			return;
		}
	}
	if( i == MAX_PC_BONUS ) {
		ShowWarning("pc_bonus_itembonus: Type (%d) reached max (%d) possible bonuses for this player.\n", type, MAX_PC_BONUS);
		return;
	}
	if( cap_rate && (val < -10000 || val > 10000) ) {
		ShowWarning("pc_bonus_itembonus: Item bonus type %d val %d exceeds -10000~10000 range, capping.\n", type, val);
		val = cap_value(val, -10000, 10000);
	}
	bonus[i].id = id;
	bonus[i].val = val;
}

/**
 * Add a bonus(type) to player sd
 * format: bonus bBonusName,val;
 * @param sd
 * @param type Bonus type used by bBonusName
 * @param val Value that usually for rate or fixed value
 */
void pc_bonus(struct map_session_data *sd, int type, int val)
{
	struct status_data *status = NULL;
	int bonus;

	nullpo_retv(sd);

	status = &sd->base_status;

	switch(type) {
		case SP_STR:
		case SP_AGI:
		case SP_VIT:
		case SP_INT:
		case SP_DEX:
		case SP_LUK:
			if(sd->state.lr_flag != 2)
				sd->param_bonus[type - SP_STR] += val;
			break;
		case SP_ATK1:
			if(!sd->state.lr_flag) {
				bonus = status->rhw.atk + val;
				status->rhw.atk = cap_value(bonus, 0, USHRT_MAX);
			} else if(sd->state.lr_flag == 1) {
				bonus = status->lhw.atk + val;
				status->lhw.atk = cap_value(bonus, 0, USHRT_MAX);
			}
			break;
		case SP_ATK2:
			if(!sd->state.lr_flag) {
				bonus = status->rhw.atk2 + val;
				status->rhw.atk2 = cap_value(bonus, 0, USHRT_MAX);
			} else if(sd->state.lr_flag == 1) {
				bonus = status->lhw.atk2 + val;
				status->lhw.atk2 = cap_value(bonus, 0, USHRT_MAX);
			}
			break;
		case SP_BASE_ATK:
			if(sd->state.lr_flag != 2) {
#ifdef RENEWAL
				bonus = sd->bonus.eatk + val;
				sd->bonus.eatk = cap_value(bonus, SHRT_MIN, SHRT_MAX);
#else
				bonus = status->batk + val;
				status->batk = cap_value(bonus, 0, USHRT_MAX);
#endif
			}
			break;
		case SP_DEF1:
			if(sd->state.lr_flag != 2) {
				bonus = status->def + val;
#ifdef RENEWAL
				status->def = cap_value(bonus, SHRT_MIN, SHRT_MAX);
#else
				status->def = cap_value(bonus, CHAR_MIN, CHAR_MAX);
#endif
			}
			break;
		case SP_DEF2:
			if(sd->state.lr_flag != 2) {
				bonus = status->def2 + val;
				status->def2 = cap_value(bonus, SHRT_MIN, SHRT_MAX);
			}
			break;
		case SP_MDEF1:
			if(sd->state.lr_flag != 2) {
				bonus = status->mdef + val;
#ifdef RENEWAL
				status->mdef = cap_value(bonus, SHRT_MIN, SHRT_MAX);
#else
				status->mdef = cap_value(bonus, CHAR_MIN, CHAR_MAX);
#endif
				if(sd->state.lr_flag == 3)
					sd->bonus.shieldmdef += bonus;  //For Royal Guard
			}
			break;
		case SP_MDEF2:
			if(sd->state.lr_flag != 2) {
				bonus = status->mdef2 + val;
				status->mdef2 = cap_value(bonus, SHRT_MIN, SHRT_MAX);
			}
			break;
		case SP_HIT:
			if(sd->state.lr_flag != 2) {
				bonus = status->hit + val;
				status->hit = cap_value(bonus, SHRT_MIN, SHRT_MAX);
			} else
				sd->bonus.arrow_hit += val;
			break;
		case SP_FLEE1:
			if(sd->state.lr_flag != 2) {
				bonus = status->flee + val;
				status->flee = cap_value(bonus, SHRT_MIN, SHRT_MAX);
			}
			break;
		case SP_FLEE2:
			if(sd->state.lr_flag != 2) {
				bonus = status->flee2 + val * 10;
				status->flee2 = cap_value(bonus, SHRT_MIN, SHRT_MAX);
			}
			break;
		case SP_CRITICAL:
			if(sd->state.lr_flag != 2) {
				bonus = status->cri + val * 10;
				status->cri = cap_value(bonus, SHRT_MIN, SHRT_MAX);
			} else
				sd->bonus.arrow_cri += val * 10;
			break;
		case SP_ATKELE:
			PC_BONUS_CHK_ELEMENT(val, SP_ATKELE);
			switch(sd->state.lr_flag) {
				case 2:
					switch(sd->status.weapon) {
						case W_BOW:	case W_REVOLVER:
						case W_RIFLE:	case W_GATLING:
						case W_SHOTGUN:	case W_GRENADE:
							status->rhw.ele = val; //Become weapon element
							break;
						default:
							sd->bonus.arrow_ele = val; //Become ammo element
							break;
					}
					break;
				case 1:
					status->lhw.ele = val;
					break;
				default:
					status->rhw.ele = val;
					break;
			}
			break;
		case SP_DEFELE:
			PC_BONUS_CHK_ELEMENT(val, SP_DEFELE);
			if(sd->state.lr_flag != 2)
				status->def_ele = val;
			break;
		case SP_MAXHP:
			if(sd->state.lr_flag != 2)
				sd->bonus.hp += val;
			break;
		case SP_MAXSP:
			if(sd->state.lr_flag != 2)
				sd->bonus.sp += val;
			break;
		case SP_MAXHPRATE:
			if(sd->state.lr_flag != 2)
				sd->hprate += val;
			break;
		case SP_MAXSPRATE:
			if(sd->state.lr_flag != 2)
				sd->sprate += val;
			break;
		case SP_SPRATE:
			if(sd->state.lr_flag != 2)
				sd->dsprate += val;
			break;
		case SP_ATTACKRANGE:
			switch(sd->state.lr_flag) {
				case 2:
					switch(sd->status.weapon) {
						case W_BOW:	case W_REVOLVER:
						case W_RIFLE:	case W_GATLING:
						case W_SHOTGUN:	case W_GRENADE:
							status->rhw.range += val;
							break;
					}
					break;
				case 1:
					status->lhw.range += val;
					break;
				default:
					status->rhw.range += val;
					break;
			}
			break;
		case SP_SPEED_RATE:	//Non stackable increase
			if(sd->state.lr_flag != 2) {
				sd->bonus.speed_rate = min(sd->bonus.speed_rate, -val);
				if(sd->bonus.speed_rate < 0)
					clif_status_load(&sd->bl, SI_MOVHASTE_INFINITY, 1);
			}
			break;
		case SP_SPEED_ADDRATE: //Stackable increase
			if(sd->state.lr_flag != 2)
				sd->bonus.speed_add_rate -= val;
			break;
		case SP_ASPD: //Raw increase
			if(sd->state.lr_flag != 2)
				sd->bonus.aspd_add -= 10 * val;
			break;
		case SP_ASPD_RATE: //Stackable increase - Made it linear as per rodatazone
			if(sd->state.lr_flag != 2) {
#ifndef RENEWAL_ASPD
				status->aspd_rate -= 10 * val;
#else
				status->aspd_rate2 += val;
#endif
			}
			break;
		case SP_HP_RECOV_RATE:
			if(sd->state.lr_flag != 2)
				sd->hprecov_rate += val;
			break;
		case SP_SP_RECOV_RATE:
			if(sd->state.lr_flag != 2)
				sd->sprecov_rate += val;
			break;
		case SP_CRITICAL_DEF:
			if(sd->state.lr_flag != 2)
				sd->bonus.critical_def += val;
			break;
		case SP_NEAR_ATK_DEF:
			if(sd->state.lr_flag != 2)
				sd->bonus.near_attack_def_rate += val;
			break;
		case SP_LONG_ATK_DEF:
			if(sd->state.lr_flag != 2)
				sd->bonus.long_attack_def_rate += val;
			break;
		case SP_DOUBLE_RATE:
			if(!sd->state.lr_flag)
				sd->bonus.double_rate = max(sd->bonus.double_rate, val);
			break;
		case SP_DOUBLE_ADD_RATE:
			if(!sd->state.lr_flag)
				sd->bonus.double_add_rate += val;
			break;
		case SP_MATK_RATE:
			if(sd->state.lr_flag != 2)
				sd->matk_rate += val;
			break;
		case SP_IGNORE_DEF_ELE:
			PC_BONUS_CHK_ELEMENT(val, SP_IGNORE_DEF_ELE);
			if(!sd->state.lr_flag)
				sd->right_weapon.ignore_def_ele |= 1<<val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.ignore_def_ele |= 1<<val;
			break;
		case SP_IGNORE_DEF_RACE:
			PC_BONUS_CHK_RACE(val, SP_IGNORE_DEF_RACE);
			if(!sd->state.lr_flag)
				sd->right_weapon.ignore_def_race |= 1<<val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.ignore_def_race |= 1<<val;
			break;
		case SP_IGNORE_DEF_CLASS:
			PC_BONUS_CHK_CLASS(val, SP_IGNORE_DEF_CLASS);
			if(!sd->state.lr_flag)
				sd->right_weapon.ignore_def_class |= 1<<val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.ignore_def_class |= 1<<val;
			break;
		case SP_ATK_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.atk_rate += val;
			break;
		case SP_MAGIC_ATK_DEF:
			if(sd->state.lr_flag != 2)
				sd->bonus.magic_def_rate += val;
			break;
		case SP_MISC_ATK_DEF:
			if(sd->state.lr_flag != 2)
				sd->bonus.misc_def_rate += val;
			break;
		case SP_IGNORE_MDEF_ELE:
			PC_BONUS_CHK_ELEMENT(val, SP_IGNORE_MDEF_ELE);
			if(sd->state.lr_flag != 2)
				sd->bonus.ignore_mdef_ele |= 1<<val;
			break;
		case SP_IGNORE_MDEF_RACE:
			PC_BONUS_CHK_RACE(val, SP_IGNORE_MDEF_RACE);
			if(sd->state.lr_flag != 2)
				sd->bonus.ignore_mdef_race |= 1<<val;
			break;
		case SP_IGNORE_MDEF_CLASS:
			PC_BONUS_CHK_CLASS(val, SP_IGNORE_MDEF_CLASS);
			if(sd->state.lr_flag != 2)
				sd->bonus.ignore_mdef_class |= 1<<val;
			break;
		case SP_PERFECT_HIT_RATE:
			if(sd->state.lr_flag != 2 && sd->bonus.perfect_hit < val)
				sd->bonus.perfect_hit = val;
			break;
		case SP_PERFECT_HIT_ADD_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.perfect_hit_add += val;
			break;
		case SP_CRITICAL_RATE:
			if(sd->state.lr_flag != 2)
				sd->critical_rate += val;
			break;
		case SP_DEF_RATIO_ATK_ELE:
			PC_BONUS_CHK_ELEMENT(val, SP_DEF_RATIO_ATK_ELE);
			if(!sd->state.lr_flag)
				sd->right_weapon.def_ratio_atk_ele |= 1<<val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.def_ratio_atk_ele |= 1<<val;
			break;
		case SP_DEF_RATIO_ATK_RACE:
			PC_BONUS_CHK_RACE(val, SP_DEF_RATIO_ATK_RACE);
			if(!sd->state.lr_flag)
				sd->right_weapon.def_ratio_atk_race |= 1<<val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.def_ratio_atk_race |= 1<<val;
			break;
		case SP_DEF_RATIO_ATK_CLASS:
			PC_BONUS_CHK_CLASS(val, SP_DEF_RATIO_ATK_CLASS);
			if(!sd->state.lr_flag)
				sd->right_weapon.def_ratio_atk_class |= 1<<val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.def_ratio_atk_class |= 1<<val;
			break;
		case SP_HIT_RATE:
			if(sd->state.lr_flag != 2)
				sd->hit_rate += val;
			break;
		case SP_FLEE_RATE:
			if(sd->state.lr_flag != 2)
				sd->flee_rate += val;
			break;
		case SP_FLEE2_RATE:
			if(sd->state.lr_flag != 2)
				sd->flee2_rate += val;
			break;
		case SP_DEF_RATE:
			if(sd->state.lr_flag != 2)
				sd->def_rate += val;
			break;
		case SP_DEF2_RATE:
			if(sd->state.lr_flag != 2)
				sd->def2_rate += val;
			break;
		case SP_MDEF_RATE:
			if(sd->state.lr_flag != 2)
				sd->mdef_rate += val;
			break;
		case SP_MDEF2_RATE:
			if(sd->state.lr_flag != 2)
				sd->mdef2_rate += val;
			break;
		case SP_RESTART_FULL_RECOVER:
			if(sd->state.lr_flag != 2)
				sd->special_state.restart_full_recover = 1;
			break;
		case SP_NO_CASTCANCEL:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_castcancel = 1;
			break;
		case SP_NO_CASTCANCEL2:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_castcancel2 = 1;
			break;
		case SP_NO_SIZEFIX:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_sizefix = 1;
			break;
		case SP_NO_MAGIC_DAMAGE:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_magic_damage = max(sd->special_state.no_magic_damage,val);
			break;
		case SP_NO_WEAPON_DAMAGE:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_weapon_damage = max(sd->special_state.no_weapon_damage,val);
			break;
		case SP_NO_MISC_DAMAGE:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_misc_damage = max(sd->special_state.no_misc_damage,val);
			break;
		case SP_NO_GEMSTONE:
			if(sd->state.lr_flag != 2 && sd->special_state.no_gemstone != 2)
				sd->special_state.no_gemstone = 1;
			break;
		case SP_INTRAVISION: //Maya Purple Card effect allowing to see Hiding/Cloaking people [DracoRPG]
			if(sd->state.lr_flag != 2) {
				sd->special_state.intravision = 1;
				clif_status_load(&sd->bl, SI_CLAIRVOYANCE, 1);
			}
			break;
		case SP_NO_KNOCKBACK:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_knockback = 1;
			break;
		case SP_SPLASH_RANGE:
			sd->bonus.splash_range = max(sd->bonus.splash_range, val);
			break;
		case SP_SPLASH_ADD_RANGE:
			sd->bonus.splash_add_range += val;
			break;
		case SP_SHORT_WEAPON_DAMAGE_RETURN:
			if(sd->state.lr_flag != 2)
				sd->bonus.short_weapon_damage_return += val;
			break;
		case SP_LONG_WEAPON_DAMAGE_RETURN:
			if(sd->state.lr_flag != 2)
				sd->bonus.long_weapon_damage_return += val;
			break;
		case SP_MAGIC_DAMAGE_RETURN: //AppleGirl Was Here
			if(sd->state.lr_flag != 2)
				sd->bonus.magic_damage_return += val;
			break;
		case SP_ALL_STATS: //[Valaris]
			if(sd->state.lr_flag != 2) {
				sd->param_bonus[SP_STR - SP_STR] += val;
				sd->param_bonus[SP_AGI - SP_STR] += val;
				sd->param_bonus[SP_VIT - SP_STR] += val;
				sd->param_bonus[SP_INT - SP_STR] += val;
				sd->param_bonus[SP_DEX - SP_STR] += val;
				sd->param_bonus[SP_LUK - SP_STR] += val;
			}
			break;
		case SP_AGI_VIT: //[Valaris]
			if(sd->state.lr_flag != 2) {
				sd->param_bonus[SP_AGI - SP_STR] += val;
				sd->param_bonus[SP_VIT - SP_STR] += val;
			}
			break;
		case SP_AGI_DEX_STR: //[Valaris]
			if(sd->state.lr_flag != 2) {
				sd->param_bonus[SP_AGI - SP_STR] += val;
				sd->param_bonus[SP_DEX - SP_STR] += val;
				sd->param_bonus[SP_STR - SP_STR] += val;
			}
			break;
		case SP_PERFECT_HIDE: //[Valaris]
			if(sd->state.lr_flag != 2)
				sd->special_state.perfect_hiding = 1;
			break;
		case SP_UNBREAKABLE:
			if(sd->state.lr_flag != 2)
				sd->bonus.unbreakable += val;
			break;
		case SP_UNBREAKABLE_WEAPON:
			if(sd->state.lr_flag != 2)
				sd->bonus.unbreakable_equip |= EQP_WEAPON;
			break;
		case SP_UNBREAKABLE_ARMOR:
			if(sd->state.lr_flag != 2)
				sd->bonus.unbreakable_equip |= EQP_ARMOR;
			break;
		case SP_UNBREAKABLE_HELM:
			if(sd->state.lr_flag != 2)
				sd->bonus.unbreakable_equip |= EQP_HELM;
			break;
		case SP_UNBREAKABLE_SHIELD:
			if(sd->state.lr_flag != 2)
				sd->bonus.unbreakable_equip |= EQP_SHIELD;
			break;
		case SP_UNBREAKABLE_GARMENT:
			if(sd->state.lr_flag != 2)
				sd->bonus.unbreakable_equip |= EQP_GARMENT;
			break;
		case SP_UNBREAKABLE_SHOES:
			if(sd->state.lr_flag != 2)
				sd->bonus.unbreakable_equip |= EQP_SHOES;
			break;
		case SP_CLASSCHANGE: //[Valaris]
			if(sd->state.lr_flag != 2)
				sd->bonus.classchange = val;
			break;
		case SP_LONG_ATK_RATE:
			if(sd->state.lr_flag != 2)	//[Lupus] it should stack, too. As any other cards rate bonuses
				sd->bonus.long_attack_atk_rate += val;
			break;
		case SP_BREAK_WEAPON_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.break_weapon_rate += val;
			break;
		case SP_BREAK_ARMOR_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.break_armor_rate += val;
			break;
		case SP_ADD_STEAL_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.add_steal_rate += val;
			break;
		case SP_DELAYRATE:
			if(sd->state.lr_flag != 2)
				sd->delayrate += val;
			break;
		case SP_COOLDOWNRATE:
			if(sd->state.lr_flag != 2)
				sd->cooldownrate += val;
			break;
		case SP_CRIT_ATK_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.crit_atk_rate += val;
			break;
		case SP_NO_REGEN:
			if(sd->state.lr_flag != 2)
				sd->regen.state.block |= val;
			break;
		case SP_UNSTRIPABLE:
			if(sd->state.lr_flag != 2)
				sd->bonus.unstripable += val;
			break;
		case SP_UNSTRIPABLE_WEAPON:
			if(sd->state.lr_flag != 2)
				sd->bonus.unstripable_equip |= EQP_WEAPON;
			break;
		case SP_UNSTRIPABLE_ARMOR:
			if(sd->state.lr_flag != 2)
				sd->bonus.unstripable_equip |= EQP_ARMOR;
			break;
		case SP_UNSTRIPABLE_HELM:
			if(sd->state.lr_flag != 2)
				sd->bonus.unstripable_equip |= EQP_HELM;
			break;
		case SP_UNSTRIPABLE_SHIELD:
			if(sd->state.lr_flag != 2)
				sd->bonus.unstripable_equip |= EQP_SHIELD;
			break;
		case SP_HP_DRAIN_VALUE:
			if(!sd->state.lr_flag) {
				sd->right_weapon.hp_drain_class[CLASS_NORMAL] += val;
				sd->right_weapon.hp_drain_class[CLASS_BOSS] += val;
			} else if(sd->state.lr_flag == 1) {
				sd->left_weapon.hp_drain_class[CLASS_NORMAL] += val;
				sd->left_weapon.hp_drain_class[CLASS_BOSS] += val;
			}
			break;
		case SP_SP_DRAIN_VALUE:
			if(!sd->state.lr_flag) {
				sd->right_weapon.sp_drain_class[CLASS_NORMAL] += val;
				sd->right_weapon.sp_drain_class[CLASS_BOSS] += val;
			} else if(sd->state.lr_flag == 1) {
				sd->left_weapon.sp_drain_class[CLASS_NORMAL] += val;
				sd->left_weapon.sp_drain_class[CLASS_BOSS] += val;
			}
			break;
		case SP_SP_GAIN_VALUE:
			if(!sd->state.lr_flag)
				sd->bonus.sp_gain_value += val;
			break;
		case SP_HP_GAIN_VALUE:
			if(!sd->state.lr_flag)
				sd->bonus.hp_gain_value += val;
			break;
		case SP_MAGIC_SP_GAIN_VALUE:
			if(!sd->state.lr_flag)
				sd->bonus.magic_sp_gain_value += val;
			break;
		case SP_MAGIC_HP_GAIN_VALUE:
			if(!sd->state.lr_flag)
				sd->bonus.magic_hp_gain_value += val;
			break;
		case SP_ADD_HEAL_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.add_heal_rate += val;
			break;
		case SP_ADD_HEAL2_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.add_heal2_rate += val;
			break;
		case SP_ADD_ITEM_HEAL_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.itemhealrate2 += val;
			break;
		case SP_EMATK:
			if(sd->state.lr_flag != 2)
				sd->bonus.ematk += val;
			break;
#ifdef RENEWAL_CAST
		case SP_VARCASTRATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.varcastrate -= val;
			break;
		case SP_FIXCASTRATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.fixcastrate = min(sd->bonus.fixcastrate,val);
			break;
		case SP_ADD_VARIABLECAST:
			if(sd->state.lr_flag != 2)
				sd->bonus.add_varcast += val;
			break;
		case SP_ADD_FIXEDCAST:
			if(sd->state.lr_flag != 2)
				sd->bonus.add_fixcast += val;
			break;
#else
		case SP_CASTRATE:
			if(sd->state.lr_flag != 2)
				sd->castrate += val;
			break;
#endif
		case SP_ADDMAXWEIGHT:
			if(sd->state.lr_flag != 2)
				sd->add_max_weight += val;
			break;
		case SP_ABSORB_DMG_MAXHP:
			sd->bonus.absorb_dmg_maxhp = u8max(sd->bonus.absorb_dmg_maxhp, val);
			break;
		case SP_CRITICAL_LONG:
			sd->bonus.critical_long += val * 10;
			break;
		case SP_WEAPON_ATK_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.weapon_atk_rate += val;
			break;
		case SP_WEAPON_MATK_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.weapon_matk_rate += val;
			break;
		case SP_NO_MADO_FUEL:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_mado_fuel = 1;
			break;
		case SP_NO_WALKDELAY:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_walkdelay = 1;
			break;
		default:
			if(running_npc_stat_calc_event)
				ShowWarning("pc_bonus: unknown bonus type %d %d in OnPCStatCalcEvent!\n", type, val);
			else if(current_equip_pos > 0)
				ShowWarning("pc_bonus: unknown bonus type %d %d in item #%d\n", type, val, sd->inventory_data[pc_checkequip(sd, current_equip_pos, false)]->nameid);
			else if(current_equip_card_id > 0 || current_equip_item_index > 0)
				ShowWarning("pc_bonus: unknown bonus type %d %d in item #%d\n", type, val, (current_equip_card_id ? current_equip_card_id : sd->inventory_data[current_equip_item_index]->nameid));
			else
				ShowWarning("pc_bonus: unknown bonus type %d %d in unknown usage. Report this!\n", type, val);
			break;
	}
}

/**
 * Player bonus (type) with args type2 and val, called trough bonus2 (NPC)
 * format: bonus2 bBonusName,type2,val;
 * @param sd
 * @param type Bonus type used by bBonusName
 * @param type2
 * @param val Value that usually for rate or fixed value
 */
void pc_bonus2(struct map_session_data *sd, int type, int type2, int val)
{
	nullpo_retv(sd);

	switch(type) {
		case SP_ADDDEF_ELE:
			PC_BONUS_CHK_ELEMENT(type2, SP_ADDDEF_ELE);
			if(!sd->state.lr_flag)
				sd->right_weapon.adddefele[type2] += val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.adddefele[type2] += val;
			else if(sd->state.lr_flag == 2)
				sd->arrow_adddefele[type2] += val;
			else if(sd->state.lr_flag == 3)
				sd->shield_adddefele[type2] += val;
			break;
		case SP_ADDRACE:
			PC_BONUS_CHK_RACE(type2, SP_ADDRACE);
			if(!sd->state.lr_flag)
				sd->right_weapon.addrace[type2] += val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.addrace[type2] += val;
			else if(sd->state.lr_flag == 2)
				sd->arrow_addrace[type2] += val;
			else if(sd->state.lr_flag == 3)
				sd->shield_addrace[type2] += val;
			break;
		case SP_ADDCLASS:
			PC_BONUS_CHK_CLASS(type2, SP_ADDCLASS);
			if(!sd->state.lr_flag)
				sd->right_weapon.addclass[type2] += val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.addclass[type2] += val;
			else if(sd->state.lr_flag == 2)
				sd->arrow_addclass[type2] += val;
			else if(sd->state.lr_flag == 3)
				sd->shield_addclass[type2] += val;
			break;
		case SP_ADDSIZE:
			PC_BONUS_CHK_SIZE(type2, SP_ADDSIZE);
			if(!sd->state.lr_flag)
				sd->right_weapon.addsize[type2] += val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.addsize[type2] += val;
			else if(sd->state.lr_flag == 2)
				sd->arrow_addsize[type2] += val;
			else if(sd->state.lr_flag == 3)
				sd->shield_addsize[type2] += val;
			break;
		case SP_SUBELE:
			PC_BONUS_CHK_ELEMENT(type2, SP_SUBELE);
			if(sd->state.lr_flag != 2)
				sd->subele[type2] += val;
			break;
		case SP_SUBRACE:
			PC_BONUS_CHK_RACE(type2, SP_SUBRACE);
			if(sd->state.lr_flag != 2)
				sd->subrace[type2] += val;
			break;
		case SP_SUBCLASS:
			PC_BONUS_CHK_CLASS(type2, SP_SUBCLASS);
			if(sd->state.lr_flag != 2)
				sd->subclass[type2] += val;
			break;
		case SP_ADDEFF:
			PC_BONUS_CHK_SC(type2, SP_ADDEFF);
			pc_bonus_addeff(sd->addeff, (sc_type)type2, (sd->state.lr_flag != 2 ? val : 0),
				(sd->state.lr_flag == 2 ? val : 0), 0, 0);
			break;
		case SP_ADDEFF2:
			PC_BONUS_CHK_SC(type2, SP_ADDEFF2);
			pc_bonus_addeff(sd->addeff, (sc_type)type2, (sd->state.lr_flag != 2 ? val : 0),
				(sd->state.lr_flag == 2 ? val : 0), ATF_SELF, 0);
			break;
		case SP_RESEFF:
			PC_BONUS_CHK_SC(type2, SP_RESEFF);
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->reseff, type, type2, val, true);
			break;
		case SP_MAGIC_ADDDEF_ELE:
			PC_BONUS_CHK_ELEMENT(type2, SP_MAGIC_ADDDEF_ELE);
			if(sd->state.lr_flag != 2)
				sd->magic_adddefele[type2] += val;
			break;
		case SP_MAGIC_ADDRACE:
			PC_BONUS_CHK_RACE(type2, SP_MAGIC_ADDRACE);
			if(sd->state.lr_flag != 2)
				sd->magic_addrace[type2] += val;
			break;
		case SP_MAGIC_ADDCLASS:
			PC_BONUS_CHK_CLASS(type2, SP_MAGIC_ADDCLASS);
			if(sd->state.lr_flag != 2)
				sd->magic_addclass[type2] += val;
			break;
		case SP_MAGIC_ADDSIZE:
			PC_BONUS_CHK_SIZE(type2, SP_MAGIC_ADDSIZE);
			if(sd->state.lr_flag != 2)
				sd->magic_addsize[type2] += val;
			break;
		case SP_MAGIC_ATK_ELE:
			PC_BONUS_CHK_ELEMENT(type2, SP_MAGIC_ATK_ELE);
			if(sd->state.lr_flag != 2)
				sd->magic_atkele[type2] += val;
			break;
		case SP_ADD_DAMAGE_CLASS: {
				struct weapon_data *wd = (sd->state.lr_flag == 1 ? &sd->left_weapon : &sd->right_weapon);

				pc_bonus_itembonus(wd->add_dmg, type, type2, val, false);
			}
			break;
		case SP_ADD_MAGIC_DAMAGE_CLASS:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->add_mdmg, type, type2, val, false);
			break;
		case SP_ADD_DEF_MONSTER:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->add_def, type, type2, val, false);
			break;
		case SP_ADD_MDEF_MONSTER:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->add_mdef, type, type2, val, false);
			break;
		case SP_HP_DRAIN_RATE:
			if(!sd->state.lr_flag) {
				sd->right_weapon.hp_drain_rate.rate += type2;
				sd->right_weapon.hp_drain_rate.per += val;
			} else if(sd->state.lr_flag == 1) {
				sd->left_weapon.hp_drain_rate.rate += type2;
				sd->left_weapon.hp_drain_rate.per += val;
			}
			break;
		case SP_SP_DRAIN_RATE:
			if(!sd->state.lr_flag) {
				sd->right_weapon.sp_drain_rate.rate += type2;
				sd->right_weapon.sp_drain_rate.per += val;
			} else if(sd->state.lr_flag == 1) {
				sd->left_weapon.sp_drain_rate.rate += type2;
				sd->left_weapon.sp_drain_rate.per += val;
			}
			break;
		case SP_SP_VANISH_RATE:
			if(sd->state.lr_flag != 2) {
				sd->bonus.sp_vanish_rate += type2;
				sd->bonus.sp_vanish_per += val;
			}
			break;
		case SP_HP_VANISH_RATE:
			if(sd->state.lr_flag != 2) {
				sd->bonus.hp_vanish_rate += type2;
				sd->bonus.hp_vanish_per += val;
			}
			break;
		case SP_GET_ZENY_NUM:
			if(sd->state.lr_flag != 2 && sd->bonus.get_zeny_rate < val) {
				sd->bonus.get_zeny_rate = val;
				sd->bonus.get_zeny_num = type2;
			}
			break;
		case SP_ADD_GET_ZENY_NUM:
			if(sd->state.lr_flag != 2) {
				sd->bonus.get_zeny_rate += val;
				sd->bonus.get_zeny_num += type2;
			}
			break;
		case SP_WEAPON_COMA_ELE:
			PC_BONUS_CHK_ELEMENT(type2, SP_WEAPON_COMA_ELE);
			if(sd->state.lr_flag != 2) {
				sd->weapon_coma_ele[type2] += val;
				sd->special_state.bonus_coma = 1;
			}
			break;
		case SP_WEAPON_COMA_RACE:
			PC_BONUS_CHK_RACE(type2, SP_WEAPON_COMA_RACE);
			if(sd->state.lr_flag != 2) {
				sd->weapon_coma_race[type2] += val;
				sd->special_state.bonus_coma = 1;
			}
			break;
		case SP_WEAPON_COMA_CLASS:
			PC_BONUS_CHK_CLASS(type2, SP_WEAPON_COMA_CLASS);
			if(sd->state.lr_flag != 2) {
				sd->weapon_coma_class[type2] += val;
				sd->special_state.bonus_coma = 1;
			}
			break;
		case SP_CRITICAL_ADDRACE:
			PC_BONUS_CHK_RACE(type2, SP_CRITICAL_ADDRACE);
			if(sd->state.lr_flag != 2)
				sd->critaddrace[type2] += val * 10;
			break;
		case SP_ADDEFF_WHENHIT:
			PC_BONUS_CHK_SC(type2, SP_ADDEFF_WHENHIT);
			if(sd->state.lr_flag != 2)
				pc_bonus_addeff(sd->addeff2, (sc_type)type2, val, 0, 0, 0);
			break;
		case SP_SKILL_ATK:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->skillatk, type, type2, val, false);
			break;
		case SP_SKILL_HEAL:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->skillheal, type, type2, val, false);
			break;
		case SP_SKILL_HEAL2:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->skillheal2, type, type2, val, false);
			break;
		case SP_ADD_SKILL_BLOW:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->skillblown, type, type2, val, false);
			break;
		case SP_HP_LOSS_RATE:
			if(sd->state.lr_flag != 2) {
				sd->hp_loss.value = type2;
				sd->hp_loss.rate = val;
			}
			break;
		case SP_HP_REGEN_RATE:
			if(sd->state.lr_flag != 2) {
				sd->hp_regen.value = type2;
				sd->hp_regen.rate = val;
			}
			break;
		case SP_REGEN_PERCENT_HP:
			if(sd->state.lr_flag != 2) {
				sd->percent_hp_regen.value = type2;
				sd->percent_hp_regen.rate = val;
			}
			break;
		case SP_REGEN_PERCENT_SP:
			if(sd->state.lr_flag != 2) {
				sd->percent_sp_regen.value = type2;
				sd->percent_sp_regen.rate = val;
			}
			break;
		case SP_ADDRACE2:
			PC_BONUS_CHK_RACE2(type2, SP_ADDRACE2);
			if(sd->state.lr_flag != 2)
				sd->right_weapon.addrace2[type2] += val;
			else
				sd->left_weapon.addrace2[type2] += val;
			break;
		case SP_SUBSIZE:
			PC_BONUS_CHK_SIZE(type2, SP_SUBSIZE);
			if(sd->state.lr_flag != 2)
				sd->subsize[type2] += val;
			break;
		case SP_SUBRACE2:
			PC_BONUS_CHK_RACE2(type2, SP_SUBRACE2);
			if(sd->state.lr_flag != 2)
				sd->subrace2[type2] += val;
			break;
		case SP_ADD_ITEM_HEAL_RATE:
			if(!itemdb_exists(type2)) {
				ShowWarning("pc_bonus2: SP_ADD_ITEM_HEAL_RATE Invalid item with id %d\n", type2);
				break;
			}
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->itemhealrate, type, type2, val, false);
			break;
		case SP_ADD_ITEMGROUP_HEAL_RATE:
			if(!type2 || !itemdb_group_exists(type2)) {
				ShowWarning("pc_bonus2: SP_ADD_ITEMGROUP_HEAL_RATE: Invalid item group with id %d\n", type2);
				break;
			}
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->itemgrouphealrate, type, type2, val, false);
			break;
		case SP_EXP_ADDRACE:
			PC_BONUS_CHK_RACE(type2, SP_EXP_ADDRACE);
			if(sd->state.lr_flag != 2)
				sd->expaddrace[type2] += val;
			break;
		case SP_EXP_ADDCLASS:
			PC_BONUS_CHK_CLASS(type2, SP_EXP_ADDCLASS);
			if(sd->state.lr_flag != 2)
				sd->expaddclass[type2] += val;
			break;
		case SP_SP_GAIN_RACE:
			PC_BONUS_CHK_RACE(type2, SP_SP_GAIN_RACE);
			if(sd->state.lr_flag != 2)
				sd->sp_gain_race[type2] += val;
			break;
		case SP_ADD_MONSTER_DROP_ITEM:
			if(sd->state.lr_flag != 2)
				pc_bonus_item_drop(sd->add_drop, type2, 0, CLASS_ALL, RC_NONE_, val);
			break;
		case SP_ADD_MONSTER_DROP_ITEMGROUP:
			if(sd->state.lr_flag != 2)
				pc_bonus_item_drop(sd->add_drop, 0, type2, CLASS_ALL, RC_NONE_, val);
			break;
		case SP_SP_LOSS_RATE:
			if(sd->state.lr_flag != 2) {
				sd->sp_loss.value = type2;
				sd->sp_loss.rate = val;
			}
			break;
		case SP_SP_REGEN_RATE:
			if(sd->state.lr_flag != 2) {
				sd->sp_regen.value = type2;
				sd->sp_regen.rate = val;
			}
			break;
		case SP_HP_DRAIN_VALUE_RACE:
			PC_BONUS_CHK_RACE(type2, SP_HP_DRAIN_VALUE_RACE);
			if(!sd->state.lr_flag)
				sd->right_weapon.hp_drain_race[type2] += val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.hp_drain_race[type2] += val;
			break;
		case SP_SP_DRAIN_VALUE_RACE:
			PC_BONUS_CHK_RACE(type2, SP_SP_DRAIN_VALUE_RACE);
			if(!sd->state.lr_flag)
				sd->right_weapon.sp_drain_race[type2] += val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.sp_drain_race[type2] += val;
			break;
		case SP_HP_DRAIN_VALUE_CLASS:
			PC_BONUS_CHK_CLASS(type2, SP_HP_DRAIN_VALUE_CLASS);
			if(!sd->state.lr_flag)
				sd->right_weapon.hp_drain_class[type2] += val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.hp_drain_class[type2] += val;
			break;
		case SP_SP_DRAIN_VALUE_CLASS:
			PC_BONUS_CHK_CLASS(type2, SP_SP_DRAIN_VALUE_CLASS);
			if(!sd->state.lr_flag)
				sd->right_weapon.sp_drain_class[type2] += val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.sp_drain_class[type2] += val;
			break;
		case SP_IGNORE_MDEF_RACE_RATE:
			PC_BONUS_CHK_RACE(type2, SP_IGNORE_MDEF_RACE_RATE);
			if(sd->state.lr_flag != 2)
				sd->ignore_mdef_by_race[type2] += val;
			break;
		case SP_IGNORE_MDEF_CLASS_RATE:
			PC_BONUS_CHK_CLASS(type2, SP_IGNORE_MDEF_CLASS_RATE);
			if(sd->state.lr_flag != 2)
				sd->ignore_mdef_by_class[type2] += val;
			break;
		case SP_IGNORE_DEF_RACE_RATE:
			PC_BONUS_CHK_RACE(type2, SP_IGNORE_DEF_RACE_RATE);
			if(sd->state.lr_flag != 2)
				sd->ignore_def_by_race[type2] += val;
			break;
		case SP_IGNORE_DEF_CLASS_RATE:
			PC_BONUS_CHK_CLASS(type2, SP_IGNORE_DEF_CLASS_RATE);
			if(sd->state.lr_flag != 2)
				sd->ignore_def_by_class[type2] += val;
			break;
		case SP_SKILL_USE_SP_RATE:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->skillusesprate, type, type2, val, false);
			break;
		case SP_SKILL_DELAY:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->skilldelay, type, type2, val, false);
			break;
		case SP_SKILL_COOLDOWN:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->skillcooldown, type, type2, val, false);
			break;
#ifdef RENEWAL_CAST
		case SP_VARCASTRATE:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->skillcastrate, type, type2, -val, false); //Send inversed value here
			break;
		case SP_FIXCASTRATE:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->skillfixcastrate, type, type2, -val, false); //Send inversed value here
			break;
		case SP_SKILL_VARIABLECAST:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->skillvarcast, type, type2, val, false);
			break;
		case SP_SKILL_FIXEDCAST:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->skillfixcast, type, type2, val, false);
			break;
#else
		case SP_CASTRATE:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->skillcastrate, type, type2, val, false);
			break;
#endif
		case SP_SKILL_USE_SP:
			if(sd->state.lr_flag != 2)
				pc_bonus_itembonus(sd->skillusesp, type, type2, val, false);
			break;
		case SP_SUB_SKILL:
			pc_bonus_itembonus(sd->subskill, type, type2, val, false);
			break;
		case SP_SUBDEF_ELE:
			PC_BONUS_CHK_ELEMENT(type2,SP_SUBDEF_ELE);
			sd->subdefele[type2] += val;
			break;
		case SP_DROP_ADDRACE:
			PC_BONUS_CHK_RACE(type2, SP_DROP_ADDRACE);
			if(sd->state.lr_flag != 2)
				sd->dropaddrace[type2] += val;
			break;
		case SP_DROP_ADDCLASS:
			PC_BONUS_CHK_CLASS(type2, SP_DROP_ADDCLASS);
			if(sd->state.lr_flag != 2)
				sd->dropaddclass[type2] += val;
			break;
		case SP_MAGIC_ADDRACE2:
			PC_BONUS_CHK_RACE2(type2, SP_MAGIC_ADDRACE2);
			if(sd->state.lr_flag != 2)
				sd->magic_addrace2[type2] += val;
			break;
		default:
			if(running_npc_stat_calc_event)
				ShowWarning("pc_bonus2: unknown bonus type %d %d %d in OnPCStatCalcEvent!\n", type, type2, val);
			else if(current_equip_pos > 0)
				ShowWarning("pc_bonus2: unknown bonus type %d %d %d in item #%d\n", type, type2, val, sd->inventory_data[pc_checkequip(sd, current_equip_pos, false)]->nameid);
			else if(current_equip_card_id > 0 || current_equip_item_index > 0)
				ShowWarning("pc_bonus2: unknown bonus type %d %d %d in item #%d\n", type, type2, val, (current_equip_card_id ? current_equip_card_id : sd->inventory_data[current_equip_item_index]->nameid));
			else
				ShowWarning("pc_bonus2: unknown bonus type %d %d %d in unknown usage. Report this!\n", type, type2, val);
			break;
	}
}

/**
 * Gives item bonus to player for format: bonus3 bBonusName,type2,type3,val;
 * @param sd
 * @param type Bonus type used by bBonusName
 * @param type2
 * @param type3
 * @param val Value that usually for rate or fixed value
 */
void pc_bonus3(struct map_session_data *sd, int type, int type2, int type3, int val)
{
	nullpo_retv(sd);

	switch(type) {
		case SP_ADD_MONSTER_DROP_ITEM:
			if(sd->state.lr_flag != 2)
				pc_bonus_item_drop(sd->add_drop, type2, 0, CLASS_NONE, type3, val);
			break;
		case SP_ADD_MONSTER_ID_DROP_ITEM:
			if(sd->state.lr_flag != 2)
				pc_bonus_item_drop(sd->add_drop, type2, 0, CLASS_NONE, -type3, val);
			break;
		case SP_ADD_CLASS_DROP_ITEM:
			if(sd->state.lr_flag != 2)
				pc_bonus_item_drop(sd->add_drop, type2, 0, type3, RC_NONE_, val);
			break;
		case SP_AUTOSPELL:
			if(sd->state.lr_flag != 2) {
				int target = skill_get_inf(type2); //Support or Self (non-auto-target) skills should pick self

				target = (target&INF_SUPPORT_SKILL) || ((target&INF_SELF_SKILL) && !(skill_get_inf2(type2)&INF2_NO_TARGET_SELF));
				pc_bonus_autospell(sd->autospell, (target ? -type2 : type2), type3, val, 0, current_equip_card_id);
			}
			break;
		case SP_AUTOSPELL_WHENHIT:
			if(sd->state.lr_flag != 2) {
				int target = skill_get_inf(type2); //Support or Self (non-auto-target) skills should pick self.

				target = (target&INF_SUPPORT_SKILL) || ((target&INF_SELF_SKILL) && !(skill_get_inf2(type2)&INF2_NO_TARGET_SELF));
				pc_bonus_autospell(sd->autospell2, (target ? -type2 : type2), type3, val, BF_NORMAL|BF_SKILL, current_equip_card_id);
			}
			break;
		case SP_ADD_MONSTER_DROP_ITEMGROUP:
			if(sd->state.lr_flag != 2)
				pc_bonus_item_drop(sd->add_drop, 0, type2, CLASS_NONE, type3, val);
			break;
		case SP_ADD_CLASS_DROP_ITEMGROUP:
			if (sd->state.lr_flag != 2)
				pc_bonus_item_drop(sd->add_drop, 0, type2, type3, RC_NONE_, val);
			break;
		case SP_ADDEFF:
			PC_BONUS_CHK_SC(type2, SP_ADDEFF);
			pc_bonus_addeff(sd->addeff, (sc_type)type2, (sd->state.lr_flag != 2 ? type3 : 0),
				(sd->state.lr_flag == 2 ? type3 : 0), val, 0);
			break;
		case SP_ADDEFF_WHENHIT:
			PC_BONUS_CHK_SC(type2, SP_ADDEFF_WHENHIT);
			if(sd->state.lr_flag != 2)
				pc_bonus_addeff(sd->addeff2, (sc_type)type2, type3, 0, val, 0);
			break;
		case SP_ADDEFF_ONSKILL:
			PC_BONUS_CHK_SC(type3, SP_ADDEFF_ONSKILL);
			if(sd->state.lr_flag != 2)
				pc_bonus_addeff_onskill(sd->addeff3, (sc_type)type3, val, type2, ATF_TARGET, 0);
			break;
		case SP_ADDDEF_ELE: {
				struct weapon_data *wd = (sd->state.lr_flag == 1 ? &sd->left_weapon : &sd->right_weapon);

				PC_BONUS_CHK_ELEMENT(type2, SP_ADDDEF_ELE);
				if(sd->state.lr_flag != 2)
					pc_bonus_adddefele(wd->adddefele2, (unsigned char)type2, type3, val);
			}
			break;
		case SP_SUBELE:
			PC_BONUS_CHK_ELEMENT(type2, SP_SUBELE);
			if(sd->state.lr_flag != 2)
				pc_bonus_subele(sd->subele2, (unsigned char)type2, type3, val);
			break;
		case SP_STATE_NORECOVER_RACE:
			PC_BONUS_CHK_RACE(type2, SP_STATE_NORECOVER_RACE);
			if(sd->state.lr_flag != 2) {
				sd->norecover_state_race[type2].rate = type3;
				sd->norecover_state_race[type2].tick = val;
			}
			break;
		case SP_SP_VANISH_RACE_RATE:
			PC_BONUS_CHK_RACE(type2, SP_SP_VANISH_RACE_RATE);
			if(sd->state.lr_flag != 2) {
				sd->sp_vanish_race[type2].rate += type3;
				sd->sp_vanish_race[type2].per += val;
			}
			break;
		case SP_HP_VANISH_RACE_RATE:
			PC_BONUS_CHK_RACE(type2, SP_HP_VANISH_RACE_RATE);
			if(sd->state.lr_flag != 2) {
				sd->hp_vanish_race[type2].rate += type3;
				sd->hp_vanish_race[type2].per += val;
			}
			break;
		default:
			if(running_npc_stat_calc_event)
				ShowWarning("pc_bonus3: unknown bonus type %d %d %d %d in OnPCStatCalcEvent!\n", type, type2, type3, val);
			else if(current_equip_pos > 0)
				ShowWarning("pc_bonus3: unknown bonus type %d %d %d %d in item #%d\n", type, type2, type3, val, sd->inventory_data[pc_checkequip(sd, current_equip_pos, false)]->nameid);
			else if(current_equip_card_id > 0 || current_equip_item_index > 0)
				ShowWarning("pc_bonus3: unknown bonus type %d %d %d %d in item #%d\n", type, type2, type3, val, (current_equip_card_id ? current_equip_card_id : sd->inventory_data[current_equip_item_index]->nameid));
			else
				ShowWarning("pc_bonus3: unknown bonus type %d %d %d %d in unknown usage. Report this!\n", type, type2, type3, val);
			break;
	}
}

/**
 * Gives item bonus to player for format: bonus4 bBonusName,type2,type3,type4,val;
 * @param sd
 * @param type Bonus type used by bBonusName
 * @param type2
 * @param type3
 * @param type4
 * @param val Value that usually for rate or fixed value
 */
void pc_bonus4(struct map_session_data *sd, int type, int type2, int type3, int type4, int val)
{
	nullpo_retv(sd);

	switch(type) {
		case SP_AUTOSPELL:
			if(sd->state.lr_flag != 2)
				pc_bonus_autospell(sd->autospell, (val&1 ? type2 : -type2), (val&2 ? -type3 : type3), type4, 0, current_equip_card_id);
			break;
		case SP_AUTOSPELL_WHENHIT:
			if(sd->state.lr_flag != 2)
				pc_bonus_autospell(sd->autospell2, (val&1 ? type2 : -type2), (val&2 ? -type3 : type3), type4, BF_NORMAL|BF_SKILL, current_equip_card_id);
			break;
		case SP_AUTOSPELL_ONSKILL:
			if(sd->state.lr_flag != 2) {
				int target = skill_get_inf(type3); //Support or Self (non-auto-target) skills should pick self

				target = (target&INF_SUPPORT_SKILL) || ((target&INF_SELF_SKILL) && !(skill_get_inf2(type3)&INF2_NO_TARGET_SELF));
				pc_bonus_autospell_onskill(sd->autospell3, type2, (target ? -type3 : type3), type4, val, current_equip_card_id);
			}
			break;
		case SP_ADDEFF:
			PC_BONUS_CHK_SC(type2, SP_ADDEFF);
			pc_bonus_addeff(sd->addeff, (sc_type)type2, (sd->state.lr_flag != 2 ? type3 : 0),
				(sd->state.lr_flag == 2 ? type3 : 0), type4, val);
			break;
		case SP_ADDEFF_WHENHIT:
			PC_BONUS_CHK_SC(type2, SP_ADDEFF_WHENHIT);
			if(sd->state.lr_flag != 2)
				pc_bonus_addeff(sd->addeff2, (sc_type)type2, type3, 0, type4, val);
			break;
		case SP_ADDEFF_ONSKILL:
			PC_BONUS_CHK_SC(type3, SP_ADDEFF_ONSKILL);
			if(sd->state.lr_flag != 2)
				pc_bonus_addeff_onskill(sd->addeff3, (sc_type)type3, type4, type2, val, 0);
			break;
		case SP_SET_DEF_RACE:
			PC_BONUS_CHK_RACE(type2, SP_SET_DEF_RACE);
			if(sd->state.lr_flag != 2) {
				sd->def_set_race[type2].rate = type3;
				sd->def_set_race[type2].tick = type4;
				sd->def_set_race[type2].value = val;
			}
			break;
		case SP_SET_MDEF_RACE:
			PC_BONUS_CHK_RACE(type2, SP_SET_MDEF_RACE);
			if(sd->state.lr_flag != 2) {
				sd->mdef_set_race[type2].rate = type3;
				sd->mdef_set_race[type2].tick = type4;
				sd->mdef_set_race[type2].value = val;
			}
			break;
		default:
			if(running_npc_stat_calc_event)
				ShowWarning("pc_bonus4: unknown bonus type %d %d %d %d %d in OnPCStatCalcEvent!\n", type, type2, type3, type4, val);
			else if(current_equip_pos > 0)
				ShowWarning("pc_bonus4: unknown bonus type %d %d %d %d %d in item #%d\n", type, type2, type3, type4, val, sd->inventory_data[pc_checkequip(sd, current_equip_pos, false)]->nameid);
			else if(current_equip_card_id > 0 || current_equip_item_index > 0)
				ShowWarning("pc_bonus4: unknown bonus type %d %d %d %d %d in item #%d\n", type, type2, type3, type4, val, (current_equip_card_id ? current_equip_card_id : sd->inventory_data[current_equip_item_index]->nameid));
			else
				ShowWarning("pc_bonus4: unknown bonus type %d %d %d %d %d in unknown usage. Report this!\n", type, type2, type3, type4, val);
			break;
	}
}

/**
 * Gives item bonus to player for format: bonus5 bBonusName,type2,type3,type4,val;
 * @param sd
 * @param type Bonus type used by bBonusName
 * @param type2
 * @param type3
 * @param type4
 * @param val Value that usually for rate or fixed value
 */
void pc_bonus5(struct map_session_data *sd, int type, int type2, int type3, int type4, int type5, int val)
{
	nullpo_retv(sd);

	switch(type) {
		case SP_AUTOSPELL:
			if(sd->state.lr_flag != 2)
				pc_bonus_autospell(sd->autospell, (val&1 ? type2 : -type2), (val&2 ? -type3 : type3), type4, type5, current_equip_card_id);
			break;
		case SP_AUTOSPELL_WHENHIT:
			if(sd->state.lr_flag != 2)
				pc_bonus_autospell(sd->autospell2, (val&1 ? type2 : -type2), (val&2 ? -type3 : type3), type4, type5, current_equip_card_id);
			break;
		case SP_AUTOSPELL_ONSKILL:
			if(sd->state.lr_flag != 2)
				pc_bonus_autospell_onskill(sd->autospell3, type2, (val&1 ? -type3 : type3), (val&2 ? -type4 : type4), type5, current_equip_card_id);
			break;
		case SP_ADDEFF_ONSKILL:
			PC_BONUS_CHK_SC(type3, SP_ADDEFF_ONSKILL);
			if(sd->state.lr_flag != 2)
				pc_bonus_addeff_onskill(sd->addeff3, (sc_type)type3, type4, type2, type5, val);
			break;
		default:
			if(running_npc_stat_calc_event)
				ShowWarning("pc_bonus5: unknown bonus type %d %d %d %d %d %d in OnPCStatCalcEvent!\n", type, type2, type3, type4, type5, val);
			else if(current_equip_pos > 0)
				ShowWarning("pc_bonus5: unknown bonus type %d %d %d %d %d %d in item #%d\n", type, type2, type3, type4, type5, val, sd->inventory_data[pc_checkequip(sd, current_equip_pos, false)]->nameid);
			else if(current_equip_card_id > 0 || current_equip_item_index > 0)
				ShowWarning("pc_bonus5: unknown bonus type %d %d %d %d %d %d in item #%d\n", type, type2, type3, type4, type5, val, (current_equip_card_id ? current_equip_card_id : sd->inventory_data[current_equip_item_index]->nameid));
			else
				ShowWarning("pc_bonus5: unknown bonus type %d %d %d %d %d %d in unknown usage. Report this!\n", type, type2, type3, type4, type5, val);
			break;
	}
}

/**
 *	Grants a player a given skill. Flag values are:
 *	0 - Grant permanent skill to be bound to skill tree
 *	1 - Grant an item skill (temporary)
 *	2 - Like 1, except the level granted can stack with previously learned level.
 *	4 - Like 0, except the skill will ignore skill tree (saves through job changes and resets).
 */
int pc_skill(struct map_session_data *sd, int id, int level, int flag)
{
	nullpo_ret(sd);

	if( id <= 0 || id >= MAX_SKILL || !skill_db[id].name) {
		ShowError("pc_skill: Skill with id %d does not exist in the skill database\n", id);
		return 0;
	}
	if( level > MAX_SKILL_LEVEL ) {
		ShowError("pc_skill: Skill level %d too high. Max lv supported is %d\n", level, MAX_SKILL_LEVEL);
		return 0;
	}
	if( flag == 2 && sd->status.skill[id].lv + level > MAX_SKILL_LEVEL ) {
		ShowError("pc_skill: Skill level bonus %d too high. Max lv supported is %d. Curr lv is %d\n", level, MAX_SKILL_LEVEL, sd->status.skill[id].lv);
		return 0;
	}

	switch( flag ) {
		case 0: //Set skill data overwriting whatever was there before
			sd->status.skill[id].id   = id;
			sd->status.skill[id].lv   = level;
			sd->status.skill[id].flag = SKILL_FLAG_PERMANENT;
			if( !level ) { //Remove skill
				sd->status.skill[id].id = 0;
				clif_deleteskill(sd,id);
			} else
				clif_addskill(sd,id);
			if( !skill_get_inf(id) || (skill_get_inf3(id)&INF3_BOOST_PASSIVE && (pc_checkskill(sd, SU_POWEROFLAND) > 0 || pc_checkskill(sd, SU_POWEROFSEA) > 0)) )
				status_calc_pc(sd, SCO_NONE); //Only recalculate for passive skills and active skills that boost the effects of passive skills
			break;
		case 1: //Item bonus skill
			if( sd->status.skill[id].id == id ) {
				if( sd->status.skill[id].lv >= level )
					return 0;
				if( sd->status.skill[id].flag == SKILL_FLAG_PERMANENT ) //Non-granted skill, store it's level
					sd->status.skill[id].flag = SKILL_FLAG_REPLACED_LV_0 + sd->status.skill[id].lv;
			} else {
				sd->status.skill[id].id   = id;
				sd->status.skill[id].flag = SKILL_FLAG_TEMPORARY;
			}
			sd->status.skill[id].lv = level;
			break;
		case 2: //Add skill bonus on top of what you had
			if( sd->status.skill[id].id == id ) {
				if( sd->status.skill[id].flag == SKILL_FLAG_PERMANENT )
					sd->status.skill[id].flag = SKILL_FLAG_REPLACED_LV_0 + sd->status.skill[id].lv; //Store previous level
			} else {
				sd->status.skill[id].id   = id;
				sd->status.skill[id].flag = SKILL_FLAG_TEMPORARY; //Set that this is a bonus skill
			}
			sd->status.skill[id].lv += level;
			break;
		case 4: //Permanent granted skills ignore the skill tree
			sd->status.skill[id].id   = id;
			sd->status.skill[id].lv   = level;
			sd->status.skill[id].flag = SKILL_FLAG_PERM_GRANTED;
			if( !level ) { //Remove skill
				sd->status.skill[id].id = 0;
				clif_deleteskill(sd,id);
			} else
				clif_addskill(sd,id);
			if( !skill_get_inf(id) ) //Only recalculate for passive skills
				status_calc_pc(sd, SCO_NONE);
			break;
		default: //Unknown flag?
			return 0;
	}
	return 1;
}

/**
 * Checks if the given card can be inserted into the given equipment piece.
 *
 * @param sd        The current character.
 * @param idx_card  The card's inventory index (note: it must be a valid index and can be checked by pc_can_insert_card)
 * @param idx_equip The target equipment's inventory index.
 * @retval true if the card can be inserted.
 */
bool pc_can_insert_card_into(struct map_session_data *sd, int idx_card, int idx_equip)
{
	int i;

	nullpo_ret(sd);

	if( idx_equip < 0 || idx_equip >= MAX_INVENTORY || sd->inventory_data[idx_equip] == NULL )
		return false; //Invalid item index

	if( sd->inventory.u.items_inventory[idx_equip].nameid <= 0 || sd->inventory.u.items_inventory[idx_equip].amount < 1 )
		return false; //Target item missing

	if( sd->inventory_data[idx_equip]->type != IT_WEAPON && sd->inventory_data[idx_equip]->type != IT_ARMOR )
		return false; //Only weapons and armor are allowed

	if( sd->inventory.u.items_inventory[idx_equip].identify == 0 )
		return false; //Target must be identified

	if( itemdb_isspecial(sd->inventory.u.items_inventory[idx_equip].card[0]) )
		return false; //Card slots reserved for other purposes

	if( sd->inventory.u.items_inventory[idx_equip].equip != 0 )
		return false; //Item must be unequipped

	if( (sd->inventory_data[idx_equip]->equip&sd->inventory_data[idx_card]->equip) == 0 )
		return false; //Card cannot be compounded on this item type

	if( sd->inventory_data[idx_equip]->type == IT_WEAPON && sd->inventory_data[idx_card]->equip == EQP_SHIELD )
		return false; //Attempted to place shield card on left-hand weapon

	ARR_FIND(0, sd->inventory_data[idx_equip]->slot, i, sd->inventory.u.items_inventory[idx_equip].card[i] == 0);
	if( i == sd->inventory_data[idx_equip]->slot )
		return false; //No free slots
	return true;
}

/**
 * Checks if the given item is card and it can be inserted into some equipment.
 *
 * @param sd        The current character.
 * @param idx_card  The card's inventory index.
 * @retval true if the card can be inserted.
 */
bool pc_can_insert_card(struct map_session_data *sd, int idx_card)
{
	nullpo_ret(sd);

	if( idx_card < 0 || idx_card >= MAX_INVENTORY || sd->inventory_data[idx_card] == NULL )
		return false; //Invalid card index

	if( sd->inventory.u.items_inventory[idx_card].nameid <= 0 || sd->inventory.u.items_inventory[idx_card].amount < 1 )
		return false; //Target card missing

	if( sd->inventory_data[idx_card]->type != IT_CARD )
		return false; //Must be a card
	return true;
}

/*==========================================
 * Append a card to an item ?
 *------------------------------------------*/
int pc_insert_card(struct map_session_data *sd, int idx_card, int idx_equip)
{
	unsigned short nameid;

	nullpo_ret(sd);

	if( sd->state.trading != 0 )
		return 0;

	if( !pc_can_insert_card(sd, idx_card) || !pc_can_insert_card_into(sd, idx_card, idx_equip) )
		return 0;

	//Remember the card id to insert
	nameid = sd->inventory.u.items_inventory[idx_card].nameid;

	if( pc_delitem(sd, idx_card, 1, 1, 0, LOG_TYPE_OTHER) == 1 ) //Failed
		clif_insert_card(sd, idx_equip, idx_card, 1);
	else { //Success
		int i;

		ARR_FIND(0, sd->inventory_data[idx_equip]->slot, i, sd->inventory.u.items_inventory[idx_equip].card[i] == 0);
		if( i == sd->inventory_data[idx_equip]->slot )
			return 0; //No free slots
		log_pick_pc(sd, LOG_TYPE_OTHER, -1, &sd->inventory.u.items_inventory[idx_equip]);
		sd->inventory.u.items_inventory[idx_equip].card[i] = nameid;
		log_pick_pc(sd, LOG_TYPE_OTHER,  1, &sd->inventory.u.items_inventory[idx_equip]);
		clif_insert_card(sd, idx_equip, idx_card, 0);
	}
	return 0;
}

/**
 * Returns the count of unidentified items with the option to identify too.
 * @param sd: Player data
 * @param identify_item: Whether or not to identify any unidentified items
 * @return Unidentified items count
 */
int pc_identifyall(struct map_session_data *sd, bool identify_item)
{
	int unidentified_count = 0;
	int i;

	for( i = 0; i < MAX_INVENTORY; i++ ) {
		if( sd->inventory.u.items_inventory[i].nameid > 0 && sd->inventory.u.items_inventory[i].identify != 1 ) {
			if( identify_item ) {
				sd->inventory.u.items_inventory[i].identify = 1;
				clif_item_identified(sd, i, 0);
			}
			unidentified_count++;
		}
	}

	return unidentified_count;
}

//
// Items
//

/*==========================================
 * Update buying value by skills
 *------------------------------------------*/
int pc_modifybuyvalue(struct map_session_data *sd,int orig_value)
{
	uint16 lv;
	int val = orig_value, rate1 = 0, rate2 = 0;

	if((lv = pc_checkskill(sd,MC_DISCOUNT)) > 0) //Merchant discount
		rate1 = 5 + lv * 2 - (lv == 10 ? 1 : 0);
	if((lv = pc_checkskill(sd,RG_COMPULSION)) > 0) //Rogue discount
		rate2 = 5 + lv * 4;
	if(rate1 < rate2)
		rate1 = rate2;
	if(rate1)
		val = (int)((double)orig_value * (double)(100 - rate1) / 100.);
	if(val < battle_config.min_shop_buy)
		val = battle_config.min_shop_buy;

	return val;
}

/*==========================================
 * Update selling value by skills
 *------------------------------------------*/
int pc_modifysellvalue(struct map_session_data *sd,int orig_value)
{
	uint16 lv;
	int val = orig_value, rate = 0;

	if((lv = pc_checkskill(sd,MC_OVERCHARGE)) > 0) //Over Charge
		rate = 5 + lv * 2 - (lv == 10 ? 1 : 0);
	if(rate)
		val = (int)((double)orig_value * (double)(100 + rate) / 100.);
	if(val < battle_config.min_shop_sell)
		val = battle_config.min_shop_sell;

	return val;
}

/**
 * Checking if we have enough place on inventory for new item
 * Make sure to take 30k as limit (for client I guess)
 * @param sd
 * @param nameid
 * @param amount
 * @return e_chkitem_result
 */
char pc_checkadditem(struct map_session_data *sd, unsigned short nameid, int amount)
{
	int i;
	struct item_data *data;

	nullpo_ret(sd);

	if(amount > MAX_AMOUNT)
		return CHKADDITEM_OVERAMOUNT;

	data = itemdb_search(nameid);

	if(!itemdb_isstackable2(data))
		return CHKADDITEM_NEW;

	if(data->stack.inventory && amount > data->stack.amount)
		return CHKADDITEM_OVERAMOUNT;

	for(i = 0; i < MAX_INVENTORY; i++) {
		//FIXME: This does not consider the checked item's cards, thus could check a wrong slot for stackability.
		if(sd->inventory.u.items_inventory[i].nameid == nameid) {
			if(amount > MAX_AMOUNT - sd->inventory.u.items_inventory[i].amount ||
				(data->stack.inventory && amount > data->stack.amount - sd->inventory.u.items_inventory[i].amount))
				return CHKADDITEM_OVERAMOUNT;
			return CHKADDITEM_EXIST;
		}
	}

	return CHKADDITEM_NEW;
}

/**
 * Return number of available place in inventory
 * Each non stackable item will reduce place by 1
 * @param sd
 * @return Number of empty slots
 */
uint8 pc_inventoryblank(struct map_session_data *sd)
{
	uint8 i, b;

	nullpo_ret(sd);

	for(i = 0, b = 0; i < MAX_INVENTORY; i++)
		if(!sd->inventory.u.items_inventory[i].nameid)
			b++;

	return b;
}

/**
 * Attempts to remove zeny from player
 * @param sd: Player
 * @param zeny: Zeny removed
 * @param type: Log type
 * @param tsd: (optional) From who to log (if null take sd)
 * @return 0: Success, 1: Failed (Removing negative Zeny or not enough Zeny), 2: Player not found
 */
char pc_payzeny(struct map_session_data *sd, int zeny, enum e_log_pick_type type, struct map_session_data *tsd)
{
	nullpo_retr(-1, sd);

	zeny = cap_value(zeny, -MAX_ZENY, MAX_ZENY); //Prevent command UB
	if( zeny < 0 ) {
		ShowError("pc_payzeny: Paying negative Zeny (zeny=%d, account_id=%d, char_id=%d).\n", zeny, sd->status.account_id, sd->status.char_id);
		return 1;
	}

	if( sd->status.zeny < zeny )
		return 1; //Not enough.

	sd->status.zeny -= zeny;
	clif_updatestatus(sd, SP_ZENY);
	if( !tsd )
		tsd = sd;

	log_zeny(sd, type, tsd, -zeny);
	if( zeny > 0 && sd->state.showzeny ) {
		char output[CHAT_SIZE_MAX];

		sprintf(output, "Removed %dz.", zeny);
		clif_messagecolor(&sd->bl, color_table[COLOR_LIGHT_GREEN], output, false, SELF);
	}

	return 0;
}

/**
 * Attempts to give zeny to player
 * @param sd: Player
 * @param zeny: Zeny gained
 * @param type: Log type
 * @param tsd: (optional) From who to log (if null take sd)
 * @return -1: Player not found, 0: Success, 1: Giving negative Zeny
 */
char pc_getzeny(struct map_session_data *sd, int zeny, enum e_log_pick_type type, struct map_session_data *tsd)
{
	nullpo_retr(-1, sd);

	zeny = cap_value(zeny, -MAX_ZENY, MAX_ZENY); //Prevent command UB
	if( zeny < 0 ) {
		ShowError("pc_getzeny: Obtaining negative Zeny (zeny=%d, account_id=%d, char_id=%d).\n", zeny, sd->status.account_id, sd->status.char_id);
		return 1;
	}

	if( zeny > MAX_ZENY - sd->status.zeny )
		zeny = MAX_ZENY - sd->status.zeny;

	sd->status.zeny += zeny;
	clif_updatestatus(sd, SP_ZENY);

	if( !tsd )
		tsd = sd;

	log_zeny(sd, type, tsd, zeny);
	if( zeny > 0 && sd->state.showzeny ) {
		char output[CHAT_SIZE_MAX];

		sprintf(output, "Gained %dz.", zeny);
		clif_messagecolor(&sd->bl, color_table[COLOR_LIGHT_GREEN], output, false, SELF);
	}

	achievement_update_objective(sd, AG_GET_ZENY, 1, sd->status.zeny);

	return 0;
}

/**
 * Attempts to remove Cash Points from player
 * @param sd: Player
 * @param price: Total points (cash + kafra) the player has to pay
 * @param points: Kafra points the player has to pay
 * @param type: Log type
 * @return -1: Not enough points, otherwise success (cash+points)
 */
int pc_paycash(struct map_session_data *sd, int price, int points, e_log_pick_type type)
{
	int cash;

	nullpo_retr(-1, sd);

	points = cap_value(points, 0, MAX_ZENY); //Prevent command UB

	cash = price - points;
	if( sd->cashPoints < cash || sd->kafraPoints < points ) {
		ShowError("pc_paycash: Not enough points (cash=%d, kafra=%d) to cover the price (cash=%d, kafra=%d) (account_id=%d, char_id=%d).\n", sd->cashPoints, sd->kafraPoints, cash, points, sd->status.account_id, sd->status.char_id);
		return -1;
	}

	if( cash ) {
		pc_setaccountreg(sd, CASHPOINT_VAR, sd->cashPoints - cash);
		sd->cashPoints -= cash;
		log_cash(sd, type, LOG_CASH_TYPE_CASH, -cash);
	}

	if( points ) {
		pc_setaccountreg(sd, KAFRAPOINT_VAR, sd->kafraPoints - points);
		sd->kafraPoints -= points;
		log_cash(sd, type, LOG_CASH_TYPE_KAFRA, -points);
	}

	if( battle_config.cashshop_show_points ) {
		char output[CHAT_SIZE_MAX];

		sprintf(output, msg_txt(sd, 504), points, cash, sd->kafraPoints, sd->cashPoints); // Used %d kafra points and %d cash points. %d kafra and %d cash points remaining.
		clif_messagecolor(&sd->bl, color_table[COLOR_LIGHT_GREEN], output, false, SELF);
	}

	return cash + points;
}

/**
 * Attempts to give Cash Points to player
 * @param sd: Player
 * @param cash: Cash points the player gets
 * @param points: Kafra points the player gets
 * @param type: Log type
 * @return -1: Error, otherwise success (cash or points)
 */
int pc_getcash(struct map_session_data *sd, int cash, int points, e_log_pick_type type)
{
	char output[CHAT_SIZE_MAX];

	nullpo_retr(-1, sd);

	cash = cap_value(cash, 0, MAX_ZENY); //Prevent command UB
	points = cap_value(points, 0, MAX_ZENY); //Prevent command UB

	if( cash > 0 ) {
		if( cash > MAX_ZENY - sd->cashPoints ) {
			ShowWarning("pc_getcash: Cash point overflow (cash=%d, have cash=%d, account_id=%d, char_id=%d).\n", cash, sd->cashPoints, sd->status.account_id, sd->status.char_id);
			cash = MAX_ZENY - sd->cashPoints;
		}
		pc_setaccountreg(sd, CASHPOINT_VAR, sd->cashPoints + cash);
		sd->cashPoints += cash;
		log_cash(sd, type, LOG_CASH_TYPE_CASH, cash);
		if( battle_config.cashshop_show_points ) {
			sprintf(output, msg_txt(sd, 505), cash, sd->cashPoints);
			clif_messagecolor(&sd->bl, color_table[COLOR_LIGHT_GREEN], output, false, SELF);
		}
		return cash;
	}

	if( points > 0 ) {
		if( points > MAX_ZENY - sd->kafraPoints ) {
			ShowWarning("pc_getcash: Kafra point overflow (points=%d, have points=%d, account_id=%d, char_id=%d).\n", points, sd->kafraPoints, sd->status.account_id, sd->status.char_id);
			points = MAX_ZENY - sd->kafraPoints;
		}
		pc_setaccountreg(sd, KAFRAPOINT_VAR, sd->kafraPoints + points);
		sd->kafraPoints += points;
		log_cash(sd, type, LOG_CASH_TYPE_KAFRA, points);
		if( battle_config.cashshop_show_points ) {
			sprintf(output, msg_txt(sd, 506), points, sd->kafraPoints);
			clif_messagecolor(&sd->bl, color_table[COLOR_LIGHT_GREEN], output, false, SELF);
		}
		return points;
	}

	return -1; //Shouldn't happen but just in case
}

/**
 * Searches for the specified item ID in inventory and return its inventory index.
 *
 * If the item is found, the returned value is guaranteed to be a valid index
 * (non-negative, smaller than MAX_INVENTORY).
 *
 * @param sd      Character to search on.
 * @param nameid  The item ID to search.
 * @return the inventory index of the first instance of the requested item.
 * @return INDEX_NOT_FOUND if the item wasn't found.
 */
short pc_search_inventory(struct map_session_data *sd, unsigned short nameid)
{
	short i;

	nullpo_retr(INDEX_NOT_FOUND, sd);

	ARR_FIND(0, MAX_INVENTORY, i, (sd->inventory.u.items_inventory[i].nameid == nameid && (sd->inventory.u.items_inventory[i].amount > 0 || !nameid)));
	return (i < MAX_INVENTORY) ? i : INDEX_NOT_FOUND;
}

/** Attempt to add a new item to player inventory
 * @param sd
 * @param item
 * @param amount
 * @param log_type
 * @return see e_additem_result
 */
enum e_additem_result pc_additem(struct map_session_data *sd, struct item *item, int amount, e_log_pick_type log_type)
{
	struct item_data *id;
	int16 i;
	unsigned int w;

	nullpo_retr(ADDITEM_INVALID, sd);
	nullpo_retr(ADDITEM_INVALID, item);

	if( !item->nameid || amount <= 0 )
		return ADDITEM_INVALID;

	if( amount > MAX_AMOUNT )
		return ADDITEM_OVERAMOUNT;

	id = itemdb_search(item->nameid);
	if( id->stack.inventory && amount > id->stack.amount ) //Item stack limitation
		return ADDITEM_STACKLIMIT;

	w = id->weight * amount;
	if( sd->weight + w > sd->max_weight )
		return ADDITEM_OVERWEIGHT;

	i = MAX_INVENTORY;

	if( id->flag.guid && !item->unique_id )
		item->unique_id = pc_generate_unique_id(sd);

	if( itemdb_isstackable2(id) && !item->expire_time ) { //Stackable | Non Rental
		for( i = 0; i < MAX_INVENTORY; i++ ) {
			if( sd->inventory.u.items_inventory[i].nameid == item->nameid &&
				sd->inventory.u.items_inventory[i].bound == item->bound &&
				!sd->inventory.u.items_inventory[i].expire_time &&
				sd->inventory.u.items_inventory[i].unique_id == item->unique_id &&
				!memcmp(&sd->inventory.u.items_inventory[i].card, &item->card, sizeof(item->card)) )
			{
				if( amount > MAX_AMOUNT - sd->inventory.u.items_inventory[i].amount ||
					(id->stack.inventory && amount > id->stack.amount - sd->inventory.u.items_inventory[i].amount) )
					return ADDITEM_OVERAMOUNT;
				sd->inventory.u.items_inventory[i].amount += amount;
				clif_additem(sd, i, amount, 0);
				break;
			}
		}
	}

	if( i >= MAX_INVENTORY ) {
		i = pc_search_inventory(sd, 0);
		if( i == INDEX_NOT_FOUND )
			return ADDITEM_OVERITEM;
		memcpy(&sd->inventory.u.items_inventory[i], item, sizeof(sd->inventory.u.items_inventory[0]));
		//Clear equip and favorite fields first, just in case
		if( item->equip )
			sd->inventory.u.items_inventory[i].equip = 0;
		if( item->favorite )
			sd->inventory.u.items_inventory[i].favorite = 0;
		if( item->equipSwitch )
			sd->inventory.u.items_inventory[i].equipSwitch = 0;
		sd->inventory.u.items_inventory[i].amount = amount;
		sd->inventory_data[i] = id;
		sd->last_addeditem_index = i;
		if( !itemdb_isstackable2(id) || id->flag.guid )
			sd->inventory.u.items_inventory[i].unique_id = (item->unique_id ? item->unique_id : pc_generate_unique_id(sd));
		clif_additem(sd, i, amount, 0);
	}

	log_pick_pc(sd, log_type, amount, &sd->inventory.u.items_inventory[i]);
	sd->weight += w;
	clif_updatestatus(sd, SP_WEIGHT);

	if( id->flag.autoequip ) //Auto-equip
		pc_equipitem(sd, i, id->equip, false);

	if( item->expire_time ) { //Rental item check
		if( item->expire_time <= time(NULL) ) {
			clif_rental_expired(sd->fd, i, sd->inventory.u.items_inventory[i].nameid);
			pc_delitem(sd, i, sd->inventory.u.items_inventory[i].amount, 1, 0, LOG_TYPE_OTHER);
		} else {
			unsigned int seconds = (unsigned int)(item->expire_time - time(NULL));

			clif_rental_time(sd->fd, sd->inventory.u.items_inventory[i].nameid, (int)seconds);
			pc_inventory_rental_add(sd, (int)seconds);
		}
	}

	achievement_update_objective(sd, AG_GET_ITEM, 1, id->value_sell);

	return ADDITEM_SUCCESS;
}

/**
 * Remove an item at index n from inventory by amount.
 * @param sd
 * @param n Item index in inventory
 * @param amount
 * @param type &1: Don't notify deletion; &2 Don't notify weight change; &4 Don't calculate status
 * @param reason Delete reason
 * @param log_type e_log_pick_type
 * @return 1 - invalid itemid or negative amount; 0 - Success
 */
char pc_delitem(struct map_session_data *sd, int n, int amount, int type, short reason, e_log_pick_type log_type)
{
	nullpo_retr(1, sd);

	if(n < 0 || !sd->inventory_data[n] || !sd->inventory.u.items_inventory[n].nameid || amount <= 0 ||
		sd->inventory.u.items_inventory[n].amount < amount)
		return 1;

	log_pick_pc(sd, log_type, -amount, &sd->inventory.u.items_inventory[n]);

	sd->inventory.u.items_inventory[n].amount -= amount;
	sd->weight -= sd->inventory_data[n]->weight * amount;
	if(sd->inventory.u.items_inventory[n].amount <= 0) {
		if(sd->inventory.u.items_inventory[n].equip)
			pc_unequipitem(sd, n, (!(type&4) ? 1 : 0)|2);
		memset(&sd->inventory.u.items_inventory[n], 0, sizeof(sd->inventory.u.items_inventory[0]));
		sd->inventory_data[n] = NULL;
	}
	if(!(type&1))
		clif_delitem(sd, n, amount, reason);
	if(!(type&2))
		clif_updatestatus(sd, SP_WEIGHT);

	return 0;
}

/**
 * Attempt to drop an item.
 * @param sd
 * @param n Item index in inventory
 * @param amount Amount of item
 * @return False = fail; True = success
 */
bool pc_dropitem(struct map_session_data *sd, int n, int amount)
{
	nullpo_retr(false, sd);

	if(n < 0 || n >= MAX_INVENTORY)
		return false;

	if(amount <= 0)
		return false;

	if(sd->inventory.u.items_inventory[n].nameid <= 0 ||
		sd->inventory.u.items_inventory[n].amount <= 0 ||
		sd->inventory.u.items_inventory[n].amount < amount ||
		sd->state.trading || sd->state.vending ||
		!sd->inventory_data[n]) //pc_delitem would fail on this case
		return false;

	if(sd->menuskill_id || sd->inventory.u.items_inventory[n].equipSwitch)
		return false;

	if(mapdata[sd->bl.m].flag.nodrop) {
		clif_displaymessage (sd->fd, msg_txt(sd, 271));
		return false; //Can't drop items in nodrop mapflag maps
	}

	if(!pc_candrop(sd,&sd->inventory.u.items_inventory[n])) {
		clif_displaymessage (sd->fd, msg_txt(sd, 263));
		return false;
	}

	if(!map_addflooritem(&sd->inventory.u.items_inventory[n], amount, sd->bl.m, sd->bl.x, sd->bl.y, 0, 0, 0, 2, 0, false))
		return false;

	pc_delitem(sd, n, amount, 1, 0, LOG_TYPE_PICKDROP_PLAYER);
	clif_dropitem(sd, n, amount);
	return true;
}

/**
 * Attempt to pick up an item.
 * @param sd
 * @param fitem Item that will be picked
 * @return False = fail; True = success
 */
bool pc_takeitem(struct map_session_data *sd, struct flooritem_data *fitem)
{
	int flag = 0;
	unsigned int tick = gettick();
	struct party_data *p = NULL;

	nullpo_retr(false, sd);
	nullpo_retr(false, fitem);

	if(!check_distance_bl(&fitem->bl, &sd->bl, 2) && sd->ud.skill_id != BS_GREED)
		return false; //Distance is too far

	if(sd->sc.cant.pickup)
		return false;

	if(sd->status.party_id)
		p = party_search(sd->status.party_id);

	if(fitem->first_get_charid > 0 && fitem->first_get_charid != sd->status.char_id) {
		struct map_session_data *first_sd = map_charid2sd(fitem->first_get_charid);

		if(DIFF_TICK(tick,fitem->first_get_tick) < 0) {
			if(!(p && (p->party.item&1) &&
				first_sd && first_sd->status.party_id == sd->status.party_id))
				return false;
		} else if(fitem->second_get_charid > 0 && fitem->second_get_charid != sd->status.char_id) {
			struct map_session_data *second_sd = map_charid2sd(fitem->second_get_charid);

			if(DIFF_TICK(tick, fitem->second_get_tick) < 0) {
				if(!(p && (p->party.item&1) &&
					((first_sd && first_sd->status.party_id == sd->status.party_id) ||
					(second_sd && second_sd->status.party_id == sd->status.party_id))))
					return false;
			} else if(fitem->third_get_charid > 0 && fitem->third_get_charid != sd->status.char_id) {
				struct map_session_data *third_sd = map_charid2sd(fitem->third_get_charid);

				if(DIFF_TICK(tick,fitem->third_get_tick) < 0) {
					if(!(p && (p->party.item&1) &&
						((first_sd && first_sd->status.party_id == sd->status.party_id) ||
						(second_sd && second_sd->status.party_id == sd->status.party_id) ||
						(third_sd && third_sd->status.party_id == sd->status.party_id))))
						return false;
				}
			}
		}
	}

	//This function takes care of giving the item to whoever should have it, considering party-share options
	if((flag = party_share_loot(p,sd,&fitem->item, fitem->first_get_charid))) {
		clif_additem(sd,0,0,flag);
		return true;
	}

	//Display pickup animation
	pc_stop_attack(sd);
	clif_takeitem(&sd->bl,&fitem->bl);

	//Somehow, if party's pickup distribution is 'Even Share', no announcement
	if(fitem->mob_id && (itemdb_search(fitem->item.nameid))->flag.broadcast && (!p || !(p->party.item&2)))
		intif_broadcast_obtain_special_item(sd, fitem->item.nameid, fitem->mob_id, ITEMOBTAIN_TYPE_MONSTER_ITEM);

	map_clearflooritem(&fitem->bl);
	return true;
}

/**
 * Check if item is usable.
 * @param sd
 * @param n Item index in inventory
 * @param amount Amount of item
 * @return False = fail; True = success
 */
bool pc_isUseitem(struct map_session_data *sd, int n)
{
	struct item_data *item;
	unsigned short nameid;

	nullpo_retr(false, sd);

	item = sd->inventory_data[n];
	nameid = sd->inventory.u.items_inventory[n].nameid;

	if( !item )
		return false;

	if( !itemdb_is_item_usable(item) ) //Not consumable item
		return false;

	if( pc_has_permission(sd,PC_PERM_ITEM_UNCONDITIONAL) )
		return true;

	if( mapdata[sd->bl.m].flag.noitemconsumption ) //Consumable but mapflag prevent it
		return false;

	if( DIFF_TICK(sd->canuseitem_tick,gettick()) > 0 ) //Prevent mass item usage [Skotlex]
		return false;

	if( sd->state.storage_flag && item->type != IT_CASH ) {
		clif_messagecolor(&sd->bl,color_table[COLOR_RED],msg_txt(sd,388),false,SELF); // You cannot use this item while storage is open.
		return false;
	}

	if( item->flag.dead_branch && (mapdata[sd->bl.m].flag.nobranch || map_flag_gvg2(sd->bl.m)) )
		return false;

	switch( nameid ) {
		case ITEMID_ANODYNE:
			if( map_flag_gvg2(sd->bl.m) )
				return false;
			break;
		case ITEMID_WING_OF_FLY:
		case ITEMID_GIANT_FLY_WING:
		case ITEMID_N_FLY_WING:
			if( mapdata[sd->bl.m].flag.noteleport || map_flag_gvg2(sd->bl.m) ) {
				clif_skill_teleportmessage(sd,0);
				return false;
			}
			if( nameid == ITEMID_GIANT_FLY_WING ) {
				struct party_data *pd = party_search(sd->status.party_id);

				if( pd ) {
					int i;

					ARR_FIND(0,MAX_PARTY,i,(pd->data[i].sd == sd && pd->party.member[i].leader));
					if( i == MAX_PARTY ) { //User is not party leader
						clif_msg(sd,ITEM_PARTY_MEMBER_NOT_SUMMONED);
						break;
					}
					ARR_FIND(0,MAX_PARTY,i,(pd->data[i].sd && pd->data[i].sd != sd && pd->data[i].sd->bl.m == sd->bl.m && !pc_isdead(pd->data[i].sd)));
					if( i == MAX_PARTY ) { //No party members found on same map
						clif_msg(sd,ITEM_PARTY_NO_MEMBER_IN_MAP);
						break;
					}
				} else {
					clif_msg(sd,ITEM_PARTY_MEMBER_NOT_SUMMONED);
					break;
				}
			}
		//Fall through
		case ITEMID_WING_OF_BUTTERFLY:
		case ITEMID_N_BUTTERFLY_WING:
		case ITEMID_DUN_TELE_SCROLL1:
		case ITEMID_DUN_TELE_SCROLL2:
		case ITEMID_DUN_TELE_SCROLL3:
		case ITEMID_WOB_RUNE:
		case ITEMID_WOB_SCHWALTZ:
		case ITEMID_WOB_RACHEL:
		case ITEMID_WOB_LOCAL:
		case ITEMID_SIEGE_TELEPORT_SCROLL:
			if( sd->duel_group && !battle_config.duel_allow_teleport ) {
				clif_displaymessage(sd->fd, msg_txt(sd, 663));
				return false;
			}
			if( mapdata[sd->bl.m].flag.noreturn && nameid != ITEMID_WING_OF_FLY && nameid != ITEMID_GIANT_FLY_WING && nameid != ITEMID_N_FLY_WING )
				return false;
			break;
		case ITEMID_MERCENARY_RED_POTION:
		case ITEMID_MERCENARY_BLUE_POTION:
		case ITEMID_M_CENTER_POTION:
		case ITEMID_M_AWAKENING_POTION:
		case ITEMID_M_BERSERK_POTION:
			if( !sd->md || !sd->md->db )
				return false;
			if( sd->md->sc.data[SC_BERSERK] )
				return false;
			if( nameid == ITEMID_M_AWAKENING_POTION && sd->md->db->lv < 40 )
				return false;
			if( nameid == ITEMID_M_BERSERK_POTION && sd->md->db->lv < 80 )
				return false;
			break;
		case ITEMID_NEURALIZER:
			if( !mapdata[sd->bl.m].flag.reset )
				return false;
			break;
		case ITEMID_SQUID_BBQ:
			if( sd->sc.data[SC_JP_EVENT04] )
				return false;
			break;
	}

	//Mercenary Scrolls
	if( nameid >= ITEMID_BOW_MERCENARY_SCROLL1 && nameid <= ITEMID_SPEARMERCENARY_SCROLL10 && sd->md )
		return false;

	if( item->flag.group || item->type == IT_CASH ) { //Safe check type cash disappear when overweight [Napster]
		if( pc_is90overweight(sd) ) {
			clif_msg(sd,ITEM_CANT_OBTAIN_WEIGHT);
			return false;
		}
		if( !pc_inventoryblank(sd) ) {
			clif_messagecolor(&sd->bl,color_table[COLOR_RED],msg_txt(sd,1477),false,SELF); // Item cannot be open when inventory is full.
			return false;
		}
	}

	if( item->sex != 2 && sd->status.sex != item->sex ) //Gender check
		return false;

	if( (item->elv && sd->status.base_level < (unsigned int)item->elv) ||
		(item->elvmax && sd->status.base_level > (unsigned int)item->elvmax) ) { //Required level check
		clif_msg(sd,ITEM_CANT_USE_NEED_LEVEL); // You cannot use this item with your current level.
		return false;
	}

	if( !((1<<(sd->class_&MAPID_BASEMASK))&(item->class_base[sd->class_&JOBL_2_1 ? 1 : (sd->class_&JOBL_2_2 ? 2 : 0)])) )
		return false; //Not equipable by class [Skotlex]

	if( !pc_isItemClass(sd,item) )
		return false;

	//Statuses that don't let the player use items
	if( sd->sc.count &&
		(sd->sc.data[SC_BERSERK] ||
		sd->sc.data[SC_SATURDAYNIGHTFEVER] ||
		(sd->sc.data[SC_GRAVITATION] && sd->sc.data[SC_GRAVITATION]->val3 == BCT_SELF) ||
		sd->sc.data[SC_TRICKDEAD] ||
		sd->sc.data[SC_HIDING] ||
		sd->sc.data[SC__SHADOWFORM] ||
		sd->sc.data[SC__INVISIBILITY] ||
		sd->sc.data[SC__MANHOLE] ||
		sd->sc.data[SC_KAGEHUMI] ||
		(sd->sc.data[SC_NOCHAT] && sd->sc.data[SC_NOCHAT]->val1&MANNER_NOITEM) ||
		sd->sc.data[SC_KINGS_GRACE] ||
		sd->sc.data[SC_SUHIDE]) )
		return false;

	if( nameid != ITEMID_NAUTHIZ &&
		((sd->sc.opt1 && sd->sc.opt1 != OPT1_STONEWAIT && sd->sc.opt1 != OPT1_BURNING) ||
		sd->sc.data[SC_DEEPSLEEP] || sd->sc.data[SC_CRYSTALIZE]) )
		return false;

	if( item->flag.dead_branch )
		log_branch(sd);

	return true;
}

/*==========================================
 * Last checks to use an item.
 * Return:
 *	0 = fail
 *	1 = success
 *------------------------------------------*/
int pc_useitem(struct map_session_data *sd, int n)
{
	unsigned int tick = gettick();
	int amount;
	unsigned short nameid;
	struct script_code *script;
	struct item item;
	struct item_data *id;

	nullpo_ret(sd);

	if( sd->npc_id ) { //This flag enables you to use items while in an NPC [Skotlex]
		if( sd->progressbar.npc_id ) {
			clif_progressbar_abort(sd);
			return 0;
		}
#ifdef RENEWAL
		clif_msg(sd,WORK_IN_PROGRESS);
		return 0;
#else
		if( !sd->npc_item_flag )
			return 0;
#endif
	}

	if( sd->npc_shopid )
		return 0;

	item = sd->inventory.u.items_inventory[n];
	id = sd->inventory_data[n];

	if( !item.nameid || item.amount <= 0 )
		return 0;

	if( !pc_isUseitem(sd,n) )
		return 0;

	//Store information for later use before it is lost (via pc_delitem) [Paradox924X]
	nameid = id->nameid;

	if( id->flag.restricted_consume ) {
		if( pc_issit(sd) ) {
			clif_msg(sd,ITEM_NOUSE_SITTING); // You cannot use this item while sitting.
			return 0;
		}
		if( sd->sc.data[SC_ALL_RIDING] && nameid != ITEMID_BOARDING_HALTER )
			return 0; //Items with delayed consume are not meant to work while in mounts except ITEMID_BOARDING_HALTER
	}

	if( id->delay > 0 && !pc_has_permission(sd,PC_PERM_ITEM_UNCONDITIONAL) && pc_itemcd_check(sd,id,tick,n) )
		return 0;

	//On restricted maps the item is consumed but the effect is not used
	if( !pc_has_permission(sd,PC_PERM_ITEM_UNCONDITIONAL) && itemdb_isNoEquip(id,sd->bl.m) ) {
		clif_msg(sd,ITEM_CANT_USE_AREA); // This item cannot be used within this area.
		//Need confirmation for delayed consumption items
		if( battle_config.allow_consume_restricted_item && !item.expire_time ) {
			clif_useitemack(sd,n,item.amount - 1,true);
			pc_delitem(sd,n,1,1,0,LOG_TYPE_CONSUME);
		}
		return 0; //Regardless, effect is not run
	}

	sd->itemid = item.nameid;
	sd->itemindex = n;

	if( sd->catch_target_class != PET_CATCH_FAIL ) //Abort pet catching
		sd->catch_target_class = PET_CATCH_FAIL;

	amount = item.amount;
	script = id->script;

	if( item.card[0] == CARD0_CREATE && pc_famerank(MakeDWord(item.card[2],item.card[3]), MAPID_ALCHEMIST) )
		potion_flag = 2; //Famous player's potions have 50% more efficiency

	//Update item use time
	sd->canuseitem_tick = tick + battle_config.item_use_interval;

	run_script(script,0,sd->bl.id,fake_nd->bl.id);
	potion_flag = 0;

	//Check if the item is to be consumed immediately [Skotlex]
	if( id->flag.keepAfterUse || ((nameid == ITEMID_EARTH_SCROLL_1_3 || ITEMID_EARTH_SCROLL_1_5) &&
		sd->sc.data[SC_EARTHSCROLL] && rnd()%100 >= sd->sc.data[SC_EARTHSCROLL]->val2) ) //[marquis007]
		clif_useitemack(sd,n,amount,true); //Do not consume item
	else {
		clif_useitemack(sd,n,amount - 1,true);
		pc_delitem(sd,n,1,1,0,LOG_TYPE_CONSUME);
	}

	return 1;
}

/**
 * Add item on cart for given index.
 * @param sd
 * @param item
 * @param amount
 * @param log_type
 * @return see e_additem_result
 */
enum e_additem_result pc_cart_additem(struct map_session_data *sd, struct item *item, int amount, e_log_pick_type log_type)
{
	struct item_data *data;
	int i, w;

	nullpo_retr(ADDITEM_INVALID, sd);
	nullpo_retr(ADDITEM_INVALID, item);

	if( !(item->nameid) || amount <= 0 )
		return ADDITEM_INVALID;

	if( itemdb_ishatched_egg(item) )
		return ADDITEM_INVALID;

	data = itemdb_search(item->nameid);
	if( data->stack.cart && amount > data->stack.amount ) //Item stack limitation
		return ADDITEM_STACKLIMIT;

	if( !itemdb_cancartstore(item, pc_get_group_level(sd)) ||
		(item->bound > BOUND_ACCOUNT && !pc_can_give_bounded_items(sd)) ) { //Check item trade restrictions [Skotlex]
		clif_displaymessage(sd->fd, msg_txt(sd, 264));
		return ADDITEM_INVALID;
	}

	if( (w = data->weight * amount) + sd->cart_weight > sd->cart_weight_max )
		return ADDITEM_OVERWEIGHT;

	i = MAX_CART;
	if( itemdb_isstackable2(data) && !item->expire_time ) {
		for( i = 0; i < MAX_CART; i++ ) {
			if( sd->cart.u.items_cart[i].nameid == item->nameid &&
				sd->cart.u.items_cart[i].bound == item->bound &&
				sd->cart.u.items_cart[i].unique_id == item->unique_id &&
				!memcmp(sd->cart.u.items_cart[i].card, item->card, sizeof(item->card)) )
				break;
		}
	}

	if( i < MAX_CART ) { //Item already in cart, stack it
		if( amount > MAX_AMOUNT - sd->cart.u.items_cart[i].amount ||
			(data->stack.cart && amount > data->stack.amount - sd->cart.u.items_cart[i].amount) )
			return ADDITEM_OVERAMOUNT;
		sd->cart.u.items_cart[i].amount += amount;
		clif_cart_additem(sd,i,amount,0);
	} else { //Item not stackable or not present, add it
		ARR_FIND(0, MAX_CART, i, sd->cart.u.items_cart[i].nameid == 0);
		if( i == MAX_CART )
			return ADDITEM_OVERITEM;
		memcpy(&sd->cart.u.items_cart[i], item, sizeof(sd->cart.u.items_cart[0]));
		sd->cart.u.items_cart[i].id = 0;
		sd->cart.u.items_cart[i].amount = amount;
		sd->cart_num++;
		clif_cart_additem(sd,i,amount,0);
	}
	sd->cart.u.items_cart[i].favorite = 0; //Clear
	sd->cart.u.items_cart[i].equipSwitch = 0;
	log_pick_pc(sd, log_type, amount, &sd->cart.u.items_cart[i]);

	sd->cart_weight += w;
	clif_updatestatus(sd,SP_CARTINFO);

	return ADDITEM_SUCCESS;
}

/**
 * Delete item on cart for given index.
 * @param sd
 * @param n
 * @param amount
 * @param type
 * @param log_type
 */
void pc_cart_delitem(struct map_session_data *sd, int n, int amount, int type, e_log_pick_type log_type)
{
	nullpo_retv(sd);

	if(sd->cart.u.items_cart[n].nameid == 0 ||
	   sd->cart.u.items_cart[n].amount < amount)
		return;

	log_pick_pc(sd, log_type, -amount, &sd->cart.u.items_cart[n]);

	sd->cart.u.items_cart[n].amount -= amount;
	sd->cart_weight -= itemdb_weight(sd->cart.u.items_cart[n].nameid) * amount;
	if(sd->cart.u.items_cart[n].amount <= 0) {
		memset(&sd->cart.u.items_cart[n],0,sizeof(sd->cart.u.items_cart[0]));
		sd->cart_num--;
	}
	if(!type) {
		clif_cart_delitem(sd,n,amount);
		clif_updatestatus(sd,SP_CARTINFO);
	}
}

/**
 * Transfer item from inventory to cart.
 * @param sd
 * @param idx
 * @param amount
 */
void pc_putitemtocart(struct map_session_data *sd, int idx, int amount)
{
	struct item *item_data;
	enum e_additem_result flag;

	nullpo_retv(sd);

	if( idx < 0 || idx >= MAX_INVENTORY ) //Invalid index check [Skotlex]
		return;

	item_data = &sd->inventory.u.items_inventory[idx];

	if( !item_data->nameid || amount < 1 || item_data->amount < amount || sd->state.vending || sd->state.prevend )
		return;

	if( item_data->equipSwitch ) {
		clif_msg(sd,ITEM_EQUIP_SWITCH);
		return;
	}

	if( (flag = pc_cart_additem(sd,item_data,amount,LOG_TYPE_NONE)) == ADDITEM_SUCCESS )
		pc_delitem(sd,idx,amount,0,5,LOG_TYPE_NONE);
	else {
		clif_cart_additem_ack(sd,(flag == ADDITEM_OVERAMOUNT) ? ADDITEM_TO_CART_FAIL_COUNT : ADDITEM_TO_CART_FAIL_WEIGHT);
		clif_additem(sd,idx,amount,0);
		clif_delitem(sd,idx,amount,0);
	}
}

/*==========================================
 * Get number of item in cart.
 * Return:
 *	-1 = itemid not found or no amount found
 *	x = remaining itemid on cart after get
 *------------------------------------------*/
int pc_cartitem_amount(struct map_session_data *sd, int idx, int amount)
{
	struct item *item_data;

	nullpo_retr(-1, sd);

	item_data = &sd->cart.u.items_cart[idx];
	if( item_data->nameid == 0 || item_data->amount == 0 )
		return -1;

	return item_data->amount - amount;
}

/**
 * Retrieve an item at index idx from cart.
 * @param sd
 * @param idx
 * @param amount
 */
void pc_getitemfromcart(struct map_session_data *sd, int idx, int amount)
{
	struct item *item_data;
	unsigned char flag = 0;

	nullpo_retv(sd);

	if( idx < 0 || idx >= MAX_CART ) //Invalid index check [Skotlex]
		return;

	item_data =& sd->cart.u.items_cart[idx];

	if( !item_data->nameid || amount < 1 || item_data->amount < amount || sd->state.vending || sd->state.prevend )
		return;

	if( (flag = pc_additem(sd,item_data,amount,LOG_TYPE_NONE)) == ADDITEM_SUCCESS )
		pc_cart_delitem(sd,idx,amount,0,LOG_TYPE_NONE);
	else {
		clif_cart_delitem(sd,idx,amount);
		clif_additem(sd,idx,amount,flag);
		clif_cart_additem(sd,idx,amount,0);
	}
}

/*==========================================
 * Bound Item Check
 * Type:
 * 1 Account Bound
 * 2 Guild Bound
 * 3 Party Bound
 * 4 Character Bound
 *------------------------------------------*/
int pc_bound_chk(TBL_PC *sd, enum bound_type type, int *idxlist)
{
	int i = 0, j = 0;

	for( i = 0; i < MAX_INVENTORY; i++ ) {
		if( sd->inventory.u.items_inventory[i].nameid > 0 && sd->inventory.u.items_inventory[i].amount > 0 && sd->inventory.u.items_inventory[i].bound == type ) {
			idxlist[j] = i;
			j++;
		}
	}
	return j;
}

/*==========================================
 *  Display item stolen msg to player sd
 *------------------------------------------*/
int pc_show_steal(struct block_list *bl, va_list ap)
{
	struct map_session_data *sd;
	int itemid;
	struct item_data *item = NULL;
	char output[100];

	sd = va_arg(ap, struct map_session_data *);
	itemid = va_arg(ap, int);

	if( !(item = itemdb_exists(itemid)) )
		sprintf(output, "%s stole an Unknown Item (id: %i).", sd->status.name, itemid);
	else
		sprintf(output, "%s stole %s.", sd->status.name, item->jname);
	clif_displaymessage(((struct map_session_data *)bl)->fd, output);

	return 0;
}

/**
 * Steal an item from bl (mob).
 * @param sd: Player data
 * @param bl: Object to steal from
 * @param skill_lv: Level of skill used
 * @return True on success or false otherwise
 */
bool pc_steal_item(struct map_session_data *sd, struct block_list *bl, uint16 skill_lv)
{
	int i, itemid;
	int stealRate;
#ifndef RENEWAL
	double stealBonus;
#endif
	unsigned char flag = 0;
	struct status_data *sd_status, *md_status;
	struct mob_data *md;
	struct item tmp_item;

	if( !sd || !bl || bl->type != BL_MOB )
		return false;

	md = (TBL_MOB *)bl;

	if( md->state.steal_flag == UCHAR_MAX || (md->sc.opt1 && md->sc.opt1 != OPT1_BURNING) )
		return false; //Already stolen from status change check

	sd_status = status_get_status_data(&sd->bl);
	md_status = status_get_status_data(bl);

	if( md->master_id || status_has_mode(md_status,MD_STATUS_IMMUNE) || status_get_race2(&md->bl) == RC2_TREASURE ||
		mapdata[bl->m].flag.nomobloot || //Check noloot map flag [Lorky]
		(battle_config.skill_steal_max_tries && //Reached limit of steal attempts [Lupus]
		md->state.steal_flag++ >= battle_config.skill_steal_max_tries) )
	{ //Can't steal from
		md->state.steal_flag = UCHAR_MAX;
		return false;
	}

	//Base skill success chance (percentual)
	stealRate = (sd_status->dex - md_status->dex) / 2 + skill_lv * 6 + 4;
	stealRate += sd->bonus.add_steal_rate;
	stealRate = max(stealRate,0);

	if( !stealRate )
		return false;

#ifndef RENEWAL
	stealBonus = stealRate / 100.;
#else
	if( rnd()%100 >= stealRate )
		return false;
#endif

	//Try dropping one item, in the order from first to last possible slot
	//Droprate is affected by the skill success rate
	for( i = 0; i < MAX_MOB_DROP; i++ ) {
		int dropRate;

		if( !md->db->dropitem[i].nameid || md->db->dropitem[i].steal_protected || !itemdb_exists(md->db->dropitem[i].nameid) )
			continue;
		dropRate = md->db->dropitem[i].p;
#ifndef RENEWAL
		dropRate *= stealBonus;
#endif
		if( rnd()%10000 < dropRate )
			break; //Success
	}
	if( i == MAX_MOB_DROP )
		return false;

	itemid = md->db->dropitem[i].nameid;
	memset(&tmp_item,0,sizeof(tmp_item));
	tmp_item.nameid = itemid;
	tmp_item.amount = 1;
	tmp_item.identify = itemdb_isidentified(itemid);
	mob_setdropitem_option(&tmp_item,&md->db->dropitem[i]);
	flag = pc_additem(sd,&tmp_item,1,LOG_TYPE_PICKDROP_PLAYER);

	//@TODO: Should we disable stealing when the item you stole couldn't be added to your inventory?
	//       Perhaps players will figure out a way to exploit this behaviour otherwise?
	md->state.steal_flag = UCHAR_MAX; //You can't steal from this mob any more

	if( flag ) { //Failed to steal due to overweight
		clif_additem(sd,0,0,flag);
		return false;
	}

	if( battle_config.show_steal_in_same_party )
		party_foreachsamemap(pc_show_steal,sd,AREA_SIZE,sd,tmp_item.nameid);

	//Logs items, Stolen from mobs [Lupus]
	log_pick_mob(md,LOG_TYPE_STEAL,-1,&tmp_item);

	//A Rare Steal Global Announce by Lupus
	if( md->db->dropitem[i].p <= battle_config.rare_drop_announce ) {
		struct item_data *i_data = NULL;
		char message[128];

		i_data = itemdb_search(itemid);
		sprintf(message,msg_txt(NULL,542),(sd->status.name[0] ? sd->status.name : "GM"),md->db->jname,i_data->jname,(float)md->db->dropitem[i].p / 100);
		//MSG: "'%s' stole %s's %s (chance: %0.02f%%)"
		intif_broadcast(message,strlen(message) + 1,BC_DEFAULT);
	}
	return true;
}

/**
 * Steals zeny from a monster through the RG_STEALCOIN skill.
 * @param sd: Source character
 * @param bl: Target monster
 * @return Amount of stolen zeny (0 in case of failure)
 */
int pc_steal_coin(struct map_session_data *sd, struct block_list *bl)
{
	struct mob_data *md = NULL;
	uint8 lv;
	int rate;

	if( !sd || !bl || bl->type != BL_MOB )
		return 0;

	md = (TBL_MOB *)bl;

	if( md->state.steal_coin_flag || md->sc.data[SC_STONE] || md->sc.data[SC_FREEZE] ||
		status_bl_has_mode(bl,MD_STATUS_IMMUNE) || status_get_race2(bl) == RC2_TREASURE )
		return 0;

	lv = pc_checkskill(sd,RG_STEALCOIN);
	rate = lv * 10 + (sd->status.base_level - md->level) * 2 + sd->battle_status.dex / 2 + sd->battle_status.luk / 2;

	if( rnd()%1000 < rate ) {
		//mob_lv * skill_lv / 10 + random[mob_lv * 8, mob_lv * 10]
		int amount = md->level * lv / 10 + md->level * 8 + rnd()%(md->level * 2 + 1);

		pc_getzeny(sd,amount,LOG_TYPE_STEAL,NULL);
		md->state.steal_coin_flag = 1;
		return amount;
	}
	return 0;
}

/**
 * Set's a player position.
 * @param sd
 * @param mapindex
 * @param x
 * @param y
 * @param clrtype
 * @return SETPOS_OK           Success
 *         SETPOS_MAPINDEX     Invalid map index
 *         SETPOS_NO_MAPSERVER Map not in this map-server, and failed to locate alternate map-server.
 *         SETPOS_AUTOTRADE    Player is in autotrade state
 */
enum e_setpos pc_setpos(struct map_session_data *sd, unsigned short mapindex, int x, int y, clr_type clrtype)
{
	int16 m;

	nullpo_retr(SETPOS_OK, sd);

	if( !mapindex || !mapindex_id2name(mapindex) ) {
		ShowDebug("pc_setpos: Passed mapindex(%d) is invalid!\n", mapindex);
		return SETPOS_MAPINDEX;
	}

	if( !battle_config.feature_autotrade_move && sd->state.autotrade && (sd->vender_id || sd->buyer_id) )
		return SETPOS_AUTOTRADE;

	if( battle_config.revive_onwarp && pc_isdead(sd) ) { // Revive dead people before warping them
		pc_setstand(sd);
		pc_setrestartvalue(sd,1);
	}

	m = map_mapindex2mapid(mapindex);

	sd->state.changemap = (sd->mapindex != mapindex);
	sd->state.warping = 1;

	if( sd->state.changemap ) { // Misc map-changing settings
		uint16 current_map_instance_id = mapdata[sd->bl.m].instance_id;
		uint16 new_map_instance_id = mapdata[m].instance_id;
		int i;

		if( current_map_instance_id != new_map_instance_id ) {
			if( current_map_instance_id )
				instance_delusers(current_map_instance_id); // Update instance timer for the map on leave
			if( new_map_instance_id )
				instance_addusers(new_map_instance_id); // Update instance timer for the map on enter
		}
		sd->state.pmap = sd->bl.m;
		if( sd->sc.count ) { // Cancel some map related stuff
			if( sd->sc.data[SC_JAILED] )
				return SETPOS_MAPINDEX; // You may not get out!
			status_change_end(&sd->bl, SC_BOSSMAPINFO, INVALID_TIMER);
			status_change_end(&sd->bl, SC_CLOAKING, INVALID_TIMER);
			status_change_end(&sd->bl, SC_WARM, INVALID_TIMER);
			status_change_end(&sd->bl, SC_SUN_COMFORT, INVALID_TIMER);
			status_change_end(&sd->bl, SC_MOON_COMFORT, INVALID_TIMER);
			status_change_end(&sd->bl, SC_STAR_COMFORT, INVALID_TIMER);
			status_change_end(&sd->bl, SC_MIRACLE, INVALID_TIMER);
			if( sd->sc.data[SC_KNOWLEDGE] ) {
				struct status_change_entry *sce = sd->sc.data[SC_KNOWLEDGE];

				if( sce->timer != INVALID_TIMER )
					delete_timer(sce->timer, status_change_timer);
				sce->timer = add_timer(gettick() + skill_get_time(SG_KNOWLEDGE, sce->val1), status_change_timer, sd->bl.id, SC_KNOWLEDGE);
			}
			status_change_end(&sd->bl, SC_CLOAKINGEXCEED, INVALID_TIMER);
			status_change_end(&sd->bl, SC_PROPERTYWALK, INVALID_TIMER);
			status_change_end(&sd->bl, SC_NEWMOON, INVALID_TIMER);
		}
		for( i = 0; i < EQI_MAX; i++ ) {
			if( sd->equip_index[i] >= 0 && pc_isequip(sd, sd->equip_index[i]) )
				pc_unequipitem(sd, sd->equip_index[i], 2);
		}
		if( battle_config.clear_unit_onwarp&BL_PC )
			skill_clear_unitgroup(&sd->bl);
		party_send_dot_remove(sd); // Minimap dot fix [Kevin]
		guild_send_dot_remove(sd);
		bg_send_dot_remove(sd);
		if( sd->regen.state.gc )
			sd->regen.state.gc = 0;
		// Make sure vending is allowed here
		if( sd->state.vending && mapdata[m].flag.novending ) {
			clif_displaymessage (sd->fd, msg_txt(sd, 276)); // "You can't open a shop on this map."
			vending_closevending(sd);
		}
		channel_pcquit(sd, 4); // Quit map channel
	}

	if( m < 0 ) {
		uint32 ip;
		uint16 port;

		// If can't find any map-servers, just abort setting position
		if( !sd->mapindex || map_mapname2ipport(mapindex, &ip, &port) )
			return SETPOS_NO_MAPSERVER;
		if( sd->npc_id )
			npc_event_dequeue(sd);
		npc_script_event(sd, NPCE_LOGOUT);
		// Remove from map, THEN change x/y coordinates
		unit_remove_map_pc(sd, clrtype);
		sd->mapindex = mapindex;
		sd->bl.x = x;
		sd->bl.y = y;
		pc_clean_skilltree(sd);
		chrif_save(sd, CSAVE_CHANGE_MAPSERV|CSAVE_INVENTORY|CSAVE_CART);
		chrif_changemapserver(sd, ip, (short)port);
		// Free session data from this map server [Kevin]
		unit_free_pc(sd);
		return SETPOS_OK;
	}

	if( x < 0 || x >= mapdata[m].xs || y < 0 || y >= mapdata[m].ys ) {
		ShowError("pc_setpos: attempt to place player '%s' (%d:%d) on invalid coordinates (%s-%d,%d)\n", sd->status.name, sd->status.account_id, sd->status.char_id, mapindex_id2name(mapindex), x, y);
		x = y = 0; // Make it random
	}

	if( x == 0 && y == 0 ) { // Pick a random walkable cell
		int c = 0;

		do {
			x = rnd()%(mapdata[m].xs - 2) + 1;
			y = rnd()%(mapdata[m].ys - 2) + 1;
			c++;
			if( c > (mapdata[m].xs * mapdata[m].ys) * 3 ) { //Force out
				ShowError("pc_setpos: couldn't found a valid coordinates for player '%s' (%d:%d) on (%s), preventing warp\n", sd->status.name, sd->status.account_id, sd->status.char_id, mapindex_id2name(mapindex));
				return SETPOS_OK; //Preventing warp
				//break; //Allow warp anyway
			}
		} while( map_getcell(m, x, y, CELL_CHKNOPASS) || (!battle_config.teleport_on_portal && npc_check_areanpc(1, m, x, y, 1)) );
	}

	if( sd->state.vending && map_getcell(m, x, y, CELL_CHKNOVENDING) ) {
		clif_displaymessage(sd->fd, msg_txt(sd, 204)); // "You can't open a shop on this cell."
		vending_closevending(sd);
	}

	if( sd->bl.prev ) {
		unit_remove_map_pc(sd, clrtype);
		clif_changemap(sd, m, x, y); // [MouseJstr]
	} else if( sd->state.active ) // Tag player for rewarping after map-loading is done [Skotlex]
		sd->state.rewarp = 1;

	sd->mapindex = mapindex;
	sd->bl.m = m;
	sd->bl.x = sd->ud.to_x = x;
	sd->bl.y = sd->ud.to_y = y;

	if( sd->status.guild_id > 0 && mapdata[m].flag.gvg_castle ) { // Increased guild castle regen [Valaris]
		struct guild_castle *gc = guild_mapindex2gc(sd->mapindex);

		if( gc && gc->guild_id == sd->status.guild_id )
			sd->regen.state.gc = 1;
	}

	if( sd->status.pet_id > 0 && sd->pd && sd->pd->pet.intimate >= PETINTIMATE_AWKWARD ) {
		sd->pd->bl.m = m;
		sd->pd->bl.x = sd->pd->ud.to_x = x;
		sd->pd->bl.y = sd->pd->ud.to_y = y;
		sd->pd->ud.dir = sd->ud.dir;
	}

	if( hom_is_active(sd->hd) ) {
		sd->hd->bl.m = m;
		sd->hd->bl.x = sd->hd->ud.to_x = x;
		sd->hd->bl.y = sd->hd->ud.to_y = y;
		sd->hd->ud.dir = sd->ud.dir;
	}

	if( sd->md ) {
		sd->md->bl.m = m;
		sd->md->bl.x = sd->md->ud.to_x = x;
		sd->md->bl.y = sd->md->ud.to_y = y;
		sd->md->ud.dir = sd->ud.dir;
	}

	if( sd->ed ) {
		sd->ed->bl.m = m;
		sd->ed->bl.x = sd->ed->ud.to_x = x;
		sd->ed->bl.y = sd->ed->ud.to_y = y;
		sd->ed->ud.dir = sd->ud.dir;
	}

	pc_cell_basilica(sd);

	//Check if we gonna be rewarped [lighta]
	if( npc_check_areanpc(1, m, x, y, 0) )
		sd->count_rewarp++;
	else
		sd->count_rewarp = 0;

	//Given autotrades have no clients you have to trigger this manually
	//Otherwise they get stuck in memory limbo bugreport:7495
	if( battle_config.feature_autotrade_move && sd->state.autotrade && (sd->vender_id || sd->buyer_id) )
		clif_parse_LoadEndAck(0, sd);

	return SETPOS_OK;
}

/**
 * Warp player sd to random location on current map.
 * May fail if no walkable cell found (1000 attempts).
 * Return:
 *	0 = Success
 *	1,2,3 = Fail
 */
char pc_randomwarp(struct map_session_data *sd, clr_type type)
{
	int x, y, i = 0;
	int16 m;

	nullpo_ret(sd);

	m = sd->bl.m;

	if (mapdata[sd->bl.m].flag.noteleport) //Teleport forbidden
		return 3;

	do {
		x = rnd()%(mapdata[m].xs - 2) + 1;
		y = rnd()%(mapdata[m].ys - 2) + 1;
	} while ((map_getcell(m,x,y,CELL_CHKNOPASS) || (!battle_config.teleport_on_portal && npc_check_areanpc(1,m,x,y,1))) && (i++) < 1000);

	if (i < 1000)
		return pc_setpos(sd,map_id2index(sd->bl.m),x,y,type);

	return 3;
}

/*==========================================
 * Records a memo point at sd's current position
 * pos - entry to replace, (-1: shift oldest entry out)
 *------------------------------------------*/
bool pc_memo(struct map_session_data *sd, int pos)
{
	uint16 lv;

	nullpo_retr(false, sd);

	// Check mapflags
	if( sd->bl.m >= 0 && (mapdata[sd->bl.m].flag.nomemo || mapdata[sd->bl.m].flag.nowarpto) && !pc_has_permission(sd, PC_PERM_WARP_ANYWHERE) ) {
		clif_skill_teleportmessage(sd, 1); // "Saved point cannot be memorized."
		return false;
	}
	// Check inputs
	if( pos < -1 || pos >= MAX_MEMOPOINTS )
		return false; // Invalid input
	// Check required skill level
	lv = pc_checkskill(sd, AL_WARP);
	if( lv < 1 ) {
		clif_skill_memomessage(sd, 2); // "You haven't learned Warp."
		return false;
	}
	if( lv < 2 || lv - 2 < pos ) {
		clif_skill_memomessage(sd, 1); // "Skill Level is not high enough."
		return false;
	}
	if( pos == -1 ) {
		uint8 i;

		// Prevent memo-ing the same map multiple times
		ARR_FIND(0, MAX_MEMOPOINTS, i, sd->status.memo_point[i].map == map_id2index(sd->bl.m));
		memmove(&sd->status.memo_point[1], &sd->status.memo_point[0], (u8min(i,MAX_MEMOPOINTS - 1)) * sizeof(struct point));
		pos = 0;
	}
	if( mapdata[sd->bl.m].instance_id ) {
		clif_displaymessage(sd->fd, msg_txt(sd, 384)); // You cannot create a memo in an instance.
		return false;
	}
	sd->status.memo_point[pos].map = map_id2index(sd->bl.m);
	sd->status.memo_point[pos].x = sd->bl.x;
	sd->status.memo_point[pos].y = sd->bl.y;

	clif_skill_memomessage(sd, 0);

	return true;
}

//
// Skills
//
/*==========================================
 * Return player sd skill_lv learned for given skill
 *------------------------------------------*/
uint8 pc_checkskill(struct map_session_data *sd, uint16 skill_id)
{
	if( !sd )
		return 0;

	if( skill_id >= GD_SKILLBASE && skill_id < GD_MAX ) {
		struct guild *g = NULL;

		if( sd->status.guild_id > 0 && (g = sd->guild) )
			return guild_checkskill(g,skill_id);
		return 0;
	} else if( skill_id >= ARRAYLENGTH(sd->status.skill) ) {
		ShowError("pc_checkskill: Invalid skill id %d (char_id=%d).\n", skill_id, sd->status.char_id);
		return 0;
	}

	if( sd->status.skill[skill_id].id == skill_id )
		return (sd->status.skill[skill_id].lv);

	return 0;
}

uint8 pc_checkskill_summoner(struct map_session_data *sd, enum e_summoner_type type)
{
	uint8 count = 0;

	if( !sd )
		return 0;

	switch( type ) {
		case TYPE_SEAFOOD:
			count = pc_checkskill(sd,SU_TUNABELLY) + pc_checkskill(sd,SU_TUNAPARTY) + pc_checkskill(sd,SU_BUNCHOFSHRIMP) + pc_checkskill(sd,SU_FRESHSHRIMP) +
				pc_checkskill(sd,SU_GROOMING) + pc_checkskill(sd,SU_PURRING) + pc_checkskill(sd,SU_SHRIMPARTY);
			break;
		case TYPE_PLANT:
			count = pc_checkskill(sd,SU_SV_STEMSPEAR) + pc_checkskill(sd,SU_CN_POWDERING) + pc_checkskill(sd,SU_CN_METEOR) + pc_checkskill(sd,SU_SV_ROOTTWIST) +
				pc_checkskill(sd,SU_CHATTERING) + pc_checkskill(sd,SU_MEOWMEOW) + pc_checkskill(sd,SU_NYANGGRASS);
			break;
		case TYPE_ANIMAL:
			count = pc_checkskill(sd,SU_SCAROFTAROU) + pc_checkskill(sd,SU_PICKYPECK) + pc_checkskill(sd,SU_ARCLOUSEDASH) + pc_checkskill(sd,SU_LUNATICCARROTBEAT) +
				pc_checkskill(sd,SU_HISS) + pc_checkskill(sd,SU_POWEROFFLOCK) + pc_checkskill(sd,SU_SVG_SPIRIT);
			break;
	}

	return count;
}

/**
 * Check if we still have the correct weapon to continue the skill (actually status)
 * If not ending it
 * @param sd
 * @return 0: Error, 1: Check done
 */
static void pc_checkallowskill(struct map_session_data *sd)
{
	const enum sc_type scw_list[] = {
		SC_TWOHANDQUICKEN,
		SC_ONEHAND,
		SC_AURABLADE,
		SC_PARRYING,
		SC_SPEARQUICKEN,
		SC_ADRENALINE,
		SC_ADRENALINE2,
		SC_DANCING,
		SC_GATLINGFEVER,
	};
	uint8 i;

	nullpo_retv(sd);

	if(!sd->sc.count)
		return;

	for(i = 0; i < ARRAYLENGTH(scw_list); i++) { //Skills requiring specific weapon types
		if(scw_list[i] == SC_DANCING && !battle_config.dancing_weaponswitch_fix)
			continue;
		if(sd->sc.data[scw_list[i]] && !pc_check_weapontype(sd, skill_get_weapontype(status_sc2skill(scw_list[i]))))
			status_change_end(&sd->bl, scw_list[i], INVALID_TIMER);
	}

	//SC_STRUP requires bare hands
	if(sd->sc.data[SC_STRUP] && sd->status.weapon != W_FIST)
		status_change_end(&sd->bl, SC_STRUP, INVALID_TIMER);

	if(sd->status.shield <= 0) { //Skills requiring a shield
		const enum sc_type scs_list[] = {
			SC_AUTOGUARD,
			SC_DEFENDER,
			SC_REFLECTSHIELD,
			SC_REFLECTDAMAGE
		};

		for(i = 0; i < ARRAYLENGTH(scs_list); i++)
			if(sd->sc.data[scs_list[i]])
				status_change_end(&sd->bl, scs_list[i], INVALID_TIMER);
	}
}

/*==========================================
 * Return equiped itemid? on player sd at pos
 * Return
 *	-1  : mean nothing equiped
 *	idx : (this index could be used in inventory to found item_data)
 * checkall : check all position for equip switch
 *------------------------------------------*/
short pc_checkequip(struct map_session_data *sd, int pos, bool checkall)
{
	uint8 i;

	nullpo_retr(-1, sd);

	for(i = 0; i < EQI_MAX; i++) {
		if(pos&equip_bitmask[i]) {
			if(checkall && (pos&~equip_bitmask[i]) != 0 && sd->equip_index[i] == -1)
				continue; //Check all if any match is found
			return sd->equip_index[i];
		}
	}

	return -1;
}

/**
 * Check if sd as nameid equiped somewhere
 * @sd : the player session
 * @nameid : id of the item to check
 * @min : : see pc.h enum equip_index from ? to @max
 * @max : see pc.h enum equip_index for @min to ?
 * Return
 *	0 : No nameid equiped
 */
bool pc_checkequip2(struct map_session_data *sd, unsigned short nameid, int min, int max)
{
	int i;

	for(i = min; i < max; i++) {
		if(equip_bitmask[i]) {
			short idx = sd->equip_index[i];

			if(sd->inventory.u.items_inventory[idx].nameid == nameid)
				return 1;
		}
	}

	return 0;
}

/*==========================================
 * Convert's from the client's lame Job ID system
 * to the map server's 'makes sense' system. [Skotlex]
 *------------------------------------------*/
int pc_jobid2mapid(unsigned short b_class)
{
	switch(b_class) {
		//Novice And 1-1 Jobs
		case JOB_NOVICE:                return MAPID_NOVICE;
		case JOB_SWORDMAN:              return MAPID_SWORDMAN;
		case JOB_MAGE:                  return MAPID_MAGE;
		case JOB_ARCHER:                return MAPID_ARCHER;
		case JOB_ACOLYTE:               return MAPID_ACOLYTE;
		case JOB_MERCHANT:              return MAPID_MERCHANT;
		case JOB_THIEF:                 return MAPID_THIEF;
		case JOB_TAEKWON:               return MAPID_TAEKWON;
		case JOB_WEDDING:               return MAPID_WEDDING;
		case JOB_GUNSLINGER:            return MAPID_GUNSLINGER;
		case JOB_NINJA:                 return MAPID_NINJA;
		case JOB_XMAS:                  return MAPID_XMAS;
		case JOB_SUMMER:                return MAPID_SUMMER;
		case JOB_HANBOK:                return MAPID_HANBOK;
		case JOB_GANGSI:                return MAPID_GANGSI;
		case JOB_OKTOBERFEST:           return MAPID_OKTOBERFEST;
		case JOB_SUMMER2:               return MAPID_SUMMER2;
		//2-1 Jobs
		case JOB_SUPER_NOVICE:          return MAPID_SUPER_NOVICE;
		case JOB_KNIGHT:                return MAPID_KNIGHT;
		case JOB_WIZARD:                return MAPID_WIZARD;
		case JOB_HUNTER:                return MAPID_HUNTER;
		case JOB_PRIEST:                return MAPID_PRIEST;
		case JOB_BLACKSMITH:            return MAPID_BLACKSMITH;
		case JOB_ASSASSIN:              return MAPID_ASSASSIN;
		case JOB_STAR_GLADIATOR:        return MAPID_STAR_GLADIATOR;
		case JOB_REBELLION:             return MAPID_REBELLION;
		case JOB_KAGEROU:
		case JOB_OBORO:                 return MAPID_KAGEROUOBORO;
		case JOB_DEATH_KNIGHT:          return MAPID_DEATH_KNIGHT;
		//2-2 Jobs
		case JOB_CRUSADER:              return MAPID_CRUSADER;
		case JOB_SAGE:                  return MAPID_SAGE;
		case JOB_BARD:
		case JOB_DANCER:                return MAPID_BARDDANCER;
		case JOB_MONK:                  return MAPID_MONK;
		case JOB_ALCHEMIST:             return MAPID_ALCHEMIST;
		case JOB_ROGUE:                 return MAPID_ROGUE;
		case JOB_SOUL_LINKER:           return MAPID_SOUL_LINKER;
		case JOB_DARK_COLLECTOR:        return MAPID_DARK_COLLECTOR;
		//Trans Novice And Trans 1-1 Jobs
		case JOB_NOVICE_HIGH:           return MAPID_NOVICE_HIGH;
		case JOB_SWORDMAN_HIGH:         return MAPID_SWORDMAN_HIGH;
		case JOB_MAGE_HIGH:             return MAPID_MAGE_HIGH;
		case JOB_ARCHER_HIGH:           return MAPID_ARCHER_HIGH;
		case JOB_ACOLYTE_HIGH:          return MAPID_ACOLYTE_HIGH;
		case JOB_MERCHANT_HIGH:         return MAPID_MERCHANT_HIGH;
		case JOB_THIEF_HIGH:            return MAPID_THIEF_HIGH;
		//Trans 2-1 Jobs
		case JOB_LORD_KNIGHT:           return MAPID_LORD_KNIGHT;
		case JOB_HIGH_WIZARD:           return MAPID_HIGH_WIZARD;
		case JOB_SNIPER:                return MAPID_SNIPER;
		case JOB_HIGH_PRIEST:           return MAPID_HIGH_PRIEST;
		case JOB_WHITESMITH:            return MAPID_WHITESMITH;
		case JOB_ASSASSIN_CROSS:        return MAPID_ASSASSIN_CROSS;
		//Trans 2-2 Jobs
		case JOB_PALADIN:               return MAPID_PALADIN;
		case JOB_PROFESSOR:             return MAPID_PROFESSOR;
		case JOB_CLOWN:
		case JOB_GYPSY:                 return MAPID_CLOWNGYPSY;
		case JOB_CHAMPION:              return MAPID_CHAMPION;
		case JOB_CREATOR:               return MAPID_CREATOR;
		case JOB_STALKER:               return MAPID_STALKER;
		//Baby Novice And Baby 1-1 Jobs
		case JOB_BABY:                  return MAPID_BABY;
		case JOB_BABY_SWORDMAN:         return MAPID_BABY_SWORDMAN;
		case JOB_BABY_MAGE:             return MAPID_BABY_MAGE;
		case JOB_BABY_ARCHER:           return MAPID_BABY_ARCHER;
		case JOB_BABY_ACOLYTE:          return MAPID_BABY_ACOLYTE;
		case JOB_BABY_MERCHANT:         return MAPID_BABY_MERCHANT;
		case JOB_BABY_THIEF:            return MAPID_BABY_THIEF;
		case JOB_BABY_TAEKWON:          return MAPID_BABY_TAEKWON;
		case JOB_BABY_GUNSLINGER:       return MAPID_BABY_GUNSLINGER;
		case JOB_BABY_NINJA:            return MAPID_BABY_NINJA;
		case JOB_BABY_SUMMONER:         return MAPID_BABY_SUMMONER;
		//Baby 2-1 Jobs
		case JOB_SUPER_BABY:            return MAPID_SUPER_BABY;
		case JOB_BABY_KNIGHT:           return MAPID_BABY_KNIGHT;
		case JOB_BABY_WIZARD:           return MAPID_BABY_WIZARD;
		case JOB_BABY_HUNTER:           return MAPID_BABY_HUNTER;
		case JOB_BABY_PRIEST:           return MAPID_BABY_PRIEST;
		case JOB_BABY_BLACKSMITH:       return MAPID_BABY_BLACKSMITH;
		case JOB_BABY_ASSASSIN:         return MAPID_BABY_ASSASSIN;
		case JOB_BABY_STAR_GLADIATOR:   return MAPID_BABY_STAR_GLADIATOR;
		case JOB_BABY_REBELLION:        return MAPID_BABY_REBELLION;
		case JOB_BABY_KAGEROU:
		case JOB_BABY_OBORO:            return MAPID_BABY_KAGEROUOBORO;
		//Baby 2-2 Jobs
		case JOB_BABY_CRUSADER:         return MAPID_BABY_CRUSADER;
		case JOB_BABY_SAGE:             return MAPID_BABY_SAGE;
		case JOB_BABY_BARD:
		case JOB_BABY_DANCER:           return MAPID_BABY_BARDDANCER;
		case JOB_BABY_MONK:             return MAPID_BABY_MONK;
		case JOB_BABY_ALCHEMIST:        return MAPID_BABY_ALCHEMIST;
		case JOB_BABY_ROGUE:            return MAPID_BABY_ROGUE;
		case JOB_BABY_SOUL_LINKER:      return MAPID_BABY_SOUL_LINKER;
		//3-1 Jobs
		case JOB_SUPER_NOVICE_E:        return MAPID_SUPER_NOVICE_E;
		case JOB_RUNE_KNIGHT:           return MAPID_RUNE_KNIGHT;
		case JOB_WARLOCK:               return MAPID_WARLOCK;
		case JOB_RANGER:                return MAPID_RANGER;
		case JOB_ARCH_BISHOP:           return MAPID_ARCH_BISHOP;
		case JOB_MECHANIC:              return MAPID_MECHANIC;
		case JOB_GUILLOTINE_CROSS:      return MAPID_GUILLOTINE_CROSS;
		case JOB_STAR_EMPEROR:          return MAPID_STAR_EMPEROR;
		//3-2 Jobs
		case JOB_ROYAL_GUARD:           return MAPID_ROYAL_GUARD;
		case JOB_SORCERER:              return MAPID_SORCERER;
		case JOB_MINSTREL:
		case JOB_WANDERER:              return MAPID_MINSTRELWANDERER;
		case JOB_SURA:                  return MAPID_SURA;
		case JOB_GENETIC:               return MAPID_GENETIC;
		case JOB_SHADOW_CHASER:         return MAPID_SHADOW_CHASER;
		case JOB_SOUL_REAPER:           return MAPID_SOUL_REAPER;
		//Trans 3-1 Jobs
		case JOB_RUNE_KNIGHT_T:         return MAPID_RUNE_KNIGHT_T;
		case JOB_WARLOCK_T:             return MAPID_WARLOCK_T;
		case JOB_RANGER_T:              return MAPID_RANGER_T;
		case JOB_ARCH_BISHOP_T:         return MAPID_ARCH_BISHOP_T;
		case JOB_MECHANIC_T:            return MAPID_MECHANIC_T;
		case JOB_GUILLOTINE_CROSS_T:    return MAPID_GUILLOTINE_CROSS_T;
		//Trans 3-2 Jobs
		case JOB_ROYAL_GUARD_T:         return MAPID_ROYAL_GUARD_T;
		case JOB_SORCERER_T:            return MAPID_SORCERER_T;
		case JOB_MINSTREL_T:
		case JOB_WANDERER_T:            return MAPID_MINSTRELWANDERER_T;
		case JOB_SURA_T:                return MAPID_SURA_T;
		case JOB_GENETIC_T:             return MAPID_GENETIC_T;
		case JOB_SHADOW_CHASER_T:       return MAPID_SHADOW_CHASER_T;
		//Baby 3-1 Jobs
		case JOB_SUPER_BABY_E:          return MAPID_SUPER_BABY_E;
		case JOB_BABY_RUNE:             return MAPID_BABY_RUNE;
		case JOB_BABY_WARLOCK:          return MAPID_BABY_WARLOCK;
		case JOB_BABY_RANGER:           return MAPID_BABY_RANGER;
		case JOB_BABY_BISHOP:           return MAPID_BABY_BISHOP;
		case JOB_BABY_MECHANIC:         return MAPID_BABY_MECHANIC;
		case JOB_BABY_CROSS:            return MAPID_BABY_CROSS;
		case JOB_BABY_STAR_EMPEROR:     return MAPID_BABY_STAR_EMPEROR;
		//Baby 3-2 Jobs
		case JOB_BABY_GUARD:            return MAPID_BABY_GUARD;
		case JOB_BABY_SORCERER:         return MAPID_BABY_SORCERER;
		case JOB_BABY_MINSTREL:
		case JOB_BABY_WANDERER:         return MAPID_BABY_MINSTRELWANDERER;
		case JOB_BABY_SURA:             return MAPID_BABY_SURA;
		case JOB_BABY_GENETIC:          return MAPID_BABY_GENETIC;
		case JOB_BABY_CHASER:           return MAPID_BABY_CHASER;
		case JOB_BABY_SOUL_REAPER:      return MAPID_BABY_SOUL_REAPER;
		//Summoner Job
		case JOB_SUMMONER:              return MAPID_SUMMONER;
		default:
			return -1;
	}
}

//Reverts the map-style class id to the client-style one.
int pc_mapid2jobid(unsigned short class_, int sex)
{
	switch(class_) {
		//Novice And 1-1 Jobs
		case MAPID_NOVICE:                return JOB_NOVICE;
		case MAPID_SWORDMAN:              return JOB_SWORDMAN;
		case MAPID_MAGE:                  return JOB_MAGE;
		case MAPID_ARCHER:                return JOB_ARCHER;
		case MAPID_ACOLYTE:               return JOB_ACOLYTE;
		case MAPID_MERCHANT:              return JOB_MERCHANT;
		case MAPID_THIEF:                 return JOB_THIEF;
		case MAPID_TAEKWON:               return JOB_TAEKWON;
		case MAPID_WEDDING:               return JOB_WEDDING;
		case MAPID_GUNSLINGER:            return JOB_GUNSLINGER;
		case MAPID_NINJA:                 return JOB_NINJA;
		case MAPID_XMAS:                  return JOB_XMAS;
		case MAPID_SUMMER:                return JOB_SUMMER;
		case MAPID_HANBOK:                return JOB_HANBOK;
		case MAPID_GANGSI:                return JOB_GANGSI;
		case MAPID_OKTOBERFEST:           return JOB_OKTOBERFEST;
		case MAPID_SUMMER2:               return JOB_SUMMER2;
		//2-1 Jobs
		case MAPID_SUPER_NOVICE:          return JOB_SUPER_NOVICE;
		case MAPID_KNIGHT:                return JOB_KNIGHT;
		case MAPID_WIZARD:                return JOB_WIZARD;
		case MAPID_HUNTER:                return JOB_HUNTER;
		case MAPID_PRIEST:                return JOB_PRIEST;
		case MAPID_BLACKSMITH:            return JOB_BLACKSMITH;
		case MAPID_ASSASSIN:              return JOB_ASSASSIN;
		case MAPID_STAR_GLADIATOR:        return JOB_STAR_GLADIATOR;
		case MAPID_REBELLION:             return JOB_REBELLION;
		case MAPID_KAGEROUOBORO:          return (sex ? JOB_KAGEROU : JOB_OBORO);
		case MAPID_DEATH_KNIGHT:          return JOB_DEATH_KNIGHT;
		//2-2 Jobs
		case MAPID_CRUSADER:              return JOB_CRUSADER;
		case MAPID_SAGE:                  return JOB_SAGE;
		case MAPID_BARDDANCER:            return (sex ? JOB_BARD : JOB_DANCER);
		case MAPID_MONK:                  return JOB_MONK;
		case MAPID_ALCHEMIST:             return JOB_ALCHEMIST;
		case MAPID_ROGUE:                 return JOB_ROGUE;
		case MAPID_SOUL_LINKER:           return JOB_SOUL_LINKER;
		case MAPID_DARK_COLLECTOR:        return JOB_DARK_COLLECTOR;
		//Trans Novice And Trans 2-1 Jobs
		case MAPID_NOVICE_HIGH:           return JOB_NOVICE_HIGH;
		case MAPID_SWORDMAN_HIGH:         return JOB_SWORDMAN_HIGH;
		case MAPID_MAGE_HIGH:             return JOB_MAGE_HIGH;
		case MAPID_ARCHER_HIGH:           return JOB_ARCHER_HIGH;
		case MAPID_ACOLYTE_HIGH:          return JOB_ACOLYTE_HIGH;
		case MAPID_MERCHANT_HIGH:         return JOB_MERCHANT_HIGH;
		case MAPID_THIEF_HIGH:            return JOB_THIEF_HIGH;
		//Trans 2-1 Jobs
		case MAPID_LORD_KNIGHT:           return JOB_LORD_KNIGHT;
		case MAPID_HIGH_WIZARD:           return JOB_HIGH_WIZARD;
		case MAPID_SNIPER:                return JOB_SNIPER;
		case MAPID_HIGH_PRIEST:           return JOB_HIGH_PRIEST;
		case MAPID_WHITESMITH:            return JOB_WHITESMITH;
		case MAPID_ASSASSIN_CROSS:        return JOB_ASSASSIN_CROSS;
		//Trans 2-2 Jobs
		case MAPID_PALADIN:               return JOB_PALADIN;
		case MAPID_PROFESSOR:             return JOB_PROFESSOR;
		case MAPID_CLOWNGYPSY:            return (sex ? JOB_CLOWN : JOB_GYPSY);
		case MAPID_CHAMPION:              return JOB_CHAMPION;
		case MAPID_CREATOR:               return JOB_CREATOR;
		case MAPID_STALKER:               return JOB_STALKER;
		//Baby Novice And Baby 1-1 Jobs
		case MAPID_BABY:                  return JOB_BABY;
		case MAPID_BABY_SWORDMAN:         return JOB_BABY_SWORDMAN;
		case MAPID_BABY_MAGE:             return JOB_BABY_MAGE;
		case MAPID_BABY_ARCHER:           return JOB_BABY_ARCHER;
		case MAPID_BABY_ACOLYTE:          return JOB_BABY_ACOLYTE;
		case MAPID_BABY_MERCHANT:         return JOB_BABY_MERCHANT;
		case MAPID_BABY_THIEF:            return JOB_BABY_THIEF;
		case MAPID_BABY_TAEKWON:          return JOB_BABY_TAEKWON;
		case MAPID_BABY_GUNSLINGER:       return JOB_BABY_GUNSLINGER;
		case MAPID_BABY_NINJA:            return JOB_BABY_NINJA;
		case MAPID_BABY_SUMMONER:         return JOB_BABY_SUMMONER;
		//Baby 2-1 Jobs
		case MAPID_SUPER_BABY:            return JOB_SUPER_BABY;
		case MAPID_BABY_KNIGHT:           return JOB_BABY_KNIGHT;
		case MAPID_BABY_WIZARD:           return JOB_BABY_WIZARD;
		case MAPID_BABY_HUNTER:           return JOB_BABY_HUNTER;
		case MAPID_BABY_PRIEST:           return JOB_BABY_PRIEST;
		case MAPID_BABY_BLACKSMITH:       return JOB_BABY_BLACKSMITH;
		case MAPID_BABY_ASSASSIN:         return JOB_BABY_ASSASSIN;
		case MAPID_BABY_STAR_GLADIATOR:   return JOB_BABY_STAR_GLADIATOR;
		case MAPID_BABY_REBELLION:        return JOB_BABY_REBELLION;
		case MAPID_BABY_KAGEROUOBORO:     return (sex ? JOB_BABY_KAGEROU : JOB_BABY_OBORO);
		//Baby 2-2 Jobs
		case MAPID_BABY_CRUSADER:         return JOB_BABY_CRUSADER;
		case MAPID_BABY_SAGE:             return JOB_BABY_SAGE;
		case MAPID_BABY_BARDDANCER:       return (sex ? JOB_BABY_BARD : JOB_BABY_DANCER);
		case MAPID_BABY_MONK:             return JOB_BABY_MONK;
		case MAPID_BABY_ALCHEMIST:        return JOB_BABY_ALCHEMIST;
		case MAPID_BABY_ROGUE:            return JOB_BABY_ROGUE;
		case MAPID_BABY_SOUL_LINKER:      return JOB_BABY_SOUL_LINKER;
		//3-1 Jobs
		case MAPID_SUPER_NOVICE_E:        return JOB_SUPER_NOVICE_E;
		case MAPID_RUNE_KNIGHT:           return JOB_RUNE_KNIGHT;
		case MAPID_WARLOCK:               return JOB_WARLOCK;
		case MAPID_RANGER:                return JOB_RANGER;
		case MAPID_ARCH_BISHOP:           return JOB_ARCH_BISHOP;
		case MAPID_MECHANIC:              return JOB_MECHANIC;
		case MAPID_GUILLOTINE_CROSS:      return JOB_GUILLOTINE_CROSS;
		case MAPID_STAR_EMPEROR:          return JOB_STAR_EMPEROR;
		//3-2 Jobs
		case MAPID_ROYAL_GUARD:           return JOB_ROYAL_GUARD;
		case MAPID_SORCERER:              return JOB_SORCERER;
		case MAPID_MINSTRELWANDERER:      return (sex ? JOB_MINSTREL : JOB_WANDERER);
		case MAPID_SURA:                  return JOB_SURA;
		case MAPID_GENETIC:               return JOB_GENETIC;
		case MAPID_SHADOW_CHASER:         return JOB_SHADOW_CHASER;
		case MAPID_SOUL_REAPER:           return JOB_SOUL_REAPER;
		//Trans 3-1 Jobs
		case MAPID_RUNE_KNIGHT_T:         return JOB_RUNE_KNIGHT_T;
		case MAPID_WARLOCK_T:             return JOB_WARLOCK_T;
		case MAPID_RANGER_T:              return JOB_RANGER_T;
		case MAPID_ARCH_BISHOP_T:         return JOB_ARCH_BISHOP_T;
		case MAPID_MECHANIC_T:            return JOB_MECHANIC_T;
		case MAPID_GUILLOTINE_CROSS_T:    return JOB_GUILLOTINE_CROSS_T;
		//Trans 3-2 Jobs
		case MAPID_ROYAL_GUARD_T:         return JOB_ROYAL_GUARD_T;
		case MAPID_SORCERER_T:            return JOB_SORCERER_T;
		case MAPID_MINSTRELWANDERER_T:    return (sex ? JOB_MINSTREL_T : JOB_WANDERER_T);
		case MAPID_SURA_T:                return JOB_SURA_T;
		case MAPID_GENETIC_T:             return JOB_GENETIC_T;
		case MAPID_SHADOW_CHASER_T:       return JOB_SHADOW_CHASER_T;
		//Baby 3-1 Jobs
		case MAPID_SUPER_BABY_E:          return JOB_SUPER_BABY_E;
		case MAPID_BABY_RUNE:             return JOB_BABY_RUNE;
		case MAPID_BABY_WARLOCK:          return JOB_BABY_WARLOCK;
		case MAPID_BABY_RANGER:           return JOB_BABY_RANGER;
		case MAPID_BABY_BISHOP:           return JOB_BABY_BISHOP;
		case MAPID_BABY_MECHANIC:         return JOB_BABY_MECHANIC;
		case MAPID_BABY_CROSS:            return JOB_BABY_CROSS;
		case MAPID_BABY_STAR_EMPEROR:     return JOB_BABY_STAR_EMPEROR;
		//Baby 3-2 Jobs
		case MAPID_BABY_GUARD:            return JOB_BABY_GUARD;
		case MAPID_BABY_SORCERER:         return JOB_BABY_SORCERER;
		case MAPID_BABY_MINSTRELWANDERER: return (sex ? JOB_BABY_MINSTREL : JOB_BABY_WANDERER);
		case MAPID_BABY_SURA:             return JOB_BABY_SURA;
		case MAPID_BABY_GENETIC:          return JOB_BABY_GENETIC;
		case MAPID_BABY_CHASER:           return JOB_BABY_CHASER;
		case MAPID_BABY_SOUL_REAPER:      return JOB_BABY_SOUL_REAPER;
		//Summoner Job
		case MAPID_SUMMONER:              return JOB_SUMMONER;
		default:
			return -1;
	}
}

/*====================================================
 * This function return the name of the job (by [Yor])
 *----------------------------------------------------*/
const char *job_name(int class_)
{
	switch (class_) {
		case JOB_NOVICE:
		case JOB_SWORDMAN:
		case JOB_MAGE:
		case JOB_ARCHER:
		case JOB_ACOLYTE:
		case JOB_MERCHANT:
		case JOB_THIEF:
			return msg_txt(NULL, 550 - JOB_NOVICE + class_);

		case JOB_KNIGHT:
		case JOB_PRIEST:
		case JOB_WIZARD:
		case JOB_BLACKSMITH:
		case JOB_HUNTER:
		case JOB_ASSASSIN:
			return msg_txt(NULL, 557 - JOB_KNIGHT + class_);

		case JOB_KNIGHT2:
			return msg_txt(NULL, 557);

		case JOB_CRUSADER:
		case JOB_MONK:
		case JOB_SAGE:
		case JOB_ROGUE:
		case JOB_ALCHEMIST:
		case JOB_BARD:
		case JOB_DANCER:
			return msg_txt(NULL, 563 - JOB_CRUSADER + class_);

		case JOB_CRUSADER2:
			return msg_txt(NULL, 563);

		case JOB_WEDDING:
		case JOB_SUPER_NOVICE:
		case JOB_GUNSLINGER:
		case JOB_NINJA:
		case JOB_XMAS:
			return msg_txt(NULL, 570 - JOB_WEDDING + class_);

		case JOB_SUMMER:
		case JOB_SUMMER2:
			return msg_txt(NULL, 621);

		case JOB_HANBOK:
			return msg_txt(NULL, 694);

		case JOB_OKTOBERFEST:
			return msg_txt(NULL, 696);

		case JOB_NOVICE_HIGH:
		case JOB_SWORDMAN_HIGH:
		case JOB_MAGE_HIGH:
		case JOB_ARCHER_HIGH:
		case JOB_ACOLYTE_HIGH:
		case JOB_MERCHANT_HIGH:
		case JOB_THIEF_HIGH:
			return msg_txt(NULL, 575 - JOB_NOVICE_HIGH + class_);

		case JOB_LORD_KNIGHT:
		case JOB_HIGH_PRIEST:
		case JOB_HIGH_WIZARD:
		case JOB_WHITESMITH:
		case JOB_SNIPER:
		case JOB_ASSASSIN_CROSS:
			return msg_txt(NULL, 582 - JOB_LORD_KNIGHT + class_);

		case JOB_LORD_KNIGHT2:
			return msg_txt(NULL, 582);

		case JOB_PALADIN:
		case JOB_CHAMPION:
		case JOB_PROFESSOR:
		case JOB_STALKER:
		case JOB_CREATOR:
		case JOB_CLOWN:
		case JOB_GYPSY:
			return msg_txt(NULL, 588 - JOB_PALADIN + class_);

		case JOB_PALADIN2:
			return msg_txt(NULL, 588);

		case JOB_BABY:
		case JOB_BABY_SWORDMAN:
		case JOB_BABY_MAGE:
		case JOB_BABY_ARCHER:
		case JOB_BABY_ACOLYTE:
		case JOB_BABY_MERCHANT:
		case JOB_BABY_THIEF:
			return msg_txt(NULL, 595 - JOB_BABY + class_);

		case JOB_BABY_KNIGHT:
		case JOB_BABY_PRIEST:
		case JOB_BABY_WIZARD:
		case JOB_BABY_BLACKSMITH:
		case JOB_BABY_HUNTER:
		case JOB_BABY_ASSASSIN:
			return msg_txt(NULL, 602 - JOB_BABY_KNIGHT + class_);

		case JOB_BABY_KNIGHT2:
			return msg_txt(NULL, 602);

		case JOB_BABY_CRUSADER:
		case JOB_BABY_MONK:
		case JOB_BABY_SAGE:
		case JOB_BABY_ROGUE:
		case JOB_BABY_ALCHEMIST:
		case JOB_BABY_BARD:
		case JOB_BABY_DANCER:
			return msg_txt(NULL, 608 - JOB_BABY_CRUSADER + class_);

		case JOB_BABY_CRUSADER2:
			return msg_txt(NULL, 608);

		case JOB_SUPER_BABY:
			return msg_txt(NULL, 615);

		case JOB_TAEKWON:
			return msg_txt(NULL, 616);
		case JOB_STAR_GLADIATOR:
		case JOB_STAR_GLADIATOR2:
			return msg_txt(NULL, 617);
		case JOB_SOUL_LINKER:
			return msg_txt(NULL, 618);

		case JOB_GANGSI:
		case JOB_DEATH_KNIGHT:
		case JOB_DARK_COLLECTOR:
			return msg_txt(NULL, 622 - JOB_GANGSI + class_);

		case JOB_RUNE_KNIGHT:
		case JOB_WARLOCK:
		case JOB_RANGER:
		case JOB_ARCH_BISHOP:
		case JOB_MECHANIC:
		case JOB_GUILLOTINE_CROSS:
			return msg_txt(NULL, 625 - JOB_RUNE_KNIGHT + class_);

		case JOB_RUNE_KNIGHT_T:
		case JOB_WARLOCK_T:
		case JOB_RANGER_T:
		case JOB_ARCH_BISHOP_T:
		case JOB_MECHANIC_T:
		case JOB_GUILLOTINE_CROSS_T:
			return msg_txt(NULL, 681 - JOB_RUNE_KNIGHT_T + class_);

		case JOB_ROYAL_GUARD:
		case JOB_SORCERER:
		case JOB_MINSTREL:
		case JOB_WANDERER:
		case JOB_SURA:
		case JOB_GENETIC:
		case JOB_SHADOW_CHASER:
			return msg_txt(NULL, 631 - JOB_ROYAL_GUARD + class_);

		case JOB_ROYAL_GUARD_T:
		case JOB_SORCERER_T:
		case JOB_MINSTREL_T:
		case JOB_WANDERER_T:
		case JOB_SURA_T:
		case JOB_GENETIC_T:
		case JOB_SHADOW_CHASER_T:
			return msg_txt(NULL, 687 - JOB_ROYAL_GUARD_T + class_);

		case JOB_RUNE_KNIGHT2:
		case JOB_RUNE_KNIGHT_T2:
			return msg_txt(NULL, 625);

		case JOB_ROYAL_GUARD2:
		case JOB_ROYAL_GUARD_T2:
			return msg_txt(NULL, 631);

		case JOB_RANGER2:
		case JOB_RANGER_T2:
			return msg_txt(NULL, 627);

		case JOB_MECHANIC2:
		case JOB_MECHANIC_T2:
			return msg_txt(NULL, 629);

		case JOB_BABY_RUNE:
		case JOB_BABY_WARLOCK:
		case JOB_BABY_RANGER:
		case JOB_BABY_BISHOP:
		case JOB_BABY_MECHANIC:
		case JOB_BABY_CROSS:
		case JOB_BABY_GUARD:
		case JOB_BABY_SORCERER:
		case JOB_BABY_MINSTREL:
		case JOB_BABY_WANDERER:
		case JOB_BABY_SURA:
		case JOB_BABY_GENETIC:
		case JOB_BABY_CHASER:
			return msg_txt(NULL, 638 - JOB_BABY_RUNE + class_);

		case JOB_BABY_RUNE2:
			return msg_txt(NULL, 638);

		case JOB_BABY_GUARD2:
			return msg_txt(NULL, 644);

		case JOB_BABY_RANGER2:
			return msg_txt(NULL, 640);

		case JOB_BABY_MECHANIC2:
			return msg_txt(NULL, 642);

		case JOB_SUPER_NOVICE_E:
		case JOB_SUPER_BABY_E:
			return msg_txt(NULL, 651 - JOB_SUPER_NOVICE_E + class_);

		case JOB_KAGEROU:
		case JOB_OBORO:
			return msg_txt(NULL, 653 - JOB_KAGEROU + class_);

		case JOB_REBELLION:
			return msg_txt(NULL, 695);

		case JOB_SUMMONER:
			return msg_txt(NULL, 697);

		case JOB_BABY_SUMMONER:
			return msg_txt(NULL, 698);

		case JOB_BABY_NINJA:
		case JOB_BABY_KAGEROU:
		case JOB_BABY_OBORO:
		case JOB_BABY_TAEKWON:
		case JOB_BABY_STAR_GLADIATOR:
		case JOB_BABY_SOUL_LINKER:
		case JOB_BABY_GUNSLINGER:
		case JOB_BABY_REBELLION:
			return msg_txt(NULL, 745 - JOB_BABY_NINJA + class_);

		case JOB_BABY_STAR_GLADIATOR2:
			return msg_txt(NULL, 749);

		case JOB_STAR_EMPEROR:
		case JOB_SOUL_REAPER:
		case JOB_BABY_STAR_EMPEROR:
		case JOB_BABY_SOUL_REAPER:
			return msg_txt(NULL, 924 - JOB_STAR_EMPEROR + class_);

		case JOB_STAR_EMPEROR2:
			return msg_txt(NULL, 924);

		case JOB_BABY_STAR_EMPEROR2:
			return msg_txt(NULL, 926);

		default:
			return msg_txt(NULL, 655);
	}
}

/*====================================================
 * Timered function to make id follow a target.
 * @id = bl.id (player only atm)
 * target is define in sd->followtarget (bl.id)
 * used by pc_follow.
 *----------------------------------------------------*/
TIMER_FUNC(pc_follow_timer)
{
	struct map_session_data *sd;
	struct block_list *tbl;

	sd = map_id2sd(id);
	nullpo_ret(sd);

	if (sd->followtimer != tid) {
		ShowError("pc_follow_timer %d != %d\n",sd->followtimer,tid);
		sd->followtimer = INVALID_TIMER;
		return 0;
	}

	sd->followtimer = INVALID_TIMER;
	tbl = map_id2bl(sd->followtarget);

	if (tbl == NULL || pc_isdead(sd)) {
		pc_stop_following(sd);
		return 0;
	}

	// Either player or target is currently detached from map blocks (could be teleporting)
	// But still connected to this map, so we'll just increment the timer and check back later
	if (sd->bl.prev != NULL && tbl->prev != NULL &&
		sd->ud.skilltimer == INVALID_TIMER && sd->ud.attacktimer == INVALID_TIMER && sd->ud.walktimer == INVALID_TIMER) {
		if (sd->bl.m == tbl->m && unit_can_reach_bl(&sd->bl,tbl, AREA_SIZE, 0, NULL, NULL)) {
			if (!check_distance_bl(&sd->bl, tbl, 5))
				unit_walktobl(&sd->bl, tbl, 5, 0);
		} else
			pc_setpos(sd, map_id2index(tbl->m), tbl->x, tbl->y, CLR_TELEPORT);
	}
	sd->followtimer = add_timer(
		tick + 1000, // Increase time a bit to loosen up map's load
		pc_follow_timer, sd->bl.id, 0);
	return 0;
}

int pc_stop_following(struct map_session_data *sd)
{
	nullpo_ret(sd);

	if (sd->followtimer != INVALID_TIMER) {
		delete_timer(sd->followtimer,pc_follow_timer);
		sd->followtimer = INVALID_TIMER;
	}
	sd->followtarget = -1;
	sd->ud.target_to = 0;

	unit_stop_walking(&sd->bl, USW_FIXPOS);

	return 0;
}

int pc_follow(struct map_session_data *sd,int target_id)
{
	struct block_list *bl = map_id2bl(target_id);

	if (!bl /*|| bl->type != BL_PC*/)
		return 1;
	if (sd->followtimer != INVALID_TIMER)
		pc_stop_following(sd);

	sd->followtarget = target_id;
	pc_follow_timer(INVALID_TIMER, gettick(), sd->bl.id, 0);

	return 0;
}

int pc_checkbaselevelup(struct map_session_data *sd) {
	uint32 nextb = pc_nextbaseexp(sd);
	int point;

	if (!nextb || sd->status.base_exp < nextb || pc_is_maxbaselv(sd))
		return 0;

	do {
		sd->status.base_exp -= nextb;
		if ((!battle_config.multi_level_up || (battle_config.multi_level_up_base > 0 && sd->status.base_level >= battle_config.multi_level_up_base)) && sd->status.base_exp > nextb - 1)
			sd->status.base_exp = nextb - 1; //Kyoki pointed out that the max overcarry exp is the exp needed for the previous level -1 [Skotlex]
		point = pc_gets_status_point(sd->status.base_level);
		sd->status.base_level++;
		sd->status.status_point += point;
		if (pc_is_maxbaselv(sd)) {
			sd->status.base_exp = umin(sd->status.base_exp, MAX_LEVEL_BASE_EXP);
			break;
		}
	} while ((nextb = pc_nextbaseexp(sd)) > 0 && sd->status.base_exp >= nextb);

	if (battle_config.pet_lv_rate && sd->pd) //Update pet's level [Skotlex]
		status_calc_pet(sd->pd, SCO_NONE);

	clif_updatestatus(sd, SP_STATUSPOINT);
	clif_updatestatus(sd, SP_BASELEVEL);
	clif_updatestatus(sd, SP_BASEEXP);
	clif_updatestatus(sd, SP_NEXTBASEEXP);
	status_calc_pc(sd, SCO_FORCE);
	status_percent_heal(&sd->bl, 100, 100);

	if ((sd->class_&MAPID_UPPERMASK) == MAPID_SUPER_NOVICE) {
		sc_start(&sd->bl, &sd->bl, status_skill2sc(PR_KYRIE), 100, 1, skill_get_time(PR_KYRIE, 1));
		sc_start(&sd->bl, &sd->bl, status_skill2sc(PR_IMPOSITIO), 100, 1, skill_get_time(PR_IMPOSITIO, 1));
		sc_start(&sd->bl, &sd->bl, status_skill2sc(PR_MAGNIFICAT), 100, 1, skill_get_time(PR_MAGNIFICAT, 1));
		sc_start(&sd->bl, &sd->bl, status_skill2sc(PR_GLORIA), 100, 1, skill_get_time(PR_GLORIA, 1));
		sc_start(&sd->bl, &sd->bl, status_skill2sc(PR_SUFFRAGIUM), 100, 1, skill_get_time(PR_SUFFRAGIUM, 1));
		if (sd->state.snovice_dead_flag)
			sd->state.snovice_dead_flag = 0; //Re-enable steel body resurrection on dead
	} else if ((sd->class_&MAPID_BASEMASK) == MAPID_TAEKWON) {
		sc_start(&sd->bl, &sd->bl, status_skill2sc(AL_INCAGI), 100, 10, 600000);
		sc_start(&sd->bl, &sd->bl, status_skill2sc(AL_BLESSING), 100, 10, 600000);
	}
	clif_misceffect(&sd->bl, 0);
	npc_script_event(sd, NPCE_BASELVUP); //LORDALFA - LVLUPEVENT

	if (sd->status.party_id)
		party_send_levelup(sd);

	pc_baselevelchanged(sd);
	achievement_update_objective(sd, AG_GOAL_LEVEL, 1, sd->status.base_level);
	achievement_update_objective(sd, AG_GOAL_STATUS, 2, sd->status.base_level, sd->status.class_);
	return 1;
}

void pc_baselevelchanged(struct map_session_data *sd) {
	uint8 i;

	for (i = 0; i < EQI_MAX; i++) {
		if (sd->equip_index[i] >= 0 &&
			sd->inventory_data[sd->equip_index[i]]->elvmax && sd->status.base_level > (unsigned int)sd->inventory_data[sd->equip_index[i]]->elvmax)
			pc_unequipitem(sd, sd->equip_index[i], 1|2);
	}
	pc_show_questinfo(sd);
}

int pc_checkjoblevelup(struct map_session_data *sd)
{
	uint32 nextj = pc_nextjobexp(sd);

	nullpo_ret(sd);

	if (!nextj || sd->status.job_exp < nextj || pc_is_maxjoblv(sd))
		return 0;

	do {
		sd->status.job_exp -= nextj;
		if ((!battle_config.multi_level_up || (battle_config.multi_level_up_job > 0 && sd->status.job_level >= battle_config.multi_level_up_job)) && sd->status.job_exp > nextj - 1)
			sd->status.job_exp = nextj - 1; //Kyoki pointed out that the max overcarry exp is the exp needed for the previous level -1 [Skotlex]
		sd->status.job_level++;
		sd->status.skill_point++;
		if (pc_is_maxjoblv(sd)) {
			sd->status.job_exp = umin(sd->status.job_exp, MAX_LEVEL_JOB_EXP);
			break;
		}
	} while ((nextj = pc_nextjobexp(sd)) > 0 && sd->status.job_exp >= nextj);

	clif_updatestatus(sd, SP_JOBLEVEL);
	clif_updatestatus(sd, SP_JOBEXP);
	clif_updatestatus(sd, SP_NEXTJOBEXP);
	clif_updatestatus(sd, SP_SKILLPOINT);
	status_calc_pc(sd, SCO_FORCE);
	clif_misceffect(&sd->bl, 1);

	if (pc_checkskill(sd, SG_DEVIL) > 0 && ((sd->class_&MAPID_THIRDMASK) == MAPID_STAR_EMPEROR || pc_is_maxjoblv(sd)))
		clif_status_load(&sd->bl, SI_DEVIL1, 1); //Permanent blind effect from SG_DEVIL

	npc_script_event(sd, NPCE_JOBLVUP);
	achievement_update_objective(sd, AG_GOAL_LEVEL, 1, sd->status.job_level);
	return 1;
}

/** Alters experiences calculation based on self bonuses that do not get even shared to the party.
 * @param sd Player
 * @param base_exp Base EXP before peronal bonuses
 * @param job_exp Job EXP before peronal bonuses
 * @param src Block list that affecting the exp calculation
 */
static void pc_calcexp(struct map_session_data *sd, uint32 *base_exp, uint32 *job_exp, struct block_list *src)
{
	int bonus = 0, bm_bonus = 0, vip_bonus_base = 0, vip_bonus_job = 0;

	if (src) {
		struct status_data *status = status_get_status_data(src);

		if (sd->expaddrace[status->race])
			bonus += sd->expaddrace[status->race];
		if (sd->expaddrace[RC_ALL])
			bonus += sd->expaddrace[RC_ALL];
		if (sd->expaddclass[status->class_])
			bonus += sd->expaddclass[status->class_];
		if (sd->expaddclass[CLASS_ALL])
			bonus += sd->expaddclass[CLASS_ALL];
		if (sd->sc.data[SC_JP_EVENT04] && status->race == RC_FISH)
			bonus += sd->sc.data[SC_JP_EVENT04]->val1;
		if (battle_config.pk_mode && (int)(status_get_lv(src) - sd->status.base_level) >= 20)
			bonus += 15; //pk_mode additional exp if monster > 20 levels [Valaris]
		if (src->type == BL_MOB && pc_isvip(sd)) { //EXP bonus for VIP player
			vip_bonus_base = battle_config.vip_base_exp_increase;
			vip_bonus_job = battle_config.vip_job_exp_increase;
		}
	}

	//Give EXPBOOST for quests even if src is NULL
	if (sd->sc.data[SC_EXPBOOST]) {
		bm_bonus += sd->sc.data[SC_EXPBOOST]->val1;
		if (battle_config.vip_bm_increase && pc_isvip(sd)) //Increase Battle Manual EXP rate for VIP
			bm_bonus += (sd->sc.data[SC_EXPBOOST]->val1 / battle_config.vip_bm_increase);
	}

	if (*base_exp) {
		uint32 exp;
		int base_rate = 100 + vip_bonus_base;
		int base_bonus = (int)((double)bonus * base_rate / 100.);

		exp = (uint32)((double)*base_exp * (base_rate + base_bonus + bm_bonus) / 100.);
		*base_exp =  cap_value(exp, 1, UINT32_MAX);
	}

	//Give JEXPBOOST for quests even if src is NULL
	if (sd->sc.data[SC_JEXPBOOST]) {
		bm_bonus += sd->sc.data[SC_JEXPBOOST]->val1;
		if (battle_config.vip_bm_increase && pc_isvip(sd)) //Increase Job Manual EXP rate for VIP
			bm_bonus += (sd->sc.data[SC_JEXPBOOST]->val1 / battle_config.vip_bm_increase);
	}

	if (*job_exp) {
		uint32 exp;
		int job_rate = 100 + vip_bonus_job;
		int job_bonus = (int)((double)bonus * job_rate / 100.);

		exp = (uint32)((double)*job_exp * (job_rate + job_bonus + bm_bonus) / 100.);
		*job_exp = cap_value(exp, 1, UINT32_MAX);
	}
}

/**
 * Show EXP gained by player in percentage by @showexp
 * @param sd Player
 * @param base_exp Base EXP gained/loss
 * @param next_base_exp Base EXP needed for next base level
 * @param job_exp Job EXP gained/loss
 * @param next_job_exp Job EXP needed for next job level
 * @param lost True:EXP penalty, lose EXP
 */
void pc_gainexp_disp(struct map_session_data *sd, uint32 base_exp, uint32 next_base_exp, uint32 job_exp, uint32 next_job_exp, bool lost)
{
	char output[CHAT_SIZE_MAX];

	nullpo_retv(sd);

	sprintf(output, msg_txt(sd, 740), // Experience %s Base:%ld (%0.2f%%) Job:%ld (%0.2f%%)
		(lost) ? msg_txt(sd, 739) : msg_txt(sd, 738),
		(long)base_exp * (lost ? -1 : 1), (base_exp / (float)next_base_exp * 100 * (lost ? -1 : 1)),
		(long)job_exp * (lost ? -1 : 1), (job_exp / (float)next_job_exp * 100 * (lost ? -1 : 1)));
	clif_messagecolor(&sd->bl, color_table[COLOR_LIGHT_GREEN], output, false, SELF);
}

/**
 * Give Base or Job EXP to player, then calculate remaining exp for next lvl
 * @param sd Player
 * @param src EXP source
 * @param base_exp Base EXP gained
 * @param base_exp Job EXP gained
 * @param exp_flag 1: Quest EXP; 2: Param Exp (Ignore Guild EXP tax, EXP adjustments)
 * @return true if success
 */
void pc_gainexp(struct map_session_data *sd, struct block_list *src, uint32 base_exp, uint32 job_exp, uint8 exp_flag)
{
	uint32 nextb = 0, nextj = 0;
	uint8 flag = 0; //1: Base EXP given, 2: Job EXP given, 4: Max Base level, 8: Max Job Level

	nullpo_retv(sd);

	if (!sd->bl.prev || pc_isdead(sd))
		return;

	if (!(exp_flag&2)) {
		if (!battle_config.pvp_exp && mapdata[sd->bl.m].flag.pvp) //[MouseJstr]
			return; //No exp on pvp maps
		pc_calcexp(sd, &base_exp, &job_exp, src);
		if (sd->status.guild_id > 0)
			base_exp -= guild_payexp(sd, base_exp);
	}

#ifdef RENEWAL //Homunculus gains 10% of final EXP owner gained from any source
	if (hom_is_active(sd->hd) && (base_exp || job_exp))
		hom_gainexp(sd->hd, (base_exp + job_exp) * battle_config.homunculus_exp_gain / 100);
#endif

	flag = (base_exp ? 1 : 0)|(job_exp ? 2 : 0)|(pc_is_maxbaselv(sd) ? 4 : 0)|(pc_is_maxjoblv(sd) ? 8 : 0);
	nextb = pc_nextbaseexp(sd);
	nextj = pc_nextjobexp(sd);

	if (flag&4) {
		if (sd->status.base_exp >= MAX_LEVEL_BASE_EXP)
			base_exp = 0;
		else if ((uint64)sd->status.base_exp + base_exp >= MAX_LEVEL_BASE_EXP)
			base_exp = MAX_LEVEL_BASE_EXP - sd->status.base_exp;
	}

	if (flag&8) {
		if (sd->status.job_exp >= MAX_LEVEL_JOB_EXP)
			job_exp = 0;
		else if ((uint64)sd->status.job_exp + job_exp >= MAX_LEVEL_JOB_EXP)
			job_exp = MAX_LEVEL_JOB_EXP - sd->status.job_exp;
	}

	//Note that this value should never be greater than the original therefore no overflow checks are needed [Skotlex]
	if (!(exp_flag&2) && battle_config.max_exp_gain_rate && (base_exp || job_exp)) {
		if (nextb > 0) {
			double nextbp = (double)base_exp / (double)nextb;

			if (nextbp > battle_config.max_exp_gain_rate / 1000.)
				base_exp = (uint32)(battle_config.max_exp_gain_rate / 1000. * nextb);
		}
		if (nextj > 0) {
			double nextjp = (double)job_exp / (double)nextj;

			if (nextjp > battle_config.max_exp_gain_rate / 1000.)
				job_exp = (uint32)(battle_config.max_exp_gain_rate / 1000. * nextj);
		}
	}

	//Give EXP for Base Level
	if (base_exp) {
		if ((uint64)sd->status.base_exp + base_exp > UINT32_MAX)
			sd->status.base_exp = UINT32_MAX;
		else
			sd->status.base_exp += base_exp;
		if (!pc_checkbaselevelup(sd))
			clif_updatestatus(sd, SP_BASEEXP);
	}

	//Give EXP for Job Level
	if (job_exp) {
		if ((uint64)sd->status.job_exp + job_exp > UINT32_MAX)
			sd->status.job_exp = UINT32_MAX;
		else
			sd->status.job_exp += job_exp;
		if (!pc_checkjoblevelup(sd))
			clif_updatestatus(sd, SP_JOBEXP);
	}

	if (flag&1)
		clif_displayexp(sd, (flag&4) ? 0 : base_exp, SP_BASEEXP, exp_flag&1, false);

	if (flag&2)
		clif_displayexp(sd, (flag&8) ? 0 : job_exp, SP_JOBEXP, exp_flag&1, false);

	if (sd->state.showexp && (base_exp || job_exp))
		pc_gainexp_disp(sd, base_exp, nextb, job_exp, nextj, false);
}

/**
 * Lost Base/Job EXP from a player
 * @param sd Player
 * @param base_exp Base EXP lost
 * @param job_exp Job EXP lost
 */
void pc_lostexp(struct map_session_data *sd, uint32 base_exp, uint32 job_exp)
{
	nullpo_retv(sd);

	if (base_exp) {
		base_exp = umin(sd->status.base_exp, base_exp);
		sd->status.base_exp -= base_exp;
		clif_displayexp(sd, base_exp, SP_BASEEXP, false, true);
		clif_updatestatus(sd, SP_BASEEXP);
	}

	if (job_exp) {
		job_exp = umin(sd->status.job_exp, job_exp);
		sd->status.job_exp -= job_exp;
		clif_displayexp(sd, job_exp, SP_JOBEXP, false, true);
		clif_updatestatus(sd, SP_JOBEXP);
	}

	if (sd->state.showexp && (base_exp || job_exp))
		pc_gainexp_disp(sd, base_exp, pc_nextbaseexp(sd), job_exp, pc_nextjobexp(sd), true);
}

/**
 * Returns max base level for this character's class.
 * @param class_: Player's class
 * @return Max Base Level
 */
static unsigned int pc_class_maxbaselv(unsigned short class_) {
	return job_info[pc_class2idx(class_)].max_level[0];
}

/**
 * Returns max base level for this character.
 * @param sd Player
 * @return Max Base Level
 */
unsigned int pc_maxbaselv(struct map_session_data *sd) {
	return pc_class_maxbaselv(sd->status.class_);
}

/**
 * Returns max job level for this character's class.
 * @param class_: Player's class
 * @return Max Job Level
 */
static unsigned int pc_class_maxjoblv(unsigned short class_) {
	return job_info[pc_class2idx(class_)].max_level[1];
}

/**
 * Returns max job level for this character.
 * @param sd Player
 * @return Max Job Level
 */
unsigned int pc_maxjoblv(struct map_session_data *sd) {
	return pc_class_maxjoblv(sd->status.class_);
}

/**
 * Check if player is reached max base level
 * @param sd
 * @return True if reached max level
 */
bool pc_is_maxbaselv(struct map_session_data *sd) {
	nullpo_retr(false, sd);

	return (sd->status.base_level >= pc_maxbaselv(sd));
}

/**
 * Check if player is reached max base level
 * @param sd
 * @return True if reached max level
 */
bool pc_is_maxjoblv(struct map_session_data *sd) {
	nullpo_retr(false, sd);

	return (sd->status.job_level >= pc_maxjoblv(sd));
}

/**
 * Base exp needed for player to level up.
 * @param sd
 * @return Base EXP needed for next base level
 */
uint32 pc_nextbaseexp(struct map_session_data *sd) {
	nullpo_ret(sd);

	if (!sd->status.base_level) //Is this even possible?
		return 0;
	if (pc_is_maxbaselv(sd))
		return MAX_LEVEL_BASE_EXP; //On max level, player's base EXP limit is 99,999,999
	return job_info[pc_class2idx(sd->status.class_)].exp_table[0][sd->status.base_level - 1];
}

/**
 * Job exp needed for player to level up.
 * @param sd
 * @return Job EXP needed for next job level
 */
uint32 pc_nextjobexp(struct map_session_data *sd) {
	nullpo_ret(sd);

	if (!sd->status.job_level) //Is this even possible?
		return 0;
	if (pc_is_maxjoblv(sd))
		return MAX_LEVEL_JOB_EXP; //On max level, player's job EXP limit is 999,999,999
	return job_info[pc_class2idx(sd->status.class_)].exp_table[1][sd->status.job_level - 1];
}

/// Returns the value of the specified stat.
static int pc_getstat(struct map_session_data *sd, int type)
{
	nullpo_retr(-1, sd);

	switch( type ) {
		case SP_STR: return sd->status.str;
		case SP_AGI: return sd->status.agi;
		case SP_VIT: return sd->status.vit;
		case SP_INT: return sd->status.int_;
		case SP_DEX: return sd->status.dex;
		case SP_LUK: return sd->status.luk;
		default:
			return -1;
	}
}

/// Sets the specified stat to the specified value.
/// Returns the new value.
static int pc_setstat(struct map_session_data *sd, int type, int val)
{
	nullpo_retr(-1, sd);

	switch( type ) {
		case SP_STR: sd->status.str = val; break;
		case SP_AGI: sd->status.agi = val; break;
		case SP_VIT: sd->status.vit = val; break;
		case SP_INT: sd->status.int_ = val; break;
		case SP_DEX: sd->status.dex = val; break;
		case SP_LUK: sd->status.luk = val; break;
		default:
			return -1;
	}

	return val;
}

// Calculates the number of status points PC gets when leveling up (from level to level+1)
int pc_gets_status_point(int level)
{
	if( battle_config.use_statpoint_table ) //Use values from "db/statpoint.txt"
		return (statp[level + 1] - statp[level]);
	else //Default increase
		return ((level + 15) / 5);
}

#ifdef RENEWAL_STAT
	/// Renewal status point cost formula
	#define PC_STATUS_POINT_COST(low) (((low) < 100) ? (2 + ((low) - 1) / 10) : (16 + 4 * (((low) - 100) / 5)))
#else
	/// Pre-Renewal status point cost formula
	#define PC_STATUS_POINT_COST(low) (1 + ((low) + 9) / 10)
#endif

/// Returns the number of stat points needed to change the specified stat by val.
/// If val is negative, returns the number of stat points that would be needed to
/// raise the specified stat from (current value - val) to current value.
int pc_need_status_point(struct map_session_data *sd, int type, int val)
{
	int low, high, sp = 0, max = 0;

	if( val == 0 )
		return 0;

	low = pc_getstat(sd,type);
	max = pc_maxparameter(sd,(enum e_params)(type - SP_STR));

	if( low >= max && val > 0 )
		return 0; //Official servers show '0' when max is reached

	high = low + val;

	if( val < 0 )
		swap(low, high);

	for( ; low < high; low++ )
		sp += PC_STATUS_POINT_COST(low);

	return sp;
}

/**
 * Returns the value the specified stat can be increased by with the current
 * amount of available status points for the current character's class.
 *
 * @param sd   The target character.
 * @param type Stat to verify.
 * @return Maximum value the stat could grow by.
 */
int pc_maxparameterincrease(struct map_session_data *sd, int type)
{
	int base, final_value, status_points, max_param;

	nullpo_ret(sd);

	base = final_value = pc_getstat(sd,type);
	status_points = sd->status.status_point;
	max_param = pc_maxparameter(sd,(enum e_params)(type - SP_STR));

	while( final_value <= max_param && status_points >= 0 ) {
		status_points -= PC_STATUS_POINT_COST(final_value);
		final_value++;
	}

	final_value--;

	return (final_value > base ? final_value - base : 0);
}

/**
 * Raises a stat by the specified amount.
 *
 * Obeys max_parameter limits.
 * Subtracts status points according to the cost of the increased stat points.
 *
 * @param sd       The target character.
 * @param type     The stat to change (see enum _sp)
 * @param increase The stat increase (strictly positive) amount.
 * @return true  if the stat was increased by any amount.
 * @return false if there were no changes.
 */
bool pc_statusup(struct map_session_data *sd, int type, int increase)
{
	int max_increase = 0, current = 0, needed_points = 0, final_value = 0;

	nullpo_ret(sd);

	if( type < SP_STR || type > SP_LUK || increase <= 0 ) {
		clif_statusupack(sd,type,0,0);
		return false;
	}

	//Check limits
	current = pc_getstat(sd,type);
	max_increase = pc_maxparameterincrease(sd,type);
	increase = cap_value(increase,0,max_increase); //Cap to the maximum status points available
	if( increase <= 0 || current + increase > pc_maxparameter(sd,(enum e_params)(type - SP_STR)) ) {
		clif_statusupack(sd,type,0,0);
		return false;
	}

	//Check status points
	needed_points = pc_need_status_point(sd,type,increase);
	if( needed_points < 0 || needed_points > sd->status.status_point ) { //Sanity check
		clif_statusupack(sd,type,0,0);
		return false;
	}

	//Set new values
	final_value = pc_setstat(sd,type,current + increase);
	sd->status.status_point -= needed_points;

	status_calc_pc(sd,SCO_NONE);

	//Update increase cost indicator
	clif_updatestatus(sd,SP_USTR + type - SP_STR);

	//Update statpoint count
	clif_updatestatus(sd,SP_STATUSPOINT);

	//Update stat value
	clif_statusupack(sd,type,1,final_value); //Required
	clif_updatestatus(sd,type);

	achievement_update_objective(sd,AG_GOAL_STATUS,1,final_value);

	return true;
}

/**
 * Raises a stat by the specified amount.
 *
 * Obeys max_parameter limits.
 * Does not subtract status points for the cost of the modified stat points.
 *
 * @param sd   The target character.
 * @param type The stat to change (see enum _sp)
 * @param val  The stat increase (or decrease) amount.
 * @return the stat increase amount.
 * @return 0 if no changes were made.
 */
int pc_statusup2(struct map_session_data *sd, int type, int val)
{
	int max, need;

	nullpo_ret(sd);

	if( type < SP_STR || type > SP_LUK ) {
		clif_statusupack(sd,type,0,0);
		return 0;
	}

	need = pc_need_status_point(sd,type,1);
	max = pc_maxparameter(sd,(enum e_params)(type - SP_STR)); //Set new value
	val = pc_setstat(sd,type,cap_value(pc_getstat(sd,type) + val,1,max));

	status_calc_pc(sd,SCO_NONE);

	//Update increase cost indicator
	if( need != pc_need_status_point(sd,type,1) )
		clif_updatestatus(sd,SP_USTR + type - SP_STR);

	//Update stat value
	clif_statusupack(sd,type,1,val); //Required
	clif_updatestatus(sd,type);

	return val;
}

// Checks to see if a skill exist's on a job's skill tree.
static bool pc_search_job_skilltree(int b_class, int id)
{
	int i;

	b_class = pc_class2idx(b_class);
	ARR_FIND(0, MAX_SKILL_TREE, i, (skill_tree[b_class][i].id == 0 || skill_tree[b_class][i].id == id));
	if( i < MAX_SKILL_TREE && skill_tree[b_class][i].id == id )
		return true;
	else
		return false;
}

/*==========================================
 * Update skill_lv for player sd
 * Skill point allocation
 *------------------------------------------*/
int pc_skillup(struct map_session_data *sd, uint16 skill_id)
{
	short used_skill_points, check_1st_job, check_2nd_job;
	int i, c = 0;

	nullpo_ret(sd);

	i = pc_calc_skilltree_normalize_job(sd);
	c = pc_mapid2jobid(i, sd->status.sex);

	if( c == -1 ) { //Unable to normalize job??
		ShowError("pc_skillup: Unable to normalize job %d for character %s (%d:%d)\n", i, sd->status.name, sd->status.account_id, sd->status.char_id);
		return 0;
	}

	c = pc_class2idx(c);

	if( skill_id >= GD_SKILLBASE && skill_id < GD_SKILLBASE + MAX_GUILDSKILL ) {
		guild_skillup(sd, skill_id);
		return 0;
	}

	if( skill_id >= HM_SKILLBASE && skill_id < HM_SKILLBASE + MAX_HOMUNSKILL && sd->hd ) {
		hom_skillup(sd->hd, skill_id);
		return 0;
	}

	if( skill_id >= MAX_SKILL )
		return 0;

	if( !pc_search_job_skilltree(c, skill_id) ) {
		used_skill_points = pc_calc_skillpoint(sd);
		if( (sd->class_&MAPID_UPPERMASK) == MAPID_SUPER_NOVICE ) {
			//Super Novice is the 2nd job of the Novice, but well treat it as 1st for the upcoming check and message
			check_1st_job = 9 + (sd->change_level_3rd - 1);
			check_2nd_job = 0;
		} else {
			check_1st_job = 9 + (sd->change_level_2nd - 1);
			check_2nd_job = 9 + (sd->change_level_3rd - 1) + (sd->change_level_2nd - 1);
		}
		if( used_skill_points < check_1st_job ) {
			clif_msg_value(sd, NEED_MORE_FIRSTJOBSKILL, (check_1st_job - used_skill_points));
			return 0;
		}
		if( used_skill_points < check_2nd_job ) {
			clif_msg_value(sd, NEED_MORE_SECONDJOBSKILL, (check_2nd_job - used_skill_points));
			return 0;
		}
	}

	if( sd->status.skill_point > 0 &&
		sd->status.skill[skill_id].id &&
		sd->status.skill[skill_id].flag == SKILL_FLAG_PERMANENT && //Don't allow raising while you have granted skills [Skotlex]
		sd->status.skill[skill_id].lv < skill_tree_get_max(skill_id, sd->status.class_) )
	{
		uint16 skill_lv;
		int range, upgradable;

		sd->status.skill[skill_id].lv++;
		sd->status.skill_point--;
		if( !skill_get_inf(skill_id) || (skill_get_inf3(skill_id)&INF3_BOOST_PASSIVE && (pc_checkskill(sd, SU_POWEROFLAND) > 0 || pc_checkskill(sd, SU_POWEROFSEA) > 0)) )
			status_calc_pc(sd, SCO_NONE); //Only recalculate for passive skills and active skills that boost the effects of passive skills
		else if( !sd->status.skill_point && pc_is_taekwon_ranker(sd) )
			pc_calc_skilltree(sd); //Required to grant all TK Ranker skills
		else
			pc_check_skilltree(sd, skill_id); //Check if a new skill can Lvlup
		skill_lv = sd->status.skill[skill_id].lv;
		range = skill_get_range2(&sd->bl, skill_id, skill_lv, false);
		upgradable = (skill_lv < skill_tree_get_max(sd->status.skill[skill_id].id, sd->status.class_) ? 1 : 0);
		clif_skillup(sd, skill_id, skill_lv, range, upgradable);
		clif_updatestatus(sd, SP_SKILLPOINT);
		if( skill_id == GN_REMODELING_CART ) //Cart weight info was updated by status_calc_pc
			clif_updatestatus(sd, SP_CARTINFO);
		if( pc_checkskill(sd, SG_DEVIL) > 0 && ((sd->class_&MAPID_THIRDMASK) == MAPID_STAR_EMPEROR || pc_is_maxjoblv(sd)) )
			clif_status_load(&sd->bl, SI_DEVIL1, 1);
		if( !pc_has_permission(sd, PC_PERM_ALL_SKILL) ) //May skill everything at any time anyways, and this would cause a huge slowdown
			clif_skillinfoblock(sd);
	}

	return 0;
}

/*==========================================
 * /allskill
 *------------------------------------------*/
int pc_allskillup(struct map_session_data *sd)
{
	int i;

	nullpo_ret(sd);

	for (i = 0; i < MAX_SKILL; i++) {
		if (sd->status.skill[i].flag != SKILL_FLAG_PERMANENT && sd->status.skill[i].flag != SKILL_FLAG_PERM_GRANTED && sd->status.skill[i].flag != SKILL_FLAG_PLAGIARIZED) {
			sd->status.skill[i].lv = (sd->status.skill[i].flag == SKILL_FLAG_TEMPORARY ? 0 : sd->status.skill[i].flag - SKILL_FLAG_REPLACED_LV_0);
			sd->status.skill[i].flag = SKILL_FLAG_PERMANENT;
			if (sd->status.skill[i].lv == 0)
				sd->status.skill[i].id = 0;
		}
	}

	if (pc_has_permission(sd,PC_PERM_ALL_SKILL)) {
		//Get ALL skills except npc/guild ones [Skotlex]
		//And except SG_DEVIL [Komurka] and MO_TRIPLEATTACK and RG_SNATCHER [ultramage]
		for (i = 0; i < MAX_SKILL; i++) {
			switch (i) {
				case SG_DEVIL:
				case MO_TRIPLEATTACK:
				case RG_SNATCHER:
					continue;
				default:
					if (!(skill_get_inf2(i)&(INF2_NPC_SKILL|INF2_GUILD_SKILL)) &&
						(sd->status.skill[i].lv = skill_get_max(i))) //Non existant skills should return a max of 0 anyway
						sd->status.skill[i].id = i;
					break;
			}
		}
	} else {
		int id;

		for (i = 0; i < MAX_SKILL_TREE && (id = skill_tree[pc_class2idx(sd->status.class_)][i].id) > 0; i++) {
			int inf2 = skill_get_inf2(id);

			if ((inf2&INF2_QUEST_SKILL && !battle_config.quest_skill_learn) ||
				(inf2&(INF2_WEDDING_SKILL|INF2_SPIRIT_SKILL)) ||
				id == SG_DEVIL)
				continue; //Cannot be learned normally
			sd->status.skill[id].id = id;
			sd->status.skill[id].lv = skill_tree_get_max(id,sd->status.class_); //Celest
		}
	}
	status_calc_pc(sd,SCO_NONE);
	//Required because if you could level up all skills previously
	//the update will not be sent as only the lv variable changes
	clif_skillinfoblock(sd);
	return 0;
}

/*==========================================
 * /resetlvl
 *------------------------------------------*/
int pc_resetlvl(struct map_session_data *sd,int type)
{
	int  i;

	nullpo_ret(sd);

	if (type != 3) //Also reset skills
		pc_resetskill(sd, 0);

	if (type == 1) {
		sd->status.skill_point = 0;
		sd->status.base_level = 1;
		sd->status.job_level = 1;
		sd->status.base_exp = 0;
		sd->status.job_exp = 0;
		if (sd->sc.option != 0)
			sd->sc.option = 0;

		sd->status.str = 1;
		sd->status.agi = 1;
		sd->status.vit = 1;
		sd->status.int_ = 1;
		sd->status.dex = 1;
		sd->status.luk = 1;
		if (sd->status.class_ == JOB_NOVICE_HIGH) {
			sd->status.status_point = 100; //Not 88 [celest]
			//Give platinum skills upon changing
			pc_skill(sd, NV_FIRSTAID, 1, 0);
			pc_skill(sd, NV_TRICKDEAD, 1, 0);
		}
	}

	if (type == 2) {
		sd->status.skill_point = 0;
		sd->status.base_level = 1;
		sd->status.job_level = 1;
		sd->status.base_exp = 0;
		sd->status.job_exp = 0;
	}
	if (type == 3) {
		sd->status.base_level = 1;
		sd->status.base_exp = 0;
	}
	if (type == 4) {
		sd->status.job_level = 1;
		sd->status.job_exp = 0;
	}

	clif_updatestatus(sd, SP_STATUSPOINT);
	clif_updatestatus(sd, SP_STR);
	clif_updatestatus(sd, SP_AGI);
	clif_updatestatus(sd, SP_VIT);
	clif_updatestatus(sd, SP_INT);
	clif_updatestatus(sd, SP_DEX);
	clif_updatestatus(sd, SP_LUK);
	clif_updatestatus(sd, SP_BASELEVEL);
	clif_updatestatus(sd, SP_JOBLEVEL);
	clif_updatestatus(sd, SP_STATUSPOINT);
	clif_updatestatus(sd, SP_BASEEXP);
	clif_updatestatus(sd, SP_JOBEXP);
	clif_updatestatus(sd, SP_NEXTBASEEXP);
	clif_updatestatus(sd, SP_NEXTJOBEXP);
	clif_updatestatus(sd, SP_SKILLPOINT);

	clif_updatestatus(sd, SP_USTR); //Updates needed stat points - Valaris
	clif_updatestatus(sd, SP_UAGI);
	clif_updatestatus(sd, SP_UVIT);
	clif_updatestatus(sd, SP_UINT);
	clif_updatestatus(sd, SP_UDEX);
	clif_updatestatus(sd, SP_ULUK); //End Addition

	for (i = 0; i < EQI_MAX; i++) { //Unequip items that can't be equipped by base 1 [Valaris]
		if (sd->equip_index[i] >= 0 && pc_isequip(sd, sd->equip_index[i]))
			pc_unequipitem(sd, sd->equip_index[i], 2);
	}

	if ((type == 1 || type == 2 || type == 3) && sd->status.party_id)
		party_send_levelup(sd);

	status_calc_pc(sd, SCO_FORCE);
	clif_skillinfoblock(sd);

	return 0;
}
/*==========================================
 * /resetstate
 *------------------------------------------*/
int pc_resetstate(struct map_session_data *sd)
{
	nullpo_ret(sd);

	if (battle_config.use_statpoint_table) { //New statpoint table used here - Dexity
		if (sd->status.base_level > MAX_LEVEL) { //statp[] goes out of bounds, can't reset!
			ShowError("pc_resetstate: Can't reset stats of %d:%d, the base level (%d) is greater than the max level supported (%d)\n",
				sd->status.account_id, sd->status.char_id, sd->status.base_level, MAX_LEVEL);
			return 0;
		}

		sd->status.status_point = statp[sd->status.base_level] + (sd->class_&JOBL_UPPER ? 52 : 0); //Extra 52 + 48 = 100 stat points
	} else {
		int add = 0;
		add += pc_need_status_point(sd, SP_STR, 1 - pc_getstat(sd, SP_STR));
		add += pc_need_status_point(sd, SP_AGI, 1 - pc_getstat(sd, SP_AGI));
		add += pc_need_status_point(sd, SP_VIT, 1 - pc_getstat(sd, SP_VIT));
		add += pc_need_status_point(sd, SP_INT, 1 - pc_getstat(sd, SP_INT));
		add += pc_need_status_point(sd, SP_DEX, 1 - pc_getstat(sd, SP_DEX));
		add += pc_need_status_point(sd, SP_LUK, 1 - pc_getstat(sd, SP_LUK));

		sd->status.status_point += add;
	}

	pc_setstat(sd, SP_STR, 1);
	pc_setstat(sd, SP_AGI, 1);
	pc_setstat(sd, SP_VIT, 1);
	pc_setstat(sd, SP_INT, 1);
	pc_setstat(sd, SP_DEX, 1);
	pc_setstat(sd, SP_LUK, 1);

	clif_updatestatus(sd, SP_STR);
	clif_updatestatus(sd, SP_AGI);
	clif_updatestatus(sd, SP_VIT);
	clif_updatestatus(sd, SP_INT);
	clif_updatestatus(sd, SP_DEX);
	clif_updatestatus(sd, SP_LUK);

	clif_updatestatus(sd, SP_USTR);	//Updates needed stat points - Valaris
	clif_updatestatus(sd, SP_UAGI);
	clif_updatestatus(sd, SP_UVIT);
	clif_updatestatus(sd, SP_UINT);
	clif_updatestatus(sd, SP_UDEX);
	clif_updatestatus(sd, SP_ULUK);	//End Addition

	clif_updatestatus(sd, SP_STATUSPOINT);

	if (sd->mission_mobid) { //bugreport:2200
		sd->mission_mobid = 0;
		sd->mission_count = 0;
		pc_setglobalreg(sd, "TK_MISSION_ID", 0);
	}

	status_calc_pc(sd, SCO_NONE);

	return 1;
}

/*==========================================
 * /resetskill
 * if flag&1, perform block resync and status_calc call.
 * if flag&2, just count total amount of skill points used by player, do not really reset.
 * if flag&4, just reset the skills if the player class is a bard/dancer type (for changesex.)
 *------------------------------------------*/
int pc_resetskill(struct map_session_data *sd, int flag)
{
	int i, skill_point = 0;

	nullpo_ret(sd);

	if( flag&4 && (sd->class_&MAPID_UPPERMASK) != MAPID_BARDDANCER )
		return 0;

	if( !(flag&2) ) { //Remove stuff lost when resetting skills
		//It has been confirmed on official servers that when you reset skills with a ranked Taekwon your skills are not reset (because you have all of them anyway)
		if( pc_is_taekwon_ranker(sd) )
			return 0;
		if( pc_checkskill(sd, SG_DEVIL) > 0 )
			clif_status_load(&sd->bl, SI_DEVIL1, 0); //Remove permanent blindness due to skill-reset [Skotlex]
		i = sd->sc.option;
		if( i&OPTION_RIDING && pc_checkskill(sd, KN_RIDING) > 0 )
			i &= ~OPTION_RIDING;
		if( i&OPTION_FALCON && pc_checkskill(sd, HT_FALCON) > 0 )
			i &= ~OPTION_FALCON;
		if( i&OPTION_DRAGON && pc_checkskill(sd, RK_DRAGONTRAINING) > 0 )
			i &= ~OPTION_DRAGON;
		if( i&OPTION_WUG && pc_checkskill(sd, RA_WUGMASTERY) > 0 )
			i &= ~OPTION_WUG;
		if( i&OPTION_WUGRIDER && pc_checkskill(sd, RA_WUGRIDER) > 0 )
			i &= ~OPTION_WUGRIDER;
		if( i&OPTION_MADOGEAR && (sd->class_&MAPID_THIRDMASK) == MAPID_MECHANIC )
			i &= ~OPTION_MADOGEAR;
		if( pc_checkskill(sd, MC_PUSHCART) > 0 ) {
#ifndef NEW_CARTS
			if( i&OPTION_CART )
				i &= ~OPTION_CART;
#else
			if( sd->sc.data[SC_PUSH_CART] )
				pc_setcart(sd, 0);
#endif
		}
		if( i != sd->sc.option )
			pc_setoption(sd, i);
		if( hom_is_active(sd->hd) && pc_checkskill(sd, AM_CALLHOMUN) )
			hom_vaporize(sd, HOM_ST_REST);
		if( sd->sc.data[SC_SPRITEMABLE] && pc_checkskill(sd, SU_SPRITEMABLE) > 0 )
			status_change_end(&sd->bl, SC_SPRITEMABLE, INVALID_TIMER);
		if( sd->sc.data[SC_SOULATTACK] && pc_checkskill(sd, SU_SOULATTACK) > 0 )
			status_change_end(&sd->bl, SC_SOULATTACK, INVALID_TIMER);
	}

	for( i = 1; i < MAX_SKILL; i++ ) {
		int inf2;
		int lv = sd->status.skill[i].lv;

		if( lv < 1 )
			continue;
		inf2 = skill_get_inf2(i);
		if( inf2&(INF2_WEDDING_SKILL|INF2_SPIRIT_SKILL) ) //Avoid reseting wedding/linker skills
			continue;
		//Don't reset trick dead if not a novice/baby
		if( i == NV_TRICKDEAD && (sd->class_&MAPID_UPPERMASK) != MAPID_NOVICE ) {
			sd->status.skill[i].lv = 0;
			sd->status.skill[i].flag = SKILL_FLAG_PERMANENT;
			continue;
		}
		//Do not reset basic skill
		if( i == NV_BASIC && (sd->class_&MAPID_UPPERMASK) != MAPID_NOVICE )
			continue;
		if( sd->status.skill[i].flag == SKILL_FLAG_PERM_GRANTED )
			continue;
		if( flag&4 && !skill_ischangesex(i) )
			continue;
		if( inf2&INF2_QUEST_SKILL && !battle_config.quest_skill_learn ) {
			//Only handle quest skills in a special way when you can't learn them manually
			if( battle_config.quest_skill_reset && !(flag&2) ) { //Wipe them
				sd->status.skill[i].lv = 0;
				sd->status.skill[i].flag = SKILL_FLAG_PERMANENT;
			}
			continue;
		}
		if( sd->status.skill[i].flag == SKILL_FLAG_PERMANENT )
			skill_point += lv;
		else if( sd->status.skill[i].flag == SKILL_FLAG_REPLACED_LV_0 )
			skill_point += (sd->status.skill[i].flag - SKILL_FLAG_REPLACED_LV_0);
		if( !(flag&2) ) { //Reset
			sd->status.skill[i].lv = 0;
			sd->status.skill[i].flag = SKILL_FLAG_PERMANENT;
		}
	}
	if( flag&2 || !skill_point )
		return skill_point;
	sd->status.skill_point += skill_point;
	if( !(flag&2) ) { //Remove all statuses that can't be inactivated without a skill
		if( sd->sc.data[SC_READYSTORM] )
			status_change_end(&sd->bl, SC_READYSTORM, INVALID_TIMER);
		if( sd->sc.data[SC_READYDOWN] )
			status_change_end(&sd->bl, SC_READYDOWN, INVALID_TIMER);
		if( sd->sc.data[SC_READYTURN] )
			status_change_end(&sd->bl, SC_READYTURN, INVALID_TIMER);
		if( sd->sc.data[SC_READYCOUNTER] )
			status_change_end(&sd->bl, SC_READYCOUNTER, INVALID_TIMER);
		if( sd->sc.data[SC_DODGE] )
			status_change_end(&sd->bl, SC_DODGE, INVALID_TIMER);
	}
	if( flag&1 ) {
		clif_updatestatus(sd, SP_SKILLPOINT);
		clif_skillinfoblock(sd);
		status_calc_pc(sd, SCO_FORCE);
	}
	return skill_point;
}

/*==========================================
 * /resetfeel [Komurka]
 *------------------------------------------*/
int pc_resetfeel(struct map_session_data *sd)
{
	int i;

	nullpo_ret(sd);

	for( i = 0; i < MAX_PC_FEELHATE; i++ ) {
		sd->feel_map[i].m = -1;
		sd->feel_map[i].index = 0;
		pc_setglobalreg(sd,sg_info[i].feel_var,0);
	}

	return 0;
}

int pc_resethate(struct map_session_data *sd)
{
	int i;

	nullpo_ret(sd);

	for( i = 0; i < MAX_PC_FEELHATE; i++ ) {
		sd->hate_mob[i] = -1;
		pc_setglobalreg(sd,sg_info[i].hate_var,0);
	}
	return 0;
}

int pc_skillatk_bonus(struct map_session_data *sd, uint16 skill_id)
{
	int i, bonus = 0;

	nullpo_ret(sd);

	skill_id = skill_dummy2skill_id(skill_id);
	ARR_FIND(0, MAX_PC_BONUS, i, sd->skillatk[i].id == skill_id);
	if( i < MAX_PC_BONUS )
		bonus = sd->skillatk[i].val;

	return bonus;
}

int pc_sub_skillatk_bonus(struct map_session_data *sd, uint16 skill_id)
{
	int i, bonus = 0;

	nullpo_ret(sd);

	skill_id = skill_dummy2skill_id(skill_id);
	ARR_FIND(0, MAX_PC_BONUS, i, sd->subskill[i].id == skill_id);
	if( i < MAX_PC_BONUS )
		bonus = sd->subskill[i].val;

	return bonus;
}

int pc_skillheal_bonus(struct map_session_data *sd, uint16 skill_id) {
	int i, bonus = sd->bonus.add_heal_rate;

	if( bonus ) {
		switch( skill_id ) {
			case AL_HEAL:           if( !(battle_config.skill_add_heal_rate&1) ) bonus = 0; break;
			case PR_SANCTUARY:      if( !(battle_config.skill_add_heal_rate&2) ) bonus = 0; break;
			case AM_POTIONPITCHER:  if( !(battle_config.skill_add_heal_rate&4) ) bonus = 0; break;
			case CR_SLIMPITCHER:    if( !(battle_config.skill_add_heal_rate&8) ) bonus = 0; break;
			case BA_APPLEIDUN:      if( !(battle_config.skill_add_heal_rate&16) ) bonus = 0; break;
			case AB_HIGHNESSHEAL:   if( !(battle_config.skill_add_heal_rate&32) ) bonus = 0; break;
		}
	}

	ARR_FIND(0, MAX_PC_BONUS, i, sd->skillheal[i].id == skill_id);
	if( i < MAX_PC_BONUS )
		bonus += sd->skillheal[i].val;

	return bonus;
}

int pc_skillheal2_bonus(struct map_session_data *sd, uint16 skill_id) {
	int i, bonus = sd->bonus.add_heal2_rate;

	ARR_FIND(0, MAX_PC_BONUS, i, sd->skillheal2[i].id == skill_id);
	if( i < MAX_PC_BONUS )
		bonus += sd->skillheal2[i].val;

	return bonus;
}

void pc_respawn(struct map_session_data *sd, clr_type clrtype)
{
	if( !pc_isdead(sd) )
		return; // Not applicable

	if( sd->bg_id && bg_member_respawn(sd) )
		return; // Member revived by battleground

	pc_setstand(sd);
	pc_setrestartvalue(sd, 3);

	if( pc_setpos(sd, sd->status.save_point.map, sd->status.save_point.x, sd->status.save_point.y, clrtype) != SETPOS_OK )
		clif_resurrection(&sd->bl, 1); //If warping fails, send a normal stand up packet
}

static TIMER_FUNC(pc_respawn_timer)
{
	struct map_session_data *sd = NULL;

	if( (sd = map_id2sd(id)) ) {
		sd->pvp_point = 0;
		pc_respawn(sd,CLR_OUTSIGHT);
	}

	return 0;
}

/*==========================================
 * Invoked when a player has received damage
 *------------------------------------------*/
void pc_damage(struct map_session_data *sd,struct block_list *src,unsigned int hp, unsigned int sp)
{
	if( sp )
		clif_updatestatus(sd,SP_SP);

	if( hp )
		clif_updatestatus(sd,SP_HP);
	else
		return;

	if( !src )
		return;

	if( pc_issit(sd) ) {
		pc_setstand(sd);
		skill_sit(sd,false);
	}

	if( sd->progressbar.npc_id )
		clif_progressbar_abort(sd);

	if( sd->status.pet_id > 0 && sd->pd && battle_config.pet_damage_support )
		pet_target_check(sd->pd,src,1);

	if( sd->status.ele_id > 0 )
		elemental_set_target(sd,src);

	if( battle_config.prevent_logout_trigger&PLT_DAMAGE )
		sd->canlog_tick = gettick();
}

TIMER_FUNC(pc_close_npc_timer)
{
	TBL_PC *sd = map_id2sd(id);

	if( sd )
		pc_close_npc(sd,data);
	return 0;
}

/**
 * Method to properly close a NPC for player and clear anything related.
 * @param sd: Player attached
 * @param flag: Method of closure
 *   1: Produce a close button and end the NPC
 *   2: End the NPC (best for no dialog windows)
 */
void pc_close_npc(struct map_session_data *sd, int flag) {
	nullpo_retv(sd);

	if (sd->npc_id || sd->npc_shopid) {
		if (sd->state.using_fake_npc) {
			clif_clearunit_single(sd->npc_id, CLR_OUTSIGHT, sd->fd);
			sd->state.using_fake_npc = 0;
		}
		if (sd->st) {
			if (sd->st->state == RUN) { //Wait ending code execution
				add_timer(gettick() + 500,pc_close_npc_timer,sd->bl.id,flag);
				return;
			}
			sd->st->state = ((flag == 1 && sd->st->mes_active) ? CLOSE : END);
			sd->st->mes_active = 0;
		}
		sd->state.menu_or_input = 0;
		sd->npc_menu = 0;
		sd->npc_shopid = 0;
#ifdef SECURE_NPCTIMEOUT
		sd->npc_idle_timer = INVALID_TIMER;
#endif
		if (sd->st) {
			if (sd->st->state == CLOSE) {
				clif_scriptclose(sd,sd->npc_id);
				clif_scriptclear(sd,sd->npc_id);
				sd->st->state = END; //Force to end now
			}
			if (sd->st->state == END) { //Free attached scripts that are waiting
				script_free_state(sd->st);
				sd->st = NULL;
				sd->npc_id = 0;
			}
		}
	}
}

/*==========================================
 * Invoked when a player has negative current hp
 *------------------------------------------*/
int pc_dead(struct map_session_data *sd, struct block_list *src)
{
	int k = 0;
	unsigned int tick = gettick();

	nullpo_retr(0,sd);

	//Activate Steel body if a super novice dies at 99+% EXP [celest]
	//Super Novices have no kill or die functions attached when saved by their angel
	if( (sd->class_&MAPID_UPPERMASK) == MAPID_SUPER_NOVICE && !sd->state.snovice_dead_flag ) {
		uint32 nextb = pc_nextbaseexp(sd);

		if( nextb && get_percentage(sd->status.base_exp,nextb) >= 99 ) {
			sd->state.snovice_dead_flag = 1;
			pc_setrestartvalue(sd,1);
			status_percent_heal(&sd->bl,100,100);
			clif_resurrection(&sd->bl,1);
			if( battle_config.pc_invincible_time )
				pc_setinvincibletimer(sd,battle_config.pc_invincible_time);
			sc_start(&sd->bl,&sd->bl,status_skill2sc(MO_STEELBODY),100,5,skill_get_time(MO_STEELBODY,5));
			if( map_flag_gvg2(sd->bl.m) )
				pc_respawn_timer(INVALID_TIMER,gettick(),sd->bl.id,0);
			return 0;
		}
	}

	for( k = 0; k < MAX_DEVOTION; k++ ) {
		if( sd->devotion[k] ) {
			struct block_list *bl = map_id2bl(sd->devotion[k]);

			status_change_end(bl,SC_DEVOTION,INVALID_TIMER);
		}
	}

	pc_crimson_marks_clear(sd);

	for( k = 0; k < MAX_HOWL_MINES; k++ ) {
		if( sd->howl_mine[k] ) {
			struct block_list *bl = map_id2bl(sd->howl_mine[k]);

			status_change_end(bl,SC_H_MINE,INVALID_TIMER);
		}
	}

	for( k = 0; k < MAX_STELLAR_MARKS; k++ ) {
		if( sd->stellar_mark[k] ) {
			struct block_list *bl = map_id2bl(sd->stellar_mark[k]);

			status_change_end(bl,SC_FLASHKICK,INVALID_TIMER);
		}
	}

	pc_united_souls_clear(sd);

	if( sd->status.pet_id > 0 && sd->pd ) {
		struct pet_data *pd = sd->pd;

		if( !mapdata[sd->bl.m].flag.noexppenalty ) {
			pet_set_intimate(pd,pd->pet.intimate - pd->petDB->die);
			if( pd->pet.intimate < PETINTIMATE_AWKWARD )
				pd->pet.intimate = 0;
			clif_send_petdata(sd,sd->pd,1,pd->pet.intimate);
		}
		if( sd->pd->target_id ) //Unlock all targets
			pet_unlocktarget(sd->pd);
	}

	if( hom_is_active(sd->hd) && battle_config.homunculus_auto_vapor )
		hom_vaporize(sd,HOM_ST_REST);

	if( sd->md )
		mercenary_delete(sd->md,3); //Your mercenary soldier has ran away

	if( sd->ed )
		elemental_delete(sd->ed,0);

	//Leave duel if you die [LuzZza]
	if( battle_config.duel_autoleave_when_die ) {
		if( sd->duel_group > 0 )
			duel_leave(sd->duel_group,sd);
		if( sd->duel_invite > 0 )
			duel_reject(sd->duel_invite,sd);
	}

	pc_close_npc(sd,2); //Close npc if we were using one

	//e.g. not killed through pc_damage
	if( pc_issit(sd) )
		clif_status_load(&sd->bl,SI_SIT,0);

	pc_setdead(sd);
	clif_party_dead(sd);
	pc_setglobalreg(sd,PCDIECOUNTER_VAR,sd->die_counter + 1);
	pc_setparam(sd,SP_KILLERRID,src ? src->id : 0);

	//Reset menu skills/item skills
	if( sd->skillitem )
		sd->skillitem = sd->skillitemlv = 0;
	if( sd->menuskill_id )
		sd->menuskill_id = sd->menuskill_val = sd->menuskill_val2 = 0;
	//Reset ticks
	sd->hp_loss.tick = sd->sp_loss.tick = sd->hp_regen.tick = sd->sp_regen.tick = 0;

	if( sd->spiritball > 0 )
		pc_delspiritball(sd,sd->spiritball,0);

	if( sd->shieldball > 0 )
		pc_delshieldball(sd,sd->shieldball,0);

	if( sd->rageball > 0 )
		pc_delrageball(sd,sd->rageball,0);

	if( sd->charmball_type != CHARM_TYPE_NONE && sd->charmball > 0 )
		pc_delcharmball(sd,sd->charmball,sd->charmball_type);

	if( sd->soulball > 0 )
		pc_delsoulball(sd,sd->soulball,0);

	if( src ) {
		switch( src->type ) {
			case BL_PC: {
					struct map_session_data *ssd = (struct map_session_data *)src;

					pc_setparam(ssd,SP_KILLEDRID,sd->bl.id);
					npc_script_event(ssd,NPCE_KILLPC);

					if( battle_config.pk_mode&2 ) {
						ssd->status.manner -= 5;
						if( ssd->status.manner < 0 )
							sc_start(&sd->bl,src,SC_NOCHAT,100,0,0);
#if 0
						//PK/Karma system code (not enabled yet) [celest]
						//originally from Kade Online, so i don't know if any of these is correct ^^;
						//NOTE: karma is measured REVERSE, so more karma = more 'evil' / less honourable,
						//karma going down = more 'good' / more honourable.
						//The Karma System way.

						if( sd->status.karma > ssd->status.karma ) { //If player killed was more evil
							sd->status.karma--;
							ssd->status.karma--;
						} else if( sd->status.karma < ssd->status.karma ) // If player killed was more good
							ssd->status.karma++;

						//Or the PK System way
						if( sd->status.karma > 0 ) //Player killed is dishonourable?
							ssd->status.karma--; //Honour points earned
						sd->status.karma++;	//Honour points lost

						//@TODO: Receive exp on certain occasions
#endif
					}
				}
				break;
			case BL_MOB: {
					struct mob_data *md = (struct mob_data *)src;

					if( md->target_id == sd->bl.id )
						mob_unlocktarget(md,tick);
					if( battle_config.mobs_level_up && md->status.hp &&
						(unsigned int)md->level < pc_maxbaselv(sd) &&
						!md->guardian_data && !md->special_state.ai //Guardians/summons should not level. [Skotlex]
					) { //Monster level up [Valaris]
						clif_misceffect(&md->bl,0);
						md->level++;
						status_calc_mob(md,SCO_NONE);
						status_percent_heal(src,10,0);

						if( battle_config.show_mob_info&4 ) //Update name with new level
							clif_name_area(&md->bl);
					}
					src = battle_get_master(src); //Maybe Player Summon
				}
				break;
			case BL_PET: //Pass on to master
			case BL_HOM:
			case BL_MER:
				src = battle_get_master(src);
				break;
		}
	}

	if( battle_config.bone_drop == 2 || (battle_config.bone_drop == 1 && mapdata[sd->bl.m].flag.pvp) ) {
		struct item item_tmp;

		memset(&item_tmp,0,sizeof(item_tmp));
		item_tmp.nameid = ITEMID_SKULL_;
		item_tmp.identify = 1;
		item_tmp.card[0] = CARD0_CREATE;
		item_tmp.card[1] = 0;
		item_tmp.card[2] = GetWord(sd->status.char_id,0); //CharId
		item_tmp.card[3] = GetWord(sd->status.char_id,1);
		map_addflooritem(&item_tmp,1,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0,0,false);
	}

	//Remove bonus_script when dead
	pc_bonus_script_clear(sd,BSF_REM_ON_DEAD);

	//Changed penalty options, added death by player if pk_mode [Valaris]
	if( battle_config.death_penalty_type &&
		(sd->class_&MAPID_UPPERMASK) != MAPID_NOVICE && //Only novices will receive no penalty
		!mapdata[sd->bl.m].flag.noexppenalty && !map_flag_gvg2(sd->bl.m) &&
		!sd->sc.data[SC_BABY] && !sd->sc.data[SC_LIFEINSURANCE] )
	{
		uint32 base_penalty = 0;
		uint32 job_penalty = 0;
		int zeny_penalty = 0;

		if( pc_isvip(sd) ) { //EXP penalty for VIP
			base_penalty = battle_config.vip_exp_penalty_base;
			if( battle_config.death_penalty_base != 100 )
				base_penalty = umin(base_penalty,battle_config.death_penalty_base);
			job_penalty = battle_config.vip_exp_penalty_job;
			if( battle_config.death_penalty_job != 100 )
				job_penalty = umin(job_penalty,battle_config.death_penalty_job);
			zeny_penalty = battle_config.vip_zeny_penalty;
		} else {
			base_penalty = battle_config.death_penalty_base;
			job_penalty = battle_config.death_penalty_job;
			zeny_penalty = battle_config.zeny_penalty;
		}

		if( ((battle_config.death_penalty_maxlv&1) || !pc_is_maxbaselv(sd)) && base_penalty > 0 ) {
			switch( battle_config.death_penalty_type ) {
				case 1: base_penalty = (uint32)(pc_nextbaseexp(sd) * (base_penalty / 10000.)); break;
				case 2: base_penalty = (uint32)(sd->status.base_exp * (base_penalty / 10000.)); break;
			}
			if( base_penalty ) { //Recheck after altering to speedup
				if( battle_config.pk_mode && src && src->type == BL_PC )
					base_penalty *= 2;
				base_penalty = umin(sd->status.base_exp,base_penalty);
			}
		} else
			base_penalty = 0;

		if( ((battle_config.death_penalty_maxlv&2) || !pc_is_maxjoblv(sd)) && job_penalty > 0 ) {
			switch( battle_config.death_penalty_type ) {
				case 1: job_penalty = (uint32)(pc_nextjobexp(sd) * (job_penalty / 10000.)); break;
				case 2: job_penalty = (uint32)(sd->status.job_exp * (job_penalty / 10000.)); break;
			}
			if( job_penalty ) {
				if( battle_config.pk_mode && src && src->type == BL_PC )
					job_penalty *= 2;
				job_penalty = umin(sd->status.job_exp,job_penalty);
			}
		} else
			job_penalty = 0;

		if( base_penalty || job_penalty )
			pc_lostexp(sd,base_penalty,job_penalty);

		if( zeny_penalty > 0 && !mapdata[sd->bl.m].flag.nozenypenalty ) {
			zeny_penalty = (uint32)(sd->status.zeny * (zeny_penalty / 10000.));
			if( zeny_penalty )
				pc_payzeny(sd,zeny_penalty,LOG_TYPE_PICKDROP_PLAYER,NULL);
		}
	}

	//Moved this outside so it works when PVP isn't enabled and during pk mode [Ancyker]
	if( mapdata[sd->bl.m].flag.pvp_nightmaredrop ) {
		int j;

		for( j = 0; j < MAX_DROP_PER_MAP; j++ ) {
			int id = mapdata[sd->bl.m].drop_list[j].drop_id;
			int type = mapdata[sd->bl.m].drop_list[j].drop_type;
			int per = mapdata[sd->bl.m].drop_list[j].drop_per;
			int i;

			if( !id )
				continue;
			if( id == -1 ) {
				int eq_num = 0, eq_n[MAX_INVENTORY];

				memset(eq_n,0,sizeof(eq_n));
				for( i = 0; i < MAX_INVENTORY; i++ ) {
					if( (type == 1 && !sd->inventory.u.items_inventory[i].equip) ||
						(type == 2 && sd->inventory.u.items_inventory[i].equip) ||
						type == 3 )
					{
						int l;

						ARR_FIND(0,MAX_INVENTORY,l,eq_n[l] <= 0);
						if( l < MAX_INVENTORY )
							eq_n[l] = i;
						eq_num++;
					}
				}
				if( eq_num > 0 ) {
					int n = eq_n[rnd()%eq_num];

					if( rnd()%10000 < per ) {
						if( sd->inventory.u.items_inventory[n].equip )
							pc_unequipitem(sd,n,1|2);
						pc_dropitem(sd,n,1);
					}
				}
			} else if( id > 0 ) {
				for( i = 0; i < MAX_INVENTORY; i++ ) {
					if( sd->inventory.u.items_inventory[i].nameid == id &&
						rnd()%10000 < per &&
						((type == 1 && !sd->inventory.u.items_inventory[i].equip) ||
						(type == 2 && sd->inventory.u.items_inventory[i].equip) ||
						type == 3) )
					{
						if( sd->inventory.u.items_inventory[i].equip )
							pc_unequipitem(sd,i,1|2);
						pc_dropitem(sd,i,1);
						break;
					}
				}
			}
		}
	}

	//Remove autotrade to prevent autotrading from save point
	if( sd->state.autotrade && map_flag_vs(sd->bl.m) ) {
		if( sd->state.vending )
			vending_closevending(sd);
		if( sd->state.buyingstore )
			buyingstore_close(sd);
		map_quit(sd);
	}

	//PVP
	//Disable certain pvp functions on pk_mode [Valaris]
	if( mapdata[sd->bl.m].flag.pvp && !battle_config.pk_mode && !mapdata[sd->bl.m].flag.pvp_nocalcrank ) {
		sd->pvp_point -= 5;
		sd->pvp_lost++;
		if( src && src->type == BL_PC ) {
			struct map_session_data *ssd = (struct map_session_data *)src;

			ssd->pvp_point++;
			ssd->pvp_won++;
		}
		if( sd->pvp_point < 0 ) {
			add_timer(tick + 1,pc_respawn_timer,sd->bl.id,0);
			return 1|8;
		}
	}

	//GvG
	if( map_flag_gvg2(sd->bl.m) ) {
		add_timer(tick + 1,pc_respawn_timer,sd->bl.id,0);
		return 1|8;
	} else if( sd->bg_id ) {
		struct battleground_data *bg = bg_team_search(sd->bg_id);

		if( bg && bg->mapindex > 0 ) { //Respawn by BG
			add_timer(tick + 1,pc_respawn_timer,sd->bl.id,0);
			return 1|8;
		}
	}

	//Reset "can log out" tick
	if( battle_config.prevent_logout )
		sd->canlog_tick = gettick() - battle_config.prevent_logout;

	return 1;
}

void pc_revive(struct map_session_data *sd,unsigned int hp, unsigned int sp)
{
	if( hp )
		clif_updatestatus(sd,SP_HP);

	if( sp )
		clif_updatestatus(sd,SP_SP);

	pc_setstand(sd);

	if( battle_config.pc_invincible_time > 0 )
		pc_setinvincibletimer(sd,battle_config.pc_invincible_time);

	if( sd->state.gmaster_flag ) {
		guild_guildaura_refresh(sd,GD_LEADERSHIP,guild_checkskill(sd->guild,GD_LEADERSHIP));
		guild_guildaura_refresh(sd,GD_GLORYWOUNDS,guild_checkskill(sd->guild,GD_GLORYWOUNDS));
		guild_guildaura_refresh(sd,GD_SOULCOLD,guild_checkskill(sd->guild,GD_SOULCOLD));
		guild_guildaura_refresh(sd,GD_HAWKEYES,guild_checkskill(sd->guild,GD_HAWKEYES));
	}
}

// script
//
/*==========================================
 * script reading pc status registry
 *------------------------------------------*/
int pc_readparam(struct map_session_data *sd, int type)
{
	int val = 0;

	nullpo_ret(sd);

	switch( type ) {
		case SP_SKILLPOINT:		val = sd->status.skill_point; break;
		case SP_STATUSPOINT:		val = sd->status.status_point; break;
		case SP_ZENY:			val = sd->status.zeny; break;
		case SP_BASELEVEL:		val = sd->status.base_level; break;
		case SP_JOBLEVEL:		val = sd->status.job_level; break;
		case SP_CLASS:			val = sd->status.class_; break;
		case SP_BASEJOB:		val = pc_mapid2jobid(sd->class_&MAPID_UPPERMASK, sd->status.sex); break; //Base job, extracting upper type
		case SP_UPPER:			val = (sd->class_&JOBL_UPPER) ? 1 : (sd->class_&JOBL_BABY) ? 2 : 0; break;
		case SP_BASECLASS:		val = pc_mapid2jobid(sd->class_&MAPID_BASEMASK, sd->status.sex); break; //Extract base class tree [Skotlex]
		case SP_SEX:			val = sd->status.sex; break;
		case SP_WEIGHT:			val = sd->weight; break;
		case SP_MAXWEIGHT:		val = sd->max_weight; break;
		case SP_BASEEXP:		val = sd->status.base_exp; break;
		case SP_JOBEXP:			val = sd->status.job_exp; break;
		case SP_NEXTBASEEXP:		val = pc_nextbaseexp(sd); break;
		case SP_NEXTJOBEXP:		val = pc_nextjobexp(sd); break;
		case SP_HP:			val = sd->battle_status.hp; break;
		case SP_MAXHP:			val = sd->battle_status.max_hp; break;
		case SP_SP:			val = sd->battle_status.sp; break;
		case SP_MAXSP:			val = sd->battle_status.max_sp; break;
		case SP_STR:			val = sd->status.str; break;
		case SP_AGI:			val = sd->status.agi; break;
		case SP_VIT:			val = sd->status.vit; break;
		case SP_INT:			val = sd->status.int_; break;
		case SP_DEX:			val = sd->status.dex; break;
		case SP_LUK:			val = sd->status.luk; break;
		case SP_KARMA:			val = sd->status.karma; break;
		case SP_MANNER:			val = sd->status.manner; break;
		case SP_FAME:			val = sd->status.fame; break;
		case SP_KILLERRID:		val = sd->killerrid; break;
		case SP_KILLEDRID:		val = sd->killedrid; break;
		case SP_SITTING:		val = (pc_issit(sd) ? 1 : 0); break;
		case SP_CHARMOVE:		val = sd->status.character_moves; break;
		case SP_CHARRENAME:		val = sd->status.rename; break;
		case SP_CHARFONT:		val = sd->status.font; break;
		case SP_BANK_VAULT:		val = sd->bank_vault; break;
		case SP_CASHPOINTS:		val = sd->cashPoints; break;
		case SP_KAFRAPOINTS:		val = sd->kafraPoints; break;
		case SP_ROULETTE_BRONZE:	val = sd->roulette_point.bronze; break;
		case SP_ROULETTE_SILVER:	val = sd->roulette_point.silver; break;
		case SP_ROULETTE_GOLD:		val = sd->roulette_point.gold; break;
		case SP_KILLEDGID:		val = sd->killedgid; break;
		case SP_PCDIECOUNTER:		val = sd->die_counter; break;
		case SP_COOKMASTERY:		val = sd->cook_mastery; break;
		case SP_LANGTYPE:		val = sd->langtype; break;
		case SP_CRITICAL:		val = sd->battle_status.cri / 10; break;
		case SP_ASPD:			val = (2000 - sd->battle_status.amotion) / 10; break;
		case SP_BASE_ATK:
#ifdef RENEWAL
			val = sd->bonus.eatk;
#else
			val = sd->battle_status.batk;
#endif
			break;
		case SP_DEF1:			val = sd->battle_status.def; break;
		case SP_DEF2:			val = sd->battle_status.def2; break;
		case SP_MDEF1:			val = sd->battle_status.mdef; break;
		case SP_MDEF2:			val = sd->battle_status.mdef2; break;
		case SP_HIT:			val = sd->battle_status.hit; break;
		case SP_FLEE1:			val = sd->battle_status.flee; break;
		case SP_FLEE2:			val = sd->battle_status.flee2; break;
		case SP_DEFELE:			val = sd->battle_status.def_ele; break;
		case SP_MAXHPRATE:		val = sd->hprate; break;
		case SP_MAXSPRATE:		val = sd->sprate; break;
		case SP_SPRATE:			val = sd->dsprate; break;
		case SP_SPEED_RATE:		val = sd->bonus.speed_rate; break;
		case SP_SPEED_ADDRATE:		val = sd->bonus.speed_add_rate; break;
		case SP_ASPD_RATE:
#ifndef RENEWAL_ASPD
			val = sd->battle_status.aspd_rate;
#else
			val = sd->battle_status.aspd_rate2;
#endif
			break;
		case SP_HP_RECOV_RATE:		val = sd->hprecov_rate; break;
		case SP_SP_RECOV_RATE:		val = sd->sprecov_rate; break;
		case SP_CRITICAL_DEF:		val = sd->bonus.critical_def; break;
		case SP_NEAR_ATK_DEF:		val = sd->bonus.near_attack_def_rate; break;
		case SP_LONG_ATK_DEF:		val = sd->bonus.long_attack_def_rate; break;
		case SP_DOUBLE_RATE:		val = sd->bonus.double_rate; break;
		case SP_DOUBLE_ADD_RATE:	val = sd->bonus.double_add_rate; break;
		case SP_MATK_RATE:		val = sd->matk_rate; break;
		case SP_ATK_RATE:		val = sd->bonus.atk_rate; break;
		case SP_MAGIC_ATK_DEF:		val = sd->bonus.magic_def_rate; break;
		case SP_MISC_ATK_DEF:		val = sd->bonus.misc_def_rate; break;
		case SP_PERFECT_HIT_RATE:	val = sd->bonus.perfect_hit; break;
		case SP_PERFECT_HIT_ADD_RATE:	val = sd->bonus.perfect_hit_add; break;
		case SP_CRITICAL_RATE:		val = sd->critical_rate; break;
		case SP_HIT_RATE:		val = sd->hit_rate; break;
		case SP_FLEE_RATE:		val = sd->flee_rate; break;
		case SP_FLEE2_RATE:		val = sd->flee2_rate; break;
		case SP_DEF_RATE:		val = sd->def_rate; break;
		case SP_DEF2_RATE:		val = sd->def2_rate; break;
		case SP_MDEF_RATE:		val = sd->mdef_rate; break;
		case SP_MDEF2_RATE:		val = sd->mdef2_rate; break;
		case SP_RESTART_FULL_RECOVER:	val = (sd->special_state.restart_full_recover ? 1 : 0); break;
		case SP_NO_CASTCANCEL:		val = (sd->special_state.no_castcancel ? 1 : 0); break;
		case SP_NO_CASTCANCEL2:		val = (sd->special_state.no_castcancel2 ? 1 : 0); break;
		case SP_NO_SIZEFIX:		val = (sd->special_state.no_sizefix ? 1 : 0); break;
		case SP_NO_MAGIC_DAMAGE:	val = sd->special_state.no_magic_damage; break;
		case SP_NO_WEAPON_DAMAGE:	val = sd->special_state.no_weapon_damage; break;
		case SP_NO_MISC_DAMAGE:		val = sd->special_state.no_misc_damage; break;
		case SP_NO_GEMSTONE:		val = (sd->special_state.no_gemstone ? 1 : 0); break;
		case SP_INTRAVISION:		val = (sd->special_state.intravision ? 1 : 0); break;
		case SP_NO_KNOCKBACK:		val = (sd->special_state.no_knockback ? 1 : 0); break;
		case SP_SPLASH_RANGE:		val = sd->bonus.splash_range; break;
		case SP_SPLASH_ADD_RANGE:	val = sd->bonus.splash_add_range; break;
		case SP_SHORT_WEAPON_DAMAGE_RETURN:	val = sd->bonus.short_weapon_damage_return; break;
		case SP_LONG_WEAPON_DAMAGE_RETURN:	val = sd->bonus.long_weapon_damage_return; break;
		case SP_MAGIC_DAMAGE_RETURN:	val = sd->bonus.magic_damage_return; break;
		case SP_PERFECT_HIDE:		val = (sd->special_state.perfect_hiding ? 1 : 0); break;
		case SP_UNBREAKABLE:		val = sd->bonus.unbreakable; break;
		case SP_UNBREAKABLE_WEAPON:	val = (sd->bonus.unbreakable_equip&EQP_WEAPON) ? 1 : 0; break;
		case SP_UNBREAKABLE_ARMOR:	val = (sd->bonus.unbreakable_equip&EQP_ARMOR) ? 1 : 0; break;
		case SP_UNBREAKABLE_HELM:	val = (sd->bonus.unbreakable_equip&EQP_HELM) ? 1 : 0; break;
		case SP_UNBREAKABLE_SHIELD:	val = (sd->bonus.unbreakable_equip&EQP_SHIELD) ? 1 : 0; break;
		case SP_UNBREAKABLE_GARMENT:	val = (sd->bonus.unbreakable_equip&EQP_GARMENT) ? 1 : 0; break;
		case SP_UNBREAKABLE_SHOES:	val = (sd->bonus.unbreakable_equip&EQP_SHOES) ? 1 : 0; break;
		case SP_CLASSCHANGE:		val = sd->bonus.classchange; break;
		case SP_LONG_ATK_RATE:		val = sd->bonus.long_attack_atk_rate; break;
		case SP_BREAK_WEAPON_RATE:	val = sd->bonus.break_weapon_rate; break;
		case SP_BREAK_ARMOR_RATE:	val = sd->bonus.break_armor_rate; break;
		case SP_ADD_STEAL_RATE:		val = sd->bonus.add_steal_rate; break;
		case SP_DELAYRATE:		val = sd->delayrate; break;
		case SP_COOLDOWNRATE:		val = sd->cooldownrate; break;
		case SP_CRIT_ATK_RATE:		val = sd->bonus.crit_atk_rate; break;
		case SP_UNSTRIPABLE:		val = sd->bonus.unstripable; break;
		case SP_UNSTRIPABLE_WEAPON:	val = (sd->bonus.unstripable_equip&EQP_WEAPON) ? 1 : 0; break;
		case SP_UNSTRIPABLE_ARMOR:	val = (sd->bonus.unstripable_equip&EQP_ARMOR) ? 1 : 0; break;
		case SP_UNSTRIPABLE_HELM:	val = (sd->bonus.unstripable_equip&EQP_HELM) ? 1 : 0; break;
		case SP_UNSTRIPABLE_SHIELD:	val = (sd->bonus.unstripable_equip&EQP_SHIELD) ? 1 : 0; break;
		case SP_SP_GAIN_VALUE:		val = sd->bonus.sp_gain_value; break;
		case SP_HP_GAIN_VALUE:		val = sd->bonus.hp_gain_value; break;
		case SP_MAGIC_SP_GAIN_VALUE:	val = sd->bonus.magic_sp_gain_value; break;
		case SP_MAGIC_HP_GAIN_VALUE:	val = sd->bonus.magic_hp_gain_value; break;
		case SP_ADD_HEAL_RATE:		val = sd->bonus.add_heal_rate; break;
		case SP_ADD_HEAL2_RATE:		val = sd->bonus.add_heal2_rate; break;
		case SP_ADD_ITEM_HEAL_RATE:	val = sd->bonus.itemhealrate2; break;
		case SP_EMATK:			val = sd->bonus.ematk; break;
#ifdef RENEWAL_CAST
		case SP_VARCASTRATE:		val = sd->bonus.varcastrate; break;
		case SP_FIXCASTRATE:		val = sd->bonus.fixcastrate; break;
		case SP_ADD_VARIABLECAST:	val = sd->bonus.add_varcast; break;
		case SP_ADD_FIXEDCAST:		val = sd->bonus.add_fixcast; break;
#else
		case SP_CASTRATE:		val = sd->castrate; break;
#endif
		case SP_NO_MADO_FUEL:	val = (sd->special_state.no_mado_fuel ? 1 : 0); break;
		case SP_NO_WALKDELAY:		val = (sd->special_state.no_walkdelay ? 1 : 0); break;
		default:
			ShowError("pc_readparam: Attempt to read unknown parameter '%d'.\n", type);
			return -1;
	}

	return val;
}

/*==========================================
 * script set pc status registry
 *------------------------------------------*/
bool pc_setparam(struct map_session_data *sd, int type, int val) {
	nullpo_retr(false,sd);

	switch( type ) {
		case SP_BASELEVEL:
			if( (unsigned int)val > pc_maxbaselv(sd) ) //Capping to max
				val = pc_maxbaselv(sd);
			if( (unsigned int)val > sd->status.base_level ) {
				int i = 0;
				int stat = 0;

				for( i = 0; i < (int)((unsigned int)val - sd->status.base_level); i++ )
					stat += pc_gets_status_point(sd->status.base_level + i);
				sd->status.status_point += stat;
			}
			sd->status.base_level = (unsigned int)val;
			sd->status.base_exp = 0;
			//clif_updatestatus(sd, SP_BASELEVEL); //Gets updated at the bottom
			clif_updatestatus(sd, SP_NEXTBASEEXP);
			clif_updatestatus(sd, SP_STATUSPOINT);
			clif_updatestatus(sd, SP_BASEEXP);
			status_calc_pc(sd, SCO_FORCE);
			if( sd->status.party_id )
				party_send_levelup(sd);
			break;
		case SP_JOBLEVEL:
			if( (unsigned int)val >= sd->status.job_level ) {
				if( (unsigned int)val > pc_maxjoblv(sd) )
					val = pc_maxjoblv(sd);
				sd->status.skill_point += val - sd->status.job_level;
				clif_updatestatus(sd, SP_SKILLPOINT);
			}
			sd->status.job_level = (unsigned int)val;
			sd->status.job_exp = 0;
			//clif_updatestatus(sd, SP_JOBLEVEL); //Gets updated at the bottom
			clif_updatestatus(sd, SP_NEXTJOBEXP);
			clif_updatestatus(sd, SP_JOBEXP);
			status_calc_pc(sd, SCO_FORCE);
			break;
		case SP_SKILLPOINT:
			sd->status.skill_point = val;
			break;
		case SP_STATUSPOINT:
			sd->status.status_point = val;
			break;
		case SP_ZENY:
			if( val < 0 )
				return false; //Can't set negative zeny
			log_zeny(sd, LOG_TYPE_SCRIPT, sd, -(sd->status.zeny - cap_value(val, 0, MAX_ZENY)));
			sd->status.zeny = cap_value(val, 0, MAX_ZENY);
			break;
		case SP_BASEEXP:
			val = cap_value(val, 0, INT_MAX);
			if ((uint32)val < sd->status.base_exp)
				pc_lostexp(sd, sd->status.base_exp - val, 0); //Lost
			else
				pc_gainexp(sd, NULL, val - sd->status.base_exp, 0, 2); //Gained
			return true;
		case SP_JOBEXP:
			val = cap_value(val, 0, INT_MAX);
			if ((uint32)val < sd->status.job_exp)
				pc_lostexp(sd, 0, sd->status.job_exp - val); //Lost
			else
				pc_gainexp(sd, NULL, 0, val - sd->status.job_exp, 2); //Gained
			return true;
		case SP_SEX:
			sd->status.sex = (val ? SEX_MALE : SEX_FEMALE);
			break;
		case SP_WEIGHT:
			sd->weight = val;
			break;
		case SP_MAXWEIGHT:
			sd->max_weight = val;
			break;
		case SP_HP:
			sd->battle_status.hp = cap_value(val, 1, (int)sd->battle_status.max_hp);
			break;
		case SP_MAXHP:
			if( sd->status.base_level < 100 )
				sd->battle_status.max_hp = cap_value(val, 1, battle_config.max_hp_lv99);
			else if( sd->status.base_level < 151 )
				sd->battle_status.max_hp = cap_value(val, 1, battle_config.max_hp_lv150);
			else
				sd->battle_status.max_hp = cap_value(val, 1, battle_config.max_hp);
			if( sd->battle_status.max_hp < sd->battle_status.hp ) {
				sd->battle_status.hp = sd->battle_status.max_hp;
				clif_updatestatus(sd, SP_HP);
			}
			break;
		case SP_SP:
			sd->battle_status.sp = cap_value(val, 0, (int)sd->battle_status.max_sp);
			break;
		case SP_MAXSP:
			sd->battle_status.max_sp = cap_value(val, 1, battle_config.max_sp);
			if( sd->battle_status.max_sp < sd->battle_status.sp ) {
				sd->battle_status.sp = sd->battle_status.max_sp;
				clif_updatestatus(sd, SP_SP);
			}
			break;
		case SP_STR:
			sd->status.str = cap_value(val, 1, pc_maxparameter(sd, PARAM_STR));
			break;
		case SP_AGI:
			sd->status.agi = cap_value(val, 1, pc_maxparameter(sd, PARAM_AGI));
			break;
		case SP_VIT:
			sd->status.vit = cap_value(val, 1, pc_maxparameter(sd, PARAM_VIT));
			break;
		case SP_INT:
			sd->status.int_ = cap_value(val, 1, pc_maxparameter(sd, PARAM_INT));
			break;
		case SP_DEX:
			sd->status.dex = cap_value(val, 1, pc_maxparameter(sd, PARAM_DEX));
			break;
		case SP_LUK:
			sd->status.luk = cap_value(val, 1, pc_maxparameter(sd, PARAM_LUK));
			break;
		case SP_KARMA:
			sd->status.karma = val;
			break;
		case SP_MANNER:
			sd->status.manner = val;
			if( val < 0 )
				sc_start(NULL, &sd->bl, SC_NOCHAT, 100, 0, 0);
			else {
				status_change_end(&sd->bl, SC_NOCHAT, INVALID_TIMER);
				clif_manner_message(sd, 5);
			}
			return true; //status_change_start/status_change_end already sends packets warning the client
		case SP_FAME:
			sd->status.fame = val;
			break;
		case SP_KILLERRID:
			sd->killerrid = val;
			return true;
		case SP_KILLEDRID:
			sd->killedrid = val;
			return true;
		case SP_CHARMOVE:
			sd->status.character_moves = val;
			return true;
		case SP_CHARRENAME:
			sd->status.rename = val;
			return true;
		case SP_CHARFONT:
			sd->status.font = val;
			clif_font(sd);
			return true;
		case SP_BANK_VAULT:
			if( val < 0 )
				return false;
			log_zeny(sd, LOG_TYPE_BANK, sd, -(sd->bank_vault - cap_value(val, 0, MAX_BANK_ZENY)));
			sd->bank_vault = cap_value(val, 0, MAX_BANK_ZENY);
			pc_setreg2(sd, BANK_VAULT_VAR, sd->bank_vault);
			return true;
		case SP_ROULETTE_BRONZE:
			sd->roulette_point.bronze = val;
			pc_setreg2(sd, ROULETTE_BRONZE_VAR, sd->roulette_point.bronze);
			return true;
		case SP_ROULETTE_SILVER:
			sd->roulette_point.silver = val;
			pc_setreg2(sd, ROULETTE_SILVER_VAR, sd->roulette_point.silver);
			return true;
		case SP_ROULETTE_GOLD:
			sd->roulette_point.gold = val;
			pc_setreg2(sd, ROULETTE_GOLD_VAR, sd->roulette_point.gold);
			return true;
		case SP_KILLEDGID:
			sd->killedgid = val;
			return true;
		case SP_CASHPOINTS:
			if( val < 0 )
				return false;
			if( !sd->state.connect_new )
				log_cash(sd, LOG_TYPE_SCRIPT, LOG_CASH_TYPE_CASH, -(sd->cashPoints - cap_value(val, 0, MAX_ZENY)));
			sd->cashPoints = cap_value(val, 0, MAX_ZENY);
			pc_setaccountreg(sd, CASHPOINT_VAR, sd->cashPoints);
			return true;
		case SP_KAFRAPOINTS:
			if( val < 0 )
				return false;
			if( !sd->state.connect_new )
				log_cash(sd, LOG_TYPE_SCRIPT, LOG_CASH_TYPE_KAFRA, -(sd->kafraPoints - cap_value(val, 0, MAX_ZENY)));
			sd->kafraPoints = cap_value(val, 0, MAX_ZENY);
			pc_setaccountreg(sd, KAFRAPOINT_VAR, sd->kafraPoints);
			return true;
		case SP_PCDIECOUNTER:
			if( val < 0 )
				return false;
			if( sd->die_counter == val )
				return true;
			sd->die_counter = val;
			if( !sd->die_counter && (sd->class_&MAPID_UPPERMASK) == MAPID_SUPER_NOVICE )
				status_calc_pc(sd, SCO_NONE); //Lost the bonus
			pc_setglobalreg(sd, PCDIECOUNTER_VAR, sd->die_counter);
			return true;
		case SP_COOKMASTERY:
			if( val < 0 )
				return false;
			if( sd->cook_mastery == val )
				return true;
			val = cap_value(val, 0, 1999);
			sd->cook_mastery = val;
			pc_setglobalreg(sd, COOKMASTERY_VAR, sd->cook_mastery);
			return true;
		case SP_LANGTYPE:
			sd->langtype = val;
			pc_setaccountreg(sd, LANGTYPE_VAR, sd->langtype);
			return true;
		default:
			ShowError("pc_setparam: Attempted to set unknown parameter '%d'.\n", type);
			return false;
	}
	clif_updatestatus(sd,type);

	return true;
}

/*==========================================
 * HP/SP Healing. If flag is passed, the heal type is through clif_heal, otherwise update status.
 *------------------------------------------*/
void pc_heal(struct map_session_data *sd, unsigned int hp, unsigned int sp, int type)
{
	if (type) {
		if (hp)
			clif_heal(sd->fd,SP_HP,hp);
		if (sp)
			clif_heal(sd->fd,SP_SP,sp);
	} else {
		if(hp)
			clif_updatestatus(sd,SP_HP);
		if(sp)
			clif_updatestatus(sd,SP_SP);
	}
}

/**
 * Heal player HP and/or SP linearly. Calculate any bonus based on active statuses.
 * @param sd: Player data
 * @param itemid: Item ID
 * @param hp: HP to heal
 * @param sp: SP to heal
 * @param fixed
 * @return Amount healed to an object
 */
int pc_itemheal(struct map_session_data *sd, int itemid, int hp, int sp, bool fixed)
{
	int bonus, tmp, penalty = 0;

	if(!fixed) {
		if(hp) {
			int i;

			bonus = 100 + (sd->battle_status.vit<<1) + pc_checkskill(sd,SM_RECOVERY) * 10 + pc_checkskill(sd,AM_LEARNINGPOTION) * 5;
			//A potion produced by an Alchemist in the Fame Top 10 gets +50% effect [DracoRPG]
			if(potion_flag == 2) {
				bonus += 50;
				if(sd->sc.data[SC_SPIRIT] && sd->sc.data[SC_SPIRIT]->val2 == SL_ROGUE)
					bonus += 100; //Receive an additional +100% effect from ranked potions to HP only
			}
			//All item bonuses
			bonus += sd->bonus.itemhealrate2;
			//Item Group bonuses
			bonus += pc_get_itemgroup_bonus(sd,itemid);
			//Individual item bonuses
			ARR_FIND(0, MAX_PC_BONUS, i, sd->itemhealrate[i].id == itemid);
			if(i < MAX_PC_BONUS)
				bonus += sd->itemhealrate[i].val;
			//Recovery Potion
			if(sd->sc.data[SC_INCHEALRATE])
				bonus += sd->sc.data[SC_INCHEALRATE]->val1;
			//2014 Halloween Event : Pumpkin Bonus
			if(sd->sc.data[SC_MTF_PUMPKIN] && itemid == ITEMID_PUMPKIN)
				bonus += sd->sc.data[SC_MTF_PUMPKIN]->val1;
			//Overflow check
			tmp = hp * bonus / 100;
			if(bonus != 100 && tmp > hp)
				hp = tmp;
		}
		if(sp) {
			bonus = 100 + (sd->battle_status.int_<<1) + pc_checkskill(sd,MG_SRECOVERY) * 10 + pc_checkskill(sd,AM_LEARNINGPOTION) * 5;
			if(potion_flag == 2)
				bonus += 50;
			tmp = sp * bonus / 100;
			if(bonus != 100 && tmp > sp)
				sp = tmp;
		}
		if(sd->sc.data[SC_VITALITYACTIVATION]) {
			hp += hp * 50 / 100;
			sp -= sp * 50 / 100;
		}
		if(sd->sc.data[SC_WATER_INSIGNIA] && sd->sc.data[SC_WATER_INSIGNIA]->val1 == 2) {
			hp += hp / 10;
			sp += sp / 10;
		}
		//Critical Wound and Death Hurt stacks with each other
		if(sd->sc.data[SC_CRITICALWOUND])
			penalty += sd->sc.data[SC_CRITICALWOUND]->val2;
		if(sd->sc.data[SC_DEATHHURT])
			penalty += 20;
	}
	if(sd->sc.data[SC_NORECOVER_STATE])
		penalty = 100;
	//Apply a penalty to recovery if there is one
	if(penalty > 0) {
		hp -= hp * penalty / 100;
		sp -= sp * penalty / 100;
	}
	if(sd->sc.data[SC_EXTREMITYFIST2])
		sp = 0;
	if(sd->sc.data[SC_BITESCAR])
		hp = 0;
	return status_heal(&sd->bl,hp,sp,1);
}

/*==========================================
 * HP/SP Recovery
 * Heal player hp nad/or sp by rate
 *------------------------------------------*/
int pc_percentheal(struct map_session_data *sd,int hp,int sp)
{
	nullpo_ret(sd);

	if(hp > 100)
		hp = 100;
	else if(hp < -100)
		hp = -100;

	if(sp > 100)
		sp = 100;
	else if(sp <-100)
		sp = -100;

	if(hp >= 0 && sp >= 0) //Heal
		return status_percent_heal(&sd->bl, hp, sp);

	if(hp <= 0 && sp <= 0) //Damage (negative rates indicate % of max rather than current), and only kill target IF the specified amount is 100%
		return status_percent_damage(NULL, &sd->bl, hp, sp, hp == -100);

	//Crossed signs
	if(hp) {
		if(hp > 0)
			status_percent_heal(&sd->bl, hp, 0);
		else
			status_percent_damage(NULL, &sd->bl, hp, 0, hp == -100);
	}

	if(sp) {
		if(sp > 0)
			status_percent_heal(&sd->bl, 0, sp);
		else
			status_percent_damage(NULL, &sd->bl, 0, sp, false);
	}
	return 0;
}

static int jobchange_killclone(struct block_list *bl, va_list ap)
{
	struct mob_data *md;
	int flag;

	md = (struct mob_data *)bl;
	nullpo_ret(md);
	flag = va_arg(ap, int);

	if (md->master_id && md->special_state.clone && md->master_id == flag)
		status_kill(&md->bl);
	return 1;
}

/**
 * Called when player changes job
 * Rewrote to make it tidider [Celest]
 * @param sd
 * @param job JOB ID. See enum e_job
 * @param upper 1 - JOBL_UPPER; 2 - JOBL_BABY
 * @return True if success, false if failed
 */
bool pc_jobchange(struct map_session_data *sd, int job, char upper)
{
	int i, fame_flag = 0;
	int b_class;

	nullpo_retr(false,sd);

	if (job < 0)
		return false;

	//Normalize job
	b_class = pc_jobid2mapid(job);
	if (b_class == -1)
		return false;

	switch (upper) {
		case 1:
			b_class |= JOBL_UPPER;
			break;
		case 2:
			b_class |= JOBL_BABY;
			break;
	}

	//This will automatically adjust bard/dancer classes to the correct gender
	//That is, if you try to jobchange into dancer, it will turn you to bard
	job = pc_mapid2jobid(b_class, sd->status.sex);
	if (job == -1)
		return false;

	if ((unsigned short)b_class == sd->class_)
		return false; //Nothing to change

	if ((b_class&JOBL_2) && !(sd->class_&JOBL_2) && (b_class&MAPID_UPPERMASK) != MAPID_SUPER_NOVICE) {
		//Changing from 1st to 2nd job
		sd->change_level_2nd = sd->status.job_level;
		pc_setglobalreg (sd,"jobchange_level",sd->change_level_2nd);
	} else if ((b_class&JOBL_THIRD) && !(sd->class_&JOBL_THIRD) && (b_class&MAPID_THIRDMASK) != MAPID_SUPER_NOVICE_E) {
		//Changing from 2nd to 3rd job
		sd->change_level_3rd = sd->status.job_level;
		pc_setglobalreg (sd,"jobchange_level_3rd",sd->change_level_3rd);
	}

	if (sd->cloneskill_idx >= 0) {
		if (sd->status.skill[sd->cloneskill_idx].flag == SKILL_FLAG_PLAGIARIZED) {
			sd->status.skill[sd->cloneskill_idx].id = 0;
			sd->status.skill[sd->cloneskill_idx].lv = 0;
			sd->status.skill[sd->cloneskill_idx].flag = SKILL_FLAG_PERMANENT;
			clif_deleteskill(sd,pc_readglobalreg(sd,SKILL_VAR_PLAGIARISM));
		}
		sd->cloneskill_idx = -1;
		pc_setglobalreg(sd,SKILL_VAR_PLAGIARISM,0);
		pc_setglobalreg(sd,SKILL_VAR_PLAGIARISM_LV,0);
	}

	if (sd->reproduceskill_idx >= 0) {
		if (sd->status.skill[sd->reproduceskill_idx].flag == SKILL_FLAG_PLAGIARIZED) {
			sd->status.skill[sd->reproduceskill_idx].id = 0;
			sd->status.skill[sd->reproduceskill_idx].lv = 0;
			sd->status.skill[sd->reproduceskill_idx].flag = SKILL_FLAG_PERMANENT;
			clif_deleteskill(sd,pc_readglobalreg(sd,SKILL_VAR_REPRODUCE));
		}
		sd->reproduceskill_idx = -1;
		pc_setglobalreg(sd,SKILL_VAR_REPRODUCE,0);
		pc_setglobalreg(sd,SKILL_VAR_REPRODUCE_LV,0);
	}

	//Give or reduce transcendent status points
	if ((b_class&JOBL_UPPER) && !(sd->class_&JOBL_UPPER)) { //Change from a non t class to a t class -> give points
		sd->status.status_point += battle_config.transcendent_status_points;
		clif_updatestatus(sd,SP_STATUSPOINT);
	} else if (!(b_class&JOBL_UPPER) && (sd->class_&JOBL_UPPER)) { //Change from a t class to a non t class -> remove points
		if (sd->status.status_point < battle_config.transcendent_status_points) {
			//The player already used his bonus points, so we have to reset his status points
			pc_resetstate(sd);
		}
		sd->status.status_point -= battle_config.transcendent_status_points;
		clif_updatestatus(sd,SP_STATUSPOINT);
	}

	if ((b_class&MAPID_UPPERMASK) != (sd->class_&MAPID_UPPERMASK)) { //Things to remove when changing class tree
		const int class_ = pc_class2idx(sd->status.class_);
		uint16 skill_id;

		for (i = 0; i < MAX_SKILL_TREE && (skill_id = skill_tree[class_][i].id) > 0; i++) {
			enum sc_type sc = status_skill2sc(skill_id);

			//Remove status specific to your current tree skills
			if (sc > SC_COMMON_MAX && sd->sc.data[sc])
				status_change_end(&sd->bl,sc,INVALID_TIMER);
		}
	}

	//Going off star glad lineage, reset feel to not store no-longer-used vars in the database
	if ((sd->class_&MAPID_UPPERMASK) == MAPID_STAR_GLADIATOR && (b_class&MAPID_UPPERMASK) != MAPID_STAR_GLADIATOR)
		pc_resetfeel(sd);

	//Reset body style to 0 before changing job to avoid errors since not every job has a alternate outfit
	sd->status.body = 0;
	clif_changelook(&sd->bl,LOOK_BODY2,0);

	sd->status.class_ = job;
	fame_flag = pc_famerank(sd->status.char_id,sd->class_&MAPID_UPPERMASK);
	sd->class_ = (unsigned short)b_class;
	sd->status.job_level = 1;
	sd->status.job_exp = 0;

	if (sd->status.base_level > pc_maxbaselv(sd)) {
		sd->status.base_level = pc_maxbaselv(sd);
		sd->status.base_exp = 0;
		pc_resetstate(sd);
		clif_updatestatus(sd,SP_STATUSPOINT);
		clif_updatestatus(sd,SP_BASELEVEL);
		clif_updatestatus(sd,SP_BASEEXP);
		clif_updatestatus(sd,SP_NEXTBASEEXP);
	}

	clif_updatestatus(sd,SP_JOBLEVEL);
	clif_updatestatus(sd,SP_JOBEXP);
	clif_updatestatus(sd,SP_NEXTJOBEXP);

	for (i = 0; i < EQI_MAX; i++) {
		if (sd->equip_index[i] >= 0 && pc_isequip(sd,sd->equip_index[i]))
			pc_unequipitem(sd,sd->equip_index[i],2); //Unequip invalid item for class
	}

	//Change look, if disguised, you need to undisguise
	//to correctly calculate new job sprite without
	if (sd->disguise)
		pc_disguise(sd,0);

	status_set_viewdata(&sd->bl,job);
	clif_changelook(&sd->bl,LOOK_BASE,sd->vd.class_); //Move sprite update to prevent client crashes with incompatible equipment [Valaris]
#if PACKETVER >= 20151104
	clif_changelook(&sd->bl,LOOK_HAIR,sd->vd.hair_style); //Update player's head (only matters when switching to or from Doram)
#endif
	if(sd->vd.cloth_color)
		clif_changelook(&sd->bl,LOOK_CLOTHES_COLOR,sd->vd.cloth_color);
	//if(sd->vd.body_style)
	//	clif_changelook(&sd->bl,LOOK_BODY2,sd->vd.body_style);

	//Update skill tree
	pc_calc_skilltree(sd);
	clif_skillinfoblock(sd);

	if (sd->ed)
		elemental_delete(sd->ed, 0);
	if (sd->state.vending)
		vending_closevending(sd);
	if (sd->state.buyingstore)
		buyingstore_close(sd);

	map_foreachinmap(jobchange_killclone,sd->bl.m,BL_MOB,sd->bl.id);

	//Remove peco/cart/falcon
	i = sd->sc.option;
	if (i&OPTION_RIDING && !pc_checkskill(sd,KN_RIDING))
		i &= ~OPTION_RIDING;
	if (i&OPTION_FALCON && !pc_checkskill(sd,HT_FALCON))
		i &= ~OPTION_FALCON;
	if (i&OPTION_DRAGON && !pc_checkskill(sd,RK_DRAGONTRAINING))
		i &= ~OPTION_DRAGON;
	if (i&OPTION_WUGRIDER && !pc_checkskill(sd,RA_WUGMASTERY))
		i &= ~OPTION_WUGRIDER;
	if (i&OPTION_WUG && !pc_checkskill(sd,RA_WUGMASTERY))
		i &= ~OPTION_WUG;
	if (i&OPTION_MADOGEAR) //You do not need a skill for this
		i &= ~OPTION_MADOGEAR;
	if (!pc_checkskill(sd,MC_PUSHCART)) {
#ifndef NEW_CARTS
		if (i&OPTION_CART)
			i &= ~OPTION_CART;
#else
		if (sd->sc.data[SC_PUSH_CART])
			pc_setcart(sd,0);
#endif
	}
	if (i != sd->sc.option)
		pc_setoption(sd,i);

	if (hom_is_active(sd->hd) && !pc_checkskill(sd,AM_CALLHOMUN))
		hom_vaporize(sd,HOM_ST_REST);

	if (sd->sc.data[SC_SPRITEMABLE] && !pc_checkskill(sd,SU_SPRITEMABLE))
		status_change_end(&sd->bl,SC_SPRITEMABLE,INVALID_TIMER);

	if (sd->sc.data[SC_SOULATTACK] && !pc_checkskill(sd,SU_SOULATTACK))
		status_change_end(&sd->bl,SC_SOULATTACK,INVALID_TIMER);

	if (sd->status.manner < 0)
		clif_changestatus(sd,SP_MANNER,sd->status.manner);

	status_calc_pc(sd,SCO_FORCE);
	pc_checkallowskill(sd);
	pc_equiplookall(sd);
	pc_show_questinfo(sd);
	pc_update_job_and_level(sd);
	achievement_update_objective(sd,AG_JOB_CHANGE,1,job);
	chrif_save(sd,CSAVE_NORMAL);

	//If you were previously famous, not anymore.
	if (fame_flag)
		chrif_buildfamelist();
	else if (sd->status.fame > 0) {
		switch (sd->class_&MAPID_UPPERMASK) { //It may be that now they are famous?
			case MAPID_BLACKSMITH:
			case MAPID_ALCHEMIST:
			case MAPID_TAEKWON:
				chrif_buildfamelist();
				break;
		}
	}

	return true;
}

/*==========================================
 * Tell client player sd has change equipement
 *------------------------------------------*/
void pc_equiplookall(struct map_session_data *sd)
{
	nullpo_retv(sd);

	clif_changelook(&sd->bl,LOOK_WEAPON,0);
	clif_changelook(&sd->bl,LOOK_SHOES,0);
	clif_changelook(&sd->bl,LOOK_HEAD_BOTTOM,sd->status.head_bottom);
	clif_changelook(&sd->bl,LOOK_HEAD_TOP,sd->status.head_top);
	clif_changelook(&sd->bl,LOOK_HEAD_MID,sd->status.head_mid);
	clif_changelook(&sd->bl,LOOK_ROBE,sd->status.robe);
}

/*==========================================
 * Tell client player sd has change look (hair,equip...)
 *------------------------------------------*/
void pc_changelook(struct map_session_data *sd, int type, int val)
{
	nullpo_retv(sd);

	switch(type) {
		case LOOK_BASE:
			status_set_viewdata(&sd->bl,val);
			clif_changelook(&sd->bl,LOOK_BASE,sd->vd.class_);
			clif_changelook(&sd->bl,LOOK_WEAPON,sd->status.weapon);
			if (sd->vd.cloth_color)
				clif_changelook(&sd->bl,LOOK_CLOTHES_COLOR,sd->vd.cloth_color);
			clif_skillinfoblock(sd);
			return;
		case LOOK_HAIR: //Use the battle_config limits! [Skotlex]
			if ((sd->class_&MAPID_BASEMASK) == MAPID_SUMMONER)
				val = cap_value(val,MIN_DORAM_HAIR_STYLE,MAX_DORAM_HAIR_STYLE);
			else
				val = cap_value(val,MIN_HAIR_STYLE,MAX_HAIR_STYLE);
			if (sd->status.hair != val) {
				sd->status.hair = val;
				if (sd->status.guild_id) //Update Guild Window [Skotlex]
					intif_guild_change_memberinfo(sd->status.guild_id,sd->status.account_id,sd->status.char_id,
					GMI_HAIR,&sd->status.hair,sizeof(sd->status.hair));
			}
			break;
		case LOOK_WEAPON:
			sd->status.weapon = val;
			break;
		case LOOK_HEAD_BOTTOM:
			sd->status.head_bottom = val;
			sd->setlook_head_bottom = val;
			break;
		case LOOK_HEAD_TOP:
			sd->status.head_top = val;
			sd->setlook_head_top = val;
			break;
		case LOOK_HEAD_MID:
			sd->status.head_mid = val;
			sd->setlook_head_mid = val;
			break;
		case LOOK_HAIR_COLOR: //Use the battle_config limits! [Skotlex]
			if ((sd->class_&MAPID_BASEMASK) == MAPID_SUMMONER)
				val = cap_value(val,MIN_DORAM_HAIR_COLOR,MAX_DORAM_HAIR_COLOR);
			else
				val = cap_value(val,MIN_HAIR_COLOR,MAX_HAIR_COLOR);
			if (sd->status.hair_color != val) {
				sd->status.hair_color = val;
				if (sd->status.guild_id) //Update Guild Window. [Skotlex]
					intif_guild_change_memberinfo(sd->status.guild_id,sd->status.account_id,sd->status.char_id,
					GMI_HAIR_COLOR,&sd->status.hair_color,sizeof(sd->status.hair_color));
			}
			break;
		case LOOK_CLOTHES_COLOR: //Use the battle_config limits! [Skotlex]
			if ((sd->class_&MAPID_BASEMASK) == MAPID_SUMMONER)
				val = cap_value(val,MIN_DORAM_CLOTH_COLOR,MAX_DORAM_CLOTH_COLOR);
			else
				val = cap_value(val,MIN_CLOTH_COLOR,MAX_CLOTH_COLOR);
			sd->status.clothes_color = val;
			break;
		case LOOK_SHIELD:
			sd->status.shield = val;
			break;
		case LOOK_SHOES:
			break;
		case LOOK_ROBE:
			sd->status.robe = val;
			sd->setlook_robe = val;
			break;
		case LOOK_BODY2:
			val = cap_value(val,MIN_BODY_STYLE,MAX_BODY_STYLE);
			sd->status.body = val;
			break;
	}
	clif_changelook(&sd->bl,type,val);
}

/*==========================================
 * Give an option (type) to player (sd) and display it to client
 *------------------------------------------*/
void pc_setoption(struct map_session_data *sd, int type)
{
	int p_type, new_look = 0;

	nullpo_retv(sd);

	p_type = sd->sc.option;

	//Option has to be changed client-side before the class sprite or it won't always work (eg: Wedding sprite) [Skotlex]
	sd->sc.option = type;
	clif_changeoption(&sd->bl);

	if ((type&OPTION_RIDING && !(p_type&OPTION_RIDING)) ||
		(type&OPTION_DRAGON && !(p_type&OPTION_DRAGON) && pc_checkskill(sd,RK_DRAGONTRAINING) > 0))
	{ //Mounting
		clif_status_load(&sd->bl,SI_RIDING,1);
		status_calc_pc(sd,SCO_NONE);
	} else if ((!(type&OPTION_RIDING) && p_type&OPTION_RIDING) || (!(type&OPTION_DRAGON) && p_type&OPTION_DRAGON)) { //Dismount
		clif_status_load(&sd->bl,SI_RIDING,0);
		status_calc_pc(sd,SCO_NONE);
	}

#ifndef NEW_CARTS
	if (type&OPTION_CART && !(p_type&OPTION_CART)) { //Cart On
		clif_cartlist(sd);
		clif_updatestatus(sd,SP_CARTINFO);
		if (pc_checkskill(sd,MC_PUSHCART) < 10)
			status_calc_pc(sd,SCO_NONE); //Apply speed penalty
	} else if (!(type&OPTION_CART) && p_type&OPTION_CART) { //Cart Off
		clif_clearcart(sd->fd);
		if (pc_checkskill(sd,MC_PUSHCART) < 10)
			status_calc_pc(sd,SCO_NONE); //Remove speed penalty
		if (sd->equip_index[EQI_AMMO] > 0)
			pc_unequipitem(sd,sd->equip_index[EQI_AMMO],2);
	}
#endif

	if (type&OPTION_FALCON && !(p_type&OPTION_FALCON)) //Falcon ON
		clif_status_load(&sd->bl,SI_FALCON,1);
	else if (!(type&OPTION_FALCON) && p_type&OPTION_FALCON) //Falcon OFF
		clif_status_load(&sd->bl,SI_FALCON,0);

	if (type&OPTION_WUGRIDER && !(p_type&OPTION_WUGRIDER)) { //Mounting
		clif_status_load(&sd->bl,SI_WUGRIDER,1);
		status_calc_pc(sd,SCO_NONE);
	} else if (!(type&OPTION_WUGRIDER) && p_type&OPTION_WUGRIDER) { //Dismount
		clif_status_load(&sd->bl,SI_WUGRIDER,0);
		status_calc_pc(sd,SCO_NONE);
	}

	if (type&OPTION_MADOGEAR && !(p_type&OPTION_MADOGEAR)) { //Madogear ON
		static const sc_type scs[] = { SC_MAXIMIZEPOWER,SC_OVERTHRUST,SC_WEAPONPERFECTION,SC_ADRENALINE,SC_CARTBOOST,SC_MELTDOWN,SC_MAXOVERTHRUST };
		uint8 i;

		status_calc_pc(sd,SCO_NONE);
		for (i = 0; i < ARRAYLENGTH(scs); i++) {
			int skill_id = status_sc2skill(scs[i]);

			if (!(skill_get_inf3(skill_id)&INF3_USABLE_MADO))
				status_change_end(&sd->bl,scs[i],INVALID_TIMER);
		}
		pc_bonus_script_clear(sd,BSF_REM_ON_MADOGEAR);
	} else if (!(type&OPTION_MADOGEAR) && (p_type&OPTION_MADOGEAR)) { //Madogear OFF
		status_calc_pc(sd,SCO_NONE);
		status_change_end(&sd->bl,SC_SHAPESHIFT,INVALID_TIMER);
		status_change_end(&sd->bl,SC_HOVERING,INVALID_TIMER);
		status_change_end(&sd->bl,SC_ACCELERATION,INVALID_TIMER);
		status_change_end(&sd->bl,SC_OVERHEAT,INVALID_TIMER);
		status_change_end(&sd->bl,SC_OVERHEAT_LIMITPOINT,INVALID_TIMER);
		status_change_end(&sd->bl,SC_MAGNETICFIELD,INVALID_TIMER);
		status_change_end(&sd->bl,SC_NEUTRALBARRIER_MASTER,INVALID_TIMER);
		status_change_end(&sd->bl,SC_STEALTHFIELD_MASTER,INVALID_TIMER);
		pc_bonus_script_clear(sd,BSF_REM_ON_MADOGEAR);
		if (sd->equip_index[EQI_AMMO] > 0)
			pc_unequipitem(sd,sd->equip_index[EQI_AMMO],2);
	}

	if (type&OPTION_FLYING && !(p_type&OPTION_FLYING))
		new_look = JOB_STAR_GLADIATOR2;
	else if (!(type&OPTION_FLYING) && p_type&OPTION_FLYING)
		new_look = -1;

	if (sd->disguise || !new_look)
		return; //Disguises break sprite changes

	if (new_look < 0) { //Restore normal look
		status_set_viewdata(&sd->bl, sd->status.class_);
		new_look = sd->vd.class_;
	}

	pc_stop_attack(sd); //Stop attacking on new view change (to prevent wedding/santa attacks
	clif_changelook(&sd->bl,LOOK_BASE,new_look);

	if (sd->vd.cloth_color)
		clif_changelook(&sd->bl,LOOK_CLOTHES_COLOR,sd->vd.cloth_color);
	if (sd->vd.body_style)
		clif_changelook(&sd->bl,LOOK_BODY2,sd->vd.body_style);
	clif_skillinfoblock(sd); //Skill list needs to be updated after base change
}

/**
 * Give player a cart
 * @param sd Player
 * @param type 0:Remove cart, 1 ~ MAX_CARTS: Cart type
 */
bool pc_setcart(struct map_session_data *sd, int type) {
#ifndef NEW_CARTS
	int cart[6] = { 0x0000,OPTION_CART1,OPTION_CART2,OPTION_CART3,OPTION_CART4,OPTION_CART5 };
	int option;
#endif
	nullpo_retr(false,sd);

	if( type < 0 || type > MAX_CARTS )
		return false; //Never trust the values sent by the client! [Skotlex]

	if( type && !pc_checkskill(sd,MC_PUSHCART) )
		return false; //Push cart is required

#ifdef NEW_CARTS
	switch( type ) {
		case 0:
			if( !sd->sc.data[SC_PUSH_CART] )
				return true;
			status_change_end(&sd->bl,SC_CARTBOOST,INVALID_TIMER);
			status_change_end(&sd->bl,SC_GN_CARTBOOST,INVALID_TIMER);
			status_change_end(&sd->bl,SC_PUSH_CART,INVALID_TIMER);
			clif_clearcart(sd->fd);
			clif_updatestatus(sd,SP_CARTINFO);
			if( sd->equip_index[EQI_AMMO] > 0 )
				pc_unequipitem(sd,sd->equip_index[EQI_AMMO],2);
			break;
		default: //Everything else is an allowed ID so we can move on
			if( !sd->sc.data[SC_PUSH_CART] ) { //First time, so fill cart data
				clif_cartlist(sd);
				status_calc_cart_weight(sd,CALCWT_ITEM|CALCWT_MAXBONUS|CALCWT_CARTSTATE);
			}
			clif_updatestatus(sd,SP_CARTINFO);
			sc_start(&sd->bl,&sd->bl,SC_PUSH_CART,100,type,-1);
			break;
	}

	if( pc_checkskill(sd,MC_PUSHCART) < 10 )
		status_calc_pc(sd,SCO_NONE); //Recalc speed penalty
#else
	//Update option
	option = sd->sc.option;
	option &= ~OPTION_CART; //Clear cart bits
	option |= cart[type]; //Set cart
	pc_setoption(sd,option);
#endif

	return true;
}

/*==========================================
 * Give player a falcon
 *------------------------------------------*/
void pc_setfalcon(TBL_PC *sd, int flag)
{
	if( flag ) {
		if( (pc_iswug(sd) || pc_isridingwug(sd)) && !battle_config.warg_can_falcon )
			return;
		if( pc_checkskill(sd,HT_FALCON) > 0 ) //Add falcon if he have the skill
			pc_setoption(sd,sd->sc.option|OPTION_FALCON);
	} else if( pc_isfalcon(sd) )
		pc_setoption(sd,sd->sc.option&~OPTION_FALCON); //Remove falcon
}

/*==========================================
 *  Set player riding
 *------------------------------------------*/
void pc_setriding(struct map_session_data *sd, int flag)
{
	if( sd->sc.data[SC_ALL_RIDING] )
		return;
	if( flag ) {
		if( pc_checkskill(sd,KN_RIDING) > 0 ) //Add peco
			pc_setoption(sd,sd->sc.option|OPTION_RIDING);
	} else if( pc_isriding(sd) )
		pc_setoption(sd,sd->sc.option&~OPTION_RIDING);
}

/**
 * Gives player a mado
 * @param flag 1 Set mado
 */
void pc_setmadogear(struct map_session_data *sd, int flag)
{
	if( flag ) {
		if( pc_checkskill(sd,NC_MADOLICENCE) > 0 )
			pc_setoption(sd,sd->sc.option|OPTION_MADOGEAR);
	} else if( pc_ismadogear(sd) )
		pc_setoption(sd,sd->sc.option&~OPTION_MADOGEAR);
}

/**
 * Determines whether a player can attack based on status changes
 *  Why not use status_check_skilluse?
 *  "src MAY be null to indicate we shouldn't check it, this is a ground-based skill attack."
 *  Even ground-based attacks should be blocked by these statuses
 * Called from unit_attack and unit_attack_timer_sub
 * @return true Can attack
 */
bool pc_can_attack(struct map_session_data *sd, int target_id)
{
	nullpo_retr(false, sd);

	if( pc_is90overweight(sd) || pc_isridingwug(sd) )
		return false;

	if( sd->sc.count &&
		(sd->sc.data[SC_BASILICA] ||
		(sd->sc.data[SC_GRAVITATION] && sd->sc.data[SC_GRAVITATION]->val3 == BCT_SELF) ||
		sd->sc.data[SC__SHADOWFORM] ||
		sd->sc.data[SC_FALLENEMPIRE] ||
		sd->sc.data[SC_CURSEDCIRCLE_ATKER] ||
		sd->sc.data[SC_CURSEDCIRCLE_TARGET] ||
		sd->sc.data[SC_CRYSTALIZE] ||
		(sd->sc.data[SC_VOICEOFSIREN] && sd->sc.data[SC_VOICEOFSIREN]->val2 == target_id) ||
		sd->sc.data[SC_ALL_RIDING] || //The client doesn't let you, this is to make cheat-safe
		sd->sc.data[SC_KINGS_GRACE]) )
		return false;

	return true;
}

/*==========================================
 * Check if player can drop an item
 *------------------------------------------*/
bool pc_candrop(struct map_session_data *sd, struct item *item)
{
	if( item && (itemdb_ishatched_egg(item) || item->expire_time || (item->bound && !pc_can_give_bounded_items(sd))) )
		return false;
	if( !pc_can_give_items(sd) || sd->sc.cant.drop ) //Check if this GM level can drop items
		return false;
	return (itemdb_isdropable(item, pc_get_group_level(sd)));
}

/*==========================================
 * Read ram register for player sd
 * get val (int) from reg for player sd
 *------------------------------------------*/
int pc_readreg(struct map_session_data *sd, int reg)
{
	int i;

	nullpo_ret(sd);

	ARR_FIND(0, sd->reg_num, i, sd->reg[i].index == reg);
	return (i < sd->reg_num) ? sd->reg[i].data : 0;
}

/*==========================================
 * Set ram register for player sd
 * memo val(int) at reg for player sd
 *------------------------------------------*/
bool pc_setreg(struct map_session_data *sd, int reg, int val)
{
	int i;

	nullpo_retr(false,sd);

	ARR_FIND(0, sd->reg_num, i, sd->reg[i].index == reg);
	if( i < sd->reg_num ) { //Overwrite existing entry
		sd->reg[i].data = val;
		return true;
	}

	ARR_FIND(0, sd->reg_num, i, sd->reg[i].data == 0);
	if( i == sd->reg_num ) { //Nothing free, increase size
		sd->reg_num++;
		RECREATE(sd->reg, struct script_reg, sd->reg_num);
	}
	sd->reg[i].index = reg;
	sd->reg[i].data = val;

	return true;
}

/*==========================================
 * Read ram register for player sd
 * get val (str) from reg for player sd
 *------------------------------------------*/
char *pc_readregstr(struct map_session_data *sd, int reg)
{
	int i;

	nullpo_ret(sd);

	ARR_FIND(0, sd->regstr_num, i,  sd->regstr[i].index == reg);
	return (i < sd->regstr_num) ? sd->regstr[i].data : NULL;
}

/*==========================================
 * Set ram register for player sd
 * memo val(str) at reg for player sd
 *------------------------------------------*/
bool pc_setregstr(struct map_session_data *sd, int reg, const char *str)
{
	int i;

	nullpo_retr(false,sd);

	ARR_FIND(0, sd->regstr_num, i, sd->regstr[i].index == reg);
	if( i < sd->regstr_num ) { //Found entry, update
		if( !str || *str == '\0' ) { //Empty string
			if( sd->regstr[i].data )
				aFree(sd->regstr[i].data);
			sd->regstr[i].data = NULL;
		} else if( sd->regstr[i].data ) { //Recreate
			size_t len = strlen(str) + 1;

			RECREATE(sd->regstr[i].data, char, len);
			memcpy(sd->regstr[i].data, str, len * sizeof(char));
		} else //Create
			sd->regstr[i].data = aStrdup(str);
		return true;
	}

	if( !str || *str == '\0' )
		return true; //Nothing to add, empty string

	ARR_FIND(0, sd->regstr_num, i, sd->regstr[i].data == NULL);
	if( i == sd->regstr_num ) { //Nothing free, increase size
		sd->regstr_num++;
		RECREATE(sd->regstr, struct script_regstr, sd->regstr_num);
	}
	sd->regstr[i].index = reg;
	sd->regstr[i].data = aStrdup(str);

	return true;
}

int pc_readregistry(struct map_session_data *sd, const char *reg, int type)
{
	struct global_reg *sd_reg;
	int i, max;

	nullpo_ret(sd);
	switch( type ) {
		case 3: //Char reg
			sd_reg = sd->save_reg.global;
			max = sd->save_reg.global_num;
			break;
		case 2: //Account reg
			sd_reg = sd->save_reg.account;
			max = sd->save_reg.account_num;
			break;
		case 1: //Account2 reg
			sd_reg = sd->save_reg.account2;
			max = sd->save_reg.account2_num;
			break;
		default:
			return 0;
	}
	if( max == -1 ) {
		ShowError("pc_readregistry: Trying to read reg value %s (type %d) before it's been loaded!\n", reg, type);
		//This really shouldn't happen, so it's possible the data was lost somewhere, we should request it again.
		intif_request_registry(sd, (type == 3 ? 4 : type));
		return 0;
	}

	ARR_FIND(0, max, i, strcmp(sd_reg[i].str,reg) == 0);
	return (i < max) ? atoi(sd_reg[i].value) : 0;
}

char *pc_readregistry_str(struct map_session_data *sd, const char *reg, int type)
{
	struct global_reg *sd_reg;
	int i, max;

	nullpo_ret(sd);
	switch( type ) {
		case 3: //Char reg
			sd_reg = sd->save_reg.global;
			max = sd->save_reg.global_num;
			break;
		case 2: //Account reg
			sd_reg = sd->save_reg.account;
			max = sd->save_reg.account_num;
			break;
		case 1: //Account2 reg
			sd_reg = sd->save_reg.account2;
			max = sd->save_reg.account2_num;
			break;
		default:
			return NULL;
	}
	if( max == -1 ) {
		ShowError("pc_readregistry: Trying to read reg value %s (type %d) before it's been loaded!\n", reg, type);
		//This really shouldn't happen, so it's possible the data was lost somewhere, we should request it again.
		intif_request_registry(sd, (type == 3 ? 4 : type));
		return NULL;
	}

	ARR_FIND(0, max, i, strcmp(sd_reg[i].str,reg) == 0);
	return (i < max) ? sd_reg[i].value : NULL;
}

bool pc_setregistry(struct map_session_data *sd, const char *reg, int val, int type)
{
	struct global_reg *sd_reg;
	int i, *max, regmax;

	nullpo_retr(false,sd);

	switch( type ) {
		case 3: //Char reg
			sd_reg = sd->save_reg.global;
			max = &sd->save_reg.global_num;
			regmax = GLOBAL_REG_NUM;
			break;
		case 2: //Account reg
			sd_reg = sd->save_reg.account;
			max = &sd->save_reg.account_num;
			regmax = ACCOUNT_REG_NUM;
			break;
		case 1: //Account2 reg
			sd_reg = sd->save_reg.account2;
			max = &sd->save_reg.account2_num;
			regmax = ACCOUNT_REG2_NUM;
			break;
		default:
			return false;
	}

	if( *max == -1 ) {
		ShowError("pc_setregistry : refusing to set %s (type %d) until vars are received.\n", reg, type);
		return true;
	}

	//Delete reg
	if( !val ) {
		ARR_FIND(0, *max, i, strcmp(sd_reg[i].str,reg) == 0);
		if( i < *max ) {
			if( i != *max - 1 )
				memcpy(&sd_reg[i], &sd_reg[*max - 1], sizeof(struct global_reg));
			memset(&sd_reg[*max - 1], 0, sizeof(struct global_reg));
			(*max)--;
			sd->state.reg_dirty |= 1<<(type - 1); //Mark this registry as "need to be saved"
		}
		return true;
	}
	//Change value if found
	ARR_FIND(0, *max, i, strcmp(sd_reg[i].str,reg) == 0);
	if( i < *max ) {
		safesnprintf(sd_reg[i].value, sizeof(sd_reg[i].value), "%d", val);
		sd->state.reg_dirty |= 1<<(type - 1);
		return true;
	}

	//Add value if not found
	if( i < regmax ) {
		memset(&sd_reg[i], 0, sizeof(struct global_reg));
		safestrncpy(sd_reg[i].str, reg, sizeof(sd_reg[i].str));
		safesnprintf(sd_reg[i].value, sizeof(sd_reg[i].value), "%d", val);
		(*max)++;
		sd->state.reg_dirty |= 1<<(type - 1);
		return true;
	}

	ShowError("pc_setregistry : couldn't set %s, limit of registries reached (%d)\n", reg, regmax);
	return false;
}

bool pc_setregistry_str(struct map_session_data *sd, const char *reg, const char *val, int type)
{
	struct global_reg *sd_reg;
	int i, *max, regmax;

	nullpo_retr(false,sd);

	if (reg[strlen(reg) - 1] != '$') {
		ShowError("pc_setregistry_str : reg %s must be string (end in '$') to use this!\n", reg);
		return false;
	}

	switch (type) {
		case 3: //Char reg
			sd_reg = sd->save_reg.global;
			max = &sd->save_reg.global_num;
			regmax = GLOBAL_REG_NUM;
			break;
		case 2: //Account reg
			sd_reg = sd->save_reg.account;
			max = &sd->save_reg.account_num;
			regmax = ACCOUNT_REG_NUM;
			break;
		case 1: //Account2 reg
			sd_reg = sd->save_reg.account2;
			max = &sd->save_reg.account2_num;
			regmax = ACCOUNT_REG2_NUM;
			break;
		default:
			return false;
	}
	if (*max == -1) {
		ShowError("pc_setregistry_str : refusing to set %s (type %d) until vars are received.\n", reg, type);
		return false;
	}

	//Delete reg
	if (!val || strcmp(val,"") == 0) {
		ARR_FIND(0, *max, i, strcmp(sd_reg[i].str, reg) == 0);
		if (i < *max) {
			if (i != *max - 1)
				memcpy(&sd_reg[i], &sd_reg[*max - 1], sizeof(struct global_reg));
			memset(&sd_reg[*max - 1], 0, sizeof(struct global_reg));
			(*max)--;
			sd->state.reg_dirty |= 1<<(type - 1); //Mark this registry as "need to be saved"
			if (type != 3) intif_saveregistry(sd, type);
		}
		return true;
	}

	//Change value if found
	ARR_FIND(0, *max, i, strcmp(sd_reg[i].str, reg) == 0);
	if (i < *max) {
		safestrncpy(sd_reg[i].value, val, sizeof(sd_reg[i].value));
		sd->state.reg_dirty |= 1<<(type - 1); //Mark this registry as "need to be saved"
		if (type != 3) intif_saveregistry(sd, type);
		return true;
	}

	//Add value if not found
	if (i < regmax) {
		memset(&sd_reg[i], 0, sizeof(struct global_reg));
		safestrncpy(sd_reg[i].str, reg, sizeof(sd_reg[i].str));
		safestrncpy(sd_reg[i].value, val, sizeof(sd_reg[i].value));
		(*max)++;
		sd->state.reg_dirty |= 1<<(type - 1); //Mark this registry as "need to be saved"
		if (type != 3) intif_saveregistry(sd, type);
		return true;
	}

	ShowError("pc_setregistry : couldn't set %s, limit of registries reached (%d)\n", reg, regmax);
	return false;
}

/**
 * Set value of player variable
 * @param sd Player
 * @param reg Variable name
 * @param value
 * @return True if success, false if failed.
 */
bool pc_setreg2(struct map_session_data *sd, const char *reg, int val) {
	char prefix = reg[0];

	nullpo_retr(false, sd);

	if (reg[strlen(reg) - 1] == '$') {
		ShowError("pc_setreg2: Invalid variable scope '%s'\n", reg);
		return false;
	}

	val = cap_value(val, INT_MIN, INT_MAX);

	switch (prefix) {
		case '.':
		case '\'':
		case '$':
			ShowError("pc_setreg2: Invalid variable scope '%s'\n", reg);
			return false;
		case '@':
			return pc_setreg(sd, add_str(reg), val);
		case '#':
			return (reg[1] == '#') ? pc_setaccountreg2(sd, reg, val) : pc_setaccountreg(sd, reg, val);
		default:
			return pc_setglobalreg(sd, reg, val);
	}

	return false;
}

/**
 * Get value of player variable
 * @param sd Player
 * @param reg Variable name
 * @return Variable value or 0 if failed.
 */
int pc_readreg2(struct map_session_data *sd, const char *reg) {
	char prefix = reg[0];

	nullpo_ret(sd);

	if (reg[strlen(reg) - 1] == '$') {
		ShowError("pc_readreg2: Invalid variable scope '%s'\n", reg);
		return 0;
	}

	switch (prefix) {
		case '.':
		case '\'':
		case '$':
			ShowError("pc_readreg2: Invalid variable scope '%s'\n", reg);
			return 0;
		case '@':
			return pc_readreg(sd, add_str(reg));
		case '#':
			return (reg[1] == '#') ? pc_readaccountreg2(sd, reg) : pc_readaccountreg(sd, reg);
		default:
			return pc_readglobalreg(sd, reg);
	}

	return 0;
}

/*==========================================
 * Exec eventtimer for player sd (retrieved from map_session (id))
 *------------------------------------------*/
static TIMER_FUNC(pc_eventtimer)
{
	struct map_session_data *sd = map_id2sd(id);
	char *p = (char *)data;
	int i;

	if (!sd)
		return 0;

	ARR_FIND(0, MAX_EVENTTIMER, i, sd->eventtimer[i] == tid);
	if (i < MAX_EVENTTIMER) {
		sd->eventtimer[i] = INVALID_TIMER;
		sd->eventcount--;
		npc_event(sd,p,0);
	} else
		ShowError("pc_eventtimer: no such event timer\n");

	if (p)
		aFree(p);
	return 0;
}

/*==========================================
 * Add eventtimer for player sd ?
 *------------------------------------------*/
bool pc_addeventtimer(struct map_session_data *sd,int tick,const char *name)
{
	int i;

	nullpo_retr(false,sd);

	ARR_FIND(0, MAX_EVENTTIMER, i, sd->eventtimer[i] == INVALID_TIMER);
	if(i == MAX_EVENTTIMER)
		return false;

	sd->eventtimer[i] = add_timer(gettick() + tick, pc_eventtimer, sd->bl.id, (intptr_t)aStrdup(name));
	sd->eventcount++;

	return true;
}

/*==========================================
 * Del eventtimer for player sd ?
 *------------------------------------------*/
bool pc_deleventtimer(struct map_session_data *sd,const char *name)
{
	char *p = NULL;
	int i;

	nullpo_retr(false,sd);

	if (sd->eventcount == 0)
		return false;

	//Find the named event timer
	ARR_FIND(0, MAX_EVENTTIMER, i,
		sd->eventtimer[i] != INVALID_TIMER &&
		(p = (char *)(get_timer(sd->eventtimer[i])->data)) != NULL &&
		strcmp(p,name) == 0
	);
	if (i == MAX_EVENTTIMER)
		return false; //Not found

	delete_timer(sd->eventtimer[i],pc_eventtimer);
	sd->eventtimer[i] = INVALID_TIMER;
	if (sd->eventcount > 0)
		sd->eventcount--;
	aFree(p);

	return true;
}

/*==========================================
 * Update eventtimer count for player sd
 *------------------------------------------*/
void pc_addeventtimercount(struct map_session_data *sd,const char *name,int tick)
{
	int i;

	nullpo_retv(sd);

	for (i = 0; i < MAX_EVENTTIMER; i++) {
		if (sd->eventtimer[i] != INVALID_TIMER && strcmp((char *)(get_timer(sd->eventtimer[i])->data),name) == 0) {
			addtick_timer(sd->eventtimer[i],tick);
			break;
		}
	}
}

/*==========================================
 * Remove all eventtimer for player sd
 *------------------------------------------*/
void pc_cleareventtimer(struct map_session_data *sd)
{
	int i;

	nullpo_retv(sd);

	if (sd->eventcount == 0)
		return;
	for (i = 0; i < MAX_EVENTTIMER; i++)
		if (sd->eventtimer[i] != INVALID_TIMER) {
			char *p = (char *)(get_timer(sd->eventtimer[i])->data);

			delete_timer(sd->eventtimer[i],pc_eventtimer);
			sd->eventtimer[i] = INVALID_TIMER;
			if (sd->eventcount > 0) //Avoid looping to max val
				sd->eventcount--;
			if (p)
				aFree(p);
		}
}

/**
 * Called when an item with combo is worn
 * @param *sd
 * @param *data struct item_data
 * @return success numbers of succeed combo
 */
static int pc_checkcombo(struct map_session_data *sd, struct item_data *data) {
	uint16 i;
	int success = 0;

	for( i = 0; i < data->combos_count; i++ ) {
		struct itemchk {
			int idx;
			unsigned short nameid;
			short card[MAX_SLOTS];
		} *combo_idx;
		int idx, j;
		int nb_itemCombo;
		unsigned int pos = 0;

		//Ensure this isn't a duplicate combo
		if( sd->combos.bonus ) {
			int x;

			ARR_FIND(0, sd->combos.count, x, sd->combos.id[x] == data->combos[i]->id);
			//Found a match, skip this combo
			if( x < sd->combos.count )
				continue;
		}

		nb_itemCombo = data->combos[i]->count;
		if( nb_itemCombo < 2 ) //A combo with less then 2 item? How that possible?
			continue;
		CREATE(combo_idx, struct itemchk, nb_itemCombo);
		for( j = 0; j < nb_itemCombo; j++ ) {
			combo_idx[j].idx = -1;
			combo_idx[j].nameid = -1;
			memset(combo_idx[j].card, -1, MAX_SLOTS);
		}

		for( j = 0; j < nb_itemCombo; j++ ) {
			uint16 id = data->combos[i]->nameid[j], k;
			bool found = false;

			for( k = 0; k < EQI_MAX; k++ ) {
				short index = sd->equip_index[k];

				if( index < 0 )
					continue;
				if( pc_is_same_equip_index((enum equip_index)k,sd->equip_index,index) )
					continue;
				if( !sd->inventory_data[index] )
					continue;
				if( itemdb_type(id) != IT_CARD ) {
					if( sd->inventory_data[index]->nameid != id )
						continue;
					if( j > 0 ) { //Check if this item not already used
						bool do_continue = false; //Used to continue that specific loop with some check that also use some loop
						uint8 z;

						for( z = 0; z < nb_itemCombo - 1; z++ ) {
							if( combo_idx[z].idx == index && combo_idx[z].nameid == id )
								do_continue = true; //We already have that index recorded
						}
						if( do_continue )
							continue;
					}
					combo_idx[j].idx = index;
					combo_idx[j].nameid = id;
					pos |= sd->inventory.u.items_inventory[index].equip;
					found = true;
					break;
				} else { //Cards and enchants
					uint16 z;

					if( itemdb_isspecial(sd->inventory.u.items_inventory[index].card[0]) )
						continue;
					for( z = 0; z < MAX_SLOTS; z++ ) {
						bool do_continue = false;

						if( sd->inventory.u.items_inventory[index].card[z] != id )
							continue;
						if( j > 0 ) {
							int c1, c2;

							for( c1 = 0; c1 < nb_itemCombo - 1; c1++ ) {
								if( combo_idx[c1].idx == index && combo_idx[c1].nameid == id ) {
									for( c2 = 0; c2 < MAX_SLOTS; c2++ ) {
										if( combo_idx[c1].card[c2] == id ) {
											do_continue = true; //We already have that card recorded (at this same idx)
											break;
										}
									}
								}
							}
						}
						if( do_continue )
							continue;
						combo_idx[j].idx = index;
						combo_idx[j].nameid = id;
						combo_idx[j].card[z] = id;
						pos |= sd->inventory.u.items_inventory[index].equip;
						found = true;
						break;
					}
				}
			}
			if( !found )
				break; //We haven't found all the ids for this combo, so we can return
		}
		aFree(combo_idx);
		//Means we broke out of the count loop w/o finding all ids, we can move to the next combo
		if( j < nb_itemCombo )
			continue;
		//We got here, means all items in the combo are matching
		idx = sd->combos.count;
		if( sd->combos.bonus == NULL ) {
			CREATE(sd->combos.bonus, struct script_code *, 1);
			CREATE(sd->combos.id, unsigned short, 1);
			CREATE(sd->combos.pos, unsigned int, 1);
			sd->combos.count = 1;
		} else {
			RECREATE(sd->combos.bonus, struct script_code *, ++sd->combos.count);
			RECREATE(sd->combos.id, unsigned short, sd->combos.count);
			RECREATE(sd->combos.pos, unsigned int, sd->combos.count);
		}
		//We simply copy the pointer
		sd->combos.bonus[idx] = data->combos[i]->script;
		//Save this combo's id
		sd->combos.id[idx] = data->combos[i]->id;
		//Save pos of combo
		sd->combos.pos[idx] = pos;
		success++;
	}
	return success;
}

/**
 * Called when an item with combo is removed
 * @param *sd
 * @param *data struct item_data
 * @return retval numbers of removed combo
 */
static int pc_removecombo(struct map_session_data *sd, struct item_data *data) {
	int i, retval = 0;

	if( sd->combos.bonus == NULL )
		return 0; //Nothing to do here, player has no combos
	for( i = 0; i < data->combos_count; i++ ) {
		//Check if this combo exists in this user
		int x = 0, cursor = 0, j;

		ARR_FIND(0, sd->combos.count, x, sd->combos.id[x] == data->combos[i]->id);
		//No match, skip this combo
		if( !(x < sd->combos.count) )
			continue;
		sd->combos.bonus[x] = NULL;
		sd->combos.id[x] = 0;
		sd->combos.pos[x] = 0;
		retval++;
		//Check if combo requirements still fit
		if( pc_checkcombo(sd, data) )
			continue;
		//Move next value to empty slot
		for( j = 0, cursor = 0; j < sd->combos.count; j++ ) {
			if( sd->combos.bonus[j] == NULL )
				continue;
			if( cursor != j ) {
				sd->combos.bonus[cursor] = sd->combos.bonus[j];
				sd->combos.id[cursor] = sd->combos.id[j];
				sd->combos.pos[cursor] = sd->combos.pos[j];
			}
			cursor++;
		}
		//It's empty, we can clear all the memory
		if( (sd->combos.count = cursor) == 0 ) {
			aFree(sd->combos.bonus);
			aFree(sd->combos.id);
			aFree(sd->combos.pos);
			sd->combos.bonus = NULL;
			sd->combos.id = NULL;
			sd->combos.pos = NULL;
			return retval; //We also can return at this point for we have no more combos to check
		}
	}
	return retval;
}

/**
 * Load combo data(s) of player
 * @param *sd
 * @return ret numbers of succeed combo
 */
int pc_load_combo(struct map_session_data *sd) {
	int i, ret = 0;

	for( i = 0; i < EQI_MAX; i++ ) {
		struct item_data *id = NULL;
		short idx = sd->equip_index[i];

		if( idx < 0 || !(id = sd->inventory_data[idx]) )
			continue;
		if( id->combos_count )
			ret += pc_checkcombo(sd,id);
		if( !itemdb_isspecial(sd->inventory.u.items_inventory[idx].card[0]) ) {
			struct item_data *data = NULL;
			int j;

			for( j = 0; j < MAX_SLOTS; j++ ) {
				if( !sd->inventory.u.items_inventory[idx].card[j] )
					continue;
				if( (data = itemdb_exists(sd->inventory.u.items_inventory[idx].card[j])) && data->combos_count )
					ret += pc_checkcombo(sd,data);
			}
		}
	}
	return ret;
}

/*==========================================
 * Equip item on player sd at req_pos from inventory index n
 * return: false - fail; true - success
 *------------------------------------------*/
bool pc_equipitem(struct map_session_data *sd, short n, int req_pos, bool equipswitch)
{
	int i = 0, pos = 0, flag = 0, iflag = 0;
	struct item_data *id;
	uint8 res = ITEM_EQUIP_ACK_OK;
	bool status_calc = false;
	short *equip_index;

	nullpo_retr(false,sd);

	if( n < 0 || n >= MAX_INVENTORY ) {
		if( equipswitch )
			clif_equipswitch_add(sd,n,req_pos,true);
		else
			clif_equipitemack(sd,0,0,ITEM_EQUIP_ACK_FAIL);
		return false;
	}
	if( DIFF_TICK(sd->canequip_tick,gettick()) > 0 ) {
		if( equipswitch )
			clif_equipswitch_add(sd,n,req_pos,true);
		else
			clif_equipitemack(sd,n,0,ITEM_EQUIP_ACK_FAIL);
		return false;
	}
	if( !(id = sd->inventory_data[n]) )
		return false;
	pos = pc_equippoint(sd,n); //With a few exceptions, item should go in all specified slots
	if( battle_config.battle_log && !equipswitch )
		ShowInfo("equip %hu(%d) %x:%x\n",sd->inventory.u.items_inventory[n].nameid,n,id->equip,req_pos);
	if( (res = pc_isequip(sd,n)) ) {
		if( equipswitch )
			clif_equipswitch_add(sd,n,req_pos,true);
		else
			clif_equipitemack(sd,n,0,res);
		return false;
	}
	if( equipswitch && id->type == IT_AMMO ) {
		clif_equipswitch_add(sd,n,req_pos,true);
		return false;
	}
	if( !(pos&req_pos) || sd->inventory.u.items_inventory[n].equip || sd->inventory.u.items_inventory[n].attribute ) { //[Valaris]
		if( equipswitch )
			clif_equipswitch_add(sd,n,req_pos,true);
		else
			clif_equipitemack(sd,n,0,ITEM_EQUIP_ACK_FAIL);
		return false;
	}
	if( sd->sc.count &&
		(sd->sc.data[SC_BERSERK] ||
		sd->sc.data[SC_SATURDAYNIGHTFEVER] ||
		sd->sc.data[SC_KYOUGAKU] ||
		(sd->sc.data[SC_PYROCLASTIC] && sd->inventory_data[n]->type == IT_WEAPON)) )
	{
		if( equipswitch )
			clif_equipswitch_add(sd,n,req_pos,true);
		else
			clif_equipitemack(sd,n,0,ITEM_EQUIP_ACK_FAIL);
		return false;
	}
	equip_index = (equipswitch ? sd->equip_switch_index : sd->equip_index);
	if( !equipswitch && id->flag.bindOnEquip && !sd->inventory.u.items_inventory[n].bound ) {
		sd->inventory.u.items_inventory[n].bound = (char)battle_config.default_bind_on_equip;
		clif_notify_bindOnEquip(sd,n);
	}
	if( pos == EQP_ACC ) { //Accessories should only go in one of the two
		pos = (req_pos&EQP_ACC);
		if( pos == EQP_ACC ) //User specified both slots
			pos = (equip_index[EQI_ACC_R] >= 0 ? EQP_ACC_L : EQP_ACC_R);
		//Accessories that have cards that force equip location
		for( i = 0; i < sd->inventory_data[n]->slot; i++ ) {
			struct item_data *card_data = NULL;

			if( !sd->inventory.u.items_inventory[n].card[i] )
				continue;
			if( (card_data = itemdb_exists(sd->inventory.u.items_inventory[n].card[i])) ) {
				int card_pos = card_data->equip;

				if( card_pos == EQP_ACC_L || card_pos == EQP_ACC_R ) {
					pos = card_pos; //Use the card's equip position
					break;
				}
			}
		}
	}
	if( pos == EQP_ARMS && id->equip == EQP_HAND_R ) { //Dual wield capable weapon
		pos = (req_pos&EQP_ARMS);
		if( pos == EQP_ARMS ) //User specified both slots, pick one for them
			pos = (equip_index[EQI_HAND_R] >= 0 ? EQP_HAND_L : EQP_HAND_R);
	}
	//Shadow System
	if( pos == EQP_SHADOW_ACC ) {
		pos = (req_pos&EQP_SHADOW_ACC);
		if( pos == EQP_SHADOW_ACC )
			pos = (equip_index[EQI_SHADOW_ACC_R] >= 0 ? EQP_SHADOW_ACC_L : EQP_SHADOW_ACC_R);
	}
	if( pos == EQP_SHADOW_ARMS && id->equip == EQP_SHADOW_WEAPON) {
		pos = (req_pos&EQP_SHADOW_ARMS);
		if( pos == EQP_SHADOW_ARMS )
			pos = (equip_index[EQI_SHADOW_WEAPON] >= 0 ? EQP_SHADOW_SHIELD : EQP_SHADOW_WEAPON);
	}
	//Update skill-block range database when weapon range changes [Skotlex]
	if( (pos&EQP_HAND_R) && (battle_config.use_weapon_skill_range&BL_PC) ) {
		short idx = equip_index[EQI_HAND_R];

		if( idx < 0 || !sd->inventory_data[idx] ) //No data, or no weapon equipped
			flag = 1;
		else
			flag = (id->range != sd->inventory_data[idx]->range);
	}
	if( equipswitch ) {
		for( i = 0; i < EQI_MAX; i++ ) {
			if( pos&equip_bitmask[i] ) {
				if( sd->equip_switch_index[i] >= 0 ) //If there was already an item assigned to this slot
					pc_equipswitch_remove(sd,sd->equip_switch_index[i]);
				sd->equip_switch_index[i] = n; //Assign the new index to it
			}
		}
		sd->inventory.u.items_inventory[n].equipSwitch = pos;
		clif_equipswitch_add(sd,n,pos,false);
		return true;
	} else {
		for( i = 0; i < EQI_MAX; i++ ) {
			if( pos&equip_bitmask[i] ) {
				if( sd->equip_index[i] >= 0 ) //Slot taken, remove item from there
					pc_unequipitem(sd,sd->equip_index[i],2|8);
				sd->equip_index[i] = n;
			}
		}
		pc_equipswitch_remove(sd,n);
		if( pos == EQP_AMMO ) {
			clif_arrowequip(sd,n);
			clif_arrow_fail(sd,3);
		} else
			clif_equipitemack(sd,n,pos,ITEM_EQUIP_ACK_OK);
		sd->inventory.u.items_inventory[n].equip = pos;
	}
	if( pos&EQP_HAND_R ) {
		sd->weapontype1 = id->look;
		pc_calcweapontype(sd);
		clif_changelook(&sd->bl,LOOK_WEAPON,sd->status.weapon);
	}
	if( pos&EQP_HAND_L ) {
		if( id->type == IT_WEAPON ) {
			sd->status.shield = 0;
			sd->weapontype2 = id->look;
		} else if( id->type == IT_ARMOR ) {
			sd->status.shield = id->look;
			sd->weapontype2 = 0;
		}
		pc_calcweapontype(sd);
		clif_changelook(&sd->bl,LOOK_SHIELD,sd->status.shield);
	}
	if( battle_config.ammo_unequip && (pos&EQP_ARMS) && id->type == IT_WEAPON ) {
		short idx = sd->equip_index[EQI_AMMO];

		if( idx >= 0 ) {
			switch( sd->inventory_data[idx]->look ) {
				case AMMO_ARROW:
					if( id->look != W_BOW && id->look != W_MUSICAL && id->look != W_WHIP )
						pc_unequipitem(sd,idx,2|8);
					break;
				case AMMO_BULLET:
				case AMMO_SHELL:
					if( id->look != W_REVOLVER && id->look != W_RIFLE && id->look != W_GATLING && id->look != W_SHOTGUN
#ifdef RENEWAL
						&& id->look != W_GRENADE
#endif
						)
						pc_unequipitem(sd,idx,2|8);
					break;
#ifndef RENEWAL
				case AMMO_GRENADE:
					if( id->look != W_GRENADE )
						pc_unequipitem(sd,idx,2|8);
					break;
#endif
			}
		}
	}
	if( pos&EQP_SHOES )
		clif_changelook(&sd->bl,LOOK_SHOES,0);
	pc_set_costume_view(sd);
	pc_checkallowskill(sd); //Check if status changes should be halted
	iflag = sd->npc_item_flag;
	if( id->combos_count && pc_checkcombo(sd,id) ) //Check for combos
		status_calc = true;
	if( itemdb_isspecial(sd->inventory.u.items_inventory[n].card[0]) )
		; //No cards
	else {
		for( i = 0; i < MAX_SLOTS; i++ ) {
			struct item_data *data = NULL;

			if( !sd->inventory.u.items_inventory[n].card[i] )
				continue;
			if( (data = itemdb_exists(sd->inventory.u.items_inventory[n].card[i])) && data->combos_count && pc_checkcombo(sd,data) )
				status_calc = true;
		}
	}
	if( status_calc )
		status_calc_pc(sd,SCO_NONE);
	//OnEquip script [Skotlex]
	if( id->equip_script && (pc_has_permission(sd,PC_PERM_USE_ALL_EQUIPMENT) || !itemdb_isNoEquip(id,sd->bl.m)) )
		run_script(id->equip_script,0,sd->bl.id,fake_nd->bl.id); //Only run the script if item isn't restricted
	if( itemdb_isspecial(sd->inventory.u.items_inventory[n].card[0]) )
		; //No cards
	else {
		for( i = 0; i < MAX_SLOTS; i++ ) {
			struct item_data *data = NULL;

			if( !sd->inventory.u.items_inventory[n].card[i] )
				continue;
			if( (data = itemdb_exists(sd->inventory.u.items_inventory[n].card[i])) && data->equip_script &&
				(pc_has_permission(sd,PC_PERM_USE_ALL_EQUIPMENT) || !itemdb_isNoEquip(data,sd->bl.m)) )
				run_script(data->equip_script,0,sd->bl.id,fake_nd->bl.id);
		}
	}
	status_calc_pc(sd,SCO_FORCE);
	if( flag ) //Update skill data
		clif_skillinfoblock(sd);
	sd->npc_item_flag = iflag;
	return true;
}

static void pc_unequipitem_sub(struct map_session_data *sd, int n, int flag) {
	int i = 0, iflag = 0;
	bool status_calc = false;

	//Check for activated autobonus [Inkfish]
	if( sd->state.autobonus&sd->inventory.u.items_inventory[n].equip )
		sd->state.autobonus &= ~sd->inventory.u.items_inventory[n].equip;
	sd->inventory.u.items_inventory[n].equip = 0;
	pc_checkallowskill(sd);
	iflag = sd->npc_item_flag;
	if( sd->inventory_data[n] ) { //Check for combos
		if( sd->inventory_data[n]->combos_count && pc_removecombo(sd,sd->inventory_data[n]) )
			status_calc = true;
		if( itemdb_isspecial(sd->inventory.u.items_inventory[n].card[0]) )
			; //No cards
		else {
			for( i = 0; i < MAX_SLOTS; i++ ) {
				struct item_data *data = NULL;

				if( !sd->inventory.u.items_inventory[n].card[i] )
					continue;
				if( (data = itemdb_exists(sd->inventory.u.items_inventory[n].card[i])) && data->combos_count && pc_removecombo(sd,data) )
					status_calc = true;
			}
		}
	}
	if( status_calc )
		status_calc_pc(sd,SCO_NONE);
	if( sd->inventory_data[n] ) { //OnUnEquip script [Skotlex]
		if( sd->inventory_data[n]->unequip_script )
			run_script(sd->inventory_data[n]->unequip_script,0,sd->bl.id,fake_nd->bl.id);
		if( itemdb_isspecial(sd->inventory.u.items_inventory[n].card[0]) )
			; //No cards
		else {
			for( i = 0; i < MAX_SLOTS; i++ ) {
				struct item_data *data = NULL;

				if( !sd->inventory.u.items_inventory[n].card[i] )
					continue;
				if( (data = itemdb_exists(sd->inventory.u.items_inventory[n].card[i])) && data->unequip_script )
					run_script(data->unequip_script,0,sd->bl.id,fake_nd->bl.id);
			}
		}
	}
	if( flag&1 )
		status_calc_pc(sd,SCO_FORCE);
	sd->npc_item_flag = iflag;
}

/*==========================================
 * Called when attemting to unequip an item from player
 * Flag:
 *	0 - only unequip
 *	1 - calculate status after unequipping
 *	2 - force unequip
 *	4 - unequip by 'breakequip' or 'skill_break_equip'
 *	8 - unequip by switching equipment
 * return: false - fail; true - success
 *------------------------------------------*/
void pc_unequipitem(struct map_session_data *sd, int n, int flag) {
	int i = 0;
	int pos;

	nullpo_retv(sd);

	if( n < 0 || n >= MAX_INVENTORY ) {
		clif_unequipitemack(sd,0,0,0);
		return;
	}
	if( !(pos = sd->inventory.u.items_inventory[n].equip) ) {
		clif_unequipitemack(sd,n,0,0);
		return; //Nothing to unequip
	}
	if( !(flag&2) && sd->sc.count &&
		(sd->sc.data[SC_BERSERK] ||
		sd->sc.data[SC_SATURDAYNIGHTFEVER] ||
		sd->sc.data[SC_KYOUGAKU] ||
		(sd->sc.data[SC_PYROCLASTIC] && sd->inventory_data[n]->type == IT_WEAPON)) )
	{
		clif_unequipitemack(sd,n,0,0);
		return; //Cannot unequip
	}
	if( battle_config.battle_log )
		ShowInfo("Unequip %d %x:%x\n",n,pc_equippoint(sd,n),pos);
	for( i = 0; i < EQI_MAX; i++ ) {
		if( pos&equip_bitmask[i] )
			sd->equip_index[i] = -1;
	}
	if( pos&EQP_HAND_R ) {
		sd->weapontype1 = 0;
		sd->status.weapon = sd->weapontype2;
		pc_calcweapontype(sd);
		clif_changelook(&sd->bl,LOOK_WEAPON,sd->status.weapon);
	}
	if( pos&EQP_HAND_L ) {
		if( sd->status.shield && battle_getcurrentskill(&sd->bl) == LG_SHIELDSPELL )
			unit_skillcastcancel(&sd->bl,0); //Cancel Shield Spell if player swaps shields
		sd->status.shield = sd->weapontype2 = 0;
		pc_calcweapontype(sd);
		clif_changelook(&sd->bl,LOOK_SHIELD,sd->status.shield);
	}
	if( pos&EQP_SHOES )
		clif_changelook(&sd->bl,LOOK_SHOES,0);
	clif_unequipitemack(sd,n,pos,1);
	pc_set_costume_view(sd);
	//On equipment change
	if( !(flag&4) )
		status_change_end(&sd->bl,SC_CONCENTRATION,INVALID_TIMER);
	//On weapon change (both hands)
	if( (pos&EQP_ARMS) && sd->inventory_data[n]->type == IT_WEAPON ) {
		if( battle_config.ammo_unequip && !(flag&8) ) {
			switch( sd->inventory_data[n]->look ) {
				case W_BOW:
				case W_MUSICAL:
				case W_WHIP:
				case W_REVOLVER:
				case W_RIFLE:
				case W_GATLING:
				case W_SHOTGUN:
				case W_GRENADE:
					{
						short idx = sd->equip_index[EQI_AMMO];

						if( idx >= 0 ) {
							sd->equip_index[EQI_AMMO] = -1;
							clif_unequipitemack(sd,idx,sd->inventory.u.items_inventory[idx].equip,1);
							pc_unequipitem_sub(sd,idx,0);
						}
					}
					break;
			}
		}
		if( !sd->sc.data[SC_SEVENWIND] || sd->sc.data[SC_ASPERSIO] ) //Check for seven wind (but not level seven!)
			skill_enchant_elemental_end(&sd->bl,SC_NONE);
		if( !battle_config.dancing_weaponswitch_fix )
			status_change_end(&sd->bl,SC_DANCING,INVALID_TIMER); //When unequipping, stop dancing [Skotlex]
#ifdef RENEWAL
		if( pos&EQP_HAND_R )
			status_change_end(&sd->bl,SC_EDP,INVALID_TIMER);
#endif
		status_change_end(&sd->bl,SC_FEARBREEZE,INVALID_TIMER);
		status_change_end(&sd->bl,SC_EXEEDBREAK,INVALID_TIMER);
		status_change_end(&sd->bl,SC_STRIKING,INVALID_TIMER);
		status_change_end(&sd->bl,SC_HEAT_BARREL,INVALID_TIMER);
		status_change_end(&sd->bl,SC_P_ALTER,INVALID_TIMER);
	}
	//On armor change
	if( pos&EQP_ARMOR )
		status_change_end(&sd->bl,SC_ARMOR_RESIST,INVALID_TIMER);
	//On ammo change
	if( sd->inventory_data[n]->type == IT_AMMO )
		status_change_end(&sd->bl,SC_P_ALTER,INVALID_TIMER);
	pc_unequipitem_sub(sd,n,flag);
}

int pc_equipswitch(struct map_session_data *sd, int index) {
	int position = sd->inventory.u.items_inventory[index].equipSwitch; //Get the target equip mask
	short equippedItem = pc_checkequip(sd,position,true); //Get the currently equipped item

	if( equippedItem == -1 ) { //No item equipped at the target
		pc_equipswitch_remove(sd,index); //Remove it from the equip switch
		pc_equipitem(sd,index,position,false);
		return position;
	} else {
		int unequipped_index = -1;
		int unequipped_position = 0;
		int i, all_position;

		for( i = 0; i < EQI_MAX; i++ ) { //Unequip all items that interfere
			unequipped_index = sd->equip_index[i];
			if( unequipped_index >= 0 && (position&equip_bitmask[i]) ) {
				struct item *unequipped_item = &sd->inventory.u.items_inventory[unequipped_index];

				//Store the unequipped index and position mask for later
				if( !unequipped )
					CREATE(unequipped,struct s_unequipped,1);
				else
					RECREATE(unequipped,struct s_unequipped,unequipped_count + 1);
				unequipped[unequipped_count].index = unequipped_index;
				unequipped[unequipped_count].position = unequipped_item->equip;
				unequipped_count++;
				//Keep the position for later
				unequipped_position |= unequipped_item->equip;
				//Unequip the item
				pc_unequipitem(sd,unequipped_index,0);
			}
		}
		all_position = position|unequipped_position;
		for( i = 0; i < EQI_MAX; i++ ) { //Equip everything that is hit by the mask
			int exchange_index = sd->equip_switch_index[i];

			if( exchange_index >= 0 && (all_position&equip_bitmask[i]) ) {
				struct item *exchange_item = &sd->inventory.u.items_inventory[exchange_index];
				int exchange_position = exchange_item->equipSwitch; //Store the target position

				pc_equipswitch_remove(sd,exchange_index); //Remove the item from equip switch
				pc_equipitem(sd,exchange_index,exchange_position,false); //Equip the item at the destinated position
			}
		}
		for( i = 0; i < unequipped_count; i++ ) { //Place all unequipped items into the equip switch window
			int j;

			unequipped_index = unequipped[i].index;
			unequipped_position = unequipped[i].position;
			for( j = 0; j < EQI_MAX; j++ ) { //Rebuild the index cache
				if( unequipped_position&equip_bitmask[j] )
					sd->equip_switch_index[j] = unequipped_index;
			}
			sd->inventory.u.items_inventory[unequipped_index].equipSwitch = unequipped_position; //Set the correct position mask
			clif_equipswitch_add(sd,unequipped_index,unequipped_position,false); //Notify the client
		}
		if( unequipped ) {
			aFree(unequipped);
			unequipped = NULL;
			unequipped_count = 0;
		}
		return all_position;
	}
}

void pc_equipswitch_remove(struct map_session_data *sd, int index) {
	struct item *item = &sd->inventory.u.items_inventory[index];
	int i;

	if( !item->equipSwitch )
		return;
	for( i = 0; i < EQI_MAX; i++ ) {
		if( sd->equip_switch_index[i] == index ) //If a match is found
			sd->equip_switch_index[i] = -1; //Remove it from the slot
	}
	//Send out one packet for all slots using the current item's mask
	clif_equipswitch_remove(sd,index,item->equipSwitch,false);
	item->equipSwitch = 0;
}

/*==========================================
 * Checking if player (sd) has an invalid item
 * and is unequipped on map load (item_noequip)
 *------------------------------------------*/
void pc_checkitem(struct map_session_data *sd) {
	int i, calc_flag = 0;
	struct item *it;

	nullpo_retv(sd);

	//Avoid reorganizing items when we are vending, as that leads to exploits (pointed out by End of Exam)
	if( sd->state.vending )
		return;

	pc_check_available_item(sd,ITMCHK_NONE); //Check for invalid(ated) items

	for( i = 0; i < MAX_INVENTORY; i++ ) {
		it = &sd->inventory.u.items_inventory[i];

		if( !it->nameid )
			continue;
		if( !it->equip )
			continue;
		if( it->equip&~pc_equippoint(sd,i) ||
			(!pc_has_permission(sd,PC_PERM_USE_ALL_EQUIPMENT) &&
			!battle_config.allow_equip_restricted_item &&
			itemdb_isNoEquip(sd->inventory_data[i],sd->bl.m)) )
		{
			pc_unequipitem(sd,i,2);
			calc_flag = 1;
			continue;
		}
	}

	for( i = 0; i < MAX_INVENTORY; i++ ) {
		it = &sd->inventory.u.items_inventory[i];

		if( !it->nameid )
			continue;
		if( !it->equipSwitch )
			continue;
		if( it->equipSwitch&~pc_equippoint(sd,i) ||
			(!pc_has_permission(sd,PC_PERM_USE_ALL_EQUIPMENT) &&
			!battle_config.allow_equip_restricted_item &&
			itemdb_isNoEquip(sd->inventory_data[i],sd->bl.m)) )
		{
			int j;

			for( j = 0; j < EQI_MAX; j++ ) {
				if( sd->equip_switch_index[j] == i )
					sd->equip_switch_index[j] = -1;
			}
			sd->inventory.u.items_inventory[i].equipSwitch = 0;
			continue;
		}
	}

	if( calc_flag && sd->state.active ) {
		pc_checkallowskill(sd);
		status_calc_pc(sd,SCO_NONE);
	}
}

/**
 * Checks for unavailable items and removes them.
 * @param sd: Player data
 * @param type Forced check:
 *   1 - Inventory
 *   2 - Cart
 *   4 - Storage
 */
void pc_check_available_item(struct map_session_data *sd, uint8 type) {
	int i;
	unsigned short nameid;
	char output[256];

	nullpo_retv(sd);

	if( battle_config.item_check&ITMCHK_INVENTORY && (type&ITMCHK_INVENTORY) ) { //Check for invalid(ated) items in inventory
		for( i = 0; i < MAX_INVENTORY; i++ ) {
			nameid = sd->inventory.u.items_inventory[i].nameid;
			if( !nameid )
				continue;
			if( !itemdb_available(nameid) ) {
				sprintf(output,msg_txt(sd,709),nameid); // Item %hu has been removed from your inventory.
				clif_displaymessage(sd->fd,output);
				ShowWarning("Removed invalid/disabled item (ID: %hu, amount: %d) from inventory (char_id: %d).\n",nameid,sd->inventory.u.items_inventory[i].amount,sd->status.char_id);
				pc_delitem(sd,i,sd->inventory.u.items_inventory[i].amount,4,0,LOG_TYPE_OTHER);
				continue;
			}
			if( !sd->inventory.u.items_inventory[i].unique_id && !itemdb_isstackable(nameid) )
				sd->inventory.u.items_inventory[i].unique_id = pc_generate_unique_id(sd);
		}
	}

	if( battle_config.item_check&ITMCHK_CART && (type&ITMCHK_CART) ) { //Check for invalid(ated) items in cart
		for( i = 0; i < MAX_CART; i++ ) {
			nameid = sd->cart.u.items_cart[i].nameid;
			if( !nameid )
				continue;
			if( !itemdb_available(nameid) ) {
				sprintf(output,msg_txt(sd,710),nameid); // Item %hu has been removed from your cart.
				clif_displaymessage(sd->fd,output);
				ShowWarning("Removed invalid/disabled item (ID: %hu, amount: %d) from cart (char_id: %d).\n",nameid,sd->cart.u.items_cart[i].amount,sd->status.char_id);
				pc_cart_delitem(sd,i,sd->cart.u.items_cart[i].amount,0,LOG_TYPE_OTHER);
				continue;
			}
			if( !sd->cart.u.items_cart[i].unique_id && !itemdb_isstackable(nameid) )
				sd->cart.u.items_cart[i].unique_id = pc_generate_unique_id(sd);
		}
	}

	if( battle_config.item_check&ITMCHK_STORAGE && (type&ITMCHK_STORAGE) ) { //Check for invalid(ated) items in storage
		for( i = 0; i < sd->storage.max_amount; i++ ) {
			nameid = sd->storage.u.items_storage[i].nameid;
			if( !nameid )
				continue;
			if( !itemdb_available(nameid) ) {
				sprintf(output,msg_txt(sd,711),nameid); // Item %hu has been removed from your storage.
				clif_displaymessage(sd->fd,output);
				ShowWarning("Removed invalid/disabled item (ID: %hu, amount: %d) from storage (char_id: %d).\n",nameid,sd->storage.u.items_storage[i].amount,sd->status.char_id);
				storage_delitem(sd,&sd->storage,i,sd->storage.u.items_storage[i].amount);
				continue;
			}
			if( !sd->storage.u.items_storage[i].unique_id && !itemdb_isstackable(nameid) )
				sd->storage.u.items_storage[i].unique_id = pc_generate_unique_id(sd);
		}
	}
}

/*==========================================
 * Update PVP rank for sd1 in cmp to sd2
 *------------------------------------------*/
static int pc_calc_pvprank_sub(struct block_list *bl,va_list ap)
{
	struct map_session_data *sd1,*sd2;

	sd1 = (struct map_session_data *)bl;
	sd2 = va_arg(ap,struct map_session_data *);

	if( pc_isinvisible(sd1) || pc_isinvisible(sd2) )
		return 0; //Cannot register pvp rank for hidden GMs

	if( sd1->pvp_point > sd2->pvp_point )
		sd2->pvp_rank++;
	return 0;
}

/*==========================================
 * Calculate new rank beetween all present players (map_foreachinallarea)
 * and display result
 *------------------------------------------*/
int pc_calc_pvprank(struct map_session_data *sd)
{
	int old = sd->pvp_rank;
	struct map_data *m = &mapdata[sd->bl.m];

	sd->pvp_rank = 1;
	map_foreachinmap(pc_calc_pvprank_sub,sd->bl.m,BL_PC,sd);
	if( old != sd->pvp_rank || sd->pvp_lastusers != m->users_pvp )
		clif_pvpset(sd,sd->pvp_rank,sd->pvp_lastusers = m->users_pvp,0);
	return sd->pvp_rank;
}

/*==========================================
 * Calculate next sd ranking calculation from config
 *------------------------------------------*/
TIMER_FUNC(pc_calc_pvprank_timer)
{
	struct map_session_data *sd = map_id2sd(id);

	if( !sd )
		return 0;

	sd->pvp_timer = INVALID_TIMER;

	if( pc_isinvisible(sd) )
		return 0; //Do not calculate the pvp rank for a hidden GM

	if( pc_calc_pvprank(sd) > 0 )
		sd->pvp_timer = add_timer(gettick() + PVP_CALCRANK_INTERVAL,pc_calc_pvprank_timer,id,data);
	return 0;
}

/*==========================================
 * Checking if sd is married.
 * Return:
 *	partner_id = yes
 *	0 = no
 *------------------------------------------*/
int pc_ismarried(struct map_session_data *sd)
{
	if( !sd )
		return -1;
	if( sd->status.partner_id )
		return sd->status.partner_id;
	else
		return 0;
}

/*==========================================
 * Marry player sd to player dstsd.
 * Return:
 *  false = fail
 *  true = success
 *------------------------------------------*/
bool pc_marriage(struct map_session_data *sd,struct map_session_data *dstsd)
{
	if( !sd || !dstsd || sd->status.partner_id || dstsd->status.partner_id || (sd->class_&JOBL_BABY) || (dstsd->class_&JOBL_BABY) )
		return false;

	sd->status.partner_id = dstsd->status.char_id;
	dstsd->status.partner_id = sd->status.char_id;
	achievement_update_objective(sd, AG_MARRY, 1, 1);
	achievement_update_objective(dstsd, AG_MARRY, 1, 1);

	return true;
}

/*==========================================
 * Divorce sd from its partner.
 * Return:
 *  false = fail
 *  true  = success
 *------------------------------------------*/
bool pc_divorce(struct map_session_data *sd)
{
	struct map_session_data *p_sd;
	int i;

	if( !sd || !pc_ismarried(sd) )
		return false;

	if( !sd->status.partner_id )
		return false; //Char is not married

	if( !(p_sd = map_charid2sd(sd->status.partner_id)) ) { //Lets char server do the divorce
		if( chrif_divorce(sd->status.char_id, sd->status.partner_id) )
			return false; //No char server connected
		return true;
	}

	//Both players online, lets do the divorce manually
	sd->status.partner_id = 0;
	p_sd->status.partner_id = 0;
	for( i = 0; i < MAX_INVENTORY; i++ ) {
		if( sd->inventory.u.items_inventory[i].nameid == WEDDING_RING_M || sd->inventory.u.items_inventory[i].nameid == WEDDING_RING_F )
			pc_delitem(sd, i, 1, 0, 0, LOG_TYPE_OTHER);
		if( p_sd->inventory.u.items_inventory[i].nameid == WEDDING_RING_M || p_sd->inventory.u.items_inventory[i].nameid == WEDDING_RING_F )
			pc_delitem(p_sd, i, 1, 0, 0, LOG_TYPE_OTHER);
	}

	clif_divorced(sd, p_sd->status.name);
	clif_divorced(p_sd, sd->status.name);

	return true;
}

/**
 * Get the partner map_session_data of a player
 * @param sd : the husband|wife session
 * @return partner session or NULL
 */
struct map_session_data *pc_get_partner(struct map_session_data *sd)
{
	if( !sd || !pc_ismarried(sd) )
		return NULL;
	return map_charid2sd(sd->status.partner_id);
}

/**
 * Get the father map_session_data of a player
 * @param sd : the baby session
 * @return father session or NULL
 */
struct map_session_data *pc_get_father(struct map_session_data *sd)
{
	if( !sd || !(sd->class_&JOBL_BABY) || !sd->status.father )
		return NULL;
	return map_charid2sd(sd->status.father);
}

/**
 * Get the mother map_session_data of a player
 * @param sd : the baby session
 * @return mother session or NULL
 */
struct map_session_data *pc_get_mother(struct map_session_data *sd)
{
	if( !sd || !(sd->class_&JOBL_BABY) || !sd->status.mother )
		return NULL;
	return map_charid2sd(sd->status.mother);
}

/**
 * Get the child map_session_data of a player
 * @param sd : the parent session
 * @return child session or NULL
 */
struct map_session_data *pc_get_child(struct map_session_data *sd)
{
	if( !sd || !pc_ismarried(sd) || !sd->status.child )
		return NULL;
	return map_charid2sd(sd->status.child);
}

/*==========================================
 * Set player sd to bleed. (losing hp and/or sp each diff_tick)
 *------------------------------------------*/
void pc_bleeding(struct map_session_data *sd, unsigned int diff_tick)
{
	int hp = 0, sp = 0;

	if (pc_isdead(sd))
		return;

	if (sd->hp_loss.value) {
		sd->hp_loss.tick += diff_tick;
		while (sd->hp_loss.tick >= sd->hp_loss.rate) {
			hp += sd->hp_loss.value;
			sd->hp_loss.tick -= sd->hp_loss.rate;
		}
		if (hp >= sd->battle_status.hp)
			hp = sd->battle_status.hp - 1; //Script drains cannot kill you
	}

	if (sd->sp_loss.value) {
		sd->sp_loss.tick += diff_tick;
		while (sd->sp_loss.tick >= sd->sp_loss.rate) {
			sp += sd->sp_loss.value;
			sd->sp_loss.tick -= sd->sp_loss.rate;
		}
	}

	if (hp > 0 || sp > 0)
		status_zap(&sd->bl, hp, sp);
}

//Character regen. Flag is used to know which types of regen can take place.
//&1: HP regen
//&2: SP regen
void pc_regen(struct map_session_data *sd, unsigned int diff_tick)
{
	int hp = 0, sp = 0;

	if (sd->hp_regen.value) {
		sd->hp_regen.tick += diff_tick;
		while (sd->hp_regen.tick >= sd->hp_regen.rate) {
			hp += sd->hp_regen.value;
			sd->hp_regen.tick -= sd->hp_regen.rate;
		}
	}

	if (sd->sp_regen.value) {
		sd->sp_regen.tick += diff_tick;
		while (sd->sp_regen.tick >= sd->sp_regen.rate) {
			sp += sd->sp_regen.value;
			sd->sp_regen.tick -= sd->sp_regen.rate;
		}
	}

	if (sd->percent_hp_regen.value) {
		sd->percent_hp_regen.tick += diff_tick;
		while (sd->percent_hp_regen.tick >= sd->percent_hp_regen.rate) {
			hp += sd->percent_hp_regen.value * sd->status.max_hp;
			sd->percent_hp_regen.tick -= sd->percent_hp_regen.rate;
		}
	}

	if (sd->percent_sp_regen.value) {
		sd->percent_sp_regen.tick += diff_tick;
		while (sd->percent_sp_regen.tick >= sd->percent_sp_regen.rate) {
			sp += sd->percent_sp_regen.value * sd->status.max_sp;
			sd->percent_sp_regen.tick -= sd->percent_sp_regen.rate;
		}
	}

	if (hp > 0 || sp > 0)
		status_heal(&sd->bl, hp, sp, 0);
}

/*==========================================
 * Memo player sd savepoint. (map,x,y)
 *------------------------------------------*/
void pc_setsavepoint(struct map_session_data *sd, short mapindex,int x,int y)
{
	nullpo_retv(sd);

	sd->status.save_point.map = mapindex;
	sd->status.save_point.x = x;
	sd->status.save_point.y = y;
}

/*==========================================
 * Save 1 player data  at autosave intervalle
 *------------------------------------------*/
static TIMER_FUNC(pc_autosave)
{
	int interval;
	struct s_mapiterator *iter;
	struct map_session_data *sd;
	static int last_save_id = 0, save_flag = 0;

	if (save_flag == 2) //Someone was saved on last call, normal cycle
		save_flag = 0;
	else
		save_flag = 1; //Noone was saved, so save first found char

	iter = mapit_getallusers();
	for (sd = (TBL_PC *)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC *)mapit_next(iter)) {
		if (!sd->state.pc_loaded)
			continue; //Player data hasn't fully loaded
		if (sd->bl.id == last_save_id && save_flag != 1) {
			save_flag = 1;
			continue;
		}
		if (save_flag != 1) //Not our turn to save yet
			continue;
		//Save char
		last_save_id = sd->bl.id;
		save_flag = 2;
		if (pc_isvip(sd)) //Check if we're still vip
			chrif_req_login_operation(sd->status.account_id,sd->status.name,CHRIF_OP_LOGIN_VIP,0,0x1);
		chrif_save(sd,CSAVE_INVENTORY|CSAVE_CART);
		break;
	}
	mapit_free(iter);

	interval = autosave_interval / (map_usercount() + 1);
	if (interval < minsave_interval)
		interval = minsave_interval;
	add_timer(gettick() + interval,pc_autosave,0,0);

	return 0;
}

static int pc_daynight_timer_sub(struct map_session_data *sd,va_list ap)
{
	if (sd->state.night != night_flag && mapdata[sd->bl.m].flag.nightenabled) { //Night/day state does not match
		clif_status_load(&sd->bl, SI_SKE, night_flag); //New night effect by dynamix [Skotlex]
		sd->state.night = night_flag;
		return 1;
	}
	return 0;
}
/*================================================
 * timer to do the day [Yor]
 * data: 0 = called by timer, 1 = gmcommand/script
 *------------------------------------------------*/
TIMER_FUNC(map_day_timer)
{
	char tmp_soutput[1024];

	if (!data && battle_config.day_duration <= 0) //If we want a day
		return 0;

	if (!night_flag)
		return 0; //Already day

	night_flag = 0; //0 = day, 1 = night [Yor]
	map_foreachpc(pc_daynight_timer_sub);
	strcpy(tmp_soutput, (!data ? msg_txt(NULL, 502) : msg_txt(NULL, 60))); // The day has arrived!
	intif_broadcast(tmp_soutput, strlen(tmp_soutput) + 1, BC_DEFAULT);
	return 0;
}

/*================================================
 * timer to do the night [Yor]
 * data: 0 = called by timer, 1 = gmcommand/script
 *------------------------------------------------*/
TIMER_FUNC(map_night_timer)
{
	char tmp_soutput[1024];

	if (!data && battle_config.night_duration <= 0) //If we want a night
		return 0;

	if (night_flag)
		return 0; //Already night

	night_flag = 1; //0 = day, 1 = night [Yor]
	map_foreachpc(pc_daynight_timer_sub);
	strcpy(tmp_soutput, (!data ? msg_txt(NULL, 503) : msg_txt(NULL, 59))); // The night has fallen
	intif_broadcast(tmp_soutput, strlen(tmp_soutput) + 1, BC_DEFAULT);
	return 0;
}

void pc_setstand(struct map_session_data *sd)
{
	nullpo_retv(sd);

	status_change_end(&sd->bl, SC_TENSIONRELAX, INVALID_TIMER);
	status_change_end(&sd->bl, SC_MEIKYOUSISUI, INVALID_TIMER);

	if (sd->sc.data[SC_SITDOWN_FORCE] || sd->sc.data[SC_BANANA_BOMB_SITDOWN])
		return;

	clif_status_load(&sd->bl, SI_SIT, 0);
	clif_standing(&sd->bl); //Inform area PC is standing

	//Reset skill-related recovery (only when sit) tick
	sd->ssregen.tick.hp = sd->ssregen.tick.sp = 0;

	sd->state.dead_sit = sd->vd.dead_sit = 0;
	if (pc_isdead(sd))
		clif_party_dead(sd);
}

/**
 * Calculate Overheat value
 * @param sd: Player data
 * @param heat: Amount of Heat to adjust
 */
void pc_overheat(struct map_session_data *sd, int16 heat)
{
	struct status_change_entry *sce = NULL;
	int16 limit[] = { 150,200,280,360,450 };
	uint16 skill_lv;

	nullpo_retv(sd);

	skill_lv = cap_value(pc_checkskill(sd, NC_MAINFRAME), 0, 4);
	if( (sce = sd->sc.data[SC_OVERHEAT_LIMITPOINT]) ) {
		sce->val1 += heat;
		sce->val1 = cap_value(sce->val1, 0, 1000);
		if( sd->sc.data[SC_OVERHEAT] )
			status_change_end(&sd->bl, SC_OVERHEAT, INVALID_TIMER);
		if( sce->val1 > limit[skill_lv] )
			sc_start(&sd->bl, &sd->bl, SC_OVERHEAT, 100, sce->val1, 1000);
	} else if( heat > 0 )
		sc_start(&sd->bl, &sd->bl, SC_OVERHEAT_LIMITPOINT, 100, heat, 1000);
}

/**
 * Check if player is autolooting given itemID.
 */
bool pc_isautolooting(struct map_session_data *sd, unsigned short nameid)
{
	uint8 i = 0;
	bool j = false;

	if( !sd->state.autolooting && !sd->state.autolootingtype )
		return false;

	if( sd->state.autolooting )
		ARR_FIND(0, AUTOLOOTITEM_SIZE, i, sd->state.autolootid[i] == nameid);

	if( sd->state.autolootingtype && sd->state.autoloottype&(1<<itemdb_type(nameid)) )
		j = true;

	return (sd->state.autolooting ? (i != AUTOLOOTITEM_SIZE) : (sd->state.autolootingtype ? j : false));
}

/**
 * Checks if player can use @/#command
 * @param sd Player map session data
 * @param command Command name without @/# and params
 * @param type is it atcommand or charcommand
 */
bool pc_can_use_command(struct map_session_data *sd, const char *command, AtCommandType type)
{
	return pc_group_can_use_command(pc_get_group_id(sd), command, type);
}

/**
 * Checks if commands used by a player should be logged
 * according to their group setting.
 * @param sd Player map session data
 */
bool pc_should_log_commands(struct map_session_data *sd)
{
	return pc_group_should_log_commands(pc_get_group_id(sd));
}

#if defined(RENEWAL_DROP) || defined(RENEWAL_EXP)
/**
 * Renewal EXP/Item Drop rate modifier based on level penalty
 * @param level_diff: Monster and Player level difference
 * @param mob_class: Monster class
 * @param mode: Monster mode
 * @param type: 1 - EXP, 2 - Item Drop
 * @return Penalty rate
 */
int pc_level_penalty_mod(int level_diff, uint32 mob_class, enum e_mode mode, int type)
{
	int rate = 100;

	if (type == 2 && (mode&MD_FIXED_ITEMDROP))
		return rate;

	if (level_diff < 0)
		level_diff = MAX_LEVEL + (~level_diff + 1);

	if ((rate = level_penalty[type][mob_class][level_diff]) > 0) //Monster class found, return rate
		return rate;

	return 100; //Penalty not found, return default
}
#endif

int pc_split_str(char *str, char **val, int num)
{
	int i;

	for (i = 0; i < num && str; i++) {
		val[i] = str;
		str = strchr(str,',');
		if (str && i < num - 1) //Do not remove a trailing comma
			*str++ = 0;
	}
	return i;
}

int pc_split_atoi(char *str, int *val, char sep, int max)
{
	int i, j;

	for (i = 0; i < max; i++) {
		if (!str)
			break;
		val[i] = atoi(str);
		str = strchr(str,sep);
		if (str)
			*str++ = 0;
	}
	//Zero up the remaining
	for (j = i; j < max; j++)
		val[j] = 0;
	return i;
}

/*==========================================
 * sub DB reading.
 * Function used to read skill_tree.txt
 *------------------------------------------*/
static bool pc_readdb_skilltree(char *fields[], int columns, int current)
{
	uint32 baselv, joblv, baselv_max, joblv_max;
	uint16 skill_id, skill_lv, skill_lv_max;
	int idx, class_;
	unsigned int i, offset, skill_idx;

	class_ = atoi(fields[0]);
	skill_id = (uint16)atoi(fields[1]);
	skill_lv = (uint16)atoi(fields[2]);

	if (columns == 5 + MAX_PC_SKILL_REQUIRE * 2) { //Base/Job level requirement extra columns
		baselv = (uint32)atoi(fields[3]);
		joblv = (uint32)atoi(fields[4]);
		offset = 5;
	} else if (columns == 3 + MAX_PC_SKILL_REQUIRE * 2) {
		baselv = joblv = 0;
		offset = 3;
	} else {
		ShowWarning("pc_readdb_skilltree: Invalid number of colums in skill %hu of job %d's tree.\n", skill_id, class_);
		return false;
	}

	if (!pcdb_checkid(class_)) {
		ShowWarning("pc_readdb_skilltree: Invalid job class %d specified.\n", class_);
		return false;
	}

	idx = pc_class2idx(class_);

	if (!skill_get_index(skill_id)) {
		ShowWarning("pc_readdb_skilltree: Unable to load skill %hu into job %d's tree.", skill_id, class_);
		return false;
	}

	if (skill_lv > (skill_lv_max = skill_get_max(skill_id))) {
		ShowWarning("pc_readdb_skilltree: Skill %hu's level %hu exceeds job %d's max level %hu. Capping skill level..\n", skill_id, skill_lv, class_, skill_lv_max);
		skill_lv = skill_lv_max;
	}

	if (baselv > (baselv_max = pc_class_maxbaselv(class_))) {
		ShowWarning("pc_readdb_skilltree: Skill %hu's base level requirement %d exceeds job %d's max base level %d. Capping skill base level..\n", skill_id, baselv, class_, baselv_max);
		baselv = baselv_max;
	}

	if (joblv > (joblv_max = pc_class_maxjoblv(class_))) {
		ShowWarning("pc_readdb_skilltree: Skill %hu's job level requirement %d exceeds job %d's max job level %d. Capping skill job level..\n", skill_id, joblv, class_, joblv_max);
		joblv = joblv_max;
	}

	//This is to avoid adding two lines for the same skill [Skotlex]
	ARR_FIND(0, MAX_SKILL_TREE, skill_idx, (!skill_tree[idx][skill_idx].id || skill_tree[idx][skill_idx].id == skill_id));
	if (skill_idx == MAX_SKILL_TREE) {
		ShowWarning("pc_readdb_skilltree: Unable to load skill %hu into job %d's tree. Maximum number of skills per job has been reached.\n", skill_id, class_);
		return false;
	} else if(skill_tree[idx][skill_idx].id)
		ShowNotice("pc_readdb_skilltree: Overwriting skill %hu for job %d.\n", skill_id, class_);

	skill_tree[idx][skill_idx].id = skill_id;
	skill_tree[idx][skill_idx].lv = skill_lv;
	skill_tree[idx][skill_idx].baselv = baselv;
	skill_tree[idx][skill_idx].joblv = joblv;

	for (i = 0; i < MAX_PC_SKILL_REQUIRE; i++) {
		skill_id = (uint16)atoi(fields[i * 2 + offset]);
		skill_lv = (uint16)atoi(fields[i * 2 + offset + 1]);
		if (!skill_id)
			continue;
		if (!skill_get_index(skill_id)) {
			ShowWarning("pc_readdb_skilltree: Unable to load requirement skill %hu into job %d's tree.", skill_id, class_);
			return false;
		}
		if (skill_lv > (skill_lv_max = skill_get_max(skill_id))) {
			ShowWarning("pc_readdb_skilltree: Skill %hu's level %hu exceeds job %d's max level %hu. Capping skill level..\n", skill_id, skill_lv, class_, skill_lv_max);
			skill_lv = skill_lv_max;
		}
		skill_tree[idx][skill_idx].need[i].id = skill_id;
		skill_tree[idx][skill_idx].need[i].lv = skill_lv;
	}
	return true;
}

#if defined(RENEWAL_DROP) || defined(RENEWAL_EXP)
static bool pc_readdb_levelpenalty(char *fields[], int columns, int current)
{
	int type, class_, diff;

	type = atoi(fields[0]); //1 = experience, 2 = item drop
	class_ = atoi(fields[1]);
	diff = atoi(fields[2]);

	if (type != 1 && type != 2) {
		ShowWarning("pc_readdb_levelpenalty: Invalid type %d specified.\n", type);
		return false;
	}

	if (!CHK_CLASS(class_)) {
		ShowWarning("pc_readdb_levelpenalty: Invalid class %d specified.\n", class_);
		return false;
	}

	diff = min(diff, MAX_LEVEL);

	if (diff < 0)
		diff = min(MAX_LEVEL + (~(diff) + 1), MAX_LEVEL * 2);

	level_penalty[type][class_][diff] = atoi(fields[3]);

	return true;
}
#endif

/** [Cydh]
 * Calculates base hp of player. Reference: http://irowiki.org/wiki/Max_HP
 * @param level Base level of player
 * @param class_ Job ID @see enum e_job
 * @return base_hp
 */
static unsigned int pc_calc_basehp(uint16 level, uint16 class_) {
	double base_hp;
	uint16 i, idx = pc_class2idx(class_);

	base_hp = 35 + level * (job_info[idx].hp_multiplicator / 100.);
#ifndef RENEWAL
	switch (class_) {
		case JOB_NINJA:
		case JOB_BABY_NINJA:
		case JOB_GUNSLINGER:
		case JOB_BABY_GUNSLINGER:
			if (level >= 10)
				base_hp += 90;
			break;
	}
#endif
	for (i = 2; i <= level; i++)
		base_hp += floor(i * (job_info[idx].hp_factor / 100.) + 0.5); //Don't have round()
	if (class_ == JOB_SUMMONER || class_ == JOB_BABY_SUMMONER)
		base_hp += floor((base_hp / 2) + 0.5);
	return (unsigned int)base_hp;
}

/** [Playtester]
 * Calculates base sp of player.
 * @param level Base level of player
 * @param class_ Job ID @see enum e_job
 * @return base_sp
 */
static unsigned int pc_calc_basesp(uint16 level, uint16 class_) {
	double base_sp;
	uint16 idx = pc_class2idx(class_);

	base_sp = 10 + floor(level * (job_info[idx].sp_factor / 100.));
	switch (class_) {
		case JOB_NINJA:
		case JOB_BABY_NINJA:
			if (level >= 10)
				base_sp -= 22;
			else
				base_sp = 11 + 3 * level;
			break;
		case JOB_GUNSLINGER:
		case JOB_BABY_GUNSLINGER:
			if (level >= 10)
				base_sp -= 18;
			else
				base_sp = 9 + 3 * level;
			break;
		case JOB_SUMMONER:
		case JOB_BABY_SUMMONER:
			base_sp -= floor(base_sp / 2);
			break;
	}
	return (unsigned int)base_sp;
}

//Reading job_db1.txt line, (class,weight,HPFactor,HPMultiplicator,SPFactor,aspd/lvl...)
static bool pc_readdb_job1(char *fields[], int columns, int current) {
	int idx, class_;
	unsigned int i;

	class_ = atoi(fields[0]);

	if (!pcdb_checkid(class_)) {
		ShowWarning("status_readdb_job1: Invalid job class %d specified.\n", class_);
		return false;
	}
	idx = pc_class2idx(class_);

	job_info[idx].max_weight_base = atoi(fields[1]);
	job_info[idx].hp_factor = atoi(fields[2]);
	job_info[idx].hp_multiplicator = atoi(fields[3]);
	job_info[idx].sp_factor = atoi(fields[4]);

#ifdef RENEWAL_ASPD
	for (i = 0; i <= MAX_WEAPON_TYPE; i++)
#else
	for (i = 0; i < MAX_WEAPON_TYPE; i++)
#endif
		job_info[idx].aspd_base[i] = atoi(fields[i + 5]);

	return true;
}

//Reading job_db2.txt line (class,JobLv1,JobLv2,JobLv3,...)
static bool pc_readdb_job2(char *fields[], int columns, int current)
{
	int idx, class_, i;

	class_ = atoi(fields[0]);

	if (!pcdb_checkid(class_)) {
		ShowWarning("status_readdb_job2: Invalid job class %d specified.\n", class_);
		return false;
	}
	idx = pc_class2idx(class_);

	for (i = 1; i < columns; i++)
		job_info[idx].job_bonus[i - 1] = atoi(fields[i]);

	return true;
}

static void pc_readdb_job_exp_libconfig_sub(struct config_setting_t *it, int16 count, const char *source)
{
	struct config_setting_t *job = NULL, *exp = NULL, *expval = NULL;
	int i, i32, maxlvl, idx, type, class_, job_count, start, exp_count;
	const char *name;

	nullpo_retv(it);
	nullpo_retv(source);

	if (!config_setting_lookup_int(it, "MaxLevel", &i32) || i32 < 1 || i32 > MAX_LEVEL) {
		ShowError("pc_readdb_job_exp_libconfig_sub: Missing or invalid MaxLevel %d in \"%s\", entry #%d.\n", i32, source, count);
		return;
	}
	maxlvl = i32;
	if ((job = config_setting_get_member(it, "JobID")) && !config_setting_is_aggregate(job)) {
		ShowError("pc_readdb_job_exp_libconfig_sub: Invalid JobID format in \"%s\", entry #%d.\n", source, count);
		return;
	}
	job_count = config_setting_length(job);
	if (!config_setting_lookup_string(it, "Type", &name)) {
		ShowError("pc_readdb_job_exp_libconfig_sub: Missing Type in \"%s\", entry #%d.\n", source, count);
		return;
	}
	if (!script_get_constant(name, &i32) || (i32 != BASE_EXP && i32 != JOB_EXP)) {
		ShowError("pc_readdb_job_exp_libconfig_sub: Invalid Type '%s' in \"%s\", entry #%d.\n", name, source, count);
		return;
	}
	type = i32;
	if ((exp = config_setting_get_member(it, "Experience")) && config_setting_is_group(exp)) {
		start = 1;
		if (config_setting_lookup_int(exp, "StartLevel", &i32) && i32 >= 0)
			start = i32;
		if ((expval = config_setting_get_member(exp, "Value")) && !config_setting_is_aggregate(expval)) {
			ShowError("pc_readdb_job_exp_libconfig_sub: Invalid Experience Value format in \"%s\", entry #%d.\n", source, count);
			return;
		}
		exp_count = config_setting_length(expval);
	}
	for (i = 0; i < job_count; i++) {
		int j;

		class_ = config_setting_get_int_elem(job, i);
		if (!pcdb_checkid(class_)) {
			ShowError("pc_readdb_job_exp_libconfig_sub: Invalid JobID %d in \"%s\", entry #%d.\n", class_, source, count);
			continue;
		}
		idx = pc_class2idx(class_);
		job_info[idx].max_level[type] = maxlvl;
		if (start == 1) {
			int val1 = maxlvl;
			int val2 = 0;

			if (exp_count < val1) {
				ShowWarning("pc_readdb_job_exp_libconfig_sub: Found MaxLevel %u for JobID %d, but EXP Value only goes up to level %u in \"%s\", entry #%d.\n", maxlvl, class_, exp_count, source, count);
				ShowInfo("Filling the missing values with the last EXP value.\n");
				val2 = val1 - exp_count;
				val1 = exp_count;
			}
			for (j = 0; j < val1; j++)
				job_info[idx].exp_table[type][j] = config_setting_get_int_elem(expval, j);
			for (j = 0; val2 && j < val2; j++) {
				job_info[idx].exp_table[type][val1] = config_setting_get_int_elem(expval, exp_count - 1);
				val1++;
			}
		} else {
			int val1 = start - 1;
			int val2 = maxlvl - val1;

			for (j = 0; j < val1; j++)
				job_info[idx].exp_table[type][j] = j + 1;
			for (j = 0; j < val2; j++) {
				job_info[idx].exp_table[type][val1] = config_setting_get_int_elem(expval, j);
				val1++;
			}
		}
	}
}

static void pc_readdb_job_exp_libconfig(const char *filename)
{
	struct config_setting_t *jdb = NULL, *t = NULL;
	char filepath[256];
	int count = 0;

	safesnprintf(filepath, sizeof(filepath), "%s/%s", db_path, filename);
	if (config_read_file(&job_exp_db_conf, filepath))
		return;
	if (!(jdb = config_lookup(&job_exp_db_conf, "job_exp_db")))
		return;
	while ((t = config_setting_get_elem(jdb, count))) {
		pc_readdb_job_exp_libconfig_sub(t, count, filename);
		count++;
	}
	config_destroy(&job_exp_db_conf);
	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s/%s"CL_RESET"'.\n", count, db_path, filename);
}

/**
 * #ifdef HP_SP_TABLES, reads 'job_basehpsp_db.txt to replace hp/sp results from formula
 * startlvl,endlvl,class,type,values...
 */
#ifdef HP_SP_TABLES
static bool pc_readdb_job_basehpsp(char *fields[], int columns, int current)
{
	int i, startlvl, endlvl;
	int job_count, jobs[CLASS_COUNT];
	short type;

	startlvl = atoi(fields[0]);
	if (startlvl < 1) {
		ShowError("pc_readdb_job_basehpsp: Invalid start level %d specified.\n", startlvl);
		return false;
	}
	endlvl = atoi(fields[1]);
	if (endlvl < 1 || endlvl < startlvl) {
		ShowError("pc_readdb_job_basehpsp: Invalid end level %d specified.\n", endlvl);
		return false;
	}
	if ((endlvl - startlvl + 1 + 4) > columns) { //NB values = (maxlvl - startlvl) + 1 - index1stvalue
		ShowError("pc_readdb_job_basehpsp: Number of columns %d (needs %d) defined is too low for start level %d, max level %d.\n",columns,(endlvl-startlvl+1+4),startlvl,endlvl);
		return false;
	}
	type = atoi(fields[3]);
	if (type < 0 || type > 1) {
		ShowError("pc_readdb_job_basehpsp: Invalid type %d specified.\n", type);
		return false;
	}

	job_count = pc_split_atoi(fields[2], jobs, ':', CLASS_COUNT);
	if (job_count < 1)
		return false;

	for (i = 0; i < job_count; i++) {
		int idx, job_id = jobs[i], use_endlvl;

		if (!pcdb_checkid(job_id)) {
			ShowError("pc_readdb_job_basehpsp: Invalid job class %d specified.\n", job_id);
			return false;
		}
		idx = pc_class2idx(job_id);
		if (startlvl > job_info[idx].max_level[0]) {
			ShowError("pc_readdb_job_basehpsp: Invalid start level %d specified.\n", startlvl);
			return false;
		}
		//Just read until available max level for this job, don't use MAX_LEVEL!
		use_endlvl = endlvl;
		if (use_endlvl > job_info[idx].max_level[0])
			use_endlvl = job_info[idx].max_level[0];

		if (type == 0) { //HP type
			uint16 j;

			for (j = 0; j < use_endlvl; j++) {
				if (atoi(fields[j + 4])) {
					uint16 lvl_idx = startlvl - 1 + j;

					job_info[idx].base_hp[lvl_idx] = atoi(fields[j + 4]);
					//Tells if this HP is lower than previous level (but not for 99->100)
					if (lvl_idx - 1 >= 0 && lvl_idx != 99 && job_info[idx].base_hp[lvl_idx] < job_info[idx].base_hp[lvl_idx - 1])
						ShowWarning("pc_readdb_job_basehpsp: HP value at entry %d col %d is lower than previous level (job=%d,lvl=%d,oldval=%d,val=%d).\n",
							current,j + 4,job_id,lvl_idx + 1,job_info[idx].base_hp[lvl_idx - 1],job_info[idx].base_hp[lvl_idx]);
				}
			}
		} else { //SP type
			uint16 j;

			for (j = 0; j < use_endlvl; j++) {
				if (atoi(fields[j + 4])) {
					uint16 lvl_idx = startlvl - 1 + j;

					job_info[idx].base_sp[lvl_idx] = atoi(fields[j + 4]);
					//Tells if this SP is lower than previous level (but not for 99->100)
					if (lvl_idx - 1 >= 0 && lvl_idx != 99 && job_info[idx].base_sp[lvl_idx] < job_info[idx].base_sp[lvl_idx - 1])
						ShowWarning("pc_readdb_job_basehpsp: SP value at entry %d col %d is lower than previous level (job=%d,lvl=%d,oldval=%d,val=%d).\n",
							current,j + 4,job_id,lvl_idx + 1,job_info[idx].base_sp[lvl_idx - 1],job_info[idx].base_sp[lvl_idx]);
				}
			}
		}
	}
	return true;
}
#endif

/** [Cydh]
 * Reads 'job_param_db.txt' to check max. param each job and store them to job_info[].max_param.*
 */
static bool pc_readdb_job_param(char *fields[], int columns, int current)
{
	int idx, class_;
	uint16 str, agi, vit, int_, dex, luk;

	script_get_constant(trim(fields[0]),&class_);

	if ((idx = pc_class2idx(class_)) < 0) {
		ShowError("pc_readdb_job_param: Invalid job '%s'. Skipping!",fields[0]);
		return false;
	}
	str = cap_value(atoi(fields[1]),10,SHRT_MAX);
	agi = atoi(fields[2]) ? cap_value(atoi(fields[2]),10,SHRT_MAX) : str;
	vit = atoi(fields[3]) ? cap_value(atoi(fields[3]),10,SHRT_MAX) : str;
	int_ = atoi(fields[4]) ? cap_value(atoi(fields[4]),10,SHRT_MAX) : str;
	dex = atoi(fields[5]) ? cap_value(atoi(fields[5]),10,SHRT_MAX) : str;
	luk = atoi(fields[6]) ? cap_value(atoi(fields[6]),10,SHRT_MAX) : str;

	job_info[idx].max_param.str = str;
	job_info[idx].max_param.agi = agi;
	job_info[idx].max_param.vit = vit;
	job_info[idx].max_param.int_ = int_;
	job_info[idx].max_param.dex = dex;
	job_info[idx].max_param.luk = luk;

	return true;
}

/**
 * Read job_noenter_map.txt
 */
static bool pc_readdb_job_noenter_map(char *str[], int columns, int current) {
	int idx, class_ = -1;

	if (ISDIGIT(str[0][0]))
		class_ = atoi(str[0]);
	else {
		if (!script_get_constant(str[0], &class_)) {
			ShowError("pc_readdb_job_noenter_map: Invalid job %s specified.\n", str[0]);
			return false;
		}
	}
	if (!pcdb_checkid(class_) || (idx = pc_class2idx(class_)) < 0) {
		ShowError("pc_readdb_job_noenter_map: Invalid job %s specified.\n", str[0]);
		return false;
	}
	job_info[idx].noenter_map.zone = atoi(str[1]);
	job_info[idx].noenter_map.group_lv = atoi(str[2]);

	return true;
}

static void pc_readdb_attendance_libconfig_sub(struct config_setting_t *t, const char *name, const char *source)
{
	struct config_setting_t *reward = NULL;
	int start = 0, end = 0;
	char aDay[5];

	nullpo_retv(t);
	nullpo_retv(name);
	nullpo_retv(source);

	if (!config_setting_lookup_int(t, "Start", &start)) {
		ShowWarning("pc_readdb_attendance_libconfig_sub: Missing 'Start' for entry '%s' in \"%s\".\n", name, source);
		return;
	}
	if (!config_setting_lookup_int(t, "End", &end)) {
		ShowWarning("pc_readdb_attendance_libconfig_sub: Missing 'End' for entry '%s' in \"%s\".\n", name, source);
		return;
	}
	if (end < date_get(DT_YYYYMMDD)) {
		ShowWarning("pc_readdb_attendance_libconfig_sub: Outdate period for entry '%s' in \"%s\".\n", name, source);
		return;
	}

	CREATE(attendance_periods, struct s_attendance_period, 1);
	attendance_periods->start = start;
	attendance_periods->end = end;

	if ((reward = config_setting_get_member(t, "Rewards")) && config_setting_is_group(reward)) {
		struct config_setting_t *n = NULL;
		bool duplicate[MAX_ATTENDANCE_DAY];
		int j = 0;

		memset(&duplicate, 0, sizeof(duplicate));

		while ((n = config_setting_get_elem(reward, j++)) && config_setting_is_group(n)) {
			char *nDay = config_setting_name(n);
			struct item_data *id = NULL;
			int day = 0, i32 = 0;
			uint16 at_itemid;
			uint16 at_amount;

			memset(&aDay, 0, sizeof(aDay));

			if (!strspn(&nDay[strlen(nDay) - 1], "0123456789") || (day = atoi(strncpy(aDay, nDay + 3, 4))) <= 0) {
				ShowWarning("pc_readdb_attendance_libconfig_sub: Invalid format '%s' for entry %s in \"%s\".\n", nDay, name, source);
				return;
			}
			if (day <= 0 || day > MAX_ATTENDANCE_DAY) {
				ShowWarning("pc_readdb_attendance_libconfig_sub: Out of range '%s' (maximum: Day%d) for entry %s in \"%s\".\n", nDay, MAX_ATTENDANCE_DAY, name, source);
				return;
			}
			day--;
			if (duplicate[day])
				ShowWarning("pc_readdb_attendance_libconfig_sub: Duplicate '%s' reward for entry %s in \"%s\", overwriting previous entry...\n", nDay, name, source);
			else
				duplicate[day] = true;
			if (!config_setting_lookup_int(n, "ItemId", &i32)) {
				ShowWarning("pc_readdb_attendance_libconfig_sub: No '%s' reward defined for entry %s in \"%s\".\n", nDay, name, source);
				return;
			} else {
				if (!(id = itemdb_exists(i32))) {
					ShowWarning("pc_readdb_attendance_libconfig_sub: '%s' has non-existance Item ID %d for entry %s in \"%s\".\n", nDay, i32, name, source);
					return;
				}
				at_itemid = id->nameid;
			}
			if (!config_setting_lookup_int(n, "Amount", &i32))
				at_amount = 1;
			else {
				if (i32 <= 0) {
					ShowWarning("pc_readdb_attendance_libconfig_sub: '%s' has invalid reward amount %d in for entry %s in \"%s\".\n", nDay, i32, name, source);
					return;
				}
				at_amount = i32;
				if (at_amount > MAX_AMOUNT) {
					ShowWarning("pc_readdb_attendance_libconfig_sub: '%s' has %d reward amount (maximum: %d) for entry %s in \"%s\".\n", nDay, i32, MAX_AMOUNT, name, source);
					at_amount = MAX_AMOUNT;
				}
			}
			if (!attendance_periods->rewards)
				CREATE(attendance_periods->rewards, struct s_attendance_reward, 1);
			else
				RECREATE(attendance_periods->rewards, struct s_attendance_reward, attendance_periods->reward_count + 1);
			attendance_periods->rewards[day].itemid = at_itemid;
			attendance_periods->rewards[day].amount = at_amount;
			attendance_periods->reward_count++;
		}
	}
}

static void pc_readdb_attendance_libconfig(const char *filename)
{
	struct config_t attendance_db_conf;
	struct config_setting_t *t = NULL;
	char filepath[256];
	int count = 0;

	safesnprintf(filepath, sizeof(filepath), "%s/%s", db_path, filename);
	if (conf_read_file(&attendance_db_conf, filepath))
		return;
	if ((t = config_setting_get_elem(attendance_db_conf.root, 0))) {
		char *name = config_setting_name(t);

		pc_readdb_attendance_libconfig_sub(t, name, filename);
		count++;
	}
	config_destroy(&attendance_db_conf);
	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s/%s"CL_RESET"'.\n", count, db_path, filename);
}

static void pc_readdb_stylist_libconfig_sub(struct config_setting_t *it, int16 count, const char *source)
{
	struct config_setting_t *human = NULL, *doram = NULL;
	int i32, idx, type = 0, style = 0, zeny = 0, itemID = 0, boxItemID = 0;
	const char *name;

	nullpo_retv(it);
	nullpo_retv(source);

	if (!config_setting_lookup_string(it, "Type", &name)) {
		ShowWarning("pc_readdb_stylist_libconfig_sub: Missing Type in \"%s\", entry #%d.\n", source, count);
		return;
	}
	if (!script_get_constant(name, &type)) {
		ShowWarning("pc_readdb_stylist_libconfig_sub: Invalid Type '%s' in \"%s\", entry #%d.\n", name, source, count);
		return;
	}
	if (type < 0 || type >= MAX_STYLIST_TYPE) {
		ShowWarning("pc_readdb_stylist_libconfig_sub: Out of range Type '%s' in \"%s\", entry #%d.\n", name, source, count);
		return;
	}
	if (!config_setting_lookup_int(it, "Style", &i32) || i32 < 0) {
		ShowWarning("pc_readdb_stylist_libconfig_sub: Missing or invalid Style %d in \"%s\", entry #%d.\n", i32, source, count);
		return;
	}
	style = i32;
	if (config_setting_lookup_int(it, "Index", &i32))
		idx = i32;
	else
		idx = style;
	RECREATE(stylist_datas[type], struct s_stylist_data, stylist_data_count + 1);
	memset(&stylist_datas[type][idx - 1].req, 0, sizeof(struct s_stylist_data_req) * 2);
	stylist_datas[type][idx - 1].id = style;
	if ((human = config_setting_get_member(it, "Human")) && config_setting_is_group(human)) {
		if (config_setting_lookup_int(human, "Zeny", &i32))
			zeny = i32;
		if (config_setting_lookup_int(human, "ItemID", &i32))
			itemID = i32;
		if (config_setting_lookup_int(human, "BoxItemID", &i32))
			boxItemID = i32;
		stylist_datas[type][idx - 1].req[0].zeny = zeny;
		stylist_datas[type][idx - 1].req[0].itemid = itemID;
		stylist_datas[type][idx - 1].req[0].boxid = boxItemID;
	}
	if ((doram = config_setting_get_member(it, "Doram")) && config_setting_is_group(doram)) {
		if (config_setting_lookup_int(doram, "Zeny", &i32))
			zeny = i32;
		if (config_setting_lookup_int(doram, "ItemID", &i32))
			itemID = i32;
		if (config_setting_lookup_int(doram, "BoxItemID", &i32))
			boxItemID = i32;
		stylist_datas[type][idx - 1].req[1].zeny = zeny;
		stylist_datas[type][idx - 1].req[1].itemid = itemID;
		stylist_datas[type][idx - 1].req[1].boxid = boxItemID;
	}
	stylist_data_count++;
}

static void pc_readdb_stylist_libconfig(const char *filename)
{
	struct config_setting_t *sdb = NULL, *t = NULL;
	char filepath[256];
	int count = 0;

	safesnprintf(filepath, sizeof(filepath), "%s/%s", db_path, filename);
	if (config_read_file(&stylist_db_conf, filepath))
		return;
	if (!(sdb = config_lookup(&stylist_db_conf, "stylist_db")))
		return;
	while ((t = config_setting_get_elem(sdb, count))) {
		pc_readdb_stylist_libconfig_sub(t, count, filename);
		count++;
	}
	config_destroy(&stylist_db_conf);
	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s/%s"CL_RESET"'.\n", count, db_path, filename);
}

/*==========================================
 * pc DB reading.
 * job_exp.txt		- required experience values
 * skill_tree.txt	- skill tree for every class
 * attr_fix.txt		- elemental adjustment table
 * job_db1.txt		- job,weight,hp_factor,hp_multiplicator,sp_factor,aspds/lvl
 * job_db2.txt		- job,stats bonuses/lvl
 * job_maxhpsp_db.txt	- strtlvl,maxlvl,job,type,values/lvl (values=hp|sp)
 *------------------------------------------*/
void pc_readdb(void)
{
	int i, j, k;
	unsigned int entries = 0;
	FILE *fp;
	char line[24000];

	//Reset
	memset(job_info, 0, sizeof(job_info)); //job_info table

#if defined(RENEWAL_DROP) || defined(RENEWAL_EXP)
	sv_readdb(db_path, "re/level_penalty.txt", ',', 4, 4, -1, &pc_readdb_levelpenalty);
	for( k = 1; k < 3; k++ ) { //Fill in the blanks
		for( j = 0; j < CLASS_ALL; j++ ) {
			int tmp = 0;

			for( i = 0; i < MAX_LEVEL * 2; i++ ) {
				if( i == MAX_LEVEL + 1 )
					tmp = level_penalty[k][j][0]; //Reset
				if( level_penalty[k][j][i] > 0 )
					tmp = level_penalty[k][j][i];
				else
					level_penalty[k][j][i] = tmp;
			}
		}
	}
#endif

	//Reset then read attr_fix.txt
	for( i = 0; i < MAX_ELE_LEVEL; i++ ) {
		for( j = 0; j < ELE_ALL; j++ )
			for( k = 0; k < ELE_ALL; k++ )
				attr_fix_table[i][j][k] = 100;
	}

	sprintf(line, "%s/"DBPATH"attr_fix.txt", db_path);
	fp = fopen(line, "r");
	if( fp == NULL ) {
		ShowError("Can't read %s\n", line);
		return;
	}
	while( fgets(line, sizeof(line), fp) ) {
		int lv;

		if( line[0] == '/' && line[1] == '/' )
			continue;
		lv = atoi(line);
		if( !CHK_ELEMENT_LEVEL(lv) )
			continue;
		for( i = 0; i < ELE_ALL; ) {
			char *p;

			if( !fgets(line, sizeof(line), fp) )
				break;
			if( line[0] == '/' && line[1] == '/' )
				continue;
			for( j = 0, p = line; j < ELE_ALL && p; j++ ) {
				while( *p > 0 && *p == 32 ) //Skipping newline and space (32 = ' ')
					p++;
				attr_fix_table[lv - 1][i][j] = atoi(p);
				p = strchr(p, ',');
				if( p )
					*p++ = 0;
			}

			i++;
		}
		entries++;
	}
	fclose(fp);
	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s/"DBPATH"attr_fix.txt"CL_RESET"'.\n", entries, db_path);

	 //Reset then read statspoint
	memset(statp,0,sizeof(statp));
	i = 1;

	sprintf(line, "%s/"DBPATH"statpoint.txt", db_path);
	fp = fopen(line, "r");
	if( fp == NULL ) {
		ShowWarning("Can't read '"CL_WHITE"%s"CL_RESET"'... Generating DB.\n", line);
		//return;
	} else {
		entries = 0;
		while( fgets(line, sizeof(line), fp) ) {
			int stat;

			if( line[0] == '/' && line[1] == '/' )
				continue;
			if( (stat = strtoul(line, NULL, 10)) < 0 )
				stat = 0;
			if( i > MAX_LEVEL )
				break;
			statp[i] = stat;
			i++;
			entries++;
		}
		fclose(fp);
		ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s/"DBPATH"statpoint.txt"CL_RESET"'.\n", entries, db_path);
	}
	//Generate the remaining parts of the db if necessary
	k = battle_config.use_statpoint_table; //Save setting
	battle_config.use_statpoint_table = 0; //Temporarily disable to force pc_gets_status_point use default values
	statp[0] = 45; //Seed value
	for( ; i <= MAX_LEVEL; i++ )
		statp[i] = statp[i - 1] + pc_gets_status_point(i - 1);
	battle_config.use_statpoint_table = k; //Restore setting

#ifdef RENEWAL_ASPD
	sv_readdb(db_path, "re/job_db1.txt", ',', 6 + MAX_WEAPON_TYPE, 6 + MAX_WEAPON_TYPE,CLASS_COUNT, &pc_readdb_job1);
#else
	sv_readdb(db_path, "pre-re/job_db1.txt" , ',', 5 + MAX_WEAPON_TYPE, 5 + MAX_WEAPON_TYPE,CLASS_COUNT, &pc_readdb_job1);
#endif
	sv_readdb(db_path, "job_db2.txt", ',', 1, 1 + MAX_LEVEL, CLASS_COUNT, &pc_readdb_job2);

	pc_readdb_job_exp_libconfig(DBPATH"job_exp.conf");

#ifdef HP_SP_TABLES
	sv_readdb(db_path, DBPATH"job_basehpsp_db.txt", ',', 4, 4 + 500, CLASS_COUNT * 2, &pc_readdb_job_basehpsp); //@TODO: Make it support until lvl 500!
#endif
	sv_readdb(db_path, DBPATH"job_param_db.txt", ',', 2, PARAM_MAX + 1, CLASS_COUNT, &pc_readdb_job_param);
	sv_readdb(db_path, DBPATH"job_noenter_map.txt", ',', 3, 3, CLASS_COUNT, &pc_readdb_job_noenter_map);

	//Reset and read skilltree (needs to be read after pc_readdb_job_exp_libconfig to get max base and job levels)
	memset(skill_tree, 0, sizeof(skill_tree));
	sv_readdb(db_path, DBPATH"skill_tree.txt", ',', 3 + MAX_PC_SKILL_REQUIRE * 2, 5 + MAX_PC_SKILL_REQUIRE * 2, -1, &pc_readdb_skilltree);

	//Checking if all class have their data
	for( i = 0; i < JOB_MAX; i++ ) {
		int idx;

		if( !pcdb_checkid(i) )
			continue;
		if( i == JOB_WEDDING || i == JOB_XMAS || i == JOB_SUMMER || i == JOB_HANBOK || i == JOB_OKTOBERFEST || i == JOB_SUMMER2 )
			continue; //Classes that do not need exp tables
		idx = pc_class2idx(i);
		if( !job_info[idx].max_level[0] )
			ShowWarning("Class %s (%d) does not have a base exp table.\n", job_name(i), i);
		if( !job_info[idx].max_level[1] )
			ShowWarning("Class %s (%d) does not have a job exp table.\n", job_name(i), i);
		//Init and checking the empty value of Base HP/SP [Cydh]
		for( j = 0; j < (job_info[idx].max_level[0] ? job_info[idx].max_level[0] : MAX_LEVEL); j++ ) {
			if( job_info[idx].base_hp[j] == 0 )
				job_info[idx].base_hp[j] = pc_calc_basehp(j + 1, i);
			if( job_info[idx].base_sp[j] == 0 )
				job_info[idx].base_sp[j] = pc_calc_basesp(j + 1, i);
		}
	}

	if( battle_config.feature_attendance )
		pc_readdb_attendance_libconfig(DBPATH"attendance_db.conf");

	if( battle_config.feature_stylistui )
		pc_readdb_stylist_libconfig("stylist_db.conf");
}

// Read MOTD on startup. [Valaris]
int pc_read_motd(void)
{
	FILE *fp;

	//Clear old MOTD
	memset(motd_text, 0, sizeof(motd_text));

	//Read current MOTD
	if( (fp = fopen(motd_txt, "r")) != NULL ) {
		unsigned int entries = 0;
		char buf[CHAT_SIZE_MAX];

		while( entries < MOTD_LINE_SIZE && fgets(buf, CHAT_SIZE_MAX, fp) ) {
			unsigned int lines = 0;
			size_t len;

			lines++;
			if( buf[0] == '/' && buf[1] == '/' )
				continue;
			len = strlen(buf);
			while( len && (buf[len-1] == '\r' || buf[len - 1] == '\n') ) //Strip trailing EOL characters
				len--;
			if( len ) {
				char *ptr;

				buf[len] = 0;
				if( (ptr = strstr(buf, " :") ) != NULL && ptr - buf >= NAME_LENGTH) //Crashes newer clients
					ShowWarning("Found sequence '"CL_WHITE" :"CL_RESET"' on line '"CL_WHITE"%u"CL_RESET"' in '"CL_WHITE"%s"CL_RESET"'. This can cause newer clients to crash.\n", lines, motd_txt);
			} else { //Empty line
				buf[0] = ' ';
				buf[1] = 0;
			}
			safestrncpy(motd_text[entries], buf, CHAT_SIZE_MAX);
			entries++;
		}
		fclose(fp);
		ShowStatus("Done reading '"CL_WHITE"%u"CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n", entries, motd_txt);
	} else
		ShowWarning("File '"CL_WHITE"%s"CL_RESET"' not found.\n", motd_txt);

	return 0;
}

void pc_itemcd_do(struct map_session_data *sd, bool load) {
	int i, cursor = 0;
	struct item_cd *cd = NULL;

	if( load ) {
		if( !(cd = idb_get(itemcd_db, sd->status.char_id)) )
			return; //No item cooldown is associated with this character
		for( i = 0; i < MAX_ITEMDELAYS; i++ ) {
			if( cd->nameid[i] && DIFF_TICK(gettick(),cd->tick[i]) < 0 ) {
				sd->item_delay[cursor].tick = cd->tick[i];
				sd->item_delay[cursor].nameid = cd->nameid[i];
				cursor++;
			}
		}
		idb_remove(itemcd_db,sd->status.char_id);
	} else {
		if( !(cd = idb_get(itemcd_db,sd->status.char_id)) ) { //Create a new skill cooldown object for map storage
			CREATE(cd, struct item_cd, 1);
			idb_put(itemcd_db, sd->status.char_id, cd);
		}
		for( i = 0; i < MAX_ITEMDELAYS; i++ ) {
			if( sd->item_delay[i].nameid && DIFF_TICK(gettick(),sd->item_delay[i].tick) < 0 ) {
				cd->tick[cursor] = sd->item_delay[i].tick;
				cd->nameid[cursor] = sd->item_delay[i].nameid;
				cursor++;
			}
		}
	}
}

/**
 * Add item delay to player's item delay data
 * @param sd Player
 * @param id Item data
 * @param tick Current tick
 * @param n Item index in inventory
 * @return 0: No delay, can consume item.
 *         1: Has delay, cancel consumption.
 */
uint8 pc_itemcd_add(struct map_session_data *sd, struct item_data *id, unsigned int tick, unsigned short n) {
	int i;

	ARR_FIND(0, MAX_ITEMDELAYS, i, sd->item_delay[i].nameid == id->nameid );
	if( i == MAX_ITEMDELAYS ) //Item not found. try first empty now
		ARR_FIND(0, MAX_ITEMDELAYS, i, !sd->item_delay[i].nameid);
	if( i < MAX_ITEMDELAYS ) {
		if( sd->item_delay[i].nameid ) { //Found
			if( DIFF_TICK(sd->item_delay[i].tick, tick) > 0 ) {
				int e_tick = DIFF_TICK(sd->item_delay[i].tick, tick) / 1000;
				char e_msg[100];

				if( e_tick > 99 )
					sprintf(e_msg ,msg_txt(sd, 379), (double)e_tick / 60); // Able to use %.1f min later.
				else
					sprintf(e_msg, msg_txt(sd, 380), e_tick + 1); // Able to use %d sec later.
				clif_messagecolor(&sd->bl,color_table[COLOR_YELLOW],e_msg,false,SELF);
				return 1; //Delay has not expired yet
			}
		} else //Not yet used item (all slots are initially empty)
			sd->item_delay[i].nameid = id->nameid;
		if( !(id->nameid == ITEMID_BOARDING_HALTER && sd->sc.option&(OPTION_WUGRIDER|OPTION_RIDING|OPTION_DRAGON|OPTION_MADOGEAR)) )
			sd->item_delay[i].tick = tick + id->delay;
	} else //Should not happen
		ShowError("pc_itemcd_add: Exceeded item delay array capacity! (nameid=%hu, char_id=%d)\n", id->nameid, sd->status.char_id);
	//Clean up used delays so we can give room for more
	for( i = 0; i < MAX_ITEMDELAYS; i++ ) {
		if( DIFF_TICK(sd->item_delay[i].tick, tick) <= 0 ) {
			sd->item_delay[i].tick = 0;
			sd->item_delay[i].nameid = 0;
		}
	}
	return 0;
}

/**
 * Check if player has delay to reuse item
 * @param sd Player
 * @param id Item data
 * @param tick Current tick
 * @param n Item index in inventory
 * @return 0: No delay, can consume item.
 *         1: Has delay, cancel consumption.
 */
uint8 pc_itemcd_check(struct map_session_data *sd, struct item_data *id, unsigned int tick, unsigned short n) {
	struct status_change *sc = NULL;

	nullpo_retr(0, sd);
	nullpo_retr(0, id);

	//Do normal delay assignment
	if( id->delay_sc <= SC_NONE || id->delay_sc >= SC_MAX || !(sc = &sd->sc) )
		return pc_itemcd_add(sd, id, tick, n);

	//Send reply of delay remains
	if( sc->data[id->delay_sc] ) {
		const struct TimerData *timer = get_timer(sc->data[id->delay_sc]->timer);

		clif_msg_value(sd, ITEM_REUSE_LIMIT, (timer ? DIFF_TICK(timer->tick, tick) / 1000 : 99));
		return 1;
	}

	sc_start(&sd->bl, &sd->bl, (sc_type)id->delay_sc, 100, id->nameid, id->delay);
	return 0;
}

/**
 * Clear the dmglog data from player
 * @param sd
 * @param md
 */
static void pc_clear_log_damage_sub(int char_id, struct mob_data *md) {
	uint8 i = 0;

	ARR_FIND(0, DAMAGELOG_SIZE, i, md->dmglog[i].id == char_id);
	if( i < DAMAGELOG_SIZE ) {
		md->dmglog[i].id = 0;
		md->dmglog[i].dmg = 0;
		md->dmglog[i].flag = 0;
	}
}

/**
 * Add log to player's dmglog
 * @param sd
 * @param id Monster's GID
 */
void pc_damage_log_add(struct map_session_data *sd, int id) {
	uint8 i = 0;

	if( !sd || !id )
		return;
	//Only store new data, don't need to renew the old one with same id
	ARR_FIND(0, DAMAGELOG_SIZE, i, sd->dmglog[i] == id);
	if( i < DAMAGELOG_SIZE_PC )
		return;
	for( i = 0; i < DAMAGELOG_SIZE_PC; i++ ) {
		if( sd->dmglog[i] == 0 ) {
			sd->dmglog[i] = id;
			return;
		}
	}
}

/**
 * Clear dmglog data from player
 * @param sd
 * @param id Monster's id
 */
void pc_damage_log_clear(struct map_session_data *sd, int id) {
	struct mob_data *md = NULL;
	uint8 i = 0;

	if( !sd )
		return;
	if( !id ) {
		for( i = 0; i < DAMAGELOG_SIZE_PC; i++ ) {
			if( !sd->dmglog[i] ) //Skip the empty value
				continue;
			if( (md = map_id2md(sd->dmglog[i])) )
				pc_clear_log_damage_sub(sd->status.char_id, md);
			sd->dmglog[i] = 0;
		}
	} else {
		if( (md = map_id2md(id)) )
			pc_clear_log_damage_sub(sd->status.char_id,md);
		ARR_FIND(0, DAMAGELOG_SIZE_PC, i, sd->dmglog[i] == id); //Find the id position
		if( i < DAMAGELOG_SIZE_PC )
			sd->dmglog[i] = 0;
	}
}

/**
 * Status change data arrived from char-server
 * @param sd: Player data
 */
void pc_scdata_received(struct map_session_data *sd) {
	pc_inventory_rentals(sd); //Needed here to remove rentals that have status changes after chrif_load_scdata has finished

	if( pc_has_permission(sd, PC_PERM_ATTENDANCE) && pc_attendance_enabled() && !pc_attendance_rewarded_today(sd) )
		clif_ui_open(sd, OUT_UI_ATTENDANCE, pc_attendance_counter(sd));

	clif_weight_limit(sd);
	sd->state.pc_loaded = true;

	if( !sd->state.connect_new && sd->fd ) { //Character already loaded map! Gotta trigger LoadEndAck manually
		sd->state.connect_new = 1;
		clif_parse_LoadEndAck(sd->fd, sd);
	}

	if( pc_iscarton(sd) ) {
		sd->cart_weight_max = 0; //Force a client refesh
		status_calc_cart_weight(sd, CALCWT_ITEM|CALCWT_MAXBONUS|CALCWT_CARTSTATE);
	}
}

/**
 * Check player account expiration time and rental item expirations
 * @param sd: Player data
 */
void pc_check_expiration(struct map_session_data *sd) {
#ifndef ENABLE_SC_SAVING
	pc_inventory_rentals(sd); //Check here if status change saving is disabled
#endif

	if( sd->expiration_time ) { //Don't display if it's unlimited or unknow value
		const time_t exp_time = sd->expiration_time;
		struct tm now;
		char tmpstr[1024];

		localtime_r(&exp_time, &now);
		strftime(tmpstr,sizeof(tmpstr) - 1,msg_txt(sd,501),&now); // "Your account time limit is: %d-%m-%Y %H:%M:%S."
		clif_wis_message(sd->fd,wisp_server_name,tmpstr,strlen(tmpstr));

		pc_expire_check(sd);
	}
}

TIMER_FUNC(pc_expiration_timer) {
	struct map_session_data *sd = map_id2sd(id);

	if( !sd )
		return 0;

	sd->expiration_tid = INVALID_TIMER;

	if( sd->fd )
		clif_authfail_fd(sd->fd,10);

	map_quit(sd);

	return 0;
}

TIMER_FUNC(pc_autotrade_timer) {
	struct map_session_data *sd = map_id2sd(id);

	if( !sd )
		return 0;

	sd->autotrade_tid = INVALID_TIMER;

	if( sd->state.autotrade&2 )
		vending_reopen(sd);

	if( sd->state.autotrade&4 )
		buyingstore_reopen(sd);

	if( !sd->vender_id && !sd->buyer_id ) {
		sd->state.autotrade = 0;
		map_quit(sd);
	}

	return 0;
}

/* This timer exists only when a character with a expire timer > 24h is online */
/* It loops thru online players once an hour to check whether a new < 24h is available */
TIMER_FUNC(pc_global_expiration_timer) {
	struct s_mapiterator *iter;
	struct map_session_data *sd;

	iter = mapit_getallusers();

	for( sd = (TBL_PC *)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC *)mapit_next(iter) )
		if( sd->expiration_time )
			pc_expire_check(sd);

	mapit_free(iter);

  return 0;
}

void pc_expire_check(struct map_session_data *sd) {
	if( sd->expiration_tid != INVALID_TIMER ) //Ongoing timer
		return;

	//Not within the next 24h, enable the global check
	if( sd->expiration_time > (time(NULL) + ((60 * 60) * 24)) ) {
		//Global check not running, enable
		if( pc_expiration_tid == INVALID_TIMER ) //Starts in 1h, repeats every hour
			pc_expiration_tid = add_timer_interval(gettick() + ((1000 * 60) * 60), pc_global_expiration_timer, 0, 0, ((1000 * 60) * 60));

		return;
	}

	sd->expiration_tid = add_timer(gettick() + (unsigned int)(sd->expiration_time - time(NULL)) * 1000, pc_expiration_timer, sd->bl.id, 0);
}

/**
 * Deposit some money to bank
 * @param sd: Player
 * @param money Amount of money to deposit
 */
enum e_BANKING_DEPOSIT_ACK pc_bank_deposit(struct map_session_data *sd, int money) {
	unsigned int limit_check = money + sd->bank_vault;

	if( money <= 0 || limit_check > MAX_BANK_ZENY )
		return BDA_OVERFLOW;
	else if( money > sd->status.zeny )
		return BDA_NO_MONEY;
	if( pc_payzeny(sd,money,LOG_TYPE_BANK,NULL) )
		return BDA_NO_MONEY;
	sd->bank_vault += money;
	pc_setreg2(sd,BANK_VAULT_VAR,sd->bank_vault);
	if( save_settings&CHARSAVE_BANK )
		chrif_save(sd,CSAVE_NORMAL);
	return BDA_SUCCESS;
}

/**
 * Withdraw money from bank
 * @param sd: Player
 * @param money Amount of money that will be withdrawn
 */
enum e_BANKING_WITHDRAW_ACK pc_bank_withdraw(struct map_session_data *sd, int money) {
	unsigned int limit_check = money + sd->status.zeny;

	if( money <= 0 )
		return BWA_UNKNOWN_ERROR;
	else if( money > sd->bank_vault )
		return BWA_NO_MONEY;
	else if( limit_check > MAX_ZENY ) {
		//No official response for this scenario exists
		clif_messagecolor(&sd->bl,color_table[COLOR_RED],msg_txt(sd,1509),false,SELF); // You can't withdraw that much money.
		return BWA_UNKNOWN_ERROR;
	}
	if( pc_getzeny(sd,money,LOG_TYPE_BANK,NULL) )
		return BWA_NO_MONEY;
	sd->bank_vault -= money;
	pc_setreg2(sd,BANK_VAULT_VAR,sd->bank_vault);
	if( save_settings&CHARSAVE_BANK )
		chrif_save(sd,CSAVE_NORMAL);
	return BWA_SUCCESS;
}

/**
 * Show version to player
 * @param sd: Player
 */
void pc_show_version(struct map_session_data *sd) {
	const char *git = get_git_hash();
	char buf[CHAT_SIZE_MAX];

	if( git[0] != UNKNOWN_VERSION ) {
		sprintf(buf,msg_txt(sd,1295),"GIT Hash: ",git); // idAthena Git Hash: %s
		clif_displaymessage(sd->fd,buf);
	} else
		clif_displaymessage(sd->fd,msg_txt(sd,1296)); // Can't find current Git version.
}

/**
 * Clear Crimson Marks data from caster
 * @param sd: Player
 */
void pc_crimson_marks_clear(struct map_session_data *sd) {
	uint8 i;

	nullpo_retv(sd);

	for( i = 0; i < MAX_CRIMSON_MARKS; i++ ) {
		if( sd->crimson_mark[i] ) {
			struct block_list *bl = map_id2bl(sd->crimson_mark[i]);

			status_change_end(bl,SC_C_MARKER,INVALID_TIMER);
		}
	}
}

/**
 * Clear Soul Unity data from caster
 * @param sd: Player
 */
void pc_united_souls_clear(struct map_session_data *sd) {
	uint8 i;

	nullpo_retv(sd);

	for( i = 0; i < MAX_UNITED_SOULS; i++ ) {
		if( sd->united_soul[i] ) {
			struct block_list *bl = map_id2bl(sd->united_soul[i]);

			status_change_end(bl,SC_SOULUNITY,INVALID_TIMER);
		}
	}
}

/**
 * Run bonus_script on player
 * @param sd
 * @author [Cydh]
 */
void pc_bonus_script(struct map_session_data *sd) {
	int now = gettick();
	struct linkdb_node *node = NULL, *next = NULL;

	if (!sd || !(node = sd->bonus_script.head))
		return;
	while (node) {
		struct s_bonus_script_entry *entry = NULL;

		next = node->next;
		if ((entry = (struct s_bonus_script_entry *)node->data)) {
			if (entry->tid == INVALID_TIMER) { //Only start timer for new bonus_script
				if (entry->icon != SI_BLANK) //Gives status icon if exist
					clif_status_change(&sd->bl,entry->icon,1,entry->tick,1,0,0);
				entry->tick += now;
				entry->tid = add_timer(entry->tick,pc_bonus_script_timer,sd->bl.id,(intptr_t)entry);
			}
			if (entry->script)
				run_script(entry->script,0,sd->bl.id,0);
			else
				ShowError("pc_bonus_script: The script has been removed somewhere. \"%s\"\n",StringBuf_Value(entry->script_buf));
		}
		node = next;
	}
}

/**
 * Add bonus_script to player
 * @param sd Player
 * @param script_str Script string
 * @param dur Duration in ms
 * @param icon SI
 * @param flag Flags @see enum e_bonus_script_flags
 * @param type 0 - None, 1 - Buff, 2 - Debuff
 * @return New created entry pointer or NULL if failed or NULL if duplicate fail
 * @author [Cydh]
 */
struct s_bonus_script_entry *pc_bonus_script_add(struct map_session_data *sd, const char *script_str, uint32 dur, enum si_type icon, uint16 flag, uint8 type) {
	struct script_code *script = NULL;
	struct linkdb_node *node = NULL;
	struct s_bonus_script_entry *entry = NULL;

	if (!sd)
		return NULL;
	if (!(script = parse_script(script_str,"bonus_script",0,SCRIPT_IGNORE_EXTERNAL_BRACKETS))) {
		ShowError("pc_bonus_script_add: Failed to parse script '%s' (CID:%d).\n",script_str,sd->status.char_id);
		return NULL;
	}
	if ((node = sd->bonus_script.head)) { //Duplication checks
		while (node) {
			entry = (struct s_bonus_script_entry *)node->data;
			if (strcmpi(script_str,StringBuf_Value(entry->script_buf)) == 0) {
				int newdur = gettick() + dur;

				if (flag&BSF_FORCE_REPLACE && entry->tick < newdur) { //Change duration
					settick_timer(entry->tid,newdur);
					script_free_code(script);
					return NULL;
				} else if (flag&BSF_FORCE_DUPLICATE) //Allow duplicate
					break;
				else { //No duplicate bonus
					script_free_code(script);
					return NULL;
				}
			}
			node = node->next;
		}
	}
	CREATE(entry,struct s_bonus_script_entry,1);
	entry->script_buf = StringBuf_Malloc();
	StringBuf_AppendStr(entry->script_buf,script_str);
	entry->tid = INVALID_TIMER;
	entry->flag = flag;
	entry->icon = icon;
	entry->tick = dur; //Use duration first, on run change to expire time
	entry->type = type;
	entry->script = script;
	sd->bonus_script.count++;
	return entry;
}

/**
 * Remove bonus_script data from player
 * @param sd: Target player
 * @param list: Bonus script entry from player
 * @author [Cydh]
 */
void pc_bonus_script_free_entry(struct map_session_data *sd, struct s_bonus_script_entry *entry) {
	if (entry->tid != INVALID_TIMER)
		delete_timer(entry->tid,pc_bonus_script_timer);
	if (entry->script)
		script_free_code(entry->script);
	if (entry->script_buf)
		StringBuf_Free(entry->script_buf);
	if (sd) {
		if (entry->icon != SI_BLANK)
			clif_status_load(&sd->bl,entry->icon,0);
		if (sd->bonus_script.count > 0)
			sd->bonus_script.count--;
	}
	aFree(entry);
}

/**
 * Do final process if no entry left
 * @param sd
 */
static inline void pc_bonus_script_check_final(struct map_session_data *sd) {
	if (sd->bonus_script.count == 0) {
		if (sd->bonus_script.head && sd->bonus_script.head->data)
			pc_bonus_script_free_entry(sd,(struct s_bonus_script_entry *)sd->bonus_script.head->data);
		linkdb_final(&sd->bonus_script.head);
	}
}

/**
 * Timer for bonus_script
 * @param tid
 * @param tick
 * @param id
 * @param data
 * @author [Cydh]
 */
TIMER_FUNC(pc_bonus_script_timer) {
	struct map_session_data *sd;
	struct s_bonus_script_entry *entry = (struct s_bonus_script_entry *)data;

	sd = map_id2sd(id);
	if (!sd) {
		ShowError("pc_bonus_script_timer: Null pointer id: %d tid: %d\n",id,tid);
		return 0;
	}
	if (tid == INVALID_TIMER)
		return 0;
	if (!sd->bonus_script.head || entry == NULL) {
		ShowError("pc_bonus_script_timer: Invalid entry pointer %p!\n",entry);
		return 0;
	}
	linkdb_erase(&sd->bonus_script.head,(void *)((intptr_t)entry));
	pc_bonus_script_free_entry(sd,entry);
	pc_bonus_script_check_final(sd);
	status_calc_pc(sd,SCO_NONE);
	return 0;
}

/**
 * Check then clear all active timer(s) of bonus_script data from player based on reason
 * @param sd: Target player
 * @param flag: Reason to remove the bonus_script. e_bonus_script_flags or e_bonus_script_types
 * @author [Cydh]
 */
void pc_bonus_script_clear(struct map_session_data *sd, uint16 flag) {
	struct linkdb_node *node = NULL;
	uint16 count = 0;

	if (!sd || !(node = sd->bonus_script.head))
		return;
	while (node) {
		struct linkdb_node *next = node->next;
		struct s_bonus_script_entry *entry = (struct s_bonus_script_entry *)node->data;

		if (entry &&
				((flag == BSF_PERMANENT) ||                  //Remove all with permanent bonus
				(!flag && !(entry->flag&BSF_PERMANENT)) ||   //Remove all WITHOUT permanent bonus
				(flag&entry->flag) ||                        //Matched flag
				(flag&BSF_REM_BUFF   && entry->type == 1) || //Remove buff
				(flag&BSF_REM_DEBUFF && entry->type == 2)))  //Remove debuff
		{
			linkdb_erase(&sd->bonus_script.head,(void *)((intptr_t)entry));
			pc_bonus_script_free_entry(sd,entry);
			count++;
		}
		node = next;
	}
	pc_bonus_script_check_final(sd);
	if (count && !(flag&BSF_REM_ON_LOGOUT)) //Don't need to do this if log out
		status_calc_pc(sd,SCO_NONE);
}

/** [Cydh]
 * Gives/removes SC_BASILICA when player steps in/out the cell with 'cell_basilica'
 * @param sd: Target player
 */
void pc_cell_basilica(struct map_session_data *sd) {
	nullpo_retv(sd);

	if (!map_getcell(sd->bl.m,sd->bl.x,sd->bl.y,CELL_CHKBASILICA)) {
		if (sd->sc.data[SC_BASILICA])
			status_change_end(&sd->bl,SC_BASILICA,INVALID_TIMER);
	} else if (!sd->sc.data[SC_BASILICA])
		sc_start(&sd->bl,&sd->bl,SC_BASILICA,100,0,-1);
}

/** [Cydh]
 * Get maximum specified parameter for specified class
 * @param class_: sd->class
 * @param sex: sd->status.sex
 * @param flag: parameter will be checked
 * @return max_param
 */
short pc_maxparameter(struct map_session_data *sd, enum e_params param) {
	int idx = -1, class_ = sd->class_;

	if ((idx = pc_class2idx(pc_mapid2jobid(class_,sd->status.sex))) >= 0) {
		short max_param = 0;

		switch (param) {
			case PARAM_STR: max_param = job_info[idx].max_param.str; break;
			case PARAM_AGI: max_param = job_info[idx].max_param.agi; break;
			case PARAM_VIT: max_param = job_info[idx].max_param.vit; break;
			case PARAM_INT: max_param = job_info[idx].max_param.int_; break;
			case PARAM_DEX: max_param = job_info[idx].max_param.dex; break;
			case PARAM_LUK: max_param = job_info[idx].max_param.luk; break;
		}
		if (max_param > 0)
			return max_param;
	}

	return ((class_&MAPID_UPPERMASK) == MAPID_KAGEROUOBORO || (class_&MAPID_UPPERMASK) == MAPID_REBELLION ||
		(class_&MAPID_BASEMASK) == MAPID_SUMMONER || (class_&JOBL_THIRD) ?
		((class_&JOBL_BABY) ? battle_config.max_baby_parameter_renewal_jobs : battle_config.max_parameter_renewal_jobs) :
		((class_&JOBL_BABY) ? battle_config.max_baby_parameter : battle_config.max_parameter));
}

/**
 * Get max ASPD for player based on Class
 * @param sd Player
 * @return ASPD
 */
short pc_maxaspd(struct map_session_data *sd) {
	nullpo_ret(sd);

	return ((sd->class_&JOBL_THIRD) ? battle_config.max_third_aspd :
			(((sd->class_&MAPID_UPPERMASK) == MAPID_KAGEROUOBORO || (sd->class_&MAPID_UPPERMASK) == MAPID_REBELLION) ? battle_config.max_extended_aspd :
			((sd->class_&MAPID_BASEMASK) == MAPID_SUMMONER ? battle_config.max_summoner_aspd : battle_config.max_aspd)));
}

/**
 * Calculates total item-group related bonuses for the given item
 * @param sd Player
 * @param nameid Item ID
 * @return Heal rate
 */
short pc_get_itemgroup_bonus(struct map_session_data *sd, unsigned short nameid) {
	short bonus = 0;
	uint8 i;

	for (i = 0; i < MAX_PC_BONUS && sd->itemgrouphealrate[i].val; i++) {
		uint16 group_id = sd->itemgrouphealrate[i].id, j, max;
		struct s_item_group_db *group = NULL;

		if (!group_id || !(group = itemdb_group_exists(group_id)))
			continue;
		max = group->random[0].data_qty;
		ARR_FIND(0, max, j, group->random[0].data[j].nameid == nameid);
		if (j < max)
			bonus += sd->itemgrouphealrate[i].val;
	}

	return bonus;
}

/**
 * Calculates total item-group related bonuses for the given item group
 * @param sd Player
 * @param group_id Item Group ID
 * @return Heal rate
 */
short pc_get_itemgroup_bonus_group(struct map_session_data *sd, uint16 group_id) {
	uint8 i;

	ARR_FIND(0, MAX_PC_BONUS, i, sd->itemgrouphealrate[i].id == group_id);
	if (i < MAX_PC_BONUS)
		return sd->itemgrouphealrate[i].val;

	return 0;
}

/**
 * Check if player's equip index in same specified position, like for 2-Handed weapon & Heagdear (inc. costume)
 * @param eqi Item EQI of enum equip_index
 * @param *equip_index Player's equip_index[]
 * @param index Known index item in inventory from sd->equip_index[] to compare with specified EQI in *equip_index
 * @return True if item in same inventory index, False if doesn't
 */
bool pc_is_same_equip_index(enum equip_index eqi, short *equip_index, short index) {
	if (index < 0 || index >= MAX_INVENTORY)
		return true;
	if (eqi == EQI_HAND_R && equip_index[EQI_HAND_L] == index)
		return true; //Dual weapon checks
	if (eqi == EQI_HEAD_MID && equip_index[EQI_HEAD_LOW] == index)
		return true; //Headgear with Mid & Low location
	if (eqi == EQI_HEAD_TOP && (equip_index[EQI_HEAD_MID] == index || equip_index[EQI_HEAD_LOW] == index))
		return true; //Headgear with Top & Mid or Low location
	if (eqi == EQI_COSTUME_HEAD_MID && equip_index[EQI_COSTUME_HEAD_LOW] == index)
		return true; //Headgear with Mid & Low location
	if (eqi == EQI_COSTUME_HEAD_TOP && (equip_index[EQI_COSTUME_HEAD_MID] == index || equip_index[EQI_COSTUME_HEAD_LOW] == index))
		return true; //Headgear with Top & Mid or Low location

	return false;
}

/**
 * Generate Unique item ID for player
 * @param sd : Player
 * @return A generated Unique item ID
 */
uint64 pc_generate_unique_id(struct map_session_data *sd) {
	nullpo_ret(sd);

	return ((uint64)sd->status.char_id<<32)|sd->status.uniqueitem_counter++;
}

/**
 * Toggle to remember if the questinfo is displayed yet or not.
 * @param qi_display Display flag
 * @param show If show is true and qi_display is 0, set qi_display to 1 and show the event bubble.
 *             If show is false and qi_display is 1, set qi_display to 0 and hide the event bubble.
 */
static void pc_show_questinfo_sub(struct map_session_data *sd, bool *qi_display, struct questinfo *qi, bool show) {
	if (show) { //Check if need to be displayed
		if ((*qi_display) != 1) {
			(*qi_display) = 1;
			clif_quest_show_event(sd, &qi->nd->bl, qi->icon, qi->color);
		}
	} else { //Check if need to be hide
		if ((*qi_display) != 0) {
			(*qi_display) = 0;
#if PACKETVER >= 20120410
			clif_quest_show_event(sd, &qi->nd->bl, 9999, 0);
#else
			clif_quest_show_event(sd, &qi->nd->bl, 0, 0);
#endif
		}
	}
}

/**
 * Show available NPC Quest / Event Icon Check [Kisuka]
 * @param sd Player
 */
void pc_show_questinfo(struct map_session_data *sd) {
#if PACKETVER >= 20090218
	struct questinfo *qi = NULL;
	unsigned short i;
	uint8 j;
	int8 mystate = 0;
	bool failed = false;

	nullpo_retv(sd);

	if (sd->bl.m < 0 || sd->bl.m >= MAX_MAPINDEX)
		return;

	if (!mapdata[sd->bl.m].qi_count || !mapdata[sd->bl.m].qi_data)
		return;

	if (mapdata[sd->bl.m].qi_count != sd->qi_count)
		return; //Init was not called yet

	for (i = 0; i < mapdata[sd->bl.m].qi_count; i++) {
		qi = &mapdata[sd->bl.m].qi_data[i];
		if (!qi)
			continue;
		if (quest_check(sd, qi->quest_id, HAVEQUEST) != -1) { //Check if quest is not started
			pc_show_questinfo_sub(sd, &sd->qi_display[i], qi, false);
			continue;
		}
		if (sd->status.base_level < qi->min_level || sd->status.base_level > qi->max_level) { //Level range checks
			pc_show_questinfo_sub(sd, &sd->qi_display[i], qi, false);
			continue;
		}
		if (qi->req_count) { //Quest requirements
			failed = false;
			for (j = 0; j < qi->req_count; j++) {
				mystate = quest_check(sd, qi->req[j].quest_id, HAVEQUEST);
				mystate = mystate + (mystate < 1);
				if (mystate != qi->req[j].state) {
					failed = true;
					break;
				}
			}
			if (failed) {
				pc_show_questinfo_sub(sd, &sd->qi_display[i], qi, false);
				continue;
			}
		}
		if (qi->jobid_count) { //Job requirements
			failed = true;
			for (j = 0; j < qi->jobid_count; j++) {
				if (pc_mapid2jobid(sd->class_,sd->status.sex) == qi->jobid[j]) {
					pc_show_questinfo_sub(sd, &sd->qi_display[i], qi, true);
					failed = false;
					break;
				}
			}
			if (!failed)
				continue;
			pc_show_questinfo_sub(sd, &sd->qi_display[i], qi, false);
		} else
			pc_show_questinfo_sub(sd, &sd->qi_display[i], qi, true);
	}
#endif
}

/**
 * Reinit the questinfo for player when changing map
 * @param sd Player
 */
void pc_show_questinfo_reinit(struct map_session_data *sd) {
#if PACKETVER >= 20090218
	nullpo_retv(sd);

	if (sd->qi_display) {
		aFree(sd->qi_display);
		sd->qi_display = NULL;
	}

	sd->qi_count = 0;

	if (sd->bl.m < 0 || sd->bl.m >= MAX_MAPINDEX)
		return;

	if (!mapdata[sd->bl.m].qi_count || !mapdata[sd->bl.m].qi_data)
		return;

	CREATE(sd->qi_display, bool, (sd->qi_count = mapdata[sd->bl.m].qi_count));
#endif
}

/**
 * Verifies a chat message, searching for atcommands, checking if the sender
 * character can chat, and updating the idle timer.
 *
 * @param sd      The sender character.
 * @param message The message text.
 * @return Whether the message is a valid chat message.
 */
bool pc_process_chat_message(struct map_session_data *sd, const char *message) {
	if (is_atcommand(sd->fd, sd, message, 1))
		return false;

	if (sd->sc.cant.chat)
		return false; //No "chatting" while muted

	if (battle_config.min_chat_delay) { //[Skotlex]
		if (DIFF_TICK(sd->cantalk_tick, gettick()) > 0)
			return false;
		sd->cantalk_tick = gettick() + battle_config.min_chat_delay;
	}

	sd->idletime = last_tick; //Reset idle time when using chat

	return true;
}

/**
 * Checks a chat message, scanning for the Super Novice prayer sequence.
 *
 * If a match is found, the angel is invoked or the counter is incremented as
 * appropriate.
 *
 * @param sd      The sender character.
 * @param message The message text.
 */
void pc_check_supernovice_call(struct map_session_data *sd, const char *message) {
	double nextb = pc_nextbaseexp(sd);
	int percent = 0;

	if ((sd->class_&MAPID_UPPERMASK) != MAPID_SUPER_NOVICE)
		return;

	if (!nextb)
		return;

	//0%, 10%, 20%, ...
	percent = (int)((double)sd->status.base_exp * 1000. / nextb);
	if ((battle_config.snovice_call_type || percent) && !(percent%100)) { //10.0%, 20.0%, ..., 90.0%
		switch (sd->state.snovice_call_flag) {
			case 0:
				if (strstr(message, msg_txt(NULL, 1481))) // "Dear angel, can you hear my voice?"
					sd->state.snovice_call_flag = 1;
				break;
			case 1: {
					char buf[256];

					snprintf(buf, 256, msg_txt(NULL, 1482), sd->status.name);
					if (strstr(message, buf)) // "I am %s Super Novice~"
						sd->state.snovice_call_flag = 2;
				}
				break;
			case 2:
				if (strstr(message, msg_txt(NULL, 1483))) // "Help me out~ Please~ T_T"
					sd->state.snovice_call_flag = 3;
				break;
			case 3:
				sc_start(&sd->bl, &sd->bl, status_skill2sc(MO_EXPLOSIONSPIRITS), 100, 17, skill_get_time(MO_EXPLOSIONSPIRITS, 5)); //Lv17-> +50 critical (noted by Poki) [Skotlex]
				clif_skill_nodamage(&sd->bl, &sd->bl, MO_EXPLOSIONSPIRITS, 5, 1); //Prayer always shows successful Lv5 cast and disregards noskill restrictions
				sd->state.snovice_call_flag = 0;
				break;
		}
	}
}

/**
 * Check if a job is allowed to enter the map
 * @param jobid Job ID see enum e_job or sd->status.class_
 * @param m ID -an index- for direct indexing map[] array
 * @return 1 if job is allowed, 0 otherwise
 */
bool pc_job_can_entermap(enum e_job jobid, int m, int group_lv) {
	uint16 idx = 0;

	//Map is other map server
	//FIXME: Currently, a map-server doesn't recognized map's attributes on other server, so we assume it's fine to warp
	if (m < 0)
		return true;

	if (m >= MAX_MAP_PER_SERVER || !mapdata[m].cell)
		return false;

	if (!pcdb_checkid(jobid))
		return false;

	idx = pc_class2idx(jobid);
	if (!job_info[idx].noenter_map.zone || group_lv > job_info[idx].noenter_map.group_lv)
		return true;

	if ((!map_flag_vs2(m) && job_info[idx].noenter_map.zone&1) || //Normal
		(mapdata[m].flag.pvp && job_info[idx].noenter_map.zone&2) || //PVP
		(map_flag_gvg2_no_te(m) && job_info[idx].noenter_map.zone&4) || //GVG
		(mapdata[m].flag.battleground && job_info[idx].noenter_map.zone&8) || //Battleground
		(map_flag_gvg2_te(m) && job_info[idx].noenter_map.zone&16) || //WOE:TE
		(mapdata[m].flag.restricted && job_info[idx].noenter_map.zone&(8 * mapdata[m].zone)) //Zone restriction
		)
		return false;

	return true;
}

/**
 * Tells client about player's costume view on mapchange for checking 'nocostume' mapflag.
 * @param sd
 */
void pc_set_costume_view(struct map_session_data *sd) {
	int i = -1, head_low = 0, head_mid = 0, head_top = 0, robe = 0;
	struct item_data *id = NULL;

	nullpo_retv(sd);

	head_low = sd->status.head_bottom;
	head_mid = sd->status.head_mid;
	head_top = sd->status.head_top;
	robe = sd->status.robe;

	sd->status.head_bottom = sd->status.head_mid = sd->status.head_top = sd->status.robe = 0;

	//Added check to prevent sending the same look on multiple slots ->
	//causes client to redraw item on top of itself. (suggested by Lupus)
	//Normal headgear checks
	if ((i = sd->equip_index[EQI_HEAD_LOW]) != -1 && (id = sd->inventory_data[i])) {
		if (!(id->equip&(EQP_HEAD_MID|EQP_HEAD_TOP)))
			sd->status.head_bottom = id->look;
		else
			sd->status.head_bottom = 0;
	}
	if ((i = sd->equip_index[EQI_HEAD_MID]) != -1 && (id = sd->inventory_data[i])) {
		if (!(id->equip&(EQP_HEAD_TOP)))
			sd->status.head_mid = id->look;
		else
			sd->status.head_mid = 0;
	}
	if ((i = sd->equip_index[EQI_HEAD_TOP]) != -1 && (id = sd->inventory_data[i]))
		sd->status.head_top = id->look;
	if ((i = sd->equip_index[EQI_GARMENT]) != -1 && (id = sd->inventory_data[i]))
		sd->status.robe = id->look;

	//Costumes check
	if (!mapdata[sd->bl.m].flag.nocostume) {
		if ((i = sd->equip_index[EQI_COSTUME_HEAD_LOW]) != -1 && (id = sd->inventory_data[i])) {
			if (!(id->equip&(EQP_COSTUME_HEAD_MID|EQP_COSTUME_HEAD_TOP)))
				sd->status.head_bottom = id->look;
			else
				sd->status.head_bottom = 0;
		}
		if ((i = sd->equip_index[EQI_COSTUME_HEAD_MID]) != -1 && (id = sd->inventory_data[i])) {
			if (!(id->equip&EQP_COSTUME_HEAD_TOP))
				sd->status.head_mid = id->look;
			else
				sd->status.head_mid = 0;
		}
		if ((i = sd->equip_index[EQI_COSTUME_HEAD_TOP]) != -1 && (id = sd->inventory_data[i]))
			sd->status.head_top = id->look;
		if ((i = sd->equip_index[EQI_COSTUME_GARMENT]) != -1 && (id = sd->inventory_data[i]))
			sd->status.robe = id->look;
	}

	if (sd->setlook_head_bottom)
		sd->status.head_bottom = sd->setlook_head_bottom;
	if (sd->setlook_head_mid)
		sd->status.head_mid = sd->setlook_head_mid;
	if (sd->setlook_head_top)
		sd->status.head_top = sd->setlook_head_top;
	if (sd->setlook_robe)
		sd->status.robe = sd->setlook_robe;

	if (head_low != sd->status.head_bottom)
		clif_changelook(&sd->bl, LOOK_HEAD_BOTTOM, sd->status.head_bottom);
	if (head_mid != sd->status.head_mid)
		clif_changelook(&sd->bl, LOOK_HEAD_MID, sd->status.head_mid);
	if (head_top != sd->status.head_top)
		clif_changelook(&sd->bl, LOOK_HEAD_TOP, sd->status.head_top);
	if (robe != sd->status.robe)
		clif_changelook(&sd->bl, LOOK_ROBE, sd->status.robe);
}

void pc_update_job_and_level(struct map_session_data *sd) {
	nullpo_retv(sd);

	if (sd->status.party_id) {
		struct party_data *p;
		int i;

		if ((p = party_search(sd->status.party_id))) {
			ARR_FIND(0, MAX_PARTY, i, p->party.member[i].char_id == sd->status.char_id);
			if (i < MAX_PARTY) {
				p->party.member[i].class_ = sd->status.class_;
				clif_party_job_and_level(sd);
			}
		}
	}
}

struct s_attendance_period *pc_attendance_period(void) {
	uint32 date = date_get(DT_YYYYMMDD);

	if (!attendance_periods)
		return NULL;

	if (attendance_periods->start <= date && attendance_periods->end >= date)
		return attendance_periods;

	return NULL;
}

bool pc_attendance_enabled(void) {
	//Check if the attendance feature is disabled
	if (!battle_config.feature_attendance)
		return false;

	//Check if there is a running attendance period
	return (pc_attendance_period() != NULL);
}

static inline bool pc_attendance_rewarded_today(struct map_session_data *sd) {
	return pc_readreg2(sd, ATTENDANCE_DATE_VAR) >= date_get(DT_YYYYMMDD);
}

int32 pc_attendance_counter(struct map_session_data *sd) {
	struct s_attendance_period *period = NULL;
	int counter = 0;

	//No running attendance period
	if (!(period = pc_attendance_period()))
		return 0;

	//Get the counter for the current period
	counter = pc_readreg2(sd, ATTENDANCE_COUNT_VAR);

	//Check if we have a remaining counter from a previous period
	if (counter > 0 && pc_readreg2(sd, ATTENDANCE_DATE_VAR) < period->start) {
		pc_setreg2(sd, ATTENDANCE_COUNT_VAR, 0); //Reset the counter to zero
		return 0;
	}

	return 10 * counter + (pc_attendance_rewarded_today(sd) ? 1 : 0);
}

void pc_attendance_claim_reward(struct map_session_data *sd) {
	struct s_attendance_period *period = NULL;
	struct mail_message msg;
	int32 attendance_counter = 0;

	//If the user's group does not have the permission
	if (!pc_has_permission(sd, PC_PERM_ATTENDANCE))
		return;

	//Check if the attendance feature is disabled
	if (!pc_attendance_enabled())
		return;

	//Check if the user already got his reward today
	if (pc_attendance_rewarded_today(sd))
		return;

	attendance_counter = pc_readreg2(sd, ATTENDANCE_COUNT_VAR);
	attendance_counter += 1;

	if (!(period = pc_attendance_period()))
		return;

	if (period->reward_count < attendance_counter)
		return;

	pc_setreg2(sd, ATTENDANCE_DATE_VAR, date_get(DT_YYYYMMDD));
	pc_setreg2(sd, ATTENDANCE_COUNT_VAR, attendance_counter);

	if (save_settings&CHARSAVE_ATTENDANCE)
		chrif_save(sd, CSAVE_NORMAL);

	memset(&msg, 0, sizeof(struct mail_message));

	msg.dest_id = sd->status.char_id;
	safestrncpy(msg.send_name, msg_txt(NULL, 764), NAME_LENGTH);
	safesnprintf(msg.title, MAIL_TITLE_LENGTH, msg_txt(NULL, 765), attendance_counter);
	safesnprintf(msg.body, MAIL_BODY_LENGTH, msg_txt(NULL, 766), attendance_counter);

	msg.item[0].nameid = period->rewards[attendance_counter - 1].itemid;
	msg.item[0].amount = period->rewards[attendance_counter - 1].amount;
	msg.item[0].identify = 1;

	msg.status = MAIL_NEW;
	msg.type = MAIL_INBOX_NORMAL;
	msg.timestamp = time(NULL);

	intif_Mail_send(0, &msg);

	clif_attendence_response(sd, attendance_counter);
}

static void pc_attendance_clear(void) {
	if (attendance_periods) {
		if (attendance_periods->rewards) {
			aFree(attendance_periods->rewards);
			attendance_periods->rewards = NULL;
			attendance_periods->reward_count = 0;
		}
		aFree(attendance_periods);
		attendance_periods = NULL;
	}
}

bool pc_has_second_costume(int class_) {
	if (class_&JOBL_THIRD)
		return true;

	return false;
}

struct s_stylist_data *pc_stylist_data(int type, int16 idx) {
	if (type < 0 || type >= MAX_STYLIST_TYPE)
		return NULL;

	if (idx < 0 || idx >= stylist_data_count)
		return NULL;

	return &stylist_datas[type][idx];
}

static bool pc_stylist_validate_requirements(struct map_session_data *sd, int type, int16 idx) {
	struct s_stylist_data *entry = pc_stylist_data(type, idx);
	struct s_stylist_data_req *human = &entry->req[0], *doram = &entry->req[1];
	bool is_doram = ((sd->class_&MAPID_BASEMASK) == MAPID_SUMMONER);
	struct item it;

	nullpo_retr(false, sd);

	if (is_doram) {
		if (!doram)
			return false;
	} else {
		if (!human)
			return false;
	}

	if (entry->id >= 0) {
		if (is_doram) {
			if (doram->zeny && pc_payzeny(sd, doram->zeny, LOG_TYPE_CONSUME, NULL))
				return false;
			else if (doram->itemid) {
				it.nameid = doram->itemid;
				it.amount = 1;
				if (pc_delitem(sd, pc_search_inventory(sd, it.nameid), it.amount, 0, 0, LOG_TYPE_OTHER))
					return false;
			} else if (doram->boxid) {
				it.nameid = doram->boxid;
				it.amount = 1;
				if (pc_delitem(sd, pc_search_inventory(sd, it.nameid), it.amount, 0, 0, LOG_TYPE_OTHER))
					return false;
			}
		} else {
			if (human->zeny && pc_payzeny(sd, human->zeny, LOG_TYPE_CONSUME, NULL))
				return false;
			else if (human->itemid) {
				it.nameid = human->itemid;
				it.amount = 1;
				if (pc_delitem(sd, pc_search_inventory(sd, it.nameid), it.amount, 0, 0, LOG_TYPE_OTHER))
					return false;
			} else if (human->boxid) {
				it.nameid = human->boxid;
				it.amount = 1;
				if (pc_delitem(sd, pc_search_inventory(sd, it.nameid), it.amount, 0, 0, LOG_TYPE_OTHER))
					return false;
			}
		}
		return true;
	}

	return false;
}

static void pc_stylist_recieve_item(struct map_session_data *sd, uint16 nameid) {
	struct mail_message msg;

	nullpo_retv(sd);

	memset(&msg, 0, sizeof(struct mail_message));

	msg.dest_id = sd->status.char_id;
	safestrncpy(msg.send_name, msg_txt(NULL, 772), NAME_LENGTH); // Styling Shop
	safestrncpy(msg.title, msg_txt(NULL, 773), MAIL_TITLE_LENGTH); // Item has been delivered
	safestrncpy(msg.body, msg_txt(NULL, 774), MAIL_BODY_LENGTH); // Thank you for purchasing

	msg.item[0].nameid = nameid;
	msg.item[0].amount = 1;
	msg.item[0].identify = 1;

	msg.status = MAIL_NEW;
	msg.type = MAIL_INBOX_NORMAL;
	msg.timestamp = time(NULL);

	intif_Mail_send(0, &msg);
}

void pc_stylist_process(struct map_session_data *sd, int type, int16 idx, bool isItem) {
	struct s_stylist_data *entry = NULL;

	nullpo_retv(sd);

	idx -= 1;

	if ((entry = pc_stylist_data(type, idx)) && pc_stylist_validate_requirements(sd, type, idx)) {
		if (!isItem)
			pc_changelook(sd, type, entry->id);
		else
			pc_stylist_recieve_item(sd, entry->id);
	}
}

static void pc_stylist_clear(void) {
	int i;

	for (i = 0; i < MAX_STYLIST_TYPE; i++) {
		if (stylist_datas[i]) {
			aFree(stylist_datas[i]);
			stylist_datas[i] = NULL;
			stylist_data_count = 0;
		}
	}
}

/*==========================================
 * pc Init/Terminate
 *------------------------------------------*/
void do_final_pc(void) {
	db_destroy(itemcd_db);
	do_final_pc_groups();

	ers_destroy(pc_sc_display_ers);

	if (battle_config.feature_attendance)
		pc_attendance_clear();
	if (battle_config.feature_stylistui)
		pc_stylist_clear();
}

void do_init_pc(void) {
	itemcd_db = idb_alloc(DB_OPT_RELEASE_DATA);

	pc_readdb();
	pc_read_motd(); //Read MOTD [Valaris]

	add_timer_func_list(pc_invincible_timer, "pc_invincible_timer");
	add_timer_func_list(pc_eventtimer, "pc_eventtimer");
	add_timer_func_list(pc_inventory_rental_end, "pc_inventory_rental_end");
	add_timer_func_list(pc_calc_pvprank_timer, "pc_calc_pvprank_timer");
	add_timer_func_list(pc_autosave, "pc_autosave");
	add_timer_func_list(pc_spiritball_timer, "pc_spiritball_timer");
	add_timer_func_list(pc_shieldball_timer, "pc_shieldball_timer");
	add_timer_func_list(pc_rageball_timer, "pc_rageball_timer");
	add_timer_func_list(pc_charmball_timer, "pc_charmball_timer");
	add_timer_func_list(pc_soulball_timer, "pc_soulball_timer");
	add_timer_func_list(pc_follow_timer, "pc_follow_timer");
	add_timer_func_list(pc_endautobonus, "pc_endautobonus");
	add_timer_func_list(pc_global_expiration_timer, "pc_global_expiration_timer");
	add_timer_func_list(pc_expiration_timer, "pc_expiration_timer");
	add_timer_func_list(pc_autotrade_timer, "pc_autotrade_timer");

	add_timer(gettick() + autosave_interval, pc_autosave, 0, 0);

	//0 = day, 1 = night [Yor]
	night_flag = battle_config.night_at_start ? 1 : 0;

	if (battle_config.day_duration > 0 && battle_config.night_duration > 0) {
		int day_duration = battle_config.day_duration;
		int night_duration = battle_config.night_duration;

		//Add night/day timer [Yor]
		add_timer_func_list(map_day_timer, "map_day_timer");
		add_timer_func_list(map_night_timer, "map_night_timer");

		day_timer_tid   = add_timer_interval(gettick() + (night_flag ? 0 : day_duration) + night_duration, map_day_timer,   0, 0, day_duration + night_duration);
		night_timer_tid = add_timer_interval(gettick() + day_duration + (night_flag ? night_duration : 0), map_night_timer, 0, 0, day_duration + night_duration);
	}

	do_init_pc_groups();

	pc_sc_display_ers = ers_new(sizeof(struct sc_display_entry), "pc.c::pc_sc_display_ers", ERS_OPT_NONE);
}
