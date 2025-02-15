// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/cbasetypes.h"
#include "../common/timer.h"
#include "../common/nullpo.h"
#include "../common/malloc.h"
#include "../common/random.h"
#include "../common/showmsg.h"
#include "../common/strlib.h"
#include "../common/utils.h"
#include "../common/ers.h"

#include "map.h"
#include "path.h"
#include "clif.h"
#include "pc.h"
#include "status.h"
#include "skill.h"
#include "pet.h"
#include "homunculus.h"
#include "mercenary.h"
#include "elemental.h"
#include "mob.h"
#include "npc.h"
#include "battle.h"
#include "battleground.h"
#include "party.h"
#include "itemdb.h"
#include "script.h"
#include "intif.h"
#include "log.h"
#include "chrif.h"
#include "guild.h"
#include "date.h"
#include "unit.h"
#include "achievement.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define SKILLUNITTIMER_INTERVAL 100
#define TIMERSKILL_INTERVAL 150

//Ranges reserved for mapping skill ids to skilldb offsets
#define HM_SKILLRANGEMIN 1101
#define HM_SKILLRANGEMAX (HM_SKILLRANGEMIN + MAX_HOMUNSKILL)
#define MC_SKILLRANGEMIN 1301
#define MC_SKILLRANGEMAX (MC_SKILLRANGEMIN + MAX_MERCSKILL)
#define EL_SKILLRANGEMIN 1501
#define EL_SKILLRANGEMAX (EL_SKILLRANGEMIN + MAX_ELEMENTALSKILL)
#define GD_SKILLRANGEMIN 1701
#define GD_SKILLRANGEMAX (GD_SKILLRANGEMIN + MAX_GUILDSKILL)

static struct eri *skill_unit_ers = NULL; //For handling skill_unit's [Skotlex]
static struct eri *skill_timer_ers = NULL; //For handling skill_timerskills [Skotlex]
static DBMap *bowling_db = NULL; //int mob_id -> struct mob_data*

DBMap *skillunit_db = NULL; //int id -> struct skill_unit*

/**
 * Skill Unit Persistency during endack routes (mostly for songs see bugreport:4574)
 */
DBMap *skillusave_db = NULL; //char_id -> struct skill_usave
struct skill_usave {
	uint16 skill_id, skill_lv;
};

struct s_skill_db skill_db[MAX_SKILL_DB];

struct s_skill_produce_db skill_produce_db[MAX_SKILL_PRODUCE_DB];

struct s_skill_arrow_db skill_arrow_db[MAX_SKILL_ARROW_DB];

struct s_skill_abra_db skill_abra_db[MAX_SKILL_ABRA_DB];

struct s_skill_improvise_db {
	uint16 skill_id;
	unsigned short per; //1-10000
};
struct s_skill_improvise_db skill_improvise_db[MAX_SKILL_IMPROVISE_DB];

#define MAX_SKILL_CHANGEMATERIAL_DB 75
#define MAX_SKILL_CHANGEMATERIAL_SET 3
struct s_skill_changematerial_db {
	unsigned short nameid,
		unique_id,
		rate,
		qty[MAX_SKILL_CHANGEMATERIAL_SET],
		qty_rate[MAX_SKILL_CHANGEMATERIAL_SET];
};
struct s_skill_changematerial_db skill_changematerial_db[MAX_SKILL_CHANGEMATERIAL_DB];

//Warlock
struct s_skill_spellbook_db skill_spellbook_db[MAX_SKILL_SPELLBOOK_DB];

//Guillotine Cross
struct s_skill_magicmushroom_db skill_magicmushroom_db[MAX_SKILL_MAGICMUSHROOM_DB];

struct s_skill_unit_layout skill_unit_layout[MAX_SKILL_UNIT_LAYOUT];
int firewall_unit_pos;
int icewall_unit_pos;
int earthstrain_unit_pos;
int firerain_unit_pos;

struct s_skill_nounit_layout skill_nounit_layout[MAX_SKILL_UNIT_LAYOUT];
int overbrand_nounit_pos;
int overbrand_brandish_nounit_pos;

//Early declaration
int skill_block_check(struct block_list *bl, enum sc_type type, uint16 skill_id);
static int skill_check_unit_range(struct block_list *bl, int x, int y, uint16 skill_id, uint16 skill_lv);
static int skill_check_unit_range2(struct block_list *bl, int x, int y, uint16 skill_id, uint16 skill_lv, bool isNearNPC);
static int skill_destroy_trap(struct block_list *bl, va_list ap);
static int skill_check_condition_mob_master_sub(struct block_list *bl, va_list ap);
static bool skill_check_condition_sc_required(struct map_session_data *sd, unsigned short skill_id, struct skill_condition *require);
//Use this function for splash skills that can't hit icewall when cast by players
static inline int splash_target(struct block_list *bl) {
	return (bl->type == BL_MOB ? BL_SKILL|BL_CHAR : BL_CHAR);
}

//Returns the id of the skill, or 0 if not found.
int skill_name2id(const char *name)
{
	if( !name )
		return 0;

	return strdb_iget(skilldb_name2id, name);
}

//Maps skill ids to skill db offsets.
//Returns the skill's array index, or 0 (Unknown Skill).
int skill_get_index(uint16 skill_id)
{
	//Avoid ranges reserved for mapping guild/homun/mercenary/elemental skills
	if( (skill_id >= GD_SKILLRANGEMIN && skill_id <= GD_SKILLRANGEMAX) ||
		(skill_id >= HM_SKILLRANGEMIN && skill_id <= HM_SKILLRANGEMAX) ||
		(skill_id >= MC_SKILLRANGEMIN && skill_id <= MC_SKILLRANGEMAX) ||
		(skill_id >= EL_SKILLRANGEMIN && skill_id <= EL_SKILLRANGEMAX) )
		return 0;

	//Map skill id to skill db index
	if( skill_id >= GD_SKILLBASE )
		skill_id = GD_SKILLRANGEMIN + skill_id - GD_SKILLBASE;
	else if( skill_id >= EL_SKILLBASE )
		skill_id = EL_SKILLRANGEMIN + skill_id - EL_SKILLBASE;
	else if( skill_id >= MC_SKILLBASE )
		skill_id = MC_SKILLRANGEMIN + skill_id - MC_SKILLBASE;
	else if( skill_id >= HM_SKILLBASE )
		skill_id = HM_SKILLRANGEMIN + skill_id - HM_SKILLBASE;

	//Validate result
	if( !skill_id || skill_id >= MAX_SKILL_DB )
		return 0;

	return skill_id;
}

const char *skill_get_name(uint16 skill_id)
{
	return skill_db[skill_get_index(skill_id)].name;
}

const char *skill_get_desc(uint16 skill_id)
{
	return skill_db[skill_get_index(skill_id)].desc;
}

//Out of bounds error checking [celest]
//Checks/adjusts id
static void skill_chk(uint16 *skill_id)
{
	*skill_id = skill_get_index(*skill_id);
}

//Checks/adjusts level
static void skill_chk2(uint16 *skill_lv)
{
	*skill_lv = (*skill_lv < 1) ? 1 : (*skill_lv > MAX_SKILL_LEVEL) ? MAX_SKILL_LEVEL : *skill_lv;
}

//Checks/adjusts index.
//Make sure we don't use negative index
static void skill_chk3(int *idx) {
	*idx = max(*idx, 0);
}

#define skill_get(var,id) { skill_chk(&id); if (!id) return 0; return var; }
#define skill_get2(var,id,lv) { skill_chk(&id); if (!id) return 0; skill_chk2(&lv); return var; }
#define skill_get3(var,id,x) { skill_chk(&id); if (!id) return 0; skill_chk3(&x); return var; }

//Skill DB
int skill_get_hit(uint16 skill_id)                               { skill_get(skill_db[skill_id].hit, skill_id); }
int skill_get_inf(uint16 skill_id)                               { skill_get(skill_db[skill_id].inf, skill_id); }
int skill_get_ele(uint16 skill_id ,uint16 skill_lv)              { skill_get2(skill_db[skill_id].element[skill_lv - 1], skill_id, skill_lv); }
int skill_get_nk(uint16 skill_id)                                { skill_get(skill_db[skill_id].nk, skill_id); }
int skill_get_max(uint16 skill_id)                               { skill_get(skill_db[skill_id].max, skill_id); }
int skill_get_range(uint16 skill_id ,uint16 skill_lv)            { skill_get2(skill_db[skill_id].range[skill_lv - 1], skill_id, skill_lv); }
int skill_get_splash(uint16 skill_id ,uint16 skill_lv)           { skill_get2((skill_db[skill_id].splash[skill_lv - 1] >= 0 ? skill_db[skill_id].splash[skill_lv - 1] : AREA_SIZE), skill_id, skill_lv); }
int skill_get_num(uint16 skill_id ,uint16 skill_lv)              { skill_get2(skill_db[skill_id].num[skill_lv - 1], skill_id, skill_lv); }
int skill_get_cast(uint16 skill_id ,uint16 skill_lv)             { skill_get2(skill_db[skill_id].cast[skill_lv - 1], skill_id, skill_lv); }
int skill_get_delay(uint16 skill_id ,uint16 skill_lv)            { skill_get2(skill_db[skill_id].delay[skill_lv - 1], skill_id, skill_lv); }
int skill_get_walkdelay(uint16 skill_id, uint16 skill_lv)        { skill_get2(skill_db[skill_id].walkdelay[skill_lv - 1], skill_id, skill_lv); }
static int skill_get_time_sub(uint16 skill_id, uint16 skill_lv)  { skill_get2(skill_db[skill_id].upkeep_time[skill_lv - 1], skill_id, skill_lv); }
int skill_get_time(uint16 skill_id, uint16 skill_lv)
{
	int duration = skill_get_time_sub(skill_id, skill_lv);

	if (duration == -1)
		return 3600000; //Set as infinite duration
	return duration;
}
static int skill_get_time2_sub(uint16 skill_id, uint16 skill_lv) { skill_get2(skill_db[skill_id].upkeep_time2[skill_lv - 1], skill_id, skill_lv); }
int skill_get_time2(uint16 skill_id, uint16 skill_lv)
{
	int duration = skill_get_time2_sub(skill_id, skill_lv);

	if (duration == -1)
		return 3600000;
	return duration;
}
int skill_get_castdef(uint16 skill_id)                           { skill_get(skill_db[skill_id].cast_def_rate, skill_id); }
int skill_get_inf2(uint16 skill_id)                              { skill_get(skill_db[skill_id].inf2, skill_id); }
int skill_get_inf3(uint16 skill_id)                              { skill_get(skill_db[skill_id].inf3, skill_id); }
int skill_get_castcancel(uint16 skill_id)                        { skill_get(skill_db[skill_id].castcancel, skill_id); }
int skill_get_maxcount(uint16 skill_id, uint16 skill_lv)         { skill_get2(skill_db[skill_id].maxcount[skill_lv - 1], skill_id, skill_lv); }
int skill_get_blewcount(uint16 skill_id, uint16 skill_lv)        { skill_get2(skill_db[skill_id].blewcount[skill_lv - 1], skill_id, skill_lv); }
int skill_get_castnodex(uint16 skill_id, uint16 skill_lv)        { skill_get2(skill_db[skill_id].castnodex[skill_lv - 1], skill_id, skill_lv); }
int skill_get_delaynodex(uint16 skill_id, uint16 skill_lv)       { skill_get2(skill_db[skill_id].delaynodex[skill_lv - 1], skill_id, skill_lv); }
int skill_get_nocast(uint16 skill_id)                            { skill_get(skill_db[skill_id].nocast, skill_id); }
int skill_get_type(uint16 skill_id)                              { skill_get(skill_db[skill_id].skill_type, skill_id); }
int skill_get_unit_id(uint16 skill_id, int flag)                 { skill_get3(skill_db[skill_id].unit_id[flag], skill_id, flag); }
int skill_get_unit_interval(uint16 skill_id)                     { skill_get(skill_db[skill_id].unit_interval, skill_id); }
int skill_get_unit_range(uint16 skill_id, uint16 skill_lv)       { skill_get2(skill_db[skill_id].unit_range[skill_lv - 1], skill_id, skill_lv); }
int skill_get_unit_target(uint16 skill_id)                       { skill_get(skill_db[skill_id].unit_target&BCT_ALL, skill_id); }
int skill_get_unit_bl_target(uint16 skill_id)                    { skill_get(skill_db[skill_id].unit_target&BL_ALL, skill_id); }
int skill_get_unit_flag(uint16 skill_id)                         { skill_get(skill_db[skill_id].unit_flag, skill_id); }
int skill_get_unit_layout_type(uint16 skill_id, uint16 skill_lv) { skill_get2(skill_db[skill_id].unit_layout_type[skill_lv - 1], skill_id, skill_lv); }
int skill_get_cooldown(uint16 skill_id, uint16 skill_lv)         { skill_get2(skill_db[skill_id].cooldown[skill_lv - 1], skill_id, skill_lv); }
#ifdef RENEWAL_CAST
int skill_get_fixed_cast(uint16 skill_id, uint16 skill_lv)       { skill_get2(skill_db[skill_id].fixed_cast[skill_lv - 1], skill_id, skill_lv); }
#endif
//Skill requirements
int skill_get_hp(uint16 skill_id, uint16 skill_lv)         { skill_get2(skill_db[skill_id].require.hp[skill_lv - 1], skill_id, skill_lv); }
int skill_get_mhp(uint16 skill_id, uint16 skill_lv)        { skill_get2(skill_db[skill_id].require.mhp[skill_lv - 1], skill_id, skill_lv); }
int skill_get_sp(uint16 skill_id, uint16 skill_lv)         { skill_get2(skill_db[skill_id].require.sp[skill_lv - 1], skill_id, skill_lv); }
int skill_get_hp_rate(uint16 skill_id, uint16 skill_lv)    { skill_get2(skill_db[skill_id].require.hp_rate[skill_lv - 1], skill_id, skill_lv); }
int skill_get_sp_rate(uint16 skill_id, uint16 skill_lv)    { skill_get2(skill_db[skill_id].require.sp_rate[skill_lv - 1], skill_id, skill_lv); }
int skill_get_zeny(uint16 skill_id, uint16 skill_lv)       { skill_get2(skill_db[skill_id].require.zeny[skill_lv - 1], skill_id, skill_lv); }
int skill_get_weapontype(uint16 skill_id)                  { skill_get(skill_db[skill_id].require.weapon, skill_id); }
int skill_get_ammotype(uint16 skill_id)                    { skill_get(skill_db[skill_id].require.ammo, skill_id); }
int skill_get_ammo_qty(uint16 skill_id, uint16 skill_lv)   { skill_get2(skill_db[skill_id].require.ammo_qty[skill_lv - 1], skill_id, skill_lv); }
int skill_get_state(uint16 skill_id)                       { skill_get(skill_db[skill_id].require.state, skill_id); }
//int skill_get_status(uint16 skill_id, int idx)           { skill_get3(skill_db[skill_id].require.status[idx], skill_id, idx); }
int skill_get_status_count(uint16 skill_id)                { skill_get(skill_db[skill_id].require.status_count, skill_id); }
int skill_get_spiritball(uint16 skill_id, uint16 skill_lv) { skill_get2(skill_db[skill_id].require.spiritball[skill_lv - 1], skill_id, skill_lv); }
int skill_get_itemid(uint16 skill_id, int idx)             { skill_get3(skill_db[skill_id].require.itemid[idx], skill_id, idx); }
int skill_get_itemqty(uint16 skill_id, int idx)            { skill_get3(skill_db[skill_id].require.amount[idx], skill_id, idx); }
int skill_get_itemeq(uint16 skill_id, int idx)             { skill_get3(skill_db[skill_id].require.eqItem[idx], skill_id, idx); }

int skill_tree_get_max(uint16 skill_id, int b_class)
{
	int i;

	b_class = pc_class2idx(b_class);
	ARR_FIND(0, MAX_SKILL_TREE, i, (!skill_tree[b_class][i].id || skill_tree[b_class][i].id == skill_id));
	if( i < MAX_SKILL_TREE && skill_tree[b_class][i].id == skill_id )
		return skill_tree[b_class][i].lv;
	else
		return skill_get_max(skill_id);
}

int skill_frostjoke_scream(struct block_list *bl, va_list ap);
int skill_attack_area(struct block_list *bl, va_list ap);
struct skill_unit_group *skill_locate_element_field(struct block_list *bl); //[Skotlex]
int skill_graffitiremover(struct block_list *bl, va_list ap); //[Valaris]
int skill_greed(struct block_list *bl, va_list ap);
static int skill_cell_overlap(struct block_list *bl, va_list ap);
static int skill_trap_splash(struct block_list *bl, va_list ap);
struct skill_unit_group_tickset *skill_unitgrouptickset_search(struct block_list *bl, struct skill_unit_group *group, int tick);
static int skill_unit_onplace(struct skill_unit *unit, struct block_list *bl, unsigned int tick);
int skill_unit_onleft(uint16 skill_id, struct block_list *bl, unsigned int tick);
static int skill_unit_effect(struct block_list *bl, va_list ap);
static int skill_flicker_bind_trap(struct block_list *bl, va_list ap);

int skill_get_casttype(uint16 skill_id)
{
	int inf = skill_get_inf(skill_id);

	if (inf&(INF_GROUND_SKILL))
		return CAST_GROUND;
	if (inf&INF_SUPPORT_SKILL)
		return CAST_NODAMAGE;
	if (inf&INF_SELF_SKILL) {
		if(skill_get_inf2(skill_id)&INF2_NO_TARGET_SELF)
			return CAST_DAMAGE; //Combo skill
		return CAST_NODAMAGE;
	}
	if (skill_get_nk(skill_id)&NK_NO_DAMAGE)
		return CAST_NODAMAGE;
	return CAST_DAMAGE;
}

//Returns actual skill range taking into account attack range and AC_OWL [Skotlex]
int skill_get_range2(struct block_list *bl, uint16 skill_id, uint16 skill_lv, bool isServer)
{
	int range, inf3 = 0;

	if( bl->type == BL_MOB && (battle_config.mob_ai&0x400) )
		return 9; //Mobs have a range of 9 regardless of skill used

	range = skill_get_range(skill_id, skill_lv);
	if( range < 0 ) {
		if( battle_config.use_weapon_skill_range&bl->type )
			return status_get_range(bl);
		range *= -1;
	}

	if( isServer && range > 14 )
		range = 14; //Server-sided base range can't be above 14

	inf3 = skill_get_inf3(skill_id);
	if( inf3&(INF3_EFF_VULTURE|INF3_EFF_SNAKEEYE) ) {
		if( bl->type == BL_PC ) {
			if( inf3&INF3_EFF_VULTURE )
				range += pc_checkskill((TBL_PC *)bl, AC_VULTURE);
			if( inf3&INF3_EFF_SNAKEEYE ) //Allow GS skills to be effected by the range of Snake Eyes [Reddozen]
				range += pc_checkskill((TBL_PC *)bl, GS_SNAKEEYE);
		} else
			range += battle_config.mob_eye_range_bonus;
	}

	if( inf3&(INF3_EFF_SHADOWJUMP|INF3_EFF_RADIUS|INF3_EFF_RESEARCHTRAP) ) {
		if( bl->type == BL_PC ) {
			if( inf3&INF3_EFF_SHADOWJUMP )
				range = skill_get_range(NJ_SHADOWJUMP, pc_checkskill((TBL_PC *)bl, NJ_SHADOWJUMP));
			if( inf3&INF3_EFF_RADIUS )
				range += pc_checkskill((TBL_PC *)bl, WL_RADIUS);
			if( inf3&INF3_EFF_RESEARCHTRAP )
				range += (1 + pc_checkskill((TBL_PC *)bl, RA_RESEARCHTRAP)) / 2;
		}
	}

	if( !range && bl->type != BL_PC )
		return 9; //Enable non players to use self skills on others [Skotlex]
	return range;
}

/** Copy Referral: dummy skills should point to their source.
 * @param skill_id Dummy skill ID
 * @return Real skill id if found
 */
unsigned short skill_dummy2skill_id(unsigned short skill_id) {
	switch( skill_id ) {
		case AB_DUPLELIGHT_MELEE:
		case AB_DUPLELIGHT_MAGIC:
			return AB_DUPLELIGHT;
		case WL_CHAINLIGHTNING_ATK:
			return WL_CHAINLIGHTNING;
		case WL_TETRAVORTEX_FIRE:
		case WL_TETRAVORTEX_WATER:
		case WL_TETRAVORTEX_WIND:
		case WL_TETRAVORTEX_GROUND:
			return WL_TETRAVORTEX;
		case WL_SUMMON_ATK_FIRE:
			return WL_SUMMONFB;
		case WL_SUMMON_ATK_WIND:
			return WL_SUMMONBL;
		case WL_SUMMON_ATK_WATER:
			return WL_SUMMONWB;
		case WL_SUMMON_ATK_GROUND:
			return WL_SUMMONSTONE;
		//case NC_MAGMA_ERUPTION_DOTDAMAGE:
			//return NC_MAGMA_ERUPTION;
		case LG_OVERBRAND_BRANDISH:
		case LG_OVERBRAND_PLUSATK:
			return LG_OVERBRAND;
		case WM_REVERBERATION_MELEE:
		case WM_REVERBERATION_MAGIC:
			return WM_REVERBERATION;
		case WM_SEVERE_RAINSTORM_MELEE:
			return WM_SEVERE_RAINSTORM;
		case GN_CRAZYWEED_ATK:
			return GN_CRAZYWEED;
		case GN_HELLS_PLANT_ATK:
			return GN_HELLS_PLANT;
		case GN_SLINGITEM_RANGEMELEEATK:
			return GN_SLINGITEM;
		case SJ_FALLINGSTAR_ATK:
		case SJ_FALLINGSTAR_ATK2:
			return SJ_FALLINGSTAR;
		case OB_OBOROGENSOU_TRANSITION_ATK:
			return OB_OBOROGENSOU;
		case RL_R_TRIP_PLUSATK:
			return RL_R_TRIP;
		case RL_B_FLICKER_ATK:
			return RL_FLICKER;
		case SU_SV_ROOTTWIST_ATK:
			return SU_SV_ROOTTWIST;
		case SU_PICKYPECK_DOUBLE_ATK:
			return SU_PICKYPECK;
		case SU_CN_METEOR2:
			return SU_CN_METEOR;
		case SU_LUNATICCARROTBEAT2:
			return SU_LUNATICCARROTBEAT;
		case NPC_MAXPAIN_ATK:
			return NPC_MAXPAIN;
		case NPC_REVERBERATION_ATK:
			return NPC_REVERBERATION;
	}
	return skill_id;
}

/**
 * Check skill unit maxcount
 * @param src: Caster to check against
 * @param x: X location of skill
 * @param y: Y location of skill
 * @param skill_id: Skill used
 * @param skill_lv: Skill level used
 * @param type: Type of unit to check against for battle_config checks
 * @param display_failure: Display skill failure message
 * @return True on skill cast success or false on failure
 */
bool skill_pos_maxcount_check(struct block_list *src, int16 x, int16 y, uint16 skill_id, uint16 skill_lv, enum bl_type type, bool display_failure) {
	struct unit_data *ud = NULL;
	struct map_session_data *sd = NULL;
	int i, maxcount = 0;

	if( !src )
		return false;

	ud = unit_bl2ud(src);
	sd = map_id2sd(src->id);

	if( !(type&battle_config.skill_reiteration) && skill_get_unit_flag(skill_id)&UF_NOREITERATION && skill_check_unit_range(src, x, y, skill_id, skill_lv) ) {
		if( sd && display_failure )
			clif_skill_fail(sd, skill_id, USESKILL_FAIL_LEVEL, 0, 0);
		return false;
	}

	if( type&battle_config.skill_nofootset && skill_get_unit_flag(skill_id)&UF_NOFOOTSET && skill_check_unit_range2(src, x, y, skill_id, skill_lv, false) ) {
		if( sd && display_failure )
			clif_skill_fail(sd, skill_id, USESKILL_FAIL_LEVEL, 0, 0);
		return false;
	}

	if( type&battle_config.land_skill_limit && (maxcount = skill_get_maxcount(skill_id, skill_lv)) > 0 ) {
		for( i = 0; i < MAX_SKILLUNITGROUP && ud->skillunit[i] && maxcount; i++ ) {
			if( ud->skillunit[i]->skill_id == skill_id )
				maxcount--;
		}
		if( !maxcount ) {
			if( sd && display_failure )
				clif_skill_fail(sd, skill_id, USESKILL_FAIL_LEVEL, 0, 0);
			return false;
		}
	}

	return true;
}

/** Calculates heal value of skill's effect
 * @param src
 * @param target
 * @param skill_id
 * @param skill_lv
 * @param heal
 * @return modified heal value
 */
int skill_calc_heal(struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, bool heal) {
	uint16 lv;
	int i, hp = 0, bonus = 100;
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct map_session_data *tsd = BL_CAST(BL_PC, target);
	struct status_change *sc, *tsc;

	sc = status_get_sc(src);
	tsc = status_get_sc(target);

	switch( skill_id ) {
		case BA_APPLEIDUN: //HP recovery
#ifdef RENEWAL
			hp = 100 + skill_lv * 5 + status_get_vit(src) / 2;
#else
			hp = 30 + skill_lv * 5 + status_get_vit(src) / 2;
#endif
			if( sd )
				hp += 5 * pc_checkskill(sd, BA_MUSICALLESSON);
			break;
		case PR_SANCTUARY:
			hp = (skill_lv > 6) ? 777 : skill_lv * 100;
			break;
		case NPC_EVILLAND:
			hp = (skill_lv > 6) ? 666 : skill_lv * 100;
			break;
		case SU_TUNABELLY:
			hp = (skill_lv * 20 - 10) * status_get_max_hp(target) / 100;
			break;
		default:
			if( skill_lv >= battle_config.max_heal_lv )
				return battle_config.max_heal;
			if( skill_id == SU_BUNCHOFSHRIMP )
				hp = (status_get_lv(src) + status_get_int(src)) / 5 * 15;
			else if( skill_id == SU_FRESHSHRIMP ) {
				hp = (status_get_lv(src) + status_get_int(src)) / 5 * 8 + status_get_lv(src) / 80 * 2;
				hp += status_get_int(src) - (status_get_int(src) - 1) / 3 + 1;
			} else {
#ifdef RENEWAL
				/**
				* Renewal Heal Formula
				* Formula: ( [(Base Level + INT) / 5] x 30 ) x (Heal Level / 10) x (Modifiers) + MATK
				*/
				hp = ((status_get_lv(src) + status_get_int(src)) / 5 * 30) *
					(skill_id == AB_HIGHNESSHEAL ? (sd ? pc_checkskill(sd, AL_HEAL) : 10) : skill_lv) / 10;
#else
				hp = (status_get_lv(src) + status_get_int(src)) / 8 *
					((skill_id == AB_HIGHNESSHEAL ? (sd ? pc_checkskill(sd, AL_HEAL) : 10) : skill_lv) * 8 + 4);
#endif
			}
			if( sd ) {
				if( (lv = pc_checkskill(sd, HP_MEDITATIO)) > 0 )
					bonus += lv * 2;
				if( pc_checkskill(sd, SU_POWEROFSEA) > 0 ) {
					bonus += 8;
					if( pc_checkskill_summoner(sd, TYPE_SEAFOOD) >= 20 )
						bonus += 16;
				}
				if( skill_id == SU_FRESHSHRIMP && pc_checkskill(sd, SU_SPIRITOFSEA) > 0 )
					bonus += 16;
				if( tsd && sd->status.partner_id == tsd->status.char_id && (sd->class_&MAPID_UPPERMASK) == MAPID_SUPER_NOVICE && !sd->status.sex )
					bonus += 100;
			} else if( src->type == BL_HOM && (lv = hom_checkskill(((TBL_HOM *)src), HLIF_BRAIN)) > 0 )
				bonus += lv * 2;
			break;
	}

	if( sd && (i = pc_skillheal_bonus(sd, skill_id)) )
		bonus += i;

	if( sc && sc->count ) {
		if( sc->data[SC_OFFERTORIUM] )
			bonus += sc->data[SC_OFFERTORIUM]->val2;
	}

	if( bonus != 100 )
		hp = hp * bonus / 100;

#ifdef RENEWAL
	switch( skill_id ) { //MATK part of the RE heal formula [malufett]
		case BA_APPLEIDUN:
		case PR_SANCTUARY:
		case NPC_EVILLAND:
		case SU_TUNABELLY:
		case SU_FRESHSHRIMP:
			break;
		default:
			hp += status_get_matk(src, 3);
			break;
	}
#endif

	if( skill_id == AB_HIGHNESSHEAL ) //Highness Heal increases healing by a percentage
		hp += hp * (70 + 30 * skill_lv) / 100;

	if( (!heal || (target && target->type == BL_MER)) && skill_id != NPC_EVILLAND )
		hp /= 2;

	if( tsd && (i = pc_skillheal2_bonus(tsd, skill_id)) )
		hp += hp * i / 100;

	if( tsc && tsc->count ) {
		if( skill_id != NPC_EVILLAND && skill_id != BA_APPLEIDUN ) {
			if( tsc->data[SC_INCHEALRATE] )
				hp += hp * tsc->data[SC_INCHEALRATE]->val1 / 100;
			if( tsc->data[SC_WATER_INSIGNIA] && tsc->data[SC_WATER_INSIGNIA]->val1 == 2 )
				hp += hp / 10;
		}
		if( heal ) { //Has no effect on offensive heal [Inkfish]
			uint8 penalty = 0;

			if( tsc->data[SC_CRITICALWOUND] )
				penalty += tsc->data[SC_CRITICALWOUND]->val2;
			if( tsc->data[SC_DEATHHURT] )
				penalty += 20;
#ifdef RENEWAL
			if( tsc->data[SC_FUSION] )
				penalty += 25;
#endif
			if( tsc->data[SC_NORECOVER_STATE] )
				penalty = 100;
			if( penalty > 0 )
				hp -= hp * penalty / 100;
		}
	}

	return hp;
}

/** Making Plagiarism and Reproduce check their own function
 * Previous prevention for NPC skills, Wedding skills, and INF3_DIS_PLAGIA are removed since we use skill_copyable_db.txt [Cydh]
 * @param sd: Player who will copy the skill
 * @param skill_id: Target skill
 * @return 0 - Cannot be copied; 1 - Can be copied by Plagiarism 2 - Can be copied by Reproduce
 * @author Aru - For previous check; Jobbie for class restriction idea; Cydh expands the copyable skill
 */
static char skill_isCopyable(struct map_session_data *sd, uint16 skill_id) {
	uint16 idx = skill_get_index(skill_id);

	//Only copy skill that player doesn't have or the skill is old clone
	if( sd->status.skill[idx].id && sd->status.skill[idx].flag != SKILL_FLAG_PLAGIARIZED )
		return 0;

	//Check if the skill is copyable by class
	if( !pc_has_permission(sd, PC_PERM_ALL_SKILL) ) {
		uint16 job_allowed = skill_db[idx].copyable.joballowed;

		while( 1 ) {
			if( (job_allowed&0x01) && sd->status.class_ == JOB_ROGUE ) break;
			if( (job_allowed&0x02) && sd->status.class_ == JOB_STALKER ) break;
			if( (job_allowed&0x04) && sd->status.class_ == JOB_SHADOW_CHASER ) break;
			if( (job_allowed&0x08) && sd->status.class_ == JOB_SHADOW_CHASER_T ) break;
			if( (job_allowed&0x10) && sd->status.class_ == JOB_BABY_ROGUE ) break;
			if( (job_allowed&0x20) && sd->status.class_ == JOB_BABY_CHASER ) break;
				return 0;
		}
	}

	if( (skill_db[idx].copyable.option&1) && pc_checkskill(sd, RG_PLAGIARISM) && !sd->sc.data[SC_PRESERVE] )
		return 1; //Plagiarism only able to copy skill while SC_PRESERVE is not active and skill is copyable by Plagiarism

	if( (skill_db[idx].copyable.option&2) && pc_checkskill(sd, SC_REPRODUCE) && sd->sc.data[SC__REPRODUCE] )
		return 2; //Reproduce can copy skill if SC__REPRODUCE is active and the skill is copyable by Reproduce

	return 0;
}

/**
 * Check if the skill is ok to cast and when.
 * Done before skill_check_condition_castbegin, requirement
 * @param skill_id: Skill ID that casted
 * @param sd: Player who casted
 * @return true: Skill cannot be used, false: otherwise
 * @author [MouseJstr]
 */
bool skill_isNotOk(uint16 skill_id, struct map_session_data *sd)
{
	int16 m;
	uint16 idx = skill_get_index(skill_id);
	uint32 skill_nocast = 0;

	nullpo_retr(true, sd);

	m = sd->bl.m;

	if (!idx)
		return true; //Invalid skill id

	if (pc_has_permission(sd, PC_PERM_SKILL_UNCONDITIONAL))
		return false; //Can do any damn thing they want

	if (skill_id == AL_TELEPORT && sd->skillitem == skill_id && sd->skillitemlv > 2)
		return false; //Teleport lv 3 bypasses this check [Inkfish]

	if (mapdata[m].flag.noskill && skill_id != ALL_EQSWITCH)
		return true;

	//Epoque:
	//This code will compare the player's attack motion value which is influenced by ASPD before
	//allowing a skill to be cast. This is to prevent no-delay ACT files from spamming skills such as
	//AC_DOUBLE which do not have a skill delay and are not regarded in terms of attack motion
	//Attempted to cast a skill before the attack motion has finished
	if (!sd->state.autocast && sd->skillitem != skill_id && sd->canskill_tick &&
		DIFF_TICK(gettick(), sd->canskill_tick) < sd->battle_status.amotion * battle_config.skill_amotion_leniency / 100)
		return true;

	if (skill_blockpc_get(sd, skill_id) != INVALID_TIMER) {
		clif_skill_fail(sd, skill_id, USESKILL_FAIL_SKILLINTERVAL, 0, 0);
		return true;
	}

	/**
	 * It has been confirmed on a official server (thanks to Yommy) that item-cast skills bypass all the restrictions above
	 * Also, without this check, an exploit where an item casting + healing (or any other kind buff) isn't deleted after used on a restricted map
	 */
	if (sd->skillitem == skill_id)
		return false;

	skill_nocast = skill_get_nocast(skill_id);
	//Check skill restrictions [Celest]
	if ((!map_flag_vs2(m) && skill_nocast&1) ||
		(mapdata[m].flag.pvp && skill_nocast&2) ||
		(map_flag_gvg2_no_te(m) && skill_nocast&4) ||
		(mapdata[m].flag.battleground && skill_nocast&8) ||
		(map_flag_gvg2_te(m) && skill_nocast&16) ||
		(mapdata[m].flag.restricted && mapdata[m].zone && skill_nocast&(8 * mapdata[m].zone))) {
		clif_msg(sd, SKILL_CANT_USE_AREA); //This skill cannot be used within this area
		return true;
	}

	if (sd->sc.data[SC_ALL_RIDING])
		return true; //You can't use skills while in the new mounts (The client doesn't let you, this is to make cheat-safe)

	switch (skill_id) {
		case AL_WARP:
		case RETURN_TO_ELDICASTES:
		case ALL_GUARDIAN_RECALL:
		case ECLAGE_RECALL:
		case ALL_NIFLHEIM_RECALL:
		case ALL_PRONTERA_RECALL:
			if (mapdata[m].flag.nowarp) {
				clif_skill_teleportmessage(sd, 0);
				return true;
			}
			return false;
		case AL_TELEPORT:
		case SC_FATALMENACE:
		case SC_DIMENSIONDOOR:
		case ALL_ODINS_RECALL:
		case WE_CALLALLFAMILY:
			if (mapdata[m].flag.noteleport) {
				clif_skill_teleportmessage(sd, 0);
				return true;
			}
			return false; //Gonna be checked in 'skill_castend_nodamage_id'
		case WE_CALLPARTNER:
		case WE_CALLPARENT:
		case WE_CALLBABY:
			if (mapdata[m].flag.nomemo) {
				clif_skill_teleportmessage(sd, 1);
				return true;
			}
			break;
		case MC_VENDING:
		case ALL_BUYING_STORE:
			if (mapdata[sd->bl.m].flag.novending) {
				clif_displaymessage(sd->fd, msg_txt(sd, 276)); // "You can't open a shop on this map"
				clif_skill_fail(sd, skill_id, USESKILL_FAIL_LEVEL, 0, 0);
				return true;
			}
			if (map_getcell(sd->bl.m, sd->bl.x, sd->bl.y, CELL_CHKNOVENDING)) {
				clif_displaymessage(sd->fd, msg_txt(sd, 204)); // "You can't open a shop on this cell."
				clif_skill_fail(sd, skill_id, USESKILL_FAIL_LEVEL, 0, 0);
				return true;
			}
			if (npc_isnear(&sd->bl)) {
				//Uncomment for more verbose message
				//char output[150];
				//sprintf(output, msg_txt(sd, 662), battle_config.min_npc_vendchat_distance);
				//clif_displaymessage(sd->fd, output);
				clif_skill_fail(sd, skill_id, USESKILL_FAIL_THERE_ARE_NPC_AROUND, 0, 0);
				return true;
			}
		case MC_IDENTIFY: //Always allowed
			return false;
		case WZ_ICEWALL: //Noicewall flag [Valaris]
			if (mapdata[m].flag.noicewall) {
				clif_skill_fail(sd, skill_id, USESKILL_FAIL_LEVEL, 0, 0);
				return true;
			}
			break;
		case GD_EMERGENCYCALL:
		case GD_ITEMEMERGENCYCALL:
			if (!(battle_config.emergency_call&(is_agit_start() ? 2 : 1)) ||
				!(battle_config.emergency_call&(map_flag_gvg2(m) ? 8 : 4)) ||
				((battle_config.emergency_call&16) && mapdata[m].flag.nowarpto && !(mapdata[m].flag.gvg_castle || mapdata[m].flag.gvg_te_castle)))
			{
				clif_skill_fail(sd, skill_id, USESKILL_FAIL_LEVEL, 0, 0);
				return true;
			}
			break;
	}
	return false;
}

/** Check if the homunculus skill is ok to be processed
 * After checking from Homunculus side, also check the master condition
 * @param skill_id: Skill ID that casted
 * @param hd: Homunculus who casted
 * @return true: Skill cannot be used, false: otherwise
 */
bool skill_isNotOk_hom(uint16 skill_id, struct homun_data *hd)
{
	uint16 idx = skill_get_index(skill_id);

	nullpo_retr(true, hd);

	if (!idx)
		return true; //Invalid skill id

	if (hd->blockskill[idx] > 0)
		return true;

	//Use master's criteria
	return skill_isNotOk(skill_id, hd->master);
}

/** Check if the mercenary skill is ok to be processed
 * After checking from Homunculus side, also check the master condition
 * @param skill_id: Skill ID that casted
 * @param md: Mercenary who casted
 * @return true: Skill cannot be used, false: otherwise
 */
bool skill_isNotOk_mercenary(uint16 skill_id, struct mercenary_data *md)
{
	uint16 idx = skill_get_index(skill_id);

	nullpo_retr(true, md);

	if (!idx)
		return true; //Invalid Skill ID

	if (md->blockskill[idx] > 0)
		return true;

	return skill_isNotOk(skill_id, md->master);
}

/** Check if the skill can be casted near NPC or not
 * @param src Object who casted
 * @param skill_id Skill ID that casted
 * @param skill_lv Skill Lv
 * @param pos_x Position x of the target
 * @param pos_y Position y of the target
 * @return true: Skill cannot be used, false: otherwise
 * @author [Cydh]
 */
bool skill_isNotOk_npcRange(struct block_list *src, uint16 skill_id, uint16 skill_lv, int pos_x, int pos_y) {
	int inf;

	if (!src || !skill_get_index(skill_id) )
		return true;

	if (src->type == BL_PC && pc_has_permission(BL_CAST(BL_PC, src), PC_PERM_SKILL_UNCONDITIONAL))
		return false;

	inf = skill_get_inf(skill_id);

	if (inf&INF_SELF_SKILL) { //If self skill
		pos_x = src->x;
		pos_y = src->y;
	}

	if (pos_x <= 0)
		pos_x = src->x;

	if (pos_y <= 0)
		pos_y = src->y;

	return skill_check_unit_range2(src, pos_x, pos_y, skill_id, skill_lv, true);
}

struct s_skill_unit_layout *skill_get_unit_layout(uint16 skill_id, uint16 skill_lv, struct block_list *src, int x, int y)
{
	int pos = skill_get_unit_layout_type(skill_id,skill_lv);
	uint8 dir;

	nullpo_retr(NULL, src);

	if( pos < -1 || pos >= MAX_SKILL_UNIT_LAYOUT ) {
		ShowError("skill_get_unit_layout: unsupported layout type %d for skill %d (level %d)\n", pos, skill_id, skill_lv);
		pos = cap_value(pos, 0, MAX_SQUARE_LAYOUT); //Cap to nearest square layout
	}

	if( src->type == BL_MOB && skill_lv >= 10 && skill_id == WZ_WATERBALL ) //Monsters sometimes deploy more units on level 10
		pos = 4; //9x9 Area

	if( pos != -1 ) //Simple single-definition layout
		return &skill_unit_layout[pos];

	dir = (src->x == x && src->y == y) ? 6 : map_calc_dir(src,x,y); //6 - default aegis direction

	if( skill_id == MG_FIREWALL )
		return &skill_unit_layout[firewall_unit_pos + dir];
	else if( skill_id == WZ_ICEWALL )
		return &skill_unit_layout[icewall_unit_pos + dir];
	else if( skill_id == WL_EARTHSTRAIN )
		return &skill_unit_layout[earthstrain_unit_pos + dir];
	else if( skill_id == RL_FIRE_RAIN )
		return &skill_unit_layout[firerain_unit_pos + dir];

	ShowError("skill_get_unit_layout: unknown unit layout for skill %d (level %d)\n", skill_id, skill_lv);
	return &skill_unit_layout[0]; //Default 1x1 layout
}

struct s_skill_nounit_layout* skill_get_nounit_layout(uint16 skill_id, uint16 skill_lv, struct block_list *src, int x, int y, int dir)
{
	if( skill_id == LG_OVERBRAND )
		return &skill_nounit_layout[overbrand_nounit_pos + dir];
	else if( skill_id == LG_OVERBRAND_BRANDISH )
		return &skill_nounit_layout[overbrand_brandish_nounit_pos + dir];

	ShowError("skill_get_nounit_layout: unknown no-unit layout for skill %d (level %d)\n", skill_id, skill_lv);
	return &skill_nounit_layout[0];
}

/**
 * Stores temporary values.
 * Common usages:
 * [0] holds number of targets in area
 * [1] holds the id of the original target
 * [2] counts how many targets have been processed. Counter is added in skill_area_sub if the foreach function flag is: flag&(SD_SPLASH|SD_PREAMBLE)
 */
static int skill_area_temp[8];

/*==========================================
 * Add effect to skill when hit succesfully target
 *------------------------------------------*/
int skill_additional_effect(struct block_list *src, struct block_list *bl, uint16 skill_id, uint16 skill_lv, int attack_type, int dmg_lv, unsigned int tick)
{
	struct map_session_data *sd, *dstsd;
	struct mob_data *md, *dstmd;
	struct homun_data *hd;
	struct status_data *sstatus, *tstatus;
	struct status_change *sc, *tsc;
	enum sc_type status;
	uint8 lv = 0;
	int rate;

	nullpo_ret(src);
	nullpo_ret(bl);

	if( skill_id && !skill_lv )
		return 0; //Don't forget auto attacks! [Celest]

	if( dmg_lv < ATK_BLOCK )
		return 0; //Don't apply effect if miss

	sd = BL_CAST(BL_PC,src);
	md = BL_CAST(BL_MOB,src);
	hd = BL_CAST(BL_HOM,src);
	dstsd = BL_CAST(BL_PC,bl);
	dstmd = BL_CAST(BL_MOB,bl);

	sc = status_get_sc(src);
	tsc = status_get_sc(bl);
	sstatus = status_get_status_data(src);
	tstatus = status_get_status_data(bl);

	//Taekwon combos activate on traps, so we need to check them even for targets that don't have status
	if( src->type == BL_PC && !skill_id && !(attack_type&BF_SKILL) && sc ) {
		if( sc->data[SC_READYSTORM] && sc_start4(src,src,SC_COMBO,15,TK_STORMKICK,0,2,0,2000 - 4 * sstatus->agi - 2 * sstatus->dex) )
			; //Stance triggered
		else if( sc->data[SC_READYDOWN] && sc_start4(src,src,SC_COMBO,15,TK_DOWNKICK,0,2,0,2000 - 4 * sstatus->agi - 2 * sstatus->dex) )
			; //Stance triggered
		else if( sc->data[SC_READYTURN] && sc_start4(src,src,SC_COMBO,15,TK_TURNKICK,0,2,0,2000 - 4 * sstatus->agi - 2 * sstatus->dex) )
			; //Stance triggered
		else if( sc->data[SC_READYCOUNTER] ) { //Additional chance from SG_FRIEND [Komurka]
			rate = 20;
			if( sc->data[SC_SKILLRATE_UP] && sc->data[SC_SKILLRATE_UP]->val1 == TK_COUNTER ) {
				rate += rate * sc->data[SC_SKILLRATE_UP]->val2 / 100;
				status_change_end(src,SC_SKILLRATE_UP,INVALID_TIMER);
			}
			sc_start4(src,src,SC_COMBO,rate,TK_COUNTER,0,2,0,2000 - 4 * sstatus->agi - 2 * sstatus->dex); //Stance triggered
		}
	}

	//Skill additional effect is about adding effects to the target
	//So if the target can't be inflicted with statuses, this is pointless
	if( !tsc )
		return 0;

	if( sd ) { //These statuses would be applied anyway even if the damage was blocked by some skills [Inkfish]
		if( !battle_skill_check_no_cardfix_atk(skill_id) ) { //Trigger status effects
			enum sc_type type;
			uint8 i, sc_flag = SCFLAG_NONE;
			unsigned int time = 0;

			for( i = 0; i < MAX_PC_BONUS && sd->addeff[i].flag; i++ ) {
				rate = sd->addeff[i].rate;
				if( attack_type&BF_LONG )
					rate += sd->addeff[i].arrow_rate;
				if( !rate )
					continue;
				if( (sd->addeff[i].flag&(ATF_WEAPON|ATF_MAGIC|ATF_MISC)) != (ATF_WEAPON|ATF_MAGIC|ATF_MISC) ) { //Trigger has attack type consideration
					if( (sd->addeff[i].flag&ATF_WEAPON && attack_type&BF_WEAPON) ||
						(sd->addeff[i].flag&ATF_MAGIC && attack_type&BF_MAGIC) ||
						(sd->addeff[i].flag&ATF_MISC && attack_type&BF_MISC) ) ;
					else
						continue;
				}
				if( (sd->addeff[i].flag&(ATF_LONG|ATF_SHORT)) != (ATF_LONG|ATF_SHORT) ) { //Trigger has range consideration
					if( (sd->addeff[i].flag&ATF_LONG && !(attack_type&BF_LONG)) ||
						(sd->addeff[i].flag&ATF_SHORT && !(attack_type&BF_SHORT)) )
						continue; //Range failed
				}
				type = sd->addeff[i].sc;
				time = sd->addeff[i].duration;
				if( time ) //Fixed duration
					sc_flag = SCFLAG_FIXEDTICK;
				if( sd->addeff[i].flag&ATF_TARGET )
					status_change_start(src,bl,type,rate,7,0,0,0,time,sc_flag);
				if( sd->addeff[i].flag&ATF_SELF )
					status_change_start(src,src,type,rate,7,0,0,0,time,sc_flag);
			}
		}

		if( skill_id ) { //Trigger status effects on skills
			enum sc_type type;
			uint8 i, sc_flag = SCFLAG_NONE;
			unsigned int time = 0;

			for( i = 0; i < MAX_PC_BONUS && sd->addeff3[i].skill_id; i++ ) {
				if( skill_id != sd->addeff3[i].skill_id || !sd->addeff3[i].rate )
					continue;
				type = sd->addeff3[i].sc;
				time = sd->addeff3[i].duration;
				if( time )
					sc_flag = SCFLAG_FIXEDTICK;
				if( sd->addeff3[i].target&ATF_TARGET )
					status_change_start(src,bl,type,sd->addeff3[i].rate,7,0,0,0,time,sc_flag);
				if( sd->addeff3[i].target&ATF_SELF )
					status_change_start(src,src,type,sd->addeff3[i].rate,7,0,0,0,time,sc_flag);
			}
		}
	}

	if( dmg_lv < ATK_DEF ) //No damage, do nothing
		return 0;

	switch( skill_id ) {
		case 0: { //Normal attacks (no skill used)
				if( attack_type&BF_SKILL )
					break; //If a normal attack is a skill, it's splash damage [Inkfish]
				if( sd ) {
					if( pc_isfalcon(sd) && sd->status.weapon == W_BOW && (lv = pc_checkskill(sd,HT_BLITZBEAT)) > 0 &&
						rnd()%1000 < sstatus->luk * 10 / 3 + 1 ) { //Automatic trigger of Blitz Beat
						rate = (status_get_job_lv(src) + 9) / 10;
						skill_castend_damage_id(src,bl,HT_BLITZBEAT,(sd->class_&JOBL_THIRD ? lv : min(lv,rate)),tick,SD_LEVEL);
					}
					if( pc_iswug(sd) && (lv = pc_checkskill(sd,RA_WUGSTRIKE)) > 0 && rnd()%1000 < sstatus->luk * 10 / 3 + 1 )
						skill_castend_damage_id(src,bl,RA_WUGSTRIKE,lv,tick,SD_LEVEL); //Automatic trigger of Warg Strike [Jobbie]
					if( dstmd && sd->status.weapon != W_BOW && (lv = pc_checkskill(sd,RG_SNATCHER)) > 0 &&
						rnd()%1000 < 55 + lv * 15 + pc_checkskill(sd,TF_STEAL) * 10 ) { //Gank
						if( pc_steal_item(sd,bl,pc_checkskill(sd,TF_STEAL)) )
							clif_skill_nodamage(src,bl,TF_STEAL,lv,1);
						else
							clif_skill_fail(sd,RG_SNATCHER,USESKILL_FAIL_LEVEL,0,0);
					}
				}
				if( sc ) {
					struct status_change_entry *sce = NULL;

					//Enchant Poison gives a chance to poison attacked enemies
					if( (sce = sc->data[SC_ENCPOISON]) ) //NOTE: Don't use sc_start since chance comes in 1/10000 rate
						status_change_start(src,bl,SC_POISON,sce->val2,sce->val1,0,0,0,skill_get_time2(AS_ENCHANTPOISON,sce->val1),SCFLAG_NONE);
					if( (sce = sc->data[SC_EDP]) ) //Enchant Deadly Poison gives a chance to deadly poison attacked enemies
						sc_start(src,bl,SC_DPOISON,sce->val2,sce->val1,skill_get_time2(ASC_EDP,sce->val1));
				}
			}
			break;
		case SM_BASH: //BaseChance gets multiplied with BaseLevel/50.0; 500/50 simplifies to 10 [Playtester]
			if( skill_lv > 5 && sd && pc_checkskill(sd,SM_FATALBLOW) > 0 )
				status_change_start(src,bl,SC_STUN,(skill_lv - 5) * status_get_lv(src) * 10,skill_lv,0,0,0,skill_get_time2(SM_FATALBLOW,skill_lv),SCFLAG_NONE);
			break;
		case MER_CRASH:
			sc_start(src,bl,SC_STUN,6 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case AL_HOLYLIGHT:
			status_change_end(bl,SC_KYRIE,INVALID_TIMER);
			break;
		case AS_VENOMKNIFE: //Poison chance must be that of Envenom [Skotlex]
		case AS_SPLASHER:
			lv = (skill_id == AS_SPLASHER ? skill_lv : (sd ? pc_checkskill(sd,TF_POISON) : 10));
			sc_start(src,bl,SC_POISON,10 + lv * 4,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case AS_SONICBLOW:
			sc_start(src,bl,SC_STUN,10 + skill_lv * 2,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case WZ_FIREPILLAR:
			unit_set_walkdelay(bl,tick,skill_get_time2(skill_id,skill_lv),1);
			break;
		case MG_FROSTDIVER:
#ifndef RENEWAL
		case WZ_FROSTNOVA:
#endif
			if( !sc_start(src,bl,SC_FREEZE,min(35 + skill_lv * 3,60 + skill_lv),skill_lv,skill_get_time2(skill_id,skill_lv)) &&
				skill_id == MG_FROSTDIVER && sd )
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			break;
#ifdef RENEWAL
		case WZ_FROSTNOVA:
			sc_start(src,bl,SC_FREEZE,33 + skill_lv * 5,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
#endif
		case WZ_STORMGUST:
#ifdef RENEWAL //Storm Gust counter was dropped in renewal
			sc_start(src,bl,SC_FREEZE,65 - skill_lv * 5,skill_lv,skill_get_time2(skill_id,skill_lv));
#else
			//On third hit, there is a 150% to freeze the target
			if( tsc->sg_counter >= 3 &&
				sc_start(src,bl,SC_FREEZE,150,skill_lv,skill_get_time2(skill_id,skill_lv)) )
				tsc->sg_counter = 0;
			//Being it only resets on success it'd keep stacking and eventually overflowing on mvps, so we reset at a high value
			else if( tsc->sg_counter > 250 )
				tsc->sg_counter = 0;
#endif
			break;
		case WZ_METEOR:
			sc_start(src,bl,SC_STUN,3 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case WZ_VERMILION:
			sc_start(src,bl,SC_BLIND,min(4 * skill_lv,40),skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case WZ_HEAVENDRIVE:
			status_change_end(bl,SC_SV_ROOTTWIST,INVALID_TIMER);
			break;
		case HT_FREEZINGTRAP:
		case MA_FREEZINGTRAP:
			sc_start(src,bl,SC_FREEZE,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case HT_FLASHER:
			sc_start(src,bl,SC_BLIND,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case HT_LANDMINE:
		case MA_LANDMINE:
			sc_start(src,bl,SC_STUN,10,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case HT_SHOCKWAVE:
			status_percent_damage(src,bl,0,-(5 + skill_lv * 15),false);
			break;
		case HT_SANDMAN:
		case MA_SANDMAN:
			sc_start(src,bl,SC_SLEEP,40 + skill_lv * 10,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case TF_SPRINKLESAND:
			sc_start(src,bl,SC_BLIND,20,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case TF_THROWSTONE:
			if( !sc_start(src,bl,SC_STUN,3,skill_lv,skill_get_time(skill_id,skill_lv)) ) //Only blind if success
				sc_start(src,bl,SC_BLIND,3,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case NPC_DARKCROSS:
		case CR_HOLYCROSS:
			sc_start(src,bl,SC_BLIND,3 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case CR_GRANDCROSS: //Chance to cause blind status vs demon and undead element, but not against players
			attack_type |= BF_WEAPON|BF_LONG|BF_NORMAL; //Can trigger physical autospells
			if( bl->type != BL_PC && (battle_check_undead(tstatus->race,tstatus->def_ele) || tstatus->race == RC_DEMON) )
		//Fall through
		case NPC_GRANDDARKNESS:
			sc_start(src,bl,SC_BLIND,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case AM_DEMONSTRATION:
			skill_break_equip(src,bl,EQP_WEAPON,100 * skill_lv,BCT_ENEMY);
			break;
		case AM_ACIDTERROR:
			sc_start(src,bl,SC_BLEEDING,3 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			if( bl->type == BL_PC && rnd()%1000 < 10 * skill_get_time(skill_id,skill_lv) ) {
				skill_break_equip(src,bl,EQP_ARMOR,10000,BCT_ENEMY);
				clif_emotion(bl,E_OMG); //Emote icon still shows even there is no armor equip
			}
			break;
		case CR_SHIELDCHARGE:
			sc_start(src,bl,SC_STUN,15 + skill_lv * 5,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case PA_PRESSURE:
			status_percent_damage(src,bl,0,15 + skill_lv * 5,false);
		//Fall through
		case HW_GRAVITATION:
			attack_type |= BF_WEAPON|BF_LONG|BF_NORMAL; //Can trigger physical autospells
			break;
		case RG_RAID:
			sc_start(src,bl,SC_STUN,10 + skill_lv * 3,skill_lv,skill_get_time(skill_id,skill_lv));
			sc_start(src,bl,SC_BLIND,10 + skill_lv * 3,skill_lv,skill_get_time2(skill_id,skill_lv));
#ifdef RENEWAL
			sc_start(src,bl,SC_RAID,100,7,5000);
			break;
		 case RG_BACKSTAP:
			sc_start(src,bl,SC_STUN,5 + skill_lv * 2,skill_lv,skill_get_time(skill_id,skill_lv));
#endif
			break;
		case BA_FROSTJOKER:
			sc_start(src,bl,SC_FREEZE,15 + skill_lv * 5,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case DC_SCREAM:
			sc_start(src,bl,SC_STUN,25 + skill_lv * 5,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case BD_LULLABY: //(Custom chance) "Chance is increased with INT" [iRO Wiki]
			sc_start(src,bl,SC_SLEEP,15 + sstatus->int_ / 3,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case DC_UGLYDANCE:
			rate = 5 + skill_lv * 5;
			rate += (sd ? pc_checkskill(sd,DC_DANCINGLESSON) + 5 : 15);
			status_zap(bl,0,rate);
			break;
		case SL_STUN:
			if( tstatus->size == SZ_MEDIUM ) //Only stuns mid-sized mobs
				sc_start(src,bl,SC_STUN,30 + skill_lv * 10,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case NPC_PETRIFYATTACK:
			sc_start4(src,bl,status_skill2sc(skill_id),20 * skill_lv,
				skill_lv,0,0,skill_get_time(skill_id,skill_lv),skill_get_time2(skill_id,skill_lv));
			break;
		case NPC_CURSEATTACK:
		case NPC_SLEEPATTACK:
		case NPC_BLINDATTACK:
		case NPC_POISON:
		case NPC_SILENCEATTACK:
		case NPC_BLEEDING:
		case NPC_STUNATTACK:
			sc_start(src,bl,status_skill2sc(skill_id),20 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case NPC_ACIDBREATH:
		case NPC_ICEBREATH:
			sc_start(src,bl,status_skill2sc(skill_id),70,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case NPC_MENTALBREAKER:
			//Based on observations by Tharis, Mental Breaker should do SP damage
			//Equal to MATK * Skill Level
			rate = status_get_matk(src,2) * skill_lv;
			status_zap(bl,0,rate);
			break;
		//Equipment breaking monster skills [Celest]
		case NPC_WEAPONBRAKER:
			skill_break_equip(src,bl,EQP_WEAPON,150 * skill_lv,BCT_ENEMY);
			break;
		case NPC_ARMORBRAKE:
			skill_break_equip(src,bl,EQP_ARMOR,150 * skill_lv,BCT_ENEMY);
			break;
		case NPC_HELMBRAKE:
			skill_break_equip(src,bl,EQP_HELM,150 * skill_lv,BCT_ENEMY);
			break;
		case NPC_SHIELDBRAKE:
			skill_break_equip(src,bl,EQP_SHIELD,150 * skill_lv,BCT_ENEMY);
			break;
		case CH_TIGERFIST:
			sc_start(src,bl,SC_STOP,10 + skill_lv * 10,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case LK_SPIRALPIERCE:
		case ML_SPIRALPIERCE:
			if( dstsd || dstmd )
				sc_start(src,bl,SC_STOP,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case PF_FOGWALL:
			if( bl->id != src->id && !tsc->data[SC_DELUGE] )
				sc_start(src,bl,SC_BLIND,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case LK_HEADCRUSH: //Headcrush has chance of causing Bleeding status, except on demon and undead element
			if( !(battle_check_undead(tstatus->race,tstatus->def_ele) || tstatus->race == RC_DEMON) )
				sc_start(src,bl,SC_BLEEDING,50,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case LK_JOINTBEAT:
			status = status_skill2sc(skill_id);
			if( tsc->jb_flag ) {
				sc_start2(src,bl,status,5 + skill_lv * 5,skill_lv,(tsc->jb_flag&BREAK_FLAGS),skill_get_time2(skill_id,skill_lv));
				tsc->jb_flag = 0;
			}
			break;
		case ASC_METEORASSAULT:
			switch( rnd()%3 ) { //Any enemies hit by this skill will receive Stun, Blind, or Bleeding status ailment with a 5%+skill_lv*5% chance
				case 0:
					sc_start(src,bl,SC_BLIND,5 + skill_lv * 5,skill_lv,skill_get_time2(skill_id,1));
					break;
				case 1:
					sc_start(src,bl,SC_STUN,5 + skill_lv * 5,skill_lv,skill_get_time2(skill_id,2));
					break;
				case 2:
					sc_start(src,bl,SC_BLEEDING,5 + skill_lv * 5,skill_lv,skill_get_time2(skill_id,3));
					break;
			}
			break;
		case HW_NAPALMVULCAN:
			sc_start(src,bl,SC_CURSE,5 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case WS_CARTTERMINATION: //Cart termination
			sc_start(src,bl,SC_STUN,5 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case CR_ACIDDEMONSTRATION:
			skill_break_equip(src,bl,EQP_WEAPON|EQP_ARMOR,100 * skill_lv,BCT_ENEMY);
			break;
		case TK_DOWNKICK:
			sc_start(src,bl,SC_STUN,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case TK_JUMPKICK:
			if( dstsd && dstsd->class_ != MAPID_SOUL_LINKER && !tsc->data[SC_PRESERVE] ) { //Debuff the following statuses
				status_change_end(bl,SC_SPIRIT,INVALID_TIMER);
				status_change_end(bl,SC_ADRENALINE2,INVALID_TIMER);
				status_change_end(bl,SC_KAITE,INVALID_TIMER);
				status_change_end(bl,SC_KAAHI,INVALID_TIMER);
				status_change_end(bl,SC_ONEHAND,INVALID_TIMER);
				status_change_end(bl,SC_ASPDPOTION2,INVALID_TIMER);
				//status_change_end(bl,SC_SOULGOLEM,INVALID_TIMER);
				//status_change_end(bl,SC_SOULSHADOW,INVALID_TIMER);
				//status_change_end(bl,SC_SOULFALCON,INVALID_TIMER);
				//status_change_end(bl,SC_SOULFAIRY,INVALID_TIMER);
			}
			break;
		case TK_TURNKICK:
		case MO_BALKYOUNG: //NOTE: attack_type is passed as BF_WEAPON for the actual target, BF_MISC for the splash-affected mobs
			if( attack_type&BF_MISC ) //70% base stun chance
				sc_start(src,bl,SC_STUN,70,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case GS_BULLSEYE: //0.1% coma rate
			if( tstatus->race == RC_BRUTE || tstatus->race == RC_DEMIHUMAN )
				status_change_start(src,bl,SC_COMA,10,skill_lv,0,0,0,0,SCFLAG_NONE);
			break;
		case GS_PIERCINGSHOT:
			sc_start(src,bl,SC_BLEEDING,3 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case NJ_HYOUSYOURAKU:
			sc_start(src,bl,SC_FREEZE,10 + skill_lv * 10,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case GS_FLING:
			sc_start(src,bl,SC_FLING,100,(sd ? sd->spiritball_old : 5),skill_get_time(skill_id,skill_lv));
			break;
		case GS_DISARM:
			if( skill_strip_equip(src,bl,skill_id,skill_lv) )
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;
		case NPC_EVILLAND:
			sc_start(src,bl,SC_BLIND,5 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case NPC_HELLJUDGEMENT:
			sc_start(src,bl,SC_CURSE,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case NPC_CRITICALWOUND:
			sc_start(src,bl,SC_CRITICALWOUND,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case NPC_FIRESTORM:
			sc_start(src,bl,SC_BURNT,100,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case RK_HUNDREDSPEAR:
			if( rnd()%100 < 10 + skill_lv * 3 ) {
				if( !(lv = (sd ? pc_checkskill(sd,KN_SPEARBOOMERANG) : 5)) )
					break; //Spear Boomerang auto cast chance only works if you have it
				skill_blown(src,bl,6,-1,0);
				skill_castend_damage_id(src,bl,KN_SPEARBOOMERANG,lv,tick,0);
			}
			break;
		case RK_WINDCUTTER:
			sc_start(src,bl,SC_FEAR,3 + skill_lv * 2,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case RK_DRAGONBREATH:
			sc_start(src,bl,SC_BURNING,15,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case RK_DRAGONBREATH_WATER:
			sc_start(src,bl,SC_FREEZING,15,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case AB_ADORAMUS:
			rate = status_get_job_lv(src) / 2 + skill_lv * 4;
			if (sc_start(src,bl,SC_ADORAMUS,rate,skill_lv,skill_get_time2(skill_id,skill_lv)))
				sc_start(src,bl,SC_BLIND,100,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case WL_CRIMSONROCK:
			sc_start(src,bl,SC_STUN,40,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case NPC_COMET:
		case WL_COMET:
			sc_start(src,bl,SC_BURNING,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case WL_EARTHSTRAIN:
			skill_strip_equip(src,bl,skill_id,skill_lv);
			break;
		case WL_FROSTMISTY:
			rate = 2 + (skill_lv - 1) * status_get_job_lv(src) / 7; //Job level bonus [exneval]
			rate += 25 + skill_lv * 5;
			sc_start(src,bl,SC_FREEZING,rate,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case WL_JACKFROST:
		case NPC_JACKFROST:
			sc_start(src,bl,SC_FREEZE,200,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case RA_WUGBITE:
			if( sd ) {
				rate = 50 + skill_lv * 10 + pc_checkskill(sd,RA_TOOTHOFWUG) * 2;
				sc_start(src,bl,SC_BITE,rate,skill_lv,skill_get_time(skill_id,skill_lv) + pc_checkskill(sd,RA_TOOTHOFWUG) * 500);
			}
			break;
		case RA_AIMEDBOLT:
			status_change_end(bl,SC_ANKLE,INVALID_TIMER);
			status_change_end(bl,SC_ELECTRICSHOCKER,INVALID_TIMER);
			status_change_end(bl,SC_BITE,INVALID_TIMER);
			break;
		case RA_SENSITIVEKEEN:
			if( rnd()%100 < 8 * skill_lv && sd )
				skill_castend_damage_id(src,bl,RA_WUGBITE,pc_checkskill(sd,RA_WUGBITE),tick,SD_ANIMATION);
			break;
		case RA_FIRINGTRAP:
			sc_start(src,bl,SC_BURNING,50 + skill_lv * 10,skill_lv,skill_get_time2(skill_id,skill_lv));
			attack_type |= BF_WEAPON|BF_SHORT|BF_NORMAL;
			break;
		case RA_ICEBOUNDTRAP:
			sc_start(src,bl,SC_FREEZING,50 + skill_lv * 10,skill_lv,skill_get_time2(skill_id,skill_lv));
			attack_type |= BF_WEAPON|BF_SHORT|BF_NORMAL;
			break;
		case NC_PILEBUNKER:
			if( rnd()%100 < 25 + skill_lv * 15 ) {
				status_change_end(bl,SC_ASSUMPTIO,INVALID_TIMER);
				status_change_end(bl,SC_STEELBODY,INVALID_TIMER);
				status_change_end(bl,SC_GT_CHANGE,INVALID_TIMER);
				status_change_end(bl,SC_GT_REVITALIZE,INVALID_TIMER);
				status_change_end(bl,SC_AUTOGUARD,INVALID_TIMER);
				status_change_end(bl,SC_REFLECTSHIELD,INVALID_TIMER);
				status_change_end(bl,SC_DEFENDER,INVALID_TIMER);
				status_change_end(bl,SC_REFLECTDAMAGE,INVALID_TIMER);
				status_change_end(bl,SC_PRESTIGE,INVALID_TIMER);
				status_change_end(bl,SC_BANDING,INVALID_TIMER);
			}
			break;
		case NC_FLAMELAUNCHER:
			sc_start(src,bl,SC_BURNING,20 + skill_lv * 10,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case NC_COLDSLOWER:
			//Status chances are applied officially through a check
			//The skill first trys to give the frozen status to targets that are hit
			sc_start(src,bl,SC_FREEZE,10 * skill_lv,skill_lv,skill_get_time(skill_id,skill_lv));
			if( !tsc->data[SC_FREEZE] ) //If it fails to give the frozen status, it will attempt to give the freezing status
				sc_start(src,bl,SC_FREEZING,20 + skill_lv * 10,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case NC_POWERSWING:
			status_change_start(src,bl,SC_STUN,1000,skill_lv,0,0,0,skill_get_time(skill_id,skill_lv),SCFLAG_FIXEDTICK|SCFLAG_FIXEDRATE);
			if( rnd()%100 < 5 * skill_lv && sd && (sd->status.weapon == W_1HAXE || sd->status.weapon == W_2HAXE) )
				skill_castend_damage_id(src,bl,NC_AXEBOOMERANG,pc_checkskill(sd,NC_AXEBOOMERANG),tick,0);
			break;
		case NC_MAGMA_ERUPTION:
			sc_start(src,bl,SC_STUN,90,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case NC_MAGMA_ERUPTION_DOTDAMAGE:
			sc_start(src,bl,SC_BURNING,10 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case GC_WEAPONCRUSH:
			skill_castend_nodamage_id(src,bl,skill_id,skill_lv,tick,BCT_ENEMY);
			break;
		case GC_VENOMPRESSURE: {
				struct status_change_entry *sce = NULL;

				if( sc && (sce = sc->data[SC_POISONINGWEAPON]) && rnd()%100 < 70 + 5 * skill_lv ) {
					sc_start(src,bl,(sc_type)sce->val2,100,sce->val1,skill_get_time2(GC_POISONINGWEAPON,1));
					status_change_end(src,SC_POISONINGWEAPON,INVALID_TIMER);
					clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				}
			}
			break;
		case LG_SHIELDPRESS:
			status_change_start(src,bl,SC_STUN,(30 + skill_lv * 8 + sstatus->dex / 10 +
				status_get_job_lv(src) / 4) * 100,skill_lv,0,0,0,skill_get_time(skill_id,skill_lv),SCFLAG_FIXEDTICK);
			break;
		case LG_PINPOINTATTACK:
			rate = (sd ? pc_checkskill(sd,LG_PINPOINTATTACK) : 5) * 5 + (sstatus->agi + status_get_lv(src)) / 10;
			switch( skill_lv ) {
				case 1:
					sc_start(src,bl,SC_BLEEDING,rate,skill_lv,skill_get_time(skill_id,skill_lv));
					break;
				case 2:
					skill_break_equip(src,bl,EQP_HELM,100 * rate,BCT_ENEMY);
					break;
				case 3:
					skill_break_equip(src,bl,EQP_SHIELD,100 * rate,BCT_ENEMY);
					break;
				case 4:
					skill_break_equip(src,bl,EQP_ARMOR,100 * rate,BCT_ENEMY);
					break;
				case 5:
					skill_break_equip(src,bl,EQP_WEAPON,100 * rate,BCT_ENEMY);
					break;
			}
			break;
		case LG_MOONSLASHER:
			rate = 32 + skill_lv * 8;
			//Uses skill_addtimerskill to avoid damage and setsit packet overlaping
			//Officially clif_setsit is received about 500 ms after damage packet
			if( rnd()%100 < rate && dstsd )
				skill_addtimerskill(src,tick + 500,bl->id,0,0,skill_id,skill_lv,BF_WEAPON,0);
			else if( dstmd )
				sc_start(src,bl,SC_STOP,100,skill_lv,skill_get_time(skill_id,skill_lv) + 1000 * rnd()%3);
			break;
		case LG_RAYOFGENESIS: //50% chance to cause Blind on Undead and Demon monsters
			if( battle_check_undead(tstatus->race,tstatus->def_ele) || tstatus->race == RC_DEMON )
				sc_start(src,bl,SC_BLIND,50,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case LG_EARTHDRIVE:
			skill_break_equip(src,src,EQP_SHIELD,100 * skill_lv,BCT_SELF);
			sc_start(src,bl,SC_EARTHDRIVE,100,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case LG_HESPERUSLIT:
			if( sc && sc->data[SC_BANDING] ) {
				if( (battle_config.hesperuslit_bonus_stack && sc->data[SC_BANDING]->val2 >= 4) || sc->data[SC_BANDING]->val2 == 4 )
					status_change_start(src,bl,SC_STUN,10000,skill_lv,0,0,0,rnd_value(4000,8000),SCFLAG_FIXEDTICK);
				if( ((battle_config.hesperuslit_bonus_stack && sc->data[SC_BANDING]->val2 >= 6) || sc->data[SC_BANDING]->val2 == 6) &&
					(sd ? (lv = pc_checkskill(sd,LG_PINPOINTATTACK)) : 5) > 0 )
					skill_castend_damage_id(src,bl,LG_PINPOINTATTACK,rnd_value(1,lv),tick,0);
			}
			break;
		case SR_DRAGONCOMBO:
			sc_start(src,bl,SC_STUN,skill_lv,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case SR_FALLENEMPIRE:
			sc_start(src,bl,SC_FALLENEMPIRE,100,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case SR_WINDMILL:
			if( dstsd )
				skill_addtimerskill(src,tick + 500,bl->id,0,0,skill_id,skill_lv,BF_WEAPON,0);
			else if( dstmd )
				sc_start(src,bl,SC_STUN,100,skill_lv,1000 * rnd()%4);
			break;
		case SR_GENTLETOUCH_QUIET: //[(Skill Level x 5) + (Caster's DEX + Caster's Base Level) / 10]
			sc_start(src,bl,SC_SILENCE,(sstatus->dex + status_get_lv(src)) / 10 + skill_lv * 5,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case SR_EARTHSHAKER:
			sc_start(src,bl,SC_STUN,25 + skill_lv * 5,skill_lv,skill_get_time(skill_id,skill_lv));
			status_change_end(bl,SC_SV_ROOTTWIST,INVALID_TIMER);
			break;
		case SR_HOWLINGOFLION:
			status_change_end(bl,SC_SWINGDANCE,INVALID_TIMER);
			status_change_end(bl,SC_SYMPHONYOFLOVER,INVALID_TIMER);
			status_change_end(bl,SC_MOONLITSERENADE,INVALID_TIMER);
			status_change_end(bl,SC_RUSHWINDMILL,INVALID_TIMER);
			status_change_end(bl,SC_ECHOSONG,INVALID_TIMER);
			status_change_end(bl,SC_HARMONIZE,INVALID_TIMER);
			status_change_end(bl,SC_NETHERWORLD,INVALID_TIMER);
			status_change_end(bl,SC_VOICEOFSIREN,INVALID_TIMER);
			status_change_end(bl,SC_GLOOMYDAY,INVALID_TIMER);
			status_change_end(bl,SC_SONGOFMANA,INVALID_TIMER);
			status_change_end(bl,SC_DANCEWITHWUG,INVALID_TIMER);
			status_change_end(bl,SC_MELODYOFSINK,INVALID_TIMER);
			status_change_end(bl,SC_BEYONDOFWARCRY,INVALID_TIMER);
			status_change_end(bl,SC_UNLIMITEDHUMMINGVOICE,INVALID_TIMER);
			status_change_end(bl,SC_FRIGG_SONG,INVALID_TIMER);
			if( tsc && tsc->data[SC_SATURDAYNIGHTFEVER] ) {
				tsc->data[SC_SATURDAYNIGHTFEVER]->val2 = 0;
				status_change_end(bl,SC_SATURDAYNIGHTFEVER,INVALID_TIMER);
			}
			sc_start(src,bl,SC_FEAR,5 + skill_lv * 5,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case SO_EARTHGRAVE:
			sc_start(src,bl,SC_BLEEDING,5 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case SO_DIAMONDDUST:
			rate = 5 + skill_lv * 5;
			if( sc && sc->data[SC_COOLER_OPTION] )
				rate += sc->data[SC_COOLER_OPTION]->val3 / 5;
			sc_start(src,bl,SC_CRYSTALIZE,rate,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case SO_VARETYR_SPEAR:
			sc_start(src,bl,SC_STUN,5 * skill_lv,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case GN_DEMONIC_FIRE:
			sc_start(src,bl,SC_BURNING,4 + 4 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case GN_FIRE_EXPANSION_ACID:
			lv = max((sd ? pc_checkskill(sd,CR_ACIDDEMONSTRATION) : 10),5);
			skill_break_equip(src,bl,EQP_WEAPON|EQP_ARMOR,100 * lv,BCT_ENEMY);
			break;
		case GN_SLINGITEM_RANGEMELEEATK:
			if( sd ) {
				uint16 baselv = status_get_lv(src),
					joblv = status_get_job_lv(src),
					tbaselv = status_get_lv(bl);

				switch( sd->itemid ) {
					case ITEMID_COCONUT_BOMB: //Causes stun and bleeding
						sc_start(src,bl,SC_STUN,5 + joblv / 2,skill_lv,1000 * joblv / 3);
						sc_start(src,bl,SC_BLEEDING,3 + joblv / 2,skill_lv,1000 * baselv / 4 + joblv / 3);
						break;
					case ITEMID_MELON_BOMB: //Reduces ASPD and movement speed
						sc_start4(src,bl,SC_MELON_BOMB,100,skill_lv,20 + joblv,10 + joblv / 2,0,1000 * baselv / 4);
						break;
					case ITEMID_BANANA_BOMB: { //Reduces LUK and chance to force sit, must do the force sit success chance first before LUK reduction
							uint16 dur = (battle_config.banana_bomb_duration ? battle_config.banana_bomb_duration : 1000 * joblv / 4);

							sc_start(src,bl,SC_BANANA_BOMB_SITDOWN,baselv + joblv + sstatus->dex / 6 - tbaselv - tstatus->agi / 4 - tstatus->luk / 5,skill_lv,dur);
							sc_start(src,bl,SC_BANANA_BOMB,100,skill_lv,30000);
						}
						break;
				}
				sd->itemid = -1;
			}
			break;
		case GN_HELLS_PLANT_ATK:
			sc_start(src,bl,SC_STUN, 20 + skill_lv * 10,skill_lv,skill_get_time(skill_id,skill_lv));
			sc_start(src,bl,SC_BLEEDING,5 + skill_lv * 5,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case EL_WIND_SLASH:
			sc_start(src,bl,SC_BLEEDING,25,skill_lv,skill_get_time(skill_id,skill_lv)); //Custom
			break;
		case EL_STONE_HAMMER:
			sc_start(src,bl,SC_STUN,25,skill_lv,skill_get_time(skill_id,skill_lv)); //Custom
			break;
		case EL_ROCK_CRUSHER:
		case EL_ROCK_CRUSHER_ATK:
			sc_start(src,bl,status_skill2sc(skill_id),50,skill_lv,skill_get_time(EL_ROCK_CRUSHER,skill_lv));
			break;
		case EL_TYPOON_MIS:
		case EL_TYPOON_MIS_ATK:
			sc_start(src,bl,SC_SILENCE,25,skill_lv,skill_get_time(EL_TYPOON_MIS,skill_lv)); //Custom
			break;
		case SJ_FULLMOONKICK:
			sc_start(src,bl,SC_BLIND,15 + skill_lv * 5,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case SJ_STAREMPEROR:
			sc_start(src,bl,SC_SILENCE,50 + skill_lv * 10,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case SP_CURSEEXPLOSION:
			status_change_end(bl,SC_CURSE,INVALID_TIMER);
			break;
		case SP_SHA:
			sc_start(src,bl,SC_SP_SHA,100,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case KO_JYUMONJIKIRI:
			sc_start(src,bl,SC_JYUMONJIKIRI,100,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case SP_SOULEXPLOSION:
		case KO_SETSUDAN:
			status_change_end(bl,SC_SPIRIT,INVALID_TIMER);
			status_change_end(bl,SC_SOULGOLEM,INVALID_TIMER);
			status_change_end(bl,SC_SOULSHADOW,INVALID_TIMER);
			status_change_end(bl,SC_SOULFALCON,INVALID_TIMER);
			status_change_end(bl,SC_SOULFAIRY,INVALID_TIMER);
			break;
		case KO_MAKIBISHI:
			status_change_start(src,bl,SC_STUN,1000 * skill_lv,skill_lv,0,0,0,skill_get_time2(skill_id,skill_lv),SCFLAG_FIXEDTICK);
			break;
		case MH_NEEDLE_OF_PARALYZE:
			sc_start(src,bl,SC_PARALYSIS,30 + skill_lv * 5,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case MH_XENO_SLASHER:
			sc_start(src,bl,SC_BLEEDING,skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case MH_SILVERVEIN_RUSH:
			sc_start(src,bl,SC_STUN,5 * skill_lv,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case MH_MIDNIGHT_FRENZY: {
				int spiritball = (hd ? hd->homunculus.spiritball : 1);

				sc_start(src,bl,SC_FEAR,spiritball * (10 + skill_lv * 2),skill_lv,skill_get_time(skill_id,skill_lv));
			}
			break;
		case MH_TINDER_BREAKER: {
				int duration = max(skill_lv,(status_get_str(src) / 7 - status_get_str(bl) / 10)) * 1000;

				sc_start(src,bl,SC_TINDER_BREAKER,100,skill_lv,duration);
				sc_start(src,src,SC_TINDER_BREAKER,100,skill_lv,duration);
			}
			break;
		case MH_CBC:
			if( hd ) {
				int HPdamage = 400 * skill_lv + 4 * hd->homunculus.level;
				int SPdamage = 10 * skill_lv + hd->homunculus.level / 5 + hd->homunculus.dex / 10;
				int duration = max(skill_lv,(status_get_str(src) / 7 - status_get_str(bl) / 10)) * 1000;

				//A bonus is applied to HPdamage using SPdamage formula x10 if entity is a monster
				if( !(bl->type&BL_CONSUME) ) {
					HPdamage += 10 * SPdamage;
					SPdamage = 0; //Signals later that entity is a monster
				}
				sc_start4(src,bl,SC_CBC,100,skill_lv,HPdamage,SPdamage,0,duration);
				status_change_end(bl,SC_TINDER_BREAKER,INVALID_TIMER);
			}
			break;
		case MH_EQC:
			if( hd ) {
				sc_start(src,bl,SC_STUN,100,skill_lv,1000 * hd->homunculus.level / 50 + 500 * skill_lv);
				sc_start(src,bl,SC_EQC,100,skill_lv,skill_get_time(skill_id,skill_lv));
				status_change_end(bl,SC_TINDER_BREAKER,INVALID_TIMER);
			}
			break;
		case MH_STAHL_HORN:
			sc_start(src,bl,SC_STUN,20 + skill_lv * 2,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case MH_LAVA_SLIDE:
			sc_start(src,bl,SC_BURNING,5 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case GN_ILLUSIONDOPING:
			if( sc_start(src,bl,SC_ILLUSIONDOPING,100 - skill_lv * 10,skill_lv,skill_get_time(skill_id,skill_lv)) )
				sc_start(src,bl,SC_HALLUCINATION,100,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case RL_MASS_SPIRAL:
			sc_start(src,bl,SC_BLEEDING,30 + skill_lv * 10,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
		case RL_BANISHING_BUSTER: {
				uint16 i;

				if( rnd()%100 >= 50 + 10 * skill_lv ) {
					if( sd )
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				if( status_isimmune(bl) ||
					(dstsd && (dstsd->class_&MAPID_UPPERMASK) == MAPID_SOUL_LINKER) ||
					(tsc->data[SC_SPIRIT] && tsc->data[SC_SPIRIT]->val2 == SL_ROGUE) )
					break;
				if( dstsd )
					pc_bonus_script_clear(dstsd,BSF_REM_ON_DISPELL);
				for( i = 0; i < SC_MAX; i++ ) {
					if( !tsc->data[i] )
						continue;
					if( !(status_get_sc_type((sc_type)i)&SC_REM_DISPELL) )
						continue;
					switch( i ) {
						case SC_SILENCE:
							if( tsc->data[i]->val4 )
								continue;
							break;
						case SC_WHISTLE:
						case SC_ASSNCROS:
						case SC_POEMBRAGI:
						case SC_APPLEIDUN:
						case SC_HUMMING:
						case SC_DONTFORGETME:
						case SC_FORTUNE:
						case SC_SERVICE4U:
							if( !battle_config.dispel_song || !tsc->data[i]->val4 )
								continue;
							break;
						case SC_ASSUMPTIO:
							if( bl->type == BL_MOB )
								continue;
							break;
						case SC_BERSERK:
						case SC_SATURDAYNIGHTFEVER:
							tsc->data[i]->val2 = 0;
							break;
					}
					status_change_end(bl,(sc_type)i,INVALID_TIMER);
				}
			}
			break;
		case RL_B_TRAP:
			status_change_end(bl,SC_B_TRAP,INVALID_TIMER);
			break;
		case RL_S_STORM: {
				short dex_effect = (status_get_dex(src) - 100) / 4;
				short agi_effect = (status_get_agi(bl) - 100) / 4;
				short added_chance = 0;

				dex_effect = max(dex_effect,0);
				agi_effect = max(agi_effect,0);
				added_chance = dex_effect - (agi_effect + status_get_lv(bl) / 10);
				added_chance = max(added_chance,0);
				skill_break_equip(src,bl,EQP_HELM,100 * (5 * skill_lv + added_chance),BCT_ENEMY);
			}
			break;
		case RL_SLUGSHOT:
			sc_start(src,bl,SC_STUN,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case RL_AM_BLAST:
			sc_start(src,bl,SC_ANTI_M_BLAST,20 + skill_lv * 10,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case RL_HAMMER_OF_GOD:
			sc_start(src,bl,SC_STUN,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case SU_SCRATCH:
			sc_start(src,bl,SC_BLEEDING,skill_lv * 10 + 70,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case SU_SV_STEMSPEAR:
			sc_start(src,bl,SC_BLEEDING,10,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case SU_CN_METEOR2:
			sc_start(src,bl,SC_CURSE,20,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case SU_SCAROFTAROU:
			sc_start(src,bl,SC_BITESCAR,10,skill_lv,skill_get_time(skill_id,skill_lv)); //Custom
			status_change_start(src,bl,SC_STUN,100,skill_lv,0,0,0,skill_get_time2(skill_id,skill_lv),SCFLAG_FIXEDTICK); //Custom
			break;
		case SU_LUNATICCARROTBEAT2:
			sc_start(src,bl,SC_STUN,20,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case MER_INVINCIBLEOFF2:
			sc_start(src,bl,SC_INVINCIBLEOFF,100,skill_lv,skill_get_time(skill_id,skill_lv));
			break;
	} //End of switch skill_id

	//Pass heritage to Master for status causing effects [Skotlex]
	if( md && battle_config.summons_trigger_autospells && md->master_id && md->special_state.ai ) {
		sd = map_id2sd(md->master_id);
		src = (sd ? &sd->bl : src);
	}

	if( attack_type&BF_WEAPON ) {
		if( sd ) {
			if( sd->special_state.bonus_coma ) { //Coma
				rate = sd->weapon_coma_ele[tstatus->def_ele] + sd->weapon_coma_ele[ELE_ALL];
				rate += sd->weapon_coma_race[tstatus->race] + sd->weapon_coma_race[RC_ALL];
				rate += sd->weapon_coma_class[tstatus->class_] + sd->weapon_coma_class[CLASS_ALL];
				if( rate )
					status_change_start(src,bl,SC_COMA,rate,0,0,0,0,0,SCFLAG_NONE);
			}
			if( battle_config.equip_self_break_rate ) { //Breaking Equipment
				rate = battle_config.equip_natural_break_rate;
				if( sc ) {
					if( sc->data[SC_OVERTHRUST] )
						rate += 10;
					if( sc->data[SC_MAXOVERTHRUST] )
						rate += 10;
				}
				if( rate ) //Self weapon breaking
					skill_break_equip(src,src,EQP_WEAPON,rate,BCT_SELF);
			}
		}
		//Acid Terror/Cart Termination/Tomahawk won't trigger breaking data
		if( battle_config.equip_skill_break_rate && skill_id != AM_ACIDTERROR && skill_id != WS_CARTTERMINATION && skill_id != ITM_TOMAHAWK ) {
			rate = 0;
			if( sd )
				rate += sd->bonus.break_weapon_rate;
			if( sc && sc->data[SC_MELTDOWN] )
				rate += sc->data[SC_MELTDOWN]->val2;
			if( rate ) //Target weapon breaking
				skill_break_equip(src,bl,EQP_WEAPON,rate,BCT_ENEMY);
			rate = 0;
			if( sd )
				rate += sd->bonus.break_armor_rate;
			if( sc && sc->data[SC_MELTDOWN] )
				rate += sc->data[SC_MELTDOWN]->val3;
			if( rate ) //Target armor breaking
				skill_break_equip(src,bl,EQP_ARMOR,rate,BCT_ENEMY);
		}
	}

	if( sd ) {
		int i;

		if( !skill_id ) { //This effect does not work with skills
			if( !status_has_mode(tstatus,MD_STATUS_IMMUNE) && sd->bonus.classchange && rnd()%10000 < sd->bonus.classchange ) {
				int mob_id = mob_get_random_id(MOBG_Branch_Of_Dead_Tree,RMF_DB_RATE,0);

				if( mob_id && mobdb_checkid(mob_id) && dstmd )
					mob_class_change(dstmd,mob_id); //Polymorph
			}
			if( sd->def_set_race[tstatus->race].rate )
				status_change_start(src,bl,SC_DEFSET_NUM,sd->def_set_race[tstatus->race].rate,sd->def_set_race[tstatus->race].value,0,0,0,sd->def_set_race[tstatus->race].tick,SCFLAG_FIXEDTICK);
			if( sd->mdef_set_race[tstatus->race].rate )
				status_change_start(src,bl,SC_MDEFSET_NUM,sd->mdef_set_race[tstatus->race].rate,sd->mdef_set_race[tstatus->race].value,0,0,0,sd->mdef_set_race[tstatus->race].tick,SCFLAG_FIXEDTICK);
			if( sd->norecover_state_race[tstatus->race].rate )
				status_change_start(src,bl,SC_NORECOVER_STATE,sd->norecover_state_race[tstatus->race].rate,0,0,0,0,sd->norecover_state_race[tstatus->race].tick,SCFLAG_NONE);
			if( sc && sc->count ) {
				uint16 as_skill_id;
				uint16 as_skill_lv;

				if( !status_isdead(bl) && rnd()%100 < status_get_job_lv(src) / 2 ) {
					if( sc->data[SC_WILD_STORM_OPTION] )
						as_skill_id = sc->data[SC_WILD_STORM_OPTION]->val2;
					else if( sc->data[SC_UPHEAVAL_OPTION] )
						as_skill_id = sc->data[SC_UPHEAVAL_OPTION]->val2;
					else if( sc->data[SC_TROPIC_OPTION] )
						as_skill_id = sc->data[SC_TROPIC_OPTION]->val3;
					else if( sc->data[SC_CHILLY_AIR_OPTION] )
						as_skill_id = sc->data[SC_CHILLY_AIR_OPTION]->val3;
					else
						as_skill_id = 0;
					if( as_skill_id && status_charge(src,0,skill_get_sp(as_skill_id,5)) ) {
						struct unit_data *ud = unit_bl2ud(src);

						sd->state.autocast = 1;
						skill_castend_damage_id(src,bl,as_skill_id,5,tick,0);
						sd->state.autocast = 0;
						if( ud ) { //Set can act delay [Skotlex]
							int delay = skill_delayfix(src,as_skill_id,5);

							if( DIFF_TICK(ud->canact_tick,tick + delay) < 0 ) {
								ud->canact_tick = max(tick + delay,ud->canact_tick);
								if( battle_config.display_status_timers )
									clif_status_change(src,SI_POSTDELAY,1,delay,0,0,0);
							}
						}
					}
				}
				if( sc->data[SC_PYROCLASTIC] && rnd()%100 < sc->data[SC_PYROCLASTIC]->val3 ) {
					as_skill_id = BS_HAMMERFALL;
					as_skill_lv = sc->data[SC_PYROCLASTIC]->val1;
					if( status_charge(src,0,skill_get_sp(as_skill_id,as_skill_lv)) ) {
						struct unit_data *ud = unit_bl2ud(src);

						sd->state.autocast = 1;
						skill_castend_pos2(src,bl->x,bl->y,as_skill_id,as_skill_lv,tick,0);
						sd->state.autocast = 0;
						if( ud ) {
							int delay = skill_delayfix(src,as_skill_id,as_skill_lv);

							if( DIFF_TICK(ud->canact_tick,tick + delay) < 0 ) {
								ud->canact_tick = max(tick + delay,ud->canact_tick);
								if( battle_config.display_status_timers )
									clif_status_change(src,SI_POSTDELAY,1,delay,0,0,0);
							}
						}
					}
				}
			}
		}
		if( !status_isdead(bl) && sd->autospell[0].id ) { //Autospell when attacking
			struct block_list *tbl = NULL;
			struct unit_data *ud = NULL;
			int16 as_skill_id, as_skill_lv;
			int type;

			for( i = 0; i < ARRAYLENGTH(sd->autospell) && sd->autospell[i].id; i++ ) {
				if( !(((sd->autospell[i].flag)&attack_type)&BF_WEAPONMASK &&
					 ((sd->autospell[i].flag)&attack_type)&BF_RANGEMASK &&
					 ((sd->autospell[i].flag)&attack_type)&BF_SKILLMASK) )
					continue; //One or more trigger conditions were not fulfilled

				as_skill_id = (sd->autospell[i].id > 0 ? sd->autospell[i].id : -sd->autospell[i].id);
				sd->state.autocast = 1;

				if( skill_isNotOk(as_skill_id,sd) ) {
					sd->state.autocast = 0;
					continue;
				}

				sd->state.autocast = 0;
				as_skill_lv = (sd->autospell[i].lv ? sd->autospell[i].lv : 1);

				if( as_skill_lv < 0 )
					as_skill_lv = 1 + rnd()%(-as_skill_lv);

				rate = sd->autospell[i].rate;

				if( (attack_type&(BF_LONG|BF_MAGIC)) == BF_LONG )
					rate /= 2;

				if( rnd()%1000 >= rate )
					continue;

				tbl = (sd->autospell[i].id < 0 ? src : bl);

				if( (type = skill_get_casttype(as_skill_id)) == CAST_GROUND &&
					!skill_pos_maxcount_check(src,tbl->x,tbl->y,as_skill_id,as_skill_lv,BL_PC,false) )
					continue;

				if( battle_config.autospell_check_range &&
					!battle_check_range(src,tbl,skill_get_range2(src,as_skill_id,as_skill_lv,true)) )
					continue;

				if( as_skill_id == AS_SONICBLOW )
					pc_stop_attack(sd); //Special case, Sonic Blow autospell should stop the player attacking
				else if( as_skill_id == PF_SPIDERWEB ) //Special case, due to its nature of coding
					type = CAST_GROUND;

				sd->state.autocast = 1;
				skill_consume_requirement(sd,as_skill_id,as_skill_lv,1);
				skill_toggle_magicpower(src,as_skill_id);

				switch( type ) {
					case CAST_GROUND:
						skill_castend_pos2(src,tbl->x,tbl->y,as_skill_id,as_skill_lv,tick,0);
						break;
					case CAST_NODAMAGE:
						skill_castend_nodamage_id(src,tbl,as_skill_id,as_skill_lv,tick,0);
						break;
					case CAST_DAMAGE:
						skill_castend_damage_id(src,tbl,as_skill_id,as_skill_lv,tick,0);
						break;
				}

				sd->state.autocast = 0;

				if( (ud = unit_bl2ud(src)) ) {
					int delay = skill_delayfix(src,as_skill_id,as_skill_lv);

					if( DIFF_TICK(ud->canact_tick,tick + delay) < 0 ) {
						ud->canact_tick = max(tick + delay,ud->canact_tick);
						if( battle_config.display_status_timers )
							clif_status_change(src,SI_POSTDELAY,1,delay,0,0,0);
					}
				}
			}
		}
		pc_exeautobonus(sd,sd->autobonus,attack_type,false); //Autobonus when attacking
	}

	return 0;
}

int skill_onskillusage(struct map_session_data *sd, struct block_list *bl, uint16 skill_id, unsigned int tick)
{
	int i;

	if( !sd || !skill_id )
		return 0;

	for( i = 0; i < ARRAYLENGTH(sd->autospell3) && sd->autospell3[i].flag; i++ ) {
		struct block_list *tbl = NULL;
		int16 as_skill_id, as_skill_lv;
		int type;

		if( sd->autospell3[i].flag != skill_id )
			continue;

		if( sd->autospell3[i].lock )
			continue; //Autospell already being executed

		as_skill_id = (sd->autospell3[i].id > 0 ? sd->autospell3[i].id : -sd->autospell3[i].id);
		sd->state.autocast = 1; //Set this to bypass sd->canskill_tick check

		if( skill_isNotOk(as_skill_id,sd) ) {
			sd->state.autocast = 0;
			continue;
		}

		sd->state.autocast = 0;

		if( as_skill_id >= 0 && !bl )
			continue; //No target

		if( rnd()%1000 >= sd->autospell3[i].rate )
			continue;

		as_skill_lv = (sd->autospell3[i].lv ? sd->autospell3[i].lv : 1);

		if( as_skill_id < 0 ) {
			tbl = &sd->bl;
			as_skill_id *= -1;
			as_skill_lv = 1 + rnd()%(-as_skill_lv); //Random skill level
		} else
			tbl = bl;

		if( (type = skill_get_casttype(as_skill_id)) == CAST_GROUND &&
			!skill_pos_maxcount_check(&sd->bl,tbl->x,tbl->y,as_skill_id,as_skill_lv,BL_PC,false) )
			continue;

		if( battle_config.autospell_check_range &&
			!battle_check_range(&sd->bl,tbl,skill_get_range2(&sd->bl,as_skill_id,as_skill_lv,true)) )
			continue;

		sd->state.autocast = 1;
		sd->autospell3[i].lock = true;
		skill_consume_requirement(sd,as_skill_id,as_skill_lv,1);

		switch( type ) {
			case CAST_GROUND:   skill_castend_pos2(&sd->bl,tbl->x,tbl->y,as_skill_id,as_skill_lv,tick,0); break;
			case CAST_NODAMAGE: skill_castend_nodamage_id(&sd->bl,tbl,as_skill_id,as_skill_lv,tick,0); break;
			case CAST_DAMAGE:   skill_castend_damage_id(&sd->bl,tbl,as_skill_id,as_skill_lv,tick,0); break;
		}

		sd->autospell3[i].lock = false;
		sd->state.autocast = 0;
	}

	pc_exeautobonus(sd,sd->autobonus3,skill_id,true);

	return 1;
}

/**
 * Splitted off from skill_additional_effect, which is never called when the
 * attack skill kills the enemy. Place in this function counter status effects
 * when using skills (eg: Asura's sp regen penalty, or counter-status effects
 * from cards) that will take effect on the source, not the target. [Skotlex]
 * NOTE: Currently this function only applies to Extremity Fist and BF_WEAPON
 * type of skills, so not every instance of skill_additional_effect needs a call
 * to this one.
 */
int skill_counter_additional_effect(struct block_list *src, struct block_list *bl, uint16 skill_id, uint16 skill_lv, int attack_type, unsigned int tick)
{
	struct map_session_data *sd = NULL;
	struct map_session_data *dstsd = NULL;
	struct status_change *sc = NULL;
	int rate;

	nullpo_ret(src);
	nullpo_ret(bl);

	if(skill_id && !skill_lv)
		return 0; //Don't forget auto attacks! [Celest]

	sc = status_get_sc(src);
	sd = BL_CAST(BL_PC, src);
	dstsd = BL_CAST(BL_PC, bl);

	if(dstsd && attack_type&BF_WEAPON) { //Counter effects
		enum sc_type type;
		uint8 i, sc_flag = SCFLAG_NONE;
		unsigned int time = 0;

		for(i = 0; i < MAX_PC_BONUS && dstsd->addeff2[i].flag; i++) {
			rate = dstsd->addeff2[i].rate;
			if(attack_type&BF_LONG)
				rate += dstsd->addeff2[i].arrow_rate;
			if(!rate)
				continue;
			if((dstsd->addeff2[i].flag&(ATF_LONG|ATF_SHORT)) != (ATF_LONG|ATF_SHORT)) { //Trigger has range consideration
				if((dstsd->addeff2[i].flag&ATF_LONG && !(attack_type&BF_LONG)) ||
					(dstsd->addeff2[i].flag&ATF_SHORT && !(attack_type&BF_SHORT)))
					continue; //Range failed
			}
			type = dstsd->addeff2[i].sc;
			time = dstsd->addeff2[i].duration;
			if(time)
				sc_flag = SCFLAG_FIXEDTICK;
			if(dstsd->addeff2[i].flag&ATF_TARGET && bl->id != src->id)
				status_change_start(src,src,type,rate,7,0,0,0,time,sc_flag);
			if(dstsd->addeff2[i].flag&ATF_SELF && !status_isdead(bl))
				status_change_start(src,bl,type,rate,7,0,0,0,time,sc_flag);
		}
	}

	switch(skill_id) {
#ifndef RENEWAL
		case MO_EXTREMITYFIST:
			sc_start(src,src,SC_EXTREMITYFIST,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
#endif
		case GS_FULLBUSTER:
			sc_start(src,src,SC_BLIND,2 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;
		case HFLI_SBR44: //[orn]
		case HVAN_EXPLOSION:
			{
				TBL_HOM *hd = BL_CAST(BL_HOM,src);

				if(hd) {
					hd->homunculus.intimacy = (skill_id == HFLI_SBR44 ? 200 : 100); //hom_intimacy_grade2intimacy(HOMGRADE_HATE_WITH_PASSION)
					if(hd->master)
						clif_send_homdata(hd->master,SP_INTIMATE,hd->homunculus.intimacy / 100);
				}
			}
			break;
		case LG_HESPERUSLIT: //Generates rage spheres for all Royal Guards in banding that has Force of Vanguard active
			if(sc && sc->data[SC_BANDING] && ((battle_config.hesperuslit_bonus_stack && sc->data[SC_BANDING]->val2 >= 7) || sc->data[SC_BANDING]->val2 == 7) && sd)
				party_foreachsamemap(party_sub_count_banding,sd,skill_get_splash(LG_BANDING,1),2);
			break;
		case MH_SONIC_CRAW:
			sc_start(src,src,SC_SONIC_CLAW_POSTDELAY,100,skill_lv,2000);
			break;
		case MH_SILVERVEIN_RUSH:
			sc_start(src,src,SC_SILVERVEIN_RUSH_POSTDELAY,100,skill_lv,2000);
			break;
		case MH_MIDNIGHT_FRENZY:
			sc_start2(src,src,SC_MIDNIGHT_FRENZY_POSTDELAY,100,skill_lv,bl->id,2000);
			break;
		case MH_TINDER_BREAKER:
			sc_start(src,src,SC_TINDER_BREAKER_POSTDELAY,100,skill_lv,2000);
			break;
		case MH_CBC:
			sc_start(src,src,SC_CBC_POSTDELAY,100,skill_lv,2000);
			status_change_end(src,SC_TINDER_BREAKER,INVALID_TIMER);
			break;
		case MH_EQC:
			status_change_end(src,SC_CBC_POSTDELAY,INVALID_TIMER); //End of grappler combo as it doesn't loop
			status_change_end(src,SC_TINDER_BREAKER,INVALID_TIMER);
			break;
	}

	if(sd) {
		if((sd->class_&MAPID_UPPERMASK) == MAPID_STAR_GLADIATOR && !mapdata[sd->bl.m].flag.nosumstarmiracle) //Star Gladiator Miracle [Komurka]
			status_change_start(src,src,SC_MIRACLE,battle_config.sg_miracle_skill_ratio,1,0,0,0,battle_config.sg_miracle_skill_duration,SCFLAG_NONE);
		if(skill_id && attack_type&BF_MAGIC && status_isdead(bl) && !(skill_get_inf(skill_id)&(INF_GROUND_SKILL|INF_SELF_SKILL)) &&
			(rate = pc_checkskill(sd,HW_SOULDRAIN)) > 0) { //Soul Drain should only work on targeted spells [Skotlex]
			if(pc_issit(sd))
				pc_setstand(sd); //Character stuck in attacking animation while 'sitting' fix [Skotlex]
			if(!((skill_get_nk(skill_id)&NK_SPLASH) && skill_area_temp[1] != bl->id)) {
				clif_skill_nodamage(src,bl,HW_SOULDRAIN,rate,1);
				status_heal(src,0,status_get_lv(bl) * (95 + rate * 15) / 100,2);
			}
		}
		if(status_isdead(bl)) {
			int sp = 0, hp = 0;

			if((attack_type&(BF_SHORT|BF_WEAPON)) == (BF_SHORT|BF_WEAPON)) {
				sp += sd->bonus.sp_gain_value;
				sp += sd->sp_gain_race[status_get_race(bl)] + sd->sp_gain_race[RC_ALL];
				hp += sd->bonus.hp_gain_value;
			}
			if(attack_type&BF_MAGIC) {
				sp += sd->bonus.magic_sp_gain_value;
				hp += sd->bonus.magic_hp_gain_value;
				if(skill_id == WZ_WATERBALL && sc && sc->data[SC_SPIRIT] &&
					sc->data[SC_SPIRIT]->val2 == SL_WIZARD &&
					sc->data[SC_SPIRIT]->val3 == WZ_WATERBALL) //(bugreport:5303)
					sc->data[SC_SPIRIT]->val3 = 0; //Clear bounced spell check
			}
			if(hp || sp) //Updated to force healing to allow healing through berserk
				status_heal(src,hp,sp,(battle_config.show_hp_sp_gain ? 3 : 1));
		}
	}

	if(dstsd && !status_isdead(bl) && !(skill_id && skill_get_nk(skill_id)&NK_NO_DAMAGE)) {
		if((sc = status_get_sc(bl)) && sc->data[SC_DORAM_SVSP] && (attack_type&(BF_LONG|BF_MAGIC)))
			skill_castend_damage_id(bl,src,SU_SV_STEMSPEAR,(dstsd ? pc_checkskill(dstsd,SU_SV_STEMSPEAR) : 5),tick,0);
		if(dstsd->autospell2[0].id) { //Trigger counter-spells to retaliate against damage causing skills
			struct block_list *tbl = NULL;
			struct unit_data *ud = NULL;
			int i, type;
			int16 as_skill_id, as_skill_lv;

			for(i = 0; i < ARRAYLENGTH(dstsd->autospell2) && dstsd->autospell2[i].id; i++) {
				if(!(((dstsd->autospell2[i].flag)&attack_type)&BF_WEAPONMASK &&
					 ((dstsd->autospell2[i].flag)&attack_type)&BF_RANGEMASK &&
					 ((dstsd->autospell2[i].flag)&attack_type)&BF_SKILLMASK))
					continue; //One or more trigger conditions were not fulfilled

				as_skill_id = (dstsd->autospell2[i].id > 0 ? dstsd->autospell2[i].id : -dstsd->autospell2[i].id);
				dstsd->state.autocast = 1;

				if(skill_isNotOk(as_skill_id,dstsd)) {
					dstsd->state.autocast = 0;
					continue;
				}

				dstsd->state.autocast = 0;
				as_skill_lv = (dstsd->autospell2[i].lv ? dstsd->autospell2[i].lv : 1);

				if(as_skill_lv < 0)
					as_skill_lv = 1 + rnd()%(-as_skill_lv);

				rate = dstsd->autospell2[i].rate;

				if((attack_type&(BF_LONG|BF_MAGIC)) == BF_LONG)
					rate /= 2;

				if(rnd()%1000 >= rate)
					continue;

				tbl = (dstsd->autospell2[i].id < 0 ? bl : src);

				if((type = skill_get_casttype(as_skill_id)) == CAST_GROUND &&
					!skill_pos_maxcount_check(bl,tbl->x,tbl->y,as_skill_id,as_skill_lv,BL_PC,false))
					continue;

				if(battle_config.autospell_check_range &&
					!battle_check_range(bl,tbl,skill_get_range2(bl,as_skill_id,as_skill_lv,true)))
					continue;

				dstsd->state.autocast = 1;
				skill_consume_requirement(dstsd,as_skill_id,as_skill_lv,1);

				switch(type) {
					case CAST_GROUND:
						skill_castend_pos2(bl,tbl->x,tbl->y,as_skill_id,as_skill_lv,tick,0);
						break;
					case CAST_NODAMAGE:
						skill_castend_nodamage_id(bl,tbl,as_skill_id,as_skill_lv,tick,0);
						break;
					case CAST_DAMAGE:
						skill_castend_damage_id(bl,tbl,as_skill_id,as_skill_lv,tick,0);
						break;
				}

				dstsd->state.autocast = 0;

				if((ud = unit_bl2ud(bl))) {
					int delay = skill_delayfix(bl,as_skill_id,as_skill_lv);

					if(DIFF_TICK(ud->canact_tick, tick + delay) < 0) {
						ud->canact_tick = max(tick + delay,ud->canact_tick);
						if(battle_config.display_status_timers)
							clif_status_change(bl,SI_POSTDELAY,1,delay,0,0,0);
					}
				}
			}
		}
		pc_exeautobonus(dstsd,dstsd->autobonus2,attack_type,false); //Autobonus when attacked
	}

	return 0;
}

/*=========================================================================
 Breaks equipment. On-non players causes the corresponding strip effect.
 - rate goes from 0 to 10000 (100.00%)
 - flag is a BCT_ flag to indicate which type of adjustment should be used
   (BCT_SELF/BCT_ENEMY/BCT_PARTY) are the valid values.
--------------------------------------------------------------------------*/
int skill_break_equip(struct block_list *src, struct block_list *bl, unsigned short pos, int rate, int flag)
{
	const int pos_list[4]        = { EQP_HELM,EQP_WEAPON,EQP_SHIELD,EQP_ARMOR };
	const enum sc_type sc_atk[4] = { SC_STRIPHELM,SC_STRIPWEAPON,SC_STRIPSHIELD,SC_STRIPARMOR };
	const enum sc_type sc_def[4] = { SC_CP_HELM,SC_CP_WEAPON,SC_CP_SHIELD,SC_CP_ARMOR };
	struct status_change *sc = status_get_sc(bl);
	TBL_PC *sd = BL_CAST(BL_PC,bl);
	int i;

	if (sc && !sc->count)
		sc = NULL;

	if (sd) {
		if (sd->bonus.unbreakable_equip)
			pos &= ~sd->bonus.unbreakable_equip;
		if (sd->bonus.unbreakable)
			rate -= rate * sd->bonus.unbreakable / 100;
		if (pos&EQP_WEAPON) {
			switch (sd->status.weapon) {
				case W_FIST: //Bare fists should not break
				case W_1HAXE:
				case W_2HAXE:
				case W_MACE: //Axes and Maces can't be broken [DracoRPG]
				case W_2HMACE:
				case W_STAFF:
				case W_2HSTAFF:
				case W_BOOK: //Rods and Books can't be broken [Skotlex]
				case W_HUUMA:
					pos &= ~EQP_WEAPON;
					break;
			}
		}
	}
	if (flag&BCT_ENEMY) {
		if (battle_config.equip_skill_break_rate != 100)
			rate = rate * battle_config.equip_skill_break_rate / 100;
	} else if (flag&(BCT_SELF|BCT_PARTY)) {
		if (battle_config.equip_self_break_rate != 100)
			rate = rate * battle_config.equip_self_break_rate / 100;
	}
	for (i = 0; i < ARRAYLENGTH(pos_list); i++) {
		if (pos&pos_list[i]) {
			if (sc && sc->count && sc->data[sc_def[i]])
				pos &= ~pos_list[i];
			else if (rnd()%10000 >= rate)
				pos &= ~pos_list[i];
			else if (!sd) //Cause strip effect
				sc_start(src,bl,sc_atk[i],100,0,skill_get_time(status_sc2skill(sc_atk[i]),1));
		}
	}
	if (!pos) //Nothing to break
		return 0;
	if (sd) {
		for (i = 0; i < EQI_MAX; i++) {
			short j = sd->equip_index[i];

			if (j < 0 || sd->inventory.u.items_inventory[j].attribute || !sd->inventory_data[j])
				continue;
			switch(i) {
				case EQI_HEAD_TOP: //Upper Head
					flag = (pos&EQP_HELM);
					break;
				case EQI_ARMOR: //Body
					flag = (pos&EQP_ARMOR);
					break;
				case EQI_HAND_R: //Right/Left hands
				case EQI_HAND_L:
					flag = (((pos&EQP_WEAPON) && sd->inventory_data[j]->type == IT_WEAPON) ||
						((pos&EQP_SHIELD) && sd->inventory_data[j]->type == IT_ARMOR));
					break;
				case EQI_SHOES:
					flag = (pos&EQP_SHOES);
					break;
				case EQI_GARMENT:
					flag = (pos&EQP_GARMENT);
					break;
				default:
					continue;
			}
			if (flag) {
				sd->inventory.u.items_inventory[j].attribute = 1;
				pc_unequipitem(sd,j,1|2|4);
			}
		}
		clif_equiplist(sd);
	}

	return pos; //Return list of pieces broken
}

/**
 * Strip equipment from a target
 * @param src: Source of call
 * @param bl: Target to strip
 * @param skill_id: Skill used
 * @param skill_lv: Skill level used
 * @return True on successful strip or false otherwise
 */
bool skill_strip_equip(struct block_list *src, struct block_list *bl, uint16 skill_id, uint16 skill_lv)
{
	const int pos[5]             = { EQP_WEAPON,EQP_SHIELD,EQP_ARMOR,EQP_HELM,EQP_ACC };
	const enum sc_type sc_atk[5] = { SC_STRIPWEAPON,SC_STRIPSHIELD,SC_STRIPARMOR,SC_STRIPHELM,SC__STRIPACCESSORY };
	const enum sc_type sc_def[5] = { SC_CP_WEAPON,SC_CP_SHIELD,SC_CP_ARMOR,SC_CP_HELM,SC_NONE };
	struct status_data *sstatus = NULL;
	struct status_data *tstatus = NULL;
	struct map_session_data *tsd = NULL;
	struct status_change *tsc = NULL;
	int rate, time = 0, location = 0, mod = 100;
	uint8 i;

	nullpo_retr(false,src);
	nullpo_retr(false,bl);

	sstatus = status_get_status_data(src);
	tstatus = status_get_status_data(bl);
	tsd = BL_CAST(BL_PC,bl);
	tsc = status_get_sc(bl);

	if (!tsc || (tsc->option&OPTION_MADOGEAR))
		return false; //Mado Gear cannot be divested [Ind]

	switch (skill_id) { //Success chance
		case RG_STRIPWEAPON:
		case RG_STRIPARMOR:
		case RG_STRIPSHIELD:
		case RG_STRIPHELM:
		case GC_WEAPONCRUSH:
			rate = 50 * (skill_lv + 1) + 2 * (sstatus->dex - tstatus->dex);
			mod = 1000;
			break;
		case ST_FULLSTRIP: {
				int min_rate = 50 + 20 * skill_lv;

				rate = min_rate + 2 * (sstatus->dex - tstatus->dex);
				rate = max(rate,min_rate);
				mod = 1000;
			}
			break;
		case GS_DISARM:
			rate = sstatus->dex / (4 * (7 - skill_lv)) + sstatus->luk / (4 * (6 - skill_lv));
			rate = rate + status_get_lv(src) - (tstatus->agi * rate / 100) - tstatus->luk - status_get_lv(bl);
			break;
		case WL_EARTHSTRAIN:
			rate = 6 * skill_lv + status_get_job_lv(src) / 4 + sstatus->dex / 10;
			break;
		case SC_STRIPACCESSARY:
			rate = 12 + 2 * skill_lv;
			break;
		default:
			return false;
	}

	if (tsd && tsd->bonus.unstripable)
		rate -= tsd->bonus.unstripable;

	rate = max(rate,0);

	if (rnd()%mod >= rate)
		return false;

	switch (skill_id) { //Status duration
		case SC_STRIPACCESSARY:
		case GS_DISARM:
			time = skill_get_time(skill_id,skill_lv);
			break;
		case WL_EARTHSTRAIN:
		case RG_STRIPWEAPON:
		case RG_STRIPARMOR:
		case RG_STRIPSHIELD:
		case RG_STRIPHELM:
		case GC_WEAPONCRUSH:
		case ST_FULLSTRIP:
			if (skill_id == WL_EARTHSTRAIN)
				time = skill_get_time2(skill_id,skill_lv);
			else
				time = skill_get_time(skill_id,skill_lv);
 			if (tsd)
				time += skill_lv + 500 * (sstatus->dex - tstatus->dex);
			else {
				time += 15000;
				time += skill_lv + 500 * (sstatus->dex - tstatus->dex);
			}
			break;
	}

	switch (skill_id) { //Location
		case GC_WEAPONCRUSH:
		case RG_STRIPWEAPON:
		case GS_DISARM:
			location = EQP_WEAPON;
			break;
		case RG_STRIPARMOR:
			location = EQP_ARMOR;
			break;
		case RG_STRIPSHIELD:
			location = EQP_SHIELD;
			break;
		case RG_STRIPHELM:
			location = EQP_HELM;
			break;
		case ST_FULLSTRIP:
			location = EQP_WEAPON|EQP_SHIELD|EQP_ARMOR|EQP_HELM;
			break;
		case SC_STRIPACCESSARY:
			location = EQP_ACC;
			break;
		case WL_EARTHSTRAIN:
			location = EQP_SHIELD|EQP_ARMOR|EQP_HELM;
			if (skill_lv >= 4)
				location |= EQP_WEAPON;
			if (skill_lv >= 5)
				location |= EQP_ACC;
			break;
	}

	if (tsd && tsd->bonus.unstripable_equip)
		location &= ~tsd->bonus.unstripable_equip;

	for (i = 0; i < ARRAYLENGTH(pos); i++) {
		if (location&pos[i] && tsc->data[sc_def[i]])
			location &= ~pos[i];
	}

	if (!location)
		return false;

	for (i = 0; i < ARRAYLENGTH(pos); i++) {
		if (location&pos[i] && !sc_start(src,bl,sc_atk[i],100,skill_lv,time))
			location &= ~pos[i];
	}

	return (location ? true : false);
}

/**
 * Used to knock back players, monsters, traps, etc
 * @param src Object that give knock back
 * @param target Object that receive knock back
 * @param count Number of knock back cell requested
 * @param dir Direction indicates the way OPPOSITE to the knockback direction (or -1 for default behavior)
 * @param flag
 *  0x01 - position update packets must not be sent
 *  0x02 - ignores players' special_state.no_knockback
 *  These flags "return 'count' instead of 0 if target is can't be knocked back"
 *  0x04 - at WOE/BG map
 *  0x08 - if target is MD_KNOCKBACK_IMMUNE
 *  0x10 - if target has 'special_state.no_knockback'
 *  0x20 - if target is in Basilica area
 * @return Number of knocked back cells done
 */
short skill_blown(struct block_list *src, struct block_list *target, char count, int8 dir, unsigned char flag)
{
	int dx = 0, dy = 0;
	int reason = 0, checkflag = 0;
	struct status_change *tsc = status_get_sc(target);

	nullpo_ret(src);
	nullpo_ret(target);

	if( !count )
		return count; //Actual knockback distance is 0

	//Create flag needed in unit_blown_immune
	if( target->id != src->id )
		checkflag |= 0x1; //Offensive
	if( !(flag&0x02) )
		checkflag |= 0x2; //Knockback type
	if( status_get_class_(src) == CLASS_BOSS )
		checkflag |= 0x4; //Boss attack

	//Get reason and check for flags
	reason = unit_blown_immune(target, checkflag);
	switch( reason ) {
		case 1: return ((flag&0x04) ? count : 0); //No knocking back in WoE / BG
		case 2: return ((flag&0x08) ? count : 0); //Bosses or immune can't be knocked back
		case 3: return ((flag&0x10) ? count : 0); //Target has special_state.no_knockback (equip)
		case 4: return ((flag&0x20) ? count : 0); //Basilica caster can't be knocked back by normal monsters
		case 5: return count; //Trap can't be knocked back
	}

	if( dir == -1 ) //<Optimized>: do the computation here instead of outside
		dir = map_calc_dir(target, src->x, src->y); //Direction from src to target, reversed

	if( dir >= 0 && dir < 8 ) { //Take the reversed 'direction' and reverse it
		dx = -dirx[dir];
		dy = -diry[dir];
	}

	if( tsc && tsc->data[SC_SU_STOOP] ) //Any knockback will cancel it
		status_change_end(target, SC_SU_STOOP, INVALID_TIMER);

	return unit_blown(target, dx, dy, count, flag); //Send over the proper flag
}

/**
 * Checks if 'bl' should reflect back a spell cast by 'src'.
 * In case of success returns type of reflection, otherwise 0
 *	1 - Regular reflection (Maya)
 *	2 - SL_KAITE reflection
 */
static int skill_magic_reflect(struct block_list *src, struct block_list *bl, bool ground_skill)
{
	struct status_change *sc = status_get_sc(bl);
	struct map_session_data *sd = BL_CAST(BL_PC, bl);

	//Item-based reflection (Bypasses Boss check)
	if( !sc || !sc->data[SC_KYOMU] ) { //Kyomu doesn't reflect
		if( sd && sd->bonus.magic_damage_return && !ground_skill && rnd()%100 < sd->bonus.magic_damage_return )
			return 1;
	}

	//Magic Mirror reflection (Bypasses Boss check)
	if( sc && sc->data[SC_MAGICMIRROR] && rnd()%100 < sc->data[SC_MAGICMIRROR]->val2 )
		return 1;

	if( status_get_class_(src) == CLASS_BOSS )
		return 0;

	//Status-based reflection
	if( !sc || !sc->count )
		return 0;

	//Kyomu doesn't disable Kaite, but the "skill fail chance" part of Kyomu applies to it
	if( sc->data[SC_KAITE] && (src->type == BL_PC || status_get_lv(src) <= 80) ) { //Kaite only works against non-players if they are low-level
#ifdef RENEWAL //Renewal: 50% chance to reflect targeted magic, and does not reflect area of effect magic [exneval]
		if( sc->data[SC_KAITE]->val3 && (ground_skill || rnd()%100 < 50) )
			return 0;
#endif
		clif_specialeffect(bl, EF_ATTACKENERGY2, AREA);
		if( --sc->data[SC_KAITE]->val2 <= 0 )
			status_change_end(bl, SC_KAITE, INVALID_TIMER);
		return 2; //Kaite reflection (Doessn't bypass Boss check)
	}

	return 0;
}

/**
 * Checks whether a skill can be used in combos or not
 * @param skill_id: Target skill
 * @return	0: Skill is not a combo
 *			1: Skill is a normal combo
 *			2: Skill is combo that prioritizes auto-target even if val2 is set
 * @author Panikon
 */
int skill_is_combo(uint16 skill_id) {
	switch(skill_id) {
		case MO_CHAINCOMBO:
		case MO_COMBOFINISH:
		case CH_TIGERFIST:
		case CH_CHAINCRUSH:
		case MO_EXTREMITYFIST:
		case TK_TURNKICK:
		case TK_STORMKICK:
		case TK_DOWNKICK:
		case TK_COUNTER:
		case TK_JUMPKICK:
		case HT_POWER:
		case GC_COUNTERSLASH:
		case GC_WEAPONCRUSH:
		case SR_DRAGONCOMBO:
		case SJ_SOLARBURST:
			return 1;
		case SR_FALLENEMPIRE:
		case SR_TIGERCANNON:
		case SR_GATEOFHELL:
			return 2;
	}
	return 0;
}

/**
 * Combo handler, start stop combo status
 */
void skill_combo_toggle_inf(struct block_list *bl, uint16 skill_id, int inf) {
	struct map_session_data *sd = NULL;

	nullpo_retv(bl);

	sd = map_id2sd(bl->id);

	switch (skill_id) {
		case MO_COMBOFINISH:
		case CH_TIGERFIST:
		case CH_CHAINCRUSH:
			if (sd)
				clif_skillinfo(sd,MO_EXTREMITYFIST,inf);
			break;
		case TK_JUMPKICK:
			if (sd)
				clif_skillinfo(sd,TK_JUMPKICK,inf);
			break;
		case MO_TRIPLEATTACK:
			if (sd && pc_checkskill(sd,SR_DRAGONCOMBO) > 0)
				clif_skillinfo(sd,SR_DRAGONCOMBO,inf);
			break;
		case SR_FALLENEMPIRE:
			if (sd) {
				clif_skillinfo(sd,SR_GATEOFHELL,inf);
				clif_skillinfo(sd,SR_TIGERCANNON,inf);
			}
			break;
	}
}

void skill_combo(struct block_list *src, struct block_list *dsrc, struct block_list *bl, uint16 skill_id, uint16 skill_lv, int tick) {
	unsigned int duration = 0; //Set to duration the user can use a combo skill or 1 for aftercast delay of pre-skill
	int nodelay = 0; //Set to 1 for no walk/attack delay, set to 2 for no walk delay
	int target_id = bl->id; //Set to 0 if combo skill should not autotarget
	struct status_change_entry *sce;
	TBL_PC *sd = BL_CAST(BL_PC,src);
	struct status_change *sc = status_get_sc(src);

	if (!sc)
		return;

	//End previous combo state after skill is invoked
	if ((sce = sc->data[SC_COMBO])) {
		switch (skill_id) {
			case TK_TURNKICK:
			case TK_STORMKICK:
			case TK_DOWNKICK:
			case TK_COUNTER:
				if (sd && pc_famerank(sd->status.char_id, MAPID_TAEKWON)) { //Extend combo time
					sce->val1 = skill_id; //Update combo-skill
					sce->val3 = skill_id;
					if (sce->timer != INVALID_TIMER)
						delete_timer(sce->timer, status_change_timer);
					sce->timer = add_timer(tick + sce->val4, status_change_timer, src->id, SC_COMBO);
					break;
				}
				unit_cancel_combo(src); //Cancel combo wait
				break;
			default:
				if (src == dsrc) //Ground skills are exceptions [Inkfish]
					status_change_end(src, SC_COMBO, INVALID_TIMER);
				break;
		}
	}

	//Start new combo
	if (sd) { //Player only
		switch (skill_id) {
			case MO_TRIPLEATTACK:
				if (pc_checkskill(sd, MO_CHAINCOMBO) > 0 || pc_checkskill(sd, SR_DRAGONCOMBO) > 0) {
					duration = 1;
					target_id = 0; //Will target current auto-target instead
				}
				break;
			case MO_CHAINCOMBO:
				if (pc_checkskill(sd, MO_COMBOFINISH) > 0 && sd->spiritball > 0) {
					duration = 1;
					target_id = 0;
				}
				break;
			case MO_COMBOFINISH:
				if (sd->status.party_id > 0) //Bonus from SG_FRIEND [Komurka]
					party_skill_check(sd, sd->status.party_id, MO_COMBOFINISH, skill_lv);
				if (pc_checkskill(sd, CH_TIGERFIST) > 0 && sd->spiritball > 0) {
					duration = 1;
					target_id = 0;
				}
			//Fall through
			case CH_TIGERFIST:
				if (!duration && pc_checkskill(sd, CH_CHAINCRUSH) > 0 && sd->spiritball > 1) {
					duration = 1;
					target_id = 0;
				}
			//Fall through
			case CH_CHAINCRUSH:
				if (!duration && pc_checkskill(sd, MO_EXTREMITYFIST) > 0 && sd->spiritball > 0 && sd->sc.data[SC_EXPLOSIONSPIRITS]) {
					duration = 1;
					target_id = 0;
				}
				break;
			case AC_DOUBLE:
				//AC_DOUBLE can start the combo with other monster types, but the
				//monster that's going to be hit by HT_POWER should be RC_BRUTE or RC_INSECT [Panikon]
				if (pc_checkskill(sd, HT_POWER) > 0) {
					duration = 2000;
					nodelay = 1; //Neither gives walk nor attack delay
					target_id = 0;
				}
				break;
			case SR_DRAGONCOMBO:
				if (pc_checkskill(sd, SR_FALLENEMPIRE) > 0)
					duration = 1;
				break;
			case SR_FALLENEMPIRE:
				if (pc_checkskill(sd, SR_TIGERCANNON) > 0 || pc_checkskill(sd, SR_GATEOFHELL) > 0)
					duration = 1;
				break;
			case SJ_PROMINENCEKICK:
				if (pc_checkskill(sd, SJ_SOLARBURST) > 0)
					duration = 1;
				break;
		}
	}

	if (duration) { //Possible to chain
		if (sd && duration == 1)
			duration = 1300 - (4 * status_get_agi(src) + 2 * status_get_dex(src));
		sc_start4(src, src, SC_COMBO, 100, skill_id, target_id, nodelay, 0, duration);
		clif_combo_delay(src, duration);
	}
}

/** Copy skill by Plagiarism or Reproduce
 * @param src: The caster
 * @param bl: The target
 * @param skill_id: Skill that casted
 * @param skill_lv: Skill level of the casted skill
 */
static void skill_do_copy(struct block_list *src,struct block_list *bl, uint16 skill_id, uint16 skill_lv) {
	TBL_PC *tsd = BL_CAST(BL_PC, bl);

	if (!tsd)
		return;
	else {
		uint16 idx;
		unsigned char lv;

		skill_id = skill_dummy2skill_id(skill_id);

		//Use skill index, avoiding out-of-bound array [Cydh]
		if (!(idx = skill_get_index(skill_id)))
			return;

		switch (skill_isCopyable(tsd, skill_id)) {
			case 1: //Copied by Plagiarism
				if (tsd->cloneskill_idx >= 0 && tsd->status.skill[tsd->cloneskill_idx].flag == SKILL_FLAG_PLAGIARIZED) {
					tsd->status.skill[tsd->cloneskill_idx].id = 0;
					tsd->status.skill[tsd->cloneskill_idx].lv = 0;
					tsd->status.skill[tsd->cloneskill_idx].flag = SKILL_FLAG_PERMANENT;
					clif_deleteskill(tsd, tsd->status.skill[tsd->cloneskill_idx].id);
				}
				//Copied level never be > player's RG_PLAGIARISM level
				lv = min(skill_lv, pc_checkskill(tsd, RG_PLAGIARISM));
				tsd->cloneskill_idx = idx;
				pc_setglobalreg(tsd, SKILL_VAR_PLAGIARISM, skill_id);
				pc_setglobalreg(tsd, SKILL_VAR_PLAGIARISM_LV, lv);
				break;
			case 2: { //Copied by Reproduce
					struct status_change *tsc = status_get_sc(bl);

					//Already did SC check
					//Skill level copied depends on Reproduce skill that used
					lv = (tsc ? tsc->data[SC__REPRODUCE]->val1 : 1);
					if( tsd->reproduceskill_idx >= 0 && tsd->status.skill[tsd->reproduceskill_idx].flag == SKILL_FLAG_PLAGIARIZED ) {
						tsd->status.skill[tsd->reproduceskill_idx].id = 0;
						tsd->status.skill[tsd->reproduceskill_idx].lv = 0;
						tsd->status.skill[tsd->reproduceskill_idx].flag = SKILL_FLAG_PERMANENT;
						clif_deleteskill(tsd, tsd->status.skill[tsd->reproduceskill_idx].id);
					}
					//Level dependent and limitation
					if( src->type == BL_PC ) //If player, max skill level is skill_get_max(skill_id)
						lv = min(lv, skill_get_max(skill_id));
					else //Monster might used skill level > allowed player max skill lv. Ex. Drake with Waterball lv. 10
						lv = min(lv, skill_lv);
					tsd->reproduceskill_idx = idx;
					pc_setglobalreg(tsd, SKILL_VAR_REPRODUCE, skill_id);
					pc_setglobalreg(tsd, SKILL_VAR_REPRODUCE_LV, lv);
				}
				break;
			default:
				return;
		}
		tsd->status.skill[idx].id = skill_id;
		tsd->status.skill[idx].lv = lv;
		tsd->status.skill[idx].flag = SKILL_FLAG_PLAGIARIZED;
		clif_addskill(tsd, skill_id);
	}
}

/**
 * Knockback the target on skill_attack
 * @param src is the master behind the attack
 * @param dsrc is the actual originator of the damage, can be the same as src, or a BL_SKILL
 * @param target is the target to be attacked.
 * @param blewcount
 * @param skill_id
 * @param skill_lv
 * @param damage
 * @param tick
 * @param flag can hold a bunch of information:
 */
void skill_attack_blow(struct block_list *src, struct block_list *dsrc, struct block_list *target, uint8 blewcount, uint16 skill_id, uint16 skill_lv, int64 damage, unsigned int tick, int flag) {
	int8 dir = -1; //Default direction

	//Only knockback if it's still alive, otherwise a "ghost" is left behind [Skotlex]
	//Reflected spells do not bounce back (src == dsrc since it only happens for direct skills)
	if (!blewcount || target == dsrc || status_isdead(target))
		return;

	//Skill specific direction
	switch (skill_id) {
		case MC_CARTREVOLUTION:
			if (battle_config.cart_revo_knockback)
				dir = 6; //Official servers push target to the West
			break;
		case AC_SHOWER:
			if (!battle_config.arrow_shower_knockback)
				dir = map_calc_dir(target, src->x, src->y);
			else
				dir = map_calc_dir(target, skill_area_temp[4], skill_area_temp[5]);
			break;
		case MG_FIREWALL:
		case EL_FIRE_MANTLE:
			dir = unit_getdir(target); //Backwards
			break;
		case WZ_STORMGUST:
			if (!battle_config.stormgust_knockback)
				dir = rnd()%8; //This ensures SG randomly pushes instead of exactly a cell backwards per official mechanics
			break;
		case WL_CRIMSONROCK:
			if (!battle_config.crimsonrock_knockback)
				dir = map_calc_dir(target, skill_area_temp[4], skill_area_temp[5]);
			break;
	}

	switch (skill_id) { //Blown-specific handling
		case LG_OVERBRAND_BRANDISH: //Give knockback damage bonus only hits the wall (bugreport:9096)
			if (skill_blown(dsrc, target, blewcount, dir, 0x04|0x08|0x10) < blewcount)
				skill_addtimerskill(src, tick + status_get_amotion(src), target->id, 0, 0, LG_OVERBRAND_PLUSATK, skill_lv, BF_WEAPON, flag|SD_ANIMATION);
			break;
		case SR_KNUCKLEARROW: {
				short x = target->x, y = target->y;

				dir = map_calc_dir(target, src->x, src->y);
				//Ignore knockback damage bonus if in WOE or target is boss/knockback immune (bugreport:9096)
				if (skill_blown(dsrc, target, blewcount, dir, 0x04|0x08|0x10) < blewcount)
					skill_addtimerskill(src, tick + 300 * ((flag&2) ? 1 : 2), target->id, 0, 0, skill_id, skill_lv, BF_WEAPON, flag|4);
				if (target->x != x || target->y != y)  {
					if (dir < 4) {
						x = target->x + 2 * (dir > 0) - 3 * (dir > 0);
						y = target->y + 1 - (dir / 2) - (dir > 2);
					} else {
						x = target->x + 2 * (dir > 4) - 1 * (dir > 4);
						y = target->y + (dir / 6) - 1 + (dir > 6);
					}
					if (unit_movepos(src, x, y, 1, true))
						clif_blown(src, target); //Move attacker position after knocked back
				}
			}
			break;
		case RL_R_TRIP:
			if (skill_blown(dsrc, target, blewcount, dir, 0x04|0x08|0x10) < blewcount)
				skill_addtimerskill(src, tick + status_get_amotion(src), target->id, 0, 0, RL_R_TRIP_PLUSATK, skill_lv, BF_WEAPON, flag|SD_ANIMATION);
			break;
		default:
			skill_blown(dsrc, target, blewcount, dir, 0);
			if (!blewcount && target->type == BL_SKILL && damage > 0) {
				TBL_SKILL *su = (TBL_SKILL *)target;

				if (su->group && su->group->skill_id == HT_BLASTMINE)
					skill_blown(src, target, 3, -1, 0);
			}
			break;
	}

	clif_fixpos(target);
}

/**
 * Does a skill attack with the given properties.
 * @param src is the master behind the attack (player/mob/pet)
 * @param dsrc is the actual originator of the damage, can be the same as src, or a BL_SKILL
 * @param bl is the target to be attacked.
 * @param flag can hold a bunch of information:
 *  flag&1
 *  flag&2  - Disable re-triggered by double casting
 *  flag&4  - Skip to blow target (because already knocked back somewhere else)
 *  flag&8  - SC_COMBO state used to deal bonus damage
 *  flag&16 - Splash targets
 *
 *  flag&0xFFF is passed to the underlying battle_calc_attack for processing
 *   (usually holds number of targets, or just 1 for simple splash attacks)
 *
 *  flag&0xF000 - Values from enum e_skill_display
 *  flag&0x3F0000 - Values from enum e_battle_check_target
 *
 *  flag&0x1000000 - Return 0 if damage was reflected
 */
int skill_attack(int attack_type, struct block_list *src, struct block_list *dsrc, struct block_list *bl, uint16 skill_id, uint16 skill_lv, unsigned int tick, int flag)
{
	struct Damage dmg;
	struct status_data *sstatus, *tstatus;
	struct status_change *sc, *tsc;
	struct map_session_data *sd, *tsd;
	int64 damage;
	int type;
	bool rmdamage = false, //Magic reflected
		additional_effects = true,
		ground_skill = false;

	if (skill_id > 0 && !skill_lv)
		return 0;

	nullpo_ret(src); //Source is the master behind the attack (player/mob/pet)
	nullpo_ret(dsrc); //dsrc is the actual originator of the damage, can be the same as src, or a skill casted by src
	nullpo_ret(bl); //Target to be attacked

	if (status_bl_has_mode(bl, MD_SKILL_IMMUNE) || (status_get_class(bl) == MOBID_EMPERIUM && !(skill_get_inf3(skill_id)&INF3_HIT_EMP)))
		return 0;

	if (src != dsrc) {
		if (!status_check_skilluse((battle_config.skill_caster_check ? src : NULL), bl, skill_id, 2))
			return 0; //When caster is not the src of attack, this is a ground skill, and as such, do the relevant target checking [Skotlex]
		ground_skill = true;
	} else if ((flag&SD_ANIMATION) && (skill_get_nk(skill_id)&NK_SPLASH)) {
		//Note that splash attacks often only check versus the targeted mob,
		//those around the splash area normally don't get checked for being hidden/cloaked/etc [Skotlex]
		if (!status_check_skilluse(src, bl, skill_id, 2))
			return 0;
	}

	sd = BL_CAST(BL_PC, src);
	tsd = BL_CAST(BL_PC, bl);

	//To block skills that aren't called via battle_check_target (bugreport:8203) [Panikon]
	if (sd && ((bl->type == BL_MOB && pc_has_permission(sd, PC_PERM_DISABLE_PVM)) ||
		(bl->type == BL_PC && pc_has_permission(sd, PC_PERM_DISABLE_PVP))))
		return 0;

	sstatus = status_get_status_data(src);
	tstatus = status_get_status_data(bl);
	sc = status_get_sc(src);
	tsc = status_get_sc(bl);

	if (tsc && !tsc->count)
		tsc = NULL; //Don't need it

	if (tsc && tsc->data[SC_TRICKDEAD])
		return 0; //Trick Dead protects you from damage, but not from buffs and the like, hence it's placed here

	if (sc && sc->data[SC_GRAVITATION] && sc->data[SC_GRAVITATION]->val3 == BCT_SELF && skill_id != HW_GRAVITATION && sd && !sd->state.autocast)
		return 0; //When Gravitational Field is active, damage can only be dealt by Gravitational Field and Autospells

	dmg = battle_calc_attack(attack_type, src, bl, skill_id, skill_lv, (flag&0xFFF));

	if (ground_skill && skill_id != GS_GROUNDDRIFT)
		dmg.amotion = 0;

	//NOTE: This check maybe breaks the battle_calc_attack, and maybe need better calculation
	//Skotlex: Adjusted to the new system
	if (src->type == BL_PET) { //[Valaris]
		struct pet_data *pd = (TBL_PET *)src;

		if (pd->a_skill && pd->a_skill->div_ && pd->a_skill->id == skill_id) { //petskillattack2
			if (battle_config.pet_ignore_infinite_def || !is_infinite_defense(bl, dmg.flag)) {
				int element = skill_get_ele(skill_id, skill_lv);

				if (element == -1)
					element = sstatus->rhw.ele;
				if (element != ELE_NEUTRAL || !(battle_config.attack_attr_none&BL_PET))
					dmg.damage = battle_attr_fix(src, bl, pd->a_skill->damage, element, tstatus->def_ele, tstatus->ele_lv);
				else
					dmg.damage = pd->a_skill->damage; //Fixed damage
			} else
				dmg.damage = pd->a_skill->div_;
			dmg.damage2 = 0;
			dmg.div_ = pd->a_skill->div_;
		}
	}

	if ((dmg.damage > 0 || dmg.damage2 > 0) && (dmg.flag&BF_MAGIC)) {
		if (skill_id == NPC_EARTHQUAKE && battle_config.eq_single_target_reflectable && (flag&0xFFF) > 1)
			ground_skill = true; //Earthquake on multiple targets is not counted as a target skill [Inkfish]
		type = skill_magic_reflect(src, bl, ground_skill);
		if (tsc) {
			if (tsc->data[SC_DEVOTION]) {
				struct status_change_entry *sce_d = tsc->data[SC_DEVOTION];
				struct block_list *d_bl = map_id2bl(sce_d->val1);

				if (d_bl &&
					((d_bl->type == BL_MER && ((TBL_MER *)d_bl)->master && ((TBL_MER *)d_bl)->master->bl.id == bl->id) ||
					(d_bl->type == BL_PC && ((TBL_PC *)d_bl)->devotion[sce_d->val2] == bl->id)) &&
					check_distance_bl(bl, d_bl, sce_d->val3) && (type = skill_magic_reflect(src, d_bl, ground_skill)))
					bl = d_bl;
			} else if (tsc->data[SC__SHADOWFORM]) {
				struct status_change_entry *sce_s = tsc->data[SC__SHADOWFORM];
				struct map_session_data *s_sd = map_id2sd(sce_s->val2);

				if (s_sd && s_sd->shadowform_id == bl->id && (type = skill_magic_reflect(src, &s_sd->bl, ground_skill)))
					bl = &s_sd->bl;
			}
		}
		if (type) { //Magic reflection, switch caster/target
			struct block_list *tbl = bl;

			rmdamage = true;
			bl = src;
			src = tbl;
			dsrc = (ground_skill ? dsrc : tbl);
			sd = BL_CAST(BL_PC, src);
			tsd = BL_CAST(BL_PC, bl);
			tsc = status_get_sc(bl);
			if (battle_check_target(src, bl, BCT_ENEMY) <= 0)
				return 0; //Friendly fire isn't allowed
			if (tsc && !tsc->count)
				tsc = NULL; //Don't need it
			flag |= 2; //bugreport:2564 flag&2 disables double casting trigger
			dmg.blewcount = 0; //bugreport:7859 magical reflect'd zeroes blewcount
			if (type == 2 && tsc && tsc->data[SC_SPIRIT] && tsc->data[SC_SPIRIT]->val2 == SL_WIZARD) { //Spirit of Wizard blocks Kaite's reflection
				short i = (tsd ? pc_search_inventory(tsd, ITEMID_FRAGMENT_OF_CRYSTAL) : INDEX_NOT_FOUND);;

				if (i != INDEX_NOT_FOUND) { //Consume one fragment per hit of the casted skill? [Skotlex]
					if (tsd)
						pc_delitem(tsd, i, 1, 0, 1, LOG_TYPE_CONSUME);
					dmg.damage = dmg.damage2 = 0;
					dmg.dmg_lv = ATK_MISS;
					tsc->data[SC_SPIRIT]->val3 = skill_id;
					tsc->data[SC_SPIRIT]->val4 = dsrc->id;
				}
			} else if (type != 2) //Kaite bypasses
				additional_effects = false;
#if MAGIC_REFLECTION_TYPE //Official Magic Reflection Behavior : Reflected damage also affected by caster's gears
	#ifndef RENEWAL //In pre-renewal Kaite's reflection will do a full damage
			if (type != 2) {
	#endif
				if (dmg.dmg_lv != ATK_MISS) { //Wiz SL cancelled and consumed fragment
					int s_ele = skill_get_ele(skill_id, skill_lv);

					if (s_ele == -1) //The skill takes the weapon's element
						s_ele = sstatus->rhw.ele;
					else if (s_ele == -2) //Use status element
						s_ele = status_get_attack_sc_element(src, status_get_sc(src));
					else if (s_ele == -3) //Use random element
						s_ele = rnd()%ELE_ALL;
					dmg.damage = battle_attr_fix(bl, bl, dmg.damage, s_ele, status_get_element(bl), status_get_element_level(bl));
					if (tsc && tsc->data[SC_ENERGYCOAT]) {
						struct status_data *status = status_get_status_data(bl);
						int per = 100 * status->sp / status->max_sp - 1; //100% should be counted as the 80~99% interval

						per /= 20; //Uses 20% SP intervals
						//SP Cost: 1% + 0.5% per every 20% SP
						if (!status_charge(bl, 0, (10 + 5 * per) * status->max_sp / 1000))
							status_change_end(bl, SC_ENERGYCOAT, INVALID_TIMER);
						//Reduction: 6% + 6% every 20%
						dmg.damage -= dmg.damage * (6 * (1 + per)) / 100;
					}
				}
	#ifndef RENEWAL
			}
	#endif
		}
#endif
		if (tsc) {
			if (tsc->data[SC_MAGICROD] && !ground_skill) {
				int sp = skill_get_sp(skill_id, skill_lv);

				dmg.damage = dmg.damage2 = 0;
				dmg.dmg_lv = ATK_MISS; //This will prevent skill additional effect from taking effect [Skotlex]
				sp = sp * tsc->data[SC_MAGICROD]->val2 / 100;
				if (skill_id == WZ_WATERBALL && skill_lv > 1)
					sp = sp / ((skill_lv|1) * (skill_lv|1)); //Estimate SP cost of a single water-ball
				status_heal(bl, 0, sp, 2);
#ifndef RENEWAL
				clif_skill_nodamage(bl, bl, SA_MAGICROD, skill_lv, 1);
#endif
			}
		}
	}

	damage = dmg.damage + dmg.damage2;

	if (damage > 0 && bl->type == BL_MOB) {
		struct mob_data *md = BL_CAST(BL_MOB, bl);

		if (md && md->db->dmg_mod != 100)
			damage = damage * md->db->dmg_mod / 100;
	}

	switch (skill_id) {
		case AL_INCAGI:
		case CASH_INCAGI:
		case MER_INCAGI:
		case AL_BLESSING:
		case CASH_BLESSING:
		case MER_BLESSING:
			if (tsd && tsd->sc.data[SC_CHANGEUNDEAD])
				damage = 1;
			break;
		case SR_TIGERCANNON:
			if (!(flag&16))
				battle_damage_temp[0] = damage;
			break;
	}

	//Skill hit type
	type = (!skill_id ? DMG_SPLASH : skill_get_hit(skill_id));

	if (damage < dmg.div_ && (skill_id != CH_PALMSTRIKE || (skill_id == SC_TRIANGLESHOT && rnd()%100 >= 1 + skill_lv)))
		dmg.blewcount = 0; //Only pushback when it hit for other

	switch (skill_id) {
		case CR_GRANDCROSS:
		case NPC_GRANDDARKNESS:
			if (battle_config.gx_disptype)
				dsrc = src;
			if (bl->id == src->id)
				type = DMG_ENDURE;
			else
				flag |= SD_ANIMATION;
			break;
		case NJ_TATAMIGAESHI: //For correct knockback
			dsrc = src;
		//Fall through
		case MO_TRIPLEATTACK:
			flag |= SD_ANIMATION;
			break;
		case TK_COUNTER: { //Bonus from SG_FRIEND [Komurka]
				int level;

				if (sd && sd->status.party_id > 0 && (level = pc_checkskill(sd, SG_FRIEND) > 0))
					party_skill_check(sd, sd->status.party_id, TK_COUNTER, level);
			}
			break;
		case SL_STIN:
		case SL_STUN:
			if (!(sc && sc->data[SC_SMA]) && skill_lv >= 7)
				sc_start(src, src, SC_SMA, 100, skill_lv, skill_get_time(SL_SMA, skill_lv));
			break;
		case GN_WALLOFTHORN:
			type = flag;
			break;
		case SP_SPA:
			sc_start(src, src, SC_USE_SKILL_SP_SPA, 100, skill_lv, skill_get_time(skill_id, skill_lv));
			break;
		case SP_SHA:
			sc_start(src, src, SC_USE_SKILL_SP_SHA, 100, skill_lv, skill_get_time2(skill_id, skill_lv));
			break;
		case SP_SWHOO:
			sc_start(src, src, SC_USE_SKILL_SP_SHA, 100, skill_lv, skill_get_time(skill_id, skill_lv));
			break;
	}

	//Combo handling
	skill_combo(src, dsrc, bl, skill_id, skill_lv, tick);

	//Display damage
	switch (skill_id) {
		case PA_GOSPEL: //Should look like Holy Cross [Skotlex]
			dmg.dmotion = clif_skill_damage(src, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, CR_HOLYCROSS, -1, DMG_SPLASH);
			break;
		//Skills that need be passed as a normal attack for the client to display correctly
		case HVAN_EXPLOSION:
		case NPC_SELFDESTRUCTION:
			if (src->type == BL_PC)
				dmg.blewcount = 10;
			dmg.amotion = 0; //Disable delay or attack will do no damage since source is dead by the time it takes effect [Skotlex]
		//Fall through
		case KN_AUTOCOUNTER:
		case NPC_CRITICALSLASH:
		case TF_DOUBLE:
		case GS_CHAINACTION:
			dmg.dmotion = clif_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, (enum e_damage_type)dmg.type, dmg.damage2, false);
			break;
		case HW_GRAVITATION:
			dmg.dmotion = clif_damage(bl, bl, 0, 0, 0, damage, 1, DMG_ENDURE, 0, false);
			break;
		case AS_SPLASHER:
			if (flag&SD_ANIMATION) //The surrounding targets
				dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, -1, DMG_SPLASH); //Needs -1 as skill level
			else //The central target doesn't display an animation
				dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, -2, DMG_SPLASH); //Needs -2(!) as skill level
			break;
		case SN_SHARPSHOOTING:
		case MA_SHARPSHOOTING:
		case NJ_KAMAITACHI:
		case NPC_DARKPIERCING:
		case LG_CANNONSPEAR:
		case LG_MOONSLASHER:
		case GN_ILLUSIONDOPING:
		case MH_HEILIGE_STANGE:
		case EL_CIRCLE_OF_FIRE:
		case EL_FIRE_MANTLE:
		case EL_FIRE_BOMB_ATK:
		case EL_FIRE_WAVE_ATK:
		case EL_WATER_SCREW_ATK:
		case EL_TIDAL_WEAPON:
		case EL_HURRICANE_ATK:
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, NV_BASIC, -1, DMG_SPLASH);
			break;
		case EL_STONE_RAIN:
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, -1, (flag&1) ? DMG_MULTI_HIT : DMG_SPLASH);
			break;
		case AB_HIGHNESSHEAL:
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, AL_HEAL, -1, DMG_SKILL);
			break;
		case WL_HELLINFERNO:
			if (flag&ELE_DARK)
				dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, NV_BASIC, -1, DMG_SPLASH);
			else
				dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, skill_lv, DMG_SKILL);
			break;
		case NPC_COMET:
		case WL_COMET:
			dmg.dmotion = clif_skill_damage(src, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, skill_lv, DMG_MULTI_HIT);
			break;
		case WL_CHAINLIGHTNING_ATK:
		case GN_CRAZYWEED_ATK:
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, -1, DMG_SPLASH);
			break;
		case WL_TETRAVORTEX_FIRE:
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, WL_TETRAVORTEX_WIND, -1, DMG_SPLASH);
			break;
		case MG_FIREBALL:
		case GN_CARTCANNON:
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, -1, DMG_SKILL);
			break;
		case GN_SPORE_EXPLOSION:
			if (flag&SD_ANIMATION)
				dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, NV_BASIC, -1, DMG_SPLASH);
			else
				dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, -2, DMG_SPLASH);
			break;
		case GN_FIRE_EXPANSION_ACID:
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, CR_ACIDDEMONSTRATION, skill_lv, DMG_MULTI_HIT);
			break;
		case LG_SHIELDPRESS:
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, status_get_amotion(src), dmg.dmotion, damage, dmg.div_, skill_id, skill_lv, DMG_SKILL);
			break;
		case LG_OVERBRAND:
		case LG_OVERBRAND_BRANDISH:
			dmg.amotion = status_get_amotion(src) * 2;
		//Fall through
		case LG_OVERBRAND_PLUSATK:
		case RL_R_TRIP_PLUSATK:
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, status_get_amotion(src), dmg.dmotion, damage, dmg.div_, skill_id, -1, DMG_SPLASH);
			break;
		case WZ_SIGHTBLASTER:
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, (flag&SD_LEVEL) ? -1 : skill_lv, DMG_SPLASH);
			break;
		case HT_FLASHER:
		case HT_FREEZINGTRAP:
		case HT_BLASTMINE:
		case HT_CLAYMORETRAP:
		case RA_CLUSTERBOMB:
		case RA_FIRINGTRAP:
		case RA_ICEBOUNDTRAP:
			dmg.dmotion = clif_skill_damage(src, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, (flag&SD_LEVEL) ? -1 : skill_lv, DMG_SPLASH);
			if (ground_skill)
				break; //Avoid damage display redundancy
		//Fall through
		case HT_LANDMINE:
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, -1, DMG_SKILL);
			break;
		case RL_QD_SHOT:
			if (flag&SD_ANIMATION)
				dmg.dmotion = clif_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, DMG_MULTI_HIT, 0, false);
			else
				dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, -2, DMG_SKILL);
			break;
		case RL_H_MINE:
			if (flag&SD_ANIMATION) //Client has a glitch where the status animation appears on hit targets, even by exploaded mines [Rytech]
				dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, 0, -2, DMG_SPLASH);
			else
				dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, skill_lv, type);
			break;
		case SJ_NOVAEXPLOSING:
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, -2, DMG_SKILL);
			break;
		case SJ_FALLINGSTAR_ATK:
		case SJ_FALLINGSTAR_ATK2:
		case SP_CURSEEXPLOSION:
		case SP_SPA:
		case SP_SHA:
		case SU_BITE:
		case SU_SCRATCH:
		case SU_SV_STEMSPEAR:
		case SU_SCAROFTAROU:
		case SU_PICKYPECK:
		case SU_PICKYPECK_DOUBLE_ATK:
		case SU_LUNATICCARROTBEAT:
		case SU_LUNATICCARROTBEAT2:
			if (dmg.div_ < 2)
				type = DMG_SPLASH;
			if (!(flag&SD_ANIMATION))
				clif_skill_nodamage(dsrc, bl, skill_id, skill_lv, 1);
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, -2, type);
			break;
		case AB_DUPLELIGHT_MELEE:
		case AB_DUPLELIGHT_MAGIC:
			dmg.amotion = 300; //Makes the damage value not overlap with previous damage (when displayed by the client)
		//Fall through
		default:
			if ((flag&SD_ANIMATION) && dmg.div_ < 2) //Disabling skill animation doesn't works on multi-hit
				type = DMG_SPLASH;
			if (src->type == BL_SKILL) {
				TBL_SKILL *unit = (TBL_SKILL *)src;
				struct skill_unit_group *group = NULL;

				if (unit && (group = unit->group) && (skill_get_inf2(group->skill_id)&INF2_TRAP)) { //Show damage on trap targets
					clif_skill_damage(src, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, (flag&SD_LEVEL) ? -1 : skill_lv, DMG_SPLASH);
					break;
				}
			}
			dmg.dmotion = clif_skill_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, damage, dmg.div_, skill_id, (flag&SD_LEVEL) ? -1 : skill_lv, type);
			break;
	}

	map_freeblock_lock();

	//Can't copy skills if the blow will kill you [Skotlex]
	if (skill_id && skill_get_index(skill_id) > 0 && (dmg.damage + dmg.damage2) > 0 && damage < status_get_hp(bl))
		skill_do_copy(src, bl, skill_id, skill_lv);

	//Skills with can't walk delay also stop normal attacking for that duration when the attack connects [Skotlex]
	if (dmg.dmg_lv >= ATK_MISS && (type = skill_get_walkdelay(skill_id, skill_lv)) > 0) {
		struct unit_data *ud = unit_bl2ud(src);

		if (ud && DIFF_TICK(ud->attackabletime, tick + type) < 0)
			ud->attackabletime = tick + type;
	}

	if (!dmg.amotion) { //Instant damage
		if (!tsc || (!tsc->data[SC_DEVOTION] && !tsc->data[SC__SHADOWFORM] && !tsc->data[SC_WATER_SCREEN_OPTION] &&
			skill_id != CR_REFLECTSHIELD) || skill_id == HW_GRAVITATION || skill_id == NPC_EVILLAND)
			status_fix_damage(src, bl, damage, dmg.dmotion); //Deal damage before knockback to allow stuff like Fire Wall + Storm Gust combo
		if (!status_isdead(bl) && additional_effects)
			skill_additional_effect(src, bl, skill_id, skill_lv, dmg.flag, dmg.dmg_lv, tick);
		if (damage > 0) //Counter status effects [Skotlex]
			skill_counter_additional_effect(src, bl, skill_id, skill_lv, dmg.flag, tick);
	}

	if (!(flag&4)) //Blow!
		skill_attack_blow(src, dsrc, bl, (uint8)dmg.blewcount, skill_id, skill_lv, damage, tick, flag);

	if (dmg.amotion) //Delayed damage must be dealt after the knockback (it needs to know actual position of target)
		battle_delay_damage(tick, dmg.amotion, src, bl, dmg.flag, skill_id, skill_lv, damage, dmg.dmg_lv, dmg.dmotion, additional_effects, false, false);

	if (tsc) {
		if (skill_id != PA_PRESSURE && skill_id != HW_GRAVITATION && skill_id != NPC_EVILLAND &&
			skill_id != SJ_NOVAEXPLOSING && skill_id != SP_SOULEXPLOSION) {
			if (tsc->data[SC_DEVOTION]) {
				struct status_change_entry *sce_d = tsc->data[SC_DEVOTION];
				struct block_list *d_bl = map_id2bl(sce_d->val1);

				if (d_bl &&
					((d_bl->type == BL_MER && ((TBL_MER *)d_bl)->master && ((TBL_MER *)d_bl)->master->bl.id == bl->id) ||
					(d_bl->type == BL_PC && ((TBL_PC *)d_bl)->devotion[sce_d->val2] == bl->id)) &&
					check_distance_bl(bl, d_bl, sce_d->val3))
				{
					clif_damage(d_bl, d_bl, tick, 0, dmg.dmotion, damage, 0, DMG_NORMAL, 0, false);
					status_fix_damage(NULL, d_bl, damage, 0);
					if (damage > 0)
						skill_counter_additional_effect(src, d_bl, skill_id, skill_lv, dmg.flag, tick);
				} else {
					status_change_end(bl, SC_DEVOTION, INVALID_TIMER);
					if (!dmg.amotion)
						status_fix_damage(src, bl, damage, dmg.dmotion);
				}
			} else {
				if (tsc->data[SC__SHADOWFORM]) {
					struct status_change_entry *sce_s = tsc->data[SC__SHADOWFORM];
					struct map_session_data *s_sd = map_id2sd(sce_s->val2);

					if (s_sd && s_sd->shadowform_id == bl->id) {
						clif_damage(&s_sd->bl, &s_sd->bl, tick, 0, dmg.dmotion, damage, 0, DMG_NORMAL, 0, false);
						status_fix_damage(NULL, &s_sd->bl, damage, 0);
						if (damage > 0) {
							skill_counter_additional_effect(src, &s_sd->bl, skill_id, skill_lv, dmg.flag, tick);
							if (--(sce_s->val3) <= 0)
								status_change_end(bl, SC__SHADOWFORM, INVALID_TIMER);
						}
					}
				}
				if (tsc->data[SC_WATER_SCREEN_OPTION]) {
					struct status_change_entry *sce_e = tsc->data[SC_WATER_SCREEN_OPTION];
					struct block_list *e_bl = map_id2bl(sce_e->val1);

					if (e_bl) {
						clif_damage(e_bl, e_bl, tick, 0, dmg.dmotion, damage, 0, DMG_NORMAL, 0, false);
						status_fix_damage(NULL, e_bl, damage, 0);
					}
				}
			}
		}
	}

	if (damage > 0) { //Post-damage effects
		switch (skill_id) {
			case RG_INTIMIDATE: {
					int rate = (sd ? 50 + skill_lv * 5 : 90);

					rate = rate + status_get_lv(src) - status_get_lv(bl);
					if (rnd()%100 < rate && !status_has_mode(tstatus, MD_STATUS_IMMUNE))
						skill_addtimerskill(src, tick + status_get_amotion(src) + 500, bl->id, 0, 0, skill_id, skill_lv, 0, flag);
				}
				break;
			case WL_HELLINFERNO: //Hell Inferno has a chance of giving burning status when the fire damage hits
				if (!status_isdead(bl) && !(flag&ELE_DARK))
					sc_start(src, bl, SC_BURNING, 55 + 5 * skill_lv, skill_lv, skill_get_time2(skill_id, skill_lv));
				break;
			case SC_FATALMENACE:
				if (!status_has_mode(tstatus, MD_STATUS_IMMUNE))
					skill_addtimerskill(src, tick + 800, bl->id, skill_area_temp[4], skill_area_temp[5], skill_id, skill_lv, 0, flag);
				skill_addtimerskill(src, tick + 810, src->id, skill_area_temp[4], skill_area_temp[5], skill_id, skill_lv, 0, flag);
				break;
			case SR_TIGERCANNON: {
					int64 sp_damage = damage / 10; //10% of damage dealt

					clif_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, sp_damage, 1, DMG_NORMAL, 0, true);
					status_zap(bl, 0, sp_damage);
				}
				break;
			case WM_METALICSOUND: {
					int64 sp_damage = damage * 100 / (100 * (110 - (sd ? pc_checkskill(sd,WM_LESSON) : 10) * 10));

					clif_damage(dsrc, bl, tick, dmg.amotion, dmg.dmotion, sp_damage, 1, DMG_NORMAL, 0, true);
					status_zap(bl, 0, sp_damage);
				}
				break;
			case GN_BLOOD_SUCKER:
				status_heal(src, damage * (5 + 5 * skill_lv) / 100, 0, 0); //5 + 5% per level
				break;
		}
		if (skill_id && bl->type == BL_SKILL) { //Wall of Thorn damaged by Fire element skill
			struct skill_unit *unit = (struct skill_unit *)bl;
			struct skill_unit_group *group = NULL;
			struct block_list *src2 = NULL;
			int element = battle_get_weapon_element(&dmg, src, bl, skill_id, skill_lv, EQI_HAND_R);

			if (unit && (group = unit->group) && group->skill_id == GN_WALLOFTHORN && element == ELE_FIRE && (src2 = map_id2bl(group->src_id))) {
				group->unit_id = UNT_USED_TRAPS;
				group->limit = 0;
				src2->val1 = skill_get_time(group->skill_id, group->skill_lv) - DIFF_TICK(tick, group->tick); //Fire Wall duration [exneval]
				skill_unitsetting(src2, group->skill_id, group->skill_lv, group->val3>>16, group->val3&0xffff, 1);
			}
		}
		if (tsc && tsc->data[SC_GENSOU]) {
			battle_damage_temp[0] = damage;
			skill_castend_damage_id(bl, src, OB_OBOROGENSOU_TRANSITION_ATK, tsc->data[SC_GENSOU]->val1, tick, 0);
		}
		if (sd) {
			bool isdraindamage = ((dmg.flag&BF_WEAPON) && !battle_skill_check_no_cardfix_atk(skill_id));

			if (battle_config.left_cardfix_to_right)
				battle_drain(sd, bl, dmg.damage, dmg.damage, tstatus->race, tstatus->class_, isdraindamage);
			else
				battle_drain(sd, bl, dmg.damage, dmg.damage2, tstatus->race, tstatus->class_, isdraindamage);
			skill_onskillusage(sd, bl, skill_id, tick);
		}
	}

	if (!(flag&2)) {
		switch (skill_id) {
			case MG_COLDBOLT:
			case MG_FIREBOLT:
			case MG_LIGHTNINGBOLT:
				if (sc && sc->data[SC_DOUBLECAST] && rnd()%100 < sc->data[SC_DOUBLECAST]->val2)
					skill_addtimerskill(src, tick + dmg.amotion, bl->id, 0, 0, skill_id, skill_lv, attack_type, flag|2);
				break;
			case SU_BITE:
			case SU_SCRATCH:
			case SU_SV_STEMSPEAR:
			case SU_SCAROFTAROU:
			case SU_PICKYPECK:
			case SU_PICKYPECK_DOUBLE_ATK:
				if (rnd()%100 < 10 * status_get_lv(src) / 30)
					skill_addtimerskill(src, tick + dmg.amotion + 1000, bl->id, 0, 0, skill_id, skill_lv, attack_type, flag|2);
				break;
		}
	}

	map_freeblock_unlock();

	if ((flag&0x1000000) && rmdamage)
		return 0; //Should return 0 when damage was reflected

	return (int)cap_value(damage, INT_MIN, INT_MAX);
}

/*==========================================
 * Sub function for recursive skill call.
 * Checking bl battle flag and display damage
 * then call func with source,target,skill_id,skill_lv,tick,flag
 *------------------------------------------*/
typedef int (*SkillFunc)(struct block_list *, struct block_list *, int, int, unsigned int, int);
int skill_area_sub(struct block_list *bl, va_list ap)
{
	struct block_list *src;
	uint16 skill_id, skill_lv;
	int flag;
	unsigned int tick;
	SkillFunc func;

	nullpo_ret(bl);

	src = va_arg(ap,struct block_list *);
	skill_id = va_arg(ap,int);
	skill_lv = va_arg(ap,int);
	tick = va_arg(ap,unsigned int);
	flag = va_arg(ap,int);
	func = va_arg(ap,SkillFunc);

	//Several splash skills need this initial dummy packet to display correctly
	if (battle_check_target(src,bl,flag) > 0) {
		if ((flag&SD_PREAMBLE) && !skill_area_temp[2])
			clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
		if (flag&(SD_SPLASH|SD_PREAMBLE))
			skill_area_temp[2]++;
		return func(src,bl,skill_id,skill_lv,tick,flag);
	}
	return 0;
}

static int skill_check_unit_range_sub(struct block_list *bl, va_list ap)
{
	struct skill_unit *unit;
	uint16 skill_id, g_skill_id;

	if (!bl->prev || bl->type != BL_SKILL)
		return 0;

	unit = (struct skill_unit *)bl;

	if (!unit->alive)
		return 0;

	skill_id = va_arg(ap,int);
	g_skill_id = unit->group->skill_id;

	switch (skill_id) {
		case AL_PNEUMA:
			if (g_skill_id == SA_LANDPROTECTOR)
				break; //Pneuma doesn't work even if just one cell overlaps with Land Protector
		//Fall through
		case MG_SAFETYWALL:
		case MH_STEINWAND:
			if (g_skill_id != MG_SAFETYWALL && g_skill_id != MH_STEINWAND && g_skill_id != AL_PNEUMA)
				return 0;
			break;
		case HT_SKIDTRAP:
		case MA_SKIDTRAP:
		case HT_LANDMINE:
		case MA_LANDMINE:
		case HT_ANKLESNARE:
		case HT_SHOCKWAVE:
		case HT_SANDMAN:
		case MA_SANDMAN:
		case HT_FLASHER:
		case HT_FREEZINGTRAP:
		case MA_FREEZINGTRAP:
		case HT_BLASTMINE:
		case HT_CLAYMORETRAP:
		case HP_BASILICA:
			if (g_skill_id == AS_VENOMDUST || g_skill_id == MH_POISON_MIST)
				break;
		//Fall through
		case AL_WARP:
		case RA_ELECTRICSHOCKER:
		case RA_CLUSTERBOMB:
		case RA_MAGENTATRAP:
		case RA_COBALTTRAP:
		case RA_MAIZETRAP:
		case RA_VERDURETRAP:
		case RA_FIRINGTRAP:
		case RA_ICEBOUNDTRAP:
		case GN_THORNS_TRAP:
			if (skill_get_inf2(g_skill_id)&INF2_TRAP)
				break;
		//Fall through
		default:
			if (g_skill_id != skill_id) //Avoid stacking with same kind of trap [Skotlex]
				return 0;
			break;
	}
	return 1;
}

static int skill_check_unit_range(struct block_list *bl, int x, int y, uint16 skill_id, uint16 skill_lv)
{
	//Non players do not check for the skill's splash-trigger area
	int range = (bl->type == BL_PC ? skill_get_unit_range(skill_id,skill_lv) : 0);
	int layout_type = skill_get_unit_layout_type(skill_id,skill_lv);

	if (layout_type == -1 || layout_type > MAX_SQUARE_LAYOUT) {
		ShowError("skill_check_unit_range: unsupported layout type %d for skill %d\n",layout_type,skill_id);
		return 0;
	}

	range += layout_type;
	return map_foreachinallarea(skill_check_unit_range_sub,bl->m,x - range,y - range,x + range,y + range,BL_SKILL,skill_id);
}

static int skill_check_unit_range2_sub(struct block_list *bl, va_list ap)
{
	uint16 skill_id;

	if (!bl->prev)
		return 0;

	skill_id = va_arg(ap,int);

	if (status_isdead(bl) && skill_id != AL_WARP)
		return 0;

	if (skill_id == HP_BASILICA && bl->type == BL_PC)
		return 0;

	if (skill_id == AM_DEMONSTRATION && bl->type == BL_MOB && ((TBL_MOB *)bl)->mob_id == MOBID_EMPERIUM)
		return 0; //Allow casting Bomb/Demonstration Right under emperium [Skotlex]
	return 1;
}

/** Used to check range condition of the casted skill. Used if the skill has UF_NOFOOTSET or INF2_NO_NEARNPC
 * @param bl Object that casted skill
 * @param x Position x of the target
 * @param y Position y of the target
 * @param skill_id The casted skill
 * @param skill_lv The skill Lv
 * @param isNearNPC 'true' means, check the range between target and nearer NPC by using npc_isnear and range calculation [Cydh]
 * @return 0: No object (BL_CHAR or BL_PC) within the range. If 'isNearNPC' the target object is BL_NPC
 */
static int skill_check_unit_range2(struct block_list *bl, int x, int y, uint16 skill_id, uint16 skill_lv, bool isNearNPC)
{
	int range = 0, type;

	//Range for INF2_NO_NEARNPC is using skill splash value [Cydh]
	if (isNearNPC)
		range = skill_get_splash(skill_id,skill_lv);

	//While checking INF2_NO_NEARNPC and the range from splash is 0, get the range from skill_unit range and layout [Cydh]
	if (!isNearNPC || !range) {
		switch (skill_id) {
			case WZ_ICEWALL:
				range = 2;
				break;
			case GN_HELLS_PLANT:
				range = 0;
				break;
			default: {
					int layout_type = skill_get_unit_layout_type(skill_id,skill_lv);

					if (layout_type == -1 || layout_type > MAX_SQUARE_LAYOUT) {
						ShowError("skill_check_unit_range2: unsupported layout type %d for skill %d\n",layout_type,skill_id);
						return 0;
					}
					range = skill_get_unit_range(skill_id,skill_lv) + layout_type;
					if (!isNearNPC && (skill_id == SC_MANHOLE || skill_id == SC_DIMENSIONDOOR ||
						skill_id == SC_CHAOSPANIC || skill_id == WM_POEMOFNETHERWORLD))
						range = 0;
				}
				break;
		}
	}

	//Check the additional range [Cydh]
	if (isNearNPC && skill_db[skill_get_index(skill_id)].unit_nonearnpc_range)
		range += skill_db[skill_get_index(skill_id)].unit_nonearnpc_range;

	if (!isNearNPC) { //Doesn't check the NPC range
		//If the caster is a monster/NPC, only check for players. Otherwise just check characters
		if (bl->type == BL_PC)
			type = BL_CHAR;
		else
			type = BL_PC;
	} else
		type = BL_NPC;

	return (!isNearNPC ?
		/* !isNearNPC is used for UF_NOFOOTSET, regardless the NPC position, only check the BL_CHAR or BL_PC */
		map_foreachinallarea(skill_check_unit_range2_sub,bl->m,x - range,y - range,x + range,y + range,type,skill_id) :
		/* isNearNPC is used to check range from NPC */
		map_foreachinallarea(npc_isnear_sub,bl->m,x - range,y - range,x + range,y + range,type,skill_id));
}

/*==========================================
 * Checks that you have the requirements for casting a skill for homunculus/mercenary.
 * Flag:
 * &1: finished casting the skill (invoke hp/sp/item consumption)
 * &2: picked menu entry (Warp Portal, Teleport and other menu based skills)
 *------------------------------------------*/
static int skill_check_condition_mercenary(struct block_list *bl, uint16 skill_id, uint16 skill_lv, int type)
{
	struct status_data *status;
	struct status_change *sc;
	struct map_session_data *sd = NULL;
	int i, hp, sp, hp_rate, sp_rate, state, mhp, spiritball;
	uint16 idx = skill_get_index(skill_id);
	int itemid[MAX_SKILL_ITEM_REQUIRE], amount[ARRAYLENGTH(itemid)], index[ARRAYLENGTH(itemid)];

	if( skill_lv < 1 || skill_lv > MAX_SKILL_LEVEL )
		return 0;

	nullpo_retr(0,bl);

	switch( bl->type ) {
		case BL_HOM: sd = ((TBL_HOM *)bl)->master; break;
		case BL_MER: sd = ((TBL_MER *)bl)->master; break;
	}

	status = status_get_status_data(bl);
	sc = status_get_sc(bl);

	if( sc && !sc->count )
		sc = NULL;

	if( !idx )
		return 0;

	//Requirements
	for( i = 0; i < ARRAYLENGTH(itemid); i++ ) {
		itemid[i] = skill_db[idx].require.itemid[i];
		amount[i] = skill_db[idx].require.amount[i];
	}

	hp = skill_db[idx].require.hp[skill_lv - 1];
	sp = skill_db[idx].require.sp[skill_lv - 1];
	hp_rate = skill_db[idx].require.hp_rate[skill_lv - 1];
	sp_rate = skill_db[idx].require.sp_rate[skill_lv - 1];
	state = skill_db[idx].require.state;
	spiritball = skill_db[idx].require.spiritball[skill_lv - 1];
	if( (mhp = skill_db[idx].require.mhp[skill_lv - 1]) > 0 )
		hp += (status->max_hp * mhp) / 100;
	if( hp_rate > 0 )
		hp += (status->hp * hp_rate) / 100;
	else
		hp += (status->max_hp * (-hp_rate)) / 100;
	if( sp_rate > 0 )
		sp += (status->sp * sp_rate) / 100;
	else
		sp += (status->max_sp * (-sp_rate)) / 100;

	if( bl->type == BL_HOM ) {
		struct homun_data *hd = BL_CAST(BL_HOM,bl);

		switch( skill_id ) {
			case HFLI_SBR44:
				if( hd->homunculus.intimacy <= 200 ) { //hom_intimacy_grade2intimacy(HOMGRADE_HATE_WITH_PASSION)
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_RELATIONGRADE,0,0);
					return 0;
				}
				break;
			case HVAN_EXPLOSION:
				if( hd->homunculus.intimacy < (unsigned int)battle_config.hvan_explosion_intimate ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_RELATIONGRADE,0,0);
					return 0;
				}
				break;
			case MH_SUMMON_LEGION: {
					uint8 c = 0;

					//Check to see if any Hornet's, Giant Hornet's, or Luciola Vespa's are currently summoned
					for( i = MOBID_S_HORNET; i <= MOBID_S_LUCIOLA_VESPA; i++ )
						map_foreachinmap(skill_check_condition_mob_master_sub,hd->bl.m,BL_MOB,hd->bl.id,i,skill_id,&c);
					//If any of the above 3 summons are found, fail the skill
					if( c > 0 ) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						return 0;
					}
				}
				break;
			case MH_LIGHT_OF_REGENE:
				if( hom_get_intimacy_grade(hd) < HOMGRADE_LOYAL ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_RELATIONGRADE,0,0);
					return 0;
				}
				break;
			case MH_SONIC_CRAW:
				if( !(sc && sc->data[SC_STYLE_CHANGE] && sc->data[SC_STYLE_CHANGE]->val1 == MH_MD_FIGHTING) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_STYLE_CHANGE_FIGHTER,0,0);
					return 0;
				}
				break;
			case MH_SILVERVEIN_RUSH:
				if( !(sc && sc->data[SC_STYLE_CHANGE] && sc->data[SC_STYLE_CHANGE]->val1 == MH_MD_FIGHTING) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_STYLE_CHANGE_FIGHTER,0,0);
					return 0;
				}
				if( !(sc && sc->data[SC_SONIC_CLAW_POSTDELAY]) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_COMBOSKILL,MH_SONIC_CRAW,0);
					return 0;
				}
				break;
			case MH_MIDNIGHT_FRENZY:
				if( !(sc && sc->data[SC_STYLE_CHANGE] && sc->data[SC_STYLE_CHANGE]->val1 == MH_MD_FIGHTING) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_STYLE_CHANGE_FIGHTER,0,0);
					return 0;
				}
				if( !(sc && sc->data[SC_SILVERVEIN_RUSH_POSTDELAY]) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_COMBOSKILL,MH_SILVERVEIN_RUSH,0);
					return 0;
				}
				break;
			case MH_GOLDENE_FERSE:
				if( sc && sc->data[SC_ANGRIFFS_MODUS] )
					return 0; //Can't be used with MH_ANGRIFFS_MODUS
				break;
			case MH_ANGRIFFS_MODUS:
				if( sc && sc->data[SC_GOLDENE_FERSE] )
					return 0; //Can't be used with MH_GOLDENE_FERSE
				break;
			case MH_TINDER_BREAKER:
				if( !(sc && sc->data[SC_STYLE_CHANGE] && sc->data[SC_STYLE_CHANGE]->val1 == MH_MD_GRAPPLING) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_STYLE_CHANGE_GRAPPLER,0,0);
					return 0;
				}
				break;
			case MH_CBC:
				if( !(sc && sc->data[SC_STYLE_CHANGE] && sc->data[SC_STYLE_CHANGE]->val1 == MH_MD_GRAPPLING) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_STYLE_CHANGE_GRAPPLER,0,0);
					return 0;
				}
				if( !(sc && sc->data[SC_TINDER_BREAKER_POSTDELAY]) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_COMBOSKILL,MH_TINDER_BREAKER,0);
					return 0;
				}
				break;
			case MH_EQC:
				if( !(sc && sc->data[SC_STYLE_CHANGE] && sc->data[SC_STYLE_CHANGE]->val1 == MH_MD_GRAPPLING) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_STYLE_CHANGE_GRAPPLER,0,0);
					return 0;
				}
				if( !(sc && sc->data[SC_CBC_POSTDELAY]) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_COMBOSKILL,MH_CBC,0);
					return 0;
				}
				break;
		}
		if( spiritball > 0 ) {
			if( hd->homunculus.spiritball < spiritball ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_SPIRIT,spiritball,0);
				return 0;
			}
			hom_delspiritball(hd,spiritball,1);
		}
	}

	if( !(type&2) ) {
		if( hp > 0 && status->hp < (unsigned int)hp ) {
			clif_skill_fail(sd,skill_id,USESKILL_FAIL_HP_INSUFFICIENT,0,0);
			return 0;
		}
		if( sp > 0 && status->sp < (unsigned int)sp ) {
			clif_skill_fail(sd,skill_id,USESKILL_FAIL_SP_INSUFFICIENT,0,0);
			return 0;
		}
	}

	if( !type ) {
		switch( state ) {
			case ST_MOVE_ENABLE:
				if( !unit_can_move(bl) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					return 0;
				}
				break;
		}
	}

	if( !(type&1) )
		return 1;

	//Check item existences
	for( i = 0; i < ARRAYLENGTH(itemid); i++ ) {
		index[i] = INDEX_NOT_FOUND;
		if( itemid[i] <= 0 )
			continue; //No item
		index[i] = pc_search_inventory(sd, itemid[i]);
		if( index[i] == INDEX_NOT_FOUND || sd->inventory.u.items_inventory[index[i]].amount < amount[i] ) {
			clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			return 0;
		}
	}

	//Consume items
	for( i = 0; i < ARRAYLENGTH(itemid); i++ )
		if( index[i] != INDEX_NOT_FOUND )
			pc_delitem(sd,index[i],amount[i],0,1,LOG_TYPE_CONSUME);

	if( type&2 )
		return 1;

	if( sp || hp )
		status_zap(bl,hp,sp);

	return 1;
}

/*==========================================
 *
 *------------------------------------------*/
int skill_area_sub_count(struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, unsigned int tick, int flag)
{
	nullpo_ret(src);

	switch (skill_id) {
		case RL_D_TAIL: {
				struct status_change *tsc = status_get_sc(target);

				if (!(tsc && tsc->data[SC_C_MARKER] && tsc->data[SC_C_MARKER]->val2 == src->id))
					return 0;
			}
			break;
	}
	return 1;
}

/*==========================================
 *
 *------------------------------------------*/
static TIMER_FUNC(skill_timerskill)
{
	struct block_list *src = map_id2bl(id), *target;
	struct unit_data *ud = unit_bl2ud(src);
	struct skill_timerskill *skl;
	struct skill_unit *unit = NULL;
	int range;

	nullpo_ret(src);
	nullpo_ret(ud);

	skl = ud->skilltimerskill[data];

	nullpo_ret(skl);

	ud->skilltimerskill[data] = NULL;

	do {
		if (!src->prev)
			break; //Source not on Map
		if (skl->target_id) {
			target = map_id2bl(skl->target_id);
			if ((skl->skill_id == RG_INTIMIDATE || skl->skill_id == SC_FATALMENACE) &&
				(!target || !target->prev || !check_distance_bl(src,target,AREA_SIZE)))
				target = src; //Required since it has to warp
			if (!target || !target->prev || src->m != target->m) {
				if (skl->skill_id == SR_SKYNETBLOW) {
					clif_skill_damage(src,src,tick,status_get_amotion(src),0,-30000,1,skl->skill_id,skl->skill_lv,DMG_SKILL);
					skill_area_temp[1] = 0;
					map_foreachinallrange(skill_area_sub,src,skill_get_splash(skl->skill_id,skl->skill_lv),BL_CHAR|BL_SKILL,src,
						skl->skill_id,skl->skill_lv,tick,skl->flag|BCT_ENEMY|SD_SPLASH|1,skill_castend_damage_id);
				}
				break; //Target offline, not on map, or in different maps
			}
			if (status_isdead(src)) {
				switch(skl->skill_id) { //Exceptions
					case NPC_DANCINGBLADE_ATK:
					case WL_CHAINLIGHTNING_ATK:
					case WL_TETRAVORTEX_FIRE:
					case WL_TETRAVORTEX_WATER:
					case WL_TETRAVORTEX_WIND:
					case WL_TETRAVORTEX_GROUND:
					//For SR_FLASHCOMBO
					case SR_DRAGONCOMBO:
					case SR_FALLENEMPIRE:
					case SR_TIGERCANNON:
					case SR_SKYNETBLOW:
						break;
					default:
						continue; //Caster is dead
				}
			}
			if (status_isdead(target) && skl->skill_id != WZ_WATERBALL && skl->skill_id != RG_INTIMIDATE)
				break;
			switch (skl->skill_id) {
				case KN_AUTOCOUNTER:
					clif_skill_nodamage(src,target,skl->skill_id,skl->skill_lv,1);
					break;
				case RG_INTIMIDATE:
					if (!unit_warp(src,-1,-1,-1,CLR_TELEPORT)) {
						short x, y;

						map_search_freecell(src,0,&x,&y,1,1,0);
						if (target->id != src->id && !status_isdead(target))
							unit_warp(target,-1,x,y,CLR_TELEPORT);
					}
					break;
				case BA_FROSTJOKER:
				case DC_SCREAM:
					range = skill_get_splash(skl->skill_id,skl->skill_lv);
					map_foreachinallarea(skill_frostjoke_scream,skl->map,skl->x-range,skl->y-range,
						skl->x + range,skl->y + range,BL_CHAR,src,skl->skill_id,skl->skill_lv,tick);
					break;
				case PR_LEXDIVINA:
					if (src->type == BL_MOB) { //Monsters use the default duration when casting Lex Divina
						sc_start(src,target,SC_SILENCE,skl->type,skl->skill_lv,skill_get_time2(status_sc2skill(status_skill2sc(skl->skill_id)),1));
						break;
					}
				//Fall trough
				case MER_LEXDIVINA:
				case PR_STRECOVERY:
				case BS_HAMMERFALL:
					sc_start(src,target,status_skill2sc(skl->skill_id),skl->type,skl->skill_lv,skill_get_time2(skl->skill_id,skl->skill_lv));
					break;
				case WZ_WATERBALL: { //Get the next waterball cell to consume
						struct s_skill_unit_layout *layout;
						int i;

						layout = skill_get_unit_layout(skl->skill_id,skl->skill_lv,src,skl->x,skl->y);
						for (i = skl->type; i >= 0 && i < layout->count; i++) {
							int ux = skl->x + layout->dx[i];
							int uy = skl->y + layout->dy[i];

							if ((unit = map_find_skill_unit_oncell(src,ux,uy,WZ_WATERBALL,NULL,0)))
								break;
						}
					}
				//Fall through
				case WZ_JUPITEL: //Official behaviour is to hit as long as there is a line of sight, regardless of distance
					if (skl->type > 0 && !status_isdead(target) && path_search_long(NULL,src->m,src->x,src->y,target->x,target->y,CELL_CHKWALL)) {
						//Apply canact delay here to prevent hacks (unlimited casting)
						ud->canact_tick = max(tick + status_get_amotion(src), ud->canact_tick);
						skill_attack(BF_MAGIC,src,src,target,skl->skill_id,skl->skill_lv,tick,skl->flag);
					}
					if (unit && !status_isdead(target) && !status_isdead(src)) {
						skill_delunit(unit); //Consume unit for next waterball
						//Timer will continue and walkdelay set until target is dead, even if there is currently no line of sight
						unit_set_walkdelay(src,tick,TIMERSKILL_INTERVAL,1);
						skill_addtimerskill(src,tick + TIMERSKILL_INTERVAL,target->id,skl->x,skl->y,skl->skill_id,skl->skill_lv,skl->type + 1,skl->flag);
					} else {
						struct status_change *sc = status_get_sc(src);

						if (sc && sc->data[SC_SPIRIT] &&
							sc->data[SC_SPIRIT]->val2 == SL_WIZARD &&
							sc->data[SC_SPIRIT]->val3 == skl->skill_id)
							sc->data[SC_SPIRIT]->val3 = 0; //Clear bounced spell check
					}
					break;
				case NPC_DANCINGBLADE_ATK:
					skill_attack(BF_WEAPON,src,src,target,skl->skill_id,skl->skill_lv,tick,skl->flag);
					if (skl->type < 4) {
						struct block_list *nbl = NULL;

						nbl = battle_getenemyarea(src,target->x,target->y,5,splash_target(src),skill_area_temp[1],skl->skill_id);
						skill_addtimerskill(src,tick + 651,(nbl ? nbl : target)->id,0,0,NPC_DANCINGBLADE_ATK,skl->skill_lv,skl->type + 1,0);
					}
					break;
				case WL_CHAINLIGHTNING_ATK:
					skill_toggle_magicpower(src,skl->skill_id); //Only the first hit will be amplified
					skill_attack(BF_MAGIC,src,src,target,skl->skill_id,skl->skill_lv,tick,9 - skl->type); //Attack the current target
					if (skl->type < (4 + skl->skill_lv - 1) && skl->x < 3) { //Remaining bounces
						struct block_list *nbl = NULL; //Next bounce target

						//Search for a new target around current one in 7x7 range
						if (!(nbl = battle_getenemyarea(src,target->x,target->y,3,splash_target(src),target->id,skl->skill_id)))
							skl->x++;
						else
							skl->x = 0;
						skill_addtimerskill(src,tick + 651,(nbl ? nbl : target)->id,skl->x,0,WL_CHAINLIGHTNING_ATK,skl->skill_lv,skl->type + 1,0);
					}
					break;
				case WL_TETRAVORTEX_FIRE:
				case WL_TETRAVORTEX_WATER:
				case WL_TETRAVORTEX_WIND:
				case WL_TETRAVORTEX_GROUND:
					if (status_isimmune(target))
						break;
					clif_skill_nodamage(src,target,skl->skill_id,-1,1);
					skill_attack(BF_MAGIC,src,src,target,skl->skill_id,skl->skill_lv,tick,skl->flag|SD_LEVEL|SD_ANIMATION);
					if (skl->type == 4) { //Status inflicts are depend on what summoned element is used
						const enum sc_type scs[] = { SC_BURNING,SC_BLEEDING,SC_FREEZING,SC_STUN };
						int rate = skl->y, index = skl->x - 1, time = skill_get_time2(WL_TETRAVORTEX,index + 1);

						sc_start(src,target,scs[index],rate,skl->skill_lv,time);
					}
					break;
				case WL_RELEASE: {
						struct map_session_data *sd = NULL;
						struct status_change *sc = NULL;
						int i, delay;

						skill_toggle_magicpower(src,skl->skill_id); //No hit will be amplified
						if ((sc = status_get_sc(src)) && sc->data[SC_FREEZE_SP]) {
							uint16 r_skill_id, r_skill_lv, point, s = 0;
							int spell[SC_MAXSPELLBOOK - SC_SPELLBOOK1 + 1];

							for (i = SC_SPELLBOOK1; i <= SC_MAXSPELLBOOK; i++) { //List all available spell to be released
								if (sc->data[i])
									spell[s++] = i;
							}
							if (!s)
								break;
							i = spell[(s == 1 ? 0 : rnd()%s)]; //Random select of spell to be released
							if (sc->data[i]) { //Now extract the data from the preserved spell
								r_skill_id = sc->data[i]->val1;
								r_skill_lv = sc->data[i]->val2;
								point = sc->data[i]->val3;
								status_change_end(src,(sc_type)i,INVALID_TIMER);
							} else //Something went wrong
								break;
							if (sc->data[SC_FREEZE_SP]->val2 > point)
								sc->data[SC_FREEZE_SP]->val2 -= point;
							else //Last spell to be released
								status_change_end(src,SC_FREEZE_SP,INVALID_TIMER);
							if ((sd = map_id2sd(src->id))) {
								if (!skill_check_condition_castend(sd,r_skill_id,r_skill_lv))
									break;
								skill_consume_requirement(sd,r_skill_id,r_skill_lv,1);
							}
							switch (skill_get_casttype(r_skill_id)) {
								case CAST_GROUND:
									skill_castend_pos2(src,target->x,target->y,r_skill_id,r_skill_lv,tick,0);
									break;
								case CAST_NODAMAGE:
									skill_castend_nodamage_id(src,target,r_skill_id,r_skill_lv,tick,0);
									break;
								case CAST_DAMAGE:
									skill_castend_damage_id(src,target,r_skill_id,r_skill_lv,tick,0);
									break;
							}
							delay = skill_delayfix(src,r_skill_id,r_skill_lv);
							if (DIFF_TICK(ud->canact_tick,tick + delay) < 0) {
								ud->canact_tick = max(tick + delay,ud->canact_tick);
								if (battle_config.display_status_timers)
									clif_status_change(src,SI_POSTDELAY,1,delay,0,0,0);
							}
						}
					}
					break;
				case NPC_REVERBERATION_ATK:
				case WM_REVERBERATION_MELEE:
				case WM_REVERBERATION_MAGIC:
					skill_castend_damage_id(src,target,skl->skill_id,skl->skill_lv,tick,skl->flag|SD_LEVEL|SD_ANIMATION);
					break;
				case SC_FATALMENACE:
					if (target->id == src->id) { //Caster's part
						short x = skl->x, y = skl->y;

						map_search_freecell(0,src->m,&x,&y,2,2,1);
						unit_warp(src,-1,x,y,CLR_TELEPORT);
					} else //Target's part
						unit_warp(target,-1,skl->x,skl->y,CLR_TELEPORT);
					break;
				case LG_MOONSLASHER:
				case SR_WINDMILL:
					if (target->type == BL_PC) {
						struct map_session_data *tsd = ((TBL_PC *)target);

						if (tsd && !pc_issit(tsd)) {
							pc_setsit(tsd);
							skill_sit(tsd,true);
							clif_sitting(target);
						}
					}
					break;
				//For SR_FLASHCOMBO
				case SR_DRAGONCOMBO:
				case SR_FALLENEMPIRE:
				case SR_TIGERCANNON:
				case SR_SKYNETBLOW:
					if (distance_xy(src->x,src->y,target->x,target->y) >= 3)
						break;
					skill_castend_damage_id(src,target,skl->skill_id,skl->skill_lv,tick,skl->flag);
					break;
				case SU_BITE:
				case SU_SCRATCH:
				case SU_SV_STEMSPEAR:
				case SU_SCAROFTAROU:
				case SU_PICKYPECK:
				case SU_PICKYPECK_DOUBLE_ATK:
					{
						int delay = skill_delayfix(src,skl->skill_id,skl->skill_lv);

						skill_castend_damage_id(src,target,skl->skill_id,skl->skill_lv,tick,skl->flag);
						if (DIFF_TICK(ud->canact_tick,tick + delay) < 0) {
							ud->canact_tick = max(tick + delay,ud->canact_tick);
							if (battle_config.display_status_timers)
								clif_status_change(src,SI_POSTDELAY,1,delay,0,0,0);
						}
					}
					break;
				case NPC_PULSESTRIKE2:
					skill_castend_damage_id(src,target,skl->skill_id,skl->skill_lv,tick,skl->flag);
					break;
				case CH_PALMSTRIKE: {
						struct status_change *tsc = status_get_sc(target);
						struct status_change *sc = status_get_sc(src);

						if ((tsc && (tsc->option&OPTION_HIDE)) || (sc && (sc->option&OPTION_HIDE))) {
							skill_blown(src,target,skill_get_blewcount(skl->skill_id,skl->skill_lv),-1,0);
							break;
						}
					}
				//Fall through
				default:
					skill_attack(skl->type,src,src,target,skl->skill_id,skl->skill_lv,tick,skl->flag);
					break;
			}
		} else {
			if (src->m != skl->map)
				break;
			switch (skl->skill_id) {
				case NPC_RUN:
					skill_castend_nodamage_id(src,src,skl->skill_id,skl->skill_lv,tick,skl->flag);
					break;
				case GN_CRAZYWEED_ATK: {
						int dummy = 1, i = skill_get_unit_range(skl->skill_id,skl->skill_lv);

						map_foreachinallarea(skill_cell_overlap,src->m,skl->x-i,skl->y-i,skl->x+i,skl->y+i,BL_SKILL,skl->skill_id,&dummy,src);
					}
				//Fall through
				case WL_EARTHSTRAIN:
				case GN_WALLOFTHORN:
				case NC_MAGMA_ERUPTION:
					skill_unitsetting(src,skl->skill_id,skl->skill_lv,skl->x,skl->y,skl->flag);
					break;
				case LG_OVERBRAND_BRANDISH: {
						int i, dir = map_calc_dir(src,skl->x,skl->y);
						int x = src->x, y = src->y;
						struct s_skill_nounit_layout *layout = skill_get_nounit_layout(skl->skill_id,skl->skill_lv,src,x,y,dir);

						for (i = 0; i < layout->count; i++)
							map_foreachincell(skill_area_sub,src->m,x+layout->dx[i],y+layout->dy[i],BL_CHAR,src,skl->skill_id,skl->skill_lv,tick,skl->flag|BCT_ENEMY|SD_ANIMATION|1,skill_castend_damage_id);
					}
					break;
				case RL_FIRE_RAIN: {
						int dummy = 1, i = skill_get_splash(skl->skill_id,skl->skill_lv);

						if (rnd()%100 < 15 + 5 * skl->skill_lv)
							map_foreachinallarea(skill_cell_overlap,src->m,skl->x-i,skl->y-i,skl->x+i,skl->y+i,BL_SKILL,skl->skill_id,&dummy,src);
						skill_unitsetting(src,skl->skill_id,skl->skill_lv,skl->x,skl->y,skl->flag);
					}
					break;
			}
		}
	} while (0);
	//Free skl now that it is no longer needed
	ers_free(skill_timer_ers,skl);
	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
int skill_addtimerskill(struct block_list *src, unsigned int tick, int target, int x, int y, uint16 skill_id, uint16 skill_lv, int type, int flag)
{
	int i;
	struct unit_data *ud;

	nullpo_retr(1, src);

	if (!src->prev)
		return 0;

	ud = unit_bl2ud(src);

	nullpo_retr(1, ud);

	ARR_FIND(0, MAX_SKILLTIMERSKILL, i, !ud->skilltimerskill[i]);
	if (i == MAX_SKILLTIMERSKILL)
		return 1;

	ud->skilltimerskill[i] = ers_alloc(skill_timer_ers, struct skill_timerskill);
	ud->skilltimerskill[i]->timer = add_timer(tick, skill_timerskill, src->id, i);
	ud->skilltimerskill[i]->src_id = src->id;
	ud->skilltimerskill[i]->target_id = target;
	ud->skilltimerskill[i]->skill_id = skill_id;
	ud->skilltimerskill[i]->skill_lv = skill_lv;
	ud->skilltimerskill[i]->map = src->m;
	ud->skilltimerskill[i]->x = x;
	ud->skilltimerskill[i]->y = y;
	ud->skilltimerskill[i]->type = type;
	ud->skilltimerskill[i]->flag = flag;
	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
int skill_cleartimerskill(struct block_list *src)
{
	int i;
	struct unit_data *ud;

	nullpo_ret(src);
	ud = unit_bl2ud(src);
	nullpo_ret(ud);

	for (i = 0; i < MAX_SKILLTIMERSKILL; i++) {
		if (ud->skilltimerskill[i]) {
			switch (ud->skilltimerskill[i]->skill_id) {
				case WL_TETRAVORTEX_FIRE:
				case WL_TETRAVORTEX_WATER:
				case WL_TETRAVORTEX_WIND:
				case WL_TETRAVORTEX_GROUND:
				//For SR_FLASHCOMBO
				case SR_DRAGONCOMBO:
				case SR_FALLENEMPIRE:
				case SR_TIGERCANNON:
				case SR_SKYNETBLOW:
					continue;
			}
			delete_timer(ud->skilltimerskill[i]->timer, skill_timerskill);
			ers_free(skill_timer_ers, ud->skilltimerskill[i]);
			ud->skilltimerskill[i] = NULL;
		}
	}
	return 1;
}

static int skill_active_reverberation(struct block_list *bl, va_list ap) {
	struct skill_unit *unit = (TBL_SKILL *)bl;
	struct skill_unit_group *group = NULL;

	nullpo_ret(unit);

	if (bl->type != BL_SKILL)
		return 0;
	if (unit->alive && (group = unit->group) && group->unit_id == UNT_REVERBERATION) {
		map_foreachinallrange(skill_trap_splash, bl, skill_get_splash(group->skill_id, group->skill_lv), group->bl_flag, bl, gettick());
		unit->limit = DIFF_TICK(gettick(), group->tick);
		group->unit_id = UNT_USED_TRAPS;
	}
	return 1;
}

/**
 * Reveal hidden trap
 */
static int skill_reveal_trap(struct block_list *bl, va_list ap)
{
	TBL_SKILL *unit = (TBL_SKILL *)bl;
	struct skill_unit_group *group = NULL;

	if (unit->alive && (group = unit->group) && unit->hidden && skill_get_inf2(group->skill_id)&INF2_TRAP) {
		//Change look is not good enough, the client ignores it as an actual trap still [Skotlex]
		//clif_changetraplook(bl, group->unit_id);
		unit->hidden = false;
		skill_getareachar_skillunit_visibilty(unit, AREA);
		return 1;
	}
	return 0;
}

/**
 * Attempt to reaveal trap in area
 * @param src Skill caster
 * @param range Affected range
 * @param x
 * @param y
 * TODO: Remove hardcode usages for this function
 */
void skill_reveal_trap_inarea(struct block_list *src, int range, int x, int y)
{
	if (!battle_config.traps_setting)
		return;
	nullpo_retv(src);
	map_foreachinallarea(skill_reveal_trap, src->m, x-range, y-range, x+range, y+range, BL_SKILL);
}

/**
 * Process tarot card's effects
 * @author [Playtester]
 * @param src: Source of the tarot card effect
 * @param bl: Target of the tartor card effect
 * @param skill_id: ID of the skill used
 * @param skill_lv: Level of the skill used
 * @param tick: Processing tick time
 * @return Card number
 */
static int skill_tarotcard(struct block_list *src, struct block_list *bl, uint16 skill_id, uint16 skill_lv, int tick)
{
	uint8 card = 0;

	if (battle_config.tarotcard_equal_chance) //eAthena equal chances
		card = rnd()%14 + 1;
	else { //Official chances
		int rate = rnd()%100;

		if (rate < 10) card = 1;
		else if (rate < 20) card = 2;
		else if (rate < 30) card = 3;
		else if (rate < 37) card = 4;
		else if (rate < 47) card = 5;
		else if (rate < 62) card = 6;
		else if (rate < 63) card = 7;
		else if (rate < 69) card = 8;
		else if (rate < 74) card = 9;
		else if (rate < 82) card = 10;
		else if (rate < 83) card = 11;
		else if (rate < 85) card = 12;
		else if (rate < 90) card = 13;
		else card = 14;
	}
	switch (card) {
		case 1: //The Fool
			status_percent_damage(src, bl, 0, 100, false);
			break;
		case 2: //The Magician
			sc_start(src, bl, SC_INCMATKRATE, 100, -50, skill_get_time2(skill_id, skill_lv));
			break;
		case 3: //The High Priestess
			status_change_clear_buffs(bl, SCCB_BUFFS|SCCB_CHEM_PROTECT, 0);
			break;
		case 4: //The Chariot
			status_fix_damage(src, bl, 1000,
				clif_damage(src, bl, tick, 0, 0, 1000, 0, DMG_NORMAL, 0, false));
			if (!status_isdead(bl)) {
				int pos[] = { EQP_HELM,EQP_SHIELD,EQP_ARMOR };

				skill_break_equip(src, bl, pos[rnd()%3], 10000, BCT_ENEMY);
			}
			break;
		case 5: //Strength
			sc_start(src, bl, SC_INCATKRATE, 100, -50, skill_get_time2(skill_id, skill_lv));
			break;
		case 6: //The Lovers
			if (!map_flag_vs(bl->m))
				unit_warp(bl, -1, -1, -1, CLR_TELEPORT);
			status_heal(src, 2000, 0, 0);
			break;
		case 7: //Wheel of Fortune
			//Recursive call
			skill_tarotcard(src, bl, skill_id, skill_lv, tick);
			skill_tarotcard(src, bl, skill_id, skill_lv, tick);
			break;
		case 8: { //The Hanged Man
				enum sc_type sc[] = { SC_STONE,SC_FREEZE,SC_STOP };
				uint8 rand_eff = rnd()%3;
				int time = (!rand_eff ? skill_get_time2(skill_id, skill_lv) : skill_get_time2(status_sc2skill(sc[rand_eff]), 1));

				sc_start(src, bl, sc[rand_eff], 100, skill_lv, time);
			}
			break;
		case 9: //Death
			sc_start(src, bl, SC_POISON, 100, skill_lv, skill_get_time2(status_sc2skill(SC_POISON), 1));
			sc_start(src, bl, SC_CURSE, 100, skill_lv, skill_get_time2(status_sc2skill(SC_CURSE), 1));
			sc_start(src, bl, SC_COMA, 100, skill_lv, 0);
			break;
		case 10: //Temperance
			sc_start(src, bl, SC_CONFUSION, 100, skill_lv, skill_get_time2(skill_id, skill_lv));
			break;
		case 11: //The Devil
			status_fix_damage(src, bl, 6666,
				clif_damage(src, bl, tick, 0, 0, 6666, 0, DMG_NORMAL, 0, false));
			sc_start(src, bl, SC_INCATKRATE, 100, -50, skill_get_time2(skill_id, skill_lv));
			sc_start(src, bl, SC_INCMATKRATE, 100, -50, skill_get_time2(skill_id, skill_lv));
			sc_start(src, bl, SC_CURSE, 100, skill_lv, skill_get_time2(status_sc2skill(SC_CURSE), 1));
			break;
		case 12: //The Tower
			status_fix_damage(src, bl, 4444,
				clif_damage(src, bl, tick, 0, 0, 4444, 0, DMG_NORMAL, 0, false));
			break;
		case 13: //The Star
			sc_start(src, bl, SC_STUN, 100, skill_lv, skill_get_time2(status_sc2skill(SC_STUN), 1));
			break;
		case 14: //The Sun
#ifdef RENEWAL
			sc_start(src, bl, SC_TAROTCARD, 100, skill_lv, skill_get_time2(skill_id, skill_lv));
#endif
			sc_start(src,bl,SC_INCATKRATE, 100, -20, skill_get_time2(skill_id, skill_lv));
			sc_start(src,bl,SC_INCMATKRATE, 100, -20, skill_get_time2(skill_id, skill_lv));
			sc_start(src,bl,SC_INCHITRATE, 100, -20, skill_get_time2(skill_id, skill_lv));
			sc_start(src,bl,SC_INCFLEERATE, 100, -20, skill_get_time2(skill_id, skill_lv));
			sc_start(src,bl,SC_INCDEFRATE, 100, -20, skill_get_time2(skill_id, skill_lv));
			break;
	}

	return card;
}

/*==========================================
 *
 *
 *------------------------------------------*/
int skill_castend_damage_id(struct block_list *src, struct block_list *bl, uint16 skill_id, uint16 skill_lv, unsigned int tick, int flag)
{
	struct map_session_data *sd;
	struct status_data *sstatus, *tstatus;
	struct status_change *sc, *tsc;

	if (skill_id && !skill_lv)
		return 0;

	nullpo_retr(1,src);
	nullpo_retr(1,bl);

	if (src->m != bl->m)
		return 1;

	sd = BL_CAST(BL_PC,src);

	if (!bl->prev || status_isdead(bl))
		return 1;

	switch (skill_id) {
		case WL_TETRAVORTEX:
		case WL_RELEASE:
			break;
		default: //GTB makes all targeted magic display miss with a single bolt
			if (skill_get_type(skill_id) == BF_MAGIC && status_isimmune(bl)) {
				sc_type sct = status_skill2sc(skill_id);

				if(sct != SC_NONE)
					status_change_end(bl,sct,INVALID_TIMER);
				clif_skill_damage(src,bl,tick,status_get_amotion(src),status_get_dmotion(bl),0,1,
					skill_id,skill_lv,skill_get_hit(skill_id));
				return 1;
			}
			break;
	}

	sc = status_get_sc(src);
	tsc = status_get_sc(bl);

	if (sc && !sc->count)
		sc = NULL; //Unneeded

	if (tsc && !tsc->count)
		tsc = NULL;

	sstatus = status_get_status_data(src);
	tstatus = status_get_status_data(bl);

	map_freeblock_lock();

	switch (skill_id) {
		case MER_CRASH:
		case SM_BASH:
		case MS_BASH:
		case MC_MAMMONITE:
		case TF_DOUBLE:
		case AC_DOUBLE:
		case MA_DOUBLE:
		case AS_SONICBLOW:
		case KN_PIERCE:
		case ML_PIERCE:
		case KN_SPEARBOOMERANG:
		case TF_SPRINKLESAND:
		case AC_CHARGEARROW:
		case MA_CHARGEARROW:
		case RG_INTIMIDATE:
#ifndef RENEWAL
		case AM_ACIDTERROR:
#endif
		case BA_MUSICALSTRIKE:
		case DC_THROWARROW:
		case BA_DISSONANCE:
		case CR_HOLYCROSS:
		case NPC_DARKCROSS:
		case CR_SHIELDCHARGE:
		case CR_SHIELDBOOMERANG:
		case MO_TRIPLEATTACK:
		case NPC_PIERCINGATT:
		case NPC_MENTALBREAKER:
		case NPC_RANGEATTACK:
		case NPC_CRITICALSLASH:
		case NPC_COMBOATTACK:
		case NPC_POISON:
		case NPC_RANDOMATTACK:
		case NPC_WATERATTACK:
		case NPC_GROUNDATTACK:
		case NPC_FIREATTACK:
		case NPC_WINDATTACK:
		case NPC_POISONATTACK:
		case NPC_HOLYATTACK:
		case NPC_DARKNESSATTACK:
		case NPC_TELEKINESISATTACK:
		case NPC_UNDEADATTACK:
		case NPC_ARMORBRAKE:
		case NPC_WEAPONBRAKER:
		case NPC_HELMBRAKE:
		case NPC_SHIELDBRAKE:
		case NPC_BLINDATTACK:
		case NPC_SILENCEATTACK:
		case NPC_STUNATTACK:
		case NPC_PETRIFYATTACK:
		case NPC_CURSEATTACK:
		case NPC_SLEEPATTACK:
		case LK_AURABLADE:
		case LK_SPIRALPIERCE:
		case ML_SPIRALPIERCE:
		case LK_HEADCRUSH:
		case CG_ARROWVULCAN:
		case HW_MAGICCRASHER:
		case ITM_TOMAHAWK:
		case CH_CHAINCRUSH:
		case CH_TIGERFIST:
		case PA_SHIELDCHAIN: //Shield Chain
		case PA_SACRIFICE:
		case WS_CARTTERMINATION: //Cart Termination
		case AS_VENOMKNIFE:
		case HT_PHANTASMIC:
		case TK_DOWNKICK:
		case TK_COUNTER:
		case GS_CHAINACTION:
		case GS_TRIPLEACTION:
		case GS_BULLSEYE:
#ifndef RENEWAL
		case GS_MAGICALBULLET:
#endif
		case GS_TRACKING:
		case GS_PIERCINGSHOT:
		case GS_RAPIDSHOWER:
		case GS_DUST:
		case GS_DISARM: //Added disarm [Reddozen]
		case GS_FULLBUSTER:
		case NJ_SYURIKEN:
		case NJ_KUNAI:
		case HFLI_MOON: //[orn]
		case HFLI_SBR44: //[orn]
		case NPC_BLEEDING:
		case NPC_CRITICALWOUND:
		case NPC_HELLPOWER:
		case RK_SONICWAVE:
		case RK_HUNDREDSPEAR:
		case RK_STORMBLAST:
		case AB_DUPLELIGHT_MELEE:
		case RA_AIMEDBOLT:
		case NC_BOOSTKNUCKLE:
		case NC_PILEBUNKER:
		case NC_AXEBOOMERANG:
		case NC_POWERSWING:
		case GC_CROSSIMPACT:
		case GC_WEAPONCRUSH:
		case GC_VENOMPRESSURE:
		case SC_TRIANGLESHOT:
		case SC_FEINTBOMB:
		case LG_BANISHINGPOINT:
		case LG_OVERBRAND:
		case LG_OVERBRAND_BRANDISH:
		case LG_SHIELDPRESS:
		case LG_RAGEBURST:
		case LG_HESPERUSLIT:
		case SR_DRAGONCOMBO:
		case SR_FALLENEMPIRE:
		case SR_CRESCENTELBOW_AUTOSPELL:
		case SR_GATEOFHELL:
		case SR_GENTLETOUCH_QUIET:
		case SR_HOWLINGOFLION:
		case WM_SEVERE_RAINSTORM_MELEE:
		case WM_GREAT_ECHO:
		case GN_SLINGITEM_RANGEMELEEATK:
		case NC_MAGMA_ERUPTION:
		case RL_AM_BLAST:
		case RL_SLUGSHOT:
		case KO_SETSUDAN:
		case SU_BITE:
		case SU_SCAROFTAROU:
		case MH_NEEDLE_OF_PARALYZE:
		case MH_SONIC_CRAW:
		case MH_SILVERVEIN_RUSH:
		case MH_MIDNIGHT_FRENZY:
		case MH_CBC:
		case EL_WIND_SLASH:
		case MER_INVINCIBLEOFF2:
			skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case NPC_GUIDEDATTACK:
			skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			sc_start(src,src,status_skill2sc(skill_id),100,skill_lv,skill_get_time(skill_id,skill_lv));
			break;

		case TF_POISON:
			skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			if (!sc_start(src,bl,SC_POISON,10 + skill_lv * 4,skill_lv,skill_get_time2(skill_id,skill_lv)) && sd)
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			break;

		case LK_JOINTBEAT: //Decide the ailment first (affects attack damage and effect)
			switch (rnd()%6) {
				case 0: flag |= BREAK_ANKLE; break;
				case 1: flag |= BREAK_WRIST; break;
				case 2: flag |= BREAK_KNEE; break;
				case 3: flag |= BREAK_SHOULDER; break;
				case 4: flag |= BREAK_WAIST; break;
				case 5: flag |= BREAK_NECK; break;
			}
			if (tsc) //@TODO: Is there really no cleaner way to do this?
				tsc->jb_flag = flag;
			skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case MO_COMBOFINISH: //Becomes a splash attack when Soul Linked
			if (!(flag&1) && sc && sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_MONK) {
				map_foreachinshootrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR|BL_SKILL,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_damage_id);
			} else
				skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case TK_STORMKICK: //Taekwon kicks [Dralnu]
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			skill_area_temp[1] = 0;
			map_foreachinshootrange(skill_attack_area,src,skill_get_splash(skill_id,skill_lv),BL_CHAR|BL_SKILL,
				BF_WEAPON,src,src,skill_id,skill_lv,tick,flag,BCT_ENEMY);
			break;

		case KN_CHARGEATK: {
				bool path2 = path_search_long(NULL,src->m,src->x,src->y,bl->x,bl->y,CELL_CHKWALL);
				unsigned int dist = distance_bl(src,bl);
				uint8 dir = map_calc_dir(bl,src->x,src->y);

				//Teleport to target (if not on WoE grounds)
				if (!map_flag_gvg2(src->m) && !mapdata[src->m].flag.battleground && unit_movepos(src,bl->x,bl->y,0,true))
					skill_blown(src,src,1,(dir + 4)%8,0); //Target position is actually one cell next to the target
				if (path2) { //Cause damage and knockback if the path to target was a straight one
					if (skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,dist))
						skill_blown(src,bl,dist,dir,0);
					//HACK: Since knockback officially defaults to the left, the client also turns to the left
					//Therefore make the caster look in the direction of the target
					unit_setdir(src,(dir + 4)%8);
				}
			}
			break;

		case NC_FLAMELAUNCHER:
		case LG_CANNONSPEAR:
			if (skill_id == LG_CANNONSPEAR)
				clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			skill_area_temp[1] = bl->id;
			if (battle_config.skill_eightpath_algorithm) { //Use official AoE algorithm
				map_foreachindir(skill_attack_area,src->m,src->x,src->y,bl->x,bl->y,
					skill_get_splash(skill_id,skill_lv),skill_get_maxcount(skill_id,skill_lv),0,splash_target(src),
					skill_get_type(skill_id),src,src,skill_id,skill_lv,tick,flag,BCT_ENEMY);
			} else {
				map_foreachinpath(skill_attack_area,src->m,src->x,src->y,bl->x,bl->y,
					skill_get_splash(skill_id,skill_lv),skill_get_maxcount(skill_id,skill_lv),splash_target(src),
					skill_get_type(skill_id),src,src,skill_id,skill_lv,tick,flag,BCT_ENEMY);
			}
			break;

		case SN_SHARPSHOOTING:
		case MA_SHARPSHOOTING:
		case NJ_KAMAITACHI:
		case NPC_DARKPIERCING:
			clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
		//Fall through
		case NPC_ACIDBREATH:
		case NPC_DARKNESSBREATH:
		case NPC_FIREBREATH:
		case NPC_ICEBREATH:
		case NPC_THUNDERBREATH:
			skill_area_temp[1] = bl->id;
			if (battle_config.skill_eightpath_algorithm) { //These skills hit at least the target if the AoE doesn't hit
				if (!(map_foreachindir(skill_attack_area,src->m,src->x,src->y,bl->x,bl->y,
					skill_get_splash(skill_id,skill_lv),skill_get_maxcount(skill_id,skill_lv),0,splash_target(src),
					skill_get_type(skill_id),src,src,skill_id,skill_lv,tick,flag,BCT_ENEMY)))
					skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag);
			} else {
				map_foreachinpath(skill_attack_area,src->m,src->x,src->y,bl->x,bl->y,
					skill_get_splash(skill_id,skill_lv),skill_get_maxcount(skill_id,skill_lv),splash_target(src),
					skill_get_type(skill_id),src,src,skill_id,skill_lv,tick,flag,BCT_ENEMY);
			}
			if (skill_id == SN_SHARPSHOOTING)
				status_change_end(src,SC_CAMOUFLAGE,INVALID_TIMER);
			break;

		case MO_INVESTIGATE:
			status_change_end(src,SC_BLADESTOP,INVALID_TIMER);
			skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case RG_BACKSTAP:
			status_change_end(src,SC_HIDING,INVALID_TIMER);
			skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case MO_FINGEROFFENSIVE:
			status_change_end(src,SC_BLADESTOP,INVALID_TIMER);
			skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			if (battle_config.finger_offensive_type && sd) {
				int i;

				for (i = 1; i < sd->spiritball_old; i++)
					skill_addtimerskill(src,tick + i * 200,bl->id,0,0,skill_id,skill_lv,BF_WEAPON,flag);
			}
			break;

		case MO_CHAINCOMBO:
			status_change_end(src,SC_BLADESTOP,INVALID_TIMER);
			skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case NJ_ISSEN:
		case MO_EXTREMITYFIST:
			{
				struct block_list *mbl = bl; //For NJ_ISSEN
				short x, y, i = 2; //Move 2 cells (From target)
				short dir = map_calc_dir(src,bl->x,bl->y);

				if (skill_id == MO_EXTREMITYFIST) {
					mbl = src; //For MO_EXTREMITYFIST
					i = 3; //Move 3 cells (From caster)
				}
				if (dir > 0 && dir < 4)
					x = -i;
				else if (dir > 4)
					x = i;
				else
					x = 0;
				if (dir > 2 && dir < 6)
					y = -i;
				else if (dir == 7 || dir < 2)
					y = i;
				else
					y = 0;
				if ((mbl == src || //Ashura Strike still has slide effect in GVG
					(!map_flag_gvg2(src->m) && !mapdata[src->m].flag.battleground))) {
					//The cell is not reachable (wall, object, ...), move next to the target
					if (!(unit_movepos(src,mbl->x + x,mbl->y + y,1,true))) {
						if (x > 0)
							x = -1;
						else if (x < 0)
							x = 1;
						if (y > 0)
							y = -1;
						else if (y < 0)
							y = 1;
						unit_movepos(src,bl->x + x,bl->y + y,1,true);
					}
					clif_blown(src,mbl);
					clif_spiritball(src);
				}
				if (battle_check_target(src,bl,BCT_ENEMY) > 0 && !(tsc && tsc->data[SC_HIDING])) //For MO_EXTREMITYFIST
					skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag);
				if (skill_id == MO_EXTREMITYFIST) {
					status_set_sp(src,0,0);
					status_change_end(src,SC_EXPLOSIONSPIRITS,INVALID_TIMER);
					status_change_end(src,SC_BLADESTOP,INVALID_TIMER);
#ifdef RENEWAL
					sc_start(src,src,SC_EXTREMITYFIST2,100,skill_lv,skill_get_time(skill_id,skill_lv));
#endif
				} else {
#ifdef RENEWAL
					status_set_hp(src,umax(status_get_max_hp(src) / 100,1),0);
#else
					status_set_hp(src,1,0);
#endif
					status_change_end(src,SC_NEN,INVALID_TIMER);
					status_change_end(src,SC_HIDING,INVALID_TIMER);
				}
			}
			break;

		case HT_POWER:
			if (tstatus->race == RC_BRUTE || tstatus->race == RC_INSECT)
				skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		//Splash attack skills
		case AS_GRIMTOOTH:
		case MC_CARTREVOLUTION:
		case NPC_SPLASHATTACK:
			flag |= SD_PREAMBLE; //A fake packet will be sent for the first target to be hit
		case SM_MAGNUM:
		case MS_MAGNUM:
		case AS_SPLASHER:
		case HT_BLITZBEAT:
		case AC_SHOWER:
		case MA_SHOWER:
		case MG_NAPALMBEAT:
		case MG_FIREBALL:
		case RG_RAID:
		case HW_NAPALMVULCAN:
		case NJ_HUUMA:
		case ASC_METEORASSAULT:
		case GS_SPREADATTACK:
		case NPC_EARTHQUAKE:
		case NPC_PULSESTRIKE:
		case NPC_PULSESTRIKE2:
		case NPC_HELLJUDGEMENT:
		case NPC_VAMPIRE_GIFT:
		case NPC_MAXPAIN_ATK:
		case NPC_JACKFROST:
		case NPC_REVERBERATION_ATK:
		case RK_WINDCUTTER:
		case RK_IGNITIONBREAK:
		case RK_DRAGONBREATH:
		case RK_DRAGONBREATH_WATER:
		case AB_JUDEX:
		case AB_ADORAMUS:
		case WL_SOULEXPANSION:
		case WL_CRIMSONROCK:
		case WL_FROSTMISTY:
		case WL_JACKFROST:
		case RA_ARROWSTORM:
		case RA_WUGDASH:
		case NC_VULCANARM:
		case NC_COLDSLOWER:
		case NC_ARMSCANNON:
		case NC_SELFDESTRUCTION:
		case NC_AXETORNADO:
		case GC_ROLLINGCUTTER:
		case GC_COUNTERSLASH:
		case SC_FATALMENACE:
		case LG_MOONSLASHER:
		case LG_EARTHDRIVE:
		case SR_SKYNETBLOW:
		case SR_TIGERCANNON:
		case SR_RAMPAGEBLASTER:
		case SR_WINDMILL:
		case SR_RIDEINLIGHTNING:
		case WM_REVERBERATION_MELEE:
		case WM_REVERBERATION_MAGIC:
		case SO_EARTHGRAVE:
		case SO_DIAMONDDUST:
		case SO_POISON_BUSTER:
		case SO_VARETYR_SPEAR:
		case GN_CART_TORNADO:
		case GN_CARTCANNON:
		case GN_SPORE_EXPLOSION:
		case GN_DEMONIC_FIRE:
		case GN_FIRE_EXPANSION_ACID:
		case GN_ILLUSIONDOPING:
		case RL_S_STORM:
		case RL_FIREDANCE:
		case RL_R_TRIP:
		case RL_HAMMER_OF_GOD:
		case SJ_FULLMOONKICK:
		case SJ_NEWMOONKICK:
		case SJ_STAREMPEROR:
		case SJ_SOLARBURST:
		case SJ_FALLINGSTAR_ATK2:
		case SP_CURSEEXPLOSION:
		case SP_SHA:
		case SP_SWHOO:
		case KO_BAKURETSU:
		case KO_HAPPOKUNAI:
		case KO_HUUMARANKA:
		case OB_OBOROGENSOU_TRANSITION_ATK:
		case SU_SCRATCH:
		case SU_LUNATICCARROTBEAT:
		case SU_LUNATICCARROTBEAT2:
		case MH_XENO_SLASHER:
		case MH_HEILIGE_STANGE:
		case MH_MAGMA_FLOW:
		case EL_FIRE_BOMB_ATK:
		case EL_FIRE_WAVE_ATK:
		case EL_WATER_SCREW_ATK:
		case EL_HURRICANE_ATK:
			if (flag&1) { //Recursive invocation
				int sflag = skill_area_temp[0]&0xFFF;
				int heal = 0;

				if (flag&SD_LEVEL)
					sflag |= SD_LEVEL; //-1 will be used in packets instead of the skill level
				if (skill_area_temp[1] != bl->id && !(skill_get_inf2(skill_id)&INF2_NPC_SKILL))
					sflag |= SD_ANIMATION; //Original target gets no animation (as well as all NPC skills)
				switch (skill_id) {
					case SM_MAGNUM:
					case MS_MAGNUM:
						//For players, damage depends on distance, so add it to flag if it is > 1
						sflag |= (sd ? distance_bl(src,bl) : 0);
						break;
#ifdef RENEWAL
					case MG_FIREBALL:
						sflag |= distance_xy(bl->x,bl->y,skill_area_temp[4],skill_area_temp[5]);
						break;
#endif
					case AS_SPLASHER:
					case GN_SPORE_EXPLOSION:
						if (sflag&SD_ANIMATION)
							sflag |= 16;
						break;
					case WL_FROSTMISTY:
						if (tsc && tsc->data[SC_FREEZING]) {
							map_freeblock_unlock();
							return 0;
						}
						break;
					case LG_MOONSLASHER:
					case SR_WINDMILL:
						if (tsc && tsc->data[SC_HOVERING]) {
							map_freeblock_unlock();
							return 0;
						}
						break;
					case SR_TIGERCANNON:
						if (sflag&SD_ANIMATION)
							sflag |= 16;
					//Fall through
					case SR_SKYNETBLOW:
						if (flag&8)
							sflag |= 8;
						break;
					case SJ_FALLINGSTAR_ATK2:
						if (!(sflag&SD_ANIMATION)) {
							map_freeblock_unlock();
							return 0;
						}
						break;
					case SP_SHA:
					case SP_SWHOO:
						if (!battle_config.allow_es_magic_pc && bl->type != BL_MOB) {
							map_freeblock_unlock();
							return 0;
						}
						break;
				}
				heal = (int)skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,sflag);
				if (skill_id == NPC_VAMPIRE_GIFT && heal) {
					clif_skill_nodamage(NULL,src,AL_HEAL,heal,1);
					status_heal(src,heal,0,0);
				}
			} else {
				int starget = BL_CHAR|BL_SKILL;
				int target = BCT_ENEMY;

				skill_area_temp[0] = 0;
				skill_area_temp[1] = bl->id;
				skill_area_temp[2] = 0;
				switch (skill_id) {
					case GN_CARTCANNON:
					case GN_ILLUSIONDOPING:
					case MH_HEILIGE_STANGE:
						clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
						break;
					case SR_TIGERCANNON:
						if (!(flag&32) && sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == SR_FALLENEMPIRE)
							flag |= 8;
						break;
#ifdef RENEWAL
					case MG_FIREBALL:
						skill_area_temp[4] = bl->x;
						skill_area_temp[5] = bl->y;
						break;
#endif
					case WL_CRIMSONROCK:
						if (!battle_config.crimsonrock_knockback) {
							skill_area_temp[4] = bl->x;
							skill_area_temp[5] = bl->y;
						}
						break;
					case SC_FATALMENACE: {
							short x, y;

							map_search_freecell(src,0,&x,&y,-1,-1,0);
							//Destination area
							skill_area_temp[4] = x;
							skill_area_temp[5] = y;
							starget = splash_target(src);
						}
						break;
					case NPC_REVERBERATION_ATK:
					case WM_REVERBERATION_MELEE:
					case WM_REVERBERATION_MAGIC:
					case NC_ARMSCANNON:
						skill_area_temp[1] = 0;
						starget = splash_target(src);
						break;
					case SO_POISON_BUSTER:
						if (!(tsc && tsc->data[SC_POISON])) {
							if (sd)
								clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
							map_freeblock_unlock();
							return 0;
						}
						starget = splash_target(src);
						status_change_end(bl,SC_POISON,INVALID_TIMER);
						break;
					case GN_SPORE_EXPLOSION:
						if (map_flag_vs(src->m))
							target = BCT_ALL;
						break;
					case SP_SHA:
					case SP_SWHOO:
						if (sd && !battle_config.allow_es_magic_pc && bl->type != BL_MOB) {
							status_change_start(src,src,SC_STUN,10000,skill_lv,0,0,0,500,SCFLAG_FIXEDTICK|SCFLAG_FIXEDRATE);
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
							map_freeblock_unlock();
							return 0;
						} else if (skill_id == SP_SWHOO)
							status_change_end(src,SC_USE_SKILL_SP_SPA,INVALID_TIMER);
						break;
					case SU_LUNATICCARROTBEAT:
						if (sd && pc_search_inventory(sd,ITEMID_CARROT) != INDEX_NOT_FOUND)
							skill_id = SU_LUNATICCARROTBEAT2;
						break;
				}
				//If skill damage should be split among targets, count them
				//SD_LEVEL -> Forced splash damage for Auto Blitz-Beat -> count targets
				//Special case: Venom Splasher uses a different range for searching than for splashing
				if ((flag&SD_LEVEL) || (skill_get_nk(skill_id)&NK_SPLASHSPLIT))
					skill_area_temp[0] = map_foreachinallrange(skill_area_sub,bl,(skill_id == AS_SPLASHER || skill_id == GN_SPORE_EXPLOSION) ? 1 : skill_get_splash(skill_id,skill_lv),starget,src,skill_id,skill_lv,tick,target,skill_area_sub_count);
				//Recursive invocation of skill_castend_damage_id() with flag|1
				map_foreachinrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),starget,src,
					skill_id,skill_lv,tick,flag|target|SD_SPLASH|1,skill_castend_damage_id);
				if (skill_id == RA_ARROWSTORM)
					status_change_end(src,SC_CAMOUFLAGE,INVALID_TIMER);
				if (skill_id == AS_SPLASHER) {
					map_freeblock_unlock();
					return 1; //Already consume the requirement item, so end it
				}
			}
			break;

		case KN_BRANDISHSPEAR:
		case ML_BRANDISH:
			//Coded apart for it needs the flag passed to the damage calculation
			if (skill_area_temp[1] != bl->id)
				skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag|SD_ANIMATION);
			else
				skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case KN_BOWLINGBASH:
		case MS_BOWLINGBASH:
			{
				int min_x, max_x, min_y, max_y, i, c, dir, tx, ty;

				//Chain effect and check range gets reduction by recursive depth, as this can reach 0, we don't use blowcount
				c = (skill_lv - (flag&0xFFF) + 1) / 2;
				//Determine the Bowling Bash area depending on configuration
				if (battle_config.bowling_bash_area == 0) {
					//Gutter line system
					min_x = ((src->x) - c) - ((src->x) - c)%40;
					if (min_x < 0) min_x = 0;
					max_x = min_x + 39;
					min_y = ((src->y) - c) - ((src->y) - c)%40;
					if (min_y < 0) min_y = 0;
					max_y = min_y + 39;
				} else if (battle_config.bowling_bash_area == 1) {
					//Gutter line system without demi gutter bug
					min_x = src->x - (src->x)%40;
					max_x = min_x + 39;
					min_y = src->y - (src->y)%40;
					max_y = min_y + 39;
				} else {
					//Area around caster
					min_x = src->x - battle_config.bowling_bash_area;
					max_x = src->x + battle_config.bowling_bash_area;
					min_y = src->y - battle_config.bowling_bash_area;
					max_y = src->y + battle_config.bowling_bash_area;
				}
				//Initialization, break checks, direction
				if ((flag&0xFFF) > 0) {
					//Ignore monsters outside area
					if (bl->x < min_x || bl->x > max_x || bl->y < min_y || bl->y > max_y)
						break;
					//Ignore monsters already in list
					if (idb_exists(bowling_db,bl->id))
						break;
					//Random direction
					dir = rnd()%8;
				} else {
					//Create an empty list of already hit targets
					db_clear(bowling_db);
					//Direction is walkpath
					dir = (unit_getdir(src) + 4)%8;
				}
				//Add current target to the list of already hit targets
				idb_put(bowling_db,bl->id,bl);
				//Keep moving target in direction square by square
				tx = bl->x;
				ty = bl->y;
				for (i = 0; i < c; i++) {
					//Target coordinates (get changed even if knockback fails)
					tx -= dirx[dir];
					ty -= diry[dir];
					//If target cell is a wall then break
					if (map_getcell(bl->m,tx,ty,CELL_CHKWALL))
						break;
					skill_blown(src,bl,1,dir,0);
					//Splash around target cell, but only cells inside area; we first have to check the area is not negative
					if ((max(min_x,tx - 1) <= min(max_x,tx + 1)) &&
						(max(min_y,ty - 1) <= min(max_y,ty + 1)) &&
						(map_foreachinallarea(skill_area_sub,bl->m,max(min_x,tx - 1),max(min_y,ty - 1),min(max_x,tx + 1),
							min(max_y,ty + 1),splash_target(src),src,skill_id,skill_lv,tick,flag|BCT_ENEMY,skill_area_sub_count))) {
						//Recursive call
						map_foreachinallarea(skill_area_sub,bl->m,max(min_x,tx - 1),max(min_y,ty - 1),min(max_x,tx + 1),
							min(max_y,ty + 1),splash_target(src),src,skill_id,skill_lv,tick,(flag|BCT_ENEMY) + 1,skill_castend_damage_id);
						//Self-collision
						if (bl->x >= min_x && bl->x <= max_x && bl->y >= min_y && bl->y <= max_y)
							skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,(flag&0xFFF) > 0 ? SD_ANIMATION : 0);
						break;
					}
				}
				//Original hit or chain hit depending on flag
				skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,(flag&0xFFF) > 0 ? SD_ANIMATION : 0);
			}
			break;

		case KN_SPEARSTAB:
			if (flag&1) {
				if (skill_area_temp[1] == bl->id)
					break;
				if (skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,SD_ANIMATION))
					skill_blown(src,bl,skill_area_temp[2],-1,0);
			} else {
				int x = bl->x, y = bl->y, i, dir;

				dir = map_calc_dir(bl,src->x,src->y);
				skill_area_temp[1] = bl->id;
				skill_area_temp[2] = skill_get_blewcount(skill_id,skill_lv);
				//All the enemies between the caster and the target are hit, as well as the target
				if (skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,0))
					skill_blown(src,bl,skill_area_temp[2],-1,0);
				for (i = 0; i < 4; i++) {
					map_foreachincell(skill_area_sub,bl->m,x,y,BL_CHAR,src,skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_damage_id);
					x += dirx[dir];
					y += diry[dir];
				}
			}
			break;

		case TK_TURNKICK:
		case MO_BALKYOUNG: //Active part of the attack (Skill-attack) [Skotlex]
			skill_area_temp[1] = bl->id; //NOTE: This is used in skill_castend_nodamage_id to avoid affecting the target
			if (skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag))
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
			break;
		case CH_PALMSTRIKE: //Palm Strike takes effect 1 sec after casting [Skotlex]
			//clif_skill_nodamage(src,bl,skill_id,skill_lv,0); //Can't make this one display the correct attack animation delay
			clif_damage(src,bl,tick,status_get_amotion(src),0,-1,1,DMG_ENDURE,0,false); //Display an absorbed damage attack
			skill_addtimerskill(src,tick + (1000 + status_get_amotion(src)),bl->id,0,0,skill_id,skill_lv,BF_WEAPON,flag);
			break;

		case PR_TURNUNDEAD:
			if (!battle_check_undead(tstatus->race,tstatus->def_ele)) {
				map_freeblock_unlock();
				return 1;
			}
		//Fall through
		case MG_SOULSTRIKE:
		case NPC_DARKSTRIKE:
		case MG_COLDBOLT:
		case MG_FIREBOLT:
		case MG_LIGHTNINGBOLT:
		case ALL_RESURRECTION:
		case WZ_EARTHSPIKE:
		case AL_HEAL:
		case AL_HOLYLIGHT:
		case NPC_DARKTHUNDER:
		case NPC_FIRESTORM:
		case PR_ASPERSIO:
		case MG_FROSTDIVER:
		case WZ_SIGHTBLASTER:
		case WZ_SIGHTRASHER:
		case NJ_KOUENKA:
		case NJ_HYOUSENSOU:
		case NJ_HUUJIN:
		case AB_HIGHNESSHEAL:
		case AB_DUPLELIGHT_MAGIC:
		case WM_METALICSOUND:
		case KO_KAIHOU:
		case MH_ERASER_CUTTER:
		case EL_FIRE_ARROW:
		case EL_ICE_NEEDLE:
			skill_attack(BF_MAGIC,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case NPC_MAGICALATTACK:
			skill_attack(BF_MAGIC,src,src,bl,skill_id,skill_lv,tick,flag);
			sc_start(src,src,status_skill2sc(skill_id),100,skill_lv,skill_get_time(skill_id,skill_lv));
			break;

		case HVAN_CAPRICE: { //[blackhole89]
				uint16 sid = 0;

				if (src->type == BL_PC) { //For CD_In_Mouth (18666)
					switch (rnd()%5) {
						case 0: sid = MG_COLDBOLT; break;
						case 1: sid = MG_FIREBOLT; break;
						case 2: sid = MG_LIGHTNINGBOLT; break;
						case 3: sid = WZ_EARTHSPIKE; break;
						case 4: sid = MG_SOULSTRIKE; break;
					}
				} else {
					switch (rnd()%4) {
						case 0: sid = MG_COLDBOLT; break;
						case 1: sid = MG_FIREBOLT; break;
						case 2: sid = MG_LIGHTNINGBOLT; break;
						case 3: sid = WZ_EARTHSPIKE; break;
					}
					flag |= SD_LEVEL;
				}
				skill_attack(BF_MAGIC,src,src,bl,sid,skill_lv,tick,flag);
			}
			break;

		case WZ_WATERBALL: //Deploy waterball cells, these are used and turned into waterballs via the timerskill
			skill_unitsetting(src,skill_id,skill_lv,src->x,src->y,0);
			skill_addtimerskill(src,tick,bl->id,src->x,src->y,skill_id,skill_lv,0,flag);
			break;

		case WZ_JUPITEL: //Jupitel Thunder is delayed by 150ms, you can cast another spell before the knockback
			skill_addtimerskill(src,tick + TIMERSKILL_INTERVAL,bl->id,0,0,skill_id,skill_lv,1,flag);
			break;

		case PR_BENEDICTIO: //Should attack undead and demons [Skotlex]
			if (battle_check_undead(tstatus->race,tstatus->def_ele) || tstatus->race == RC_DEMON)
				skill_attack(BF_MAGIC,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case SL_SMA:
			status_change_end(src,SC_SMA,INVALID_TIMER);
			status_change_end(src,SC_USE_SKILL_SP_SHA,INVALID_TIMER);
		//Fall through
		case SL_STIN:
		case SL_STUN:
		case SP_SPA:
			if (sd && !battle_config.allow_es_magic_pc && bl->type != BL_MOB) {
				status_change_start(src,src,SC_STUN,10000,skill_lv,0,0,0,500,SCFLAG_FIXEDTICK|SCFLAG_FIXEDRATE);
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			} else
				skill_attack(BF_MAGIC,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case NPC_DARKBREATH:
			clif_emotion(src,E_AG);
		//Fall through
		case NPC_SMOKING:
		case TF_THROWSTONE:
#ifdef RENEWAL
		case AM_ACIDTERROR:
#endif
		case PA_PRESSURE:
		case ASC_BREAKER:
		case SN_FALCONASSAULT:
		case CR_ACIDDEMONSTRATION:
		case GS_FLING:
		case NJ_ZENYNAGE:
		case LG_RAYOFGENESIS:
			skill_attack(BF_MISC,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case KO_MUCHANAGE: {
				int rate = max((1000 - (10000 / (sstatus->dex + sstatus->luk) * 5)) * (skill_lv / 2 + 5) / 100,0);

				if (rnd()%100 < rate) //Success chance of hitting is applied to each enemy separately
					skill_attack(BF_MISC,src,src,bl,skill_id,skill_lv,tick,skill_area_temp[0]&0xFFF);
			}
			break;

		case NPC_SELFDESTRUCTION:
		case HVAN_EXPLOSION:
			if (bl->id != src->id)
				skill_attack(BF_MISC,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case PF_SOULBURN: //[Celest]
			if (rnd()%100 < (skill_lv < 5 ? 30 + skill_lv * 10 : 70)) {
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				if (skill_lv == 5)
					skill_attack(BF_MAGIC,src,src,bl,skill_id,skill_lv,tick,flag);
				status_percent_damage(src,bl,0,100,false);
			} else {
				clif_skill_nodamage(src,src,skill_id,skill_lv,1);
				if (skill_lv == 5)
					skill_attack(BF_MAGIC,src,src,src,skill_id,skill_lv,tick,flag);
				status_percent_damage(src,src,0,100,false);
			}
			break;

		case NPC_BLOODDRAIN:
		case NPC_ENERGYDRAIN:
			{
				int heal = skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag);

				if (heal > 0) {
					clif_skill_nodamage(NULL,src,AL_HEAL,heal,1);
					status_heal(src,heal,0,0);
				}
			}
			break;

		case NJ_KASUMIKIRI:
			if (skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag) > 0)
				sc_start(src,src,SC_HIDING,100,skill_lv,skill_get_time(skill_id,skill_lv));
			break;

		case NJ_KIRIKAGE: {
				short x, y;

				map_search_freecell(bl,0,&x,&y,1,1,0);
				if (unit_movepos(src,x,y,0,false)) {
					clif_blown(src,src);
					status_change_end(src,SC_HIDING,INVALID_TIMER);
					skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
				}
			}
			break;

		case NJ_BAKUENRYU:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			skill_castend_pos2(src,bl->x,bl->y,skill_id,skill_lv,tick,0); //Place units around target
			return 0;

		case GC_DARKILLUSION:
		case SR_KNUCKLEARROW:
		case KO_JYUMONJIKIRI:
		case MH_STAHL_HORN:
		case MH_TINDER_BREAKER:
			{
				uint8 dir = map_calc_dir(bl,src->x,src->y);

				if (unit_movepos(src,bl->x,bl->y,1,true)) {
					skill_blown(src,src,1,(dir + 4)%8,0);
					if (skill_id == SR_KNUCKLEARROW)
						flag |= 2;
					skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
					if (skill_id == GC_DARKILLUSION && rnd()%100 < 4 * skill_lv)
						skill_castend_damage_id(src,bl,GC_CROSSIMPACT,skill_lv,tick,flag);
				}
			}
			break;

		case GC_CROSSRIPPERSLASHER:
			skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			status_change_end(src,SC_ROLLINGCUTTER,INVALID_TIMER);
			break;

		case GC_PHANTOMMENACE: //Only hits invisible targets
			if ((flag&1) && tsc) {
				if ((tsc->option&(OPTION_HIDE|OPTION_CLOAK)) || tsc->data[SC_CAMOUFLAGE]) {
					status_change_end(bl,SC_HIDING,INVALID_TIMER);
					status_change_end(bl,SC_CLOAKING,INVALID_TIMER);
					status_change_end(bl,SC_CAMOUFLAGE,INVALID_TIMER);
					status_change_end(bl,SC_CLOAKINGEXCEED,INVALID_TIMER);
					status_change_end(bl,SC_NEWMOON,INVALID_TIMER);
					skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
				}
				if (tsc->data[SC__SHADOWFORM] && rnd()%100 < 100 - tsc->data[SC__SHADOWFORM]->val1 * 10)
					status_change_end(bl,SC__SHADOWFORM,INVALID_TIMER);
			}
			break;

		case GC_DARKCROW:
			skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			sc_start(src,bl,status_skill2sc(skill_id),100,skill_lv,skill_get_time(skill_id,skill_lv)); //Should be applied even on miss
			break;

		case WL_DRAINLIFE: {
				int heal = skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag);
				int rate = 70 + 4 * skill_lv;

				if (bl->type == BL_SKILL || //Don't absorb heal from Ice Walls or other skill units
					status_get_hp(src) == status_get_max_hp(src)) //Don't absorb when caster was in full HP
					heal = 0;
				else {
					heal = heal * 8 * skill_lv / 100;
					heal = heal * status_get_lv(src) / 100; //Base Level bonus [exneval]
				}
				rate -= skill_lv * status_get_job_lv(src) / 50; //Job Level bonus [exneval]
				if (heal && rnd()%100 < rate)
					status_heal(src,heal,0,2);
			}
			break;

		case WL_TETRAVORTEX: {
				int i, j = 0;
				int types[][2] = { {0,0},{0,0},{0,0},{0,0} };
				uint16 skid;

				if (!sd) {
					for (i = 1; i <= 4; i++) {
						switch (i) {
							case 1: skid = WL_TETRAVORTEX_FIRE; break;
							case 2: skid = WL_TETRAVORTEX_WATER; break;
							case 3: skid = WL_TETRAVORTEX_WIND; break;
							case 4: skid = WL_TETRAVORTEX_GROUND; break;
						}
						if (j < 4) {
							int sc_index = 0, rate = 0;

							types[j][0] = i;
							types[j][1] = 25; //25% each for equal sharing
							if (j == 3) {
								sc_index = types[rnd()%4][0];
								for (j = 0; j < 4; j++) {
									if (types[j][0] == sc_index)
										rate += types[j][1];
								}
							}
							skill_addtimerskill(src,tick + i * 206,bl->id,sc_index,rate,skid,skill_lv,j,flag);
						}
						j++;
					}
				} else if (sc) {
					int k;

					for (i = SC_SPHERE_4; i <= SC_SPHERE_5; i++) {
						if (sc->data[i])
							k = i;
					}
					for (i = k; i >= SC_SPHERE_1; i--) {
						if (sc->data[i]) {
							skid = WL_TETRAVORTEX_FIRE + (sc->data[i]->val1 - WLS_FIRE) + (sc->data[i]->val1 == WLS_WIND) - (sc->data[i]->val1 == WLS_WATER);
							if (j < 4) {
								int sc_index = 0, rate = 0;

								types[j][0] = (sc->data[i]->val1 - WLS_FIRE) + 1;
								types[j][1] = 25;
								if (j == 3) {
									sc_index = types[rnd()%4][0];
									for (j = 0; j < 4; j++) {
										if (types[j][0] == sc_index)
											rate += types[j][1];
									}
								}
								skill_addtimerskill(src,tick + (k - i + 1) * 206,bl->id,sc_index,rate,skid,skill_lv,j,flag);
							}
							status_change_end(src,(sc_type)i,INVALID_TIMER);
							j++;
						}
					}
				}
			}
			break;

		case WL_RELEASE:
			if (sc) {
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				//Priority is to release Spell Book
				if (sc->data[SC_FREEZE_SP]) //Check sealed spells
					skill_addtimerskill(src,tick + status_get_adelay(src),bl->id,0,0,skill_id,skill_lv,BF_MAGIC,flag);
				else { //Summon Balls
					int i, j;

					for (i = SC_SPHERE_1; i <= SC_SPHERE_5; i++) {
						if (sc->data[i])
							j = i;
					}
					for (i = j; i >= SC_SPHERE_1; i--) {
						if (sc->data[i]) {
							uint16 skid = WL_SUMMON_ATK_FIRE + (sc->data[i]->val1 - WLS_FIRE);

							skill_addtimerskill(src,tick + (j - i + 1) * status_get_adelay(src),bl->id,0,0,skid,sc->data[i]->val2,BF_MAGIC,flag|SD_LEVEL|SD_ANIMATION);
							status_change_end(src,(sc_type)i,INVALID_TIMER);
							if (skill_lv == 1)
								break;
						}
					}
				}
			}
			break;

		case WL_HELLINFERNO:
			skill_attack(BF_MAGIC,src,src,bl,skill_id,skill_lv,tick,flag);
			skill_addtimerskill(src,tick + 200,bl->id,0,0,skill_id,skill_lv,BF_MAGIC,flag|ELE_DARK);
			break;

		case RA_WUGSTRIKE:
			if (sd && pc_isridingwug(sd)) {
				short x[8] = { 0,-1,-1,-1,0,1,1,1 };
				short y[8] = { 1,1,0,-1,-1,-1,0,1 };
				uint8 dir = map_calc_dir(bl,src->x,src->y);

				if (unit_movepos(src,bl->x + x[dir],bl->y + y[dir],1,true)) {
					clif_blown(src,bl);
					skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
				}
				break;
			}
		//Fall through
		case RA_WUGBITE:
			if (path_search(NULL,src->m,src->x,src->y,bl->x,bl->y,1,CELL_CHKNOREACH))
				skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			else if (sd && skill_id == RA_WUGBITE) //Only RA_WUGBITE has the skill fail message
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			break;

		case RA_SENSITIVEKEEN:
			if (bl->type != BL_SKILL) { //Only hits invisible targets
				if (tsc) {
					if ((tsc->option&(OPTION_HIDE|OPTION_CLOAK)) || tsc->data[SC_CAMOUFLAGE]) {
						status_change_end(bl,SC_HIDING,INVALID_TIMER);
						status_change_end(bl,SC_CLOAKING,INVALID_TIMER);
						status_change_end(bl,SC_CAMOUFLAGE,INVALID_TIMER);
						status_change_end(bl,SC_CLOAKINGEXCEED,INVALID_TIMER);
						status_change_end(bl,SC_NEWMOON,INVALID_TIMER);
						skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
					}
					if (tsc->data[SC__SHADOWFORM] && rnd()%100 < 100 - tsc->data[SC__SHADOWFORM]->val1 * 10)
						status_change_end(bl,SC__SHADOWFORM,INVALID_TIMER);
				}
			} else {
				struct skill_unit *unit = BL_CAST(BL_SKILL,bl);
				struct skill_unit_group *group = NULL;

				if (unit && (group = unit->group) && (skill_get_inf2(group->skill_id)&INF2_TRAP) &&
					(group->item_id == ITEMID_TRAP || group->item_id == ITEMID_SPECIAL_ALLOY_TRAP)) {
					if (!(group->unit_id == UNT_USED_TRAPS || (group->unit_id == UNT_ANKLESNARE && group->val2))) {
						struct item item_tmp;

						memset(&item_tmp,0,sizeof(item_tmp));
						item_tmp.nameid = group->item_id;
						item_tmp.identify = 1;
						if (item_tmp.nameid)
							map_addflooritem(&item_tmp,1,bl->m,bl->x,bl->y,0,0,0,4,0,false);
					}
					skill_delunit(unit);
				}
			}
			break;

		case LG_PINPOINTATTACK: {
				uint8 dir = map_calc_dir(bl,src->x,src->y);

				if (!map_flag_gvg2(src->m) && !mapdata[src->m].flag.battleground && unit_movepos(src,bl->x,bl->y,1,true))
					skill_blown(src,src,1,(dir + 4)%8,0);
				skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			}
			break;

		case LG_SHIELDSPELL:
			if (skill_lv == 1)
				skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			else if (skill_lv == 2)
				skill_attack(BF_MAGIC,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case SR_EARTHSHAKER:
			if (flag&1) {
				if (tsc && ((tsc->option&(OPTION_HIDE|OPTION_CLOAK|OPTION_CHASEWALK)) || tsc->data[SC_CAMOUFLAGE] || tsc->data[SC__SHADOWFORM])) {
					status_change_end(bl,SC_HIDING,INVALID_TIMER);
					status_change_end(bl,SC_CLOAKING,INVALID_TIMER);
					status_change_end(bl,SC_CHASEWALK,INVALID_TIMER);
					status_change_end(bl,SC_CAMOUFLAGE,INVALID_TIMER);
					status_change_end(bl,SC_CLOAKINGEXCEED,INVALID_TIMER);
					status_change_end(bl,SC_NEWMOON,INVALID_TIMER);
					if (tsc->data[SC__SHADOWFORM] && rnd()%100 < 100 - tsc->data[SC__SHADOWFORM]->val1 * 10)
						status_change_end(bl,SC__SHADOWFORM,INVALID_TIMER);
				}
				skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			}
			break;

		case WM_SOUND_OF_DESTRUCTION:
			if (tsc && (tsc->data[SC_SWINGDANCE] || tsc->data[SC_SYMPHONYOFLOVER] || tsc->data[SC_MOONLITSERENADE] ||
				tsc->data[SC_RUSHWINDMILL] || tsc->data[SC_ECHOSONG] || tsc->data[SC_HARMONIZE] ||
				tsc->data[SC_NETHERWORLD] || tsc->data[SC_VOICEOFSIREN] || tsc->data[SC_DEEPSLEEP] ||
				tsc->data[SC_SIRCLEOFNATURE] || tsc->data[SC_GLOOMYDAY] || tsc->data[SC_SONGOFMANA] ||
				tsc->data[SC_DANCEWITHWUG] || tsc->data[SC_SATURDAYNIGHTFEVER] || tsc->data[SC_LERADSDEW] ||
				tsc->data[SC_MELODYOFSINK] || tsc->data[SC_BEYONDOFWARCRY] || tsc->data[SC_UNLIMITEDHUMMINGVOICE] ||
				tsc->data[SC_FRIGG_SONG]) &&
				rnd()%100 < 4 * skill_lv + 2 * (sd ? pc_checkskill(sd,WM_LESSON) : 10) + 10 * skill_chorus_count(sd,0))
			{
				skill_attack(BF_MISC,src,src,bl,skill_id,skill_lv,tick,flag);
				status_change_start(src,bl,SC_STUN,10000,skill_lv,0,0,0,skill_get_time(skill_id,skill_lv),SCFLAG_FIXEDRATE);
				status_change_end(bl,SC_SWINGDANCE,INVALID_TIMER);
				status_change_end(bl,SC_SYMPHONYOFLOVER,INVALID_TIMER);
				status_change_end(bl,SC_MOONLITSERENADE,INVALID_TIMER);
				status_change_end(bl,SC_RUSHWINDMILL,INVALID_TIMER);
				status_change_end(bl,SC_ECHOSONG,INVALID_TIMER);
				status_change_end(bl,SC_HARMONIZE,INVALID_TIMER);
				status_change_end(bl,SC_NETHERWORLD,INVALID_TIMER);
				status_change_end(bl,SC_VOICEOFSIREN,INVALID_TIMER);
				status_change_end(bl,SC_DEEPSLEEP,INVALID_TIMER);
				status_change_end(bl,SC_SIRCLEOFNATURE,INVALID_TIMER);
				status_change_end(bl,SC_GLOOMYDAY,INVALID_TIMER);
				status_change_end(bl,SC_SONGOFMANA,INVALID_TIMER);
				status_change_end(bl,SC_DANCEWITHWUG,INVALID_TIMER);
				status_change_end(bl,SC_LERADSDEW,INVALID_TIMER);
				status_change_end(bl,SC_MELODYOFSINK,INVALID_TIMER);
				status_change_end(bl,SC_BEYONDOFWARCRY,INVALID_TIMER);
				status_change_end(bl,SC_UNLIMITEDHUMMINGVOICE,INVALID_TIMER);
				status_change_end(bl,SC_FRIGG_SONG,INVALID_TIMER);
				if (tsc->data[SC_SATURDAYNIGHTFEVER]) {
					tsc->data[SC_SATURDAYNIGHTFEVER]->val2 = 0;
					status_change_end(bl,SC_SATURDAYNIGHTFEVER,INVALID_TIMER);
				}
			}
			break;

		case RL_MASS_SPIRAL:
		case RL_BANISHING_BUSTER:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag|SD_ANIMATION);
			break;

		case RL_H_MINE:
			if (flag&1) //Splash damage from explosion
				skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag|SD_ANIMATION|16);
			else if (flag&4) { //Triggered by RL_FLICKER
				flag = 0; //Reset flag
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR|BL_SKILL,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_SPLASH|1,skill_castend_damage_id);
				if (tsc && tsc->data[SC_H_MINE] && tsc->data[SC_H_MINE]->val2 == src->id) {
					tsc->data[SC_H_MINE]->val4 = 1; //Mark the SC end because not expired
					status_change_end(bl,SC_H_MINE,INVALID_TIMER);
				}
			} else {
				//Check if the target is already tagged by another source
				if (tsc && tsc->data[SC_H_MINE] && tsc->data[SC_H_MINE]->val2 != src->id) {
					if (sd)
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				//Tagging the target
				if (sd) {
					short count = MAX_HOWL_MINES;
					uint8 i = 0;

					ARR_FIND(0,count,i,sd->howl_mine[i] == bl->id);
					if (i == count) {
						ARR_FIND(0,count,i,sd->howl_mine[i] == 0);
						if (i == count) {
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
							break;
						}
					}
					if (skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag)) {
						sd->howl_mine[i] = bl->id;
						sc_start4(src,bl,SC_H_MINE,100,skill_lv,src->id,i,0,skill_get_time(skill_id,skill_lv));
					}
				}
			}
			break;

		case RL_QD_SHOT:
		case RL_D_TAIL:
			if (flag&1) {
				if (skill_id == RL_QD_SHOT && skill_area_temp[1] == bl->id)
					break;
				if (tsc && tsc->data[SC_C_MARKER] && tsc->data[SC_C_MARKER]->val2 == src->id)
					skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag|SD_ANIMATION);
			}
			break;

		case SJ_FLASHKICK:
			//Check if the target is already tagged by another source
			if (tsc && tsc->data[SC_FLASHKICK] && tsc->data[SC_FLASHKICK]->val2 != src->id) {
				if (sd)
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			//Tagging the target
			if (sd) {
				short count = MAX_STELLAR_MARKS;
				uint8 i = 0;

				ARR_FIND(0,count,i,sd->stellar_mark[i] == bl->id);
				if (i == count) {
					ARR_FIND(0, count, i, sd->stellar_mark[i] == 0);
					if (i == count) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						break;
					}
				}
				if (skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag)) {
					sd->stellar_mark[i] = bl->id;
					sc_start4(src,bl,SC_FLASHKICK,100,skill_lv,src->id,i,0,skill_get_time(skill_id,skill_lv));
				}
			}
			break;

		case SJ_NOVAEXPLOSING:
			if (!(sc && sc->data[SC_DIMENSION]))
				sc_start(src,src,SC_NOVAEXPLOSING,100,skill_lv,skill_get_time(skill_id,skill_lv));
			skill_attack(BF_MISC,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case SJ_PROMINENCEKICK:
			if (flag&1)
				skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag|SD_ANIMATION|16);
			else {
				skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag);
				skill_area_temp[1] = 0;
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_SPLASH|1,skill_castend_damage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case SJ_FALLINGSTAR_ATK:
			if (flag&1) {
				if (!tsc || !tsc->data[SC_FLASHKICK] || tsc->data[SC_FLASHKICK]->val2 != src->id)
					break;
				skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag);
				skill_castend_damage_id(src,bl,SJ_FALLINGSTAR_ATK2,skill_lv,tick,0);
			} else {
				skill_area_temp[1] = 0;
				map_foreachinrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),splash_target(src),src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_SPLASH|1,skill_castend_damage_id);
			}
			break;

		case SP_SOULEXPLOSION:
			if (!(tsc && (tsc->data[SC_SPIRIT] || tsc->data[SC_SOULGOLEM] || tsc->data[SC_SOULSHADOW] ||
				tsc->data[SC_SOULFALCON] || tsc->data[SC_SOULFAIRY])) || tstatus->hp < 10 * tstatus->max_hp / 100) {
				if (sd)
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			skill_attack(BF_MISC,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case EL_TYPOON_MIS:
		case EL_ROCK_CRUSHER:
			if (rnd()%100 < 50)
				skill_attack(BF_MAGIC,src,src,bl,skill_id + 1,skill_lv,tick,flag);
			else
				skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case EL_STONE_RAIN:
			if (flag&1)
				skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag|SD_ANIMATION);
			else {
				clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
				if (rnd()%100 < 30)
					map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_SPLASH|1,skill_castend_damage_id);
				else
					skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag);
			}
			break;

		case EL_STONE_HAMMER:
			clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case EL_TIDAL_WEAPON: {
				struct elemental_data *ed = BL_CAST(BL_ELEM,src);

				if (ed) {
					if (rnd()%100 < 50) {
						clif_skill_damage(src,src,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
						skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag);
					} else {
						struct status_change *e_sc = status_get_sc(&ed->bl);
						enum sc_type type = status_skill2sc(skill_id), type2;

						type2 = (sc_type)(type - 1);
						if ((e_sc && e_sc->data[type2]) || (tsc && tsc->data[type]))
							elemental_clean_single_effect(ed,skill_id);
						else {
							sc_start(src,src,type2,100,skill_lv,skill_get_time(skill_id,skill_lv));
							sc_start(src,&ed->master->bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
							clif_skill_damage(src,&ed->master->bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
						}
					}
				}
			}
			break;

		case MH_EQC: {
				TBL_HOM *hd = BL_CAST(BL_HOM,src);

				if (status_bl_has_mode(bl,MD_STATUS_IMMUNE)) {
					if (hd && hd->master)
						clif_skill_fail(hd->master,skill_id,USESKILL_FAIL_TOTARGET,0,0);
					break;
				}
				skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag);
			}
			break;

		case SU_SV_STEMSPEAR:
			if (sd && pc_checkskill(sd,SU_SPIRITOFLAND) > 0)
				sc_start(src,src,SC_DORAM_WALKSPEED,100,skill_lv,skill_get_time(SU_SPIRITOFLAND,1));
			skill_attack(BF_MAGIC,src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case SU_PICKYPECK: {
				uint16 skid = (status_get_hp(bl) <= 50 * status_get_max_hp(bl) / 100 ? SU_PICKYPECK_DOUBLE_ATK : skill_id);

				skill_attack(BF_WEAPON,src,src,bl,skid,skill_lv,tick,flag);
			}
			break;

		case SU_SVG_SPIRIT:
			skill_area_temp[1] = bl->id;
			map_foreachinpath(skill_attack_area,src->m,src->x,src->y,bl->x,bl->y,
				skill_get_splash(skill_id,skill_lv),skill_get_maxcount(skill_id,skill_lv),splash_target(src),
				skill_get_type(skill_id),src,src,skill_id,skill_lv,tick,flag,BCT_ENEMY);
			break;

		case 0: //No skill - basic/normal attack
			if (sd) {
				if (flag&3) {
					if (skill_area_temp[1] != bl->id) //For splash basic attack
						skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag|SD_LEVEL|16);
				} else {
					skill_area_temp[1] = bl->id;
					map_foreachinallrange(skill_area_sub,bl,sd->bonus.splash_range,BL_CHAR,src,
						skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_damage_id);
					flag |= 1; //Set flag to 1 so ammo is not double-consumed [Skotlex]
				}
			}
			break;

		default:
			ShowWarning("skill_castend_damage_id: Unknown skill used:%d\n",skill_id);
			clif_skill_damage(src,bl,tick,status_get_amotion(src),status_get_dmotion(bl),0,
				abs(skill_get_num(skill_id,skill_lv)),skill_id,skill_lv,skill_get_hit(skill_id));
			map_freeblock_unlock();
			return 1;
	}

	if (sc && sc->data[SC_CURSEDCIRCLE_ATKER]) //Should only remove after the skill has been casted
		status_change_end(src,SC_CURSEDCIRCLE_ATKER,INVALID_TIMER);

	if (sd && !(flag&1)) { //Ensure that the skill last-cast tick is recorded
		sd->canskill_tick = gettick();

		if (sd->state.arrow_atk) //Consume ammo on last invocation to this skill
			battle_consume_ammo(sd,skill_id,skill_lv);

		//Perform skill requirement consumption
		skill_consume_requirement(sd,skill_id,skill_lv,2);
	}

	map_freeblock_unlock();
	return 0;
}

/**
 * Use no-damage skill from 'src' to 'bl
 * @param src Caster
 * @param bl Target of the skill, bl maybe same with src for self skill
 * @param skill_id
 * @param skill_lv
 * @param tick
 * @param flag Various value, &1: Recursive effect
 */
int skill_castend_nodamage_id(struct block_list *src, struct block_list *bl, uint16 skill_id, uint16 skill_lv, unsigned int tick, int flag)
{
	struct map_session_data *sd, *dstsd;
	struct mob_data *md, *dstmd;
	struct homun_data *hd;
	struct mercenary_data *mer;
	struct status_data *sstatus, *tstatus;
	struct status_change *tsc;
	struct status_change_entry *tsce;
	enum sc_type type;
	int i = 0, partybonus = 0;

	if(skill_id && !skill_lv)
		return 0; //Celest

	nullpo_retr(1,src);
	nullpo_retr(1,bl);

	if(src->m != bl->m)
		return 1;

	sd = BL_CAST(BL_PC,src);
	hd = BL_CAST(BL_HOM,src);
	md = BL_CAST(BL_MOB,src);
	mer = BL_CAST(BL_MER,src);

	dstsd = BL_CAST(BL_PC,bl);
	dstmd = BL_CAST(BL_MOB,bl);

	if(!bl->prev || status_isdead(src))
		return 1;

	if(bl->id != src->id && status_isdead(bl)) { //Skills that may be cast on dead targets
		switch(skill_id) {
			case NPC_WIDESOULDRAIN:
			case PR_REDEMPTIO:
			case ALL_RESURRECTION:
			case WM_DEADHILLHERE:
			case WE_ONEFOREVER:
				break;
			default:
				return 1;
		}
	}

	sstatus = status_get_status_data(src);
	tstatus = status_get_status_data(bl);

	if(sd)
		partybonus = (sd->status.party_id ? party_foreachsamemap(party_sub_count,sd,0) : 1);

	switch(skill_id) {
		case HLIF_HEAL:	{ //[orn]
				struct block_list *m_bl = NULL;

				if((m_bl = battle_get_master(src))) {
					bl = m_bl;
					if(bl->type != BL_HOM) {
						if(sd)
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						break;
					}
				}
			}
		//Fall through
		case AL_HEAL:
		case AB_HIGHNESSHEAL:
		case AL_INCAGI:
		case ALL_RESURRECTION:
		case PR_ASPERSIO:
			//Check for undead skills that convert a no-damage skill into a damage one [Skotlex]
			//Apparently only player casted skills can be offensive like this
			if(sd && battle_check_undead(status_get_race(bl),status_get_element(bl)) && skill_id != AL_INCAGI) {
				if(battle_check_target(src,bl,BCT_ENEMY) <= 0) //Offensive heal does not works on non-enemies [Skotlex]
					return 0;
				return skill_castend_damage_id(src,bl,skill_id,skill_lv,tick,flag);
			}
			break;
		case RK_MILLENNIUMSHIELD:
		case RK_CRUSHSTRIKE:
		case RK_REFRESH:
		case RK_GIANTGROWTH:
		case RK_STONEHARDSKIN:
		case RK_VITALITYACTIVATION:
		case RK_STORMBLAST:
		case RK_FIGHTINGSPIRIT:
		case RK_ABUNDANCE:
			if(sd) {
				if(rnd()%100 >= 85 + pc_checkskill(sd,RK_RUNEMASTERY) + (sstatus->dex + sstatus->luk) / 20) { //Activation failed
					uint8 rate = rnd()%100;

					if(rate < 1 && sd->status.party_id)
						party_foreachsamemap(skill_area_sub,sd,skill_get_splash(ALL_RESURRECTION,1),src,ALL_RESURRECTION,1,tick,flag|BCT_PARTY,skill_castend_nodamage_id);
					else if(rate < 2)
						status_percent_heal(src,100,100);
					else if(rate < 4 && !mapdata[src->m].flag.noteleport)
						pc_randomwarp(sd,CLR_TELEPORT);
					else if(rate < 7)
						status_heal(src,10000,0,2);
					else if(rate < 10)
						status_fix_damage(NULL,src,99999,0);
					else if(rate < 20) {
						const enum sc_type scs[] = { SC_FREEZE,SC_STUN,SC_SLEEP,SC_POISON,SC_SILENCE,SC_BLIND };
						const int time[] = { 30,5,18,18,18,18 };

						for(i = 0; i < ARRAYLENGTH(scs); i++)
							status_change_start(src,src,scs[i],10000,0,0,0,0,1000 * time[i],SCFLAG_FIXEDRATE);
					}
					return 0;
				}
			}
			break;
		case MH_STEINWAND: {
				struct block_list *s_src = battle_get_master(src);

				if(!skill_check_unit_range(src,src->x,src->y,skill_id,skill_lv)) //Prevent reiteration
					skill_castend_pos2(src,src->x,src->y,skill_id,skill_lv,tick,flag); //Cast on homon
				if(s_src && !skill_check_unit_range(s_src,s_src->x,s_src->y,skill_id,skill_lv))
					skill_castend_pos2(s_src,s_src->x,s_src->y,skill_id,skill_lv,tick,flag); //Cast on master
				return 0;
			}
			break;
		case NPC_SMOKING: //Since it is a self skill, this one ends here rather than in damage_id [Skotlex]
			return skill_castend_damage_id(src,bl,skill_id,skill_lv,tick,flag);
		default:
			if(bl->id == src->id && skill_get_unit_id(skill_id,0)) //Skill is actually ground placed
				return skill_castend_pos2(src,bl->x,bl->y,skill_id,skill_lv,tick,0);
			break;
	}

	type = status_skill2sc(skill_id);
	tsc = status_get_sc(bl);
	tsce = (tsc && type != SC_NONE) ? tsc->data[type] : NULL;

#if 0
	if(bl->id != src->id && type > SC_NONE && (i = skill_get_ele(skill_id,skill_lv)) > ELE_NEUTRAL &&
		skill_get_inf(skill_id) != INF_SUPPORT_SKILL && battle_attr_fix(NULL,NULL,100,i,tstatus->def_ele,tstatus->ele_lv) <= 0)
		return 1; //Skills that cause an status should be blocked if the target element blocks its element
#endif

	map_freeblock_lock();

	switch(skill_id) {
		case HLIF_HEAL:	//[orn]
		case AL_HEAL:
		case AB_HIGHNESSHEAL:
		case SU_TUNABELLY:
			{
				int heal, heal_get_jobexp;
				uint8 flag2 = 0;

				if(skill_id == HLIF_HEAL) {
					struct block_list *m_bl = NULL;

					if((m_bl = battle_get_master(src)))
						bl = m_bl;
				}

				heal = skill_calc_heal(src,bl,skill_id,skill_lv,true);

				if(status_isimmune(bl) || (dstmd && (dstmd->mob_id == MOBID_EMPERIUM || status_get_class_(bl) == CLASS_BATTLEFIELD)))
					heal = 0;

				if(tsc && tsc->count) {
					if(tsc->data[SC_KAITE] && !status_has_mode(sstatus,MD_STATUS_IMMUNE)) { //Bounce back heal
						if(--tsc->data[SC_KAITE]->val2 <= 0)
							status_change_end(bl,SC_KAITE,INVALID_TIMER);
						heal = 0; //When you try to heal under Kaite, the heal is voided
					} else if( tsc->data[SC_BERSERK] || tsc->data[SC_SATURDAYNIGHTFEVER] )
						heal = 0; //Needed so that it actually displays 0 when healing
				}

				if(skill_id == AL_HEAL || skill_id == AB_HIGHNESSHEAL) {
					if(tsc && tsc->data[SC_AKAITSUKI] && heal) {
						skill_akaitsuki_damage(src,bl,heal,skill_id,skill_lv,tick);
						break;
					}
					status_change_end(bl,SC_BITESCAR,INVALID_TIMER);
				} else if(skill_id == SU_TUNABELLY)
					flag2 |= 3;

				heal_get_jobexp = status_heal(bl,heal,0,flag2);
				clif_skill_nodamage(src,bl,skill_id,(flag2 ? skill_lv : heal),1);

				if(sd && dstsd && heal > 0 && sd != dstsd && battle_config.heal_exp > 0) {
					heal_get_jobexp = heal_get_jobexp * battle_config.heal_exp / 100;
					if(heal_get_jobexp <= 0)
						heal_get_jobexp = 1;
					pc_gainexp(sd,bl,0,heal_get_jobexp,0);
				}
			}
			break;

		case PR_REDEMPTIO:
			if(flag&1) {
				if(!status_isdead(bl))
					break;
				if(!skill_area_temp[0]) {
					status_set_hp(src,1,0);
					status_set_sp(src,1,0);
				}
				skill_area_temp[0]++;
				skill_area_temp[0] = battle_config.exp_cost_redemptio_limit - skill_area_temp[0]; //The actual penalty
				if(skill_area_temp[0] > 0 && battle_config.exp_cost_redemptio) //If total penalty is 1% => reduced 0.2% penalty per each revived player
					pc_lostexp(sd,umin(sd->status.base_exp,(pc_nextbaseexp(sd) * skill_area_temp[0] * battle_config.exp_cost_redemptio / battle_config.exp_cost_redemptio_limit) / 100),0);
				status_revive(bl,50,0);
				clif_skill_nodamage(bl,bl,ALL_RESURRECTION,-1,1);
			} else if(sd) {
				struct skill_condition req = skill_get_requirement(sd,skill_id,skill_lv);
				unsigned int exp, exp_needp = battle_config.exp_cost_redemptio;

				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				if(exp_needp && (exp = pc_nextbaseexp(sd)) > 0 && get_percentage(sd->status.base_exp,exp) < exp_needp) {
					map_freeblock_unlock();
					return 1; //Not enough EXP
				}
				if(!sd->status.party_id)
					break;
				status_zap(src,0,req.sp);
				skill_area_temp[0] = 0;
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,
					skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			}
			break;

		case ALL_RESURRECTION: {
				int per = 0, sper = 0;

				if(!status_isdead(bl))
					break;
				if(sd && (map_flag_gvg2(bl->m) || mapdata[bl->m].flag.battleground)) { //No reviving in WoE grounds!
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				if(mapdata[bl->m].flag.pvp && dstsd && dstsd->pvp_point < 0)
					break;
				if(tsc && tsc->data[SC_HELLPOWER]) {
					clif_skill_nodamage(src,bl,ALL_RESURRECTION,skill_lv,1);
					break;
				}
				switch(skill_lv) {
					case 1: per = 10; break;
					case 2: per = 30; break;
					case 3: per = 50; break;
					case 4: per = 80; break;
				}
				if(dstsd && dstsd->special_state.restart_full_recover)
					per = sper = 100;
				if(status_revive(bl,per,sper)) {
					if(!(flag&1)) //AB_EPICLESIS have no resurrection animation
						clif_skill_nodamage(src,bl,ALL_RESURRECTION,skill_lv,1);
					if(sd && dstsd && battle_config.resurrection_exp) {
						uint32 bexp = 0, jexp = 0;
						int lv = status_get_lv(bl) - status_get_lv(src),
							jlv = status_get_job_lv(bl) - status_get_job_lv(src);

						if(lv && pc_nextbaseexp(dstsd)) {
							bexp = (uint32)((double)dstsd->status.base_exp * (double)lv * (double)battle_config.resurrection_exp / 1000000.);
							bexp = umax(bexp,1);
						}
						if(jlv && pc_nextjobexp(dstsd)) {
							jexp = (uint32)((double)dstsd->status.job_exp * (double)jlv * (double)battle_config.resurrection_exp / 1000000.);
							jexp = umax(jexp,1);
						}
						if(bexp || jexp)
							pc_gainexp(sd,bl,bexp,jexp,0);
					}
				}
			}
			break;

		case AL_DECAGI:
		case MER_DECAGI:
			if (!(i = sc_start(src,bl,type,(50 + skill_lv * 3 + (status_get_lv(src) + sstatus->int_) / 5),skill_lv,skill_get_time(skill_id,skill_lv)))) {
				if (sd)
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				map_freeblock_unlock();
				return 1;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,i);
			break;

		case AL_CRUCIS:
			if (flag&1)
				sc_start(src,bl,type,23 + skill_lv * 4 + status_get_lv(src) - status_get_lv(bl),skill_lv,skill_get_time(skill_id,skill_lv));
			else {
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
			}
			break;

		case PR_LEXDIVINA:
		case MER_LEXDIVINA:
			if (tsce)
				status_change_end(bl,type,INVALID_TIMER);
			else
				skill_addtimerskill(src,tick + 1000,bl->id,0,0,skill_id,skill_lv,100,flag);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SA_ABRACADABRA: {
				int abra_skill_id = 0, abra_skill_lv, checked = 0, checked_max = MAX_SKILL_ABRA_DB * 3;

				do {
					i = rnd()%MAX_SKILL_ABRA_DB;
					abra_skill_id = skill_abra_db[i].skill_id;
					abra_skill_lv = min(skill_lv,skill_get_max(abra_skill_id));
				} while (checked++ < checked_max && (!abra_skill_id || rnd()%10000 >= skill_abra_db[i].per[max(skill_lv - 1,0)]));
				if (!skill_get_index(abra_skill_id))
					break;
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				if (sd) { //Player-casted
					sd->state.abra_flag = 1;
					sd->skillitem = abra_skill_id;
					sd->skillitemlv = abra_skill_lv;
					sd->skilliteminf = 0;
					clif_item_skill(sd,abra_skill_id,abra_skill_lv,0);
				} else { //Mob-casted
					struct unit_data *ud = unit_bl2ud(src);
					int inf = skill_get_inf(abra_skill_id);

					if (!ud)
						break;
					if ((inf&INF_SELF_SKILL) || (inf&INF_SUPPORT_SKILL)) {
						if (src->type == BL_PET)
							bl = (struct block_list *)((TBL_PET *)src)->master;
						if (!bl)
							bl = src;
						unit_skilluse_id(src,bl->id,abra_skill_id,abra_skill_lv);
					} else { //Assume offensive skills
						int target_id = 0;

						if (ud->target)
							target_id = ud->target;
						else switch (src->type) {
							case BL_MOB: target_id = ((TBL_MOB *)src)->target_id; break;
							case BL_PET: target_id = ((TBL_PET *)src)->target_id; break;
						}
						if (!target_id)
							break;
						if (skill_get_casttype(abra_skill_id) == CAST_GROUND) {
							bl = map_id2bl(target_id);
							if (!bl)
								bl = src;
							unit_skilluse_pos(src,bl->x,bl->y,abra_skill_id,abra_skill_lv);
						} else
							unit_skilluse_id(src,target_id,abra_skill_id,abra_skill_lv);
					}
				}
			}
			break;

		case SA_COMA:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time2(skill_id,skill_lv)));
			break;

		case SA_FULLRECOVERY:
			if (!status_isimmune(bl))
				status_percent_heal(bl,100,100);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case NPC_ALLHEAL: {
				int heal = 0;

				if (dstmd) { //Reset Damage Logs
					memset(dstmd->dmglog,0,sizeof(dstmd->dmglog));
					dstmd->tdmg = 0;
				}
				if (!status_isimmune(bl))
					heal = status_percent_heal(bl,100,0);
				clif_skill_nodamage(NULL,bl,AL_HEAL,heal,1);
			}
			break;

		case SA_SUMMONMONSTER:
			if (sd)
				mob_once_spawn(sd,src->m,src->x,src->y,"--ja--",-1,1,"",SZ_SMALL,AI_NONE);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SA_LEVELUP:
			if (sd && pc_nextbaseexp(sd))
				pc_gainexp(sd,NULL,pc_nextbaseexp(sd) * 10 / 100,0,0);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SA_INSTANTDEATH:
			status_kill(src);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SA_QUESTION:
			clif_emotion(src,E_WHAT);
		//Fall through
		case SA_GRAVITY:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SA_CLASSCHANGE:
		case SA_MONOCELL:
			if (dstmd) {
				int mob_id = (skill_id == SA_MONOCELL ? MOBID_PORING : mob_get_random_id(MOBG_ClassChange,RMF_DB_RATE,0));

				if (status_has_mode(&dstmd->status,MD_STATUS_IMMUNE)) {
					if (sd)
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				mob_class_change(dstmd,mob_id);
				if (status_has_mode(&dstmd->status,MD_STATUS_IMMUNE)) {
					const enum sc_type scs[] = { SC_QUAGMIRE,SC_PROVOKE,SC_ROKISWEIL,SC_GRAVITATION,SC_SUITON,
						SC_STRIPWEAPON,SC_STRIPSHIELD,SC_STRIPARMOR,SC_STRIPHELM,SC_BLADESTOP };

					if (!tsc)
						break;
					for (i = SC_COMMON_MIN; i <= SC_COMMON_MAX; i++)
						if (tsc->data[i])
							status_change_end(bl,(sc_type)i,INVALID_TIMER);
					for (i = 0; i < ARRAYLENGTH(scs); i++)
						if (tsc->data[scs[i]])
							status_change_end(bl,scs[i],INVALID_TIMER);
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case SA_DEATH:
			if (dstmd && status_has_mode(&dstmd->status,MD_STATUS_IMMUNE)) {
				if (sd)
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			status_kill(bl);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SA_FORTUNE:
			if (sd)
				pc_getzeny(sd,status_get_lv(bl) * 100,LOG_TYPE_STEAL,NULL);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SA_TAMINGMONSTER:
			if (sd && dstmd) {
				ARR_FIND(0,MAX_PET_DB,i,dstmd->mob_id == pet_db[i].class_);
				if (i < MAX_PET_DB)
					pet_catch_process1(sd,dstmd->mob_id);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case CR_PROVIDENCE:
			if (dstsd && (dstsd->class_&MAPID_UPPERMASK) == MAPID_CRUSADER) { //Check they are not another crusader [Skotlex]
				if (sd)
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				map_freeblock_unlock();
				return 1;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			break;

		case CG_MARIONETTE: {
				struct status_change *sc = status_get_sc(src);

				//Cannot cast on another bard/dancer-type class of the same gender as caster
				if (sd && dstsd && (dstsd->class_&MAPID_UPPERMASK) == MAPID_BARDDANCER && dstsd->status.sex == sd->status.sex) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					map_freeblock_unlock();
					return 1;
				}
				if (sc && tsc) {
					if (!sc->data[SC_MARIONETTE] && !tsc->data[SC_MARIONETTE2]) {
						sc_start(src,src,SC_MARIONETTE,100,bl->id,skill_get_time(skill_id,skill_lv));
						sc_start(src,bl,SC_MARIONETTE2,100,src->id,skill_get_time(skill_id,skill_lv));
						clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
					} else if (sc->data[SC_MARIONETTE ] &&  sc->data[SC_MARIONETTE ]->val1 == bl->id &&
						tsc->data[SC_MARIONETTE2] && tsc->data[SC_MARIONETTE2]->val1 == src->id) {
						status_change_end(src,SC_MARIONETTE,INVALID_TIMER);
						status_change_end(bl,SC_MARIONETTE2,INVALID_TIMER);
					} else {
						if (sd)
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						map_freeblock_unlock();
						return 1;
					}
				}
			}
			break;

		case RG_CLOSECONFINE:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start4(src,bl,type,100,skill_lv,src->id,0,0,skill_get_time(skill_id,skill_lv)));
			break;

		case SA_FLAMELAUNCHER: //Added failure chance and chance to break weapon if turned on [Valaris]
		case SA_FROSTWEAPON:
		case SA_LIGHTNINGLOADER:
		case SA_SEISMICWEAPON:
			if (dstsd) {
				if (dstsd->status.weapon == W_FIST ||
					(dstsd->sc.count && !dstsd->sc.data[type] &&
					( //Allow re-enchanting to lenghten time [Skotlex]
						dstsd->sc.data[SC_FIREWEAPON] ||
						dstsd->sc.data[SC_WATERWEAPON] ||
						dstsd->sc.data[SC_WINDWEAPON] ||
						dstsd->sc.data[SC_EARTHWEAPON] ||
						dstsd->sc.data[SC_SHADOWWEAPON] ||
						dstsd->sc.data[SC_GHOSTWEAPON] ||
						dstsd->sc.data[SC_ENCPOISON]
					)
					))
				{
					if (sd)
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					clif_skill_nodamage(src,bl,skill_id,skill_lv,0);
					break;
				}
			}
			//100% success rate at lv4 & 5, but lasts longer at lv5
			if (!clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,60 + skill_lv * 10,skill_lv,skill_get_time(skill_id,skill_lv)))) {
				if (sd)
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				if (skill_break_equip(src,bl,EQP_WEAPON,10000,BCT_PARTY) && sd && sd != dstsd)
					clif_displaymessage(sd->fd,msg_txt(sd,669)); // You broke the target's weapon.
			}
			break;

		case PR_ASPERSIO:
			if (bl->type != BL_MOB)
				i = sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
			clif_skill_nodamage(src,bl,skill_id,skill_lv,i);
			break;

		case ITEM_ENCHANTARMS:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start2(src,bl,type,100,skill_lv,skill_get_ele(skill_id,skill_lv),skill_get_time(skill_id,skill_lv)));
			break;

		case TK_SEVENWIND:
			switch (skill_get_ele(skill_id,skill_lv)) {
				case ELE_EARTH : type = SC_EARTHWEAPON;  break;
				case ELE_WIND  : type = SC_WINDWEAPON;   break;
				case ELE_WATER : type = SC_WATERWEAPON;  break;
				case ELE_FIRE  : type = SC_FIREWEAPON;   break;
				case ELE_GHOST : type = SC_GHOSTWEAPON;  break;
				case ELE_DARK  : type = SC_SHADOWWEAPON; break;
				case ELE_HOLY  : type = SC_ASPERSIO;     break;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			sc_start(src,bl,SC_SEVENWIND,100,skill_lv,skill_get_time(skill_id,skill_lv));
			break;

		case PR_KYRIE:
		case MER_KYRIE:
			clif_skill_nodamage(bl,bl,skill_id,-1,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			break;

		//Passive Magnum, should had been casted on yourself
		case SM_MAGNUM:
		case MS_MAGNUM:
			skill_area_temp[1] = 0;
			map_foreachinshootrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_SKILL|BL_CHAR,src,
				skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_damage_id);
			//Initiate 20% of your damage becomes fire element
			sc_start2(src,src,SC_WATK_ELEMENT,100,ELE_FIRE,20,skill_get_time2(skill_id,skill_lv));
			clif_skill_nodamage(src,src,skill_id,skill_lv,1);
			break;

		case TK_JUMPKICK:
			if (unit_movepos(src,bl->x,bl->y,2,true)) {
				clif_blown(src,bl);
				skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			}
			break;

		case AL_INCAGI:
		case AL_BLESSING:
		case MER_INCAGI:
		case MER_BLESSING:
			if (bl->type == BL_PC && tsc->data[SC_CHANGEUNDEAD]) {
				skill_attack(BF_MISC,src,src,bl,skill_id,skill_lv,tick,flag);
				break;
			}
		//Fall through
		case SM_ENDURE:
		case PR_SLOWPOISON:
		case PR_IMPOSITIO:
		case PR_LEXAETERNA:
		case PR_SUFFRAGIUM:
		case PR_BENEDICTIO:
		case LK_BERSERK:
		case MS_BERSERK:
		case KN_TWOHANDQUICKEN:
		case KN_ONEHAND:
		case MER_QUICKEN:
		case CR_SPEARQUICKEN:
		case CR_REFLECTSHIELD:
		case MS_REFLECTSHIELD:
		case AS_POISONREACT:
		case MC_LOUD:
		case MG_ENERGYCOAT:
		case MO_EXPLOSIONSPIRITS:
		case MO_STEELBODY:
		case MO_BLADESTOP:
		case SA_REVERSEORCISH:
		case ALL_REVERSEORCISH:
		case LK_AURABLADE:
		case LK_PARRYING:
		case MS_PARRYING:
		case LK_CONCENTRATION:
		case WS_CARTBOOST:
		case SN_SIGHT:
		case WS_MELTDOWN:
		case WS_OVERTHRUSTMAX:
		case ST_REJECTSWORD:
		case HW_MAGICPOWER:
		case PF_MEMORIZE:
		case PA_SACRIFICE:
		case ASC_EDP:
		case PF_DOUBLECASTING:
		case SG_SUN_COMFORT:
		case SG_MOON_COMFORT:
		case SG_STAR_COMFORT:
#ifndef RENEWAL
		case GS_MADNESSCANCEL:
#endif
		case GS_ADJUSTMENT:
		case GS_INCREASING:
#ifdef RENEWAL
		case GS_MAGICALBULLET:
#endif
		case NJ_KASUMIKIRI:
		case NJ_UTSUSEMI:
		case NJ_NEN:
		case NPC_DEFENDER:
		case NPC_MAGICMIRROR:
		case ST_PRESERVE:
		case NPC_INVINCIBLE:
		case NPC_INVINCIBLEOFF:
		case NPC_MAXPAIN:
		case ALL_ANGEL_PROTECT:
		case ALL_RAY_OF_PROTECTION:
		case RK_ENCHANTBLADE:
		case RK_CRUSHSTRIKE:
		case RA_FEARBREEZE:
		case AB_EXPIATIO:
		case AB_DUPLELIGHT:
		case AB_SECRAMENT:
		case AB_OFFERTORIUM:
		case NC_ACCELERATION:
		case NC_HOVERING:
		case NC_SHAPESHIFT:
		case WL_RECOGNIZEDSPELL:
		case GC_VENOMIMPRESS:
		case GC_HALLUCINATIONWALK:
		case SC_INVISIBILITY:
		case SC_DEADLYINFECT:
		case LG_EXEEDBREAK:
		case LG_PRESTIGE:
		case SR_CRESCENTELBOW:
		case SR_LIGHTNINGWALK:
		case SR_GENTLETOUCH_ENERGYGAIN:
		case SR_GENTLETOUCH_CHANGE:
		case SR_GENTLETOUCH_REVITALIZE:
		case GN_CARTBOOST:
		case ALL_FULL_THROTTLE:
		case RA_UNLIMIT:
		case WL_TELEKINESIS_INTENSE:
		case ALL_ODINS_POWER:
		case RL_E_CHAIN:
		case RL_P_ALTER:
		case RL_HEAT_BARREL:
		case SJ_LIGHTOFMOON:
		case SJ_LIGHTOFSTAR:
		case SJ_FALLINGSTAR:
		case SJ_BOOKOFDIMENSION:
		case SJ_LIGHTOFSUN:
		case SP_SOULREAPER:
		case SU_STOOP:
		case SU_ARCLOUSEDASH:
		case SU_TUNAPARTY:
		case SU_FRESHSHRIMP:
		case SU_GROOMING:
		case HLIF_CHANGE:
		case HAMI_BLOODLUST:
		case HFLI_FLEET:
		case HFLI_SPEED:
		case MH_ANGRIFFS_MODUS:
		case MH_GOLDENE_FERSE:
		case MH_PAIN_KILLER:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			break;

		case SJ_GRAVITYCONTROL: {
				int fall_damage = (sstatus->batk + sstatus->rhw.atk) - tstatus->def2;

				if (dstsd)
					fall_damage += dstsd->weight / 10;
				fall_damage -= tstatus->def;
				fall_damage = max(fall_damage,1);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,
					sc_start2(src,bl,type,100,skill_lv,fall_damage,skill_get_time(skill_id,skill_lv)));
			}
			break;

		case KN_AUTOCOUNTER:
			sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
			skill_addtimerskill(src,tick + 100,bl->id,0,0,skill_id,skill_lv,BF_WEAPON,flag);
			break;

		case SO_STRIKING:
			if (battle_check_target(src,bl,BCT_PARTY) > 0 || map_flag_vs(src->m)) {
				int bonus = 0;

				if (dstsd) {
					short index = dstsd->equip_index[EQI_HAND_R];

					if( index >= 0 && dstsd->inventory_data[index] && dstsd->inventory_data[index]->type == IT_WEAPON )
						bonus = (8 + 2 * skill_lv) * dstsd->inventory_data[index]->wlv;
				}
				bonus += 5 * (sd ? (pc_checkskill(sd,SA_FLAMELAUNCHER) + pc_checkskill(sd,SA_FROSTWEAPON) + pc_checkskill(sd,SA_LIGHTNINGLOADER) + pc_checkskill(sd,SA_SEISMICWEAPON)) : 20);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,
					sc_start2(src,bl,type,100,skill_lv,bonus,skill_get_time(skill_id,skill_lv)));
			}
			break;

		case NPC_HALLUCINATION:
		case NPC_HELLPOWER:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,20 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv)));
			break;

		case NPC_STOP:
			if (clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start2(src,bl,type,100,skill_lv,src->id,skill_get_time(skill_id,skill_lv))))
				sc_start2(src,src,type,100,skill_lv,bl->id,skill_get_time(skill_id,skill_lv));
			break;

		case HP_ASSUMPTIO:
			if (bl->type == BL_MOB) {
				if (sd)
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			break;

		case MG_SIGHT:
		case MER_SIGHT:
		case AL_RUWACH:
		case WZ_SIGHTBLASTER:
		case NPC_WIDESIGHT:
		case NPC_STONESKIN:
		case NPC_ANTIMAGIC:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start2(src,bl,type,100,skill_lv,skill_id,skill_get_time(skill_id,skill_lv)));
			break;

		case HLIF_AVOID: {
				struct block_list *m_bl = NULL;

				if (!(m_bl = battle_get_master(src)))
					break;
				clif_skill_nodamage(m_bl,m_bl,skill_id,skill_lv,
					sc_start(src,m_bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv))); //Master
				clif_skill_nodamage(src,src,skill_id,skill_lv,
					sc_start(src,src,type,100,skill_lv,skill_get_time(skill_id,skill_lv))); //Homun
			}
			break;

		case HAMI_DEFENCE: {
				struct block_list *m_bl = NULL;

				if (!(m_bl = battle_get_master(src)))
					break;
				sc_start(src,m_bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
				clif_skill_nodamage(src,src,skill_id,skill_lv,
					sc_start(src,src,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			}
			break;

		case NJ_BUNSINJYUTSU:
			//On official recasting cancels existing mirror image [helvetica]
			status_change_end(bl,SC_BUNSINJYUTSU,INVALID_TIMER);
			status_change_end(bl,SC_NEN,INVALID_TIMER);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			break;

		case AS_ENCHANTPOISON: //Prevent spamming [Valaris]
			if (dstsd && dstsd->sc.count && (
				dstsd->sc.data[SC_FIREWEAPON] ||
				dstsd->sc.data[SC_WATERWEAPON] ||
				dstsd->sc.data[SC_WINDWEAPON] ||
				dstsd->sc.data[SC_EARTHWEAPON] ||
				dstsd->sc.data[SC_SHADOWWEAPON] ||
				dstsd->sc.data[SC_GHOSTWEAPON]
				//dstsd->sc.data[SC_ENCPOISON] //People say you should be able to recast to lengthen the timer [Skotlex]
			)) {
				if (sd)
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,0);
				break;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			break;

		case LK_TENSIONRELAX:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start4(src,bl,type,100,skill_lv,0,0,skill_get_time2(skill_id,skill_lv),skill_get_time(skill_id,skill_lv)));
			break;

		case MC_CHANGECART:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case MC_CARTDECORATE:
			if (sd)
				clif_SelectCart(sd);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case TK_MISSION:
			if (sd) {
				int id;

				if (sd->mission_mobid && (sd->mission_count || rnd()%100)) { //Cannot change target when already have one
					clif_mission_info(sd,sd->mission_mobid,sd->mission_count);
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				if (!(id = mob_get_random_id(MOBG_Taekwon_Mission,RMF_NONE,0))) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				sd->mission_mobid = id;
				sd->mission_count = 0;
				pc_setglobalreg(sd,"TK_MISSION_ID",id);
				clif_mission_info(sd,id,0);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case AC_CONCENTRATION: {
				int splash = skill_get_splash(skill_id,skill_lv);

				map_foreachinallrange(status_change_timer_sub,src,splash,BL_CHAR,src,NULL,type,tick);
				skill_reveal_trap_inarea(src,splash,src->x,src->y);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,
					sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			}
			break;

		case SM_PROVOKE:
		case SM_SELFPROVOKE:
		case MER_PROVOKE:
			{ // Official chance is 70% + 3%*skill_lv + srcBaseLevel% - tarBaseLevel%
				int rate = (skill_id == SM_SELFPROVOKE ? 100 : (70 + 3 * skill_lv + status_get_lv(src) - status_get_lv(bl)));

				if (!(i = sc_start(src,bl,type,rate,skill_lv,skill_get_time(skill_id,skill_lv)))) {
					if (sd)
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					map_freeblock_unlock();
					return 1;
				}
				if (dstmd) {
					dstmd->state.provoke_flag = src->id;
					mob_target(dstmd,src,skill_get_range2(src,skill_id,skill_lv,true));
				}
				unit_skillcastcancel(bl,2);
				clif_skill_nodamage(src,bl,(skill_id == SM_SELFPROVOKE ? SM_PROVOKE : skill_id),skill_lv,i);
			}
			break;

		case CR_DEVOTION:
		case ML_DEVOTION:
			{
				int lv, count;

				if (!dstsd || (!sd && !mer) || //Only players can be devoted
					!check_distance_bl(src,bl,skill_get_range2(src,skill_id,skill_lv,true))) {
					if (sd)
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				if ((lv = status_get_lv(src) - dstsd->status.base_level) < 0)
					lv = -lv;
				if (lv > battle_config.devotion_level_difference || //Level difference requeriments
					(dstsd->sc.data[type] && dstsd->sc.data[type]->val1 != src->id) || //Cannot Devote a player devoted from another source
					(skill_id == ML_DEVOTION && (!mer || mer != dstsd->md)) || //Mercenary only can devote owner
					(dstsd->class_&MAPID_UPPERMASK) == MAPID_CRUSADER || //Crusader Cannot be devoted
					(dstsd->sc.data[SC_HELLPOWER])) //Players affected by SC_HELLPOWER cannot be devoted
				{
					if (sd)
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				count = (sd ? min(skill_lv,MAX_DEVOTION) : 1);
				if (sd) { //Player Devoting Player
					ARR_FIND(0,count,i,sd->devotion[i] == bl->id); //Check if target's already devoted
					if (i == count) {
						ARR_FIND(0,count,i,sd->devotion[i] == 0);
						if (i == count) { //No free slots, skill fail
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
							break;
						}
					}
					sd->devotion[i] = bl->id;
				} else
					mer->devotion_flag = 1; //Mercenary Devoting Owner
				clif_skill_nodamage(src,bl,skill_id,skill_lv,
					sc_start4(src,bl,type,100,src->id,i,skill_get_range2(src,skill_id,skill_lv,true),0,skill_get_time2(skill_id,skill_lv)));
				clif_devotion(src,NULL);
			}
			break;

		case MO_CALLSPIRITS:
			if (sd)
				pc_addspiritball(sd,skill_get_time(skill_id,skill_lv),pc_getmaxspiritball(sd,0));
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case CH_SOULCOLLECT:
			if (sd) {
				int max = pc_getmaxspiritball(sd,5);

				for (i = 0; i < max; i++)
					pc_addspiritball(sd,skill_get_time(skill_id,skill_lv),max);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case MO_KITRANSLATION:
			if (sd)
				pc_delspiritball(sd,1,0);
			if (dstsd)
				pc_addspiritball(dstsd,skill_get_time(skill_id,skill_lv),5);
			break;

		case TK_TURNKICK:
		case MO_BALKYOUNG: //Passive part of the attack, splash knock-back + stun [Skotlex]
			if (skill_area_temp[1] != bl->id) {
				skill_blown(src,bl,skill_get_blewcount(skill_id,skill_lv),-1,0);
				skill_additional_effect(src,bl,skill_id,skill_lv,BF_MISC,ATK_DEF,tick); //Use Misc rather than weapon to signal passive pushback
			}
			break;

		case MO_ABSORBSPIRITS:
			if (dstsd && (sd == dstsd || map_flag_vs(src->m) || (sd && sd->duel_group && sd->duel_group == dstsd->duel_group)) &&
				(dstsd->class_&MAPID_BASEMASK) != MAPID_GUNSLINGER && (dstsd->class_&MAPID_UPPERMASK) != MAPID_REBELLION) {
				//Split the if for readability, and included gunslingers in the check so that their coins cannot be removed [Reddozen]
				if (dstsd->spiritball > 0) {
					i = dstsd->spiritball * 7;
					pc_delspiritball(dstsd,dstsd->spiritball,0);
				}
				if (dstsd->charmball_type != CHARM_TYPE_NONE && dstsd->charmball > 0) {
					i += dstsd->charmball * 7;
					pc_delcharmball(dstsd,dstsd->charmball,dstsd->charmball_type);
				}
			} else if (dstmd && !status_has_mode(tstatus,MD_STATUS_IMMUNE) && rnd()%100 < 20) {
				//Check if target is a monster and not a Boss, for the 20% chance to absorb 2 SP per monster's level [Reddozen]
				i = 2 * dstmd->level;
				mob_target(dstmd,src,0);
			}
			if (sd) {
				struct skill_condition req = skill_get_requirement(sd,skill_id,skill_lv);

				if (i || dstmd)
					status_zap(src,0,req.sp);
				if (!i)
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				else
					status_heal(src,0,i,3);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case AC_MAKINGARROW:
			if (sd)
				clif_arrow_create_list(sd);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case AM_PHARMACY:
			if (sd)
				clif_skill_produce_mix_list(sd,skill_id,22);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SA_CREATECON:
			if (sd)
				clif_elementalconverter_list(sd);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case BS_HAMMERFALL:
			skill_addtimerskill(src,tick + 1000,bl->id,0,0,skill_id,skill_lv,min(20 + 10 * skill_lv,50 + 5 * skill_lv),flag);
			break;

		case RG_RAID:
			skill_area_temp[1] = 0;
			map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR|BL_SKILL,src,
				skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_damage_id);
			status_change_end(src,SC_HIDING,INVALID_TIMER);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		//List of self skills that give damage around caster
		case ASC_METEORASSAULT:
		case RK_IGNITIONBREAK:
		case RK_STORMBLAST:
		case GC_COUNTERSLASH:
		case WL_FROSTMISTY:
		case WL_JACKFROST:
		case NPC_JACKFROST:
		case SR_RAMPAGEBLASTER:
		case SR_WINDMILL:
		case SR_HOWLINGOFLION:
		case GN_CART_TORNADO:
		case RL_R_TRIP:
		case RL_FIREDANCE:
		case RL_D_TAIL:
		case SJ_FULLMOONKICK:
		case SJ_NEWMOONKICK:
		case SJ_STAREMPEROR:
		case SJ_SOLARBURST:
		case KO_HAPPOKUNAI:
			{
				int starget = BL_CHAR|BL_SKILL;

				if (skill_id == SR_HOWLINGOFLION || skill_id == RL_D_TAIL)
					starget = splash_target(src);
				else if (skill_id == SJ_NEWMOONKICK)
					sc_start(src,src,SC_NEWMOON,100,skill_lv,skill_get_time(skill_id,skill_lv));
				else if (skill_id == SJ_STAREMPEROR) {
					if (sd) {
						pc_delshieldball(sd,sd->shieldball,1);
						for (i = 0; i < 2; i++)
							pc_addshieldball(sd,skill_get_time2(SJ_BOOKOFDIMENSION,1),2,status_get_sp(src));
					}
					status_change_end(src,SC_DIMENSION,INVALID_TIMER);
				}
				skill_area_temp[1] = 0;
				map_foreachinrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),starget,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_SPLASH|1,skill_castend_damage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case GS_SPREADATTACK:
		case GC_PHANTOMMENACE:
		case NC_AXETORNADO:
		case LG_MOONSLASHER:
		case SR_EARTHSHAKER:
			skill_area_temp[1] = 0;
			map_foreachinrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR|BL_SKILL,src,
				skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_SPLASH|1,skill_castend_damage_id);
			clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			break;

		case NPC_PULSESTRIKE:
		case NPC_HELLJUDGEMENT:
		case NPC_VAMPIRE_GIFT:
			skill_castend_damage_id(src,src,skill_id,skill_lv,tick,flag);
			break;

		case KN_BRANDISHSPEAR:
		case ML_BRANDISH:
			skill_area_temp[1] = bl->id;
			if (skill_lv >= 10)
				map_foreachindir(skill_area_sub,src->m,src->x,src->y,bl->x,bl->y,skill_get_splash(skill_id,skill_lv),1,skill_get_maxcount(skill_id,skill_lv) - 1,splash_target(src),src,skill_id,skill_lv,tick,flag|BCT_ENEMY|(sd ? 3 : 0),skill_castend_damage_id);
			if (skill_lv >= 7)
				map_foreachindir(skill_area_sub,src->m,src->x,src->y,bl->x,bl->y,skill_get_splash(skill_id,skill_lv),1,skill_get_maxcount(skill_id,skill_lv) - 2,splash_target(src),src,skill_id,skill_lv,tick,flag|BCT_ENEMY|(sd ? 2 : 0),skill_castend_damage_id);
			if (skill_lv >= 4)
				map_foreachindir(skill_area_sub,src->m,src->x,src->y,bl->x,bl->y,skill_get_splash(skill_id,skill_lv),1,skill_get_maxcount(skill_id,skill_lv) - 3,splash_target(src),src,skill_id,skill_lv,tick,flag|BCT_ENEMY|(sd ? 1 : 0),skill_castend_damage_id);
			map_foreachindir(skill_area_sub,src->m,src->x,src->y,bl->x,bl->y,skill_get_splash(skill_id,skill_lv),skill_get_maxcount(skill_id,skill_lv) - 3,0,splash_target(src),src,skill_id,skill_lv,tick,flag|BCT_ENEMY|0,skill_castend_damage_id);
			break;

		case WZ_SIGHTRASHER:
			//Passive side of the attack
			map_foreachinshootrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_CHAR|BL_SKILL,src,
				skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_ANIMATION|1,skill_castend_damage_id);
			status_change_end(src,SC_SIGHT,INVALID_TIMER);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case WZ_FROSTNOVA:
			skill_area_temp[1] = 0;
			map_foreachinshootrange(skill_attack_area,src,skill_get_splash(skill_id,skill_lv),splash_target(src),
				BF_MAGIC,src,src,skill_id,skill_lv,tick,flag,BCT_ENEMY);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case NPC_SELFDESTRUCTION:
		case HVAN_EXPLOSION:
			//Self Destruction hits everyone in range (allies + enemies)
			//Except for Summoned Marine spheres on non-versus maps, where it's just enemy [orn]
			i = ((!md || md->special_state.ai == AI_SPHERE) && !map_flag_vs(src->m) ? BCT_ENEMY : BCT_ALL);
			map_delblock(src); //Required to prevent chain-self-destructions hitting back
			map_foreachinshootrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR|BL_SKILL,src,
				skill_id,skill_lv,tick,flag|i|SD_ANIMATION|SD_SPLASH,skill_castend_damage_id);
			if (map_addblock(src))
				return 1;
			status_damage(src,src,sstatus->max_hp,0,0,1);
			clif_skill_nodamage(src,src,skill_id,-1,1);
			break;

		case AL_ANGELUS:
		case PR_MAGNIFICAT:
		case PR_GLORIA:
		case SN_WINDWALK:
		case CASH_BLESSING:
		case CASH_INCAGI:
		case CASH_ASSUMPTIO:
			if (!sd || !sd->status.party_id || (flag&1))
				clif_skill_nodamage(bl,bl,skill_id,skill_lv,sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			else if (sd)
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case MER_MAGNIFICAT:
			if (flag&1)
				clif_skill_nodamage(bl,bl,skill_id,skill_lv,sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			else if (mer && mer->master) {
				if (!mer->master->status.party_id)
					clif_skill_nodamage(src,&mer->master->bl,skill_id,skill_lv,sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
				else
					party_foreachsamemap(skill_area_sub,mer->master,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			}
			break;

		case BS_ADRENALINE:
		case BS_ADRENALINE2:
		case BS_WEAPONPERFECT:
		case BS_OVERTHRUST:
			if (!sd || !sd->status.party_id || (flag&1)) {
				int weapontype = skill_get_weapontype(skill_id);

				if (!weapontype || !dstsd || pc_check_weapontype(dstsd,weapontype))
					clif_skill_nodamage(bl,bl,skill_id,skill_lv,sc_start2(src,bl,type,100,skill_lv,(bl->id == src->id ? 1 : 0),skill_get_time(skill_id,skill_lv)));
			} else if (sd)
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case SL_KAITE:
		case SL_KAAHI:
		case SL_KAIZEL:
		case SL_KAUPE:
		case SP_KAUTE:
			{
				uint8 flag2 = 0;

				if (sd) {
					if (!dstsd ||
						!((sd->sc.data[SC_SPIRIT] && sd->sc.data[SC_SPIRIT]->val2 == SL_SOULLINKER) ||
						(dstsd->class_&MAPID_UPPERMASK) == MAPID_SOUL_LINKER ||
						dstsd->status.char_id == sd->status.char_id ||
						dstsd->status.char_id == sd->status.partner_id ||
						dstsd->status.char_id == sd->status.child ||
						(skill_id == SP_KAUTE && dstsd->sc.data[SC_SOULUNITY])))
					{
						status_change_start(src,src,SC_STUN,10000,skill_lv,0,0,0,500,SCFLAG_FIXEDRATE);
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						break;
					}
#ifdef RENEWAL
					if (skill_id == SL_KAITE && sd->sc.data[SC_SPIRIT] && sd->sc.data[SC_SPIRIT]->val2 == SL_SOULLINKER)
						flag2 = 1;
#endif
				}
				if (skill_id == SP_KAUTE)
					clif_skill_nodamage(src,bl,skill_id,skill_lv,status_heal(bl,0,status_get_max_sp(bl) * (10 + 2 * skill_lv) / 100,2));
				else
					clif_skill_nodamage(src,bl,skill_id,skill_lv,sc_start4(src,bl,type,100,skill_lv,0,flag2,0,skill_get_time(skill_id,skill_lv)));
			}
			break;

		case TF_HIDING:
		case BS_MAXIMIZE:
		case AS_CLOAKING:
		case NV_TRICKDEAD:
		case CR_AUTOGUARD:
		case ML_AUTOGUARD:
		case ST_CHASEWALK:
		case SG_FUSION:
		case CR_SHRINK:
		case GC_CLOAKINGEXCEED:
		case RA_CAMOUFLAGE:
		case SJ_LUNARSTANCE:
		case SJ_STARSTANCE:
		case SJ_UNIVERSESTANCE:
		case SJ_SUNSTANCE:
		case SU_HIDE:
			if (tsce)
				i = status_change_end(bl,type,INVALID_TIMER);
			else
				i = sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
			if (skill_id == TF_HIDING || skill_id == AS_CLOAKING || skill_id == ST_CHASEWALK || skill_id == GC_CLOAKINGEXCEED)
				clif_skill_nodamage(src,bl,skill_id,-1,i);
			else
				clif_skill_nodamage(src,bl,skill_id,skill_lv,i);
			break;

		case CR_DEFENDER:
		case ML_DEFENDER:
			if (tsce) {
				status_change_end(bl,type,INVALID_TIMER);
				map_freeblock_unlock();
				return 1;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			break;

		case SM_AUTOBERSERK:
		case MER_AUTOBERSERK:
		case TK_READYSTORM:
		case TK_READYDOWN:
		case TK_READYTURN:
		case TK_READYCOUNTER:
		case TK_DODGE:
#ifdef RENEWAL
		case GS_MADNESSCANCEL:
#endif
		case GS_GATLINGFEVER:
		case LG_FORCEOFVANGUARD:
		case SC_REPRODUCE:
		case KO_YAMIKUMO:
			if (tsce)
				i = status_change_end(bl,type,INVALID_TIMER);
			else {
				i = sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
				if (skill_id == SC_REPRODUCE)
					clif_specialeffect(bl,EF_RECOGNIZED2,AREA);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,i);
			break;

		case TK_RUN:
			if (tsce)
				i = status_change_end(bl,type,INVALID_TIMER);
			else
				i = sc_start2(src,bl,type,100,skill_lv,unit_getdir(bl),0);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,i);
			if (sd) //If the client receives a skill-use packet inmediately before a walkok packet, it will discard the walk packet! [Skotlex]
				clif_walkok(sd); //So aegis has to resend the walk ok
			break;

		case BD_ADAPTATION:
			if (tsc && tsc->data[SC_DANCING]) {
				status_change_end(bl,SC_DANCING,INVALID_TIMER);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case BA_FROSTJOKER:
		case DC_SCREAM:
			skill_addtimerskill(src,tick + 3000,bl->id,src->x,src->y,skill_id,skill_lv,0,flag);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			//Custom hack to make the mob display the skill, because these skills don't show the skill use text themselves
			//NOTE: mobs don't have the sprite animation that is used when performing this skill (will cause glitches)
			if (md) {
				char temp[70];

				snprintf(temp,sizeof(temp),"%s : %s !!",md->name,skill_db[skill_get_index(skill_id)].desc);
				clif_disp_overhead(&md->bl,temp);
			}
			break;

		case BA_PANGVOICE:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,SC_CONFUSION,50,7,skill_get_time(skill_id,skill_lv)));
			break;

		case DC_WINKCHARM:
			if (dstsd)
				clif_skill_nodamage(src,bl,skill_id,skill_lv,sc_start(src,bl,SC_CONFUSION,30,7,skill_get_time2(skill_id,skill_lv)));
			else if (dstmd) {
				if (status_get_lv(src) > status_get_lv(bl) &&
					(tstatus->race == RC_DEMON || tstatus->race == RC_DEMIHUMAN || tstatus->race == RC_ANGEL) &&
					!status_has_mode(tstatus,MD_STATUS_IMMUNE))
					clif_skill_nodamage(src,bl,skill_id,skill_lv,
						sc_start2(src,bl,type,70,skill_lv,src->id,skill_get_time(skill_id,skill_lv)));
				else {
					if (sd)
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					clif_skill_nodamage(src,bl,skill_id,skill_lv,0);
				}
			}
			break;

		case TF_STEAL:
			if (sd && !pc_steal_item(sd,bl,skill_lv)) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL,0,0);
				break;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case RG_STEALCOIN:
			if (sd && !pc_steal_coin(sd,bl)) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			if (dstmd) {
				dstmd->state.provoke_flag = src->id;
				mob_target(dstmd,src,skill_get_range2(src,skill_id,skill_lv,true));
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case MG_STONECURSE: {
				int rate = skill_lv * 4 + 20;

				if (status_isimmune(bl))
					break;
				if (sd && sd->sc.data[SC_PETROLOGY_OPTION])
					rate += sd->sc.data[SC_PETROLOGY_OPTION]->val3;
				if (tsce)
					i = status_change_end(bl,type,INVALID_TIMER);
				else {
					i = sc_start4(src,bl,type,rate,skill_lv,0,skill_get_time(skill_id,skill_lv),0,skill_get_time2(skill_id,skill_lv));
					if (!i) {
						if (skill_lv > 5)
							flag |= 1; //Level 6-10 doesn't consume a red gem if it fails [celest]
						if (sd)
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						break;
					}
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,i);
			}
			break;

		case NV_FIRSTAID:
			status_heal(bl,5,0,0);
			clif_skill_nodamage(src,bl,skill_id,5,1);
			break;

		case AL_CURE:
			if (status_isimmune(bl))
				clif_skill_nodamage(src,bl,skill_id,skill_lv,0);
			else {
				status_change_end(bl,SC_SILENCE,INVALID_TIMER);
				status_change_end(bl,SC_BLIND,INVALID_TIMER);
				status_change_end(bl,SC_CONFUSION,INVALID_TIMER);
				status_change_end(bl,SC_BITESCAR,INVALID_TIMER);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case TF_DETOXIFY:
			status_change_end(bl,SC_POISON,INVALID_TIMER);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case PR_STRECOVERY:
			if (status_isimmune(bl))
				clif_skill_nodamage(src,bl,skill_id,skill_lv,0);
			else {
				status_change_end(bl,SC_FREEZE,INVALID_TIMER);
				if (tsc && tsc->data[SC_STONE] && tsc->opt1 == OPT1_STONE)
					status_change_end(bl,SC_STONE,INVALID_TIMER);
				status_change_end(bl,SC_SLEEP,INVALID_TIMER);
				status_change_end(bl,SC_STUN,INVALID_TIMER);
				status_change_end(bl,SC_WHITEIMPRISON,INVALID_TIMER);
				status_change_end(bl,SC_STASIS,INVALID_TIMER);
				status_change_end(bl,SC_NETHERWORLD,INVALID_TIMER);
				if (battle_check_undead(tstatus->race,tstatus->def_ele))
					skill_addtimerskill(src,tick + 1000,bl->id,0,0,skill_id,skill_lv,100,flag);
				if (dstmd)
					mob_unlocktarget(dstmd,tick);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		//Mercenary Supportive Skills
		case MER_BENEDICTION:
			status_change_end(bl,SC_CURSE,INVALID_TIMER);
			status_change_end(bl,SC_BLIND,INVALID_TIMER);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case MER_COMPRESS:
			status_change_end(bl,SC_BLEEDING,INVALID_TIMER);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case MER_MENTALCURE:
			status_change_end(bl,SC_CONFUSION,INVALID_TIMER);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case MER_RECUPERATE:
			status_change_end(bl,SC_POISON,INVALID_TIMER);
			status_change_end(bl,SC_SILENCE,INVALID_TIMER);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case MER_REGAIN:
			status_change_end(bl,SC_SLEEP,INVALID_TIMER);
			status_change_end(bl,SC_STUN,INVALID_TIMER);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case MER_TENDER:
			status_change_end(bl,SC_FREEZE,INVALID_TIMER);
			status_change_end(bl,SC_STONE,INVALID_TIMER);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case MER_SCAPEGOAT:
			if (mer && mer->master) {
				status_heal(&mer->master->bl,mer->battle_status.hp,0,2);
				status_damage(src,src,mer->battle_status.max_hp,0,0,1);
			}
			break;

		case MER_ESTIMATION:
			if (!mer)
				break;
			sd = mer->master;
		//Fall through
		case WZ_ESTIMATION:
			if (!sd)
				break;
			if (dstsd) { //Fail on Players
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			if (dstmd && dstmd->mob_id == MOBID_EMPERIUM)
				break; //Cannot be used on Emperium
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			clif_skill_estimation(sd,bl);
			if (skill_id == MER_ESTIMATION)
				sd = NULL;
			break;

		case BS_REPAIRWEAPON:
			if (sd && dstsd)
				clif_item_repair_list(sd,dstsd,skill_lv);
			break;

		case MC_IDENTIFY:
			if (sd) {
				clif_item_identify_list(sd);
				if (sd->menuskill_id != MC_IDENTIFY || sd->skillitem == skill_id) {
					map_freeblock_unlock();
					return 1; //Only consume SP when succeeded
				}
			}
			break;

		case WS_WEAPONREFINE: //Weapon Refining [Celest]
			if (sd)
				clif_item_refine_list(sd);
			break;

		case MC_VENDING:
			if (sd) { //Prevent vending of GMs with unnecessary Level to trade/drop [Skotlex]
				if (!pc_can_give_items(sd)) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					map_freeblock_unlock();
					return 1;
				} else {
					sd->state.prevend = 1;
					sd->vend_skill_lv = skill_lv;
					ARR_FIND(0,MAX_CART,i,(sd->cart.u.items_cart[i].nameid && !sd->cart.u.items_cart[i].id));
					if (i < MAX_CART)
						intif_storage_save(sd,&sd->cart);
					else
						clif_openvendingreq(sd,2 + skill_lv);
				}
			}
			break;

		case AL_TELEPORT:
		case ALL_ODINS_RECALL:
			if (sd) {
				if (mapdata[bl->m].flag.noteleport && skill_lv <= 2) {
					clif_skill_teleportmessage(sd,0);
					break;
				}
				if (!battle_config.duel_allow_teleport && sd->duel_group && skill_lv <= 2) { //Duel restriction [LuzZza]
					char output[128];

					sprintf(output,msg_txt(sd,365),skill_get_name(AL_TELEPORT));
					clif_displaymessage(sd->fd,output); //"Duel: Can't use %s in duel."
					break;
				}
				if (sd->hd && (battle_config.hom_setting&HOMSET_RESET_REUSESKILL_TELEPORTED))
					memset(sd->hd->blockskill,0,sizeof(hd->blockskill));
				if (sd->state.autocast || ((sd->skillitem == AL_TELEPORT ||
					battle_config.skip_teleport_lv1_menu) && skill_lv == 1) || skill_lv == 3) {
					if (skill_lv == 1)
						pc_randomwarp(sd,CLR_TELEPORT);
					else
						pc_setpos(sd,sd->status.save_point.map,sd->status.save_point.x,sd->status.save_point.y,CLR_TELEPORT);
					break;
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				if (skill_lv == 1 && skill_id != ALL_ODINS_RECALL)
					clif_skill_warppoint(sd,skill_id,skill_lv,(unsigned short)-1,0,0,0);
				else
					clif_skill_warppoint(sd,skill_id,skill_lv,(unsigned short)-1,sd->status.save_point.map,0,0);
			} else
				unit_warp(bl,-1,-1,-1,CLR_TELEPORT);
			break;

		case NPC_EXPULSION:
			unit_warp(bl,-1,-1,-1,CLR_TELEPORT);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case AL_HOLYWATER: {
				struct skill_unit *unit = NULL;

				if (sd && !skill_produce_mix(sd,skill_id,ITEMID_HOLY_WATER,0,0,0,0,1)) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				if ((unit = map_find_skill_unit_oncell(bl,bl->x,bl->y,NJ_SUITON,NULL,0)))
					skill_delunit(unit);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case TF_PICKSTONE:
			if (sd) {
				unsigned char eflag;
				struct item item_tmp;
				struct block_list tbl;

				memset(&item_tmp,0,sizeof(item_tmp));
				memset(&tbl,0,sizeof(tbl)); //[MouseJstr]
				item_tmp.nameid = ITEMID_STONE;
				item_tmp.identify = 1;
				tbl.id = 0;
				//Commented because of duplicate animation [Lemongrass]
				//At the moment this displays the pickup animation a second time
				//If this is required in older clients, we need to add a version check here
				//clif_takeitem(&sd->bl,&tbl);
				if ((eflag = pc_additem(sd,&item_tmp,1,LOG_TYPE_PRODUCE))) {
					clif_additem(sd,0,0,eflag);
					map_addflooritem(&item_tmp,1,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0,0,false);
				}
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case ASC_CDP:
			if (sd && !skill_produce_mix(sd,skill_id,ITEMID_POISON_BOTTLE,0,0,0,0,1)) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_STUFF_INSUFFICIENT,0,0);
				break;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case RG_STRIPWEAPON:
		case RG_STRIPSHIELD:
		case RG_STRIPARMOR:
		case RG_STRIPHELM:
		case ST_FULLSTRIP:
		case GC_WEAPONCRUSH:
		case SC_STRIPACCESSARY:
			{
				bool strip = false;

				if( skill_id == ST_FULLSTRIP && tsc &&
					tsc->data[SC_CP_WEAPON] && tsc->data[SC_CP_HELM] &&
					tsc->data[SC_CP_ARMOR] && tsc->data[SC_CP_SHIELD] )
				{
					if( sd )
						clif_gospel_info(sd,0x28); //Special message when trying to use Full Strip on FCP [Jobbie]
					break;
				}
				//Attempts to strip
				if( (strip = skill_strip_equip(src,bl,skill_id,skill_lv)) || (skill_id != ST_FULLSTRIP && skill_id != GC_WEAPONCRUSH) )
					clif_skill_nodamage(src,bl,skill_id,skill_lv,strip);
				if( !strip && sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			}
			break;

		case AM_BERSERKPITCHER:
		case AM_POTIONPITCHER:
			{
				int hp = 0, sp = 0;

				if( sd ) {
					int x, bonus = 100;
					struct skill_condition req = skill_get_requirement(sd,skill_id,skill_lv);

					if( dstmd ) {
						if( dstmd->mob_id != MOBID_EMPERIUM )
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						map_freeblock_unlock();
						return 1;
					}
					x = skill_lv%11 - 1;
					i = pc_search_inventory(sd,req.itemid[x]);
					if( i == INDEX_NOT_FOUND || req.itemid[x] <= 0 || !sd->inventory_data[i] ||
						sd->inventory.u.items_inventory[i].amount < req.amount[x] ) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						map_freeblock_unlock();
						return 1;
					}
					if( skill_id == AM_BERSERKPITCHER ) {
						if( dstsd && dstsd->status.base_level < (unsigned int)sd->inventory_data[i]->elv ) {
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
							map_freeblock_unlock();
							return 1;
						}
					}
					potion_flag = 1;
					potion_hp = potion_sp = potion_per_hp = potion_per_sp = 0;
					potion_target = bl->id;
					run_script(sd->inventory_data[i]->script,0,sd->bl.id,0);
					potion_flag = potion_target = 0;
					if( sd->sc.data[SC_SPIRIT] && sd->sc.data[SC_SPIRIT]->val2 == SL_ALCHEMIST )
						bonus += sd->status.base_level;
					if( potion_per_hp > 0 || potion_per_sp > 0 ) {
						hp = tstatus->max_hp * potion_per_hp / 100;
						hp = hp * (100 + pc_checkskill(sd,AM_POTIONPITCHER) * 10 + pc_checkskill(sd,AM_LEARNINGPOTION) * 5) * bonus / 10000;
						if( dstsd ) {
							sp = dstsd->status.max_sp * potion_per_sp / 100;
							sp = sp * (100 + pc_checkskill(sd,AM_POTIONPITCHER) * 10 + pc_checkskill(sd,AM_LEARNINGPOTION) * 5) * bonus / 10000;
						}
					} else {
						if( potion_hp > 0 ) {
							hp = potion_hp * (100 + pc_checkskill(sd,AM_POTIONPITCHER) * 10 + pc_checkskill(sd,AM_LEARNINGPOTION) * 5) * bonus / 10000;
							hp = hp * (100 + (tstatus->vit<<1)) / 100;
							if( dstsd )
								hp = hp * (100 + pc_checkskill(dstsd,SM_RECOVERY) * 10) / 100;
						}
						if( potion_sp > 0 ) {
							sp = potion_sp * (100 + pc_checkskill(sd,AM_POTIONPITCHER) * 10 + pc_checkskill(sd,AM_LEARNINGPOTION) * 5) * bonus / 10000;
							sp = sp * (100 + (tstatus->int_<<1)) / 100;
							if( dstsd )
								sp = sp * (100 + pc_checkskill(dstsd,MG_SRECOVERY) * 10) / 100;
						}
					}

					if( (bonus = pc_get_itemgroup_bonus_group(sd,IG_POTION)) ) {
						hp += hp * bonus / 100;
						sp += sp * bonus / 100;
					}

					if( (i = pc_skillheal_bonus(sd,skill_id)) ) {
						hp += hp * i / 100;
						sp += sp * i / 100;
					}
				} else {
					//Maybe replace with potion_hp, but I'm unsure how that works [Playtester]
					switch( skill_lv ) {
						case 1: hp = 45; break;
						case 2: hp = 105; break;
						case 3: hp = 175; break;
						default: hp = 325; break;
					}
					hp = (hp + rnd()%(skill_lv * 20 + 1)) * (150 + skill_lv * 10) / 100;
					hp = hp * (100 + (tstatus->vit<<1)) / 100;
					if( dstsd )
						hp = hp * (100 + pc_checkskill(dstsd,SM_RECOVERY) * 10) / 100;
				}
				if( dstsd && (i = pc_skillheal2_bonus(dstsd,skill_id)) ) {
					hp += hp * i / 100;
					sp += sp * i / 100;
				}
				if( tsc && tsc->count ) {
					uint8 penalty = 0;

					if( tsc->data[SC_WATER_INSIGNIA] && tsc->data[SC_WATER_INSIGNIA]->val1 == 2 ) {
						hp += hp / 10;
						sp += sp / 10;
					}
					if( tsc->data[SC_CRITICALWOUND] )
						penalty += tsc->data[SC_CRITICALWOUND]->val2;
					if( tsc->data[SC_DEATHHURT] )
						penalty += 20;
					if( tsc->data[SC_NORECOVER_STATE] )
						penalty = 100;
					if( penalty > 0 ) {
						hp -= hp * penalty / 100;
						sp -= sp * penalty / 100;
					}
					if( tsc->data[SC_EXTREMITYFIST2] )
						sp = 0;
				}
				status_heal(bl,hp,sp,0);
				if( hp > 0 || (skill_id == AM_POTIONPITCHER && sp <= 0) )
					clif_skill_nodamage(NULL,bl,AL_HEAL,hp,1);
				if( sp > 0 )
					clif_skill_nodamage(NULL,bl,MG_SRECOVERY,sp,1);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case AM_CP_WEAPON:
		case AM_CP_SHIELD:
		case AM_CP_ARMOR:
		case AM_CP_HELM:
			{
				unsigned int equip[] = { EQP_WEAPON,EQP_SHIELD,EQP_ARMOR,EQP_HEAD_TOP };

				if( sd && (bl->type != BL_PC || (dstsd && pc_checkequip(dstsd,equip[skill_id - AM_CP_WEAPON],false) < 0)) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					map_freeblock_unlock(); //Don't consume item requirements
					return 1;
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,
					sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			}
			break;

		case AM_TWILIGHT1: //Prepare 200 White Potions
			if( sd && !skill_produce_mix(sd,skill_id,ITEMID_WHITE_POTION,0,0,0,0,200) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case AM_TWILIGHT2: //Prepare 200 Slim White Potions
			if( sd && !skill_produce_mix(sd,skill_id,ITEMID_WHITE_SLIM_POTION,0,0,0,0,200) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case AM_TWILIGHT3:
			if( sd ) {
				short
#ifdef RENEWAL
					balcohol = pc_search_inventory(sd,ITEMID_ALCOL_CREATE_BOOK),
					bgrenade = pc_search_inventory(sd,ITEMID_FIREBOTTLE_CREATE_BOOK),
					bacid = pc_search_inventory(sd,ITEMID_ACID_CREATE_BOOK),
#endif
					ebottle = pc_search_inventory(sd,ITEMID_EMPTY_BOTTLE);

				if( ebottle != INDEX_NOT_FOUND )
					ebottle = sd->inventory.u.items_inventory[ebottle].amount;
#ifdef RENEWAL //In renewal, as long as have all three books and 200 Empty Bottle, can do any combination except "Fire Bottle without Alcohol" [exneval]
				if( balcohol == INDEX_NOT_FOUND || bgrenade == INDEX_NOT_FOUND || bacid == INDEX_NOT_FOUND ||
					ebottle < 200 ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				if( skill_can_produce_mix(sd,ITEMID_ALCOHOL,0,-1,100) ) { //100 Alcohol
					if( skill_produce_mix(sd,skill_id,ITEMID_ALCOHOL,0,0,0,0,100) &&
						skill_can_produce_mix(sd,ITEMID_FIRE_BOTTLE,0,-1,50) ) //50 Fire Bottle
						skill_produce_mix(sd,skill_id,ITEMID_FIRE_BOTTLE,0,0,0,0,50);
				}
				if( skill_can_produce_mix(sd,ITEMID_ACID_BOTTLE,0,-1,50) ) //50 Acid Bottle
					skill_produce_mix(sd,skill_id,ITEMID_ACID_BOTTLE,0,0,0,0,50);
#else
				//Check if you can produce all three, if not, then fail
				if( !skill_can_produce_mix(sd,ITEMID_ALCOHOL,0,-1,100) || //100 Alcohol
					!skill_can_produce_mix(sd,ITEMID_ACID_BOTTLE,0,-1,50) || //50 Acid Bottle
					!skill_can_produce_mix(sd,ITEMID_FIRE_BOTTLE,0,-1,50) || //50 Flame Bottle
					ebottle < 200 ) //200 empty bottle are required at total
				{
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				skill_produce_mix(sd,skill_id,ITEMID_ALCOHOL,0,0,0,0,100);
				skill_produce_mix(sd,skill_id,ITEMID_ACID_BOTTLE,0,0,0,0,50);
				skill_produce_mix(sd,skill_id,ITEMID_FIRE_BOTTLE,0,0,0,0,50);
#endif
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SA_DISPELL:
			if( skill_lv < 6 || (flag&1) ) {
				if( sd && dstsd && !map_flag_vs(bl->m) && (!sd->duel_group || sd->duel_group != dstsd->duel_group) &&
					battle_check_target(src,bl,BCT_PARTY) <= 0 ) {
					map_freeblock_unlock();
					return 1;
				}
				if( rnd()%100 >= 50 + 10 * skill_lv ) {
					if( sd )
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					map_freeblock_unlock();
					return 1;
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				if( status_isimmune(bl) ||
					(dstsd && (dstsd->class_&MAPID_UPPERMASK) == MAPID_SOUL_LINKER) || //Soul Linkers are naturally immune
					!(tsc && tsc->count) ||
					(tsc->data[SC_SPIRIT] && tsc->data[SC_SPIRIT]->val2 == SL_ROGUE) ) //Rogue's spirit defends against dispel
					break;
				if( dstsd ) //Remove bonus_script by Dispell
					pc_bonus_script_clear(dstsd,BSF_REM_ON_DISPELL);
				for( i = 0; i < SC_MAX; i++ ) {
					if( !tsc->data[i] )
						continue;
					if( !(status_get_sc_type((sc_type)i)&SC_REM_DISPELL) )
						continue;
					switch( i ) {
						case SC_SILENCE:
							if( tsc->data[i]->val4 )
								continue; //Silence from Silent Breeze can't be dispeled
							break;
						case SC_WHISTLE:
						case SC_ASSNCROS:
						case SC_POEMBRAGI:
						case SC_APPLEIDUN:
						case SC_HUMMING:
						case SC_DONTFORGETME:
						case SC_FORTUNE:
						case SC_SERVICE4U:
							if( !battle_config.dispel_song || !tsc->data[i]->val4 )
								continue; //If in song area don't end it, even if config enabled
							break;
						case SC_ASSUMPTIO:
							if( bl->type == BL_MOB )
								continue;
							break;
						case SC_BERSERK:
						case SC_SATURDAYNIGHTFEVER:
							tsc->data[i]->val2 = 0; //Mark a dispelled berserk to avoid setting hp to 100 by setting hp penalty to 0
							break;
					}
					status_change_end(bl,(sc_type)i,INVALID_TIMER);
				}
			} else //Affect all targets on splash area
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,skill_id,skill_lv,tick,flag|1,skill_castend_nodamage_id);
			break;

		case TF_BACKSLIDING: //This is the correct implementation as per packet logging information [Skotlex]
			skill_blown(src,bl,skill_get_blewcount(skill_id,skill_lv),unit_getdir(bl),3);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			clif_blown(src,src); //Always blow, otherwise it shows a casting animation [Lemongrass]
			break;

		case TK_HIGHJUMP: {
				int x, x1, y, y1, dir = unit_getdir(src);
				int range = skill_get_range(skill_id,skill_lv);

				//Fails on noteleport maps, except for GvG and BG maps [Skotlex]
				if (mapdata[src->m].flag.noteleport && !(mapdata[src->m].flag.battleground || map_flag_gvg2(src->m))) {
					clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
					break;
				} else if (dir%2) { //Diagonal
					x = src->x + dirx[dir] * range * 2 / 3;
					y = src->y + diry[dir] * range * 2 / 3;
				} else {
					x = src->x + dirx[dir] * range;
					y = src->y + diry[dir] * range;
				}
				x1 = x + dirx[dir];
				y1 = y + diry[dir];
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				if (!map_count_oncell(src->m,x,y,BL_PC|BL_NPC|BL_MOB,0) && map_getcell(src->m,x,y,CELL_CHKREACH) &&
					!map_count_oncell(src->m,x1,y1,BL_PC|BL_NPC|BL_MOB,0) && map_getcell(src->m,x1,y1,CELL_CHKREACH) &&
					unit_movepos(src,x,y,1,false))
					clif_blown(src,src);
			}
			break;

		case SA_CASTCANCEL:
		case SO_SPELLFIST:
			unit_skillcastcancel(src,1);
			if (sd) {
				if (skill_id == SA_CASTCANCEL) {
					int sp = skill_get_sp(sd->skill_id_old,sd->skill_lv_old);

					sp = max(sp * (90 - (skill_lv - 1) * 20) / 100,0);
					status_zap(src,0,sp);
				} else {
					sc_start4(src,src,type,100,skill_lv,skill_lv + 1,sd->skill_id_old,sd->skill_lv_old,skill_get_time(skill_id,skill_lv));
					sd->skill_id_old = sd->skill_lv_old = 0;
				}
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SA_SPELLBREAKER: {
				int sp;

				if (tsc && tsc->data[SC_MAGICROD]) {
					sp = skill_get_sp(skill_id,skill_lv);
					sp = sp * tsc->data[SC_MAGICROD]->val2 / 100;
					sp = max(sp,1);
					status_heal(bl,0,sp,2);
					status_percent_damage(bl,src,0,-20,false); //20% MaxSP damage
				} else {
					struct unit_data *ud = unit_bl2ud(bl);
					int bl_skill_id = 0, bl_skill_lv = 0, hp = 0;

					if (!ud || ud->skilltimer == INVALID_TIMER)
						break; //Nothing to cancel
					bl_skill_id = ud->skill_id;
					bl_skill_lv = ud->skill_lv;
					if (status_has_mode(tstatus,MD_STATUS_IMMUNE)) { //Only 10% success chance against bosses [Skotlex]
						if (rnd()%100 < 90) {
							if (sd)
								clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
							break;
						}
					}
					//HP damage only on vs-maps when against players and skill need to be at level 5
					if ((!dstsd || map_flag_vs(bl->m)) && skill_lv >= 5)
						hp = tstatus->max_hp / 50; //Damage 2% HP [Skotlex]
					unit_skillcastcancel(bl,0);
					sp = skill_get_sp(bl_skill_id,bl_skill_lv);
					status_damage(src,bl,hp,sp,clif_damage(src,bl,tick,0,0,hp,0,DMG_NORMAL,0,false),1);
					if (hp) //Absorb half damaged HP [Skotlex]
						hp /= 2;
					if (sp) //Recover some of the SP used
						sp = sp * (25 * (skill_lv - 1)) / 100;
					if (hp || sp)
						status_heal(src,hp,sp,2);
					clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				}
			}
			break;

		case SA_MAGICROD:
			sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
#ifdef RENEWAL
			clif_skill_nodamage(src,src,skill_id,skill_lv,1);
#endif
			break;

		case SA_AUTOSPELL:
			if (sd)
				clif_autospell(sd,skill_lv);
			else {
				int maxlv = 1, spellid = 0;
				static const int spellarray[3] = { MG_COLDBOLT,MG_FIREBOLT,MG_LIGHTNINGBOLT };

				if (skill_lv >= 10) {
					spellid = MG_FROSTDIVER;
					//if (tsc && tsc->data[SC_SPIRIT] && tsc->data[SC_SPIRIT]->val2 == SA_SAGE)
						//maxlv = 10;
					//else
						maxlv = skill_lv - 9;
				} else if (skill_lv >= 8) {
					spellid = MG_FIREBALL;
					maxlv = skill_lv - 7;
				} else if (skill_lv >= 5) {
					spellid = MG_SOULSTRIKE;
					maxlv = skill_lv - 4;
				} else if (skill_lv >= 2) {
					i = rnd()%3;
					spellid = spellarray[i];
					maxlv = skill_lv - 1;
				} else if (skill_lv > 0) {
					spellid = MG_NAPALMBEAT;
					maxlv = 3;
				}
				if (spellid > 0)
					sc_start4(src,src,SC_AUTOSPELL,100,skill_lv,spellid,maxlv,0,skill_get_time(SA_AUTOSPELL,skill_lv));
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case BS_GREED:
			if (src->type == BL_PC)
				map_foreachinallrange(skill_greed,bl,skill_get_splash(skill_id,skill_lv),BL_ITEM,bl);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SA_ELEMENTWATER:
		case SA_ELEMENTFIRE:
		case SA_ELEMENTGROUND:
		case SA_ELEMENTWIND:
			if (bl->type != BL_MOB || status_has_mode(tstatus,MD_STATUS_IMMUNE))
				break; //Only works on monsters (Except status immune monsters)
		//Fall through
		case NPC_ATTRICHANGE:
		case NPC_CHANGEWATER:
		case NPC_CHANGEGROUND:
		case NPC_CHANGEFIRE:
		case NPC_CHANGEWIND:
		case NPC_CHANGEPOISON:
		case NPC_CHANGEHOLY:
		case NPC_CHANGEDARKNESS:
		case NPC_CHANGETELEKINESIS:
		case NPC_CHANGEUNDEAD:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start2(src,bl,type,100,skill_lv,skill_get_ele(skill_id,skill_lv),skill_get_time(skill_id,skill_lv)));
			break;

		case NPC_PROVOCATION:
			if (md)
				mob_unlocktarget(md,tick);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case NPC_KEEPING:
		case NPC_BARRIER:
			{
				int skill_time = skill_get_time(skill_id,skill_lv);
				struct unit_data *ud = unit_bl2ud(bl);

				//Disable attacking/acting/moving for skill's duration
				if (clif_skill_nodamage(src,bl,skill_id,skill_lv,sc_start(src,bl,type,100,skill_lv,skill_time)) && ud)
					ud->attackabletime = ud->canact_tick = ud->canmove_tick = tick + skill_time;
			}
			break;

		case NPC_REBIRTH:
			if (md && md->state.rebirth)
				break; //Only works once
			sc_start(src,bl,type,100,skill_lv,INVALID_TIMER);
			break;

		case NPC_DARKBLESSING:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start2(src,bl,type,50 + skill_lv * 5,skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv)));
			break;

		case NPC_LICK:
			status_zap(bl,0,100);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,skill_lv * 20,skill_lv,skill_get_time2(skill_id,skill_lv)));
			break;

		case NPC_SUICIDE:
			status_kill(src); //When suiciding, neither exp nor drops is given
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case NPC_SUMMONSLAVE:
		case NPC_SUMMONMONSTER:
		case NPC_DEATHSUMMON:
			if (md && md->skill_idx >= 0)
				mob_summonslave(md,md->db->skill[md->skill_idx].val,skill_lv,skill_id);
			break;

		case NPC_CALLSLAVE:
			mob_warpslave(src,MOB_SLAVEDISTANCE);
			break;

		case NPC_RANDOMMOVE:
			if (md) {
				md->next_walktime = tick - 1;
				mob_randomwalk(md,tick);
			}
			break;

		case NPC_SPEEDUP: //Or does it increase casting rate? Just a guess
			i = SC_ASPDPOTION0 + skill_lv - 1;
			if (i > SC_ASPDPOTION3)
				i = SC_ASPDPOTION3;
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,(sc_type)i,100,skill_lv,skill_lv * 60000));
			break;

		case NPC_REVENGE: //Not really needed, but adding here anyway
			if (md && md->master_id) {
				struct block_list *mbl, *tbl;

				if (!(mbl = map_id2bl(md->master_id)) || !(tbl = battle_gettargeted(mbl)))
					break;
				md->state.provoke_flag = tbl->id;
				mob_target(md,tbl,sstatus->rhw.range);
			}
			break;

		case NPC_RUN:
			if (md) {
				struct block_list *tbl = map_id2bl(md->target_id);

				if (tbl) {
					unit_escape(src,tbl,rnd()%10 + 1);
					skill_addtimerskill(src,tick + 50,0,0,0,skill_id,skill_lv,0,flag);
				}
			}
			break;

		case NPC_TRANSFORMATION:
		case NPC_METAMORPHOSIS:
			if (md && md->skill_idx >= 0) {
				int class_ = mob_random_class(md->db->skill[md->skill_idx].val,0);

				if (skill_lv > 1) //Multiply the rest of mobs [Skotlex]
					mob_summonslave(md,md->db->skill[md->skill_idx].val,skill_lv - 1,skill_id);
				if (class_)
					mob_class_change(md,class_);
			}
			break;

		case NPC_EMOTION_ON:
		case NPC_EMOTION:
			//val[0] is the emotion to use
			//NPC_EMOTION & NPC_EMOTION_ON can change a mob's mode 'permanently' [Skotlex]
			//val[1] 'sets' the mode
			//val[2] adds to the current mode
			//val[3] removes from the current mode
			//val[4] if set, asks to delete the previous mode change
			if (md && md->skill_idx >= 0 && tsc) {
				clif_emotion(bl,md->db->skill[md->skill_idx].val[0]);
				if (md->db->skill[md->skill_idx].val[4] && tsce)
					status_change_end(bl,type,INVALID_TIMER);
				//If mode gets set by NPC_EMOTION then the target should be reset [Playtester]
				if (skill_id == NPC_EMOTION && md->db->skill[md->skill_idx].val[1])
					mob_unlocktarget(md,tick);
				if (md->db->skill[md->skill_idx].val[1] || md->db->skill[md->skill_idx].val[2])
					sc_start4(src,src,type,100,skill_lv,
						md->db->skill[md->skill_idx].val[1],
						md->db->skill[md->skill_idx].val[2],
						md->db->skill[md->skill_idx].val[3],
						skill_get_time(skill_id,skill_lv));
				//Reset aggressive state depending on resulting mode
				md->state.aggressive = (status_has_mode(&md->status,MD_ANGRY) ? 1 : 0);
			}
			break;

		case NPC_POWERUP:
			sc_start(src,bl,SC_INCATKRATE,100,200,skill_get_time(skill_id,skill_lv));
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,100,skill_get_time(skill_id,skill_lv)));
			break;

		case NPC_AGIUP:
			sc_start(src,bl,SC_SPEEDUP0,100,50,skill_get_time(skill_id,skill_lv));
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,100,skill_get_time(skill_id,skill_lv)));
			break;

		case NPC_INVISIBLE: //Have val4 passed as 6 is for "infinite cloak" (do not end on attack/skill use)
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start4(src,bl,type,100,skill_lv,0,0,6,skill_get_time(skill_id,skill_lv)));
			break;

		case NPC_SIEGEMODE: //Not sure what it does
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case WE_MALE:
			if (sd) {
				struct map_session_data *p_sd = map_charid2sd(sd->status.partner_id);

				if (status_get_hp(src) > status_get_max_hp(src) / 10) {
					int hp_rate = (!skill_lv ? 0 : skill_get_hp_rate(skill_id,skill_lv));
					int gain_hp; //The earned is the same % of the target HP than it costed the caster [Skotlex]

					if (p_sd) {
						gain_hp = status_get_max_hp(&p_sd->bl) * abs(hp_rate) / 100;
						clif_skill_nodamage(src,&p_sd->bl,skill_id,status_heal(&p_sd->bl,gain_hp,0,0),1);
					}
				} else
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			}
			break;

		case WE_FEMALE:
			if (sd) {
				struct map_session_data *p_sd = map_charid2sd(sd->status.partner_id);

				if (status_get_sp(src) > status_get_max_sp(src) / 10) {
					int sp_rate = (!skill_lv ? 0 : skill_get_sp_rate(skill_id,skill_lv));
					int gain_sp; //The earned is the same % of the target SP than it costed the caster [Skotlex]

					if (p_sd) {
						gain_sp = status_get_max_sp(&p_sd->bl) * abs(sp_rate) / 100;
						clif_skill_nodamage(src,&p_sd->bl,skill_id,status_heal(&p_sd->bl,0,gain_sp,0),1);
					}
				} else
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			}
			break;

		//Parent-Baby skills
		case WE_BABY:
			if (sd) {
				struct map_session_data *f_sd = pc_get_father(sd);
				struct map_session_data *m_sd = pc_get_mother(sd);
				bool we_baby_parents = false;

				if (f_sd && check_distance_bl(bl,&f_sd->bl,AREA_SIZE)) {
					sc_start(src,&f_sd->bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
					clif_specialeffect(&f_sd->bl,EF_BABY,AREA);
					we_baby_parents = true;
				}
				if (m_sd && check_distance_bl(bl,&m_sd->bl,AREA_SIZE)) {
					sc_start(src,&m_sd->bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
					clif_specialeffect(&m_sd->bl,EF_BABY,AREA);
					we_baby_parents = true;
				}
				if (!we_baby_parents ||
					(sd->status.party_id && //Not in same party
					//If both are online they should all be in same team
					((!f_sd || sd->status.party_id != f_sd->status.party_id) &&
					(!m_sd || sd->status.party_id != m_sd->status.party_id))))
				{
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					map_freeblock_unlock();
					return 1;
				} else
					status_change_start(src,bl,SC_STUN,10000,skill_lv,0,0,0,skill_get_time2(skill_id,skill_lv),SCFLAG_FIXEDRATE);
			}
			break;

		case WE_CALLALLFAMILY:
			if (sd) {
				struct map_session_data *p_sd = pc_get_partner(sd);
				struct map_session_data *c_sd = pc_get_child(sd);

				//Fail if no family members are found
				if (!p_sd && !c_sd) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					map_freeblock_unlock();
					return 1;
				}
				//Partner must be on the same map and in same party as the caster
				if (p_sd && !status_isdead(&p_sd->bl) && p_sd->bl.m == sd->bl.m && p_sd->status.party_id == sd->status.party_id)
					pc_setpos(p_sd,map_id2index(sd->bl.m),sd->bl.x,sd->bl.y,CLR_TELEPORT);
				//Child must be on the same map and in same party as the caster
				if (c_sd && !status_isdead(&c_sd->bl) && c_sd->bl.m == sd->bl.m && c_sd->status.party_id == sd->status.party_id)
					pc_setpos(c_sd,map_id2index(sd->bl.m),sd->bl.x,sd->bl.y,CLR_TELEPORT);
			}
			break;

		case WE_ONEFOREVER:
			if (sd) {
				struct map_session_data *p_sd = pc_get_partner(sd);
				struct map_session_data *c_sd = pc_get_child(sd);

				if (!p_sd && !c_sd && !dstsd) { //Fail if no family members are found
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					map_freeblock_unlock();
					return 1;
				}
				if (map_flag_gvg2(bl->m) || mapdata[bl->m].flag.battleground) { //No reviving in WoE grounds!
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				if (status_isdead(bl)) {
					int per = 30, sper = 0;

					if (tsc && tsc->data[SC_HELLPOWER])
						break;
					if (mapdata[bl->m].flag.pvp && dstsd->pvp_point < 0)
						break;
					if (dstsd->special_state.restart_full_recover)
						per = sper = 100;
					if ((dstsd == p_sd || dstsd == c_sd) && status_revive(bl,per,sper)) //Only family members can be revived
						clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				}
			}
			break;

		case WE_CHEERUP:
			if (sd) {
				struct map_session_data *f_sd = pc_get_father(sd);
				struct map_session_data *m_sd = pc_get_mother(sd);

				if (!f_sd && !m_sd && !dstsd) { //Fail if no parents are found
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					map_freeblock_unlock();
					return 1;
				}
				if (flag&1) { //Buff can only be given to parents in 7x7 AoE around baby
					if (dstsd == f_sd || dstsd == m_sd)
						clif_skill_nodamage(src,bl,skill_id,skill_lv,sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
				} else
					map_foreachinrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_PC,src,skill_id,skill_lv,tick,flag|BCT_ALL|1,skill_castend_nodamage_id);
			}
			break;

		case PF_HPCONVERSION: {
				int hp, sp;

				hp = sstatus->max_hp / 10;
				sp = hp * 10 * skill_lv / 100;
				if (!status_charge(src,hp,0)) {
					if (sd)
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				status_heal(bl,0,sp,2);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case MA_REMOVETRAP:
		case HT_REMOVETRAP:
			{
				struct skill_unit *unit = BL_CAST(BL_SKILL,bl);
				struct skill_unit_group *group = NULL;

				//Mercenaries can remove any trap
				//Players can only remove their own traps or traps on vs-maps
				if (unit && (group = unit->group) && (src->type == BL_MER || group->src_id == src->id || map_flag_vs(bl->m)) &&
					(skill_get_inf2(group->skill_id)&INF2_TRAP) && (group->item_id == ITEMID_TRAP || group->item_id == ITEMID_SPECIAL_ALLOY_TRAP)) {
					//Prevent picking up expired traps
					if (sd && !(group->unit_id == UNT_USED_TRAPS || (group->unit_id == UNT_ANKLESNARE && group->val2))) {
						uint8 flag2;

						if (battle_config.skill_removetrap_type) {
							for (i = 0; i < 10; i++) { //Get back all items used to deploy the trap
								if (skill_get_itemid(group->skill_id,i + 1) > 0) {
									struct item item_tmp;

									memset(&item_tmp,0,sizeof(item_tmp));
									item_tmp.nameid = skill_get_itemid(group->skill_id,i + 1);
									item_tmp.identify = 1;
									item_tmp.amount = skill_get_itemqty(group->skill_id,i + 1);
									if (item_tmp.nameid && (flag2 = pc_additem(sd,&item_tmp,item_tmp.amount,LOG_TYPE_OTHER))) {
										clif_additem(sd,0,0,flag2);
										map_addflooritem(&item_tmp,item_tmp.amount,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,4,0,false);
									}
								}
							}
						} else { //Get back 1 trap
							struct item item_tmp;

							memset(&item_tmp,0,sizeof(item_tmp));
							item_tmp.nameid = group->item_id;
							item_tmp.identify = 1;
							if (item_tmp.nameid && (flag2 = pc_additem(sd,&item_tmp,1,LOG_TYPE_OTHER))) {
								clif_additem(sd,0,0,flag2);
								map_addflooritem(&item_tmp,1,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,4,0,false);
							}
						}
					}
					skill_delunit(unit);
					clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				} else if (sd)
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			}
			break;

		case HT_SPRINGTRAP: {
				struct skill_unit *unit = (struct skill_unit *)bl;
				struct skill_unit_group *group = NULL;

				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				if (bl->type == BL_SKILL && unit && (group = unit->group)) {
					switch (group->unit_id) {
						case UNT_ANKLESNARE:
							if (group->val2)
								break; //If it is already trapping something don't spring it, remove trap should be used instead
						//Fall through
						case UNT_BLASTMINE:
						case UNT_SKIDTRAP:
						case UNT_LANDMINE:
						case UNT_SHOCKWAVE:
						case UNT_SANDMAN:
						case UNT_FLASHER:
						case UNT_FREEZINGTRAP:
						case UNT_CLAYMORETRAP:
						case UNT_TALKIEBOX:
							group->unit_id = UNT_USED_TRAPS;
							clif_changetraplook(bl,UNT_USED_TRAPS);
							group->limit = DIFF_TICK(tick + 1500,group->tick);
							unit->limit = DIFF_TICK(tick + 1500,group->tick);
							break;
					}
				}
			}
			break;

		case BD_ENCORE:
			if (sd)
				unit_skilluse_id(src,src->id,sd->skill_id_dance,sd->skill_lv_dance);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case AS_SPLASHER:
			if (status_has_mode(tstatus,MD_STATUS_IMMUNE)
#ifndef RENEWAL //Dropped the 3/4 hp requirement
				|| tstatus-> hp > tstatus->max_hp * 3 / 4
#endif
			) {
				if (sd)
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				map_freeblock_unlock();
				return 1;
			}
			if (sd) {
				struct skill_condition req = skill_get_requirement(sd,skill_id,skill_lv);

				i = pc_search_inventory(sd,req.itemid[0]);
				if (i == INDEX_NOT_FOUND || req.itemid[0] <= 0 || !sd->inventory_data[i] ||
					sd->inventory.u.items_inventory[i].amount < req.amount[0]) {
					skill_consume_requirement(sd,skill_id,skill_lv,1);
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					map_freeblock_unlock();
					return 1;
				}
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start2(src,bl,type,100,skill_lv,src->id,skill_get_time(skill_id,skill_lv)));
#ifndef RENEWAL
			if (sd)
				skill_blockpc_start(sd,skill_id,skill_get_time(skill_id,skill_lv) + 3000);
#endif
			break;

		case PF_MINDBREAKER:
			if (tsce) { //HelloKitty2 (?) explained that this silently fails when target is already inflicted [Skotlex]
				map_freeblock_unlock();
				return 1;
			} else { //Has a 55% + skill_lv * 5% success chance
				if (sc_start(src,bl,type,55 + 5 * skill_lv,skill_lv,skill_get_time(skill_id,skill_lv))) {
					unit_skillcastcancel(bl,0);
					status_change_end(bl,SC_FREEZE,INVALID_TIMER);
					if (tsc && tsc->data[SC_STONE] && tsc->opt1 == OPT1_STONE)
						status_change_end(bl,SC_STONE,INVALID_TIMER);
					status_change_end(bl,SC_SLEEP,INVALID_TIMER);
					status_change_end(bl,SC_DEEPSLEEP,INVALID_TIMER);
					if (dstmd)
						mob_target(dstmd,src,skill_get_range2(src,skill_id,skill_lv,true));
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case PF_SOULCHANGE: {
				unsigned int sp1 = 0, sp2 = 0;

				if (dstmd) {
					if (dstmd->state.soul_change_flag) {
						if (sd)
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						break;
					}
					dstmd->state.soul_change_flag = 1;
					sp2 = sstatus->max_sp * 3 / 100;
					status_heal(src,0,sp2,2);
				} else {
					sp1 = sstatus->sp;
					sp2 = tstatus->sp;
#ifdef RENEWAL
					sp1 = sp1 / 2;
					sp2 = sp2 / 2;
#endif
					if (tsc && (tsc->data[SC_EXTREMITYFIST2] || tsc->data[SC_NORECOVER_STATE]))
						sp1 = tstatus->sp;
					status_set_sp(src,sp2,3);
					status_set_sp(bl,sp1,3);
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case CR_SLIMPITCHER:
			if (dstmd && (dstmd->mob_id == MOBID_EMPERIUM || status_get_class_(bl) == CLASS_BATTLEFIELD))
				break; //Updated to block Slim Pitcher from working on gvg monsters
			if (potion_hp || potion_sp) {
				int hp = potion_hp, sp = potion_sp;

				hp = hp * (100 + (tstatus->vit<<1)) / 100;
				sp = sp * (100 + (tstatus->int_<<1)) / 100;
				if (dstsd) {
					if (hp)
						hp = hp * (100 + pc_checkskill(dstsd,SM_RECOVERY) * 10 + pc_skillheal2_bonus(dstsd,skill_id)) / 100;
					if (sp)
						sp = sp * (100 + pc_checkskill(dstsd,MG_SRECOVERY) * 10 + pc_skillheal2_bonus(dstsd,skill_id)) / 100;
				}
				if (tsc && tsc->count) {
					uint8 penalty = 0;

					if (tsc->data[SC_WATER_INSIGNIA] && tsc->data[SC_WATER_INSIGNIA]->val1 == 2) {
						hp += hp / 10;
						sp += sp / 10;
					}
					if (tsc->data[SC_CRITICALWOUND])
						penalty += tsc->data[SC_CRITICALWOUND]->val2;
					if (tsc->data[SC_DEATHHURT])
						penalty += 20;
					if (tsc->data[SC_NORECOVER_STATE])
						penalty = 100;
					if (penalty > 0) {
						hp -= hp * penalty / 100;
						sp -= sp * penalty / 100;
					}
				}
				status_heal(bl,hp,sp,0);
				if (hp > 0)
					clif_skill_nodamage(NULL,bl,AL_HEAL,hp,1);
				if (sp > 0)
					clif_skill_nodamage(NULL,bl,MG_SRECOVERY,sp,1);
			}
			break;

		case CR_FULLPROTECTION: {
				unsigned int equip[] = { EQP_WEAPON,EQP_SHIELD,EQP_ARMOR,EQP_HEAD_TOP };
				int s = 0, skilltime = skill_get_time(skill_id,skill_lv);

				for (i = 0; i < 4; i++) {
					if (bl->type != BL_PC || (dstsd && pc_checkequip(dstsd,equip[i],false) < 0))
						continue;
					sc_start(src,bl,(sc_type)(SC_CP_WEAPON + i),100,skill_lv,skilltime);
					s++;
				}
				if (!s) {
					if (sd)
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					flag |= 1; //Don't consume item requirements
				} else
					clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case RG_CLEANER: //[AppleGirl]
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case CG_LONGINGFREEDOM: //Can't use Longing for Freedom while under Sheltering Bliss [Skotlex]
			if (tsc && !tsce && (tsce = tsc->data[SC_DANCING]) && tsce->val4 && (tsce->val1&0xFFFF) != CG_MOONLIT)
				clif_skill_nodamage(src,bl,skill_id,skill_lv,sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			break;

		case CG_TAROTCARD: {
				int card = -1;

				if ((tsc && (tsc->data[SC_BASILICA] || tsc->data[SC_TAROTCARD])) ||
					(dstmd && (dstmd->mob_id == MOBID_EMPERIUM || status_get_class_(bl) == CLASS_BATTLEFIELD))) {
					map_freeblock_unlock();
					return 1;
				}
				if (rnd()%100 >= skill_lv * 8) {
					if (sd)
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				if (sd) {
					struct skill_condition req = skill_get_requirement(sd,skill_id,skill_lv);

					status_zap(src,0,req.sp); //Consume sp only if succeeded [Inkfish]
				}
				card = skill_tarotcard(src,bl,skill_id,skill_lv,tick); //Actual effect is executed here
				clif_specialeffect((card == 6 ? src : bl),EF_TAROTCARD1 + card - 1,AREA);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case SL_ALCHEMIST:
		case SL_ASSASIN:
		case SL_BARDDANCER:
		case SL_BLACKSMITH:
		case SL_CRUSADER:
		case SL_HUNTER:
		case SL_KNIGHT:
		case SL_MONK:
		case SL_PRIEST:
		case SL_ROGUE:
		case SL_SAGE:
		case SL_SOULLINKER:
		case SL_STAR:
		case SL_SUPERNOVICE:
		case SL_WIZARD:
		case SL_DEATHKNIGHT:
		case SL_COLLECTOR:
		case SL_NINJA:
		case SL_GUNNER:
			if (tsc && (tsc->data[SC_SOULGOLEM] || tsc->data[SC_SOULSHADOW] ||
				tsc->data[SC_SOULFALCON] || tsc->data[SC_SOULFAIRY])) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			//NOTE: here, 'type' has the value of the associated MAPID, not of the SC_SPIRIT constant
			if (dstsd && ((dstsd->class_&MAPID_UPPERMASK) == type ||
				(skill_id == SL_NINJA && (dstsd->class_&MAPID_UPPERMASK) == MAPID_KAGEROUOBORO) ||
				(skill_id == SL_GUNNER && (dstsd->class_&MAPID_UPPERMASK) == MAPID_REBELLION)))
			{
				if (skill_id == SL_SUPERNOVICE && dstsd->die_counter && !(rnd()%100)) { //Erase death count 1% of the casts
					dstsd->die_counter = 0;
					pc_setglobalreg(dstsd,PCDIECOUNTER_VAR,0);
					clif_specialeffect(bl,EF_ANGEL2,AREA);
					//SC_SPIRIT invokes status_calc_pc for us
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,
					sc_start4(src,bl,SC_SPIRIT,100,skill_lv,skill_id,0,0,skill_get_time(skill_id,skill_lv)));
				sc_start(src,src,SC_SMA,100,skill_lv,skill_get_time(SL_SMA,skill_lv));
			} else if (sd)
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			break;

		case SL_HIGH:
			if (tsc && (tsc->data[SC_SOULGOLEM] || tsc->data[SC_SOULSHADOW] ||
				tsc->data[SC_SOULFALCON] || tsc->data[SC_SOULFAIRY])) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			if (dstsd && !(dstsd->class_&JOBL_2) && (dstsd->class_&JOBL_UPPER) && dstsd->status.base_level < 70) {
				clif_skill_nodamage(src,bl,skill_id,skill_lv,
					sc_start4(src,bl,type,100,skill_lv,skill_id,0,0,skill_get_time(skill_id,skill_lv)));
				sc_start(src,src,SC_SMA,100,skill_lv,skill_get_time(SL_SMA,skill_lv));
			} else if (sd)
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			break;

		case SL_SWOO:
			if (tsce) {
				if (sd)
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				status_change_start(src,src,SC_STUN,10000,skill_lv,0,0,0,10000,SCFLAG_FIXEDRATE);
				status_change_end(bl,SC_SWOO,INVALID_TIMER);
				break;
			}
		//Fall through
		case SL_SKA: //[marquis007]
		case SL_SKE:
			if (dstmd && (dstmd->mob_id == MOBID_EMPERIUM || status_get_class_(bl) == CLASS_BATTLEFIELD)) {
				map_freeblock_unlock();
				return 1;
			}
			if (!battle_config.allow_es_magic_pc && bl->type != BL_MOB) {
				if (sd) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					skill_consume_requirement(sd,skill_id,skill_lv,1);
				}
				status_change_start(src,src,SC_STUN,10000,skill_lv,0,0,0,500,SCFLAG_FIXEDTICK|SCFLAG_FIXEDRATE);
				map_freeblock_unlock();
				return 1;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			if (skill_id == SL_SKE)
				sc_start(src,src,SC_SMA,100,skill_lv,skill_get_time(SL_SMA,skill_lv));
			break;

		//New guild skills [Celest]
		case GD_BATTLEORDER:
		case GD_REGENERATION:
		case GD_RESTORE:
			if (flag&1) {
				if (status_get_guild_id(src) == status_get_guild_id(bl)) {
					if (skill_id == GD_RESTORE)
						clif_skill_nodamage(src,bl,AL_HEAL,status_percent_heal(bl,90,90),1);
					else
						sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
				}
			} else if (status_get_guild_id(src)) {
				map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_PC,src,
					skill_id,skill_lv,tick,flag|BCT_GUILD|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				if (sd)
					guild_block_skill(sd,skill_get_time2(skill_id,skill_lv));
			}
			break;

		case GD_EMERGENCYCALL:
		case GD_ITEMEMERGENCYCALL:
			{ //I don't know if it actually summons in a circle, but oh well
				int8 dx[9] = {-1, 1, 0, 0,-1, 1,-1, 1, 0};
				int8 dy[9] = { 0, 0, 1,-1, 1,-1,-1, 1, 0};
				uint8 j = 0, calls = 0, called = 0;
				struct guild *g = (sd ? sd->guild : guild_search(status_get_guild_id(src)));

				if (!g)
					break;
				if (skill_id == GD_ITEMEMERGENCYCALL) {
					switch (skill_lv) {
						case 1: calls = 7; break;
						case 2: calls = 12; break;
						case 3: calls = 20; break;
						default: calls = 0; break;
					}
				}
				for (i = 0; i < g->max_member && (!calls || (calls && called < calls)); i++, j++) {
					if (j > 8)
						j = 0;
					if ((dstsd = g->member[i].sd) && sd != dstsd && !dstsd->state.autotrade && !pc_isdead(dstsd)) {
						if (mapdata[dstsd->bl.m].flag.nowarp && !map_flag_gvg2(dstsd->bl.m))
							continue;
						if (!pc_job_can_entermap((enum e_job)dstsd->status.class_,src->m,dstsd->group_level))
							continue;
						if (map_getcell(src->m,src->x+dx[j],src->y + dy[j],CELL_CHKNOREACH))
							dx[j] = dy[j] = 0;
						if (!pc_setpos(dstsd,map_id2index(src->m),src->x + dx[j],src->y + dy[j],CLR_RESPAWN))
							called++;
					}
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				if (sd)
					guild_block_skill(sd,skill_get_time2(skill_id,skill_lv));
			}
			break;

		case SG_FEEL:
			if (sd) { //AuronX reported you CAN memorize the same map as all three [Skotlex]
				if (!sd->feel_map[skill_lv - 1].index)
					clif_feel_req(sd->fd,sd,skill_lv);
				else
					clif_feel_info(sd,skill_lv - 1,1);
			}
			break;

		case SG_HATE:
			if (sd && !pc_set_hate_mob(sd,skill_lv - 1,bl)) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case GS_GLITTERING:
			if (sd) {
				if (rnd()%100 < 20 + 10 * skill_lv)
					pc_addspiritball(sd,skill_get_time(skill_id,skill_lv),10);
				else if (sd->spiritball > 0 && !pc_checkskill(sd,RL_RICHS_COIN))
					pc_delspiritball(sd,1,0);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case GS_CRACKER:
			i = max(65 - 5 * distance_bl(src,bl),30); //Base rate
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,i,skill_lv,skill_get_time2(skill_id,skill_lv)));
			break;

		case AM_CALLHOMUN: //[orn]
			if (sd && !hom_call(sd)) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case AM_REST:
			if (sd && !hom_vaporize(sd,HOM_ST_REST)) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case HAMI_CASTLE: //[orn]
			if (rnd()%100 < 20 * skill_lv) {
				struct block_list *m_bl = battle_get_master(src);
				int x = src->x, y = src->y;

				if (m_bl && !unit_blown_immune(src,0x1) && unit_movepos(src,m_bl->x,m_bl->y,0,false)) { //Move source
					clif_skill_nodamage(src,src,skill_id,skill_lv,1);
					clif_blown(src,m_bl);
					if (unit_movepos(m_bl,x,y,0,false)) { //Move target
						clif_skill_nodamage(m_bl,m_bl,skill_id,skill_lv,1);
						clif_blown(m_bl,m_bl);
					}
					map_foreachinallrange(unit_changetarget,src,AREA_SIZE,BL_MOB,m_bl,src); //Only affect monsters
				}
			} else if (hd && hd->master) //Failed
				clif_skill_fail(hd->master,skill_id,USESKILL_FAIL_LEVEL,0,0);
			else if (sd)
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			break;

		case HVAN_CHAOTIC: { //[orn]
				static const int per[5][2] = {{20,50},{50,60},{25,75},{60,64},{34,67}};
				int r = rnd()%100;

				i = (skill_lv - 1)%5;
				if (r < per[i][0]) //Self
					bl = src;
				else if(r < per[i][1]) //Master
					bl = battle_get_master(src);
				else //Enemy
					bl = map_id2bl(battle_gettarget(src));
				if (!bl)
					bl = src;
				i = skill_calc_heal(src,bl,skill_id,1 + rnd()%skill_lv,true);
				status_heal(bl,i,0,0);
				clif_skill_nodamage(src,bl,AL_HEAL,i,1);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case NPC_DRAGONFEAR:
			if( flag&1 ) {
				const enum sc_type sc[] = { SC_STUN,SC_SILENCE,SC_CONFUSION,SC_BLEEDING };
				int j;

				j = i = rnd()%ARRAYLENGTH(sc);
				while( !sc_start(src,bl,sc[i],100,skill_lv,skill_get_time2(skill_id,i + 1)) ) {
					i++;
					if( i == ARRAYLENGTH(sc) )
						i = 0;
					if( i == j )
						break;
				}
				break;
			}
		//Fall through
		case NPC_WIDEBLEEDING:
		case NPC_WIDECONFUSE:
		case NPC_WIDECURSE:
		case NPC_WIDEFREEZE:
		case NPC_WIDESLEEP:
		case NPC_WIDESILENCE:
		case NPC_WIDESTONE:
		case NPC_WIDESTUN:
		case NPC_SLOWCAST:
		case NPC_WIDEHELLDIGNITY:
		case NPC_WIDEHEALTHFEAR:
		case NPC_WIDEBODYBURNNING:
		case NPC_WIDEFROSTMISTY:
		case NPC_WIDECOLD:
		case NPC_WIDE_DEEP_SLEEP:
		case NPC_WIDESIREN:
		case NPC_WIDEWEB:
			if( flag&1 ) {
				switch( type ) {
					case SC_VOICEOFSIREN:
						sc_start2(src,bl,type,100,skill_lv,src->id,skill_get_time2(skill_id,skill_lv));
						break;
					default:
						sc_start(src,bl,type,100,skill_lv,skill_get_time2(skill_id,skill_lv));
						break;
				}
			} else {
				skill_area_temp[2] = 0; //For SD_PREAMBLE
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_PREAMBLE|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case NPC_WIDESOULDRAIN:
			if( flag&1 )
				status_percent_damage(src,bl,0,((skill_lv - 1)%5 + 1) * 20,false);
			else {
				skill_area_temp[2] = 0; //For SD_PREAMBLE
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_PREAMBLE|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case NPC_FIRESTORM: {
				int sflag = flag;

				if( skill_lv > 1 )
					sflag |= 4;
				map_foreachinshootrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),splash_target(src),src,
					skill_id,skill_lv,tick,sflag|BCT_ENEMY|SD_ANIMATION|1,skill_castend_damage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case ALL_PARTYFLEE:
			if( !sd || !sd->status.party_id || (flag&1) )
				clif_skill_nodamage(src,bl,skill_id,skill_lv,sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			else if( sd )
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case NPC_TALK:
		case ALL_WEWISH:
		case ALL_CATCRY:
		case ALL_DREAM_SUMMERNIGHT:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case ALL_BUYING_STORE:
			if( sd ) //Players only, skill allows 5 buying slots
				clif_skill_nodamage(src,bl,skill_id,skill_lv,buyingstore_setup(sd,MAX_BUYINGSTORE_SLOTS) ? 0 : 1);
			break;

		case RK_DEATHBOUND:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			sc_start(src,bl,SC_TELEPORT_FIXEDCASTINGDELAY,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;

		case RK_DRAGONHOWLING:
			if( flag&1 )
				sc_start(src,bl,type,50 + 6 * skill_lv,skill_lv,skill_get_time(skill_id,skill_lv));
			else {
				map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case RK_PHANTOMTHRUST:
			if( (dstmd && (dstmd->mob_id == MOBID_EMPERIUM || status_get_class_(bl) == CLASS_BATTLEFIELD)) ||
				(dstsd && battle_check_target(src,bl,BCT_PARTY) <= 0) ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				map_freeblock_unlock();
				return 1;
			}
			if( battle_check_target(src,bl,BCT_ENEMY) > 0 )
				skill_attack(BF_WEAPON,src,src,bl,skill_id,skill_lv,tick,flag);
			unit_setdir(src,map_calc_dir(src,bl->x,bl->y));
			skill_blown(src,bl,distance_bl(src,bl) - 1,unit_getdir(src),0);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case RK_MILLENNIUMSHIELD:
		case RK_REFRESH:
		case RK_GIANTGROWTH:
		case RK_STONEHARDSKIN:
		case RK_VITALITYACTIVATION:
		case RK_ABUNDANCE:
			if( skill_id == RK_MILLENNIUMSHIELD ) {
				if( sd )
					skill_generate_millenniumshield(sd,skill_id,skill_lv);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			} else
				clif_skill_nodamage(src,bl,skill_id,skill_lv,sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			sc_start2(src,bl,SC_LUXANIMA,100,skill_lv,status_skill2sc(skill_id),skill_get_time(skill_id,skill_lv));
			break;

		case RK_FIGHTINGSPIRIT: {
				int aspd_bonus = (sd ? pc_checkskill(sd,RK_RUNEMASTERY) : 10) * 4;

				if( !sd || !sd->status.party_id || (flag&1) ) {
					if( skill_area_temp[5] > 1 )
						sc_start2(src,bl,type,100,skill_area_temp[5],aspd_bonus,skill_get_time(skill_id,skill_lv));
					else
						sc_start2(src,bl,type,100,77,aspd_bonus,skill_get_time(skill_id,skill_lv));
					if( bl->id != src->id )
						sc_start(src,bl,type,100,skill_area_temp[5] / 2,skill_get_time(skill_id,skill_lv));
				} else {
					skill_area_temp[5] = party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,
						skill_id,skill_lv,tick,BCT_PARTY,skill_area_sub_count);
					skill_area_temp[5] = 70 + 7 * skill_area_temp[5]; //ATK Bonus
					party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,
						skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
					clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				}
			}
			break;

		case RK_LUXANIMA:
			if( flag&1 ) {
				if( bl->id == src->id )
					break;
				if( skill_area_temp[5]&0x10 && dstsd )
					skill_generate_millenniumshield(dstsd,skill_id,skill_lv);
				if( skill_area_temp[5]&0x20 )
					sc_start(src,bl,SC_REFRESH,100,skill_lv,skill_get_time(skill_id,skill_lv));
				if( skill_area_temp[5]&0x40 )
					sc_start(src,bl,SC_GIANTGROWTH,100,skill_lv,skill_get_time(skill_id,skill_lv));
				if( skill_area_temp[5]&0x80 )
					sc_start(src,bl,SC_STONEHARDSKIN,100,skill_lv,skill_get_time(skill_id,skill_lv));
				if( skill_area_temp[5]&0x100 )
					sc_start(src,bl,SC_VITALITYACTIVATION,100,skill_lv,skill_get_time(skill_id,skill_lv));
				if( skill_area_temp[5]&0x200 )
					sc_start(src,bl,SC_ABUNDANCE,100,skill_lv,skill_get_time(skill_id,skill_lv));
				status_change_clear_buffs(bl,SCCB_LUXANIMA,0); //For bonus_script
			} else {
				if( sd && sd->status.party_id && tsce ) {
					switch( tsce->val2 ) {
						case SC_MILLENNIUMSHIELD:
							skill_area_temp[5] = 0x10;
							break;
						case SC_REFRESH:
							skill_area_temp[5] = 0x20;
							break;
						case SC_GIANTGROWTH:
							skill_area_temp[5] = 0x40;
							break;
						case SC_STONEHARDSKIN:
							skill_area_temp[5] = 0x80;
							break;
						case SC_VITALITYACTIVATION:
							skill_area_temp[5] = 0x100;
							break;
						case SC_ABUNDANCE:
							skill_area_temp[5] = 0x200;
							break;
					}
					if( tsce->val2 == SC_MILLENNIUMSHIELD )
						pc_delshieldball(sd,sd->shieldball,0);
					else
						status_change_end(bl,(sc_type)tsce->val2,INVALID_TIMER);
					status_change_end(bl,SC_LUXANIMA,INVALID_TIMER);
					party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,
						skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case GC_ROLLINGCUTTER: {
				short count = 1;

				if( tsc && tsc->data[SC_ROLLINGCUTTER] ) { //Every time the skill is casted the status change is reseted adding a counter
					count += (short)tsc->data[SC_ROLLINGCUTTER]->val1;
					count = min(count,10);
					status_change_end(bl,SC_ROLLINGCUTTER,INVALID_TIMER);
				}
				sc_start(src,bl,SC_ROLLINGCUTTER,100,count,skill_get_time(skill_id,skill_lv));
				skill_area_temp[2] = 0;
				map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_SPLASH|SD_PREAMBLE|1,skill_castend_damage_id);
				clif_skill_nodamage(src,src,skill_id,skill_lv,1);
			}
			break;

		case GC_WEAPONBLOCKING:
			if( tsc && tsc->data[SC_WEAPONBLOCKING] )
				status_change_end(bl,SC_WEAPONBLOCKING,INVALID_TIMER);
			else
				sc_start(src,bl,SC_WEAPONBLOCKING,100,skill_lv,skill_get_time(skill_id,skill_lv));
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case GC_CREATENEWPOISON:
			if( sd )
				clif_skill_produce_mix_list(sd,skill_id,25);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case GC_POISONINGWEAPON:
			if( sd )
				clif_poison_list(sd,skill_lv);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case GC_ANTIDOTE:
			status_change_end(bl,SC_PARALYSE,INVALID_TIMER);
			status_change_end(bl,SC_PYREXIA,INVALID_TIMER);
			status_change_end(bl,SC_DEATHHURT,INVALID_TIMER);
			status_change_end(bl,SC_LEECHESEND,INVALID_TIMER);
			status_change_end(bl,SC_VENOMBLEED,INVALID_TIMER);
			status_change_end(bl,SC_MAGICMUSHROOM,INVALID_TIMER);
			status_change_end(bl,SC_TOXIN,INVALID_TIMER);
			status_change_end(bl,SC_OBLIVIONCURSE,INVALID_TIMER);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case AB_ANCILLA:
			if( sd )
				skill_produce_mix(sd,skill_id,ITEMID_ANCILLA,0,0,0,0,1);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case AB_CLEMENTIA:
		case AB_CANTO:
			if( !sd || !sd->status.party_id || (flag&1) ) {
				i = (sd ? (skill_id == AB_CLEMENTIA ? pc_checkskill(sd,AL_BLESSING) : pc_checkskill(sd,AL_INCAGI)) : 10);
				clif_skill_nodamage(bl,bl,skill_id,-1,
					sc_start(src,bl,type,100,i + status_get_job_lv(src) / 10,skill_get_time(skill_id,skill_lv)));
			} else if( sd )
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case AB_PRAEFATIO:
		case AB_RENOVATIO:
			if( !sd || !sd->status.party_id || (flag&1) ) {
				if( skill_id == AB_PRAEFATIO )
					clif_skill_nodamage(bl,bl,skill_id,-1,sc_start4(src,bl,type,100,skill_lv,0,0,partybonus,skill_get_time(skill_id,skill_lv)));
				else
					clif_skill_nodamage(bl,bl,skill_id,-1,sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			} else if( sd )
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case AB_CHEAL:
			if( !sd || !sd->status.party_id || (flag&1) ) {
				if( dstsd && pc_ismadogear(dstsd) )
					break;
				if( !battle_check_undead(tstatus->race,tstatus->def_ele) && !(tsc && (tsc->data[SC_BERSERK] || tsc->data[SC_SATURDAYNIGHTFEVER])) ) {
					i = skill_calc_heal(src,bl,AL_HEAL,(sd ? pc_checkskill(sd,AL_HEAL) : 10),true);
					if( partybonus )
						i += i / 100 * partybonus * 10 / 4;
					if( status_isimmune(bl) )
						i = 0;
					status_heal(bl,i,0,0);
					status_change_end(bl,SC_BITESCAR,INVALID_TIMER);
					clif_skill_nodamage(src,bl,skill_id,i,1);
				}
			} else if( sd )
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case AB_ORATIO:
			if( flag&1 )
				sc_start(src,bl,type,40 + 5 * skill_lv,skill_lv,skill_get_time(skill_id,skill_lv));
			else {
				map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case AB_LAUDAAGNUS:
			if( !sd || !sd->status.party_id || (flag&1) ) {
				if( tsc && (tsc->data[SC_FREEZE] || tsc->data[SC_STONE] || tsc->data[SC_BLIND] ||
					tsc->data[SC_BURNING] || tsc->data[SC_FREEZING] || tsc->data[SC_CRYSTALIZE])) {
					if( rnd()%100 >= 60 + 10 * skill_lv )
						break;
					status_change_end(bl,SC_FREEZE,INVALID_TIMER);
					status_change_end(bl,SC_STONE,INVALID_TIMER);
					status_change_end(bl,SC_BLIND,INVALID_TIMER);
					status_change_end(bl,SC_BURNING,INVALID_TIMER);
					status_change_end(bl,SC_FREEZING,INVALID_TIMER);
					status_change_end(bl,SC_CRYSTALIZE,INVALID_TIMER);
				} else //Success rate only applies to the curing effect and not stat bonus. Bonus status only applies to non infected targets
					clif_skill_nodamage(bl,bl,skill_id,-1,sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			} else if( sd )
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case AB_LAUDARAMUS:
			if( !sd || !sd->status.party_id || (flag&1) ) {
				if( tsc && (tsc->data[SC_SLEEP] || tsc->data[SC_STUN] ||
					tsc->data[SC_MANDRAGORA] || tsc->data[SC_SILENCE] || tsc->data[SC_DEEPSLEEP]) ) {
					if( rnd()%100 >= 60 + 10 * skill_lv )
						break;
					status_change_end(bl,SC_SLEEP,INVALID_TIMER);
					status_change_end(bl,SC_STUN,INVALID_TIMER);
					status_change_end(bl,SC_MANDRAGORA,INVALID_TIMER);
					status_change_end(bl,SC_SILENCE,INVALID_TIMER);
					status_change_end(bl,SC_DEEPSLEEP,INVALID_TIMER);
				} else
					clif_skill_nodamage(bl,bl,skill_id,-1,sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			} else if( sd )
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case AB_CLEARANCE:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			if( rnd()%100 >= 60 + 8 * skill_lv || status_isimmune(bl) || !(tsc && tsc->count) )
				break;
			if( dstsd ) //Remove bonus_script by Clearance
				pc_bonus_script_clear(dstsd,BSF_REM_ON_CLEARANCE);
			for( i = 0; i < SC_MAX; i++ ) {
				if( !tsc->data[i] )
					continue;
				if( !(status_get_sc_type((sc_type)i)&SC_REM_CLEARANCE) )
					continue;
				switch( i ) {
					case SC_SILENCE:
						if( tsc->data[i]->val4 )
							continue;
						break;
					case SC_WHISTLE:
					case SC_ASSNCROS:
					case SC_POEMBRAGI:
					case SC_APPLEIDUN:
					case SC_HUMMING:
					case SC_DONTFORGETME:
					case SC_FORTUNE:
					case SC_SERVICE4U:
						if( !battle_config.dispel_song || !tsc->data[i]->val4 )
							continue;
						break;
					case SC_ASSUMPTIO:
						if( bl->type == BL_MOB )
							continue;
						break;
					case SC_BERSERK:
					case SC_SATURDAYNIGHTFEVER:
						tsc->data[i]->val2 = 0;
						break;
				}
				status_change_end(bl,(sc_type)i,INVALID_TIMER);
			}
			break;

		case AB_SILENTIUM:
			map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
				PR_LEXDIVINA,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case AB_VITUPERATUM:
			if( flag&1 )
				clif_skill_nodamage(src,bl,skill_id,skill_lv,sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			else {
				map_foreachinrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case AB_CONVENIO:
			if( sd ) {
				struct party_data *p = party_search(sd->status.party_id);
				int count = 0;

				//Only usable in party
				if( !p ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				//Only usable as party leader.
				ARR_FIND(0, MAX_PARTY, i, p->data[i].sd == sd);
				if( i == MAX_PARTY || !p->party.member[i].leader ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				//Do the teleport part
				for( i = 0; i < MAX_PARTY; ++i ) {
					struct map_session_data *pl_sd = p->data[i].sd;

					if( !pl_sd || pl_sd == sd || pl_sd->status.party_id != p->party.party_id || pc_isdead(pl_sd) || sd->bl.m != pl_sd->bl.m )
						continue;
					if( !(mapdata[sd->bl.m].flag.noteleport || mapdata[sd->bl.m].flag.pvp || mapdata[sd->bl.m].flag.battleground || map_flag_gvg2(sd->bl.m)) ) {
						pc_setpos(pl_sd,map_id2index(sd->bl.m),sd->bl.x,sd->bl.y,CLR_TELEPORT);
						count++;
					}
				}
				if( !count )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			}
			break;

		case WL_WHITEIMPRISON: {
				int rate = 23 + (1 - skill_lv) * status_get_job_lv(src) / 7; //Job level bonus [exneval]
				int time;

				if( bl->id == src->id ) {
					rate = 100; //On self, 100%
					time = 5000;
				} else {
					if( bl->type == BL_PC ) {
						rate += 20 + 10 * skill_lv; //On players, (20 + 10 * Skill Level)%
						time = skill_get_time(skill_id,skill_lv);
					} else {
						rate += 40 + 10 * skill_lv; //On monsters, (40 + 10 * Skill Level)%
						time = skill_get_time2(skill_id,skill_lv);
					}
				}
				if( !(i = sc_start2(src,bl,type,rate,skill_lv,src->id,time)) ) {
					if( sd )
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,i);
			}
			break;

		case WL_MARSHOFABYSS:
			if( !(i = sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv))) ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_TOTARGET,0,0);
				map_freeblock_unlock();
				return 1;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,i);
			break;

		case WL_SIENNAEXECRATE:
			if( status_isimmune(bl) || !tsc )
				break; //Doesn't send failure packet if it fails on defense
			if( flag&1 ) {
				if( skill_area_temp[1] == bl->id )
					break; //Already work on this target
				if( tsc->data[type] )
					status_change_end(bl,type,INVALID_TIMER);
				else
					status_change_start(src,bl,type,10000,skill_lv,0,0,0,skill_get_time(skill_id,skill_lv),SCFLAG_FIXEDTICK);
			} else {
				int rate = (skill_lv - 1) * status_get_job_lv(src) / 15 - 2; //Job level bonus [exneval]

				rate += 45 + 5 * skill_lv;
				if( rnd()%100 < rate ) { //Success on first target
					if( !tsc->data[type] )
						rate = status_change_start(src,bl,type,10000,skill_lv,0,0,0,skill_get_time(skill_id,skill_lv),SCFLAG_FIXEDTICK);
					else {
						rate = 1;
						status_change_end(bl,type,INVALID_TIMER);
					}
					if( rate ) {
						skill_area_temp[1] = bl->id;
						map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
						clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
					}
				} else if( sd ) { //Failure on rate
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					flag |= 1;
				}
			}
			break;

		case WL_STASIS:
			if( flag&1 )
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
			else {
				map_foreachinrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
					skill_id,skill_lv,tick,(map_flag_vs(src->m) ? BCT_ALL : BCT_ENEMY|BCT_SELF)|flag|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case NPC_DANCINGBLADE:
			skill_area_temp[1] = bl->id;
		//Fall through
		case WL_CHAINLIGHTNING:
			skill_addtimerskill(src,tick + 150,bl->id,0,0,skill_id + 1,skill_lv,0,0);
			break;

		case WL_SUMMONFB:
		case WL_SUMMONBL:
		case WL_SUMMONWB:
		case WL_SUMMONSTONE:
			for( i = SC_SPHERE_1; i <= SC_SPHERE_5; i++ ) {
				if( !(tsc && tsc->data[i]) ) { //Officially it doesn't work like a stack
					int ele = WLS_FIRE + (skill_id - WL_SUMMONFB) - (skill_id == WL_SUMMONSTONE ? 4 : 0);

					clif_skill_nodamage(src,bl,skill_id,skill_lv,
						sc_start2(src,bl,(sc_type)i,100,ele,skill_lv,skill_get_time(skill_id,skill_lv)));
					break;
				}
			}
			break;

		case WL_READING_SB:
			if( sd ) {
				for( i = SC_SPELLBOOK1; i <= SC_MAXSPELLBOOK; i++ ) {
					if( !(tsc && tsc->data[i]) )
						break;
				}
				if( i == SC_MAXSPELLBOOK ) {
					clif_skill_fail(sd,WL_READING_SB,USESKILL_FAIL_SPELLBOOK_READING,0,0);
					break;
				}
				sc_start(src,bl,SC_STOP,100,skill_lv,-1); //Can't move while selecting a spellbook
				clif_spellbook_list(sd);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case RA_WUGMASTERY:
			if( sd ) {
				if( !pc_iswug(sd) && !pc_isridingwug(sd) )
					pc_setoption(sd,sd->sc.option|OPTION_WUG);
				else if( !pc_isridingwug(sd) )
					pc_setoption(sd,sd->sc.option&~OPTION_WUG);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case RA_WUGRIDER:
			if( sd ) {
				if( pc_iswug(sd) && !pc_isridingwug(sd) ) {
					pc_setoption(sd,sd->sc.option&~OPTION_WUG);
					pc_setoption(sd,sd->sc.option|OPTION_WUGRIDER);
				} else if( pc_isridingwug(sd) ) {
					pc_setoption(sd,sd->sc.option&~OPTION_WUGRIDER);
					pc_setoption(sd,sd->sc.option|OPTION_WUG);
				}
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case RA_WUGDASH:
			if( tsce )
				clif_skill_nodamage(src,bl,skill_id,skill_lv,status_change_end(bl,type,INVALID_TIMER));
			else {
				clif_skill_nodamage(src,bl,skill_id,skill_lv,
					sc_start4(src,bl,type,100,skill_lv,unit_getdir(bl),0,0,0));
				clif_walkok(sd);
			}
			break;

		case RA_SENSITIVEKEEN:
			map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_CHAR|BL_SKILL,src,
				skill_id,skill_lv,tick,flag|BCT_ENEMY,skill_castend_damage_id);
			clif_skill_damage(src,src,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case NC_F_SIDESLIDE:
		case NC_B_SIDESLIDE:
			{
				int dir = (skill_id == NC_F_SIDESLIDE ? (unit_getdir(src) + 4)%8 : unit_getdir(src));

				skill_blown(src,bl,skill_get_blewcount(skill_id,skill_lv),dir,2);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case NC_SELFDESTRUCTION:
			if( sd ) {
				if( pc_ismadogear(sd) )
					pc_setmadogear(sd,0);
				skill_area_temp[1] = 0;
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR|BL_SKILL,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_SPLASH|1,skill_castend_damage_id);
				status_set_sp(src,0,0);
				skill_clear_unitgroup(src);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case NC_EMERGENCYCOOL:
			if( sd ) {
				struct skill_condition req = skill_get_requirement(sd,skill_id,skill_lv);
				int16 limit[] = { -45,-75,-105 };

				for( i = 0; i < req.eqItem_count; i++ ) {
					if( pc_search_inventory(sd,req.eqItem[i]) != INDEX_NOT_FOUND )
						break;
				}
				pc_overheat(sd,limit[i]);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case NC_INFRAREDSCAN:
			if( flag&1 ) {
				if( tsc && ((tsc->option&(OPTION_HIDE|OPTION_CLOAK)) || tsc->data[SC_CAMOUFLAGE] || tsc->data[SC__SHADOWFORM]) ) {
					status_change_end(bl,SC_HIDING,INVALID_TIMER);
					status_change_end(bl,SC_CLOAKING,INVALID_TIMER);
					status_change_end(bl,SC_CAMOUFLAGE,INVALID_TIMER);
					status_change_end(bl,SC_CLOAKINGEXCEED,INVALID_TIMER);
					status_change_end(bl,SC_NEWMOON,INVALID_TIMER);
					if( tsc->data[SC__SHADOWFORM] && rnd()%100 < 100 - tsc->data[SC__SHADOWFORM]->val1 * 10 )
						status_change_end(bl,SC__SHADOWFORM,INVALID_TIMER);
					sc_start(src,bl,SC_INFRAREDSCAN,100,skill_lv,skill_get_time(skill_id,skill_lv));
				}
			} else {
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),splash_target(src),src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
				clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			}
			break;

		case NC_ANALYZE:
			sc_start(src,bl,type,30 + 12 * skill_lv,skill_lv,skill_get_time(skill_id,skill_lv));
			clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			break;

		case NC_MAGNETICFIELD:
			if( flag&1 )
				sc_start2(src,bl,SC_MAGNETICFIELD,100,skill_lv,src->id,skill_get_time(skill_id,skill_lv));
			else {
				if( map_flag_vs(src->m) ) //Doesn't affect the caster in non-PVP maps [exneval]
					sc_start2(src,bl,type,100,skill_lv,src->id,skill_get_time(skill_id,skill_lv));
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),splash_target(src),src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_SPLASH|1,skill_castend_nodamage_id);
				clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			}
			break;

		case NC_REPAIR:
			if( sd ) {
				uint8 percent = 0;
				int heal;

				if( !dstsd || !pc_ismadogear(dstsd) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_MADOGEAR_RIDE,0,0);
					break;
				}
				switch( skill_lv ) {
					case 1: percent = 4; break;
					case 2: percent = 7; break;
					case 3: percent = 13; break;
					case 4: percent = 17; break;
					case 5: percent = 23; break;
				}
				heal = tstatus->max_hp * percent / 100;
				status_heal(bl,heal,0,0);
				clif_skill_nodamage(src,bl,AL_HEAL,heal,1);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case NC_DISJOINT:
			if( bl->type != BL_MOB )
				break;
			if( (md = map_id2md(bl->id)) && md->mob_id >= MOBID_SILVERSNIPER && md->mob_id <= MOBID_MAGICDECOY_WIND )
				status_kill(bl);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SC_AUTOSHADOWSPELL:
			if( sd ) {
				if( (sd->reproduceskill_idx >= 0 && sd->status.skill[sd->reproduceskill_idx].id) ||
					(sd->cloneskill_idx >= 0 && sd->status.skill[sd->cloneskill_idx].id) ) {
					//The skill_lv is stored in val1 used in skill_select_menu to determine the used skill lvl [Xazax]
					sc_start(src,src,SC_STOP,100,skill_lv,-1);
					clif_autoshadowspell_list(sd);
				} else {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_IMITATION_SKILL_NONE,0,0);
					break;
				}
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SC_SHADOWFORM:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start4(src,src,type,100,skill_lv,bl->id,4 + skill_lv,0,skill_get_time(skill_id,skill_lv)));
			break;

		case SC_BODYPAINT:
			if( flag&1 ) {
				status_change_end(bl,SC_HIDING,INVALID_TIMER);
				status_change_end(bl,SC_CLOAKING,INVALID_TIMER);
				status_change_end(bl,SC_CAMOUFLAGE,INVALID_TIMER);
				status_change_end(bl,SC_CLOAKINGEXCEED,INVALID_TIMER);
				status_change_end(bl,SC_NEWMOON,INVALID_TIMER);
				if( tsc && tsc->data[SC__SHADOWFORM] && rnd()%100 < 100 - tsc->data[SC__SHADOWFORM]->val1 * 10 )
					status_change_end(bl,SC__SHADOWFORM,INVALID_TIMER);
				sc_start(src,bl,type,20 + 5 * skill_lv,skill_lv,skill_get_time(skill_id,skill_lv));
				sc_start(src,bl,SC_BLIND,53 + 2 * skill_lv,skill_lv,skill_get_time2(skill_id,skill_lv));
			} else {
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,0,1);
			}
			break;

		case SC_ENERVATION:
		case SC_GROOMY:
		case SC_IGNORANCE:
		case SC_LAZINESS:
		case SC_UNLUCKY:
		case SC_WEAKNESS:
			{
				int joblvbonus = 0, rate = 0;

				joblvbonus = status_get_job_lv(src);
				//First we set the success chance based on the caster's build which increases the chance
				rate = 10 * skill_lv + rnd_value(sstatus->dex / 12,sstatus->dex / 4) + joblvbonus + status_get_lv(src) / 10 -
				//Then we reduce the success chance based on the target's build
				rnd_value(tstatus->agi / 6,tstatus->agi / 3) - tstatus->luk / 10 - (dstsd ? (dstsd->max_weight / 10 - dstsd->weight / 10) / 100 : 0) - status_get_lv(bl) / 10;
				//Finally we set the minimum success chance cap based on the caster's skill level and DEX
				rate = cap_value(rate,skill_lv + sstatus->dex / 20,100);
				if( !(i = sc_start(src,bl,type,rate,skill_lv,skill_get_time(skill_id,skill_lv))) ) {
					skill_consume_requirement(sd,skill_id,skill_lv,1); //Consume SP even if failed
					map_freeblock_unlock();
					return 1;
				}
				if( tsc ) {
					//If the target was successfully inflected with the Ignorance status, drain some of the targets SP
					if( tsc->data[SC__IGNORANCE] && skill_id == SC_IGNORANCE ) {
							int sp = 100 * skill_lv;

							if( dstmd )
								sp = dstmd->level;
							if( !dstmd )
								status_zap(bl,0,sp);
							status_heal(src,0,sp / 2,3);
					}
					//If the target was successfully inflected with the Unlucky status, give 1 of 3 random status's
					//Targets in the Unlucky status will be affected by one of the 3 random status's reguardless of resistance
					if( tsc->data[SC__UNLUCKY] && skill_id == SC_UNLUCKY ) {
						switch( rnd()%skill_lv ) {
							case 0:
								status_change_start(src,bl,SC_POISON,10000,skill_lv,0,0,0,skill_get_time(skill_id,skill_lv),SCFLAG_FIXEDTICK|SCFLAG_FIXEDRATE);
								break;
							case 1:
								status_change_start(src,bl,SC_BLIND,10000,skill_lv,0,0,0,skill_get_time(skill_id,skill_lv),SCFLAG_FIXEDTICK|SCFLAG_FIXEDRATE);
								break;
							case 2:
								status_change_start(src,bl,SC_SILENCE,10000,skill_lv,0,0,0,skill_get_time(skill_id,skill_lv),SCFLAG_FIXEDTICK|SCFLAG_FIXEDRATE);
								break;
						}
					}
					clif_skill_nodamage(src,bl,skill_id,-1,i);
				}
			}
			break;

		case LG_TRAMPLE:
			if( flag&1 )
				status_change_end(bl,SC_SV_ROOTTWIST,INVALID_TIMER);
			else {
				if( rnd()%100 < 25 + 25 * skill_lv ) {
					map_foreachinallrange(skill_destroy_trap,bl,skill_get_splash(skill_id,skill_lv),BL_SKILL,src,tick);
					map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),splash_target(src),src,
						skill_id,skill_lv,tick,flag|BCT_ALL|SD_SPLASH|1,skill_castend_nodamage_id);
				}
				clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			}
			break;

		case LG_REFLECTDAMAGE:
			if( tsc && tsc->data[type] )
				status_change_end(bl,type,INVALID_TIMER);
			else
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case LG_SHIELDSPELL:
			if( sd ) {
				short effect_number = rnd()%3 + 1; //Effect number, each level has 3 unique effects thats randomly picked from
				short splash_range = 0; //Splash AoE, used for splash AoE ATK/MATK and Lex Divina
				short index = sd->equip_index[EQI_HAND_L], shield_def = 0, shield_mdef = 0, shield_refine = 0;
				struct item_data *shield_data = NULL;

				if( index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_ARMOR )
					shield_data = sd->inventory_data[index];
				if( !shield_data || shield_data->type != IT_ARMOR ) //Group with 'skill_unconditional' gets these as default
					shield_def = shield_mdef = shield_refine = 10;
				else {
					shield_def = shield_data->def;
					shield_mdef = sd->bonus.shieldmdef;
					shield_refine = sd->inventory.u.items_inventory[sd->equip_index[EQI_HAND_L]].refine;
				}
				switch( skill_lv ) {
					case 1: { //DEF Based
							if( effect_number == 1 ) {
#ifdef RENEWAL
								if( shield_def >= 0 && shield_def <= 40 )
#else
								if( shield_def >= 0 && shield_def <= 4 )
#endif
									splash_range = 1;
#ifdef RENEWAL
								else if( shield_def >= 41 && shield_def <= 80 )
#else
								else if( shield_def >= 5 && shield_def <= 8 )
#endif
									splash_range = 2;
								else
									splash_range = 3;
							}
							switch( effect_number ) {
								case 1: //Splash AoE ATK
									sc_start(src,bl,SC_SHIELDSPELL_DEF,100,effect_number,-1);
									clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
									map_foreachinallrange(skill_area_sub,src,splash_range,BL_CHAR,src,skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_damage_id);
									status_change_end(bl,SC_SHIELDSPELL_DEF,INVALID_TIMER);
									break;
								case 2: //% Damage Reflecting Increase
#ifdef RENEWAL
									sc_start2(src,bl,SC_SHIELDSPELL_DEF,100,effect_number,shield_def / 10,shield_def * 1000);
#else
									sc_start2(src,bl,SC_SHIELDSPELL_DEF,100,effect_number,shield_def,shield_def * 1000 * 10);
#endif
									break;
								case 3: //Equipment Attack Increase
#ifdef RENEWAL
									sc_start2(src,bl,SC_SHIELDSPELL_DEF,100,effect_number,shield_def,shield_def * 3000);
#else
									sc_start2(src,bl,SC_SHIELDSPELL_DEF,100,effect_number,shield_def * 10,shield_def * 3000 * 10);
#endif
									break;
							}
						}
						break;
					case 2: { //MDEF Based
							if( !shield_mdef )
								break; //Nothing should happen if the shield has no MDEF, not even displaying a message
							if( effect_number != 3 ) {
								if( shield_mdef >= 1 && shield_mdef <= 3 )
									splash_range = 1;
								else if( shield_mdef >= 4 && shield_mdef <= 5 )
									splash_range = 2;
								else
									splash_range = 3;
							}
							switch( effect_number ) {
								case 1: //Splash AoE MATK
									sc_start(src,bl,SC_SHIELDSPELL_MDEF,100,effect_number,-1);
									clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
									map_foreachinallrange(skill_area_sub,src,splash_range,BL_CHAR,src,skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_damage_id);
									status_change_end(bl,SC_SHIELDSPELL_MDEF,INVALID_TIMER);
									break;
								case 2: //Splash AoE Lex Divina
									shield_mdef = min(shield_mdef,10); //Level of Lex Divina to cast
									sc_start(src,bl,SC_SHIELDSPELL_MDEF,100,effect_number,-1);
									map_foreachinallrange(skill_area_sub,src,splash_range,BL_CHAR,src,PR_LEXDIVINA,shield_mdef,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
									status_change_end(bl,SC_SHIELDSPELL_MDEF,INVALID_TIMER);
									break;
								case 3: //Casts Magnificat
									if( sc_start(src,bl,SC_SHIELDSPELL_MDEF,100,effect_number,shield_mdef * 30000) )
										clif_skill_nodamage(src,bl,PR_MAGNIFICAT,skill_lv,sc_start(src,bl,SC_MAGNIFICAT,100,1,shield_mdef * 30000));
									break;
							}
						}
						break;
					case 3: //Refine Based
						if( !shield_refine )
							break;
						switch( effect_number ) {
							case 1: //Allows you to break armor at a 100% rate when you do damage
								sc_start(src,bl,SC_SHIELDSPELL_REF,100,effect_number,shield_refine * 30000);
								break;
							case 2: //Increases DEF and Status Effect resistance depending on Shield refine rate
#ifdef RENEWAL
								sc_start4(src,bl,SC_SHIELDSPELL_REF,100,effect_number,shield_refine * 10 * status_get_lv(src) / 100,(shield_refine * 2) + (sstatus->luk / 10),0,shield_refine * 20000);
#else
								sc_start4(src,bl,SC_SHIELDSPELL_REF,100,effect_number,shield_refine,(shield_refine * 2) + (sstatus->luk / 10),0,shield_refine * 20000);
#endif
								break;
							case 3: //Recovers HP depending on Shield refine rate
								sc_start(src,bl,SC_SHIELDSPELL_REF,100,effect_number,-1); //HP Recovery
								status_heal(bl,sstatus->max_hp * ((status_get_lv(src) / 10) + (shield_refine + 1)) / 100,0,2);
								status_change_end(bl,SC_SHIELDSPELL_REF,INVALID_TIMER);
								break;
						}
						break;
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case LG_PIETY:
			if( flag&1 )
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
			else {
				skill_area_temp[2] = 0;
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_PC,src,skill_id,skill_lv,tick,flag|SD_PREAMBLE|BCT_PARTY|BCT_SELF|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case LG_EARTHDRIVE: {
				int dummy = 1;

				i = skill_get_splash(skill_id,skill_lv);
				map_foreachinallarea(skill_cell_overlap,src->m,src->x-i,src->y-i,src->x+i,src->y+i,BL_SKILL,skill_id,&dummy,src);
				map_foreachinallrange(skill_area_sub,bl,i,BL_CHAR,src,skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_damage_id);
				clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			}
			break;

		case LG_INSPIRATION:
			if( sd && battle_config.exp_cost_inspiration )
				pc_lostexp(sd,umin(sd->status.base_exp,pc_nextbaseexp(sd) * battle_config.exp_cost_inspiration / 100),0); //1% penalty
			status_change_clear_buffs(bl,SCCB_BUFFS|SCCB_DEBUFFS|SCCB_REFRESH,0);
			clif_skill_nodamage(bl,src,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			break;

		case SR_SKYNETBLOW: {
				struct status_change *sc = status_get_sc(src);

				if( sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == SR_DRAGONCOMBO )
					flag |= 8;
				skill_area_temp[1] = 0;
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),splash_target(src),src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_SPLASH|1,skill_castend_damage_id);
				clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			}
			break;

		case SR_CURSEDCIRCLE:
			if( flag&1 ) {
				if( sc_start2(src,bl,type,100,skill_lv,src->id,skill_get_time(skill_id,skill_lv))) {
					if( bl->type == BL_MOB )
						mob_unlocktarget((TBL_MOB *)bl,gettick());
					map_freeblock_unlock();
					return 1;
				}
			} else {
				int count = map_forcountinrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),(sd ? sd->spiritball_old : 15),BL_CHAR,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);

				if( sd )
					pc_delspiritball(sd,count,0);
				clif_skill_nodamage(src,src,skill_id,skill_lv,
					sc_start2(src,src,SC_CURSEDCIRCLE_ATKER,100,skill_lv,count,skill_get_time(skill_id,skill_lv)));
				clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			}
			break;

		case SR_RAISINGDRAGON:
			sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
			if( sd ) {
				int max = pc_getmaxspiritball(sd,5);

				for( i = 0; i < max; i++ ) //Don't call more than max available spheres
					pc_addspiritball(sd,skill_get_time(MO_CALLSPIRITS,1),max);
				//Only starts the status at the highest learned level if you learned it
				if( (i = pc_checkskill(sd,MO_EXPLOSIONSPIRITS)) > 0 )
					sc_start(src,bl,SC_EXPLOSIONSPIRITS,100,i,skill_get_time(skill_id,skill_lv));
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SR_ASSIMILATEPOWER:
			if( flag&1 ) {
				uint16 sp = 0;

				if( dstsd && (sd == dstsd || map_flag_vs(src->m) || (sd && sd->duel_group && sd->duel_group == dstsd->duel_group)) &&
					(dstsd->class_&MAPID_BASEMASK) != MAPID_GUNSLINGER && (dstsd->class_&MAPID_UPPERMASK) != MAPID_REBELLION ) {
					if( dstsd->spiritball > 0 ) {
						sp = dstsd->spiritball;
						pc_delspiritball(dstsd,dstsd->spiritball,0);
					}
					if( dstsd->charmball_type != CHARM_TYPE_NONE && dstsd->charmball > 0 ) {
						sp += dstsd->charmball;
						pc_delcharmball(dstsd,dstsd->charmball,dstsd->charmball_type);
					}
					if( sp )
						status_percent_heal(src,0,sp); //1% SP per spiritball
				}
				clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			} else
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),splash_target(src),src,skill_id,skill_lv,tick,flag|BCT_ALL|SD_SPLASH|1,skill_castend_nodamage_id);
			break;

		case SR_POWERVELOCITY:
			if( sd && dstsd ) {
				for( i = 0; i < sd->spiritball; i++ )
					pc_addspiritball(dstsd,skill_get_time(MO_CALLSPIRITS,1),pc_getmaxspiritball(dstsd,5));
				pc_delspiritball(sd,sd->spiritball,0);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SR_GENTLETOUCH_CURE:
			if( dstmd && (dstmd->mob_id == MOBID_EMPERIUM || status_get_class_(bl) == CLASS_BATTLEFIELD) )
				break;
			if( rnd()%100 < skill_lv * 5 + (status_get_dex(src) + status_get_lv(src)) / 4 - rnd_value(1,10) ) {
				int heal = 120 * skill_lv + status_get_max_hp(bl) * skill_lv / 100;

				status_change_end(bl,SC_STONE,INVALID_TIMER);
				status_change_end(bl,SC_FREEZE,INVALID_TIMER);
				status_change_end(bl,SC_STUN,INVALID_TIMER);
				status_change_end(bl,SC_POISON,INVALID_TIMER);
				status_change_end(bl,SC_SILENCE,INVALID_TIMER);
				status_change_end(bl,SC_BLIND,INVALID_TIMER);
				status_change_end(bl,SC_HALLUCINATION,INVALID_TIMER);
				status_heal(bl,heal,0,1);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case SR_FLASHCOMBO: {
				const uint16 combo_skid[] = { SR_DRAGONCOMBO,SR_FALLENEMPIRE,SR_TIGERCANNON,SR_SKYNETBLOW };
				const uint16 combo_sklv[] = { 10,5,10,5 };
				const int delay[] = { 0,250,500,2000 };

				if( sd )
					sd->ud.attackabletime = sd->canuseitem_tick = sd->ud.canact_tick = tick + delay[3];
				clif_skill_nodamage(src,bl,skill_id,skill_lv,
					sc_start(src,src,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
				for( i = 0; i < ARRAYLENGTH(combo_skid); i++ ) {
					if( combo_skid[i] == SR_TIGERCANNON )
						flag |= 32;
					skill_addtimerskill(src,tick + delay[i],bl->id,0,0,combo_skid[i],(sd ? pc_checkskill(sd,combo_skid[i]) : combo_sklv[i]),BF_WEAPON,flag);
				}
			}
			break;

		case WA_SWING_DANCE:
		case WA_SYMPHONY_OF_LOVER:
		case WA_MOONLIT_SERENADE:
		case MI_RUSH_WINDMILL:
		case MI_ECHOSONG:
			if( !sd || !sd->status.party_id || (flag&1) ) {
				sc_start4(src,bl,type,100,skill_lv,(sd ? pc_checkskill(sd,WM_LESSON) : 10),status_get_job_lv(src),0,skill_get_time(skill_id,skill_lv));
				if( bl->id == src->id )
					clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			} else if( sd )
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case MI_HARMONIZE:
			if( bl->id != src->id )
				clif_skill_nodamage(src,bl,skill_id,skill_lv,sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			clif_skill_nodamage(src,src,skill_id,skill_lv,
				sc_start(src,src,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			break;

		case WM_DEADHILLHERE:
			if( !status_isdead(bl) )
				break;
			if( rnd()%100 < 88 + 2 * skill_lv ) {
				int heal = 0;

				status_zap(bl,0,tstatus->sp * (60 - 10 * skill_lv) / 100);
				heal = max(tstatus->sp,1);
				status_fixed_revive(bl,heal,0);
				status_set_sp(bl,0,0);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			} else if( sd ) {
				skill_consume_requirement(sd,skill_id,skill_lv,3);
				map_freeblock_unlock();
				return 1;
			}
			break;

		case WM_LULLABY_DEEPSLEEP:
			if( flag&1 ) { //[(Skill Level x 4) + (Voice Lessons Skill Level x 2) + (Caster's Base Level / 15) + (Caster's Job Level / 5)] %
				int rate = 4 * skill_lv + 2 * (sd ? pc_checkskill(sd,WM_LESSON) : 10) + status_get_lv(src) / 15 + status_get_job_lv(src) / 5;
				int duration = skill_get_time(skill_id,skill_lv) - (status_get_base_status(bl)->int_ * 50 + status_get_lv(bl) * 50); //Duration reduction for Deep Sleep Lullaby is doubled

				if( bl->id == src->id )
					break;
				sc_start(src,bl,type,rate,skill_lv,duration);
			} else {
				map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
					skill_id,skill_lv,tick,flag|BCT_ALL|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case WM_SIRCLEOFNATURE:
			if( flag&1 )
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
			else {
				map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_PC,src,
					skill_id,skill_lv,tick,flag|BCT_ALL|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case WM_VOICEOFSIREN:
			if( flag&1 ) {
				int rate = 6 * skill_lv + 2 * (sd ? pc_checkskill(sd,WM_LESSON) : 10) + status_get_job_lv(src) / 2;

				if( bl->id == src->id )
					break;
				sc_start2(src,bl,type,rate,skill_lv,src->id,skill_get_time(skill_id,skill_lv));
			} else {
				int sflag = (map_flag_vs(src->m) ? BCT_ENEMY : BCT_ALL);

				map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_CHAR|BL_SKILL,src,
					skill_id,skill_lv,tick,flag|sflag|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case WM_GLOOMYDAY:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start4(src,bl,type,100,skill_lv,0,0,(sd ? pc_checkskill(sd,WM_LESSON) : 10),skill_get_time(skill_id,skill_lv)));
			break;

		case WM_SONG_OF_MANA:
			if( flag&1 )
				sc_start2(src,bl,type,100,skill_lv,skill_chorus_count(sd,2),skill_get_time(skill_id,skill_lv));
			else if( sd ) {
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,
					skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case WM_DANCE_WITH_WUG:
			if( flag&1 )
				sc_start2(src,bl,type,100,skill_lv,skill_chorus_count(sd,1),skill_get_time(skill_id,skill_lv));
			else if( sd ) {
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,
					skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case WM_LERADS_DEW:
			if( flag&1 )
				sc_start2(src,bl,type,100,skill_lv,skill_chorus_count(sd,3),skill_get_time(skill_id,skill_lv));
			else if( sd ) {
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,
					skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case WM_UNLIMITED_HUMMING_VOICE:
			if( flag&1 ) {
				if( bl->id == src->id )
					break;
				sc_start2(src,bl,type,100,skill_lv,skill_chorus_count(sd,0),skill_get_time(skill_id,skill_lv));
			} else {
				map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_PC,src,
					skill_id,skill_lv,tick,flag|BCT_ALL|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case WM_SATURDAY_NIGHT_FEVER:
			if( flag&1 ) {
				if( tsc && ((tsc->option&(OPTION_HIDE|OPTION_CLOAK|OPTION_CHASEWALK)) ||
					tsc->data[SC_CAMOUFLAGE] || tsc->data[SC_STEALTHFIELD] || tsc->data[SC__SHADOWFORM]) )
					break;
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
				if( skill_area_temp[5] > 7 )
					status_fix_damage(src,bl,9999,clif_damage(src,bl,tick,0,0,9999,0,DMG_NORMAL,0,false));
			} else {
				int rate = status_get_int(src) / 6 + status_get_job_lv(src) / 5 + skill_lv * 4;

				if( rnd()%100 >= rate ) {
					if( sd )
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				skill_area_temp[5] = map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_PC,src,
					skill_id,skill_lv,tick,BCT_ALL,skill_area_sub_count);
				map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_PC,src,
					skill_id,skill_lv,tick,flag|BCT_ALL|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case WM_MELODYOFSINK:
		case WM_BEYOND_OF_WARCRY:
			if( flag&1 )
				sc_start4(src,bl,type,100,skill_lv,skill_chorus_count(sd,3),skill_chorus_count(sd,1),0,skill_get_time(skill_id,skill_lv));
			else {
				if( rnd()%100 < 15 + 5 * skill_lv + min(5 * skill_chorus_count(sd,3),65) ) {
					map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_PC,src,
						skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
					clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				}
			}
			break;

		case WM_RANDOMIZESPELL: {
				int improv_skill_id = 0, improv_skill_lv, checked = 0, checked_max = MAX_SKILL_IMPROVISE_DB * 3;

				do {
					i = rnd()%MAX_SKILL_IMPROVISE_DB;
					improv_skill_id = skill_improvise_db[i].skill_id;
				} while( checked++ < checked_max && (improv_skill_id == 0 || rnd()%10000 >= skill_improvise_db[i].per) );
				if( !skill_get_index(improv_skill_id) ) {
					if( sd )
						clif_skill_fail(sd, skill_id, USESKILL_FAIL_LEVEL, 0, 0);
					break;
				}
				improv_skill_lv = 4 + skill_lv;
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				if( sd ) {
					sd->state.abra_flag = 2;
					sd->skillitem = improv_skill_id;
					sd->skillitemlv = improv_skill_lv;
					sd->skilliteminf = 0;
					clif_item_skill(sd,improv_skill_id,improv_skill_lv,0);
				} else {
					struct unit_data *ud = unit_bl2ud(src);
					int inf = skill_get_inf(improv_skill_id);

					if( !ud )
						break;
					if( (inf&INF_SELF_SKILL) || (inf&INF_SUPPORT_SKILL) ) {
						if( src->type == BL_PET )
							bl = (struct block_list *)((TBL_PET *)src)->master;
						if( !bl )
							bl = src;
						unit_skilluse_id(src,bl->id,improv_skill_id,improv_skill_lv);
					} else {
						int target_id = 0;

						if( ud->target )
							target_id = ud->target;
						else switch( src->type ) {
							case BL_MOB: target_id = ((TBL_MOB *)src)->target_id; break;
							case BL_PET: target_id = ((TBL_PET *)src)->target_id; break;
						}
						if( !target_id )
							break;
						if( skill_get_casttype(improv_skill_id) == CAST_GROUND ) {
							bl = map_id2bl(target_id);
							if( !bl )
								bl = src;
							unit_skilluse_pos(src,bl->x,bl->y,improv_skill_id,improv_skill_lv);
						} else
							unit_skilluse_id(src,target_id,improv_skill_id,improv_skill_lv);
					}
				}
			}
			break;

		case WM_FRIGG_SONG:
			if( flag&1 )
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
			else {
				if( sd && sd->status.party_id && map_flag_vs(bl->m) )
					party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
				else
					map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_PC,src,skill_id,skill_lv,tick,flag|BCT_NOENEMY|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case RETURN_TO_ELDICASTES:
		case ALL_GUARDIAN_RECALL:
		case ECLAGE_RECALL:
		case ALL_NIFLHEIM_RECALL:
		case ALL_PRONTERA_RECALL:
			if( sd ) {
				short x = 0, y = 0; //Destiny position
				unsigned short mapindex = 0;

				switch( skill_id ) {
					default:
					case RETURN_TO_ELDICASTES:
						x = 198;
						y = 187;
						mapindex = mapindex_name2id(MAP_DICASTES);
						break;
					case ALL_GUARDIAN_RECALL:
						x = 44;
						y = 151;
						mapindex = mapindex_name2id(MAP_MORA);
						break;
					case ECLAGE_RECALL:
						x = 47;
						y = 31;
						mapindex = mapindex_name2id(MAP_ECLAGE_IN);
						break;
					case ALL_NIFLHEIM_RECALL:
						x = 193;
						y = 186;
						mapindex = mapindex_name2id(MAP_NIFLHEIM);
						break;
					case ALL_PRONTERA_RECALL:
						x = 159;
						y = 152;
						mapindex = mapindex_name2id(MAP_PRONTERA);
						break;
				}
				if( !mapindex ) { //Given map not found?
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					map_freeblock_unlock();
					return 1;
				} else
					pc_setpos(sd,mapindex,x,y,CLR_TELEPORT);
			}
			break;

		case ECL_SNOWFLIP:
		case ECL_PEONYMAMY:
		case ECL_SADAGUI:
		case ECL_SEQUOIADUST:
			switch( skill_id ) {
				case ECL_SNOWFLIP:
					status_change_end(bl,SC_SLEEP,INVALID_TIMER);
					status_change_end(bl,SC_BLEEDING,INVALID_TIMER);
					status_change_end(bl,SC_BURNING,INVALID_TIMER);
					status_change_end(bl,SC_DEEPSLEEP,INVALID_TIMER);
					break;
				case ECL_PEONYMAMY:
					status_change_end(bl,SC_FREEZE,INVALID_TIMER);
					status_change_end(bl,SC_FREEZING,INVALID_TIMER);
					status_change_end(bl,SC_CRYSTALIZE,INVALID_TIMER);
					break;
				case ECL_SADAGUI:
					status_change_end(bl,SC_STUN,INVALID_TIMER);
					status_change_end(bl,SC_CONFUSION,INVALID_TIMER);
					status_change_end(bl,SC_HALLUCINATION,INVALID_TIMER);
					status_change_end(bl,SC_FEAR,INVALID_TIMER);
					break;
				case ECL_SEQUOIADUST:
					status_change_end(bl,SC_STONE,INVALID_TIMER);
					status_change_end(bl,SC_POISON,INVALID_TIMER);
					status_change_end(bl,SC_CURSE,INVALID_TIMER);
					status_change_end(bl,SC_BLIND,INVALID_TIMER);
					status_change_end(bl,SC_DECREASEAGI,INVALID_TIMER);
					status_change_end(bl,SC_ORCISH,INVALID_TIMER);
					break;
			}
			clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,1,DMG_SKILL);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case GM_SANDMAN:
			if( tsc ) {
				if( tsc->opt1 == OPT1_SLEEP )
					tsc->opt1 = 0;
				else
					tsc->opt1 = OPT1_SLEEP;
				clif_changeoption(bl);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case SO_ARRULLO: { //[(15 + 5 * Skill Level) + (Caster's INT / 5) + (Caster's Job Level / 5) - (Target's INT / 6) - (Target's LUK / 10)]%
				int rate = (15 + 5 * skill_lv) + status_get_int(src) / 5 + status_get_job_lv(src) / 5 - status_get_int(bl) / 6 - status_get_luk(bl) / 10;

				clif_skill_nodamage(src,bl,skill_id,skill_lv,
					sc_start(src,bl,type,rate,skill_lv,skill_get_time(skill_id,skill_lv)));
			}
			break;

		case SO_SUMMON_AGNI:
		case SO_SUMMON_AQUA:
		case SO_SUMMON_VENTUS:
		case SO_SUMMON_TERA:
			if( sd ) {
				//Remove previous elemental first
				if( sd->ed )
					elemental_delete(sd->ed,0);
				//Summoning the new one
				elemental_create(sd,skill_id - SO_SUMMON_AGNI,skill_lv,skill_get_time(skill_id,skill_lv));
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SO_EL_CONTROL:
			if( sd && sd->ed ) {
				if( skill_lv == 4 )
					elemental_delete(sd->ed,0);
				else
					sc_start(src,&sd->ed->bl,SC_EL_PASSIVE + (skill_lv - 1),100,skill_lv,-1);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SO_EL_ACTION:
			if( sd && sd->ed ) {
				sd->skill_id_old = skill_id;
				elemental_action(sd->ed,bl,tick);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SO_EL_CURE:
			if( sd && sd->ed ) {
				int hp = status_get_max_hp(src) * 10 / 100,
					sp = status_get_max_hp(src) * 10 / 100;

				status_heal(&sd->ed->bl,hp,sp,1);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SO_ELEMENTAL_SHIELD:
			if( flag&1 ) {
				skill_unitsetting(bl,MG_SAFETYWALL,skill_lv + 5,bl->x,bl->y,skill_area_temp[5]);
				skill_unitsetting(bl,AL_PNEUMA,1,bl->x,bl->y,0);
			} else {
				if( sd && sd->ed ) {
					int e_class = status_get_class(&sd->ed->bl);

					switch( e_class ) {
						case ELEMENTALID_AGNI_M:
						case ELEMENTALID_AQUA_M:
						case ELEMENTALID_VENTUS_M:
						case ELEMENTALID_TERA_M:
							skill_area_temp[5] = 2;
							break;
						case ELEMENTALID_AGNI_L:
						case ELEMENTALID_AQUA_L:
						case ELEMENTALID_VENTUS_L:
						case ELEMENTALID_TERA_L:
							skill_area_temp[5] = 4;
							break;
					}
					elemental_delete(sd->ed,0);
					if( sd->status.party_id )
						party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
					else {
						skill_unitsetting(src,MG_SAFETYWALL,skill_lv + 5,src->x,src->y,skill_area_temp[5]);
						skill_unitsetting(src,AL_PNEUMA,1,src->x,src->y,0);
					}
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case GN_CHANGEMATERIAL:
		case SO_EL_ANALYSIS:
			if( sd )
				clif_skill_itemlistwindow(sd,skill_id,skill_lv);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case GN_BLOOD_SUCKER: {
				struct status_change *sc = status_get_sc(src);

				if( sc && sc->bs_counter < skill_get_maxcount(skill_id,skill_lv) ) {
					if( tsc && tsc->data[type] ) {
						sc->bs_counter--;
						status_change_end(src,type,INVALID_TIMER); //The first one cancels and the last one will take effect resetting the timer
					}
					if( (i = sc_start2(src,bl,type,100,skill_lv,src->id,skill_get_time(skill_id,skill_lv))) )
						sc->bs_counter++;
					clif_skill_nodamage(src,bl,skill_id,skill_lv,i);
				} else if( sd ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
			}
			break;

		case GN_SPORE_EXPLOSION:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start2(src,bl,type,100,skill_lv,src->id,skill_get_time(skill_id,skill_lv)));
			break;

		case GN_MANDRAGORA:
			if( flag&1 ) {
				int chance = 25 + 10 * skill_lv - (tstatus->vit + tstatus->luk) / 5;

				if( chance < 10 )
					chance = 10; //Minimal chance is 10%
				if( tsc && tsc->data[type] )
					break;
				if( rnd()%100 < chance ) { //Coded to both inflect status and drain target's SP only when successful [Rytech]
					sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
					status_zap(bl,0,status_get_max_sp(bl) * (25 + 5 * skill_lv) / 100);
				}
			} else {
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,src,skill_id,skill_lv,1);
			}
			break;

		case GN_SLINGITEM:
			if( sd ) {
				short idx = sd->equip_index[EQI_AMMO], ammo_id;
				uint16 baselv;

				//Check if there's any ammo equipped
				if( idx < 0 )
					break; //No ammo
				//Check if ammo ID is that of a Genetic throwing item
				ammo_id = sd->inventory_data[idx]->nameid;
				if( ammo_id <= 0 )
					break;
				//Used to tell other parts of the code which damage formula and status to use
				sd->itemid = ammo_id;
				//Thrower's BaseLv affects HP and SP increase potions when thrown
				baselv = status_get_lv(src);
				if( itemdb_is_slingatk(ammo_id) ) { //If thrown item is a bomb or a lump, then its a attack type ammo
					if( battle_check_target(src,bl,BCT_ENEMY) > 0 ) { //Only allow throwing attacks at enemies
						if( ammo_id == ITEMID_PINEAPPLE_BOMB ) //Pineapple Bombs deal 5x5 splash damage on targeted enemy
							map_foreachinallrange(skill_area_sub,bl,2,BL_CHAR,src,GN_SLINGITEM_RANGEMELEEATK,skill_lv,tick,flag|BCT_ENEMY|SD_LEVEL|SD_ANIMATION|SD_SPLASH|1,skill_castend_damage_id);
						else //All other bombs and lumps hits one enemy
							skill_castend_damage_id(src,bl,GN_SLINGITEM_RANGEMELEEATK,skill_lv,tick,flag|SD_LEVEL|SD_ANIMATION);
					} else //Otherwise, it fails, shows animation and removes items
						clif_skill_fail(sd,GN_SLINGITEM_RANGEMELEEATK,USESKILL_FAIL,0,0);
				} else if( itemdb_is_slingbuff(ammo_id) ) { //If thrown item is a potion, food, powder, or overcooked food, then its a buff type ammo
					switch( ammo_id ) {
						case ITEMID_MYSTERIOUS_POWDER: //MaxHP -2%
							sc_start(src,bl,SC_MYSTERIOUS_POWDER,100,2,10000);
							break;
						case ITEMID_BOOST500_TO_THROW: //ASPD +10%
							sc_start(src,bl,SC_BOOST500,100,10,500000);
							break;
						case ITEMID_FULL_SWINGK_TO_THROW: //WATK +50
							sc_start(src,bl,SC_FULL_SWING_K,100,50,500000);
							break;
						case ITEMID_MANA_PLUS_TO_THROW: //MATK +50
							sc_start(src,bl,SC_MANA_PLUS,100,50,500000);
							break;
						case ITEMID_CURE_FREE_TO_THROW: //Cures Silence, Bleeding, Poison, Curse, Orcish, Undead, Blind, Confusion, DPoison and heals 500 HP
							status_change_end(bl,SC_SILENCE,INVALID_TIMER);
							status_change_end(bl,SC_BLEEDING,INVALID_TIMER);
							status_change_end(bl,SC_POISON,INVALID_TIMER);
							status_change_end(bl,SC_CURSE,INVALID_TIMER);
							status_change_end(bl,SC_ORCISH,INVALID_TIMER);
							status_change_end(bl,SC_CHANGEUNDEAD,INVALID_TIMER);
							status_change_end(bl,SC_BLIND,INVALID_TIMER);
							status_change_end(bl,SC_CONFUSION,INVALID_TIMER);
							status_change_end(bl,SC_DPOISON,INVALID_TIMER);
							status_heal(bl,500,0,0);
							break;
						case ITEMID_STAMINA_UP_M_TO_THROW: //MaxHP +5%
							sc_start(src,bl,SC_MUSTLE_M,100,5,500000);
							break;
						case ITEMID_DIGESTIVE_F_TO_THROW: //MaxSP +5%
							sc_start(src,bl,SC_LIFE_FORCE_F,100,5,500000);
							break;
						case ITEMID_HP_INC_POTS_TO_THROW: //MaxHP +(500 + Thrower BaseLv * 10 / 3) and heals 1% MaxHP
							sc_start4(src,bl,SC_PROMOTE_HEALTH_RESERCH,100,2,1,baselv,0,500000);
							status_percent_heal(bl,1,0);
							break;
						case ITEMID_HP_INC_POTM_TO_THROW: //MaxHP +(1500 + Thrower BaseLv * 10 / 3) and heals 2% MaxHP
							sc_start4(src,bl,SC_PROMOTE_HEALTH_RESERCH,100,2,2,baselv,0,500000);
							status_percent_heal(bl,2,0);
							break;
						case ITEMID_HP_INC_POTL_TO_THROW: //MaxHP +(2500 + Thrower BaseLv * 10 / 3) and heals 5% MaxHP
							sc_start4(src,bl,SC_PROMOTE_HEALTH_RESERCH,100,2,3,baselv,0,500000);
							status_percent_heal(bl,5,0);
							break;
						case ITEMID_SP_INC_POTS_TO_THROW: //MaxSP +(Thrower BaseLv / 10 - 5)% and recovers 2% MaxSP
							sc_start4(src,bl,SC_ENERGY_DRINK_RESERCH,100,2,1,baselv,0,500000);
							status_percent_heal(bl,0,2);
							break;
						case ITEMID_SP_INC_POTM_TO_THROW: //MaxSP +(Thrower BaseLv / 10)% and recovers 4% MaxSP
							sc_start4(src,bl,SC_ENERGY_DRINK_RESERCH,100,2,2,baselv,0,500000);
							status_percent_heal(bl,0,4);
							break;
						case ITEMID_SP_INC_POTL_TO_THROW: //MaxSP +(Thrower BaseLv / 10 + 5)% and recovers 8% MaxSP
							sc_start4(src,bl,SC_ENERGY_DRINK_RESERCH,100,2,3,baselv,0,500000);
							status_percent_heal(bl,0,8);
							break;
						case ITEMID_EN_WHITE_POTZ_TO_THROW: //Natural HP Recovery +20% and heals 1000 HP
							sc_start(src,bl,SC_EXTRACT_WHITE_POTION_Z,100,20,500000);
							pc_itemheal((TBL_PC *)bl,ITEMID_EN_WHITE_POTZ_TO_THROW,1000,0,true);
							break;
						case ITEMID_VITATA500_TO_THROW: //Natural SP Recovery +20%, MaxSP +5%, and recovers 200 SP
							sc_start2(src,bl,SC_VITATA_500,100,20,5,500000);
							pc_itemheal((TBL_PC *)bl,ITEMID_VITATA500_TO_THROW,0,200,true);
							break;
						case ITEMID_EN_CEL_JUICE_TO_THROW: //ASPD +10%
							sc_start(src,bl,SC_EXTRACT_SALAMINE_JUICE,100,10,500000);
							break;
						case ITEMID_SAVAGE_BBQ_TO_THROW: //STR +20
							sc_start(src,bl,SC_SAVAGE_STEAK,100,20,300000);
							break;
						case ITEMID_WUG_COCKTAIL_TO_THROW: //INT +20
							sc_start(src,bl,SC_COCKTAIL_WARG_BLOOD,100,20,300000);
							break;
						case ITEMID_M_BRISKET_TO_THROW: //VIT +20
							sc_start(src,bl,SC_MINOR_BBQ,100,20,300000);
							break;
						case ITEMID_SIROMA_ICETEA_TO_THROW: //DEX +20
							sc_start(src,bl,SC_SIROMA_ICE_TEA,100,20,300000);
							break;
						case ITEMID_DROCERA_STEW_TO_THROW: //AGI +20
							sc_start(src,bl,SC_DROCERA_HERB_STEAMED,100,20,300000);
							break;
						case ITEMID_PETTI_NOODLE_TO_THROW: //LUK +20
							sc_start(src,bl,SC_PUTTI_TAILS_NOODLES,100,20,300000);
							break;
						case ITEMID_BLACK_THING_TO_THROW: //Reduces all stats by random 5 - 10
							sc_start(src,bl,SC_STOMACHACHE,100,rnd_value(5,10),60000);
							break;
					}
				}
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case GN_MIX_COOKING:
		case GN_MAKEBOMB:
		case GN_S_PHARMACY:
			if( sd ) {
				int qty = 1;

				sd->skill_id_old = skill_id;
				sd->skill_lv_old = skill_lv;
				if( skill_id != GN_S_PHARMACY && skill_lv > 1 )
					qty = 10;
				clif_cooking_list(sd,(skill_id - GN_MIX_COOKING) + 27,skill_id,qty,(skill_id == GN_S_PHARMACY ? 6 : (skill_id == GN_MAKEBOMB ? 5 : 4)));
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case EL_CIRCLE_OF_FIRE:
		case EL_FIRE_CLOAK:
		case EL_WATER_DROP:
		case EL_WIND_STEP:
		case EL_WIND_CURTAIN:
		case EL_SOLID_SKIN:
		case EL_STONE_SHIELD:
		case EL_PYROTECHNIC:
		case EL_HEATER:
		case EL_TROPIC:
		case EL_AQUAPLAY:
		case EL_COOLER:
		case EL_CHILLY_AIR:
		case EL_GUST:
		case EL_BLAST:
		case EL_WILD_STORM:
		case EL_PETROLOGY:
		case EL_CURSED_SOIL:
		case EL_UPHEAVAL:
			{
				struct elemental_data *ed = BL_CAST(BL_ELEM,src);

				if( ed && ed->master ) {
					struct status_change *sc = status_get_sc(&ed->bl);
					struct block_list *e_bl = &ed->master->bl;
					sc_type type2 = (sc_type)(type - 1);

					if( (sc && sc->data[type2]) || (tsc && tsc->data[type]) )
						elemental_clean_single_effect(ed,skill_id);
					else {
						sc_start(src,e_bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
						sc_start(src,src,type2,100,skill_lv,skill_get_time(skill_id,skill_lv));
						switch( skill_id ) {
							case EL_WIND_STEP: //There aren't teleport, just push the master away
								clif_skill_damage(src,e_bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
								skill_blown(src,e_bl,(rnd()%skill_get_blewcount(skill_id,skill_lv)) + 1,rnd()%8,0);
								break;
							case EL_SOLID_SKIN:
								clif_skill_damage(src,e_bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
								clif_skill_damage(src,src,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
								break;
							default:
								clif_skill_damage(src,src,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
								break;
						}
					}
				}
			}
			break;

		case EL_WATER_SCREEN: {
				struct elemental_data *ed = BL_CAST(BL_ELEM,src);

				if( ed && ed->master ) {
					struct status_change *sc = status_get_sc(&ed->bl);
					struct block_list *e_bl = &ed->master->bl;
					sc_type type2 = (sc_type)(type - 1);

					if( (sc && sc->data[type2]) || (tsc && tsc->data[type]) )
						elemental_clean_single_effect(ed,skill_id);
					else { //This not heals at the end
						sc_start(src,e_bl,type,100,src->id,skill_get_time(skill_id,skill_lv));
						sc_start(src,src,type2,100,skill_lv,skill_get_time(skill_id,skill_lv));
						clif_skill_damage(src,src,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
					}
				}
			}
			break;

		case EL_FIRE_BOMB:
		case EL_FIRE_WAVE:
		case EL_WATER_SCREW:
		case EL_HURRICANE:
			if( rnd()%100 < 30 ) {
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id + 1,skill_lv),BL_CHAR,src,skill_id + 1,skill_lv,tick,flag|BCT_ENEMY|SD_SPLASH|1,skill_castend_damage_id);
				clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			} else
				skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag);
			break;

		case RL_RICHS_COIN:
			for( i = 0; i < 10; i++ ) {
				if( sd )
					pc_addspiritball(sd,skill_get_time(skill_id,skill_lv),10);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case RL_FLICKER:
			if( flag&1 ) {
				if( tsc && tsc->data[SC_H_MINE] && tsc->data[SC_H_MINE]->val2 == src->id ) {
					flag = 0; //Reset flag
					sc_start(src,bl,SC_H_MINE_EXPLOSION,100,skill_lv,100); //Explosion animation
					skill_castend_damage_id(src,bl,RL_H_MINE,tsc->data[SC_H_MINE]->val1,tick,flag|4);
				}
			} else { //Search for active howling mines and binding traps
				map_foreachinallrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
				map_foreachinallrange(skill_flicker_bind_trap,src,AREA_SIZE,BL_SKILL,src,tick);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case RL_QD_SHOT:
			skill_area_temp[1] = bl->id;
			skill_attack(skill_get_type(skill_id),src,src,bl,skill_id,skill_lv,tick,flag);
			map_foreachinrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
				skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_SPLASH|1,skill_castend_damage_id);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case RL_C_MARKER:
			//Check if the target is already tagged by another source
			if( tsce && tsce->val2 != src->id ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			//Marking the target
			if( sd ) {
				short count = MAX_CRIMSON_MARKS;

				i = 0;
				ARR_FIND(0,count,i,sd->crimson_mark[i] == bl->id);
				if( i == count ) {
					ARR_FIND(0,count,i,sd->crimson_mark[i] == 0);
					if( i == count ) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						break;
					}
				}
				sd->crimson_mark[i] = bl->id;
				sc_start4(src,bl,type,100,skill_lv,src->id,i,0,skill_get_time(skill_id,skill_lv));
			} else //If mob casts this, at least SC_C_MARKER as debuff
				sc_start2(src,bl,type,100,skill_lv,src->id,skill_get_time(skill_id,skill_lv));
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			clif_skill_damage(src,bl,tick,0,status_get_dmotion(src),-30000,1,skill_id,skill_lv,DMG_SPLASH);
			break;

		case SJ_DOCUMENT:
			if( sd ) {
				if( skill_lv == 1 || skill_lv == 3 )
					pc_resetfeel(sd);
				if( skill_lv == 2 || skill_lv == 3 )
					pc_resethate(sd);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SP_SOULGOLEM:
		case SP_SOULSHADOW:
		case SP_SOULFALCON:
		case SP_SOULFAIRY:
			if( !dstsd ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			if( tsc && (tsc->data[SC_SPIRIT] || tsc->data[SC_SOULGOLEM] || tsc->data[SC_SOULSHADOW] ||
				tsc->data[SC_SOULFALCON] || tsc->data[SC_SOULFAIRY]) ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			break;

		case SP_SOULCURSE:
			if( flag&1 )
				sc_start(src,bl,type,30 + 10 * skill_lv,skill_lv,skill_get_time(skill_id,skill_lv));
			else {
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case SP_SOULUNITY:
			if( flag&1 ) {
				//Fail if a player is in unity with another source
				if( tsce && tsce->val2 != src->id )
					break;
				//Unite player's soul with caster's soul
				if( sd ) {
					short count = min(5 + skill_lv,MAX_UNITED_SOULS);
					i = 0;

					ARR_FIND(0, count, i, sd->united_soul[i] == bl->id);
					if( i == count ) {
						ARR_FIND(0, count, i, sd->united_soul[i] == 0);
						if( i == count ) {
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
							break;
						}
					}
					sd->united_soul[i] = bl->id;
					sc_start4(src,bl,type,100,skill_lv,src->id,i,0,skill_get_time(skill_id,skill_lv));
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			} else if( sd )
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case SP_SOULREVOLVE:
			if( !(tsc && (tsc->data[SC_SPIRIT] || tsc->data[SC_SOULGOLEM] || tsc->data[SC_SOULSHADOW] ||
				tsc->data[SC_SOULFALCON] || tsc->data[SC_SOULFAIRY])) ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
			status_change_end(bl,SC_SPIRIT,INVALID_TIMER);
			status_change_end(bl,SC_SOULGOLEM,INVALID_TIMER);
			status_change_end(bl,SC_SOULSHADOW,INVALID_TIMER);
			status_change_end(bl,SC_SOULFALCON,INVALID_TIMER);
			status_change_end(bl,SC_SOULFAIRY,INVALID_TIMER);
			status_heal(bl,0,50 * skill_lv,2);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case SP_SOULCOLLECT:
			if( tsce ) {
				status_change_end(bl,type,INVALID_TIMER);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				map_freeblock_unlock();
				return 1;
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start2(src,bl,type,100,skill_lv,(sd ? pc_checkskill(sd,SP_SOULENERGY) : 5),skill_get_time(skill_id,skill_lv)));
			break;

		case KO_KAHU_ENTEN:
		case KO_HYOUHU_HUBUKI:
		case KO_KAZEHU_SEIRAN:
		case KO_DOHU_KOUKAI:
			if( sd ) {
				int ele_type = skill_get_ele(skill_id,skill_lv);

				pc_addcharmball(sd,skill_get_time(skill_id,skill_lv),MAX_CHARMBALL,ele_type);
			}
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			break;

		case KO_MEIKYOUSISUI: {
				const enum sc_type scs[] = { SC_POISON,SC_CURSE,SC_BLIND,SC_FEAR,SC_BURNING,SC_FREEZING };
				signed char remove_attempt = 100; //Safety to prevent infinite looping in the randomizer
				bool debuff_active = false; //Flag if debuff is found to be active
				bool debuff_removed = false; //Flag when a debuff was removed

				//Don't send the ZC_USE_SKILL packet or it will lock up the player's sprite when the forced sitting happens
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
				//Remove a random debuff
				if( tsc && tsc->count ) { //Check if any of the listed passable debuffs are active
					for( i = 0; i < ARRAYLENGTH(scs); i++ ) {
						if( tsc->data[scs[i]] ) { //If a debuff is found, mark true.
							debuff_active = true;
							break; //End the check
						}
					}
					if( debuff_active ) { //Debuff found? 1 or more of them are likely active
						while( !debuff_removed && remove_attempt > 0 ) { //Randomly select a possible debuff and see if its active
							i = rnd()%8;
							if( tsc->data[scs[i]] ) { //Selected debuff active? If yes then remove it and mark it was removed
								status_change_end(bl,scs[i],INVALID_TIMER);
								debuff_removed = true;
							} else //Failed to remove
								remove_attempt--;
						}
					}
				}
			}
			break;

		case KO_ZANZOU: {
				struct mob_data *md2 = NULL;

				if( (md2 = mob_once_spawn_sub(src,src->m,src->x,src->y,status_get_name(src),MOBID_KO_KAGE,"",SZ_SMALL,AI_NONE)) ) {
					md2->master_id = src->id;
					md2->special_state.ai = AI_ZANZOU;
					if( md2->deletetimer != INVALID_TIMER )
						delete_timer(md2->deletetimer,mob_timer_delete);
					md2->deletetimer = add_timer(gettick() + skill_get_time(skill_id,skill_lv),mob_timer_delete,md2->bl.id,0);
					mob_spawn(md2);
					map_foreachinallrange(unit_changetarget,src,AREA_SIZE,BL_CHAR,src,&md2->bl);
					skill_blown(src,bl,skill_get_blewcount(skill_id,skill_lv),unit_getdir(bl),0);
					clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
				}
			}
			break;

		case KO_KYOUGAKU: {
				int rate = 45 + skill_lv * 5 - status_get_int(bl) / 10;

				clif_skill_nodamage(src,bl,skill_id,skill_lv,
					sc_start(src,bl,type,rate,skill_lv,skill_get_time(skill_id,skill_lv)));
			}
			break;

		case KO_JYUSATSU: {
				int rate = max(5,(45 + skill_lv * 10 - status_get_int(bl) / 2));

				if( (i = sc_start(src,bl,type,rate,skill_lv,skill_get_time(skill_id,skill_lv))) ) {
					status_zap(bl,tstatus->max_hp * skill_lv * 5 / 100,0);
					clif_skill_nodamage(src,bl,skill_id,skill_lv,i);
				}
				if( status_get_lv(bl) <= status_get_lv(src) )
					status_change_start(src,bl,SC_COMA,10,skill_lv,0,0,0,0,SCFLAG_NONE);
			}
			break;

		case KO_GENWAKU:
			if( battle_check_target(src,bl,BCT_ENEMY) > 0 ) {
				int rate = max(5,(45 + skill_lv * 5 - status_get_int(bl) / 10));
				int x = src->x, y = src->y;

				if( rnd()%100 < rate ) {
					if( !unit_blown_immune(src,0x1) && unit_movepos(src,bl->x,bl->y,0,false) ) {
						clif_blown(src,bl);
						clif_skill_nodamage(src,src,skill_id,skill_lv,
							sc_start4(src,src,type,25,skill_lv,0,0,1,skill_get_time(skill_id,skill_lv)));
					}
					if( !unit_blown_immune(bl,0x1) && unit_movepos(bl,x,y,0,false) ) {
						if( dstsd && pc_issit(dstsd) )
							pc_setstand(dstsd);
						clif_blown(bl,bl);
						sc_start4(src,bl,type,75,skill_lv,0,0,1,skill_get_time(skill_id,skill_lv));
						map_foreachinallrange(unit_changetarget,src,AREA_SIZE,BL_CHAR,src,bl);
					}
				}
			}
			break;

		case OB_OBOROGENSOU: {
				int rate = max(5,(25 + skill_lv * 10 - status_get_int(bl) / 2));

				if( rnd()%100 >= rate ) {
					if( sd )
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
				clif_skill_nodamage(src,bl,skill_id,skill_lv,
					sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
				clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			}
			break;

		case SP_SOULDIVISION:
		case KO_IZAYOI:
		case KG_KYOMU:
		case KG_KAGEMUSYA:
		case OB_ZANGETSU:
		case OB_AKAITSUKI:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			break;

		case KG_KAGEHUMI:
			if( flag&1 ) {
				if( bl->type != BL_PC )
					break;
				if( tsc && ((tsc->option&(OPTION_HIDE|OPTION_CLOAK)) || tsc->data[SC_CAMOUFLAGE] || tsc->data[SC__SHADOWFORM]) ) {
					status_change_end(bl,SC_HIDING,INVALID_TIMER);
					status_change_end(bl,SC_CLOAKING,INVALID_TIMER);
					status_change_end(bl,SC_CAMOUFLAGE,INVALID_TIMER);
					status_change_end(bl,SC_CLOAKINGEXCEED,INVALID_TIMER);
					status_change_end(bl,SC_NEWMOON,INVALID_TIMER);
					if( tsc->data[SC__SHADOWFORM] && rnd()%100 < 100 - tsc->data[SC__SHADOWFORM]->val1 * 10 )
						status_change_end(bl,SC__SHADOWFORM,INVALID_TIMER);
					sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
				}
			} else {
				map_foreachinrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR|BL_SKILL,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
				clif_skill_damage(src,bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case MH_LIGHT_OF_REGENE:
			if( hd ) {
				sc_start(src,&hd->master->bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
				hd->homunculus.intimacy = hom_intimacy_grade2intimacy(HOMGRADE_CORDIAL); //Change to cordial
				clif_send_homdata(hd->master,SP_INTIMATE,hd->homunculus.intimacy / 100); //Refresh intimacy info
			}
			break;

		case MH_OVERED_BOOST:
			if( hd ) {
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
				sc_start(src,&hd->master->bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
			}
			break;

		case MH_SILENT_BREEZE:
			if( hd ) {
				int heal;

				if( dstmd && (dstmd->mob_id == MOBID_EMPERIUM || status_get_class_(bl) == CLASS_BATTLEFIELD) )
					break;
				if( tsc ) {
					const enum sc_type scs[] = {
						SC_MANDRAGORA,SC_HARMONIZE,SC_DEEPSLEEP,SC_VOICEOFSIREN,SC_SLEEP,SC_CONFUSION,SC_HALLUCINATION
					};

					for( i = 0; i < ARRAYLENGTH(scs); i++ ) {
						if( tsc->data[scs[i]] )
							status_change_end(bl,scs[i],INVALID_TIMER);
					}
				}
				heal = 5 * (status_get_lv(src) +
#ifdef RENEWAL
					status_base_matk(bl,&hd->battle_status,status_get_lv(src))
#else
					status_base_matk_min(&hd->battle_status)
#endif
					);
				status_change_start(src,src,type,10000,skill_lv,0,0,1,skill_get_time(skill_id,skill_lv),SCFLAG_NOAVOID|SCFLAG_FIXEDTICK|SCFLAG_FIXEDRATE);
				if( battle_check_target(src,bl,BCT_ENEMY) > 0 && !status_has_mode(tstatus,MD_STATUS_IMMUNE) )
					status_change_start(src,bl,type,10000,skill_lv,0,0,1,skill_get_time(skill_id,skill_lv),SCFLAG_NOAVOID|SCFLAG_FIXEDTICK|SCFLAG_FIXEDRATE);
				status_heal(bl,heal,0,2);
				clif_skill_nodamage(src,bl,AL_HEAL,heal,1);
				clif_skill_nodamage(src,src,skill_id,skill_lv,1);
			}
			break;

		case MH_GRANITIC_ARMOR:
		case MH_PYROCLASTIC:
			if( hd ) {
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
				sc_start(src,&hd->master->bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
			}
			break;

		case MH_STYLE_CHANGE:
			if( hd ) {
				struct status_change_entry *sce = hd->sc.data[type];

				if( sce ) { //In preparation for other bl usage
					if( sce->val1 == MH_MD_FIGHTING ) {
						clif_status_load(&hd->master->bl,SI_STYLE_CHANGE,0);
						sce->val1 = MH_MD_GRAPPLING;
					} else {
						clif_status_change(&hd->master->bl,SI_STYLE_CHANGE,1,-1,0,0,0);
						sce->val1 = MH_MD_FIGHTING;
					}
				}
			}
			break;

		case MH_MAGMA_FLOW:
			if( flag&2 ) { //Splash AoE around the homunculus should only trigger by chance when status is active
				skill_area_temp[1] = 0;
				map_foreachinallrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),splash_target(src),src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_SPLASH|1,skill_castend_damage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			} else if( !flag ) //Using the skill normally only starts the status, it does not trigger a splash AoE attack this way
				clif_skill_nodamage(src,bl,skill_id,skill_lv,sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			break;

		case SU_SV_ROOTTWIST:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start2(src,bl,type,100,skill_lv,src->id,skill_get_time(skill_id,skill_lv)));
			if( sd && pc_checkskill(sd,SU_SPIRITOFLAND) > 0 )
				sc_start(src,src,SC_DORAM_MATK,100,skill_lv,skill_get_time(SU_SPIRITOFLAND,1));
			break;

		case SU_BUNCHOFSHRIMP:
			if( !sd || !sd->status.party_id || (flag&1) ) {
				int duration = skill_get_time(skill_id,skill_lv);

				if( sd && pc_checkskill(sd,SU_SPIRITOFSEA) > 0 )
					duration += skill_get_time2(skill_id,skill_lv);
				clif_skill_nodamage(bl,bl,skill_id,skill_lv,
					sc_start(src,bl,type,100,skill_lv,duration));
			} else if( sd )
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case SU_POWEROFFLOCK:
			if( flag&1 ) {
				sc_start(src,bl,SC_FEAR,100,skill_lv,skill_get_time(skill_id,skill_lv));
				sc_start(src,bl,SC_FREEZE,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			} else {
				map_foreachinrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
					skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
				clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			}
			break;

		case SU_HISS:
			if( !sd || !sd->status.party_id || (flag&1) ) {
				clif_skill_nodamage(bl,bl,skill_id,-1,
					sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
				sc_start(src,bl,SC_DORAM_WALKSPEED,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			} else if( sd )
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case SU_PURRING:
			if( !sd || !sd->status.party_id || (flag&1) )
				clif_skill_nodamage(bl,bl,SU_GROOMING,-1,sc_start(src,bl,SC_GROOMING,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			else if( sd )
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case SU_SHRIMPARTY:
			if( !sd || !sd->status.party_id || (flag&1) ) {
				i = (sd ? pc_checkskill(sd,SU_FRESHSHRIMP) : 5);
				clif_skill_nodamage(bl,bl,SU_FRESHSHRIMP,-1,
					sc_start(src,bl,SC_FRESHSHRIMP,100,i,skill_get_time(SU_FRESHSHRIMP,i)));
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
			} else if( sd )
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case SU_MEOWMEOW:
			if( !sd || !sd->status.party_id || (flag&1) ) {
				sc_start(src,bl,SC_CHATTERING,100,skill_lv,skill_get_time(skill_id,skill_lv));
				sc_start(src,bl,SC_DORAM_WALKSPEED,100,skill_lv,skill_get_time(skill_id,skill_lv));
				clif_skill_damage(bl,bl,tick,0,status_get_dmotion(bl),-30000,1,skill_id,-1,DMG_SPLASH);
				if( bl->id == src->id )
					clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			} else if( sd )
				party_foreachsamemap(skill_area_sub,sd,skill_get_splash(skill_id,skill_lv),src,skill_id,skill_lv,tick,flag|BCT_PARTY|1,skill_castend_nodamage_id);
			break;

		case SU_CHATTERING:
			clif_skill_nodamage(src,bl,skill_id,skill_lv,
				sc_start(src,bl,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			sc_start(src,bl,SC_DORAM_WALKSPEED,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;

		case NPC_PULSESTRIKE2:
			for( i = 0; i < 3; i++ )
				skill_addtimerskill(src,tick + i * 1000,bl->id,0,0,skill_id,skill_lv,0,0);
			break;

		case ALL_EQSWITCH:
			if( sd ) {
				int position;

				clif_equipswitch_reply(sd,false);
				for( i = 0, position = 0; i < EQI_MAX; i++ ) {
					if( sd->equip_switch_index[i] >= 0 && !(position&equip_bitmask[i]) )
						position |= pc_equipswitch(sd,sd->equip_switch_index[i]);
				}
			}
			break;

		default:
			ShowWarning("skill_castend_nodamage_id: Unknown skill used:%d\n",skill_id);
			clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
			map_freeblock_unlock();
			return 1;
	}

	if( skill_id != SR_CURSEDCIRCLE ) {
		struct status_change *sc = status_get_sc(src);

		if( sc && sc->data[SC_CURSEDCIRCLE_ATKER] )
			status_change_end(src,SC_CURSEDCIRCLE_ATKER,INVALID_TIMER);
	}

	if( dstmd ) { //Mob skill event for no damage skills (damage ones are handled in battle_calc_damage) [Skotlex]
		mob_log_damage(dstmd,src,0); //Log interaction (counts as 'attacker' for the exp bonus)
		mobskill_event(dstmd,src,tick,MSC_SKILLUSED|(skill_id<<16));
	}

	if( sd && !(flag&1) ) { //Ensure that the skill last-cast tick is recorded
		sd->canskill_tick = gettick();

		if( sd->state.arrow_atk ) //Consume ammo on last invocation to this skill
			battle_consume_ammo(sd,skill_id,skill_lv);

		//Perform skill requirement consumption
		skill_consume_requirement(sd,skill_id,skill_lv,2);
	}

	map_freeblock_unlock();
	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
TIMER_FUNC(skill_castend_id)
{
	struct block_list *target, *src;
	struct map_session_data *sd;
	struct mob_data *md;
	struct homun_data *hd;
	struct unit_data *ud;
	struct status_change *sc = NULL, *tsc = NULL;
	int inf, inf2, inf3, flag = 0, i = 0;

	if( !(src = map_id2bl(id)) ) {
		ShowDebug("skill_castend_id: src == NULL (tid=%d, id=%d)\n",tid,id);
		return 0; //Not found
	}

	if( !(ud = unit_bl2ud(src)) ) {
		ShowDebug("skill_castend_id: ud == NULL (tid=%d, id=%d)\n",tid,id);
		return 0;
	}

	sd = BL_CAST(BL_PC,src);
	md = BL_CAST(BL_MOB,src);
	hd = BL_CAST(BL_HOM,src);

	sc = status_get_sc(src);

	if( !src->prev ) {
		ud->skilltimer = INVALID_TIMER;
		return 0;
	}

	if( ud->skill_id != SA_CASTCANCEL && ud->skill_id != SO_SPELLFIST ) { //Otherwise handled in unit_skillcastcancel()
		if( ud->skilltimer != tid ) {
			ShowError("skill_castend_id: Timer mismatch %d!=%d!\n",ud->skilltimer,tid);
			ud->skilltimer = INVALID_TIMER;
			return 0;
		}
		ud->skilltimer = INVALID_TIMER;
		if( (sd && pc_checkskill(sd,SA_FREECAST) > 0) || ud->skill_id == LG_EXEEDBREAK )
			status_calc_bl(src,SCB_SPEED|SCB_ASPD); //Restore original walk speed
	}

	if( ud->skilltarget == id )
		target = src;
	else
		target = map_id2bl(ud->skilltarget);

	tsc = status_get_sc(target);

	do { //Use a do so that you can break out of it when the skill fails
		if( !target || !target->prev )
			break;
		if( src->m != target->m || status_isdead(src) )
			break;
		switch( ud->skill_id ) {
			//These should become skill_castend_pos
			case WE_CALLPARTNER:
				if( sd )
					clif_callpartner(sd);
			//Fall through
			case WE_CALLPARENT:
				if( sd ) {
					struct map_session_data *f_sd = pc_get_father(sd);
					struct map_session_data *m_sd = pc_get_mother(sd);

					if( (f_sd && f_sd->state.autotrade) || (m_sd && m_sd->state.autotrade) )
						break;
				}
			//Fall through
			case WE_CALLBABY:
				if( sd ) {
					struct map_session_data *c_sd = pc_get_child(sd);

					if( c_sd && c_sd->state.autotrade )
						break;
				}
			//Fall through
			case AM_RESURRECTHOMUN:
				//Find a random spot to place the skill [Skotlex]
				inf2 = skill_get_splash(ud->skill_id,ud->skill_lv);
				ud->skillx = target->x + inf2;
				ud->skilly = target->y + inf2;
				if( inf2 && !map_random_dir(target,&ud->skillx,&ud->skilly) ) {
					ud->skillx = target->x;
					ud->skilly = target->y;
				}
				ud->skilltimer = tid;
				return skill_castend_pos(tid,tick,id,data);
			case PF_SPIDERWEB:
			case GN_WALLOFTHORN:
			case SU_CN_POWDERING:
			case MH_SUMMON_LEGION:
				ud->skillx = target->x;
				ud->skilly = target->y;
				ud->skilltimer = tid;
				return skill_castend_pos(tid,tick,id,data);
			case RL_HAMMER_OF_GOD:
				if( (sc = status_get_sc(target)) && sc->data[SC_C_MARKER] ) {
					ud->skillx = target->x;
					ud->skilly = target->y;
				} else {
					inf2 = rnd()%AREA_SIZE + 1;
					ud->skillx = src->x + inf2;
					ud->skilly = src->y + inf2;
					if( !map_random_dir(src,&ud->skillx,&ud->skilly) ) {
						ud->skillx = target->x;
						ud->skilly = target->y;
					}
				}
				ud->skilltimer = tid;
				return skill_castend_pos(tid,tick,id,data);
		}
		if( target->id != src->id && (status_bl_has_mode(target,MD_SKILL_IMMUNE) || (status_get_class(target) == MOBID_EMPERIUM &&
			!(skill_get_inf3(ud->skill_id)&INF3_HIT_EMP))) && skill_get_casttype(ud->skill_id) == CAST_NODAMAGE )
			break;
		//Check target validity
		inf = skill_get_inf(ud->skill_id);
		inf2 = skill_get_inf2(ud->skill_id);
		inf3 = skill_get_inf3(ud->skill_id);
		if( (inf&INF_ATTACK_SKILL) || //Offensive skill
			((inf&INF_SELF_SKILL) && (inf2&INF2_NO_TARGET_SELF)) ) //Combo skills
			inf = BCT_ENEMY;
		else if( inf2&INF2_NO_ENEMY )
			inf = BCT_NOENEMY;
		else
			inf = BCT_NOONE;
		if( inf2&(INF2_PARTY_ONLY|INF2_GUILD_ONLY) && target->id != src->id ) {
			inf |=
				(inf2&INF2_PARTY_ONLY ? BCT_PARTY : BCT_NOONE)|
				(inf2&INF2_GUILD_ONLY ? BCT_GUILD : BCT_NOONE);
			inf &= ~BCT_NEUTRAL; //Remove neutral targets (but allow enemy if skill is designed to be so)
		}
		if( ud->skill_id == SC_SHADOWFORM || //In official Shadow Form target is all [exneval]
			ud->skill_id == WM_DEADHILLHERE )
			inf = BCT_ALL;
		if( inf && battle_check_target(src,target,inf) <= 0 ) {
			bool check_target = true;

			switch( ud->skill_id ) {
				case PR_LEXDIVINA:
				case MER_LEXDIVINA:
					if( tsc && tsc->data[SC_SILENCE] )
						check_target = false;
					break;
				case MO_EXTREMITYFIST:
					check_target = false; //Check it later
					break;
			}
			if( check_target ) {
				if( sd )
					clif_skill_fail(sd,ud->skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
		}
		if( (inf&BCT_ENEMY) && tsc && tsc->data[SC_FOGWALL] && rnd()%100 < 75 )
			break; //Fogwall makes all offensive-type targeted skills fail at 75%
		if( sc && sc->count ) {
			bool skill_block = false;

			if( (sc->data[SC_BITE] && (inf3&INF3_BITE_BLOCK)) ||
				(sc->data[SC_ASH] && rnd()%100 < 50) ||
				(sc->data[SC_KYOMU] && rnd()%100 < 5 * sc->data[SC_KYOMU]->val1) ||
				(sc->data[SC_KAGEHUMI] && (inf3&INF3_KAGEHUMI_BL)) )
				skill_block = true;
			if( skill_block ) {
				if( sd )
					clif_skill_fail(sd,ud->skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
		}
		if( tid != INVALID_TIMER && !status_check_skilluse(src,target,ud->skill_id,1) )
			break; //Avoid doing double checks for instant-cast skills
		if( md ) {
			md->last_thinktime = tick + MIN_MOBTHINKTIME;
			if( md->skill_idx >= 0 && md->db->skill[md->skill_idx].emotion >= 0 )
				clif_emotion(src,md->db->skill[md->skill_idx].emotion);
		}
		if( target->id != src->id && battle_config.skill_add_range &&
			!check_distance_bl(src,target,skill_get_range2(src,ud->skill_id,ud->skill_lv,true) + battle_config.skill_add_range) ) {
			if( sd ) {
				clif_skill_fail(sd,ud->skill_id,USESKILL_FAIL_LEVEL,0,0);
				if( battle_config.skill_out_range_consume ) //Consume items anyway [Skotlex]
					skill_consume_requirement(sd,ud->skill_id,ud->skill_lv,3);
			}
			break;
		}
#ifdef OFFICIAL_WALKPATH
		if( skill_get_casttype(ud->skill_id) != CAST_NODAMAGE && !path_search_long(NULL,src->m,src->x,src->y,target->x,target->y,CELL_CHKWALL) ) {
			if( sd ) {
				clif_skill_fail(sd,ud->skill_id,USESKILL_FAIL_LEVEL,0,0);
				skill_consume_requirement(sd,ud->skill_id,ud->skill_lv,3); //Consume items anyway
			}
			break;
		}
#endif
		if( (inf2&INF2_NO_NEARNPC) && skill_isNotOk_npcRange(src,ud->skill_id,ud->skill_lv,target->x,target->y) ) {
			if( sd )
				clif_skill_fail(sd,ud->skill_id,USESKILL_FAIL_POS,0,0);
			break; //Fail if the targeted skill is near NPC [Cydh]
		}
		if( sd ) {
			if( !skill_check_condition_castend(sd,ud->skill_id,ud->skill_lv) )
				break;
			else if( target->id != src->id && (status_bl_has_mode(target,MD_SKILL_IMMUNE) ||
				(status_get_class(target) == MOBID_EMPERIUM && !(skill_get_inf3(ud->skill_id)&INF3_HIT_EMP))) &&
				skill_get_casttype(ud->skill_id) == CAST_DAMAGE )
			{
				clif_skill_fail(sd,ud->skill_id,USESKILL_FAIL_LEVEL,0,0);
				break;
			}
		}
		if( (src->type == BL_MER || src->type == BL_HOM) && !skill_check_condition_mercenary(src,ud->skill_id,ud->skill_lv,1) )
			break;
		if( ud->state.running && ud->skill_id == TK_JUMPKICK ) {
			ud->state.running = 0;
			status_change_end(src,SC_RUN,INVALID_TIMER);
			flag = 1;
		}
		if( ud->walktimer != INVALID_TIMER && ud->skill_id != TK_RUN && ud->skill_id != RA_WUGDASH )
			unit_stop_walking(src,USW_FIXPOS);
		if( sd ) {
			switch( ud->skill_id ) {
				case GS_DESPERADO:
				case RL_FIREDANCE:
					sd->canequip_tick = tick + skill_get_time(ud->skill_id,ud->skill_lv);
					break;
				case CR_GRANDCROSS:
				case NPC_GRANDDARKNESS:
					if( sc && sc->data[SC_STRIPSHIELD] ) {
						const struct TimerData *timer = get_timer(sc->data[SC_STRIPSHIELD]->timer);

						if( timer && timer->func == status_change_timer && DIFF_TICK(timer->tick,gettick() + skill_get_time(ud->skill_id,ud->skill_lv)) > 0 )
							break;
					}
					sc_start2(src,src,SC_STRIPSHIELD,100,0,1,skill_get_time(ud->skill_id,ud->skill_lv));
					break;
			}
		}
		if( skill_get_state(ud->skill_id) != ST_MOVE_ENABLE )
			unit_set_walkdelay(src,tick,battle_config.default_walk_delay + skill_get_walkdelay(ud->skill_id,ud->skill_lv),1);
		if( battle_config.skill_log && battle_config.skill_log&src->type )
			ShowInfo("Type %d, ID %d skill castend id [id =%d, lv=%d, target ID %d]\n",src->type,src->id,ud->skill_id,ud->skill_lv,target->id);
		map_freeblock_lock();
		if( skill_get_casttype(ud->skill_id) == CAST_NODAMAGE )
			i = skill_castend_nodamage_id(src,target,ud->skill_id,ud->skill_lv,tick,flag);
		else
			i = skill_castend_damage_id(src,target,ud->skill_id,ud->skill_lv,tick,flag);
		if( !i ) {
			if( !sd || sd->skillitem != ud->skill_id || skill_get_delay(ud->skill_id,ud->skill_lv) )
				ud->canact_tick = max(tick + skill_delayfix(src,ud->skill_id,ud->skill_lv),ud->canact_tick - SECURITY_CASTTIME);
			if( battle_config.display_status_timers )
				clif_status_change(src,SI_POSTDELAY,1,skill_delayfix(src,ud->skill_id,ud->skill_lv),0,0,0);
			if( sd ) {
				if( (inf = skill_cooldownfix(src,ud->skill_id,ud->skill_lv)) > 0 )
					skill_blockpc_start(sd,ud->skill_id,inf);
				if( ud->skill_id != HP_BASILICA )
					skill_consume_requirement(sd,ud->skill_id,ud->skill_lv,1);
				skill_onskillusage(sd,target,ud->skill_id,tick);
			}
			if( hd && (inf = skill_cooldownfix(src,ud->skill_id,ud->skill_lv)) > 0 )
				skill_blockhomun_start(hd,ud->skill_id,inf);
		}
		if( sc && sc->count ) {
			if( sc->data[SC_SPIRIT] &&
				sc->data[SC_SPIRIT]->val2 == SL_WIZARD &&
				sc->data[SC_SPIRIT]->val3 == ud->skill_id && ud->skill_id != WZ_WATERBALL )
				sc->data[SC_SPIRIT]->val3 = 0; //Clear bounced spell check
			if( sc->data[SC_DANCING] && (skill_get_inf2(ud->skill_id)&INF2_SONG_DANCE) && sd )
				skill_blockpc_start(sd,BD_ADAPTATION,3000);
		}
		if( ud->skill_id != RA_CAMOUFLAGE )
			status_change_end(src,SC_CAMOUFLAGE,INVALID_TIMER);
		if( (ud->skill_id == RL_QD_SHOT || !(skill_get_inf(ud->skill_id)&INF_SELF_SKILL)) &&
			target->id != src->id && ud->skill_id != KO_GENWAKU )
			unit_setdir(src,map_calc_dir(src,target->x,target->y));
		if( sd && ud->skill_id != SA_ABRACADABRA && ud->skill_id != WM_RANDOMIZESPELL )
			sd->skillitem = sd->skillitemlv = sd->skilliteminf = 0; //They just set the data so leave it as it is [Inkfish]
		if( ud->skilltimer == INVALID_TIMER ) {
			if( md )
				md->skill_idx = -1;
			else //Mobs can't clear this one as it is used for skill condition 'afterskill'
				ud->skill_id = 0;
			ud->skill_lv = ud->skilltarget = 0;
		}
		map_freeblock_unlock();
		return 1;
	} while( 0 );

	if( !sd || sd->skillitem != ud->skill_id || skill_get_delay(ud->skill_id,ud->skill_lv) )
		ud->canact_tick = tick;

	ud->skill_id = ud->skill_lv = ud->skilltarget = 0;

	//You can't place a skill failed packet here because it would be
	//sent in ALL cases, even cases where skill_check_condition fails
	//which would lead to double 'skill failed' messages u.u [Skotlex]
	if( sd )
		sd->state.abra_flag = sd->skillitem = sd->skillitemlv = sd->skilliteminf = 0;
	else if( md )
		md->skill_idx = -1;
	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
TIMER_FUNC(skill_castend_pos)
{
	struct block_list *src = map_id2bl(id);
	struct map_session_data *sd;
	struct unit_data *ud = unit_bl2ud(src);
	struct mob_data *md;
	struct homun_data *hd;
	struct status_change *sc = status_get_sc(src);
	int inf, i = 0;

	nullpo_ret(ud);

	sd = BL_CAST(BL_PC,src);
	md = BL_CAST(BL_MOB,src);
	hd = BL_CAST(BL_HOM,src);

	if( !src->prev ) {
		ud->skilltimer = INVALID_TIMER;
		return 0;
	}

	if( ud->skilltimer != tid ) {
		ShowError("skill_castend_pos: Timer mismatch %d!=%d\n",ud->skilltimer,tid);
		ud->skilltimer = INVALID_TIMER;
		return 0;
	}

	ud->skilltimer = INVALID_TIMER;

	if( (sd && pc_checkskill(sd,SA_FREECAST) > 0) || ud->skill_id == LG_EXEEDBREAK )
		status_calc_bl(src,SCB_SPEED|SCB_ASPD); //Restore original walk speed

	do {
		if( status_isdead(src) )
			break;
		if( !skill_pos_maxcount_check(src,ud->skillx,ud->skilly,ud->skill_id,ud->skill_lv,src->type,true) )
			break;
		if( sc && ((sc->data[SC_ASH] && rnd()%100 < 50) || (sc->data[SC_KYOMU] && rnd()%100 < 5 * sc->data[SC_KYOMU]->val1)) ) {
			if( sd )
				clif_skill_fail(sd,ud->skill_id,USESKILL_FAIL_LEVEL,0,0);
			break;
		}
		if( tid != INVALID_TIMER ) { //Avoid double checks on instant cast skills [Skotlex]
			if( !status_check_skilluse(src,NULL,ud->skill_id,1) )
				break;
			if( battle_config.skill_add_range &&
				!check_distance_blxy(src,ud->skillx,ud->skilly,skill_get_range2(src,ud->skill_id,ud->skill_lv,true) + battle_config.skill_add_range) ) {
				if( sd && battle_config.skill_out_range_consume ) //Consume items anyway
					skill_consume_requirement(sd,ud->skill_id,ud->skill_lv,3);
				break;
			}
		}
		if( skill_get_inf2(ud->skill_id)&INF2_NO_NEARNPC && skill_isNotOk_npcRange(src,ud->skill_id,ud->skill_lv,ud->skillx,ud->skilly) ) {
			if( sd )
				clif_skill_fail(sd,ud->skill_id,USESKILL_FAIL_POS,0,0);
			break; //Fail if the targeted skill is near NPC [Cydh]
		}
		if( ud->skill_id == SA_LANDPROTECTOR )
			clif_skill_poseffect(src,ud->skill_id,ud->skill_lv,ud->skillx,ud->skilly,tick);
		if( sd && ud->skill_id != AL_WARP && !skill_check_condition_castend(sd,ud->skill_id,ud->skill_lv) )
			break;
		if( (src->type == BL_MER || src->type == BL_HOM) && !skill_check_condition_mercenary(src,ud->skill_id,ud->skill_lv,1) )
			break;
		if( md ) {
			md->last_thinktime = tick + MIN_MOBTHINKTIME;
			if( md->skill_idx >= 0 && md->db->skill[md->skill_idx].emotion >= 0 )
				clif_emotion(src,md->db->skill[md->skill_idx].emotion);
		}
		if( battle_config.skill_log && battle_config.skill_log&src->type )
			ShowInfo("Type %d, ID %d skill castend pos [id=%d, lv=%d, (%d,%d)]\n",src->type,src->id,ud->skill_id,ud->skill_lv,ud->skillx,ud->skilly);
		if( ud->walktimer != INVALID_TIMER )
			unit_stop_walking(src,USW_FIXPOS);
//		if( sd ) {
//			switch( ud->skill_id ) {
//				case ????:
//					sd->canequip_tick = tick + ????;
//					break;
//			}
//		}
		unit_set_walkdelay(src,tick,battle_config.default_walk_delay + skill_get_walkdelay(ud->skill_id,ud->skill_lv),1);
		map_freeblock_lock();
		if( src->val1 ) //Reset this every time placed a skill [exneval]
			src->val1 = 0;
		i = skill_castend_pos2(src,ud->skillx,ud->skilly,ud->skill_id,ud->skill_lv,tick,0);
		if( !i ) {
			if( !sd || sd->skillitem != ud->skill_id || skill_get_delay(ud->skill_id,ud->skill_lv) )
				ud->canact_tick = max(tick + skill_delayfix(src,ud->skill_id,ud->skill_lv),ud->canact_tick - SECURITY_CASTTIME);
			if( battle_config.display_status_timers )
				clif_status_change(src,SI_POSTDELAY,1,skill_delayfix(src,ud->skill_id,ud->skill_lv),0,0,0);
			if( sd ) {
				if( (inf = skill_cooldownfix(src,ud->skill_id,ud->skill_lv)) > 0 )
					skill_blockpc_start(sd,ud->skill_id,inf);
				skill_consume_requirement(sd,ud->skill_id,ud->skill_lv,1);
				skill_onskillusage(sd,NULL,ud->skill_id,tick);
			}
			if( hd && (inf = skill_cooldownfix(src,ud->skill_id,ud->skill_lv)) > 0 )
				skill_blockhomun_start(hd,ud->skill_id,inf);
		}
		status_change_end(src,SC_CAMOUFLAGE,INVALID_TIMER);
		if( !(skill_get_inf(ud->skill_id)&INF_SELF_SKILL) && ud->skill_id != NJ_SHADOWJUMP && ud->skill_id != SU_LOPE )
			unit_setdir(src,map_calc_dir(src,ud->skillx,ud->skilly));
		if( sd && sd->skillitem != AL_WARP )
			sd->skillitem = sd->skillitemlv = sd->skilliteminf = 0; //Warp-Portal through items will clear data in skill_castend_map [Inkfish]
		if( ud->skilltimer == INVALID_TIMER ) {
			if( md )
				md->skill_idx = -1;
			else //Non mobs can't clear this one as it is used for skill condition 'afterskill'
				ud->skill_id = 0;
			ud->skill_lv = ud->skillx = ud->skilly = 0;
		}
		map_freeblock_unlock();
		return 1;
	} while( 0 );

	if( !sd || sd->skillitem != ud->skill_id || skill_get_delay(ud->skill_id,ud->skill_lv) )
		ud->canact_tick = tick;

	ud->skill_id = ud->skill_lv = 0;

	if( sd )
		sd->state.abra_flag = sd->skillitem = sd->skillitemlv = sd->skilliteminf = 0;
	else if( md )
		md->skill_idx = -1;
	return 0;
}

//Skill count without self
static int skill_count_wos(struct block_list *bl, va_list ap) {
	struct block_list *src = va_arg(ap, struct block_list *);

	if( bl->id != src->id )
		return 1;
	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
int skill_castend_pos2(struct block_list *src, int x, int y, uint16 skill_id, uint16 skill_lv, unsigned int tick, int flag)
{
	struct map_session_data *sd;
	struct status_change *sc;
	struct status_change_entry *sce;
	struct skill_unit_group *group;
	enum sc_type type;
	int i;

	//if( skill_lv <= 0 )
		//return 0;
	if( skill_id > 0 && !skill_lv )
		return 0; //Celest

	nullpo_ret(src);

	if( status_isdead(src) )
		return 0;

	sd = BL_CAST(BL_PC,src);
	sc = status_get_sc(src);
	type = status_skill2sc(skill_id);
	sce = (sc && type != SC_NONE) ? sc->data[type] : NULL;

	switch (skill_id) { //Skill effect
		case WZ_METEOR:
		case WZ_ICEWALL:
		case MO_BODYRELOCATION:
		case SA_LANDPROTECTOR:
		case NJ_BAKUENRYU:
		case PA_GOSPEL:
		case CR_SLIMPITCHER:
		case CR_CULTIVATION:
		case HW_GANBANTEIN:
		case NJ_SHADOWJUMP:
		case RK_WINDCUTTER:
		case WL_EARTHSTRAIN:
		case NC_MAGICDECOY:
		case SC_CHAOSPANIC:
		case SC_FEINTBOMB:
		case LG_EARTHDRIVE:
		case RL_FALLEN_ANGEL:
		case RL_FIRE_RAIN:
		case SU_CN_METEOR:
		case SU_LOPE:
		case EL_FIRE_MANTLE:
		case EL_WATER_BARRIER:
		case EL_ZEPHYR:
		case EL_POWER_OF_GAIA:
			break; //Effect is displayed on respective switch case
		default:
			if( skill_get_inf(skill_id)&INF_SELF_SKILL )
				clif_skill_nodamage(src,src,skill_id,skill_lv,1);
			else
				clif_skill_poseffect(src,skill_id,skill_lv,x,y,tick);
			break;
	}

	switch(skill_id) {
		case PR_BENEDICTIO:
			skill_area_temp[1] = src->id;
			i = skill_get_splash(skill_id,skill_lv);
			map_foreachinallarea(skill_area_sub,src->m,x-i,y-i,x+i,y+i,BL_PC,src,
				skill_id,skill_lv,tick,flag|BCT_ALL|1,skill_castend_nodamage_id);
			map_foreachinallarea(skill_area_sub,src->m,x-i,y-i,x+i,y+i,BL_CHAR,src,
				skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_damage_id);
			break;

		case BS_HAMMERFALL:
			i = skill_get_splash(skill_id,skill_lv);
			map_foreachinallarea(skill_area_sub,src->m,x-i,y-i,x+i,y+i,BL_CHAR,src,
				skill_id,skill_lv,tick,flag|BCT_ENEMY|2,skill_castend_nodamage_id);
			break;

		case HT_DETECTING:
			i = skill_get_splash(skill_id,skill_lv);
			map_foreachinallarea(status_change_timer_sub,src->m,x-i,y-i,x+i,y+i,BL_CHAR,src,NULL,SC_CONCENTRATE,tick);
			skill_reveal_trap_inarea(src,i,x,y);
			break;

		case NPC_LEX_AETERNA:
			i = skill_get_splash(skill_id,skill_lv);
			map_foreachinallarea(skill_area_sub,src->m,x-i,y-i,x+i,y+i,BL_CHAR,src,
				PR_LEXAETERNA,1,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
			break;

		case SA_VOLCANO:
		case SA_DELUGE:
		case SA_VIOLENTGALE:
			if( (group = skill_locate_element_field(src)) && (group->skill_id == SA_VOLCANO ||
				group->skill_id == SA_DELUGE || group->skill_id == SA_VIOLENTGALE) ) {
				if( group->limit - DIFF_TICK(tick,group->tick) > 0 ) {
					skill_unitsetting(src,skill_id,skill_lv,x,y,0);
					return 0; //Does not consumes if the skill is already active [Skotlex]
				} else
					group->limit = 0; //Disable it
			}
			skill_unitsetting(src,skill_id,skill_lv,x,y,0);
			break;

		//Skill Unit Setting
		case MG_SAFETYWALL: {
				int dummy = 1;

				if( map_foreachincell(skill_cell_overlap,src->m,x,y,BL_SKILL,skill_id,&dummy,src) ) {
					skill_unitsetting(src,skill_id,skill_lv,x,y,0);
					return 0; //Don't consume gems if cast on LP
				}
			}
		//Fall through
		case MG_FIREWALL:
		case MG_THUNDERSTORM:
		case AL_PNEUMA:
		case WZ_FIREPILLAR:
		case WZ_QUAGMIRE:
		case WZ_VERMILION:
		case WZ_STORMGUST:
		case WZ_HEAVENDRIVE:
		case PR_SANCTUARY:
		case PR_MAGNUS:
		case CR_GRANDCROSS:
		case NPC_GRANDDARKNESS:
		case HT_SKIDTRAP:
		case MA_SKIDTRAP:
		case HT_LANDMINE:
		case MA_LANDMINE:
		case HT_ANKLESNARE:
		case HT_SHOCKWAVE:
		case HT_SANDMAN:
		case MA_SANDMAN:
		case HT_FLASHER:
		case HT_FREEZINGTRAP:
		case MA_FREEZINGTRAP:
		case HT_BLASTMINE:
		case HT_CLAYMORETRAP:
		case AS_VENOMDUST:
		case AM_DEMONSTRATION:
		case PF_FOGWALL:
		case PF_SPIDERWEB:
		case HT_TALKIEBOX:
		case WE_CALLPARTNER:
		case WE_CALLPARENT:
		case WE_CALLBABY:
		case SA_LANDPROTECTOR:
		case BD_LULLABY:
		case BD_RICHMANKIM:
		case BD_ETERNALCHAOS:
		case BD_DRUMBATTLEFIELD:
		case BD_RINGNIBELUNGEN:
		case BD_ROKISWEIL:
		case BD_INTOABYSS:
		case BD_SIEGFRIED:
		case BA_DISSONANCE:
		case BA_POEMBRAGI:
		case BA_WHISTLE:
		case BA_ASSASSINCROSS:
		case BA_APPLEIDUN:
		case DC_UGLYDANCE:
		case DC_HUMMING:
		case DC_DONTFORGETME:
		case DC_FORTUNEKISS:
		case DC_SERVICEFORYOU:
		case CG_MOONLIT:
		case GS_DESPERADO:
#ifdef RENEWAL
		case NJ_HUUMA:
#endif
		case NJ_KAENSIN:
		case NJ_BAKUENRYU:
		case NJ_SUITON:
		case NJ_HYOUSYOURAKU:
		case NJ_RAIGEKISAI:
		case NJ_KAMAITACHI:
		case NPC_EARTHQUAKE:
		case NPC_EVILLAND:
		case NPC_VENOMFOG:
		case NPC_COMET:
		case NPC_ICEMINE:
		case NPC_FLAMECROSS:
		case NPC_HELLBURNING:
		case NPC_WIDESUCK:
		case NPC_REVERBERATION:
		case WL_COMET:
		case RA_ELECTRICSHOCKER:
		case RA_CLUSTERBOMB:
		case RA_MAGENTATRAP:
		case RA_COBALTTRAP:
		case RA_MAIZETRAP:
		case RA_VERDURETRAP:
		case RA_FIRINGTRAP:
		case RA_ICEBOUNDTRAP:
		case SC_MANHOLE:
		case SC_DIMENSIONDOOR:
		case SC_MAELSTROM:
		case SC_BLOODYLUST:
		case WM_REVERBERATION:
		case WM_POEMOFNETHERWORLD:
		case SO_PSYCHIC_WAVE:
		case SO_WARMER:
		case SO_VACUUM_EXTREME:
		case SO_FIRE_INSIGNIA:
		case SO_WATER_INSIGNIA:
		case SO_WIND_INSIGNIA:
		case SO_EARTH_INSIGNIA:
		case GN_THORNS_TRAP:
		case GN_DEMONIC_FIRE:
		case GN_HELLS_PLANT:
		case SJ_BOOKOFCREATINGSTAR:
		case KO_ZENKAI:
		case MH_LAVA_SLIDE:
		case MH_VOLCANIC_ASH:
		case MH_POISON_MIST:
		case MH_STEINWAND:
		case LG_KINGS_GRACE:
			flag |= 1; //Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete)
		case GS_GROUNDDRIFT: //Ammo should be deleted right away
			skill_unitsetting(src,skill_id,skill_lv,x,y,0);
			break;

		case WZ_ICEWALL:
			if( skill_unitsetting(src,skill_id,skill_lv,x,y,0) ) {
				clif_skill_poseffect(src,skill_id,skill_lv,x,y,tick);
				map_setcell(src->m,x,y,CELL_NOICEWALL,true);
			}
			break;

		case RG_GRAFFITI: //Graffiti [Valaris]
			skill_clear_unitgroup(src);
			skill_unitsetting(src,skill_id,skill_lv,x,y,0);
			break;

		case HP_BASILICA:
			if( sce ) {
				status_change_end(src,SC_BASILICA,INVALID_TIMER);
				return 0;
			} else {
				skill_clear_unitgroup(src);
				skill_unitsetting(src,skill_id,skill_lv,x,y,0);
				skill_consume_requirement(sd,skill_id,skill_lv,3);
			}
			return 1;

		case CG_HERMODE:
			skill_clear_unitgroup(src);
			if( (group = skill_unitsetting(src,skill_id,skill_lv,x,y,0)) )
				sc_start4(src,src,SC_DANCING,100,skill_id,0,skill_lv,group->group_id,skill_get_time(skill_id,skill_lv));
			break;

		case RG_CLEANER: //[Valaris]
			i = skill_get_splash(skill_id,skill_lv);
			map_foreachinallarea(skill_graffitiremover,src->m,x-i,y-i,x+i,y+i,BL_SKILL);
			break;

		case SU_CN_METEOR:
			if( sd ) {
				if( pc_checkskill(sd,SU_SPIRITOFLAND) > 0 )
					sc_start(src,src,SC_DORAM_SVSP,100,skill_lv,skill_get_time(SU_SPIRITOFLAND,1));
				if( pc_search_inventory(sd,ITEMID_CATNIP_FRUIT) != INDEX_NOT_FOUND )
					skill_id = SU_CN_METEOR2;
			}
		//Fall through
		case WZ_METEOR:
			{
				int area = skill_get_splash(skill_id,skill_lv);
				short tmp_x = 0, tmp_y = 0;

				//Creates a random cell in the splash area
				for( i = 1; i <= skill_get_time(skill_id,skill_lv) / skill_get_unit_interval(skill_id); i++ ) {
					tmp_x = x - area + rnd()%(area * 2 + 1);
					tmp_y = y - area + rnd()%(area * 2 + 1);
					skill_unitsetting(src,skill_id,skill_lv,tmp_x,tmp_y,i * skill_get_unit_interval(skill_id));
				}
				clif_skill_nodamage(src,src,skill_id,skill_lv,1);
			}
			break;

		case AL_WARP:
			if( sd ) {
				clif_skill_warppoint(sd,skill_id,skill_lv,sd->status.save_point.map,
					(skill_lv >= 2) ? sd->status.memo_point[0].map : 0,
					(skill_lv >= 3) ? sd->status.memo_point[1].map : 0,
					(skill_lv >= 4) ? sd->status.memo_point[2].map : 0
				);
			}
			if( sc && sc->data[SC_CURSEDCIRCLE_ATKER] )
				status_change_end(src,SC_CURSEDCIRCLE_ATKER,INVALID_TIMER);
			return 0; //Not to consume items

		case MO_BODYRELOCATION:
			if( unit_movepos(src,x,y,2,true) ) {
#if PACKETVER >= 20111005
				clif_snap(src,src->x,src->y);
#else
				clif_skill_poseffect(src,skill_id,skill_lv,src->x,src->y,tick);
#endif
				if( sd )
					skill_blockpc_start(sd,MO_EXTREMITYFIST,2000);
			}
			break;

		case NJ_SHADOWJUMP:
			if( !map_flag_gvg2(src->m) && !mapdata[src->m].flag.battleground &&
				unit_movepos(src,x,y,1,false) ) //You don't move on GVG grounds
				clif_blown(src,src);
			status_change_end(src,SC_HIDING,INVALID_TIMER);
			clif_skill_nodamage(src,src,skill_id,skill_lv,1);
			break;

		case AM_SPHEREMINE:
		case AM_CANNIBALIZE:
			{
				int summons[5] = { MOBID_G_MANDRAGORA,MOBID_G_HYDRA,MOBID_G_FLORA,MOBID_G_PARASITE,MOBID_G_GEOGRAPHER };
				int mob_id = skill_id == AM_SPHEREMINE ? MOBID_MARINE_SPHERE : summons[skill_lv - 1];
				int ai = (skill_id == AM_SPHEREMINE) ? AI_SPHERE : AI_FLORA;
				struct mob_data *md;

				//Correct info, don't change any of this! [celest]
				md = mob_once_spawn_sub(src,src->m,x,y,status_get_name(src),mob_id,"",SZ_SMALL,ai);
				if( md ) {
					md->master_id = src->id;
					md->special_state.ai = (enum mob_ai)ai;
					if( md->deletetimer != INVALID_TIMER )
						delete_timer(md->deletetimer,mob_timer_delete);
					md->deletetimer = add_timer(gettick() + skill_get_time(skill_id,skill_lv),mob_timer_delete,md->bl.id,0);
					mob_spawn(md); //Now it is ready for spawning
				}
			}
			break;

		case CR_SLIMPITCHER: //Slim Pitcher [Celest]
			if( sd ) {
				int j = 0;
				struct skill_condition req = skill_get_requirement(sd,skill_id,skill_lv);

				i = skill_lv%11 - 1;
				j = pc_search_inventory(sd,req.itemid[i]);
				if( j == INDEX_NOT_FOUND || req.itemid[i] <= 0 || !sd->inventory_data[j] ||
					sd->inventory.u.items_inventory[j].amount < req.amount[i] ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					return 1;
				}
				potion_flag = 1;
				potion_hp = 0;
				potion_sp = 0;
				run_script(sd->inventory_data[j]->script,0,sd->bl.id,0);
				potion_flag = 0;
				//Apply skill bonuses
				i = pc_checkskill(sd,CR_SLIMPITCHER) * 10
					+ pc_checkskill(sd,AM_POTIONPITCHER) * 10
					+ pc_checkskill(sd,AM_LEARNINGPOTION) * 5
					+ pc_skillheal_bonus(sd,skill_id);

				potion_hp = potion_hp * (100 + i) / 100;
				potion_sp = potion_sp * (100 + i) / 100;

				if( potion_hp > 0 || potion_sp > 0 ) {
					i = skill_get_splash(skill_id, skill_lv);
					map_foreachinallarea(skill_area_sub,src->m,x-i,y-i,x+i,y+i,BL_CHAR,src,
						skill_id,skill_lv,tick,flag|BCT_PARTY|BCT_GUILD|1,skill_castend_nodamage_id);
				}
			} else {
				struct item_data *item = itemdb_search(skill_get_itemid(skill_id,skill_lv));

				potion_flag = 1;
				potion_hp = 0;
				potion_sp = 0;
				run_script(item->script,0,src->id,0);
				potion_flag = 0;
				i = skill_get_max(CR_SLIMPITCHER) * 10;

				potion_hp = potion_hp * (100 + i) / 100;
				potion_sp = potion_sp * (100 + i) / 100;

				if( potion_hp > 0 || potion_sp > 0 ) {
					i = skill_get_splash(skill_id,skill_lv);
					map_foreachinallarea(skill_area_sub,src->m,x-i,y-i,x+i,y+i,BL_CHAR,src,
						skill_id,skill_lv,tick,flag|BCT_PARTY|BCT_GUILD|1,skill_castend_nodamage_id);
				}
			}
			clif_skill_poseffect(src,skill_id,skill_lv,x,y,tick);
			break;

		case HW_GANBANTEIN:
			if( rnd()%100 < 80 ) {
				int dummy = 1;

				i = skill_get_splash(skill_id,skill_lv);
				map_foreachinallarea(skill_cell_overlap,src->m,x-i,y-i,x+i,y+i,BL_SKILL,skill_id,&dummy,src);
				clif_skill_poseffect(src,skill_id,skill_lv,x,y,tick);
			} else {
				if( sd ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					skill_consume_requirement(sd,skill_id,skill_lv,2); //Consume gem even if failed
					return 1;
				}
			}
			break;

		case HW_GRAVITATION:
			if( (group = skill_unitsetting(src,skill_id,skill_lv,x,y,0)) )
				sc_start4(src,src,type,100,skill_lv,0,BCT_SELF,group->group_id,skill_get_time(skill_id,skill_lv));
			break;

		case CR_CULTIVATION: //Plant Cultivation [Celest]
			if( sd ) {
				if( map_count_oncell(src->m,x,y,BL_CHAR,0) > 0 ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_POS,0,0);
					return 1;
				}
				if( rnd()%100 < 50 ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					return 1;
				} else {
					TBL_MOB *md = mob_once_spawn_sub(src,src->m,x,y,"--ja--",(skill_lv < 2 ? MOBID_BLACK_MUSHROOM + rnd()%2 : MOBID_RED_PLANT + rnd()%6),"",SZ_SMALL,AI_NONE);

					if( !md )
						break;
					if( (i = skill_get_time(skill_id,skill_lv)) > 0 ) {
						if( md->deletetimer != INVALID_TIMER )
							delete_timer(md->deletetimer,mob_timer_delete);
						md->deletetimer = add_timer (tick + i,mob_timer_delete,md->bl.id,0);
					}
					mob_spawn(md);
					clif_skill_poseffect(src,skill_id,skill_lv,x,y,tick);
				}
			}
			break;

		case SG_SUN_WARM:
		case SG_MOON_WARM:
		case SG_STAR_WARM:
			skill_clear_unitgroup(src);
			if( (group = skill_unitsetting(src,skill_id,skill_lv,src->x,src->y,0)) )
				sc_start4(src,src,type,100,skill_lv,0,0,group->group_id,skill_get_time(skill_id,skill_lv));
			break;

		case PA_GOSPEL:
			if( sce && sce->val4 == BCT_SELF ) {
				status_change_end(src,SC_GOSPEL,INVALID_TIMER);
				break;
			} else {
				if( (group = skill_unitsetting(src,skill_id,skill_lv,src->x,src->y,0)) ) {
					if( sce )
						status_change_end(src,type,INVALID_TIMER); //Was under someone else's Gospel [Skotlex]
					sc_start4(src,src,type,100,skill_lv,0,group->group_id,BCT_SELF,skill_get_time(skill_id,skill_lv));
					clif_skill_poseffect(src,skill_id,skill_lv,0,0,tick); //PA_GOSPEL music packet
				}
			}
			break;

		case NJ_TATAMIGAESHI:
			if( skill_unitsetting(src,skill_id,skill_lv,src->x,src->y,0) )
				sc_start(src,src,type,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;

		case AM_RESURRECTHOMUN:	//[orn]
			if( sd && !hom_ressurect(sd,20 * skill_lv,x,y) )
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			break;

		case RK_WINDCUTTER:
			clif_skill_damage(src,src,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
		//Fall through
		case AC_SHOWER:
			if( skill_id == AC_SHOWER ) {
				status_change_end(src,SC_CAMOUFLAGE,INVALID_TIMER);
				//Cast center might be relevant later (e.g. for knockback direction)
				skill_area_temp[4] = x;
				skill_area_temp[5] = y;
			}
		//Fall though
		case MA_SHOWER:
		case RK_DRAGONBREATH:
		case RK_DRAGONBREATH_WATER:
		case NC_COLDSLOWER:
		case LG_RAYOFGENESIS:
		case SO_EARTHGRAVE:
		case SO_DIAMONDDUST:
		case RL_HAMMER_OF_GOD:
		case KO_BAKURETSU:
		case KO_HUUMARANKA:
		case MH_XENO_SLASHER:
			skill_area_temp[1] = 0;
			i = skill_get_splash(skill_id,skill_lv);
			map_foreachinarea(skill_area_sub,src->m,x-i,y-i,x+i,y+i,BL_CHAR|BL_SKILL,src,
				skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_damage_id);
			break;

		case KO_MUCHANAGE:
		case SR_RIDEINLIGHTNING:
		case WM_GREAT_ECHO:
		case WM_SOUND_OF_DESTRUCTION:
			i = skill_get_splash(skill_id,skill_lv);
			if( skill_id == KO_MUCHANAGE ) {
				battle_damage_temp[0] = rnd_value(50,100);
				skill_area_temp[0] = map_foreachinallarea(skill_area_sub,src->m,x-i,y-i,x+i,y+i,BL_CHAR,src,
					skill_id,skill_lv,tick,BCT_ENEMY,skill_area_sub_count);
			}
			map_foreachinallarea(skill_area_sub,src->m,x-i,y-i,x+i,y+i,BL_CHAR,src,
				skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_damage_id);
			break;

		case GC_POISONSMOKE:
			skill_unitsetting(src,skill_id,skill_lv,x,y,flag);
			clif_skill_damage(src,src,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			break;

		case AB_EPICLESIS:
			if( !map_flag_vs(src->m) ) {
				i = skill_get_splash(skill_id,skill_lv);
				map_foreachinallarea(skill_area_sub,src->m,x-i,y-i,x+i,y+i,BL_CHAR,src,
					ALL_RESURRECTION,3,tick,flag|BCT_NOENEMY|1,skill_castend_nodamage_id);
			}
			skill_unitsetting(src,skill_id,skill_lv,x,y,0);
			break;

		case WL_EARTHSTRAIN: {
				int wave = skill_lv + 4, dir = map_calc_dir(src,x,y);
				int sx = x = src->x, sy = y = src->y; //Store first caster's location to avoid glitch on unit setting

				for( i = 1; i <= wave; i++ ) {
					switch( dir ) {
						case DIR_NORTH: case DIR_NORTHWEST: case DIR_NORTHEAST: sy = y + i; break;
						case DIR_SOUTHWEST: case DIR_SOUTH: case DIR_SOUTHEAST: sy = y - i; break;
						case DIR_WEST: sx = x - i; break;
						case DIR_EAST: sx = x + i; break;
					}
					skill_addtimerskill(src,tick + i * 200,0,sx,sy,skill_id,skill_lv,0,flag);
					//skill_addtimerskill(src,tick + i * 140,0,sx,sy,skill_id,skill_lv,0,flag);
				}
			}
			break;

		case RA_DETONATOR:
			i = skill_get_splash(skill_id,skill_lv);
			map_foreachinallarea(skill_detonator,src->m,x-i,y-i,x+i,y+i,BL_SKILL,src);
			clif_skill_damage(src,src,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
			break;

		case NC_NEUTRALBARRIER:
		case NC_STEALTHFIELD:
			skill_clear_unitgroup(src); //To remove previous skills, can't use both
			if( (group = skill_unitsetting(src,skill_id,skill_lv,src->x,src->y,0)) ) {
				enum sc_type type2 = (skill_id == NC_NEUTRALBARRIER ? SC_NEUTRALBARRIER_MASTER : SC_STEALTHFIELD_MASTER);

				sc_start2(src,src,type2,100,skill_lv,group->group_id,skill_get_time(skill_id,skill_lv));
			}
			break;

		case NC_SILVERSNIPER: {
				struct mob_data *md = mob_once_spawn_sub(src,src->m,x,y,status_get_name(src),MOBID_SILVERSNIPER,"",SZ_SMALL,AI_NONE);

				if( md ) {
					md->master_id = src->id;
					md->special_state.ai = AI_FAW;
					if( md->deletetimer != INVALID_TIMER )
						delete_timer(md->deletetimer,mob_timer_delete);
					md->deletetimer = add_timer(gettick() + skill_get_time(skill_id,skill_lv),mob_timer_delete,md->bl.id,0);
					mob_spawn(md);
				}
			}
			break;

		case NC_MAGICDECOY:
			if( sd && !clif_magicdecoy_list(sd,skill_lv,x,y) )
				return 1;
			clif_skill_poseffect(src,skill_id,skill_lv,x,y,tick);
			break;

		case SC_CHAOSPANIC:
			if( rnd()%100 >= 35 + 15 * skill_lv ) {
				if( sd )
					skill_consume_requirement(sd,skill_id,skill_lv,3);
				return 1;
			}
			skill_unitsetting(src,skill_id,skill_lv,x,y,0);
			clif_skill_poseffect(src,skill_id,skill_lv,x,y,tick);
			break;

		case SC_FEINTBOMB:
			if( (group = skill_unitsetting(src,skill_id,skill_lv,x,y,0)) ) { //Set bomb on current position
				skill_blown(src,src,3 * skill_lv,unit_getdir(src),2);
				map_foreachinallrange(unit_changetarget,src,AREA_SIZE,BL_MOB,src,&group->unit->bl);
				clif_skill_nodamage(src,src,skill_id,skill_lv,
					sc_start(src,src,type,100,skill_lv,skill_get_time(skill_id,skill_lv)));
			} else {
				if( sd )
					skill_consume_requirement(sd,skill_id,skill_lv,3);
				return 1;
			}
			break;

		case SC_ESCAPE:
			skill_unitsetting(src,skill_id,skill_lv,x,y,0);
			skill_blown(src,src,skill_get_blewcount(skill_id,skill_lv),unit_getdir(src),2);
			break;

		case LG_OVERBRAND: {
				int dir = map_calc_dir(src,x,y);
				int sx = src->x, sy = src->y;
				struct s_skill_nounit_layout *layout = skill_get_nounit_layout(skill_id,skill_lv,src,sx,sy,dir);

				for( i = 0; i < layout->count; i++ )
					map_foreachincell(skill_area_sub,src->m,sx+layout->dx[i],sy+layout->dy[i],BL_CHAR,src,skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_ANIMATION|1,skill_castend_damage_id);
				skill_addtimerskill(src,tick + status_get_amotion(src),0,x,y,LG_OVERBRAND_BRANDISH,skill_lv,0,flag);
			}
			break;

		case LG_BANDING:
			if( sc && sc->data[SC_BANDING] )
				skill_clear_unitgroup(src);
			else if( (group = skill_unitsetting(src,skill_id,skill_lv,src->x,src->y,0)) )
				sc_start4(src,src,SC_BANDING,100,skill_lv,0,0,group->group_id,skill_get_time(skill_id,skill_lv));
			break;

		case WM_DOMINION_IMPULSE:
			i = skill_get_splash(skill_id,skill_lv);
			map_foreachinallarea(skill_active_reverberation,src->m,x-i,y-i,x+i,y+i,BL_SKILL);
			break;

		case WM_SEVERE_RAINSTORM:
			if( sd )
				sd->canequip_tick = tick + skill_get_time(skill_id,skill_lv);
			skill_unitsetting(src,skill_id,skill_lv,x,y,0);
			break;

		case SO_CLOUD_KILL:
			if( !skill_unitsetting(src,skill_id,skill_lv,x,y,0) ) {
				if( sd )
					skill_consume_requirement(sd,skill_id,skill_lv,2);
				return 1;
			}
			break;

		case GN_WALLOFTHORN: {
				static const int dx[] = {-2,-2,-2,-2,-2,-1, 0, 1, 2, 2, 2, 2, 2, 1, 0,-1};
				static const int dy[] = { 2, 1, 0,-1,-2,-2,-2,-2,-2,-1, 0, 1, 2, 2, 2, 2};
				struct unit_data *ud = unit_bl2ud(src);

				if( !ud )
					break;
				for( i = 0; i < 16; i++ ) {
					x = ud->skillx + dx[i];
					y = ud->skilly + dy[i];
					skill_unitsetting(src,skill_id,skill_lv,x,y,0);
				}
			}
			break;

		case GN_CRAZYWEED: {
				int area = skill_get_splash(skill_id,skill_lv);

				for( i = 0; i < 3 + (skill_lv / 2); i++ ) {
					int tmp_x = x - area + rnd()%(area * 2 + 1);
					int tmp_y = y - area + rnd()%(area * 2 + 1);

					skill_addtimerskill(src,tick + i * 250,0,tmp_x,tmp_y,GN_CRAZYWEED_ATK,skill_lv,0,flag);
				}
			}
			break;

		case GN_FIRE_EXPANSION: {
				uint8 lv = 0;

				if( (group = skill_locate_element_field(src)) ) {
					int ux = group->val3>>16, uy = group->val3&0xffff;

					if( group->skill_id == GN_DEMONIC_FIRE && distance_xy(x,y,ux,uy) < 3 ) {
						switch( skill_lv ) {
							case 1:
								src->val1 = group->limit - DIFF_TICK(tick,group->tick);
								skill_delunitgroup(group);
								skill_unitsetting(src,group->skill_id,group->skill_lv,ux,uy,1);
								break;
							case 2:
								map_foreachinallarea(skill_area_sub,src->m,ux - 2,uy - 2,ux + 2,uy + 2,BL_CHAR,src,
									GN_DEMONIC_FIRE,skill_lv + 20,tick,flag|BCT_ENEMY|SD_LEVEL|1,skill_castend_damage_id);
								skill_delunitgroup(group);
								break;
							case 3:
								skill_delunitgroup(group);
								skill_unitsetting(src,GN_FIRE_EXPANSION_SMOKE_POWDER,1,ux,uy,0);
								break;
							case 4:
								skill_delunitgroup(group);
								skill_unitsetting(src,GN_FIRE_EXPANSION_TEAR_GAS,1,ux,uy,0);
								break;
							case 5: //If player knows a level of Acid Demonstration greater then 5, that level will be casted
								lv = max((sd ? pc_checkskill(sd,CR_ACIDDEMONSTRATION) : 10),5);
								map_foreachinallarea(skill_area_sub,src->m,ux - 2,uy - 2,ux + 2,uy + 2,BL_CHAR,src,
									GN_FIRE_EXPANSION_ACID,lv,tick,flag|BCT_ENEMY|SD_LEVEL|1,skill_castend_damage_id);
								skill_delunitgroup(group);
								break;
						}
					}
				}
			}
			break;

		case SO_FIREWALK:
		case SO_ELECTRICWALK:
			if( sce )
				status_change_end(src,type,INVALID_TIMER);
			sc_start2(src,src,type,100,skill_id,skill_lv,skill_get_time(skill_id,skill_lv));
			break;

		case SO_ARRULLO:
			i = skill_get_splash(skill_id,skill_lv);
			map_foreachinallarea(skill_area_sub,src->m,x-i,y-i,x+i,y+i,BL_CHAR,src,
				skill_id,skill_lv,tick,flag|BCT_ENEMY|1,skill_castend_nodamage_id);
			break;

		case EL_FIRE_MANTLE:
		case EL_WATER_BARRIER:
		case EL_ZEPHYR:
		case EL_POWER_OF_GAIA:
			{
				struct elemental_data *ed = BL_CAST(BL_ELEM,src);

				if( ed && ed->master ) {
					struct block_list *e_bl = &ed->master->bl;

					clif_skill_damage(src,e_bl,tick,status_get_amotion(src),0,-30000,1,skill_id,skill_lv,DMG_SKILL);
					skill_unitsetting(src,skill_id,skill_lv,e_bl->x,e_bl->y,0);
				}
			}
			break;

		case KO_MAKIBISHI: {
				short area = skill_get_splash(skill_id,skill_lv);
				short castx = x, casty = y; //Player's location on skill use
				short placex = 0, placey = 0; //Where to place the makibishi
				uint8 attempts = 0; //Number of attempts to place the makibishi (Prevents infinite loops)

				for( i = 0; i < 2 + skill_lv; i++ ) {
					//Select a random cell
					placex = x - area + rnd()%(area * 2 + 1);
					placey = y - area + rnd()%(area * 2 + 1);
					//Only place the makibishi if its not on the cell where the player is standing and its not on top of another
					if( !((placex == castx && placey == casty) || skill_check_unit_range(src,placex,placey,skill_id,skill_lv)) )
						skill_unitsetting(src,skill_id,skill_lv,placex,placey,0);
					else if( attempts < 20 ) { //20 attempts is enough (Very rarely do we hit this limit)
						attempts++; //If it tried to place on a spot where the player was standing or where another makibishi is, make another attempt
						i--; //Mark down (The makibishi placement was unsuccessful)
					}
				}
			}
			break;

		case RL_B_TRAP:
			if( !skill_unitsetting(src,skill_id,skill_lv,x,y,0) ) {
				if( sd )
					skill_consume_requirement(sd,skill_id,skill_lv,3);
				return 1;
			}
			break;

		case RL_FALLEN_ANGEL:
			if( unit_movepos(src,x,y,2,true) ) {
				clif_snap(src,src->x,src->y);
				sc_start(src,src,type,100,skill_lv,skill_get_time(skill_id,skill_lv));
			} else if( sd )
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			break;

		case RL_FIRE_RAIN: {
				int dir = map_calc_dir(src,x,y);
				int sx = x, sy = y;

				for( i = 1; i <= 10; i++ ) {
					switch( dir ) {
						case DIR_NORTH: case DIR_NORTHWEST: case DIR_NORTHEAST: sy = y + i; break;
						case DIR_SOUTHWEST: case DIR_SOUTH: case DIR_SOUTHEAST: sy = y - i; break;
						case DIR_WEST: sx = x - i; break;
						case DIR_EAST: sx = x + i; break;
					}
					skill_addtimerskill(src,tick + i * 140,0,sx,sy,skill_id,skill_lv,0,flag);
				}
				clif_skill_damage(src,src,tick,0,status_get_dmotion(src),-30000,1,skill_id,skill_lv,DMG_SPLASH);
				clif_skill_nodamage(src,src,skill_id,skill_lv,1);
			}
			break;

		case NC_MAGMA_ERUPTION:
			i = skill_get_splash(skill_id,skill_lv);
			map_foreachinallarea(skill_area_sub,src->m,x-i,y-i,x+i,y+i,BL_CHAR,src,
				skill_id,skill_lv,tick,flag|BCT_ENEMY|SD_ANIMATION|1,skill_castend_damage_id);
			skill_addtimerskill(src,tick + status_get_amotion(src) * 2,0,x,y,skill_id,skill_lv,0,flag);
			break;

		case SU_CN_POWDERING:
		case SU_NYANGGRASS:
			skill_unitsetting(src,skill_id,skill_lv,x,y,0);
			if( sd && pc_checkskill(sd,SU_SPIRITOFLAND) > 0 ) {
				if( skill_id == SU_CN_POWDERING )
					sc_start(src,src,SC_DORAM_FLEE2,100,skill_lv,skill_get_time(SU_SPIRITOFLAND,1));
				else
					sc_start(src,src,SC_DORAM_MATK,100,skill_lv,skill_get_time(SU_SPIRITOFLAND,1));
			}
			break;

		case SU_LOPE:
			//Fails on noteleport maps, except for GvG and BG maps
			if( mapdata[src->m].flag.noteleport && !(mapdata[src->m].flag.battleground || map_flag_gvg2(src->m)) ) {
				x = src->x;
				y = src->y;
			}
			if( !map_count_oncell(src->m,x,y,BL_PC|BL_NPC|BL_MOB,0) && map_getcell(src->m,x,y,CELL_CHKREACH) && unit_movepos(src,x,y,1,false) )
				clif_blown(src,src);
			clif_skill_nodamage(src,src,skill_id,skill_lv,1);
			break;

		case MH_SUMMON_LEGION: {
				short summons[5] = { MOBID_S_HORNET,MOBID_S_GIANT_HORNET,MOBID_S_GIANT_HORNET,MOBID_S_LUCIOLA_VESPA,MOBID_S_LUCIOLA_VESPA };
				uint8 summon_count = (5 + skill_lv) / 2;
				struct mob_data *sum_md = NULL;

				for( i = 0; i < summon_count; i++ ) {
					sum_md = mob_once_spawn_sub(src,src->m,x,y,status_get_name(src),summons[skill_lv - 1],"",SZ_SMALL,AI_NONE);
					if( sum_md ) {
						sum_md->master_id = src->id;
						sum_md->special_state.ai = AI_LEGION;
						if( sum_md->deletetimer != INVALID_TIMER )
							delete_timer(sum_md->deletetimer,mob_timer_delete);
						sum_md->deletetimer = add_timer(gettick() + skill_get_time(skill_id,skill_lv),mob_timer_delete,sum_md->bl.id,0);
						mob_spawn(sum_md);
					}
				}
			}
			break;

		default:
			ShowWarning("skill_castend_pos2: Unknown skill used:%d\n",skill_id);
			return 1;
	}

	if( sc && sc->data[SC_CURSEDCIRCLE_ATKER] )
		status_change_end(src,SC_CURSEDCIRCLE_ATKER,INVALID_TIMER);

	if( sd ) { //Ensure that the skill last-cast tick is recorded
		sd->canskill_tick = gettick();

		if( sd->state.arrow_atk && !(flag&1) ) //Consume ammo if this is a ground skill
			battle_consume_ammo(sd,skill_id,skill_lv);

		//Perform skill requirement consumption
		skill_consume_requirement(sd,skill_id,skill_lv,2);
	}

	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
int skill_castend_map(struct map_session_data *sd, uint16 skill_id, const char *mapname)
{
	nullpo_ret(sd);

//Simplify skill_failed code
#define skill_failed(sd) { sd->menuskill_id = sd->menuskill_val = 0; }
	if( skill_id != sd->menuskill_id )
		return 0;

	if( !sd->bl.prev || pc_isdead(sd) ) {
		skill_failed(sd);
		return 0;
	}

	if( !status_check_skilluse(&sd->bl,NULL,skill_id,0) ) {
		skill_failed(sd);
		return 0;
	}

	pc_stop_attack(sd);

	if( battle_config.skill_log && battle_config.skill_log&BL_PC )
		ShowInfo("PC %d skill castend skill = %d map = %s\n",sd->bl.id,skill_id,mapname);

	if( !strcmp(mapname,"cancel") ) {
		skill_failed(sd);
		return 0;
	}

	switch( skill_id ) {
		case AL_TELEPORT:
		case ALL_ODINS_RECALL:
			//The storage window is closed automatically by the client
			//When there's any kind of map change, so we need to restore it automatically [bugreport:8027]
			if( !strcmp(mapname,"Random") )
				pc_randomwarp(sd,CLR_TELEPORT);
			else if( sd->menuskill_val > 1 || skill_id == ALL_ODINS_RECALL ) //Need lv2 to be able to warp here
				pc_setpos(sd,sd->status.save_point.map,sd->status.save_point.x,sd->status.save_point.y,CLR_TELEPORT);
			clif_refresh_storagewindow(sd);
			break;

		case AL_WARP: {
				const struct point *p[4];
				struct skill_unit_group *group;
				int i, lv, wx, wy;
				int maxcount = 0;
				int x, y;
				unsigned short mapindex;

				mapindex  = mapindex_name2id((char *)mapname);
				if( !mapindex ) { //Given map not found?
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					skill_failed(sd);
					return 0;
				}

				p[0] = &sd->status.save_point;
				p[1] = &sd->status.memo_point[0];
				p[2] = &sd->status.memo_point[1];
				p[3] = &sd->status.memo_point[2];

				if( (maxcount = skill_get_maxcount(skill_id, sd->menuskill_val)) > 0 ) {
					for( i = 0; i < MAX_SKILLUNITGROUP && sd->ud.skillunit[i] && maxcount; i++ ) {
						if( sd->ud.skillunit[i]->skill_id == skill_id )
							maxcount--;
					}
					if( !maxcount ) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						skill_failed(sd);
						return 0;
					}
				}

				lv = (sd->skillitem == skill_id ? sd->skillitemlv : pc_checkskill(sd,skill_id));
				wx = sd->menuskill_val>>16;
				wy = sd->menuskill_val&0xffff;

				if( lv <= 0 )
					return 0;

				if( lv > 4 )
					lv = 4; //Crash prevention

				//Check if the chosen map exists in the memo list
				ARR_FIND(0, lv, i, mapindex == p[i]->map);
				if( i < lv ) {
					x = p[i]->x;
					y = p[i]->y;
				} else {
					skill_failed(sd);
					return 0;
				}

				if( !skill_check_condition_castend(sd,sd->menuskill_id,lv) ) { //This checks versus skill_id/skill_lv
					skill_failed(sd);
					return 0;
				}

				skill_consume_requirement(sd,sd->menuskill_id,lv,2);
				sd->skillitem = sd->skillitemlv = sd->skilliteminf = 0; //Clear data that's skipped in 'skill_castend_pos' [Inkfish]

				if( (group = skill_unitsetting(&sd->bl,skill_id,lv,wx,wy,0)) == NULL ) {
					skill_failed(sd);
					return 0;
				}

				group->val1 = (group->val1<<16)|(short)0;
				//Record the destination coordinates
				group->val2 = (x<<16)|y;
				group->val3 = mapindex;
			}
			break;
	}

	sd->menuskill_id = sd->menuskill_val = 0;
	return 0;
#undef skill_failed
}

///Transforms 'target' skill unit into dissonance (if conditions are met)
static int skill_dance_overlap_sub(struct block_list *bl, va_list ap)
{
	struct skill_unit *target = (struct skill_unit *)bl;
	struct skill_unit *src = va_arg(ap, struct skill_unit *);
	int flag = va_arg(ap, int);

	if (target == src)
		return 0;

	if (!target->group || !(target->group->state.song_dance&0x1))
		return 0;

	if (!(target->val2&src->val2&~UF_ENSEMBLE)) //They don't match (song + dance) is valid
		return 0;

	if (flag) //Set dissonance
		target->val2 |= UF_ENSEMBLE; //Add ensemble to signal this unit is overlapping
	else //Remove dissonance
		target->val2 &= ~UF_ENSEMBLE;

	skill_getareachar_skillunit_visibilty(target, AREA);

	return 1;
}

//Does the song/dance overlapping -> dissonance check. [Skotlex]
//When flag is 0, this unit is about to be removed, cancel the dissonance effect
//When 1, this unit has been positioned, so start the cancel effect.
int skill_dance_overlap(struct skill_unit *unit, int flag)
{
	if (!unit || !unit->group || !(unit->group->state.song_dance&0x1))
		return 0;

	if (unit->val1 != unit->group->skill_id) { //Reset state
		unit->val1 = unit->group->skill_id;
		unit->val2 &= ~UF_ENSEMBLE;
	}

	return map_foreachincell(skill_dance_overlap_sub, unit->bl.m, unit->bl.x, unit->bl.y, BL_SKILL, unit, flag);
}

/**
 * Converts this group information so that it is handled as a Dissonance or Ugly Dance cell.
 * @param unit Skill unit data (from BA_DISSONANCE or DC_UGLYDANCE)
 * @param flag 0 Convert
 * @param flag 1 Revert
 * @return true success
 * @TODO: This should be completely removed later and rewritten
 *	The entire execution of the overlapping songs instances is dirty and hacked together
 *	Overlapping cells should be checked on unit entry, not infinitely loop checked causing 1000's of executions a song/dance
 */
static bool skill_dance_switch(struct skill_unit *unit, int flag)
{
	static int prevflag = 1; //By default the backup is empty
	static struct skill_unit_group backup;
	struct skill_unit_group *group;

	if( !unit || !(group = unit->group) )
		return false;

	//val2&UF_ENSEMBLE is a hack to indicate dissonance
	if ( !((group->state.song_dance&0x1) && (unit->val2&UF_ENSEMBLE)) )
		return false;

	if( flag == prevflag ) { //Protection against attempts to read an empty backup/write to a full backup
		ShowError("skill_dance_switch: Attempted to %s (skill_id=%d, skill_lv=%d, src_id=%d).\n",
			(flag ? "read an empty backup" : "write to a full backup"),
			group->skill_id, group->skill_lv, group->src_id);
		return false;
	}

	prevflag = flag;

	if( !flag ) { //Transform
		uint16 skill_id = (unit->val2&UF_SONG) ? BA_DISSONANCE : DC_UGLYDANCE;

		//Backup
		backup.skill_id    = group->skill_id;
		backup.skill_lv    = group->skill_lv;
		backup.unit_id     = group->unit_id;
		backup.target_flag = group->target_flag;
		backup.bl_flag     = group->bl_flag;
		backup.interval    = group->interval;

		//Replace
		group->skill_id    = skill_id;
		group->skill_lv    = 1;
		group->unit_id     = skill_get_unit_id(skill_id,0);
		group->target_flag = skill_get_unit_target(skill_id);
		group->bl_flag     = skill_get_unit_bl_target(skill_id);
		group->interval    = skill_get_unit_interval(skill_id);
	} else { //Restore
		group->skill_id    = backup.skill_id;
		group->skill_lv    = backup.skill_lv;
		group->unit_id     = backup.unit_id;
		group->target_flag = backup.target_flag;
		group->bl_flag     = backup.bl_flag;
		group->interval    = backup.interval;
	}

	return true;
}

/**
 * Initializes and sets a ground skill / skill unit. Usually called after skill_casted_pos() or skill_castend_map()
 * @param src Object that triggers the skill
 * @param skill_id Skill ID
 * @param skill_lv Skill level of used skill
 * @param x Position x
 * @param y Position y
 * @param flag &1: Used to determine when the skill 'morphs' (Warp portal becomes active, or Fire Pillar becomes active)
 * @return skill_unit_group
 */
struct skill_unit_group *skill_unitsetting(struct block_list *src, uint16 skill_id, uint16 skill_lv, int16 x, int16 y, int flag)
{
	struct skill_unit_group *group;
	int i, limit, val1 = 0, val2 = 0, val3 = 0;
	int link_group_id = 0;
	int target, bl, interval, range, unit_flag, req_item = 0;
	struct s_skill_unit_layout *layout;
	struct map_session_data *sd;
	struct status_data *status;
	struct status_change *sc;
	int active_flag = 1;
	int subunt = 0;
	bool hidden = false;

	nullpo_retr(NULL,src);

	limit = skill_get_time(skill_id,skill_lv);
	range = skill_get_unit_range(skill_id,skill_lv);
	interval = skill_get_unit_interval(skill_id);
	target = skill_get_unit_target(skill_id);
	bl = skill_get_unit_bl_target(skill_id);
	unit_flag = skill_get_unit_flag(skill_id);
	layout = skill_get_unit_layout(skill_id,skill_lv,src,x,y);

	if( mapdata[src->m].unit_count ) {
		ARR_FIND(0,mapdata[src->m].unit_count,i,mapdata[src->m].units[i]->skill_id == skill_id);
		if( i < mapdata[src->m].unit_count )
			limit = limit * mapdata[src->m].units[i]->modifier / 100;
	}

	sd = BL_CAST(BL_PC,src);
	status = status_get_status_data(src);
	sc = status_get_sc(src);
	hidden = (unit_flag&UF_HIDDEN_TRAP && (battle_config.traps_setting == 2 || (battle_config.traps_setting == 1 && map_flag_vs(src->m))));

	switch( skill_id ) {
		case MG_SAFETYWALL:
			val2 = skill_lv + 1;
#ifdef RENEWAL
			val3 = 300 * skill_lv + 65 * (status->int_ + status_get_lv(src)) + status->max_sp;
#endif
			if( flag ) //Hit bonus from each elemental class [exneval]
				val2 += flag;
			flag = 0;
			break;
		case MG_FIREWALL:
			if( sc && sc->data[SC_VIOLENTGALE] )
				limit = limit * 3 / 2;
			val2 = skill_lv + 4;
			break;
		case AL_WARP:
			val1 = skill_lv + 6;
			if( !(flag&1) )
				limit = 2000;
			else { //Previous implementation (not used anymore)
				//Warp Portal morphing to active mode, extract relevant data from src [Skotlex]
				if( src->type != BL_SKILL )
					return NULL;
				group = ((TBL_SKILL *)src)->group;
				src = map_id2bl(group->src_id);
				if( !src )
					return NULL;
				val2 = group->val2; //Copy the (x,y) position you warp to
				val3 = group->val3; //As well as the mapindex to warp to
			}
			break;
		case HP_BASILICA:
			val1 = src->id; //Store caster id
			break;
		case PR_SANCTUARY:
		case NPC_EVILLAND:
			val1 = skill_lv + 3;
			break;
		case WZ_METEOR:
		case SU_CN_METEOR:
		case SU_CN_METEOR2:
			limit = flag;
			flag = 0; //Flag should not influence anything else for these skills
			break;
		case WZ_FIREPILLAR:
			if( flag&1 )
				limit = 1000;
			val1 = skill_lv + 2;
			break;
		case WZ_QUAGMIRE:
		case AM_DEMONSTRATION:
		case MH_VOLCANIC_ASH:
			if( battle_config.vs_traps_bctall && map_flag_vs(src->m) && (src->type&battle_config.vs_traps_bctall) )
				target = BCT_ALL; //The target changes to "all" if used in a versus map [Skotlex]
			break;
		case HT_SKIDTRAP:
		case MA_SKIDTRAP:
			val1 = ((src->x)<<16)|(src->y); //Save position of caster
		//Fall through
		case HT_ANKLESNARE:
		case HT_SHOCKWAVE:
		case HT_SANDMAN:
		case MA_SANDMAN:
		case HT_CLAYMORETRAP:
		case HT_LANDMINE:
		case MA_LANDMINE:
		case HT_FLASHER:
		case HT_FREEZINGTRAP:
		case MA_FREEZINGTRAP:
		case HT_BLASTMINE:
		case RA_ELECTRICSHOCKER:
		case RA_CLUSTERBOMB:
		case RA_MAGENTATRAP:
		case RA_COBALTTRAP:
		case RA_MAIZETRAP:
		case RA_VERDURETRAP:
		case RA_FIRINGTRAP:
		case RA_ICEBOUNDTRAP:
		case SC_ESCAPE:
			{
				struct skill_condition req = skill_get_requirement(sd,skill_id,skill_lv);

				ARR_FIND(0,MAX_SKILL_ITEM_REQUIRE,i,req.itemid[i] && (req.itemid[i] == ITEMID_TRAP || req.itemid[i] == ITEMID_SPECIAL_ALLOY_TRAP));
				if( i != MAX_SKILL_ITEM_REQUIRE && req.itemid[i] )
					req_item = req.itemid[i];
				if( map_flag_gvg2(src->m) || mapdata[src->m].flag.battleground )
					limit *= 4; //Longer trap times in WoE [celest]
				if( battle_config.vs_traps_bctall && map_flag_vs(src->m) && (src->type&battle_config.vs_traps_bctall) )
					target = BCT_ALL;
			}
			break;
		case SA_VOLCANO:
		case SA_DELUGE:
		case SA_VIOLENTGALE:
		case SA_LANDPROTECTOR:
			{
				struct skill_unit_group *old_group = NULL;

				//HelloKitty confirmed that these are interchangeable
				//So you can change element and not consume gemstones
				if( (old_group = skill_locate_element_field(src)) ) {
					if( (old_group->skill_id == SA_VOLCANO || old_group->skill_id == SA_DELUGE ||
						old_group->skill_id == SA_VIOLENTGALE) && old_group->limit > 0 ) {
						//Use the previous limit (minus the elapsed time) [Skotlex]
						limit = old_group->limit - DIFF_TICK(gettick(),old_group->tick);
						if( limit < 0 ) //This can happen
							limit = skill_get_time(skill_id,skill_lv);
					}
					skill_clear_group(src,1);
				}
			}
			break;
		case BA_WHISTLE:
#ifdef RENEWAL //Flee increase
			val1 = 3 * skill_lv + status->agi / 15;
#else
			val1 = skill_lv + status->agi / 10;
#endif
			val2 = (skill_lv + 1) / 2 + status->luk / 30; //Perfect dodge increase
			if( sd ) {
				val1 += pc_checkskill(sd,BA_MUSICALLESSON) / 2;
				val2 += pc_checkskill(sd,BA_MUSICALLESSON) / 5;
			}
			val2 *= 10;
			break;
		case DC_HUMMING:
#ifdef RENEWAL //Hit increase
			val1 = 20 + 2 * skill_lv + status->dex / 15;
#else
			val1 = 1 + 2 * skill_lv + status->dex / 10;
#endif
			if( sd )
				val1 += pc_checkskill(sd,DC_DANCINGLESSON);
			break;
		case BA_POEMBRAGI:
			val1 = 3 * skill_lv + status->dex / 10; //Casting time reduction
			//For some reason at level 10 the base delay reduction is 50%
			val2 = (skill_lv < 10 ? 3 * skill_lv : 50) + status->int_ / 5; //After-cast delay reduction
			if( sd ) {
				val1 += pc_checkskill(sd,BA_MUSICALLESSON);
				val2 += 2 * pc_checkskill(sd,BA_MUSICALLESSON);
			}
			break;
		case DC_DONTFORGETME:
#ifdef RENEWAL
			val1 = 3 * skill_lv + status->dex / 15; //ASPD decrease
			val2 = 2 * skill_lv + status->agi / 20; //Movement speed adjustment
#else
			val1 = 5 + 3 * skill_lv + status->dex / 10;
			val2 = 5 + 3 * skill_lv + status->agi / 10;
#endif
			if( sd ) {
				val1 += pc_checkskill(sd,DC_DANCINGLESSON);
#ifdef RENEWAL
				val2 += pc_checkskill(sd,DC_DANCINGLESSON) / 2;
#else
				val2 += pc_checkskill(sd,DC_DANCINGLESSON);
#endif
			}
			val1 *= 10; //ASPD works with 1000 as 100%
			break;
		case BA_APPLEIDUN:
			val1 = 5 + 2 * skill_lv + status->vit / 10; //MaxHP percent increase
			if( sd )
				val1 += pc_checkskill(sd,BA_MUSICALLESSON) / 2;
			break;
		case DC_SERVICEFORYOU:
			val1 = 15 + skill_lv + status->int_ / 10; //MaxSP percent increase
			val2 = 20 + 3 * skill_lv + status->int_ / 10; //SP cost reduction
			if( sd ) {
				val1 += pc_checkskill(sd,DC_DANCINGLESSON) / 2;
				val2 += pc_checkskill(sd,DC_DANCINGLESSON) / 2;
			}
			break;
		case BA_ASSASSINCROSS:
#ifdef RENEWAL //ASPD increase
			val1 = skill_lv + status->agi / 20;
#else
			val1 = 5 + skill_lv + status->agi / 20;
#endif
			if( sd )
				val1 += pc_checkskill(sd,BA_MUSICALLESSON) / 2;
			val1 *= 10;
			break;
		case DC_FORTUNEKISS:
			val1 = 10 + skill_lv + status->luk / 10; //Critical increase
			val1 *= 10; //Because every 10 crit is an actual cri point
			if( sd )
				val1 += 5 * pc_checkskill(sd,DC_DANCINGLESSON);
			break;
		case BD_DRUMBATTLEFIELD:
#ifdef RENEWAL
			val1 = (skill_lv + 5) * 25; //Atk increase
			val2 = skill_lv * 10; //Def increase
#else
			val1 = (skill_lv + 1) * 25;
			val2 = (skill_lv + 1) * 2;
#endif
			break;
		case BD_RINGNIBELUNGEN:
			val1 = (skill_lv + 2) * 25; //Atk increase
			break;
		case BD_RICHMANKIM:
			val1 = 25 + 11 * skill_lv; //Exp increase bonus
			break;
		case BD_SIEGFRIED:
			val1 = 55 + skill_lv * 5; //Elemental resistance
			val2 = skill_lv * 10; //Status ailment resistance
			break;
		case WE_CALLPARTNER:
			if( sd )
				val1 = sd->status.partner_id;
			break;
		case WE_CALLPARENT:
			if( sd ) {
				val1 = sd->status.father;
				val2 = sd->status.mother;
			}
			break;
		case WE_CALLBABY:
			if( sd )
				val1 = sd->status.child;
			break;
		case HW_GRAVITATION:
			if( sc && sc->data[SC_GRAVITATION] && sc->data[SC_GRAVITATION]->val3 == BCT_SELF )
				link_group_id = sc->data[SC_GRAVITATION]->val4;
			break;
		case NJ_KAENSIN:
			val2 = (skill_lv + 1) / 2 + 4;
		//Fall through
		case NJ_SUITON:
			skill_clear_group(src,1); //Delete previous Kaensins/Suitons
			break;
		case GS_GROUNDDRIFT: {
#ifdef RENEWAL
				int ammo_id[5] = { ITEMID_LIGHTING_BULLET,ITEMID_BLIND_BULLET,ITEMID_POISON_BULLET,ITEMID_ICE_BULLET,ITEMID_FLARE_BULLET };
				short idx;

				if( sd && (idx = sd->equip_index[EQI_AMMO]) >= 0 && sd->inventory_data[idx] ) {
					for( i = 0; i < ARRAYLENGTH(ammo_id); i++ ) {
						if( sd->inventory.u.items_inventory[idx].nameid != ammo_id[i] )
							continue;
						val1 = status->rhw.ele;
					}
					if( !val1 )
						val1 = ELE_NEUTRAL;
				}
#else
				int element[5] = { ELE_WIND,ELE_DARK,ELE_POISON,ELE_WATER,ELE_FIRE };

				val1 = status->rhw.ele;
				if( !val1 )
					val1 = element[rnd()%5];
#endif
				switch( val1 ) {
					case ELE_FIRE:
						subunt++;
					case ELE_WATER:
						subunt++;
					case ELE_POISON:
						subunt++;
					case ELE_DARK:
						subunt++;
					case ELE_WIND:
						break;
					case ELE_NEUTRAL:
						subunt = UNT_GROUNDDRIFT_NEUTRAL - UNT_GROUNDDRIFT_WIND;
						break;
				}
			}
			break;
		case GC_POISONSMOKE:
			if( !(sc && sc->data[SC_POISONINGWEAPON]) )
				return NULL;
			val2 = sc->data[SC_POISONINGWEAPON]->val2; //Type of Poison
			val3 = sc->data[SC_POISONINGWEAPON]->val1;
			limit = skill_get_time(skill_id,skill_lv);
			break;
		case NPC_COMET:
		case WL_COMET:
			if( sc ) {
				sc->pos_x = x;
				sc->pos_y = y;
			}
			break;
		case SC_CHAOSPANIC:
			skill_clear_group(src,4);
			break;
		case SC_MAELSTROM:
			skill_clear_group(src,8);
			break;
		case SC_BLOODYLUST:
			skill_clear_group(src,16);
			break;
		case SO_CLOUD_KILL:
			skill_clear_group(src,32);
			break;
		case SO_WARMER:
			skill_clear_group(src,64);
			break;
		case SO_VACUUM_EXTREME:
			//Coordinates
			val1 = x;
			val2 = y;
			//Suck target at n seconds
			val3 = 0;
			break;
		case GN_WALLOFTHORN:
			if( flag&1 ) { //Turned become Firewall
				limit = src->val1;
				bl |= BL_SKILL;
			}
			val3 = (x<<16)|y; //Firewall coordinates
			break;
		case GN_DEMONIC_FIRE:
			if( flag&1 ) //Fire Expansion level 1
				limit = src->val1 + 10000;
			val3 = (x<<16)|y;
			break;
		case GN_FIRE_EXPANSION_SMOKE_POWDER: //Fire Expansion level 3
		case GN_FIRE_EXPANSION_TEAR_GAS: //Fire Expansion level 4
			limit = ((sd ? pc_checkskill(sd,GN_DEMONIC_FIRE) : 5) + 1) * limit;
			break;
		case MH_STEINWAND:
			val2 = skill_lv + 4;
			val3 = 300 * skill_lv + 65 * (status->int_ + status_get_lv(src)) + status->max_sp;
			break;
		case EL_FIRE_MANTLE:
			val2 = skill_lv;
			break;
		case KO_ZENKAI:
			if( sd ) {
				val1 = sd->charmball_old;
				val2 = sd->charmball_type;
				limit *= val1;
				subunt = val2 - 1;
			}
			break;
		case NPC_VENOMFOG:
			interval *= skill_lv;
			break;
	}

	//Init skill unit group
	nullpo_retr(NULL,(group = skill_initunitgroup(src,layout->count,skill_id,skill_lv,skill_get_unit_id(skill_id,flag&1) + subunt,limit,interval)));
	group->val1 = val1;
	group->val2 = val2;
	group->val3 = val3;
	group->link_group_id = link_group_id;
	group->target_flag = target;
	group->bl_flag = bl;
	group->state.ammo_consume = (sd && sd->state.arrow_atk && skill_id != GS_GROUNDDRIFT && skill_id != WM_SEVERE_RAINSTORM && skill_id != GN_WALLOFTHORN); //Store if this skill needs to consume ammo
	group->state.song_dance = (unit_flag&(UF_DANCE|UF_SONG) ? 1 : 0)|(unit_flag&UF_ENSEMBLE ? 2 : 0); //Signals if this is a song/dance/duet
	group->state.guildaura = (skill_id >= GD_LEADERSHIP && skill_id <= GD_HAWKEYES ? 1 : 0);
	group->item_id = req_item;

	//If tick is greater than current, do not invoke onplace function just yet [Skotlex]
	if( DIFF_TICK(group->tick,gettick()) > SKILLUNITTIMER_INTERVAL )
		active_flag = 0;

	if( skill_id == HT_TALKIEBOX || skill_id == RG_GRAFFITI ) { //Put message for Talkie Box & Graffiti
		group->valstr = (char *)aMalloc(MESSAGE_SIZE * sizeof(char));
		if( sd )
			safestrncpy(group->valstr,sd->message,MESSAGE_SIZE);
		else //Eh, we have to write something here, even though mobs shouldn't use this [Skotlex]
			safestrncpy(group->valstr,"Boo!",MESSAGE_SIZE);
	}

	if( group->state.song_dance ) { //Dance skill
		if( sd ) {
			sd->skill_id_dance = skill_id;
			sd->skill_lv_dance = skill_lv;
		}
		if( sc_start4(src,src,SC_DANCING,100,skill_id,group->group_id,skill_lv,((group->state.song_dance&2) ? BCT_SELF : 0),limit + 1000) &&
			sd && (group->state.song_dance&2) && skill_id != CG_HERMODE ) //Hermod is a encore with a warp!
			skill_check_pc_partner(sd,skill_id,&skill_lv,1,1);
	}

	//Set skill unit
	limit = group->limit;
	for( i = 0; i < layout->count; i++ ) {
		struct skill_unit *unit;
		int ux = x + layout->dx[i];
		int uy = y + layout->dy[i];
		int unit_val1 = skill_lv;
		int unit_val2 = 0, unit_val3 = 0, unit_val4 = 0;
		int alive = 1;

		if( ux <= 0 || uy <= 0 || ux >= mapdata[src->m].xs || uy >= mapdata[src->m].ys )
			continue; //Are the coordinates out of range?

		if( !group->state.song_dance && map_getcell(src->m,ux,uy,CELL_CHKNOREACH) )
			continue; //Don't place skill units on walls (except for songs/dances/encores)

		if( battle_config.skill_wall_check && (unit_flag&UF_PATHCHECK) && !path_search_long(NULL,src->m,ux,uy,src->x,src->y,CELL_CHKWALL) )
			continue; //No path between cell and caster

		switch( skill_id ) {
			//HP for Skill unit that can be damaged, see also skill_unit_ondamaged
			case HT_LANDMINE:
			case MA_LANDMINE:
			case HT_ANKLESNARE:
			case HT_SHOCKWAVE:
			case HT_SANDMAN:
			case MA_SANDMAN:
			case HT_FLASHER:
			case HT_FREEZINGTRAP:
			case MA_FREEZINGTRAP:
			case HT_SKIDTRAP:
			case MA_SKIDTRAP:
			case HT_CLAYMORETRAP:
			case HT_BLASTMINE:
			case SC_ESCAPE:
				unit_val1 = 3500;
				break;
			case MG_FIREWALL:
			case NJ_KAENSIN:
			case EL_FIRE_MANTLE:
				unit_val2 = group->val2;
				break;
			case WZ_ICEWALL:
				unit_val1 = 200 + 200 * skill_lv;
				unit_val2 = map_getcell(src->m,ux,uy,CELL_GETTYPE);
				break;
			case WZ_WATERBALL: //Check if there are cells that can be turned into waterball units
				if( !sd || map_getcell(src->m,ux,uy,CELL_CHKWATER) || (map_find_skill_unit_oncell(src,ux,uy,SA_DELUGE,NULL,1)) ||
					(map_find_skill_unit_oncell(src,ux,uy,NJ_SUITON,NULL,1)) )
					break; //Turn water, deluge or suiton into waterball cell
				continue;
			case GS_DESPERADO:
				unit_val1 = abs(layout->dx[i]);
				unit_val2 = abs(layout->dy[i]);
				if( unit_val1 < 2 || unit_val2 < 2 ) { //Nearby cross, linear decrease with no diagonals
					if( unit_val2 > unit_val1 )
						unit_val1 = unit_val2;
					if( unit_val1 )
						unit_val1--;
					unit_val1 = 36 - 12 * unit_val1;
				} else //Diagonal edges
					unit_val1 = 28 - 4 * unit_val1 - 4 * unit_val2;
				if( unit_val1 < 1 )
					unit_val1 = 1;
				unit_val2 = 0;
				break;
			case NPC_REVERBERATION:
			case WM_REVERBERATION:
				unit_val1 = 1 + skill_lv;
				break;
			case GN_WALLOFTHORN:
				if( flag&1 ) { //Turned become Firewall
					unit_val2 = 16;
					break;
				}
				unit_val1 = 2000 + 2000 * skill_lv; //Thorn Walls HP
				unit_val2 = 20; //Max hits
				unit_val3 = 16; //Max deal hits
				break;
			default:
				if( group->state.song_dance&0x1 )
					unit_val2 = (unit_flag&(UF_DANCE|UF_SONG)); //Store whether this is a song/dance
				break;
		}

		if( (unit_flag&UF_RANGEDSINGLEUNIT) && i == (layout->count / 2) )
			unit_val4 |= UF_RANGEDSINGLEUNIT; //Center

		if( sd && map_getcell(src->m,ux,uy,CELL_CHKMAELSTROM) ) //Does not recover SP from monster skills
			map_foreachincell(skill_maelstrom_suction,src->m,ux,uy,BL_SKILL,skill_id,skill_lv);

		//Check active cell to failing or remove current unit
		map_foreachincell(skill_cell_overlap,src->m,ux,uy,BL_SKILL,skill_id,&alive,src);

		if( !alive )
			continue;

		nullpo_retr(NULL,(unit = skill_initunit(group,i,ux,uy,unit_val1,unit_val2,unit_val3,unit_val4,hidden)));
		unit->limit = limit;
		unit->range = range;

		if( skill_id == PF_FOGWALL && alive == 2 ) { //Double duration of cells on top of Deluge/Suiton
			unit->limit *= 2;
			group->limit = unit->limit;
		}

		//Execute on all targets standing on this cell
		if( !unit->range && active_flag )
			map_foreachincell(skill_unit_effect,unit->bl.m,unit->bl.x,unit->bl.y,group->bl_flag,&unit->bl,gettick(),1);
	}

	if( !group->alive_count ) { //Cell was blocked completely by Land Protector or Maelstrom
		skill_delunitgroup(group);
		return NULL;
	}

	//Success, unit created
	switch( skill_id ) {
		case NJ_TATAMIGAESHI: //Store number of tiles
			group->val1 = group->alive_count;
			break;
	}

	return group;
}

/*==========================================
 *
 *------------------------------------------*/
void ext_skill_unit_onplace(struct skill_unit *unit, struct block_list *bl, unsigned int tick)
{
	skill_unit_onplace(unit,bl,tick);
}

/**
 * Triggeres when 'target' (based on skill unit target) is stand at unit area
 * while skill unit initialized or moved (such by knock back).
 * As a follow of skill_unit_effect flag&1
 * @param unit
 * @param bl Target
 * @param tick
 */
static int skill_unit_onplace(struct skill_unit *unit, struct block_list *bl, unsigned int tick)
{
	struct skill_unit_group *group;
	struct block_list *src; //Actual source that cast the skill unit
	TBL_PC *sd;
	struct status_change *sc;
	struct status_change_entry *sce;
	enum sc_type type;
	uint16 skill_id, skill_lv;

	if( !unit || !unit->alive || !unit->group )
		return 0;

	nullpo_ret(bl);

	if( !bl->prev || status_isdead(bl) )
		return 0;

	group = unit->group;

	nullpo_ret(src = map_id2bl(group->src_id));

	skill_id = group->skill_id; //In case the group is deleted, we need to return the correct skill id, still
	skill_lv = group->skill_lv;

	sd = BL_CAST(BL_PC,bl);
	sc = status_get_sc(bl);
	type = status_skill2sc(skill_id);
	sce = (sc && type != SC_NONE) ? sc->data[type] : NULL;

	if( skill_get_type(skill_id) == BF_MAGIC && ((skill_id != SA_LANDPROTECTOR &&
		map_getcell(unit->bl.m,unit->bl.x,unit->bl.y,CELL_CHKLANDPROTECTOR)) ||
		map_getcell(bl->m,bl->x,bl->y,CELL_CHKMAELSTROM)) )
		return 0; //AoE skills are ineffective [Skotlex]

	if( skill_get_inf2(skill_id)&(INF2_SONG_DANCE|INF2_ENSEMBLE_SKILL) && map_getcell(bl->m,bl->x,bl->y,CELL_CHKBASILICA) )
		return 0; //Songs don't work in Basilica

	if( sc ) {
		if( (sc->option&OPTION_HIDE) && !(skill_get_inf3(skill_id)&INF3_HIT_HIDING) )
			return 0; //Hidden characters are immuned, except to these skills [Skotlex]
		if( sc->data[SC_HOVERING] ) {
			switch( group->unit_id ) {
				case UNT_QUAGMIRE:
				case UNT_GRAVITATION:
				case UNT_VOLCANO:
				case UNT_DELUGE:
				case UNT_VIOLENTGALE:
				case UNT_SUITON:
					return 0;
			}
		}
	}

	switch( group->unit_id ) {
		case UNT_SPIDERWEB:
			if( sc ) {
				//Duration in PVM is: 1st - 8s, 2nd - 16s, 3rd - 8s
				//Duration in PVP is: 1st - 4s, 2nd - 8s, 3rd - 12s
				int time = skill_get_time2(skill_id,skill_lv);
				const struct TimerData *td;

				if( map_flag_vs(bl->m) )
					time /= 2;
				if( sc->data[type] ) {
					if( sc->data[type]->val2 && sc->data[type]->val3 && sc->data[type]->val4 ) {
						group->limit = DIFF_TICK(tick,group->tick);
						break; //Already triple affected, immune
					}
					//Don't increase val1 here, we need a higher val in status_change_start so it overwrites the old one
					if( map_flag_vs(bl->m) && sc->data[type]->val1 < 3 )
						time *= (sc->data[type]->val1 + 1);
					else if( !map_flag_vs(bl->m) && sc->data[type]->val1 < 2 )
						time *= (sc->data[type]->val1 + 1);
					//Add group id to status change
					if( !sc->data[type]->val2 )
						sc->data[type]->val2 = group->group_id;
					else if( !sc->data[type]->val3 )
						sc->data[type]->val3 = group->group_id;
					else if( !sc->data[type]->val4 )
						sc->data[type]->val4 = group->group_id;
					//Overwrite status change with new duration
					if( (td = get_timer(sc->data[type]->timer)) )
						sc_start4(src,bl,type,100,sc->data[type]->val1 + 1,sc->data[type]->val2,sc->data[type]->val3,sc->data[type]->val4,max(DIFF_TICK(td->tick,tick),time));
				} else if( sc_start4(src,bl,type,100,1,group->group_id,0,0,time) ) {
					td = (sc->data[type] ? get_timer(sc->data[type]->timer) : NULL);
					if( td )
						time = DIFF_TICK(td->tick,tick);
					if( !unit_blown_immune(bl,0x9) && unit_movepos(bl,unit->bl.x,unit->bl.y,0,false) )
						clif_blown(bl,&unit->bl);
				}
				group->val2 = bl->id;
				group->limit = DIFF_TICK(tick,group->tick) + time;
			}
			break;

		case UNT_SAFETYWALL:
			if( !sce )
				sc_start4(src,bl,type,100,skill_lv,skill_id,group->group_id,0,group->limit);
			break;

		case UNT_PNEUMA:
			if( !sce )
				sc_start2(src,bl,type,100,skill_lv,group->group_id,group->limit);
			break;

		case UNT_BLOODYLUST:
			if( bl->id == src->id )
				break; //Doesn't affect the caster
			if( !sce )
				sc_start4(src,bl,type,100,skill_lv,0,SC__BLOODYLUST,0,group->limit);
			break;

		case UNT_WARP_WAITING: {
				int working = group->val1&0xffff;

				if( bl->type == BL_PC && !working ) {
					if( (!sd->chatID || battle_config.chat_warpportal) &&
						sd->ud.to_x == unit->bl.x && sd->ud.to_y == unit->bl.y ) {
						int x = group->val2>>16;
						int y = group->val2&0xffff;
						int count = group->val1>>16;
						unsigned short m = group->val3;

						if( --count <= 0 )
							skill_delunitgroup(group);
						if( map_mapindex2mapid(m) == sd->bl.m && x == sd->bl.x && y == sd->bl.y )
							working = 1; //We break it because officials break it, lovely stuff
						group->val1 = (count<<16)|working;
						if( pc_job_can_entermap((enum e_job)sd->status.class_,map_mapindex2mapid(m),sd->group_level) )
							pc_setpos(sd,m,x,y,CLR_TELEPORT);
					}
				} else if( bl->type == BL_MOB && (battle_config.mob_warp&2) ) {
					int16 m = map_mapindex2mapid(group->val3);

					if( m < 0 )
						break; //Map not available on this map-server
					unit_warp(bl,m,group->val2>>16,group->val2&0xffff,CLR_TELEPORT);
				}
			}
			break;

		case UNT_QUAGMIRE:
			if( !sce && battle_check_target(&unit->bl,bl,group->target_flag) > 0 )
				sc_start4(src,bl,type,100,skill_lv,group->group_id,0,0,group->limit);
			break;

		case UNT_VOLCANO:
		case UNT_DELUGE:
		case UNT_VIOLENTGALE:
		case UNT_FIRE_INSIGNIA:
		case UNT_WATER_INSIGNIA:
		case UNT_WIND_INSIGNIA:
		case UNT_EARTH_INSIGNIA:
			if( !sce )
				sc_start(src,bl,type,100,skill_lv,group->limit);
			break;

		case UNT_WATER_BARRIER:
		case UNT_ZEPHYR:
		case UNT_POWER_OF_GAIA:
			if( bl->id == src->id )
				break; //Doesn't affect the elemental
			if( !sce )
				sc_start(src,bl,type,100,skill_lv,group->limit);
			break;

		case UNT_SUITON:
			if( !sce )
				sc_start4(src,bl,type,100,skill_lv,
				(map_flag_vs(bl->m) || (battle_check_target(&unit->bl,bl,BCT_ENEMY) > 0 ? 1 : 0)), //Send val2 to reduce agi
				0,0,group->limit);
			break;

		case UNT_HERMODE:
			if( bl->id != src->id && battle_check_target(&unit->bl,bl,BCT_PARTY|BCT_GUILD) > 0 )
				status_change_clear_buffs(bl,SCCB_BUFFS,0); //Should dispell only allies
		//Fall through
		case UNT_RICHMANKIM:
		case UNT_ETERNALCHAOS:
		case UNT_DRUMBATTLEFIELD:
		case UNT_RINGNIBELUNGEN:
		case UNT_ROKISWEIL:
		case UNT_INTOABYSS:
		case UNT_SIEGFRIED:
			 //Needed to check when a dancer/bard leaves their ensemble area
			if( bl->id == src->id && !(sc && sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_BARDDANCER) )
				return skill_id;
			if( !sce )
				sc_start4(src,bl,type,100,skill_lv,group->val1,group->val2,0,group->limit);
			break;
		case UNT_APPLEIDUN:
			if( !battle_config.song_timer_reset && sce )
				return 0; //Aegis style: Apple of idun doesn't update its effect
		//Fall through
		case UNT_WHISTLE:
		case UNT_ASSASSINCROSS:
		case UNT_POEMBRAGI:
		case UNT_HUMMING:
		case UNT_DONTFORGETME:
		case UNT_FORTUNEKISS:
		case UNT_SERVICEFORYOU:
			if( bl->id == src->id && !(sc && sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_BARDDANCER) )
				return 0; //Don't buff themselves without link
			if( !sce )
				sc_start4(src,bl,type,100,skill_lv,group->val1,group->val2,0,group->limit);
			else if( battle_config.song_timer_reset && //eA style: Readjust timers since the effect will not last long
				sce->val4 == 1 ) { //From here songs are already active
				sce->val4 = 0; //Remove the mark that we stepped out
				delete_timer(sce->timer,status_change_timer);
				sce->timer = add_timer(tick + group->limit,status_change_timer,bl->id,type); //Put duration 1 back
			} else if( !battle_config.song_timer_reset ) { //Aegis style: Songs won't renew unless finished
				const struct TimerData *td = get_timer(sce->timer);

				if( DIFF_TICK(td->tick,tick) < group->interval ) { //Update with new values as the current one will vanish soon
					delete_timer(sce->timer,status_change_timer);
					sce->timer = add_timer(tick + group->limit,status_change_timer,bl->id,type);
					sce->val1 = skill_lv;
					sce->val2 = group->val1;
					sce->val3 = group->val2;
					sce->val4 = 0;
				}
			}
			break;

		case UNT_FOGWALL:
			if( !sce ) {
				sc_start4(src,bl,type,100,skill_lv,group->val1,group->val2,group->group_id,group->limit);
				if( battle_check_target(&unit->bl,bl,BCT_ENEMY) > 0 )
					skill_additional_effect(src,bl,skill_id,skill_lv,BF_MISC,ATK_DEF,tick);
			}
			break;

		case UNT_GRAVITATION:
			if( !sce )
				sc_start4(src,bl,type,100,skill_lv,0,BCT_ENEMY,group->group_id,group->limit);
			break;

		case UNT_MOONLIT:
			if( sc && sc->data[SC_DANCING] && (sc->data[SC_DANCING]->val1&0xFFFF) == CG_MOONLIT )
				break; //Knockback out of area if affected char isn't in Moonlit effect
			if( bl->id == src->id )
				break; //Also needed to prevent infinite loop crash
			skill_blown(src,bl,skill_get_blewcount(skill_id,skill_lv),unit_getdir(bl),0);
			break;

		case UNT_EPICLESIS:
			status_change_end(bl,SC_HIDING,INVALID_TIMER);
			status_change_end(bl,SC_CLOAKING,INVALID_TIMER);
			status_change_end(bl,SC_CAMOUFLAGE,INVALID_TIMER);
			status_change_end(bl,SC_CLOAKINGEXCEED,INVALID_TIMER);
			status_change_end(bl,SC_NEWMOON,INVALID_TIMER);
			if( sc && sc->data[SC__SHADOWFORM] && rnd()%100 < 100 - sc->data[SC__SHADOWFORM]->val1 * 10 )
				status_change_end(bl,SC__SHADOWFORM,INVALID_TIMER);
			if( !sce )
				sc_start(src,bl,type,100,skill_lv,group->limit);
			break;

		case UNT_STEALTHFIELD:
			if( bl->id == src->id )
				break;
		//Fall through
		case UNT_NEUTRALBARRIER:
			if( !sce )
				sc_start(src,bl,type,100,skill_lv,group->limit);
			break;

		case UNT_BANDING:
			if( !sce ) {
				uint8 rate = status_get_lv(src) / 5 + 5 * skill_lv - status_get_agi(src) / 10;

				sc_start(src,bl,type,rate,skill_lv,skill_get_time2(skill_id,skill_lv));
			}
			break;

		case UNT_CLOUD_KILL:
			if( !sce && sc_start4(src,bl,type,100,skill_lv,src->id,unit->bl.id,0,group->limit) )
				status_change_start(src,bl,SC_POISON,10000,skill_lv,0,0,0,skill_get_time2(skill_id,skill_lv),SCFLAG_FIXEDTICK);
			break;

		case UNT_WARMER: {
#ifdef RENEWAL
				struct mob_data *md = BL_CAST(BL_MOB,bl);

				if( md && md->mob_id == MOBID_EMPERIUM )
					break;
#endif
				if( !sce )
					sc_start2(src,bl,type,100,skill_lv,src->id,group->limit);
			}
			break;

		case UNT_DEMONIC_FIRE:
			if( !sce && skill_attack(BF_MAGIC,src,&unit->bl,bl,skill_id,skill_lv,tick,0) )
				sc_start4(src,bl,type,100,skill_lv,src->id,unit->bl.id,0,group->limit);
			break;

		case UNT_FIRE_EXPANSION_SMOKE_POWDER:
			if( !sce )
				sc_start(src,bl,type,100,skill_lv,group->limit);
			break;

		case UNT_FIRE_EXPANSION_TEAR_GAS:
			if( !sce && sc_start4(src,bl,type,100,skill_lv,0,src->id,0,group->limit) )
				sc_start(src,bl,SC_TEARGAS_SOB,100,skill_lv,group->limit);
			break;

		case UNT_VOLCANIC_ASH:
			if( !sce )
				sc_start(src,bl,type,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;

		case UNT_KINGS_GRACE:
			if( !map_flag_gvg2(src->m) )
				group->target_flag &= ~(BCT_NEUTRAL|BCT_GUILD);
			if( !sce && battle_check_target(&unit->bl,bl,group->target_flag) > 0 )
				sc_start(src,bl,type,100,skill_lv,group->limit);
			break;

		case UNT_CREATINGSTAR:
			if( !sce )
				sc_start4(src,bl,type,100,skill_lv,src->id,unit->bl.id,0,group->limit);
			break;

		case UNT_CN_POWDERING:
			if( bl->id == src->id )
				break; //Does not affect the caster
			if( !sce && battle_check_target(&unit->bl,bl,BCT_ENEMY) > 0 )
				sc_start(src,bl,type,100,skill_lv,group->limit);
			break;

		case UNT_NYANGGRASS:
			if( !sce )
				sc_start(src,bl,type,100,skill_lv,group->limit);
			break;

		case UNT_GD_LEADERSHIP:
		case UNT_GD_GLORYWOUNDS:
		case UNT_GD_SOULCOLD:
		case UNT_GD_HAWKEYES:
			if( !sce && battle_check_target(&unit->bl,bl,group->target_flag) > 0 )
				sc_start(src,bl,type,100,skill_lv,1000);
			break;
	}
	return skill_id;
}

/**
 * Process skill unit each interval (group->interval, see interval field of skill_unit_db.txt)
 * @param unit Skill unit
 * @param bl Valid 'target' above the unit, that has been check in skill_unit_timer_sub_onplace
 * @param tick
 */
static int skill_unit_onplace_timer(struct skill_unit *unit, struct block_list *bl, unsigned int tick)
{
	struct skill_unit_group *group;
	struct block_list *src;
	TBL_PC *tsd;
	struct status_data *tstatus;
	struct status_change *sc, *tsc;
	struct skill_unit_group_tickset *ts;
	enum sc_type type;
	uint16 skill_id, skill_lv;
	int diff = 0;

	if (!unit || !unit->alive || !unit->group)
		return 0;

	nullpo_ret(bl);

	if (!bl->prev || status_isdead(bl))
		return 0;

	group = unit->group;

	nullpo_ret(src = map_id2bl(group->src_id));

	skill_id = group->skill_id;
	skill_lv = group->skill_lv;

	tsd = BL_CAST(BL_PC,bl);
	sc = status_get_sc(src);
	tsc = status_get_sc(bl);
	tstatus = status_get_status_data(bl);
	type = status_skill2sc(skill_id);

	if (sc && sc->data[SC_VOICEOFSIREN] && sc->data[SC_VOICEOFSIREN]->val2 == bl->id && (skill_get_inf2(skill_id)&INF2_TRAP))
		return 0;

	if (tsc) {
		if (tsc->data[SC_HOVERING]) {
			switch (group->unit_id) {
				case UNT_SKIDTRAP:
				case UNT_LANDMINE:
				case UNT_ANKLESNARE:
				case UNT_FLASHER:
				case UNT_SHOCKWAVE:
				case UNT_SANDMAN:
				case UNT_FREEZINGTRAP:
				case UNT_BLASTMINE:
				case UNT_CLAYMORETRAP:
				case UNT_GRAVITATION:
				case UNT_ELECTRICWALK:
				case UNT_FIREWALK:
				case UNT_VACUUM_EXTREME:
					return 0;
			}
		}
		if (tsc->data[SC__MANHOLE] && (skill_get_inf2(skill_id)&INF2_TRAP))
			return 0;
	}

	if ((ts = skill_unitgrouptickset_search(bl,group,tick))) {
		diff = DIFF_TICK(tick,ts->tick);
		if (diff < 0)
			return 0; //Not all have it, eg: Traps don't have it even though they can be hit by Heaven's Drive [Skotlex]
		ts->tick = tick + group->interval;
		if ((skill_id == CR_GRANDCROSS || skill_id == NPC_GRANDDARKNESS) && !battle_config.gx_allhit)
			ts->tick += group->interval * (map_count_oncell(bl->m,bl->x,bl->y,BL_CHAR,0) - 1);
	}

	//Wall of Thorn damaged by Fire element unit [Cydh]
	//NOTE: This check doesn't matter the location, as long as one of units touched, this check will be executed
	if (bl->type == BL_SKILL && (skill_get_ele(skill_id,skill_lv) == ELE_FIRE || (skill_id == GN_WALLOFTHORN && group->unit_id == UNT_FIREWALL))) {
		struct skill_unit *unit2 = (struct skill_unit *)bl;
		struct skill_unit_group *group2 = NULL;
		struct block_list *src2 = NULL;

		if (unit2 && (group2 = unit2->group) && group2->unit_id == UNT_WALLOFTHORN && (src2 = map_id2bl(group2->src_id))) {
			group2->unit_id = UNT_USED_TRAPS;
			group2->limit = 0;
			if (!src2->val1)
				src2->val1 = skill_get_time(group2->skill_id,group2->skill_lv) - DIFF_TICK(tick,group2->tick);
			//Set delay for each Thorn when turning to Fire Wall [exneval]
			skill_addtimerskill(src2,tick + 500,0,group2->val3>>16,group2->val3&0xffff,group2->skill_id,group2->skill_lv,0,1);
		}
		return skill_id;
	}

	switch (group->unit_id) {
		//Units that deals simple attack
		case UNT_DEMONSTRATION:
		case UNT_DISSONANCE:
		case UNT_GRAVITATION:
		case UNT_TATAMIGAESHI:
		case UNT_EARTHSTRAIN:
		case UNT_FIREWALK:
		case UNT_ELECTRICWALK:
		case UNT_PSYCHIC_WAVE:
		case UNT_MAKIBISHI:
		case UNT_FIRE_RAIN:
		case UNT_VENOMFOG:
		case UNT_ICEMINE:
		case UNT_FLAMECROSS:
		case UNT_HELLBURNING:
			skill_attack(skill_get_type(skill_id),src,&unit->bl,bl,skill_id,skill_lv,tick,0);
			break;

		case UNT_DUMMYSKILL:
			switch (skill_id) {
				case SG_SUN_WARM: //SG skills [Komurka]
				case SG_MOON_WARM:
				case SG_STAR_WARM:
					{
						int count = 0;
						const int x = bl->x, y = bl->y;

						map_freeblock_lock();
						do { //If target isn't knocked back it should hit every "interval" ms [Playtester]
							if (bl->type == BL_PC)
								status_zap(bl,0,15); //Sp damage to players
							else if (status_charge(src,0,2)) { //Costs 2 SP per hit to mobs
								if (!skill_attack(skill_get_type(skill_id),src,&unit->bl,bl,skill_id,skill_lv,tick + count * group->interval,0))
									status_charge(src,0,8); //Costs additional 8 SP if miss
							} else { //Should end when out of sp
								group->limit = DIFF_TICK(tick,group->tick);
								break;
							}
						} while (x == bl->x && y == bl->y && group->alive_count &&
							++count < SKILLUNITTIMER_INTERVAL / group->interval && !status_isdead(bl));
						map_freeblock_unlock();
					}
					break;
#ifndef RENEWAL
				case WZ_STORMGUST:
					//SG counter was dropped in renewal
					//SG counter does not reset per stormgust. IE: One hit from a SG and two hits from another will freeze you
					if (tsc) {
						tsc->sg_counter++; //SG hit counter
						if (skill_attack(skill_get_type(skill_id),src,&unit->bl,bl,skill_id,skill_lv,tick,0) <= 0)
							tsc->sg_counter = 0; //Attack absorbed
					}
					break;
#endif
				case GS_DESPERADO:
					if (rnd()%100 < unit->val1)
						skill_attack(skill_get_type(skill_id),src,&unit->bl,bl,skill_id,skill_lv,tick,0);
					break;
				case NPC_WIDESUCK: {
						int heal = skill_attack(skill_get_type(skill_id),src,&unit->bl,bl,skill_id,skill_lv,tick,0);

						if (heal > 0) {
							clif_skill_nodamage(src,bl,skill_id,skill_lv,1);
							clif_skill_nodamage(NULL,src,AL_HEAL,heal,1);
							status_heal(src,heal,0,0);
						}
					}
					break;
				default:
					skill_attack(skill_get_type(skill_id),src,&unit->bl,bl,skill_id,skill_lv,tick,0);
					break;
			}
			break;

		case UNT_FIREWALL:
		case UNT_KAEN:
		case UNT_FIRE_MANTLE:
			{
				uint8 flag = 0;
				int count = 0;
				const int x = bl->x, y = bl->y;

				if (skill_id == GN_WALLOFTHORN) {
					if (battle_check_target(src,bl,BCT_ENEMY) <= 0)
						break;
					flag = 1;
				}
				do //Take into account these hit more times than the timer interval can handle
					skill_attack(BF_MAGIC,src,&unit->bl,bl,(flag ? MG_FIREWALL : skill_id),skill_lv,tick + count * group->interval,0);
				while (--unit->val2 && x == bl->x && y == bl->y &&
					++count < SKILLUNITTIMER_INTERVAL / group->interval && !status_isdead(bl));
				if (unit->val2 <= 0)
					skill_delunit(unit);
			}
			break;

		case UNT_SANCTUARY:
			if (battle_check_undead(tstatus->race,tstatus->def_ele) || tstatus->race == RC_DEMON) {
				if (battle_check_target(&unit->bl,bl,BCT_ENEMY) > 0 && //Only damage enemies with offensive Sanctuary [Skotlex]
					skill_attack(BF_MAGIC,src,&unit->bl,bl,skill_id,skill_lv,tick,0))
					group->val1 -= 1; //Reduce the number of targets that can still be hit
			} else {
				int heal = skill_calc_heal(src,bl,skill_id,skill_lv,true);
				struct mob_data *md = BL_CAST(BL_MOB,bl);

				if (md) {
#ifdef RENEWAL
					if (md->mob_id == MOBID_EMPERIUM)
						break;
#endif
					if (status_get_class_(bl) == CLASS_BATTLEFIELD)
						break;
				}
				if (tstatus->hp >= tstatus->max_hp)
					break;
				if (status_isimmune(bl))
					heal = 0;
				if (tsc && tsc->data[SC_AKAITSUKI] && heal)
					skill_akaitsuki_damage(&unit->bl,bl,heal,skill_id,skill_lv,tick);
				else {
					clif_skill_nodamage(&unit->bl,bl,AL_HEAL,heal,1);
					status_heal(bl,heal,0,0);
				}
			}
			break;

		case UNT_EARTHQUAKE:
			skill_area_temp[0] = map_foreachinallrange(skill_area_sub,&unit->bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
				skill_id,skill_lv,tick,BCT_ENEMY,skill_area_sub_count);
			skill_area_temp[1] = src->id;
			skill_area_temp[2] = 0;
			map_foreachinallrange(skill_area_sub,&unit->bl,skill_get_splash(skill_id,skill_lv),splash_target(src),src,
				skill_id,skill_lv,tick,BCT_ENEMY|SD_SPLASH|SD_PREAMBLE|1,skill_castend_damage_id);
			break;

		case UNT_EVILLAND: //Will heal demon and undead element monsters, but not players
			if (bl->type == BL_PC || (!battle_check_undead(tstatus->race,tstatus->def_ele) && tstatus->race != RC_DEMON)) {
				if (battle_check_target(&unit->bl,bl,BCT_ENEMY) > 0) //Damage enemies
					skill_attack(BF_MISC,src,&unit->bl,bl,skill_id,skill_lv,tick,0);
			} else {
				int heal = skill_calc_heal(src,bl,skill_id,skill_lv,true);

				if (tstatus->hp >= tstatus->max_hp)
					break;
				if (status_isimmune(bl))
					heal = 0;
				clif_skill_nodamage(&unit->bl,bl,AL_HEAL,heal,1);
				status_heal(bl,heal,0,0);
			}
			break;

		case UNT_MAGNUS:
			if (!battle_check_undead(tstatus->race,tstatus->def_ele) && tstatus->race != RC_DEMON)
				break;
			skill_attack(BF_MAGIC,src,&unit->bl,bl,skill_id,skill_lv,tick,0);
			break;

		case UNT_FIREPILLAR_WAITING:
			skill_unitsetting(src,skill_id,skill_lv,unit->bl.x,unit->bl.y,1);
			skill_delunit(unit);
			break;

		case UNT_SKIDTRAP:
			//Knockback away from position of user during placement [Playtester]
			skill_blown(&unit->bl,bl,skill_get_blewcount(skill_id,skill_lv),
				(map_calc_dir_xy(group->val1>>16,group->val1&0xFFFF,bl->x,bl->y,6) + 4)%8,0);
			//Target will be stopped for 3 seconds
			sc_start(src,bl,SC_STOP,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			group->unit_id = UNT_USED_TRAPS;
			clif_changetraplook(&unit->bl,UNT_USED_TRAPS);
			group->limit = DIFF_TICK(tick,group->tick) + 1500; //Gets removed after 1.5 secs once activated
			break;

		case UNT_ANKLESNARE:
			if (!group->val2) {
				int time = skill_get_time2(skill_id,skill_lv);
				const struct TimerData *td = NULL;

				status_change_start(src,bl,type,10000,skill_lv,group->group_id,0,0,time,SCFLAG_FIXEDRATE);
				if (!unit_blown_immune(bl,0x9) && unit_movepos(bl,unit->bl.x,unit->bl.y,0,false))
					clif_blown(bl,&unit->bl);
				if (tsc && tsc->data[type] && (td = get_timer(tsc->data[type]->timer)))
					time = DIFF_TICK(td->tick,tick);
				clif_skillunit_update(&unit->bl);
				if (unit->hidden) {
					unit->hidden = false;
					skill_getareachar_skillunit_visibilty(unit,AREA); //Show the hidden trap
				}
				group->val2 = bl->id;
				group->limit = DIFF_TICK(tick,group->tick) + time;
			}
			break;

		case UNT_ELECTRICSHOCKER:
			if (bl->id != src->id) {
				status_change_start(src,bl,type,10000,skill_lv,group->group_id,0,0,skill_get_time2(skill_id,skill_lv),SCFLAG_FIXEDRATE);
				if (!unit_blown_immune(bl,0x9) && unit_movepos(bl,unit->bl.x,unit->bl.y,0,false))
					clif_blown(bl,&unit->bl);
				map_foreachinallrange(skill_trap_splash,&unit->bl,skill_get_splash(skill_id,skill_lv),group->bl_flag,&unit->bl,tick);
				group->unit_id = UNT_USED_TRAPS; //Changed ID so it does not invoke a for each in area again
				group->limit = DIFF_TICK(tick,group->tick); //Gets removed immediately once activated
			}
			break;

		case UNT_VENOMDUST:
		case UNT_B_TRAP:
			sc_start(src,bl,type,100,skill_lv,skill_get_time2(skill_id,skill_lv));
			break;

		case UNT_LANDMINE: //Land Mine only hits single target
			skill_attack(skill_get_type(skill_id),src,&unit->bl,bl,skill_id,skill_lv,tick,0);
			group->unit_id = UNT_USED_TRAPS;
			clif_changetraplook(&unit->bl,UNT_FIREPILLAR_ACTIVE);
			group->limit = DIFF_TICK(tick,group->tick) + 1500;
			break;

		case UNT_MAGENTATRAP:
		case UNT_COBALTTRAP:
		case UNT_MAIZETRAP:
		case UNT_VERDURETRAP:
			if (bl->type == BL_PC)
				break; //It won't work on players
		//Fall through
		case UNT_BLASTMINE:
		case UNT_SHOCKWAVE:
		case UNT_SANDMAN:
		case UNT_FLASHER:
		case UNT_FREEZINGTRAP:
		case UNT_FIREPILLAR_ACTIVE:
		case UNT_CLAYMORETRAP:
			if (skill_get_nk(skill_id)&NK_SPLASHSPLIT)
				skill_area_temp[0] = map_foreachinrange(skill_area_sub,&unit->bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,skill_id,skill_lv,tick,BCT_ENEMY,skill_area_sub_count);
			map_foreachinrange(skill_trap_splash,&unit->bl,skill_get_splash(skill_id,skill_lv),group->bl_flag,&unit->bl,tick);
			group->unit_id = UNT_USED_TRAPS;
			if (group->unit_id != UNT_FIREPILLAR_ACTIVE)
				clif_changetraplook(&unit->bl,UNT_USED_TRAPS);
			group->limit = DIFF_TICK(tick,group->tick) + 1500;
			break;

		case UNT_FIRINGTRAP:
			if (bl->id == src->id)
				break;
			map_foreachinrange(skill_trap_splash,&unit->bl,skill_get_splash(skill_id,skill_lv),group->bl_flag|BL_SKILL|~BCT_SELF,&unit->bl,tick);
			group->unit_id = UNT_USED_TRAPS;
			group->limit = DIFF_TICK(tick,group->tick);
			break;

		case UNT_ICEBOUNDTRAP:
		case UNT_CLUSTERBOMB:
			if (bl->id == src->id)
				break;
			map_foreachinrange(skill_trap_splash,&unit->bl,skill_get_splash(skill_id,skill_lv),group->bl_flag|BL_SKILL|~BCT_SELF,&unit->bl,tick);
			group->unit_id = UNT_USED_TRAPS;
			group->limit = DIFF_TICK(tick,group->tick) + 1000;
			break;

		case UNT_TALKIEBOX:
			if (bl->id == src->id || group->val2)
				break;
			clif_talkiebox(&unit->bl,group->valstr);
			group->unit_id = UNT_USED_TRAPS;
			clif_changetraplook(&unit->bl,UNT_USED_TRAPS);
			group->limit = DIFF_TICK(tick,group->tick) + 5000;
			group->val2 = -1;
			break;

		case UNT_LULLABY:
			if (bl->id == src->id)
				break;
			skill_additional_effect(src,bl,skill_id,skill_lv,BF_LONG|BF_SKILL|BF_MISC,ATK_DEF,tick);
			break;

		case UNT_UGLYDANCE:
			if (bl->id != src->id)
				skill_additional_effect(src,bl,skill_id,skill_lv,BF_LONG|BF_SKILL|BF_MISC,ATK_DEF,tick);
			break;

		case UNT_APPLEIDUN: {
				int heal;
#ifdef RENEWAL
				struct mob_data *md = BL_CAST(BL_MOB,bl);

				if (md && md->mob_id == MOBID_EMPERIUM)
					break;
#endif
				if (bl->id == src->id && !(tsc && tsc->data[SC_SPIRIT] && tsc->data[SC_SPIRIT]->val2 == SL_BARDDANCER))
					break; //Don't buff themselves!
				if (!battle_config.song_timer_reset && //Aegis style: Check if the remaining time is enough to survive the next update
					!(tsc && tsc->data[type] && tsc->data[type]->val4 == 1)) //Apple of Idun is not active, start it now
					sc_start4(src,bl,type,100,skill_lv,group->val1,group->val2,0,group->limit);
				if (tstatus->hp < tstatus->max_hp) {
					heal = skill_calc_heal(src,bl,skill_id,skill_lv,true);
					if (tsc && tsc->data[SC_AKAITSUKI] && heal)
						skill_akaitsuki_damage(&unit->bl,bl,heal,skill_id,skill_lv,tick);
					else {
						clif_skill_nodamage(&unit->bl,bl,AL_HEAL,heal,1);
						status_heal(bl,heal,0,0);
					}
				}
			}
			break;

		case UNT_POEMBRAGI:
		case UNT_WHISTLE:
		case UNT_ASSASSINCROSS:
		case UNT_HUMMING:
		case UNT_DONTFORGETME:
		case UNT_FORTUNEKISS:
		case UNT_SERVICEFORYOU:
			if (battle_config.song_timer_reset)
				break; //eA style: Doesn't need this
			if (bl->id == src->id && !(tsc && tsc->data[SC_SPIRIT] && tsc->data[SC_SPIRIT]->val2 == SL_BARDDANCER))
				break; //Don't let buff themselves!
			if (!battle_config.song_timer_reset && //Aegis style: Check if song has enough time to survive the next check
				tsc && tsc->data[type] && tsc->data[type]->val4 == 1) {
				const struct TimerData *td = get_timer(tsc->data[type]->timer);

				if (DIFF_TICK(td->tick,tick) < group->interval) { //Update with new values as the current one will vanish
					delete_timer(tsc->data[type]->timer,status_change_timer);
					tsc->data[type]->timer = add_timer(tick + group->limit,status_change_timer,bl->id,type);
					tsc->data[type]->val1 = skill_lv;
					tsc->data[type]->val2 = group->val1;
					tsc->data[type]->val3 = group->val2;
					tsc->data[type]->val4 = 0;
				}
			} else
				sc_start4(src,bl,type,100,skill_lv,group->val1,group->val2,0,group->limit); //Song was not active. start it now
			break;

		case UNT_GOSPEL:
			if (bl->id == src->id || rnd()%100 >= 50 + skill_lv * 5)
				break;
			if (battle_check_target(src,bl,BCT_PARTY) > 0) { //Support Effect only on party, not guild
				int heal;
				int i = rnd()%13; //Positive buff count
				int time = skill_get_time2(skill_id,skill_lv); //Duration

				switch (i) {
					case 0: //Heal 1000~9999 HP
						heal = rnd()%9000 + 1000;
						clif_skill_nodamage(src,bl,AL_HEAL,heal,1);
						status_heal(bl,heal,0,0);
						break;
					case 1: //End all negative status
						if (tsd)
							clif_gospel_info(tsd,0x15);
						status_change_clear_buffs(bl,SCCB_DEBUFFS|SCCB_REFRESH,0);
						break;
					case 2: //Immunity to all status
						if (tsd)
							clif_gospel_info(tsd,0x16);
						sc_start(src,bl,SC_SCRESIST,100,100,time);
						break;
					case 3: //MaxHP +100%
						if (tsd)
							clif_gospel_info(tsd,0x17);
						sc_start(src,bl,SC_INCMHPRATE,100,100,time);
						break;
					case 4: //MaxSP +100%
						if (tsd)
							clif_gospel_info(tsd,0x18);
						sc_start(src,bl,SC_INCMSPRATE,100,100,time);
						break;
					case 5: //All stats +20
						sc_start(src,bl,SC_INCALLSTATUS,100,20,time);
						if (tsd)
							clif_gospel_info(tsd,0x19);
						break;
					case 6: //Level 10 Blessing
						sc_start(src,bl,SC_BLESSING,100,10,skill_get_time(AL_BLESSING,10));
						break;
					case 7: //Level 10 Increase AGI
						sc_start(src,bl,SC_INCREASEAGI,100,10,skill_get_time(AL_INCAGI,10));
						break;
					case 8: //Enchant weapon with Holy element
						if (tsd)
							clif_gospel_info(tsd,0x1c);
						sc_start(src,bl,SC_ASPERSIO,100,1,time);
						break;
					case 9: //Enchant armor with Holy element
						if (tsd)
							clif_gospel_info(tsd,0x1d);
						sc_start(src,bl,SC_BENEDICTIO,100,1,time);
						break;
					case 10: //DEF +25%
						if (tsd)
							clif_gospel_info(tsd,0x1e);
						sc_start(src,bl,SC_INCDEFRATE,100,25,10000); //10 seconds
						break;
					case 11: //ATK +100%
						if (tsd)
							clif_gospel_info(tsd,0x1f);
						sc_start(src,bl,SC_INCATKRATE,100,100,time);
						break;
					case 12: //HIT/Flee +50
						if (tsd)
							clif_gospel_info(tsd,0x20);
						sc_start(src,bl,SC_INCHIT,100,50,time);
						sc_start(src,bl,SC_INCFLEE,100,50,time);
						break;
				}
			} else if (battle_check_target(&unit->bl,bl,BCT_ENEMY) > 0) { //Offensive Effect
				int i = rnd()%10; //Negative buff count

				switch (i) {
					case 0: //Deal 3000~7999 damage reduced by DEF
					case 1: //Deal 1500~5499 damage unreducable
						skill_attack(BF_MISC,src,&unit->bl,bl,skill_id,skill_lv,tick,1);
						break;
					case 2: //Curse
						sc_start(src,bl,SC_CURSE,100,1,1800000); //30 minutes
						break;
					case 3: //Blind
						sc_start(src,bl,SC_BLIND,100,1,1800000);
						break;
					case 4: //Poison
						sc_start(src,bl,SC_POISON,100,1,1800000);
						break;
					case 5: //Level 10 Provoke
						clif_skill_nodamage(NULL,bl,SM_PROVOKE,10,
							sc_start(src,bl,SC_PROVOKE,100,10,-1)); //Infinite
						break;
					case 6: //DEF -100%
						sc_start(src,bl,SC_INCDEFRATE,100,-100,20000); //20 seconds
						break;
					case 7: //ATK -100%
						sc_start(src,bl,SC_INCATKRATE,100,-100,20000);
						break;
					case 8: //Flee -100%
						sc_start(src,bl,SC_INCFLEERATE,100,-100,20000);
						break;
					case 9: //Speed/ASPD -75%
						sc_start4(src,bl,SC_GOSPEL,100,1,0,0,BCT_ENEMY,20000);
						break;
				}
			}
			break;

		case UNT_BASILICA:
			if (battle_check_target(&unit->bl,bl,BCT_ENEMY) > 0)
				skill_blown(&unit->bl,bl,skill_get_blewcount(skill_id,skill_lv),unit_getdir(bl),0);
			else
				sc_start4(src,bl,type,100,0,0,group->group_id,src->id,group->limit);
			break;

		case UNT_GROUNDDRIFT_WIND:
		case UNT_GROUNDDRIFT_DARK:
		case UNT_GROUNDDRIFT_POISON:
		case UNT_GROUNDDRIFT_WATER:
		case UNT_GROUNDDRIFT_FIRE:
		case UNT_GROUNDDRIFT_NEUTRAL:
			map_foreachinallrange(skill_trap_splash,&unit->bl,skill_get_splash(skill_id,skill_lv),group->bl_flag,&unit->bl,tick);
			group->unit_id = UNT_USED_TRAPS;
			//clif_changetraplook(&unit->bl,UNT_FIREPILLAR_ACTIVE);
			group->limit = DIFF_TICK(tick,group->tick);
			break;

		case UNT_POISONSMOKE:
			if (!(tsc && tsc->data[group->val2]) && rnd()%100 < 50)
				sc_start2(src,bl,(sc_type)group->val2,100,group->val3,src->id,skill_get_time2(GC_POISONINGWEAPON,1));
			break;

		case UNT_MANHOLE:
			if (!group->val2 && bl->id != src->id) {
				int range = skill_get_unit_range(skill_id,skill_lv);

				if (status_change_start(src,bl,type,10000,skill_lv,group->group_id,0,0,skill_get_time2(skill_id,skill_lv),SCFLAG_FIXEDRATE) &&
					distance_xy(unit->bl.x,unit->bl.y,bl->x,bl->y) <= range &&
					unit_movepos(bl,unit->bl.x,unit->bl.y,0,false))
				{
					clif_blown(bl,&unit->bl);
					group->val2 = bl->id;
				}
			}
			break;

		case UNT_DIMENSIONDOOR:
			if (tsd && !mapdata[bl->m].flag.noteleport)
				pc_randomwarp(tsd,CLR_TELEPORT);
			else if (bl->type == BL_MOB && (battle_config.mob_warp&8))
				unit_warp(bl,-1,-1,-1,CLR_TELEPORT);
			break;

		case UNT_CHAOSPANIC:
			sc_start4(src,bl,type,100,skill_lv,group->group_id,0,1,skill_get_time2(skill_id,skill_lv));
			break;

		case UNT_REVERBERATION:
			map_foreachinallrange(skill_trap_splash,&unit->bl,skill_get_splash(skill_id,skill_lv),group->bl_flag,&unit->bl,tick);
			group->unit_id = UNT_USED_TRAPS;
			clif_changetraplook(&unit->bl,UNT_USED_TRAPS);
			group->limit = DIFF_TICK(tick,group->tick) + 1000;
			break;

		case UNT_SEVERE_RAINSTORM:
			skill_attack(BF_WEAPON,src,&unit->bl,bl,WM_SEVERE_RAINSTORM_MELEE,skill_lv,tick,0);
			break;

		case UNT_POEMOFNETHERWORLD:
			if (tsc && tsc->data[SC_NETHERWORLD_POSTDELAY])
				break;
			if (battle_check_target(src,bl,BCT_PARTY) <= 0) {
				sc_start(src,bl,type,100,skill_lv,skill_get_time2(skill_id,skill_lv));
				group->limit = DIFF_TICK(tick,group->tick);
				group->unit_id = UNT_USED_TRAPS;
			}
			break;

		case UNT_THORNS_TRAP:
			if (!group->val2) {
				sc_start4(src,bl,type,100,skill_lv,src->id,unit->bl.id,0,group->limit - DIFF_TICK(tick,group->tick));
				group->val2 = bl->id;
			}
			break;

		case UNT_WALLOFTHORN:
			if (status_bl_has_mode(bl,MD_STATUS_IMMUNE))
				break;
			if ((map_flag_vs(bl->m) && tsd && (!tsd->status.party_id ||
				battle_check_target(src,bl,BCT_PARTY) <= 0)) ||
				battle_check_target(&unit->bl,bl,BCT_ENEMY) > 0)
				skill_attack(skill_get_type(skill_id),src,&unit->bl,bl,skill_id,skill_lv,tick,4);
			skill_blown(&unit->bl,bl,skill_get_blewcount(skill_id,skill_lv),unit_getdir(bl),2);
			unit->val3--;
			break;

		case UNT_HELLS_PLANT: {
				struct mob_data *md = BL_CAST(BL_MOB,bl);

				if (md && md->mob_id == MOBID_EMPERIUM)
					break;
				if (battle_check_target(&unit->bl,bl,BCT_ENEMY) > 0)
					skill_attack(skill_get_type(GN_HELLS_PLANT_ATK),src,&unit->bl,bl,GN_HELLS_PLANT_ATK,skill_lv,tick,0);
				if (bl->id != src->id) //The caster is the only one who can step on the plants, without destroying them
					skill_delunit(group->unit); //Deleting it directly to avoid extra hits
			}
			break;

		case UNT_VACUUM_EXTREME:
			if (tsc && (tsc->data[SC_HALLUCINATIONWALK] || tsc->data[type]))
				break;
			//Apply effect and suck targets one-by-one each n seconds
			sc_start4(src,bl,type,100,skill_lv,group->group_id,(group->val1<<16)|(group->val2),++group->val3 * 500,group->limit - DIFF_TICK(tick,group->tick));
			break;

		case UNT_LAVA_SLIDE:
			skill_attack(BF_WEAPON,src,&unit->bl,bl,skill_id,skill_lv,tick,0);
			if (++group->val1 > 4) //After 5 separate hits have been dealt, destroy the unit
				group->limit = DIFF_TICK(tick,group->tick);
			break;

		case UNT_POISON_MIST:
			skill_attack(BF_MAGIC,src,&unit->bl,bl,skill_id,skill_lv,tick,0);
			status_change_start(src,bl,SC_BLIND,(10 + 10 * skill_lv) * 100,
				skill_lv,skill_id,0,0,skill_get_time2(skill_id,skill_lv),SCFLAG_FIXEDTICK|SCFLAG_FIXEDRATE);
			break;

		case UNT_ZENKAI_WATER:
		case UNT_ZENKAI_LAND:
		case UNT_ZENKAI_FIRE:
		case UNT_ZENKAI_WIND:
			if (battle_check_target(&unit->bl,bl,BCT_ENEMY) > 0) {
				switch (group->unit_id) {
					case UNT_ZENKAI_WATER:
						switch (rnd()%3 + 1) {
							case 1:
								sc_start(src,bl,SC_FREEZE,100,skill_lv,30000);
								break;
							case 2:
								sc_start(src,bl,SC_FREEZING,100,skill_lv,40000);
								break;
							case 3:
								sc_start(src,bl,SC_CRYSTALIZE,100,skill_lv,20000);
								break;
						}
						break;
					case UNT_ZENKAI_LAND:
						switch (rnd()%2 + 1) {
							case 1:
								sc_start(src,bl,SC_STONE,100,skill_lv,20000);
								break;
							case 2:
								sc_start(src,bl,SC_POISON,100,skill_lv,20000);
								break;
						}
						break;
					case UNT_ZENKAI_FIRE:
						sc_start(src,bl,SC_BURNING,100,skill_lv,20000);
						break;
					case UNT_ZENKAI_WIND:
						switch (rnd()%3 + 1) {
							case 1:
								sc_start(src,bl,SC_SLEEP,100,skill_lv,20000);
								break;
							case 2:
								sc_start(src,bl,SC_SILENCE,100,skill_lv,20000);
								break;
							case 3:
								sc_start(src,bl,SC_DEEPSLEEP,100,skill_lv,20000);
								break;
						}
						break;
				}
			} else
				sc_start2(src,bl,type,100,skill_lv,group->val2,skill_get_time2(skill_id,skill_lv));
			break;

		case UNT_MAGMA_ERUPTION:
			skill_attack(BF_MISC,src,&unit->bl,bl,NC_MAGMA_ERUPTION_DOTDAMAGE,skill_lv,tick,0);
			break;
	}

	if (bl->type == BL_MOB && bl->id != src->id)
		mobskill_event((TBL_MOB *)bl,src,tick,MSC_SKILLUSED|(skill_id<<16));

	return skill_id;
}

/**
 * Triggered when a char steps out of a skill unit
 * @param unit Skill unit from char moved out
 * @param bl Char
 * @param tick
 */
static int skill_unit_onout(struct skill_unit *unit, struct block_list *bl, unsigned int tick)
{
	struct skill_unit_group *group;
	struct status_change *sc;
	struct status_change_entry *sce;
	enum sc_type type;

	if( !unit || !unit->group )
		return 0;

	nullpo_ret(bl);

	group = unit->group;
	sc = status_get_sc(bl);
	type = status_skill2sc(group->skill_id);
	sce = (sc && type != SC_NONE) ? sc->data[type] : NULL;

	if( !bl->prev ||
		(status_isdead(bl) && group->unit_id != UNT_ANKLESNARE) ) //Need to delete the trap if the source died
		return 0;

	switch( group->unit_id ) {
		case UNT_SAFETYWALL:
		case UNT_PNEUMA:
			if( sce )
				status_change_end(bl, type, INVALID_TIMER);
			break;

		case UNT_BASILICA:
			if( sce && sce->val4 != bl->id )
				status_change_end(bl, type, INVALID_TIMER);
			break;

		case UNT_HERMODE: //Clear Hermode if the owner moved
			if( sce && sce->val3 == BCT_SELF && sce->val4 == group->group_id )
				status_change_end(bl, type, INVALID_TIMER);
			break;

		//Used for updating timers in song overlap instances
		case UNT_UGLYDANCE:
		case UNT_DISSONANCE:
			{
				short i;

				for( i = BA_WHISTLE; i <= DC_SERVICEFORYOU; i++ ) {
					if( skill_get_inf2(i)&(INF2_SONG_DANCE) ) {
						type = status_skill2sc(i);
						sce = (sc && type != SC_NONE) ? sc->data[type] : NULL;
						if( sce )
							return i;
					}
				}
			}
		//Fall through
		case UNT_WHISTLE:
		case UNT_ASSASSINCROSS:
		case UNT_POEMBRAGI:
		case UNT_APPLEIDUN:
		case UNT_HUMMING:
		case UNT_DONTFORGETME:
		case UNT_FORTUNEKISS:
		case UNT_SERVICEFORYOU:
			if( bl->id == group->src_id && !(sc && sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_BARDDANCER) )
				return -1;
			break;
	}

	return group->skill_id;
}

/**
 * Triggered when a char steps out of a skill group (entirely) [Skotlex]
 * @param skill_id Skill ID
 * @param bl A char
 * @param tick
 */
int skill_unit_onleft(uint16 skill_id, struct block_list *bl, unsigned int tick)
{
	struct status_change *sc;
	struct status_change_entry *sce;
	enum sc_type type;

	sc = status_get_sc(bl);
	if (sc && !sc->count)
		sc = NULL;

	type = status_skill2sc(skill_id);
	sce = (sc && type != SC_NONE) ? sc->data[type] : NULL;

	switch (skill_id) {
		case WZ_QUAGMIRE:
			if (bl->type == BL_MOB)
				break;
			if (sce)
				status_change_end(bl, type, INVALID_TIMER);
			break;

		case BD_LULLABY:
		case BD_RICHMANKIM:
		case BD_ETERNALCHAOS:
		case BD_DRUMBATTLEFIELD:
		case BD_RINGNIBELUNGEN:
		case BD_ROKISWEIL:
		case BD_INTOABYSS:
		case BD_SIEGFRIED:
			//Check if you just stepped out of your ensemble skill to cancel dancing [Skotlex]
			//We don't check for SC_LONGING because someone could always have knocked you back and out of the song/dance
			//FIXME: This code is not perfect, it doesn't checks for the real ensemble's owner,
			//it only checks if you are doing the same ensemble. So if there's two chars doing an ensemble
			//which overlaps, by stepping outside of the other parther's ensemble will cause you to cancel
			//your own. Let's pray that scenario is pretty unlikely and noone will complain too much about it
			if (sc && sc->data[SC_DANCING] && (sc->data[SC_DANCING]->val1&0xFFFF) == skill_id)
				status_change_end(bl, SC_DANCING, INVALID_TIMER);
		//Fall through
		case MG_SAFETYWALL:
		case AL_PNEUMA:
		case SA_VOLCANO:
		case SA_DELUGE:
		case SA_VIOLENTGALE:
		case HW_GRAVITATION:
		case CG_HERMODE:
		case NJ_SUITON:
		case AB_EPICLESIS:
		case NC_STEALTHFIELD:
		case NC_NEUTRALBARRIER:
		case LG_BANDING:
		case SC_MAELSTROM:
		case SC_BLOODYLUST:
		case SO_CLOUD_KILL:
		case SO_WARMER:
		case SO_FIRE_INSIGNIA:
		case SO_WATER_INSIGNIA:
		case SO_WIND_INSIGNIA:
		case SO_EARTH_INSIGNIA:
		case GN_DEMONIC_FIRE:
		case GN_FIRE_EXPANSION_SMOKE_POWDER:
		case GN_FIRE_EXPANSION_TEAR_GAS:
		case SJ_BOOKOFCREATINGSTAR:
		case SU_CN_POWDERING:
		case SU_NYANGGRASS:
		case MH_STEINWAND:
		case EL_WATER_BARRIER:
		case EL_ZEPHYR:
		case EL_POWER_OF_GAIA:
			if (sce)
				status_change_end(bl, type, INVALID_TIMER);
			break;
		case DC_UGLYDANCE: //Used for updating song timers in overlap instances
		case BA_DISSONANCE:
			{
				short i;

				for (i = BA_WHISTLE; i <= DC_SERVICEFORYOU; i++) {
					if (skill_get_inf2(i)&(INF2_SONG_DANCE)) {
						type = status_skill2sc(i);
						sce = (sc && type != SC_NONE) ? sc->data[type] : NULL;
						if (sce && !sce->val4) { //We don't want dissonance updating this anymore
							delete_timer(sce->timer, status_change_timer);
							sce->val4 = 1; //Store the fact that this is a "reduced" duration effect
							sce->timer = add_timer(tick + skill_get_time2(i, sce->val1), status_change_timer, bl->id, type);
						}
					}
				}
			}
			break;
		case BA_POEMBRAGI:
		case BA_WHISTLE:
		case BA_ASSASSINCROSS:
		case BA_APPLEIDUN:
		case DC_HUMMING:
		case DC_DONTFORGETME:
		case DC_FORTUNEKISS:
		case DC_SERVICEFORYOU:
			if (sce) {
				if (battle_config.song_timer_reset || //eA style: Update every time
					(!battle_config.song_timer_reset && sce->val4 != 1)) { //Aegis style: Update only when it was not a reduced effect
					delete_timer(sce->timer, status_change_timer);
					sce->val4 = 1;
					sce->timer = add_timer(tick + skill_get_time2(skill_id, sce->val1), status_change_timer, bl->id, type);
				}
			}
			break;
		case PF_FOGWALL:
			if (sce) {
				status_change_end(bl, type, INVALID_TIMER);
				if ((sce = sc->data[SC_BLIND])) {
					if (bl->type == BL_PC) //Players get blind ended immediately, others have it still for 30 secs [Skotlex]
						status_change_end(bl, SC_BLIND, INVALID_TIMER);
					else {
						delete_timer(sce->timer, status_change_timer);
						sce->timer = add_timer(tick + 30000, status_change_timer, bl->id, SC_BLIND);
					}
				}
			}
			break;
		case GD_LEADERSHIP:
		case GD_GLORYWOUNDS:
		case GD_SOULCOLD:
		case GD_HAWKEYES:
			if (!(sce && sce->val4))
				status_change_end(bl, type, INVALID_TIMER);
			break;
	}

	return skill_id;
}

/*==========================================
 * Invoked when a unit cell has been placed/removed/deleted.
 * flag values:
 * flag&1: Invoke onplace function (otherwise invoke onout)
 * flag&4: Invoke a onleft call (the unit might be scheduled for deletion)
 * flag&8: Recursive
 *------------------------------------------*/
static int skill_unit_effect(struct block_list *bl, va_list ap)
{
	struct skill_unit *unit = va_arg(ap,struct skill_unit *);
	struct skill_unit_group *group;
	unsigned int tick = va_arg(ap,unsigned int);
	unsigned int flag = va_arg(ap,unsigned int);
	uint16 skill_id;
	bool dissonance = false;
	bool isTarget = false;

	if( !unit || (!unit->alive && !(flag&4)) || !unit->group || !bl->prev )
		return 0;

	group = unit->group;

	if( !(flag&8) ) {
		dissonance = skill_dance_switch(unit,0);
		//Target-type check
		isTarget = (group->bl_flag&bl->type && battle_check_target(&unit->bl, bl, group->target_flag) > 0);
	}

	//Necessary in case the group is deleted after calling on_place/on_out [Skotlex]
	skill_id = group->skill_id;

	if( isTarget ) {
		if( flag&1 )
			skill_unit_onplace(unit,bl,tick);
		else {
			if( skill_unit_onout(unit,bl,tick) == -1 )
				return 0; //Don't let a Bard/Dancer update their own song timer
		}
		if( flag&4 )
			skill_unit_onleft(skill_id, bl, tick);
	} else if( !isTarget && flag&4 && (group->state.song_dance&0x1 || (group->src_id == bl->id && group->state.song_dance&0x2)) )
		skill_unit_onleft(skill_id, bl, tick); //Ensemble check to terminate it

	if( dissonance ) {
		skill_dance_switch(unit,1);
		//We placed a dissonance, let's update
		map_foreachincell(skill_unit_effect,unit->bl.m,unit->bl.x,unit->bl.y,group->bl_flag,&unit->bl,gettick(),4|8);
	}

	return 0;
}

/**
 * Check skill unit while receiving damage
 * @param unit Skill unit
 * @param damage Received damage
 * @return Damage
 */
int skill_unit_ondamaged(struct skill_unit *unit, int64 damage)
{
	struct skill_unit_group *group;

	if( !unit || !unit->group )
		return 0;

	group = unit->group;

	switch( group->unit_id ) {
		case UNT_BLASTMINE:
		case UNT_SKIDTRAP:
		case UNT_LANDMINE:
		case UNT_SHOCKWAVE:
		case UNT_SANDMAN:
		case UNT_FLASHER:
		case UNT_CLAYMORETRAP:
		case UNT_FREEZINGTRAP:
		case UNT_ANKLESNARE:
		case UNT_ICEWALL:
		case UNT_WALLOFTHORN:
		case UNT_REVERBERATION:
			unit->val1 -= (int)cap_value(damage,INT_MIN,INT_MAX);
			break;
		default:
			damage = 0;
			break;
	}

	return (int)cap_value(damage,INT_MIN,INT_MAX);
}

/**
 * Check char condition around the skill caster
 * @param bl Char around area
 * @param *c Counter for 'valid' condition found
 * @param *p_sd Stores 'rid' of char found
 * @param skill_id Skill ID
 * @param skill_lv Level of used skill
 */
int skill_check_condition_char_sub(struct block_list *bl, va_list ap)
{
	int *c, skill_id, inf2;
	struct block_list *src;
	struct map_session_data *sd;
	struct map_session_data *tsd;
	int *p_sd; //Contains the list of characters found

	nullpo_ret(bl);
	nullpo_ret(tsd = (struct map_session_data *)bl);
	nullpo_ret(src = va_arg(ap,struct block_list *));
	nullpo_ret(sd = (struct map_session_data *)src);

	c = va_arg(ap,int *);
	p_sd = va_arg(ap,int *);
	skill_id = va_arg(ap,int);
	inf2 = skill_get_inf2(skill_id);

	if( skill_id == PR_BENEDICTIO ) {
		if( *c >= 2 ) //Check for two companions for Benedictio [Skotlex]
			return 0;
	} else if( (inf2&INF2_CHORUS_SKILL) || skill_id == WL_COMET ) {
		if( *c == MAX_PARTY ) //Check for partners for Chorus or Comet, cap if the entire party is accounted for
			return 0;
	} else if( *c >= 1 ) //Check for one companion for all other cases
		return 0;

	if( bl->id == src->id )
		return 0;

	if( pc_isdead(tsd) )
		return 0;

	if( !status_check_skilluse(src,bl,skill_id,0) )
		return 0;

	if( inf2&INF2_CHORUS_SKILL ) {
		if( tsd->status.party_id && sd->status.party_id &&
			tsd->status.party_id == sd->status.party_id &&
			(tsd->class_&MAPID_THIRDMASK) == MAPID_MINSTRELWANDERER )
			p_sd[(*c)++] = tsd->bl.id;
		return 1;
	} else {
		switch( skill_id ) {
			case PR_BENEDICTIO: {
					uint8 dir = map_calc_dir(&sd->bl,tsd->bl.x,tsd->bl.y);

					dir = (unit_getdir(&sd->bl) + dir)%8; //This adjusts dir to account for the direction the sd is facing
					if( (tsd->class_&MAPID_BASEMASK) == MAPID_ACOLYTE && (dir == 2 || dir == 6) && //Must be standing to the left/right of Priest
						tsd->status.sp >= 10 ) //Required Acolyte classes need have more than 10 SP
						p_sd[(*c)++] = tsd->bl.id;
				}
				return 1;
			case AB_ADORAMUS:
				if( (tsd->class_&MAPID_UPPERMASK) == MAPID_PRIEST )
					p_sd[(*c)++] = tsd->bl.id;
				return 1;
			case WL_COMET:
				if( tsd->status.party_id && sd->status.party_id &&
					tsd->status.party_id == sd->status.party_id &&
					(tsd->class_&MAPID_THIRDMASK) == MAPID_WARLOCK &&
					pc_checkskill(tsd, skill_id) > 0 )
					p_sd[(*c)++] = tsd->bl.id;
				return 1;
			default: { //Warning: Assuming Ensemble Dance/Songs for code speed [Skotlex]
					uint16 skill_lv;

					if( pc_issit(tsd) || !unit_can_move(&tsd->bl) )
						return 0;
					if( tsd->status.sex != sd->status.sex &&
						(tsd->class_&MAPID_UPPERMASK) == MAPID_BARDDANCER &&
						(skill_lv = pc_checkskill(tsd, skill_id)) > 0 &&
						(tsd->status.weapon == W_MUSICAL || tsd->status.weapon == W_WHIP) &&
						tsd->status.party_id && sd->status.party_id &&
						tsd->status.party_id == sd->status.party_id &&
						!tsd->sc.data[SC_DANCING] )
					{
						p_sd[(*c)++] = tsd->bl.id;
						return skill_lv;
					}
				}
				break;
		}
	}
	return 0;
}

/**
 * Checks and stores partners for ensemble skills [Skotlex]
 * Max partners is limited to 5.
 * @param sd Caster
 * @param skill_id
 * @param skill_lv
 * @param range Area range to check
 * @param cast_flag Special handle
 */
int skill_check_pc_partner(struct map_session_data *sd, uint16 skill_id, uint16 *skill_lv, int range, int cast_flag)
{
	static int c = 0;
	static int p_sd[MAX_PARTY];
	bool is_chorus = (skill_get_inf2(skill_id)&INF2_CHORUS_SKILL);
	uint8 i = 0;

	if (!battle_config.player_skill_partner_check || pc_has_permission(sd, PC_PERM_SKILL_UNCONDITIONAL))
		return (is_chorus ? MAX_PARTY : 99); //As if there were infinite partners

	if (cast_flag) { //Execute the skill on the partners
		struct map_session_data *tsd;

		switch (skill_id) {
			case PR_BENEDICTIO:
				for (i = 0; i < c; i++)
					if ((tsd = map_id2sd(p_sd[i])))
						status_charge(&tsd->bl, 0, 10);
				return c;
			case AB_ADORAMUS:
				if (c && (tsd = map_id2sd(p_sd[0]))) {
					i = 2 * (*skill_lv);
					status_charge(&tsd->bl, 0, i);
				}
				break;
			case WL_COMET:
				i = (c > 1 ? rnd()%c : 0);
				if (c && (tsd = map_id2sd(p_sd[i])))
					status_charge(&tsd->bl, 0, skill_get_sp(skill_id, *skill_lv) / 2);
				break;
			default: //Warning: Assuming Ensemble skills here (for speed)
				if (c && sd->sc.data[SC_DANCING] && (tsd = map_id2sd(p_sd[0]))) {
					sd->sc.data[SC_DANCING]->val4 = tsd->bl.id;
					sc_start4(&sd->bl, &tsd->bl, SC_DANCING, 100, skill_id, sd->sc.data[SC_DANCING]->val2, *skill_lv, sd->bl.id, skill_get_time(skill_id, *skill_lv) + 1000);
					clif_skill_nodamage(&tsd->bl, &sd->bl, skill_id, *skill_lv, 1);
					tsd->skill_id_dance = skill_id;
					tsd->skill_lv_dance = *skill_lv;
				}
				return c;
		}
	}

	//Else: New search for partners
	c = 0;
	memset(p_sd, 0, sizeof(p_sd));
	i = map_foreachinallrange(skill_check_condition_char_sub, &sd->bl, range, BL_PC, &sd->bl, &c, &p_sd, skill_id);

	//Apply the average lv to encore skills
	//I know c should be one, but this shows how it could be used for the average of n partners
	if (skill_id != PR_BENEDICTIO && skill_id != AB_ADORAMUS && skill_id != WL_COMET)
		*skill_lv = (i + (*skill_lv)) / (c + 1);
	return c;
}

/**
 * Sub function to count how many spawned mob is around.
 * Some skills check with matched AI.
 * @param rid Source ID
 * @param mob_class Monster ID
 * @param skill_id Used skill
 * @param *c Counter for found monster
 */
static int skill_check_condition_mob_master_sub(struct block_list *bl, va_list ap)
{
	int *c, src_id, mob_id, skill;
	uint16 ai;
	struct mob_data *md;

	md = (struct mob_data *)bl;
	src_id = va_arg(ap,int);
	mob_id = va_arg(ap,int);
	skill = va_arg(ap,int);
	c = va_arg(ap,int *);

	ai = (unsigned)(skill == AM_SPHEREMINE ? AI_SPHERE : skill == KO_ZANZOU ? AI_ZANZOU : skill == MH_SUMMON_LEGION ?
		AI_LEGION : skill == NC_SILVERSNIPER ? AI_FAW : skill == NC_MAGICDECOY ? AI_FAW : AI_FLORA);

	if( md->master_id != src_id || md->special_state.ai != ai )
		return 0; //Nothing to do here

	if( md->mob_id == mob_id )
		(*c)++;

	return 1;
}

/**
 * Determines if a given skill should be made to consume ammo
 * when used by the player. [Skotlex]
 * @param sd Player
 * @param skill_id Skill ID
 * @return True if skill is need ammo; False otherwise.
 */
int skill_isammotype(struct map_session_data *sd, uint16 skill_id)
{
	return (
		battle_config.ammo_decrement == 2 &&
		(sd->status.weapon == W_BOW || (sd->status.weapon >= W_REVOLVER && sd->status.weapon <= W_GRENADE)) &&
		skill_id != HT_PHANTASMIC &&
		skill_get_type(skill_id) == BF_WEAPON &&
		!(skill_get_nk(skill_id)&NK_NO_DAMAGE) &&
		!skill_get_spiritball(skill_id,1) //Assume spirit spheres are used as ammo instead.
	);
}

/**
 * Check target before cast a targeted skill
 * @param src Player who uses skill
 * @param bl Player who been targeted
 * @param skill_id
 * @return True if condition is met, False otherwise
 */
bool skill_check_condition_target(struct block_list *src, struct block_list *bl, uint16 skill_id, uint16 skill_lv)
{
	struct map_session_data *sd = map_id2sd(src->id);
	struct map_session_data *tsd = map_id2sd(bl->id);

	switch( skill_id ) {
		case AL_HEAL:
		case AL_INCAGI:
		case AL_DECAGI:
		case SA_DISPELL: //Mado Gear is immuned to Dispell according to bugreport:49 [Ind]
		case AB_HIGHNESSHEAL:
		case SU_TUNABELLY:
		case SU_FRESHSHRIMP:
			if( tsd && pc_ismadogear(tsd) ) { //Supportive skills that can't be cast in users with mado
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_TOTARGET,0,0);
				return false;
			}
			break;
		case MG_NAPALMBEAT:
		case MG_FIREBALL:
		case HT_BLITZBEAT:
		case AS_GRIMTOOTH:
		case MO_COMBOFINISH:
		case NC_VULCANARM:
		case SR_TIGERCANNON:
			if( bl->type == BL_SKILL ) { //These can damage traps, but can't target traps directly
				TBL_SKILL *su = (TBL_SKILL *)bl;

				if( !su || !su->group )
					return false;
				if( skill_get_inf2(su->group->skill_id)&INF2_TRAP )
					return false;
			}
			break;
		case RG_BACKSTAP: {
				uint8 dir = map_calc_dir(src,bl->x,bl->y), t_dir = unit_getdir(bl);

				if( check_distance_bl(src,bl,0) || map_check_dir(dir,t_dir) ) {
					if( sd )
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					return false;
				}
			}
			break;
		case WE_MALE:
		case WE_FEMALE:
			if( sd ) {
				struct map_session_data *p_sd = map_charid2sd(sd->status.partner_id);

				if( !p_sd || !&p_sd->bl ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					return false;
				}
			}
			break;
		case MO_KITRANSLATION:
			if( bl->id == src->id || battle_check_target(src,bl,BCT_PARTY) <= 0 ||
				(tsd && (tsd->spiritball >= 5 ||
				(tsd->class_&MAPID_BASEMASK) == MAPID_GUNSLINGER ||
				(tsd->class_&MAPID_UPPERMASK) == MAPID_REBELLION ||
				(tsd->class_&MAPID_THIRDMASK) == MAPID_ROYAL_GUARD)) )
			{
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case PF_MINDBREAKER:
			if( status_bl_has_mode(bl,MD_STATUS_IMMUNE) || battle_check_undead(status_get_race(bl),status_get_element(bl)) ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case ALL_ANGEL_PROTECT:
			if( bl->type != BL_PC ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case AB_CLEARANCE:
			if( (bl->type == BL_MOB && (status_get_class(bl) == MOBID_EMPERIUM || status_get_class_(bl) == CLASS_BATTLEFIELD)) ||
				(bl->type == BL_PC && battle_check_target(src,bl,BCT_PARTY) <= 0) ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_TOTARGET,0,0);
				return false;
			}
			break;
		case AB_SECRAMENT:
			if( battle_check_target(src,bl,BCT_NOENEMY) <= 0 ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_TOTARGET,0,0);
				return false;
			}
			break;
		case WL_WHITEIMPRISON:
			if( (bl->id != src->id && battle_check_target(src,bl,BCT_ENEMY) <= 0) || status_get_class_(bl) == CLASS_BOSS ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_TOTARGET,0,0);
				return false;
			}
			break;
		case LG_PIETY:
			if( bl->id != src->id && battle_check_target(src,bl,BCT_PARTY) <= 0 ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case SC_SHADOWFORM:
			if( bl->type != BL_PC ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_TOTARGET,0,0);
				return false;
			}
			break;
		case SR_POWERVELOCITY:
			if( tsd && (tsd->class_&MAPID_BASEMASK) == MAPID_GUNSLINGER && (tsd->class_&MAPID_UPPERMASK) == MAPID_REBELLION ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			if( bl->id == src->id ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_TOTARGET,0,0);
				return false;
			}
			break;
		case KO_KYOUGAKU:
			if( !map_flag_gvg2(src->m) && !mapdata[src->m].flag.battleground ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_SIEGE,0,0);
				return false;
			}
			if( !tsd || tsd->sc.data[SC_KYOUGAKU] ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_TOTARGET,0,0);
				return false;
			}
			break;
		case KO_JYUSATSU:
		case KG_KAGEMUSYA:
		case OB_OBOROGENSOU:
			if( bl->type != BL_PC ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_TOTARGET,0,0);
				return false;
			}
			break;
		case RL_D_TAIL: {
				int count;

				count = map_foreachinrange(skill_area_sub,src,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,
					skill_id,skill_lv,gettick(),BCT_ENEMY,skill_area_sub_count);
				if( !count ) {
					if( sd )
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					return false;
				}
			}
			break;
		case SU_SV_ROOTTWIST:
			if( status_bl_has_mode(bl,MD_STATUS_IMMUNE) ) {
				if( sd )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_TOTARGET,0,0);
				return false;
			}
			break;
		case HLIF_HEAL:
		case HLIF_AVOID:
		case HAMI_DEFENCE:
		case HAMI_CASTLE:
			if( !battle_get_master(src) )
				return false;
			break;
	}

	return true;
}

/**
 * Check SC required to cast a skill
 * @param sc
 * @param skill_id
 * @return True if condition is met, False otherwise
 */
static bool skill_check_condition_sc_required(struct map_session_data *sd, unsigned short skill_id, struct skill_condition *require)
{
	uint8 i = 0;
	struct status_change *sc = NULL;

	nullpo_ret(sd);

	if( !require->status_count )
		return true;

	if( !require || !skill_get_index(skill_id) )
		return false;

	if( !(sc = &sd->sc) ) {
		clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
		return false;
	}

	//May has multiple requirements
	for( i = 0; i < require->status_count; i++ ) {
		enum sc_type req_sc = require->status[i];

		if( req_sc == SC_NONE )
			continue;

		switch( req_sc ) {
			//Official fail msg
			case SC_PUSH_CART:
				if( !sc->data[SC_PUSH_CART] ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_CART,0,0);
					return false;
				}
				break;
			case SC_POISONINGWEAPON:
				if( !sc->data[SC_POISONINGWEAPON] ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_GC_POISONINGWEAPON,0,0);
					return false;
				}
				break;
			default:
				if( !sc->data[req_sc] ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_CONDITION,0,0);
					return false;
				}
				break;
		}
	}

	return true;
}

/** Check skill condition when cast begin
 * For ammo, only check if the skill need ammo
 * For checking ammo requirement (type and amount) will be skill_check_condition_castend
 * @param sd Player who uses skill
 * @param skill_id ID of used skill
 * @param skill_lv Level of used skill
 * @return true: All condition passed, false: Failed
 */
bool skill_check_condition_castbegin(struct map_session_data *sd, uint16 skill_id, uint16 skill_lv)
{
	struct status_data *status;
	struct status_change *sc;
	struct skill_condition require;
	int i;
	uint32 inf2;

	nullpo_retr(false,sd);

	if( sd->chatID )
		return false;

	//GMs don't override the skillItem check, otherwise they can use items without them being consumed! [Skotlex]
	if( pc_has_permission(sd,PC_PERM_SKILL_UNCONDITIONAL) && sd->skillitem != skill_id ) {
		sd->state.arrow_atk = (skill_get_ammotype(skill_id) ? 1 : 0); //Need to do arrow state check
		sd->spiritball_old = sd->spiritball; //Need to do spiritball check
		sd->rageball_old = sd->rageball; //Need to do rageball check
		sd->charmball_old = sd->charmball; //Need to do charmball check
		sd->soulball_old = sd->soulball; //Need to do soulball check
		return true;
	}

	switch( sd->menuskill_id ) {
		case AM_PHARMACY:
			switch( skill_id ) {
				case AM_PHARMACY:
				case AC_MAKINGARROW:
				case BS_REPAIRWEAPON:
				case AM_TWILIGHT1:
				case AM_TWILIGHT2:
				case AM_TWILIGHT3:
					return false;
			}
			break;
		case GN_MIX_COOKING:
		case GN_MAKEBOMB:
		case GN_S_PHARMACY:
		case GN_CHANGEMATERIAL:
			if( sd->menuskill_id != skill_id )
				return false;
			break;
	}

	status = &sd->battle_status;

	sc = &sd->sc;

	if( sc && !sc->count )
		sc = NULL;

	if( sd->skillitem != skill_id ) {
		uint32 inf3;

		if( pc_is90overweight(sd) ) {
			clif_skill_fail(sd,skill_id,USESKILL_FAIL_WEIGHTOVER,0,0);
			return false;
		}
		inf3 = skill_get_inf3(skill_id);
		if( pc_isridingwug(sd) && !(inf3&INF3_USABLE_WARG) ) //Check the skills that can be used while mounted on a warg
			return false; //In official there is no fail message
		if( pc_ismadogear(sd) && !(inf3&INF3_USABLE_MADO) ) { //Check the skills that can be used while mounted on a mado
			clif_skill_fail(sd,skill_id,USESKILL_FAIL_MADOGEAR_RIDE,0,0);
			return false;
		}
	}

	if( skill_lv < 1 || skill_lv > MAX_SKILL_LEVEL )
		return false;

	require = skill_get_requirement(sd,skill_id,skill_lv);

	sd->state.arrow_atk = (require.ammo ? 1 : 0); //Can only update state when weapon/arrow info is checked

	inf2 = skill_get_inf2(skill_id);

	//Perform skill-group checks
	if( (inf2&INF2_ENSEMBLE_SKILL) && !skill_check_pc_partner(sd,skill_id,&skill_lv,1,0) ) {
		clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_HELPER,0,0);
		return false;
	}

	if( (inf2&INF2_CHORUS_SKILL) && !skill_check_pc_partner(sd,skill_id,&skill_lv,AREA_SIZE + 1,0) ) {
		clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_HELPER,0,0);
		return false;
	}

	switch( skill_id ) { //Perform skill-specific checks (and actions)
		case AL_WARP:
			if( !battle_config.duel_allow_teleport && sd->duel_group ) { //Duel restriction [LuzZza]
				char output[128];

				sprintf(output,msg_txt(sd,365),skill_get_name(AL_WARP));
				clif_displaymessage(sd->fd,output); //"Duel: Can't use %s in duel."
				return false;
			}
			break;
		case AL_HOLYWATER:
			if( map_getcell(sd->bl.m,sd->bl.x,sd->bl.y,CELL_CHKLANDPROTECTOR) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false; //Aqua Benedicta will not cast on LP [secretdataz]
			}
			break;
		case SO_SPELLFIST:
			if( sd->skill_id_old != MG_FIREBOLT && sd->skill_id_old != MG_COLDBOLT && sd->skill_id_old != MG_LIGHTNINGBOLT ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
		case SA_CASTCANCEL:
			if( sd->ud.skilltimer == INVALID_TIMER ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case AS_CLOAKING: {
				if( skill_lv < 3 && ((sd->bl.type == BL_PC && (battle_config.pc_cloak_check_type&1)) ||
					(sd->bl.type != BL_PC && (battle_config.monster_cloak_check_type&1))) ) { //Check for walls.
					static const int dx[] = { 0,1,0,-1,-1, 1,1,-1};
					static const int dy[] = {-1,0,1, 0,-1,-1,1, 1};

					ARR_FIND(0,8,i,map_getcell(sd->bl.m,sd->bl.x + dx[i],sd->bl.y + dy[i],CELL_CHKNOPASS) != 0);
					if( i == 8 ) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						return false;
					}
				}
			}
			break;
		case CR_GRANDCROSS:
			if( status_isimmune(&sd->bl) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case MO_CALLSPIRITS:
			if( sc && sc->data[SC_RAISINGDRAGON] )
				skill_lv += sc->data[SC_RAISINGDRAGON]->val1;
			if( sd->spiritball >= skill_lv ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case MO_FINGEROFFENSIVE:
		case GS_FLING:
			if( sd->spiritball > 0 && sd->spiritball < require.spiritball )
				sd->spiritball_old = require.spiritball = sd->spiritball;
			else
				sd->spiritball_old = require.spiritball;
			break;
		case MO_CHAINCOMBO:
			if( !sc )
				return false;
			if( sc->data[SC_BLADESTOP] )
				break;
			if( sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == MO_TRIPLEATTACK )
				break;
			return false;
		case MO_COMBOFINISH:
			if( !(sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == MO_CHAINCOMBO) )
				return false;
			break;
		case CH_TIGERFIST:
			if( !(sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == MO_COMBOFINISH) )
				return false;
			break;
		case CH_CHAINCRUSH:
			if( !(sc && sc->data[SC_COMBO]) )
				return false;
			if( sc->data[SC_COMBO]->val1 != MO_COMBOFINISH && sc->data[SC_COMBO]->val1 != CH_TIGERFIST )
				return false;
			break;
		case MO_EXTREMITYFIST:
#ifndef RENEWAL
			//if( sc && sc->data[SC_EXTREMITYFIST] ) //To disable Asura during the 5 min skill block uncomment this
				//return false;
#endif
			if( sc && (sc->data[SC_BLADESTOP] || sc->data[SC_CURSEDCIRCLE_ATKER]) )
				break;
			if( sc && sc->data[SC_COMBO] ) {
				switch( sc->data[SC_COMBO]->val1 ) {
					case MO_COMBOFINISH:
					case CH_TIGERFIST:
					case CH_CHAINCRUSH:
						break;
					default:
						return false;
				}
			} else if( !unit_can_move(&sd->bl) ) { //Placed here as ST_MOVE_ENABLE should not apply if rooted or on a combo. [Skotlex]
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case TK_MISSION:
			if( (sd->class_&MAPID_UPPERMASK) != MAPID_TAEKWON ) { //Cannot be used by Non-Taekwon classes
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case TK_READYCOUNTER:
		case TK_READYDOWN:
		case TK_READYSTORM:
		case TK_READYTURN:
		case TK_JUMPKICK:
			if( (sd->class_&MAPID_UPPERMASK) == MAPID_SOUL_LINKER ) { //Soul Linkers cannot use this skill
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case TK_TURNKICK:
		case TK_STORMKICK:
		case TK_DOWNKICK:
		case TK_COUNTER:
			if( (sd->class_&MAPID_UPPERMASK) == MAPID_SOUL_LINKER )
				return false; //Anti-Soul Linker check in case you job-changed with Stances active
			if( !(sc && sc->data[SC_COMBO]) || sc->data[SC_COMBO]->val1 == TK_JUMPKICK )
				return false; //Combo needs to be ready
			if( sc->data[SC_COMBO]->val3 ) { //Kick chain
				if( sc->data[SC_COMBO]->val3 != skill_id )
					break; //Do not repeat a kick
				status_change_end(&sd->bl,SC_COMBO,INVALID_TIMER);
				return false;
			}
			if( sc->data[SC_COMBO]->val1 != skill_id && !pc_is_taekwon_ranker(sd) ) { //Cancel combo wait
				unit_cancel_combo(&sd->bl);
				return false;
			}
			break; //Combo ready
		case BD_ADAPTATION: {
				int time;

				if( !(sc && sc->data[SC_DANCING]) ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					return false;
				}
				time = 1000 * (sc->data[SC_DANCING]->val3>>16);
				if( skill_get_time(
					(sc->data[SC_DANCING]->val1&0xFFFF), //Dance Skill ID
					(sc->data[SC_DANCING]->val1>>16)) //Dance Skill LV
					- time < skill_get_time2(skill_id,skill_lv) )
				{
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					return false;
				}
			}
			break;
		case PR_BENEDICTIO:
			if( skill_check_pc_partner(sd,skill_id,&skill_lv,1,0) < 2 ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case SL_SMA:
			if( !(sc && (sc->data[SC_SMA] || sc->data[SC_USE_SKILL_SP_SHA])) )
				return false;
			break;
		case HT_POWER:
			if( !(sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == AC_DOUBLE) )
				return false;
			break;
		case CG_HERMODE:
			if( !npc_check_areanpc(1,sd->bl.m,sd->bl.x,sd->bl.y,skill_get_splash(skill_id,skill_lv)) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case CG_MOONLIT: { //Check there's no wall in the range+1 area around the caster [Skotlex]
				int range = skill_get_splash(skill_id,skill_lv) + 1;
				int size = range * 2 + 1;

				for( i = 0; i < size * size; i++ ) {
					int x = sd->bl.x + (i % size - range);
					int y = sd->bl.y + (i / size - range);

					if( map_getcell(sd->bl.m,x,y,CELL_CHKWALL) ) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						return false;
					}
				}
			}
			break;
		case HP_BASILICA:
			if( !(sc && sc->data[SC_BASILICA]) ) {
				if( sd ) {
					int range = skill_get_unit_layout_type(skill_id,skill_lv) + 1;
					int size = range * 2 + 1;

					if( map_getcell(sd->bl.m,sd->bl.x,sd->bl.y,CELL_CHKLANDPROTECTOR) ||
						map_getcell(sd->bl.m,sd->bl.x,sd->bl.y,CELL_CHKMAELSTROM) ) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						return false;
					}
					//Needs 7x7 clear area
					for( i = 0; i < size * size; i++ ) {
						int x = sd->bl.x + (i % size - range);
						int y = sd->bl.y + (i / size - range);

						if( map_getcell(sd->bl.m,x,y,CELL_CHKWALL) ) {
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
							return false;
						}
					}
					if( map_foreachinallrange(skill_count_wos,&sd->bl,range,BL_ALL,&sd->bl) ) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						return false;
					}
				}
			}
			break;
		case AM_TWILIGHT2:
		case AM_TWILIGHT3:
			if( !party_skill_check(sd,sd->status.party_id,skill_id,skill_lv) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case SG_SUN_WARM:
		case SG_MOON_WARM:
		case SG_STAR_WARM:
			if( sc && sc->data[SC_MIRACLE] )
				break;
			i = skill_id - SG_SUN_WARM;
			if( sd->bl.m == sd->feel_map[i].m )
				break;
			clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			return false;
		case SG_SUN_COMFORT:
		case SG_MOON_COMFORT:
		case SG_STAR_COMFORT:
			if( sc && sc->data[SC_MIRACLE] )
				break;
			i = skill_id - SG_SUN_COMFORT;
			if( sd->bl.m == sd->feel_map[i].m && (battle_config.allow_skill_without_day || sg_info[i].day_func()) )
				break;
			clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			return false;
		case SG_FUSION:
			if( sc && sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_STAR )
				break;
			//Auron insists we should implement SP consumption when you are not Soul Linked [Skotlex]
			//Only invoke on skill begin cast (instant cast skill) [Kevin]
			if( require.sp > 0 ) {
				if( status->sp < (unsigned int)require.sp )
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_SP_INSUFFICIENT,0,0);
				else
					status_zap(&sd->bl,0,require.sp);
			}
			return false;
		case GD_BATTLEORDER:
		case GD_REGENERATION:
		case GD_RESTORE:
			if( !map_flag_gvg2(sd->bl.m) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
		//Fall through
		case GD_EMERGENCYCALL:
		case GD_ITEMEMERGENCYCALL:
			if( !sd->status.guild_id || !sd->state.gmaster_flag )
				return false; //Other checks were already done in skill_isNotOk()
			break;
		case GS_GLITTERING:
			if( sd->spiritball >= 10 ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case GS_MADNESSCANCEL:
			if( sc && (sc->data[SC_P_ALTER] || sc->data[SC_HEAT_BARREL]) ) {
				clif_msg(sd,SKILL_REBEL_GUN_FAIL);
				return false;
			}
			break;
		case NJ_ISSEN:
#ifdef RENEWAL
			if( status->hp < status->hp / 100 )
#else
			if( status->hp < 2 )
#endif
			{
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
		case NJ_BUNSINJYUTSU:
			if( !(sc && sc->data[SC_NEN]) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case NJ_ZENYNAGE:
		case KO_MUCHANAGE:
			if( sd->status.zeny < require.zeny ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_MONEY,0,0);
				return false;
			}
			break;
		case PF_HPCONVERSION:
			if( status->sp == status->max_sp )
				return false; //Unusable when at full SP
			break;
		case AM_CALLHOMUN: //Can't summon if a hom is already out
			if( sd->status.hom_id && sd->hd && !sd->hd->homunculus.vaporize ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case AM_REST: //Can't vapo homun if you don't have an active homunc or it's hp is < 80%
			if( !hom_is_active(sd->hd) || sd->hd->battle_status.hp < (sd->hd->battle_status.max_hp * 80 / 100) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
#ifdef RENEWAL
		case ASC_EDP:
			if( sd->weapontype1 == W_FIST ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_WRONG_WEAPON,0,0);
				return false;
			}
			break;
#endif
		case AB_ANCILLA: {
				int count = 0;

				for( i = 0; i < MAX_INVENTORY; i++ ) {
					if( sd->inventory.u.items_inventory[i].nameid == ITEMID_ANCILLA )
						count += sd->inventory.u.items_inventory[i].amount;
				}
				if( count >= 3 ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_ANCILLA_NUMOVER,0,0);
					return false;
				}
				i = pc_search_inventory(sd,require.itemid[0]);
				if( i == INDEX_NOT_FOUND || sd->inventory.u.items_inventory[i].amount < require.amount[0] ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_BLUEJAMSTONE,0,0);
					return false;
				}
			}
			break;
		case AB_EPICLESIS:
			i = pc_search_inventory(sd,require.itemid[0]);
			if( i == INDEX_NOT_FOUND || sd->inventory.u.items_inventory[i].amount < require.amount[0] ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_HOLYWATER,0,0);
				return false;
			}
			break;
		case AB_ADORAMUS:
		case WL_COMET:
			i = pc_search_inventory(sd,require.itemid[0]);
			if( i == INDEX_NOT_FOUND || sd->inventory.u.items_inventory[i].amount < require.amount[0] ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_ITEM,require.amount[0],require.itemid[0]);
				return false;
			}
			break;
		case WL_SUMMONFB:
		case WL_SUMMONBL:
		case WL_SUMMONWB:
		case WL_SUMMONSTONE:
		case WL_TETRAVORTEX:
		case WL_RELEASE:
			{
				int j = 0;

				for( i = SC_SPHERE_1; i <= SC_SPHERE_5; i++ ) {
					if( !(sc && sc->data[i]) )
						continue;
					j++;
				}
				switch( skill_id ) {
					case WL_TETRAVORTEX:
						if( j < 4 ) {
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_CONDITION,0,0);
							return false;
						}
						break;
					case WL_RELEASE:
						for( i = SC_SPELLBOOK1; i <= SC_MAXSPELLBOOK; i++ ) {
							if( sc && sc->data[i] )
								j++;
						}
						if( !j ) {
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_SUMMON_NONE,0,0);
							return false;
						}
						break;
					default:
						if( j == 5 ) {
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_SUMMON,0,0);
							return false;
						}
						break;
				}
			}
			break;
		case GC_HALLUCINATIONWALK:
			if( sc && (sc->data[SC_HALLUCINATIONWALK] || sc->data[SC_HALLUCINATIONWALK_POSTDELAY]) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case GC_COUNTERSLASH:
		case GC_WEAPONCRUSH:
			if( !(sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == GC_WEAPONBLOCKING) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_GC_WEAPONBLOCKING,0,0);
				return false;
			}
			break;
		case RA_WUGMASTERY:
			if( (pc_isfalcon(sd) && !battle_config.warg_can_falcon) || pc_isridingwug(sd) || (sc && sc->data[SC__GROOMY]) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case RA_WUGSTRIKE:
			if( !pc_iswug(sd) && !pc_isridingwug(sd) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case RA_WUGRIDER:
			if( (pc_isfalcon(sd) && !battle_config.warg_can_falcon) || (!pc_isridingwug(sd) && !pc_iswug(sd)) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case RA_WUGDASH:
			if( !pc_isridingwug(sd) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			} else {
				int16 sx = sd->bl.x;
				int16 sy = sd->bl.y;
				uint8 dir = (unit_getdir(&sd->bl))%8;

				switch( dir ) {
					case DIR_NORTH: sy++; break;
					case DIR_NORTHWEST: sx--; sy++; break;
					case DIR_WEST: sx--; break;
					case DIR_SOUTHWEST: sx--; sy--; break;
					case DIR_SOUTH: sy--; break;
					case DIR_SOUTHEAST: sx++; sy--; break;
					case DIR_EAST: sx++; break;
					case DIR_NORTHEAST: sx++; sy++; break;
				}
				if( map_count_oncell(sd->bl.m,sx,sy,BL_CHAR,1) > 0 )
					return false;
			}
			break;
		case NC_SILVERSNIPER:
		case NC_MAGICDECOY:
			{
				uint8 maxcount = skill_get_maxcount(skill_id,skill_lv);
				uint8 c = 0;
				short mob_id = 0;

				if( battle_config.land_skill_limit && maxcount > 0 && (battle_config.land_skill_limit&BL_PC) ) {
					if( skill_id == NC_SILVERSNIPER ) //Check for Silver Sniper
						map_foreachinmap(skill_check_condition_mob_master_sub,sd->bl.m,BL_MOB,sd->bl.id,MOBID_SILVERSNIPER,skill_id,&c);
					else { //Check for Magic Decoy Fire/Water/Earth/Wind types
						for( mob_id = MOBID_MAGICDECOY_FIRE; mob_id <= MOBID_MAGICDECOY_WIND; mob_id++ )
							map_foreachinmap(skill_check_condition_mob_master_sub,sd->bl.m,BL_MOB,sd->bl.id,mob_id,skill_id,&c);
					}
					if( c >= maxcount ) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_SUMMON,0,0);
						return false;
					}
				}
			}
			break;
		case LG_PRESTIGE:
			if( sc && sc->data[SC_BANDING] ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case LG_RAGEBURST:
			if( sd->rageball > 0 )
				sd->rageball_old = sd->rageball;
			break;
		case LG_SHIELDSPELL: {
				short index = sd->equip_index[EQI_HAND_L];
				struct item_data *shield_data = NULL;

				if( index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_ARMOR )
					shield_data = sd->inventory_data[index];
				//Skill will first check if a shield is equipped, if none is found the skill will fail
				if( !shield_data || shield_data->type != IT_ARMOR ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					break;
				}
			}
			break;
		case LG_RAYOFGENESIS:
		case LG_HESPERUSLIT:
			if( sc && sc->data[SC_INSPIRATION] )
				return true; //Don't check for partner
			if( !(sc && sc->data[SC_BANDING]) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			if( sc->data[SC_BANDING]->val2 < (skill_id == LG_RAYOFGENESIS ? 2 : 3) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_ROYAL_GUARD_BANDING,0,0);
				return false;
			}
			break;
		case LG_INSPIRATION: {
				unsigned int exp, exp_needp = battle_config.exp_cost_inspiration;

				if( exp_needp && (exp = pc_nextbaseexp(sd)) > 0 && get_percentage(sd->status.base_exp,exp) < exp_needp ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_EXP_1PERCENT,0,0);
					return false;
				}
			}
			break;
		case SC_MANHOLE:
		case SC_DIMENSIONDOOR:
			if( sc && sc->data[SC_MAGNETICFIELD] ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case SR_FALLENEMPIRE:
			if( !(sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == SR_DRAGONCOMBO) )
				return false;
			break;
		case SR_RAMPAGEBLASTER:
			if( sd->spiritball > 0 )
				sd->spiritball_old = sd->spiritball;
			else {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_SPIRIT,0,0);
				return false;
			}
			break;
		case SR_CRESCENTELBOW:
			if( sc && sc->data[SC_CRESCENTELBOW] ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_DUPLICATE,0,0);
				return false;
			}
			break;
		case SR_CURSEDCIRCLE:
			if( !battle_config.cursed_circle_in_gvg && map_flag_gvg2(sd->bl.m) ) {
				if( map_foreachinallrange(mob_count_sub,&sd->bl,skill_get_splash(skill_id,skill_lv),BL_MOB,
					MOBID_EMPERIUM,MOBID_GUARDIAN_STONE1,MOBID_GUARDIAN_STONE2) ) {
					char output[128];

					sprintf(output,"%s",msg_txt(sd,382)); // You're too close to a stone or emperium to do this skill
					clif_messagecolor(&sd->bl,color_table[COLOR_RED],output,false,SELF);
					return false;
				}
			}
			if( sd->spiritball > 0 )
				sd->spiritball_old = sd->spiritball;
			else {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_SPIRIT,1,0);
				return false;
			}
			break;
		case SO_FIREWALK:
		case SO_ELECTRICWALK: //Can't be casted until you've walked all cells
			if( sc && sc->data[SC_PROPERTYWALK] &&
				sc->data[SC_PROPERTYWALK]->val3 < skill_get_maxcount(sc->data[SC_PROPERTYWALK]->val1,sc->data[SC_PROPERTYWALK]->val2) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case SO_EL_CONTROL:
			if( !sd->status.ele_id || !sd->ed ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case RL_RICHS_COIN:
			if( sd->spiritball >= 10 ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_SUMMON,0,0);
				return false;
			}
			break;
		case RL_P_ALTER:
			if( sc && (sc->data[SC_MADNESSCANCEL] || sc->data[SC_HEAT_BARREL]) ) {
				clif_msg(sd,SKILL_REBEL_GUN_FAIL);
				return false;
			}
			break;
		case RL_HEAT_BARREL:
			if( sc && (sc->data[SC_MADNESSCANCEL] || sc->data[SC_P_ALTER]) ) {
				clif_msg(sd,SKILL_REBEL_GUN_FAIL);
				return false;
			}
			break;
		case SJ_FULLMOONKICK:
			if( !(sc && sc->data[SC_NEWMOON]) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case SJ_SOLARBURST:
			if( !(sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == SJ_PROMINENCEKICK) )
				return false;
			break;
		case SP_SOULUNITY:
			if( !sd->status.party_id ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case SP_SWHOO:
			if( !(sc && sc->data[SC_USE_SKILL_SP_SPA]) )
				return false;
			break;
		case KO_JYUMONJIKIRI:
			if( sd->weapontype1 && (sd->weapontype2 || sd->status.shield) )
				return true;
			else {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case KO_KAHU_ENTEN:
		case KO_HYOUHU_HUBUKI:
		case KO_KAZEHU_SEIRAN:
		case KO_DOHU_KOUKAI:
			if( sd->charmball_type == skill_get_ele(skill_id,skill_lv) && sd->charmball >= MAX_CHARMBALL ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_SUMMON,0,0);
				return false;
			}
			break;
		case KO_KAIHOU:
		case KO_ZENKAI:
			if( sd->charmball_type == CHARM_TYPE_NONE || sd->charmball <= 0 ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_SUMMON_NONE,0,0);
				return false;
			}
			sd->charmball_old = sd->charmball;
			break;
	}

	switch( require.state ) { //Check state required
		case ST_HIDDEN:
			if( !pc_ishiding(sd) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case ST_RIDING:
			if( !pc_isriding(sd) && !pc_isridingdragon(sd) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case ST_FALCON:
			if( !pc_isfalcon(sd) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case ST_CART:
			if( !pc_iscarton(sd) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_CART,0,0);
				return false;
			}
			break;
		case ST_SHIELD:
			if( sd->status.shield <= 0 ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case ST_RECOV_WEIGHT_RATE:
#ifdef RENEWAL
			if( pc_is70overweight(sd) ) {
#else
			if( pc_is50overweight(sd) ) {
#endif
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case ST_MOVE_ENABLE:
			if( sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == skill_id )
				sd->ud.canmove_tick = gettick(); //When using a combo, cancel the can't move delay to enable the skill [Skotlex]
			if( !unit_can_move(&sd->bl) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case ST_WATER:
			if( sc && (sc->data[SC_DELUGE] || sc->data[SC_SUITON]) )
				break;
			if( map_getcell(sd->bl.m,sd->bl.x,sd->bl.y,CELL_CHKWATER) )
				break;
			clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
			return false;
		case ST_RIDINGDRAGON:
			if( !pc_isridingdragon(sd) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_DRAGON,0,0);
				return false;
			}
			break;
		case ST_WUG:
			if( !pc_iswug(sd) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case ST_RIDINGWUG:
			if( !pc_isridingwug(sd) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case ST_MADO:
			if( !pc_ismadogear(sd) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_MADOGEAR,0,0);
				return false;
			}
			break;
		case ST_ELEMENTALSPIRIT:
			if( !sd->ed ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_EL_SUMMON,0,0);
				return false;
			}
			break;
		case ST_PECO:
			if( !pc_isriding(sd) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case ST_SUNSTANCE:
			if( !(sc && (sc->data[SC_SUNSTANCE] || sc->data[SC_UNIVERSESTANCE])) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case ST_LUNARSTANCE:
			if( !(sc && (sc->data[SC_LUNARSTANCE] || sc->data[SC_UNIVERSESTANCE])) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case ST_STARSTANCE:
			if( !(sc && (sc->data[SC_STARSTANCE] || sc->data[SC_UNIVERSESTANCE])) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case ST_UNIVERSESTANCE:
			if( !(sc && sc->data[SC_UNIVERSESTANCE]) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
	}

	if( require.status_count ) { //Check the status required
		switch( skill_id ) {
			case WZ_SIGHTRASHER:
				break; //Being checked later in skill_check_condition_castend()
			default:
				if( !skill_check_condition_sc_required(sd,skill_id,&require) )
					return false;
				break;
		}
	}

	if( require.mhp > 0 && get_percentage(status->hp, status->max_hp) < require.mhp ) {
		clif_skill_fail(sd,skill_id,USESKILL_FAIL_HP_INSUFFICIENT,0,0);
		return false;
	}

	if( require.weapon && !pc_check_weapontype(sd,require.weapon) ) {
		switch( skill_id ) {
			case RA_AIMEDBOLT:
				break;
			default:
				if( require.weapon&(1<<W_REVOLVER) )
					clif_msg(sd,SKILL_NEED_REVOLVER);
				else if( require.weapon&(1<<W_RIFLE) )
					clif_msg(sd,SKILL_NEED_RIFLE);
				else if( require.weapon&(1<<W_GATLING) )
					clif_msg(sd,SKILL_NEED_GATLING);
				else if( require.weapon&(1<<W_SHOTGUN) )
					clif_msg(sd,SKILL_NEED_SHOTGUN);
				else if( require.weapon&(1<<W_GRENADE) )
					clif_msg(sd,SKILL_NEED_GRENADE);
				else
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_WRONG_WEAPON,0,0);
				return false;
		}
	}

	if( require.sp > 0 && status->sp < (unsigned int)require.sp ) {
		clif_skill_fail(sd,skill_id,USESKILL_FAIL_SP_INSUFFICIENT,0,0);
		return false;
	}

	if( require.zeny > 0 && sd->status.zeny < require.zeny ) {
		clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_MONEY,0,0);
		return false;
	}

	switch( skill_id ) {
		case AM_BERSERKPITCHER:
		case AM_POTIONPITCHER:
		case CR_SLIMPITCHER:
			break;
		default:
			for( i = 0; i < MAX_SKILL_ITEM_REQUIRE; ++i ) {
				short index[MAX_SKILL_ITEM_REQUIRE];

				if( !require.itemid[i] )
					continue;
				index[i] = pc_search_inventory(sd,require.itemid[i]);
				if( index[i] == INDEX_NOT_FOUND || sd->inventory.u.items_inventory[index[i]].amount < require.amount[i] ) {
					switch( require.itemid[i] ) {
						case ITEMID_YELLOW_GEMSTONE:
						case ITEMID_RED_GEMSTONE:
						case ITEMID_BLUE_GEMSTONE:
						case ITEMID_HOLY_WATER:
							break;
						case ITEMID_ANCILLA:
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_ANCILLA,0,0);
							return false;
						case ITEMID_PAINT_BRUSH:
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_PAINTBRUSH,0,0);
							return false;
						default:
							if( itemdb_is_elementpoint(require.itemid[i]) )
								clif_skill_fail(sd,skill_id,USESKILL_FAIL_STUFF_INSUFFICIENT,0,0);
							else
								clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_ITEM,require.amount[i],require.itemid[i]);
							return false;
					}
				}
			}
			break;
	}

	switch( skill_id ) {
		case GS_FLING:
		case GS_TRIPLEACTION:
		case GS_BULLSEYE:
		case GS_ADJUSTMENT:
		case GS_MAGICALBULLET:
		case MH_SONIC_CRAW:
		case MH_SILVERVEIN_RUSH:
		case MH_MIDNIGHT_FRENZY:
		case MH_TINDER_BREAKER:
		case MH_CBC:
		case MH_EQC:
			break;
		case GS_INCREASING:
		case GS_CRACKER:
		case GS_MADNESSCANCEL:
		case RL_FLICKER:
		case RL_E_CHAIN:
		case RL_C_MARKER:
		case RL_P_ALTER:
		case RL_FALLEN_ANGEL:
		case RL_HEAT_BARREL:
		case RL_HAMMER_OF_GOD:
			if( (require.spiritball > 0 && sd->spiritball < require.spiritball) ||
				(require.spiritball == -1 && !sd->spiritball) ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_COIN,(require.spiritball > 0 ? require.spiritball : 0),0);
				return false;
			}
			if( require.spiritball == -1 )
				sd->spiritball_old = sd->spiritball;
			break;
		case SP_SOULGOLEM:
		case SP_SOULSHADOW:
		case SP_SOULFALCON:
		case SP_SOULFAIRY:
		case SP_SOULCURSE:
		case SP_SPA:
		case SP_SHA:
		case SP_SWHOO:
		case SP_SOULUNITY:
		case SP_SOULDIVISION:
		case SP_SOULREAPER:
		case SP_SOULEXPLOSION:
		case SP_KAUTE:
			if( require.spiritball > 0 && sd->soulball < require.spiritball ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_SPIRIT,require.spiritball,0);
				return false;
			}
			break;
		default:
			if( require.spiritball > 0 && sd->spiritball < require.spiritball ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_SPIRIT,require.spiritball,0);
				return false;
			}
			break;
	}

	if( require.ammo ) {
		short idx = sd->equip_index[EQI_AMMO];

		if( idx < 0 || !sd->inventory_data[idx] || !(require.ammo&(1<<sd->inventory_data[idx]->look)) ||
			sd->inventory.u.items_inventory[idx].amount < require.ammo_qty ) {
			switch( skill_id ) {
				case BA_MUSICALSTRIKE:
				case DC_THROWARROW:
				case CG_ARROWVULCAN:
				case GS_BULLSEYE:
					break;
				case RA_ARROWSTORM:
					clif_arrow_fail(sd,0);
					return false;
				case NC_ARMSCANNON:
				case GN_CARTCANNON:
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_CANONBALL,0,0);
					return false;
				case GN_SLINGITEM:
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_WRONG_WEAPON,0,0);
					return false;
				default:
					if( require.ammo&((1<<AMMO_BULLET)|(1<<AMMO_SHELL)|(1<<AMMO_GRENADE)) ) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_BULLET,0,0);
						return false;
					}
					break;
			}
		}
	}

	if( require.eqItem_count ) {
		uint8 count = require.eqItem_count;

		for( i = 0; i < require.eqItem_count; i++ ) {
			uint16 reqeqit = require.eqItem[i];

			if( !reqeqit )
				break; //Skill has no required item(s)
			switch( skill_id ) {
				case NC_PILEBUNKER:
				case RL_P_ALTER:
					if( !pc_checkequip2(sd,reqeqit,EQI_ACC_L,EQI_MAX) ) {
						count--;
						if( !count ) {
							if( skill_id == RL_P_ALTER )
								clif_msg(sd,SKILL_NEED_HOLY_BULLET);
							else
								clif_skill_fail(sd,skill_id,USESKILL_FAIL_WRONG_WEAPON,0,0);
							return false;
						} else
							continue;
					}
					break;
				case NC_ACCELERATION:
				case NC_SELFDESTRUCTION:
				case NC_SHAPESHIFT:
				case NC_EMERGENCYCOOL:
				case NC_MAGNETICFIELD:
				case NC_NEUTRALBARRIER:
				case NC_STEALTHFIELD:
					if( pc_search_inventory(sd,reqeqit) == INDEX_NOT_FOUND ) {
						count--;
						if( !count ) {
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_EQUIPMENT,0,require.eqItem[0]);
							return false;
						} else
							continue;
					}
					break;
				default: //Check if equiped item
					if( !pc_checkequip2(sd,reqeqit,EQI_ACC_L,EQI_MAX) ) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_EQUIPMENT,0,reqeqit);
						return false;
					}
					break;
			}
		}
	}

	return true;
}

/** Check skill condition when cast end.
 * Checking ammo requirement (type and amount) will be here, not at skill_check_condition_castbegin
 * @param sd Player who uses skill
 * @param skill_id ID of used skill
 * @param skill_lv Level of used skill
 * @return true: All condition passed, false: Failed
 */
bool skill_check_condition_castend(struct map_session_data *sd, uint16 skill_id, uint16 skill_lv)
{
	struct skill_condition require;
	struct status_data *status;
	struct status_change *sc;
	int i;

	nullpo_retr(false,sd);

	switch( skill_id ) {
		case MO_INVESTIGATE:
		case MO_FINGEROFFENSIVE:
		case MO_EXTREMITYFIST:
		case SJ_FULLMOONKICK:
			break;
		default:
			if( !skill_check_condition_castbegin(sd,skill_id,skill_lv) )
				return false;
			break;
	}

	status = &sd->battle_status;

	sc = &sd->sc;

	if( sc && !sc->count )
		sc = NULL;

	//Perform skill-specific checks (and actions)
	switch( skill_id ) {
		case PR_BENEDICTIO:
		case AB_ADORAMUS:
		case WL_COMET:
			skill_check_pc_partner(sd,skill_id,&skill_lv,1,1);
			break;
		case AM_CANNIBALIZE:
		case AM_SPHEREMINE:
			{
				int c = 0;
				int summons[5] = { MOBID_G_MANDRAGORA,MOBID_G_HYDRA,MOBID_G_FLORA,MOBID_G_PARASITE,MOBID_G_GEOGRAPHER };
				int maxcount = (skill_id == AM_CANNIBALIZE) ? 6 - skill_lv : skill_get_maxcount(skill_id,skill_lv);
				int mob_id = (skill_id == AM_CANNIBALIZE)? summons[skill_lv - 1] : MOBID_MARINE_SPHERE;

				if( battle_config.land_skill_limit && maxcount > 0 && (battle_config.land_skill_limit&BL_PC) ) {
					i = map_foreachinmap(skill_check_condition_mob_master_sub,sd->bl.m,BL_MOB,sd->bl.id,mob_id,skill_id,&c);
					if( c >= maxcount || (skill_id == AM_CANNIBALIZE && c != i && (battle_config.summon_flora&2)) ) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						return false; //Fails when exceed max limit, there are other plant types already out
					}
				}
			}
			break;
		case GS_MADNESSCANCEL:
			if( sc && sc->data[SC_ADJUSTMENT] ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case GS_ADJUSTMENT:
			if( sc && sc->data[SC_MADNESSCANCEL] ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
				return false;
			}
			break;
		case KO_ZANZOU: {
				uint8 c = 0;

				i = map_foreachinmap(skill_check_condition_mob_master_sub,sd->bl.m,BL_MOB,sd->bl.id,MOBID_KO_KAGE,skill_id,&c);
				if( c >= skill_get_maxcount(skill_id,skill_lv) || c != i ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
					return false;
				}
			}
			break;
	}

	require = skill_get_requirement(sd,skill_id,skill_lv);

	if( require.status_count ) {
		switch( skill_id ) {
			case WZ_SIGHTRASHER:
				if( !skill_check_condition_sc_required(sd,skill_id,&require) )
					return false;
				break;
		}
	}

	if( require.hp > 0 && status->hp <= (unsigned int)require.hp ) {
		clif_skill_fail(sd,skill_id,USESKILL_FAIL_HP_INSUFFICIENT,0,0);
		return false;
	}

	if( require.weapon && !pc_check_weapontype(sd,require.weapon) ) {
		switch( skill_id ) {
			case RA_AIMEDBOLT:
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_WRONG_WEAPON,0,0);
				return false;
		}
	}

	switch( skill_id ) {
		case AS_SPLASHER:
		case AB_ANCILLA:
		case AB_EPICLESIS:
			break;
		default:
			for( i = 0; i < MAX_SKILL_ITEM_REQUIRE; ++i ) {
				short index[MAX_SKILL_ITEM_REQUIRE];

				if( !require.itemid[i] )
					continue;
				index[i] = pc_search_inventory(sd,require.itemid[i]);
				if( index[i] == INDEX_NOT_FOUND || sd->inventory.u.items_inventory[index[i]].amount < require.amount[i] ) {
					if( skill_id == SA_DISPELL || skill_id == HP_BASILICA || skill_id == HW_GANBANTEIN ) {
						clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0,0);
						return false;
					}
					switch( require.itemid[i] ) {
						case ITEMID_YELLOW_GEMSTONE:
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_ITEM,require.amount[i],require.itemid[i]);
							return false;
						case ITEMID_RED_GEMSTONE:
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_REDJAMSTONE,0,0);
							return false;
						case ITEMID_BLUE_GEMSTONE:
							if( skill_id == ALL_RESURRECTION && sd->skillitem == skill_id )
								continue;
							else if( skill_id == SA_LANDPROTECTOR )
								clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_ITEM,require.amount[i],require.itemid[i]);
							else
								clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_BLUEJAMSTONE,0,0);
							return false;
						case ITEMID_HOLY_WATER:
							clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_HOLYWATER,0,0);
							return false;
					}
				}
			}
			break;
	}

	switch( skill_id ) {
		case MO_FINGEROFFENSIVE:
			if( require.spiritball > 0 && sd->spiritball < require.spiritball )
				sd->spiritball_old = sd->spiritball;
			break;
		case GS_FLING:
		case GS_TRIPLEACTION:
		case GS_BULLSEYE:
		case GS_ADJUSTMENT:
		case GS_MAGICALBULLET:
			if( require.spiritball > 0 && sd->spiritball < require.spiritball ) {
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_COIN,require.spiritball,0);
				return false;
			}
			break;
	}

	if( require.ammo ) {
		short idx = sd->equip_index[EQI_AMMO];
		int ammo_qty = require.ammo_qty;

		switch( skill_id ) {
			case WM_SEVERE_RAINSTORM:
			case RL_R_TRIP:
			case RL_FIRE_RAIN:
				ammo_qty += 1; //2016-10-26 kRO update made these skills require an extra ammo to cast
				break;
		}
		if( idx < 0 || !sd->inventory_data[idx] || sd->inventory.u.items_inventory[idx].amount < ammo_qty ) {
			if( require.ammo&((1<<AMMO_BULLET)|(1<<AMMO_SHELL)|(1<<AMMO_GRENADE)) )
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_BULLET,0,0);
			else if( require.ammo&(1<<AMMO_KUNAI) )
				clif_skill_fail(sd,skill_id,USESKILL_FAIL_NEED_EQUIPMENT_KUNAI,0,0);
			else
				clif_arrow_fail(sd,0);
			return false;
		}
	}

	return true;
}

/**
 * Consume skill requirement
 * @param sd Player who uses the skill
 * @param skill_id ID of used skill
 * @param skill_lv Level of used skill
 * @param type Consume type
 *  type&1: consume others requirement
 *  type&2: consume items requirement
 */
void skill_consume_requirement(struct map_session_data *sd, uint16 skill_id, uint16 skill_lv, short type)
{
	struct skill_condition require;

	nullpo_retv(sd);

	require = skill_get_requirement(sd,skill_id,skill_lv);

	if( type&1 ) {
		switch( skill_id ) {
			case MO_ABSORBSPIRITS:
			case PR_REDEMPTIO:
			case CG_TAROTCARD:
				require.sp = 0; //Will consume SP in skill_cast_nodamage_id [Inkfish]
				break;
			default:
				if( sd->state.autocast )
					require.sp = 0;
				if( skill_disable_check(&sd->sc,skill_id) )
					require.sp = 0; //No SP requirement when canceling the status
				break;
		}

		if( require.hp || require.sp )
			status_zap(&sd->bl,require.hp,require.sp);

		if( require.spiritball > 0 ) {
			if( sd->spiritball > 0 )
				pc_delspiritball(sd,require.spiritball,0);
			if( sd->soulball > 0 )
				pc_delsoulball(sd,require.spiritball,0);
		} else if( require.spiritball == -1 ) {
			if( sd->spiritball > 0 )
				pc_delspiritball(sd,sd->spiritball,0);
			if( sd->rageball > 0 )
				pc_delrageball(sd,sd->rageball,0);
			if( sd->charmball > 0 && sd->charmball_type != CHARM_TYPE_NONE )
				pc_delcharmball(sd,sd->charmball,sd->charmball_type);
		}

		if( require.zeny > 0 ) {
			if( skill_id == NJ_ZENYNAGE )
				require.zeny = 0; //Zeny is reduced on skill_attack
			if( sd->status.zeny < require.zeny )
				require.zeny = sd->status.zeny;
			pc_payzeny(sd,require.zeny,LOG_TYPE_CONSUME,NULL);
		}
	}

	if( type&2 ) {
		struct status_change *sc = &sd->sc;
		int n, i;

		if( sc && !sc->count )
			sc = NULL;

		for( i = 0; i < MAX_SKILL_ITEM_REQUIRE; ++i ) {
			if( !require.itemid[i] )
				continue;

			if( itemdb_is_gemstone(require.itemid[i]) && skill_id != HW_GANBANTEIN &&
				sc && sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_WIZARD )
				continue; //Gemstones are checked, but not substracted from inventory

			switch( skill_id ) {
				case SA_SEISMICWEAPON:
					if( sc && sc->data[SC_UPHEAVAL_OPTION] && rnd()%100 < 50 )
						continue;
					break;
				case SA_FLAMELAUNCHER:
				case SA_VOLCANO:
					if( sc && sc->data[SC_TROPIC_OPTION] && rnd()%100 < 50 )
						continue;
					break;
				case SA_FROSTWEAPON:
				case SA_DELUGE:
					if( sc && sc->data[SC_CHILLY_AIR_OPTION] && rnd()%100 < 50 )
						continue;
					break;
				case SA_LIGHTNINGLOADER:
				case SA_VIOLENTGALE:
					if( sc && sc->data[SC_WILD_STORM_OPTION] && rnd()%100 < 50 )
						continue;
					break;
			}

			if( (n = pc_search_inventory(sd,require.itemid[i])) != INDEX_NOT_FOUND )
				pc_delitem(sd,n,require.amount[i],0,1,LOG_TYPE_CONSUME);
		}
	}
}

/**
 * Get skill requirements and return the value after some additional/reduction condition (such item bonus and status change)
 * @param sd Player's that will be checked
 * @param skill_id Skill that's being used
 * @param skill_lv Skill level of used skill
 * @return skill_condition Struct 'skill_condition' that store the modified skill requirements
 */
struct skill_condition skill_get_requirement(struct map_session_data *sd, uint16 skill_id, uint16 skill_lv)
{
	struct skill_condition require;
	struct status_data *status;
	struct status_change *sc;
	int i, hp_rate, sp_rate, sp_skill_rate_bonus = 100;
	uint16 idx = skill_get_index(skill_id);
	bool level_dependent = false;

	memset(&require,0,sizeof(require));

	if( !sd )
		return require;

	sc = &sd->sc;

	if( sc && !sc->count )
		sc = NULL;

	if( !idx ) //Invalid skill id
		return require;

	if( skill_lv < 1 || skill_lv > MAX_SKILL_LEVEL )
		return require;

	status = &sd->battle_status;
	require.hp = skill_db[idx].require.hp[skill_lv - 1];
	hp_rate = skill_db[idx].require.hp_rate[skill_lv - 1];

	if( hp_rate > 0 )
		require.hp += (status->hp * hp_rate) / 100;
	else
		require.hp += (status->max_hp * (-hp_rate)) / 100;

	require.sp = skill_db[idx].require.sp[skill_lv - 1];

	if( sd->skill_id_old == BD_ENCORE && skill_id == sd->skill_id_dance )
		require.sp /= 2;

	if( skill_id == sd->status.skill[sd->reproduceskill_idx].id )
		require.sp += require.sp * 30 / 100;

	sp_rate = skill_db[idx].require.sp_rate[skill_lv - 1];

	if( sp_rate > 0 )
		require.sp += (status->sp * sp_rate) / 100;
	else
		require.sp += (status->max_sp * (-sp_rate)) / 100;

	if( sd->dsprate != 100 )
		require.sp = require.sp * sd->dsprate / 100;

	ARR_FIND(0,MAX_PC_BONUS,i,sd->skillusesprate[i].id == skill_id);
	if( i < MAX_PC_BONUS )
		sp_skill_rate_bonus += sd->skillusesprate[i].val;
	if( sp_skill_rate_bonus != 100 )
		require.sp = cap_value(require.sp * sp_skill_rate_bonus / 100,0,SHRT_MAX);

	ARR_FIND(0,MAX_PC_BONUS,i,sd->skillusesp[i].id == skill_id);
	if( i < MAX_PC_BONUS )
		require.sp += sd->skillusesp[i].val;

	if( sc ) {
		if( sc->data[SC_RECOGNIZEDSPELL] )
			require.sp += require.sp * 25 / 100;
		if( sc->data[SC__LAZINESS] )
			require.sp += sc->data[SC__LAZINESS]->val1 * 10;
		if( sc->data[SC_UNLIMITEDHUMMINGVOICE] )
			require.sp += require.sp * sc->data[SC_UNLIMITEDHUMMINGVOICE]->val3 / 100;
		if( sc->data[SC_TELEKINESIS_INTENSE] && skill_get_ele(skill_id,skill_lv) == ELE_GHOST)
			require.sp -= require.sp * sc->data[SC_TELEKINESIS_INTENSE]->val2 / 100;
		if( sc->data[SC_OFFERTORIUM] )
			require.sp += require.sp * sc->data[SC_OFFERTORIUM]->val3 / 100;
	}

	if( sd->skillitem == skill_id )
		require.sp = 0;

	require.zeny = skill_db[idx].require.zeny[skill_lv - 1];

	if( sc && sc->data[SC__UNLUCKY] ) {
		if( sc->data[SC__UNLUCKY]->val1 < 3 )
			require.zeny += sc->data[SC__UNLUCKY]->val1 * 250;
		else
			require.zeny += 1000;
	}

	require.spiritball = skill_db[idx].require.spiritball[skill_lv - 1];
	require.state = skill_db[idx].require.state;
	require.mhp = skill_db[idx].require.mhp[skill_lv - 1];
	require.weapon = skill_db[idx].require.weapon;
	require.ammo_qty = skill_db[idx].require.ammo_qty[skill_lv - 1];

	if( require.ammo_qty )
		require.ammo = skill_db[idx].require.ammo;

	if( !require.ammo && skill_id && skill_isammotype(sd,skill_id) ) { //Assume this skill is using the weapon, therefore it requires arrows
		require.ammo = 0xFFFFFFFF; //Enable use on all ammo types
		require.ammo_qty = 1;
	}

	require.status_count = skill_db[idx].require.status_count;
	require.status = skill_db[idx].require.status;
	require.eqItem_count = skill_db[idx].require.eqItem_count;
	require.eqItem = skill_db[idx].require.eqItem;

	switch( skill_id ) {
		//Skill level-dependent checks
		case NC_SHAPESHIFT: //NOTE: Please make sure Magic_Gear_Fuel in the last position in skill_require_db.txt
		case NC_REPAIR: //NOTE: Please make sure Repair_Kit in the last position in skill_require_db.txt
			require.itemid[1] = skill_db[idx].require.itemid[MAX_SKILL_ITEM_REQUIRE - 1];
			require.amount[1] = skill_db[idx].require.amount[MAX_SKILL_ITEM_REQUIRE - 1];
		//Fall through
		case WZ_FIREPILLAR: //No gems required at level 1-5 [celest]
		case GN_FIRE_EXPANSION:
		case SO_SUMMON_AGNI:
		case SO_SUMMON_AQUA:
		case SO_SUMMON_VENTUS:
		case SO_SUMMON_TERA:
		case SO_FIRE_INSIGNIA:
		case SO_WATER_INSIGNIA:
		case SO_WIND_INSIGNIA:
		case SO_EARTH_INSIGNIA:
		case KO_MAKIBISHI:
			require.itemid[0] = skill_db[idx].require.itemid[min(skill_lv - 1,MAX_SKILL_ITEM_REQUIRE - 1)];
			require.amount[0] = skill_db[idx].require.amount[min(skill_lv - 1,MAX_SKILL_ITEM_REQUIRE - 1)];
			level_dependent = true;
		//Fall through
		default: //Normal skill requirements and gemstone checks
			for( i = 0; i < (!level_dependent ? MAX_SKILL_ITEM_REQUIRE : 2); i++ ) {
				//Skip this for level_dependent requirement, just looking forward for gemstone removal. Assumed if there is gemstone there
				if( !level_dependent ) {
					switch( skill_id ) {
						case AM_POTIONPITCHER:
						case CR_SLIMPITCHER:
						case CR_CULTIVATION:
							if( i != skill_lv%11 - 1 )
								continue;
							break;
						case AM_CALLHOMUN:
							if( sd->status.hom_id )
								continue; //Don't delete items when hom is already out
							break;
						case AB_ADORAMUS:
						case WL_COMET:
							if( skill_check_pc_partner(sd,skill_id,&skill_lv,1,0) )
								continue;
							break;
					}
					require.itemid[i] = skill_db[idx].require.itemid[i];
					require.amount[i] = skill_db[idx].require.amount[i];
					if( (skill_id >= HT_SKIDTRAP && skill_id <= HT_TALKIEBOX && pc_checkskill(sd,RA_RESEARCHTRAP) > 0) ||
						skill_id == SC_ESCAPE ) {
						short item_index;

						if( (item_index = pc_search_inventory(sd,require.itemid[i])) == INDEX_NOT_FOUND ||
							sd->inventory.u.items_inventory[item_index].amount < require.amount[i] ) {
							require.itemid[i] = ITEMID_SPECIAL_ALLOY_TRAP;
							require.amount[i] = 1;
						}
						break;
					}
				}
				if( itemdb_is_gemstone(require.itemid[i]) ) { //Check requirement for gemstone
					if( sd->special_state.no_gemstone == 2 ) //Remove all Magic Stone required for all skills for VIP
						require.itemid[i] = require.amount[i] = 0;
					else { //All gem skills except Hocus Pocus and Ganbantein can cast for free with Mistress card [helvetica]
						if( (sd->special_state.no_gemstone || (sc && sc->data[SC_INTOABYSS])) && skill_id != HW_GANBANTEIN ) {
							if( skill_id != SA_ABRACADABRA )
								require.itemid[i] = require.amount[i] = 0;
							else if( --require.amount[i] < 1 )
								require.amount[i] = 1;
						}
					}
				}
				if( require.itemid[i] == ITEMID_MAGIC_GEAR_FUEL && sd->special_state.no_mado_fuel )
					require.itemid[i] = require.amount[i] = 0;
			}
			break;
	}

	//Check for cost reductions due to skills & SCs
	switch( skill_id ) {
		case MC_MAMMONITE:
			if( pc_checkskill(sd,BS_UNFAIRLYTRICK) > 0 )
				require.zeny -= require.zeny * 10 / 100;
			break;
		case AL_HOLYLIGHT:
			if( sc && sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_PRIEST )
				require.sp *= 5;
			break;
		case SL_SMA:
		case SL_STUN:
		case SL_STIN:
			{
				int kaina_lv = (sd ? pc_checkskill(sd,SL_KAINA) : 7);

				if( !kaina_lv || !sd || sd->status.base_level < 70 )
					break;
				if( sd->status.base_level >= 90 )
					require.sp -= require.sp * 7 * kaina_lv / 100;
				else if( sd->status.base_level >= 80 )
					require.sp -= require.sp * 5 * kaina_lv / 100;
				else if( sd->status.base_level >= 70 )
					require.sp -= require.sp * 3 * kaina_lv / 100;
			}
			break;
		case MO_CHAINCOMBO:
		case MO_COMBOFINISH:
		case CH_TIGERFIST:
		case CH_CHAINCRUSH:
			if( sc && sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_MONK )
				require.sp = 2; //Monk Spirit makes monk/champion combo skills cost 2 SP regardless of original cost
			break;
		case MO_BODYRELOCATION:
			if( sc && sc->data[SC_EXPLOSIONSPIRITS] )
				require.spiritball = 0;
			break;
		case MO_EXTREMITYFIST:
			if( sc ) {
				if( sc->data[SC_BLADESTOP] )
					require.spiritball--;
				else if( sc->data[SC_COMBO] ) {
					switch( sc->data[SC_COMBO]->val1 ) {
						case MO_COMBOFINISH:
							require.spiritball = 4;
							break;
						case CH_TIGERFIST:
							require.spiritball = 3;
							break;
						case CH_CHAINCRUSH: //It should consume whatever is left as long as it's at least 1
							require.spiritball = (sd->spiritball ? sd->spiritball : 1);
							break;
					}
				} else if( sc->data[SC_RAISINGDRAGON] && sd->spiritball > 5 )
					require.spiritball = sd->spiritball; //Must consume all regardless of the amount required
			}
			break;
#ifdef RENEWAL
		case WZ_FIREPILLAR:
			if( skill_lv < 5 )
				break;
		//Fall through
		case HW_GRAVITATION:
		case MG_SAFETYWALL:
		case MG_STONECURSE:
			if( sc && sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_WIZARD )
				require.sp += require.sp * 50 / 100;
			break;
		case AS_SONICBLOW:
			if( sc ) {
				if( sc->data[SC_EDP] )
					require.sp += require.sp;
				if( sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_ASSASIN )
					require.sp *= 2;
			}
			break;
		case BA_POEMBRAGI:
		case BA_WHISTLE:
		case BA_ASSASSINCROSS:
		case BA_APPLEIDUN:
			if( !sd->status.sex && sc && sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_BARDDANCER )
				require.sp *= 2;
			break;
		case DC_HUMMING:
		case DC_DONTFORGETME:
		case DC_FORTUNEKISS:
		case DC_SERVICEFORYOU:
			if( sd->status.sex && sc && sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_BARDDANCER )
				require.sp *= 2;
			break;
#endif
		case GC_CROSSIMPACT:
		case GC_COUNTERSLASH:
			if( sc && sc->data[SC_EDP] )
				require.sp += require.sp;
			break;
		case SR_GATEOFHELL:
			if( sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == SR_FALLENEMPIRE )
				require.sp -= require.sp * 10 / 100;
			break;
		case WM_GREAT_ECHO:
			require.sp -= require.sp * 20 * skill_chorus_count(sd,0) / 100; //-20% each W/M in the party
			break;
		case SO_SUMMON_AGNI:
		case SO_SUMMON_AQUA:
		case SO_SUMMON_VENTUS:
		case SO_SUMMON_TERA:
			{
				int spirit_sympathy = pc_checkskill(sd,SO_EL_SYMPATHY);

				if( spirit_sympathy )
					require.sp -= require.sp * (5 + 5 * spirit_sympathy) / 100;
			}
			break;
		case SO_PSYCHIC_WAVE:
			if( sc && (sc->data[SC_HEATER_OPTION] || sc->data[SC_COOLER_OPTION] ||
				sc->data[SC_BLAST_OPTION] ||  sc->data[SC_CURSED_SOIL_OPTION]) )
				require.sp += require.sp / 2; //1.5x SP cost
			break;
	}

	//Check if player is using the copied skill [Cydh]
	if( sd->status.skill[idx].flag == SKILL_FLAG_PLAGIARIZED ) {
		uint16 req_opt = skill_db[idx].copyable.req_opt;

		if( req_opt&0x0001 ) require.hp = 0;
		if( req_opt&0x0002 ) require.mhp = 0;
		if( req_opt&0x0004 ) require.sp = 0;
		if( req_opt&0x0008 ) require.hp_rate = 0;
		if( req_opt&0x0010 ) require.sp_rate = 0;
		if( req_opt&0x0020 ) require.zeny = 0;
		if( req_opt&0x0040 ) require.weapon = 0;
		if( req_opt&0x0080 ) { require.ammo = 0; require.ammo_qty = 0; }
		if( req_opt&0x0100 ) require.state = ST_NONE;
		if( req_opt&0x0200 ) require.status_count = 0;
		if( req_opt&0x0400 ) require.spiritball = 0;
		if( req_opt&0x0800 ) { memset(require.itemid,0,sizeof(require.itemid)); memset(require.amount,0,sizeof(require.amount)); }
		if( req_opt&0x1000 ) require.eqItem_count = 0;
	}

	return require;
}

/*==========================================
 * Does cast-time reductions based on dex, item bonuses and config setting
 *------------------------------------------*/
int skill_castfix(struct block_list *bl, uint16 skill_id, uint16 skill_lv) {
	double time = skill_get_cast(skill_id, skill_lv);

	nullpo_ret(bl);

#ifndef RENEWAL_CAST
	{
		struct map_session_data *sd = BL_CAST(BL_PC, bl);
		struct status_change *sc = status_get_sc(bl);
		int reduce_ct_r = 0;
		uint8 flag = skill_get_castnodex(skill_id, skill_lv);

		//Calculate base cast time (reduced by dex)
		if( !(flag&1) ) {
			int scale = battle_config.castrate_dex_scale - status_get_dex(bl);

			if( scale > 0 ) //Not instant cast
				time = time * (float)scale / battle_config.castrate_dex_scale;
			else
				return 0; //Instant cast
		}

		//Calculate cast time reduced by item/card bonuses
		if( sd ) {
			uint8 i;

			if( !(flag&4) && sd->castrate != 100 )
				reduce_ct_r += 100 - sd->castrate;
			//Skill-specific reductions work regardless of flag
			ARR_FIND(0, MAX_PC_BONUS, i, sd->skillcastrate[i].id == skill_id);
			if( i < MAX_PC_BONUS )
				time += time * sd->skillcastrate[i].val / 100;
		}

		//These cast time reductions are processed even if the skill fails
		if( sc && sc->count ) {
			//Magic Strings stacks additively with item bonuses
			if( !(flag&2) && sc->data[SC_POEMBRAGI] )
				reduce_ct_r += sc->data[SC_POEMBRAGI]->val2;
			//Foresight halves the cast time, it does not stack additively
			if( sc->data[SC_MEMORIZE] ) {
				if( !(flag&2) )
					time -= time * 50 / 100;
				//Foresight counter gets reduced even if the skill is not affected by it
				if( --sc->data[SC_MEMORIZE]->val2 <= 0 )
					status_change_end(bl, SC_MEMORIZE, INVALID_TIMER);
			}
		}

		time = time * (1 - (float)reduce_ct_r / 100);
	}
#endif

	//Config cast time multiplier
	if( battle_config.cast_rate != 100 )
		time = time * battle_config.cast_rate / 100;

	//Return final cast time
	time = max(time, 0);
	//ShowInfo("Castime castfix = %f\n",time);

	return (int)time;
}

#ifndef RENEWAL_CAST
/** Skill cast time modifiers for Pre-Re cast
 * @param bl: The caster
 * @param time: Cast time before Status Change addition or reduction
 * @return time: Modified castime after status change addition or reduction
 */
int skill_castfix_sc(struct block_list *bl, double time, uint16 skill_id, uint16 skill_lv)
{
	struct status_change *sc = status_get_sc(bl);
	struct status_change_entry *sce = NULL;
	uint8 flag = skill_get_castnodex(skill_id, skill_lv);

	if( time < 0 )
		return 0;

	if( bl->type == BL_MOB || bl->type == BL_NPC )
		return (int)time; //Cast time is fixed nothing to alter

	if( sc && sc->count ) {
		if( !(flag&2) ) {
			if( (sce = sc->data[SC_SLOWCAST]) )
				time += time * sce->val2 / 100;
			if( sc->data[SC_FREEZING] )
				time += time * 50 / 100;
			if( (sce = sc->data[SC__LAZINESS]) )
				time += time * sce->val3 / 100;
			if( (sce = sc->data[SC_PARALYSIS]) )
				time += time * sce->val3 / 100;
			if( (sce = sc->data[SC_SUFFRAGIUM]) )
				time -= time * sce->val2 / 100;
			if( (sce = sc->data[SC_SOULFAIRY]) )
				time -= time * sce->val3 / 100;
			if( sc->data[SC_IZAYOI] )
				time -= time * 50 / 100;
			if( (sce = sc->data[SC_WATER_INSIGNIA]) && sce->val1 == 3 &&
				skill_get_type(skill_id) == BF_MAGIC && skill_get_ele(skill_id, skill_lv) == ELE_WATER )
				time -= time * 30 / 100;
			if( (sce = sc->data[SC_TELEKINESIS_INTENSE]) )
				time -= time * sce->val2 / 100;
		}
		//Suffragium ends even if the skill is not affected by it
		status_change_end(bl, SC_SUFFRAGIUM, INVALID_TIMER);
	}

	time = max(time, 0);
	//ShowInfo("Castime castfix_sc = %f\n",time);

	return (int)time;
}
#else
/** Skill cast time calculation for RENEWAL_CAST
 * @param bl: The caster
 * @param time: Cast time without reduction
 * @param skill_id: Skill ID of the casted skill
 * @param skill_lv: Skill level of the casted skill
 * @return time: Modified castime after status and bonus addition or reduction
 */
int skill_vfcastfix(struct block_list *bl, double time, uint16 skill_id, uint16 skill_lv)
{
	struct map_session_data *sd = BL_CAST(BL_PC,bl);
	struct status_change *sc = status_get_sc(bl);
	struct status_change_entry *sce = NULL;
	int fixed = skill_get_fixed_cast(skill_id, skill_lv), fixcast_r = 0, varcast_r = 0, reduce_ct_r = 0;
	uint8 i = 0, flag = skill_get_castnodex(skill_id, skill_lv);

	if( time < 0 )
		return 0;

	if( bl->type == BL_MOB || bl->type == BL_NPC )
		return (int)time; //Cast time is fixed nothing to alter

	if( fixed < 0 || !battle_config.default_fixed_castrate ) //No fixed cast time
		fixed = 0;
	else if( !fixed ) {
		fixed = (int)time * battle_config.default_fixed_castrate / 100; //Fixed time
		time = time * (100 - battle_config.default_fixed_castrate) / 100; //Variable time
	}

	//Increases/Decreases fixed/variable cast time of a skill by item/card bonuses
	if( sd && !(flag&4) ) {
		if( sd->bonus.varcastrate )
			reduce_ct_r += sd->bonus.varcastrate;
		if( sd->bonus.fixcastrate )
			fixcast_r -= sd->bonus.fixcastrate; //Just speculation
		if( sd->bonus.add_varcast )
			time += sd->bonus.add_varcast;
		if( sd->bonus.add_fixcast )
			fixed += sd->bonus.add_fixcast;
		ARR_FIND(0, MAX_PC_BONUS, i, sd->skillfixcast[i].id == skill_id);
		if( i < MAX_PC_BONUS )
			fixed += sd->skillfixcast[i].val;
		ARR_FIND(0, MAX_PC_BONUS, i, sd->skillvarcast[i].id == skill_id);
		if( i < MAX_PC_BONUS )
			time += sd->skillvarcast[i].val;
		ARR_FIND(0, MAX_PC_BONUS, i, sd->skillcastrate[i].id == skill_id);
		if( i < MAX_PC_BONUS )
			reduce_ct_r += sd->skillcastrate[i].val;
		ARR_FIND(0, MAX_PC_BONUS, i, sd->skillfixcastrate[i].id == skill_id);
		if( i < MAX_PC_BONUS )
			fixcast_r = max(fixcast_r, sd->skillfixcastrate[i].val);
	}

	if( sc && sc->count ) {
		if( !(flag&2) ) {
			//All variable cast additive bonuses must come first
			if( (sce = sc->data[SC_SLOWCAST]) )
				VARCAST_REDUCTION(-sce->val2);
#ifdef RENEWAL
			if( (sce = sc->data[SC_SPIRIT]) && sce->val2 == SL_WIZARD ) {
				switch( skill_id ) {
					case WZ_FIREPILLAR:
						if( skill_lv < 5 )
							break;
					//Fall through
					case HW_GRAVITATION:
					case MG_SAFETYWALL:
					case MG_STONECURSE:
						VARCAST_REDUCTION(-50);
						break;
				}
			}
#endif
			if( (sce = sc->data[SC__LAZINESS]) )
				VARCAST_REDUCTION(-sce->val3);
			if( (sce = sc->data[SC_PARALYSIS]) )
				VARCAST_REDUCTION(-sce->val3);
			//Variable cast reduction bonuses
			if( (sce = sc->data[SC_SUFFRAGIUM]) )
				VARCAST_REDUCTION(sce->val2);
			if( (sce = sc->data[SC_MEMORIZE]) ) {
				reduce_ct_r += 50;
				if( --sce->val2 <= 0 )
					status_change_end(bl, SC_MEMORIZE, INVALID_TIMER);
			}
			if( (sce = sc->data[SC_POEMBRAGI]) )
				reduce_ct_r += sce->val2;
			if( (sce = sc->data[SC_SOULFAIRY]) )
				VARCAST_REDUCTION(sce->val3);
			if( sc->data[SC_IZAYOI] )
				VARCAST_REDUCTION(50);
			if( (sce = sc->data[SC_WATER_INSIGNIA]) && sce->val1 == 3 &&
				skill_get_type(skill_id) == BF_MAGIC && skill_get_ele(skill_id, skill_lv) == ELE_WATER )
				VARCAST_REDUCTION(30); //Reduces 30% Variable Cast Time of Water spells
			if( (sce = sc->data[SC_TELEKINESIS_INTENSE]) )
				VARCAST_REDUCTION(sce->val2);
			//Fixed cast reduction bonuses
			if( sc->data[SC_FREEZING] )
				fixcast_r -= 50;
			if( (sce = sc->data[SC_SECRAMENT]) )
				fixcast_r = max(fixcast_r, sce->val2);
			if( sd && skill_id >= WL_WHITEIMPRISON && skill_id <= WL_FREEZE_SP  ) {
				uint16 lv = pc_checkskill(sd, WL_RADIUS);
				int reduce_fc_r = 0;

				if( lv )
					reduce_fc_r = (status_get_int(bl) + status_get_lv(bl)) / 15 + lv * 5;
				fixcast_r = max(fixcast_r, reduce_fc_r);
			}
			if( (sce = sc->data[SC_DANCEWITHWUG]) )
				fixcast_r = max(fixcast_r, sce->val4);
			if( (sce = sc->data[SC_HEAT_BARREL]) )
				fixcast_r = max(fixcast_r, sce->val3);
			if( (sce = sc->data[SC_FENRIR_CARD]) )
				fixcast_r = max(fixcast_r, sce->val2);
			//Fixed cast non percentage bonuses
			if( sc->data[SC_TELEPORT_FIXEDCASTINGDELAY] && skill_id == AL_TELEPORT )
				fixed += 1000;
			if( (sce = sc->data[SC_MANDRAGORA]) )
				fixed += sce->val1 * 500;
			if( sc->data[SC_GUST_OPTION] || sc->data[SC_BLAST_OPTION] || sc->data[SC_WILD_STORM_OPTION] )
				fixed -= 1000;
			if( sc->data[SC_IZAYOI] )
				fixed = 0;
		}
		status_change_end(bl, SC_SUFFRAGIUM, INVALID_TIMER);
	}

	//Now compute overall factors
	if( varcast_r < 0 )
		time = time * (1 - (float)min(varcast_r, 100) / 100);

	if( !(flag&1) ) //Reduction from status point
		time = time * (1 - sqrt(((float)(status_get_dex(bl) * 2 + status_get_int(bl)) / battle_config.vcast_stat_scale)));

	time = time * (1 - (float)min(reduce_ct_r, 100) / 100);
	time = max(time + (1 - (double)min(fixcast_r, 100) / 100) * max(fixed, 0), 0); //Underflow checking/capping

	return (int)time;
}
#endif

/*==========================================
 * Does delay reductions based on dex/agi, sc data, item bonuses, ...
 *------------------------------------------*/
int skill_delayfix(struct block_list *bl, uint16 skill_id, uint16 skill_lv)
{
	int delaynodex = skill_get_delaynodex(skill_id, skill_lv);
	int time = skill_get_delay(skill_id, skill_lv);
	struct map_session_data *sd = NULL;
	struct status_change *sc = status_get_sc(bl);

	nullpo_ret(bl);

	if( skill_id == SA_ABRACADABRA || skill_id == WM_RANDOMIZESPELL )
		return 0; //Will use picked skill's delay

	if( bl->type&battle_config.no_skill_delay )
		return battle_config.min_skill_delay_limit;

	if( time < 0 )
		time = -time + status_get_amotion(bl); //If set to < 0, add to attack motion

	//Delay reductions
	if( sc && sc->data[SC_SPIRIT] ) {
		switch (skill_id) {
			case CR_SHIELDBOOMERANG:
				if( sc->data[SC_SPIRIT]->val2 == SL_CRUSADER )
					time /= 2;
				break;
			case AS_SONICBLOW:
				if( !map_flag_gvg2(bl->m) && !mapdata[bl->m].flag.battleground && sc->data[SC_SPIRIT]->val2 == SL_ASSASIN )
					time /= 2;
				break;
		}
	}

	if( !(delaynodex&1) ) {
		if( battle_config.delay_dependon_dex ) { //If skill delay is allowed to be reduced by dex
			int scale = battle_config.castrate_dex_scale - status_get_dex(bl);

			if( scale > 0 )
				time = time * scale / battle_config.castrate_dex_scale;
			else //To be capped later to minimum
				time = 0;
		}
		if( battle_config.delay_dependon_agi ) { //If skill delay is allowed to be reduced by agi
			int scale = battle_config.castrate_dex_scale - status_get_agi(bl);

			if( scale > 0 )
				time = time * scale / battle_config.castrate_dex_scale;
			else
				time = 0;
		}
	}

	if( !(delaynodex&2) ) {
		if( sc && sc->count ) {
			if( sc->data[SC_POEMBRAGI] )
				time -= time * sc->data[SC_POEMBRAGI]->val3 / 100;
			if( sc->data[SC_WIND_INSIGNIA] && sc->data[SC_WIND_INSIGNIA]->val1 == 3 &&
				skill_get_type(skill_id) == BF_MAGIC && skill_get_ele(skill_id, skill_lv) == ELE_WIND )
				time -= time * 50 / 100; //After Delay of Wind element spells reduced by 50%
			if( sc->data[SC_SOULDIVISION] )
				time += time * sc->data[SC_SOULDIVISION]->val2 / 100;
		}
	}

	if( !(delaynodex&4) && (sd = map_id2sd(bl->id)) ) {
		uint8 i;

		ARR_FIND(0, MAX_PC_BONUS, i, sd->skilldelay[i].id == skill_id);
		if( i < MAX_PC_BONUS )
			time += sd->skilldelay[i].val;
		if( sd->delayrate != 100 )
			time = time * sd->delayrate / 100;
	}

	if( battle_config.delay_rate != 100 )
		time = time * battle_config.delay_rate / 100;

	//ShowInfo("Delay delayfix = %f\n", time);
	return max(time, 0);
}

int skill_cooldownfix(struct block_list *bl, uint16 skill_id, uint16 skill_lv)
{
	struct map_session_data *sd = NULL;
	struct status_change *sc = status_get_sc(bl);
	int time;

	nullpo_ret(bl);

	time = skill_get_cooldown(skill_id, skill_lv);

	if( bl->type&battle_config.no_skill_cooldown )
		return battle_config.min_skill_cooldown_limit;

	if( skill_id == SJ_NOVAEXPLOSING && sc && sc->data[SC_DIMENSION] )  {
		status_change_end(bl,SC_DIMENSION,INVALID_TIMER);
		time = 0; //Dimension removes Nova Explosion's cooldown
	}

	if( (sd = map_id2sd(bl->id)) ) {
		uint8 i;

		if (skill_id == SU_TUNABELLY && pc_checkskill(sd, SU_SPIRITOFSEA) > 0)
			time -= skill_get_time(skill_id, skill_lv);
		ARR_FIND(0, MAX_PC_BONUS, i, sd->skillcooldown[i].id == skill_id);
		if( i < MAX_PC_BONUS )
			time += sd->skillcooldown[i].val;
		if( sd->cooldownrate != 100 )
			time = time * sd->cooldownrate / 100;
	}

	if( battle_config.cooldown_rate != 100 )
		time = time * battle_config.cooldown_rate / 100;

	return max(time, battle_config.min_skill_cooldown_limit);
}


/*==========================================
 * Weapon Repair [Celest/DracoRPG]
 *------------------------------------------*/
void skill_repairweapon(struct map_session_data *sd, int idx) {
	int material;
	int materials[4] = { 1002,998,999,756 };
	struct item *item;
	struct map_session_data *target_sd;

	nullpo_retv(sd);

	if( !(target_sd = map_id2sd(sd->menuskill_val)) ) //Failed
		return;

	if( idx == 0xFFFF ) //No item selected ('Cancel' clicked)
		return;

	if( idx < 0 || idx >= MAX_INVENTORY )
		return; //Invalid index??

	item = &target_sd->inventory.u.items_inventory[idx];
	if( !item->nameid || item->card[0] == CARD0_PET || !item->attribute )
		return; //Again invalid item

	if( sd != target_sd && !battle_check_range(&sd->bl,&target_sd->bl,skill_get_range2(&sd->bl,sd->menuskill_id,sd->menuskill_val2,true)) ) {
		clif_item_repaireffect(sd,idx,1);
		return;
	}

	if( target_sd->inventory_data[idx]->type == IT_WEAPON )
		material = materials [ target_sd->inventory_data[idx]->wlv - 1 ]; //Lv1/2/3/4 weapons consume 1 Iron Ore/Iron/Steel/Rough Oridecon
	else
		material = materials [2]; //Armors consume 1 Steel

	if( pc_search_inventory(sd,material) == INDEX_NOT_FOUND ) {
		clif_skill_fail(sd,sd->menuskill_id,USESKILL_FAIL_LEVEL,0,0);
		return;
	}

	clif_skill_nodamage(&sd->bl,&target_sd->bl,sd->menuskill_id,1,1);

	item->attribute = 0; //Clear broken state

	clif_equiplist(target_sd);

	pc_delitem(sd,pc_search_inventory(sd,material),1,0,0,LOG_TYPE_CONSUME);

	clif_item_repaireffect(sd,idx,0);

	if( sd != target_sd )
		clif_item_repaireffect(target_sd,idx,0);
}

/*==========================================
 * Item Appraisal
 *------------------------------------------*/
void skill_identify (struct map_session_data *sd, int idx)
{
	int flag = 1;

	nullpo_retv(sd);

	if(idx >= 0 && idx < MAX_INVENTORY) {
		if(sd->inventory.u.items_inventory[idx].nameid > 0 && sd->inventory.u.items_inventory[idx].identify == 0) {
			flag = 0;
			sd->inventory.u.items_inventory[idx].identify = 1;
		}
	}
	clif_item_identified(sd,idx,flag);
}

/*==========================================
 * Weapon Refine [Celest]
 *------------------------------------------*/
void skill_weaponrefine(struct map_session_data *sd, int idx)
{
	nullpo_retv(sd);

	if (idx >= 0 && idx < MAX_INVENTORY) {
		struct item *item;
		struct item_data *ditem = sd->inventory_data[idx];

		item = &sd->inventory.u.items_inventory[idx];
		if (item->nameid > 0 && ditem->type == IT_WEAPON) {
			int i = 0, per;
			int material[5] = { 0,ITEMID_PHRACON,ITEMID_EMVERETARCON,ITEMID_ORIDECON,ITEMID_ORIDECON };

			if (ditem->flag.no_refine) { //If the item isn't refinable
				clif_skill_fail(sd,sd->menuskill_id,USESKILL_FAIL_LEVEL,0,0);
				return;
			}
			if (item->refine >= sd->menuskill_val || item->refine >= 10) {
				clif_upgrademessage(sd->fd,2,item->nameid);
				return;
			}
			if ((i = pc_search_inventory(sd,material[ditem->wlv])) == INDEX_NOT_FOUND) {
				clif_upgrademessage(sd->fd,3,material[ditem->wlv]);
				return;
			}
			per = status_get_refine_chance((enum e_refine_type)ditem->wlv,(int)item->refine,false);
			if (sd->class_&JOBL_THIRD)
				per += 10;
			else
				per += (((signed int)sd->status.job_level) - 50) / 2; //Updated per the new kro descriptions [Skotlex]
			pc_delitem(sd,i,1,0,0,LOG_TYPE_OTHER);
			if (rnd()%100 < per) {
				int ep = 0;

				log_pick_pc(sd,LOG_TYPE_OTHER,-1,item);
				item->refine++;
				log_pick_pc(sd,LOG_TYPE_OTHER,1,item);
				if (item->equip) {
					ep = item->equip;
					pc_unequipitem(sd,idx,1|2);
				}
				clif_delitem(sd,idx,1,3);
				clif_upgrademessage(sd->fd,0,item->nameid);
				clif_inventorylist(sd);
				clif_refine(sd->fd,0,idx,item->refine);
				if (ep)
					pc_equipitem(sd,idx,ep,false);
				clif_misceffect(&sd->bl,3);
				achievement_update_objective(sd,AG_REFINE_SUCCESS,2,ditem->wlv,item->refine);
				if (item->refine == 10 &&
					item->card[0] == CARD0_FORGE &&
					(int)MakeDWord(item->card[2],item->card[3]) == sd->status.char_id)
				{ //Fame point system [DracoRPG]
					switch (ditem->wlv) {
						case 1:
							pc_addfame(sd,battle_config.fame_refine_lv1); //Success to refine to +10 a lv1 weapon you forged = +1 fame point
							break;
						case 2:
							pc_addfame(sd,battle_config.fame_refine_lv2); //Success to refine to +10 a lv2 weapon you forged = +25 fame point
							break;
						case 3:
							pc_addfame(sd,battle_config.fame_refine_lv3); //Success to refine to +10 a lv3 weapon you forged = +1000 fame point
							break;
					}
				}
			} else {
				item->refine = 0;
				if (item->equip)
					pc_unequipitem(sd,idx,1|2);
				clif_upgrademessage(sd->fd,1,item->nameid);
				clif_refine(sd->fd,1,idx,item->refine);
				pc_delitem(sd,idx,1,0,2,LOG_TYPE_OTHER);
				clif_misceffect(&sd->bl,2);
				achievement_update_objective(sd,AG_REFINE_FAIL,1,1);
				clif_emotion(&sd->bl,E_OMG);
			}
		}
	}
}

/*==========================================
 *
 *------------------------------------------*/
int skill_autospell(struct map_session_data *sd, uint16 skill_id)
{
	uint16 skill_lv;
	int maxlv = 1, lv;

	nullpo_ret(sd);

	skill_lv = sd->menuskill_val;
	lv = pc_checkskill(sd,skill_id);

	if(!skill_lv || !lv)
		return 0; //Player must learn the skill before doing auto-spell [Lance]

	if(skill_id == MG_NAPALMBEAT)
		maxlv = 3;
	else if(skill_id == MG_COLDBOLT || skill_id == MG_FIREBOLT || skill_id == MG_LIGHTNINGBOLT) {
		if(sd->sc.data[SC_SPIRIT] && sd->sc.data[SC_SPIRIT]->val2 == SL_SAGE)
			maxlv = 10; //Soul Linker bonus. [Skotlex]
		else if(skill_lv == 2)
			maxlv = 1;
		else if(skill_lv == 3)
			maxlv = 2;
		else if(skill_lv >= 4)
			maxlv = 3;
	} else if(skill_id == MG_SOULSTRIKE) {
		if(skill_lv == 5)
			maxlv = 1;
		else if(skill_lv == 6)
			maxlv = 2;
		else if(skill_lv >= 7)
			maxlv = 3;
	} else if(skill_id == MG_FIREBALL) {
		if(skill_lv == 8)
			maxlv = 1;
		else if(skill_lv >= 9)
			maxlv = 2;
	} else if(skill_id == MG_FROSTDIVER)
		maxlv = 1;
	else
		return 0;

	if(maxlv > lv)
		maxlv = lv;

	sc_start4(&sd->bl,&sd->bl,SC_AUTOSPELL,100,skill_lv,skill_id,maxlv,0,skill_get_time(SA_AUTOSPELL,skill_lv));
	return 0;
}

/**
 * Count the number of players with Gangster Paradise, Peaceful Break, or Happy Break.
 * @param bl: Player object
 * @param ap: va_arg list
 * @return 1 if the player has learned Gangster Paradise, Peaceful Break, or Happy Break otherwise 0
 */
static int skill_sit_count(struct block_list *bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data *)bl;
	int flag = va_arg(ap,int);

	if(!pc_issit(sd))
		return 0;

	if(flag&1 && pc_checkskill(sd,RG_GANGSTER) > 0)
		return 1;

	if(flag&2 && (pc_checkskill(sd,TK_HPTIME) > 0 || pc_checkskill(sd,TK_SPTIME) > 0))
		return 1;

	return 0;
}

/**
 * Triggered when a player sits down to activate bonus states.
 * @param bl: Player object
 * @param ap: va_arg list
 * @return 0
 */
static int skill_sit_in(struct block_list *bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data *)bl;
	int flag = va_arg(ap,int);

	if(!pc_issit(sd))
		return 0;

	if(flag&1 && pc_checkskill(sd,RG_GANGSTER) > 0)
		sd->state.gangsterparadise = 1;

	if(flag&2 && (pc_checkskill(sd,TK_HPTIME) > 0 || pc_checkskill(sd,TK_SPTIME) > 0)) {
		sd->state.rest = 1;
		status_calc_regen(bl,&sd->battle_status,&sd->regen);
		status_calc_regen_rate(bl,&sd->regen,&sd->sc);
	}

	return 0;
}

/**
 * Triggered when a player stands up to deactivate bonus states.
 * @param bl: Player object
 * @param ap: va_arg list
 * @return 0
 */
static int skill_sit_out(struct block_list *bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data *)bl;
	int flag = va_arg(ap,int);
	int range = va_arg(ap,int);

	if(map_foreachinallrange(skill_sit_count,&sd->bl,range,BL_PC,flag) > 1)
		return 0;

	if(flag&1 && sd->state.gangsterparadise)
		sd->state.gangsterparadise = 0;

	if(flag&2 && sd->state.rest) {
		sd->state.rest = 0;
		status_calc_regen(bl,&sd->battle_status,&sd->regen);
		status_calc_regen_rate(bl,&sd->regen,&sd->sc);
	}
	return 0;
}

/**
 * Toggle Sit icon and player bonuses when sitting/standing.
 * @param sd: Player data
 * @param sitting: True when sitting or false when standing
 * @return 0
 */
int skill_sit(struct map_session_data *sd, bool sitting)
{
	int flag = 0, range = 0;
	uint16 lv;
     
	nullpo_ret(sd);

	if((lv = pc_checkskill(sd,RG_GANGSTER)) > 0) {
		flag |= 1;
		range = skill_get_splash(RG_GANGSTER,lv);
	}

	if((lv = pc_checkskill(sd,TK_HPTIME)) > 0) {
		flag |= 2;
		range = skill_get_splash(TK_HPTIME,lv);
	} else if((lv = pc_checkskill(sd,TK_SPTIME)) > 0) {
		flag |= 2;
		range = skill_get_splash(TK_SPTIME,lv);
	}

	if(sitting)
		clif_status_load(&sd->bl,SI_SIT,1);
	else
		clif_status_load(&sd->bl,SI_SIT,0);

	if(!flag) //No need to count area if no skills are learned
		return 0;

	if(sitting) {
		if (map_foreachinallrange(skill_sit_count,&sd->bl,range,BL_PC,flag) > 1)
			map_foreachinallrange(skill_sit_in,&sd->bl,range,BL_PC,flag);
	} else
		map_foreachinallrange(skill_sit_out,&sd->bl,range,BL_PC,flag,range);

	return 0;
}

/*==========================================
 * Do Forstjoke/Scream effect
 *------------------------------------------*/
int skill_frostjoke_scream(struct block_list *bl, va_list ap)
{
	struct block_list *src;
	uint16 skill_id, skill_lv;
	unsigned int tick;

	nullpo_ret(bl);
	nullpo_ret(src = va_arg(ap,struct block_list *));

	skill_id = va_arg(ap,int);
	skill_lv = va_arg(ap,int);
	if(!skill_lv)
		return 0;
	tick = va_arg(ap,unsigned int);

	if(bl->id == src->id || status_isdead(bl))
		return 0;
	if(bl->type == BL_PC) {
		struct map_session_data *sd = (struct map_session_data *)bl;

		if(sd && (sd->sc.option&(OPTION_INVISIBLE|OPTION_MADOGEAR)))
			return 0; //Frost Joke/Scream cannot target invisible or MADO Gear characters [Ind]
	}
	//It has been reported that Scream/Joke works the same regardless of woe-setting [Skotlex]
	if(battle_check_target(src,bl,BCT_ENEMY) > 0)
		skill_additional_effect(src,bl,skill_id,skill_lv,BF_MISC,ATK_DEF,tick);
	else if(battle_check_target(src,bl,BCT_PARTY) > 0 && rnd()%100 < 10)
		skill_additional_effect(src,bl,skill_id,skill_lv,BF_MISC,ATK_DEF,tick);

	return 0;
}

/**
 * Set map cell flag as skill unit effect
 * @param src Skill unit
 * @param skill_id
 * @param skill_lv
 * @param cell Cell type cell_t
 * @param flag 0/1
 */
static void skill_unitsetmapcell(struct skill_unit *src, uint16 skill_id, uint16 skill_lv, cell_t cell, bool flag)
{
	int range = skill_get_unit_range(skill_id,skill_lv);
	int x, y;

	for( y = src->bl.y - range; y <= src->bl.y + range; ++y )
		for( x = src->bl.x - range; x <= src->bl.x + range; ++x )
			map_setcell(src->bl.m, x, y, cell, flag);
}

/**
 * Do skill attack area (such splash effect) around the 'first' target.
 * First target will skip skill condition, always receive damage. But,
 * around it, still need target/condition validation by
 * battle_check_target and status_check_skilluse
 * @param bl
 * @param ap { atk_type, src, dsrc, skill_id, skill_lv, tick, flag, type }
 */
int skill_attack_area(struct block_list *bl, va_list ap)
{
	struct block_list *src, *dsrc;
	int atk_type, skill_id, skill_lv, flag, type;
	unsigned int tick;

	if (status_isdead(bl))
		return 0;

	atk_type = va_arg(ap,int);
	src = va_arg(ap,struct block_list *);
	dsrc = va_arg(ap,struct block_list *);
	skill_id = va_arg(ap,int);
	skill_lv = va_arg(ap,int);
	tick = va_arg(ap,unsigned int);
	flag = va_arg(ap,int);
	type = va_arg(ap,int);

	if (skill_area_temp[1] == bl->id) //This is the target of the skill, do a full attack and skip target checks
		return skill_attack(atk_type,src,dsrc,bl,skill_id,skill_lv,tick,flag);

	if (battle_check_target(dsrc,bl,type) <= 0 || !status_check_skilluse(NULL,bl,skill_id,2))
		return 0;

	switch (skill_id) { //Skills that don't require the animation to be removed
		case WZ_FROSTNOVA:
			if (src->x == bl->x && src->y == bl->y)
				return 0; //Does not hit current cell
		//Fall through
		case NPC_ACIDBREATH:
		case NPC_DARKNESSBREATH:
		case NPC_FIREBREATH:
		case NPC_ICEBREATH:
		case NPC_THUNDERBREATH:
			return skill_attack(atk_type,src,dsrc,bl,skill_id,skill_lv,tick,flag);
		default: //Area-splash, disable skill animation
			return skill_attack(atk_type,src,dsrc,bl,skill_id,skill_lv,tick,flag|SD_ANIMATION);
	}
}

/**
 * Clear skill unit group
 * @param bl
 * @param flag &1
 */
int skill_clear_group(struct block_list *bl, int flag)
{
	struct unit_data *ud = NULL;
	struct skill_unit_group *group[MAX_SKILLUNITGROUP];
	int i, count = 0;

	nullpo_ret(bl);

	if (!(ud = unit_bl2ud(bl)))
		return 0;

	//All groups to be deleted are first stored on an array since the array elements shift around when you delete them [Skotlex]
	for (i = 0; i < MAX_SKILLUNITGROUP && ud->skillunit[i]; i++) {
		switch (ud->skillunit[i]->skill_id) {
			case SA_DELUGE:
			case SA_VOLCANO:
			case SA_VIOLENTGALE:
			case SA_LANDPROTECTOR:
			case NJ_SUITON:
			case NJ_KAENSIN:
				if (flag&1)
					group[count++] = ud->skillunit[i];
				break;
			case SC_CHAOSPANIC:
				if (flag&4)
					group[count++] = ud->skillunit[i];
				break;
			case SC_MAELSTROM:
				if (flag&8)
					group[count++] = ud->skillunit[i];
				break;
			case SC_BLOODYLUST:
				if (flag&16)
					group[count++] = ud->skillunit[i];
				break;
			case SO_CLOUD_KILL:
				if (flag&32)
					group[count++] = ud->skillunit[i];
				break;
			case SO_WARMER:
				if (flag&64)
					group[count++] = ud->skillunit[i];
				break;
			default:
				if (flag&2 && skill_get_inf2(ud->skillunit[i]->skill_id)&INF2_TRAP)
					group[count++] = ud->skillunit[i];
				break;
		}

	}
	for (i = 0; i < count; i++)
		skill_delunitgroup(group[i]);
	return count;
}

/**
 * Returns the first element field found [Skotlex]
 * @param bl
 * @return skill_unit_group
 */
struct skill_unit_group *skill_locate_element_field(struct block_list *bl)
{
	struct unit_data *ud = NULL;
	int i;

	nullpo_ret(bl);

	if (!(ud = unit_bl2ud(bl)))
		return NULL;

	for (i = 0; i < MAX_SKILLUNITGROUP && ud->skillunit[i]; i++) {
		switch (ud->skillunit[i]->skill_id) {
			case SA_DELUGE:
			case SA_VOLCANO:
			case SA_VIOLENTGALE:
			case SA_LANDPROTECTOR:
			case GN_DEMONIC_FIRE:
				return ud->skillunit[i];
		}
	}
	return NULL;
}

//Graffiti cleaner [Valaris]
int skill_graffitiremover(struct block_list *bl, va_list ap)
{
	struct skill_unit *unit = NULL;

	nullpo_ret(bl);

	if (bl->type != BL_SKILL)
		return 0;

	unit = (struct skill_unit *)bl;

	if (unit && unit->group && unit->group->unit_id == UNT_GRAFFITI)
		skill_delunit(unit);

	return 0;
}

//Greed effect
int skill_greed(struct block_list *bl, va_list ap)
{
	struct block_list *src;

	nullpo_ret(bl);
	nullpo_ret(src = va_arg(ap,struct block_list *));

	if (src->type == BL_PC && bl->type == BL_ITEM)
		pc_takeitem(((TBL_PC *)src), ((TBL_ITEM *)bl));

	return 0;
}

/**
 *Ranger's Detonator [Jobbie]
 */
int skill_detonator(struct block_list *bl, va_list ap)
{
	struct skill_unit *unit = NULL;
	struct skill_unit_group *group = NULL;
	struct block_list *src = NULL;
	int unit_id, tick;
	uint16 skill_id;
	uint16 skill_lv;

	nullpo_ret(bl);
	nullpo_ret(src = va_arg(ap,struct block_list *));

	if (bl->type != BL_SKILL)
		return 0;

	unit = (struct skill_unit *)bl;

	if (!unit || !(group = unit->group) || group->src_id != src->id)
		return 0;

	unit_id = group->unit_id;
	tick = gettick();
	skill_id = group->skill_id;
	skill_lv = group->skill_lv;
	switch (unit_id) { //List of Hunter and Ranger Traps that can be detonate
		case UNT_BLASTMINE:
		case UNT_SANDMAN:
		case UNT_CLAYMORETRAP:
		case UNT_TALKIEBOX:
		case UNT_CLUSTERBOMB:
		case UNT_FIRINGTRAP:
		case UNT_ICEBOUNDTRAP:
			switch (unit_id) {
				case UNT_TALKIEBOX:
					clif_talkiebox(bl,group->valstr);
					group->val2 = -1;
					break;
				case UNT_CLAYMORETRAP:
				case UNT_FIRINGTRAP:
				case UNT_ICEBOUNDTRAP:
					if (skill_get_nk(skill_id)&NK_SPLASHSPLIT)
						skill_area_temp[0] = map_foreachinrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,skill_id,skill_lv,tick,BCT_ENEMY,skill_area_sub_count);
					map_foreachinrange(skill_trap_splash,bl,skill_get_splash(skill_id,skill_lv),group->bl_flag|BL_SKILL|~BCT_SELF,bl,tick);
					break;
				default:
					if (skill_get_nk(skill_id)&NK_SPLASHSPLIT)
						skill_area_temp[0] = map_foreachinrange(skill_area_sub,bl,skill_get_splash(skill_id,skill_lv),BL_CHAR,src,skill_id,skill_lv,tick,BCT_ENEMY,skill_area_sub_count);
					map_foreachinrange(skill_trap_splash,bl,skill_get_splash(skill_id,skill_lv),group->bl_flag,bl,tick);
					break;
			}
			group->unit_id = UNT_USED_TRAPS;
			clif_changetraplook(bl, UNT_USED_TRAPS);
			if (unit_id == UNT_TALKIEBOX)
				group->limit = DIFF_TICK(tick,group->tick) + 5000;
			else if (unit_id == UNT_CLUSTERBOMB || unit_id == UNT_ICEBOUNDTRAP)
				group->limit = DIFF_TICK(tick,group->tick) + 1000;
			else if (unit_id == UNT_FIRINGTRAP)
				group->limit = DIFF_TICK(tick,group->tick);
			else
				group->limit = DIFF_TICK(tick,group->tick) + 1500;
			break;
	}
	return 0;
}

int skill_banding_count(struct map_session_data *sd)
{
	int range = skill_get_splash(LG_BANDING,1);
	uint8 count = party_foreachsamemap(party_sub_count_banding,sd,range,0);
	unsigned int group_hp = party_foreachsamemap(party_sub_count_banding,sd,range,1);

	nullpo_ret(sd);

	//HP is set to the average HP of the banding group
	if (count > 1)
		status_set_hp(&sd->bl,group_hp / count,0);
	//Royal Guard count check for banding
	if (sd && sd->status.party_id) {
		if (count > MAX_PARTY)
			return MAX_PARTY;
		else if (count > 1)
			return count; //Effect bonus from additional Royal Guards if not above the max possiable
	}
	return 0;
}

/**
 * Calculates Minstrel/Wanderer bonus for Chorus skills.
 * @param sd Player who has Chorus skill active
 * @param flag
 * @return Bonus value based on party count
 */
int skill_chorus_count(struct map_session_data *sd, uint8 flag) {
	uint8 count = party_foreachsamemap(party_sub_count_chorus,sd,AREA_SIZE + 1);;

	if (!sd || !sd->status.party_id)
		return 0;
	//Bonus remains 0 unless 3 or more Minstrels/Wanderers are in the party
	if (flag != 1 && count < 3)
		return 0;
	//Maximum effect possible from 7 or more Minstrels/Wanderers
	if (flag != 3 && count > 7)
		return (flag ? 7 : 5);
	//Effect bonus from additional Minstrels/Wanderers if not above the max possible
	return ((flag == 1 || flag == 2) ? count : (count - 2));
}

void skill_generate_millenniumshield(struct map_session_data *sd, uint16 skill_id, uint16 skill_lv) {
	uint8 generate = rnd()%100; //Generates a random number, which is then used to determine how many shields will generate
	uint8 shieldnumber = 1; //Number of shield

	nullpo_retv(sd);

	if (generate < 20) //20% chance for 4 shields
		shieldnumber = 4;
	else if (generate < 50) //30% chance for 3 shields
		shieldnumber = 3;
	else if (generate < 100) //50% chance for 2 shields
		shieldnumber = 2;
	if (sd) {
		uint8 i;

		pc_delshieldball(sd,sd->shieldball,1); //Remove old shields if any exist
		for (i = 0; i < shieldnumber; i++)
			pc_addshieldball(sd,skill_get_time(skill_id,skill_lv),shieldnumber,battle_config.millennium_shield_health);
	}
}

/**
 * Rebellion's Bind Trap explosion
 */
static int skill_flicker_bind_trap(struct block_list *bl, va_list ap) {
	struct skill_unit *unit = NULL;
	struct block_list *src = NULL;
	unsigned int tick;

	nullpo_ret(bl);
	nullpo_ret(src = va_arg(ap,struct block_list *));

	tick = va_arg(ap,unsigned int);

	if (bl->type != BL_SKILL || !(unit = (struct skill_unit *)bl) || !unit->group)
		return 0;

	if (unit->group->unit_id != UNT_B_TRAP || unit->group->src_id != src->id)
		return 0;

	clif_skill_damage(src,src,tick,0,status_get_dmotion(src),-30000,1,unit->group->skill_id,unit->group->skill_lv,DMG_SPLASH);
	clif_skill_nodamage(bl,bl,RL_B_FLICKER_ATK,1,1); //Explosion animation
	map_foreachinallrange(skill_trap_splash,bl,unit->range,unit->group->bl_flag,bl,tick);
	clif_changetraplook(bl,UNT_USED_TRAPS);
	unit->group->unit_id = UNT_USED_TRAPS;
	unit->group->limit = DIFF_TICK(tick,unit->group->tick);
	return 0;
}

/*==========================================
 * Check new skill unit cell when overlapping in other skill unit cell.
 * Catched skill in cell value pushed to *unit pointer.
 * Set (*alive) to 0 will ends 'new unit' check
 *------------------------------------------*/
static int skill_cell_overlap(struct block_list *bl, va_list ap)
{
	uint16 skill_id;
	int *alive;
	struct skill_unit *unit;
	struct skill_unit_group *group;

	skill_id = va_arg(ap,int);
	alive = va_arg(ap,int *);
	unit = (struct skill_unit *)bl;

	if (!unit || !(group = unit->group) || !(*alive) ||
		group->state.guildaura) //Guild auras are not cancelled!
		return 0;

	switch (skill_id) {
		case SA_LANDPROTECTOR:
			if (group->skill_id == SA_LANDPROTECTOR) { //Check for offensive Land Protector to delete both [Skotlex]
				(*alive) = 0;
				skill_delunit(unit);
				return 1;
			}
			if ((!(skill_get_inf2(group->skill_id)&(INF2_TRAP)) && !(skill_get_inf3(group->skill_id)&(INF3_NOLP))) ||
				group->skill_id == WZ_FIREPILLAR || group->skill_id == GN_HELLS_PLANT) {
				if (skill_get_unit_flag(group->skill_id)&UF_RANGEDSINGLEUNIT) {
					if (unit->val4&UF_RANGEDSINGLEUNIT)
						skill_delunitgroup(group);
				} else
					skill_delunit(unit);
				return 1;
			}
			break;
		case HW_GANBANTEIN:
		case LG_EARTHDRIVE:
		case GN_CRAZYWEED_ATK:
			//Officially songs/dances are removed
			if (skill_get_unit_flag(group->skill_id)&UF_RANGEDSINGLEUNIT) {
				if (unit->val4&UF_RANGEDSINGLEUNIT)
					skill_delunitgroup(group);
			} else
				skill_delunit(unit);
			return 1;
		case SA_VOLCANO:
		case SA_DELUGE:
		case SA_VIOLENTGALE:
			//The official implementation makes them fail to appear when casted on top of ANYTHING
			//But I wonder if they didn't actually meant to fail when casted on top of each other?
			//Hence, I leave the alternate implementation here, commented [Skotlex]
			if (unit->range <= 0 && skill_get_unit_id(group->skill_id,0) != UNT_DUMMYSKILL) {
				(*alive) = 0;
				return 1;
			}
			/*
			switch (group->skill_id) { //These cannot override each other.
				case SA_VOLCANO:
				case SA_DELUGE:
				case SA_VIOLENTGALE:
					(*alive) = 0;
					return 1;
			}
			*/
			break;
		case PF_FOGWALL:
			switch (group->skill_id) {
				case SA_VOLCANO: //Can't be placed on top of these
				case SA_VIOLENTGALE:
					(*alive) = 0;
					return 1;
				case SA_DELUGE:
				case NJ_SUITON:
					(*alive) = 2; //Cheap 'hack' to notify the calling function that duration should be doubled [Skotlex]
					break;
			}
			break;
		case WZ_WATERBALL:
			switch (group->skill_id) {
				case SA_DELUGE:
				case NJ_SUITON:
					skill_delunit(unit); //Consumes deluge/suiton
					return 1;
			}
			break;
		case WZ_ICEWALL:
		case HP_BASILICA:
		case HW_GRAVITATION:
			//These can't be placed on top of themselves (duration can't be refreshed)
			if (group->skill_id == skill_id) {
				(*alive) = 0;
				return 1;
			}
			break;
		case RL_FIRE_RAIN:
			switch (group->unit_id) {
				case UNT_DUMMYSKILL:
					if (group->skill_id != GN_CRAZYWEED_ATK)
						break;
				//Fall through
				case UNT_LANDPROTECTOR:		case UNT_ICEWALL:		case UNT_FIREWALL:
				case UNT_WARMER:		case UNT_CLOUD_KILL:		case UNT_VACUUM_EXTREME:
				case UNT_SPIDERWEB:		case UNT_FOGWALL:		case UNT_DELUGE:
				case UNT_VIOLENTGALE:		case UNT_VOLCANO:		case UNT_QUAGMIRE:
				case UNT_GRAVITATION:		case UNT_MAGNUS:		case UNT_THORNS_TRAP:
				case UNT_WALLOFTHORN:		case UNT_DEMONIC_FIRE:		case UNT_HELLS_PLANT:
				case UNT_POISONSMOKE:		case UNT_VENOMDUST:		case UNT_MAELSTROM:
				case UNT_MANHOLE:		case UNT_DIMENSIONDOOR:		case UNT_LANDMINE:
				case UNT_BLASTMINE:		case UNT_SANDMAN:		case UNT_SHOCKWAVE:
				case UNT_SKIDTRAP:		case UNT_ANKLESNARE:		case UNT_CLAYMORETRAP:
				case UNT_TALKIEBOX:		case UNT_FREEZINGTRAP:		case UNT_VERDURETRAP:
				case UNT_ICEBOUNDTRAP:		case UNT_FIRINGTRAP:		case UNT_ELECTRICSHOCKER:
				case UNT_SUITON:		case UNT_KAEN:			case UNT_CLUSTERBOMB:
				case UNT_CHAOSPANIC:		case UNT_FEINTBOMB:		case UNT_BLOODYLUST:
				case UNT_SAFETYWALL:		case UNT_FIREPILLAR_ACTIVE:	case UNT_PNEUMA:
				case UNT_DEMONSTRATION:		case UNT_VOLCANIC_ASH:		case UNT_LAVA_SLIDE:
				case UNT_POISON_MIST:		case UNT_MAKIBISHI:		case UNT_GROUNDDRIFT_DARK:
				case UNT_GROUNDDRIFT_FIRE:	case UNT_GROUNDDRIFT_POISON:	case UNT_GROUNDDRIFT_WATER:
				case UNT_GROUNDDRIFT_WIND:	case UNT_B_TRAP:		case UNT_FLAMECROSS:
				case UNT_ICEMINE:		case UNT_GROUNDDRIFT_NEUTRAL:
					if (skill_get_unit_flag(group->skill_id)&UF_RANGEDSINGLEUNIT) {
						if (unit->val4&UF_RANGEDSINGLEUNIT)
							skill_delunitgroup(group);
					} else
						skill_delunit(unit);
					return 1;
			}
			break;
	}

	if ((group->skill_id == SA_LANDPROTECTOR && ((!(skill_get_inf2(skill_id)&(INF2_TRAP)) &&
		!(skill_get_inf3(skill_id)&(INF3_NOLP))) || skill_id == WZ_FIREPILLAR || skill_id == GN_HELLS_PLANT)) ||
		group->skill_id == SC_MAELSTROM) {
		(*alive) = 0;
		return 1;
	}

	return 0;
}

/*==========================================
 * Splash effect for skill unit 'trap type'.
 * Chance triggered when damaged, timeout, or char step on it.
 *------------------------------------------*/
static int skill_trap_splash(struct block_list *bl, va_list ap)
{
	struct block_list *src = va_arg(ap,struct block_list *);
	struct skill_unit *unit = NULL;
	int tick = va_arg(ap,int);
	struct skill_unit_group *group;
	struct block_list *ss;
	uint16 skill_id, skill_lv;

	nullpo_ret(src);

	unit = (struct skill_unit *)src;

	if( !unit || !unit->alive || !(group = unit->group) || !bl->prev )
		return 0;

	nullpo_ret(ss = map_id2bl(group->src_id));

	skill_id = group->skill_id;
	skill_lv = group->skill_lv;

	if( battle_check_target(src,bl,group->target_flag) <= 0 )
		return 0;

	switch( group->unit_id ) {
		case UNT_SHOCKWAVE:
		case UNT_SANDMAN:
		case UNT_FLASHER:
			skill_additional_effect(ss,bl,skill_id,skill_lv,BF_MISC,ATK_DEF,tick);
			break;
		case UNT_GROUNDDRIFT_WIND:
			if( skill_attack(skill_get_type(skill_id),ss,src,bl,skill_id,skill_lv,tick,group->val1) )
				sc_start(ss,bl,SC_STUN,50,skill_lv,skill_get_time2(skill_id,1));
			break;
		case UNT_GROUNDDRIFT_DARK:
			if( skill_attack(skill_get_type(skill_id),ss,src,bl,skill_id,skill_lv,tick,group->val1) )
				sc_start(ss,bl,SC_BLIND,50,skill_lv,skill_get_time2(skill_id,2));
			break;
		case UNT_GROUNDDRIFT_POISON:
			if( skill_attack(skill_get_type(skill_id),ss,src,bl,skill_id,skill_lv,tick,group->val1) )
				sc_start(ss,bl,SC_POISON,50,skill_lv,skill_get_time2(skill_id,3));
			break;
		case UNT_GROUNDDRIFT_WATER:
			if( skill_attack(skill_get_type(skill_id),ss,src,bl,skill_id,skill_lv,tick,group->val1) )
				sc_start(ss,bl,SC_FREEZE,50,skill_lv,skill_get_time2(skill_id,4));
			break;
		case UNT_GROUNDDRIFT_FIRE:
			if( skill_attack(skill_get_type(skill_id),ss,src,bl,skill_id,skill_lv,tick,group->val1) )
				skill_blown(src,bl,skill_get_blewcount(skill_id,skill_lv),-1,0);
			break;
		case UNT_GROUNDDRIFT_NEUTRAL:
			skill_attack(skill_get_type(skill_id),ss,src,bl,skill_id,skill_lv,tick,group->val1);
			break;
		case UNT_ELECTRICSHOCKER:
			if( bl->id == ss->id )
				break;
			status_change_start(ss,bl,SC_ELECTRICSHOCKER,10000,skill_lv,group->group_id,0,0,skill_get_time2(skill_id,skill_lv),SCFLAG_FIXEDRATE);
			clif_skill_damage(src,bl,tick,0,0,-30000,1,skill_id,skill_lv,DMG_SPLASH);
			break;
		case UNT_MAGENTATRAP:
		case UNT_COBALTTRAP:
		case UNT_MAIZETRAP:
		case UNT_VERDURETRAP:
			if( bl->type == BL_MOB && status_get_class_(bl) != CLASS_BOSS ) {
				struct status_data *status = status_get_status_data(bl);

				status->def_ele = skill_get_ele(skill_id,skill_lv);
				status->ele_lv = (unsigned char)skill_lv;
			}
			break;
		case UNT_REVERBERATION: //For proper skill delay animation when use with Dominion Impulse
			if( ss->type == BL_PC ) {
				skill_addtimerskill(ss,tick + status_get_amotion(ss),bl->id,0,0,WM_REVERBERATION_MELEE,skill_lv,BF_WEAPON,0);
				skill_addtimerskill(ss,tick + status_get_amotion(ss) * 2,bl->id,0,0,WM_REVERBERATION_MAGIC,skill_lv,BF_MAGIC,0);
			} else
				skill_addtimerskill(ss,tick + 50,bl->id,0,0,NPC_REVERBERATION_ATK,skill_lv,BF_WEAPON,0);
			break;
		case UNT_FIRINGTRAP:
		case UNT_ICEBOUNDTRAP:
			if( bl->id == src->id )
				break;
			if( bl->type == BL_SKILL ) {
				struct skill_unit *unit2 = (struct skill_unit *)bl;
				struct skill_unit_group *group2 = NULL;

				if( !unit2 || !(group2 = unit2->group) )
					return 0;
				if( group2->unit_id == UNT_USED_TRAPS )
					break;
			}
		//Fall through
		case UNT_CLUSTERBOMB:
			if( bl->id != ss->id )
				skill_attack(BF_MISC,ss,src,bl,skill_id,skill_lv,tick,group->val1|SD_LEVEL);
			break;
		case UNT_CLAYMORETRAP:
			if( bl->id == src->id )
				break;
			if( bl->type == BL_SKILL ) {
				struct skill_unit *unit2 = (struct skill_unit *)bl;
				struct skill_unit_group *group2 = NULL;
				int unit_id;

				if( !unit2 || !(group2 = unit2->group) )
					return 0;
				unit_id = group2->unit_id;
				switch( unit_id ) {
					case UNT_CLAYMORETRAP:
					case UNT_LANDMINE:
					case UNT_BLASTMINE:
					case UNT_SHOCKWAVE:
					case UNT_SANDMAN:
					case UNT_FLASHER:
					case UNT_FREEZINGTRAP:
					case UNT_FIRINGTRAP:
					case UNT_ICEBOUNDTRAP:
						group2->unit_id = UNT_USED_TRAPS;
						clif_changetraplook(bl, UNT_USED_TRAPS);
						if( unit_id == UNT_FIRINGTRAP )
							group2->limit = DIFF_TICK(gettick(),group2->tick);
						else if( unit_id == UNT_ICEBOUNDTRAP )
							group2->limit = DIFF_TICK(gettick(),group2->tick) + 1000;
						else
							group2->limit = DIFF_TICK(gettick(),group2->tick) + 1500;
						break;
				}
			}
		//Fall through
		default: {
				int sflag = ((skill_get_nk(skill_id)&NK_SPLASHSPLIT) ? (skill_area_temp[0]&0xFFF) : 0);

				skill_attack(skill_get_type(skill_id),ss,src,bl,skill_id,skill_lv,tick,sflag);
			}
			break;
	}
	return 1;
}

int skill_maelstrom_suction(struct block_list *bl, va_list ap)
{
	uint16 skill_id, skill_lv;
	struct skill_unit *unit = NULL;
	struct skill_unit_group *group = NULL;

	nullpo_ret(bl);

	skill_id = va_arg(ap,int);
	skill_lv = va_arg(ap,int);
	unit = (struct skill_unit *)bl;

	if( !unit || !(group = unit->group) || (skill_get_inf2(skill_id)&INF2_TRAP) )
		return 0;

	if( group->skill_id == SC_MAELSTROM ) {
		struct block_list *src = map_id2bl(group->src_id);

		if( src ) {
			int sp = group->skill_lv * skill_lv + status_get_job_lv(src) / 5;

			status_heal(src, 0, sp / 2, 1);
		}
	}
	return 0;
}

/**
 * Remove current enchanted element for new element
 * @param bl Char
 * @param type New element
 */
void skill_enchant_elemental_end(struct block_list *bl, int type)
{
	struct status_change *sc = NULL;
	const enum sc_type scs[] = { SC_ENCPOISON,SC_ASPERSIO,SC_FIREWEAPON,SC_WATERWEAPON,SC_WINDWEAPON,SC_EARTHWEAPON,SC_SHADOWWEAPON,SC_GHOSTWEAPON,SC_ENCHANTARMS };
	int i;

	nullpo_retv(bl);

	if( !(sc = status_get_sc(bl)) || !sc->count )
		return;

	for( i = 0; i < ARRAYLENGTH(scs); i++ )
		if( (type == SC_ENCHANTARMS || type != scs[i]) && sc->data[scs[i]] )
			status_change_end(bl, scs[i], INVALID_TIMER);
}

/**
 * Check cloaking condition
 * @param bl
 * @param sce
 * @return True if near wall; False otherwise
 */
bool skill_check_cloaking(struct block_list *bl, struct status_change_entry *sce)
{
	bool wall = true;

	if( (bl->type == BL_PC && battle_config.pc_cloak_check_type&1) ||
		(bl->type != BL_PC && battle_config.monster_cloak_check_type&1) )
	{ //Check for walls
		static const int dx[] = { 0, 1, 0, -1, -1,  1, 1, -1};
		static const int dy[] = {-1, 0, 1,  0, -1, -1, 1,  1};
		int i;

		ARR_FIND(0, 8, i, map_getcell(bl->m, bl->x + dx[i], bl->y + dy[i], CELL_CHKNOPASS) != 0);
		if( i == 8 )
			wall = false;
	}

	if( sce ) {
		if( !wall ) {
			if( sce->val1 < 3 ) //End cloaking
				status_change_end(bl, SC_CLOAKING, INVALID_TIMER);
			else if( sce->val4&1 ) { //Remove wall bonus
				sce->val4 &= ~1;
				status_calc_bl(bl, SCB_SPEED);
			}
		} else {
			if( !(sce->val4&1) ) { //Add wall speed bonus
				sce->val4 |= 1;
				status_calc_bl(bl, SCB_SPEED);
			}
		}
	}

	return wall;
}

/**
 * Verifies if an user can use SC_CLOAKING
 */
bool skill_can_cloak(struct map_session_data *sd)
{
	nullpo_retr(false, sd);

	//Avoid cloaking with no wall and low skill level [Skotlex]
	//Due to the cloaking card, we have to check the wall versus to known
	//skill level rather than the used one [Skotlex]
	//if( sd && val1 < 3 && skill_check_cloaking(bl, NULL) )
	if( pc_checkskill(sd, AS_CLOAKING) < 3 && !skill_check_cloaking(&sd->bl, NULL) )
		return false;

	return true;
}

/**
 * Verifies if an user can still be cloaked (AS_CLOAKING)
 * Is called via map_foreachinallrange when any kind of wall disapears
 */
int skill_check_cloaking_end(struct block_list *bl, va_list ap) {
	struct map_session_data *sd = NULL;

	nullpo_ret(bl);

	if( (sd = map_id2sd(bl->id)) && sd->sc.data[SC_CLOAKING] && !skill_can_cloak(sd) )
		status_change_end(bl, SC_CLOAKING, INVALID_TIMER);

	return 0;
}

/**
 * Check camouflage condition
 * @param bl
 * @param sce
 * @return True if near wall; False otherwise
 */
bool skill_check_camouflage(struct block_list *bl, struct status_change_entry *sce)
{
	bool wall = true;

	if( bl->type == BL_PC ) { //Check for walls
		static const int dx[] = { 0, 1, 0, -1, -1,  1, 1, -1};
		static const int dy[] = {-1, 0, 1,  0, -1, -1, 1,  1};
		int i;

		ARR_FIND(0, 8, i, map_getcell(bl->m, bl->x + dx[i], bl->y + dy[i], CELL_CHKNOPASS) != 0);
		if( i == 8 )
			wall = false;
	}

	if( sce ) {
		if( !wall && sce->val1 == 1 ) //End camouflage
			status_change_end(bl, SC_CAMOUFLAGE, INVALID_TIMER);
		status_calc_bl(bl, SCB_SPEED);
	}

	return wall;
}

/**
 * Process skill unit visibilty for single BL in area
 * @param bl
 * @param ap
 * @author [Cydh]
 */
int skill_getareachar_skillunit_visibilty_sub(struct block_list *bl, va_list ap)
{
	struct skill_unit *su = NULL;
	struct block_list *src = NULL;
	unsigned int party1 = 0;
	bool visible = true;

	nullpo_ret(bl);
	nullpo_ret((su = va_arg(ap, struct skill_unit *)));
	nullpo_ret((src = va_arg(ap, struct block_list *)));
	party1 = va_arg(ap, unsigned int);

	if(bl->id != src->id) {
		unsigned int party2 = status_get_party_id(bl);

		if(!party1 || !party2 || party1 != party2)
			visible = false;
	}
	clif_getareachar_skillunit(bl, su, SELF, visible);
	return 1;
}

/**
 * Check for skill unit visibilty in area on
 * - skill first placement
 * - moved dance
 * @param su Skill unit
 * @param target Affected target for this visibility @see enum send_target
 * @author [Cydh]
 */
void skill_getareachar_skillunit_visibilty(struct skill_unit *su, enum send_target target)
{
	nullpo_retv(su);

	if(!su->hidden) //It's not hidden, just do this!
		clif_getareachar_skillunit(&su->bl, su, target, true);
	else {
		struct block_list *src = battle_get_master(&su->bl);

		map_foreachinallarea(skill_getareachar_skillunit_visibilty_sub, su->bl.m, su->bl.x-AREA_SIZE, su->bl.y-AREA_SIZE,
			su->bl.x+AREA_SIZE, su->bl.y+AREA_SIZE, BL_PC, su, src, status_get_party_id(src));
	}
}

/**
 * Check for skill unit visibilty on single BL on insight/spawn action
 * @param su Skill unit
 * @param bl Block list
 * @author [Cydh]
 */
void skill_getareachar_skillunit_visibilty_single(struct skill_unit *su, struct block_list *bl)
{
	bool visible = true;
	struct block_list *src = NULL;

	nullpo_retv(bl);
	nullpo_retv(su);
	nullpo_retv((src = battle_get_master(&su->bl)));

	if(su->hidden && bl->id != src->id) {
		unsigned int party1 = status_get_party_id(src);
		unsigned int party2 = status_get_party_id(bl);

		if(!party1 || !party2 || party1 != party2)
			visible = false;
	}
	clif_getareachar_skillunit(bl, su, SELF, visible);
}

/**
 * Initialize new skill unit for skill unit group.
 * Overall, Skill Unit makes skill unit group which each group holds their cell datas (skill unit)
 * @param group Skill unit group
 * @param idx
 * @param x
 * @param y
 * @param val1
 * @param val2
 */
struct skill_unit *skill_initunit(struct skill_unit_group *group, int idx, int x, int y, int val1, int val2, int val3, int val4, bool hidden)
{
	struct skill_unit *unit;

	nullpo_retr(NULL, group);
	nullpo_retr(NULL, group->unit); //Crash-protection against poor coding
	nullpo_retr(NULL, (unit =& group->unit[idx]));

	if(!unit->alive)
		group->alive_count++;

	unit->bl.id = map_get_new_object_id();
	unit->bl.type = BL_SKILL;
	unit->bl.m = group->map;
	unit->bl.x = x;
	unit->bl.y = y;
	unit->group = group;
	unit->alive = 1;
	unit->val1 = val1;
	unit->val2 = val2;
	unit->val3 = val3;
	unit->val4 = val4;
	unit->prev = 0;
	unit->hidden = hidden;

	//Stores new skill unit
	idb_put(skillunit_db, unit->bl.id, unit);
	map_addiddb(&unit->bl);
	if (map_addblock(&unit->bl))
		return NULL;

	//Perform oninit actions
	switch (group->skill_id) {
		case WZ_ICEWALL:
			map_setgatcell(unit->bl.m,unit->bl.x,unit->bl.y,5);
			clif_changemapcell(0,unit->bl.m,unit->bl.x,unit->bl.y,5,AREA);
			skill_unitsetmapcell(unit,WZ_ICEWALL,group->skill_lv,CELL_ICEWALL,true);
			break;
		case SA_LANDPROTECTOR:
			skill_unitsetmapcell(unit,SA_LANDPROTECTOR,group->skill_lv,CELL_LANDPROTECTOR,true);
			break;
		case HP_BASILICA:
			skill_unitsetmapcell(unit,HP_BASILICA,group->skill_lv,CELL_BASILICA,true);
			break;
		case SC_MAELSTROM:
			skill_unitsetmapcell(unit,SC_MAELSTROM,group->skill_lv,CELL_MAELSTROM,true);
			break;
		default:
			if (group->state.song_dance&0x1) //Check for dissonance
				skill_dance_overlap(unit,1);
			break;
	}

	skill_getareachar_skillunit_visibilty(unit,AREA);
	return unit;
}

/**
 * Remove unit
 * @param unit
 */
int skill_delunit(struct skill_unit *unit)
{
	struct skill_unit_group *group;

	if( !unit || !unit->alive || !unit->group )
		return 0;

	unit->alive = 0;
	group = unit->group;

	if( group->state.song_dance&0x1 )
		skill_dance_overlap(unit,0); //Cancel dissonance effect

	if( !unit->range ) //Invoke onout event
		map_foreachincell(skill_unit_effect,unit->bl.m,unit->bl.x,unit->bl.y,group->bl_flag,&unit->bl,gettick(),4);

	switch( group->skill_id ) { //Perform ondelete actions
		case HT_ANKLESNARE:
		case GN_THORNS_TRAP:
		case SC_ESCAPE:
			{
				struct block_list *target = map_id2bl(group->val2);
				enum sc_type type = status_skill2sc(group->skill_id);

				if( target )
					status_change_end(target,type,INVALID_TIMER);
			}
			break;
		case WZ_ICEWALL:
			map_setcell(unit->bl.m,unit->bl.x,unit->bl.y,CELL_NOICEWALL,false);
			map_setgatcell(unit->bl.m,unit->bl.x,unit->bl.y,unit->val2);
			clif_changemapcell(0,unit->bl.m,unit->bl.x,unit->bl.y,unit->val2,ALL_SAMEMAP); //Hack to avoid clientside cell bug
			skill_unitsetmapcell(unit,WZ_ICEWALL,group->skill_lv,CELL_ICEWALL,false);
			//AS_CLOAKING in low levels requires a wall to be cast
			//Thus it needs to be checked again when a wall disapears! issue:8182 [Panikon]
			map_foreachinallarea(skill_check_cloaking_end,unit->bl.m,
				unit->bl.x - 1,unit->bl.y - 1,unit->bl.x + 1,unit->bl.x + 1,BL_PC); //Use 3x3 area to check for users near cell
			break;
		case SA_LANDPROTECTOR:
			skill_unitsetmapcell(unit,SA_LANDPROTECTOR,group->skill_lv,CELL_LANDPROTECTOR,false);
			break;
		case HP_BASILICA:
			skill_unitsetmapcell(unit,HP_BASILICA,group->skill_lv,CELL_BASILICA,false);
			break;
		case SC_MAELSTROM:
			skill_unitsetmapcell(unit,SC_MAELSTROM,group->skill_lv,CELL_MAELSTROM,false);
			break;
	}

	clif_skill_delunit(unit);
	unit->group = NULL;
	map_delblock(&unit->bl); //Don't free yet
	map_deliddb(&unit->bl);
	idb_remove(skillunit_db,unit->bl.id);

	if( --group->alive_count == 0 )
		skill_delunitgroup(group);

	return 0;
}

static DBMap *skillunit_group_db = NULL; //Skill unit group DB. Key int group_id -> struct skill_unit_group*

///Returns the target skill_unit_group or NULL if not found.
struct skill_unit_group *skill_id2group(int group_id)
{
	return (struct skill_unit_group *)idb_get(skillunit_group_db, group_id);
}

static int skill_unit_group_newid = MAX_SKILL_DB; //Skill Unit Group ID

/**
 * Returns a new group_id that isn't being used in skillunit_group_db.
 * Fatal error if nothing is available.
 */
static int skill_get_new_group_id(void)
{
	if( skill_unit_group_newid >= MAX_SKILL_DB && !skill_id2group(skill_unit_group_newid) )
		return skill_unit_group_newid++; //Available
	{ //Find next id
		int base_id = skill_unit_group_newid;

		while( base_id != ++skill_unit_group_newid ) {
			if( skill_unit_group_newid < MAX_SKILL_DB )
				skill_unit_group_newid = MAX_SKILL_DB;
			if( !skill_id2group(skill_unit_group_newid) )
				return skill_unit_group_newid++; //Available
		}
		//Full loop, nothing available
		ShowFatalError("skill_get_new_group_id: All ids are taken. Exiting...");
		exit(1);
	}
}

/**
 * Initialize skill unit group called while setting new unit (skill unit/ground skill) in skill_unitsetting()
 * @param src Object that cast the skill
 * @param count How many 'cells' used that needed. Related with skill layout
 * @param skill_id ID of used skill
 * @param skill_lv Skill level of used skill
 * @param unit_id Unit ID (look at skill_unit_db.txt)
 * @param limit Lifetime for skill unit, uses skill_get_time(skill_id, skill_lv)
 * @param interval Time interval
 * @return skill_unit_group
 */
struct skill_unit_group *skill_initunitgroup(struct block_list *src, int count, uint16 skill_id, uint16 skill_lv, int unit_id, int limit, int interval)
{
	struct unit_data *ud = unit_bl2ud(src);
	struct skill_unit_group *group;
	int i;

	if(!(skill_id && skill_lv))
		return 0;

	nullpo_retr(NULL, src);
	nullpo_retr(NULL, ud);

	//Find a free spot to store the new unit group
	//@TODO: Make this flexible maybe by changing this fixed array?
	ARR_FIND(0, MAX_SKILLUNITGROUP, i, ud->skillunit[i] == NULL);
	if(i == MAX_SKILLUNITGROUP) {
		//Array is full, make room by discarding oldest group
		int j = 0;
		unsigned maxdiff = 0, tick = gettick();

		for(i = 0; i < MAX_SKILLUNITGROUP && ud->skillunit[i]; i++) {
			unsigned int x = DIFF_TICK(tick,ud->skillunit[i]->tick);

			if(x > maxdiff) {
				maxdiff = x;
				j = i;
			}
		}
		skill_delunitgroup(ud->skillunit[j]);
		//Since elements must have shifted, we use the last slot
		i = MAX_SKILLUNITGROUP - 1;
	}

	group                = ers_alloc(skill_unit_ers, struct skill_unit_group);
	group->src_id        = src->id;
	group->party_id      = status_get_party_id(src);
	group->guild_id      = status_get_guild_id(src);
	group->bg_id         = bg_team_get_id(src);
	group->group_id      = skill_get_new_group_id();
	group->link_group_id = 0;
	group->unit          = (struct skill_unit *)aCalloc(count, sizeof(struct skill_unit));
	group->unit_count    = count;
	group->alive_count   = 0;
	group->val1          = 0;
	group->val2          = 0;
	group->val3          = 0;
	group->skill_id      = skill_id;
	group->skill_lv      = skill_lv;
	group->unit_id       = unit_id;
	group->map           = src->m;
	group->limit         = limit;
	group->interval      = interval;
	group->tick          = gettick();
	group->valstr        = NULL;

	ud->skillunit[i] = group;

	//Stores this new group to DBMap
	idb_put(skillunit_group_db, group->group_id, group);
	return group;
}

/**
 * Remove skill unit group
 * @param group
 * @param file
 * @param line
 * @param *func
 */
int skill_delunitgroup_(struct skill_unit_group *group, const char *file, int line, const char *func)
{
	struct block_list *src;
	struct unit_data *ud;
	struct status_change *sc;
	short i, j;
	int link_group_id;

	if( !group ) {
		ShowDebug("skill_delunitgroup: group is NULL (source=%s:%d, %s)! Please report this! (#3504)\n", file, line, func);
		return 0;
	}

	src = map_id2bl(group->src_id);
	ud = unit_bl2ud(src);

	if( !src || !ud ) {
		ShowError("skill_delunitgroup: Group's source not found! (src_id: %d skill_id: %d)\n", group->src_id, group->skill_id);
		return 0;
	}

	if( src->type == BL_PC && !status_isdead(src) && ((TBL_PC *)src)->state.warping && !((TBL_PC *)src)->state.changemap ) {
		switch( group->skill_id ) {
			case BA_DISSONANCE:
			case BA_POEMBRAGI:
			case BA_WHISTLE:
			case BA_ASSASSINCROSS:
			case BA_APPLEIDUN:
			case DC_UGLYDANCE:
			case DC_HUMMING:
			case DC_DONTFORGETME:
			case DC_FORTUNEKISS:
			case DC_SERVICEFORYOU:
			case NC_NEUTRALBARRIER:
			case NC_STEALTHFIELD:
			case LG_BANDING:
				skill_usave_add(((TBL_PC *)src), group->skill_id, group->skill_lv);
				break;
		}
	}

	sc = status_get_sc(src);

	if( skill_get_unit_flag(group->skill_id)&(UF_DANCE|UF_SONG|UF_ENSEMBLE) ) {
		if( sc && sc->data[SC_DANCING] ) {
			sc->data[SC_DANCING]->val2 = 0 ; //This prevents status_change_end attempting to redelete the group [Skotlex]
			status_change_end(src, SC_DANCING, INVALID_TIMER);
		}
	}

	//End SC from the master when the skill group is deleted
	i = SC_NONE;
	switch( group->unit_id ) {
		case UNT_GOSPEL:     i = SC_GOSPEL;     break;
		case UNT_BASILICA:   i = SC_BASILICA;   break;
	}

	if( i != SC_NONE && sc && sc->data[i] ) {
		sc->data[i]->val3 = 0; //Remove reference to this group [Skotlex]
		status_change_end(src, (sc_type)i, INVALID_TIMER);
	}

	switch( group->skill_id ) {
		case PF_SPIDERWEB: {
				struct block_list *bl = map_id2bl(group->val2);
				struct status_change *tsc = NULL;
				bool removed = true;

				//Clear group id from status change
				if( bl && (tsc = status_get_sc(bl)) && tsc->data[SC_SPIDERWEB] ) {
					if( tsc->data[SC_SPIDERWEB]->val2 == group->group_id )
						tsc->data[SC_SPIDERWEB]->val2 = 0;
					else if( tsc->data[SC_SPIDERWEB]->val3 == group->group_id )
						tsc->data[SC_SPIDERWEB]->val3 = 0;
					else if( tsc->data[SC_SPIDERWEB]->val4 == group->group_id )
						tsc->data[SC_SPIDERWEB]->val4 = 0;
					else //Group was already removed in status_change_end, don't call it again!
						removed = false;
					//The last group was cleared, end status change
					if( removed && !tsc->data[SC_SPIDERWEB]->val2 && !tsc->data[SC_SPIDERWEB]->val3 && !tsc->data[SC_SPIDERWEB]->val4 )
						status_change_end(bl, SC_SPIDERWEB, INVALID_TIMER);
				}
			}
			break;
		case SG_SUN_WARM:
		case SG_MOON_WARM:
		case SG_STAR_WARM:
			if( sc && sc->data[SC_WARM] ) {
				sc->data[SC_WARM]->val4 = 0;
				status_change_end(src, SC_WARM, INVALID_TIMER);
			}
			break;
		case NC_NEUTRALBARRIER:
			if( sc && sc->data[SC_NEUTRALBARRIER_MASTER] ) {
				sc->data[SC_NEUTRALBARRIER_MASTER]->val2 = 0;
				status_change_end(src, SC_NEUTRALBARRIER_MASTER, INVALID_TIMER);
			}
			break;
		case NC_STEALTHFIELD:
			if( sc && sc->data[SC_STEALTHFIELD_MASTER] ) {
				sc->data[SC_STEALTHFIELD_MASTER]->val2 = 0;
				status_change_end(src, SC_STEALTHFIELD_MASTER, INVALID_TIMER);
			}
			break;
		case LG_BANDING:
			if( sc && sc->data[SC_BANDING] ) {
				sc->data[SC_BANDING]->val4 = 0;
				status_change_end(src, SC_BANDING, INVALID_TIMER);
			}
			break;
	}

	if( src->type == BL_PC && group->state.ammo_consume )
		battle_consume_ammo((TBL_PC *)src, group->skill_id, group->skill_lv);

	group->alive_count = 0;

	//Remove all unit cells
	if( group->unit ) {
		for( i = 0; i < group->unit_count; i++ )
			skill_delunit(&group->unit[i]);
	}

	//Clear Talkie-box string
	if( group->valstr ) {
		aFree(group->valstr);
		group->valstr = NULL;
	}

	idb_remove(skillunit_group_db, group->group_id);
	map_freeblock(&group->unit->bl); //Schedules deallocation of whole array (HACK)
	group->unit = NULL;
	group->group_id = 0;
	group->unit_count = 0;
	link_group_id = group->link_group_id;
	group->link_group_id = 0;

	//Locate this group, swap with the last entry and delete it
	ARR_FIND(0, MAX_SKILLUNITGROUP, i, ud->skillunit[i] == group);
	ARR_FIND(i, MAX_SKILLUNITGROUP, j, ud->skillunit[j] == NULL);
	j--;
	if( i < MAX_SKILLUNITGROUP ) {
		ud->skillunit[i] = ud->skillunit[j];
		ud->skillunit[j] = NULL;
		ers_free(skill_unit_ers, group);
	} else
		ShowError("skill_delunitgroup: Group not found! (src_id: %d skill_id: %d)\n", group->src_id, group->skill_id);

	if( link_group_id ) {
		struct skill_unit_group *group_cur = skill_id2group(link_group_id);

		if( group_cur )
			skill_delunitgroup(group_cur);
	}

	return 1;
}

/**
 * Clear all Skill Unit Group from an Object, example usage when player logged off or dead
 * @param src
 */
void skill_clear_unitgroup(struct block_list *src)
{
	struct unit_data *ud;

	nullpo_retv(src);
	nullpo_retv((ud = unit_bl2ud(src)));

	while (ud->skillunit[0])
		skill_delunitgroup(ud->skillunit[0]);
}

/**
 * Search tickset for skill unit in skill unit group
 * @param bl Block List for skill_unit
 * @param group Skill unit group
 * @param tick
 * @return skill_unit_group_tickset if found
 */
struct skill_unit_group_tickset *skill_unitgrouptickset_search(struct block_list *bl, struct skill_unit_group *group, int tick)
{
	int i, j = -1, s, id;
	struct unit_data *ud;
	struct skill_unit_group_tickset *set;

	nullpo_ret(bl);

	if (group->interval == -1)
		return NULL;

	ud = unit_bl2ud(bl);
	if (!ud)
		return NULL;

	set = ud->skillunittick;

	if (skill_get_unit_flag(group->skill_id)&UF_NOOVERLAP)
		id = s = group->skill_id;
	else
		id = s = group->group_id;

	for (i = 0; i < MAX_SKILLUNITGROUPTICKSET; i++) {
		int k = (i + s)%MAX_SKILLUNITGROUPTICKSET;

		if (set[k].id == id)
			return &set[k];
		else if (j == -1 && (DIFF_TICK(tick,set[k].tick) > 0 || !set[k].id))
			j = k;
	}

	if (j == -1) {
		ShowWarning ("skill_unitgrouptickset_search: tickset is full\n");
		j = id % MAX_SKILLUNITGROUPTICKSET;
	}

	set[j].id = id;
	set[j].tick = tick;
	return &set[j];
}

/*==========================================
 * Check for validity skill unit that triggered by skill_unit_timer_sub
 * And trigger skill_unit_onplace_timer for object that maybe stands there (catched object is *bl)
 *------------------------------------------*/
int skill_unit_timer_sub_onplace(struct block_list *bl, va_list ap)
{
	struct skill_unit *unit = va_arg(ap,struct skill_unit *);
	struct skill_unit_group *group = NULL;
	unsigned int tick = va_arg(ap,unsigned int);

	if( !unit || !unit->alive || !unit->group || !bl->prev )
		return 0;

	group = unit->group;

	if( !(skill_get_inf2(group->skill_id)&(INF2_TRAP)) && !(skill_get_inf3(group->skill_id)&(INF3_NOLP)) &&
		(map_getcell(unit->bl.m,unit->bl.x,unit->bl.y,CELL_CHKLANDPROTECTOR) ||
		map_getcell(bl->m,bl->x,bl->y,CELL_CHKMAELSTROM)) )
		return 0; //AoE skills are ineffective except non-essamble dance skills and traps

	if( group->skill_id != GN_WALLOFTHORN && battle_check_target(&unit->bl,bl,group->target_flag) <= 0 )
		return 0;

	skill_unit_onplace_timer(unit,bl,tick);
	return 1;
}

/**
 * @see DBApply
 * Sub function of skill_unit_timer for executing each skill unit from skillunit_db
 */
static int skill_unit_timer_sub(DBKey key, DBData *data, va_list ap)
{
	struct skill_unit *unit = (struct skill_unit *)db_data2ptr(data);
	struct skill_unit_group *group = NULL;
	unsigned int tick = va_arg(ap,unsigned int);
	bool dissonance;

	if( !unit || !unit->alive || !unit->group )
		return 0;

	group = unit->group;

	//Check for expiration
	if( !group->state.guildaura && (DIFF_TICK(tick,group->tick) >= group->limit || DIFF_TICK(tick,group->tick) >= unit->limit) ) {
		switch( group->unit_id ) { //Skill unit expired (inlined from skill_unit_onlimit())
			case UNT_ICEWALL:
				unit->val1 -= 50; //Icewall loses 50 HP every second
				group->limit = DIFF_TICK(tick + group->interval,group->tick);
				unit->limit = DIFF_TICK(tick + group->interval,group->tick);
				if( unit->val1 <= 0 )
					skill_delunit(unit);
				break;
			case UNT_BLASTMINE:
#ifdef RENEWAL
			case UNT_CLAYMORETRAP:
#endif
			case UNT_GROUNDDRIFT_WIND:
			case UNT_GROUNDDRIFT_DARK:
			case UNT_GROUNDDRIFT_POISON:
			case UNT_GROUNDDRIFT_WATER:
			case UNT_GROUNDDRIFT_FIRE:
			case UNT_GROUNDDRIFT_NEUTRAL:
				group->unit_id = UNT_USED_TRAPS;
				//clif_changetraplook(&unit->bl,UNT_FIREPILLAR_ACTIVE);
				group->limit = DIFF_TICK(tick + 1500,group->tick);
				unit->limit = DIFF_TICK(tick + 1500,group->tick);
				break;

			case UNT_ANKLESNARE:
				if( group->val2 ) { //Used Trap doesn't return back to item
					skill_delunit(unit);
					break;
				}
			//Fall through
			case UNT_SKIDTRAP:
			case UNT_LANDMINE:
			case UNT_SHOCKWAVE:
			case UNT_SANDMAN:
			case UNT_FLASHER:
			case UNT_FREEZINGTRAP:
#ifndef RENEWAL
			case UNT_CLAYMORETRAP:
#endif
			case UNT_TALKIEBOX:
			case UNT_ELECTRICSHOCKER:
			case UNT_CLUSTERBOMB:
			case UNT_MAGENTATRAP:
			case UNT_COBALTTRAP:
			case UNT_MAIZETRAP:
			case UNT_VERDURETRAP:
			case UNT_FIRINGTRAP:
			case UNT_ICEBOUNDTRAP:
				{
					struct block_list *src = map_id2bl(group->src_id);

					if( unit->val1 && src && src->type == BL_PC &&
						(group->item_id == ITEMID_TRAP || group->item_id == ITEMID_SPECIAL_ALLOY_TRAP) ) {
						struct item item_tmp; //Revert unit back into a trap

						memset(&item_tmp,0,sizeof(item_tmp));
						item_tmp.nameid = group->item_id;
						item_tmp.identify = 1;
						map_addflooritem(&item_tmp,1,unit->bl.m,unit->bl.x,unit->bl.y,0,0,0,4,0,false);
					}
					skill_delunit(unit);
				}
				break;

			case UNT_WARP_ACTIVE:
				//Warp portal opens (morph to a UNT_WARP_WAITING cell)
				group->unit_id = skill_get_unit_id(group->skill_id,1); //UNT_WARP_WAITING
				clif_changelook(&unit->bl,LOOK_BASE,group->unit_id);
				//Restart timers
				group->limit = skill_get_time(group->skill_id,group->skill_lv);
				unit->limit = skill_get_time(group->skill_id,group->skill_lv);
				//Apply effect to all units standing on it
				map_foreachincell(skill_unit_effect,unit->bl.m,unit->bl.x,unit->bl.y,group->bl_flag,&unit->bl,gettick(),1);
				break;

			case UNT_CALLFAMILY: {
					struct map_session_data *sd = NULL;

					if( group->val1 ) {
						sd = map_charid2sd(group->val1);
						group->val1 = 0;
						if( sd && !mapdata[sd->bl.m].flag.nowarp && pc_job_can_entermap((enum e_job)sd->status.class_,unit->bl.m,sd->group_level) )
							pc_setpos(sd,map_id2index(unit->bl.m),unit->bl.x,unit->bl.y,CLR_TELEPORT);
					}
					if( group->val2 ) {
						sd = map_charid2sd(group->val2);
						group->val2 = 0;
						if( sd && !mapdata[sd->bl.m].flag.nowarp && pc_job_can_entermap((enum e_job)sd->status.class_,unit->bl.m,sd->group_level) )
							pc_setpos(sd,map_id2index(unit->bl.m),unit->bl.x,unit->bl.y,CLR_TELEPORT);
					}
					skill_delunit(unit);
				}
				break;

			case UNT_REVERBERATION:
				if( unit->val1 <= 0 ) { //If it was deactivated
					skill_delunit(unit);
					break;
				}
				clif_changetraplook(&unit->bl,UNT_USED_TRAPS);
				map_foreachinallrange(skill_trap_splash,&unit->bl,skill_get_splash(group->skill_id,group->skill_lv),group->bl_flag,&unit->bl,tick);
				group->limit = DIFF_TICK(tick,group->tick) + 1000;
				unit->limit = DIFF_TICK(tick,group->tick) + 1000;
				group->unit_id = UNT_USED_TRAPS;
				break;

			case UNT_FEINTBOMB: {
					struct block_list *src =  map_id2bl(group->src_id);

					if( src )
						map_foreachinallrange(skill_area_sub,&unit->bl,unit->range,BL_CHAR|BL_SKILL,src,group->skill_id,group->skill_lv,tick,BCT_ENEMY|SD_LEVEL|SD_ANIMATION|1,skill_castend_damage_id);
					skill_delunit(unit);
				}
				break;

			default:
				if( group->val2 && (group->skill_id == WZ_METEOR || group->skill_id == SU_CN_METEOR || group->skill_id == SU_CN_METEOR2) )
					break; //Deal damage before expiration
				skill_delunit(unit);
				break;
		}
	} else { //Skill unit is still active
		switch( group->unit_id ) {
			case UNT_BLASTMINE:
			case UNT_SKIDTRAP:
			case UNT_LANDMINE:
			case UNT_SHOCKWAVE:
			case UNT_SANDMAN:
			case UNT_FLASHER:
			case UNT_CLAYMORETRAP:
			case UNT_FREEZINGTRAP:
			case UNT_TALKIEBOX:
			case UNT_ANKLESNARE:
				if( unit->val1 <= 0 ) {
					if( group->unit_id == UNT_ANKLESNARE && group->val2 )
						skill_delunit(unit);
					else {
						clif_changetraplook(&unit->bl,(group->unit_id == UNT_LANDMINE ? UNT_FIREPILLAR_ACTIVE : UNT_USED_TRAPS));
						group->limit = DIFF_TICK(tick,group->tick) + 1500;
						group->unit_id = UNT_USED_TRAPS;
					}
				}
				break;
			case UNT_SANCTUARY:
				if( group->val1 <= 0 )
					skill_delunitgroup(group);
				break;
			case UNT_REVERBERATION:
				if( unit->val1 <= 0 ) {
					clif_changetraplook(&unit->bl,UNT_USED_TRAPS);
					map_foreachinallrange(skill_trap_splash,&unit->bl,skill_get_splash(group->skill_id,group->skill_lv),group->bl_flag,&unit->bl,tick);
					group->limit = DIFF_TICK(tick,group->tick) + 1000;
					unit->limit = DIFF_TICK(tick,group->tick) + 1000;
					group->unit_id = UNT_USED_TRAPS;
				}
				break;
			case UNT_WALLOFTHORN:
				if( unit->val1 <= 0 || unit->val3 <= 0 )
					skill_delunit(unit);
				break;
			default:
				if( group->skill_id == WZ_METEOR || group->skill_id == SU_CN_METEOR || group->skill_id == SU_CN_METEOR2 ) { //Unit will expire the next interval, start dropping Meteor
					if( !group->val2 && (DIFF_TICK(tick,group->tick) >= group->limit - group->interval || DIFF_TICK(tick,group->tick) >= unit->limit - group->interval) ) {
						struct block_list *src = NULL;

						if( (src = map_id2bl(group->src_id)) ) {
							clif_skill_poseffect(src,group->skill_id,group->skill_lv,unit->bl.x,unit->bl.y,tick);
							group->val2 = 1;
						}
					}
					return 0; //No damage until expiration
				}
				break;
		}
	}

	//Don't continue if unit is expired and has been deleted
	if( !unit->alive )
		return 0;

	dissonance = skill_dance_switch(unit,0);

	if( unit->range >= 0 && group->interval != -1 && unit->bl.id != unit->prev ) {
		map_foreachinrange(skill_unit_timer_sub_onplace,&unit->bl,unit->range,group->bl_flag,&unit->bl,tick);
		if( unit->range == -1 ) //Unit disabled, but it should not be deleted yet
			group->unit_id = UNT_USED_TRAPS;
		else if( group->unit_id == UNT_TATAMIGAESHI ) {
			unit->range = -1; //Disable processed cell
			if( --group->val1 <= 0 ) { //Number of live cells
				//All tiles were processed, disable skill
				group->target_flag = BCT_NOONE;
				group->bl_flag = BL_NUL;
			}
		} else if( group->skill_id == WZ_METEOR || group->skill_id == SU_CN_METEOR || group->skill_id == SU_CN_METEOR2 ) {
			skill_delunit(unit);
			return 0;
		}
		if( group->limit == group->interval )
			unit->prev = unit->bl.id;
	}

	if( dissonance )
		skill_dance_switch(unit,1);
	return 0;
}
/*==========================================
 * Executes on all skill units every SKILLUNITTIMER_INTERVAL miliseconds.
 *------------------------------------------*/
TIMER_FUNC(skill_unit_timer)
{
	map_freeblock_lock();

	skillunit_db->foreach(skillunit_db, skill_unit_timer_sub, tick);

	map_freeblock_unlock();
	return 0;
}

static int skill_unit_temp[20]; //Temporary storage for tracking skill unit skill ids as players move in/out of them
/*==========================================
 * Flag :
 *	1 : Store that skill_unit in array
 *	2 : Clear that skill_unit
 *	4 : call_on_left
 *------------------------------------------*/
int skill_unit_move_sub(struct block_list *bl, va_list ap)
{
	struct skill_unit *unit = (struct skill_unit *)bl;
	struct skill_unit_group *group = NULL;

	struct block_list *target = va_arg(ap,struct block_list *);
	unsigned int tick = va_arg(ap,unsigned int);
	int flag = va_arg(ap,int);
	bool dissonance;
	uint16 skill_id;
	int i;

	if( !unit || !unit->alive || !unit->group )
		return 0;

	nullpo_ret(target);

	if( !target->prev )
		return 0;

	group = unit->group;

	//Necessary in case the group is deleted after calling on_place/on_out [Skotlex]
	skill_id = group->skill_id;

	if( (flag&1) && (skill_id == PF_SPIDERWEB || //Fiberlock is never supposed to trigger on skill_unit_move [Inkfish]
		skill_id == LG_KINGS_GRACE) )
		return 0;

	dissonance = skill_dance_switch(unit,0);

	//Lullaby is the exception, bugreport:411
	if( group->interval != -1 && !(skill_get_unit_flag(skill_id)&UF_DUALMODE) && skill_id != BD_LULLABY ) {
		//Non-dualmode unit skills with a timer don't trigger when walking, so just return
		if( dissonance ) {
			skill_dance_switch(unit,1);
			skill_unit_onleft(skill_unit_onout(unit,target,tick),target,tick); //We placed a dissonance, let's update
		}
		return 0;
	}

	//Target-type check
	if( !(group->bl_flag&target->type && battle_check_target(&unit->bl,target,group->target_flag) > 0) ) {
		if( group->src_id == target->id && group->state.song_dance&0x2 ) {
			//Ensemble check to see if they went out/in of the area [Skotlex]
			if( flag&1 ) {
				if( flag&2 ) { //Clear this skill id
					ARR_FIND(0,ARRAYLENGTH(skill_unit_temp),i,skill_unit_temp[i] == skill_id);
					if( i < ARRAYLENGTH(skill_unit_temp) )
						skill_unit_temp[i] = 0;
				}
			} else {
				if( flag&2 ) { //Store this skill id
					ARR_FIND(0,ARRAYLENGTH(skill_unit_temp),i,skill_unit_temp[i] == 0);
					if( i < ARRAYLENGTH(skill_unit_temp) )
						skill_unit_temp[i] = skill_id;
					else
						ShowError("skill_unit_move_sub: Reached limit of unit objects per cell! (skill_id: %hu)\n",skill_id);
				}
			}
			if( flag&4 )
				skill_unit_onleft(skill_id,target,tick);
		}
		if( dissonance )
			skill_dance_switch(unit,1);
		return 0;
	} else {
		if( flag&1 ) {
			int result = skill_unit_onplace(unit,target,tick);

			if( flag&2 && result ) { //Clear skill ids we have stored in onout
				ARR_FIND(0,ARRAYLENGTH(skill_unit_temp),i,skill_unit_temp[i] == result);
				if( i < ARRAYLENGTH(skill_unit_temp) )
					skill_unit_temp[i] = 0;
			}
		} else {
			int result = skill_unit_onout(unit,target,tick);

			if( flag&2 && result ) { //Store this unit id
				ARR_FIND(0,ARRAYLENGTH(skill_unit_temp),i,skill_unit_temp[i] == 0);
				if( i < ARRAYLENGTH(skill_unit_temp) )
					skill_unit_temp[i] = skill_id;
				else
					ShowError("skill_unit_move_sub: Reached limit of unit objects per cell! (skill_id: %hu)\n",skill_id);
			}
		}
		//@TODO: Normally, this is dangerous since the unit and group could be freed
		//inside the onout/onplace functions. Currently it is safe because we know song/dance
		//cells do not get deleted within them [Skotlex]
		if( dissonance )
			skill_dance_switch(unit,1);
		if( flag&4 )
			skill_unit_onleft(skill_id,target,tick);
		return 1;
	}
}

/*==========================================
 * Invoked when a char has moved and unit cells must be invoked (onplace, onout, onleft)
 * Flag values:
 * flag&1: invoke skill_unit_onplace (otherwise invoke skill_unit_onout)
 * flag&2: this function is being invoked twice as a bl moves, store in memory the affected
 * units to figure out when they have left a group.
 * flag&4: Force a onleft event (triggered when the bl is killed, for example)
 *------------------------------------------*/
int skill_unit_move(struct block_list *bl, unsigned int tick, int flag)
{
	nullpo_ret(bl);

	if( !bl->prev )
		return 0;

	if( (flag&2) && !(flag&1) ) //Onout, clear data
		memset(skill_unit_temp, 0, sizeof(skill_unit_temp));

	map_foreachincell(skill_unit_move_sub,bl->m,bl->x,bl->y,BL_SKILL,bl,tick,flag);

	if( (flag&2) && (flag&1) ) { //Onplace, check any skill units you have left
		int i;

		for( i = 0; i < ARRAYLENGTH(skill_unit_temp); i++ )
			if( skill_unit_temp[i] )
				skill_unit_onleft(skill_unit_temp[i], bl, tick);
	}
	return 0;
}

/**
 * Moves skill unit group to map m with coordinates x & y (example when knocked back)
 * @param group Skill Group
 * @param m Map
 * @param dx
 * @param dy
 */
void skill_unit_move_unit_group(struct skill_unit_group *group, int16 m, int16 dx, int16 dy)
{
	int i, j;
	unsigned int tick = gettick();
	int *m_flag;
	struct skill_unit *unit1;
	struct skill_unit *unit2;

	if( group == NULL )
		return;

	if( group->unit_count <= 0 )
		return;

	if( group->unit == NULL )
		return;

	if( skill_get_unit_flag(group->skill_id)&UF_ENSEMBLE )
		return; //Ensembles may not be moved around

	m_flag = (int *)aCalloc(group->unit_count, sizeof(int));
	//   m_flag
	//		0: Neither of the following (skill_unit_onplace & skill_unit_onout are needed)
	//		1: Unit will move to a slot that had another unit of the same group (skill_unit_onplace not needed)
	//		2: Another unit from same group will end up positioned on this unit (skill_unit_onout not needed)
	//		3: Both (1 + 2)

	for( i = 0; i < group->unit_count; i++ ) {
		unit1 = &group->unit[i];
		if( !unit1->alive || unit1->bl.m != m )
			continue;
		for( j = 0; j < group->unit_count; j++ ) {
			unit2 = &group->unit[j];
			if( !unit2->alive )
				continue;
			if( unit1->bl.x + dx == unit2->bl.x && unit1->bl.y + dy == unit2->bl.y )
				m_flag[i] |= 0x1;
			if( unit1->bl.x - dx == unit2->bl.x && unit1->bl.y - dy == unit2->bl.y )
				m_flag[i] |= 0x2;
		}
	}

	j = 0;

	for( i = 0; i < group->unit_count; i++ ) {
		unit1 = &group->unit[i];
		if( !unit1->alive )
			continue;
		if( !(m_flag[i]&0x2) ) {
			if( group->state.song_dance&0x1 ) //Cancel dissonance effect
				skill_dance_overlap(unit1,0);
			map_foreachincell(skill_unit_effect,unit1->bl.m,unit1->bl.x,unit1->bl.y,group->bl_flag,&unit1->bl,tick,4);
		}
		//Move Cell using "smart" criteria (avoid useless moving around)
		switch( m_flag[i] ) {
			case 0:
				//Cell moves independently, safely move it
				map_foreachinmovearea(clif_outsight,&unit1->bl,AREA_SIZE,dx,dy,BL_PC,&unit1->bl);
				map_moveblock(&unit1->bl,unit1->bl.x + dx,unit1->bl.y + dy,tick);
				break;
			case 1:
				//Cell moves unto another cell, look for a replacement cell that won't collide
				//and has no cell moving into it (flag == 2)
				for( ; j < group->unit_count; j++ ) {
					int dx2, dy2;

					if( m_flag[j] != 2 || !group->unit[j].alive )
						continue;
					//Move to where this cell would had moved
					unit2 = &group->unit[j];
					dx2 = unit2->bl.x + dx - unit1->bl.x;
					dy2 = unit2->bl.y + dy - unit1->bl.y;
					map_foreachinmovearea(clif_outsight,&unit1->bl,AREA_SIZE,dx2,dy2,BL_PC,&unit1->bl);
					map_moveblock(&unit1->bl,unit2->bl.x + dx,unit2->bl.y + dy,tick);
					j++; //Skip this cell as we have used it
					break;
				}
				break;
			case 2:
			case 3:
				break; //Don't move the cell as a cell will end on this tile anyway
		}
		if( !(m_flag[i]&0x2) ) { //We only moved the cell in 0-1
			if( group->state.song_dance&0x1 ) //Check for dissonance effect
				skill_dance_overlap(unit1,1);
			skill_getareachar_skillunit_visibilty(unit1,AREA);
			map_foreachincell(skill_unit_effect,unit1->bl.m,unit1->bl.x,unit1->bl.y,group->bl_flag,&unit1->bl,tick,1);
		}
	}
	aFree(m_flag);
}

/**
 * Checking product requirement in player's inventory.
 * Checking if player has the item or not, the amount, and the weight limit.
 * @param sd Player
 * @param nameid Product requested
 * @param unique_id Unique ID used
 * @param trigger Trigger criteria to match will 'ItemLv'
 * @param qty Amount of item will be created
 * @return 0 If failed or Index+1 of item found on skill_produce_db[]
 */
short skill_can_produce_mix(struct map_session_data *sd, unsigned short nameid, unsigned short unique_id, int trigger, int qty)
{
	short i, j;

	nullpo_ret(sd);

	if( !nameid || !itemdb_exists(nameid) )
		return 0;

	for( i = 0; i < MAX_SKILL_PRODUCE_DB; i++ ) {
		if( skill_produce_db[i].nameid == nameid && skill_produce_db[i].unique_id == unique_id ) {
			if( (j = skill_produce_db[i].req_skill) > 0 && !sd->state.abra_flag && pc_checkskill(sd,j) < skill_produce_db[i].req_skill_lv )
				continue; //Must iterate again to check other skills that produce it [malufett]
			if( j > 0 && sd->menuskill_id > 0 && sd->menuskill_id != j )
				continue; //Special case
			break;
		}
	}

	if( i >= MAX_SKILL_PRODUCE_DB )
		return 0;

	if( pc_checkadditem(sd, nameid, qty) == CHKADDITEM_OVERAMOUNT )
		return 0; //Cannot carry the produced stuff

	//Matching the requested produce list
	if( trigger >= 0 ) {
		if( trigger > 20 ) { //Non-weapon, non-food item (itemlv must match)
			if( skill_produce_db[i].itemlv != trigger )
				return 0;
		} else if( trigger > 10 ) { //Food (any item level between 10 and 20 will do)
			if( skill_produce_db[i].itemlv <= 10 || skill_produce_db[i].itemlv > 20 )
				return 0;
		} else { //Weapon (itemlv must be higher or equal)
			if( skill_produce_db[i].itemlv > trigger )
				return 0;
		}
	}

	//Check on player's inventory
	for( j = 0; j < MAX_PRODUCE_RESOURCE; j++ ) {
		unsigned short nameid_produce;

		if( (nameid_produce = skill_produce_db[i].mat_id[j]) == 0 )
			continue;
		if( skill_produce_db[i].mat_amount[j] == 0 ) {
			if( pc_search_inventory(sd,nameid_produce) == INDEX_NOT_FOUND )
				return 0;
		} else {
			unsigned short idx, amt;

			for( idx = 0, amt = 0; idx < MAX_INVENTORY; idx++ ) {
				if( sd->inventory.u.items_inventory[idx].nameid == nameid_produce )
					amt += sd->inventory.u.items_inventory[idx].amount;
			}
			if( itemdb_is_gemstone(nameid_produce) && sd->special_state.no_gemstone )
				continue;
			if( amt < qty * skill_produce_db[i].mat_amount[j] )
				return 0;
		}
	}
	return i + 1;
}

/**
 * Attempt to produce an item
 * @param sd Player
 * @param skill_id Skill used
 * @param nameid Requested product
 * @param unique_id Unique ID used
 * @param slot1
 * @param slot2
 * @param slot3
 * @param qty Amount of requested item
 * @return True is success, False if failed
 */
bool skill_produce_mix(struct map_session_data *sd, uint16 skill_id, unsigned short nameid, unsigned short unique_id, int slot1, int slot2, int slot3, int qty)
{
	int slot[3];
	int i, sc, ele, idx, equip, wlv, make_per = 0, flag = 0, skill_lv = 0;
	int num = -1; //Exclude the recipe
	struct status_data *status;
	struct item_data *data;

	nullpo_ret(sd);

	status = status_get_status_data(&sd->bl);

	if( sd->skill_id_old == skill_id )
		skill_lv = sd->skill_lv_old;

	if( !(idx = skill_can_produce_mix(sd,nameid,unique_id,-1,qty)) )
		return false;

	idx--;
	qty = max(qty,1);

	if( !skill_id ) //A skill can be specified for some override cases
		skill_id = skill_produce_db[idx].req_skill;

	if( skill_id == GC_RESEARCHNEWPOISON )
		skill_id = GC_CREATENEWPOISON;

	slot[0] = slot1;
	slot[1] = slot2;
	slot[2] = slot3;

	for( i = 0, sc = 0, ele = 0; i < 3; i++ ) { //Note that qty should always be one if you are using these!
		short j;

		if( slot[i] <= 0 )
			continue;
		j = pc_search_inventory(sd,slot[i]);
		if( j == INDEX_NOT_FOUND )
			continue;
		if( slot[i] == ITEMID_STAR_CRUMB ) {
			pc_delitem(sd,j,1,1,0,LOG_TYPE_PRODUCE);
			sc++;
		}
		if( slot[i] >= ITEMID_FLAME_HEART && slot[i] <= ITEMID_GREAT_NATURE && ele == 0 ) {
			static const int ele_table[4] = { 3,1,4,2 };

			pc_delitem(sd,j,1,1,0,LOG_TYPE_PRODUCE);
			ele = ele_table[slot[i] - ITEMID_FLAME_HEART];
		}
	}

	if( skill_id == RK_RUNEMASTERY ) {
		int temp_qty;
		uint8 lv = pc_checkskill(sd,skill_id);

		data = itemdb_search(nameid);
		if( lv == 10 )
			temp_qty = 1 + rnd()%3;
		else if( lv >= 5 )
			temp_qty = 1 + rnd()%2;
		else
			temp_qty = 1;
		if( data->stack.inventory ) {
			for( i = 0; i < MAX_INVENTORY; i++ ) {
				if( sd->inventory.u.items_inventory[i].nameid == nameid ) {
					if( sd->inventory.u.items_inventory[i].amount >= data->stack.amount ) {
						clif_msg(sd,RUNE_CANT_CREATE);
						return false;
					} else { //The amount fits, say we got temp_qty 4 and 19 runes, we trim temp_qty to 1
						if( temp_qty + sd->inventory.u.items_inventory[i].amount >= data->stack.amount )
							temp_qty = data->stack.amount - sd->inventory.u.items_inventory[i].amount;
					}
					break;
				}
			}
		}
		qty = temp_qty;
	}

	for( i = 0; i < MAX_PRODUCE_RESOURCE; i++ ) {
		short id, x, j;

		if( !(id = skill_produce_db[idx].mat_id[i]) || !itemdb_exists(id) )
			continue;
		if( itemdb_is_gemstone(id) && sd->special_state.no_gemstone )
			continue;
		num++;
		x = (skill_id == RK_RUNEMASTERY ? 1 : qty) * skill_produce_db[idx].mat_amount[i];
		do {
			int y = 0;

			j = pc_search_inventory(sd,id);
			if( j != INDEX_NOT_FOUND ) {
				y = sd->inventory.u.items_inventory[j].amount;
				if( y > x )
					y = x;
				pc_delitem(sd,j,y,0,0,LOG_TYPE_PRODUCE);
			} else {
				ShowError("skill_produce_mix: material item error\n");
				return false;
			}
			x -= y;
		} while( j >= 0 && x > 0 );
	}

	if( (equip = (itemdb_isequip(nameid) && skill_id != GN_CHANGEMATERIAL && skill_id != GN_MAKEBOMB)) )
		wlv = itemdb_wlv(nameid);

	if( !equip ) {
		switch( skill_id ) {
			case BS_IRON:
			case BS_STEEL:
			case BS_ENCHANTEDSTONE:
				//Ores & Metals Refining - skill bonuses are straight from kRO website [DracoRPG]
				i = pc_checkskill(sd,skill_id);
				make_per = sd->status.job_level * 20 + status->dex * 10 + status->luk * 10; //Base chance
				switch( nameid ) {
					case ITEMID_IRON:
						make_per += 4000 + i * 500; //Temper Iron bonus: +26/+32/+38/+44/+50
						break;
					case ITEMID_STEEL:
						make_per += 3000 + i * 500; //Temper Steel bonus: +35/+40/+45/+50/+55
						break;
					case ITEMID_STAR_CRUMB:
						make_per = 100000; //Star Crumbs are 100% success crafting rate? (made 1000% so it succeeds even after penalties) [Skotlex]
						break;
					default:
						make_per += 1000 + i * 500; //Enchanted Stones Craft bonus: +15/+20/+25/+30/+35
						break;
				}
				break;
			case ASC_CDP:
				make_per = (2000 + 40 * status->dex + 20 * status->luk);
				break;
			case AL_HOLYWATER:
			case AB_ANCILLA:
				make_per = 100000; //100% success
				break;
			case AM_PHARMACY: //Potion Preparation - reviewed with the help of various Ragnainfo sources [DracoRPG]
			case AM_TWILIGHT1:
			case AM_TWILIGHT2:
			case AM_TWILIGHT3:
				make_per = pc_checkskill(sd,AM_LEARNINGPOTION) * 50
					+ pc_checkskill(sd,AM_PHARMACY) * 300 + ((sd->class_&JOBL_THIRD) ? 1400 : sd->status.job_level * 20)
					+ (status->int_ / 2) * 10 + status->dex * 10 + status->luk * 10;
				if( hom_is_active(sd->hd) ) { //Player got a homun
					uint16 lv;

					if( (lv = hom_checkskill(sd->hd,HVAN_INSTRUCT)) > 0 ) //His homun is a vanil with instruction change
						make_per += lv * 100; //+1% bonus per level
				}
				switch( nameid ) {
					case ITEMID_RED_POTION:
					case ITEMID_YELLOW_POTION:
					case ITEMID_WHITE_POTION:
						make_per += (1 + rnd()%100) * 10 + 2000;
						break;
					case ITEMID_ALCOHOL:
						make_per += (1 + rnd()%100) * 10 + 1000;
						break;
					case ITEMID_FIRE_BOTTLE:
					case ITEMID_ACID_BOTTLE:
					case ITEMID_MAN_EATER_BOTTLE:
					case ITEMID_MINI_BOTTLE:
						make_per += (1 + rnd()%100) * 10;
						break;
					case ITEMID_YELLOW_SLIM_POTION:
						make_per -= (1 + rnd()%50) * 10;
						break;
					case ITEMID_WHITE_SLIM_POTION:
					case ITEMID_COATING_BOTTLE:
						make_per -= (1 + rnd()%100) * 10;
					    break;
					//Common items, receive no bonus or penalty, listed just because they are commonly produced
					case ITEMID_BLUE_POTION:
					case ITEMID_RED_SLIM_POTION:
					case ITEMID_ANODYNE:
					case ITEMID_ALOEBERA:
					//Fall through
					default:
						break;
				}
				if( battle_config.pp_rate != 100 )
					make_per = make_per * battle_config.pp_rate / 100;
				break;
			case SA_CREATECON: //Elemental Converter Creation
				make_per = 100000; //should be 100% success rate
				break;
			case RK_RUNEMASTERY: {
					int A = 100 * (51 + 2 * pc_checkskill(sd, skill_id));
					int B = 100 * status->dex / 30 + 10 * (status->luk + sd->status.job_level);
					int C = 100 * cap_value(sd->itemid,0,100); //itemid depend on makerune()
					int D = 0;

					switch( nameid ) { //Rune rank it_diff 9 craftable rune
						case ITEMID_BERKANA:
						case ITEMID_LUX_ANIMA:
							D = -2000;
							break; //Rank S
						case ITEMID_NAUTHIZ:
						case ITEMID_URUZ:
							D = -1500;
							break; //Rank A
						case ITEMID_ISA:
						case ITEMID_PERTHRO:
							D = -1000;
							break; //Rank B
						case ITEMID_RAIDO:
						case ITEMID_THURISAZ:
						case ITEMID_HAGALAZ:
						case ITEMID_EIHWAZ:
							D = -500;
							break; //Rank C
						default: D = -1500;
							break; //not specified =-15%
					}
					make_per = A + B + C + D;
				}
				break;
			case GC_CREATENEWPOISON:
				make_per = 3000 + 500 * pc_checkskill(sd,GC_RESEARCHNEWPOISON)
					+ status->dex / 3 * 10 + status->luk * 10 + sd->status.job_level * 10; //Success increase from DEX, LUK, and job level
				qty = rnd_value((3 + pc_checkskill(sd,GC_RESEARCHNEWPOISON)) / 2, (8 + pc_checkskill(sd,GC_RESEARCHNEWPOISON)) / 2);
				break;
			case GN_CHANGEMATERIAL:
				for( i = 0; i < MAX_SKILL_CHANGEMATERIAL_DB; i++ ) {
					if( skill_changematerial_db[i].nameid == nameid && skill_changematerial_db[i].unique_id == unique_id ) {
						make_per = skill_changematerial_db[i].rate * 10;
						break;
					}
				}
				break;
			case GN_S_PHARMACY: {
					int difficulty = (620 - 20 * skill_lv); //(620 - 20 * Skill Level)

					//(Caster's INT) + (Caster's DEX / 2) + (Caster's LUK) + (Caster's Job Level) + Random number between (30 ~ 150) +
					//(Caster's Base Level - 100) + (Potion Research x 5) + (Full Chemical Protection Skill Level) x (Random number between 4 ~ 10)
					make_per = status->int_ + status->dex / 2 + status->luk + sd->status.job_level + (30 + rnd()%120) +
						(sd->status.base_level - 100) + pc_checkskill(sd,AM_LEARNINGPOTION) + pc_checkskill(sd,CR_FULLPROTECTION) * (4 + rnd()%6);
					switch( nameid ) { //Difficulty factor
						case ITEMID_HP_INCREASE_POTION_SMALL:
						case ITEMID_SP_INCREASE_POTION_SMALL:
						case ITEMID_ENRICH_WHITE_POTION_Z:
							difficulty += 10;
							break;
						case ITEMID_BOMB_MUSHROOM_SPORE:
						case ITEMID_SP_INCREASE_POTION_MEDIUM:
							difficulty += 15;
							break;
						case ITEMID_BANANA_BOMB:
						case ITEMID_HP_INCREASE_POTION_MEDIUM:
						case ITEMID_SP_INCREASE_POTION_LARGE:
						case ITEMID_VITATA500:
							difficulty += 20;
							break;
						case ITEMID_SEED_OF_HORNY_PLANT:
						case ITEMID_BLOODSUCK_PLANT_SEED:
						case ITEMID_ENRICH_CELERMINE_JUICE:
							difficulty += 30;
							break;
						case ITEMID_HP_INCREASE_POTION_LARGE:
						case ITEMID_CURE_FREE:
							difficulty += 40;
							break;
					}
					if( make_per >= 400 && make_per > difficulty )
						qty = 10;
					else if( make_per >= 300 && make_per > difficulty )
						qty = 7;
					else if( make_per >= 100 && make_per > difficulty )
						qty = 6;
					else if( make_per >= 1 && make_per > difficulty )
						qty = 5;
					else
						qty = 4;
					make_per = 10000;
				}
				break;
			case GN_MAKEBOMB:
			case GN_MIX_COOKING:
				{ //(Caster's Job Level / 4) + (Caster's LUK / 2) + (Caster's DEX / 3)
					int difficulty = 30 + rnd()%120; //Random number between (30 ~ 150)

					make_per = sd->status.job_level / 4 + status->luk / 2 + status->dex / 3;
					qty = ~(5 + rnd()%5) + 1;
					switch( nameid ) { //Difficulty factor
						case ITEMID_APPLE_BOMB:
							difficulty += 5;
							break;
						case ITEMID_COCONUT_BOMB:
						case ITEMID_MELON_BOMB:
							difficulty += 10;
							break;
						case ITEMID_SAVAGE_FULL_ROAST:
						case ITEMID_COCKTAIL_WARG_BLOOD:
						case ITEMID_MINOR_STEW:
						case ITEMID_SIROMA_ICED_TEA:
						case ITEMID_DROSERA_HERB_SALAD:
						case ITEMID_PETITE_TAIL_NOODLES:
						case ITEMID_PINEAPPLE_BOMB:
							difficulty += 15;
							break;
						case ITEMID_BANANA_BOMB:
							difficulty += 20;
							break;
					}
					if( make_per >= 30 && make_per > difficulty )
						qty = 10 + rnd()%2;
					else if( make_per >= 10 && make_per > difficulty )
						qty = 10;
					else if( make_per == 10 && make_per > difficulty )
						qty = 8;
					else if( (make_per >= 50 || make_per < 30) && make_per < difficulty )
						; //Food/Bomb creation fails
					else if(make_per >= 30 && make_per < difficulty)
						qty = 5;
					if( qty < 0 || (skill_lv == 1 && make_per < difficulty) ) {
						qty = ~qty + 1;
						make_per = 0;
					} else
						make_per = 10000;
					qty = (skill_lv > 1 ? qty : 1);
				}
				break;
			default:
				if( sd->menuskill_id ==	AM_PHARMACY ) { //Assume Cooking Dish
					int base_chance = 0;

					if( sd->menuskill_val == 30 ) //Combination Kit
						base_chance = (status_get_lv(&sd->bl) < 20 ? 8000 : 1200);
					else if( sd->menuskill_val > 10 && sd->menuskill_val <= 20 ) {
						if( sd->menuskill_val >= 15 ) //Legendary Cooking Set
							base_chance = 10000; //100% Success
						else
							base_chance = 1200 * (sd->menuskill_val - 10);
					}
					if( base_chance != 10000 ) {
						make_per = base_chance
							+ 20  * (sd->status.base_level + 1)
							+ 20  * (status->dex + 1)
							+ 100 * (rnd()%(30 + 5 * (sd->cook_mastery / 400) - (6 + sd->cook_mastery / 80)) + (6 + sd->cook_mastery / 80))
							- 400 * (skill_produce_db[idx].itemlv - 11 + 1)
							- 10  * (100 - status->luk + 1)
							- 500 * (num - 1)
							- 100 * (rnd()%4 + 1);
					}
				} else
					make_per = 5000;
				break;
		}
	} else { //Weapon Forging - skill bonuses are straight from kRO website, other things from a jRO calculator [DracoRPG]
		make_per = 5000 + ((sd->class_&JOBL_THIRD) ? 1400 : sd->status.job_level * 20) + status->dex * 10 + status->luk * 10; //Base
		make_per += pc_checkskill(sd,skill_id) * 500; //Smithing skills bonus: +5/+10/+15
		make_per += pc_checkskill(sd,BS_WEAPONRESEARCH) * 100 + ((wlv >= 3) ? pc_checkskill(sd,BS_ORIDEOCON) * 100 : 0); //Weaponry Research bonus: +1/+2/+3/+4/+5/+6/+7/+8/+9/+10, Oridecon Research bonus (custom): +1/+2/+3/+4/+5
		make_per -= (ele ? 2000 : 0) + sc * 1500 + (wlv > 1 ? wlv * 1000 : 0); //Element Stone: -20%, Star Crumb: -15% each, Weapon level malus: -0/-20/-30
		if( pc_search_inventory(sd,ITEMID_EMPERIUM_ANVIL) != INDEX_NOT_FOUND )
			make_per += 1000; //+10
		else if( pc_search_inventory(sd,ITEMID_GOLDEN_ANVIL) != INDEX_NOT_FOUND )
			make_per += 500; //+5
		else if( pc_search_inventory(sd,ITEMID_ORIDECON_ANVIL) != INDEX_NOT_FOUND )
			make_per += 300; //+3
		else if( pc_search_inventory(sd,ITEMID_ANVIL) != INDEX_NOT_FOUND )
			make_per += 0; //+0?
		if( battle_config.wp_rate != 100 )
			make_per = make_per * battle_config.wp_rate / 100;
	}

	if( sd->class_&JOBL_BABY ) //If it's a Baby Class
		make_per = make_per * 50 / 100; //Baby penalty is 50% (bugreport:4847)

	if( make_per < 1 )
		make_per = 1;

	if( qty > 1 || rnd()%10000 < make_per ) { //Success, or crafting multiple items
		struct item tmp_item;

		memset(&tmp_item,0,sizeof(tmp_item));
		tmp_item.nameid = nameid;
		tmp_item.amount = 1;
		tmp_item.identify = 1;
		if( equip ) {
			tmp_item.card[0] = CARD0_FORGE;
			tmp_item.card[1] = ((sc * 5)<<8) + ele;
			tmp_item.card[2] = GetWord(sd->status.char_id,0); //CharId
			tmp_item.card[3] = GetWord(sd->status.char_id,1);
		} else { //Flag is only used on the end, so it can be used here [Skotlex]
			switch( skill_id ) {
				case BS_DAGGER:
				case BS_SWORD:
				case BS_TWOHANDSWORD:
				case BS_AXE:
				case BS_MACE:
				case BS_KNUCKLE:
				case BS_SPEAR:
					flag = battle_config.produce_item_name_input&0x1;
					break;
				case AM_PHARMACY:
				case AM_TWILIGHT1:
				case AM_TWILIGHT2:
				case AM_TWILIGHT3:
					flag = battle_config.produce_item_name_input&0x2;
					break;
				case AL_HOLYWATER:
				case AB_ANCILLA:
					flag = battle_config.produce_item_name_input&0x8;
					break;
				case ASC_CDP:
					flag = battle_config.produce_item_name_input&0x10;
					break;
				default:
					flag = battle_config.produce_item_name_input&0x80;
					break;
			}
			if( flag ) {
				tmp_item.card[0] = CARD0_CREATE;
				tmp_item.card[1] = 0;
				tmp_item.card[2] = GetWord(sd->status.char_id,0); //CharId
				tmp_item.card[3] = GetWord(sd->status.char_id,1);
			}
		}

		//if(log_config.produce > 0)
			//log_produce(sd,nameid,slot1,slot2,slot3,1);
		//@TODO: Update PICKLOG

		if( equip ) {
			clif_produceeffect(sd,0,nameid);
			clif_misceffect(&sd->bl,3);
			if( itemdb_wlv(nameid) >= 3 && ((ele? 1 : 0) + sc) >= 3 ) //Fame point system [DracoRPG]
				pc_addfame(sd,battle_config.fame_forge); //Success to forge a lv3 weapon with 3 additional ingredients = +10 fame point
		} else {
			int fame = 0;

			tmp_item.amount = 0;
			for( i = 0; i < qty; i++ ) { //Apply quantity modifiers
				if( (skill_id == GN_MIX_COOKING || skill_id == GN_MAKEBOMB || skill_id == GN_S_PHARMACY) && make_per > 1 ) {
					tmp_item.amount = qty;
					break;
				}
				if( qty == 1 || rnd()%10000 < make_per ) { //Success
					tmp_item.amount++;
					if( nameid < ITEMID_RED_SLIM_POTION || nameid > ITEMID_WHITE_SLIM_POTION )
						continue;
					if( skill_id != AM_PHARMACY &&
						skill_id != AM_TWILIGHT1 &&
						skill_id != AM_TWILIGHT2 &&
						skill_id != AM_TWILIGHT3 )
						continue;
					switch( ++sd->potion_success_counter ) { //Add fame as needed
						case 3:
							fame += battle_config.fame_pharmacy_3; //Success to prepare 3 Condensed Potions in a row
							break;
						case 5:
							fame += battle_config.fame_pharmacy_5; //Success to prepare 5 Condensed Potions in a row
							break;
						case 7:
							fame += battle_config.fame_pharmacy_7; //Success to prepare 7 Condensed Potions in a row
							break;
						case 10:
							fame += battle_config.fame_pharmacy_10; //Success to prepare 10 Condensed Potions in a row
							sd->potion_success_counter = 0;
							break;
					}
				} else //Failure
					sd->potion_success_counter = 0;
			}
			if( fame )
				pc_addfame(sd,fame);
			switch( skill_id ) { //Visual effects and the like
				case AL_HOLYWATER:
				case AB_ANCILLA:
					break;
				case AM_PHARMACY:
				case AM_TWILIGHT1:
				case AM_TWILIGHT2:
				case AM_TWILIGHT3:
				case ASC_CDP:
				case GC_CREATENEWPOISON:
					clif_produceeffect(sd,2,nameid);
					clif_misceffect(&sd->bl,5);
					break;
				case BS_IRON:
				case BS_STEEL:
				case BS_ENCHANTEDSTONE:
					clif_produceeffect(sd,0,nameid);
					clif_misceffect(&sd->bl,3);
					break;
				case RK_RUNEMASTERY:
					clif_produceeffect(sd,4,nameid);
					clif_misceffect(&sd->bl,5);
					break;
				default: //Cooking items
					if( skill_produce_db[idx].itemlv > 10 && skill_produce_db[idx].itemlv <= 20 && sd->cook_mastery < 1999 )
						pc_setglobalreg(sd,COOKMASTERY_VAR,sd->cook_mastery + (1<<((skill_produce_db[idx].itemlv - 11) / 2)) * 5);
					clif_specialeffect(&sd->bl,EF_COOKING_OK,AREA);
					break;
			}
		}
		if( skill_id == GN_CHANGEMATERIAL && tmp_item.amount ) { //Success
			int j, k = 0;

			for( i = 0; i < MAX_SKILL_CHANGEMATERIAL_DB; i++ ) {
				if( skill_changematerial_db[i].nameid == nameid && skill_changematerial_db[i].unique_id == unique_id ) {
					for( j = 0; j < MAX_SKILL_CHANGEMATERIAL_SET; j++ ) {
						if( rnd()%1000 < skill_changematerial_db[i].qty_rate[j] ) {
							int val, amt;

							amt = tmp_item.amount = qty * skill_changematerial_db[i].qty[j];
							if( !itemdb_isstackable(nameid) )
								amt = 1;
							for( val = 0; val < tmp_item.amount; val += amt ) {
								if( (flag = pc_additem(sd,&tmp_item,amt,LOG_TYPE_PRODUCE)) ) {
									clif_additem(sd,0,0,flag);
									map_addflooritem(&tmp_item,amt,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0,0,false);
								}
							}
							k++;
						}
					}
					break;
				}
			}
			if( k ) {
				clif_produceeffect(sd,6,nameid);
				clif_misceffect(&sd->bl,5);
				clif_msg_skill(sd,skill_id,ITEM_PRODUCE_SUCCESS);
				return true;
			}
		} else if( tmp_item.amount ) { //Success
			if( (flag = pc_additem(sd,&tmp_item,tmp_item.amount,LOG_TYPE_PRODUCE)) ) {
				clif_additem(sd,0,0,flag);
				map_addflooritem(&tmp_item,tmp_item.amount,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0,0,false);
			}
			if( skill_id == GN_MIX_COOKING || skill_id == GN_MAKEBOMB || skill_id ==  GN_S_PHARMACY ) {
				clif_produceeffect(sd,6,nameid);
				clif_misceffect(&sd->bl,5);
				clif_msg_skill(sd,skill_id,ITEM_PRODUCE_SUCCESS);
			}
			return true;
		}
	}

	//Failure
	//if(log_config.produce)
		//log_produce(sd,nameid,slot1,slot2,slot3,0);
	//@TODO: Update PICKLOG

	if( equip ) {
		clif_produceeffect(sd,1,nameid);
		clif_misceffect(&sd->bl,2);
	} else {
		switch( skill_id ) {
			case ASC_CDP: //25% Damage yourself, and display same effect as failed potion
				status_percent_damage(NULL,&sd->bl,-25,0,true);
			//Fall through
			case AM_PHARMACY:
			case AM_TWILIGHT1:
			case AM_TWILIGHT2:
			case AM_TWILIGHT3:
			case GC_CREATENEWPOISON:
				clif_produceeffect(sd,3,nameid);
				clif_misceffect(&sd->bl,6);
				sd->potion_success_counter = 0; //Fame point system [DracoRPG]
				break;
			case BS_IRON:
			case BS_STEEL:
			case BS_ENCHANTEDSTONE:
				clif_produceeffect(sd,1,nameid);
				clif_misceffect(&sd->bl,2);
				break;
			case RK_RUNEMASTERY:
				clif_produceeffect(sd,5,nameid);
				clif_misceffect(&sd->bl,6);
				break;
			case GN_MIX_COOKING: {
					struct item tmp_item;
					const int compensation[5] = { ITEMID_BLACK_LUMP,ITEMID_BLACK_HARD_LUMP,ITEMID_VERY_HARD_LUMP,ITEMID_BLACK_MASS,ITEMID_MYSTERIOUS_POWDER };
					int rate = rnd()%500;

					memset(&tmp_item,0,sizeof(tmp_item));
					if( rate < 50 )
						i = 4;
					else if( rate < 100 )
						i = 2 + rnd()%1;
					else if( rate < 250 )
						i = 1;
					else if( rate < 500 )
						i = 0;
					tmp_item.nameid = compensation[i];
					tmp_item.amount = qty;
					tmp_item.identify = 1;
					if( (flag = pc_additem(sd,&tmp_item,tmp_item.amount,LOG_TYPE_PRODUCE)) ) {
						clif_additem(sd,0,0,flag);
						map_addflooritem(&tmp_item,tmp_item.amount,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0,0,false);
					}
					clif_produceeffect(sd,7,nameid);
					clif_misceffect(&sd->bl,6);
					clif_msg_skill(sd,skill_id,ITEM_PRODUCE_FAIL);
				}
				break;
			case GN_MAKEBOMB:
			case GN_S_PHARMACY:
			case GN_CHANGEMATERIAL:
				clif_produceeffect(sd,7,nameid);
				clif_misceffect(&sd->bl,6);
				clif_msg_skill(sd,skill_id,ITEM_PRODUCE_FAIL);
				break;
			default: //Cooking items
				if( skill_produce_db[idx].itemlv > 10 && skill_produce_db[idx].itemlv <= 20 && sd->cook_mastery > 0 )
					pc_setglobalreg(sd,COOKMASTERY_VAR,sd->cook_mastery - (1<<((skill_produce_db[idx].itemlv - 11) / 2)) - (((1<<((skill_produce_db[idx].itemlv - 11) / 2))>>1) * 3));
				clif_specialeffect(&sd->bl,EF_COOKING_FAIL,AREA);
				break;
		}
	}
	return false;
}

/**
 * Attempt to create arrow by specified material
 * @param sd Player
 * @param nameid Item ID of material
 * @return True if created, False is failed
 */
bool skill_arrow_create(struct map_session_data *sd, unsigned short nameid)
{
	short i, j, idx = -1;
	struct item tmp_item;

	nullpo_ret(sd);

	if( !nameid || !itemdb_exists(nameid) )
		return false;

	for( i = 0; i < MAX_SKILL_ARROW_DB; i++ ) {
		if( nameid == skill_arrow_db[i].nameid ) {
			idx = i;
			break;
		}
	}

	if( !idx || (j = pc_search_inventory(sd,nameid)) == INDEX_NOT_FOUND )
		return false;

	pc_delitem(sd,j,1,0,0,LOG_TYPE_PRODUCE);
	for( i = 0; i < MAX_ARROW_RESULT; i++ ) {
		char flag = 0;

		if( !skill_arrow_db[idx].cre_id[i] || !itemdb_exists(skill_arrow_db[idx].cre_id[i]) || !skill_arrow_db[idx].cre_amount[i] )
			continue;
		memset(&tmp_item,0,sizeof(tmp_item));
		tmp_item.identify = 1;
		tmp_item.nameid = skill_arrow_db[idx].cre_id[i];
		tmp_item.amount = skill_arrow_db[idx].cre_amount[i];
		if( battle_config.produce_item_name_input&0x4 ) {
			tmp_item.card[0] = CARD0_CREATE;
			tmp_item.card[1] = 0;
			tmp_item.card[2] = GetWord(sd->status.char_id,0); //CharId
			tmp_item.card[3] = GetWord(sd->status.char_id,1);
		}
		if( (flag = pc_additem(sd,&tmp_item,tmp_item.amount,LOG_TYPE_PRODUCE)) ) {
			clif_additem(sd,0,0,flag);
			map_addflooritem(&tmp_item,tmp_item.amount,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0,0,false);
		}
	}
	return true;
}

/**
 * Enchant weapon with poison
 * @param sd Player
 * @nameid Item ID of poison type
 */
int skill_poisoningweapon(struct map_session_data *sd, unsigned short nameid) {
	sc_type type;
	int chance, i;
	char output[128];
	const char *msg;

	nullpo_ret(sd);

	if( !nameid || (i = pc_search_inventory(sd,nameid)) == INDEX_NOT_FOUND || pc_delitem(sd,i,1,0,0,LOG_TYPE_CONSUME) ) {
		clif_skill_fail(sd,GC_POISONINGWEAPON,USESKILL_FAIL_LEVEL,0,0);
		return 0;
	}
	switch( nameid ) { //t_lv used to take duration from skill_get_time2
		case ITEMID_PARALYSE:      type = SC_PARALYSE;      msg = "Paralyze";       break;
		case ITEMID_PYREXIA:       type = SC_PYREXIA;       msg = "Pyrexia";        break;
		case ITEMID_DEATHHURT:     type = SC_DEATHHURT;     msg = "Deathhurt";      break;
		case ITEMID_LEECHESEND:    type = SC_LEECHESEND;    msg = "Leech End";      break;
		case ITEMID_VENOMBLEED:    type = SC_VENOMBLEED;    msg = "Venom Bleed";    break;
		case ITEMID_TOXIN:         type = SC_TOXIN;         msg = "Toxin";          break;
		case ITEMID_MAGICMUSHROOM: type = SC_MAGICMUSHROOM; msg = "Magic Mushroom"; break;
		case ITEMID_OBLIVIONCURSE: type = SC_OBLIVIONCURSE; msg = "Oblivion Curse"; break;
		default:
			clif_skill_fail(sd,GC_POISONINGWEAPON,USESKILL_FAIL_LEVEL,0,0);
			return 0;
	}
	//Status must be forced to end so that a new poison will be applied if a player decides to change poisons [Rytech]
	status_change_end(&sd->bl,SC_POISONINGWEAPON,INVALID_TIMER);
	chance = 2 + 2 * sd->menuskill_val; //2 + 2 * skill_lv
	//In Aegis it store the level of GC_RESEARCHNEWPOISON in val1
	if( sc_start4(&sd->bl,&sd->bl,SC_POISONINGWEAPON,100,pc_checkskill(sd,GC_RESEARCHNEWPOISON),type,chance,0,skill_get_time(GC_POISONINGWEAPON,sd->menuskill_val)) ) {
		sprintf(output,"[%s] Poison effect was applied to the weapon.",msg);
		clif_messagecolor(&sd->bl,color_table[COLOR_WHITE],output,false,SELF);
	}
	return 0;
}

void skill_toggle_magicpower(struct block_list *bl, uint16 skill_id)
{
	struct status_change *sc = status_get_sc(bl);

	//Non-offensive and non-magic skills do not affect the status
	if (skill_get_nk(skill_id)&NK_NO_DAMAGE || !(skill_get_type(skill_id)&BF_MAGIC))
		return;

	//SC_MAGICPOWER needs to switch states before any damage is actually dealt
	if (sc && sc->count && sc->data[SC_MAGICPOWER]) {
		if (sc->data[SC_MAGICPOWER]->val4)
			status_change_end(bl, SC_MAGICPOWER, INVALID_TIMER);
		else {
			sc->data[SC_MAGICPOWER]->val4 = 1;
			status_calc_bl(bl, status_sc2scb_flag(SC_MAGICPOWER));
#ifndef RENEWAL
			if (bl->type == BL_PC) { //Update current display
				clif_updatestatus(((TBL_PC *)bl),SP_MATK1);
				clif_updatestatus(((TBL_PC *)bl),SP_MATK2);
			}
#endif
		}
	}
}

int skill_magicdecoy(struct map_session_data *sd, unsigned short nameid) {
	int x, y, i;
	short mob_id = 0;
	uint16 skill_lv;
	struct mob_data *md = NULL;
	struct unit_data *ud =  NULL;

	nullpo_ret(sd);

	skill_lv = sd->menuskill_val;
	if( !nameid || !itemdb_is_elementpoint(nameid) || (i = pc_search_inventory(sd,nameid)) == INDEX_NOT_FOUND ||
		!skill_lv || pc_delitem(sd,i,2,0,0,LOG_TYPE_CONSUME) ) {
		clif_skill_fail(sd,NC_MAGICDECOY,USESKILL_FAIL_LEVEL,0,0);
		return 0;
	}

	//Spawn Position
	x = sd->sc.pos_x;
	y = sd->sc.pos_y;

	//Item picked decides the mob class
	switch( nameid ) {
		case ITEMID_SCARLET_PTS:
			mob_id = MOBID_MAGICDECOY_FIRE;
			break;
		case ITEMID_INDIGO_PTS:
			mob_id = MOBID_MAGICDECOY_WATER;
			break;
		case ITEMID_YELLOW_WISH_PTS:
			mob_id = MOBID_MAGICDECOY_WIND;
			break;
		default:
			mob_id = MOBID_MAGICDECOY_EARTH;
			break;
	}

	if( (ud = unit_bl2ud(&sd->bl)) ) { //Need to reset, since the values had been cleared before the selection process is done
		ud->skill_id = NC_MAGICDECOY;
		ud->skill_lv = skill_lv;
	}

	md = mob_once_spawn_sub(&sd->bl,sd->bl.m,x,y,sd->status.name,mob_id,"",SZ_SMALL,AI_NONE);
	if( md ) {
		md->master_id = sd->bl.id;
		md->special_state.ai = AI_FAW;
		if( md->deletetimer != INVALID_TIMER )
			delete_timer(md->deletetimer,mob_timer_delete);
		md->deletetimer = add_timer(gettick() + skill_get_time(NC_MAGICDECOY,skill_lv),mob_timer_delete,md->bl.id,0);
		mob_spawn(md);
	}

	sd->sc.pos_x = sd->sc.pos_y = ud->skill_id = ud->skill_lv = 0; //Clear them
	return 0;
}

//Warlock spell books [LimitLine]
void skill_spellbook(struct map_session_data *sd, unsigned short nameid) {
	int i, max_preserve, skill_id, point;
	struct status_change *sc;

	nullpo_retv(sd);

	sc = status_get_sc(&sd->bl);
	status_change_end(&sd->bl, SC_STOP, INVALID_TIMER);

	for( i = SC_SPELLBOOK1; i <= SC_MAXSPELLBOOK; i++ ) {
		if( !(sc && sc->data[i]) )
			break;
	}

	if( i > SC_MAXSPELLBOOK ) {
		clif_skill_fail(sd, WL_READING_SB, USESKILL_FAIL_SPELLBOOK_READING, 0, 0);
		return;
	}

	ARR_FIND(0, MAX_SKILL_SPELLBOOK_DB, i, skill_spellbook_db[i].nameid == nameid); //Search for information of this item
	if( i == MAX_SKILL_SPELLBOOK_DB )
		return;

	if( !pc_checkskill(sd, (skill_id = skill_spellbook_db[i].skill_id)) ) { //User doesn't learn the skill
		sc_start(&sd->bl, &sd->bl, SC_SLEEP, 100, 1, skill_get_time(WL_READING_SB, pc_checkskill(sd, WL_READING_SB)));
		clif_skill_fail(sd, WL_READING_SB, USESKILL_FAIL_SPELLBOOK_DIFFICULT_SLEEP, 0, 0);
		return;
	}

	max_preserve = 4 * pc_checkskill(sd, WL_FREEZE_SP) + status_get_int(&sd->bl) / 10 + sd->status.base_level / 10;
	point = skill_spellbook_db[i].point;

	//This is how official seals the spells [malufett]
	if( sc && sc->data[SC_FREEZE_SP] ) {
		if( (sc->data[SC_FREEZE_SP]->val2 + point) > max_preserve ) {
			clif_skill_fail(sd, WL_READING_SB, USESKILL_FAIL_SPELLBOOK_PRESERVATION_POINT, 0, 0);
			return;
		}
		for( i = SC_SPELLBOOK1; i <= SC_MAXSPELLBOOK; i++ ) {
			if( !sc->data[i] ) {
				sc->data[SC_FREEZE_SP]->val2 += point; //Increase points
				sc_start4(&sd->bl, &sd->bl, (sc_type)i, 100, skill_id, pc_checkskill(sd, skill_id), point, 0, -1);
				break;
			}
		}
	} else {
		sc_start2(&sd->bl, &sd->bl, SC_FREEZE_SP, 100, 0, point, -1);
		sc_start4(&sd->bl, &sd->bl, SC_MAXSPELLBOOK, 100, skill_id, pc_checkskill(sd, skill_id), point, 0, -1);
	}
}

void skill_select_menu(struct map_session_data *sd, uint16 skill_id) {
	uint16 autoshadowlv, autocastid, autocastlv;

	nullpo_retv(sd);

	if( sd->sc.count && sd->sc.data[SC_STOP] ) {
		autoshadowlv = sd->sc.data[SC_STOP]->val1;
		status_change_end(&sd->bl, SC_STOP, INVALID_TIMER);
	} else
		autoshadowlv = 10; //Safety

	//First check to see if skill is a copied skill to protect against forged packets
	//Then check to see if its a skill that can be autocasted through auto shadow spell
	if( (skill_id != sd->status.skill[sd->cloneskill_idx].id && skill_id != sd->status.skill[sd->reproduceskill_idx].id) ||
		!(skill_get_inf3(sd->status.skill[skill_id].id)&INF3_AUTOSHADOWSPELL) ) {
		clif_skill_fail(sd, SC_AUTOSHADOWSPELL, USESKILL_FAIL_LEVEL, 0, 0);
		return;
	}

	autocastid = skill_id; //The skill that will be autocasted
	autocastlv = (autoshadowlv + 5) / 2; //The level the skill will be autocasted
	autocastlv = min(autocastlv, skill_get_max(skill_id)); //Don't allow autocasting level's higher then the max possible for players
	sc_start4(&sd->bl, &sd->bl, SC__AUTOSHADOWSPELL, 100, autoshadowlv, autocastid, autocastlv, 0, skill_get_time(SC_AUTOSHADOWSPELL, autoshadowlv));
}

int skill_elementalanalysis(struct map_session_data *sd, int n, uint16 skill_lv, unsigned short *item_list) {
	int i;

	nullpo_ret(sd);
	nullpo_ret(item_list);

	if( n <= 0 )
		return 1;

	for( i = 0; i < n; i++ ) {
		unsigned short nameid;
		int add_amount, del_amount, idx, product;
		struct item tmp_item;

		idx = item_list[i * 2 + 0] - 2;
		del_amount = item_list[i * 2 + 1];

		if( skill_lv == 2 )
			del_amount -= (del_amount % 10);
		add_amount = (skill_lv == 1) ? del_amount * (5 + rnd()%5) : del_amount / 10 ;

		if( (nameid = sd->inventory.u.items_inventory[idx].nameid) <= 0 || del_amount > sd->inventory.u.items_inventory[idx].amount ) {
			clif_skill_fail(sd,SO_EL_ANALYSIS,USESKILL_FAIL_LEVEL,0,0);
			return 1;
		}

		switch( nameid ) {
			//Level 1
			case ITEMID_FLAME_HEART:   product = ITEMID_BLOODY_RED;      break;
			case ITEMID_MISTIC_FROZEN: product = ITEMID_CRYSTAL_BLUE;    break;
			case ITEMID_ROUGH_WIND:    product = ITEMID_WIND_OF_VERDURE; break;
			case ITEMID_GREAT_NATURE:  product = ITEMID_YELLOW_LIVE;     break;
			//Level 2
			case ITEMID_BLOODY_RED:      product = ITEMID_FLAME_HEART;   break;
			case ITEMID_CRYSTAL_BLUE:    product = ITEMID_MISTIC_FROZEN; break;
			case ITEMID_WIND_OF_VERDURE: product = ITEMID_ROUGH_WIND;    break;
			case ITEMID_YELLOW_LIVE:     product = ITEMID_GREAT_NATURE;  break;
			default:
				clif_skill_fail(sd,SO_EL_ANALYSIS,USESKILL_FAIL_LEVEL,0,0);
				return 1;
		}

		if( pc_delitem(sd,idx,del_amount,0,1,LOG_TYPE_CONSUME) ) {
			clif_skill_fail(sd,SO_EL_ANALYSIS,USESKILL_FAIL_LEVEL,0,0);
			return 1;
		}

		if( skill_lv == 2 && rnd()%100 < 25 ) {	//At level 2 have a fail chance, you lose your items if it fails
			clif_skill_fail(sd,SO_EL_ANALYSIS,USESKILL_FAIL_LEVEL,0,0);
			return 1;
		}

		memset(&tmp_item,0,sizeof(tmp_item));
		tmp_item.nameid = product;
		tmp_item.amount = add_amount;
		tmp_item.identify = 1;
		if( tmp_item.amount ) {
			unsigned char flag = pc_additem(sd,&tmp_item,tmp_item.amount,LOG_TYPE_CONSUME);

			if( flag ) {
				clif_additem(sd,0,0,flag);
				map_addflooritem(&tmp_item,tmp_item.amount,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0,0,false);
			}
		}

	}
	return 0;
}

int skill_changematerial(struct map_session_data *sd, int n, unsigned short *item_list) {
	int i, j, k, c, p = 0, amount;
	unsigned short nameid;

	nullpo_ret(sd);
	nullpo_ret(item_list);

	//Search for objects that can be created
	for( i = 0; i < MAX_SKILL_PRODUCE_DB; i++ ) {
		if( skill_produce_db[i].itemlv == 26 ) {
			p = 0;
			do {
				c = 0;
				//Verification of overlap between the objects required and the list submitted
				for( j = 0; j < MAX_PRODUCE_RESOURCE; j++ ) {
					if( skill_produce_db[i].mat_id[j] > 0 ) {
						for( k = 0; k < n; k++ ) {
							int idx = item_list[k * 2 + 0] - 2;

							nameid = sd->inventory.u.items_inventory[idx].nameid;
							amount = item_list[k * 2 + 1];
							if( nameid > 0 && sd->inventory.u.items_inventory[idx].identify == 0 ) {
								clif_msg_skill(sd,GN_CHANGEMATERIAL,ITEM_UNIDENTIFIED);
								return 0;
							}
							if( nameid == skill_produce_db[i].mat_id[j] && (amount - p * skill_produce_db[i].mat_amount[j]) >= skill_produce_db[i].mat_amount[j] &&
								(amount - p * skill_produce_db[i].mat_amount[j])%skill_produce_db[i].mat_amount[j] == 0 ) //Must be in exact amount
								c++; //Match
						}
					} else
						break; //No more items required
				}
				p++;
			} while( n == j && c == n );
			p--;
			if( p > 0 ) {
				skill_produce_mix(sd,GN_CHANGEMATERIAL,skill_produce_db[i].nameid,skill_produce_db[i].unique_id,0,0,0,p);
				return 1;
			}
		}
	}

	if( p == 0 )
		clif_msg_skill(sd,GN_CHANGEMATERIAL,ITEM_CANT_COMBINE);
	return 0;
}

/**
 * For Royal Guard's LG_TRAMPLE
 */
static int skill_destroy_trap(struct block_list *bl, va_list ap) {
	struct skill_unit *unit = (struct skill_unit *)bl;
	struct skill_unit_group *group = NULL;
	struct block_list *src = va_arg(ap, struct block_list *);
	struct status_data *sstatus = status_get_status_data(src);
	unsigned int tick = va_arg(ap, unsigned int);

	nullpo_ret(unit);

	if( unit->alive && (group = unit->group) && skill_get_inf2(group->skill_id)&INF2_TRAP ) {
		int rate = (sstatus->dex + sstatus->agi) / 4 + group->skill_lv * 5; //Avoid splash damage rate
		uint16 skill_id = group->skill_id;
		uint16 skill_lv = group->skill_lv;

		switch( group->unit_id ) {
			case UNT_CLAYMORETRAP:
			case UNT_FIRINGTRAP:
			case UNT_ICEBOUNDTRAP:
				if( rnd()%100 < rate )
					break;
				if( skill_get_nk(skill_id)&NK_SPLASHSPLIT )
					skill_area_temp[0] = map_foreachinrange(skill_area_sub, bl, skill_get_splash(skill_id, skill_lv), BL_CHAR, src, skill_id, skill_lv, tick, BCT_ENEMY, skill_area_sub_count);
				map_foreachinrange(skill_trap_splash, bl, skill_get_splash(skill_id, skill_lv), group->bl_flag|BL_SKILL|~BCT_SELF, bl, tick);
				break;
			case UNT_LANDMINE:
			case UNT_BLASTMINE:
			case UNT_SHOCKWAVE:
			case UNT_SANDMAN:
			case UNT_FLASHER:
			case UNT_FREEZINGTRAP:
			case UNT_CLUSTERBOMB:
				if( rnd()%100 < rate )
					break;
				if( skill_get_nk(skill_id)&NK_SPLASHSPLIT )
					skill_area_temp[0] = map_foreachinrange(skill_area_sub, bl, skill_get_splash(skill_id, skill_lv), BL_CHAR, src, skill_id, skill_lv, tick, BCT_ENEMY, skill_area_sub_count);
				map_foreachinrange(skill_trap_splash, bl, skill_get_splash(skill_id, skill_lv), group->bl_flag, bl, tick);
				break;
		}
		skill_delunit(unit); //Traps aren't recovered
	}
	return 0;
}

/**
 * Deals damage to the affected target if healed from one of the following skills:
 * AL_HEAL, PR_SANCTUARY, BA_APPLEIDUN, AB_RENOVATIO, AB_HIGHNESSHEAL, SO_WARMER
 */
int skill_akaitsuki_damage(struct block_list *src, struct block_list *bl, int damage, uint16 skill_id, uint16 skill_lv, unsigned int tick) {
	nullpo_ret(src);
	nullpo_ret(bl);

	damage = damage / 2; //Damage is half of the heal amount
	clif_skill_damage(src, bl, tick, status_get_amotion(src), status_get_dmotion(bl), damage, 1,
		(skill_id == AB_HIGHNESSHEAL ? AL_HEAL : skill_id), (skill_id == AB_HIGHNESSHEAL ? -1 : skill_lv), DMG_SKILL);
	status_zap(bl, damage, 0);
	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
int skill_blockpc_get(struct map_session_data *sd, uint16 skill_id) {
	int i;

	nullpo_retr(-1, sd);

	ARR_FIND(0, MAX_SKILLCOOLDOWN, i, sd->scd[i] && sd->scd[i]->skill_id == skill_id);
	return (i >= MAX_SKILLCOOLDOWN ? -1 : i);
}

TIMER_FUNC(skill_blockpc_end) {
	struct map_session_data *sd = map_id2sd(id);
	int i = (int)data;

	if (!sd || data < 0 || data >= MAX_SKILLCOOLDOWN)
		return 0;

	if (!sd->scd[i] || sd->scd[i]->timer != tid) {
		ShowWarning("skill_blockpc_end: Invalid Timer or not Skill Cooldown.\n");
		return 0;
	}

	aFree(sd->scd[i]);
	sd->scd[i] = NULL;
	return 1;
}

/**
 * flags a singular skill as being blocked from persistent usage.
 * @param   sd        the player the skill delay affects
 * @param   skill_id  the skill which should be delayed
 * @param   tick      the length of time the delay should last
 * @param   load      whether this assignment is being loaded upon player login
 * @return  1 if successful, 0 otherwise
 */
int skill_blockpc_start(struct map_session_data *sd, uint16 skill_id, int tick) {
	int i;

	nullpo_ret(sd);

	if (!skill_id || tick < 1)
		return 0;

	ARR_FIND(0, MAX_SKILLCOOLDOWN, i, sd->scd[i] && sd->scd[i]->skill_id == skill_id);
	if (i < MAX_SKILLCOOLDOWN) { //Skill already with cooldown
		delete_timer(sd->scd[i]->timer, skill_blockpc_end);
		aFree(sd->scd[i]);
		sd->scd[i] = NULL;
	}

	ARR_FIND(0, MAX_SKILLCOOLDOWN, i, !sd->scd[i]);
	if (i < MAX_SKILLCOOLDOWN) { //Free slot found
		CREATE(sd->scd[i], struct skill_cooldown_entry, 1);
		sd->scd[i]->skill_id = skill_id;
		sd->scd[i]->timer = add_timer(gettick() + tick, skill_blockpc_end, sd->bl.id, i);
		sd->scd[i]->duration = tick;
		if (battle_config.display_status_timers && tick)
			clif_skill_cooldown(sd, skill_id, tick);
		return 1;
	} else {
		ShowWarning("skill_blockpc_start: Too many skillcooldowns, increase MAX_SKILLCOOLDOWN.\n");
		return 0;
	}
}

int skill_blockpc_clear(struct map_session_data *sd) {
	int i;

	nullpo_ret(sd);

	for (i = 0; i < MAX_SKILLCOOLDOWN; i++) {
		if (!sd->scd[i])
			continue;
		delete_timer(sd->scd[i]->timer, skill_blockpc_end);
		aFree(sd->scd[i]);
		sd->scd[i] = NULL;
	}
	return 1;
}

TIMER_FUNC(skill_blockhomun_end) //[orn]
{
	struct homun_data *hd = (TBL_HOM *)map_id2bl(id);

	if (data <= 0 || data >= MAX_SKILL)
		return 0;

	if (hd)
		hd->blockskill[data] = 0;

	return 1;
}

int skill_blockhomun_start(struct homun_data *hd, uint16 skill_id, int tick) //[orn]
{
	uint16 idx = skill_get_index(skill_id);

	nullpo_retr(-1, hd);

	if (!idx)
		return -1;

	if (tick < 1) {
		hd->blockskill[idx] = 0;
		return -1;
	}

	hd->blockskill[idx] = 1;

	return add_timer(gettick() + tick, skill_blockhomun_end, hd->bl.id, idx);
}

TIMER_FUNC(skill_blockmerc_end) //[orn]
{
	struct mercenary_data *md = (TBL_MER *)map_id2bl(id);

	if( data <= 0 || data >= MAX_SKILL )
		return 0;

	if( md )
		md->blockskill[data] = 0;

	return 1;
}

int skill_blockmerc_start(struct mercenary_data *md, uint16 skill_id, int tick)
{
	uint16 idx = skill_get_index(skill_id);

	nullpo_retr(-1, md);

	if( !idx )
		return -1;

	if( tick < 1 ) {
		md->blockskill[idx] = 0;
		return -1;
	}

	md->blockskill[idx] = 1;

	return add_timer(gettick() + tick, skill_blockmerc_end, md->bl.id, idx);
}

/**
 * Adds a new skill unit entry for this player to recast after map load
 */
void skill_usave_add(struct map_session_data *sd, uint16 skill_id, uint16 skill_lv)
{
	struct skill_usave *sus = NULL;

	if( idb_exists(skillusave_db,sd->status.char_id) )
		idb_remove(skillusave_db,sd->status.char_id);

	CREATE(sus, struct skill_usave, 1);
	idb_put(skillusave_db, sd->status.char_id, sus);

	sus->skill_id = skill_id;
	sus->skill_lv = skill_lv;
}

void skill_usave_trigger(struct map_session_data *sd)
{
	struct skill_usave *sus = NULL;
	struct skill_unit_group *group = NULL;
	enum sc_type type = SC_NONE;
	uint16 skill_id;
	uint16 skill_lv;

	if( !(sus = idb_get(skillusave_db,sd->status.char_id)) )
		return;

	skill_id = sus->skill_id;
	skill_lv = sus->skill_lv;

	switch( skill_id ) {
		case NC_NEUTRALBARRIER:
			type = SC_NEUTRALBARRIER_MASTER;
			break;
		case NC_STEALTHFIELD:
			type = SC_STEALTHFIELD_MASTER;
			break;
		case LG_BANDING:
			type = SC_BANDING;
			break;
	}

	if( (group = skill_unitsetting(&sd->bl,skill_id,skill_lv,sd->bl.x,sd->bl.y,0)) && type != SC_NONE ) {
		if( skill_id == LG_BANDING )
			sc_start4(&sd->bl,&sd->bl,type,100,skill_lv,0,0,group->group_id,skill_get_time(skill_id,skill_lv));
		else
			sc_start2(&sd->bl,&sd->bl,type,100,skill_lv,group->group_id,skill_get_time(skill_id,skill_lv));
	}
	idb_remove(skillusave_db,sd->status.char_id);
}

/**
 *
 */
int skill_split_str(char *str, char **val, int num)
{
	int i;

	for( i = 0; i < num && str; i++ ) {
		val[i] = str;
		str = strchr(str,',');
		if( str )
			*str++ = 0;
	}

	return i;
}

/**
 * Split the string with ':' as separator and put each value for a skill_lv
 * if no more value found put the latest to fill the array
 * @param str : string to split
 * @param val : array of MAX_SKILL_LEVEL to put value into
 * @return 0:error, x:number of value assign (should be MAX_SKILL_LEVEL)
 */
int skill_split_atoi(char *str, int *val)
{
	int i, j, step = 1;

	for( i = 0; i < MAX_SKILL_LEVEL; i++ ) {
		if( !str )
			break;
		val[i] = atoi(str);
		str = strchr(str,':');
		if( str )
			*str++ = 0;
	}
	if( !i ) //No data found
		return 0;
	if( i == 1 ) { //Single value, have the whole range have the same value
		for( ; i < MAX_SKILL_LEVEL; i++ )
			val[i] = val[i - 1];
		return i;
	}
	//Check for linear change with increasing steps until we reach half of the data acquired
	for( step = 1; step <= i / 2; step++ ) {
		int diff = val[i - 1] - val[i - step - 1];

		for( j = i - 1; j >= step; j-- ) {
			if( (val[j] - val[j - step]) != diff )
				break;
		}
		if( j >= step ) //No match, try next step
			continue;
		for( ; i < MAX_SKILL_LEVEL; i++ ) { //Apply linear increase
			val[i] = val[i - step] + diff;
			if( val[i] < 1 && val[i - 1] >= 0 ) { //Check if we have switched from + to -, cap the decrease to 0 in said cases
				val[i] = 1;
				diff = 0;
				step = 1;
			}
		}
		return i;
	}
	//We can't figure this one out, just fill out the stuff with the previous value
	for( ; i < MAX_SKILL_LEVEL; i++ )
		val[i] = val[i - 1];
	return i;
}

/*
 *
 */
void skill_init_unit_layout (void)
{
	int i, j, pos = 0;

	memset(skill_unit_layout,0,sizeof(skill_unit_layout));

	//Standard square layouts go first
	for( i = 0; i <= MAX_SQUARE_LAYOUT; i++ ) {
		int size = i * 2 + 1;

		skill_unit_layout[i].count = size * size;
		for( j = 0; j < size * size; j++ ) {
			skill_unit_layout[i].dx[j] = (j%size - i);
			skill_unit_layout[i].dy[j] = (j / size - i);
		}
	}

	//Afterwards add special ones
	pos = i;
	for( i = 0; i < MAX_SKILL_DB; i++ ) {
		if( !skill_db[i].unit_id[0] || skill_db[i].unit_layout_type[0] != -1 )
			continue;
		if( i >= HM_SKILLRANGEMIN && i <= EL_SKILLRANGEMAX ) {
			int skill = i;

			if( i >= EL_SKILLRANGEMIN && i <= EL_SKILLRANGEMAX ) {
				skill -= EL_SKILLRANGEMIN;
				skill += EL_SKILLBASE;
			}
			if( skill == EL_FIRE_MANTLE ) {
				static const int dx[] = {-1, 0, 1, 1, 1, 0,-1,-1};
				static const int dy[] = { 1, 1, 1, 0,-1,-1,-1, 0};

				skill_unit_layout[pos].count = 8;
				memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
				memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
			}
		} else {
			switch( i ) {
				case MG_FIREWALL:
				case WZ_ICEWALL:
				case WL_EARTHSTRAIN:
				case RL_FIRE_RAIN:
					//These will be handled later
					break;
				case PR_SANCTUARY:
				case NPC_EVILLAND: {
						static const int dx[] = {
							-1, 0, 1,-2,-1, 0, 1, 2,-2,-1,
							 0, 1, 2,-2,-1, 0, 1, 2,-1, 0, 1};
						static const int dy[] = {
							-2,-2,-2,-1,-1,-1,-1,-1, 0, 0,
							 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2};

						skill_unit_layout[pos].count = 21;
						memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
						memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
					}
					break;
				case PR_MAGNUS: {
						static const int dx[] = {
							-1, 0, 1,-1, 0, 1,-3,-2,-1, 0,
							 1, 2, 3,-3,-2,-1, 0, 1, 2, 3,
							-3,-2,-1, 0, 1, 2, 3,-1, 0, 1,-1, 0, 1};
						static const int dy[] = {
							-3,-3,-3,-2,-2,-2,-1,-1,-1,-1,
							-1,-1,-1, 0, 0, 0, 0, 0, 0, 0,
							 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3};

						skill_unit_layout[pos].count = 33;
						memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
						memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
					}
					break;
				case AS_VENOMDUST: {
						static const int dx[] = {-1, 0, 0, 0, 1};
						static const int dy[] = { 0,-1, 0, 1, 0};

						skill_unit_layout[pos].count = 5;
						memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
						memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
					}
					break;
				case CR_GRANDCROSS:
				case NPC_GRANDDARKNESS: {
						static const int dx[] = {
							 0, 0,-1, 0, 1,-2,-1, 0, 1, 2,
							-4,-3,-2,-1, 0, 1, 2, 3, 4,-2,
							-1, 0, 1, 2,-1, 0, 1, 0, 0};
						static const int dy[] = {
							-4,-3,-2,-2,-2,-1,-1,-1,-1,-1,
							 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
							 1, 1, 1, 1, 2, 2, 2, 3, 4};

						skill_unit_layout[pos].count = 29;
						memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
						memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
					}
					break;
				case PF_FOGWALL: {
						static const int dx[] = {
							-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2};
						static const int dy[] = {
							-1,-1,-1,-1,-1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};

						skill_unit_layout[pos].count = 15;
						memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
						memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
					}
					break;
				case PA_GOSPEL: {
						static const int dx[] = {
							-1, 0, 1,-1, 0, 1,-3,-2,-1, 0,
							 1, 2, 3,-3,-2,-1, 0, 1, 2, 3,
							-3,-2,-1, 0, 1, 2, 3,-1, 0, 1,
							-1, 0, 1};
						static const int dy[] = {
							-3,-3,-3,-2,-2,-2,-1,-1,-1,-1,
							-1,-1,-1, 0, 0, 0, 0, 0, 0, 0,
							 1, 1, 1, 1, 1, 1, 1, 2, 2, 2,
							 3, 3, 3};

						skill_unit_layout[pos].count = 33;
						memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
						memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
					}
					break;
				case NJ_KAENSIN: {
						static const int dx[] = {-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2};
						static const int dy[] = { 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 0, 0, 0, 0,-1,-1,-1,-1,-1,-2,-2,-2,-2,-2};

						skill_unit_layout[pos].count = 24;
						memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
						memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
					}
					break;
				case NJ_TATAMIGAESHI: {
						//Level 1 (count 4, cross of 3x3)
						static const int dx1[] = {-1, 1, 0, 0};
						static const int dy1[] = { 0, 0,-1, 1};
						//Level 2-3 (count 8, cross of 5x5)
						static const int dx2[] = {-2,-1, 1, 2, 0, 0, 0, 0};
						static const int dy2[] = { 0, 0, 0, 0,-2,-1, 1, 2};
						//Level 4-5 (count 12, cross of 7x7
						static const int dx3[] = {-3,-2,-1, 1, 2, 3, 0, 0, 0, 0, 0, 0};
						static const int dy3[] = { 0, 0, 0, 0, 0, 0,-3,-2,-1, 1, 2, 3};

						//lv1
						j = 0;
						skill_unit_layout[pos].count = 4;
						memcpy(skill_unit_layout[pos].dx,dx1,sizeof(dx1));
						memcpy(skill_unit_layout[pos].dy,dy1,sizeof(dy1));
						skill_db[i].unit_layout_type[j] = pos;
						//lv2/3
						j++;
						pos++;
						skill_unit_layout[pos].count = 8;
						memcpy(skill_unit_layout[pos].dx,dx2,sizeof(dx2));
						memcpy(skill_unit_layout[pos].dy,dy2,sizeof(dy2));
						skill_db[i].unit_layout_type[j] = pos;
						skill_db[i].unit_layout_type[++j] = pos;
						//lv4/5
						j++;
						pos++;
						skill_unit_layout[pos].count = 12;
						memcpy(skill_unit_layout[pos].dx,dx3,sizeof(dx3));
						memcpy(skill_unit_layout[pos].dy,dy3,sizeof(dy3));
						skill_db[i].unit_layout_type[j] = pos;
						skill_db[i].unit_layout_type[++j] = pos;
						//Fill in the rest using lv 5
						for( ; j < MAX_SKILL_LEVEL; j++ )
							skill_db[i].unit_layout_type[j] = pos;
						//Skip, this way the check below will fail and continue to the next skill
						pos++;
					}
					break;
				case NPC_FLAMECROSS: {
						static const int dx[] = {-2,-1, 1, 2, 0, 0, 0, 0};
						static const int dy[] = { 0, 0, 0, 0,-2,-1, 1, 2};

						skill_unit_layout[pos].count = 8;
						memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
						memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
					}
					break;
				default:
					ShowError("unknown unit layout at skill %d\n",i);
					break;
			}
		}
		if( !skill_unit_layout[pos].count )
			continue;
		for( j = 0; j < MAX_SKILL_LEVEL; j++ )
			skill_db[i].unit_layout_type[j] = pos;
		pos++;
	}

	//Firewall and icewall have 8 layouts (direction-dependent)
	firewall_unit_pos = pos;
	for( i = 0; i < 8; i++ ) {
		if( i&1 ) {
			skill_unit_layout[pos].count = 5;
			if( i&0x2 ) {
				static const int dx[] = {-1,-1, 0, 0, 1};
				static const int dy[] = { 1, 0, 0,-1,-1};

				memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
				memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
			} else {
				static const int dx[] = { 1, 1 ,0, 0,-1};
				static const int dy[] = { 1, 0, 0,-1,-1};

				memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
				memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
			}
		} else {
			skill_unit_layout[pos].count = 3;
			if( i%4 == 0 ) {
				static const int dx[] = {-1, 0, 1};
				static const int dy[] = { 0, 0, 0};

				memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
				memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
			} else {
				static const int dx[] = { 0, 0, 0};
				static const int dy[] = {-1, 0, 1};

				memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
				memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
			}
		}
		pos++;
	}
	icewall_unit_pos = pos;
	for( i = 0; i < 8; i++ ) {
		skill_unit_layout[pos].count = 5;
		if( i&1 ) {
			if( i&0x2 ) {
				static const int dx[] = {-2,-1, 0, 1, 2};
				static const int dy[] = { 2, 1, 0,-1,-2};

				memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
				memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
			} else {
				static const int dx[] = { 2, 1 ,0,-1,-2};
				static const int dy[] = { 2, 1, 0,-1,-2};

				memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
				memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
			}
		} else {
			if( i%4 == 0 ) {
				static const int dx[] = {-2,-1, 0, 1, 2};
				static const int dy[] = { 0, 0, 0, 0, 0};

				memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
				memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
			} else {
				static const int dx[] = { 0, 0, 0, 0, 0};
				static const int dy[] = {-2,-1, 0, 1, 2};

				memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
				memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
			}
		}
		pos++;
	}
	earthstrain_unit_pos = pos;
	for( i = 0; i < 8; i++ ) { //For each Direction
		skill_unit_layout[pos].count = 15;
		switch( i ) {
			case 0: case 1: case 3: case 4: case 5: case 7:
				{
					static const int dx[] = {-7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7};
					static const int dy[] = { 0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0};

					memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
				}
				break;
			case 2:
			case 6:
				{
					static const int dx[] = { 0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0};
					static const int dy[] = {-7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7};

					memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
				}
				break;
		}
		pos++;
	}
	firerain_unit_pos = pos;
	for( i = 0; i < 8; i++ ) {
		skill_unit_layout[pos].count = 3;
		switch( i ) {
			case 0: case 1: case 3: case 4: case 5: case 7:
				{
					static const int dx[] = {-1, 0, 1};
					static const int dy[] = { 0, 0, 0};

					memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
				}
				break;
			case 2: case 6:
				{
					static const int dx[] = { 0, 0, 0};
					static const int dy[] = {-1, 0, 1};

					memcpy(skill_unit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_unit_layout[pos].dy,dy,sizeof(dy));
				}
				break;
		}
		pos++;
	}

	if( pos >= MAX_SKILL_UNIT_LAYOUT )
		ShowError("skill_init_unit_layout: The skill_unit_layout has met the limit or overflowed (pos=%d)\n",pos);
}

void skill_init_nounit_layout (void)
{
	int i, pos = 0;

	memset(skill_nounit_layout,0,sizeof(skill_nounit_layout));

	overbrand_nounit_pos = pos;
	for( i = 0; i < 8; i++ ) {
		if( i&1 ) {
			skill_nounit_layout[pos].count = 33;
			if( i&2 ) {
				if( i&4 ) { // 7
					int dx[] = { 5, 6, 7, 5, 6, 4, 5, 6, 4, 5, 3, 4, 5, 3, 4, 2, 3, 4, 2, 3, 1, 2, 3, 1, 2, 0, 1, 2, 0, 1,-1, 0, 1};
					int dy[] = { 7, 6, 5, 6, 5, 6, 5, 4, 5, 4, 5, 4, 3, 4, 3, 4, 3, 2, 3, 2, 3, 2, 1, 2, 1, 2, 1, 0, 1, 0, 1, 0,-1};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				} else { // 3
					int dx[] = {-5,-6,-7,-5,-6,-4,-5,-6,-4,-5,-3,-4,-5,-3,-4,-2,-3,-4,-2,-3,-1,-2,-3,-1,-2, 0,-1,-2, 0,-1, 1, 0,-1};
					int dy[] = {-7,-6,-5,-6,-5,-6,-5,-4,-5,-4,-5,-4,-3,-4,-3,-4,-3,-2,-3,-2,-3,-2,-1,-2,-1,-2,-1, 0,-1, 0,-1, 0, 1};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				}
			} else {
				if( i&4 ) { // 5
					int dx[] = { 7, 6, 5, 6, 5, 6, 5, 4, 5, 4, 5, 4, 3, 4, 3, 4, 3, 2, 3, 2, 3, 2, 1, 2, 1, 2, 1, 0, 1, 0, 1, 0,-1};
					int dy[] = {-5,-6,-7,-5,-6,-4,-5,-6,-4,-5,-3,-4,-5,-3,-4,-2,-3,-4,-2,-3,-1,-2,-3,-1,-2, 0,-1,-2, 0,-1, 1, 0,-1};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				} else { // 1
					int dx[] = {-7,-6,-5,-6,-5,-6,-5,-4,-5,-4,-5,-4,-3,-4,-3,-4,-3,-2,-3,-2,-3,-2,-1,-2,-1,-2,-1, 0,-1, 0,-1, 0, 1};
					int dy[] = { 5, 6, 7, 5, 6, 4, 5, 6, 4, 5, 3, 4, 5, 3, 4, 2, 3, 4, 2, 3, 1, 2, 3, 1, 2, 0, 1, 2, 0, 1,-1, 0, 1};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				}
			}
		} else {
			skill_nounit_layout[pos].count = 21;
			if( i&2 ) {
				if( i&4 ) { // 6
					int dx[] = { 0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6};
					int dy[] = { 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,-1,-1,-1,-1,-1,-1,-1};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				} else { // 2
					int dx[] = {-6,-5,-4,-3,-2,-1, 0,-6,-5,-4,-3,-2,-1, 0,-6,-5,-4,-3,-2,-1, 0};
					int dy[] = { 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,-1,-1,-1,-1,-1,-1,-1};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				}
			} else {
				if( i&4 ) { // 4
					int dx[] = {-1, 0, 1,-1, 0, 1,-1, 0, 1,-1, 0, 1,-1, 0, 1,-1, 0, 1,-1, 0, 1};
					int dy[] = { 0, 0, 0,-1,-1,-1,-2,-2,-2,-3,-3,-3,-4,-4,-4,-5,-5,-5,-6,-6,-6};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				} else { // 0
					int dx[] = {-1, 0, 1,-1, 0, 1,-1, 0, 1,-1, 0, 1,-1, 0, 1,-1, 0, 1,-1, 0, 1};
					int dy[] = { 6, 6, 6, 5, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 1, 1, 1, 0, 0, 0};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				}
			}
		}
		pos++;
	}

	overbrand_brandish_nounit_pos = pos;
	for( i = 0; i < 8; i++ ) {
		if( i&1 ) {
			skill_nounit_layout[pos].count = 74;
			if( i&2 ) {
				if( i&4 ) { // 7
					int dx[] = {-2,-1, 0, 1, 2, 3, 4, 5, 6, 7, 8,-2,-1, 0, 1, 2, 3, 4, 5, 6, 7,
								-3,-2,-1, 0, 1, 2, 3, 4, 5, 6, 7,-3,-2,-1,-0, 1, 2, 3, 4, 5, 6,
								-4,-3,-2,-1, 0, 1, 2, 3, 4, 5, 6,-4,-3,-2,-1,-0, 1, 2, 3, 4, 5,
								-5,-4,-3,-2,-1, 0, 1, 2, 3, 4, 5};
					int dy[] = { 8, 7, 6, 5, 4, 3, 2, 1, 0,-1,-2, 7, 6, 5, 4, 3, 2, 1, 0,-1,-2,
								 7, 6, 5, 4, 3, 2, 1, 0,-1,-2,-3, 6, 5, 4, 3, 2, 1, 0,-1,-2,-3,
								 6, 5, 4, 3, 2, 1, 0,-1,-2,-3,-4, 5, 4, 3, 2, 1, 0,-1,-2,-3,-4,
								 5, 4, 3, 2, 1, 0,-1,-2,-3,-4,-5};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				} else { // 3
					int dx[] = { 2, 1, 0,-1,-2,-3,-4,-5,-6,-7,-8, 2, 1, 0,-1,-2,-3,-4,-5,-6,-7,
								 3, 2, 1, 0,-1,-2,-3,-4,-5,-6,-7, 3, 2, 1, 0,-1,-2,-3,-4,-5,-6,
								 4, 3, 2, 1, 0,-1,-2,-3,-4,-5,-6, 4, 3, 2, 1, 0,-1,-2,-3,-4,-5,
								 5, 4, 3, 2, 1, 0,-1,-2,-3,-4,-5};
					int dy[] = {-8,-7,-6,-5,-4,-3,-2,-1, 0, 1, 2,-7,-6,-5,-4,-3,-2,-1, 0, 1, 2,
								-7,-6,-5,-4,-3,-2,-1, 0, 1, 2, 3,-6,-5,-4,-3,-2,-1, 0, 1, 2, 3,
								-6,-5,-4,-3,-2,-1, 0, 1, 2, 3, 4,-5,-4,-3,-2,-1, 0, 1, 2, 3, 4,
								-5,-4,-3,-2,-1, 0, 1, 2, 3, 4, 5};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				}
			} else {
				if( i&4 ) { // 5
					int dx[] = { 8, 7, 6, 5, 4, 3, 2, 1, 0,-1,-2, 7, 6, 5, 4, 3, 2, 1, 0,-1,-2,
								 7, 6, 5, 4, 3, 2, 1, 0,-1,-2,-3, 6, 5, 4, 3, 2, 1, 0,-1,-2,-3,
								 6, 5, 4, 3, 2, 1, 0,-1,-2,-3,-4, 5, 4, 3, 2, 1, 0,-1,-2,-3,-4,
								 5, 4, 3, 2, 1, 0,-1,-2,-3,-4,-5};
					int dy[] = { 2, 1, 0,-1,-2,-3,-4,-5,-6,-7,-8, 2, 1, 0,-1,-2,-3,-4,-5,-6,-7,
								 3, 2, 1, 0,-1,-2,-3,-4,-5,-6,-7, 3, 2, 1, 0,-1,-2,-3,-4,-5,-6,
								 4, 3, 2, 1, 0,-1,-2,-3,-4,-5,-6, 4, 3, 2, 1, 0,-1,-2,-3,-4,-5,
								 5, 4, 3, 2, 1, 0,-1,-2,-3,-4,-5};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				} else { // 1
					int dx[] = {-8,-7,-6,-5,-4,-3,-2,-1, 0, 1, 2,-7,-6,-5,-4,-3,-2,-1, 0, 1, 2,
								-7,-6,-5,-4,-3,-2,-1, 0, 1, 2, 3,-6,-5,-4,-3,-2,-1, 0, 1, 2, 3,
								-6,-5,-4,-3,-2,-1, 0, 1, 2, 3, 4,-5,-4,-3,-2,-1, 0, 1, 2, 3, 4,
								-5,-4,-3,-2,-1, 0, 1, 2, 3, 4, 5};
					int dy[] = {-2,-1, 0, 1, 2, 3, 4, 5, 6, 7, 8,-2,-1, 0, 1, 2, 3, 4, 5, 6, 7,
								-3,-2,-1, 0, 1, 2, 3, 4, 5, 6, 7,-3,-2,-1, 0, 1, 2, 3, 4, 5, 6,
								-4,-3,-2,-1, 0, 1, 2, 3, 4, 5, 6,-4,-3,-2,-1, 0, 1, 2, 3, 4, 5,
								-5,-4,-3,-2,-1, 0, 1, 2, 3, 4, 5};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				}
			}
		} else {
			skill_nounit_layout[pos].count = 44;
			if( i&2 ) {
				if( i&4 ) { // 6
					int dx[] = { 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
					int dy[] = { 5, 5, 5, 5, 4, 4, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0,-1,-1,-1,-1,-2,-2,-2,-2,-3,-3,-3,-3,-4,-4,-4,-4,-5,-5,-5,-5};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				} else { // 2
					int dx[] = {-3,-2,-1, 0,-3,-2,-1, 0,-3,-2,-1, 0,-3,-2,-1, 0,-3,-2,-1, 0,-3,-2,-1, 0,-3,-2,-1, 0,-3,-2,-1, 0,-3,-2,-1, 0,-3,-2,-1, 0,-3,-2,-1, 0};
					int dy[] = { 5, 5, 5, 5, 4, 4, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0,-1,-1,-1,-1,-2,-2,-2,-2,-3,-3,-3,-3,-4,-4,-4,-4,-5,-5,-5,-5};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				}
			} else {
				if( i&4 ) { // 4
					int dx[] = { 5, 4, 3, 2, 1, 0,-1,-2,-3,-4,-5, 5, 4, 3, 2, 1, 0,-1,-2,-3,-4,-5, 5, 4, 3, 2, 1, 0,-1,-2,-3,-4,-5, 5, 4, 3, 2, 1, 0,-1,-2,-3,-4,-5};
					int dy[] = {-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				} else { // 0
					int dx[] = {-5,-4,-3,-2,-1, 0, 1, 2, 3, 4, 5,-5,-4,-3,-2,-1, 0, 1, 2, 3, 4, 5,-5,-4,-3,-2,-1, 0, 1, 2, 3, 4, 5,-5,-4,-3,-2,-1, 0, 1, 2, 3, 4, 5};
					int dy[] = { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

					memcpy(skill_nounit_layout[pos].dx,dx,sizeof(dx));
					memcpy(skill_nounit_layout[pos].dy,dy,sizeof(dy));
				}
			}
		}
		pos++;
	}

	if( pos >= MAX_SKILL_UNIT_LAYOUT )
		ShowError("skill_init_nounit_layout: The skill_nounit_layout has met the limit or overflowed (pos=%d)\n",pos);
}

int skill_block_check(struct block_list *bl, sc_type type , uint16 skill_id) {
	int inf = 0;
	struct status_change *sc = status_get_sc(bl);

	if( !sc || !bl || !skill_id )
		return 0; //Can do it

	switch( type ) {
		case SC_ANKLE:
			if( skill_id == AL_TELEPORT )
				return 1;
			break;
		case SC_STASIS:
			inf = skill_get_inf2(skill_id);
			//Song, Dance, Ensemble, Chorus, and all magic skills will not work in Stasis status [Rytech]
			if( inf == INF2_SONG_DANCE || inf == INF2_ENSEMBLE_SKILL ||
				inf == INF2_CHORUS_SKILL || skill_get_type(skill_id) == BF_MAGIC )
				return 1; //Can't do it
			break;
	}

	return 0;
}

/**
 * Determines whether a skill is currently active or not
 * Used for purposes of cancelling SP usage when disabling a skill
 */
int skill_disable_check(struct status_change *sc, uint16 skill_id)
{
	switch( skill_id ) { //HP & SP Consumption Check
		case TF_HIDING:
		case BS_MAXIMIZE:
		case AS_CLOAKING:
		case NV_TRICKDEAD:
		case CR_AUTOGUARD:
		case ML_AUTOGUARD:
		case PA_GOSPEL:
		case ST_CHASEWALK:
		case TK_RUN:
		case SG_FUSION:
		case CR_SHRINK:
		case GC_CLOAKINGEXCEED:
		case RA_CAMOUFLAGE:
		case SJ_LUNARSTANCE:
		case SJ_STARSTANCE:
		case SJ_UNIVERSESTANCE:
		case SJ_SUNSTANCE:
		case SP_SOULCOLLECT:
		//case SU_HIDE:
			if( !sc->data[status_skill2sc(skill_id)] )
				return 1;
			break;
		//These 2 skills contain a master and are not correctly pulled using skill2sc
		case NC_NEUTRALBARRIER:
			if( !sc->data[SC_NEUTRALBARRIER_MASTER] )
				return 1;
			break;
		case NC_STEALTHFIELD:
			if( !sc->data[SC_STEALTHFIELD_MASTER] )
				return 1;
			break;
	}

	return 0;
}

/*==========================================
 * sub-function of DB reading.
 * skill_db.txt
 *------------------------------------------*/
static bool skill_parse_row_skilldb(char *split[], int columns, int current)
{ //id,range,hit,inf,element,nk,splash,max,list_num,castcancel,cast_defence_rate,inf2,maxcount,skill_type,blow_count,name,description
	uint16 skill_id = atoi(split[0]);
	uint16 idx = skill_get_index(skill_id);

	if( (skill_id >= GD_SKILLRANGEMIN && skill_id <= GD_SKILLRANGEMAX) ||
		(skill_id >= HM_SKILLRANGEMIN && skill_id <= HM_SKILLRANGEMAX) ||
		(skill_id >= MC_SKILLRANGEMIN && skill_id <= MC_SKILLRANGEMAX) ||
		(skill_id >= EL_SKILLRANGEMIN && skill_id <= EL_SKILLRANGEMAX) )
	{
		ShowWarning("skill_parse_row_skilldb: Skill id %d is forbidden (interferes with guild/homun/mercenary/elemental skills mapping)!\n", skill_id);
		return false;
	}

	if( !idx ) //Invalid skill id
		return false;

	skill_split_atoi(split[1],skill_db[idx].range);
	skill_db[idx].hit = atoi(split[2]);
	skill_db[idx].inf = atoi(split[3]);
	skill_split_atoi(split[4],skill_db[idx].element);
	skill_db[idx].nk = (int)strtol(split[5], NULL, 0);
	skill_split_atoi(split[6],skill_db[idx].splash);
	skill_db[idx].max = atoi(split[7]);
	skill_split_atoi(split[8],skill_db[idx].num);

	if( strcmpi(split[9],"yes") == 0 )
		skill_db[idx].castcancel = 1;
	else
		skill_db[idx].castcancel = 0;
	skill_db[idx].cast_def_rate = atoi(split[10]);
	skill_db[idx].inf2 = (int)strtol(split[11], NULL, 0);
	skill_split_atoi(split[12],skill_db[idx].maxcount);
	if( strcmpi(split[13],"weapon") == 0 )
		skill_db[idx].skill_type = BF_WEAPON;
	else if( strcmpi(split[13],"magic") == 0 )
		skill_db[idx].skill_type = BF_MAGIC;
	else if( strcmpi(split[13],"misc") == 0 )
		skill_db[idx].skill_type = BF_MISC;
	else
		skill_db[idx].skill_type = 0;
	skill_split_atoi(split[14],skill_db[idx].blewcount);
	skill_db[idx].inf3 = (int)strtol(split[15], NULL, 0);
	safestrncpy(skill_db[idx].name, trim(split[16]), sizeof(skill_db[idx].name));
	safestrncpy(skill_db[idx].desc, trim(split[17]), sizeof(skill_db[idx].desc));
	strdb_iput(skilldb_name2id, skill_db[idx].name, skill_id);

	return true;
}

/** Split string to int by constanta value (const.txt) or atoi()
 * @param *str: String input
 * @param *val: Temporary storage
 * @param *delim: Delimiter (for multiple value support)
 * @param min_value: Minimum value. If the splitted value is less or equal than this, will be skipped
 * @param max: Maximum number that can be allocated
 * @return count: Number of success
 */
uint8 skill_split_atoi2(char *str, int *val, const char *delim, int min_value, uint16 max) {
	uint8 i = 0;
	char *p = strtok(str, delim);

	while( p != NULL ) {
		int n = min_value;

		trim(p);
		if( ISDIGIT(p[0]) ) //If using numeric
			n = atoi(p);
		else if( !script_get_constant(p, &n) ) { //If using constant value
			ShowError("skill_split_atoi2: Invalid value: '%s'\n", p);
			p = strtok(NULL, delim);
			continue;
		}

		if( n > min_value ) {
			val[i] = n;
			i++;
			if( i >= max )
				break;
		}
		p = strtok(NULL, delim);
	}
	return i;
}

//Clear status data from skill requirement
static void skill_destroy_requirement(void) {
	uint16 i;

	for( i = 0; i < MAX_SKILL; i++ ) {
		if( skill_db[i].require.status_count )
			aFree(skill_db[i].require.status);
		skill_db[i].require.status_count = 0;
		if( skill_db[i].require.eqItem_count )
			aFree(skill_db[i].require.eqItem);
		skill_db[i].require.eqItem_count = 0;
	}
}

/**
 * Read skill requirement from skill_require_db.txt
 * Structure: skill_id,HPCost,MaxHPTrigger,SPCost,HPRateCost,SPRateCost,ZenyCost,RequiredWeapons,RequiredAmmoTypes,RequiredAmmoAmount,RequiredState,RequiredStatuss,SpiritSphereCost,RequiredItemID1,RequiredItemAmount1,RequiredItemID2,RequiredItemAmount2,RequiredItemID3,RequiredItemAmount3,RequiredItemID4,RequiredItemAmount4,RequiredItemID5,RequiredItemAmount5,RequiredItemID6,RequiredItemAmount6,RequiredItemID7,RequiredItemAmount7,RequiredItemID8,RequiredItemAmount8,RequiredItemID9,RequiredItemAmount9,RequiredItemID10,RequiredItemAmount10,RequiredEquipment
 */
static bool skill_parse_row_requiredb(char *split[], int columns, int current)
{
	char *p;
	uint16 skill_id = atoi(split[0]), i;
	uint16 idx = skill_get_index(skill_id);

	if( !idx ) //Invalid skill id
		return false;

	skill_split_atoi(split[1],skill_db[idx].require.hp);
	skill_split_atoi(split[2],skill_db[idx].require.mhp);
	skill_split_atoi(split[3],skill_db[idx].require.sp);
	skill_split_atoi(split[4],skill_db[idx].require.hp_rate);
	skill_split_atoi(split[5],skill_db[idx].require.sp_rate);
	skill_split_atoi(split[6],skill_db[idx].require.zeny);

	//Which weapon type are required, see doc/item_db for weapon types (View column)
	p = split[7];
	while( p ) {
		int l = atoi(p);

		if( l == 99 ) { //Any weapon
			skill_db[idx].require.weapon = 0;
			break;
		} else
			skill_db[idx].require.weapon |= 1<<l;
		p = strchr(p,':');
		if( !p )
			break;
		p++;
	}

	//Ammo type that required, see doc/item_db for ammo types (View column)
	p = split[8];
	while( p ) {
		int l = atoi(p);

		if( l == 99 ) { //Any ammo type
			skill_db[idx].require.ammo = 0xFFFFFFFF;
			break;
		} else if( l ) //0 stands for no requirement
			skill_db[idx].require.ammo |= 1<<l;
		p = strchr(p,':');
		if( !p )
			break;
		p++;
	}
	skill_split_atoi(split[9],skill_db[idx].require.ammo_qty);

	if(      strcmpi(split[10],"hidden")              == 0 ) skill_db[idx].require.state = ST_HIDDEN;
	else if( strcmpi(split[10],"riding")              == 0 ) skill_db[idx].require.state = ST_RIDING;
	else if( strcmpi(split[10],"falcon")              == 0 ) skill_db[idx].require.state = ST_FALCON;
	else if( strcmpi(split[10],"cart")                == 0 ) skill_db[idx].require.state = ST_CART;
	else if( strcmpi(split[10],"shield")              == 0 ) skill_db[idx].require.state = ST_SHIELD;
	else if( strcmpi(split[10],"recover_weight_rate") == 0 ) skill_db[idx].require.state = ST_RECOV_WEIGHT_RATE;
	else if( strcmpi(split[10],"move_enable")         == 0 ) skill_db[idx].require.state = ST_MOVE_ENABLE;
	else if( strcmpi(split[10],"water")               == 0 ) skill_db[idx].require.state = ST_WATER;
	else if( strcmpi(split[10],"dragon")              == 0 ) skill_db[idx].require.state = ST_RIDINGDRAGON;
	else if( strcmpi(split[10],"warg")                == 0 ) skill_db[idx].require.state = ST_WUG;
	else if( strcmpi(split[10],"ridingwarg")          == 0 ) skill_db[idx].require.state = ST_RIDINGWUG;
	else if( strcmpi(split[10],"mado")                == 0 ) skill_db[idx].require.state = ST_MADO;
	else if( strcmpi(split[10],"elementalspirit")     == 0 ) skill_db[idx].require.state = ST_ELEMENTALSPIRIT;
	else if( strcmpi(split[10],"peco")                == 0 ) skill_db[idx].require.state = ST_PECO;
	else if( strcmpi(split[10],"sunstance")           == 0 ) skill_db[idx].require.state = ST_SUNSTANCE;
	else if( strcmpi(split[10],"lunarstance")         == 0 ) skill_db[idx].require.state = ST_LUNARSTANCE;
	else if( strcmpi(split[10],"starstance")          == 0 ) skill_db[idx].require.state = ST_STARSTANCE;
	else if( strcmpi(split[10],"universestance")      == 0 ) skill_db[idx].require.state = ST_UNIVERSESTANCE;
	else skill_db[idx].require.state = ST_NONE; //Unknown or no state

	//Status requirements
	//FIXME: Default entry should be -1/SC_ALL in skill_require_db.txt but it's 0/SC_STONE
	trim(split[11]);
	if( split[11][0] != '\0' || atoi(split[11]) ) {
		int require[MAX_SKILL_STATUS_REQUIRE];

		if( skill_db[idx].require.status_count > 0 )
			aFree(skill_db[idx].require.status);
		if( (skill_db[idx].require.status_count = skill_split_atoi2(split[11],require,":",SC_STONE,ARRAYLENGTH(require))) ) {
			CREATE(skill_db[idx].require.status,enum sc_type,skill_db[idx].require.status_count);
			for( i = 0; i < skill_db[idx].require.status_count; i++ ) {
				//@TODO: Add a check if possible here
				skill_db[idx].require.status[i] = (sc_type)require[i];
			}
		}
	}

	skill_split_atoi(split[12],skill_db[idx].require.spiritball);

	for( i = 0; i < MAX_SKILL_ITEM_REQUIRE; i++ ) {
		int itemid = atoi(split[13 + 2 * i]);

		if( itemid && !itemdb_exists(itemid) ) {
			ShowError("skill_parse_row_requiredb: Invalid item (in ITEM_REQUIRE list) %d for skill %d.\n",itemid,atoi(split[0]));
			return false;
		}
		skill_db[idx].require.itemid[i] = itemid;
		skill_db[idx].require.amount[i] = atoi(split[14 + 2 * i]);
	}

	//Equipped Item requirements
	trim(split[33]);
	if( split[33][0] != '\0' || atoi(split[33]) ) {
		int require[MAX_SKILL_EQUIP_REQUIRE];

		if( skill_db[idx].require.eqItem_count > 0 )
			aFree(skill_db[idx].require.eqItem);
		if( (skill_db[idx].require.eqItem_count = skill_split_atoi2(split[33],require,":",500,ARRAYLENGTH(require))) ) {
			CREATE(skill_db[idx].require.eqItem,uint16,skill_db[idx].require.eqItem_count);
			for( i = 0; i < skill_db[idx].require.eqItem_count; i++ ) {
				if( require[i] && !itemdb_exists(require[i]) ) {
					ShowError("skill_parse_row_requiredb: Invalid item (in EQUIP_REQUIRE list)  %d for skill %d.\n",require[i],atoi(split[0]));
					aFree(skill_db[idx].require.eqItem); //Don't need to retain this
					skill_db[idx].require.eqItem_count = 0;
					return false;
				}
				skill_db[idx].require.eqItem[i] = require[i];
			}
		}
	}
	return true;
}

/** Reads skill cast db
 * Structure: SkillID,CastingTime,AfterCastActDelay,AfterCastWalkDelay,Duration1,Duration2,Cooldown{,Fixedcast}
 */
static bool skill_parse_row_castdb(char *split[], int columns, int current)
{
	uint16 idx = skill_get_index(atoi(split[0]));

	if( !idx ) //Invalid skill id
		return false;

	skill_split_atoi(split[1],skill_db[idx].cast);
	skill_split_atoi(split[2],skill_db[idx].delay);
	skill_split_atoi(split[3],skill_db[idx].walkdelay);
	skill_split_atoi(split[4],skill_db[idx].upkeep_time);
	skill_split_atoi(split[5],skill_db[idx].upkeep_time2);
	skill_split_atoi(split[6],skill_db[idx].cooldown);
#ifdef RENEWAL_CAST
	skill_split_atoi(split[7],skill_db[idx].fixed_cast);
#endif
	return true;
}

/** Reads skill cast no dex db
 * Structure: SkillID,Cast,Delay (optional)
 */
static bool skill_parse_row_castnodexdb(char *split[], int columns, int current)
{
	uint16 idx = skill_get_index(atoi(split[0]));

	if( !idx ) //Invalid skill id
		return false;

	skill_split_atoi(split[1],skill_db[idx].castnodex);
	if( split[2] ) //Optional column
		skill_split_atoi(split[2],skill_db[idx].delaynodex);

	return true;
}

/** Reads skill no cast db
 * Structure: SkillID,Flag
 */
static bool skill_parse_row_nocastdb(char *split[], int columns, int current)
{
	uint16 idx = skill_get_index(atoi(split[0]));

	if( !idx ) //Invalid skill id
		return false;

	skill_db[idx].nocast |= atoi(split[1]);

	return true;
}

/** Reads skill unit db
 * Structure: ID,unit ID,unit ID 2,layout,range,interval,target,flag
 */
static bool skill_parse_row_unitdb(char *split[], int columns, int current)
{
	uint16 idx = skill_get_index(atoi(split[0]));

	if( !idx ) //Invalid skill id
		return false;

	skill_db[idx].unit_id[0] = strtol(split[1],NULL,16);
	skill_db[idx].unit_id[1] = strtol(split[2],NULL,16);
	skill_split_atoi(split[3],skill_db[idx].unit_layout_type);
	skill_split_atoi(split[4],skill_db[idx].unit_range);
	skill_db[idx].unit_interval = atoi(split[5]);

	trim(split[6]);
	if( !strcmpi(split[6],"noenemy") ) skill_db[idx].unit_target = BCT_NOENEMY;
	else if( !strcmpi(split[6],"friend") ) skill_db[idx].unit_target = BCT_NOENEMY;
	else if( !strcmpi(split[6],"party") ) skill_db[idx].unit_target = BCT_PARTY;
	else if( !strcmpi(split[6],"ally") ) skill_db[idx].unit_target = BCT_PARTY|BCT_GUILD;
	else if( !strcmpi(split[6],"guild") ) skill_db[idx].unit_target = BCT_GUILD;
	else if( !strcmpi(split[6],"all") ) skill_db[idx].unit_target = BCT_ALL;
	else if( !strcmpi(split[6],"enemy") ) skill_db[idx].unit_target = BCT_ENEMY;
	else if( !strcmpi(split[6],"self") ) skill_db[idx].unit_target = BCT_SELF;
	else if( !strcmpi(split[6],"sameguild") ) skill_db[idx].unit_target = BCT_GUILD|BCT_SAMEGUILD;
	else if( !strcmpi(split[6],"noone") ) skill_db[idx].unit_target = BCT_NOONE;
	else skill_db[idx].unit_target = strtol(split[6],NULL,16);

	skill_db[idx].unit_flag = strtol(split[7],NULL,16);

	if( skill_db[idx].unit_flag&UF_DEFNOTENEMY && battle_config.defnotenemy )
		skill_db[idx].unit_target = BCT_NOENEMY;

	//By default, target just characters
	skill_db[idx].unit_target |= BL_CHAR;
	if( skill_db[idx].unit_flag&UF_NOPC )
		skill_db[idx].unit_target &= ~BL_PC;
	if( skill_db[idx].unit_flag&UF_NOMOB )
		skill_db[idx].unit_target &= ~BL_MOB;
	if( skill_db[idx].unit_flag&UF_SKILL )
		skill_db[idx].unit_target |= BL_SKILL;

	return true;
}

/** Reads Produce db
 * Structure: ProduceItemID,UniqueID,ItemLV,RequireSkill,Requireskill_lv,MaterialID1,MaterialAmount1,...
 */
static bool skill_parse_row_producedb(char *split[], int columns, int current)
{
	unsigned short x, y, nameid = atoi(split[0]);

	if( !itemdb_exists(nameid) ) {
		ShowError("skill_parse_row_producedb: Invalid item %d.\n", nameid);
		return false;
	}
	if( current >= MAX_SKILL_PRODUCE_DB ) {
		ShowError("skill_parse_row_producedb: Maximum amount of entries reached (%d), increase MAX_SKILL_PRODUCE_DB\n", MAX_SKILL_PRODUCE_DB);
		return false;
	}

	skill_produce_db[current].nameid = nameid;
	skill_produce_db[current].unique_id = atoi(split[1]);
	skill_produce_db[current].itemlv = atoi(split[2]);
	skill_produce_db[current].req_skill = atoi(split[3]);
	skill_produce_db[current].req_skill_lv = atoi(split[4]);

	for( x = 5, y = 0; x + 1 < columns && split[x] && split[x + 1] && y < MAX_PRODUCE_RESOURCE; x += 2, y++ ) {
		skill_produce_db[current].mat_id[y] = atoi(split[x]);
		skill_produce_db[current].mat_amount[y] = atoi(split[x + 1]);
	}

	return true;
}

/** Reads create arrow db
 * Sturcture: SourceID,MakeID1,MakeAmount1,...,MakeID5,MakeAmount5
 */
static bool skill_parse_row_createarrowdb(char *split[], int columns, int current)
{
	int x, y, material_id = atoi(split[0]);

	if( !(itemdb_exists(material_id)) ) {
		ShowError("skill_parse_row_createarrowdb: Invalid item %d.\n", material_id);
		return false;
	}
	if( current >= MAX_SKILL_ARROW_DB ) {
		ShowError("skill_parse_row_createarrowdb: Maximum amount of entries reached (%d), increase MAX_SKILL_ARROW_DB\n", MAX_SKILL_ARROW_DB);
		return false;
	}

	skill_arrow_db[current].nameid = material_id;

	for( x = 1, y = 0; x + 1 < columns && split[x] && split[x + 1] && y < MAX_ARROW_RESULT; x += 2, y++ ) {
		skill_arrow_db[current].cre_id[y] = atoi(split[x]);
		skill_arrow_db[current].cre_amount[y] = atoi(split[x + 1]);
	}

	return true;
}

/** Reads Spell book db
 * Structure: SkillID,PreservePoints,RequiredBook
 */
static bool skill_parse_row_spellbookdb(char *split[], int columns, int current)
{
	uint16 skill_id = atoi(split[0]);
	unsigned short points = atoi(split[1]), nameid = atoi(split[2]);

	if( !skill_get_index(skill_id) || !skill_get_max(skill_id) )
		ShowError("skill_parse_row_spellbookdb: Invalid skill ID %d\n", skill_id);
	if( !skill_get_inf(skill_id) )
		ShowError("skill_parse_row_spellbookdb: Passive skills cannot be memorized (%d/%s)\n", skill_id, skill_get_name(skill_id));
	if( points < 1 )
		ShowError("skill_parse_row_spellbookdb: PreservePoints have to be 1 or above! (%d/%s)\n", skill_id, skill_get_name(skill_id));
	if( current >= MAX_SKILL_SPELLBOOK_DB )
		ShowError("skill_parse_row_spellbookdb: Maximum amount of entries reached (%d), increase MAX_SKILL_SPELLBOOK_DB\n", MAX_SKILL_SPELLBOOK_DB);
	else {
		skill_spellbook_db[current].skill_id = skill_id;
		skill_spellbook_db[current].point = points;
		skill_spellbook_db[current].nameid = nameid;
		return true;
	}

	return false;
}

/** Reads improvise db
 * Structure: SkillID,Rate
 */
static bool skill_parse_row_improvisedb(char *split[], int columns, int current)
{
	uint16 skill_id = atoi(split[0]);
	unsigned short per = atoi(split[1]);

	if( !skill_get_index(skill_id) || !skill_get_max(skill_id) ) {
		ShowError("skill_parse_row_improvisedb: Invalid skill ID %d\n", skill_id);
		return false;
	}
	if( !skill_get_inf(skill_id) ) {
		ShowError("skill_parse_row_improvisedb: Passive skills cannot be casted (%d/%s)\n", skill_id, skill_get_name(skill_id));
		return false;
	}
	if( !per ) {
		ShowError("skill_parse_row_improvisedb: Chances have to be 1 or above! (%d/%s)\n", skill_id, skill_get_name(skill_id));
		return false;
	}
	if( current >= MAX_SKILL_IMPROVISE_DB ) {
		ShowError("skill_parse_row_improvisedb: Maximum amount of entries reached (%d), increase MAX_SKILL_IMPROVISE_DB\n", MAX_SKILL_IMPROVISE_DB);
		return false;
	}

	skill_improvise_db[current].skill_id = skill_id;
	skill_improvise_db[current].per = per;

	return true;
}

/** Reads Magic mushroom db
 * Structure: SkillID
 */
static bool skill_parse_row_magicmushroomdb(char *split[], int column, int current)
{
	uint16 skill_id = atoi(split[0]);

	if( !skill_get_index(skill_id) || !skill_get_max(skill_id) ) {
		ShowError("skill_parse_row_magicmushroomdb: Invalid skill ID %d\n", skill_id);
		return false;
	}
	if( !skill_get_inf(skill_id) ) {
		ShowError("skill_parse_row_magicmushroomdb: Passive skills cannot be casted (%d/%s)\n", skill_id, skill_get_name(skill_id));
		return false;
	}
	if( current >= MAX_SKILL_MAGICMUSHROOM_DB ) {
		ShowError("skill_parse_row_magicmushroomdb: Maximum amount of entries reached (%d), increase MAX_SKILL_MAGICMUSHROOM_DB\n", MAX_SKILL_MAGICMUSHROOM_DB);
		return false;
	}

	skill_magicmushroom_db[current].skill_id = skill_id;

	return true;
}

/** Reads db of copyable skill
 * Structure: SkillName,Option{,JobAllowed{,RequirementRemoved}}
 *	SkillID,Option{,JobAllowed{,RequirementRemoved}}
 */
static bool skill_parse_row_copyabledb(char *split[], int column, int current) {
	uint16 idx = 0;
	int option = 0;

	trim(split[0]);
	if( ISDIGIT(split[0][0]) )
		idx = atoi(split[0]);
	else
		idx = skill_name2id(split[0]);

	if( !(idx = skill_get_index(idx)) ) {
		ShowError("skill_parse_row_copyabledb: Invalid skill %s\n", split[0]);
		return false;
	}
	if( (option = atoi(split[1])) > 3 ) {
		ShowError("skill_parse_row_copyabledb: Invalid option '%s'\n", split[1]);
		return false;
	}

	skill_db[idx].copyable.option = option;
	skill_db[idx].copyable.joballowed = 63;
	if( atoi(split[2]) )
		skill_db[idx].copyable.joballowed = cap_value(atoi(split[2]), 1, 63);
	skill_db[idx].copyable.req_opt = cap_value(atoi(split[3]), 0, (0x2000) - 1);

	return true;
}

/** Reads additional range for distance checking from NPC [Cydh]
 * Structure: SkillName,AdditionalRange{,NPC Type}
 *	SkillID,AdditionalRange{,NPC Type}
 */
static bool skill_parse_row_nonearnpcrangedb(char *split[], int column, int current) {
	uint16 idx = 0;

	trim(split[0]);
	if( ISDIGIT(split[0][0]) )
		idx = atoi(split[0]);
	else
		idx = skill_name2id(split[0]);

	if( !(idx = skill_get_index(idx)) ) { //Invalid skill id
		ShowError("skill_parse_row_nonearnpcrangedb: Invalid skill '%s'\n", split[0]);
		return false;
	}

	skill_db[idx].unit_nonearnpc_range = max(atoi(split[1]), 0);
	skill_db[idx].unit_nonearnpc_type = (atoi(split[2])) ? cap_value(atoi(split[2]), 1, 15) : 15;

	return true;
}

/** Reads skill chance by Abracadabra/Hocus Pocus spell
 * Structure: SkillID,DummyName,RatePerLvl
 */
static bool skill_parse_row_abradb(char *split[], int columns, int current) {
	uint16 skill_id = atoi(split[0]);

	if( !skill_get_index(skill_id) || !skill_get_max(skill_id) ) {
		ShowError("skill_parse_row_abradb: Invalid skill ID %d\n", skill_id);
		return false;
	}
	if( !skill_get_inf(skill_id) ) {
		ShowError("skill_parse_row_abradb: Passive skills cannot be casted (%d/%s)\n", skill_id, skill_get_name(skill_id));
		return false;
	}
	if( current >= MAX_SKILL_ABRA_DB ) {
		ShowError("skill_parse_row_abradb: Maximum amount of entries reached (%d), increase MAX_SKILL_ABRA_DB\n", MAX_SKILL_ABRA_DB);
		return false;
	}

	skill_abra_db[current].skill_id = skill_id;
	safestrncpy(skill_abra_db[current].name, trim(split[1]), sizeof(skill_abra_db[current].name)); //Store dummyname
	skill_split_atoi(split[2],skill_abra_db[current].per);

	return true;
}

/** Reads change material db
 * Structure: ProductID,UniqueID,BaseRate,MakeAmount1,MakeAmountRate1...,MakeAmount5,MakeAmountRate5
 */
static bool skill_parse_row_changematerialdb(char *split[], int columns, int current) {
	unsigned short x, y, nameid = atoi(split[0]);

	if( !itemdb_exists(nameid) ) {
		ShowError("skill_parse_row_changematerialdb: Invalid item %d.\n", nameid);
		return false;
	}
	if( current >= MAX_SKILL_CHANGEMATERIAL_DB ) {
		ShowError("skill_parse_row_changematerialdb: Maximum amount of entries reached (%d), increase MAX_SKILL_CHANGEMATERIAL_DB\n", MAX_SKILL_CHANGEMATERIAL_DB);
		return false;
	}
	for( x = 0; x < MAX_SKILL_PRODUCE_DB; x++ ) {
		if( skill_produce_db[x].nameid == nameid && skill_produce_db[x].req_skill == GN_CHANGEMATERIAL )
			break;
	}
	if( x >= MAX_SKILL_PRODUCE_DB ) {
		ShowError("skill_parse_row_changematerialdb: Not supported item ID(%d) for Change Material. \n", nameid);
		return false;
	}

	skill_changematerial_db[current].nameid = nameid;
	skill_changematerial_db[current].unique_id = atoi(split[1]);
	skill_changematerial_db[current].rate = atoi(split[2]);

	for( x = 3, y = 0; x + 1 < columns && split[x] && split[x + 1] && y < MAX_SKILL_CHANGEMATERIAL_SET; x += 2, y++ ) {
		skill_changematerial_db[current].qty[y] = atoi(split[x]);
		skill_changematerial_db[current].qty_rate[y] = atoi(split[x + 1]);
	}

	return true;
}

#ifdef ADJUST_SKILL_DAMAGE
/**
 * Reads skill damage adjustment
 * @author [Lilith]
 */
static bool skill_parse_row_skilldamage(char *split[], int columns, int current)
{
	uint16 skill_id = skill_name2id(split[0]);
	uint16 idx = skill_get_index(skill_id);

	if( !idx ) { //Invalid skill id
		ShowWarning("skill_parse_row_skilldamage: Invalid skill '%s'. Skipping..\n", split[0]);
		return false;
	}
	memset(&skill_db[idx].damage, 0, sizeof(struct s_skill_damage));
	skill_db[idx].damage.caster |= atoi(split[1]);
	skill_db[idx].damage.map |= atoi(split[2]);
	skill_db[idx].damage.pc = cap_value(atoi(split[3]), -100, INT_MAX);
	if( split[3] )
		skill_db[idx].damage.mob = cap_value(atoi(split[4]), -100, INT_MAX);
	if( split[4] )
		skill_db[idx].damage.boss = cap_value(atoi(split[5]), -100, INT_MAX);
	if( split[5] )
		skill_db[idx].damage.other = cap_value(atoi(split[6]), -100, INT_MAX);
	return true;
}
#endif

/*===============================
 * DB reading.
 * skill_db.txt
 * skill_require_db.txt
 * skill_cast_db.txt
 * skill_castnodex_db.txt
 * skill_nocast_db.txt
 * skill_unit_db.txt
 * produce_db.txt
 * create_arrow_db.txt
 * abra_db.txt
 *------------------------------*/
static void skill_readdb(void)
{
	//init skill db structures
	db_clear(skilldb_name2id);
	memset(skill_db,0,sizeof(skill_db));
	memset(skill_produce_db,0,sizeof(skill_produce_db));
	memset(skill_arrow_db,0,sizeof(skill_arrow_db));
	memset(skill_abra_db,0,sizeof(skill_abra_db));
	memset(skill_spellbook_db,0,sizeof(skill_spellbook_db));
	memset(skill_magicmushroom_db,0,sizeof(skill_magicmushroom_db));
	memset(skill_changematerial_db,0,sizeof(skill_changematerial_db));

	//load skill databases
	safestrncpy(skill_db[0].name, "UNKNOWN_SKILL", sizeof(skill_db[0].name));
	safestrncpy(skill_db[0].desc, "Unknown Skill", sizeof(skill_db[0].desc));

	sv_readdb(db_path, DBPATH"skill_db.txt"          , ',',    18, 18, MAX_SKILL_DB, skill_parse_row_skilldb);
	sv_readdb(db_path, DBPATH"skill_require_db.txt"  , ',',    34, 34, MAX_SKILL_DB, skill_parse_row_requiredb);
#ifdef RENEWAL_CAST
	sv_readdb(db_path, "re/skill_cast_db.txt"        , ',',     8,  8, MAX_SKILL_DB, skill_parse_row_castdb);
#else
	sv_readdb(db_path, "pre-re/skill_cast_db.txt"    , ',',     7,  7, MAX_SKILL_DB, skill_parse_row_castdb);
#endif
	sv_readdb(db_path, DBPATH"skill_castnodex_db.txt", ',',     2,  3, MAX_SKILL_DB, skill_parse_row_castnodexdb);
	sv_readdb(db_path, DBPATH"skill_unit_db.txt"     , ',',     8,  8, MAX_SKILL_DB, skill_parse_row_unitdb);
	sv_readdb(db_path, DBPATH"skill_nocast_db.txt"   , ',',     2,  2, MAX_SKILL_DB, skill_parse_row_nocastdb);
	skill_init_unit_layout();
	skill_init_nounit_layout();
	sv_readdb(db_path, "produce_db.txt"              , ',',     4,  4 + 2 * MAX_PRODUCE_RESOURCE, MAX_SKILL_PRODUCE_DB, skill_parse_row_producedb);
	sv_readdb(db_path, "create_arrow_db.txt"         , ',', 1 + 2,  1 + 2 * MAX_ARROW_RESULT, MAX_SKILL_ARROW_DB, skill_parse_row_createarrowdb);
	sv_readdb(db_path, "abra_db.txt"                 , ',',     3,  3, MAX_SKILL_ABRA_DB, skill_parse_row_abradb);
	//Warlock
	sv_readdb(db_path, "spellbook_db.txt"            , ',',     3,  3, MAX_SKILL_SPELLBOOK_DB, skill_parse_row_spellbookdb);
	//Guillotine Cross
	sv_readdb(db_path, "magicmushroom_db.txt"        , ',',     1,  1, MAX_SKILL_MAGICMUSHROOM_DB, skill_parse_row_magicmushroomdb);
	sv_readdb(db_path, "skill_copyable_db.txt"       , ',',     2,  4, MAX_SKILL_DB, skill_parse_row_copyabledb);
	sv_readdb(db_path, "skill_improvise_db.txt"      , ',',     2,  2, MAX_SKILL_IMPROVISE_DB, skill_parse_row_improvisedb);
	sv_readdb(db_path, "skill_changematerial_db.txt" , ',',     4,  4 + 2 * MAX_SKILL_CHANGEMATERIAL_SET, MAX_SKILL_CHANGEMATERIAL_DB, skill_parse_row_changematerialdb);
	sv_readdb(db_path, "skill_nonearnpc_db.txt"      , ',',     2,  3, MAX_SKILL_DB, skill_parse_row_nonearnpcrangedb);
#ifdef ADJUST_SKILL_DAMAGE
	sv_readdb(db_path, "skill_damage_db.txt"         , ',',     4,  7, MAX_SKILL_DB, skill_parse_row_skilldamage);
#endif
}

void skill_reload(void) {
	struct s_mapiterator *iter;
	struct map_session_data *sd;

	skill_destroy_requirement();
	skill_readdb();
	/* Lets update all players skill tree : so that if any skill modes were changed they're properly updated */
	iter = mapit_getallusers();
	for( sd = (TBL_PC *)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC *)mapit_next(iter) )
		clif_skillinfoblock(sd);
	mapit_free(iter);
}

/*==========================================
 *
 *------------------------------------------*/
void do_init_skill(void)
{
	skilldb_name2id = strdb_alloc(DB_OPT_DUP_KEY|DB_OPT_RELEASE_DATA,0);
	skill_readdb();

	skillunit_group_db = idb_alloc(DB_OPT_BASE);
	skillunit_db = idb_alloc(DB_OPT_BASE);
	skillusave_db = idb_alloc(DB_OPT_RELEASE_DATA);
	bowling_db = idb_alloc(DB_OPT_BASE);
	skill_unit_ers = ers_new(sizeof(struct skill_unit_group),"skill.c::skill_unit_ers",ERS_OPT_NONE);
	skill_timer_ers = ers_new(sizeof(struct skill_timerskill),"skill.c::skill_timer_ers",ERS_OPT_NONE);

	add_timer_func_list(skill_unit_timer,"skill_unit_timer");
	add_timer_func_list(skill_castend_id,"skill_castend_id");
	add_timer_func_list(skill_castend_pos,"skill_castend_pos");
	add_timer_func_list(skill_timerskill,"skill_timerskill");
	add_timer_func_list(skill_blockpc_end,"skill_blockpc_end");

	add_timer_interval(gettick() + SKILLUNITTIMER_INTERVAL,skill_unit_timer,0,0,SKILLUNITTIMER_INTERVAL);
}

void do_final_skill(void)
{
	skill_destroy_requirement();
	db_destroy(skilldb_name2id);
	db_destroy(skillunit_group_db);
	db_destroy(skillunit_db);
	db_destroy(skillusave_db);
	db_destroy(bowling_db);
	ers_destroy(skill_unit_ers);
	ers_destroy(skill_timer_ers);
}
